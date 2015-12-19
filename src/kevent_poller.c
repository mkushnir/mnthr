#include <assert.h>
#include <errno.h>
#include <math.h>

#include <sys/types.h>
#include <sys/sysctl.h>

#define NO_PROFILE
#include <mrkcommon/profile.h>

#ifdef DO_MEMDEBUG
#include <mrkcommon/memdebug.h>
MEMDEBUG_DECLARE(mrkthr_kevent_poller);
#endif

#include <mrkcommon/array.h>
/* Experimental trie use */
#include <mrkcommon/trie.h>

#include "mrkthr_private.h"

#include "diag.h"
#include <mrkcommon/dumpm.h>
//#define TRACE_VERBOSE

#include <kevent_util.h>

extern const profile_t *mrkthr_user_p;
extern const profile_t *mrkthr_swap_p;
extern const profile_t *mrkthr_sched0_p;
extern const profile_t *mrkthr_sched1_p;

static int q0 = -1;
static array_t kevents0;
static array_t kevents1;

static uint64_t nsec_zero, nsec_now;
UNUSED static uint64_t timecounter_zero, timecounter_now;
#ifdef USE_TSC
static uint64_t timecounter_freq;
#else
#   define timecounter_now nsec_now
#   define timecounter_freq (1000000000)
#endif

/**
 *
 * The kevent (direct) backend.
 *
 */

/**
 * Time bookkeeping
 */
#ifdef USE_TSC
static inline uint64_t
rdtsc(void)
{
  uint64_t res;

#if defined(__amd64__) || defined(__i386__)
  __asm __volatile ("rdtsc; shl $32,%%rdx; or %%rdx,%%rax"
                    : "=a"(res)
                    :
                    : "%rcx", "%rdx"
                   );
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        FAIL("clock_gettime");
    }
    res = ts.tv_nsec + ts.tv_sec * 1000000000;
#endif
  return res;
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
mrkthr_msec2ticks(uint64_t msec)
{
#ifdef USE_TSC
    return ((long double)msec / 1000. * (long double)timecounter_freq);
#else
    return msec * 1000000;
#endif
}


long double
mrkthr_ticks2sec(uint64_t ticks)
{
    return (long double)ticks / (long double)timecounter_freq;
}


long double
mrkthr_ticksdiff2sec(int64_t ticks)
{
    return (long double)ticks / (long double)timecounter_freq;
}


uint64_t
mrkthr_get_now(void)
{
    return nsec_now;
}


uint64_t
mrkthr_get_now_precise(void)
{
    update_now();
    return nsec_now;
}


uint64_t
mrkthr_get_now_ticks(void)
{
    return timecounter_now;
}


uint64_t
mrkthr_get_now_ticks_precise(void)
{
    update_now();
    return timecounter_now;
}


/**
 * Async events
 *
 */
static struct kevent *
result_event(int fd, int filter)
{
    struct kevent *kev;
    array_iter_t it;

    //TRACE("getting result event for FD %d filter %s", fd,
    //      kevent_filter_str(filter));

    for (kev = array_first(&kevents1, &it);
         kev != NULL;
         kev = array_next(&kevents1, &it)) {
        if (kev->ident == (uintptr_t)fd && kev->filter == filter) {
            return kev;
        }
    }

    return NULL;
}


/**
 * Allocates a new, or returns the existing event in the kevents0 array. Makes
 * sure events in the kevents0 array are unique. Upon return, *idx contains
 * the index in the kevents0 array to be used in a subsequent fast lookup.
 */
static struct kevent *
new_event(int fd, int filter, int *idx)
{
    struct kevent *kev0, *kev1 = NULL;
    array_iter_t it;

    for (kev0 = array_first(&kevents0, &it);
         kev0 != NULL;
         kev0 = array_next(&kevents0, &it)) {
        if (kevent_isempty(kev0)) {
            if (kev1 == NULL) {
                /* make note of the first empty slot */
                kev1 = kev0;
                *idx = it.iter;
            }
            continue;
        } else if (kev0->ident == (uintptr_t)fd && kev0->filter == filter) {
            kev1 = kev0;
            *idx = it.iter;
            break;
        }
    }

    /* no slot matched, init an empty or a brand new slot */

    if (kev1 == NULL) {
        /* there we no empty slots */
        kev1 = array_incr(&kevents0);
        assert(kev1 != NULL);
        kevent_init(kev1);
        *idx = kevents0.elnum - 1;
    }

    kev1->ident = fd;
    kev1->filter = filter;

    return kev1;
}

/**
 * Schedule an event to be discarded from the kqueue.
 */
static int
discard_event(int fd, int filter)
{
    struct kevent *kev;
    int idx;

    kev = new_event(fd, filter, &idx);
    EV_SET(kev, fd, filter, EV_DELETE, 0, 0, NULL);
    return 0;
}


static struct kevent *
get_event(int idx)
{
    struct kevent *kev;
    kev = array_get(&kevents0, idx);
    return kev;
}

/**
 * Remove an event from the kevents array.
 */
static void
clear_event(int fd, int filter, int idx)
{
    struct kevent *kev;
    array_iter_t it;

    /* First try fast */
    if (idx != -1 &&
        (kev = array_get(&kevents0, idx)) != NULL &&
        kev->ident == (uintptr_t)fd &&
        kev->filter == filter) {

        //CTRACE("FAST");
        //KEVENT_DUMP(kev);
        kevent_init(kev);
    } else {
        for (kev = array_first(&kevents0, &it);
             kev != NULL;
             kev = array_next(&kevents0, &it)) {

            if (kev->ident == (uintptr_t)fd && kev->filter == filter) {
                //CTRACE("SLOW");
                //KEVENT_DUMP(kev);
                kevent_init(kev);
                /* early return, assume fd/filter is unique in the kevents0 */
                return;
            }
        }
        //CTRACE("NOTFOUND");
        //KEVENT_DUMP(kev);
    }
}

void
poller_clear_event(mrkthr_ctx_t *ctx)
{
    if (ctx->pdata._kevent != -1) {
        struct kevent *kev;

        kev = get_event(ctx->pdata._kevent);
        assert(kev != NULL);
        clear_event(kev->ident, kev->filter, ctx->pdata._kevent);
    }
}


ssize_t
mrkthr_get_rbuflen(int fd)
{
    int res;
    struct kevent *kev;

    while (1) {
        kev = new_event(fd, EVFILT_READ, &me->pdata._kevent);

        /*
         * check if there is another thread waiting for the same event.
         */
        if (kev->udata != NULL) {
            /*
             * in this case we are not allowed to wait for this event,
             * sorry.
             */
            me->co.rc = CO_RC_SIMULTANEOUS;
            return -1;
        }

        EV_SET(kev, fd, EVFILT_READ, EV_ADD|EV_ONESHOT, 0, 0, me);

        /* wait for an event */
        me->co.state = CO_STATE_READ;
        res = yield();
        if (res != 0) {
            return -1;
        }

        if ((kev = result_event(fd, EVFILT_READ)) != NULL) {
            return (ssize_t)(kev->data);
        }
    }
    return -1;
}


ssize_t
mrkthr_get_wbuflen(int fd)
{
    int res;
    struct kevent *kev;

    while (1) {
        kev = new_event(fd, EVFILT_WRITE, &me->pdata._kevent);

        /*
         * check if there is another thread waiting for the same event.
         */
        if (kev->udata != NULL) {
            /*
             * in this case we are not allowed to wait for this event,
             * sorry.
             */
            me->co.rc = CO_RC_SIMULTANEOUS;
            return -1;
        }

        EV_SET(kev, fd, EVFILT_WRITE, EV_ADD|EV_ONESHOT, 0, 0, me);

        /* wait for an event */
        me->co.state = CO_STATE_WRITE;
        res = yield();

        if (res != 0) {
            return -1;
        }

        if ((kev = result_event(fd, EVFILT_WRITE)) != NULL) {
            return (ssize_t)(kev->data ? kev->data : 1024*1024);
        }
    }
    return -1;
}


/**
 * Event loop
 */
int
mrkthr_loop(void)
{
    int kevres = 0;
    int i;
    struct kevent *kev = NULL;
    struct timespec timeout, *tmout;
    int nempty, nkev;
    trie_node_t *node;
    mrkthr_ctx_t *ctx = NULL;
    array_iter_t it;

    PROFILE_START(mrkthr_sched0_p);

    while (!(mflags & CO_FLAG_SHUTDOWN)) {
        //sleep(1);
        update_now();

#ifdef TRACE_VERBOSE
        TRACE(FRED("Sifting sleepq ..."));
#endif
        /* this will make sure there are no expired ctxes in the sleepq */
        poller_sift_sleepq();

        /* get the first to wake sleeping mrkthr */
        if ((node = TRIE_MIN(&the_sleepq)) != NULL) {
            ctx = node->value;
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
        TRACE(FRED("nsec_now=%ld tmout=%ld(%ld.%ld) loop..."),
              nsec_now,
              tmout != NULL ?
                  tmout->tv_nsec + tmout->tv_sec * 1000000000 : -1,
              tmout != NULL ? tmout->tv_sec : -1,
              tmout != NULL ? tmout->tv_nsec : -1);
        array_traverse(&kevents0, (array_traverser_t)kevent_dump, NULL);
#endif

        /* how many discarded items are to the end of the kevnts0? */
        nempty = 0;
        for (kev = array_last(&kevents0, &it);
             kev != NULL;
             kev = array_prev(&kevents0, &it)) {
            if (kev->ident == (uintptr_t)(-1)) {
                ++nempty;
            } else {
                break;
            }
        }
#ifdef TRACE_VERBOSE
        TRACE("saved %d items of %ld total off from kevents0",
              nempty,
              kevents0.elnum);
#endif

        /* there are *some* events */
        nkev = kevents0.elnum - nempty;
        if (nkev != 0) {
            if (array_ensure_len_dirty(&kevents1, nkev, 0) != 0) {
                FAIL("array_ensure_len");
            }

            kevres = kevent(q0,
                         kevents0.data, nkev,
                         kevents1.data, kevents1.elnum,
                         tmout);

            update_now();

            if (kevres == -1) {
                perror("kevent");
                if (errno == EINTR) {
                    TRACE("kevent was interrupted, redoing");
                    errno = 0;
                    continue;
                }
                perror("kevent");
                break;
            }

#ifdef TRACE_VERBOSE
            TRACE("kevent returned %d", kevres);
            array_traverse(&kevents1,
                           (array_traverser_t)kevent_dump,
                           NULL);
#endif

            if (kevres == 0) {
#ifdef TRACE_VERBOSE
                TRACE("Nothing to process ...");
#endif
                if (tmout != NULL) {
#ifdef TRACE_VERBOSE
                    TRACE("Timed out.");
#endif
                    continue;
                } else {
#ifdef TRACE_VERBOSE
                    TRACE("No events, exiting.");
#endif
                    break;
                }
            }

            for (i = 0; i < kevres; ++i) {
                int pres;

                kev = array_get(&kevents1, i);

                assert(kev != NULL);

                if (kev->ident != (uintptr_t)(-1)) {

                    ctx = kev->udata;


                    /*
                     * we first clear the event, and then the handlers/co's
                     * might re-add if needed.
                     */
                    clear_event(kev->ident, kev->filter,
                                        ctx != NULL ? ctx->pdata._kevent : -1);

                    if (kev->flags & EV_ERROR) {
#ifdef TRACE_VERBOSE
                        TRACE("Error condition for FD %08lx, "
                              "(%s) skipping ...",
                              kev->ident, strerror(kev->data));
#endif
                        continue;
                    }

#ifdef TRACE_VERBOSE
                    //CTRACE("Processing:");
                    //mrkthr_dump(ctx);
#endif


                    if (ctx != NULL) {

                        /* only clear_event() makes use of it */
                        ctx->pdata._kevent = -1;

                        switch (kev->filter) {

                        case EVFILT_READ:

                            if (ctx->co.f != NULL) {

                                if (ctx->co.state != CO_STATE_READ) {
                                    TRACE(FRED("Delivering a read event "
                                               "that was not scheduled for!"));
                                }

                                if ((pres = poller_resume(ctx)) != 0) {
#ifdef TRACE_VERBOSE
                                    TRACE("Could not resume co %d "
                                          "for read FD %08lx, discarding ...",
                                          ctx->co.id, kev->ident);
#endif
                                    if ((pres & RESUME) == RESUME) {
                                        discard_event(kev->ident, kev->filter);
                                    }
                                }

                            } else {
                                TRACE("co for FD %08lx NULL, "
                                      "discarding ...", kev->ident);
                            }
                            break;

                        case EVFILT_WRITE:

                            if (ctx->co.f != NULL) {

                                if (ctx->co.state != CO_STATE_WRITE) {
                                    TRACE(FRED("Delivering a write event "
                                               "that was not scheduled for!"));
                                }

                                if ((pres = poller_resume(ctx)) != 0) {
#ifdef TRACE_VERBOSE
                                    TRACE("Could not resume co %d "
                                          "for write FD %08lx, discarding ...",
                                          ctx->co.id, kev->ident);
#endif
                                    if ((pres & RESUME) == RESUME) {
                                        discard_event(kev->ident, kev->filter);
                                    }
                                }

                            } else {
                                TRACE("co for FD %08lx is NULL, "
                                      "discarding ...", kev->ident);
                            }
                            break;

                        default:
                            TRACE("Filter %s is not supported, discarding",
                                  kevent_filter_str(kev->filter));
                            discard_event(kev->ident, kev->filter);
                        }

                    } else {

                        TRACE("no thread for FD %08lx filter %s "
                              "using default [discard]...", kev->ident,
                              kevent_filter_str(kev->filter));
                        discard_event(kev->ident, kev->filter);
                    }

                } else {
                    TRACE("kevent returned ident -1");
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
                    //TRACE("Nothing to pass to kevent(), nanosleep ? ...");
                    kevres = nanosleep(tmout, NULL);

                    if (kevres == -1) {
                        perror("nanosleep");
                        if (errno == EINTR) {
                            TRACE("nanosleep was interrupted, redoing");
                            errno = 0;
                            continue;
                        }
                        perror("kevent");
                        break;
                    }
                } else {
#ifdef TRACE_VERBOSE
                    TRACE("tmout was zero, no nanosleep.");
#endif
                }

            } else {
#ifdef TRACE_VERBOSE
                TRACE("Nothing to pass to kevent(), breaking the loop ? ...");
#endif
                kevres = 0;
                break;
            }
        }
    }

    PROFILE_STOP(mrkthr_sched0_p);

    TRACE("exiting mrkthr_loop ...");

    return kevres;
}


void
poller_mrkthr_ctx_init(struct _mrkthr_ctx *ctx)
{
    ctx->pdata._kevent = -1;
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
                   kevent_fini) != 0) {
        FAIL("array_init");
    }

    if (array_init(&kevents1, sizeof(struct kevent), 0,
                   kevent_init,
                   kevent_fini) != 0) {
        FAIL("array_init");
    }
}

void
poller_fini(void)
{
    array_fini(&kevents0);
    array_fini(&kevents1);
    close(q0);
}
