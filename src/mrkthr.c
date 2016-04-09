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
 * Basic thread locking primitive, mrkthr_signal_t.
 *
 * Basic time information. Internally the rdtsc() implementation for
 * x86-64 architecture is used to synchronize the notion of the
 * current time with the system's one. In the scheduler's execution
 * context, after each blocking system call returns, the rdtsc() is called
 * to update the library's "now" variable.  Also the corresponding
 * nanoseconds since the Epoch are calculated. The mrkthr_get_now()
 * returns the value of the former.
 *
 * Request for thread's interruption. The mrkthr_set_interrupt() will
 * turn the thread into an interrupted state, which effectively causes all
 * "yielding" mrkthr_ctx_* calls to fail on return. This way the thread
 * receives an indication for clean up and exiting.
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
#include <time.h>
#include <netdb.h>

#define NO_PROFILE
#include <mrkcommon/profile.h>

#ifdef DO_MEMDEBUG
#include <mrkcommon/memdebug.h>
MEMDEBUG_DECLARE(mrkthr);
#endif

//#define TRACE_VERBOSE
#include "diag.h"
#include <mrkcommon/dumpm.h>

#include <mrkcommon/array.h>
#include <mrkcommon/list.h>
#include <mrkcommon/dtqueue.h>
#include <mrkcommon/stqueue.h>
/* Experimental trie use */
#include <mrkcommon/btrie.h>

#include "mrkthr_private.h"


const profile_t *mrkthr_user_p;
const profile_t *mrkthr_swap_p;
const profile_t *mrkthr_sched0_p;
const profile_t *mrkthr_sched1_p;


typedef int (*writer_t) (int, int, int);

int mflags = 0;

ucontext_t main_uc;
static char main_stack[STACKSIZE];

static int co_id = 0;
static list_t ctxes;
mrkthr_ctx_t *me;

static STQUEUE(_mrkthr_ctx, free_list);

/*
 * Sleep list holds threads that are waiting for resume
 * in the future. It's prioritized by the thread's expire_ticks.
 */
btrie_t the_sleepq;


static int mrkthr_ctx_init(mrkthr_ctx_t *);
static int mrkthr_ctx_fini(mrkthr_ctx_t *);
static void co_fini_ucontext(struct _co *);
static void co_fini_other(struct _co *);
static void resume_waitq_all(mrkthr_waitq_t *);
static mrkthr_ctx_t *mrkthr_ctx_new(void);
static mrkthr_ctx_t *mrkthr_ctx_pop_free(void);
static void set_resume(mrkthr_ctx_t *);


void
push_free_ctx(mrkthr_ctx_t *ctx)
{
    //CTRACE("push_free_ctx");
    //mrkthr_dump(ctx);
    mrkthr_ctx_finalize(ctx);
    STQUEUE_ENQUEUE(&free_list, free_link, ctx);
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


static int
dump_sleepq_node(btrie_node_t *trn, uint64_t key, UNUSED void *udata)
{
    mrkthr_ctx_t *ctx = (mrkthr_ctx_t *)trn->value;
    if (ctx != NULL) {
        if (key != ctx->expire_ticks) {
            //CTRACE(FRED("trn=%p key=%016lx"), trn, key);
        } else {
            //CTRACE("trn=%p key=%016lx", trn, key);
        }
        mrkthr_dump(ctx);
    }
    return 0;
}


void
mrkthr_dump_sleepq(void)
{
    CTRACE("sleepq:");
    btrie_traverse(&the_sleepq, dump_sleepq_node, NULL);
    CTRACE("end of sleepq");
}


/* Sleep list */

void
sleepq_remove(mrkthr_ctx_t *ctx)
{
    btrie_node_t *trn;

    //CTRACE(FBLUE("SL removing"));
    //mrkthr_dump(ctx);
    //CTRACE(FBLUE("SL before removing:"));
    //mrkthr_dump_sleepq();
    //CTRACE(FBLUE("---"));

    if ((trn = btrie_find_exact(&the_sleepq, ctx->expire_ticks)) != NULL) {
        mrkthr_ctx_t *sle, *bucket_host_pretendent;

        sle = trn->value;

        assert(sle != NULL);

        //CTRACE("sle:");
        //mrkthr_dump(sle);
        //CTRACE("ctx:");
        //mrkthr_dump(ctx);
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
                    //mrkthr_dump(ctx);
                    //CTRACE("-----");
                    DTQUEUE_REMOVE(&sle->sleepq_bucket, sleepq_link, ctx);
                }
            }

        } else {
            //assert(sle == ctx);
            if (sle != ctx) {
                /*
                 * Here we have found ctx is not in the bucket.
                 * Just ignore it.
                 */
                //CTRACE("sle:");
                //mrkthr_dump(sle);
                //CTRACE("ctx: %p/%p", DTQUEUE_PREV(sleepq_link, ctx), DTQUEUE_NEXT(sleepq_link, ctx));
                //mrkthr_dump(ctx);
                //mrkthr_dump_sleepq();

                //assert(DTQUEUE_ORPHAN(&sle->sleepq_bucket, sleepq_link, ctx));
                //assert(DTQUEUE_EMPTY(&ctx->sleepq_bucket));
            } else {
                trn->value = NULL;
                btrie_remove_node(&the_sleepq, trn);
            }
        }
    } else {
    }
    //CTRACE(FBLUE("SL after removing:"));
    //mrkthr_dump_sleepq();
    //CTRACE(FBLUE("---"));
}


static void
sleepq_insert(mrkthr_ctx_t *ctx)
{
    btrie_node_t *trn;
    mrkthr_ctx_t *bucket_host;

    //CTRACE(FGREEN("SL inserting"));
    //mrkthr_dump(ctx);

    if ((trn = btrie_add_node(&the_sleepq, ctx->expire_ticks)) == NULL) {
        FAIL("btrie_add_node");
    }
    bucket_host = (mrkthr_ctx_t *)(trn->value);
    if (bucket_host != NULL) {
        //TRACE("while inserting, found bucket:");
        //mrkthr_dump(tmp);
        if (DTQUEUE_HEAD(&bucket_host->sleepq_bucket) == NULL) {
            DTQUEUE_ENQUEUE(&bucket_host->sleepq_bucket, sleepq_link, ctx);
        } else {
            DTQUEUE_INSERT_BEFORE(&bucket_host->sleepq_bucket,
                                  sleepq_link,
                                  DTQUEUE_HEAD(&bucket_host->sleepq_bucket),
                                  ctx);
        }

        //TRACE("After adding to the bucket:");
        //mrkthr_dump(tmp);
    } else {
        trn->value = ctx;
    }
    //CTRACE(FGREEN("SL after inserting:"));
    //mrkthr_dump_sleepq();
    //CTRACE(FGREEN("---"));
}


static void
sleepq_append(mrkthr_ctx_t *ctx)
{
    btrie_node_t *trn;
    mrkthr_ctx_t *bucket_host;

    //CTRACE(FGREEN("SL appending"));
    //mrkthr_dump(ctx);

    if ((trn = btrie_add_node(&the_sleepq, ctx->expire_ticks)) == NULL) {
        FAIL("btrie_add_node");
    }
    bucket_host = (mrkthr_ctx_t *)(trn->value);
    if (bucket_host != NULL) {
        //TRACE("while appending, found bucket:");
        //mrkthr_dump(tmp);
        DTQUEUE_ENQUEUE(&bucket_host->sleepq_bucket, sleepq_link, ctx);

        //TRACE("After adding to the bucket:");
        //mrkthr_dump(tmp);
    } else {
        trn->value = ctx;
    }
    //CTRACE(FGREEN("SL after appending:"));
    //mrkthr_dump_sleepq();
    //CTRACE(FGREEN("---"));
}


void
mrkthr_set_prio(mrkthr_ctx_t *ctx, int flag)
{
    ctx->sleepq_enqueue = flag ? sleepq_insert : sleepq_append;
}


/*
 * Module init/fini
 */
int
mrkthr_init(void)
{
    UNUSED size_t sz;

    if (mflags & CO_FLAG_INITIALIZED) {
        return 0;
    }

    PROFILE_INIT_MODULE();
    mrkthr_user_p = PROFILE_REGISTER("user");
    mrkthr_swap_p = PROFILE_REGISTER("swap");
    mrkthr_sched0_p = PROFILE_REGISTER("sched0");
    mrkthr_sched1_p = PROFILE_REGISTER("sched1");

#ifdef DO_MEMDEBUG
    MEMDEBUG_REGISTER(mrkthr);
#endif

    STQUEUE_INIT(&free_list);

    if (list_init(&ctxes, sizeof(mrkthr_ctx_t), 0,
                  (list_initializer_t)mrkthr_ctx_init,
                  (list_finalizer_t)mrkthr_ctx_fini) != 0) {
        FAIL("list_init");
    }

    poller_init();

    main_uc.uc_link = NULL;
    main_uc.uc_stack.ss_sp = main_stack;
    main_uc.uc_stack.ss_size = sizeof(main_stack);
    me = NULL;
    btrie_init(&the_sleepq);

    mflags |= CO_FLAG_INITIALIZED;

    return 0;
}


int
mrkthr_fini(void)
{
    if (!(mflags & CO_FLAG_INITIALIZED)) {
        return 0;
    }

    me = NULL;
    list_fini(&ctxes);
    STQUEUE_FINI(&free_list);
    btrie_fini(&the_sleepq);
    poller_fini();

    PROFILE_REPORT_SEC();
    PROFILE_FINI_MODULE();

    mflags &= ~CO_FLAG_INITIALIZED;

    return 0;
}


void
mrkthr_shutdown(void)
{
    mflags |= CO_FLAG_SHUTDOWN;
}


size_t
mrkthr_compact_sleepq(size_t threshold)
{
    size_t volume = 0;

    volume = btrie_get_volume(&the_sleepq);
    if (volume > threshold) {
        btrie_cleanup(&the_sleepq);
    }
    return volume;
}


size_t
mrkthr_get_sleepq_length(void)
{
    return btrie_get_nvals(&the_sleepq);
}


size_t
mrkthr_get_sleepq_volume(void)
{
    return btrie_get_volume(&the_sleepq);
}


int
dump_ctx_traverser(mrkthr_ctx_t *ctx, UNUSED void *udata)
{
    if ((ctx)->co.id != -1) {
        mrkthr_dump(ctx);
    }
    return 0;
}


void
mrkthr_dump_all_ctxes(void)
{
    TRACEC("all ctxes:\n");
    list_traverse(&ctxes, (list_traverser_t)dump_ctx_traverser, NULL);
    TRACEC("end of all ctxes\n");
}


/*
 * mrkthr_ctx management
 */
static int
mrkthr_ctx_init(mrkthr_ctx_t *ctx)
{
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
    ctx->co.state = CO_STATE_DORMANT;
    ctx->co.rc = 0;

    /* the rest of ctx */
    ctx->sleepq_enqueue = sleepq_append;

    DTQUEUE_INIT(&ctx->sleepq_bucket);
    DTQUEUE_ENTRY_INIT(sleepq_link, ctx);
    ctx->expire_ticks = 0;

    DTQUEUE_INIT(&ctx->waitq);

    DTQUEUE_ENTRY_INIT(waitq_link, ctx);
    ctx->hosting_waitq = NULL;

    STQUEUE_ENTRY_INIT(free_link, ctx);
    STQUEUE_ENTRY_INIT(runq_link, ctx);
    poller_mrkthr_ctx_init(ctx);

    return 0;
}


static void
co_fini_ucontext(struct _co *co)
{
    if (co->stack != MAP_FAILED) {
        munmap(co->stack, STACKSIZE);
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
    co->state = CO_STATE_DORMANT;
    // XXX let it stay for a while, and clear later ...
    //co->rc = 0;
}


void
mrkthr_ctx_finalize(mrkthr_ctx_t *ctx)
{
    /*
     * XXX not cleaning ucontext for future use.
     */

    /* remove me from sleepq */
    //DTQUEUE_FINI(&ctx->sleepq_bucket);
    //DTQUEUE_ENTRY_FINI(sleepq_link, ctx);
    ctx->expire_ticks = 0;

    co_fini_other(&ctx->co);

    /* resume all from my waitq */
    resume_waitq_all(&ctx->waitq);
    DTQUEUE_FINI(&ctx->waitq);

    /* remove me from someone else's waitq */
    if (ctx->hosting_waitq != NULL) {
        DTQUEUE_REMOVE(ctx->hosting_waitq, waitq_link, ctx);
        ctx->hosting_waitq = NULL;
    }

    poller_mrkthr_ctx_init(ctx);
}


static int
mrkthr_ctx_fini(mrkthr_ctx_t *ctx)
{
    co_fini_ucontext(&ctx->co);
    mrkthr_ctx_finalize(ctx);
    ctx->co.rc = 0;
    return 0;
}


/* Ugly hack to work around -Wclobbered, a part of -Wextra in gcc */
#ifdef __GNUC__
static int
_getcontext(ucontext_t *ucp)
{
    return getcontext(ucp);
}
#else
#define _getcontext getcontext
#endif


static mrkthr_ctx_t *
mrkthr_ctx_new(void)
{
    mrkthr_ctx_t *ctx;
    if ((ctx = list_incr(&ctxes)) == NULL) {
        FAIL("list_incr");
    }
    return ctx;
}


static mrkthr_ctx_t *
mrkthr_ctx_pop_free(void)
{
    mrkthr_ctx_t *ctx;

    if ((ctx = STQUEUE_HEAD(&free_list)) != NULL) {
        STQUEUE_DEQUEUE(&free_list, free_link);
        STQUEUE_ENTRY_FINI(free_link, ctx);
        ctx->co.rc = 0;
        //CTRACE("pop_free_ctx 0");
    } else {
        if ((ctx = list_incr(&ctxes)) == NULL) {
            FAIL("list_incr");
        }
        //CTRACE("pop_free_ctx 1");
    }
    //mrkthr_dump(ctx);
    return ctx;
}


/**
 * Return a new mrkthr_ctx_t instance. The new instance doesn't have to
 * be freed, and should be treated as an opaque object. It's internally
 * reclaimed as soon as the worker function returns.
 */
#define VNEW_BODY(get_ctx_fn)                                                  \
    int i;                                                                     \
    assert(mflags & CO_FLAG_INITIALIZED);                                      \
    ctx = get_ctx_fn();                                                        \
    assert(ctx!= NULL);                                                        \
    if (ctx->co.id != -1) {                                                    \
        mrkthr_dump(ctx);                                                      \
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
    if (ctx->co.stack == MAP_FAILED) {                                         \
        if ((ctx->co.stack = mmap(NULL,                                        \
                                  STACKSIZE,                                   \
                                  PROT_READ|PROT_WRITE,                        \
                                  MAP_PRIVATE|MAP_ANON,                        \
                                  -1,                                          \
                                  0)) == MAP_FAILED) {                         \
            TR(MRKTHR_NEW + 1);                                                \
            ctx = NULL;                                                        \
            goto vnew_body_end;                                                \
        }                                                                      \
        if (mprotect(ctx->co.stack, PAGE_SIZE, PROT_NONE) != 0) {              \
            FAIL("mprotect");                                                  \
        }                                                                      \
        ctx->co.uc.uc_stack.ss_sp = ctx->co.stack;                             \
        ctx->co.uc.uc_stack.ss_size = STACKSIZE;                               \
    }                                                                          \
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
    if (_getcontext(&ctx->co.uc) != 0) {                                       \
        TR(MRKTHR_CTX_NEW + 2);                                                \
        ctx = NULL;                                                            \
        goto vnew_body_end;                                                    \
    }                                                                          \
    makecontext(&ctx->co.uc, (void(*)(void))f, 2, ctx->co.argc, ctx->co.argv); \
vnew_body_end:                                                                 \



mrkthr_ctx_t *
mrkthr_new(const char *name, cofunc f, int argc, ...)
{
    va_list ap;
    mrkthr_ctx_t *ctx = NULL;

    va_start(ap, argc);
    VNEW_BODY(mrkthr_ctx_pop_free);
    va_end(ap);
    if (ctx == NULL) {
        FAIL("mrkthr_new");
    }
    return ctx;
}


mrkthr_ctx_t *
mrkthr_new_sig(const char *name, cofunc f, int argc, ...)
{
    va_list ap;
    mrkthr_ctx_t *ctx = NULL;

    va_start(ap, argc);
    VNEW_BODY(mrkthr_ctx_new);
    va_end(ap);
    if (ctx == NULL) {
        FAIL("mrkthr_new");
    }
    return ctx;
}


int
mrkthr_dump(const mrkthr_ctx_t *ctx)
{
    UNUSED ucontext_t uc;
    mrkthr_ctx_t *tmp;

    TRACEC("mrkthr %p/%s id=%d f=%p st=%s rc=%s exp=%016lx\n",
           ctx,
           ctx->co.name,
           ctx->co.id,
           ctx->co.f,
           CO_STATE_STR(ctx->co.state),
           CO_RC_STR(ctx->co.rc),
           ctx->expire_ticks
    );

    uc = ctx->co.uc;
    //dump_ucontext(&uc);
    if (DTQUEUE_HEAD(&ctx->sleepq_bucket) != NULL) {
        TRACEC("Bucket:\n");
        for (tmp = DTQUEUE_HEAD(&ctx->sleepq_bucket);
             tmp != NULL;
             tmp = DTQUEUE_NEXT(sleepq_link, tmp)) {

            TRACEC(" +mrkthr %p/%s id=%d f=%p st=%s rc=%s exp=%016lx\n",
                   tmp,
                   tmp->co.name,
                   tmp->co.id,
                   tmp->co.f,
                   CO_STATE_STR(tmp->co.state),
                   CO_RC_STR(tmp->co.rc),
                   tmp->expire_ticks
            );
        }
    }
    return 0;
}


PRINTFLIKE(2, 3) int
mrkthr_set_name(mrkthr_ctx_t *ctx,
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
 * mrkthr management
 */
mrkthr_ctx_t *
mrkthr_me(void)
{
    return me;
}


int
mrkthr_id(void)
{
    if (me != NULL) {
        return me->co.id;
    }
    return -1;
}


void
mrkthr_set_retval(int rv)
{
    assert(me != NULL);
    me->co.rc = rv;
}


int
yield(void)
{
    int res;

#ifdef TRACE_VERBOSE
    CTRACE("yielding from <<<");
    //mrkthr_dump(me);
#endif

    PROFILE_STOP(mrkthr_user_p);
    PROFILE_START(mrkthr_swap_p);
    res = swapcontext(&me->co.uc, &main_uc);
    PROFILE_STOP(mrkthr_swap_p);
    PROFILE_START(mrkthr_user_p);
    if(res != 0) {
        CTRACE("swapcontext() error");
        return setcontext(&main_uc);
    }

#ifdef TRACE_VERBOSE
    CTRACE("back from yield >>>");
    //mrkthr_dump(me);
#endif

    return me->co.rc;
}


static int
sleepmsec(uint64_t msec)
{
    /* first remove an old reference (if any) */
    sleepq_remove(me);

    if (msec == MRKTHR_SLEEP_FOREVER) {
        me->expire_ticks = MRKTHR_SLEEP_FOREVER;
    } else {
        if (msec == 0) {
            me->expire_ticks = 1;
        } else {
            me->expire_ticks = poller_msec2ticks_absolute(msec);
        }
    }

    //CTRACE("msec=%ld expire_ticks=%ld", msec, me->expire_ticks);

    me->sleepq_enqueue(me);

    return yield();
}


static int
sleepticks(uint64_t ticks)
{
    /* first remove an old reference (if any) */
    sleepq_remove(me);

    if (ticks == MRKTHR_SLEEP_FOREVER) {
        me->expire_ticks = MRKTHR_SLEEP_FOREVER;
    } else {
        if (ticks == 0) {
            me->expire_ticks = 1;
        } else {
            me->expire_ticks = poller_ticks_absolute(ticks);
        }
    }

    //CTRACE("ticks=%ld expire_ticks=%ld", ticks, me->expire_ticks);

    me->sleepq_enqueue(me);

    return yield();
}


int
mrkthr_sleep(uint64_t msec)
{
    assert(me != NULL);
    /* put into sleepq(SLEEP) */
    me->co.state = CO_STATE_SLEEP;
    return sleepmsec(msec);
}


int
mrkthr_sleep_ticks(uint64_t ticks)
{
    assert(me != NULL);
    /* put into sleepq(SLEEP) */
    me->co.state = CO_STATE_SLEEP;
    return sleepticks(ticks);
}


static void
append_me_to_waitq(mrkthr_waitq_t *waitq)
{
    assert(me != NULL);
    if (me->hosting_waitq != NULL) {
        DTQUEUE_REMOVE_DIRTY(me->hosting_waitq, waitq_link, me);
    }
    DTQUEUE_ENQUEUE(waitq, waitq_link, me);
    me->hosting_waitq = waitq;
}


static void
remove_me_from_waitq(mrkthr_waitq_t *waitq)
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
join_waitq(mrkthr_waitq_t *waitq)
{
    append_me_to_waitq(waitq);
    return yield();
}


int
mrkthr_join(mrkthr_ctx_t *ctx)
{
    if (!(ctx->co.state & CO_STATE_RESUMABLE)) {
        /* dormant thread, or an attempt to join self ? */
        return MRKTHR_JOIN_FAILURE;
    }
    me->co.state = CO_STATE_JOIN;
    return join_waitq(&ctx->waitq);
}


static void
resume_waitq_all(mrkthr_waitq_t *waitq)
{
    mrkthr_ctx_t *t;

    while ((t = DTQUEUE_HEAD(waitq)) != NULL) {
        assert(t->hosting_waitq = waitq);
        DTQUEUE_DEQUEUE(waitq, waitq_link);
        DTQUEUE_ENTRY_FINI(waitq_link, t);
        t->hosting_waitq = NULL;
        set_resume(t);
    }
}


static void
resume_waitq_one(mrkthr_waitq_t *waitq)
{
    mrkthr_ctx_t *t;

    if ((t = DTQUEUE_HEAD(waitq)) != NULL) {
        assert(t->hosting_waitq = waitq);
        DTQUEUE_DEQUEUE(waitq, waitq_link);
        DTQUEUE_ENTRY_FINI(waitq_link, t);
        t->hosting_waitq = NULL;
        set_resume(t);
    }
}


void
mrkthr_run(mrkthr_ctx_t *ctx)
{
    assert(ctx != me);
#ifndef NDEBUG
    if (ctx->co.state != CO_STATE_DORMANT) {
        CTRACE("precondition failed. Non-dormant ctx is %p", ctx);
        if (ctx != NULL) {
            D8(ctx, sizeof(mrkthr_ctx_t));
            CTRACE("now trying to dump it ...");
            mrkthr_dump(ctx);
        }
    }
#endif
    assert(ctx->co.state == CO_STATE_DORMANT);

    set_resume(ctx);
}


mrkthr_ctx_t *
mrkthr_spawn(const char *name, cofunc f, int argc, ...)
{
    va_list ap;
    mrkthr_ctx_t *ctx = NULL;

    va_start(ap, argc);
    VNEW_BODY(mrkthr_ctx_pop_free);
    va_end(ap);
    if (ctx == NULL) {
        FAIL("mrkthr_spawn");
    }
    mrkthr_run(ctx);
    return ctx;
}


mrkthr_ctx_t *
mrkthr_spawn_sig(const char *name, cofunc f, int argc, ...)
{
    va_list ap;
    mrkthr_ctx_t *ctx = NULL;

    va_start(ap, argc);
    VNEW_BODY(mrkthr_ctx_new);
    va_end(ap);
    if (ctx == NULL) {
        FAIL("mrkthr_spawn");
    }
    mrkthr_run(ctx);
    return ctx;
}



static void
set_resume(mrkthr_ctx_t *ctx)
{
    assert(ctx != me);

    //CTRACE("Setting for resume: ---");
    //mrkthr_dump(ctx);
    //CTRACE("---");

    //assert(ctx->co.f != NULL);
    if (ctx->co.f == NULL) {
        CTRACE("Will not resume this ctx:");
        mrkthr_dump(ctx);
        return;
    }

    /* first remove an old reference (if any) */
    sleepq_remove(ctx);

    ctx->co.state = CO_STATE_SET_RESUME;
    ctx->expire_ticks = 1;
    ctx->sleepq_enqueue(ctx);
}


/**
 * Send an interrupt signal to the thread.
 */
void
mrkthr_set_interrupt(mrkthr_ctx_t *ctx)
{
#ifndef NDEBUG
    if (ctx == me) {
        CTRACE("precondition failed. self-interrupting ctx is %p", ctx);
        if (ctx != NULL) {
            D8(ctx, sizeof(mrkthr_ctx_t));
            CTRACE("now trying to dump it ...");
            mrkthr_dump(ctx);
        }
    }
#endif
    assert(ctx != me);

    //mrkthr_dump(ctx);

    if (ctx->co.f == NULL) {
#ifdef TRACE_VERBOSE
        CTRACE("Will not interrupt this ctx:");
        mrkthr_dump(ctx);
#endif
        return;
    }

    /* first remove an old reference (if any) */
    sleepq_remove(ctx);

    /* clear event */
    if (ctx->co.state & (CO_STATE_READ | CO_STATE_WRITE)) {
        poller_clear_event(ctx);
    }

    /*
     * We are ignoring all event management rules here.
     */
    ctx->co.rc = CO_RC_USER_INTERRUPTED;
    ctx->co.state = CO_STATE_SET_INTERRUPT;
    ctx->expire_ticks = 1;
    ctx->sleepq_enqueue(ctx);
}


int
mrkthr_set_interrupt_and_join(mrkthr_ctx_t *ctx)
{
    mrkthr_set_interrupt(ctx);
    if (!(ctx->co.state & CO_STATE_RESUMABLE)) {
        /* dormant thread, or an attempt to join self ? */
        return MRKTHR_JOIN_FAILURE;
    }
    me->co.state = CO_STATE_JOIN_INTERRUPTED;
    return join_waitq(&ctx->waitq);
}


int
mrkthr_is_dead(mrkthr_ctx_t *ctx)
{
    return ctx->co.id == -1;
}


/*
 * socket/file/etc
 */

int
mrkthr_socket(const char *hostname,
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
mrkthr_socket_connect(const char *hostname,
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
        FAIL("getaddrinfo");
    }

    fd = -1;
    for (ai = ainfos; ai != NULL; ai = ai->ai_next) {
        if ((fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)) == -1) {
            continue;
        }

        if (mrkthr_connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
            perror("mrkthr_connect");
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
mrkthr_connect(int fd, const struct sockaddr *addr, socklen_t addrlen)
{
    int res = 0;

    if (fcntl(fd, F_SETFL, O_NONBLOCK) == -1) {
        perror("fcntl");
        TRRET(MRKTHR_CONNECT + 1);
    }

    if ((res = connect(fd, addr, addrlen)) != 0) {
        perror("connect");
        if (errno == EINPROGRESS) {
            int optval;
            socklen_t optlen;

            if (mrkthr_get_wbuflen(fd) < 0) {
                TRRET(MRKTHR_CONNECT + 2);
            }

            optlen = sizeof(optval);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &optval, &optlen) != 0) {
                perror("getsockopt");
                TRRET(MRKTHR_CONNECT + 3);
            }
            res = optval;
        }
    }
    return res;
}


int
mrkthr_accept_all(int fd, mrkthr_socket_t **buf, off_t *offset)
{
    ssize_t navail;
    ssize_t nread = 0;
    mrkthr_socket_t *tmp;

    assert(me != NULL);

    if ((navail = mrkthr_get_rbuflen(fd)) <= 0) {
        TRRET(MRKTHR_ACCEPT_ALL + 1);
    }

    if ((tmp = realloc(*buf, (*offset + navail) * sizeof(mrkthr_socket_t))) == NULL) {
        FAIL("realloc");
    }
    *buf = tmp;

    if (navail == 0) {
        /* EOF ? */
        TRRET(MRKTHR_ACCEPT_ALL + 2);
    }

    while (nread < navail) {
        tmp = *buf + (*offset + nread);
        tmp->addrlen = sizeof(union _mrkthr_addr);

        if ((tmp->fd = accept(fd, &tmp->addr.sa, &tmp->addrlen)) == -1) {
            //perror("accept");
            break;
        }
        ++nread;
    }

    if (nread < navail) {
        //TRACE("nread=%ld navail=%ld", nread, navail);
        if (nread == 0) {
            TRRET(MRKTHR_ACCEPT_ALL + 4);
        }
    }

    *offset += nread;

    return 0;
}


/**
 * Allocate enough space in *buf beyond *offset for a single read
 * operation and read into that location from fd.
 */
int
mrkthr_read_all(int fd, char **buf, off_t *offset)
{
    ssize_t navail;
    ssize_t nread;
    char *tmp;

    assert(me != NULL);

    if ((navail = mrkthr_get_rbuflen(fd)) <= 0) {
        TRRET(MRKTHR_READ_ALL + 1);
    }

    if ((tmp = realloc(*buf, *offset + navail)) == NULL) {
        FAIL("realloc");
    }
    *buf = tmp;

    if (navail == 0) {
        /* EOF ? */
        TRRET(MRKTHR_READ_ALL + 2);
    }

    if ((nread = read(fd, *buf + *offset, navail)) == -1) {
        perror("read");
        TRRET(MRKTHR_READ_ALL + 3);
    }

    if (nread < navail) {
        //TRACE("nread=%ld navail=%ld", nread, navail);
        if (nread == 0) {
            TRRET(MRKTHR_READ_ALL + 4);
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
mrkthr_read_allb(int fd, char *buf, ssize_t sz)
{
    ssize_t navail;
    ssize_t nread;

    assert(me != NULL);

    if ((navail = mrkthr_get_rbuflen(fd)) <= 0) {
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
 * edge-triggered version of mrkthr_read_allb()
 */
ssize_t
mrkthr_read_allb_et(int fd, char *buf, ssize_t sz)
{
    ssize_t nleft, totread;

    nleft = sz;
    totread = 0;
    while (totread < sz) {
        ssize_t nread;

        if ((nread = read(fd, buf + totread, nleft)) == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                ssize_t navail;

                if ((navail = mrkthr_get_rbuflen(fd)) < 0) {
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


/**
 * Perform a single recvfrom from fd into buf.
 * Return the number of bytes received or -1 in case of error.
 */
ssize_t
mrkthr_recvfrom_allb(int fd,
                     void * restrict buf,
                     ssize_t sz,
                     int flags,
                     struct sockaddr * restrict from,
                     socklen_t * restrict fromlen)
{
    ssize_t navail;
    ssize_t nrecv;

    assert(me != NULL);

    if ((navail = mrkthr_get_rbuflen(fd)) <= 0) {
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
mrkthr_write_all(int fd, const char *buf, size_t len)
{
    ssize_t navail;
    ssize_t nwritten;
    off_t remaining = len;

    assert(me != NULL);

    while (remaining > 0) {
        if ((navail = mrkthr_get_wbuflen(fd)) <= 0) {
            TRRET(MRKTHR_WRITE_ALL + 1);
        }

        if ((nwritten = write(fd, buf + len - remaining,
                              MIN(navail, remaining))) == -1) {

            TRRET(MRKTHR_WRITE_ALL + 2);
        }
        remaining -= nwritten;
    }
    return 0;
}


int
mrkthr_write_all_et(int fd, const char *buf, size_t len)
{
    ssize_t nwritten;
    off_t remaining = len;
    ssize_t navail = len;

    assert(me != NULL);

    while (remaining > 0) {
        if ((nwritten = write(fd, buf + len - remaining,
                              MIN(navail, remaining))) == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if ((navail = mrkthr_get_wbuflen(fd)) <= 0) {
                    TRRET(MRKTHR_WRITE_ALL + 1);
                }
                continue;

            } else {
                TRRET(MRKTHR_WRITE_ALL + 2);
            }

        }

        remaining -= nwritten;
    }
    return 0;
}


/**
 * Write len bytes from buf into fd.
 */
int
mrkthr_sendto_all(int fd,
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
        if ((navail = mrkthr_get_wbuflen(fd)) <= 0) {
            TRRET(MRKTHR_SENDTO_ALL + 1);
        }

        if ((nwritten = sendto(fd, ((const char *)buf) + len - remaining,
                               MIN((size_t)navail, remaining),
                               flags, to, tolen)) == -1) {

            TRRET(MRKTHR_SENDTO_ALL + 2);
        }
        remaining -= nwritten;
    }
    return 0;
}


/**
 * Event Primitive.
 */
void
mrkthr_signal_init(mrkthr_signal_t *signal, mrkthr_ctx_t *ctx)
{
    signal->owner = ctx;
}


void
mrkthr_signal_fini(mrkthr_signal_t *signal)
{
    signal->owner = NULL;
}


int
mrkthr_signal_has_owner(mrkthr_signal_t *signal)
{
    return signal->owner != NULL;
}


int
mrkthr_signal_subscribe(UNUSED mrkthr_signal_t *signal)
{
    assert(signal->owner == me);

    //CTRACE("holding on ...");
    me->co.state = CO_STATE_SIGNAL_SUBSCRIBE;
    return yield();
}


int
mrkthr_signal_subscribe_with_timeout(UNUSED mrkthr_signal_t *signal,
                                     uint64_t msec)
{
    int res;

    assert(signal->owner == me);

    //CTRACE("holding on ...");
    me->co.state = CO_STATE_SIGNAL_SUBSCRIBE;
    res = sleepmsec(msec);
    if (me->expire_ticks == 1) {
        /* I had been sleeping, but was resumed by signal_send() ... */
    } else {
        res = MRKTHR_WAIT_TIMEOUT;
    }
    return res;
}


void
mrkthr_signal_send(mrkthr_signal_t *signal)
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
mrkthr_signal_error(mrkthr_signal_t *signal, unsigned char rc)
{
    if (signal->owner != NULL) {
        if (signal->owner->co.state == CO_STATE_SIGNAL_SUBSCRIBE) {
            signal->owner->co.rc = rc;
            set_resume(signal->owner);
        }
    }
}


/**
 * Condition Variable Primitive.
 */
void
mrkthr_cond_init(mrkthr_cond_t *cond)
{
    DTQUEUE_INIT(&cond->waitq);
}


int
mrkthr_cond_wait(mrkthr_cond_t *cond)
{
    me->co.state = CO_STATE_CONDWAIT;
    return join_waitq(&cond->waitq);
}


void
mrkthr_cond_signal_all(mrkthr_cond_t *cond)
{
    resume_waitq_all(&cond->waitq);
}


void
mrkthr_cond_signal_one(mrkthr_cond_t *cond)
{
    resume_waitq_one(&cond->waitq);
}


void
mrkthr_cond_fini(mrkthr_cond_t *cond)
{
    mrkthr_cond_signal_all(cond);
    DTQUEUE_FINI(&cond->waitq);
}



/**
 * Semaphore Primitive.
 */
void
mrkthr_sema_init(mrkthr_sema_t *sema, int n)
{
    mrkthr_cond_init(&sema->cond);
    sema->n = n;
    sema->i = n;
}


int
mrkthr_sema_acquire(mrkthr_sema_t *sema)
{
    int res = 0;

    if (sema->i > 0) {
        --(sema->i);

    } else {
        if ((res = mrkthr_cond_wait(&sema->cond)) != 0) {
            return res;
        }

        assert((sema->i > 0) && (sema->i <= sema->n));
        --(sema->i);
    }

    return res;
}


void
mrkthr_sema_release(mrkthr_sema_t *sema)
{
    assert((sema->i >= 0) && (sema->i < sema->n));
    mrkthr_cond_signal_one(&sema->cond);
    ++(sema->i);
}


void
mrkthr_sema_fini(mrkthr_sema_t *sema)
{
    mrkthr_cond_fini(&sema->cond);
    sema->n = -1;
    sema->i = -1;
}


/**
 * Readers-writer Lock Primitive.
 */
void
mrkthr_rwlock_init(mrkthr_rwlock_t *lock)
{
    mrkthr_cond_init(&lock->cond);
    lock->fwriter = 0;
    lock->nreaders = 0;
}


int
mrkthr_rwlock_acquire_read(mrkthr_rwlock_t *lock)
{
    int res = 0;

    if (lock->fwriter) {
        if ((res = mrkthr_cond_wait(&lock->cond)) != 0) {
            return res;
        }
    }

    assert(!lock->fwriter);

    ++(lock->nreaders);

    return res;
}


int
mrkthr_rwlock_try_acquire_read(mrkthr_rwlock_t *lock)
{
    if (lock->fwriter) {
        return MRKTHR_RWLOCK_TRY_ACQUIRE_READ_FAIL;
    }

    assert(!lock->fwriter);

    ++(lock->nreaders);

    return 0;
}


void
mrkthr_rwlock_release_read(mrkthr_rwlock_t *lock)
{
    assert(!lock->fwriter);

    --(lock->nreaders);
    if (lock->nreaders == 0) {
        mrkthr_cond_signal_one(&lock->cond);
    }
}


int
mrkthr_rwlock_acquire_write(mrkthr_rwlock_t *lock)
{
    int res = 0;

    if (lock->fwriter || (lock->nreaders > 0)) {

        if ((res = mrkthr_cond_wait(&lock->cond)) != 0) {
            return res;
        }
    }

    assert(!(lock->fwriter || (lock->nreaders > 0)));

    lock->fwriter = 1;

    return res;
}


int
mrkthr_rwlock_try_acquire_write(mrkthr_rwlock_t *lock)
{
    if (lock->fwriter || (lock->nreaders > 0)) {
        return MRKTHR_RWLOCK_TRY_ACQUIRE_WRITE_FAIL;
    }

    assert(!(lock->fwriter || (lock->nreaders > 0)));

    lock->fwriter = 1;

    return 0;
}


void
mrkthr_rwlock_release_write(mrkthr_rwlock_t *lock)
{
    assert(lock->fwriter && (lock->nreaders == 0));

    lock->fwriter = 0;
    mrkthr_cond_signal_all(&lock->cond);
}


void
mrkthr_rwlock_fini(mrkthr_rwlock_t *lock)
{
    lock->fwriter = 0;
    lock->nreaders = 0;
    mrkthr_cond_fini(&lock->cond);
}


/**
 * Wait for another thread, and time it out if not completed within the
 * specified inverval of time.
 */
int
mrkthr_wait_for(uint64_t msec, const char *name, cofunc f, int argc, ...)
{
    va_list ap;
    int res;
    mrkthr_ctx_t *ctx;

    assert(me != NULL);

    va_start(ap, argc);
    VNEW_BODY(mrkthr_ctx_pop_free);
    va_end(ap);
    if (ctx == NULL) {
        FAIL("mrkthr_wait_for");
    }

    me->co.state = CO_STATE_WAITFOR;

    /* XXX put myself into both ctx->waitq and sleepq(WAITFOR) */
    append_me_to_waitq(&ctx->waitq);
    set_resume(ctx);

    //CTRACE("before sleep:");
    //mrkthr_dump_sleepq();

    res = sleepmsec(msec);

    /* now remove me from both queues */

    //CTRACE("after sleep:");
    //mrkthr_dump_sleepq();

    if (ctx->co.state == CO_STATE_DORMANT) {
        /* I had been sleeping, but by their exit I was resumed ... */

        //CTRACE("removing me:");
        //mrkthr_dump(me);

        sleepq_remove(me);

        res = ctx->co.rc;

    } else {
        /* it's timeout, we have to interrupt it */
        assert(ctx->co.state & CO_STATE_RESUMABLE);

        remove_me_from_waitq(&ctx->waitq);
#ifndef NDEBUG
        if (ctx == me) {
            CTRACE("self-interrupting from within mrkthr_wait_for:");
            mrkthr_dump(ctx);
        }
#endif
        mrkthr_set_interrupt(ctx);
        /*
         * override co.rc (was set to CO_RC_USER_INTERRUPTED in
         * mrkthr_set_interrupt())
         */
        ctx->co.rc = CO_RC_TIMEDOUT;

        res = MRKTHR_WAIT_TIMEOUT;
    }

    return res;
}

