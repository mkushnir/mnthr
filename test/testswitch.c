#include <mncommon/dumpm.h>
#include <mncommon/util.h>
#include <mnthr.h>


static int
s(UNUSED int argc, UNUSED void **argv)
{
    int i;

    i = (int)(intptr_t)argv[0];

    while (i--) {
        (void)mnthr_yield();
    }
    argv[0] = (void *)(intptr_t)i;
    return i;
}

static int
run0(UNUSED int argc, UNUSED void **argv)
{
    int i;

    for (i = 0; i < 2; ++i) {
        (void)MNTHR_SPAWN("s", s, 10);
    }
    return 0;
}


int
main(int argc, char **argv)
{
    (void)mnthr_init();
    (void)MNTHR_SPAWN("run0", run0, argc, argv);
    (void)mnthr_loop();
    (void)mnthr_fini();
    return 0;
}
