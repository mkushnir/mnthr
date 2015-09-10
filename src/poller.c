#include <assert.h>
#include <errno.h>

#include <mrkcommon/dtqueue.h>
#include <mrkcommon/stqueue.h>
/* Experimental trie use */
#include <mrkcommon/trie.h>

#include "mrkthr_private.h"

//#define TRACE_VERBOSE
#include "diag.h"
#include <mrkcommon/dumpm.h>

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
        mrkthr_ctx_finalize(ctx);
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

    res = swapcontext(&main_uc, &me->co.uc);

#ifdef TRACE_VERBOSE
    CTRACE("back from resume <<<");
    //mrkthr_dump(me);
#endif

    if (errno == EINTR) {
        perror("poller_resume(), ignoring ...");
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
        return CO_RC_EXITED;

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
    trie_node_t *trn;
    mrkthr_ctx_t *ctx;
    uint64_t now;

    /* run expired threads */

    STQUEUE_INIT(&runq);

    now = mrkthr_get_now_ticks();

    for (trn = TRIE_MIN(&the_sleepq);
         trn != NULL;
         trn = TRIE_MIN(&the_sleepq)) {

        ctx = (mrkthr_ctx_t *)(trn->value);
        assert(ctx != NULL);

        if (ctx->expire_ticks < now) {
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
            CTRACE(FBGREEN("Resuming expired thread (from bucket) >>>"));
            mrkthr_dump(bctx);
            CTRACE(FBGREEN("<<<"));
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

            if (poller_resume(bctx) != 0) {
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
            TRACE("bctx=%p", bctx);
            TRACE("ctx=%p", ctx);
            TRACE(FRED("Have to deliver a %s event "
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
            TRACE("Could not resume co %d, discarding ...",
                  ctx->co.id);
#endif
        }
    }
}

