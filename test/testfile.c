#include <assert.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>


#define TRRET_DEBUG
#include "mrkcommon/bytestream.h"
#include "mrkcommon/dumpm.h"
#include "mrkcommon/util.h"
#include "mrkthr.h"

#include "diag.h"

#ifndef NDEBUG
const char *_malloc_options = "AJ";
#endif


static int
_bytestream_consume_data(UNUSED int argc, void **argv)
{
    bytestream_t *bs;
    int fd;
    int res;

    bs = argv[0];
    fd = (int)(uintptr_t)argv[1];

    res = bytestream_consume_data(bs, fd);
    //mrkthr_set_retval(res);
    //return res;
    MRKTHRET(res);
}

static int
bytestream_consume_data_with_timeout(bytestream_t *bs, int fd, uint64_t tmout)
{
    return mrkthr_wait_for(tmout,
                           NULL,
                           _bytestream_consume_data,
                           2,
                           bs,
                           (void *)(uintptr_t)fd);
}

UNUSED static int
run0(UNUSED int argc, UNUSED void **argv)
{
    int fdin, fdout;
    bytestream_t bs;

    if ((fdin = open("qwe", O_RDONLY|O_NONBLOCK)) == -1) {
        perror("open 1");
        return 1;
    }

    if ((fdout = open("asd", O_CREAT|O_WRONLY|O_TRUNC|O_NONBLOCK, 00600)) == -1) {
        perror("open 2");
        return 1;
    }

    bytestream_init(&bs, 1024*1024);

    bs.read_more = mrkthr_bytestream_read_more;
    bs.write = mrkthr_bytestream_write;
    while (bytestream_consume_data_with_timeout(&bs, fdin, 1000) == 0) {
        SPOS(&bs) = SEOD(&bs);
    }

    SPOS(&bs) = 0;
    if (bytestream_produce_data(&bs, fdout) != 0) {
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
    bytestream_t bs;

    if ((fdin = open("qwe", O_RDONLY|O_NONBLOCK)) == -1) {
        perror("open 1");
        return 1;
    }

    if ((fdout = open("asd", O_CREAT|O_WRONLY|O_TRUNC|O_NONBLOCK, 00600)) == -1) {
        perror("open 2");
        return 1;
    }

    bytestream_init(&bs, 1024*1024);

    bs.read_more = mrkthr_bytestream_read_more;
    bs.write = mrkthr_bytestream_write;
    while ((res = bytestream_consume_data_with_timeout(&bs, fdin, 1000)) == 0) {
        //TRACE("res=%d", res);
        if (bytestream_produce_data(&bs, fdout) != 0) {
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
    bytestream_t bs;

    if ((fdin = mrkthr_socket_connect("10.1.2.10", "1234", PF_INET)) == -1) {
        perror("mrkthr_socket_connect");
        return 1;
    }

    if ((fdout = open("asd", O_CREAT|O_WRONLY|O_TRUNC|O_NONBLOCK, 00600)) == -1) {
        perror("open 2");
        return 1;
    }

    bytestream_init(&bs, 1024*1024);

    bs.read_more = mrkthr_bytestream_read_more;
    bs.write = mrkthr_bytestream_write;
    while ((res = bytestream_consume_data_with_timeout(&bs, fdin, 5000)) == 0) {
        if (bytestream_produce_data(&bs, fdout) != 0) {
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

    mrkthr_init();

    //mrkthr_spawn("run0", run0, 0);
    //mrkthr_spawn("run1", run1, 0);
    mrkthr_spawn("run2", run2, 0);
    res = mrkthr_loop();

    mrkthr_fini();
    return res;
}
