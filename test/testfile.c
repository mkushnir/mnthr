#include <assert.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>


#define TRRET_DEBUG
#include "mncommon/bytestream.h"
#include "mncommon/dumpm.h"
#include "mncommon/util.h"
#include "mnthr.h"

#include "diag.h"

#ifndef NDEBUG
const char *_malloc_options = "AJ";
#endif


static int
_bytestream_consume_data(UNUSED int argc, void **argv)
{
    mnbytestream_t *bs;
    int fd;
    int res;

    bs = argv[0];
    fd = (int)(uintptr_t)argv[1];

    res = bytestream_consume_data(bs, (void *)(intptr_t)fd);
    //mnthr_set_retval(res);
    //return res;
    MNTHRET(res);
}

static int
bytestream_consume_data_with_timeout(mnbytestream_t *bs,
                                     void *fd,
                                     uint64_t tmout)
{
    return mnthr_wait_for(tmout,
                           NULL,
                           _bytestream_consume_data,
                           2,
                           bs,
                           fd);
}

UNUSED static int
run0(UNUSED int argc, UNUSED void **argv)
{
    int fdin, fdout;
    mnbytestream_t bs;

    if ((fdin = open("qwe", O_RDONLY|O_NONBLOCK)) == -1) {
        perror("open 1");
        return 1;
    }

    if ((fdout = open("asd", O_CREAT|O_WRONLY|O_TRUNC|O_NONBLOCK, 00600)) == -1) {
        perror("open 2");
        return 1;
    }

    bytestream_init(&bs, 1024*1024);

    bs.read_more = mnthr_bytestream_read_more;
    bs.write = mnthr_bytestream_write;
    while (bytestream_consume_data_with_timeout(&bs, (void *)(intptr_t)fdin, 1000) == 0) {
        SPOS(&bs) = SEOD(&bs);
    }

    SPOS(&bs) = 0;
    if (bytestream_produce_data(&bs, (void *)(intptr_t)fdout) != 0) {
        return 1;
    }

    bytestream_fini(&bs);
    close(fdin);
    close(fdout);
    return 0;
}


UNUSED static int
run1(UNUSED int argc, UNUSED void **argv)
{
    int res;
    int fdin, fdout;
    mnbytestream_t bs;

    if ((fdin = open("qwe", O_RDONLY|O_NONBLOCK)) == -1) {
        perror("open 1");
        return 1;
    }

    if ((fdout = open("asd", O_CREAT|O_WRONLY|O_TRUNC|O_NONBLOCK, 00600)) == -1) {
        perror("open 2");
        return 1;
    }

    bytestream_init(&bs, 1024*1024);

    bs.read_more = mnthr_bytestream_read_more;
    bs.write = mnthr_bytestream_write;
    while ((res = bytestream_consume_data_with_timeout(
                &bs,
                (void *)(intptr_t)fdin,
                1000)) == 0) {
        //TRACE("res=%d", res);
        if (bytestream_produce_data(&bs, (void *)(intptr_t)fdout) != 0) {
            return 1;
        }
        bytestream_rewind(&bs);
    }

    //TRACE("res=%d", res);

    //SPOS(&bs) = 0;
    //if (bytestream_produce_data(&bs, fdout)) {
    //    return 1;
    //}

    bytestream_fini(&bs);
    close(fdin);
    close(fdout);
    return 0;
}


UNUSED static int
run2(UNUSED int argc, UNUSED void **argv)
{
    int res;
    int fdin, fdout;
    mnbytestream_t bs;

    if ((fdin = mnthr_socket_connect("10.1.2.10", "1234", PF_INET)) == -1) {
        perror("mnthr_socket_connect");
        return 1;
    }

    if ((fdout = open("asd", O_CREAT|O_WRONLY|O_TRUNC|O_NONBLOCK, 00600)) == -1) {
        perror("open 2");
        return 1;
    }

    bytestream_init(&bs, 1024*1024);

    bs.read_more = mnthr_bytestream_read_more;
    bs.write = mnthr_bytestream_write;
    while ((res = bytestream_consume_data_with_timeout(
                    &bs,
                    (void *)(intptr_t)fdin,
                    5000)) == 0) {
        if (bytestream_produce_data(&bs, (void *)(intptr_t)fdout) != 0) {
            return 1;
        }
        //D8(SDATA(&bs, 0), SEOD(&bs));
        bytestream_rewind(&bs);
    }

    bytestream_fini(&bs);
    close(fdin);
    close(fdout);
    return 0;
}


int
main(void)
{
    int res;

    mnthr_init();

    //mnthr_spawn("run0", run0, 0);
    //mnthr_spawn("run1", run1, 0);
    mnthr_spawn("run2", run2, 0);
    res = mnthr_loop();

    mnthr_fini();
    return res;
}
