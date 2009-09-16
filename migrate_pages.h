/*
 * memtoy - migrate_pages.h - interface to migrate_pages() system call
 *
 *  Copyright (c) 2005, 2006,2007 Hewlett-Packard, Inc
 *  
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 * Also contains mbind() flags not [yet] in <numa.h>
 *
 * until merged into upstream kernels [he says hopefully] and show up
 * in distros' <numa.h>
 *
 * N.B., this header makes assumptions about the values of MPOL_MF_MOVE,
 *       '_MOVE_ALL and '_LAZY
 */
#ifndef _MEMTOY_MIGRATE_PAGES_
#define _MEMTOY_MIGRATE_PAGES_
#include <sys/types.h>
#include <numaif.h>	/* is MPOL_MF_MOVE defined? */

#ifndef MPOL_NOOP
#define MPOL_NOOP (MPOL_INTERLEAVE + 1)
#endif

#ifndef MPOL_MF_MOVE
#define MPOL_MF_MOVE    (1<<1)  /* Move existing pages, if possible */

/*
 * assume we need our own migrate_pages() sys call wrapper if '_MOVE
 * not defined in <numaif.h>
 */
#define _NEED_MIGRATE_PAGES
extern int migrate_pages(const pid_t, int, unsigned long*, unsigned long*);

#endif	/* ?def MPOL_MF_MOVE */

#ifndef MPOL_MF_MOVE_ALL
#define MPOL_MF_MOVE_ALL    (MPOL_MF_MOVE << 1)
#endif

/*
 * get_mempolicy() flag -- return mems allowed
 */
#ifndef MPOL_F_MEMS_ALLOWED
#define MPOL_F_MEMS_ALLOWED (MPOL_F_ADDR << 1)
#endif

#endif
