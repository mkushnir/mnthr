#include <stdio.h>
#include <inttypes.h>


#include <mncommon/dumpm.h>
#include <mncommon/util.h>
#include <mnthr.h>
#include "unittest.h"

UNUSED static int
sleeper(UNUSED int id, UNUSED void *argv[])
{
    while (1) {
        uint64_t before, after;
        uint64_t before_nsec, after_nsec;
        int n = (int)(intptr_t)(argv[0]);

        before = mnthr_get_now_ticks_precise();
        before_nsec = mnthr_get_now_nsec_precise();
        if (mnthr_sleep(n * 1000) != 0) {
            break;
        }
        after = mnthr_get_now_ticks_precise();
        after_nsec = mnthr_get_now_nsec_precise();
        CTRACE("sleep=%Lf/%Lf", mnthr_ticks2sec(after-before), (after_nsec-before_nsec)/1000000000.L);
    }
    return 0;
}

static int
waitee(UNUSED int id, UNUSED void *argv[])
{
    UNUSED uint64_t before, after;
    UNUSED uint64_t before_nsec, after_nsec;
    int n = (int)(intptr_t)(argv[0]);
    int res;

    CTRACE("waitee %d sleeping for: %"PRId64" ...", n, (uint64_t)(n * 1000));

    before = mnthr_get_now_ticks_precise();
    before_nsec = mnthr_get_now_nsec_precise();
    if ((res = mnthr_sleep(n * 1000)) != 0) {
        CTRACE("waitee %d res=%d", n, res);
    }
    after = mnthr_get_now_ticks_precise();
    after_nsec = mnthr_get_now_nsec_precise();
    //CTRACE("sleep=%Lf/%Lf", mnthr_ticks2sec(after-before), (after_nsec-before_nsec)/1000000000.L);
    return 0;
}

UNUSED static int
waiter(UNUSED int id, UNUSED void *argv[])
{
    int res;
    int n = (int)(intptr_t)(argv[0]);

    CTRACE("waiter for waitee %d started", n);

    while (1) {
        char buf[64];

        snprintf(buf, sizeof(buf), "we%d", n);
        CTRACE(">>> about to run waitee %d ...", n);
        res = mnthr_wait_for(3000, buf, waitee, 1, n);
        CTRACE("<<< waitee %d returned %d", n, res);
        //LTRACE(n, "waitee %d res=%d", n, res);
    }
    return 0;
}

static int
spawner(UNUSED int argc, UNUSED void *argv[])
{
    int i;

    mnthr_spawn("wr2", waiter, 1, 2);
    for (i = 4; i < 6; ++i) {
        char buf[64];

        snprintf(buf, sizeof(buf), "wr%d", i);
        mnthr_spawn(buf, waiter, 1, i);
    }
    return(0);
}

static void
test0(void)
{
    if (mnthr_init() != 0) {
        perror("mnthr_init");
        return;
    }

    mnthr_spawn("spawner", spawner, 0);

    mnthr_loop();

    if (mnthr_fini() != 0) {
        perror("mnthr_fini");
        return;
    }

    return;
}

int
main(UNUSED int argc, UNUSED char *argv[])
{
    test0();
    return 0;
}

