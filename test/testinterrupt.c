#include <assert.h>
#include <stdbool.h>
#include <signal.h>

#include <mrkcommon/util.h>
#include <mrkcommon/dumpm.h>

#include <mrkthr.h>

static mrkthr_signal_t sig;

static void
myterm(UNUSED int sig)
{
    mrkthr_shutdown();
}

static int
worker10(UNUSED int argc, UNUSED void *argv[])
{
    while (true) {
        int res;
        if ((res = mrkthr_sleep(2000)) != 0) {
            CTRACE("res=%s", MRKTHR_CO_RC_STR(res));
            break;
        }
    }
    return 0;
}

static int
worker11(UNUSED int argc, UNUSED void *argv[])
{
    mrkthr_signal_init(&sig, mrkthr_me());
    while (true) {
        int res;
        if ((res = mrkthr_signal_subscribe(&sig)) != 0) {
            CTRACE("res=%s (%02x)", MRKTHR_CO_RC_STR(res), res);
            break;
        } else {
            res = mrkthr_get_retval();
            CTRACE("res=%s", MRKTHR_CO_RC_STR(res));
        }
    }
    mrkthr_signal_fini(&sig);
    return 0;
}




static int
worker2(UNUSED int argc, void *argv[])
{
    mrkthr_ctx_t *w10, *w11;
    int n = 3;

    w10 = argv[0];
    w11 = argv[1];
    while (true) {
        int res;
        if ((res = mrkthr_sleep(2300)) != 0) {
            break;
        }
        CTRACE("n=%d", n);
        if (--n <= 0) {
            if (mrkthr_is_runnable(w10)) {
                mrkthr_set_interrupt(w10);
            }

            if (mrkthr_signal_has_owner(&sig)) {
                if (n >= -3) {
                    mrkthr_signal_send(&sig);
                } else {
                    mrkthr_signal_error(&sig, 123);
                }
            }
        }
    }
    return 0;
}


static int
run(UNUSED int argc, UNUSED void *argv[])
{

    mrkthr_ctx_t *w10, *w11;
    w10 = MRKTHR_SPAWN("w10", worker10);
    w11 = MRKTHR_SPAWN("w11", worker11);
    MRKTHR_SPAWN("w2", worker2, w10, w11);
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

    (void)mrkthr_init();
    MRKTHR_SPAWN("run", run);
    (void)mrkthr_loop();
    (void)mrkthr_fini();

}
