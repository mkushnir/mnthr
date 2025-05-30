#include <assert.h>
#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/sysctl.h>

#define NO_PROFILE
#include <mncommon/profile.h>

#ifdef DO_MEMDEBUG
#include <mncommon/memdebug.h>
MEMDEBUG_DECLARE(mnthr_kevent_poller);
#endif

#include <mncommon/array.h>
/* Experimental trie use */
#include <mncommon/btrie.h>

#include "mnthr_private.h"

//#define TRACE_VERBOSE
#include "diag.h"
#include <mncommon/dumpm.h>

#include <kevent_util.h>

extern const profile_t *mnthr_user_p;
extern const profile_t *mnthr_swap_p;
extern const profile_t *mnthr_sched0_p;
extern const profile_t *mnthr_sched1_p;

/**
 *
 * The kevent (direct) backend.
 *
 */

static int q0 = -1;
/*
 * kevent bookkeeping
 */
static mnarray_t kevents0;
static mnarray_t kevents1;
/*
 * event_count holds the number of what we call "real" events.  Any
 * EV_ADD kevent counts towards real events.  While any EV_DELETE kevent
 * decrements it.
 */
static ssize_t event_count = 0;
/*
 * The maximum of event_count's.  This is the size of the kevent() output
 * array.
 */
static ssize_t event_max = 0;

static uint64_t nsec_zero, nsec_now;
UNUSED static uint64_t timecounter_zero, timecounter_now;
#ifdef USE_TSC
static uint64_t timecounter_freq;
#else
#   define timecounter_now nsec_now
#   define timecounter_freq (1000000000)
#endif

#include "config.h"

#ifndef HAVE_CLOCK_GETTIME
#include <sys/time.h>
#define CLOCK_REALTIME 0
#define CLOCK_REALTIME_PRECISE 0
static int
clock_gettime(UNUSED int id, struct timespec *ts)
{
    int res;
    struct timeval tv;

    if ((res = gettimeofday(&tv, NULL)) != 0) {
        return res;
    }
    ts->tv_sec = tv.tv_sec;
    ts->tv_nsec = tv.tv_usec * 1000;
    return 0;
}
#endif

#ifndef CLOCK_REALTIME_PRECISE
/* darwin targets */
#define CLOCK_REALTIME_PRECISE CLOCK_REALTIME
#endif

/**
 * Time bookkeeping
 */
#ifdef USE_TSC
static inline uint64_t
rdtsc(void)
{
#if defined(__amd64__)
  uint64_t res;
  __asm __volatile ("rdtsc; shl $32,%%rdx; or %%rdx,%%rax"
                    : "=a"(res)
                    :
                    :
                   );
  return res;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        FAIL("clock_gettime");
    }
    return ts.tv_nsec + ts.tv_sec * 1000000000;
#endif
}
#endif

static void
update_now(void)
{
#ifdef USE_TSC
    timecounter_now = rdtsc();
    /* do it here so that get_now() returns precomputed value */
    nsec_now = nsec_zero +
        (uint64_t)(((long double)
                    (timecounter_now -
                     timecounter_zero)) /
                   ((long double)(timecounter_freq)) *
                   1000000000.);
#else
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME_PRECISE, &ts) != 0) {
        FAIL("clock_gettime");
    }
    nsec_now = ts.tv_nsec + ts.tv_sec * 1000000000;
#endif
}

/**
 * Set the initial point of reference of the Epoch clock and synchronize
 * it with the internal initial TSC value. Can also be used to
 * periodically correct the internal wallclock along with that
 * system's.
 */
static void
wallclock_init(void)
{
    struct timespec ts;

#ifdef USE_TSC
    timecounter_zero = rdtsc();
#endif

    if (clock_gettime(CLOCK_REALTIME_PRECISE, &ts) != 0) {
        TR(WALLCLOCK_INIT + 1);
    }
    nsec_zero = ts.tv_nsec + ts.tv_sec * 1000000000;
}


uint64_t
poller_usec2ticks_absolute(uint64_t usec)
{
    return
#ifdef USE_TSC
        timecounter_now +
            (uint64_t)(((long double)usec / 1000000.) * timecounter_freq);
#else
        timecounter_now + usec * 1000;
#endif
}


uint64_t
poller_msec2ticks_absolute(uint64_t msec)
{
    return
#ifdef USE_TSC
        timecounter_now +
            (uint64_t)(((long double)msec / 1000.) * timecounter_freq);
#else
        timecounter_now + msec * 1000000;
#endif
}


uint64_t
poller_ticks_absolute(uint64_t ticks)
{
    return timecounter_now + ticks;
}


uint64_t
mnthr_msec2ticks(uint64_t msec)
{
#ifdef USE_TSC
    return ((long double)msec / 1000. * (long double)timecounter_freq);
#else
    return msec * 1000000;
#endif
}


long double
mnthr_ticks2sec(uint64_t ticks)
{
    return (long double)ticks / (long double)timecounter_freq;
}


long double
mnthr_ticksdiff2sec(int64_t ticks)
{
    return (long double)ticks / (long double)timecounter_freq;
}


uint64_t
mnthr_get_now_nsec(void)
{
    return nsec_now;
}


uint64_t
mnthr_get_now_nsec_precise(void)
{
    update_now();
    return nsec_now;
}


uint64_t
mnthr_get_now_ticks(void)
{
    return timecounter_now;
}


uint64_t
mnthr_get_now_ticks_precise(void)
{
    update_now();
    return timecounter_now;
}


/**
 * Async events
 *
 */
static struct kevent *
result_event(int idx)
{
    struct kevent *kev;
    if ((kev = array_get(&kevents1, idx)) == NULL) {
        return NULL;
    }
    return kev;
}


/**
 * Allocates a new, or returns the existing event in the kevents0 array. Makes
 * sure events in the kevents0 array are unique. Upon return, *idx contains
 * the index in the kevents0 array to be used in a subsequent fast lookup.
 */
static struct kevent *
new_event(int fd, int filter, int flags, int fflags, intptr_t data, void *udata)
{
    struct kevent *kev;

    if ((kev = array_incr(&kevents0)) == NULL) {
        FAIL("array_incr");
    }
    EV_SET(kev, fd, filter, flags, fflags, data, udata);
    return kev;
}

/**
 * Schedule an event to be discarded from the kqueue.
 */
static int
discard_event(int fd, int filter, mnthr_ctx_t *ctx)
{
    UNUSED struct kevent *kev;
    kev = new_event(fd, filter, EV_DELETE, 0, 0, ctx);
    --event_count;
    return 0;
}


/**
 * Remove an event from the kevents array.
 */
void
poller_clear_event(mnthr_ctx_t *ctx)
{
    if (ctx->pdata.kev.ident != -1) {
        if (ctx->co.state & (CO_STATE_READ | CO_STATE_WRITE)) {
            discard_event(ctx->pdata.kev.ident, ctx->pdata.kev.filter, ctx);
        } else if (ctx->co.state == CO_STATE_OTHER_POLLER) {
            /*
             * special case for mnthr_wait_for_event()
             */
            discard_event(ctx->pdata.kev.ident, EVFILT_READ, ctx);
            discard_event(ctx->pdata.kev.ident, EVFILT_WRITE, ctx);
        }
    }
}


mnthr_stat_t *
mnthr_stat_new(const char *path)
{
    mnthr_stat_t *res;
    if ((res = malloc(sizeof(mnthr_stat_t))) == NULL) {
        FAIL("malloc");
    }
    res->path = strdup(path);
    if ((res->fd = open(path, O_RDONLY)) < 0) {
        goto err;
    }

end:
    return res;

err:
    free(res);
    res = NULL;
    goto end;
}


void
mnthr_stat_destroy(mnthr_stat_t **st)
{
    if (*st != NULL) {
        if ((*st)->path != NULL) {
            free((*st)->path);
            (*st)->path = NULL;
        }
        if ((*st)->fd != -1) {
            close((*st)->fd);
            (*st)->fd = -1;
        }
        free(*st);
    }
}


int
mnthr_stat_wait(mnthr_stat_t *st)
{
    int res;
    struct kevent *kev;

    kev = new_event(st->fd,
                    EVFILT_VNODE,
                    EV_ADD|EV_ENABLE,
                    NOTE_DELETE |
                        NOTE_RENAME |
                        NOTE_WRITE |
                        NOTE_EXTEND |
                        NOTE_ATTRIB |
                        NOTE_REVOKE,
                    0,
                    me);
    ++event_count;

    me->pdata.kev.ident = st->fd;
    me->pdata.kev.filter = EVFILT_VNODE;

    /* wait for an event */
    me->co.state = CO_STATE_READ;
    res = yield();
    if (res != 0) {
        return -1;
    }

    if (me->pdata.kev.ident == -1) {
        /*
         * we haven't got to kevent() call
         */
        me->co.rc = MNTHR_CO_RC_USER_INTERRUPTED;
        res = -1;

    } else {
        if ((kev = result_event(me->pdata.kev.idx)) == NULL) {
            FAIL("result_event");
        }
        poller_mnthr_ctx_init(me);
        res = 0;
        if (kev->fflags & (NOTE_DELETE | NOTE_RENAME)) {
            res |= MNTHR_ST_DELETE;
        }
        if (kev->fflags & (NOTE_WRITE | NOTE_EXTEND)) {
            res |= MNTHR_ST_WRITE;
        }
        if (kev->fflags & (NOTE_ATTRIB | NOTE_REVOKE)) {
            res |= MNTHR_ST_ATTRIB;
        }
    }

    return res;
}


#define MNTHR_EVFILT_RW_FLAGS EV_ADD|EV_ENABLE

ssize_t
mnthr_get_rbuflen(int fd)
{
    int res;
    struct kevent *kev;

    kev = new_event(fd, EVFILT_READ, MNTHR_EVFILT_RW_FLAGS, 0, 0, me);
    ++event_count;

    me->pdata.kev.ident = fd;
    me->pdata.kev.filter = EVFILT_READ;

    /* wait for an event */
    me->co.state = CO_STATE_READ;
    res = yield();
    if (res != 0) {
        return -1;
    }

    if (me->pdata.kev.ident == -1) {
        /*
         * we haven't got to kevent() call
         */
        me->co.rc = MNTHR_CO_RC_USER_INTERRUPTED;
        res = -1;

    } else {
        if ((kev = result_event(me->pdata.kev.idx)) == NULL) {
            FAIL("result_event");
        }
        poller_mnthr_ctx_init(me);
        res = (ssize_t)kev->data;
    }

    return res;
}


int
mnthr_wait_for_read(int fd)
{
    int res;
    struct kevent *kev;

    kev = new_event(fd, EVFILT_READ, MNTHR_EVFILT_RW_FLAGS, 0, 0, me);
    ++event_count;

    me->pdata.kev.ident = fd;
    me->pdata.kev.filter = EVFILT_READ;

    /* wait for an event */
    me->co.state = CO_STATE_READ;
    res = yield();
    if (res != 0) {
        return res;
    }

    if (me->pdata.kev.ident == -1) {
        /*
         * we haven't got to kevent() call
         */
        me->co.rc = MNTHR_CO_RC_USER_INTERRUPTED;
        res = -1;

    } else {
        if ((kev = result_event(me->pdata.kev.idx)) == NULL) {
            FAIL("result_event");
        }
        poller_mnthr_ctx_init(me);
        res = 0;
    }

    return res;
}


ssize_t
mnthr_get_wbuflen(int fd)
{
    int res;
    struct kevent *kev;

    kev = new_event(fd, EVFILT_WRITE, MNTHR_EVFILT_RW_FLAGS, 0, 0, me);
    ++event_count;

    me->pdata.kev.ident = fd;
    me->pdata.kev.filter = EVFILT_WRITE;

    /* wait for an event */
    me->co.state = CO_STATE_WRITE;
    res = yield();

    if (res != 0) {
        return -1;
    }

    if (me->pdata.kev.ident == -1) {
        /*
         * we haven't got to kevent() call
         */
        me->co.rc = MNTHR_CO_RC_USER_INTERRUPTED;
        res = -1;

    } else {
        if ((kev = result_event(me->pdata.kev.idx)) == NULL) {
            FAIL("result_event");
        }
        poller_mnthr_ctx_init(me);
        res = (ssize_t)(kev->data ? kev->data : MNTHR_DEFAULT_WBUFLEN);
    }

    return res;
}


int
mnthr_wait_for_write(int fd)
{
    int res;
    struct kevent *kev;

    kev = new_event(fd, EVFILT_WRITE, MNTHR_EVFILT_RW_FLAGS, 0, 0, me);
    ++event_count;

    me->pdata.kev.ident = fd;
    me->pdata.kev.filter = EVFILT_WRITE;

    /* wait for an event */
    me->co.state = CO_STATE_WRITE;
    res = yield();

    if (res != 0) {
        return -1;
    }

    if (me->pdata.kev.ident == -1) {
        /*
         * we haven't got to kevent() call
         */
        me->co.rc = MNTHR_CO_RC_USER_INTERRUPTED;
        res = -1;

    } else {
        if ((kev = result_event(me->pdata.kev.idx)) == NULL) {
            FAIL("result_event");
        }
        poller_mnthr_ctx_init(me);
        res = 0;
    }

    return res;
}


int
mnthr_wait_for_events(int fd, int *events)
{
    int res;
    UNUSED struct kevent *rkev, *wkev;

    rkev = new_event(fd, EVFILT_READ, MNTHR_EVFILT_RW_FLAGS, 0, 0, me);
    ++event_count;
    wkev = new_event(fd, EVFILT_WRITE, MNTHR_EVFILT_RW_FLAGS, 0, 0, me);
    ++event_count;

    me->pdata.kev.ident = fd;
    me->pdata.kev.filter = 0; /* special case */

    /* wait for an event */
    me->co.state = CO_STATE_OTHER_POLLER;
    res = yield();

    if (res != 0) {
        return -1;
    }

    if (me->pdata.kev.ident == -1) {
        /*
         * we haven't got to kevent() call
         */
        me->co.rc = MNTHR_CO_RC_USER_INTERRUPTED;
        res = -1;

    } else {
        *events = me->pdata.kev.filter; /* special case */
        poller_mnthr_ctx_init(me);
        res = 0;
    }

    return res;
}


/**
 * Combined threads and events loop.
 *
 * The loop processes first threads, then events. It sleeps until the
 * earliest thread resume time, or an I/O event occurs.
 *
 */
int
mnthr_loop(void)
{
    int kevres = 0;
    struct kevent *kev = NULL;
    struct timespec timeout, *tmout;
    mnarray_iter_t it;

    PROFILE_START(mnthr_sched0_p);

    while (!(mnthr_flags & CO_FLAG_SHUTDOWN)) {
        mnbtrie_node_t *trn;
        mnthr_ctx_t *ctx = NULL;
        //sleep(1);
        update_now();

#ifdef TRACE_VERBOSE
        CTRACE(FRED("Sifting sleepq ..."));
#endif
        /* this will make sure there are no expired ctxes in the sleepq */
        poller_sift_sleepq();

        /* get the first to wake up */
        if ((trn = BTRIE_MIN(&the_sleepq)) != NULL) {
            ctx = trn->value;
            assert(ctx != NULL);

            if (ctx->expire_ticks > timecounter_now) {
#ifdef USE_TSC
                long double secs, isecs, nsecs;

                secs = (long double)(ctx->expire_ticks - timecounter_now) /
                    (long double)timecounter_freq;
                nsecs = modfl(secs, &isecs);
                //CTRACE("secs=%Lf isecs=%Lf nsecs=%Lf", secs, isecs, nsecs);
                timeout.tv_sec = isecs;
                timeout.tv_nsec = nsecs * 1000000000;
#else
                int64_t diff;

                diff = ctx->expire_ticks - timecounter_now;
                timeout.tv_sec = diff / 1000000000;
                timeout.tv_nsec = diff % 1000000000;
#endif
            } else {
                /*
                 * some time has elapsed after the call to
                 * sift_sleepq() that made an event expire.
                 */
                timeout.tv_sec = 0;
                //timeout.tv_nsec = 100000000; /* 100 msec */
                timeout.tv_nsec = 0;
            }
            tmout = &timeout;
        } else {
            tmout = NULL;
        }

#ifdef TRACE_VERBOSE
        CTRACE(FRED("nsec_now=%ld tmout=%ld(%ld.%ld) loop..."),
              (long)nsec_now,
              (long)(tmout != NULL ?
                  tmout->tv_nsec + tmout->tv_sec * 1000000000 : -1),
              (long)(tmout != NULL ? tmout->tv_sec : -1),
              (long)(tmout != NULL ? tmout->tv_nsec : -1));
        array_traverse(&kevents0, kevent_dump, NULL);
#endif

        if (ARRAY_ELNUM(&kevents0) != 0 || event_count != 0) {
            event_max = MAX(event_max, event_count);
#ifdef TRACE_VERBOSE
            struct kevent *tmp;
#endif
            /*
             * kevents1 is a grow-only array.  Here, don't call item
             * initializers when growing ("dirty" grow).
             *
             * Upon return from kevent(), kevres holds the number of
             * "valid" entries in  kevents1 which is always less than or
             * equal to ARRAY_ELNUM(&kevents1).
             */
            if (array_ensure_len_dirty(&kevents1, event_max, 0) != 0) {
                FAIL("array_ensure_len");
            }

            kevres = kevent(q0,
                            ARRAY_DATA(&kevents0), ARRAY_ELNUM(&kevents0),
                            ARRAY_DATA(&kevents1), ARRAY_ELNUM(&kevents1),
                            tmout);

#ifdef TRACE_VERBOSE
            CTRACE(FRED("...kevres=%d kevents0.elnum=%ld event_count=%ld"), kevres, ARRAY_ELNUM(&kevents0), event_count);
            for (tmp = array_first(&kevents1, &it);
                 tmp != NULL && (int)it.iter < kevres;
                 tmp = array_next(&kevents1, &it)) {
                (void)kevent_dump(tmp);
            }
#endif

            (void)array_clear(&kevents0);
            update_now();

            if (kevres == -1) {
                if (errno == EINTR) {
#ifdef TRACE_VERBOSE
                    CTRACE("kevent was interrupted, redoing");
#endif
                    errno = 0;
                    continue;
                } else if (event_count == 0) {
#ifdef TRACE_VERBOSE
                    CTRACE("kevent0 was not quite meaningful");
#endif
                    errno = 0;
                    continue;
                }
                perror("kevent");
                break;
            }

            if (kevres == 0 && event_count == 0) {
#ifdef TRACE_VERBOSE
                CTRACE("Nothing to process ...");
#endif
                if (tmout != NULL) {
#ifdef TRACE_VERBOSE
                    CTRACE("Timed out.");
#endif
                    continue;
                } else {
#ifdef TRACE_VERBOSE
                    CTRACE("No events, exiting.");
#endif
                    break;
                }
            }

            for (kev = array_first(&kevents1, &it);
                 kev != NULL && (int)it.iter < kevres;
                 kev = array_next(&kevents1, &it)) {
                int pres;

                assert(kev != NULL);
                if (kev->ident != (uintptr_t)(-1)) {
                    int corc;

                    ctx = kev->udata;
                    /*
                     * we first clear the event, and then the handlers/co's
                     * might re-add if needed.
                     */
#ifdef TRACE_VERBOSE
                    //CTRACE("Processing:");
                    //mnthr_dump(ctx);
#endif
                    if (kev->flags & EV_ERROR) {
                        /*
                         * do not tell kqueue to discard event, let the thread get away
                         * with it
                         */
                        corc = MNTHR_CO_RC_POLLER;
                    } else {
                        discard_event(kev->ident, kev->filter, ctx);
                        corc = 0;
                    }
                    if (ctx != NULL) {
                        if (ctx->co.state == CO_STATE_OTHER_POLLER) {
                            /*
                             * special case for mnthr_wait_for_event(),
                             * defer resume
                             */
                            ctx->pdata.kev.idx = -1;
                            if (kev->filter == EVFILT_READ) {
                                ctx->pdata.kev.filter |=
                                    MNTHR_WAIT_EVENT_READ;
                            } else if (kev->filter == EVFILT_WRITE) {
                                ctx->pdata.kev.filter |=
                                    MNTHR_WAIT_EVENT_WRITE;
                            } else {
                                /**/
                                FAIL("mnthr_loop");
                            }
                            set_resume_fast(ctx);

                        } else {
                            ctx->pdata.kev.idx = it.iter;
                            if (ctx->co.f != NULL) {
                                ctx->co.rc = corc;
                                if ((pres = poller_resume(ctx)) != 0) {
#ifdef TRACE_VERBOSE
                                    CTRACE("Could not resume co %ld "
                                          "for read FD %08lx (res=%d)",
                                          (long)ctx->co.id, kev->ident, pres);
#endif
                                }
                            } else {
                                //CTRACE("co for FD %08lx is NULL, "
                                //      "discarding ...", kev->ident);
                            }
                        }
                    } else {
                        CTRACE("no thread for FD %08lx filter %s "
                              "using default [discard]...", kev->ident,
                              kevent_filter_str(kev->filter));
                    }
                } else {
                    CTRACE("kevent returned ident -1");
                    KEVENT_DUMP(kev);
                    FAIL("kevent?");
                }
            }

        } else {
            /*
             * If we had specified a timeout, but we have found ourselves
             * here, there must be sleep/resume threads waiting for us.
             */
            if (tmout != NULL) {
                if (tmout->tv_sec != 0 || tmout->tv_nsec != 0) {
#ifdef TRACE_VERBOSE
                    CTRACE("Nothing to pass to kevent(), nanosleep ? ...");
#endif
                    kevres = nanosleep(tmout, NULL);

                    if (kevres == -1) {
                        if (errno == EINTR) {
                            CTRACE("nanosleep was interrupted, redoing");
                            errno = 0;
                            continue;
                        }
                        perror("nanosleep");
                        break;
                    }
                } else {
#ifdef TRACE_VERBOSE
                    CTRACE("tmout was zero, no nanosleep.");
#endif
                }
            } else {
#ifdef TRACE_VERBOSE
                CTRACE("Nothing to pass to kevent(), breaking the loop ? ...");
#endif
                kevres = 0;
                break;
            }
        }
    }

    PROFILE_STOP(mnthr_sched0_p);
    CTRACE("exiting mnthr_loop ...");
    return kevres;
}


void
poller_mnthr_ctx_init(struct _mnthr_ctx *ctx)
{
    ctx->pdata.kev.ident = -1;
    ctx->pdata.kev.filter = 0;
    ctx->pdata.kev.idx = -1;
}


void
poller_init(void)
{
#ifdef USE_TSC
    size_t sz;
    sz = sizeof(timecounter_freq);
    if (sysctlbyname("machdep.tsc_freq", &timecounter_freq, &sz, NULL, 0) != 0) {
        FAIL("sysctlbyname");
    }
#endif
    wallclock_init();

    if ((q0 = kqueue()) == -1) {
        FAIL("kqueue");
    }

    if (array_init(&kevents0, sizeof(struct kevent), 0,
                   kevent_init,
                   NULL) != 0) {
        FAIL("array_init");
    }

    if (array_init(&kevents1, sizeof(struct kevent), 0,
                   kevent_init,
                   NULL) != 0) {
        FAIL("array_init");
    }
}

void
poller_fini(void)
{
    array_fini(&kevents0);
    array_fini(&kevents1);
    close(q0);
    q0 = -1;
}
