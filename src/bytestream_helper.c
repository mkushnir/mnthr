#include <mrkcommon/bytestream.h>
#include <mrkcommon/util.h>
#include <mrkcommon/dumpm.h>
#include <mrkthr.h>

#define BLOCKSZ 4096

#ifdef USE_EV
#define mrkthr_read_allb mrkthr_read_allb_et
#define mrkthr_write_all mrkthr_write_all_et
#endif



#define MRKTHR_BYTESTREAM_READ_MORE_BODY(fn)                           \
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
mrkthr_bytestream_read_more(mnbytestream_t *stream, void *in, ssize_t sz)
{
    MRKTHR_BYTESTREAM_READ_MORE_BODY(mrkthr_read_allb);
}


ssize_t
mrkthr_bytestream_read_more_et(mnbytestream_t *stream, void *in, ssize_t sz)
{
    MRKTHR_BYTESTREAM_READ_MORE_BODY(mrkthr_read_allb_et);
}



#define MRKTHR_BYTESTREAM_WRITE_BODY(fn)                       \
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
mrkthr_bytestream_write(mnbytestream_t *stream, void *out, size_t sz)
{
    MRKTHR_BYTESTREAM_WRITE_BODY(mrkthr_write_all);
}


ssize_t
mrkthr_bytestream_write_et(mnbytestream_t *stream, void *out, size_t sz)
{
    MRKTHR_BYTESTREAM_WRITE_BODY(mrkthr_write_all_et);
}
