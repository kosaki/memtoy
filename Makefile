# Makefile for linux memory toy/tool
#
# This version should build as is on RHEL5, as long as the
# required <numa*.h> headers are available.
# See the README regarding Building.
#
# Use NUMA=no for building on a platform w/o libnuma
# and w/o numa headers 
#
SHELL   = /bin/sh

MACH    =

CMODE	= -std=gnu99
COPT	= $(CMODE) -g #-O2 #-non_shared

ifeq ($(NUMA),no)
	NUMA_DEFS  = -U_USE_MPOL_F_MEMS_ALLOWED -DNUMA_VERSION1_COMPATIBILITY -D_NEED_MIGRATE_PAGES
	LOCAL_INCL = -I ./include
	NUMA_STUBS = numa_stubs.o
	LIBNUMA    = 
	HDR_DIR    = ./include/
else
	NUMA_DEFS  = -D_USE_MPOL_F_MEMS_ALLOWED -DNUMA_VERSION1_COMPATIBILITY  #-D_NEED_MIGRATE_PAGES
	LOCAL_INCL =
	NUMA_STUBS =
	LIBNUMA    = -lnuma
	HDR_DIR    = /usr/include/
endif

# Comment out '-D_NEED_MIGRATE_PAGES' if/when build distro
# supports migrate_pages(2) in libnuma.
DEFS    = -D_GNU_SOURCE $(NUMA_DEFS)

# uncomment '-I ...' to build with local copy of numa headers -- i.e.,
# on platform w/o libnuma and numactl development package.
# See Makefile-nonnuma
INCLS   =  $(LOCAL_INCL)
CFLAGS  = $(COPT) $(DEFS) $(INCLS) $(ECFLAGS)

LDOPTS	= #-dnon_shared
# comment out '-lnuma' for platforms w/o libnuma -- laptops?
# See Makefile-nonnuma
LDLIBS	= -lreadline -lncurses $(LIBNUMA)
LDFLAGS = $(CMODE) $(LDOPTS) $(ELDFLAGS)

HDRS    = memtoy.h segment.h linux-list.h 

OBJS    = memtoy.o commands.o segment.o

# Include 'migrate_pages.o' for platforms w/o migrate_pages()
# syscall in libnuma.  Not needed for RHEL5 [and SLES10?]
# NOTE:  migrate_pages.o depends on <linux/sys.h>
#        RHEL5 [SLES10?] use <sys/syscall.h> instead
#EXTRAHDRS = /usr/include/linux/sys.h
EXTRAHDRS = /usr/include/sys/syscall.h

# Include 'numa_stubs.o' for platforms w/o libnuma -- laptops?
# migrate_pages.o becomes a stub if migrate_pages() syscall
# exists as determined by the EXTRAHDRS above.
# See Makefile-nonnuma
EXTRAOBJS = migrate_pages.o $(NUMA_STUBS)

PROGS	= memtoy

PROJ	= memtoy

#---------------------------------

all:    $(PROGS)

memtoy:  $(OBJS) $(EXTRAOBJS)
	$(CC) -o $@ $(LDFLAGS) $(OBJS) $(EXTRAOBJS) $(LDLIBS)

$(OBJS):    $(HDRS)

# extra dependencies to generate errors if headers are missing
commands.o: $(HDR_DIR)/numa.h $(HDR_DIR)/numaif.h migrate_pages.h
segment.o:  $(HDR_DIR)/numa.h

migrate_pages.o: migrate_pages.h $(EXTRAHDRS)

install:
	@echo "install not implemented"

clean:
	-rm -f *.o core.[0-9]* Log*

clobber: clean
	-rm -f  $(PROGS) cscope.*

# ------------------------------------------------
# N.B., renames current directory to new version name!
# [and, yes, this is really ugly...]
VERSION=$$(cat $$_WD/version.h|grep _VERSION|sed 's/^.* "\([0-9.a-z+-]*\)".*$$/\1/')
tarball:  clobber
	@_WD=`pwd`; _WD=`basename $$_WD`; cd ..;\
	_version=$(VERSION); _tarball=$(PROJ)-$${_version}.tar.gz; \
	_newWD=`echo $$_WD | sed  s:-.*:-$$_version:`; \
	if [ "$$_WD" != "$$_newWD" ] ; then \
		echo "Renaming '.' [$$_WD/] to $$_newWD/"; \
		mv $$_WD $$_newWD; \
	fi ; \
	tar czf - $$_newWD  >$$_tarball; \
	if [ $$? -eq 0 ]; then \
		echo "tarball at ../$$_tarball"; \
	else \
		echo "Error making tarball"; \
	fi
