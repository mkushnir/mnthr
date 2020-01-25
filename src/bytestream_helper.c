#include <mncommon/bytestream.h>
#include <mncommon/util.h>
#include <mncommon/dumpm.h>
#include <mnthr.h>

#define BLOCKSZ 4096

#ifdef USE_EV
#define mnthr_read_allb mnthr_read_allb_et
#define mnthr_write_all mnthr_write_all_et
#endif



#define MNTHR_BYTESTREAM_READ_MORE_BODY(fn)                           \
    ssize_t nread;                                                     \
    ssize_t need;                                                      \
    int fd = (intptr_t)in;                                             \
    need = (stream->eod + sz) - stream->buf.sz;                        \
    if (need > 0) {                                                    \
        if (bytestream_grow(stream, MAX(need, stream->growsz)) != 0) { \
            return -1;                                                 \
        }                                                              \
    }                                                                  \
    if ((nread = fn(fd, stream->buf.data + stream->eod, sz)) >= 0) {   \
        /*                                                             \
        TRACE("---");                                                  \
        D8(stream->buf.data + stream->eod, nread);                     \
        TRACE("<<<");                                                  \
        */                                                             \
        stream->eod += nread;                                          \
    }                                                                  \
    return nread                                                       \


ssize_t
mnthr_bytestream_read_more(mnbytestream_t *stream, void *in, ssize_t sz)
{
    MNTHR_BYTESTREAM_READ_MORE_BODY(mnthr_read_allb);
}


ssize_t
mnthr_bytestream_read_more_et(mnbytestream_t *stream, void *in, ssize_t sz)
{
    MNTHR_BYTESTREAM_READ_MORE_BODY(mnthr_read_allb_et);
}



#define MNTHR_BYTESTREAM_WRITE_BODY(fn)                       \
    ssize_t nwritten;                                          \
    int fd = (intptr_t)out;                                    \
    if ((stream->pos + (ssize_t)sz) > stream->eod) {           \
        return -1;                                             \
    }                                                          \
    nwritten = fn(fd, stream->buf.data + stream->pos, sz);     \
    if (nwritten >= 0) {                                       \
        stream->pos += nwritten;                               \
    }                                                          \
                                                               \
    return nwritten                                            \


ssize_t
mnthr_bytestream_write(mnbytestream_t *stream, void *out, size_t sz)
{
    MNTHR_BYTESTREAM_WRITE_BODY(mnthr_write_all);
}


ssize_t
mnthr_bytestream_write_et(mnbytestream_t *stream, void *out, size_t sz)
{
    MNTHR_BYTESTREAM_WRITE_BODY(mnthr_write_all_et);
}
