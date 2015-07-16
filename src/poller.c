#include <assert.h>
#include <errno.h>

#include <mrkcommon/array.h>
#include <mrkcommon/list.h>
#include <mrkcommon/dtqueue.h>
#include <mrkcommon/stqueue.h>
/* Experimental trie use */
#include <mrkcommon/trie.h>

#include "mrkthr_private.h"
#include <kevent_util.h>

#include "diag.h"
#include <mrkcommon/dumpm.h>
//#define TRACE_VERBOSE

static int q0 = -1;
static array_t kevents0;
static array_t kevents1;

/**
 *
 * The kevent backend.
 *
 */

/**
 *
 *
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
        return;
    }

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
    return;
}

static void
clear_event_by_idx(int idx)
{
    struct kevent *kev;

    kev = get_event(idx);
    assert(kev != NULL);
    clear_event(kev->ident, kev->filter, idx);
}


void
poller_clear_event(struct _mrkthr_ctx *ctx)
{
    if (ctx->_idx0 != -1) {
        clear_event_by_idx(ctx->_idx0);
    }
}


ssize_t
mrkthr_get_rbuflen(int fd)
{
    int res;
    struct kevent *kev;

    while (1) {
        kev = new_event(fd, EVFILT_READ, &me->_idx0);

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

        /*
         * XXX  Think of s/ONESHOT/CLEAR/. Now it looks like we cannot put
         * EV_CLEAR here, otherwise we are at risk of firing a read event
         * while we are being scheduled for a different event, for example
         * sleep.
         */
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
        kev = new_event(fd, EVFILT_WRITE, &me->_idx0);

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
            return (ssize_t)(kev->data);
        }
    }
    return -1;
}


static int
resume(mrkthr_ctx_t *ctx)
{
    int res;

    //CTRACE("resuming ...");
    //mrkthr_dump(ctx);

    /*
     * Can only be the result of yield or start, ie, the state cannot be
     * dormant or resumed.
     */
    if (!(ctx->co.state & CO_STATE_RESUMABLE)) {
        /* This is an error (currently no reason is known, though) */
        sleepq_remove(ctx);
        mrkthr_ctx_finalize(ctx);
        /* not sure if we can push it here ... */
        push_free_ctx(ctx);
        TRRET(RESUME + 1);
    }

    ctx->co.state = CO_STATE_RESUMED;

    me = ctx;

    res = swapcontext(&main_uc, &me->co.uc);

    if (errno == EINTR) {
        perror("resume(), ignoring ...");
        errno = 0;
        return 0;
    }
    /* no one in the thread context may touch me */
    assert(me == ctx);
    me = NULL;

    if (ctx->co.state & CO_STATE_RESUMABLE) {
        return res;

    } else if (ctx->co.state == CO_STATE_RESUMED) {
        /*
         * This is the case of the exited (dead) thread.
         */
        //CTRACE("Assuming dead ...");
        //mrkthr_dump(ctx);
        sleepq_remove(ctx);
        mrkthr_ctx_finalize(ctx);
        push_free_ctx(ctx);
        //TRRET(RESUME + 2);
        return RESUME + 2;

    } else {
        CTRACE("Unknown case:");
        mrkthr_dump(ctx);
        FAIL("resume");
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
static void
sift_sleepq(void)
{
    STQUEUE(_mrkthr_ctx, runq);
    trie_node_t *trn;
    mrkthr_ctx_t *ctx;

    /* run expired threads */

    STQUEUE_INIT(&runq);

    update_now();

#ifdef TRACE_VERBOSE
    CTRACE(FBBLUE("Processing: delta=%ld (%Lf)"),
           (int64_t)(ctx->expire_ticks) - (int64_t)timecounter_now,
           mrkthr_ticksdiff2sec(
           (int64_t)(ctx->expire_ticks) - (int64_t)timecounter_now));
    mrkthr_dump(ctx);
#endif

    for (trn = TRIE_MIN(&the_sleepq);
         trn != NULL;
         trn = TRIE_MIN(&the_sleepq)) {

        ctx = (mrkthr_ctx_t *)(trn->value);
        assert(ctx != NULL);

        if (ctx->expire_ticks < timecounter_now) {
            STQUEUE_ENQUEUE(&runq, runq_link, ctx);
            trie_remove_node(&the_sleepq, trn);
            trn = NULL;
        } else {
            break;
        }
    }

    while ((ctx = STQUEUE_HEAD(&runq)) != NULL) {
        mrkthr_ctx_t *bctx;

        STQUEUE_DEQUEUE(&runq, runq_link);
        STQUEUE_ENTRY_FINI(runq_link, ctx);

        while ((bctx = DTQUEUE_HEAD(&ctx->sleepq_bucket)) != NULL) {

#ifdef TRACE_VERBOSE
            CTRACE(FBGREEN("Resuming expired thread (from bucket)"));
            mrkthr_dump(bctx);
#endif
            DTQUEUE_DEQUEUE(&ctx->sleepq_bucket, sleepq_link);
            DTQUEUE_ENTRY_FINI(sleepq_link, bctx);

            if (!(bctx->co.state & CO_STATES_RESUMABLE_EXTERNALLY)) {
                /*
                 * We cannot resume events here that can only be
                 * resumed from within other places of mrkthr_loop().
                 *
                 * All other events not included here are
                 * CO_STATE_READ and CO_STATE_WRITE. This
                 * should never occur.
                 */
#ifdef TRACE_VERBOSE
                TRACE(FRED("Have to deliver a %s event "
                           "to co.id=%d that was not scheduled for!"),
                           CO_STATE_STR(bctx->co.state),
                           bctx->co.id);
                mrkthr_dump(bctx);
#endif
            }

            if (resume(bctx) != 0) {
#ifdef TRACE_VERBOSE
                TRACE("Could not resume co %d, discarding ...",
                      bctx->co.id);
#endif
            }
        }

        if (!(ctx->co.state & CO_STATES_RESUMABLE_EXTERNALLY)) {
            /*
             * We cannot resume events here that can only be
             * resumed from within other places of mrkthr_loop().
             *
             * All other events not included here are
             * CO_STATE_READ and CO_STATE_WRITE. This
             * should never occur.
             */
#ifdef TRACE_VERBOSE
            TRACE(FRED("Have to deliver a %s event "
                       "to co.id=%d that was not scheduled for!"),
                       CO_STATE_STR(bctx->co.state),
                       ctx->co.id);
            mrkthr_dump(ctx);
#endif
        }

#ifdef TRACE_VERBOSE
        CTRACE(FBGREEN("Resuming expired bucket owner"));
        mrkthr_dump(ctx);
#endif
        if (resume(ctx) != 0) {
#ifdef TRACE_VERBOSE
            TRACE("Could not resume co %d, discarding ...",
                  ctx->co.id);
#endif
        }
    }
}

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

    update_now();

    while (!(mflags & CO_FLAG_SHUTDOWN)) {
        //sleep(1);

#ifdef TRACE_VERBOSE
        TRACE(FRED("Sifting sleepq ..."));
#endif

        /* this will make sure there are no expired ctxes in the sleepq */
        sift_sleepq();

        /* get the first to wake sleeping mrkthr */
        if ((node = TRIE_MIN(&the_sleepq)) != NULL) {
            ctx = node->value;
            assert(ctx != NULL);
            //assert(ctx != NULL);

            if (ctx->expire_ticks > timecounter_now) {
#ifdef USE_TSC
                long double secs, isecs, nsecs;

                secs = (long double)(ctx->expire_ticks - timecounter_now) / timecounter_freq;
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
        array_traverse(&kevents0, (array_traverser_t)mrkthr_dump, NULL);
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
        //TRACE("saved %d items of %ld total off from kevents0",
        //      nempty,
        //      kevents0.elnum);

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
            array_traverse(&kevents1, (array_traverser_t)dump_ctx_traverser, NULL);
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

                kev = array_get(&kevents1, i);

                assert(kev != NULL);

                if (kev->ident != (uintptr_t)(-1)) {

                    ctx = kev->udata;


                    /*
                     * we first clear the event, and then the handlers/co's
                     * might re-add if needed.
                     */
                    clear_event(kev->ident, kev->filter,
                                        ctx != NULL ? ctx->_idx0 : -1);

                    if (kev->flags & EV_ERROR) {
#ifdef TRACE_VERBOSE
                        //TRACE("Error condition for FD %08lx, (%s) skipping ...",
                        //      kev->ident, strerror(kev->data));
#endif
                        continue;
                    }

#ifdef TRACE_VERBOSE
                    //CTRACE("Processing:");
                    //mrkthr_dump(ctx);
#endif


                    if (ctx != NULL) {

                        /* only clear_event() makes use of it */
                        ctx->_idx0 = -1;

                        switch (kev->filter) {

                        case EVFILT_READ:

                            if (ctx->co.f != NULL) {

                                if (ctx->co.state != CO_STATE_READ) {
                                    TRACE(FRED("Delivering a read event "
                                               "that was not scheduled for!"));
                                }

                                if (resume(ctx) != 0) {
                                    //TRACE("Could not resume co %d "
                                    //      "for read FD %08lx, discarding ...",
                                    //      ctx->co.id, kev->ident);
                                    discard_event(kev->ident,
                                                          kev->filter);
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

                                if (resume(ctx) != 0) {
                                    //TRACE("Could not resume co %d "
                                    //      "for write FD %08lx, discarding ...",
                                    //      ctx->co.id, kev->ident);
                                    discard_event(kev->ident,
                                                          kev->filter);
                                }

                            } else {
                                TRACE("co for FD %08lx NULL, "
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

    TRACE("exiting mrkthr_loop ...");

    return kevres;
}


void
poller_init(void)
{
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
