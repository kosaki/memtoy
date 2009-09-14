/*
 * memtoy:  commands.c - command line interface
 *
 * A brute force/ad hoc command interpreter:
 * + parse commands [interactive or batch]
 * + convert/validate arguments
 * + some general/administrative commands herein
 * + actual segment management routines in segment.c
 */
/*
 *  Copyright (c) 2005,2006,2007 Hewlett-Packard, Inc
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
 */
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#define migratepages migrate_pages	/* fix RHEL5 header snafu */
#include <numaif.h>
#include <numa.h>
#include <sched.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <readline/readline.h>
#include <readline/history.h>

#include "memtoy.h"
#include "migrate_pages.h"

#define CMD_SUCCESS 0
#define CMD_ERROR   1

char *whitespace = " \t";

/*
 * =========================================================================
 */
static int help_me(char *);	/* forward reference */

static char program_name[128];	/* enough? */
static void
set_program_name()
{
	glctx_t *gcp = &glctx;

	if (!gcp->child_name)
		return;		/* parent */

	snprintf(program_name, 128UL, "%s(%s)", gcp->program_name,
			gcp->child_name);
	gcp->program_name = program_name;
}

/*
 * required_arg -- check for a required argument; issue message if not there
 *
 * return true if arg [something] exists; else return false
 */
static bool
required_arg(char *arg, char *arg_name)
{
	glctx_t *gcp = &glctx;

	if(*arg != '\0')
		return true;

	fprintf(stderr, "%s:  command '%s' missing required argument: %s\n\n",
		gcp->program_name, gcp->cmd_name, arg_name);
	help_me(gcp->cmd_name);

	return false;
}

/*
 * get_arg_seconds - parse arg as time interval in seconds.
 * TODO:  support hh:mm:ss format?
 */
#define BOGUS_SECONDS ((unsigned int)-1)
static int
get_arg_seconds(char *arg)
{
	unsigned long argval;
	char *next;

	argval = strtoul(arg, &next, 0);
	if (argval > UINT_MAX)
		return BOGUS_SECONDS;

	return (unsigned int)argval;
}

/*
 *  size_kmgp() -- convert ascii arg to numeric and scale as requested
 */
static size_t
size_kmgp(char *arg)
{
	size_t argval;
	char *next;

	argval = strtoul(arg, &next, 0);
	if (*next == '\0')
		return argval;

	switch (tolower(*next)) {
	case 'p':	/* pages */
		argval *= glctx.pagesize;
		break;

	case 'k':
		argval <<= KILO_SHIFT;
		break;

	case 'm':
		argval <<= KILO_SHIFT * 2;
		break;

	case 'g':
		argval <<= KILO_SHIFT * 3;
		break;

	default:
		return BOGUS_SIZE;	/* bogus chars after number */
	}

	return argval;
}

static size_t
get_scaled_value(char *args, char *what)
{
	glctx_t *gcp = &glctx;
	size_t size = size_kmgp(args);

	if (size == BOGUS_SIZE) {
		fprintf(stderr, "%s:  segment %s must be numeric value"
		" followed by optional k, m, g or p [pages] scale factor.\n",
			gcp->program_name, what);
	}

	return size;
}

static int
get_range(char *args, range_t *range, char **nextarg)
{
	glctx_t *gcp = &glctx;

	if (isdigit(*args)) {
		char *nextarg;

		args = strtok_r(args, whitespace, &nextarg);
		range->offset = get_scaled_value(args, "offset");
		if (range->offset == BOGUS_SIZE)
			return CMD_ERROR;
		args = nextarg + strspn(nextarg, whitespace);

		/*
		 * <length> ... only if offset specified
		 */
		if (*args != '\0') {
			args = strtok_r(args, whitespace, &nextarg);
			if (*args != '*') {
				range->length = get_scaled_value(args, "length");
				if (range->length == BOGUS_SIZE)
					return CMD_ERROR;
			} else
				range->length = 0;	/* map to end of file */
			args = nextarg + strspn(nextarg, whitespace);
		} else {
			fprintf(stderr, "%s:  expected <length> after <offset>\n",
				gcp->program_name);
			return CMD_ERROR;
		}
	}

	*nextarg = args;
	return CMD_SUCCESS;
}

/*
 * get_shared() - check for "shared"|"private"
 * return corresponding MAP_ flag or zero [no arg]
 */
static int
get_shared(char *args)
{
	glctx_t *gcp = &glctx;
	int segflag = 0;

	if (!strncasecmp(args, "shared", strlen(args)))
		segflag = MAP_SHARED;
	else if (*args != '\0') {
		if (strncasecmp(args, "private", strlen(args))) {
			fprintf(stderr, "%s:  anon seg access type must be one of:  "
				"'private' or 'shared'\n", gcp->program_name); 
			return -1;
		} else
			segflag = MAP_PRIVATE;
		
	}
	return segflag;
}

/*
 * get_access() - check args for 'read'\'write'
 * return:
 */
#define AXCS_READ 1
#define AXCS_WRITE 2
#define AXCS_ERR 0

static int
get_access(char *args)
{
	glctx_t *gcp = &glctx;
	int axcs = AXCS_READ;
	int  len = strlen(args);

	if(tolower(*args) == 'w')
		axcs = AXCS_WRITE;
	else if(len != 0 && tolower(*args) != 'r') {
		fprintf(stderr, "%s:  segment access must be 'r[ead]' or 'w[rite]'\n",
			 gcp->program_name);
		return AXCS_ERR;
	}

	return axcs;
}

static bool
numa_supported(glctx_t *gcp)
{
	if (gcp->numa_max_node <= 0) {
		fprintf(stderr, "%s:  no NUMA support on this platform\n",
			gcp->program_name);
		return false;
	}
	return true;
}

/*
 * refresh_mems_allowed() -- update memtoy global context with allowed memories.
 *	use libnuma function 'numa_get_membind()' by default.
 *	However, numa_get_membind() just returns all nodes in the system.
 *	It does not consider cpuset constraints.
 *
 *	Optionally, build to use the get_mempolicy() MPOL_F_MEMS_ALLOWED. The
 *	call to get_mempolicy() with this flag will fail if the kernel does not
 *	support that flag.  Also, this experimental version uses knowledge of
 *	the internals of nodemask_t.
 */
static void
refresh_mems_allowed(glctx_t *gcp)
{
	int ret;

	if (numa_supported(gcp)) {
		if (!gcp->mems_allowed)
			gcp->mems_allowed =
			 (nodemask_t *)calloc(1, sizeof(nodemask_t));
#ifndef _USE_MPOL_F_MEMS_ALLOWED
		*(gcp->mems_allowed) = numa_get_membind();
#else
		ret = get_mempolicy(NULL, gcp->mems_allowed->n, 
					NUMA_NUM_NODES, NULL,
					MPOL_F_MEMS_ALLOWED);
		if (ret)
			fprintf(stderr,
			 "%s:  get_mempolicy(MPOL_F_MEMS_ALLOWED) failed: %s\n",
			gcp->program_name, strerror(errno));

#endif
	}
}

static void
refresh_cpus_allowed(glctx_t *gcp)
{
	if (!gcp->cpus_allowed)
		gcp->cpus_allowed =
		  (cpu_set_t *)calloc(1, sizeof(cpu_set_t));

	(void)sched_getaffinity(0, sizeof(*(gcp->cpus_allowed)),
		gcp->cpus_allowed);
}

/*
 * get_cpuset_from_mask() - get cpuset from a comma-separated
 * list of hex masks, up to 32-bits per "chunk"
 *
 * This function depends on internals of cpu_set_t :-(
 *
 * N.B., caller must free returned cpuset
 */
#define CPUBITS (8 * sizeof(unsigned int))
#define NCPUWORDS (CPU_SETSIZE / CPUBITS )
static int
get_cpuset_from_mask(char *args, cpu_set_t **cspp)
{
	glctx_t    *gcp = &glctx;
	cpu_set_t  *csp = (cpu_set_t *)calloc(1, sizeof(cpu_set_t));
	char       *next;
	unsigned int *cpu_bits;
	unsigned int chunk[NCPUWORDS];
	int          i, nr_chunks = 0;

	/*
	 * parse chunks
	 */
	while (*args != '\0') {
		unsigned int bits;

		if (*args != '0' && tolower(*(args+1)) != 'x') {
			fprintf(stderr, "%s:  expected hex mask for <cpu-list>",
				gcp->program_name);
			goto out_err;
		}

		if (nr_chunks >= NCPUWORDS) {
			fprintf(stderr, "%s:  too many cpus [%d max]\n",
				gcp->program_name, gcp->max_cpu_id);
			goto out_err;
		}

		bits = strtoul(args, &next, 16);
		/*
		 * ignore "leading" zero chunks
		 */
		if (nr_chunks || bits)
			chunk[nr_chunks++] = bits;

		if (*next == '\0') {
			break;
		}

		if (*next != ',') {
			fprintf(stderr,
				"%s:  expected ',' after cpu mask\n",
				gcp->program_name);
			goto out_err;
		}
		args = next+1;
	}

// TODO:  trap nr_chunks == 0 -- i.e., empty cpuset?	

	/*
	 * Transfer chunks to cpuset, using knowledge of
	 * cpu_set_t internals.  Note that chunks are in
	 * reverse order of cpu set words.
	 */
	cpu_bits = (unsigned int *)csp->__bits;
	for (i=0;  nr_chunks > 0; ++i)
		cpu_bits[i] = chunk[--nr_chunks];

	*cspp = csp;
	return i;

out_err:
	free(csp);
	return -1;
}

/*
 * get_cpuset_from_ids() -- get cpuset from comma-separated
 * list of cpu ids.
 *
 * N.B., caller must free returned cpuset
 */
static int
get_cpuset_from_ids(char *args, cpu_set_t **cspp)
{
	glctx_t    *gcp = &glctx;
	cpu_set_t  *csp = (cpu_set_t *)calloc(1, sizeof(cpu_set_t));
	char       *next = NULL;
	int         cpu, nr_cpus = 0;

	while (*args != '\0') {
		if (!isdigit(*args)) {
			fprintf(stderr, "%s:  expected digit for <cpu-list>\n",
				gcp->program_name);
			next = args;
			goto out_err;
		}

		cpu = strtoul(args, &next, 10);

		if (cpu > gcp->max_cpu_id) {
			fprintf(stderr, "%s:  cpu ids must be <= %d\n",
				gcp->program_name, gcp->max_cpu_id);
			goto out_err;
		}

		CPU_SET(cpu, csp);
		++nr_cpus;

		if (*next == '\0') {
			*cspp = csp;
			return nr_cpus;
		}
		if (*next != ',') {
			break;
		}
		args = next+1;
	}

out_err:
	if (next)
		fprintf(stderr, "%s:  unexpected character '%c' in cpu list\n",
			gcp->program_name, *next);

	free(csp);
	return -1;
}

static struct policies {
	char *pol_name;
	int   pol_flag;
} policies[] =
{
	{"default",     MPOL_DEFAULT},
	{"preferred",   MPOL_PREFERRED},
	{"bind",        MPOL_BIND},
	{"interleaved", MPOL_INTERLEAVE},
	{"noop",        MPOL_NOOP},
	{NULL, -1}
};

/*
 * get_mbind_policy() - parse <policy> argument to mbind command
 *
 * format:  <mpol>[+<flags>]
 * <mpol> is one of the policies[] above.
 * '+<flags>' = modifiers to mbind() call.  parsed by get_mbind_flags()
 */
static const char *mbind_reject = " +	";  /* space, '+', tab */
static int
get_mbind_policy(char *args, char **nextarg)
{
	glctx_t *gcp = &glctx;
	struct policies *polp;
	char            *pol;

	pol = args;
	args += strcspn(args, mbind_reject);

	for( polp = policies; polp->pol_name != NULL; ++polp) {
		size_t plen = args - pol;

		if (strncmp(pol, polp->pol_name, plen))
			continue;

		*nextarg = args;
		return polp->pol_flag;
	}

	fprintf(stderr, "%s:  unrecognized policy %s\n",
		gcp->program_name, pol);
	return CMD_ERROR;
}

/*
 * get_mbind_flags() - parse mbind(2) modifier flags
 *
 * format: [+shared][+move[+all][+lazy]]
 * 'shared' apply shared policy to shared file mappings. 
 * 'move'   specifies that currently allocated pages mapped by this process
 *          should be migrated.  => MPOL_MF_MOVE
 * 'all'    modifies 'move'.  specifies that pages be moved, even if they
 *          are mapped by more than one process/mm/vma => MPOL_MF_MOVE_ALL
 * 'lazy'   modifies 'move' or 'move'+'all'
 *
 * returns flags on success; -1 on error
 */
static int
get_mbind_flags(char *args, char **nextarg)
{
	glctx_t *gcp = &glctx;
	char    *arg;
	int      flags = 0, move_mod=0;

	arg = args;
	args += strcspn(args, mbind_reject);

	/*
	 * "shared" must be first flag, if specified
	 */
	if (!strncmp(arg, "shared", args-arg)) {
		flags |= MPOL_MF_SHARED;
		if (*args !=  '+')
			goto flags_ok;
		arg = ++args;
		args += strcspn(args, mbind_reject);
	}

	/*
	 * "move" must be next flag, if any
	 */
	if (strncmp(arg, "move", args-arg))
		goto flags_err;

	flags |= MPOL_MF_MOVE;

	/*
	 * look for 'all' and/or 'lazy'
	 */
	while (*args == '+') {
		++args;
		if (*args == '\0' || *args == ' ') {
			fprintf(stderr,
				"%s:  expected 'all' or 'lazy' after '+'\n",
				gcp->program_name);
			return -1;
		}
		arg = args;
		args += strcspn(arg, mbind_reject);
		if (!strncmp(arg, "all", args-arg)) {
			flags &= ~MPOL_MF_MOVE;
			flags |= MPOL_MF_MOVE_ALL;
		} else if (!strncmp(arg, "lazy", args-arg))
			flags |= MPOL_MF_LAZY;
		else
			goto flags_err;

	}

flags_ok:

	*nextarg = args;
	return flags;

flags_err:
	/*
	 * try to give reasonable messages, but ...
	 */
	move_mod = !strncmp(arg, "all", args-arg) ||
		   !strncmp(arg, "lazy", args-arg);
	if (flags & MPOL_MF_MOVE) {
			fprintf(stderr, "%s: expected 'all' or 'lazy' after "
					"'move'\n", gcp->program_name, arg);
	} else {
		if (move_mod)
			fprintf(stderr, "%s:  expected 'move' before: %s\n",
				gcp->program_name, arg);
		else
			fprintf(stderr, "%s: unrecognized mbind flag: %s\n",
				gcp->program_name, arg);
	}
	return -1;

}

static void
nodelist_error(glctx_t *gcp, char bogus)
{
	fprintf(stderr, "%s:  unexpected character '%c' in node list\n",
			gcp->program_name, bogus);
}

/*
 * get_nodemask() -- get nodemask from comma-separated list of node ids.
 *
 * N.B., caller must free returned nodemask
 */
static int
get_nodemask(char *args, nodemask_t **nmpp)
{
	glctx_t    *gcp = &glctx;
	nodemask_t *nmp = (nodemask_t *)calloc(1, sizeof(nodemask_t));
	char       *next = NULL;
	int         node, nr_nodes = 0;
	while (*args != '\0') {
		if (!isdigit(*args)) {
			fprintf(stderr, "%s:  expected digit for <node/list>\n",
				gcp->program_name);
			next = args;
			goto out_err;
		}

		node = strtoul(args, &next, 10);

		if (node > gcp->numa_max_node) {
			fprintf(stderr, "%s:  node ids must be <= %d\n",
				gcp->program_name, gcp->numa_max_node);
			goto out_err;
		}

		nodemask_set(nmp, node);
		++nr_nodes;

		if (*next == '\0') {
			*nmpp = nmp;
			return nr_nodes;
		}
		if (*next != ',') {
			break;
		}
		args = next+1;
	}

out_err:
	if (next)
		nodelist_error(gcp, *next);

	free(nmp);
	return -1;
}

#if 0 // was for early  migrate_pages() API arguments
/*
 * get_arg_nodeid_list() -- get list [array] of node ids from comma-separated list.
 *
 * on success, returns count of id's in list; on error -1
 */
static int
get_arg_nodeid_list(char *args, unsigned int *list)
{
	glctx_t    *gcp = &glctx;
	char       *next;
	int         node, count = 0;

	refresh_mems_allowed(gcp);
	while (*args != '\0') {
		if (!isdigit(*args)) {
			fprintf(stderr, "%s:  expected digit for <node/list>\n",
				gcp->program_name);
			return -1;
		}

		node = strtoul(args, &next, 10);

		if (node > gcp->numa_max_node) {
			fprintf(stderr, "%s:  node ids must be <= %d\n",
				gcp->program_name, gcp->numa_max_node);
			return -1;
		}

		if (!nodemask_isset(gcp->mems_allowed, node)) {
			fprintf(stderr, "%s:  node %d is not in my allowed node mask\n",
				gcp->program_name, node);
			return -1;
		}

		*(list + count++) = node;

		if (*next == '\0')
			return count;
		if (*next != ',') {
			break;
		}

		if (count >= gcp->numa_max_node) {
			fprintf(stderr, "%s:  too many node ids in list\n",
				gcp->program_name);
		}
		args = next+1;
	}

	return -1;
}
#endif

/*
 * get_arg_nodemask - parse nodemask argument if required
 */
static int
get_arg_nodemask(char *args, int policy, nodemask_t **nmpp)
{
	glctx_t *gcp = &glctx;
	int         nr_nodes = 0;

	*nmpp = NULL;
	if (policy != MPOL_DEFAULT && policy != MPOL_NOOP) {
		char c;
		if (!required_arg(args, "<node/list>"))
			return -1;
		if (*args != '*')	/* use NULL/empty nodemask */
			return get_nodemask(args, nmpp);
		else if ((c = *(++args)) != '\0') {
			nodelist_error(gcp, c);
			return -1;
		} else if (policy != MPOL_INTERLEAVE &&
				 policy != MPOL_PREFERRED) {
			fprintf(stderr,
				"%s: '*' valid only for interleave "
				"and preferred policies\n",
				 gcp->program_name);
			return -1;
		}
	}
	return 0;
}

/*
 * get_current_nodemask() -- return current thread's allowed node mask
 * via nodemask pointer arg, and # of nodes in mask via return value.
 */
static char *empty_mask="%s:  my allowed node mask is empty !!???\n";
static int
get_current_nodemask(nodemask_t **nmpp)
{
	glctx_t    *gcp = &glctx;
	int        nr_nodes = 0, max_node = gcp->numa_max_node;
	int        node;

	refresh_mems_allowed(gcp);
//TODO ?? hweight()
	for (node=0; node <= max_node; ++node) {
		if (nodemask_isset(gcp->mems_allowed, node))
			++nr_nodes;
	}

	/*
	 * shouldn't happen, but let 'em know if it does
	 */
	if (nr_nodes == 0)
		fprintf(stderr, empty_mask, gcp->program_name);

	/*
	 * return a freeable copy
	 */
	
	*nmpp = calloc(1, sizeof(**nmpp));
	**nmpp = *(gcp->mems_allowed);
	return nr_nodes;
}

/*
 * get_current_nodeid_list() - fill arg array with nodes from
 * current thread's allowed node mask.  return # of nodes in 
 * mask.
 */
static int
get_current_nodeid_list(unsigned int *fromids)
{
	glctx_t    *gcp = &glctx;
	int        nr_nodes = 0, max_node = gcp->numa_max_node;
	int        node;

	refresh_mems_allowed(gcp);
	for (node=0; node <= max_node; ++node) {
		if (nodemask_isset(gcp->mems_allowed, node))
			*(fromids + nr_nodes++) = node;
	}

	/*
	 * shouldn't happen, but let 'em know if it does
	 */
	if (nr_nodes == 0)
		fprintf(stderr, empty_mask, gcp->program_name);

	return nr_nodes;
}

static void
not_implemented()
{
	glctx_t *gcp = &glctx;

	fprintf(stderr, "%s:  %s not implemented yet\n",
		gcp->program_name, gcp->cmd_name);
}

/*
 * =========================================================================
 */

/*
 * command:  quit
 */
static void child_respond(void);
static int
quit(char *args)
{
	glctx_t *gcp = &glctx;

	child_respond();
	exit(0);	/* let cleanup() do its thing */
}

/*
 * command:  pid
 */
static int
show_pid(char *args)
{
	glctx_t *gcp = &glctx;
	
	printf("%s:  pid = %d\n", gcp->program_name, getpid());

	return CMD_SUCCESS;
}

/*
 * command:  pause
 */
static int
pause_me(char *args)
{
	// glctx_t *gcp = &glctx;
	
	pause();
	reset_signal();

	return CMD_SUCCESS;
}

/*
 * command:  numa
 */
static char *numa_header =
"  Node  Total Mem[MB]  Free Mem[MB]\n";
static int
numa_info(char *args)
{
	glctx_t      *gcp = &glctx;
	unsigned int *nodeids;
	int           nr_nodes, i;
	bool          do_header = true;
	
	if (!numa_supported(gcp))
		return CMD_ERROR;

	nodeids   = calloc(gcp->numa_max_node, sizeof(*nodeids));
	nr_nodes  = get_current_nodeid_list(nodeids);
	if(nr_nodes < 0)
		return CMD_ERROR;

	for(i=0; i < nr_nodes; ++i) {
		int  node = nodeids[i];
		long node_size, node_free;

		node_size = numa_node_size(node, &node_free);
		if (node_size < 0) {
			fprintf(stderr, "%s:  numa_node_size() failed for node %d\n",
				gcp->program_name, node);
			return CMD_ERROR;
		}

		if (do_header) {
			do_header = false;
			printf(numa_header);
		}
		printf("  %3d  %9ld      %8ld\n", node,
			 node_size/(1024*1024), node_free/(1024*1024));
	}

	return CMD_SUCCESS;
}

/*
 * command:  migrate <to-node-id[s]> [<from-node-id[s]>]
 *
 * Node id[s] - single node id or comma-separated list
 * <to-node-id[s]> - 1-for-1 with <from-node-id[s]>, OR
 * if <from-node-id[s]> omitted, <to-node-id[s]> must be
 * a single node id.
 */
static int
migrate_process(char *args)
{
	glctx_t       *gcp = &glctx;
	nodemask_t     *from_nodes = NULL, *to_nodes = NULL;
	char          *idlist, *nextarg;
	struct timeval t_start, t_end;
	int            nr_to, nr_from;
	int            nr_not_migrated;
	int            ret = CMD_ERROR;
	
	if (!numa_supported(gcp))
		return CMD_ERROR;

	/*
	 * <to-node-id[s]>
	 */
	if (!required_arg(args, "<to-node-id[s]>"))
		return CMD_ERROR;
	idlist = strtok_r(args, whitespace, &nextarg);
	nr_to = get_nodemask(idlist, &to_nodes);
	if (nr_to < 0)
		goto out_free;
	args = nextarg + strspn(nextarg, whitespace);

	if (*args != '\0') {
		/*
		 * apparently, <from-node-id[s]> present
		 */
		idlist = strtok_r(args, whitespace, &nextarg);
		nr_from = get_nodemask(idlist, &from_nodes);
		if (nr_from < 0)
			goto out_free;
	} else {
		int i;

#if 0
// nonintersection no longer a requirement.
		/*
		 * no <from-node-id[s]>, nr_to must == 1,
		 * get fromids from memory policy.
		 */
		if(nr_to > 1) {
			fprintf(stderr, "%s:  # to ids must = 1"
				" when no 'from' ids specified\n",
				gcp->program_name);
			goto out_free;
		}
#endif

		/*
		 * use current node mask as from_nodes
		 */
		nr_from = get_current_nodemask(&from_nodes);
		if(nr_from <= 0)
			goto out_free;	/* shouldn't happen! */

	}

	gettimeofday(&t_start, NULL);
	nr_not_migrated = migrate_pages(getpid(), NUMA_NUM_NODES,
					 from_nodes->n, to_nodes->n);
	if (nr_not_migrated < 0) {
		int err = errno;
		fprintf(stderr, "%s: migrate_pages() failed - %s\n",
			gcp->program_name, strerror(err));
		goto out_free;
	}
	gettimeofday(&t_end, NULL);

	printf("%s:  migration took %6.3fsecs.  %d pages could not be migrated\n",
		gcp->program_name,
		(float)(tv_diff_usec(&t_start, &t_end))/1000000.0,
		nr_not_migrated);
	ret = CMD_SUCCESS;

out_free:
	free(to_nodes);
	free(from_nodes);
	return ret;
}

/*
 * command:  show [<seg-name>|'+']
 */
static int
show_seg(char *args)
{
	glctx_t *gcp = &glctx;
	
	char *segname = NULL, *nextarg;
	
	args += strspn(args, whitespace);
	if (*args != '\0')
		segname = strtok_r(args, whitespace, &nextarg);

	if (!segment_show(segname))
		return CMD_ERROR;

	return CMD_SUCCESS;
}

/*
 * command:  anon <seg-name> <size>[kmgp] [private|shared] [addr=<addr>] [offset=<segment>]
 */
static int
anon_seg(char *args)
{
	glctx_t *gcp = &glctx;
	
	char    *segname, *nextarg;
	range_t  range = { 0L, 0L };
	int      segflag = MAP_PRIVATE;

	args += strspn(args, whitespace);

	if(!required_arg(args, "<seg-name>"))
		return CMD_ERROR;
	segname = strtok_r(args, whitespace, &nextarg);
	args = nextarg + strspn(nextarg, whitespace);

	if(!required_arg(args, "<size>"))
		return CMD_ERROR;
	args = strtok_r(args, whitespace, &nextarg);
	range.length = get_scaled_value(args, "size");
	if (range.length == BOGUS_SIZE)
		return CMD_ERROR;
	args = nextarg + strspn(nextarg, whitespace);

	/* optional args */
	while (*args != '\0') {
		char *value;
		char *name;

		args = strtok_r(args, whitespace, &nextarg);

		if (!strncasecmp(args, "shared", strlen(args))) {
			segflag = MAP_SHARED;
			goto next;
		}
		if (!strncasecmp(args, "private", strlen(args))) {
			segflag = MAP_PRIVATE;
			goto next;
		}

		/* name=value argument */
		name = strtok_r(args, "=", &value);
		if (!strncasecmp(name, "addr", strlen(args))) {
			range.offset = strtol(value, NULL, 16);
			goto next;
		}
		if (!strncasecmp(name, "offset", strlen(args))) {
			range_t segrange;
			int err;

			if (segment_range(value, &segrange) == NULL) {
				fprintf(stderr, "offset must be existing segment\n");
				return err;
			}
			range.offset = segrange.offset + segrange.length;
			goto next;
		}
	next:
		args = nextarg + strspn(nextarg, whitespace);		
	}

	if (!segment_register(SEGT_ANON, segname, &range, segflag))
		return CMD_ERROR;

	return CMD_SUCCESS;
}

/*
 * command:  file  <path-name> [<offset>[kmgp] <length>[kmgp]  [private|shared]]
 */
static int
file_seg(char *args)
{
	glctx_t *gcp = &glctx;
	
	char *pathname, *nextarg;
	range_t range = { 0L, 0L };
	int  segflag = MAP_PRIVATE;

	args += strspn(args, whitespace);

	if(!required_arg(args, "<path-name>"))
		return CMD_ERROR;
	pathname = strtok_r(args, whitespace, &nextarg);
	args = nextarg + strspn(nextarg, whitespace);

	/*
	 * offset, length are optional
	 */
	if (get_range(args, &range, &nextarg) == CMD_ERROR)
		return CMD_ERROR;
	args = nextarg;

	if (*args != '\0') {
		segflag = get_shared(args);
		if (segflag == -1)
			return CMD_ERROR;
	}

	if (!segment_register(SEGT_FILE, pathname, &range, segflag))
		return CMD_ERROR;

	return CMD_SUCCESS;
}

/*
 * command:  remove  <seg-name> [<seg-name> ...]
 */
static int
remove_seg(char *args)
{
	glctx_t *gcp = &glctx;

	args += strspn(args, whitespace);
	if(!required_arg(args, "<seg-name>"))
		return CMD_ERROR;

	while (*args != '\0') {
		char *segname, *nextarg;

		segname = strtok_r(args, whitespace, &nextarg);
		args = nextarg + strspn(nextarg, whitespace);

		if (!segment_remove(segname))
			return CMD_ERROR;
	}
	return CMD_SUCCESS;
	
}

/*
 * command:  touch <seg-name> [<offset> <length>] [read|write]
 */
static int
touch_seg(char *args)
{
	glctx_t *gcp = &glctx;

	char *segname, *nextarg;
	range_t range = { 0L, 0L };
	int axcs;

	args += strspn(args, whitespace);
	if(!required_arg(args, "<seg-name>"))
		return CMD_ERROR;
	segname = strtok_r(args, whitespace, &nextarg);
	args = nextarg + strspn(nextarg, whitespace);

	/*
	 * offset, length are optional
	 */
	if (get_range(args, &range, &nextarg) == CMD_ERROR)
		return CMD_ERROR;
	args = nextarg;

	axcs = get_access(args);
	if (axcs == AXCS_ERR)
		return CMD_ERROR;

	if (!segment_touch(segname, &range, (axcs == AXCS_WRITE)))
		return CMD_ERROR;

	return CMD_SUCCESS;
}

/*
 * command:  unmap <seg-name> 
 *
 * unmap specified segment, but remember name/size/...
 */
static int
unmap_seg(char *args)
{
	glctx_t *gcp = &glctx;
	char *segname, *nextarg;

	args += strspn(args, whitespace);
	if(!required_arg(args, "<seg-name>"))
		return CMD_ERROR;
	segname = strtok_r(args, whitespace, &nextarg);
	args = nextarg + strspn(nextarg, whitespace);

	if(!segment_unmap(segname))
		return CMD_ERROR;
	
	return CMD_SUCCESS;
}

/*
 * command:  map <seg-name> [<offset>[k|m|g|p] <length>[k|m|g|p]] [<seg-share>]
 */
static int
map_seg(char *args)
{
	glctx_t *gcp = &glctx;

	char    *segname, *nextarg;
	range_t  range = { 0L, 0L };
	range_t *rangep = NULL;
	int      segflag = 0;

	args += strspn(args, whitespace);
	if(!required_arg(args, "<seg-name>"))
		return CMD_ERROR;
	segname = strtok_r(args, whitespace, &nextarg);
	args = nextarg + strspn(nextarg, whitespace);

	/*
	 * offset, length are optional
	 */
	if (get_range(args, &range, &nextarg) == CMD_ERROR)
		return CMD_ERROR;
	if (args != nextarg) {
		rangep = &range;	/* override any registered range */
		args = nextarg;
	}

	if (*args != '\0') {
		segflag = get_shared(args);
		if (segflag == -1)
			return CMD_ERROR;
	}

	if (!segment_map(segname, rangep, segflag))
		return CMD_ERROR;

	return CMD_SUCCESS;
}

/*
 * command:  mbind <seg-name> [<offset>[kmgp] <length>[kmgp]] <policy> <node-list>
 *
 * <seg-name> can be '-' [place holder] if <policy> includes 'move+all' 
 * flags.
 */
static int
mbind_seg(char *args)
{
	glctx_t *gcp = &glctx;

	char       *segname, *nextarg, c;
	range_t     range = { 0L, 0L };
	nodemask_t *nodemask = NULL;
	int         nr_nodes = 0;
	int         policy, flags = 0;
	int         ret;

	if (!numa_supported(gcp))
		return CMD_ERROR;

	args += strspn(args, whitespace);
	if(!required_arg(args, "<seg-name>"))
		return CMD_ERROR;
	segname = strtok_r(args, whitespace, &nextarg);
	args = nextarg + strspn(nextarg, whitespace);

	/*
	 * offset, length are optional
	 */
	if (get_range(args, &range, &nextarg) == CMD_ERROR)
		return CMD_ERROR;
	args = nextarg;
	

	if(!required_arg(args, "<policy>"))
		return CMD_ERROR;
	policy = get_mbind_policy(args, &nextarg);
	if (policy < 0)
		return CMD_ERROR;

	args = nextarg + strspn(nextarg, whitespace);
	if (*args == '+') {
		flags = get_mbind_flags(++args, &nextarg);
		if (flags == -1)
			return CMD_ERROR;
	}
	args = nextarg + strspn(nextarg, whitespace);

	nr_nodes = get_arg_nodemask(args, policy, &nodemask);
	if (nr_nodes < 0)
		return CMD_ERROR;

	ret = CMD_SUCCESS;
#if 1	// for testing
	if (!segment_mbind(segname, &range, policy, nodemask, flags))
		ret = CMD_ERROR;
#endif

	if (nodemask != NULL)
		free(nodemask);
	return ret;
}

/*
 *  command:  shmem <seg-name> <seg-size>[k|m|g|p] [huge]
 *
 * create [shmget] and register a SysV shared memory segment
 * of specified size
 */
static int
shmem_seg(char *args)
{
	glctx_t *gcp = &glctx;
	
	char *segname, *nextarg;
	range_t range = { 0L, 0L };
	int segflag = MAP_SHARED;

	args += strspn(args, whitespace);

	if(!required_arg(args, "<seg-name>"))
		return CMD_ERROR;
	segname = strtok_r(args, whitespace, &nextarg);
	args = nextarg + strspn(nextarg, whitespace);

	if(!required_arg(args, "<size>"))
		return CMD_ERROR;
	args = strtok_r(args, whitespace, &nextarg);
	range.length = get_scaled_value(args, "size");
	if (range.length == BOGUS_SIZE)
		return CMD_ERROR;
	args = nextarg + strspn(nextarg, whitespace);

	if (*args) {
		char *option = args;
		option = strtok_r(args, whitespace, &nextarg);
		if (!strncasecmp(option, "huge", strlen(option)))
			segflag |= MEMTOY_MAP_HUGE;
		args = nextarg + strspn(nextarg, whitespace);
	}

	if (!segment_register(SEGT_SHM, segname, &range, segflag))
		return CMD_ERROR;

	return CMD_SUCCESS;
}

/*
 * command:  where <seg-name> [<offset>[kmgp] <length>[kmgp]]  
 *
 * show node location of specified range of segment.
 *
 * NOTE: if neither <offset> nor <length> specified, <offset> defaults
 * to 0 [start of segment], as usual, and length defaults to 64 pages 
 * rather than the entire segment.  Suitable for a "quick look" at where
 * segment resides.
 */
static int
where_seg(char *args)
{
	glctx_t *gcp = &glctx;
	
	char  *segname, *nextarg;
	range_t range = { 0L, 0L };
	int    ret;

	if (!numa_supported(gcp))
		return CMD_ERROR;

	args += strspn(args, whitespace);
	if(!required_arg(args, "<seg-name>"))
		return CMD_ERROR;
	segname = strtok_r(args, whitespace, &nextarg);
	args = nextarg + strspn(nextarg, whitespace);

	/*
	 * offset, length are optional
	 */
	if (get_range(args, &range, &nextarg) == CMD_ERROR)
		return CMD_ERROR;
	if (args == nextarg) 
		range.length = DEFAULT_LENGTH;

	if(!segment_location(segname, &range))
		return CMD_ERROR;

	return CMD_SUCCESS;
}

/*
 * show_cpus() displays cpu afinity mask from global context
 * from highest mask word with bits set, to lowest.
 *
 * Note:  use 32-bit masks for compatibility between 32- and
 *        64-bit systems.
 */
#define MASKBUFLEN 32	/* only need ~20 */
static void
show_cpus(glctx_t *gcp)
{
	static char *header = "%s:  cpu affinity mask:";
	unsigned int *cpu_bits;
	size_t linelen;
	int i, any = 0;
	char maskbuf[MASKBUFLEN];

	cpu_bits = (unsigned int *)gcp->cpus_allowed->__bits;

	printf(header, gcp->program_name);
	linelen = strlen(header) + strlen(gcp->program_name) - 2;

	for (i = NCPUWORDS - 1; i >= 0; --i) {
		size_t maskbuflen;
		unsigned int bits;

		bits = cpu_bits[i];
		if (!bits && !any)
			continue;	/* suppress leading zeros */

		++any;	/* we've seen some cpu affinity bits */

		snprintf(maskbuf, MASKBUFLEN, " 0x%8.8x", bits);
		maskbuflen = strlen(maskbuf);
		if ((linelen + maskbuflen) >= MAXCOL) {
			printf("        \n");
			linelen = 8;	/* number of spaces above */
		}
		printf(" %s", maskbuf);
		linelen += maskbuflen;
	}
	printf("\n");
}

/*
 * command:  cpus [{0x<mask>|<cpu-list>}]
 */
static int
cpus(char *args)
{
	glctx_t *gcp = &glctx;
	cpu_set_t *cpuset;
	int        ret;

	args += strspn(args, whitespace);
	if (!*args)
		goto show_cpus;

	if (*args == '0' && tolower(*(args+1)) == 'x') {
		char *next;
		ret = get_cpuset_from_mask(args, &cpuset);
		if (ret < 0)
			return CMD_ERROR;
	} else {
		ret = get_cpuset_from_ids(args, &cpuset);
		if (ret < 0)
			return CMD_ERROR;
	}

	(void)sched_setaffinity(0, sizeof(*(gcp->cpus_allowed)), cpuset);
	free(cpuset);

show_cpus:
	refresh_cpus_allowed(gcp);	/* what the kernel thinks */
	show_cpus(gcp);

	return CMD_SUCCESS;
}

/*
 * command:  mems [{0x<mask>|<node-list>}]
 */
static int
mems(char *args)
{
	glctx_t *gcp = &glctx;
	nodemask_t *nodemask = NULL;
	int         nr_nodes;

	args += strspn(args, whitespace);
	if (!*args)
		goto show_mems;
	
	if (*args == '0' && tolower(*(args+1)) == 'x') {
		char *next;
		nodemask = calloc(1, sizeof(*nodemask));
//TODO:  support >1 word of mask? and stop using internals of nodemask_t!!! 
		nodemask->n[0] = strtoul(args, &next, 0);
		if (!nodemask->n[0] || *next) {
			fprintf(stderr, "%s:  bogus mem mask:  %s  \n",
				gcp->program_name, args);
			return CMD_ERROR;
		}
	} else {
		nr_nodes = get_nodemask(args, &nodemask);
		if (nr_nodes < 0)
			return CMD_ERROR;
	}

	numa_set_membind(nodemask);	/* void fcn */
	free(nodemask);

show_mems:
	refresh_mems_allowed(gcp);

//TODO:  display > 1 word of mems_allowed, if populated.
// see show_cpus()
	printf("%s:  mems allowed = 0x%lx\n",
		gcp->program_name, gcp->mems_allowed->n[0]);

	return CMD_SUCCESS;
}

/*
 *  common argument parsing for lock()/unlock() command
 */
static int
lock_unlock(int lock, int shm, char *args)
{
	glctx_t *gcp = &glctx;

	char    *segname, *nextarg;
	range_t  range = { 0L, 0L };

	args += strspn(args, whitespace);
	if(!required_arg(args, "<seg-name>"))
		return CMD_ERROR;
	segname = strtok_r(args, whitespace, &nextarg);
	args = nextarg + strspn(nextarg, whitespace);

	/*
	 * offset, length are optional for lock/unlock.
	 * ignored for slock/sunlock.
	 */
	if (!shm && get_range(args, &range, &nextarg) == CMD_ERROR)
		return CMD_ERROR;

	if (!segment_lock_unlock(segname, &range, lock, shm))
		return CMD_ERROR;

	return CMD_SUCCESS;
}

/*
 * command:  lock <seg-name> [<offset>[k|m|g|p] <length>[k|m|g|p]]
 */
static int
lock_seg(char *args)
{
	return(lock_unlock(1, 0, args));
}

/*
 * command:  unlock <seg-name> [<offset>[k|m|g|p] <length>[k|m|g|p]]
 */
static int
unlock_seg(char *args)
{
	return(lock_unlock(0, 0, args));
}

/*
 * command:  slock <seg-name>
 */
static int
shm_lock_seg(char *args)
{
	return(lock_unlock(1, 1, args));
}

/*
 * command:  sunlock <seg-name>
 */
static int
shm_unlock_seg(char *args)
{
	return(lock_unlock(0, 1, args));
}


/*
 * =========================================================================
 * memtoy child processes.
//TODO:  ref count child_toy structure?
 */
static sigset_t sigcld_block;
static void
children_init()
{
	/*
	 * signal set for temporarily blocking SIGCLD
	 */
	sigemptyset(&sigcld_block);
	sigaddset(&sigcld_block, SIGCLD);
}

/*
 * children_free() - free list of children
 *
 * used from child to free parent's list w/o killing.
 * used from 'cleanup() to "kill" child and free list.
 */
static void
children_free(int do_kill)
{
	glctx_t *gcp = &glctx;
	struct list_head *lp, *safe;
	child_t *childp;

	if (list_empty(&gcp->children))
		return;

	list_for_each_safe(lp, safe, &gcp->children) {
		child_t *cp = list_entry(lp, child_t, c_link);
		
		list_del(&cp->c_link);
		if (do_kill)
			kill(cp->c_pid, SIGQUIT);
		free(cp->c_name);
		free(cp);
	}
}

/*
 * called from cleanup to kill off all children
 */
void
children_cleanup()
{
	children_free(1);
}

/*
 * children_list() - list all children with pid and err count
 */
static void
children_list()
{
	glctx_t *gcp = &glctx;
	struct list_head *lp;
	child_t *childp;

	if (list_empty(&gcp->children))
		return;

	printf("  _pid_ errs _name_\n");
	list_for_each(lp, &gcp->children) {
		child_t *cp = list_entry(lp, child_t, c_link);
		
		printf(" %6d %4d %s\n", cp->c_pid, cp->c_errs, cp->c_name);
	}

}

/*
 * child_find_by_name() -- look up child toy by name
 */
static child_t *
child_find_by_name(char *child_name)
{
	glctx_t *gcp = &glctx;
	struct list_head *lp;

	if (list_empty(&gcp->children))
		return NULL;

	list_for_each(lp, &gcp->children) {
		child_t *this_one = list_entry(lp, child_t, c_link);
		
		if(!strcmp(child_name, this_one->c_name))
			return this_one;
	}

	return NULL;
}

/*
 * child_find_by_pid() -- look up child toy by process id
 */
static child_t *
child_find_by_pid(pid_t cpid)
{
	glctx_t *gcp = &glctx;
	struct list_head *lp;

	if (list_empty(&gcp->children))
		return NULL;

	list_for_each(lp, &gcp->children) {
		child_t *this_one = list_entry(lp, child_t, c_link);
		
		if(cpid == this_one->c_pid)
			return this_one;
	}

	return NULL;
}

/*
 * child_wait() -- wait for child ready to read command
 */
static char *child_ready = "OK";
static void
child_wait(child_t *childp)
{
	glctx_t *gcp = &glctx;
	char child_response[8];
	int  ret;

	ret = read(childp->c_rfd, child_response, strlen(child_ready));
	if (ret < 0) {
		int err = errno;
		fprintf(stderr, "%s - error reading response from child %s"
				" - %s\n", 
			gcp->program_name, childp->c_name, strerror(err));
	}

}

/*
 * child_respond() -- tell parent we're ready for a command
 */
static void
child_respond()
{
	glctx_t *gcp = &glctx;
	int ret;

	if (!gcp->child_name)
		return;

	ret = write(gcp->response_fd, child_ready, strlen(child_ready));
	if (ret < 0) {
		int err = errno;
		fprintf(stderr, "%s - error writing response from child %s"
				" - %s\n", 
			gcp->program_name, gcp->child_name, strerror(err));
	}
}

/*
 * child <child-name> - spawn a child process to handle commands directed
 * at '/<child-name>'
 */
static int
child_spawn(char *args)
{
	glctx_t *gcp = &glctx;
	
	char    *child_name, *nextarg;
	child_t *childp;
	int	cmdpipe[2];	/* parent -> child commands */
	int     resppipe[2];	/* child -> parent response/ready */
	pid_t	childpid;

	if (list_empty(&gcp->children))
		children_init();		/* whenever we have no children */

	args += strspn(args, whitespace);

	if(!required_arg(args, "<child-name>"))
		return CMD_ERROR;
	child_name = strtok_r(args, whitespace, &nextarg);
	args = nextarg + strspn(nextarg, whitespace);

	/*
	 * lookup child name to insure uniqueness
	 */
	childp = child_find_by_name(child_name);
	if (childp != NULL) {
		fprintf(stderr, "%s:  child name %s already in use\n",
			gcp->program_name, childp->c_name);
		return CMD_ERROR;
	}

	childp = (child_t *)calloc(1, sizeof(child_t));
	childp->c_name = strdup(child_name);
	childp->c_cfd =  childp->c_rfd = -1;

	/*
	 * setup command and response pipes
	 */
	if (pipe(cmdpipe) < 0) {
		int err = errno;
		fprintf(stderr, "%s:  command pipe creation for %s failed\n"
				"\t%s\n", gcp->program_name, childp->c_name,
				strerror(err));
		goto out_free;
	}

	if (pipe(resppipe) < 0) {
		int err = errno;
		fprintf(stderr, "%s:  response pipe creation for %s failed\n"
				"\t%s\n", gcp->program_name, childp->c_name,
				strerror(err));
		goto out_closecmd;
	}

	switch (childpid = fork()) {
		sigset_t saved_mask;
		int err;
	case -1:
		err = errno;
		fprintf(stderr, "%s:  fork() for %s failed - %s\n",
				gcp->program_name, childp->c_name,
				strerror(err));
		goto out_closeall;
		break;

	case 0:		/* child */
		if (dup2(cmdpipe[0], 0) != 0) {	/* redirect standard input */
			int err = errno;
			fprintf(stderr, "%s(%s):  dup2() of stdin failed -\n"
					"\t%s\n", gcp->program_name, child_name,
					strerror(err));
			exit(1);
		}
		close(cmdpipe[0]);	/* dup'd above */
		close(cmdpipe[1]);	/* parent write fd */

		close(resppipe[0]);	/* parent read fd */
		gcp->response_fd = resppipe[1];
		gcp->child_name = strdup(child_name);
		set_program_name();
		children_free(0);	/* no kill */

		child_respond();
		process_commands();
		quit(NULL);		/* shouldn't get here */
		break;

	default:	/* parent */
		childp->c_pid = childpid;

		close(cmdpipe[0]);		/* child read fd */
		childp->c_cfd = cmdpipe[1];	/* parent write fd */

		close(resppipe[1]);		/* child write fd */
		childp->c_rfd = resppipe[0];	/* parend read fd */
	

		sigprocmask(SIG_BLOCK, &sigcld_block, &saved_mask);
		list_add_tail(&childp->c_link, &gcp->children);
		sigprocmask(SIG_SETMASK, &saved_mask, NULL);

		printf("%s:  child %s - pid %d\n",
			 gcp->program_name, childp->c_name, childp->c_pid);

		child_wait(childp);
	}

	return CMD_SUCCESS;

out_closeall:
	close(resppipe[0]);
	close(resppipe[1]);

out_closecmd:
	close(cmdpipe[0]);
	close(cmdpipe[1]);

out_free:
	free(childp);
	return CMD_ERROR;
}

/*
 * child_reap() -- called from SIGCLD handler to reap
 * child indicated by pid
 */
void
child_reap(pid_t cpid, int cstatus)
{
	glctx_t *gcp = &glctx;
	child_t *childp;

	childp = child_find_by_pid(cpid);
	if (!childp) {
		fprintf(stderr, "%s couldn't find child %d to reap\n",
			gcp->program_name, cpid);
		return;
	}

	list_del(&childp->c_link);
	close(childp->c_cfd);
	close(childp->c_rfd);
	free(childp->c_name);
	free(childp);
}

/*
 * child_send() - send command to named child.
 *
 * From '/<child-name> <command & args>' input
 *
 * special handling of <child-name> terminator so that
 * we can write:  /child/grandchild/...  command ...
 * i.e., w/o requiring whitespace after child names.
 */
static char *child_delim = "\t /";
static int
child_send(char *cmd_str)
{
	glctx_t *gcp = &glctx;
	char    *child_name, *nextarg;
	child_t *childp;
	size_t   cmdlen;
	char	 csave = 0;

	cmd_str += strspn(cmd_str, child_delim);  /* initial cruft */

	if (*cmd_str == '\0' || *cmd_str == '?') {
		children_list();
		return CMD_SUCCESS;
	}

	if(!required_arg(cmd_str, "<child-name>"))
		return CMD_ERROR;

	child_name = cmd_str;
	cmd_str += strcspn(cmd_str, child_delim);
	if (*cmd_str == '/')
		csave = '/';
	*cmd_str = '\0';	/* terminate child_name */
	nextarg = ++cmd_str;
	cmd_str = nextarg + strspn(nextarg, whitespace);

	childp = child_find_by_name(child_name);
	if (!childp) {
		fprintf(stderr, "%s-send:  I don't have a child named %s\n",
			gcp->program_name, child_name);
		return CMD_ERROR;
	}

	/*
	 * restore saved '/', if any
	 */
	if (csave)
		*(--cmd_str) = csave;

	cmdlen = strlen(cmd_str);
	cmd_str[cmdlen++] = '\n';	/* for child's fgets() */

	if (cmdlen != write(childp->c_cfd, cmd_str, cmdlen)) {
		int err = errno;
		fprintf(stderr, "%s:  write to child %s failed - %s\n",
				gcp->program_name, childp->c_name,
				strerror(err));
		childp->c_errs += 1;
//TODO:  kill child if too many errors?
		return CMD_ERROR;
	}

	child_wait(childp);

	childp->c_errs = 0;	/* reset on success */
	
	return CMD_SUCCESS;
}

/*
 * parse_signal - parse signal name/number argument
 */
static int
parse_signal(char *args)
{
	glctx_t *gcp = &glctx;
	char *nextarg, *sigend = strpbrk(args, whitespace);
	int signum;

	if (sigend)
		*sigend = '\0';

	if (!isdigit(*args)) {
		signum = signum_from_name(args);

		if (signum < 0) {
			fprintf(stderr,
			    "%s doesn't recognize signal %s\n",
			    gcp->program_name, args);
			return -1;
		}
	} else {
		signum = strtoul(args, &nextarg, 0);
		if (!signum || signum >= _NSIG) {
			fprintf(stderr,
			    "%s signal out of range - %d\n",
			    gcp->program_name, signum);
			return -1;
		}
	}

	return signum;
}

/*
 * Cruel, yes.  But sometimes necessary...
 */
static int
child_kick(char *args)
{
	glctx_t *gcp = &glctx;
	char    *child_name, *nextarg;
	child_t *childp;
	int      signum = SIGINT;	/* default */

	args += strspn(args, whitespace);
	if(!required_arg(args, "<child-name>"))
		return CMD_ERROR;
	child_name = strtok_r(args, whitespace, &nextarg);
	args = nextarg + strspn(nextarg, whitespace);

	if (*child_name == '?') {
		signal_list();
		return CMD_SUCCESS;
	}

	childp = child_find_by_name(child_name);
	if (!childp) {
		fprintf(stderr, "%s-kick:  I don't have a child named %s\n",
			gcp->program_name, child_name);
		return CMD_ERROR;
	}

	if (*args != '\0') {
		signum = parse_signal(args);
		if (signum < 0 )
			return CMD_ERROR;
	}

	kill(childp->c_pid, signum);

	return CMD_SUCCESS;
}

static int
snooze(char *args)
{
	glctx_t *gcp = &glctx;
	unsigned int seconds;

	args += strspn(args, whitespace);
	if(!required_arg(args, "<seconds>"))
		return CMD_ERROR;
	seconds = get_arg_seconds(args);
	if (seconds == BOGUS_SECONDS)
		return CMD_ERROR;

	sleep(seconds);

	return CMD_SUCCESS;
}

static int
show_task_policy(void)
{
	glctx_t *gcp = &glctx;
	nodemask_t nodemask;
	unsigned long *nodebits = nodemask.n;
	int         nr_nodes = 0;
	int         policy, flags = 0;
	int         ret;

	ret = get_mempolicy(&policy, nodebits, NUMA_NUM_NODES, 0, 0);
	if (ret)
		return CMD_ERROR;

//TODO:  display > 1 word of nodebits, if populated.
// see show_cpus()
// display as node list if "reasonable"
	printf("%s:  task policy = %s 0x%lx\n",
		gcp->program_name, policies[policy].pol_name, nodebits[0]);

	return CMD_SUCCESS;
}

static int
mpol(char *args)
{
	glctx_t *gcp = &glctx;
	char       *nextarg;
	nodemask_t *nodemask = NULL;
	unsigned long *nodebits = NULL;
	unsigned long  maxnode = 0;
	int         nr_nodes = 0;
	int         policy;
	int         ret;

	args += strspn(args, whitespace);
	if (!*args)
		return show_task_policy();

	policy = get_mbind_policy(args, &nextarg);
	if (policy < 0)
		return CMD_ERROR;
	args = nextarg + strspn(nextarg, whitespace);

	nr_nodes = get_arg_nodemask(args, policy, &nodemask);
	if (nr_nodes < 0)
		return CMD_ERROR;

	if (nodemask) {
		maxnode = NUMA_NUM_NODES;
		nodebits = nodemask->n;
	}

	ret = set_mempolicy(policy, nodebits, maxnode);

	if (nodemask != NULL)
		free(nodemask);

	if (ret) {
		int err = errno;
		fprintf(stderr, "%s:  set_mempolicy() failed - %s\n",
				gcp->program_name, strerror(err));
		return CMD_ERROR;
	}

	return CMD_SUCCESS;
}

#if 0 /* new command function template */
static int
command(char *args)
{
	glctx_t *gcp = &glctx;

	args += strspn(args, whitespace);
	not_implemented();

	return CMD_SUCCESS;
}

#endif
/*
 * =========================================================================
 */
typedef int (*cmd_func_t)(char *);

struct command {
	char       *cmd_name;    
	cmd_func_t  cmd_func;    /* */
	char       *cmd_help;
	char       *cmd_longhelp;
	
} cmd_table[] = {
	{
		.cmd_name="help",
		.cmd_func=help_me,
		.cmd_help=
			"help           - show help\n"
			"help <command> - display detailed help for <command>",
		.cmd_longhelp="",
	},
	{
		.cmd_name="man",
		.cmd_func=help_me ,
		.cmd_help="man <command> - alias for help",
		.cmd_longhelp="",
	},
	{
		.cmd_name="quit",
		.cmd_func=quit,
		.cmd_help=
			"quit           - just what you think.",
		.cmd_longhelp=
			"\tEOF on stdin has the same effect\n",
	},
	{
		.cmd_name="exit",
		.cmd_func=quit,
		.cmd_help="exit - alias for 'quit'",
		.cmd_longhelp="",
	},
	{
		.cmd_name="pid",
		.cmd_func=show_pid,
		.cmd_help=
			"pid            - show process id of this session",
		.cmd_longhelp="",
	},
	{
		.cmd_name="pause",
		.cmd_func=pause_me,
		.cmd_help=
			"pause          - pause program until signal"
			" -- e.g., INT, USR1",
		.cmd_longhelp="",
	},
	{
		.cmd_name="numa",
		.cmd_func=numa_info,
		.cmd_help=
			"numa          - display numa info as seen by this program.",
		.cmd_longhelp=
			"\tshows nodes from which program may allocate memory\n"
			"\twith total and free memory.\n",
	},
	{
		.cmd_name="migrate",
		.cmd_func=migrate_process,
		.cmd_help=
			"migrate <to-node-id[s]> [<from-node-id[s]>] - \n"
			"\tmigrate this process' memory from <from-node-id[s]>\n"
			"\tto <to-node-id[s]>.",
		.cmd_longhelp=
			"\tSpecify multiple node ids as a comma-separated list.\n"
			"\tIf <from-node-id[s]> is omitted, it defaults to memtoy's\n"
			"\tcurrent set of allowed nodes.\n" ,
	},

	{
		.cmd_name="show",
		.cmd_func=show_seg,
		.cmd_help=
			"show [<seg-name>]  - show info for segment[s]; default all",
		.cmd_longhelp=
			"\t1st char is \"segment type\":  a->anon, f->file, s->shmem, \n"
			"\th->hugepage shmem.\n"
			"\tIf <seg-name> == [or starts with] '+', show the segments from\n"
			"\tthe task's maps [/proc/<mypid>/maps] -- otherwise, not.\n"
			"\tNote:  the <seg-name> of a \"file\" segment is the \"basename\"\n"
			"\t       of the file's <pathname>.\n",
	},
	{
		.cmd_name="anon",
		.cmd_func=anon_seg,
		.cmd_help=
			"anon <seg-name> <seg-size>[k|m|g|p] [<seg-share>]\n"
			"\tdefine a MAP_ANONYMOUS segment of specified size",
		.cmd_longhelp=
			"\t<seg-name> must be unique.\n"
			"\t<seg-share> := private|shared - default = private\n",
	},
	{
		.cmd_name="file",
		.cmd_func=file_seg,
		.cmd_help=
			"file <pathname> [<offset>[k|m|g|p] <length>[k|m|g|p]] [<seg-share>] -\n"
			"\tdefine a mapped file segment of specified length starting at the\n"
			"\tspecified offset into the file.",
		.cmd_longhelp=
			"\tUse the \"basename\" of <pathname> for <seg-name> with commands\n"
			"\tthat require one.  Therefore, the file's basename must not\n"
			"\tmatch any other segment's <seg-name>.\n"
			"\t<offset> and <length> may be omitted and specified on the\n"
			"\tmap command.\n"
			"\t<seg-share> := private|shared - default = private\n",
	},
	{
		.cmd_name="shmem",
		.cmd_func=shmem_seg,
		.cmd_help=
			"shmem <seg-name> <seg-size>[k|m|g|p] [huge] - \n"
			"\tdefine a shared memory segment of specified size.",
		.cmd_longhelp=
			"\t<seg-name> must be unique.  Optional argument 'huge' requests\n"
			"\tuse of huge pages.  You may need to increase limits\n"
			"\t[/proc/sys/kernel/shmmax].  To use huge pages, you may need to\n"
			"\tincrease the number of huge pages [/proc/sys/vm/nr_hugepages].\n"
			"\tUse map/unmap to attach/detach.\n",
	},
	{
		.cmd_name="remove",
		.cmd_func=remove_seg,
		.cmd_help=
			"remove <seg-name> [<seg-name> ...] - remove the named segment[s]",
		.cmd_longhelp="",

	},

	{
		.cmd_name="map",
		.cmd_func=map_seg,
		.cmd_help=
			"map <seg-name> [<offset>[k|m|g|p] <length>[k|m|g|p]] [<seg-share>] - \n"
			"\tmmap()/shmat() a previously defined, currently unmapped() segment.",
		.cmd_longhelp=
			"\t<offset> and <length> apply only to mapped files.\n"
			"\tUse <length> of '*' or '0' to map to the end of the file.\n"
			"\tOffset and length specified here override those specified on\n"
			"\tthe file command.\n",
	},
	{
		.cmd_name="unmap",
		.cmd_func=unmap_seg,
		.cmd_help=
			"unmap <seg-name> - unmap specified segment, but remember name/size/...",
		.cmd_longhelp=
			"\tYou can't unmap segments from the task's /proc/<pid>/maps.\n",
	},
	{
		.cmd_name="lock",
		.cmd_func=lock_seg,
		.cmd_help=
			"lock <seg-name> [<offset>[k|m|g|p] <length>[k|m|g|p]] - \n"
			"\tmlock() a [range of a] previously mapped segment.",
		.cmd_longhelp=
			"\tLock the named segment from <offset> through <offset>+<length>\n"
			"\tinto memory.  If <offset> and <length> omitted, locks all pages\n"
			"\tof mapped segment.  Locking a range of a segment causes the pages\n"
			"\tbacking the specified/implied range to be faulted into memory.\n"
			"\tNOTE:  locking is restricted by resource limits for non-privileged\n"
			"\tusers.  See 'ulimit -l' -- results in Kbytes.\n",
	},
	{
		.cmd_name="unlock",
		.cmd_func=unlock_seg,
		.cmd_help=
			"unlock <seg-name> [<offset>[k|m|g|p] <length>[k|m|g|p]] - \n"
			"\tmunlock() a [range of] previously mapped segment.",
		.cmd_longhelp=
			"\tUnlock the named segment from <offset> through <offset>+<length>.\n"
			"\tIf <offset> and <length> omitted, unlocks all pages of the segment.\n",
	},
	{
		.cmd_name="slock",
		.cmd_func=shm_lock_seg,
		.cmd_help=
			"slock <shm-seg-name> - \n"
			"\tSHM_LOCK a previously mapped shmem segment into memory.",
		.cmd_longhelp=
			"\tSHM_LOCK will not automatically fault the pages of the segment\n"
	 		"\tinto memory.  Use the \"touch\" command to fault in the segment\n"
			"\tafter locking, if necessary.\n"
			"\tNOTE:  locking is restricted by resource limits for non-privileged\n"
			"\tusers.  See 'ulimit -l' -- results in Kbytes.\n",
	},
	{
		.cmd_name="sunlock",
		.cmd_func=shm_unlock_seg,
		.cmd_help=
			"sunlock <shm-seg-name> - \n"
			"\tSHM_UNLOCK a previously SHM_LOCKed shmem segment.",
		.cmd_longhelp= "",
	},
	{
		.cmd_name="touch",
		.cmd_func=touch_seg,
		.cmd_help=
			"touch <seg-name> [<offset>[k|m|g|p] <length>[k|m|g|p]] [read|write]",
		.cmd_longhelp=
			"\tread [default] or write the named segment from <offset> through\n"
			"\t<offset>+<length>.  If <offset> and <length> omitted, touches all\n"
			"\t of mapped segment.\n"
			"\tYou can't write to segments from the task's /proc/<pid>/maps.\n",
	},
	{
		.cmd_name="mbind",
		.cmd_func=mbind_seg,
		.cmd_help=
			"mbind <seg-name> [<offset>[k|m|g|p] <length>[k|m|g|p]]\n"
			"      <policy>[+shared][+move[+all][+lazy]] [<node/list>] - \n"
			"\tset the numa policy for the specified range of the named segment",
		.cmd_longhelp=
			"\tto policy --  one of {default, bind, preferred, interleaved, noop}.\n"
			"\t<node/list> specifies a node id or a comma separated list of\n"
			"\tnode ids.  <node/list> is ignored for 'default' policy, and\n"
			"\tonly the first node is used for 'preferred' policy.\n"
			"\t'+shared' apply shared policy to shared, file mapping.  This flag must\n"
			"\t         come before '+move', if specified.  Current uid must be owner\n"
			"\t        of the file and shared_file_policy must be enabled in memtoy's\n"
			"\t        cpuset.\n"
			"\t'+move' specifies that currently allocated pages be moved, if\n"
			"\t        necessary/possible, to satisfy policy.  Pages can't be\n"
			"\t        moved if they are mapped by more than one process.\n"
			"\t'+all' [valid only with +move] specifies that all eligible pages\n"
			"\t        in task be moved to satisfy policy, even if they are\n"
			"\t        mapped by more than one process.  Requires appropriate\n"
			"\t        privilege.\n"
			"\t'+lazy' [valid only with +move] requests that mbind just unmap the\n"
			"\t        pages in the specified range after setting the new policy.\n"
			"\t        Page migration will then occur when the task touches the pages\n"
			"\t        to fault them back into its page table.\n"
			"\tFor policies preferred and interleaved, <node/list> may be specified\n"
			"\tas '*' meaning local allocation for preferred policy and \"all allowed\n"
			"\tnodes\" for interleave policy.\n" ,
	},
	{
		.cmd_name="where",
		.cmd_func=where_seg,
		.cmd_help=
			"where <seg-name> [<offset>[k|m|g|p] <length>[k|m|g|p]] - \n"
			"\tshow the node location of pages in the specified range",
		.cmd_longhelp=
			"\tof the specified segment.  <offset> defaults to start of\n"
			"\tsegment; <length> defaults to 64 pages, based on segment\n"
			"\tpagesize.\n"
			"\tUse SIGINT to interrupt a long display\n",
	},
	{
		.cmd_name="cpus",
		.cmd_func=cpus ,
		.cmd_help=
			"cpus [{0x<mask>|<cpu-list>}] -"
			"\tquery/change program's cpu affinity mask.\n",
		.cmd_longhelp=
			"\tSpecify allowed cpus as a comma separated list of cpu masks:\n"
			"\t0x<mask>, where <mask> consists entirely of hex digits, high\n"
			"\torder bits first, or as a comma separated list of decimal\n"
			"\tcpu ids.\n",
	},
	{
		.cmd_name="mems",
		.cmd_func=mems ,
		.cmd_help=
			"mems [{0x<mask>|<node-list>}] -"
			"\tquery/change program's memory affinity mask.\n",
		.cmd_longhelp=
			"\tSpecify allowed memories [nodes] as 0x<mask>, where <mask>\n"
			"\tconsists entirely of hex digits, or as a comma separated list\n"
			"\tof decimal memory/node ids\n"
			"\tNOTE:  currently limited to 64 nodes\n",
	},
	{

		.cmd_name="child",
		.cmd_func=child_spawn ,
		.cmd_help=
			"child <child-name> - "
			"\tcreate a child process named <child-name>.",
		.cmd_longhelp=
			"\tChild enters command loop, reading from pipe.\n"
			"\tCommands prefixed with '/<child-name>' will be sent to the\n"
			"\tchild process.  Use '/' by itself, or '/?' to list existing\n"
			"\tchildren.  Note that children may also have children of\n"
			"\ttheir own and commands may be sent to 'grandchildren'\n"
			"\tvia '/<child>/<grandchild>[/...] <command> ...\n",
	},
	{
		.cmd_name="kick",
		.cmd_func=child_kick,
		.cmd_help=
			"kick <child> [<signal>] - "
			"post <signal> to <child>",
		.cmd_longhelp=
			"\t<signal> may be entered by number or name.\n"
			"\tUse 'kick ?' to see the list of signal names\n"
			"\tthat memtoy recognizes\n",
	},

	{
		.cmd_name="snooze",
		.cmd_func=snooze ,
		.cmd_help=
			"snooze <seconds> - snooze [sleep] for <seconds>.\n",
		.cmd_longhelp="",
	},

	{
		.cmd_name="mpol",
		.cmd_func=mpol ,
		.cmd_help=
			"mpol [<policy> [<node/list>]] -  set/query memtoy's task mempolicy.\n",
		.cmd_longhelp=
			"\t<policy> --  one of {default, bind, preferred, interleaved, noop}.\n"
			"\t<node/list> specifies a node id or a comma separated list of\n"
			"\tnode ids.  <node/list> is ignored for 'default' policy, and\n"
			"\tonly the first node is used for 'preferred' policy.\n"
			"\tIf <policy> [<node/list>] is omitted, memtoy's current task policy\n"
			"\twill be displayed.\n",
	},

#if 0 /* template for new commands */
	{
		.cmd_name="",
		.cmd_func= ,
		.cmd_help=
		.cmd_longhelp="",
	},

#endif
	{
		.cmd_name=NULL
	}
};

/*
 * command:  help|man [<command-name>]
 *
 * also called to display help when invalid arg encountered for
 * any command
 */
static int
help_me(char *args)
{
	struct command *cmdp = cmd_table;
	char *cmd, *nextarg;
	int   cmdlen;
	bool  match = false;

	args += strspn(args, whitespace);
	if (*args != '\0') {
		char *cend = strpbrk(args, whitespace);
		cmd = args;
		if (cend)
			*cend = '\0';
		cmdlen = cend - cmd;
	} else
		cmd = NULL;

	for( cmdp = cmd_table; cmdp->cmd_name != NULL; ++cmdp) {
		if (cmd == NULL ||
				!strncmp(cmd, cmdp->cmd_name, cmdlen)) {
			match = true;
			if (!cmdp->cmd_help)
				continue;	/* undocumented cmd */
			fprintf(stderr, "%s\n", cmdp->cmd_help);
			if (cmd)
				fprintf(stderr, "%s\n", cmdp->cmd_longhelp);
			else
				fprintf(stderr, "\n");
		}
	}

	if (!match) {
		fprintf(stderr, "unrecognized command:  %s\n", cmd);
		fprintf(stderr, "\tuse 'help' for a complete list of commands\n");
		return CMD_ERROR;
	}

	if (!cmd)
		fprintf(stderr, "for help on a specific command, type "
			"\"help <command>\"\n");

	return CMD_SUCCESS;
}

/*
 * =========================================================================
 */
#define CMDBUFSZ 256

static bool
unique_abbrev(char *cmd, size_t clen, struct command *cmdp)
{
	for(; cmdp->cmd_name != NULL; ++cmdp) {
		if(!strncmp(cmd, cmdp->cmd_name, clen))
			return false;	/* match: not unique */
	}
	return true;
}

static int
parse_command(char *cmdline)
{
	glctx_t *gcp = &glctx;
	char *cmd, *args;
	struct command *cmdp;

	cmdline += strspn(cmdline, whitespace);	/* possibly redundant */

	cmd = strtok_r(cmdline, whitespace, &args);

	for( cmdp = cmd_table; cmdp->cmd_name != NULL; ++cmdp) {
		size_t clen = strlen(cmd);
		int ret;

		if (strncmp(cmd, cmdp->cmd_name, clen))
			continue;
		if (!unique_abbrev(cmd, clen, cmdp+1)) {
			fprintf(stderr, "%s:  ambiguous command:  %s\n",
				gcp->program_name, cmd);
			return CMD_ERROR;
		}
		gcp->cmd_name = cmdp->cmd_name;
		ret = cmdp->cmd_func(args);
		gcp->cmd_name = NULL;
		return ret;
	}

	fprintf(stderr, "%s:  unrecognized command %s\n",
		gcp->program_name, cmd);
	return CMD_ERROR;
}

/*
 * For non-interactive input, including children reading from command pipes,
 * emulate readline(3) without prompting
 */
static char *
readline_ni(void)
{
	glctx_t *gcp = &glctx;
	char  *cmdbuf, *cmdline;
	size_t cmdlen;


	cmdbuf = calloc(1, CMDBUFSZ);
//TODO:  check return

	while(true) {
		cmdline = fgets(cmdbuf, CMDBUFSZ-1, stdin);
		if (cmdline != NULL)
			break;

		if (!feof(stdin))
			continue;	/* skip empty lines */
		printf("%s EOF on stdin\n", gcp->program_name);
		exit(0);		/* EOF */
	}

	/*
	 * trim trailing newline, if any, like readline()
	 */
	cmdlen = strlen(cmdline);
	if (cmdline[cmdlen-1] == '\n')
		cmdline[--cmdlen] = '\0';


	return cmdline;
}

/*
 * free a command line read by readline() or readline_ni()
 */
static void
cmd_free(char *cmdline)
{
	if (cmdline)
		free(cmdline);
}

static char _cmdbuf[CMDBUFSZ*2];
void
process_commands()
{
	glctx_t *gcp = &glctx;
	char  *prompt_buf;
	char  *saved_cmdline = NULL;	/* for freeing */

	/*
	 * primarily to reset children's input buffer
	 */
	if (setvbuf(stdin, _cmdbuf, _IOLBF, sizeof(_cmdbuf))) {
		int err = errno;
		fprintf(stderr, "%s - setvbuf failed - %s\n",
			gcp->program_name, strerror(err));
		exit(4);
	}

	if (is_option(INTERACTIVE) && !gcp->child_name) {
		prompt_buf = malloc(2+strlen(gcp->program_name));
//TODO:  check return.
		sprintf(prompt_buf, "%s>", gcp->program_name);
	}

	/*
	 * main command read loop.
	 * free saved command line and
	 * children respond to parent at end of each pass
	 */
	for (;; cmd_free(saved_cmdline), child_respond()) {
		char  *cmdline;
		size_t cmdlen;

		if (is_option(INTERACTIVE) && !gcp->child_name) {
			saved_cmdline = cmdline =
				 readline(prompt_buf);
			if (cmdline == NULL) {
				printf("\n");	/* flush prompt */
				exit(0);	/* EOF */
			}
			if (*cmdline)
				add_history(cmdline);
		} else
			saved_cmdline = cmdline = readline_ni();

		if (cmdline[0] == '\0')
			continue;

		/*
		 * skip leading whitespace
		 */
		cmdline += strspn(cmdline, whitespace);
		cmdlen = strlen(cmdline);

		if (cmdlen == 0) {
			//TODO:  interactive help?
			continue;	/* ignore blank lines */
		}

		if (*cmdline == '#')
			continue;	/* comments */

		/*
		 * trim trailing whitespace for ease of parsing
		 */
		while(strchr(whitespace, cmdline[cmdlen-1]))
			cmdline[--cmdlen] = '\0';

		if (cmdlen == 0)
			continue;

		/*
		 * reset signals just before parsing a command.
		 * non-interactive:  exit on SIGQUIT
		 */
		if(signalled(gcp)) {
			if(!is_option(INTERACTIVE) &&
			   gcp->siginfo->si_signo == SIGQUIT)
				exit(0);
			reset_signal();
		}

		/*
		 * forward child-directed commands
		 */
		if (*cmdline == '/') {
			child_send(++cmdline);
			//TODO:  check error?
			continue;
		}

		/*
		 * non-interactive:  errors are fatal
		 */
		if(!is_option(INTERACTIVE)) {
			vprint("%s>%s\n", gcp->program_name, cmdline);
			if(parse_command(cmdline) == CMD_ERROR) {
				fprintf(stderr, "%s:  command error\n",
					gcp->program_name);
				exit(4);
			}
			fflush(stdout);
		} else
			parse_command(cmdline);

	}
}

void
commands_init(glctx_t *gcp)
{
	refresh_mems_allowed(gcp);
	refresh_cpus_allowed(gcp);
	gcp->max_cpu_id  = MAX_CPUID;	/* hard code for now */
}
