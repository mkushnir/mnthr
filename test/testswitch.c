#include <mrkcommon/dumpm.h>
#include <mrkcommon/util.h>
#include <mrkthr.h>


static int
s(UNUSED int argc, UNUSED void **argv)
{
    int i = 1000000;
    while (i--) {
        (void)mrkthr_sleep(0);
    }
    return 0;
}

static int
run0(UNUSED int argc, UNUSED void **argv)
{
    int i;

    for (i = 0; i < 2; ++i) {
        (void)MRKTHR_SPAWN("s", s);
    }
    return 0;
}


int
main(int argc, char **argv)
{
    (void)mrkthr_init();
    (void)MRKTHR_SPAWN("run0", run0, argc, argv);
    (void)mrkthr_loop();
    (void)mrkthr_fini();
    return 0;
}
