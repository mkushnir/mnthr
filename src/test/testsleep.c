#include "mrkcommon/dumpm.h"
#include "mrkcommon/util.h"
#include "mrkthr.h"
#include "unittest.h"

static int
ff (UNUSED int id, UNUSED void *argv[])
{
    uint64_t before, after;
    uint64_t before_nsec, after_nsec;
    int n = (int)(intptr_t)(argv[0]);

    before = mrkthr_get_now_ticks_precise();
    before_nsec = mrkthr_get_now_precise();
    if (mrkthr_sleep(n * 1000) != 0) {
        return(1);
    }
    after = mrkthr_get_now_ticks_precise();
    after_nsec = mrkthr_get_now_precise();
    CTRACE("sleep=%Lf/%Lf", mrkthr_ticks2sec(after-before), (after_nsec-before_nsec)/1000000000.L);
    return(0);
}

static int
f (int id, UNUSED void *argv[])
{
    int i;

    CTRACE("id=%d", id);
    for (i = 0; i < 5; ++i) {
        mrkthr_spawn(NULL, ff, 1, i);
    }
    return(0);
}

static void
test0 (void)
{
    if (mrkthr_init() != 0) {
        perror("mrkthr_init");
        return;
    }

    mrkthr_spawn("qweqwe", f, 0);

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

