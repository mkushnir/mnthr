#include <assert.h>
#include <stdbool.h>
#include <signal.h>

#include <mncommon/util.h>
#include <mncommon/dumpm.h>

#include <mnthr.h>

static mnthr_signal_t sig;

static void
myterm(UNUSED int sig)
{
    mnthr_shutdown();
}

static int
worker10(UNUSED int argc, UNUSED void *argv[])
{
    while (true) {
        int res;
        if ((res = mnthr_sleep(2000)) != 0) {
            CTRACE("res=%s", MNTHR_CO_RC_STR(res));
            break;
        }
    }
    return 0;
}

static int
worker11(UNUSED int argc, UNUSED void *argv[])
{
    mnthr_signal_init(&sig, mnthr_me());
    while (true) {
        int res;
        if ((res = mnthr_signal_subscribe(&sig)) != 0) {
            CTRACE("res=%s (%02x)", MNTHR_CO_RC_STR(res), res);
            break;
        } else {
            res = mnthr_get_retval();
            CTRACE("res=%s", MNTHR_CO_RC_STR(res));
        }
    }
    mnthr_signal_fini(&sig);
    return 0;
}




static int
worker2(UNUSED int argc, void *argv[])
{
    UNUSED mnthr_ctx_t *w10, *w11;
    int n = 3;

    w10 = argv[0];
    w11 = argv[1];
    while (true) {
        int res;
        if ((res = mnthr_sleep(2300)) != 0) {
            break;
        }
        CTRACE("n=%d", n);
        if (--n <= 0) {
            if (mnthr_is_runnable(w10)) {
                mnthr_set_interrupt(w10);
            }

            if (mnthr_signal_has_owner(&sig)) {
                if (n >= -3) {
                    mnthr_signal_send(&sig);
                } else {
                    mnthr_signal_error(&sig, 123);
                }
            }
        }
    }
    return 0;
}


static int
run(UNUSED int argc, UNUSED void *argv[])
{

    mnthr_ctx_t *w10, *w11;
    w10 = MNTHR_SPAWN("w10", worker10);
    w11 = MNTHR_SPAWN("w11", worker11);
    MNTHR_SPAWN("w2", worker2, w10, w11);
    return 0;
}

int
main(void)
{
    if (signal(SIGINT, myterm) == SIG_ERR) {
        return 1;
    }
    if (signal(SIGTERM, myterm) == SIG_ERR) {
        return 1;
    }

    (void)mnthr_init();
    MNTHR_SPAWN("run", run);
    (void)mnthr_loop();
    (void)mnthr_fini();

}
