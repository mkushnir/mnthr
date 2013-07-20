AUTOMAKE_OPTIONS = foreign
CLEANFILES = *.core
#CLEANFILES += *.in

noinst_PROGRAMS=testco testsleep testclock testwaitfor

distdir = ../../$(PACKAGE)-$(VERSION)/src/test
dist_HEADERS = unittest.h

BUILT_SOURCES = ../diag.c ../diag.h

if DEBUG
DEBUG_FLAGS = -g -O0 $(CLANG_DEBUG)
else
DEBUG_FLAGS = -DNDEBUG -O3
endif

nodist_testco_SOURCES = ../diag.c
testco_SOURCES = ../mrkthr.c ../kevent_util.c testco.c
testco_CFLAGS = $(DEBUG_FLAGS) -Wall -Wextra -Werror -std=c99 -I.. -I$(includedir)
testco_LDFLAGS = -L$(libdir) -lmrkcommon

nodist_testsleep_SOURCES = ../diag.c
testsleep_SOURCES = ../mrkthr.c ../kevent_util.c testsleep.c
testsleep_CFLAGS = $(DEBUG_FLAGS) -Wall -Wextra -Werror -std=c99 -I.. -I$(includedir)
testsleep_LDFLAGS = -L$(libdir) -lmrkcommon

nodist_testclock_SOURCES = ../diag.c
testclock_SOURCES = ../mrkthr.c ../kevent_util.c testclock.c
testclock_CFLAGS = $(DEBUG_FLAGS) -Wall -Wextra -Werror -std=c99 -I.. -I$(includedir)
testclock_LDFLAGS = -L$(libdir) -lmrkcommon

nodist_testwaitfor_SOURCES = ../diag.c
testwaitfor_SOURCES = ../mrkthr.c ../kevent_util.c testwaitfor.c
testwaitfor_CFLAGS = $(DEBUG_FLAGS) -Wall -Wextra -Werror -std=c99 -I.. -I$(includedir)
testwaitfor_LDFLAGS = -L$(libdir) -lmrkcommon

../diag.c ../diag.h: ../diag.txt
	sh ../gen-diag

testrun: all
	for i in $(noinst_PROGRAMS); do if test -x ./$$i; then LD_LIBRARY_PATH=$(libdir) ./$$i; fi; done;
