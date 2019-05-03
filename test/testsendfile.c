#include <assert.h>
#include <err.h>
#include <fcntl.h>
#include <libgen.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <mrkcommon/array.h>
#include <mrkcommon/bytes.h>
#include <mrkcommon/dumpm.h>
#include <mrkcommon/util.h>

#include <mrkthr.h>

#define TSF_MODE_SERVER 0
#define TSF_MODE_CLIENT 1
static int mode = TSF_MODE_SERVER;
static char *host = NULL;
static int port = 0;
static mnarray_t files;


static void
usage(char *prog)
{
    printf(
"Usage: %s OPTIONS\n"
"\n"
"Options:\n"
"  -h           print this message and exit\n"
"  -c HOST      select host to connect to\n"
"  -s HOST      select host to listen at\n"
"  -p NUM       select port number\n"
"\n",
        basename(prog));
}


static int
run10(UNUSED int argc, void **argv)
{
    int fd;
    ssize_t total;

    assert(argc == 1);
    fd = (intptr_t)(argv[0]);
    total = 0;

    while (true) {
        ssize_t nread;
        char buf[1024];

        if ((nread = mrkthr_read_allb(fd, buf, sizeof(buf))) <= 0) {
            break;
        }
        total += nread;
    }
    //CTRACE("received %ld bytes", total);
    TRACEC(".");
    close(fd);

    return 0;
}


static int
run11(UNUSED int argc, void **argv)
{
    mnbytes_t *fpath, *sport;
    int sock, fd;
    struct stat sb;
    off_t offset;

    assert(argc == 1);

    fpath = argv[0];
    assert(fpath != NULL);
    BYTES_INCREF(fpath);

    sock = -1;
    fd = -1;
    sport = bytes_printf("%d", port);
    offset = 0;

    if ((sock = mrkthr_socket_connect(host, BCDATA(sport), AF_INET)) == -1) {
        goto err;
    }

    if ((fd = open(BCDATA(fpath), O_RDONLY)) == -1) {
        goto err;
    }
    if (fstat(fd, &sb) != 0) {
        goto err;
    }

    if (mrkthr_sendfile(fd, sock, &offset, sb.st_size) != 0) {
        goto err;
    }

    //CTRACE("sent %ld bytes of %s", sb.st_size, BDATA(fpath));
    TRACEC(".");


end:
    if (sock != -1) {
        close(sock);
    }
    if (fd != -1) {
        close(fd);
    }
    BYTES_DECREF(&fpath);
    BYTES_DECREF(&sport);
    return 0;

err:
    goto end;

}



static int
run01(UNUSED int argc, UNUSED void **argv)
{
    int res;
    int fd;
    mnbytes_t *sport;

    res = 0;
    sport = bytes_printf("%d", port);

    if ((fd = mrkthr_socket_bind(host, BCDATA(sport), AF_INET)) == -1) {
        goto err;
    }

    if (listen(fd, 10) != 0) {
        goto err;
    }

    while (true) {
        int res;
        mrkthr_socket_t *buf;
        off_t i, offset;

        buf = NULL;
        offset = 0;
        if ((res = mrkthr_accept_all2(fd, &buf, &offset)) != 0) {
            goto err;
        }

        //CTRACE("opening %ld", offset);

        for (i = 0; i < offset; ++i) {
            int fd;

            fd = buf[i].fd;
            MRKTHR_SPAWN("run10", run10, (void *)(intptr_t)fd);
        }

        if (buf != NULL) {
            free(buf);
            buf = NULL;
        }
    }

    if (fd != -1) {
        CTRACE("closing %d", fd);
        close(fd);
    }

end:
    BYTES_DECREF(&sport);

    MRKTHRET(res);

err:
    res = 1;
    goto end;
}


static int
run02(UNUSED int argc, UNUSED void **argv)
{
    mnbytes_t **s;
    mnarray_iter_t it;

    for (s = array_first(&files, &it);
         s != NULL;
         s = array_next(&files, &it)) {
        //CTRACE("file: %s", BDATA(*s));
        MRKTHR_SPAWN("run11", run11, *s);
    }

    return 0;
}


static int
run00(UNUSED int argc, UNUSED void **argv)
{
    if (mode == TSF_MODE_SERVER) {
        MRKTHR_SPAWN("run01", run01);
    } else {
        MRKTHR_SPAWN("run02", run02);
    }

    return 0;
}


static int
file_item_fini(mnbytes_t **s)
{
    BYTES_DECREF(s);
    return 0;
}


int
main(int argc, char *argv[])
{
    int opt, i;

    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        errx(1, "signal");
    }

    while ((opt = getopt(argc, argv, "hc:p:s:")) != -1) {
        switch (opt) {
            case 'c':
                mode = TSF_MODE_CLIENT;
                host = optarg;
                break;

            case 's':
                mode = TSF_MODE_SERVER;
                host = optarg;
                break;

            case 'p':
                port = strtol(optarg, NULL, 10);
                break;

            case 'h':
                usage(argv[0]);
                exit(0);
                break;

            default:
                usage(argv[0]);
                exit(0);
                break;
        }

    }

    host = strdup((host == NULL) ? "localhost" : host);

    if (port <= 0) {
        errx(1, "Invalid port: %d", port);
    }

    argc -= optind;
    argv += optind;

    array_init(&files,
               sizeof(mnbytes_t *),
               0,
               NULL,
               (array_finalizer_t)file_item_fini);
    for (i = 0; i < argc; ++i) {
        mnbytes_t **s;
        if ((s = array_incr(&files)) == NULL) {
            errx(1, "array_incr()");
        }
        *s = bytes_new_from_str(argv[i]);
        BYTES_INCREF(*s);
    }

    //TRACE("selected %s:%d", host, port);
    mrkthr_init();
    MRKTHR_SPAWN("run00", run00);
    mrkthr_loop();
    mrkthr_fini();
    array_fini(&files);
    free(host);
    host = NULL;

    return 0;
}
