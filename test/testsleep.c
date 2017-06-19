#include <stdio.h>
#include <inttypes.h>


#include <mrkcommon/dumpm.h>
#include <mrkcommon/util.h>
#include <mrkthr.h>
#include "unittest.h"

UNUSED static int
sleeper(UNUSED int id, UNUSED void *argv[])
{
    while (1) {
        uint64_t before, after;
        uint64_t before_nsec, after_nsec;
        int n = (int)(intptr_t)(argv[0]);

        before = mrkthr_get_now_ticks_precise();
        before_nsec = mrkthr_get_now_precise();
        if (mrkthr_sleep(n * 1000) != 0) {
            break;
        }
        after = mrkthr_get_now_ticks_precise();
        after_nsec = mrkthr_get_now_precise();
        CTRACE("sleep=%Lf/%Lf", mrkthr_ticks2sec(after-before), (after_nsec-before_nsec)/1000000000.L);
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

    before = mrkthr_get_now_ticks_precise();
    before_nsec = mrkthr_get_now_precise();
    if ((res = mrkthr_sleep(n * 1000)) != 0) {
        CTRACE("waitee %d res=%d", n, res);
    }
    after = mrkthr_get_now_ticks_precise();
    after_nsec = mrkthr_get_now_precise();
    //CTRACE("sleep=%Lf/%Lf", mrkthr_ticks2sec(after-before), (after_nsec-before_nsec)/1000000000.L);
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
        res = mrkthr_wait_for(3000, buf, waitee, 1, n);
        CTRACE("<<< waitee %d returned %d", n, res);
        //LTRACE(n, "waitee %d res=%d", n, res);
    }
    return 0;
}

static int
spawner(UNUSED int argc, UNUSED void *argv[])
{
    int i;

    mrkthr_spawn("wr2", waiter, 1, 2);
    for (i = 4; i < 6; ++i) {
        char buf[64];

        snprintf(buf, sizeof(buf), "wr%d", i);
        mrkthr_spawn(buf, waiter, 1, i);
    }
    return(0);
}

static void
test0(void)
{
    if (mrkthr_init() != 0) {
        perror("mrkthr_init");
        return;
    }

    mrkthr_spawn("spawner", spawner, 0);

    mrkthr_loop();

    if (mrkthr_fini() != 0) {
        perror("mrkthr_fini");
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

