AUTOMAKE_OPTIONS = foreign subdir-objects
CLEANFILES = *.core
#CLEANFILES += *.in
AM_MAKEFLAGS = -s

noinst_PROGRAMS=testco testsleep testclock testwaitfor testsocket testfile testprofile

distdir = $(top_srcdir)/$(PACKAGE)-$(VERSION)/test
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

nodist_testco_SOURCES = ../src/diag.c
testco_SOURCES = testco.c
testco_CFLAGS = $(RDTSC_FLAGS) @_GNU_SOURCE_MACRO@ $(DEBUG_FLAGS) -Wall -Wextra -Werror -std=c99 -I../src
testco_LDFLAGS = -L$(libdir) -lmrkthr -lmrkcommon

nodist_testsleep_SOURCES = ../src/diag.c
testsleep_SOURCES = testsleep.c
testsleep_CFLAGS = $(RDTSC_FLAGS) @_GNU_SOURCE_MACRO@ $(DEBUG_FLAGS) -Wall -Wextra -Werror -std=c99 -I../src
testsleep_LDFLAGS = -L$(libdir) -lmrkthr -lmrkcommon

nodist_testclock_SOURCES = ../src/diag.c
testclock_SOURCES = testclock.c
testclock_CFLAGS = $(RDTSC_FLAGS) @_GNU_SOURCE_MACRO@ $(DEBUG_FLAGS) -Wall -Wextra -Werror -std=c99 -I../src
testclock_LDFLAGS = -L$(libdir) -lmrkthr -lmrkcommon

nodist_testwaitfor_SOURCES = ../src/diag.c
testwaitfor_SOURCES = testwaitfor.c
testwaitfor_CFLAGS = $(RDTSC_FLAGS) @_GNU_SOURCE_MACRO@ $(DEBUG_FLAGS) -Wall -Wextra -Werror -std=c99 -I../src
testwaitfor_LDFLAGS = -L$(libdir) -lmrkthr -lmrkcommon

nodist_testsocket_SOURCES = ../src/diag.c
testsocket_SOURCES = testsocket.c
testsocket_CFLAGS = $(RDTSC_FLAGS) @_GNU_SOURCE_MACRO@ $(DEBUG_FLAGS) -Wall -Wextra -Werror -std=c99 -I../src
testsocket_LDFLAGS = -L$(libdir) -lmrkthr -lmrkcommon

nodist_testfile_SOURCES = ../src/diag.c
testfile_SOURCES = testfile.c
testfile_CFLAGS = $(RDTSC_FLAGS) @_GNU_SOURCE_MACRO@ $(DEBUG_FLAGS) -Wall -Wextra -Werror -std=c99 -I../src
testfile_LDFLAGS = -L$(libdir) -lmrkthr -lmrkcommon

nodist_testprofile_SOURCES = ../src/diag.c
testprofile_SOURCES = testprofile.c
testprofile_CFLAGS = $(RDTSC_FLAGS) @_GNU_SOURCE_MACRO@ $(DEBUG_FLAGS) -Wall -Wextra -Werror -std=c99 -I../src
testprofile_LDFLAGS = -L$(libdir) -lmrkthr -lmrkcommon

../src/diag.c ../src/diag.h: ../src/diag.txt
	$(AM_V_GEN) /bin/sh ../src/gen-diag mrkthr

testrun: all
	for i in $(noinst_PROGRAMS); do if test -x ./$$i; then LD_LIBRARY_PATH=$(libdir) ./$$i; fi; done;
