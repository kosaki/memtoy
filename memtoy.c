/*
 * memtoy.c -- toy/tool for investigating Linux [Numa] VM behavior
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
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <numa.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "memtoy.h"

/*
 * global context
 */
glctx_t glctx;	/* global context */

/*
 * command line options:
 *
 *  -v          = verbose
 *  -V          = display version
 *  -h|x	= display help.
 */
#define OPTIONS	"Vhvx"

/*
 * usage/help message
 */
char *USAGE =
"\nUsage:  %s [-v] [-V] [-{h|x}] [<script-file>]\n\n\
Where:\n\
\t-v            enable verbosity\n\
\t-V            display version info\n\
\t-h|x          show this usage/help message\n\
\n\
memtoy can be run interactively or in batch mode.  If <script-file>\n\
is specified on the command line, or standard input is NOT a 'tty',\n\
memtoy runs in batch mode.  Otherwise, it will run in interactive mode.\n\
In batch mode, any error will cause memtoy to exit with non-zero status.\n\
In interactive mode, use 'help' for a list of commands, and 'help <command>'\n\
for more info about a specific command.\n\
";


/*
 * die() - emit error message and exit w/ specified return code.
 *	   if exit_code < 0, save current errno, and fetch associated
 *	   error string.  Print error string after app error message.
 *	   Then exit with abs(exit_code).
 */
void
die(int exit_code, char *format, ... )
{
	va_list ap;
	char *errstr;
	int saverrno;

	va_start(ap, format);

	if (exit_code < 0) {
		saverrno = errno;
		errstr = strerror(errno);
	}

	(void) vfprintf(stderr, format, ap);
	va_end(ap);

	if (exit_code < 0)
		fprintf(stderr,"Error = (%d) %s\n", saverrno, errstr);

	exit(abs(exit_code));
}

void
usage(char *mesg){
	if (mesg != NULL) {
		fprintf(stderr, "%s\n", mesg);
	}
	fprintf(stderr, USAGE, glctx.program_name);
	exit(1);
}


#ifdef _DEBUG
/*
 * This function is a wrapper around "fprintf(stderr, ...)" so that we
 * can use the DPRINTF(<flag>, (<[f]printf arguments>)) macro for debug
 * prints.  See the definition of DPRINTF in XXX.h
 */
int
_dvprintf(char *format, ...)
{
	va_list ap;
	int retval;

	va_start(ap, format);

	retval = vfprintf(stderr, format, ap);

	va_end(ap);

	fflush(stderr);
	return(retval);
}
#endif

void
vprint(char *format, ...)
{
	va_list ap;
	glctx_t *gcp = &glctx;

	va_start(ap, format);

	if (!is_option(VERBOSE))
		goto out;

	(void)vfprintf(stdout, format, ap);
	fflush(stdout);

out:
	va_end(ap);
	return;

}

/*
 * =========================================================================
 */
static int signals_to_handle[] =
{
	SIGINT,  SIGQUIT, SIGSEGV, SIGBUS,
	SIGUSR1, SIGUSR2, 0
};

static char *sig_names[] =
{
	"INT",  "QUIT", "SEGV", "BUS",
	"USR1", "USR2", "unknown", 0
};

char *
sig_name(int signum)
{
	int isig=0, *sigp = signals_to_handle;

	while(*sigp) {
		if (*sigp == signum)
			break;
		++isig; ++sigp;
	}

	return sig_names[isig];
}

int
signum_from_name(const char *sig_name)
{
	int isig=0;
	char **signp = sig_names;

	if (!strncasecmp(sig_name, "sig", 3))
		sig_name += 3;

	while(strcmp(*signp, "unknown")) {
		if(!strcasecmp(sig_name, *signp))
			return signals_to_handle[isig];
		++isig; ++signp;
	}

	return -1;	/* no match */
	
}

void
signal_list(void)
{
	char **signp = sig_names;
	int  icol = 0;

	printf("Signals available to 'kick':");
	while(strcmp(*signp, "unknown")) {
		if (!icol) {
			printf("\n    ");
			icol += 4;
		}
		printf("SIG%-5s", *signp);
		++signp; icol += 8;
		if (icol > MAXCOL - 8)
			icol = 0;
	}

	printf("\n");
}

/*
 * signal_handler()
 *
 * save siginfo and name in global context
 */
void
signal_handler(int sig, siginfo_t *info, void *vcontext)
{
	glctx_t *gcp = &glctx;
	static siginfo_t infocopy;

	/*
	 * static copy of signal info.
	 * Note, additional signals, before use, can overwrite
	 */
	infocopy = *info;
	gcp->siginfo   = &infocopy;

	gcp->signame   = sig_name(sig);

	vprint("signal hander entered for sig SIG%s\n", gcp->signame);

	switch (sig) {
	case SIGSEGV:
	case SIGBUS:
		if (gcp->sigjmp) {
			gcp->sigjmp = false;
			siglongjmp(gcp->sigjmp_env, 1);
		}

		die(8, "\n%s:  signal SIG%s, but handler not armed\n",
		       gcp->program_name, gcp->signame);
		break;

	case SIGINT:
	case SIGQUIT:
		break;

	default:
		die(8, "\n%s:  Unexpected signal:  %d\n",
		        gcp->program_name, sig);
		break;
	}
}
/*
 * sigcld_handler()
 *
 * reap child and free child_t
 */
void
sigcld_handler(int sig, siginfo_t *info, void *vcontext)
{
	glctx_t *gcp = &glctx;
	pid_t cpid;
	int   cstatus, err;

	cpid = wait3(&cstatus, WNOHANG, NULL);

	if (cpid > 0 ) {
		child_reap(cpid, cstatus);
		return;
	}
	
	err = errno;
	vprint("%s:  In %s, wait3() failed - %s\n", 
		gcp->program_name, __FUNCTION__, strerror(err));
	
}

/*
 * set_signals()
 *
 * Setup signal dispositions to catch selected signals
 */
void
set_signals()
{
	glctx_t *gcp = &glctx;
	int *sigp = signals_to_handle;
	char **namep = sig_names;
	
	struct sigaction act = {
		.sa_sigaction = signal_handler,
		.sa_flags	 = SA_SIGINFO
	};

	(void)sigfillset(&(act.sa_mask));

	while (*sigp) {
		char *sig_name = *(namep++);
		int sig = *(sigp++);


		if (0 != sigaction(sig, &act, NULL)) {
			die(-1, "%s: Failed to set sigaction for %s\n",
			        gcp->program_name, sig_name);
		} else
#if 0
			vprint("%s: established handler for %s\n",
			        gcp->program_name, sig_name)
#endif
			;
	}

	/*
	 * special SIGCLD handler for deceased children
	 */
	act.sa_sigaction = sigcld_handler;
	if (0 != sigaction(SIGCLD, &act, NULL)) {
		die(-1, "%s: Failed to set sigaction for SIGCLD\n",
		        gcp->program_name);
	}

		return;
}

void
reset_signal(void)
{
//TODO:  free siginfo if/when malloc'd
	glctx.siginfo = NULL;
	glctx.sigjmp  = false;
}

void
wait_for_signal(const char *mesg)
{
	printf("%s ... ", mesg); fflush(stdout);
	pause();
	vprint("%s: wakened by signal SIG%s\n", __FUNCTION__, glctx.signame);
	reset_signal();
	printf("\n"); fflush(stdout);
}

void
show_siginfo()
{
	glctx_t *gcp = &glctx;
	siginfo_t *info = gcp->siginfo;
	void *badaddr = info->si_addr;
	char *sigcode;

	switch (info->si_signo) {
	case SIGSEGV:
		switch (info->si_code) {
		case SEGV_MAPERR:
			sigcode = "address not mapped";
			break;

		case SEGV_ACCERR:
			sigcode = "invalid access error";
			break;

		default:
			sigcode = "unknown";
			break;
		}
		break;

	case SIGBUS:
		switch (info->si_code) {
		case BUS_ADRALN:
			sigcode = "invalid address alignment";
			break;

		case BUS_ADRERR:
			sigcode = "non-existent physical address";
			break;

		default:
			sigcode = "unknown";
			break;
		}
		break;

	default:
		/*
		 * ignore SIGINT/SIGQUIT
		 */
		return;
	}

	printf("Signal SIG%s @ 0x%lx - %s\n", gcp->signame, badaddr, sigcode);

}

/*
 * =========================================================================
 */

void
touch_memory(bool rw, unsigned long *memp, size_t memlen, size_t pagesize)
{
	glctx_t *gcp = &glctx;

	unsigned long  *memend, *pp, sink;
	unsigned long longs_in_page = pagesize / sizeof (unsigned long);

	memend = memp + memlen/sizeof(unsigned long);
	vprint("!!!%s from 0x%lx thru 0x%lx\n",
		rw ? "Writing" : "Reading", memp, memend);

	for(pp = memp; pp < memend;  pp += longs_in_page) {
		// vprint("%s:  touching 0x%lx\n", __FUNCTION__, pp);
		if (!sigsetjmp(gcp->sigjmp_env, true)) {
			gcp->sigjmp = true;

			/*
			 *  Mah-ahm!  He's touching me!
			 */
			if (rw)
				*pp = (unsigned long)pp;
			else
				sink = *pp;

			gcp->sigjmp = false;
		} else {
			show_siginfo();
			reset_signal();
			break;
		}

		/*
		 * Any [handled] signal breaks the loop
		 */
		if(gcp->siginfo != NULL) {
			reset_signal();
			break;
		}
	}
}

/*
 * =========================================================================
 */

/*
 * TODO:  a better way?
 */
#define MIBUFSIZE 64
void
get_huge_pagesize(glctx_t *gcp)
{
	FILE *mi;

	mi = fopen("/proc/meminfo", "r");
	if (!mi) {
		die(-1, "%s:  failed to open /proc/meminfo\n",
			gcp->program_name);
	}

	do {
		char buf[MIBUFSIZE], *input;
		char *next;
		size_t huge_pagesize;

		input = fgets(buf, MIBUFSIZE, mi);
		if (!input)
			continue;	/* probably EOF */

		if (strncmp(input, "Hugepagesize:", strlen("Hugepagesize:")))
			continue;

		input += strlen("Hugepagesize:");
		input += strspn(input, whitespace);

		huge_pagesize = strtoul(input, &next, 0);
		if (*next != ' ') {
			die(-1, "%s:  bogus huge pagesize %s\n", 
				gcp->program_name, input);
		}

		input = next + strspn(next, whitespace);
		if (!strncmp(input, "kB", strlen("kB")))
			huge_pagesize <<= KILO_SHIFT;

		gcp->huge_pagesize = huge_pagesize;

	} while (!feof(mi));

}

void
init_glctx(glctx_t *gcp, char *arg0)
{
	struct rlimit locked_limits;
	int           ret;
	
	bzero(gcp, sizeof(glctx_t));

	gcp->program_name = basename(arg0);

	gcp->pagesize = (size_t)sysconf(_SC_PAGESIZE);
	get_huge_pagesize(gcp);

	if (numa_available() >= 0) {
		gcp->numa_max_node = numa_max_node();
	} else
		gcp->numa_max_node = -1;

	ret = getrlimit(RLIMIT_MEMLOCK, &locked_limits);
	if (ret < 0) {
		die(-1, "%s:  failed to fetch locked memory rlimit\n",
			gcp->program_name);
	}
	gcp->locked_limit = locked_limits.rlim_cur;

	commands_init(gcp);
	segment_init(gcp);

	INIT_LIST_HEAD(&gcp->children);

	if(isatty(fileno(stdin)))
		set_option(INTERACTIVE);

}

/*
 * cleanup() - at exit cleanup routine
 */
static void
cleanup()
{
	glctx_t *gcp = &glctx;

	children_cleanup();
	segment_cleanup(gcp);
} /* cleanup() */

/*
 * open_script_file -- command line contains a non-option argument.
 * Try to open it as a script file name.  Replace stdin and switch
 * to non-interactive if successful
 */
static int
open_script_file(char *script_file)
{
	glctx_t *gcp = &glctx;
	int sfd, ret;

	sfd = open(script_file, O_RDONLY);
	if (sfd < 0) {
		fprintf(stderr,
			"%s:  failed to open script %s - %s\n",
			gcp->program_name, script_file,
			strerror(errno));
		return sfd;	
	}

	ret = dup2(sfd, STDIN_FILENO);
	clear_option(INTERACTIVE);
	close(sfd);
	return ret;
}


int
parse_command_line_args(int argc, char *argv[])
{
	extern int optind;
	extern char *optarg;

	glctx_t *gcp = &glctx;
	int  argval;
	int  error = 0;

	char c;

	/*
	 * process command line options.
	 */
	while ((c = getopt(argc, argv, OPTIONS)) != (char)EOF ) {
		char *next;

		switch (c) {

		case 'v':
			set_option(VERBOSE);
			break;

		case 'h':
		case 'x':
			usage(NULL);
			/*NOTREACHED*/
			break;

		case 'V':
			printf ("memtoy " MEMTOY_VERSION " built "
			         __DATE__ " @ " __TIME__  "\n");
			exit(0);
			break;

#ifdef _DEBUG
		case '0':
			argval = strtoul(optarg, &next, 0);
			if (*next != '\0') {
				fprintf(stderr,
				    "-D <debug-mask> must be unsigned hex/decimal integer\n");
				++error;
			} else
				gcp->debug = argval;
			break;
#endif

		default:
			error=1;
			break;
		}
	}

	if (!error && optind < argc) {
		error = open_script_file(argv[optind++]);
		// TODO:  warn about ignoring extraneous args?
	}

done:

	return(error);
} 

int
main(int argc, char *argv[])
{
	glctx_t *gcp = &glctx;
	bool user_is_super;
	int error;

	init_glctx(gcp, argv[0]);
	if(!is_option(INTERACTIVE))
		setbuf(stdout, NULL);

	/*
	 * Register cleanup handler
	 */
	if (atexit(cleanup) != 0) {
		die(-1, "%s:  atexit(cleanup) registration failed\n", argv[0]);
	}

	user_is_super = (geteuid() == 0);

	error = parse_command_line_args(argc, argv);

	if (error) {
		usage(NULL);
		/*NOTREACHED*/
	}

	/*
	 * actual program logic starts here
	 */
	printf("memtoy pid:  %d\n", getpid());
	vprint("%s:  pagesize = %d\n", gcp->program_name, gcp->pagesize);
	if (gcp->numa_max_node >= 0)
		vprint("%s:  NUMA available - max node: %d\n", 
			gcp->program_name, gcp->numa_max_node);

	set_signals();

	process_commands();

	exit(0);

}
