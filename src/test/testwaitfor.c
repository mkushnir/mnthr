#include "mrkcommon/dumpm.h"
#include "mrkcommon/util.h"
#include "mrkthr.h"
#include "unittest.h"

static int
fff(UNUSED int argc, UNUSED void *argv[])
{
    TRACE("argc=%d", argc);
    mrkthr_sleep(200);
    TRACE("returning");
    return 1;
}

static int
ff(UNUSED int argc, UNUSED void *argv[])
{
    TRACE("argc=%d", argc);
    mrkthr_sleep(2000);
    TRACE("returning");
    return 1;
}

static int
f (UNUSED int argc, UNUSED void *argv[])
{
    int res;

    res = mrkthr_wait_for(1000, ff, 2, 123, 234);
    TRACE("res=%d", res);
    res = mrkthr_wait_for(1000, fff, 2, 123, 234);
    TRACE("res=%d", res);
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
    mrkthr_set_resume(t);

    res = mrkthr_loop();

    //TRACE("res=%d", res);

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

