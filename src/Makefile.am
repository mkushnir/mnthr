AUTOMAKE_OPTIONS = foreign

distdir = ../$(PACKAGE)-$(VERSION)/src
dist_HEADERS = mrkthr_private.h kevent_util.h

BUILT_SOURCES = diag.c diag.h
EXTRA_DIST = diag.txt gen-diag
CLEANFILES = $(BUILT_SOURCES) *.core
#CLEANFILES += *.in

lib_LTLIBRARIES = libmrkthr.la

nobase_include_HEADERS = mrkthr.h

libmrkthr_la_SOURCES = mrkthr.c kevent_util.c bytestream_helper.c
nodist_libmrkthr_la_SOURCES = diag.c

if DEBUG
DEBUG_FLAGS = -g -O0 $(CLANG_DEBUG)
else
DEBUG_FLAGS = -DNDEBUG -O3
endif

libmrkthr_la_CFLAGS = $(DEBUG_FLAGS) -Wall -Wextra -Werror -std=c99 -I$(includedir)

libmrkthr_la_LDFLAGS = -version-info 0:0:0 -L$(libdir) -lmrkcommon

SUBDIRS = . test

diag.c diag.h: diag.txt
	sh ./gen-diag

run: all
	for i in $(bin_PROGRAMS); do if test -x ./$$i; then LD_LIBRARY_PATH=$(libdir) ./$$i; fi; done;

testrun:
	for i in $(SUBDIRS); do if test "$$i" != "."; then cd $$i && $(MAKE) testrun && cd ..; fi; done;
