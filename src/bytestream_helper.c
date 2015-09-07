#include "mrkcommon/bytestream.h"
#include "mrkcommon/util.h"
#include "mrkthr.h"

#define BLOCKSZ 4096

#ifdef USE_EV
#define mrkthr_read_allb mrkthr_read_allb_et
#define mrkthr_write_all mrkthr_write_all_et
#endif

ssize_t
mrkthr_bytestream_read_more(bytestream_t *stream, int fd, ssize_t sz)
{
    ssize_t nread;
    ssize_t need;

    need = (stream->eod + sz) - stream->buf.sz;

    if (need > 0) {
        if (bytestream_grow(stream, MAX(need, BLOCKSZ)) != 0) {
            return -1;
        }
    }

    if ((nread = mrkthr_read_allb(fd,
                                  stream->buf.data + stream->eod,
                                  sz)) >= 0) {
        stream->eod += nread;
    }

    return nread;
}

ssize_t
mrkthr_bytestream_write(bytestream_t *stream, int fd, size_t sz)
{
    ssize_t nwritten;

    if ((stream->pos + (ssize_t)sz) > stream->eod) {
        return -1;
    }

    nwritten = mrkthr_write_all(fd, stream->buf.data + stream->pos, sz);
    stream->pos += nwritten;

    return nwritten;
}

