#include <signal.h>
#include <ucontext.h>

#include "mnthr.h"
#include "diag.h"
#include "mncommon/dumpm.h"
#include "mncommon/util.h"
#include "unittest.h"

//static int read_pipe[2];
//static int write_pipe[2];

static mnthr_ctx_t *shutdown_timer_ctx;
static int _shutdown = 0;
static const int nthreads = 100000;
static const int niter = 1;
static const int nrecur = 50;
static long total = 0;
static int ntotal = 0;
static int wt = 100;


static void
sigterm_handler(int sig, siginfo_t *info, UNUSED ucontext_t *uap)
{
    CTRACE("sig=%d si_signo=%d si_errno=%d si_si_code=%d",
          sig, info->si_signo, info->si_errno, info->si_code);

    if (!_shutdown) {
        mnthr_run(shutdown_timer_ctx);
    } else {
        _exit(0);
    }

}

static void
r(int n)
{
    //const mnthr_ctx_t *me = mnthr_me();
    UNUSED int dummy = 1;
    UNUSED uint64_t now1, now2;
    //UNUSED const mnthr_ctx_t *me = mnthr_me();

    //CTRACE("&me=%p", &me);
    if (n >= nrecur && !_shutdown) {
        int nn = niter;
        while (nn-- && !_shutdown) {
            ++dummy;
            //CTRACE(">>>");
            //mnthr_sleep(100);
            //now1 = mnthr_get_now_nsec_precise();
            mnthr_yield();
            //CTRACE("<<<");
            //CTRACE("stack=%ld", ((uintptr_t)(me->co.uc.uc_stack.ss_sp + me->co.uc.uc_stack.ss_size)) - me->co.uc.uc_mcontext.mc_rsp);
            //mnthr_ctx_dump(me);
            //now2 = mnthr_get_now_nsec_precise();
            //printf("."); fflush(stdout);
            //printf("%ld\n", (now2 - now1) / 1000);
            //CTRACE("iter=%d %ld", nn, (now2 - now1) / 1000);
        }
    } else {
        r(n + 1);
    }
}

UNUSED static int
baz(UNUSED int argc, UNUSED void *argv[])
{
    uint64_t n1, n2;
    uint64_t t;

    n1 = mnthr_get_now_nsec_precise();
    r(0);
    n2 = mnthr_get_now_nsec_precise();
    t = n2 - n1;
    ++ntotal;
    total += t;
    //fprintf(stderr, "partial total %ld\n", (n2-n1) / 1000);
    return 0;
}

UNUSED static int
bar(UNUSED int argc, UNUSED void *argv[])
{
    UNUSED mnthr_ctx_t *ctx;
    int n = nthreads;
    long double oldtotal = total;

    while (n-- && !_shutdown) {
        ctx = mnthr_spawn("baz", baz, 0);
        //mnthr_ctx_dump(ctx);
    }
    fprintf(stderr, "All started\n");
    while (!_shutdown) {
        mnthr_sleep(wt);
        if (!ntotal) {
            continue;
        }
        fprintf(stderr, "total %Lf\n", ((long double)total / (long double)ntotal / 1000.l));
        if (oldtotal != 0. && total == oldtotal) {
            break;
        }
        oldtotal = total;
    }
    return 0;
}

UNUSED static int
asd(UNUSED int argc, void *argv[])
{
    int in = (intptr_t)argv[0];
    int out = (intptr_t)argv[1];
    int res;
    char *buf = NULL;
    off_t offset = 0;

    CTRACE();
    if (mnthr_write_all(out, "asd", sizeof("asd")) != 0) {
        return 1;
    }
    CTRACE();
    while (1) {
        CTRACE();
        if ((res = mnthr_read_all(in, &buf, &offset)) != 0) {
            CTRACE("res=%d", res);
            break;
        }
        CTRACE("asd reveived:");
        D16(buf, offset);

        if (mnthr_write_all(out, buf, offset) != 0) {
            return 1;
        }
    }
    CTRACE("asd exiting ...");
    return 0;
}


UNUSED static int
qwe(UNUSED int argc, void *argv[])
{
    int in = (intptr_t)argv[0];
    int out = (intptr_t)argv[1];
    char *buf = NULL;
    off_t offset = 0;
    int res;
    int n = 3;

    while (n--) {
        CTRACE();
        if ((res = mnthr_read_all(in, &buf, &offset)) != 0) {
            CTRACE("res=%d", res);
            break;
        }
        CTRACE("qwe reveived:");
        D16(buf, offset);
        if (mnthr_write_all(out, buf, offset) != 0) {
            return 1;
        }
    }
    CTRACE("Exiting");
    return 0;
}

static int
shut_me_down(UNUSED int argc, UNUSED void *argv[])
{
    //close(read_pipe[0]);
    //close(read_pipe[1]);
    //close(write_pipe[0]);
    //close(write_pipe[1]);
    _shutdown = 1;
    CTRACE("Shutting down ...");
    return 0;
}

static void
test0(void)
{
    int res;
    struct sigaction act = {
        .sa_flags = SA_SIGINFO,
    };
    UNUSED mnthr_ctx_t *cli;
    //mnthr_ctx_t *srv;

    sigemptyset(&(act.sa_mask));

    act.sa_sigaction = (void (*)(int, siginfo_t *, void *))sigterm_handler;
    sigaction(SIGTERM, &act, NULL);
    sigaction(SIGINT, &act, NULL);


    mnthr_init();

    shutdown_timer_ctx = mnthr_new("tm", shut_me_down, 0);


    //CTRACE();
    //if (pipe(read_pipe) != 0) {
    //    FAIL("pipe");
    //}

    //CTRACE();
    //if (pipe(write_pipe) != 0) {
    //    FAIL("pipe");
    //}

    //CTRACE();

    //srv = mnthr_spawn("qwe", qwe, 2, read_pipe[0], write_pipe[1]);

    //cli = mnthr_spawn("asd", asd, 2, write_pipe[0], read_pipe[1]);

    cli = mnthr_spawn("bar", bar, 0);

    res = mnthr_loop();

    mnthr_fini();

    CTRACE("res=%d", res);
}


int
main(UNUSED int argc, UNUSED char *argv[])
{
    TRACE("size of mnthr_ctx_t is %ld", (long)mnthr_ctx_sizeof());
    test0();
    return 0;
}

