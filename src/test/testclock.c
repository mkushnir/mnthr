#include "mrkcommon/dumpm.h"
#include "mrkcommon/util.h"
#include "mrkthr.h"
#include "unittest.h"

static int
f (UNUSED int argc, UNUSED void *argv[])
{
    while (1) {
        mrkthr_sleep(500);
        CTRACE("now=%ld", mrkthr_get_now()/1000000000);
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

