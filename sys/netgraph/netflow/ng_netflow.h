/*-
 * Copyright (c) 2004 Gleb Smirnoff <glebius@FreeBSD.org>
 * Copyright (c) 2001-2003 Roman V. Palagin <romanp@unshadow.net>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	 $SourceForge: ng_netflow.h,v 1.26 2004/09/04 15:44:55 glebius Exp $
 *	 $FreeBSD$
 */

#ifndef	_NG_NETFLOW_H_
#define	_NG_NETFLOW_H_

#define NG_NETFLOW_NODE_TYPE	"netflow"
#define NGM_NETFLOW_COOKIE	1095355665

#define	NG_NETFLOW_MAXIFACES	64

/* Hook names */

#define	NG_NETFLOW_HOOK_DATA	"iface"
#define NG_NETFLOW_HOOK_EXPORT	"export"

/* Netgraph commands understood by netflow node */
enum {
    NGM_NETFLOW_INFO = 1,	/* get node info */
    NGM_NETFLOW_IFINFO,		/* get iface info */
    NGM_NETFLOW_SHOW,		/* show ip cache flow */
    NGM_NETFLOW_SETDLT,		/* set data-link type */	
    NGM_NETFLOW_SETIFINDEX, 	/* set interface index */
    NGM_NETFLOW_SETTIMEOUTS, 	/* set active/inactive flow timeouts */
};

/* This structure is returned by the NGM_NETFLOW_INFO message */
struct ng_netflow_info {
	uint64_t	nfinfo_bytes;	/* total number of accounted bytes */
	uint32_t	nfinfo_packets;	/* total number of accounted packets */
	uint32_t	nfinfo_used;	/* number of used cache records */
	uint32_t	nfinfo_free;	/* number of free records */
	uint32_t	nfinfo_inact_t;	/* flow inactive timeout */
	uint32_t	nfinfo_act_t;	/* flow active timeout */
};

/* This structure is returned by the NGM_NETFLOW_IFINFO message */
struct ng_netflow_ifinfo {
	uint32_t	ifinfo_packets;	/* number of packets for this iface */
	uint8_t		ifinfo_dlt;	/* Data Link Type, DLT_XXX */
#define	MAXDLTNAMELEN	20
	u_int16_t	ifinfo_index;	/* connected iface index */
};


/* This structure is passed to NGM_NETFLOW_SETDLT message */
struct ng_netflow_setdlt {
	uint16_t iface;		/* which iface dlt change */
	uint8_t  dlt;			/* DLT_XXX from bpf.h */
};

/* This structure is passed to NGM_NETFLOW_SETIFINDEX */
struct ng_netflow_setifindex {
	u_int16_t iface;		/* which iface index change */
	u_int16_t index;		/* new index */
};

/* This structure is passed to NGM_NETFLOW_SETTIMEOUTS */
struct ng_netflow_settimeouts {
	uint32_t	inactive_timeout;	/* flow inactive timeout */
	uint32_t	active_timeout;		/* flow active timeout */
};

/* This is unique data, which identifies flow */
struct flow_rec {
	struct in_addr	r_src;
	struct in_addr	r_dst;
	union {
		struct {
			uint16_t	s_port;	/* source TCP/UDP port */
			uint16_t	d_port; /* destination TCP/UDP port */
		} dir;
		uint32_t both;
	} ports;
	union {
		struct {
			u_char		prot;	/* IP protocol */
			u_char		tos;	/* IP TOS */
			uint16_t	i_ifx;	/* input interface index */
		} i;
		uint32_t all;
	} misc;
};

#define	r_ip_p	misc.i.prot
#define	r_tos	misc.i.tos
#define	r_i_ifx	misc.i.i_ifx
#define r_misc	misc.all
#define r_ports	ports.both
#define r_sport	ports.dir.s_port
#define r_dport	ports.dir.d_port
	
/* A flow entry which accumulates statistics */
struct flow_entry_data {
	struct flow_rec		r;
	struct in_addr		next_hop;
	uint16_t		fle_o_ifx;	/* output interface index */
#define				fle_i_ifx	r.misc.i.i_ifx
	uint8_t		dst_mask;	/* destination route mask bits */
	uint8_t		src_mask;	/* source route mask bits */
	u_long			packets;
	u_long			bytes;
	long			first;	/* uptime on first packet */
	long			last;	/* uptime on last packet */
	u_char			tcp_flags;	/* cumulative OR */
};

/*
 * How many flow records we will transfer at once
 * without overflowing socket receive buffer
 */
#define NREC_AT_ONCE		1000
#define NGRESP_SIZE		(sizeof(struct ngnf_flows) + (NREC_AT_ONCE * \
				sizeof(struct flow_entry_data)))
#define SORCVBUF_SIZE		(NGRESP_SIZE + 2 * sizeof(struct ng_mesg))

/* This struct is returned to userland, when "show cache ip flow" */
struct ngnf_flows {
	uint32_t		nentries;
	uint32_t		last;
	struct flow_entry_data	entries[0];
};

/* Everything below is for kernel */

#ifdef _KERNEL

struct flow_entry {
	struct flow_entry_data	f;

	LIST_ENTRY(flow_entry)	fle_hash;	/* entries in one hash item */
	TAILQ_ENTRY(flow_entry)	fle_work;	/* entries in work queue*/
	SLIST_ENTRY(flow_entry)	fle_free;	/* entries in free stack */
};

/* Parsing declarations */

/* Parse the info structure */
#define	NG_NETFLOW_INFO_TYPE	{			\
	{ "Bytes",	&ng_parse_uint64_type },	\
	{ "Packets",	&ng_parse_uint32_type },	\
	{ "Records used",	&ng_parse_uint32_type },\
	{ "Records free",	&ng_parse_uint32_type },\
	{ "Inactive timeout",	&ng_parse_uint32_type },\
	{ "Active timeout",	&ng_parse_uint32_type },\
	{ NULL }					\
}

/* Parse the ifinfo structure */
#define NG_NETFLOW_IFINFO_TYPE	{			\
	{ "packets",	&ng_parse_uint32_type },	\
	{ "data link type", &ng_parse_uint8_type },	\
	{ "index", &ng_parse_uint16_type },		\
	{ NULL }					\
}

/* Parse the setdlt structure */
#define	NG_NETFLOW_SETDLT_TYPE {			\
	{ "iface",	&ng_parse_uint16_type },	\
	{ "dlt",	&ng_parse_uint8_type  },	\
	{ NULL }					\
}

/* Parse the setifindex structure */
#define	NG_NETFLOW_SETIFINDEX_TYPE {			\
	{ "iface",	&ng_parse_uint16_type },	\
	{ "index",	&ng_parse_uint16_type },	\
	{ NULL }					\
}

/* Parse the settimeouts structure */
#define NG_NETFLOW_SETTIMEOUTS_TYPE {			\
	{ "inactive",	&ng_parse_uint32_type },	\
	{ "active",	&ng_parse_uint32_type },	\
	{ NULL }					\
}

/* Private hook data */
struct ng_netflow_iface {
	hook_p		hook;		/* NULL when disconnected */
	struct ng_netflow_ifinfo	info;
};

typedef struct ng_netflow_iface *iface_p;
typedef struct ng_netflow_ifinfo *ifinfo_p;

/* Structure describing our flow engine */
struct netflow {
	node_p			node;		/* link to the node itself */

	struct ng_netflow_iface	ifaces[NG_NETFLOW_MAXIFACES];	/* incoming */
	hook_p			export;		/* export data goes there */

	struct ng_netflow_info	info;
	uint32_t		flow_seq;	/* current flow sequence */

	struct callout		exp_callout;

	/* Flow cache is a big chunk of memory referenced by 'cache'.
	 * Accounting engine searches for its record using hashing index
	 * 'hash'. Expiry engine searches for its record from begining of
	 * tail queue 'expire_q'. Allocation is performed using last free
	 * stack held in singly linked list 'free_l' */
#define	CACHESIZE			65536
#define	CACHELOWAT			(CACHESIZE * 3/4)
#define	CACHEHIGHWAT			(CACHESIZE * 9/10)
	struct flow_entry		*cache;
	struct flow_hash_entry		*hash;
	TAILQ_HEAD( , flow_entry)	work_queue;
	SLIST_HEAD( , flow_entry)	free_list;
	SLIST_HEAD( , flow_entry)	expire_list;

	/* Mutexes to protect above lists */
	struct mtx			work_mtx;
	struct mtx			free_mtx;
	struct mtx			expire_mtx;

	/* ng_netflow_export_send() forms its datagram here. */
	struct netflow_export_dgram {
		struct netflow_v5_header	header;
		struct netflow_v5_record	r[NETFLOW_V5_MAX_RECORDS];
	} __attribute__((__packed__)) dgram;
};

typedef struct netflow *priv_p;

/* Header of a small list in hash cell */
struct flow_hash_entry {
	LIST_HEAD( ,flow_entry) head;
};

/* Make sure packet large enough to contain len bytes */
#define	CHECK_MLEN(m, length)	((m)->m_pkthdr.len < (length))
#define CHECK_PULLUP(m, length)	((m)->m_len < (length) && \
				(((m) = m_pullup((m),(length))) == NULL))

#define	ERROUT(x)	{ error = (x); goto done; }

/* Prototypes for netflow.c */
int	ng_netflow_cache_init(priv_p);
void	ng_netflow_cache_flush(priv_p);
void	ng_netflow_copyinfo(priv_p, struct ng_netflow_info *);
timeout_t ng_netflow_expire;
int	ng_netflow_flow_add(priv_p, struct mbuf **, iface_p);
int	ng_netflow_flow_show(priv_p, uint32_t last, struct ng_mesg *);

#endif	/* _KERNEL */
#endif	/* _NG_NETFLOW_H_ */
