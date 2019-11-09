CLEANFILES = *.core
#CLEANFILES += *.in
AM_MAKEFLAGS = -s

noinst_PROGRAMS=testco testsleep testclock testwaitfor testsocket testfile testprofile testsendfile testswitch testucontext testinterrupt

noinst_HEADERS = unittest.h

diags = ../src/diag.txt
BUILT_SOURCES = diag.c diag.h

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
LDFLAGS += -all-static
endif

if RDTSC
RDTSC_FLAGS = -DUSE_TSC
else
RDTSC_FLAGS =
endif

common_cflags = $(RDTSC_FLAGS) $(DEBUG_CC_FLAGS) -Wall -Wextra -Werror -std=c99 @_GNU_SOURCE_MACRO@ @_XOPEN_SOURCE_MACRO@ -I$(top_srcdir)/src -I$(top_srcdir) -I$(includedir)

common_ldflags = $(DEBUG_LD_FLAGS) -L$(libdir) -lmrkthr -lmrkcommon -lmndiag

nodist_testco_SOURCES = diag.c
testco_SOURCES = testco.c
testco_CFLAGS = $(common_cflags)
testco_LDFLAGS = $(common_ldflags)

nodist_testsleep_SOURCES = diag.c
testsleep_SOURCES = testsleep.c
testsleep_CFLAGS = $(common_cflags)
testsleep_LDFLAGS = $(common_ldflags)

nodist_testclock_SOURCES = diag.c
testclock_SOURCES = testclock.c
testclock_CFLAGS = $(common_cflags)
testclock_LDFLAGS = $(common_ldflags)

nodist_testwaitfor_SOURCES = diag.c
testwaitfor_SOURCES = testwaitfor.c
testwaitfor_CFLAGS = $(common_cflags)
testwaitfor_LDFLAGS = $(common_ldflags)

nodist_testsocket_SOURCES = diag.c
testsocket_SOURCES = testsocket.c
testsocket_CFLAGS = $(common_cflags)
testsocket_LDFLAGS = $(common_ldflags)

nodist_testfile_SOURCES = diag.c
testfile_SOURCES = testfile.c
testfile_CFLAGS = $(common_cflags)
testfile_LDFLAGS = $(common_ldflags)

nodist_testprofile_SOURCES = diag.c
testprofile_SOURCES = testprofile.c
testprofile_CFLAGS = $(common_cflags)
testprofile_LDFLAGS = $(common_ldflags)

nodist_testsendfile_SOURCES = diag.c
testsendfile_SOURCES = testsendfile.c
testsendfile_CFLAGS = $(common_cflags)
testsendfile_LDFLAGS = $(common_ldflags)

nodist_testswitch_SOURCES = diag.c
testswitch_SOURCES = testswitch.c
testswitch_CFLAGS = $(common_cflags)
testswitch_LDFLAGS = $(common_ldflags)

nodist_testinterrupt_SOURCES = diag.c
testinterrupt_SOURCES = testinterrupt.c
testinterrupt_CFLAGS = $(common_cflags)
testinterrupt_LDFLAGS = $(common_ldflags)

nodist_testucontext_SOURCES = diag.c
testucontext_SOURCES = testucontext.c
testucontext_CFLAGS = $(common_cflags)
testucontext_LDFLAGS = -L$(libdir) -lmrkcommon -lmndiag

diag.c diag.h: $(diags)
	$(AM_V_GEN) cat $(diags) | sort -u >diag.txt.tmp && mndiagen -v -S diag.txt.tmp -L mrkthr -H diag.h -C diag.c ../*.[ch] ./*.[ch]

testrun: all
	for i in $(noinst_PROGRAMS); do if test -x ./$$i; then LD_LIBRARY_PATH=$(libdir) ./$$i; fi; done;
