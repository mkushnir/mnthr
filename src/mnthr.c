/**
 * Micro threads support.
 *
 * This implementation is based on the FreeBSD's kqueue(2) feature and the
 * POSIX ucontext(3) feature. The coroutines concept was used to
 * implement multi-threading.
 *
 *
 * Features.
 *
 * Stack overflow protection. Thread stacks are created via mmap(2).
 * The lowest page of the stack is then protected with the PROT_NONE flag.
 * TODO: Take care of different stack layouts.
 *
 * thread locking primitives:
 *  - mnthr_signal_t, basic unreliable signal delivery.
 *  - condition variable
 *  - semaphore
 *  - inverted semaphore
 *  - reader-writer lock
 *
 * Basic time information. Internally the rdtsc() implementation for
 * x86-64 architecture is used to synchronize the notion of the
 * current time with the system's one. In the scheduler's execution
 * context, after each blocking system call returns, the rdtsc() is called
 * to update the library's "now" variable.  Also the corresponding
 * nanoseconds since the Epoch are calculated. The mnthr_get_now()
 * returns the value of the former.
 *
 * Request for thread's interruption. The mnthr_set_interrupt() will
 * turn the thread into an interrupted state, which effectively causes all
 * "yielding" mnthr_ctx_* calls to fail on return. This way the thread
 * receives an indication for clean up and exiting.
 *
 * Timed out thread execution: mnthr_wait_for(), mnthr_peek().
 *
 * I/O poller generic support.
 *
 *
 * Implementation Overview.
 *
 * Thread's programming context consists of the ucontext_t structure, that
 * is the thread's execution context, an associated with it mmap(2) based
 * stack, thread's entry point, entry point's arguments, and internal
 * state information.
 *
 * Requests for read or write, or sleep requests that usually come from
 * threads' execution contexts implicitly yield thread's execution to the
 * scheduler's context (the "main" context).
 *
 * Scheduling of a thread back for execution is determined by the
 * readiness of the thread's "event of interest". Such events of interest
 * can be either an I/O event (as a result of a read or write request), or
 * a timer event (the result of a sleep request).
 */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <netdb.h>
#include <inttypes.h>

#define NO_PROFILE
#include <mncommon/profile.h>

#ifdef DO_MEMDEBUG
#include <mncommon/memdebug.h>
MEMDEBUG_DECLARE(mnthr);
#endif

//#define TRACE_VERBOSE
//#define TRRET_DEBUG
#include "diag.h"
#include <mncommon/dumpm.h>

#include <mncommon/array.h>
#include <mncommon/dtqueue.h>
#include <mncommon/stqueue.h>
/* Experimental trie use */
#include <mncommon/btrie.h>

#include "mnthr_private.h"

#ifdef __clang__
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

#include "config.h"

#ifdef HAVE_SYS_SENDFILE_H
#include <sys/sendfile.h>
#endif

const profile_t *mnthr_user_p;
const profile_t *mnthr_swap_p;
const profile_t *mnthr_sched0_p;
const profile_t *mnthr_sched1_p;


typedef int (*writer_t) (int, int, int);

int mnthr_flags = 0;

static size_t stacksize = STACKSIZE;
ucontext_t main_uc;
static char main_stack[STACKSIZE];

static int co_id = 0;
static mnarray_t ctxes;
mnthr_ctx_t *me;

static DTQUEUE(_mnthr_ctx, free_list);

/*
 * Sleep list holds threads that are waiting for resume
 * in the future. It's prioritized by the thread's expire_ticks.
 */
mnbtrie_t the_sleepq;


static int mnthr_ctx_init(void *);
static int mnthr_ctx_fini(void *);
static void co_fini_ucontext(struct _co *);
static void co_fini_other(struct _co *);
static void resume_waitq_all(mnthr_waitq_t *);
static mnthr_ctx_t *mnthr_ctx_new(void);
static mnthr_ctx_t *mnthr_ctx_pop_free(void);
static void set_resume(mnthr_ctx_t *);
static void mnthr_ctx_finalize(struct _mnthr_ctx *);


void
push_free_ctx(mnthr_ctx_t *ctx)
{
    //CTRACE("push_free_ctx");
    //mnthr_dump(ctx);
    mnthr_ctx_finalize(ctx);
    DTQUEUE_ENQUEUE(&free_list, free_link, ctx);
}


UNUSED static void
dump_ucontext (UNUSED ucontext_t *uc)
{
#ifdef __FreeBSD__
    printf("sigmask=%08x %08x %08x %08x "
    "link=%p "
    "ss_sp=%p "
    "ss_size=%08lx "
    "ss_flags=%08x "
    "__spare__=%08x %08x %08x %08x"
#ifdef __amd64__
    "onstack=%016lx "
    "rip=%016lx "
    "flags=%016x "
    "rflags=%016lx "
    "addr=%016lx "
    "trapno=%016x "
    "cs=%016lx "
    "ss=%016lx "
    "err=%016lx "
    "rax=%016lx "
    "rbx=%016lx "
    "rcx=%016lx "
    "rdx=%016lx "
    "rdi=%016lx "
    "rsi=%016lx "
    "rbp=%016lx "
    "rsp=%016lx "
    "r8=%016lx "
    "rr=%016lx "
    "r10=%016lx "
    "r11=%016lx "
    "r12=%016lx "
    "r13=%016lx "
    "r14=%016lx "
    "r15=%016lx "
#endif

    "\n",
        uc->uc_sigmask.__bits[0],
        uc->uc_sigmask.__bits[1],
        uc->uc_sigmask.__bits[2],
        uc->uc_sigmask.__bits[3],
        uc->uc_link,
        uc->uc_stack.ss_sp,
        uc->uc_stack.ss_size,
        uc->uc_stack.ss_flags,
        uc->__spare__[0],
        uc->__spare__[1],
        uc->__spare__[2],
        uc->__spare__[3],
#ifdef __amd64__
        uc->uc_mcontext.mc_onstack,
        uc->uc_mcontext.mc_rip,
        uc->uc_mcontext.mc_flags,
        uc->uc_mcontext.mc_rflags,
        uc->uc_mcontext.mc_addr,
        uc->uc_mcontext.mc_trapno,
        uc->uc_mcontext.mc_cs,
        uc->uc_mcontext.mc_ss,
        uc->uc_mcontext.mc_err,
        uc->uc_mcontext.mc_rax,
        uc->uc_mcontext.mc_rbx,
        uc->uc_mcontext.mc_rcx,
        uc->uc_mcontext.mc_rdx,
        uc->uc_mcontext.mc_rdi,
        uc->uc_mcontext.mc_rsi,
        uc->uc_mcontext.mc_rbp,
        uc->uc_mcontext.mc_rsp,
        uc->uc_mcontext.mc_r8,
        uc->uc_mcontext.mc_r9,
        uc->uc_mcontext.mc_r10,
        uc->uc_mcontext.mc_r11,
        uc->uc_mcontext.mc_r12,
        uc->uc_mcontext.mc_r13,
        uc->uc_mcontext.mc_r14,
        uc->uc_mcontext.mc_r15
#endif
    );
#endif
}


size_t
mnthr_set_stacksize(size_t v)
{
    size_t res;

    res = stacksize;

    if (v < (PAGE_SIZE * 2)) {
        v = PAGE_SIZE * 2;
    } else if (v > (PAGE_SIZE * 2048)) {
        v = PAGE_SIZE * 2048;
    }

    if (v % PAGE_SIZE) {
        v += (PAGE_SIZE - (v % PAGE_SIZE));
    }
    stacksize = v;
    return res;
}


size_t
mnthr_ctx_sizeof(void)
{
    return sizeof(mnthr_ctx_t);
}

static int
dump_sleepq_node(mnbtrie_node_t *trn, UNUSED void *udata)
{
    mnthr_ctx_t *ctx = (mnthr_ctx_t *)trn->value;
    if (ctx != NULL) {
        TRACEC("trn=%p key=%016"PRIx64" ", trn, ctx->expire_ticks);
        mnthr_dump(ctx);
    }
    return 0;
}


void
mnthr_dump_sleepq(void)
{
    CTRACE("sleepq:");
    btrie_traverse(&the_sleepq, dump_sleepq_node, NULL);
    TRACEC("end of sleepq\n");
}


/* Sleep list */

void
sleepq_remove(mnthr_ctx_t *ctx)
{
    mnbtrie_node_t *trn;

    //CTRACE(FBLUE("SL removing"));
    //mnthr_dump(ctx);
    //CTRACE(FBLUE("SL before removing:"));
    //mnthr_dump_sleepq();
    //CTRACE(FBLUE("---"));

    if (ctx->expire_ticks == MNTHR_SLEEP_UNDEFINED) {
        return;
    }

    if ((trn = btrie_find_exact(&the_sleepq, ctx->expire_ticks)) != NULL) {
        mnthr_ctx_t *sle, *bucket_host_pretendent;

        sle = trn->value;

        assert(sle != NULL);

        //CTRACE("sle:");
        //mnthr_dump(sle);
        //CTRACE("ctx:");
        //mnthr_dump(ctx);
        /*
         * ctx is either the sle itself, or it is
         * in the sle.sleepq_bucket.
         *
         * If it is the sle, and has a non-empty
         * bucket, must transfer bucket ownership to
         * the first item in the bucket.
         */
        if ((bucket_host_pretendent =
             DTQUEUE_HEAD(&sle->sleepq_bucket)) != NULL) {

            /*
             * sle is a sleepq bucket host
             */

            if (sle == ctx) {
                /* we are going to remove a bucket host */

                //TRACEC(FYELLOW("removeH"));
                //mnthr_dump(ctx);

                DTQUEUE_DEQUEUE(&sle->sleepq_bucket, sleepq_link);
                DTQUEUE_ENTRY_FINI(sleepq_link, bucket_host_pretendent);

                bucket_host_pretendent->sleepq_bucket = sle->sleepq_bucket;

                DTQUEUE_FINI(&sle->sleepq_bucket);
                DTQUEUE_ENTRY_FINI(sleepq_link, sle);

                trn->value = bucket_host_pretendent;

            } else {
                /* we are removing from the bucket */
                if (!DTQUEUE_ORPHAN(&sle->sleepq_bucket, sleepq_link, ctx)) {
                    //CTRACE(FYELLOW("removing from bucket"));
                    //mnthr_dump(ctx);
                    //CTRACE("-----");
                    DTQUEUE_REMOVE(&sle->sleepq_bucket, sleepq_link, ctx);
                    //TRACEC(FYELLOW("removeB"));
                    //mnthr_dump(ctx);
                } else {
                    //TRACEC(FBLUE("?????? "));
                    //mnthr_dump(ctx);
                }
            }

        } else {
            //assert(sle == ctx);
            if (sle == ctx) {
                //TRACEC(FYELLOW("remove "));
                //mnthr_dump(ctx);
                trn->value = NULL;
                btrie_remove_node(&the_sleepq, trn);
            } else {
                /*
                 * Here we have found ctx is not in the bucket.
                 * Just ignore it.
                 */
                //mnthr_dump(sle);
                //CTRACE("ctx: %p/%p", DTQUEUE_PREV(sleepq_link, ctx), DTQUEUE_NEXT(sleepq_link, ctx));
                //TRACEC(FBLUE("not in sleepq 0:"));
                //mnthr_dump(ctx);
                //mnthr_dump_sleepq();

                //assert(DTQUEUE_ORPHAN(&sle->sleepq_bucket, sleepq_link, ctx));
                //assert(DTQUEUE_EMPTY(&ctx->sleepq_bucket));
            }
        }
    } else {
        //if (ctx->expire_ticks > MNTHR_SLEEP_RESUME_NOW) {
        //    TRACEC(FBLUE("not in sleepq 1:"));
        //    mnthr_dump(ctx);
        //}
    }
    //CTRACE(FBLUE("SL after removing:"));
    //mnthr_dump_sleepq();
    //CTRACE(FBLUE("---"));
}


static void
sleepq_insert(mnthr_ctx_t *ctx)
{
    mnbtrie_node_t *trn;
    mnthr_ctx_t *bucket_host;

    //CTRACE(FGREEN("SL inserting"));
    //mnthr_dump(ctx);

    if ((trn = btrie_add_node(&the_sleepq, ctx->expire_ticks)) == NULL) {
        FAIL("btrie_add_node");
    }
    //if (ctx->expire_ticks > MNTHR_SLEEP_RESUME_NOW) {
    //    TRACEC(FRED("insert "));
    //    mnthr_dump(ctx);
    //}
    bucket_host = (mnthr_ctx_t *)(trn->value);
    if (bucket_host != NULL) {
        mnthr_ctx_t *head;

        //TRACE("while inserting, found bucket:");
        //mnthr_dump(bucket_host);
        if ((head = DTQUEUE_HEAD(&bucket_host->sleepq_bucket)) == NULL) {
            DTQUEUE_ENQUEUE(&bucket_host->sleepq_bucket, sleepq_link, ctx);
        } else {

            //CTRACE("before:");
            //mnthr_dump(DTQUEUE_HEAD(&bucket_host->sleepq_bucket));
            DTQUEUE_INSERT_BEFORE(&bucket_host->sleepq_bucket,
                                  sleepq_link,
                                  head,
                                  ctx);
        }

        //TRACE("After adding to the bucket:");
        //mnthr_dump(bucket_host);
    } else {
        trn->value = ctx;
    }
    //CTRACE(FGREEN("SL after inserting:"));
    //mnthr_dump_sleepq();
    //CTRACE(FGREEN("---"));
}


static void
sleepq_insert_once(mnthr_ctx_t *ctx)
{
    mnbtrie_node_t *trn;
    if ((trn = btrie_find_exact(&the_sleepq, ctx->expire_ticks)) != NULL) {
        mnthr_ctx_t *bucket_host;

        bucket_host = trn->value;
        assert(bucket_host != NULL);

        if (bucket_host == ctx) {
            /*
             * we are done
             */
        } else {
            mnthr_ctx_t *head;

            if ((head = DTQUEUE_HEAD(&bucket_host->sleepq_bucket)) == NULL) {
                DTQUEUE_ENQUEUE(&bucket_host->sleepq_bucket, sleepq_link, ctx);
            } else {
                DTQUEUE_INSERT_BEFORE(&bucket_host->sleepq_bucket,
                                      sleepq_link,
                                      head,
                                      ctx);
            }
        }

    } else {
        sleepq_insert(ctx);
    }
}


static void
sleepq_append(mnthr_ctx_t *ctx)
{
    mnbtrie_node_t *trn;
    mnthr_ctx_t *bucket_host;

    //CTRACE(FGREEN("SL appending"));
    //mnthr_dump(ctx);

    if ((trn = btrie_add_node(&the_sleepq, ctx->expire_ticks)) == NULL) {
        FAIL("btrie_add_node");
    }
    //if (ctx->expire_ticks > MNTHR_SLEEP_RESUME_NOW) {
    //    TRACEC(FRED("append "));
    //    mnthr_dump(ctx);
    //}
    bucket_host = (mnthr_ctx_t *)(trn->value);
    if (bucket_host != NULL) {
        //TRACE("while appending, found bucket:");
        //mnthr_dump(bucket_host);
        DTQUEUE_ENQUEUE(&bucket_host->sleepq_bucket, sleepq_link, ctx);

        //TRACE("After adding to the bucket:");
        //mnthr_dump(bucket_host);
    } else {
        trn->value = ctx;
    }
    //CTRACE(FGREEN("SL after appending:"));
    //mnthr_dump_sleepq();
    //CTRACE(FGREEN("---"));
}


void
mnthr_set_prio(mnthr_ctx_t *ctx, int flag)
{
    ctx->sleepq_enqueue = flag ? sleepq_insert : sleepq_append;
}


/*
 * Module init/fini
 */
int
mnthr_init(void)
{
    UNUSED size_t sz;

    if (mnthr_flags & CO_FLAG_INITIALIZED) {
        return 0;
    }

    PROFILE_INIT_MODULE();
    mnthr_user_p = PROFILE_REGISTER("user");
    mnthr_swap_p = PROFILE_REGISTER("swap");
    mnthr_sched0_p = PROFILE_REGISTER("sched0");
    mnthr_sched1_p = PROFILE_REGISTER("sched1");

#ifdef DO_MEMDEBUG
    MEMDEBUG_REGISTER(mnthr);
#endif

    DTQUEUE_INIT(&free_list);

    if (array_init(&ctxes, sizeof(mnthr_ctx_t *), 0,
                  mnthr_ctx_init,
                  mnthr_ctx_fini) != 0) {
        FAIL("array_init");
    }

    poller_init();

    main_uc.uc_link = NULL;
    main_uc.uc_stack.ss_sp = main_stack;
    main_uc.uc_stack.ss_size = sizeof(main_stack);
    me = NULL;
    btrie_init(&the_sleepq);

    mnthr_flags |= CO_FLAG_INITIALIZED;

    return 0;
}


int
mnthr_fini(void)
{
    if (!(mnthr_flags & CO_FLAG_INITIALIZED)) {
        return 0;
    }

    me = NULL;
    array_fini(&ctxes);
    DTQUEUE_FINI(&free_list);
    btrie_fini(&the_sleepq);
    poller_fini();

    PROFILE_REPORT_SEC();
    PROFILE_FINI_MODULE();

    mnthr_flags &= ~CO_FLAG_INITIALIZED;

    return 0;
}



static int
uyuyuy(UNUSED int argc, UNUSED void **argv)
{
    return 0;
}


void
mnthr_shutdown(void)
{
    mnthr_flags |= CO_FLAG_SHUTDOWN;
    mnthr_spawn("uyuyuy", uyuyuy, 0);
}


bool
mnthr_shutting_down(void)
{
    return (bool)(mnthr_flags & CO_FLAG_SHUTDOWN);
}


size_t
mnthr_compact_sleepq(size_t threshold)
{
    size_t volume = 0;

    volume = btrie_get_volume(&the_sleepq);
    if (volume > threshold) {
        btrie_cleanup(&the_sleepq);
    }
    return volume;
}


size_t
mnthr_get_sleepq_length(void)
{
    return btrie_get_nvals(&the_sleepq);
}


size_t
mnthr_get_sleepq_volume(void)
{
    return btrie_get_volume(&the_sleepq);
}


static int
dump_ctx_traverser(void *o, UNUSED void *udata)
{
    mnthr_ctx_t **ctx = o;
    if (*ctx != NULL) {
        if ((*ctx)->co.id != -1) {
            mnthr_dump(*ctx);
        }
    }
    return 0;
}


void
mnthr_dump_all_ctxes(void)
{
    TRACEC("all ctxes:\n");
    array_traverse(&ctxes, dump_ctx_traverser, NULL);
    TRACEC("end of all ctxes\n");
}


/*
 * mnthr_ctx management
 */
static int
mnthr_ctx_init(void *o)
{
    mnthr_ctx_t **pctx = o;
    mnthr_ctx_t *ctx;

    if ((ctx = malloc(sizeof(mnthr_ctx_t))) == NULL) {
        FAIL("malloc");
    }

    /* co ucontext */
    ctx->co.stack = MAP_FAILED;
    ctx->co.uc.uc_link = NULL;
    ctx->co.uc.uc_stack.ss_sp = NULL;
    ctx->co.uc.uc_stack.ss_size = 0;
    //sigfillset(&ctx->co.uc.uc_sigmask);

    /* co other */
    ctx->co.id = -1;
    *(ctx->co.name) = '\0';
    ctx->co.f = NULL;
    ctx->co.argc = 0;
    ctx->co.argv = NULL;
    ctx->co.cld = NULL;
    ctx->co.abac = 0;
    ctx->co.state = CO_STATE_DORMANT;
    ctx->co.rc = 0;

    /* the rest of ctx */
    ctx->sleepq_enqueue = sleepq_append;

    DTQUEUE_INIT(&ctx->sleepq_bucket);
    DTQUEUE_ENTRY_INIT(sleepq_link, ctx);
    ctx->expire_ticks = MNTHR_SLEEP_UNDEFINED;

    DTQUEUE_INIT(&ctx->waitq);

    DTQUEUE_ENTRY_INIT(waitq_link, ctx);
    ctx->hosting_waitq = NULL;

    DTQUEUE_ENTRY_INIT(free_link, ctx);
    STQUEUE_ENTRY_INIT(runq_link, ctx);
    poller_mnthr_ctx_init(ctx);

    *pctx = ctx;

    return 0;
}


static void
co_fini_ucontext(struct _co *co)
{
    if (co->stack != MAP_FAILED) {
        munmap(co->stack, co->uc.uc_stack.ss_size);
        co->stack = MAP_FAILED;
    }
    co->uc.uc_link = NULL;
    co->uc.uc_stack.ss_sp = NULL;
    co->uc.uc_stack.ss_size = 0;
}


static void
co_fini_other(struct _co *co)
{
    co->id = -1;
    *co->name = '\0';
    co->f = NULL;
    co->argc = 0;
    if (co->argv != NULL) {
        free(co->argv);
        co->argv = NULL;
    }
    co->cld = NULL;
    //co->abac = 0; /* cannot zero it here */
    co->state = CO_STATE_DORMANT;
    // XXX let it stay for a while, and clear later ...
    //co->rc = 0;
    /*
     * sanity?
     */
    //if (co->stack != MAP_FAILED) {
    //    memset(co->stack + PAGE_SIZE, 0xa5, co->uc.uc_stack.ss_size - PAGE_SIZE);
    //}
}


static void
mnthr_ctx_finalize(mnthr_ctx_t *ctx)
{
    /*
     * XXX not cleaning ucontext for future use.
     */

    /* remove me from sleepq */
    //DTQUEUE_FINI(&ctx->sleepq_bucket);
    //DTQUEUE_ENTRY_FINI(sleepq_link, ctx);
    ctx->expire_ticks = MNTHR_SLEEP_UNDEFINED;

    ctx->sleepq_enqueue = sleepq_append;

    co_fini_other(&ctx->co);

    /* resume all from my waitq */
    resume_waitq_all(&ctx->waitq);
    DTQUEUE_FINI(&ctx->waitq);

    /* remove me from someone else's waitq */
    if (ctx->hosting_waitq != NULL) {
        DTQUEUE_REMOVE(ctx->hosting_waitq, waitq_link, ctx);
        ctx->hosting_waitq = NULL;
    }

    poller_mnthr_ctx_init(ctx);
}


static int
mnthr_ctx_fini(void *o)
{
    mnthr_ctx_t **pctx = o;
    if (*pctx != NULL) {
        co_fini_ucontext(&(*pctx)->co);
        mnthr_ctx_finalize(*pctx);
        (*pctx)->co.rc = 0;
        free(*pctx);
        *pctx = NULL;
    }
    return 0;
}


/* Ugly hack to work around -Wclobbered, a part of -Wextra in gcc */
#ifdef __GCC__
static int
_getcontext(ucontext_t *ucp)
{
    return getcontext(ucp);
}
#else
#define _getcontext getcontext
#endif


static mnthr_ctx_t *
mnthr_ctx_new(void)
{
    mnthr_ctx_t **ctx;
    if ((ctx = array_incr(&ctxes)) == NULL) {
        FAIL("array_incr");
    }
    return *ctx;
}


static mnthr_ctx_t *
mnthr_ctx_pop_free(void)
{
    mnthr_ctx_t *ctx;

    for (ctx = DTQUEUE_HEAD(&free_list);
         ctx != NULL;
         ctx = DTQUEUE_NEXT(free_link, ctx)) {
        if (ctx->co.abac == 0) {
            DTQUEUE_REMOVE(&free_list, free_link, ctx);
            ctx->co.rc = 0;
            goto end;
        }
    }

    ctx = mnthr_ctx_new();

end:
    return ctx;
}



size_t
mnthr_gc(void)
{
    size_t res;
    mnthr_ctx_t **pctx0, **pctx1, *tmp;
    mnarray_iter_t it0, it1;
    DTQUEUE(_mnthr_ctx, tmp_list);

    res = 0;
    DTQUEUE_INIT(&tmp_list);
    for (pctx0 = array_first(&ctxes, &it0);
         pctx0 != NULL;
         pctx0 = array_next(&ctxes, &it0)) {
        if (!DTQUEUE_ORPHAN(&free_list, free_link, *pctx0)) {
            if ((*pctx0)->co.abac > 0) {
                CTRACE("co.abac not clear during gc (keeping):");
                mnthr_dump(*pctx0);
                assert((*pctx0)->co.id == -1);
                DTQUEUE_ENTRY_INIT(free_link, *pctx0);
                DTQUEUE_ENQUEUE(&tmp_list, free_link, *pctx0);
            } else {
                ++res;
                assert((*pctx0)->co.id == -1);
                (void)array_clear_item(&ctxes, it0.iter);
            }
        }
    }

    /*
     * compact ctxes
     */
    for (pctx0 = array_first(&ctxes, &it0);
         pctx0 != NULL;
         pctx0 = array_next(&ctxes, &it0)) {
        if (*pctx0 == NULL) {
            it1 = it0;
            for (pctx1 = array_next(&ctxes, &it1);
                 pctx1 != NULL;
                 pctx1 = array_next(&ctxes, &it1)) {
                if (*pctx1 != NULL) {
                    *pctx0 = *pctx1;
                    *pctx1 = NULL;
                    break;
                }
            }
            if (*pctx0 == NULL) {
                (void)array_ensure_len_dirty(&ctxes, it0.iter, ARRAY_FLAG_SAVE);
                break;
            }
        }
    }

    DTQUEUE_INIT(&free_list);

    for (tmp = DTQUEUE_HEAD(&tmp_list);
         tmp != NULL;
         tmp = DTQUEUE_NEXT(free_link, tmp)) {
        DTQUEUE_DEQUEUE(&tmp_list, free_link);
        DTQUEUE_ENTRY_FINI(free_link, tmp);
        DTQUEUE_ENQUEUE(&free_list, free_link, tmp);
    }
    DTQUEUE_FINI(&tmp_list);

    return res;
}


/**
 * Return a new mnthr_ctx_t instance. The new instance doesn't have to
 * be freed, and should be treated as an opaque object. It's internally
 * reclaimed as soon as the worker function returns.
 */
#define VNEW_BODY(get_ctx_fn)                                                  \
    int i;                                                                     \
    assert(mnthr_flags & CO_FLAG_INITIALIZED);                                 \
    ctx = get_ctx_fn();                                                        \
    assert(ctx!= NULL);                                                        \
    if (ctx->co.id != -1) {                                                    \
        mnthr_dump(ctx);                                                       \
        CTRACE("Unclear ctx: during thread %s creation", name);                \
    }                                                                          \
    assert(ctx->co.id == -1);                                                  \
    ctx->co.id = co_id++;                                                      \
    if (name != NULL) {                                                        \
        strncpy(ctx->co.name, name, sizeof(ctx->co.name) - 1);                 \
        ctx->co.name[sizeof(ctx->co.name) - 1] = '\0';                         \
    } else {                                                                   \
        ctx->co.name[0] = '\0';                                                \
    }                                                                          \
    if (_getcontext(&ctx->co.uc) != 0) {                                       \
        TR(MNTHR_CTX_NEW + 1);                                                 \
        ctx = NULL;                                                            \
        goto vnew_body_end;                                                    \
    }                                                                          \
    if (ctx->co.stack == MAP_FAILED) {                                         \
        if ((ctx->co.stack = mmap(NULL,                                        \
                                  stacksize,                                   \
                                  PROT_READ|PROT_WRITE,                        \
                                  MAP_PRIVATE|MAP_ANON,                        \
                                  -1,                                          \
                                  0)) == MAP_FAILED) {                         \
            TR(_MNTHR_NEW + 2);                                                \
            ctx = NULL;                                                        \
            goto vnew_body_end;                                                \
        }                                                                      \
        if (mprotect(ctx->co.stack, PAGE_SIZE, PROT_NONE) != 0) {              \
            FAIL("mprotect");                                                  \
        }                                                                      \
    }                                                                          \
    ctx->co.uc.uc_stack.ss_sp = ctx->co.stack;                                 \
    ctx->co.uc.uc_stack.ss_size = stacksize;                                   \
    ctx->co.uc.uc_link = &main_uc;                                             \
    ctx->co.f = f;                                                             \
    if (argc > 0) {                                                            \
        ctx->co.argc = argc;                                                   \
        if ((ctx->co.argv = malloc(sizeof(void *) * ctx->co.argc)) == NULL) {  \
            FAIL("malloc");                                                    \
        }                                                                      \
        for (i = 0; i < ctx->co.argc; ++i) {                                   \
            ctx->co.argv[i] = va_arg(ap, void *);                              \
        }                                                                      \
    }                                                                          \
    makecontext(&ctx->co.uc, (void(*)(void))f, 2, ctx->co.argc, ctx->co.argv); \
vnew_body_end:                                                                 \



mnthr_ctx_t *
mnthr_new(const char *name, mnthr_cofunc_t f, int argc, ...)
{
    va_list ap;
    mnthr_ctx_t *ctx = NULL;

    va_start(ap, argc);
    VNEW_BODY(mnthr_ctx_pop_free);
    va_end(ap);
    if (ctx == NULL) {
        FAIL("mnthr_new");
    }
    return ctx;
}


mnthr_ctx_t *
mnthr_new_sig(const char *name, mnthr_cofunc_t f, int argc, ...)
{
    va_list ap;
    mnthr_ctx_t *ctx = NULL;

    va_start(ap, argc);
    VNEW_BODY(mnthr_ctx_new);
    va_end(ap);
    if (ctx == NULL) {
        FAIL("mnthr_new");
    }
    return ctx;
}


int
mnthr_dump(const mnthr_ctx_t *ctx)
{
    UNUSED ucontext_t uc;
    mnthr_ctx_t *tmp;
    ssize_t ssz;

#ifdef __FreeBSD__
#ifdef __amd64__
    ssz = (uintptr_t)ctx->co.stack +
          (uintptr_t)ctx->co.uc.uc_stack.ss_size -
          (uintptr_t)ctx->co.uc.uc_mcontext.mc_rsp;
#else
    ssz = -1;
#endif
#else
    ssz = -1;
#endif

    TRACEC("mnthr %p/%s id=%lld f=%p ssz=%ld st=%s rc=%s exp=%016lx\n",
           ctx,
           ctx->co.name,
           (long long)ctx->co.id,
           ctx->co.f,
           (long)ssz,
           CO_STATE_STR(ctx->co.state),
           MNTHR_CO_RC_STR(ctx->co.rc),
           (long)ctx->expire_ticks
    );

    uc = ctx->co.uc;
    //dump_ucontext(&uc);
    if (DTQUEUE_HEAD(&ctx->sleepq_bucket) != NULL) {
        TRACEC("Bucket:\n");
        for (tmp = DTQUEUE_HEAD(&ctx->sleepq_bucket);
             tmp != NULL;
             tmp = DTQUEUE_NEXT(sleepq_link, tmp)) {

            TRACEC(" +mnthr %p/%s id=%lld f=%p st=%s rc=%s exp=%016lx\n",
                   tmp,
                   tmp->co.name,
                   (long long)tmp->co.id,
                   tmp->co.f,
                   CO_STATE_STR(tmp->co.state),
                   MNTHR_CO_RC_STR(tmp->co.rc),
                   (long)tmp->expire_ticks
            );
        }
    }
    return 0;
}


PRINTFLIKE(2, 3) int
mnthr_set_name(mnthr_ctx_t *ctx,
               const char *fmt,
               ...)
{
    va_list ap;
    int res;

    va_start(ap, fmt);
    res = vsnprintf(ctx->co.name, sizeof(ctx->co.name), fmt, ap);
    va_end(ap);
    return res < (int)(sizeof(ctx->co.name)) ? 0 : 1;
}


/*
 * mnthr management
 */
mnthr_ctx_t *
mnthr_me(void)
{
    return me;
}


int
mnthr_id(void)
{
    if (me != NULL) {
        return me->co.id;
    }
    return -1;
}


int
mnthr_set_retval(int rv)
{
    int rc;
    assert(me != NULL);
    rc = me->co.rc;
    me->co.rc = rv;
    return rc;
}


int
mnthr_get_retval(void)
{
    assert(me != NULL);
    return me->co.rc;
}


void *
mnthr_set_cld(void *cld)
{
    void *res;

    assert(me != NULL);

    res = me->co.cld;
    me->co.cld = cld;
    return res;
}


void *
mnthr_get_cld(void)
{
    assert(me != NULL);
    return me->co.cld;
}


bool
mnthr_is_runnable(mnthr_ctx_t *ctx)
{
    return ctx->co.state > CO_STATE_DORMANT;
}


void
mnthr_incabac(mnthr_ctx_t *ctx)
{
    ++ctx->co.abac;
}


void
mnthr_decabac(mnthr_ctx_t *ctx)
{
    assert(ctx->co.abac > 0);
    --ctx->co.abac;
}


/*
 * Internal "yield"
 */
int
yield(void)
{
    int res;

#ifdef TRACE_VERBOSE
    CTRACE("yielding from <<<");
    //mnthr_dump(me);
#endif

    PROFILE_STOP(mnthr_user_p);
    PROFILE_START(mnthr_swap_p);
    res = swapcontext(&me->co.uc, &main_uc);
    PROFILE_STOP(mnthr_swap_p);
    PROFILE_START(mnthr_user_p);
    if(res != 0) {
        CTRACE("swapcontext() error");
        return setcontext(&main_uc);
    }

#ifdef TRACE_VERBOSE
    CTRACE("back from yield >>>");
    //mnthr_dump(me);
#endif

    return me->co.rc;
}


#define MNTHR_SET_EXPIRE_TICKS(v, fn)                  \
    if (v == MNTHR_SLEEP_FOREVER) {                    \
        me->expire_ticks = MNTHR_SLEEP_FOREVER;        \
    } else {                                           \
        if (v == 0) {                                  \
            me->expire_ticks = MNTHR_SLEEP_RESUME_NOW; \
        } else {                                       \
            me->expire_ticks = fn(v);                  \
        }                                              \
    }                                                  \


static int
sleepusec(uint64_t usec)
{
    /* first remove an old reference (if any) */
    sleepq_remove(me);

    MNTHR_SET_EXPIRE_TICKS(usec, poller_usec2ticks_absolute);

    //CTRACE("usec=%ld expire_ticks=%ld", usec, me->expire_ticks);

    me->sleepq_enqueue(me);

    return yield();
}


static int
sleepmsec(uint64_t msec)
{
    /* first remove an old reference (if any) */
    sleepq_remove(me);

    MNTHR_SET_EXPIRE_TICKS(msec, poller_msec2ticks_absolute);

    //CTRACE("msec=%ld expire_ticks=%ld", msec, me->expire_ticks);

    me->sleepq_enqueue(me);

    return yield();
}


static int
sleepticks(uint64_t ticks)
{
    /* first remove an old reference (if any) */
    sleepq_remove(me);

    MNTHR_SET_EXPIRE_TICKS(ticks, poller_ticks_absolute);

    //CTRACE("ticks=%ld expire_ticks=%ld", ticks, me->expire_ticks);

    me->sleepq_enqueue(me);

    return yield();
}


static int
sleepticks_absolute(uint64_t ticks)
{
    /* first remove an old reference (if any) */
    sleepq_remove(me);

    me->expire_ticks = ticks;

    //CTRACE("ticks=%ld expire_ticks=%ld", ticks, me->expire_ticks);

    me->sleepq_enqueue(me);

    return yield();
}


int
mnthr_sleep(uint64_t msec)
{
    assert(me != NULL);
    /* put into sleepq(SLEEP) */
    me->co.state = CO_STATE_SLEEP;
    return sleepmsec(msec);
}


int
mnthr_sleep_usec(uint64_t usec)
{
    assert(me != NULL);
    /* put into sleepq(SLEEP) */
    me->co.state = CO_STATE_SLEEP;
    return sleepusec(usec);
}


int
mnthr_sleep_ticks(uint64_t ticks)
{
    assert(me != NULL);
    /* put into sleepq(SLEEP) */
    me->co.state = CO_STATE_SLEEP;
    return sleepticks(ticks);
}


int
mnthr_yield(void)
{
    assert(me != NULL);
    /* put into sleepq(SLEEP) */
    me->co.state = CO_STATE_SLEEP;
    return sleepticks_absolute(1);
}


int
mnthr_giveup(void)
{
    assert(me != NULL);
    /* put into sleepq(SLEEP) */
    me->co.state = CO_STATE_SLEEP;
    return sleepticks_absolute(MNTHR_SLEEP_FOREVER);
}


static void
append_me_to_waitq(mnthr_waitq_t *waitq)
{
    assert(me != NULL);
    if (me->hosting_waitq != NULL) {
        DTQUEUE_REMOVE_DIRTY(me->hosting_waitq, waitq_link, me);
    }
    DTQUEUE_ENQUEUE(waitq, waitq_link, me);
    me->hosting_waitq = waitq;
}


static void
remove_me_from_waitq(mnthr_waitq_t *waitq)
{
    assert(me != NULL);
    assert(me->hosting_waitq = waitq);
    DTQUEUE_REMOVE(waitq, waitq_link, me);
    me->hosting_waitq = NULL;
}



/**
 * Sleep until the target ctx has exited.
 */
static int
join_waitq(mnthr_waitq_t *waitq)
{
    append_me_to_waitq(waitq);
    return yield();
}


int
mnthr_join(mnthr_ctx_t *ctx)
{
    if (!(ctx->co.state & CO_STATE_RESUMABLE)) {
        /* dormant thread, or an attempt to join self ? */
        return MNTHR_JOIN_FAILURE;
    }
    me->co.state = CO_STATE_JOIN;
    return join_waitq(&ctx->waitq);
}


static void
resume_waitq_all(mnthr_waitq_t *waitq)
{
    mnthr_ctx_t *t;

    while ((t = DTQUEUE_HEAD(waitq)) != NULL) {
        assert(t->hosting_waitq = waitq);
        DTQUEUE_DEQUEUE(waitq, waitq_link);
        DTQUEUE_ENTRY_FINI(waitq_link, t);
        t->hosting_waitq = NULL;
        set_resume(t);
    }
}


static void
resume_waitq_one(mnthr_waitq_t *waitq)
{
    mnthr_ctx_t *t;

    if ((t = DTQUEUE_HEAD(waitq)) != NULL) {
        assert(t->hosting_waitq = waitq);
        DTQUEUE_DEQUEUE(waitq, waitq_link);
        DTQUEUE_ENTRY_FINI(waitq_link, t);
        t->hosting_waitq = NULL;
        set_resume(t);
    }
}


void
mnthr_run(mnthr_ctx_t *ctx)
{
#ifndef NDEBUG
    if (ctx->co.state != CO_STATE_DORMANT) {
        CTRACE("precondition failed. Non-dormant ctx is %p", ctx);
        if (ctx != NULL) {
            D8(ctx, sizeof(mnthr_ctx_t));
            CTRACE("now trying to dump it ...");
            mnthr_dump(ctx);
        }
    }
#endif
    assert(ctx->co.state == CO_STATE_DORMANT);

    set_resume(ctx);
}


mnthr_ctx_t *
mnthr_spawn(const char *name, mnthr_cofunc_t f, int argc, ...)
{
    va_list ap;
    mnthr_ctx_t *ctx = NULL;

    va_start(ap, argc);
    VNEW_BODY(mnthr_ctx_pop_free);
    va_end(ap);
    if (ctx == NULL) {
        FAIL("mnthr_spawn");
    }
    mnthr_run(ctx);
    return ctx;
}


mnthr_ctx_t *
mnthr_spawn_sig(const char *name, mnthr_cofunc_t f, int argc, ...)
{
    va_list ap;
    mnthr_ctx_t *ctx = NULL;

    va_start(ap, argc);
    VNEW_BODY(mnthr_ctx_new);
    va_end(ap);
    if (ctx == NULL) {
        FAIL("mnthr_spawn");
    }
    mnthr_run(ctx);
    return ctx;
}


static void
set_resume(mnthr_ctx_t *ctx)
{
#ifndef NDEBUG
    if (ctx == me) {
        CTRACE("Attept to resume self:");
        mnthr_dump(ctx);
    }
#endif
    assert(ctx != me);

    //CTRACE("Setting for resume: ---");
    //mnthr_dump(ctx);
    //CTRACE("---");

    //assert(ctx->co.f != NULL);
    if (ctx->co.f == NULL) {
        CTRACE("Will not resume this ctx:");
        mnthr_dump(ctx);
        return;
    }

    /* first remove an old reference (if any) */
    sleepq_remove(ctx);

    ctx->co.state = CO_STATE_SET_RESUME;
    ctx->expire_ticks = MNTHR_SLEEP_RESUME_NOW;
    ctx->sleepq_enqueue(ctx);
}


void
set_resume_fast(mnthr_ctx_t *ctx)
{
#ifndef NDEBUG
    if (ctx == me) {
        CTRACE("Attept to resume self:");
        mnthr_dump(ctx);
    }
#endif
    assert(ctx != me);

    //CTRACE("Setting for resume: ---");
    //mnthr_dump(ctx);
    //CTRACE("---");

    //assert(ctx->co.f != NULL);
    if (ctx->co.f == NULL) {
        CTRACE("Will not resume this ctx:");
        mnthr_dump(ctx);
        return;
    }

    assert(ctx->expire_ticks >= MNTHR_SLEEP_RESUME_NOW);

    ctx->co.state = CO_STATE_SET_RESUME;
    ctx->expire_ticks = MNTHR_SLEEP_RESUME_NOW;
    sleepq_insert_once(ctx);
}


/**
 * Send an interrupt signal to the thread.
 */
void
mnthr_set_interrupt(mnthr_ctx_t *ctx)
{
#ifndef NDEBUG
    if (ctx == me) {
        CTRACE("precondition failed. self-interrupting ctx is %p", ctx);
        if (ctx != NULL) {
            D8(ctx, sizeof(mnthr_ctx_t));
            CTRACE("now trying to dump it ...");
            mnthr_dump(ctx);
        }
    }
#endif
    assert(ctx != me);

    //mnthr_dump(ctx);

    if (ctx->co.f == NULL) {
#ifdef TRACE_VERBOSE
        CTRACE("Will not interrupt this ctx:");
        mnthr_dump(ctx);
#endif
        return;
    }

    /* first remove an old reference (if any) */
    sleepq_remove(ctx);

    /* clear event */
    poller_clear_event(ctx);

    /*
     * We are ignoring all event management rules here.
     */
    ctx->co.rc = MNTHR_CO_RC_USER_INTERRUPTED;
    ctx->co.state = CO_STATE_SET_INTERRUPT;
    ctx->expire_ticks = MNTHR_SLEEP_RESUME_NOW;
    ctx->sleepq_enqueue(ctx);
}


int
mnthr_set_interrupt_and_join(mnthr_ctx_t *ctx)
{
    if (!(ctx->co.state & CO_STATE_RESUMABLE)) {
        /* dormant thread, or an attempt to join self ? */
        return MNTHR_JOIN_FAILURE;
    }

    mnthr_set_interrupt(ctx);

    me->co.state = CO_STATE_JOIN_INTERRUPTED;

    return join_waitq(&ctx->waitq);
}


int
mnthr_set_interrupt_and_join_with_timeout(mnthr_ctx_t *ctx, uint64_t msec)
{
    int res;
    int64_t id;

    if (!(ctx->co.state & CO_STATE_RESUMABLE)) {
        /* dormant thread, or an attempt to join self ? */
        return MNTHR_JOIN_FAILURE;
    }
    mnthr_set_interrupt(ctx);

    me->co.state = CO_STATE_JOIN_INTERRUPTED;
    MNTHR_SET_EXPIRE_TICKS(msec, poller_msec2ticks_absolute);

    append_me_to_waitq(&ctx->waitq);
    id = ctx->co.id;

    res = sleepmsec(msec);

    if (ctx->co.id != id || ctx->co.state == CO_STATE_DORMANT) {
        sleepq_remove(me);
        if (ctx->co.rc != (int)MNTHR_CO_RC_USER_INTERRUPTED) {
            res = ctx->co.rc;
        }
    } else {
        assert(ctx->co.state & CO_STATE_RESUMABLE);
        remove_me_from_waitq(&ctx->waitq);
        ctx->co.rc = MNTHR_CO_RC_TIMEDOUT;
        res = MNTHR_WAIT_TIMEOUT;
    }

    return res;
}


int
mnthr_is_dead(mnthr_ctx_t *ctx)
{
    return ctx->co.id == -1;
}


/*
 * socket/file/etc
 */

int
mnthr_socket(const char *hostname,
              const char *servname,
              int family,
              int socktype)
{
    struct addrinfo hints, *ainfos, *ai;
    int fd;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = family;
    hints.ai_socktype = socktype;
    ainfos = NULL;

    if (getaddrinfo(hostname, servname, &hints, &ainfos) != 0) {
        FAIL("getaddrinfo");
    }

    fd = -1;
    for (ai = ainfos; ai != NULL; ai = ai->ai_next) {
        if ((fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)) == -1) {
            continue;
        }

        if (fcntl(fd, F_SETFL, O_NONBLOCK) == -1) {
            perror("fcntl");
            close(fd);
            fd = -1;
        }

        break;
    }

    if (ainfos != NULL) {
        freeaddrinfo(ainfos);
    }

    return fd;
}


int
mnthr_socket_connect(const char *hostname,
                      const char *servname,
                      int family)
{
    struct addrinfo hints, *ainfos, *ai;
    int fd;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = family;
    hints.ai_socktype = SOCK_STREAM;
    ainfos = NULL;

    if (getaddrinfo(hostname, servname, &hints, &ainfos) != 0) {
        perror("getaddrinfo");
        return -1;
    }

    fd = -1;
    for (ai = ainfos; ai != NULL; ai = ai->ai_next) {
        int res;

        if ((fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)) == -1) {
            continue;
        }

        if ((res = mnthr_connect(fd, ai->ai_addr, ai->ai_addrlen)) != 0) {
            if (MNDIAG_GET_LIBRARY(res) == MNDIAG_LIBRARY_MNTHR) {
                TR(res);
            } else {
#ifdef TRACE_VERBOSE
                //perror("mnthr_connect");
                CTRACE("getsockopt in progress error: %s", strerror(res));
#endif
            }
            close(fd);
            fd = -1;
        }

        break;
    }

    if (ainfos != NULL) {
        freeaddrinfo(ainfos);
    }

    return fd;
}


int
mnthr_socket_bind(const char *hostname,
                   const char *servname,
                   int family)
{
    struct addrinfo hints, *ainfos, *ai;
    int fd;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = family;
    hints.ai_socktype = SOCK_STREAM;
    ainfos = NULL;

    if (getaddrinfo(hostname, servname, &hints, &ainfos) != 0) {
        return -1;
    }

    fd = -1;
    for (ai = ainfos; ai != NULL; ai = ai->ai_next) {
        int optval;
        if ((fd = socket(ai->ai_family,
                         ai->ai_socktype,
                         ai->ai_protocol)) == -1) {
            continue;
        }

        optval = 1;
        if (setsockopt(fd,
                       SOL_SOCKET,
                       SO_REUSEADDR,
                       &optval,
                       sizeof(optval)) != 0) {
            perror("setsockopt");
        }

        if (fcntl(fd, F_SETFL, O_NONBLOCK) == -1) {
            perror("fcntl");
            close(fd);
            fd = -1;
        }

        if (bind(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
            perror("bind");
            close(fd);
            fd = -1;
        }

        break;
    }

    if (ainfos != NULL) {
        freeaddrinfo(ainfos);
    }

    return fd;
}


int
mnthr_connect(int fd, const struct sockaddr *addr, socklen_t addrlen)
{
    int res = 0;

    if (fcntl(fd, F_SETFL, O_NONBLOCK) == -1) {
        perror("fcntl");
        TRRET(MNTHR_CONNECT + 1);
    }

    if ((res = connect(fd, addr, addrlen)) != 0) {
#ifdef TRRET_DEBUG
        perror("connect");
#endif
        if (errno == EINPROGRESS) {
            int optval;
            socklen_t optlen;

            if (mnthr_get_wbuflen(fd) < 0) {
                TRRET(MNTHR_CONNECT + 2);
            }

            optlen = sizeof(optval);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &optval, &optlen) != 0) {
                perror("getsockopt");
                TRRET(MNTHR_CONNECT + 3);
            }
            res = optval;
        }
    }

    return res;
}


int
mnthr_accept_all(int fd, mnthr_socket_t **buf, off_t *offset)
{
    ssize_t navail;
    ssize_t nread = 0;
    mnthr_socket_t *tmp;

    assert(me != NULL);

    if ((navail = mnthr_get_rbuflen(fd)) <= 0) {
        TRRET(MNTHR_ACCEPT_ALL + 1);
    }

    if ((tmp = realloc(*buf, (*offset + navail) * sizeof(mnthr_socket_t))) == NULL) {
        FAIL("realloc");
    }
    *buf = tmp;

    if (navail == 0) {
        /* EOF ? */
        TRRET(MNTHR_ACCEPT_ALL + 2);
    }

    while (nread < navail) {
        tmp = *buf + (*offset + nread);
        tmp->addrlen = sizeof(union _mnthr_addr);

        if ((tmp->fd = accept(fd, &tmp->addr.sa, &tmp->addrlen)) == -1) {
            //perror("accept");
            break;
        }
        ++nread;
    }

    if (nread < navail) {
        //TRACE("nread=%ld navail=%ld", nread, navail);
        if (nread == 0) {
            TRRET(MNTHR_ACCEPT_ALL + 4);
        }
    }

    *offset += nread;

    return 0;
}


int
mnthr_accept_all2(int fd, mnthr_socket_t **buf, off_t *offset)
{
    mnthr_socket_t *tmp;
    ssize_t navail;

    assert(me != NULL);

    if (mnthr_wait_for_read(fd) != 0) {
        TRRET(MNTHR_ACCEPT_ALL + 1);
    }

    navail = 0;
    for (navail = 0; ; ++navail) {
        if ((tmp = realloc(*buf,
                           (*offset + navail + 1) *
                                sizeof(mnthr_socket_t))) == NULL) {
            FAIL("realloc");
        }
        *buf = tmp;
        tmp = *buf + (*offset + navail);
        tmp->addrlen = sizeof(union _mnthr_addr);
        if ((tmp->fd = accept(fd, &tmp->addr.sa, &tmp->addrlen)) == -1) {
            if (errno != EAGAIN) {
                perror("accept");
            }
            break;
        }
    }

    if (navail == 0) {
        /* EOF ? */
        TRRET(MNTHR_ACCEPT_ALL + 2);
    }

    *offset += navail;

    return 0;
}


/**
 * Allocate enough space in *buf beyond *offset for a single read
 * operation and read into that location from fd.
 */
int
mnthr_read_all(int fd, char **buf, off_t *offset)
{
    ssize_t navail;
    ssize_t nread;
    char *tmp;

    assert(me != NULL);

    if ((navail = mnthr_get_rbuflen(fd)) <= 0) {
        TRRET(MNTHR_READ_ALL + 1);
    }

    if ((tmp = realloc(*buf, *offset + navail)) == NULL) {
        FAIL("realloc");
    }
    *buf = tmp;

    if (navail == 0) {
        /* EOF ? */
        TRRET(MNTHR_READ_ALL + 2);
    }

    if ((nread = read(fd, *buf + *offset, navail)) == -1) {
        perror("read");
        TRRET(MNTHR_READ_ALL + 3);
    }

    if (nread < navail) {
        //TRACE("nread=%ld navail=%ld", nread, navail);
        if (nread == 0) {
            TRRET(MNTHR_READ_ALL + 4);
        }
    }

    *offset += nread;

    return 0;
}


/**
 * Perform a single read from fd into buf.
 * Return the number of bytes read or -1 in case of error.
 */
ssize_t
mnthr_read_allb(int fd, char *buf, ssize_t sz)
{
    ssize_t navail;
    ssize_t nread;

    assert(me != NULL);

    if ((navail = mnthr_get_rbuflen(fd)) <= 0) {
        return -1;
    }

    sz = MIN(navail, sz);

    if ((nread = read(fd, buf, sz)) == -1) {
        perror("read");
        return -1;
    }

    if (nread < sz) {
        //TRACE("nread=%ld sz=%ld", nread, sz);
        if (nread == 0) {
            return -1;
        }
    }
    return nread;
}


/*
 * edge-triggered version of mnthr_read_allb()
 */
ssize_t
mnthr_read_allb_et(int fd, char *buf, ssize_t sz)
{
    ssize_t nleft, totread;

    nleft = sz;
    totread = 0;
    while (totread < sz) {
        ssize_t nread;

        if ((nread = read(fd, buf + totread, nleft)) == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                ssize_t navail;

                if ((navail = mnthr_get_rbuflen(fd)) < 0) {
                    return -1;
                }
                continue;

            } else {
                return -1;
            }
        }
        totread += nread;
        if (nread < nleft) {
            break;
        }
        nleft -= nread;
    }
    return totread;
}


ssize_t
mnthr_recv_allb(int fd, char *buf, ssize_t sz, int flags)
{
    ssize_t navail;
    ssize_t nread;

    assert(me != NULL);
    assert(sz >= 0);

    if ((navail = mnthr_get_rbuflen(fd)) <= 0) {
        return -1;
    }

    sz = MIN(navail, sz);

    if ((nread = recv(fd, buf, (size_t)sz, flags)) == -1) {
        perror("recv");
        return -1;
    }

    if (nread < sz) {
        //TRACE("nread=%ld sz=%ld", nread, sz);
        if (nread == 0) {
            return -1;
        }
    }
    return nread;
}


/**
 * Perform a single recvfrom from fd into buf.
 * Return the number of bytes received or -1 in case of error.
 */
ssize_t
mnthr_recvfrom_allb(int fd,
                     void * restrict buf,
                     ssize_t sz,
                     int flags,
                     struct sockaddr * restrict from,
                     socklen_t * restrict fromlen)
{
    ssize_t navail;
    ssize_t nrecv;

    assert(me != NULL);

    if ((navail = mnthr_get_rbuflen(fd)) <= 0) {
        return -1;
    }

    sz = MIN(navail, sz);

    if ((nrecv = recvfrom(fd, buf, (size_t)sz, flags, from, fromlen)) == -1) {
        perror("recvfrom");
        return -1;
    }

    if (nrecv < sz) {
        //TRACE("nrecv=%ld sz=%ld", nrecv, sz);
        if (nrecv == 0) {
            return -1;
        }
    }
    return nrecv;
}


/**
 * Write len bytes from buf into fd.
 */
int
mnthr_write_all(int fd, const char *buf, size_t len)
{
    ssize_t navail;
    ssize_t nwritten;
    off_t remaining = len;

    assert(me != NULL);

    while (remaining > 0) {
        if ((navail = mnthr_get_wbuflen(fd)) <= 0) {
            TRRET(MNTHR_WRITE_ALL + 1);
        }

        if ((nwritten = write(fd, buf + len - remaining,
                              MIN(navail, remaining))) == -1) {

            TRRET(MNTHR_WRITE_ALL + 2);
        }
        remaining -= nwritten;
    }
    return 0;
}


int
mnthr_write_all_et(int fd, const char *buf, size_t len)
{
    ssize_t nwritten;
    off_t remaining = len;
    ssize_t navail = len;

    assert(me != NULL);

    while (remaining > 0) {
        if ((nwritten = write(fd, buf + len - remaining,
                              MIN(navail, remaining))) == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if ((navail = mnthr_get_wbuflen(fd)) <= 0) {
                    TRRET(MNTHR_WRITE_ALL + 1);
                }
                continue;

            } else {
                TRRET(MNTHR_WRITE_ALL + 2);
            }

        }

        remaining -= nwritten;
    }
    return 0;
}


int
mnthr_send_all(int fd, const char *buf, size_t len, int flags)
{
    ssize_t navail;
    ssize_t nwritten;
    off_t remaining = len;

    assert(me != NULL);

    while (remaining > 0) {
        if ((navail = mnthr_get_wbuflen(fd)) <= 0) {
            TRRET(MNTHR_WRITE_ALL + 1);
        }

        if ((nwritten = send(fd, buf + len - remaining,
                              MIN(navail, remaining),
                              flags)) == -1) {

            TRRET(MNTHR_WRITE_ALL + 2);
        }
        remaining -= nwritten;
    }
    return 0;
}


/**
 * Write len bytes from buf into fd.
 */
int
mnthr_sendto_all(int fd,
                  const void *buf,
                  size_t len,
                  int flags,
                  const struct sockaddr *to,
                  socklen_t tolen)
{
    ssize_t navail;
    ssize_t nwritten;
    size_t remaining = len;

    assert(me != NULL);

    while (remaining > 0) {
        if ((navail = mnthr_get_wbuflen(fd)) <= 0) {
            TRRET(MNTHR_SENDTO_ALL + 1);
        }

        if ((nwritten = sendto(fd, ((const char *)buf) + len - remaining,
                               MIN((size_t)navail, remaining),
                               flags, to, tolen)) == -1) {

            TRRET(MNTHR_SENDTO_ALL + 2);
        }
        remaining -= nwritten;
    }
    return 0;
}


int
mnthr_sendfile_np(int fd,
                   int s,
                   off_t offset,
                   size_t nbytes,
                   UNUSED struct sf_hdtr *hdtr,
                   UNUSED off_t *sbytes,
                   UNUSED int flags)
{
    off_t _sbytes;

#ifdef HAVE_SF_HDTR
    //flags |= SF_NODISKIO; // sanity
#endif
    _sbytes = 0;

    while (_sbytes == 0) {
        if (mnthr_get_wbuflen(s) <= 0) {
            TRRET(MNTHR_SENDFILE + 1);
        }
#ifdef SENDFILE_DARWIN_STYLE
        _sbytes = nbytes;
#endif
        if (
#ifndef HAVE_SF_HDTR
            sendfile(fd, s, &offset, nbytes)
#else
#ifdef SENDFILE_DARWIN_STYLE
            sendfile(fd, s, offset, &_sbytes, hdtr, flags)
#else
            sendfile(fd, s, offset, nbytes, hdtr, &_sbytes, flags)
#endif
#endif
            == -1) {
            if (errno == EBUSY) {
                if (mnthr_get_rbuflen(fd) <= 0) {
                    TRRET(MNTHR_SENDFILE + 2);
                }
                continue;

            } else {
                TRRET(MNTHR_SENDFILE + 3);
            }
        }
        break;
    }

    if (sbytes != NULL) {
        *sbytes = _sbytes;
    }

    return 0;
}


int
mnthr_sendfile(int fd,
                int s,
                off_t *offset,
                size_t nbytes)
{
    ssize_t nread;

    if (mnthr_get_wbuflen(s) <= 0) {
        TRRET(MNTHR_SENDFILE + 1);
    }

    nread = 0;
    while (nbytes > 0) {
#ifndef HAVE_SF_HDTR
        if ((nread = sendfile(s, fd, offset, nbytes)) == -1) {
        }
#else
#ifdef SENDFILE_DARWIN_STYLE
        off_t len;
        len = nbytes;
        if ((nread = sendfile(s, fd, *offset, &len, NULL, 0)) == -1) {
        }
#else
        if ((nread = sendfile(s, fd, *offset, nbytes, NULL, NULL, 0)) == -1) {
        }
#endif
#endif
        if (nread  == 0) {
            break;
        }
        nbytes -= nread;
    }

    return 0;
}



/**
 * Event Primitive (Signal).
 */
void
mnthr_signal_init(mnthr_signal_t *signal, mnthr_ctx_t *ctx)
{
    signal->owner = ctx;
}


void
mnthr_signal_fini(mnthr_signal_t *signal)
{
    signal->owner = NULL;
}


int
mnthr_signal_has_owner(mnthr_signal_t *signal)
{
    return signal->owner != NULL;
}


mnthr_ctx_t *
mnthr_signal_get_owner(mnthr_signal_t *signal)
{
    return signal->owner;
}


int
mnthr_signal_subscribe(mnthr_signal_t *signal)
{
    int res;

    //CTRACE("holding on ...");
    signal->owner = me;
    me->co.state = CO_STATE_SIGNAL_SUBSCRIBE;
    res = yield();
    signal->owner = NULL;
    return res;
}


int
mnthr_signal_subscribe_with_timeout(mnthr_signal_t *signal,
                                     uint64_t msec)
{
    int res;

    //CTRACE("holding on ...");
    signal->owner = me;
    me->co.state = CO_STATE_SIGNAL_SUBSCRIBE;
    res = sleepmsec(msec);
    if (me->expire_ticks == MNTHR_SLEEP_UNDEFINED) {
        /* I had been sleeping, but was resumed by signal_send() ... */
    } else {
        res = MNTHR_WAIT_TIMEOUT;
    }
    signal->owner = NULL;
    return res;
}


void
mnthr_signal_send(mnthr_signal_t *signal)
{
    //CTRACE("signal->owner=%p", signal->owner);
    if (signal->owner != NULL) {

        if (signal->owner->co.state == CO_STATE_SIGNAL_SUBSCRIBE) {
            set_resume(signal->owner);
            return;

        } else {
            //CTRACE("Attempt to send signal for thread %d in %s "
            //       "state (ignored)",
            //       signal->owner->co.id,
            //       CO_STATE_STR(signal->owner->co.state));
        }
    }
    //CTRACE("Not resuming orphan signal. See you next time.");
}


void
mnthr_signal_error(mnthr_signal_t *signal, int rc)
{
    if (signal->owner != NULL) {
        if (signal->owner->co.state == CO_STATE_SIGNAL_SUBSCRIBE) {
            signal->owner->co.rc = rc;
            set_resume(signal->owner);
        }
    }
}


int
mnthr_signal_error_and_join(mnthr_signal_t *signal, int rc)
{
    if (signal->owner != NULL) {
        if (signal->owner->co.state == CO_STATE_SIGNAL_SUBSCRIBE) {
            signal->owner->co.rc = rc;
            set_resume(signal->owner);
            me->co.state = CO_STATE_JOIN_INTERRUPTED;
            return join_waitq(&signal->owner->waitq);
        }
    }
    return 0;
}


/**
 * Condition Variable Primitive.
 */
void
mnthr_cond_init(mnthr_cond_t *cond)
{
    DTQUEUE_INIT(&cond->waitq);
}


int
mnthr_cond_wait(mnthr_cond_t *cond)
{
    me->co.state = CO_STATE_CONDWAIT;
    return join_waitq(&cond->waitq);
}


void
mnthr_cond_signal_all(mnthr_cond_t *cond)
{
    resume_waitq_all(&cond->waitq);
}


void
mnthr_cond_signal_one(mnthr_cond_t *cond)
{
    resume_waitq_one(&cond->waitq);
}


void
mnthr_cond_fini(mnthr_cond_t *cond)
{
    mnthr_cond_signal_all(cond);
    DTQUEUE_FINI(&cond->waitq);
}



/**
 * Semaphore Primitive.
 */
void
mnthr_sema_init(mnthr_sema_t *sema, int n)
{
    mnthr_cond_init(&sema->cond);
    sema->n = n;
    sema->i = n;
}


int
mnthr_sema_acquire(mnthr_sema_t *sema)
{
    //int res = me->co.rc;
    int res = 0;

    if (sema->i > 0) {
        --(sema->i);

    } else {
        while (sema->i == 0) {
            if ((res = mnthr_cond_wait(&sema->cond)) != 0) {
                return res;
            }
        }
        assert((sema->i > 0) && (sema->i <= sema->n));
        --(sema->i);
    }

    return res;
}


int
mnthr_sema_try_acquire(mnthr_sema_t *sema)
{
    //int res = me->co.rc;
    int res = 0;

    if (sema->i > 0) {
        --(sema->i);
    } else {
        res = MNTHR_SEMA_TRY_ACQUIRE_FAIL;
    }

    return res;
}


void
mnthr_sema_release(mnthr_sema_t *sema)
{
    if (!((sema->i >= 0) && (sema->i < sema->n))) {
        CTRACE("i=%d n=%d", sema->i, sema->n);
    }
    assert((sema->i >= 0) && (sema->i < sema->n));
    mnthr_cond_signal_one(&sema->cond);
    ++(sema->i);
}


void
mnthr_sema_fini(mnthr_sema_t *sema)
{
    mnthr_cond_fini(&sema->cond);
    sema->n = -1;
    sema->i = -1;
}


/**
 * Inverted Semaphore
 */
void
mnthr_inverted_sema_init(mnthr_inverted_sema_t *sema, int n)
{
    mnthr_cond_init(&sema->cond);
    sema->n = n;
    sema->i = 0;
}


void
mnthr_inverted_sema_acquire(mnthr_inverted_sema_t *sema)
{
    assert((sema->i >= 0) && (sema->i <= sema->n));
    ++sema->i;
    mnthr_cond_signal_one(&sema->cond);
}


void
mnthr_inverted_sema_release(mnthr_inverted_sema_t *sema)
{
    --sema->i;
    assert((sema->i >= 0) && (sema->i <= sema->n));
}


int
mnthr_inverted_sema_wait(mnthr_inverted_sema_t *sema)
{
    int res;

    assert((sema->i >= 0) && (sema->i <= sema->n));
    res = 0;
    while (sema->i < sema->n) {
        if ((res = mnthr_cond_wait(&sema->cond)) != 0) {
            return res;
        }
    }

    return res;
}


void
mnthr_inverted_sema_fini(mnthr_inverted_sema_t *sema)
{
    mnthr_cond_fini(&sema->cond);
    sema->n = -1;
    sema->i = -1;
}



/**
 * Readers-writer Lock Primitive.
 */
void
mnthr_rwlock_init(mnthr_rwlock_t *lock)
{
    mnthr_cond_init(&lock->cond);
    lock->nreaders = 0;
    lock->fwriter = false;
}


int
mnthr_rwlock_acquire_read(mnthr_rwlock_t *lock)
{
    //int res = me->co.rc;
    int res = 0;

    while (lock->fwriter) {
        if ((res = mnthr_cond_wait(&lock->cond)) != 0) {
            return res;
        }
    }

    assert(!lock->fwriter);

    ++(lock->nreaders);

    return res;
}


int
mnthr_rwlock_try_acquire_read(mnthr_rwlock_t *lock)
{
    if (lock->fwriter) {
        return MNTHR_RWLOCK_TRY_ACQUIRE_READ_FAIL;
    }

    assert(!lock->fwriter);

    ++(lock->nreaders);

    return 0;
}


void
mnthr_rwlock_release_read(mnthr_rwlock_t *lock)
{
    assert(!lock->fwriter);

    --(lock->nreaders);
    if (lock->nreaders == 0) {
        mnthr_cond_signal_one(&lock->cond);
    }
}


int
mnthr_rwlock_acquire_write(mnthr_rwlock_t *lock)
{
    //int res = me->co.rc;
    int res = 0;

    while (lock->fwriter || (lock->nreaders > 0)) {

        if ((res = mnthr_cond_wait(&lock->cond)) != 0) {
            return res;
        }
    }

    assert(!(lock->fwriter || (lock->nreaders > 0)));

    lock->fwriter = true;

    return res;
}


int
mnthr_rwlock_try_acquire_write(mnthr_rwlock_t *lock)
{
    if (lock->fwriter || (lock->nreaders > 0)) {
        return MNTHR_RWLOCK_TRY_ACQUIRE_WRITE_FAIL;
    }

    assert(!(lock->fwriter || (lock->nreaders > 0)));

    lock->fwriter = true;

    return 0;
}


void
mnthr_rwlock_release_write(mnthr_rwlock_t *lock)
{
    assert(lock->fwriter && (lock->nreaders == 0));

    lock->fwriter = false;
    mnthr_cond_signal_all(&lock->cond);
}


void
mnthr_rwlock_fini(mnthr_rwlock_t *lock)
{
    mnthr_cond_fini(&lock->cond);
    lock->nreaders = 0;
    lock->fwriter = false;
}


/**
 * Coroutine based generator
 */
void
mnthr_gen_init(mnthr_gen_t *gen)
{
    MNTHR_SIGNAL_INIT(&gen->s0);
    MNTHR_SIGNAL_INIT(&gen->s1);
    gen->udata = NULL;
}


void
mnthr_gen_fini(mnthr_gen_t *gen)
{
    mnthr_signal_fini(&gen->s0);
    mnthr_signal_fini(&gen->s1);
    gen->udata = NULL;
}


int
mnthr_gen_yield(mnthr_gen_t *gen, void *udata)
{
    gen->udata = udata;
    mnthr_signal_send(&gen->s0);
    return mnthr_signal_subscribe(&gen->s1);
}


int
mnthr_gen_signal(mnthr_gen_t *gen, int rc)
{
    return mnthr_signal_error_and_join(&gen->s1, rc);
}


/**
 * Wait for another thread, and time it out if not completed within the
 * specified inverval of time.
 *
 * Return either thread's return code (typically >= 0), or
 * MNTHR_WAIT_TIMEOUT.
 */
int
mnthr_wait_for(uint64_t msec, const char *name, mnthr_cofunc_t f, int argc, ...)
{
    va_list ap;
    int res;
    mnthr_ctx_t *ctx;
    int64_t id;

    assert(me != NULL);

    va_start(ap, argc);
    VNEW_BODY(mnthr_ctx_pop_free);
    va_end(ap);
    if (ctx == NULL) {
        FAIL("mnthr_wait_for");
    }

    me->co.state = CO_STATE_WAITFOR;

    /* XXX put myself into both ctx->waitq and sleepq(WAITFOR) */
    append_me_to_waitq(&ctx->waitq);
    set_resume(ctx);
    id = ctx->co.id;

    //CTRACE("before sleep:");
    //mnthr_dump_sleepq();

    res = sleepmsec(msec);

    /* now remove me from both queues */

    //CTRACE("after sleep:");
    //mnthr_dump_sleepq();

    if (ctx->co.id != id || ctx->co.state == CO_STATE_DORMANT) {
        /* I had been sleeping, but by their exit I was resumed ... */

        //CTRACE("removing me:");
        //mnthr_dump(me);

        sleepq_remove(me);

        res = ctx->co.rc;

    } else {
        /* it's timeout, we have to interrupt it */
        assert(ctx->co.state & CO_STATE_RESUMABLE);

        remove_me_from_waitq(&ctx->waitq);
#ifndef NDEBUG
        if (ctx == me) {
            CTRACE("self-interrupting from within mnthr_wait_for:");
            mnthr_dump(ctx);
        }
#endif
        mnthr_set_interrupt(ctx);
        /*
         * override co.rc (was set to MNTHR_CO_RC_USER_INTERRUPTED in
         * mnthr_set_interrupt())
         */
        ctx->co.rc = MNTHR_CO_RC_TIMEDOUT;

        res = MNTHR_WAIT_TIMEOUT;
    }

    return res;
}


/*
 * Returns as mnthr_wait_for(), except it does not interrupt the target
 * thread.
 */
int
mnthr_peek(mnthr_ctx_t *ctx, uint64_t msec)
{
    int res;
    int64_t id;

    me->co.state = CO_STATE_PEEK;
    append_me_to_waitq(&ctx->waitq);
    id = ctx->co.id;
    res = sleepmsec(msec);
    if (ctx->co.id != id || ctx->co.state == CO_STATE_DORMANT) {
        /* I had been sleeping, but by their exit I was resumed ... */

        //CTRACE("removing me:");
        //mnthr_dump(me);

        sleepq_remove(me);

        res = ctx->co.rc;

    } else {
        /* it's timeout, we have to interrupt it */
        assert(ctx->co.state & CO_STATE_RESUMABLE);

        remove_me_from_waitq(&ctx->waitq);
#ifndef NDEBUG
        if (ctx == me) {
            CTRACE("self-interrupting from within mnthr_peek:");
            mnthr_dump(ctx);
        }
#endif
        res = MNTHR_WAIT_TIMEOUT;
    }

    return res;
}
