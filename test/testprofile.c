#define NO_PROFILE
#include <mncommon/profile.h>

#include <mncommon/dumpm.h>
#include <mncommon/util.h>
#include <mnthr.h>
#include "unittest.h"

extern const profile_t *mnthr_user_p;
extern const profile_t *mnthr_swap_p;
extern const profile_t *mnthr_sched0_p;
extern const profile_t *mnthr_sched1_p;

const profile_t *s0_p;

static int
run(UNUSED int argc, void **argv)
{
    int i, j;
    int *n;

    PROFILE_STOP(mnthr_swap_p);
    PROFILE_START(mnthr_user_p);

    n = argv[0];
    j = (int)(uintptr_t)argv[1];
    for (i = 0; i < *n; ++i) {
        if (j == 0) {
            PROFILE_START(s0_p);
        }
        mnthr_yield();
        if (j == 0) {
            PROFILE_STOP(s0_p);
        }
    }
    PROFILE_STOP(mnthr_user_p);
    PROFILE_START(mnthr_swap_p);
    return 0;
}

int
main(UNUSED int argc, UNUSED char *argv[])
{
    int i, n;

    if (mnthr_init() != 0) {
        perror("mnthr_init");
        return 0;
    }

    s0_p = PROFILE_REGISTER("s0");

    n = 100;
    for (i = 0; i < 50000; ++i) {
        mnthr_spawn("run", run, 2, &n, i);
    }

    mnthr_loop();

    if (mnthr_fini() != 0) {
        perror("mnthr_fini");
        return 0;
    }
    return 0;
}

