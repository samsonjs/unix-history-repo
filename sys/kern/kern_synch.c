/*-
 * Copyright (c) 1982, 1986, 1990, 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 * (c) UNIX System Laboratories, Inc.
 * All or some portions of this file are derived from material licensed
 * to the University of California by American Telephone and Telegraph
 * Co. or Unix System Laboratories, Inc. and are reproduced herein with
 * the permission of UNIX System Laboratories, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)kern_synch.c	8.9 (Berkeley) 5/19/95
 * $FreeBSD$
 */

#include "opt_ddb.h"
#include "opt_ktrace.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/condvar.h>
#include <sys/kernel.h>
#include <sys/ktr.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/resourcevar.h>
#include <sys/signalvar.h>
#include <sys/smp.h>
#include <sys/sx.h>
#include <sys/sysctl.h>
#include <sys/sysproto.h>
#include <sys/vmmeter.h>
#ifdef DDB
#include <ddb/ddb.h>
#endif
#ifdef KTRACE
#include <sys/uio.h>
#include <sys/ktrace.h>
#endif

#include <machine/cpu.h>

static void sched_setup(void *dummy);
SYSINIT(sched_setup, SI_SUB_KICK_SCHEDULER, SI_ORDER_FIRST, sched_setup, NULL)

int	hogticks;
int	lbolt;
int	sched_quantum;		/* Roundrobin scheduling quantum in ticks. */

static struct callout loadav_callout;
static struct callout schedcpu_callout;
static struct callout roundrobin_callout;

struct loadavg averunnable =
	{ {0, 0, 0}, FSCALE };	/* load average, of runnable procs */
/*
 * Constants for averages over 1, 5, and 15 minutes
 * when sampling at 5 second intervals.
 */
static fixpt_t cexp[3] = {
	0.9200444146293232 * FSCALE,	/* exp(-1/12) */
	0.9834714538216174 * FSCALE,	/* exp(-1/60) */
	0.9944598480048967 * FSCALE,	/* exp(-1/180) */
};

static void	endtsleep(void *);
static void	loadav(void *arg);
static void	roundrobin(void *arg);
static void	schedcpu(void *arg);

static int
sysctl_kern_quantum(SYSCTL_HANDLER_ARGS)
{
	int error, new_val;

	new_val = sched_quantum * tick;
	error = sysctl_handle_int(oidp, &new_val, 0, req);
        if (error != 0 || req->newptr == NULL)
		return (error);
	if (new_val < tick)
		return (EINVAL);
	sched_quantum = new_val / tick;
	hogticks = 2 * sched_quantum;
	return (0);
}

SYSCTL_PROC(_kern, OID_AUTO, quantum, CTLTYPE_INT|CTLFLAG_RW,
	0, sizeof sched_quantum, sysctl_kern_quantum, "I",
	"Roundrobin scheduling quantum in microseconds");

/*
 * Arrange to reschedule if necessary, taking the priorities and
 * schedulers into account.
 */
void
maybe_resched(struct thread *td)
{

	mtx_assert(&sched_lock, MA_OWNED);
	if (td->td_priority < curthread->td_priority)
		curthread->td_kse->ke_flags |= KEF_NEEDRESCHED;
}

int 
roundrobin_interval(void)
{
	return (sched_quantum);
}

/*
 * Force switch among equal priority processes every 100ms.
 * We don't actually need to force a context switch of the current process.
 * The act of firing the event triggers a context switch to softclock() and
 * then switching back out again which is equivalent to a preemption, thus
 * no further work is needed on the local CPU.
 */
/* ARGSUSED */
static void
roundrobin(arg)
	void *arg;
{

#ifdef SMP
	mtx_lock_spin(&sched_lock);
	forward_roundrobin();
	mtx_unlock_spin(&sched_lock);
#endif

	callout_reset(&roundrobin_callout, sched_quantum, roundrobin, NULL);
}

/*
 * Constants for digital decay and forget:
 *	90% of (p_estcpu) usage in 5 * loadav time
 *	95% of (p_pctcpu) usage in 60 seconds (load insensitive)
 *          Note that, as ps(1) mentions, this can let percentages
 *          total over 100% (I've seen 137.9% for 3 processes).
 *
 * Note that schedclock() updates p_estcpu and p_cpticks asynchronously.
 *
 * We wish to decay away 90% of p_estcpu in (5 * loadavg) seconds.
 * That is, the system wants to compute a value of decay such
 * that the following for loop:
 * 	for (i = 0; i < (5 * loadavg); i++)
 * 		p_estcpu *= decay;
 * will compute
 * 	p_estcpu *= 0.1;
 * for all values of loadavg:
 *
 * Mathematically this loop can be expressed by saying:
 * 	decay ** (5 * loadavg) ~= .1
 *
 * The system computes decay as:
 * 	decay = (2 * loadavg) / (2 * loadavg + 1)
 *
 * We wish to prove that the system's computation of decay
 * will always fulfill the equation:
 * 	decay ** (5 * loadavg) ~= .1
 *
 * If we compute b as:
 * 	b = 2 * loadavg
 * then
 * 	decay = b / (b + 1)
 *
 * We now need to prove two things:
 *	1) Given factor ** (5 * loadavg) ~= .1, prove factor == b/(b+1)
 *	2) Given b/(b+1) ** power ~= .1, prove power == (5 * loadavg)
 *
 * Facts:
 *         For x close to zero, exp(x) =~ 1 + x, since
 *              exp(x) = 0! + x**1/1! + x**2/2! + ... .
 *              therefore exp(-1/b) =~ 1 - (1/b) = (b-1)/b.
 *         For x close to zero, ln(1+x) =~ x, since
 *              ln(1+x) = x - x**2/2 + x**3/3 - ...     -1 < x < 1
 *              therefore ln(b/(b+1)) = ln(1 - 1/(b+1)) =~ -1/(b+1).
 *         ln(.1) =~ -2.30
 *
 * Proof of (1):
 *    Solve (factor)**(power) =~ .1 given power (5*loadav):
 *	solving for factor,
 *      ln(factor) =~ (-2.30/5*loadav), or
 *      factor =~ exp(-1/((5/2.30)*loadav)) =~ exp(-1/(2*loadav)) =
 *          exp(-1/b) =~ (b-1)/b =~ b/(b+1).                    QED
 *
 * Proof of (2):
 *    Solve (factor)**(power) =~ .1 given factor == (b/(b+1)):
 *	solving for power,
 *      power*ln(b/(b+1)) =~ -2.30, or
 *      power =~ 2.3 * (b + 1) = 4.6*loadav + 2.3 =~ 5*loadav.  QED
 *
 * Actual power values for the implemented algorithm are as follows:
 *      loadav: 1       2       3       4
 *      power:  5.68    10.32   14.94   19.55
 */

/* calculations for digital decay to forget 90% of usage in 5*loadav sec */
#define	loadfactor(loadav)	(2 * (loadav))
#define	decay_cpu(loadfac, cpu)	(((loadfac) * (cpu)) / ((loadfac) + FSCALE))

/* decay 95% of `p_pctcpu' in 60 seconds; see CCPU_SHIFT before changing */
static fixpt_t	ccpu = 0.95122942450071400909 * FSCALE;	/* exp(-1/20) */
SYSCTL_INT(_kern, OID_AUTO, ccpu, CTLFLAG_RD, &ccpu, 0, "");

/* kernel uses `FSCALE', userland (SHOULD) use kern.fscale */
static int	fscale __unused = FSCALE;
SYSCTL_INT(_kern, OID_AUTO, fscale, CTLFLAG_RD, 0, FSCALE, "");

/*
 * If `ccpu' is not equal to `exp(-1/20)' and you still want to use the
 * faster/more-accurate formula, you'll have to estimate CCPU_SHIFT below
 * and possibly adjust FSHIFT in "param.h" so that (FSHIFT >= CCPU_SHIFT).
 *
 * To estimate CCPU_SHIFT for exp(-1/20), the following formula was used:
 *	1 - exp(-1/20) ~= 0.0487 ~= 0.0488 == 1 (fixed pt, *11* bits).
 *
 * If you don't want to bother with the faster/more-accurate formula, you
 * can set CCPU_SHIFT to (FSHIFT + 1) which will use a slower/less-accurate
 * (more general) method of calculating the %age of CPU used by a process.
 */
#define	CCPU_SHIFT	11

/*
 * Recompute process priorities, every hz ticks.
 * MP-safe, called without the Giant mutex.
 */
/* ARGSUSED */
static void
schedcpu(arg)
	void *arg;
{
	register fixpt_t loadfac = loadfactor(averunnable.ldavg[0]);
	struct thread *td;
	struct proc *p;
	struct kse *ke;
	struct ksegrp *kg;
	int realstathz;
	int awake;

	realstathz = stathz ? stathz : hz;
	sx_slock(&allproc_lock);
	FOREACH_PROC_IN_SYSTEM(p) {
		mtx_lock_spin(&sched_lock);
		p->p_swtime++;
		FOREACH_KSEGRP_IN_PROC(p, kg) { 
			awake = 0;
			FOREACH_KSE_IN_GROUP(kg, ke) {
				/*
				 * Increment time in/out of memory and sleep
				 * time (if sleeping).  We ignore overflow;
				 * with 16-bit int's (remember them?)
				 * overflow takes 45 days.
				 */
				/*
				 * The kse slptimes are not touched in wakeup
				 * because the thread may not HAVE a KSE.
				 */
				if (ke->ke_state == KES_ONRUNQ) {
					awake = 1;
					ke->ke_flags &= ~KEF_DIDRUN;
				} else if ((ke->ke_state == KES_THREAD) &&
				    (TD_IS_RUNNING(ke->ke_thread))) {
					awake = 1;
					/* Do not clear KEF_DIDRUN */
				} else if (ke->ke_flags & KEF_DIDRUN) {
					awake = 1;
					ke->ke_flags &= ~KEF_DIDRUN;
				}

				/*
				 * pctcpu is only for ps?
				 * Do it per kse.. and add them up at the end?
				 * XXXKSE
				 */
				ke->ke_pctcpu
				    = (ke->ke_pctcpu * ccpu) >> FSHIFT;
				/*
				 * If the kse has been idle the entire second,
				 * stop recalculating its priority until
				 * it wakes up.
				 */
				if (ke->ke_cpticks == 0)
					continue;
#if	(FSHIFT >= CCPU_SHIFT)
				ke->ke_pctcpu += (realstathz == 100) ?
				    ((fixpt_t) ke->ke_cpticks) <<
				    (FSHIFT - CCPU_SHIFT) :
				    100 * (((fixpt_t) ke->ke_cpticks) <<
				    (FSHIFT - CCPU_SHIFT)) / realstathz;
#else
				ke->ke_pctcpu += ((FSCALE - ccpu) *
				    (ke->ke_cpticks * FSCALE / realstathz)) >>
				    FSHIFT;
#endif
				ke->ke_cpticks = 0;
			} /* end of kse loop */
			/* 
			 * If there are ANY running threads in this KSEGRP,
			 * then don't count it as sleeping.
			 */
			if (awake) {
				if (kg->kg_slptime > 1) {
					/*
					 * In an ideal world, this should not
					 * happen, because whoever woke us
					 * up from the long sleep should have
					 * unwound the slptime and reset our
					 * priority before we run at the stale
					 * priority.  Should KASSERT at some
					 * point when all the cases are fixed.
					 */
					updatepri(kg);
				}
				kg->kg_slptime = 0;
			} else {
				kg->kg_slptime++;
			}
			if (kg->kg_slptime > 1)
				continue;
			kg->kg_estcpu = decay_cpu(loadfac, kg->kg_estcpu);
		      	resetpriority(kg);
			FOREACH_THREAD_IN_GROUP(kg, td) {
				int changedqueue;
				if (td->td_priority >= PUSER) {
					/*
					 * Only change the priority
					 * of threads that are still at their
					 * user priority. 
					 * XXXKSE This is problematic
					 * as we may need to re-order
					 * the threads on the KSEG list.
					 */
					changedqueue =
					    ((td->td_priority / RQ_PPQ) !=
					     (kg->kg_user_pri / RQ_PPQ));

					td->td_priority = kg->kg_user_pri;
					if (changedqueue && TD_ON_RUNQ(td)) {
						/* this could be optimised */
						remrunqueue(td);
						td->td_priority =
						    kg->kg_user_pri;
						setrunqueue(td);
					} else {
						td->td_priority = kg->kg_user_pri;
					}
				}
			}
		} /* end of ksegrp loop */
		mtx_unlock_spin(&sched_lock);
	} /* end of process loop */
	sx_sunlock(&allproc_lock);
	wakeup(&lbolt);
	callout_reset(&schedcpu_callout, hz, schedcpu, NULL);
}

/*
 * Recalculate the priority of a process after it has slept for a while.
 * For all load averages >= 1 and max p_estcpu of 255, sleeping for at
 * least six times the loadfactor will decay p_estcpu to zero.
 */
void
updatepri(struct ksegrp *kg)
{
	register unsigned int newcpu;
	register fixpt_t loadfac = loadfactor(averunnable.ldavg[0]);

	newcpu = kg->kg_estcpu;
	if (kg->kg_slptime > 5 * loadfac)
		kg->kg_estcpu = 0;
	else {
		kg->kg_slptime--;	/* the first time was done in schedcpu */
		while (newcpu && --kg->kg_slptime)
			newcpu = decay_cpu(loadfac, newcpu);
		kg->kg_estcpu = newcpu;
	}
	resetpriority(kg);
}

/*
 * We're only looking at 7 bits of the address; everything is
 * aligned to 4, lots of things are aligned to greater powers
 * of 2.  Shift right by 8, i.e. drop the bottom 256 worth.
 */
#define TABLESIZE	128
static TAILQ_HEAD(slpquehead, thread) slpque[TABLESIZE];
#define LOOKUP(x)	(((intptr_t)(x) >> 8) & (TABLESIZE - 1))

void
sleepinit(void)
{
	int i;

	sched_quantum = hz/10;
	hogticks = 2 * sched_quantum;
	for (i = 0; i < TABLESIZE; i++)
		TAILQ_INIT(&slpque[i]);
}

/*
 * General sleep call.  Suspends the current process until a wakeup is
 * performed on the specified identifier.  The process will then be made
 * runnable with the specified priority.  Sleeps at most timo/hz seconds
 * (0 means no timeout).  If pri includes PCATCH flag, signals are checked
 * before and after sleeping, else signals are not checked.  Returns 0 if
 * awakened, EWOULDBLOCK if the timeout expires.  If PCATCH is set and a
 * signal needs to be delivered, ERESTART is returned if the current system
 * call should be restarted if possible, and EINTR is returned if the system
 * call should be interrupted by the signal (return EINTR).
 *
 * The mutex argument is exited before the caller is suspended, and
 * entered before msleep returns.  If priority includes the PDROP
 * flag the mutex is not entered before returning.
 */

int
msleep(ident, mtx, priority, wmesg, timo)
	void *ident;
	struct mtx *mtx;
	int priority, timo;
	const char *wmesg;
{
	struct thread *td = curthread;
	struct proc *p = td->td_proc;
	int sig, catch = priority & PCATCH;
	int rval = 0;
	WITNESS_SAVE_DECL(mtx);

#ifdef KTRACE
	if (KTRPOINT(td, KTR_CSW))
		ktrcsw(1, 0);
#endif
	WITNESS_SLEEP(0, &mtx->mtx_object);
	KASSERT(timo != 0 || mtx_owned(&Giant) || mtx != NULL,
	    ("sleeping without a mutex"));
	/*
	 * If we are capable of async syscalls and there isn't already
	 * another one ready to return, start a new thread
	 * and queue it as ready to run. Note that there is danger here
	 * because we need to make sure that we don't sleep allocating
	 * the thread (recursion here might be bad).
	 * Hence the TDF_INMSLEEP flag.
	 */
	if (p->p_flag & P_KSES) {
		/* Just don't bother if we are exiting
				and not the exiting thread. */
		if ((p->p_flag & P_WEXIT) && catch && p->p_singlethread != td)
			return (EINTR);
		if (td->td_mailbox && (!(td->td_flags & TDF_INMSLEEP))) {
			/*
			 * Arrange for an upcall to be readied.
			 * it will not actually happen until all
			 * pending in-kernel work for this KSEGRP
			 * has been done.
			 */
			mtx_lock_spin(&sched_lock);
			/* Don't recurse here! */
			td->td_flags |= TDF_INMSLEEP;
			thread_schedule_upcall(td, td->td_kse);
			td->td_flags &= ~TDF_INMSLEEP;
			mtx_unlock_spin(&sched_lock);
		}
	}
	mtx_lock_spin(&sched_lock);
	if (cold ) {
		/*
		 * During autoconfiguration, just give interrupts
		 * a chance, then just return.
		 * Don't run any other procs or panic below,
		 * in case this is the idle process and already asleep.
		 */
		if (mtx != NULL && priority & PDROP)
			mtx_unlock(mtx);
		mtx_unlock_spin(&sched_lock);
		return (0);
	}

	DROP_GIANT();

	if (mtx != NULL) {
		mtx_assert(mtx, MA_OWNED | MA_NOTRECURSED);
		WITNESS_SAVE(&mtx->mtx_object, mtx);
		mtx_unlock(mtx);
		if (priority & PDROP)
			mtx = NULL;
	}

	KASSERT(p != NULL, ("msleep1"));
	KASSERT(ident != NULL && TD_IS_RUNNING(td), ("msleep"));

	CTR5(KTR_PROC, "msleep: thread %p (pid %d, %s) on %s (%p)",
	    td, p->p_pid, p->p_comm, wmesg, ident);

	td->td_wchan = ident;
	td->td_wmesg = wmesg;
	td->td_ksegrp->kg_slptime = 0;
	td->td_priority = priority & PRIMASK;
	TAILQ_INSERT_TAIL(&slpque[LOOKUP(ident)], td, td_slpq);
	TD_SET_ON_SLEEPQ(td);
	if (timo)
		callout_reset(&td->td_slpcallout, timo, endtsleep, td);
	/*
	 * We put ourselves on the sleep queue and start our timeout
	 * before calling thread_suspend_check, as we could stop there, and
	 * a wakeup or a SIGCONT (or both) could occur while we were stopped.
	 * without resuming us, thus we must be ready for sleep
	 * when cursig is called.  If the wakeup happens while we're
	 * stopped, td->td_wchan will be 0 upon return from cursig.
	 */
	if (catch) {
		CTR3(KTR_PROC, "msleep caught: thread %p (pid %d, %s)", td,
		    p->p_pid, p->p_comm);
		td->td_flags |= TDF_SINTR;
		mtx_unlock_spin(&sched_lock);
		PROC_LOCK(p);
		sig = cursig(td);
		if (sig == 0 && thread_suspend_check(1))
			sig = SIGSTOP;
		mtx_lock_spin(&sched_lock);
		PROC_UNLOCK(p);
		if (sig != 0) {
			if (TD_ON_SLEEPQ(td))
				unsleep(td);
		} else if (!TD_ON_SLEEPQ(td))
			catch = 0;
	} else
		sig = 0;
	if (TD_ON_SLEEPQ(td)) {
		p->p_stats->p_ru.ru_nvcsw++;
		TD_SET_SLEEPING(td);
		mi_switch();
	}
	CTR3(KTR_PROC, "msleep resume: thread %p (pid %d, %s)", td, p->p_pid,
	    p->p_comm);
	KASSERT(TD_IS_RUNNING(td), ("running but not TDS_RUNNING"));
	td->td_flags &= ~TDF_SINTR;
	if (td->td_flags & TDF_TIMEOUT) {
		td->td_flags &= ~TDF_TIMEOUT;
		if (sig == 0)
			rval = EWOULDBLOCK;
	} else if (td->td_flags & TDF_TIMOFAIL) {
		td->td_flags &= ~TDF_TIMOFAIL;
	} else if (timo && callout_stop(&td->td_slpcallout) == 0) {
		/*
		 * This isn't supposed to be pretty.  If we are here, then
		 * the endtsleep() callout is currently executing on another
		 * CPU and is either spinning on the sched_lock or will be
		 * soon.  If we don't synchronize here, there is a chance
		 * that this process may msleep() again before the callout
		 * has a chance to run and the callout may end up waking up
		 * the wrong msleep().  Yuck.
		 */
		TD_SET_SLEEPING(td);
		p->p_stats->p_ru.ru_nivcsw++;
		mi_switch();
		td->td_flags &= ~TDF_TIMOFAIL;
	}
	mtx_unlock_spin(&sched_lock);

	if (rval == 0 && catch) {
		PROC_LOCK(p);
		/* XXX: shouldn't we always be calling cursig() */
		if (sig != 0 || (sig = cursig(td))) {
			if (SIGISMEMBER(p->p_sigacts->ps_sigintr, sig))
				rval = EINTR;
			else
				rval = ERESTART;
		}
		PROC_UNLOCK(p);
	}
#ifdef KTRACE
	if (KTRPOINT(td, KTR_CSW))
		ktrcsw(0, 0);
#endif
	PICKUP_GIANT();
	if (mtx != NULL) {
		mtx_lock(mtx);
		WITNESS_RESTORE(&mtx->mtx_object, mtx);
	}
	return (rval);
}

/*
 * Implement timeout for msleep()
 *
 * If process hasn't been awakened (wchan non-zero),
 * set timeout flag and undo the sleep.  If proc
 * is stopped, just unsleep so it will remain stopped.
 * MP-safe, called without the Giant mutex.
 */
static void
endtsleep(arg)
	void *arg;
{
	register struct thread *td = arg;

	CTR3(KTR_PROC, "endtsleep: thread %p (pid %d, %s)",
	    td, td->td_proc->p_pid, td->td_proc->p_comm);
	mtx_lock_spin(&sched_lock);
	/*
	 * This is the other half of the synchronization with msleep()
	 * described above.  If the TDS_TIMEOUT flag is set, we lost the
	 * race and just need to put the process back on the runqueue.
	 */
	if (TD_ON_SLEEPQ(td)) {
		TAILQ_REMOVE(&slpque[LOOKUP(td->td_wchan)], td, td_slpq);
		TD_CLR_ON_SLEEPQ(td);
		td->td_flags |= TDF_TIMEOUT;
	} else {
		td->td_flags |= TDF_TIMOFAIL;
	}
	TD_CLR_SLEEPING(td);
	setrunnable(td);
	mtx_unlock_spin(&sched_lock);
}

/*
 * Abort a thread, as if an interrupt had occured.  Only abort
 * interruptable waits (unfortunatly it isn't only safe to abort others).
 * This is about identical to cv_abort().
 * Think about merging them?
 * Also, whatever the signal code does...
 */
void
abortsleep(struct thread *td)
{

	mtx_assert(&sched_lock, MA_OWNED);
	/*
	 * If the TDF_TIMEOUT flag is set, just leave. A
	 * timeout is scheduled anyhow.
	 */
	if ((td->td_flags & (TDF_TIMEOUT | TDF_SINTR)) == TDF_SINTR) {
		if (TD_ON_SLEEPQ(td)) {
			unsleep(td);
			TD_CLR_SLEEPING(td);
			setrunnable(td);
		}
	}
}

/*
 * Remove a process from its wait queue
 */
void
unsleep(struct thread *td)
{

	mtx_lock_spin(&sched_lock);
	if (TD_ON_SLEEPQ(td)) {
		TAILQ_REMOVE(&slpque[LOOKUP(td->td_wchan)], td, td_slpq);
		TD_CLR_ON_SLEEPQ(td);
	}
	mtx_unlock_spin(&sched_lock);
}

/*
 * Make all processes sleeping on the specified identifier runnable.
 */
void
wakeup(ident)
	register void *ident;
{
	register struct slpquehead *qp;
	register struct thread *td;
	struct thread *ntd;
	struct proc *p;

	mtx_lock_spin(&sched_lock);
	qp = &slpque[LOOKUP(ident)];
restart:
	for (td = TAILQ_FIRST(qp); td != NULL; td = ntd) {
		ntd = TAILQ_NEXT(td, td_slpq);
		if (td->td_wchan == ident) {
			unsleep(td);
			TD_CLR_SLEEPING(td);
			setrunnable(td);
			p = td->td_proc;
			CTR3(KTR_PROC,"wakeup: thread %p (pid %d, %s)",
			    td, p->p_pid, p->p_comm);
			goto restart;
		}
	}
	mtx_unlock_spin(&sched_lock);
}

/*
 * Make a process sleeping on the specified identifier runnable.
 * May wake more than one process if a target process is currently
 * swapped out.
 */
void
wakeup_one(ident)
	register void *ident;
{
	register struct slpquehead *qp;
	register struct thread *td;
	register struct proc *p;
	struct thread *ntd;

	mtx_lock_spin(&sched_lock);
	qp = &slpque[LOOKUP(ident)];
	for (td = TAILQ_FIRST(qp); td != NULL; td = ntd) {
		ntd = TAILQ_NEXT(td, td_slpq);
		if (td->td_wchan == ident) {
			unsleep(td);
			TD_CLR_SLEEPING(td);
			setrunnable(td);
			p = td->td_proc;
			CTR3(KTR_PROC,"wakeup1: thread %p (pid %d, %s)",
			    td, p->p_pid, p->p_comm);
			break;
		}
	}
	mtx_unlock_spin(&sched_lock);
}

/*
 * The machine independent parts of mi_switch().
 */
void
mi_switch(void)
{
	struct bintime new_switchtime;
	struct thread *td = curthread;	/* XXX */
	struct proc *p = td->td_proc;	/* XXX */
	struct kse *ke = td->td_kse;
	u_int sched_nest;

	mtx_assert(&sched_lock, MA_OWNED | MA_NOTRECURSED);
	KASSERT((ke->ke_state == KES_THREAD), ("mi_switch: kse state?"));
	KASSERT(!TD_ON_RUNQ(td), ("mi_switch: called by old code"));
#ifdef INVARIANTS
	if (!TD_ON_LOCK(td) &&
	    !TD_ON_RUNQ(td) &&
	    !TD_IS_RUNNING(td))
		mtx_assert(&Giant, MA_NOTOWNED);
#endif
	KASSERT(td->td_critnest == 1,
	    ("mi_switch: switch in a critical section"));

	/*
	 * Compute the amount of time during which the current
	 * process was running, and add that to its total so far.
	 */
	binuptime(&new_switchtime);
	bintime_add(&p->p_runtime, &new_switchtime);
	bintime_sub(&p->p_runtime, PCPU_PTR(switchtime));

#ifdef DDB
	/*
	 * Don't perform context switches from the debugger.
	 */
	if (db_active) {
		mtx_unlock_spin(&sched_lock);
		db_error("Context switches not allowed in the debugger.");
	}
#endif

	/*
	 * Check if the process exceeds its cpu resource allocation.  If
	 * over max, arrange to kill the process in ast().
	 *
	 * XXX The checking for p_limit being NULL here is totally bogus,
	 * but hides something easy to trip over, as a result of us switching
	 * after the limit has been freed/set-to-NULL.  A KASSERT() will be
	 * appropriate once this is no longer a bug, to watch for regression.
	 */
	if (p->p_state != PRS_ZOMBIE && p->p_limit != NULL &&
	    p->p_limit->p_cpulimit != RLIM_INFINITY &&
	    p->p_runtime.sec > p->p_limit->p_cpulimit) {
		p->p_sflag |= PS_XCPU;
		ke->ke_flags |= KEF_ASTPENDING;
	}

	/*
	 * Finish up stats for outgoing thread.
	 */
	cnt.v_swtch++;
	PCPU_SET(switchtime, new_switchtime);
	CTR3(KTR_PROC, "mi_switch: old thread %p (pid %d, %s)", td, p->p_pid,
	    p->p_comm);
	sched_nest = sched_lock.mtx_recurse;
	td->td_lastcpu = ke->ke_oncpu;
	ke->ke_oncpu = NOCPU;
	ke->ke_flags &= ~KEF_NEEDRESCHED;
	/*
	 * At the last moment, if this thread is still marked RUNNING,
	 * then put it back on the run queue as it has not been suspended
	 * or stopped or any thing else similar.
	 */
	if (TD_IS_RUNNING(td)) {
		/* Put us back on the run queue (kse and all). */
		setrunqueue(td);
	} else if (p->p_flag & P_KSES) {
		/*
		 * We will not be on the run queue. So we must be
		 * sleeping or similar. As it's available,
		 * someone else can use the KSE if they need it.
		 * (If bound LOANING can still occur).
		 */
		kse_reassign(ke);
	}

	cpu_switch();		/* SHAZAM!!*/

	/* 
	 * Start setting up stats etc. for the incoming thread.
	 * Similar code in fork_exit() is returned to by cpu_switch()
	 * in the case of a new thread/process.
	 */
	td->td_kse->ke_oncpu = PCPU_GET(cpuid);
	sched_lock.mtx_recurse = sched_nest;
	sched_lock.mtx_lock = (uintptr_t)td;
	CTR3(KTR_PROC, "mi_switch: new thread %p (pid %d, %s)", td, p->p_pid,
	    p->p_comm);
	if (PCPU_GET(switchtime.sec) == 0)
		binuptime(PCPU_PTR(switchtime));
	PCPU_SET(switchticks, ticks);

	/*
	 * Call the switchin function while still holding the scheduler lock
	 * (used by the idlezero code and the general page-zeroing code)
	 */
	if (td->td_switchin)
		td->td_switchin();
}

/*
 * Change process state to be runnable,
 * placing it on the run queue if it is in memory,
 * and awakening the swapper if it isn't in memory.
 */
void
setrunnable(struct thread *td)
{
	struct proc *p = td->td_proc;
	struct ksegrp *kg;

	mtx_assert(&sched_lock, MA_OWNED);
	switch (p->p_state) {
	case PRS_ZOMBIE:
		panic("setrunnable(1)");
	default:
		break;
	}
	switch (td->td_state) {
	case TDS_RUNNING:
	case TDS_RUNQ:
		return;
	case TDS_INHIBITED:
		/*
		 * If we are only inhibited because we are swapped out
		 * then arange to swap in this process. Otherwise just return.
		 */
		if (td->td_inhibitors != TDI_SWAPPED)
			return;
	case TDS_CAN_RUN:
		break;
	default:
		printf("state is 0x%x", td->td_state);
		panic("setrunnable(2)");
	}
	if ((p->p_sflag & PS_INMEM) == 0) {
		if ((p->p_sflag & PS_SWAPPINGIN) == 0) {
			p->p_sflag |= PS_SWAPINREQ;
			wakeup(&proc0);
		}
	} else {
		kg = td->td_ksegrp;
		if (kg->kg_slptime > 1)
			updatepri(kg);
		kg->kg_slptime = 0;
		setrunqueue(td);
		maybe_resched(td);
	}
}

/*
 * Compute the priority of a process when running in user mode.
 * Arrange to reschedule if the resulting priority is better
 * than that of the current process.
 */
void
resetpriority(kg)
	register struct ksegrp *kg;
{
	register unsigned int newpriority;
	struct thread *td;

	mtx_lock_spin(&sched_lock);
	if (kg->kg_pri_class == PRI_TIMESHARE) {
		newpriority = PUSER + kg->kg_estcpu / INVERSE_ESTCPU_WEIGHT +
		    NICE_WEIGHT * (kg->kg_nice - PRIO_MIN);
		newpriority = min(max(newpriority, PRI_MIN_TIMESHARE),
		    PRI_MAX_TIMESHARE);
		kg->kg_user_pri = newpriority;
	}
	FOREACH_THREAD_IN_GROUP(kg, td) {
		maybe_resched(td);			/* XXXKSE silly */
	}
	mtx_unlock_spin(&sched_lock);
}

/*
 * Compute a tenex style load average of a quantity on
 * 1, 5 and 15 minute intervals.
 * XXXKSE   Needs complete rewrite when correct info is available.
 * Completely Bogus.. only works with 1:1 (but compiles ok now :-)
 */
static void
loadav(void *arg)
{
	int i, nrun;
	struct loadavg *avg;
	struct proc *p;
	struct thread *td;

	avg = &averunnable;
	sx_slock(&allproc_lock);
	nrun = 0;
	FOREACH_PROC_IN_SYSTEM(p) {
		FOREACH_THREAD_IN_PROC(p, td) {
			switch (td->td_state) {
			case TDS_RUNQ:
			case TDS_RUNNING:
				if ((p->p_flag & P_NOLOAD) != 0)
					goto nextproc;
				nrun++; /* XXXKSE */
			default:
				break;
			}
nextproc:
			continue;
		}
	}
	sx_sunlock(&allproc_lock);
	for (i = 0; i < 3; i++)
		avg->ldavg[i] = (cexp[i] * avg->ldavg[i] +
		    nrun * FSCALE * (FSCALE - cexp[i])) >> FSHIFT;

	/*
	 * Schedule the next update to occur after 5 seconds, but add a
	 * random variation to avoid synchronisation with processes that
	 * run at regular intervals.
	 */
	callout_reset(&loadav_callout, hz * 4 + (int)(random() % (hz * 2 + 1)),
	    loadav, NULL);
}

/* ARGSUSED */
static void
sched_setup(dummy)
	void *dummy;
{

	callout_init(&schedcpu_callout, 1);
	callout_init(&roundrobin_callout, 0);
	callout_init(&loadav_callout, 0);

	/* Kick off timeout driven events by calling first time. */
	roundrobin(NULL);
	schedcpu(NULL);
	loadav(NULL);
}

/*
 * We adjust the priority of the current process.  The priority of
 * a process gets worse as it accumulates CPU time.  The cpu usage
 * estimator (p_estcpu) is increased here.  resetpriority() will
 * compute a different priority each time p_estcpu increases by
 * INVERSE_ESTCPU_WEIGHT
 * (until MAXPRI is reached).  The cpu usage estimator ramps up
 * quite quickly when the process is running (linearly), and decays
 * away exponentially, at a rate which is proportionally slower when
 * the system is busy.  The basic principle is that the system will
 * 90% forget that the process used a lot of CPU time in 5 * loadav
 * seconds.  This causes the system to favor processes which haven't
 * run much recently, and to round-robin among other processes.
 */
void
schedclock(td)
	struct thread *td;
{
	struct kse *ke;
	struct ksegrp *kg;

	KASSERT((td != NULL), ("schedclock: null thread pointer"));
	ke = td->td_kse;
	kg = td->td_ksegrp;
	ke->ke_cpticks++;
	kg->kg_estcpu = ESTCPULIM(kg->kg_estcpu + 1);
	if ((kg->kg_estcpu % INVERSE_ESTCPU_WEIGHT) == 0) {
		resetpriority(kg);
		if (td->td_priority >= PUSER)
			td->td_priority = kg->kg_user_pri;
	}
}

/*
 * General purpose yield system call
 */
int
yield(struct thread *td, struct yield_args *uap)
{
	struct ksegrp *kg = td->td_ksegrp;

	mtx_assert(&Giant, MA_NOTOWNED);
	mtx_lock_spin(&sched_lock);
	td->td_priority = PRI_MAX_TIMESHARE;
	kg->kg_proc->p_stats->p_ru.ru_nvcsw++;
	mi_switch();
	mtx_unlock_spin(&sched_lock);
	td->td_retval[0] = 0;

	return (0);
}

