#include <assert.h>
#include <errno.h>

#define NO_PROFILE
#include <mrkcommon/profile.h>

#ifdef DO_MEMDEBUG
#include <mrkcommon/memdebug.h>
MEMDEBUG_DECLARE(mrkthr_poller);
#endif

#include <mrkcommon/dtqueue.h>
#include <mrkcommon/stqueue.h>
/* Experimental trie use */
#include <mrkcommon/btrie.h>

#include "mrkthr_private.h"

//#define TRACE_VERBOSE
//#define TRRET_DEBUG
#include "diag.h"
#include <mrkcommon/dumpm.h>

#ifdef __clang__
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

extern const profile_t *mrkthr_user_p;
extern const profile_t *mrkthr_swap_p;
extern const profile_t *mrkthr_sched0_p;
extern const profile_t *mrkthr_sched1_p;


int
poller_resume(mrkthr_ctx_t *ctx)
{
    int res;

    /*
     * Can only be the result of yield or start, ie, the state cannot be
     * dormant or resumed.
     */
    if (!(ctx->co.state & CO_STATE_RESUMABLE)) {
        /* This is an error (currently no reason is known, though) */
        sleepq_remove(ctx);
        /* not sure if we can push it here ... */
        push_free_ctx(ctx);
        TRRET(RESUME + 1);
    }

    ctx->co.state = CO_STATE_RESUMED;

    me = ctx;

#ifdef TRACE_VERBOSE
    CTRACE("resuming >>>");
    //mrkthr_dump(ctx);
#endif

    PROFILE_STOP(mrkthr_sched0_p);
    PROFILE_START(mrkthr_swap_p);
    res = swapcontext(&main_uc, &me->co.uc);
    PROFILE_STOP(mrkthr_swap_p);
    PROFILE_START(mrkthr_sched0_p);

#ifdef TRACE_VERBOSE
    CTRACE("back from resume <<<");
    //mrkthr_dump(me);
#endif

    if (errno == EINTR) {
        CTRACE("ignoring EINTR");
#ifdef TRACE_VERBOSE
        //mrkthr_dump(ctx);
#endif
        errno = 0;
        return 0;
    }
    /* no one in the thread context may touch me */
    assert(me == ctx);
    me = NULL;

    if (ctx->co.state & CO_STATE_RESUMABLE) {
        return ctx->co.rc;

    } else if (ctx->co.state == CO_STATE_RESUMED) {
        /*
         * This is the case of the exited (dead) thread.
         */
#ifdef TRACE_VERBOSE
        CTRACE("Assuming exited (dead) ...");
        //mrkthr_dump(ctx);
#endif
        sleepq_remove(ctx);
        push_free_ctx(ctx);
        //TRRET(RESUME + 2);
        //return CO_RC_EXITED;
        return ctx->co.rc;

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
void
poller_sift_sleepq(void)
{
    STQUEUE(_mrkthr_ctx, runq);
    btrie_node_t *trn;
    mrkthr_ctx_t *ctx;
    uint64_t now;

    /* run expired threads */

    STQUEUE_INIT(&runq);

    now = mrkthr_get_now_ticks();

    for (trn = BTRIE_MIN(&the_sleepq);
         trn != NULL;
         trn = BTRIE_MIN(&the_sleepq)) {

        ctx = (mrkthr_ctx_t *)(trn->value);
        assert(ctx != NULL);

        if (ctx->expire_ticks < now) {
            //if (ctx->expire_ticks > 1) {
            //    CTRACEC(FBYELLOW("remove "));
            //    mrkthr_dump(ctx);
            //}

            STQUEUE_ENQUEUE(&runq, runq_link, ctx);
            btrie_remove_node(&the_sleepq, trn);
            trn = NULL;
#ifdef TRACE_VERBOSE
            CTRACE(FBGREEN("Put in runq:"));
            mrkthr_dump(ctx);
#endif
        } else {
            break;
        }
    }

    while ((ctx = STQUEUE_HEAD(&runq)) != NULL) {
        mrkthr_ctx_t *bctx;

        STQUEUE_DEQUEUE(&runq, runq_link);
        STQUEUE_ENTRY_FINI(runq_link, ctx);
        ctx->expire_ticks = 0;

        if (!(ctx->co.state & CO_STATES_RESUMABLE_EXTERNALLY)) {
            /*
             * We cannot resume events here that can only be
             * resumed from within other places of mrkthr_loop().
             *
             * All other events not included here are
             * CO_STATE_READ, CO_STATE_WRITE, CO_STATE_OTHER_POLLER. This
             * should never occur.
             */
#ifdef TRACE_VERBOSE
            CTRACE("bctx=%p", bctx);
            CTRACE("ctx=%p", ctx);
            CTRACE(FYELLOW("Have to deliver a %s event "
                       "to co.id=%d that was not scheduled for!"),
                       bctx != NULL ? CO_STATE_STR(bctx->co.state) : "<bctx NULL>",
                       ctx->co.id);
            mrkthr_dump(ctx);
#endif
        }

#ifdef TRACE_VERBOSE
        CTRACE(FBGREEN("Resuming expired bucket owner >>>"));
        mrkthr_dump(ctx);
        CTRACE(FBGREEN("<<<"));
#endif
        if (poller_resume(ctx) != 0) {
#ifdef TRACE_VERBOSE
            CTRACE("Could not resume co %d, discarding ...",
                  ctx->co.id);
#endif
        }

        while ((bctx = DTQUEUE_HEAD(&ctx->sleepq_bucket)) != NULL) {
#ifdef TRACE_VERBOSE
            CTRACE(FBGREEN("Resuming expired thread (from bucket) >>>"));
            mrkthr_dump(bctx);
            CTRACE(FBGREEN("<<<"));
#endif
            DTQUEUE_DEQUEUE(&ctx->sleepq_bucket, sleepq_link);
            DTQUEUE_ENTRY_FINI(sleepq_link, bctx);
            bctx->expire_ticks = 0;

            if (!(bctx->co.state & CO_STATES_RESUMABLE_EXTERNALLY)) {
                /*
                 * We cannot resume events here that can only be
                 * resumed from within other places of mrkthr_loop().
                 *
                 * All other events not included here are
                 * CO_STATE_READ, CO_STATE_WRITE, CO_STATE_OTHER_POLLER. This
                 * should never occur.
                 */
#ifdef TRACE_VERBOSE
                CTRACE(FYELLOW("Have to deliver a %s event "
                           "to co.id=%d that was not scheduled for!"),
                           CO_STATE_STR(bctx->co.state),
                           bctx->co.id);
                mrkthr_dump(bctx);
#endif
            }

            if (poller_resume(bctx) != 0) {
#ifdef TRACE_VERBOSE
                CTRACE("Could not resume co %d, discarding ...",
                      bctx->co.id);
#endif
            }
        }
    }
}

