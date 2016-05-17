#include <assert.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#define TRRET_DEBUG
#include "mrkcommon/dumpm.h"
#include "mrkcommon/util.h"
#include "mrkthr.h"

#include "diag.h"

#ifndef NDEBUG
const char *_malloc_options = "AJ";
#endif


static int
recvthr(UNUSED int argc, void **argv)
{
    intptr_t tmp;
    int fd;

    tmp = (intptr_t)argv[0];
    fd = tmp;

    while (1) {
        char buf[1024];
        ssize_t nread;


        if ((nread = mrkthr_read_allb_et(fd, buf, sizeof(buf))) < 0) {
            break;
        }

        D8(buf, nread);
    }
    return 0;
}


static int
run0(UNUSED int argc, UNUSED void **argv)
{
    int fd;


    if ((fd = mrkthr_socket_connect("10.1.2.10", "1234", PF_INET)) == -1) {
        FAIL("mrkthr_socket_connect");
    }

    mrkthr_spawn("recvthr", recvthr, 1, (void **)(uintptr_t)fd);

    while (1) {
        char buf[1024];

        snprintf(buf, sizeof(buf), "%016lx\n", (unsigned long)mrkthr_get_now());

        if (mrkthr_write_all_et(fd, buf, strlen(buf)) != 0) {
            FAIL("mrkthr_write_all_et");
        }
        D8(buf, strlen(buf));

        mrkthr_sleep(5000);
    }

    if (fd != -1) {
        close(fd);
    }

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
