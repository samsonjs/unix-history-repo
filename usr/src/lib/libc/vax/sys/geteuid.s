/*
 * Copyright (c) 1983 Regents of the University of California.
 * All rights reserved.  The Berkeley software License Agreement
 * specifies the terms and conditions for redistribution.
 */

#ifndef lint
_sccsid:.asciz	"@(#)geteuid.s	5.2 (Berkeley) %G%"
#endif not lint

#include "SYS.h"

PSEUDO(geteuid,getuid)
	movl	r1,r0
	ret		# euid = geteuid();
