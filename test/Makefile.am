AUTOMAKE_OPTIONS = foreign subdir-objects
CLEANFILES = *.core
#CLEANFILES += *.in
AM_MAKEFLAGS = -s

noinst_PROGRAMS=testco testsleep testclock testwaitfor testsocket testfile testprofile testsendfile

noinst_HEADERS = unittest.h

BUILT_SOURCES = ../src/diag.c ../src/diag.h

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

if USE_EV
dh_platform=
ls_platform= ../src/ev_poller.c
else
dh_platform= ../src/kevent_util.h
ls_platform= ../src/kevent_poller.c ../src/kevent_util.c
endif


base_sources = ../src/mrkthr.c ../src/poller.c $(ls_platform)

common_cflags = $(RDTSC_FLAGS) $(DEBUG_FLAGS) -Wall -Wextra -Werror -std=c99 @_GNU_SOURCE_MACRO@ -I$(top_srcdir)/src -I$(top_srcdir) -I$(includedir)

nodist_testco_SOURCES = ../src/diag.c
testco_SOURCES = testco.c
testco_CFLAGS = $(common_cflags)
testco_LDFLAGS = -L$(libdir) -lmrkthr -lmrkcommon

nodist_testsleep_SOURCES = ../src/diag.c
testsleep_SOURCES = testsleep.c
testsleep_CFLAGS = $(common_cflags)
testsleep_LDFLAGS = -L$(libdir) -lmrkthr -lmrkcommon

nodist_testclock_SOURCES = ../src/diag.c
testclock_SOURCES = testclock.c
testclock_CFLAGS = $(common_cflags)
testclock_LDFLAGS = -L$(libdir) -lmrkthr -lmrkcommon

nodist_testwaitfor_SOURCES = ../src/diag.c
testwaitfor_SOURCES = testwaitfor.c
testwaitfor_CFLAGS = $(common_cflags)
testwaitfor_LDFLAGS = -L$(libdir) -lmrkthr -lmrkcommon

nodist_testsocket_SOURCES = ../src/diag.c
testsocket_SOURCES = testsocket.c
testsocket_CFLAGS = $(common_cflags)
testsocket_LDFLAGS = -L$(libdir) -lmrkthr -lmrkcommon

nodist_testfile_SOURCES = ../src/diag.c
testfile_SOURCES = testfile.c
testfile_CFLAGS = $(common_cflags)
testfile_LDFLAGS = -L$(libdir) -lmrkthr -lmrkcommon

nodist_testprofile_SOURCES = ../src/diag.c
testprofile_SOURCES = testprofile.c
testprofile_CFLAGS = $(common_cflags)
testprofile_LDFLAGS = -L$(libdir) -lmrkthr -lmrkcommon

nodist_testsendfile_SOURCES = ../src/diag.c
testsendfile_SOURCES = testsendfile.c
testsendfile_CFLAGS = $(common_cflags)
testsendfile_LDFLAGS = -L$(libdir) -lmrkthr -lmrkcommon

../src/diag.c ../src/diag.h: ../src/diag.txt
	$(AM_V_GEN) /bin/sh ../src/gen-diag mrkthr

testrun: all
	for i in $(noinst_PROGRAMS); do if test -x ./$$i; then LD_LIBRARY_PATH=$(libdir) ./$$i; fi; done;
