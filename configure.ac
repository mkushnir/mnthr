AC_INIT(mrkthr, 0.1, markiyan.kushnir@gmail.com)

AS_IF([test -n "$USE_GCC"],
[
    probe="gcc${USE_GCC}"
    AS_IF([which $probe >/dev/null],
    [
        CC="gcc${USE_GCC}"
        CXX="c++${USE_GCC}"
        CPP="cpp"
    ],
    [
        AS_IF([which gcc >/dev/null],
        [
            CC="gcc"
            CXX="g++"
            CPP="cpp"
        ],
        [
            echo "Cannot find gcc"
            exit 1
        ])
    ])
    AC_SUBST(CC_DEBUG, ['-ftrapv'])
],
[
    AS_IF([which clang >/dev/null],
    [
        CC="clang -Qunused-arguments -fcolor-diagnostics"
        CXX="clang++ -Qunused-arguments -fcolor-diagnostics"
        CPP="clang-cpp"
        clang_version=`clang --version | grep version | awk '{print $4;}'`
        clang_version_mjr=`echo $clang_version | awk -F. '{print $1;}'`
        clang_version_mnr=`echo $clang_version | awk -F. '{print $2;}'`
        AS_IF([test "$clang_version_mjr" = "3" -a "$clang_version_mnr" -lt 3],
            [AC_SUBST(CC_DEBUG, ['-ftrapv -fcatch-undefined-behavior'])],
            [AC_SUBST(CC_DEBUG, ['-fsanitize=undefined-trap -fsanitize-undefined-trap-on-error -fno-sanitize=alignment'])])
        AC_SUBST(CC_LTO, ['-flto'])
        AC_SUBST(LD_LTO, ['-Wl,--lto-O3'])
    ],
    [
        CC="cc"
        CXX="c++"
        CPP="cpp"
        AC_SUBST(CC_DEBUG, ['-ftrapv'])
    ])
])

AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_AUX_DIR([.ac-aux])
AC_CONFIG_FILES([Makefile src/Makefile man/Makefile test/Makefile])
AC_CONFIG_HEADERS(config.h)

AC_CANONICAL_HOST
AC_CANONICAL_TARGET

AS_CASE(["$target_os"],
    [*linux*], [AC_SUBST(_GNU_SOURCE_MACRO, ['-D_GNU_SOURCE'])],
    [AC_SUBST(_GNU_SOURCE_MACRO)]
)

AS_CASE(["$target_os"],
    [*darwin*], [AC_SUBST(_XOPEN_SOURCE_MACRO, ['-D_XOPEN_SOURCE'])],
    [AC_SUBST(_XOPEN_SOURCE_MACRO)]
)

AC_PREREQ

AC_PROG_CC
AC_PROG_CXX
AM_PROG_AR
AC_PROG_INSTALL

AM_INIT_AUTOMAKE([foreign subdir-objects])
LT_INIT
AM_SILENT_RULES([yes])

dnl AM_MAINTAINER_MODE

AC_CHECK_HEADER_STDBOOL

AC_CHECK_HEADERS([syslog.h netinet/in.h sys/param.h sys/types.h sys/socket.h sys/uio.h sys/time.h sys/sendfile.h limits.h sys/user.h fcntl.h netdb.h sys/ioctl.h])

AC_C_CONST
AC_C_INLINE
AC_C_RESTRICT
AC_C_FLEXIBLE_ARRAY_MEMBER
AC_TYPE_INT64_T
AC_TYPE_OFF_T
AC_TYPE_SIZE_T
AC_TYPE_SSIZE_T
AC_TYPE_UINT32_T
AC_TYPE_UINT64_T
AC_CHECK_FUNCS([clock_gettime munmap strerror gettimeofday memset socket strdup sendfile])
AC_FUNC_MALLOC
AC_FUNC_REALLOC
AC_FUNC_MMAP

AC_CHECK_TYPE([struct sf_hdtr],
                [AC_MSG_NOTICE([struct sf_hdtr is defined])
                 AC_DEFINE([HAVE_SF_HDTR], [1], [Define to 1 if struct sf_hdtr is defined])
                ],
                [ ], [[
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
]])

AC_COMPILE_IFELSE(
    [AC_LANG_PROGRAM([[
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#ifdef HAVE_SYS_SENDFILE_H
#include <sys/sendfile.h>
#endif
]], [[ (void)sendfile(-1, -1, 0, (void*)0, (void*)0, 0); ]])],
    [AC_DEFINE([SENDFILE_DARWIN_STYLE], [], [Define if the sendfile is a Darwin-style call])],
    [])


AC_PRESERVE_HELP_ORDER

AC_SUBST(MRKCOMMON_LOCAL_CFLAGS, [''])
AC_SUBST(MRKCOMMON_LOCAL_LDFLAGS, [''])

AC_ARG_ENABLE(debug,
              AC_HELP_STRING([--enable-debug],
                             [Enable debug build (default=no)]))

AM_CONDITIONAL([DEBUG], [test "$enable_debug" = "yes"])

AC_ARG_ENABLE(allstatic,
              AC_HELP_STRING([--enable-allstatic],
                             [Enable all static build (default=no)]),
              [AC_MSG_NOTICE([Will build statically])],
              [AC_MSG_NOTICE([Will not build statically])])

AM_CONDITIONAL([ALLSTATIC], [test "$enable_allstatic" = "yes"])

AC_ARG_ENABLE(lto,
              AC_HELP_STRING([--enable-lto],
                             [Enable all static build (default=no)]),
              [AC_MSG_NOTICE([Will enable LTO])],
              [AC_MSG_NOTICE([Will not enable LTO])])

AM_CONDITIONAL([LTO], [test "$enable_lto" = "yes"])

AC_ARG_WITH(rdtsc,
            AC_HELP_STRING([--with-rdtsc],
                           [Use RDTSC (default=no)]),
            [AM_CONDITIONAL([RDTSC], [with_rdtsc=yes])],
            [AM_CONDITIONAL([RDTSC], [test "$with_rdtsc" = "yes"])])

AC_LANG(C)


AS_IF([test "$with_rdtsc" = "yes"],
    [AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
static inline unsigned long long
rdtsc(void)
{
  unsigned long long res;
  __asm __volatile ("rdtsc; shl \$32,%%rdx; or %%rdx,%%rax"
                    : "=a"(res)
                    :
                    : "%rcx", "%rdx"
                   );
  return res;
} ]], [[rdtsc();]])], [AC_MSG_RESULT(checking for RDTSC... yes)], AC_MSG_FAILURE([rdtsc instruction is not supported by your CPU]))])


AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
#include <sys/param.h> /* PAGE_SIZE */
#ifndef PAGE_SIZE
#   ifdef HAVE_SYS_USER_H
#       include <sys/user.h>
#   else
#       error "PAGE_SIZE cannot be defined"
#   endif
#endif
char x[PAGE_SIZE];
]], [
])], [AC_DEFINE([HAVE_PAGE_SIZE_CONSTANT], [], [Define if PAGE_SIZE is a constant value])], [])


AS_IF([echo $build_os | grep linux >/dev/null],
    [with_ev=yes
     AM_CONDITIONAL([USE_EV], [true])],
    [AC_ARG_WITH(ev,
                 AC_HELP_STRING([--with-ev],
                                [Use libev (default=no)]),
                 [AM_CONDITIONAL([USE_EV], [with_ev=yes])],
                 [AM_CONDITIONAL([USE_EV], [test "$with_ev" = "yes"])])])


AS_IF([test "$with_ev" = "yes"],
    [#AC_CHECK_LIB(ev, ev_run, [], [AC_MSG_FAILURE(libev.so is required.)])
     AC_SUBST(LIBEV_CFLAGS, [''])
     AC_SUBST(LIBEV_LDFLAGS, ['-lev'])],
    [AC_SUBST(LIBEV_CFLAGS, [''])
     AC_SUBST(LIBEV_LDFLAGS, [''])])

#AC_CHECK_LIB(mrkcommon, _fini, [], [AC_MSG_FAILURE(libmrkcommon.so is required.)]) 
AC_CHECK_LIB(m, modfl, [], [AC_MSG_FAILURE(libm.so is required.)]) 
AC_OUTPUT
