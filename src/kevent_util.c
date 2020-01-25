#include <sys/types.h>

#include <stdio.h>
#include <sys/event.h>
#include <sys/time.h>

#include "mncommon/dumpm.h"

static const char *
kevent_flags_str (unsigned short f)
{
    static char _flags_str[1024];
#define _GEN_FLAGS(f) {f, #f}
    struct {
        unsigned short f;
        const char *s;
    } flags[] = {
        _GEN_FLAGS(EV_ADD),
        _GEN_FLAGS(EV_ENABLE),
        _GEN_FLAGS(EV_DISABLE),
        _GEN_FLAGS(EV_DISPATCH),
        _GEN_FLAGS(EV_DELETE),
        _GEN_FLAGS(EV_RECEIPT),
        _GEN_FLAGS(EV_ONESHOT),
        _GEN_FLAGS(EV_CLEAR),
        _GEN_FLAGS(EV_EOF),
        _GEN_FLAGS(EV_ERROR),
    };
    char *p = _flags_str;
    unsigned i;

    p += snprintf(p, sizeof(_flags_str), "<");
    for (i = 0;
         i < sizeof(flags)/sizeof(flags[0]);
         ++i) {

        if (f & flags[i].f) {
            p += snprintf(p,
                          sizeof(_flags_str) - (p - _flags_str),
                          "%s", flags[i].s);
            p += snprintf(p, sizeof(_flags_str), ",");
        }
    }

    p += snprintf(p - ((p - 1) == _flags_str ? 0 : 1),
                  sizeof(_flags_str),
                  ">");

    return _flags_str;
}

const char *
kevent_filter_str(int filter)
{
    return (filter == EVFILT_READ) ? "EVFILT_READ" :
          (filter == EVFILT_WRITE) ? "EVFILT_WRITE" :
          (filter == EVFILT_AIO) ? "EVFILT_AIO" :
          (filter == EVFILT_VNODE) ? "EVFILT_VNODE" :
          (filter == EVFILT_PROC) ? "EVFILT_PROC" :
          (filter == EVFILT_SIGNAL) ? "EVFILT_SIGNAL" :
          (filter == EVFILT_TIMER) ? "EVFILT_TIMER" :
          (filter == EVFILT_USER) ? "EVFILT_USER" : "<UNKNOWN>";
}

int
kevent_dump(void *data)
{
    struct kevent *kev = data;
    TRACE("%p: ident=%08lx filter=%s flags=%s[%08x] fflags=%08x data=%08lx udata=%p",
          kev,
          kev->ident,
          kevent_filter_str(kev->filter),
          kevent_flags_str(kev->flags),
          kev->flags,
          kev->fflags,
          kev->data,
          kev->udata);
    return 0;
}

int kevent_isempty(struct kevent *kev)
{
    return kev->ident == (uintptr_t)(-1);
}

int
kevent_init(void *data)
{
    struct kevent *kev = data;

    EV_SET(kev, -1, 0, 0, 0, 0, NULL);
    return 0;
}

void
kevent_copy(struct kevent *src, struct kevent *dst)
{
    EV_SET(dst, src->ident, src->filter, src->flags, src->fflags, src->data, src->udata);
}

int
kev_write(struct kevent *kev, char *data, ssize_t sz)
{
    int res = 0;
    ssize_t navail = (ssize_t)kev->data;

    if (sz > navail) {
        /* need more write */
        sz = navail;
        res = sz;
    }
    if (write(kev->ident, data, sz) != sz) {
        return -1;
    }
    return res;
}

int
kev_read(struct kevent *kev, char *buf, ssize_t sz)
{
    int res = 0;
    ssize_t navail = (ssize_t)kev->data;
    //TRACE("navail=%ld", navail);

    if (sz < navail) {
        /* more data are to read */
        res = sz;
    } else {
        sz = navail;
    }
    //TRACE("sz=%ld", sz);
    if (read(kev->ident, buf, sz) != sz) {
        return -1;
    }
    return res;
}

