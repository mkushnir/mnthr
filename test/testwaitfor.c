#include "mncommon/dumpm.h"
#include "mncommon/util.h"
#include "mnthr.h"
#include "unittest.h"

UNUSED static int
fff(UNUSED int argc, UNUSED void *argv[])
{
    CTRACE("argc=%d", argc);
    mnthr_sleep(200);
    CTRACE("returning");
    return 1;
}

UNUSED static int
ff(UNUSED int argc, UNUSED void *argv[])
{
    CTRACE("argc=%d", argc);
    mnthr_sleep(2000);
    CTRACE("returning");
    return 1;
}

static int
r (UNUSED int argc, UNUSED void *argv[])
{
    char buf[1024];
    ssize_t nread;

    memset(buf, 0, sizeof(buf));

    nread = mnthr_read_allb(0, buf, sizeof(buf));
    CTRACE("nread=%ld", nread);
    if (nread > 0) {
        CTRACE("buf='%s'", buf);
    }
    return 0;
}

static int
f (UNUSED int argc, UNUSED void *argv[])
{
    int res;

    res = mnthr_wait_for(1000, "one", ff, 2, 123, 234);
    CTRACE("res=%d", res);
    res = mnthr_wait_for(1000, "two", fff, 2, 123, 234);
    CTRACE("res=%d", res);
    mnthr_sleep(3000);
    CTRACE(FGREEN("Now type something, waiting for 5 secs ..."));
    res = mnthr_wait_for(5000, "three", r, 0);
    CTRACE("res=%d", res);
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

    //TRACE("res=%d", res);

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

