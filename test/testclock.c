#include "mncommon/dumpm.h"
#include "mncommon/util.h"
#include "mnthr.h"
#include "unittest.h"

static int
f (UNUSED int argc, UNUSED void *argv[])
{
    while (1) {
        mnthr_sleep(500);
        CTRACE("now=%ld", (long)mnthr_get_now_nsec()/1000000000);
    }

    return(0);
}

static void
test0 (void)
{
    if (mnthr_init() != 0) {
        perror("mnthr_init");
        return;
    }
    mnthr_spawn("qweqwe", f, 0);

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

