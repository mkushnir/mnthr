AUTOMAKE_OPTIONS = foreign subdir-objects
CLEANFILES = *.core
#CLEANFILES += *.in

noinst_PROGRAMS=testco testsleep testclock testwaitfor

distdir = $(top_srcdir)/$(PACKAGE)-$(VERSION)/test
dist_HEADERS = unittest.h

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

if LINUX
ls_platform= ../src/epoll_util.c
else
if FREEBSD
ls_platform= ../src/kevent_util.c
else
endif
endif

base_sources = ../src/mrkthr.c ../src/poller.c $(ls_platform)

nodist_testco_SOURCES = ../src/diag.c
testco_SOURCES = $(base_sources) testco.c
testco_CFLAGS = $(RDTSC_FLAGS) $(DEBUG_FLAGS) -Wall -Wextra -Werror -std=c99 -I../src
testco_LDFLAGS = -L$(libdir) -lmrkcommon

nodist_testsleep_SOURCES = ../src/diag.c
testsleep_SOURCES = $(base_sources) testsleep.c
testsleep_CFLAGS = $(RDTSC_FLAGS) $(DEBUG_FLAGS) -Wall -Wextra -Werror -std=c99 -I../src
testsleep_LDFLAGS = -L$(libdir) -lmrkcommon

nodist_testclock_SOURCES = ../src/diag.c
testclock_SOURCES = $(base_sources) testclock.c
testclock_CFLAGS = $(RDTSC_FLAGS) $(DEBUG_FLAGS) -Wall -Wextra -Werror -std=c99 -I../src
testclock_LDFLAGS = -L$(libdir) -lmrkcommon

nodist_testwaitfor_SOURCES = ../src/diag.c
testwaitfor_SOURCES = $(base_sources) testwaitfor.c
testwaitfor_CFLAGS = $(RDTSC_FLAGS) $(DEBUG_FLAGS) -Wall -Wextra -Werror -std=c99 -I../src
testwaitfor_LDFLAGS = -L$(libdir) -lmrkcommon

../src/diag.c ../src/diag.h: ../src/diag.txt
	sh ../src/gen-diag mrkthr

testrun: all
	for i in $(noinst_PROGRAMS); do if test -x ./$$i; then LD_LIBRARY_PATH=$(libdir) ./$$i; fi; done;
