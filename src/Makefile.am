AM_MAKEFLAGS = -s
AM_LIBTOOLFLAGS = --silent

if USE_EV
dh_platform=
ls_platform= ev_poller.c
PLATFORM_FLAGS=-DUSE_EV
else
dh_platform= kevent_util.h
ls_platform= kevent_poller.c kevent_util.c
PLATFORM_FLAGS=-DUSE_KEVENT
endif

noinst_HEADERS = mnthr_private.h $(dh_platform)

diags = diag.txt
BUILT_SOURCES = diag.c diag.h
EXTRA_DIST = $(diags)
CLEANFILES = $(BUILT_SOURCES) *.core
#CLEANFILES += *.in

lib_LTLIBRARIES = libmnthr.la

nobase_include_HEADERS = mnthr.h

libmnthr_la_SOURCES = mnthr.c poller.c $(ls_platform) bytestream_helper.c
nodist_libmnthr_la_SOURCES = diag.c

DEBUG_LD_FLAGS =
if DEBUG
DEBUG_CC_FLAGS = -g -O0 @CC_DEBUG@
DEBUG_LD_FLAGS += @LIBTOOL_NO_INSTALL@
else
DEBUG_CC_FLAGS = -DNDEBUG -O3 -fomit-frame-pointer
if LTO
DEBUG_CC_FLAGS += @CC_LTO@
DEBUG_LD_FLAGS += @LD_LTO@
endif
endif

if ALLSTATIC
libmnthr_la_LDFLAGS = -all-static
else
libmnthr_la_LDFLAGS =
endif

if RDTSC
RDTSC_FLAGS = -DUSE_TSC
else
RDTSC_FLAGS =
endif

libmnthr_la_CFLAGS = $(RDTSC_FLAGS) $(DEBUG_CC_FLAGS) $(PLATFORM_FLAGS) -Wall -Wextra -Werror -std=c99 @MNCOMMON_LOCAL_CFLAGS@ @LIBEV_CFLAGS@ @_GNU_SOURCE_MACRO@ @_XOPEN_SOURCE_MACRO@ -I$(top_srcdir)/src -I$(top_srcdir) -I$(includedir)

libmnthr_la_LDFLAGS += -version-info 0:0:0 -L$(libdir) @MNCOMMON_LOCAL_LDFLAGS@ @LIBEV_LDFLAGS@
libmnthr_la_LIBADD = -lmncommon -lmndiag

diag.c diag.h: $(diags)
	$(AM_V_GEN) cat $(diags) | sort -u >diag.txt.tmp && mndiagen -v -S diag.txt.tmp -L mnthr -H diag.h -C diag.c *.[ch]

run: all
	for i in $(bin_PROGRAMS); do if test -x ./$$i; then LD_LIBRARY_PATH=$(libdir) ./$$i; fi; done;

testrun:
	for i in $(SUBDIRS); do if test "$$i" != "."; then cd $$i && $(MAKE) testrun && cd ..; fi; done;
