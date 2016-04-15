#include <assert.h>
#include <errno.h>
#include <math.h> /* INFINITY */
#include <string.h>
#include <sys/ioctl.h>

#define NO_PROFILE
#include <mrkcommon/profile.h>

#ifdef DO_MEMDEBUG
#include <mrkcommon/memdebug.h>
MEMDEBUG_DECLARE(mrkthr_ev_poller);
#endif

#include <mrkcommon/bytes.h>
#include <mrkcommon/hash.h>
#include <mrkcommon/fasthash.h>
/* Experimental trie use */
#include <mrkcommon/btrie.h>
#include <mrkcommon/util.h>

#include "mrkthr_private.h"

//#define TRACE_VERBOSE
#include "diag.h"
#include <mrkcommon/dumpm.h>

#include <ev.h>

extern const profile_t *mrkthr_user_p;
extern const profile_t *mrkthr_swap_p;
extern const profile_t *mrkthr_sched0_p;
extern const profile_t *mrkthr_sched1_p;

static const char *
ev_str(int e)
{
    static char buf[1024];

    if (e == 0) {
        snprintf(buf, sizeof(buf), "NONE");
    } else if (e == EV_UNDEF) {
        snprintf(buf, sizeof(buf), "UNDEF");
    } else {
#define EV_STR_CASE(E) if (e & EV_##E) nwritten += snprintf(buf + nwritten, sizeof(buf) - nwritten, "|" #E)

        int nwritten;

        nwritten = 0;

        if (e & EV_READ) {
            nwritten += snprintf(buf + nwritten, sizeof(buf) - nwritten, "READ");
        }
        EV_STR_CASE(WRITE);
        EV_STR_CASE(_IOFDSET);
        EV_STR_CASE(TIMER);
        EV_STR_CASE(PERIODIC);
        EV_STR_CASE(SIGNAL);
        EV_STR_CASE(CHILD);
        EV_STR_CASE(STAT);
        EV_STR_CASE(IDLE);
        EV_STR_CASE(PREPARE);
        EV_STR_CASE(CHECK);
        EV_STR_CASE(EMBED);
        EV_STR_CASE(FORK);
        EV_STR_CASE(CLEANUP);
        EV_STR_CASE(ASYNC);
        EV_STR_CASE(CUSTOM);
        EV_STR_CASE(ERROR);
    }

    return buf;
}


#define EV_STR(e) ev_str(e)

#pragma GCC diagnostic ignored "-Wstrict-aliasing"

typedef struct _ev_item {
    union {
        ev_io io;
        ev_stat stat;
    } ev;
    bytes_t *stat_path;
    /*
     * ev.io->fd, ev.io->events
     * ev.stat->wd, ev.stat->path
     */
    uint64_t hash;
#define EV_TYPE_IO 1
#define EV_TYPE_STAT 2
    int ty;
} ev_item_t;

hash_t events;

static struct ev_loop *the_loop;

static void ev_io_cb(EV_P_ ev_io *, int);
static void ev_stat_cb(EV_P_ ev_stat *, int);

static ev_idle eidle;
static ev_timer etimer;
static ev_prepare eprepare;
static ev_check echeck;


static uint64_t timecounter_now;

/**
 *
 * ev_item
 */

static uint64_t
ev_item_hash(ev_item_t *ev)
{
    if (ev->hash == 0) {
        union {
            int *i;
            const unsigned char *c;
            uint64_t *h;
        } u;

        u.i = &ev->ty;
        ev->hash = fasthash(0, u.c, sizeof(ev->ty));
        if (ev->ty == EV_TYPE_IO) {
            int _events;
            u.i = &ev->ev.io.fd;
            ev->hash = fasthash(ev->hash, u.c, sizeof(ev->ev.io.fd));
            _events = ev->ev.io.events & ~EV__IOFDSET;
            u.i = &_events;
            ev->hash = fasthash(ev->hash, u.c, sizeof(_events));
        } else if (ev->ty == EV_TYPE_STAT) {
            uint64_t h;
            h = bytes_hash(ev->stat_path);
            u.h = &h;
            ev->hash = fasthash(ev->hash, u.c, sizeof(u.h));
        } else {
            TRACE("ev->ty=%d", ev->ty);
            FAIL("ev_item_hash");
        }
    }
    return ev->hash;
}


static int
ev_item_cmp(ev_item_t *a, ev_item_t *b)
{
    uint64_t ha, hb;

    ha = ev_item_hash(a);
    hb = ev_item_hash(b);
    if (ha != hb) {
        return ha > hb ? 1 : ha < hb ? -1 : 0;
    }
    if (a->ty != b->ty) {
        return a->ty - b->ty;
    }
    if (a->ty == EV_TYPE_IO) {
        if (a->ev.io.fd == b->ev.io.fd) {
            if (a->ev.io.events == b->ev.io.events) {
                return 0;
            }
            return a->ev.io.events > b->ev.io.events ? 1 : -1;
        }
        return a->ev.io.fd > b->ev.io.fd ? 1 : -1;
    } else if (a->ty == EV_TYPE_STAT) {
        return bytes_cmp(a->stat_path, b->stat_path);
    } else {
        FAIL("ev_item_cmp");
    }
    return 0;
}


static ev_item_t *
ev_item_new_io(int fd, int event)
{
    ev_item_t *res;

    if ((res = malloc(sizeof(ev_item_t))) == NULL) {
        FAIL("malloc");
    }
    ev_io_init(&res->ev.io, ev_io_cb, fd, event);
    res->ev.io.data = NULL;
    res->hash = 0;
    res->ty = EV_TYPE_IO;

    return res;
}


static ev_item_t *
ev_item_new_stat(const char *path, UNUSED int event)
{
    ev_item_t *res;

    if ((res = malloc(sizeof(ev_item_t))) == NULL) {
        FAIL("malloc");
    }
    res->stat_path = bytes_new_from_str(path);
    BYTES_INCREF(res->stat_path);
    ev_stat_init(&res->ev.stat,
                 ev_stat_cb,
                 res->stat_path->data, 0.0);
    res->ev.stat.data = NULL;
    res->hash = 0;
    res->ty = EV_TYPE_STAT;

    return res;
}


static void
ev_item_destroy(ev_item_t **ev)
{
    if (*ev != NULL) {
        if ((*ev)->ty == EV_TYPE_IO) {
#ifdef TRACE_VERBOSE
            CTRACE(FRED("destroying ev_io %d/%s"),
                   (*ev)->ev.io.fd,
                   EV_STR((*ev)->ev.io.events));
#endif
            ev_io_stop(the_loop, &(*ev)->ev.io);
        } else if ((*ev)->ty == EV_TYPE_STAT) {
#ifdef TRACE_VERBOSE
            CTRACE(FRED("destroying ev_stat %s/%d"),
                   (*ev)->ev.stat.path, (*ev)->ev.stat.wd);
#endif
            ev_stat_stop(the_loop, &(*ev)->ev.stat);
            BYTES_DECREF(&(*ev)->stat_path);
        } else {
            FAIL("ev_item_destroy");
        }
        free(*ev);
        *ev = NULL;
    }
}


static int
ev_item_fini(ev_item_t *key, UNUSED void *v)
{
    ev_item_destroy(&key);
    return 0;
}


static ev_item_t *
ev_io_item_get(int fd, int event)
{
    ev_item_t probe, *ev;
    hash_item_t *dit;

    probe.ev.io.fd = fd;
    probe.ev.io.events = event;
    probe.hash = 0;
    probe.ty = EV_TYPE_IO;

    if ((dit = hash_get_item(&events, &probe)) == NULL) {
        ev = ev_item_new_io(fd, event);
        hash_set_item(&events, ev, NULL);
    } else {
        ev = dit->key;
    }
    return ev;
}

static ev_item_t *
ev_stat_item_get(const char *path, UNUSED int event)
{
    ev_item_t probe, *ev;
    hash_item_t *dit;

    probe.stat_path = bytes_new_from_str(path);
    probe.hash = 0;
    probe.ty = EV_TYPE_STAT;

    if ((dit = hash_get_item(&events, &probe)) == NULL) {
        ev = ev_item_new_stat(path, event);
        hash_set_item(&events, ev, NULL);
    } else {
        ev = dit->key;
    }
    BYTES_DECREF(&probe.stat_path);
    return ev;
}


mrkthr_stat_t *
mrkthr_stat_new(const char *path)
{
    mrkthr_stat_t *res;
    if ((res = malloc(sizeof(mrkthr_stat_t))) == NULL) {
        FAIL("malloc");
    }
    res->ev = ev_stat_item_get(path, 0);
    return res;
}


void
mrkthr_stat_destroy(mrkthr_stat_t **st)
{
    if (*st != NULL) {
        hash_item_t *hit;

        if ((hit = hash_get_item(&events, (*st)->ev)) == NULL) {
            FAIL("mrkthr_stat_destroy");
        }
        hash_delete_pair(&events, hit);
        free(*st);
    }
}


int
mrkthr_stat_wait(mrkthr_stat_t *st)
{
    int res;
    hash_item_t *hit;
    ev_item_t *ev;

    assert(st->ev->ty == EV_TYPE_STAT);
    me->pdata._ev = st->ev;
    if (st->ev->ev.stat.data == NULL) {
        st->ev->ev.stat.data = me;
    } else if (st->ev->ev.stat.data != me) {
        /*
         * in this case we are not allowed to wait for this event,
         * sorry.
         */
        me->co.rc = CO_RC_SIMULTANEOUS;
        return -1;
    }

#ifdef TRACE_VERBOSE
    CTRACE(FBBLUE("starting ev_stat %s/%d"),
           st->ev->ev.stat.path,
           st->ev->ev.stat.wd);
#endif
    ev_stat_start(the_loop, &st->ev->ev.stat);

    /* wait for an event */
    me->co.state = CO_STATE_READ;
    res = yield();
    if (res != 0) {
        return -1;
    }

    if ((hit = hash_get_item(&events, st->ev)) == NULL) {
        FAIL("mrkthr_stat_wait");
    }
    ev = hit->key;

    assert(ev == st->ev);

    assert(ev->ev.stat.data == me);

    ev->ev.stat.data = NULL;
    me->pdata._ev = NULL;

    return res;
}


/**
 *
 * The ev backend.
 *
 */

/**
 * Time bookkeeping
 */
uint64_t
poller_msec2ticks_absolute(uint64_t msec)
{
    return timecounter_now + msec * 1000000;
}


uint64_t
mrkthr_msec2ticks(uint64_t msec)
{
    return msec * 1000000;
}


uint64_t
poller_ticks_absolute(uint64_t ticks)
{
    return timecounter_now + ticks;
}


long double
mrkthr_ticks2sec(uint64_t ticks)
{
    return (long double)ticks / (long double)1000000.0;
}


long double
mrkthr_ticksdiff2sec(int64_t ticks)
{
    return (long double)ticks / (long double)1000000.0;
}


uint64_t
mrkthr_get_now(void)
{
    return timecounter_now;
}


uint64_t
mrkthr_get_now_precise(void)
{
    return timecounter_now;
}


uint64_t
mrkthr_get_now_ticks(void)
{
    return timecounter_now;
}


uint64_t
mrkthr_get_now_ticks_precise(void)
{
    return timecounter_now;
}


/**
 * Async events
 *
 */
void
poller_clear_event(mrkthr_ctx_t *ctx)
{
    if (ctx->pdata._ev != NULL) {
        ev_item_t *ev;

        ev = ctx->pdata._ev;
        if (ev->ty == EV_TYPE_IO) {
#ifdef TRACE_VERBOSE
        CTRACE(FRED("clearing ev_io %d/%s"),
               ev->ev.io.fd,
               EV_STR(ev->ev.io.events));
#endif
            ev_io_stop(the_loop, &ev->ev.io);
        } else if (ev->ty == EV_TYPE_STAT) {
#ifdef TRACE_VERBOSE
        CTRACE(FRED("clearing ev_stat %s/%d"),
               ev->ev.stat.path,
               ev->ev.stat.wd);
#endif
            ev_stat_stop(the_loop, &ev->ev.stat);
        } else {
            FAIL("poller_clear_event");
        }
    }
}


static void
clear_event_io(ev_item_t *ev)
{
#ifdef TRACE_VERBOSE
    CTRACE(FRED("clearing ev_io %d/%s"),
           ev->ev.io.fd,
           EV_STR(ev->ev.io.events));
#endif
    ev_io_stop(the_loop, &ev->ev.io);
}


static void
clear_event_stat(ev_item_t *ev)
{
#ifdef TRACE_VERBOSE
    CTRACE(FRED("clearing ev_stat %s/%d"),
           ev->ev.stat.path, ev->ev.stat.wd);
#endif
    ev_stat_stop(the_loop, &ev->ev.stat);
}


ssize_t
mrkthr_get_rbuflen(int fd)
{
    ssize_t sz;
    int res;
    ev_item_t *ev, *ev1;

    ev = ev_io_item_get(fd, EV_READ);
    //ev_io_set(&ev->ev.io, fd, EV_READ);
    me->pdata._ev = ev;

    /*
     * check if there is another thread waiting for the same event.
     */
    if (ev->ev.io.data == NULL) {
        ev->ev.io.data = me;
    } else if (ev->ev.io.data != me) {
        /*
         * in this case we are not allowed to wait for this event,
         * sorry.
         */
        me->co.rc = CO_RC_SIMULTANEOUS;
        return -1;
    }

#ifdef TRACE_VERBOSE
    CTRACE(FBBLUE("starting ev_io %d/%s"), ev->ev.io.fd, EV_STR(ev->ev.io.events));
#endif
    ev_io_start(the_loop, &ev->ev.io);

    /* wait for an event */
    me->co.state = CO_STATE_READ;
    res = yield();

    ev1 = ev_io_item_get(fd, EV_READ);

    assert(ev == ev1);

    assert(ev->ev.io.data == me);

    ev->ev.io.data = NULL;
    me->pdata._ev = NULL;

    if (res != 0) {
        return -1;
    }

    sz = 0;
    if ((res = ioctl(fd, FIONREAD, &sz)) != 0) {
        perror("ioctl");
        return -1;
    }

    return sz;
}


ssize_t
mrkthr_get_wbuflen(int fd)
{
    ssize_t sz;
    int res;
    ev_item_t *ev;

    ev = ev_io_item_get(fd, EV_WRITE);
    //ev_io_set(&ev->ev.io, fd, EV_WRITE);
    me->pdata._ev = ev;

    /*
     * check if there is another thread waiting for the same event.
     */
    if (ev->ev.io.data == NULL) {
        ev->ev.io.data = me;
    } else if (ev->ev.io.data != me) {
        /*
         * in this case we are not allowed to wait for this event,
         * sorry.
         */
        me->co.rc = CO_RC_SIMULTANEOUS;
        return -1;
    }

#ifdef TRACE_VERBOSE
    CTRACE(FBBLUE("starting ev_io %d/%s"), ev->ev.io.fd, EV_STR(ev->ev.io.events));
#endif
    ev_io_start(the_loop, &ev->ev.io);

    /* wait for an event */
    me->co.state = CO_STATE_WRITE;
    res = yield();

    ev = ev_io_item_get(fd, EV_WRITE);

    assert(ev->ev.io.data == me);

    ev->ev.io.data = NULL;
    me->pdata._ev = NULL;

    if (res != 0) {
#ifdef TRACE_VERBOSE
        CTRACE("yield() res=%d", res);
#endif
        return -1;
    }

    sz = 0;
#ifdef FIONSPACE
    if ((res = ioctl(fd, FIONSPACE, &sz)) != 0) {
        return 1024*1024;
    }
#else
    sz = 1024*1024;
#endif

    return sz;
}


/**
 * Event loop
 */
static void
ev_io_cb(UNUSED EV_P_ ev_io *w, UNUSED int revents)
{
    mrkthr_ctx_t *ctx;
    ev_item_t *ev;

    ctx = w->data;

    if (ctx == NULL) {
        TRACE("no thread for FD %d filter %s "
              "using default [discard]...", w->fd,
              EV_STR(w->events));
        ev_io_stop(the_loop, w);

    } else {
        ev = ctx->pdata._ev;

        assert(ev != NULL);
        assert(&ev->ev.io == w);

        ctx->pdata._ev = NULL;

        if (ctx->co.f == NULL) {
            TRACE("co for FD %d is NULL, discarding ...", w->fd);
            clear_event_io(ev);

        } else {
            if (w->events == EV_READ) {
                if (ctx->co.state != CO_STATE_READ) {
                    TRACE(FRED("Delivering a read event "
                               "that was not scheduled for!"));
                }
            } else if (w->events == EV_WRITE) {
                if (ctx->co.state != CO_STATE_WRITE) {
                    TRACE(FRED("Delivering a read event "
                               "that was not scheduled for!"));
                }
            } else {
                TRACE("filter %s is not supporting", EV_STR(w->events));
            }

            clear_event_io(ev);

            if (poller_resume(ctx) != 0) {
#ifdef TRACE_VERBOSE
                TRACE("Could not resume co %d "
                      "for FD %d, discarding ...",
                      ctx->co.id, w->fd);
#endif
            }
        }
    }
}


static void
ev_stat_cb(UNUSED EV_P_ ev_stat *w, UNUSED int revents)
{
    mrkthr_ctx_t *ctx;
    ev_item_t *ev;

    ctx = w->data;

    if (ctx == NULL) {
        TRACE("no thread for stat path %s using default [discard]...", w->path);
        ev_stat_stop(the_loop, w);

    } else {
        ev = ctx->pdata._ev;

        assert(ev != NULL);
        assert(&ev->ev.stat == w);

        ctx->pdata._ev = NULL;

        if (ctx->co.f == NULL) {
            TRACE("co for stat path %s is NULL, discarding ...", w->path);
            clear_event_stat(ev);

        } else {
            /*
             * XXX
             */

            clear_event_stat(ev);

            if (poller_resume(ctx) != 0) {
#ifdef TRACE_VERBOSE
                TRACE("Could not resume co %d "
                      "for stat path %s, discarding ...",
                      ctx->co.id, w->path);
#endif
            }
        }
    }
}


int
mrkthr_loop(void)
{
    int res;

    PROFILE_START(mrkthr_sched0_p);
    res = ev_run(the_loop, 0);
    PROFILE_STOP(mrkthr_sched0_p);
    return res;
}


static void
_idle_cb(UNUSED EV_P_ UNUSED ev_idle *w, UNUSED int revents)
{
    CTRACE("revents=%08x", revents);
}


static void
_timer_cb(UNUSED EV_P_ UNUSED ev_timer *w, UNUSED int revents)
{
    //CTRACE("revents=%08x", revents);
}


static void
_prepare_cb(UNUSED EV_P_ UNUSED ev_prepare *w, UNUSED int revents)
{
    btrie_node_t *node;
    mrkthr_ctx_t *ctx = NULL;

    if (!(mflags & CO_FLAG_SHUTDOWN)) {
        timecounter_now = (uint64_t)(ev_now(the_loop) * 1000000000.);

#ifdef TRACE_VERBOSE
        CTRACE(FRED("Sifting sleepq ..."));
#endif
        /* this will make sure there are no expired ctxes in the sleepq */
        poller_sift_sleepq();

        /* get the first to wake sleeping mrkthr */
        if ((node = BTRIE_MIN(&the_sleepq)) != NULL) {
            ev_tstamp secs;

            ctx = node->value;
            assert(ctx != NULL);

            if (ctx->expire_ticks > timecounter_now) {
                secs = (ev_tstamp)(ctx->expire_ticks - timecounter_now) /
                    1000000000.;
            } else {
                /*
                 * some time has elapsed after the call to
                 * sift_sleepq() that made an event expire.
                 */
                secs =   0.00000095367431640625;
            }

#ifdef TRACE_VERBOSE
            CTRACE("wait %f", secs);
#endif
            etimer.repeat = secs;
            ev_timer_again(the_loop, &etimer);
        } else {
#ifdef TRACE_VERBOSE
            CTRACE("no wait");
#endif
            //etimer.repeat = 1.00000095367431640625;
            //etimer.repeat = INFINITY;
            etimer.repeat = 59.0; /* <MAX_BLOCKTIME */
            ev_timer_again(the_loop, &etimer);
            //ev_timer_stop(the_loop, &etimer);
            //ev_unref(the_loop);
        }
    } else {
        CTRACE("breaking the loop");
        ev_break(the_loop, EVBREAK_ALL);
    }
    //CTRACE("revents=%08x", revents);
}


static void
_check_cb(UNUSED EV_P_ UNUSED ev_check *w, UNUSED int revents)
{
    int npending;

    npending = ev_pending_count(the_loop);
#ifdef TRACE_VERBOSE
    CTRACE("ev_pending_count=%d", npending);
#endif
    if (npending <= 0) {
        //CTRACE("Breaking loop? ...");
        //ev_break(the_loop, EVBREAK_ALL);
    }
    timecounter_now = (uint64_t)(ev_now(the_loop) * 1000000000.);

    //CTRACE("revents=%08x", revents);
}


static void
_syserr_cb(const char *msg)
{
    FAIL(msg);
}


void
poller_mrkthr_ctx_init(struct _mrkthr_ctx *ctx)
{
    ctx->pdata._ev = NULL;
}


void
poller_init(void)
{
    the_loop = ev_loop_new(EVFLAG_NOSIGMASK);
    //CTRACE("v %d.%d", ev_version_major(), ev_version_minor());
    //CTRACE("ev_supported_backends=%08x", ev_supported_backends());
    //CTRACE("ev_recommended_backends=%08x", ev_recommended_backends());
    //CTRACE("ev_embeddable_backends=%08x", ev_embeddable_backends());
    //CTRACE("ev_backend=%08x", ev_backend(the_loop));
#if EV_MULTIPLICITY
    //CTRACE("ev_now=%lf", ev_now(the_loop));
#else
    //CTRACE("ev_now=%lf", ev_now());
#endif
    ev_now_update(the_loop);
    timecounter_now = (uint64_t)(ev_now(the_loop) * 1000000000.);

    ev_idle_init(&eidle, _idle_cb);
    ev_timer_init(&etimer, _timer_cb, 0.0, 0.0);
    ev_timer_again(the_loop, &etimer);
    ev_prepare_init(&eprepare, _prepare_cb);
    ev_prepare_start(the_loop, &eprepare);
    ev_check_init(&echeck, _check_cb);
    ev_check_start(the_loop, &echeck);
    ev_set_syserr_cb(_syserr_cb);

    hash_init(&events,
              65521,
              (hash_hashfn_t)ev_item_hash,
              (hash_item_comparator_t)ev_item_cmp,
              (hash_item_finalizer_t)ev_item_fini);
}


void
poller_fini(void)
{
    hash_fini(&events);
    ev_loop_destroy(the_loop);
}
