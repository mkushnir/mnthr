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
run0(UNUSED int argc, UNUSED void **argv)
{
    int fdin, fdout;
    bytestream_t bs;
    UNUSED ssize_t sz;

    if ((fdin = open("qwe", O_RDONLY)) == -1) {
        return 1;
    }

    if ((fdout = open("asd", O_CREAT|O_RDWR, 00700)) == -1) {
        return 1;
    }

    bytestream_init(&bs, 1024*1024);

    sz = mrkthr_get_rbuflen(fdin);
    //TRACE("mrkthr_get_rbuflen=%ld", sz);
    bs.read_more = mrkthr_bytestream_read_more;
    bs.write = mrkthr_bytestream_write;
    while (bytestream_consume_data(&bs, fdin) == 0) {
        SPOS(&bs) = SEOD(&bs);
    }

    SPOS(&bs) = 0;
    if (bytestream_produce_data(&bs, fdout)) {
        return 1;
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

    mrkthr_spawn("run0", run0, 0);
    res = mrkthr_loop();

    mrkthr_fini();
    return res;
}
