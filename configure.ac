AC_INIT(mrkthr, 0.1, markiyan.kushnir@gmail.com)
if test -n "$USE_GCC"
then
    if test "$USE_GCC" = "46" && which gcc46
    then
        CC="gcc46"
        CXX="c++46"
        CPP="cpp"
    else
        if which gcc
        then
            CC="gcc"
            CXX="g++"
            CPP="cpp"
        else
            echo "Cannot find gcc"
            exit 1
        fi
    fi
else
    if which clang >/dev/null
    then
        CC="clang -Qunused-arguments -fcolor-diagnostics"
        CXX="clang++ -Qunused-arguments -fcolor-diagnostics"
        CPP="clang-cpp"
        clang_version=`clang --version | grep version | awk '{print $4;}'`
        clang_version_mjr=`echo $clang_version | awk -F. '{print $1;}'`
        clang_version_mnr=`echo $clang_version | awk -F. '{print $2;}'`
        if test "$clang_version_mjr" = "3" -a "$clang_version_mnr" -lt 3
        then
            AC_SUBST(CLANG_DEBUG, ['-ftrapv -fcatch-undefined-behavior'])
        else
            # we assume all others are 3.3+
            AC_SUBST(CLANG_DEBUG, ['-fsanitize=undefined-trap -fsanitize-undefined-trap-on-error'])
        fi
    else
        CC="cc"
        CXX="c++"
        CPP="cpp"
    fi
fi

AC_PREREQ

AC_PROG_CC
AC_PROG_CXX
AM_PROG_AR
AC_PROG_RANLIB
AC_PROG_INSTALL

LT_INIT
AM_INIT_AUTOMAKE

dnl AM_MAINTAINER_MODE

AC_CONFIG_FILES([Makefile src/Makefile man/Makefile test/Makefile])
AC_CONFIG_HEADERS(config.h)

AC_CHECK_HEADERS([syslog.h netinet/in.h sys/param.h sys/socket.h sys/time.h limits.h sys/limits.h])

AC_C_CONST
AC_C_INLINE
AC_C_RESTRICT
AC_TYPE_INT64_T
AC_TYPE_OFF_T
AC_TYPE_SIZE_T
AC_TYPE_SSIZE_T
AC_TYPE_UINT32_T
AC_TYPE_UINT64_T

AC_CHECK_FUNCS([clock_gettime munmap strerror])
AC_CHECK_FUNCS([gettimeofday])
AC_CHECK_FUNCS([memset])
AC_FUNC_MALLOC
AC_FUNC_REALLOC
AC_FUNC_MMAP


AC_PRESERVE_HELP_ORDER

AC_ARG_ENABLE(debug,
              AC_HELP_STRING([--enable-debug],
                             [Enable debug build (default=no)]),
              [AM_CONDITIONAL([DEBUG], [enable_debug=yes])],
              [AM_CONDITIONAL([DEBUG], [test "$enable_debug" = "yes"])])

AC_ARG_WITH(rdtsc,
            AC_HELP_STRING([--with-rdtsc],
                           [Use RDTSC (default=no)]),
            [AM_CONDITIONAL([RDTSC], [with_rdtsc=yes])],
            [AM_CONDITIONAL([RDTSC], [test "$with_rdtsc" = "yes"])])

if test "$with_rdtsc" = "yes"
then
    AC_LANG(C)
    AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
#include <sys/types.h>
static inline uint64_t
rdtsc(void)
{
  uint64_t res;
  __asm __volatile ("rdtsc; shl \$32,%%rdx; or %%rdx,%%rax"
                    : "=a"(res)
                    :
                    : "%rcx", "%rdx"
                   );
  return res;
} ]], [[rdtsc();]])], [AC_MSG_RESULT(checking for RDTSC... yes)], AC_MSG_FAILURE([rdtsc instruction is not supported by your CPU]))
fi

if echo $build_os | grep linux >/dev/null
then
   with_ev=yes
   AM_CONDITIONAL([USE_EV], [true])
else
   AC_ARG_WITH(ev,
               AC_HELP_STRING([--with-ev],
                              [Use libev (default=no)]),
               [AM_CONDITIONAL([USE_EV], [with_ev=yes])],
               [AM_CONDITIONAL([USE_EV], [test "$with_ev" = "yes"])])
    
fi


if test "$with_ev" = "yes"
then
    AC_CHECK_LIB(ev, ev_run, [], [AC_MSG_FAILURE(libev.so is required.)])
fi

AC_CHECK_LIB(mrkcommon, _fini, [], [AC_MSG_FAILURE(libmrkcommon.so is required.)]) 
AC_CHECK_LIB(m, modfl, [], [AC_MSG_FAILURE(libm.so is required.)]) 
AC_OUTPUT
