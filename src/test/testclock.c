#include "mrkcommon/dumpm.h"
#include "mrkcommon/util.h"
#include "mrkthr.h"
#include "unittest.h"

static int
f (UNUSED int argc, UNUSED void *argv[])
{
    while (1) {
        mrkthr_sleep(100);
        CTRACE("now=%ld", mrkthr_get_now()/1000000000);
    }

    return(0);
}

static void
test0 (void)
{
    int res;
    mrkthr_ctx_t *t;

    if (mrkthr_init() != 0) {
        perror("mrkthr_init");
        return;
    }

    if ((t = mrkthr_new("qweqwe", f, 0)) == NULL) {
        perror("mrkthr_new");
        return;
    }
    mrkthr_run(t);

    res = mrkthr_loop();

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

