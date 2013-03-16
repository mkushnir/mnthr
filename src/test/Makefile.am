AUTOMAKE_OPTIONS = foreign
CLEANFILES = *.core
#CLEANFILES += *.in

noinst_PROGRAMS=testco testsleep testclock

distdir = ../../$(PACKAGE)-$(VERSION)/src/test
dist_HEADERS = unittest.h

BUILT_SOURCES = ../diag.c ../diag.h

#CFLAGS = -Wall -Wextra -Werror -std=c99 -I.. -g -O0 -ftrapv -fcatch-undefined-behavior -I$(HOME)/include
CFLAGS = -Wall -Wextra -Werror -std=c99 -I.. -O3 -DNDEBUG -I$(includedir)
LDFLAGS += -L$(libdir) -lmrkcommon

nodist_testco_SOURCES = ../diag.c
testco_SOURCES = ../mrkthr.c ../kevent_util.c testco.c
testco_CFLAGS =
testco_LDFLAGS =

nodist_testsleep_SOURCES = ../diag.c
testsleep_SOURCES = ../mrkthr.c ../kevent_util.c testsleep.c
testsleep_CFLAGS =
testsleep_LDFLAGS =

nodist_testclock_SOURCES = ../diag.c
testclock_SOURCES = ../mrkthr.c ../kevent_util.c testclock.c
testclock_CFLAGS =
testclock_LDFLAGS =

../diag.c ../diag.h: ../diag.txt
	sh ../gen-diag

testrun: all
	for i in $(noinst_PROGRAMS); do if test -x ./$$i; then LD_LIRARY_PATH=$(libdir) ./$$i; fi; done;
