/*
 * numa_stubs.c -- stub routines for libnuma for testing` on platforms
 * that don't have libnuma installed -- e.g., my laptop
 *
 * Copyright (C) 2006,2007, Hewlett-Packard
 *
 * Hacked from /usr/include/numa.h and numaif.h:
 *
 * Copyright (C) 2003,2004 Andi Kleen, SuSE Labs.
 *
 * libnuma is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; version
 * 2.1.
 *
 * libnuma is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should find a copy of v2.1 of the GNU Lesser General Public License
 * somewhere on your Linux system; if not, write to the Free Software 
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA 
 */
#include <sys/types.h>
#include <errno.h>
#include <numa.h>
#include <stdlib.h>

/* NUMA support available. If this returns a negative value all other function
   in this library are undefined. */
int numa_available(void)
{
	return -1;
}

/* Basic NUMA state */

/* Get max available node */
int numa_max_node(void)
{
	return 1;
}

/* Return preferred node */
int numa_preferred(void)
{
	return 0;
}

/* Return node size and free memory */
long numa_node_size(int node, long *freep)
{
	return -1; // for now
}

int numa_pagesize(void)
{
	return 0;	// ???
}

/* Set with all nodes. Only valid after numa_available. */
const nodemask_t numa_all_nodes = { 1 };

/* Set with no nodes */
const nodemask_t numa_no_nodes = { 0 };

/* Only run and allocate memory from a specific set of nodes. */
void numa_bind(const nodemask_t *nodes) { }

/* Set the NUMA node interleaving mask. 0 to turn off interleaving */
void numa_set_interleave_mask(const nodemask_t *nodemask) {}

/* Return the current interleaving mask */
nodemask_t numa_get_interleave_mask(void)
{
	return numa_all_nodes;
}

/* Some node to preferably allocate memory from for thread. */
void numa_set_preferred(int node) { }

/* Set local memory allocation policy for thread */
void numa_set_localalloc(void) { }

/* Only allocate memory from the nodes set in mask. 0 to turn off */
void numa_set_membind(const nodemask_t *nodemask) { }

/* Return current membind */ 
nodemask_t numa_get_membind(void)
{
	return numa_all_nodes;
}

int numa_get_interleave_node(void)
{
	return 0;
}

/* NUMA memory allocation. These functions always round to page size
   and are relatively slow. */

/* Alloc memory page interleaved on nodes in mask */ 
void *numa_alloc_interleaved_subset(size_t size, const nodemask_t *nodemask)
{
	return malloc(size);
}

/* Alloc memory page interleaved on all nodes. */
void *numa_alloc_interleaved(size_t size)
{
	return malloc(size);
}

/* Alloc memory located on node */
void *numa_alloc_onnode(size_t size, int node)
{
	return malloc(size);
}

/* Alloc memory on local node */
void *numa_alloc_local(size_t size)
{
	return malloc(size);
}

/* Allocation with current policy */
void *numa_alloc(size_t size)
{
	return malloc(size);
}

/* Free memory allocated by the functions above */
void numa_free(void *mem, size_t size)
{
	free(mem);
}

/* Low level functions, primarily for shared memory. All memory
   processed by these must not be touched yet */

/* Interleave an memory area. */
void numa_interleave_memory(void *mem, size_t size, const nodemask_t *mask) { }

/* Allocate a memory area on a specific node. */
void numa_tonode_memory(void *start, size_t size, int node) {}

/* Allocate memory on a mask of nodes. */
void numa_tonodemask_memory(void *mem, size_t size, const nodemask_t *mask) { }

/* Allocate a memory area on the current node. */
void numa_setlocal_memory(void *start, size_t size) {}

/* Allocate memory area with current memory policy */
void numa_police_memory(void *start, size_t size) {}

/* Run current thread only on nodes in mask */
int numa_run_on_node_mask(const nodemask_t *mask)
{
	return 0;
}

/* Run current thread only on node */
int numa_run_on_node(int node)
{
	return 0;
}

/* Return current mask of nodes the thread can run on */
nodemask_t numa_get_run_node_mask(void)
{
	return numa_all_nodes;
}

/* When strict fail allocation when memory cannot be allocated in target node(s). */
void numa_set_bind_policy(int strict) { }

/* Fail when existing memory has incompatible policy */
void numa_set_strict(int flag) { }

/* Convert node to CPU mask. -1/errno on failure, otherwise 0. */
int numa_node_to_cpus(int node, unsigned long *buffer, int buffer_len)
{
	return 1;
}

/* Error handling. */
/* This is an internal function in libnuma that can be overwritten by an user
   program. Default is to print an error to stderr and exit if numa_exit_on_error
   is true. */
//void numa_error(char *where); 

/* When true exit the program when a NUMA system call (except numa_available) 
   fails */ 
int numa_exit_on_error;

/* Warning function. Can also be overwritten. Default is to print on stderr
   once. */
//void numa_warn(int num, char *fmt, ...);


/* System calls - from numaif.h */
long get_mempolicy(int *policy, 
			  unsigned long *nmask, unsigned long maxnode,
			  void *addr, int flags)
{
	errno = ENOSYS;
	return 0;
}

long mbind(void *start, unsigned long len, int mode, 
		  unsigned long *nmask, unsigned long maxnode, unsigned flags)
{
	errno = ENOSYS;
	return -1;
}
long set_mempolicy(int mode, unsigned long *nmask, 
			  unsigned long maxnode)
{
	return 0;
}
