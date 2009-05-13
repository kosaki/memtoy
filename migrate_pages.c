/*
 * based on:
 *  Copyright (c) 2005 Silicon Graphics, Inc.
 *  All rights reserved.
 *
 *    Version 2 Jun 2005
 *
 *    Ray Bryant <raybry@sgi.com>
 *
 * Additions [for memtoy]:  Lee Schermerhorn <lee.schermerhorn@hp.com>
 */

#include "migrate_pages.h"	/* extra definitions -- not in <numa*.h> */

#ifdef _NEED_MIGRATE_PAGES
#include <sys/types.h>

#include <sys/syscall.h>          /* For NR_syscalls */
#include <stdio.h>
#include <unistd.h>             /* For __NR_ni_syscall */
#include <errno.h>

/*
 * syscall numbers keep changings as we track newer kernels.
 */
#ifndef __NR_migrate_pages
#if defined(__ia64__)
#define __NR_migrate_pages 1280 /* ia64 - 2.6.16-rc6 */
#endif
#if defined(__x86_64__)
#define __NR_migrate_pages 256 /* x86_64 - 2.6.16-rc6 */
#endif
#endif

/*
 * returns # pages NOT moved -- >= 0; or error -- < 0
 */
int
migrate_pages(const pid_t pid, int count, unsigned long *old_nodes,
		unsigned long *new_nodes)
{
#ifdef __NR_migrate_pages
	int ret;

	ret = syscall(__NR_migrate_pages, pid, count, old_nodes, new_nodes);

	return ret;
#else
	errno = ENOSYS;
	return -1;
#endif
}
#endif /* _NEED_MIGRATE_PAGES */
