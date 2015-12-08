#define NO_PROFILE
#include <mrkcommon/profile.h>

#include <mrkcommon/dumpm.h>
#include <mrkcommon/util.h>
#include <mrkthr.h>
#include "unittest.h"

extern const profile_t *mrkthr_user_p;
extern const profile_t *mrkthr_swap_p;
extern const profile_t *mrkthr_sched0_p;
extern const profile_t *mrkthr_sched1_p;

const profile_t *s0_p;

static int
run(UNUSED int argc, void **argv)
{
    int i, j;
    int *n;

    PROFILE_STOP(mrkthr_swap_p);
    PROFILE_START(mrkthr_user_p);

    n = argv[0];
    j = (int)(uintptr_t)argv[1];
    for (i = 0; i < *n; ++i) {
        if (j == 0)
            PROFILE_START(s0_p);
        mrkthr_sleep(0);
        if (j == 0)
            PROFILE_STOP(s0_p);
    }
    PROFILE_STOP(mrkthr_user_p);
    PROFILE_START(mrkthr_swap_p);
    return 0;
}

int
main(UNUSED int argc, UNUSED char *argv[])
{
    int i, n;

    if (mrkthr_init() != 0) {
        perror("mrkthr_init");
        return 0;
    }

    s0_p = PROFILE_REGISTER("s0");

    n = 100;
    for (i = 0; i < 50000; ++i) {
        mrkthr_spawn("run", run, 2, &n, i);
    }

    mrkthr_loop();

    if (mrkthr_fini() != 0) {
        perror("mrkthr_fini");
        return 0;
    }
    return 0;
}

