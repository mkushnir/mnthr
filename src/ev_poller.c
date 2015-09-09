#include <assert.h>
#include <errno.h>
#include <math.h> /* INFINITY */
#include <sys/ioctl.h>

#include <mrkcommon/dict.h>
#include <mrkcommon/fasthash.h>
/* Experimental trie use */
#include <mrkcommon/trie.h>
#include <mrkcommon/util.h>

#include "mrkthr_private.h"

//#define TRACE_VERBOSE
#include "diag.h"
#include <mrkcommon/dumpm.h>

#include <ev.h>

#define EV_STR(e)              \
    ((e) & EV_READ ? "READ" :  \
     (e) & EV_WRITE ? "WRITE" :\
     "<unknown>")              \


#pragma GCC diagnostic ignored "-Wstrict-aliasing"

typedef struct _ev_item {
    union {
        ev_io io;
    } ev;
    /*
     * ev.io->fd, ev.io->events
     */
    uint64_t hash;
} ev_item_t;

dict_t events;

static struct ev_loop *the_loop;

static void ev_io_cb(EV_P_ ev_io *, int);

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
            unsigned char *c;
        } u;
        // typeof(ev->ev.io.events)
        int events;

        u.i = &ev->ev.io.fd;
        ev->hash = fasthash(0, u.c, sizeof(ev->ev.io.fd));
        events = ev->ev.io.events & ~EV__IOFDSET;
        u.i = &events;
        ev->hash = fasthash(ev->hash, u.c, sizeof(events));
    }
    return ev->hash;
}


static int
ev_item_cmp(ev_item_t *a, ev_item_t *b)
{
    if (a->ev.io.fd == b->ev.io.fd) {
        if (a->ev.io.events == b->ev.io.events) {
            return 0;
        }
        return a->ev.io.events > b->ev.io.events ? 1 : -1;
    }
    return a->ev.io.fd > b->ev.io.fd ? 1 : -1;
}


static ev_item_t *
ev_item_new(int fd, int event)
{
    ev_item_t *res;

    if ((res = malloc(sizeof(ev_item_t))) == NULL) {
        FAIL("malloc");
    }
    ev_io_init(&res->ev.io, ev_io_cb, fd, event);
    res->ev.io.data = NULL;
    res->hash = 0;

    return res;
}


static void
ev_item_destroy(ev_item_t **ev)
{
    if (*ev != NULL) {
#ifdef TRACE_VERBOSE
        CTRACE(FRED("destroying ev_io %d/%s"),
               (*ev)->ev.io.fd,
               EV_STR((*ev)->ev.io.events));
#endif
        ev_io_stop(the_loop, &(*ev)->ev.io);
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
ev_item_get(int fd, int event)
{
    ev_item_t probe, *ev;
    dict_item_t *dit;

    probe.ev.io.fd = fd;
    probe.ev.io.events = event;
    probe.hash = 0;

    if ((dit = dict_get_item(&events, &probe)) == NULL) {
        ev = ev_item_new(fd, event);
        dict_set_item(&events, ev, NULL);
    } else {
        ev = dit->key;
    }
    return ev;
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
#ifdef TRACE_VERBOSE
        CTRACE(FRED("clearing ev_io %d/%s"),
               ev->ev.io.fd,
               EV_STR(ev->ev.io.events));
#endif
        ev_io_stop(the_loop, &ev->ev.io);
    }
}


static void
clear_event(ev_item_t *ev)
{
#ifdef TRACE_VERBOSE
    CTRACE(FRED("clearing ev_io %d/%s"),
           ev->ev.io.fd,
           EV_STR(ev->ev.io.events));
#endif
    ev_io_stop(the_loop, &ev->ev.io);
}

ssize_t
mrkthr_get_rbuflen(int fd)
{
    ssize_t sz;
    int res;
    UNUSED ev_item_t *ev, *ev1;

    ev = ev_item_get(fd, EV_READ);
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

    ev1 = ev_item_get(fd, EV_READ);

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

    ev = ev_item_get(fd, EV_WRITE);
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

    ev = ev_item_get(fd, EV_WRITE);

    assert(ev->ev.io.data == me);

    ev->ev.io.data = NULL;
    me->pdata._ev = NULL;

    if (res != 0) {
        return -1;
    }

    sz = 0;
#ifdef FIONSPACE
    if ((res = ioctl(fd, FIONSPACE, &sz)) != 0) {
        return -1;
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
            clear_event(ev);

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

            clear_event(ev);

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


int
mrkthr_loop(void)
{
    return ev_run(the_loop, 0);
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
    trie_node_t *node;
    mrkthr_ctx_t *ctx = NULL;

    if (!(mflags & CO_FLAG_SHUTDOWN)) {
        timecounter_now = (uint64_t)(ev_now(the_loop) * 1000000000.);

#ifdef TRACE_VERBOSE
        CTRACE(FRED("Sifting sleepq ..."));
#endif
        /* this will make sure there are no expired ctxes in the sleepq */
        poller_sift_sleepq();

        /* get the first to wake sleeping mrkthr */
        if ((node = TRIE_MIN(&the_sleepq)) != NULL) {
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
        CTRACE("Breaking loop? ...");
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

    dict_init(&events,
              65521,
              (dict_hashfn_t)ev_item_hash,
              (dict_item_comparator_t)ev_item_cmp,
              (dict_item_finalizer_t)ev_item_fini);
}


void
poller_fini(void)
{
    dict_fini(&events);
    ev_loop_destroy(the_loop);
}
