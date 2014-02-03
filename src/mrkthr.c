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
 *
 *
 * Miscellaneous.
 *
 * EX-TODO: Optimize sleep lists performance. Sleep lists are essentially
 * priority queues. Linear insert time is not acceptable in the thread
 * management. The issue was addressed by devising a somewhat unusual
 * combination of red-black tree (equipped with the in-order traversal)
 * and a sort of hash-like "buckets" internal to the tree entries, the
 * mrkthr_ctx_t * instances. When put together, this makes an effect of
 * a multimap, and provides for O(log(N)) sleep queue insert and delete
 * time complexity.
 *
 *
 * TODO list:
 *  - time API. Currently the only API is mrkthr_get_now() which reports
 *    the number of nanoseconds since the Epoch.
 *  - wait_for() function that would run another function and wait for its
 *    return.
 *  - port to Linux epoll
 */
#include <sys/types.h>
#include <sys/param.h> /* PAGE_SIZE */
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stdint.h>
#include <sys/sysctl.h>
#include <sys/time.h>

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "diag.h"
#include <mrkcommon/dumpm.h>
//#define TRACE_VERBOSE

#include <mrkcommon/array.h>
#include <mrkcommon/list.h>
#include <mrkcommon/dtqueue.h>
#include <mrkcommon/stqueue.h>
#include <kevent_util.h>
/* Experimental trie use */
#include <mrkcommon/trie.h>
#include "mrkthr_private.h"

#include <mrkcommon/memdebug.h>
MEMDEBUG_DECLARE(mrkthr);

typedef int (*writer_t) (int, int, int);

#define CO_FLAG_INITIALIZED 0x01
#define CO_FLAG_SHUTDOWN 0x02
static int mflags = 0;

static ucontext_t main_uc;
static char main_stack[STACKSIZE];

static int q0 = -1;
static array_t kevents0;
static array_t kevents1;

static int co_id = 0;
static list_t ctxes;
static mrkthr_ctx_t *me;

static STQUEUE(_mrkthr_ctx, free_list);

/*
 * Sleep list holds threads that are waiting for resume
 * in the future. It's prioritized by the thread's expire_ticks.
 */
static trie_t the_sleepq;

#ifdef USE_TSC
static uint64_t timecounter_freq;
#else
#   define timecounter_freq (1000000000)
#endif

static uint64_t nsec_zero, nsec_now;
#ifdef USE_TSC
static uint64_t timecounter_zero, timecounter_now;
#else
#   define timecounter_now nsec_now
#endif

static int mrkthr_ctx_init(mrkthr_ctx_t *);
static int mrkthr_ctx_fini(mrkthr_ctx_t *);
static struct kevent *new_event(int, int, int *);
static void co_fini_ucontext(struct _co *);
static void co_fini_other(struct _co *);
static void mrkthr_ctx_finalize(mrkthr_ctx_t *);
static mrkthr_ctx_t *mrkthr_vnew(const char *, cofunc, int, va_list);
static void resume_waitq_all(mrkthr_waitq_t *);
static int discard_event(int, int);
static void push_free_ctx(mrkthr_ctx_t *);
static mrkthr_ctx_t *pop_free_ctx(void);
static void set_resume(mrkthr_ctx_t *);
static void clear_event(int, int, int);
static struct kevent *get_event(int);

static inline uint64_t
rdtsc(void)
{
  uint64_t res;

  __asm __volatile ("rdtsc; shl $32,%%rdx; or %%rdx,%%rax"
                    : "=a"(res)
                    :
                    : "%rcx", "%rdx"
                   );
  return res;
}

static void
update_now(void)
{
#ifdef USE_TSC
    timecounter_now = rdtsc();
    /* do it here so that get_now() returns precomputed value */
    nsec_now = nsec_zero + (uint64_t)(((long double)(timecounter_now - timecounter_zero)) /
        ((long double)(timecounter_freq)) * 1000000000.);
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
 * periodically correct the internal wallclock along with that system's.
 */
int
wallclock_init(void)
{
    struct timespec ts;

#ifdef USE_TSC
    timecounter_zero = rdtsc();
#endif

    if (clock_gettime(CLOCK_REALTIME_PRECISE, &ts) != 0) {
        TRRET(WALLCLOCK_INIT + 1);
    }
    nsec_zero = ts.tv_nsec + ts.tv_sec * 1000000000;
    return 0;
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

UNUSED static void
dump_ucontext (ucontext_t *uc)
{
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
}

UNUSED static int
dump_sleepq_node(trie_node_t *trn, uint64_t key, UNUSED void *udata)
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
    trie_traverse(&the_sleepq, dump_sleepq_node, NULL);
    CTRACE("end of sleepq");
}

/* Sleep list */

static void
sleepq_remove(mrkthr_ctx_t *ctx)
{
    trie_node_t *trn;

    //CTRACE(FBLUE("SL removing"));
    //mrkthr_dump(ctx);
    //CTRACE(FBLUE("SL before removing:"));
    //mrkthr_dump_sleepq();
    //CTRACE(FBLUE("---"));

    if ((trn = trie_find_exact(&the_sleepq, ctx->expire_ticks)) != NULL) {
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
                trie_remove_node(&the_sleepq, trn);
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
    trie_node_t *trn;
    mrkthr_ctx_t *bucket_host;

    //CTRACE(FGREEN("SL inserting"));
    //mrkthr_dump(ctx);

    if ((trn = trie_add_node(&the_sleepq, ctx->expire_ticks)) == NULL) {
        FAIL("trie_add_node");
    }
    bucket_host = (mrkthr_ctx_t *)(trn->value);
    if (bucket_host != NULL) {
        //TRACE("while inserting, found bucket:");
        //mrkthr_dump(tmp);

        DTQUEUE_ENQUEUE(&bucket_host->sleepq_bucket, sleepq_link, ctx);

        //TRACE("After adding to the bucket:");
        //mrkthr_dump(tmp);
    } else {
        trn->value = ctx;
    }
    //CTRACE(FGREEN("SL after inserting:"));
    //mrkthr_dump_sleepq();
    //CTRACE(FGREEN("---"));
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

    MEMDEBUG_REGISTER(mrkthr);

    STQUEUE_INIT(&free_list);

    if (list_init(&ctxes, sizeof(mrkthr_ctx_t), 0,
                  (list_initializer_t)mrkthr_ctx_init,
                  (list_finalizer_t)mrkthr_ctx_fini) != 0) {
        FAIL("list_init");
    }

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

    main_uc.uc_link = NULL;
    main_uc.uc_stack.ss_sp = main_stack;
    main_uc.uc_stack.ss_size = sizeof(main_stack);
    me = NULL;
    trie_init(&the_sleepq);
#ifdef USE_TSC
    sz = sizeof(timecounter_freq);
    if (sysctlbyname("machdep.tsc_freq", &timecounter_freq, &sz, NULL, 0) != 0) {
        FAIL("sysctlbyname");
    }
#endif

    if (wallclock_init() != 0) {
        FAIL("wallclock_init");
    }

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
    array_fini(&kevents0);
    array_fini(&kevents1);
    list_fini(&ctxes);
    STQUEUE_FINI(&free_list);
    trie_fini(&the_sleepq);
    close(q0);

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

    volume = trie_get_volume(&the_sleepq);
    if (volume > threshold) {
        trie_cleanup(&the_sleepq);
    }
    return volume;
}

size_t
mrkthr_get_sleepq_length(void)
{
    return trie_get_nvals(&the_sleepq);
}

size_t
mrkthr_get_sleepq_volume(void)
{
    return trie_get_volume(&the_sleepq);
}

static int
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
    CTRACE("all ctxes:");
    list_traverse(&ctxes, (list_traverser_t)dump_ctx_traverser, NULL);
    CTRACE("end of all ctxes");
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
    DTQUEUE_INIT(&ctx->sleepq_bucket);
    DTQUEUE_ENTRY_INIT(sleepq_link, ctx);
    ctx->expire_ticks = 0;

    DTQUEUE_INIT(&ctx->waitq);

    DTQUEUE_ENTRY_INIT(waitq_link, ctx);
    ctx->hosting_waitq = NULL;

    STQUEUE_ENTRY_INIT(free_link, ctx);
    STQUEUE_ENTRY_INIT(runq_link, ctx);
    ctx->_idx0 = -1;

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
    co->rc = 0;
}


static void
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

    ctx->_idx0 = -1;
}

static int
mrkthr_ctx_fini(mrkthr_ctx_t *ctx)
{
    co_fini_ucontext(&ctx->co);
    mrkthr_ctx_finalize(ctx);
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

static void
push_free_ctx(mrkthr_ctx_t *ctx)
{
    STQUEUE_ENQUEUE(&free_list, free_link, ctx);
    //CTRACE("push_free_ctx");
    //mrkthr_dump(ctx);
}

static mrkthr_ctx_t *
pop_free_ctx(void)
{
    mrkthr_ctx_t *ctx = NULL;

    if ((ctx = STQUEUE_HEAD(&free_list)) != NULL) {
        STQUEUE_DEQUEUE(&free_list, free_link);
        STQUEUE_ENTRY_FINI(free_link, ctx);
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

static mrkthr_ctx_t *
mrkthr_vnew(const char *name, cofunc f, int argc, va_list ap)
{
    int i;
    mrkthr_ctx_t *ctx = NULL;


    assert(mflags & CO_FLAG_INITIALIZED);
    ctx = pop_free_ctx();

    assert(ctx!= NULL);
    if (ctx->co.id != -1) {
        mrkthr_dump(ctx);
        CTRACE("This happened during thread %s creation", name);
    }
    assert(ctx->co.id == -1);

    /* Thread id is actually an index into the ctxes list */
    ctx->co.id = co_id++;

    if (name != NULL) {
        strncpy(ctx->co.name, name, sizeof(ctx->co.name));
    } else {
        ctx->co.name[0] = '\0';
    }

    if (ctx->co.stack == MAP_FAILED) {
        if ((ctx->co.stack = mmap(NULL, STACKSIZE, PROT_READ|PROT_WRITE,
                                  MAP_ANON, -1, 0)) == MAP_FAILED) {
            TRRETNULL(MRKTHR_CTX_NEW + 1);
        }
        if (mprotect(ctx->co.stack, PAGE_SIZE, PROT_NONE) != 0) {
            FAIL("mprotect");
        }
        ctx->co.uc.uc_stack.ss_sp = ctx->co.stack;
        ctx->co.uc.uc_stack.ss_size = STACKSIZE;
    }

    ctx->co.uc.uc_link = &main_uc;

    ctx->co.f = f;

    if (argc > 0) {
        ctx->co.argc = argc;
        if ((ctx->co.argv = malloc(sizeof(void *) * ctx->co.argc)) == NULL) {
            FAIL("malloc");
        }
        for (i = 0; i < ctx->co.argc; ++i) {
            ctx->co.argv[i] = va_arg(ap, void *);
            //CTRACE("ctx->co.argv[%d]=%p", i, ctx->co.argv[i]);
        }
    }

    if (_getcontext(&ctx->co.uc) != 0) {
        TRRETNULL(MRKTHR_CTX_NEW + 2);
    }
    makecontext(&ctx->co.uc, (void(*)(void))f, 2, ctx->co.argc, ctx->co.argv);

    return ctx;
}

mrkthr_ctx_t *
mrkthr_new(const char *name, cofunc f, int argc, ...)
{
    va_list ap;
    mrkthr_ctx_t *ctx = NULL;

    va_start(ap, argc);
    ctx = mrkthr_vnew(name, f, argc, ap);
    va_end(ap);
    return ctx;
}

int
mrkthr_dump(const mrkthr_ctx_t *ctx)
{
    ucontext_t uc;
    mrkthr_ctx_t *tmp;

    CTRACE("mrkthr_ctx@%p '%s' id=%d f=%p st=%s rc=%s exp=%016lx",
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
        CTRACE("Bucket:");
        for (tmp = DTQUEUE_HEAD(&ctx->sleepq_bucket);
             tmp != NULL;
             tmp = DTQUEUE_NEXT(sleepq_link, tmp)) {

            CTRACE(" mrkthr_ctx@%p '%s' id=%d f=%p st=%s rc=%s exp=%016lx",
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


static int
yield(void)
{
    int res;

    //CTRACE("yielding from ...");
    //mrkthr_dump(me);
    res = swapcontext(&me->co.uc, &main_uc);
    if(res != 0) {
        CTRACE("swapcontext() error");
        return setcontext(&main_uc);
    }
    //CTRACE("back from yield ?");
    return me->co.rc;
}

static int
__sleep(uint64_t msec)
{
    /* first remove an old reference (if any) */
    sleepq_remove(me);

    if (msec == MRKTHR_SLEEP_FOREVER) {
        me->expire_ticks = MRKTHR_SLEEP_FOREVER;
    } else {
        if (msec == 0) {
            me->expire_ticks = 1;
        } else {
#ifdef USE_TSC
            me->expire_ticks = timecounter_now + (uint64_t)(((long double)msec / 1000.) * timecounter_freq);
#else
            me->expire_ticks = timecounter_now + msec * 1000000;
#endif
        }
    }

    //CTRACE("msec=%ld expire_ticks=%ld", msec, me->expire_ticks);

    sleepq_insert(me);

    return yield();
}

static int
__sleepticks(uint64_t ticks)
{
    /* first remove an old reference (if any) */
    sleepq_remove(me);

    if (ticks == MRKTHR_SLEEP_FOREVER) {
        me->expire_ticks = MRKTHR_SLEEP_FOREVER;
    } else {
        if (ticks == 0) {
            me->expire_ticks = 1;
        } else {
            me->expire_ticks = timecounter_now + ticks;
        }
    }

    sleepq_insert(me);

    return yield();
}

int
mrkthr_sleep(uint64_t msec)
{
    assert(me != NULL);
    /* put into sleepq(SLEEP) */
    me->co.state = CO_STATE_SLEEP;
    return __sleep(msec);
}

int
mrkthr_sleep_ticks(uint64_t ticks)
{
    assert(me != NULL);
    /* put into sleepq(SLEEP) */
    me->co.state = CO_STATE_SLEEP;
    return __sleepticks(ticks);
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
mrkthr_msec2ticks(uint64_t msec)
{
#ifdef USE_TSC
    return ((long double)msec / 1000. * (long double)timecounter_freq);
#else
    return msec * 1000000;
#endif
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
    ctx = mrkthr_vnew(name, f, argc, ap);
    va_end(ap);
    if (ctx == NULL) {
        FAIL("mrkthr_vnew");
    }
    mrkthr_run(ctx);
    return ctx;
}



static void
set_resume(mrkthr_ctx_t *ctx)
{
    assert(ctx != me);

    //mrkthr_dump(ctx);

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
    sleepq_insert(ctx);
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
        CTRACE("Will not interrupt this ctx:");
        mrkthr_dump(ctx);
        return;
    }

    /* first remove an old reference (if any) */
    sleepq_remove(ctx);

    /* clear event */
    if (ctx->co.state & (CO_STATE_READ | CO_STATE_WRITE)) {
        if (ctx->_idx0 != -1) {
            struct kevent *kev;

            kev = get_event(ctx->_idx0);
            assert(kev != NULL);
            clear_event(kev->ident, kev->filter, ctx->_idx0);
        }
    }

    /*
     * We are ignoring all event management rules here.
     */
    ctx->co.rc = CO_RC_USER_INTERRUPTED;
    ctx->co.state = CO_STATE_SET_INTERRUPT;
    ctx->expire_ticks = 1;
    sleepq_insert(ctx);
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

/**
 *
 * The kevent backend.
 *
 */

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

static struct kevent *
get_event(int idx)
{
    struct kevent *kev;
    kev = array_get(&kevents0, idx);
    return kev;
}


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

/**
 * Combined threads and events loop.
 *
 * The loop processes first threads, then events. It sleeps until the
 * earliest thread resume time, or an I/O event occurs.
 *
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
                 * proces_sleep_resume_list() that made an event expire.
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
        //TRACE("saved %d items of %ld total off from kevents0", nempty, kevents0.elnum);

        /* there are *some* events */
        nkev = kevents0.elnum - nempty;
        if (nkev != 0) {
            if (array_ensure_len(&kevents1, nkev, 0) != 0) {
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
mrkthr_signal_subscribe(mrkthr_signal_t *signal)
{
    assert(signal->owner == me);

    //CTRACE("holding on ...");
    me->co.state = CO_STATE_SIGNAL_SUBSCRIBE;
    return yield();
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
    ctx = mrkthr_vnew(name, f, argc, ap);
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

    res = __sleep(msec);

    /* now remove me from both queues */

    //CTRACE("after sleep:");
    //mrkthr_dump_sleepq();

    if (ctx->co.state == CO_STATE_DORMANT) {
        /* I had been sleeping, but by their exit I was resumed ... */

        //CTRACE("removing me:");
        //mrkthr_dump(me);

        sleepq_remove(me);

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

