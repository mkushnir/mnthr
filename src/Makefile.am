AUTOMAKE_OPTIONS = foreign
AM_MAKEFLAGS = -s

if USE_EV
dh_platform=
ls_platform= ev_poller.c
PLATFORM_FLAGS=-DUSE_EV
else
dh_platform= kevent_util.h
ls_platform= kevent_poller.c kevent_util.c
PLATFORM_FLAGS=-DUSE_KEVENT
endif

noinst_HEADERS = mrkthr_private.h $(dh_platform)

BUILT_SOURCES = diag.c diag.h
EXTRA_DIST = diag.txt gen-diag
CLEANFILES = $(BUILT_SOURCES) *.core
#CLEANFILES += *.in

lib_LTLIBRARIES = libmrkthr.la

nobase_include_HEADERS = mrkthr.h

libmrkthr_la_SOURCES = mrkthr.c poller.c $(ls_platform) bytestream_helper.c
nodist_libmrkthr_la_SOURCES = diag.c

if DEBUG
DEBUG_FLAGS = -g -O0 @CLANG_DEBUG@
else
DEBUG_FLAGS = -DNDEBUG -O3
endif

if RDTSC
RDTSC_FLAGS = -DUSE_TSC
else
RDTSC_FLAGS =
endif

libmrkthr_la_CFLAGS = $(RDTSC_FLAGS) $(DEBUG_FLAGS) $(PLATFORM_FLAGS) -Wall -Wextra -Werror -std=c99 @MRKCOMMON_LOCAL_CFLAGS@ @LIBEV_CFLAGS@ @_GNU_SOURCE_MACRO@ -I$(top_srcdir)/src -I$(top_srcdir) -I$(includedir)

libmrkthr_la_LDFLAGS = -version-info 0:0:0 -L$(libdir) -lmrkcommon @MRKCOMMON_LOCAL_LDFLAGS@ @LIBEV_LDFLAGS@

diag.c diag.h: diag.txt
	$(AM_V_GEN) /bin/sh ./gen-diag mrkthr

run: all
	for i in $(bin_PROGRAMS); do if test -x ./$$i; then LD_LIBRARY_PATH=$(libdir) ./$$i; fi; done;

testrun:
	for i in $(SUBDIRS); do if test "$$i" != "."; then cd $$i && $(MAKE) testrun && cd ..; fi; done;
