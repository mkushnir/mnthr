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
/* Turn off TRACE from dumpm.h */
#ifdef NDEBUG
#   ifdef TRACE
#      undef TRACE
#   endif
#   define TRACE(s, ...)
#endif
#include <mrkcommon/memdebug.h>
MEMDEBUG_DECLARE(mrkthr);

#include <mrkcommon/array.h>
#include <mrkcommon/list.h>
#include "mrkcommon/dtqueue.h"
#include "mrkcommon/stqueue.h"
#include <kevent_util.h>
/* Experimental trie use */
#include <mrkcommon/trie.h>
#include "mrkthr_private.h"

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

static uint64_t tsc_freq;
static uint64_t tsc_zero, tsc_now;
static uint64_t nsec_zero, nsec_now;

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

static inline uint64_t
rdtsc(void)
{
  uint32_t lo, hi;

  __asm __volatile ("rdtsc" : "=a"(lo), "=d"(hi));
  return (uint64_t) hi << 32 | lo;
}

static void
update_now_tsc(void)
{
    tsc_now = rdtsc();
    /* do it here so that get_now() returns precomputed value */
    nsec_now = nsec_zero + (uint64_t)(((long double)(tsc_now - tsc_zero)) /
        ((long double)(tsc_freq)) * 1000000000.);
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

    tsc_zero = rdtsc();

    if (clock_gettime(CLOCK_REALTIME_PRECISE, &ts) != 0) {
        TRRET(WALLCLOCK_INIT + 1);
    }
    nsec_zero = ts.tv_nsec + ts.tv_sec * 1000000000;
    return 0;
}

#define update_now update_now_tsc

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
    return tsc_now;
}

uint64_t
mrkthr_get_now_ticks_precise(void)
{
    update_now();
    return tsc_now;
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
dump_sleepq_node(trie_node_t *node, UNUSED void *udata)
{
    mrkthr_ctx_t *ctx = (mrkthr_ctx_t *)node->value;
    if (ctx != NULL) {
        mrkthr_dump(ctx);
    }
    return 0;
}

void
mrkthr_dump_sleepq(void)
{
    trie_traverse(&the_sleepq, dump_sleepq_node, NULL);
}

/* Sleep list */

static void
sleepq_remove(mrkthr_ctx_t *ctx)
{
    trie_node_t *trn;

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
                /* we are going to remove a sleepq bucket host */

                DTQUEUE_DEQUEUE(&sle->sleepq_bucket, sleepq_link);

                bucket_host_pretendent->sleepq_bucket = sle->sleepq_bucket;

                DTQUEUE_FINI(&sle->sleepq_bucket);

                trn->value = bucket_host_pretendent;

            } else {
                /* we are removing from the bucket */
                if (!DTQUEUE_ORPHAN(&sle->sleepq_bucket, sleepq_link, ctx)) {
                    DTQUEUE_REMOVE(&sle->sleepq_bucket, sleepq_link, ctx);
                }
            }

        } else {
            //assert(sle == ctx);
            if (sle != ctx) {
                /*
                 * A special case of a dead thread that was not happened
                 * to be in the sleepq. Otherwise, if the thread is
                 * resumable, we are in trouble.
                 */
                if (ctx->co.state & CO_STATE_RESUMABLE) {
                    CTRACE("sle:");
                    mrkthr_dump(sle);
                    CTRACE("ctx:");
                    mrkthr_dump(ctx);
                    assert(0);
                }
            } else {
                trn->value = NULL;
                trie_remove_node(&the_sleepq, trn);
            }
        }
    }
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
}

/*
 * Module init/fini
 */
int
mrkthr_init(void)
{
    size_t sz;

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
    sz = sizeof(tsc_freq);
    if (sysctlbyname("machdep.tsc_freq", &tsc_freq, &sz, NULL, 0) != 0) {
        FAIL("sysctlbyname");
    }

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
    return trie_get_nelems(&the_sleepq);
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

    co_fini_other(&ctx->co);

    /* resume all from my waitq */
    resume_waitq_all(&ctx->waitq);
    DTQUEUE_FINI(&ctx->waitq);

    /* remove me from someone else's waitq */
    if (ctx->hosting_waitq != NULL) {
        DTQUEUE_REMOVE(ctx->hosting_waitq, waitq_link, ctx);
        ctx->hosting_waitq = NULL;
        DTQUEUE_ENTRY_FINI(waitq_link, ctx);
    }

    /* remove from sleepq */

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

    CTRACE("mrkthr_ctx: co.name='%s' co.id=%d co.f=%p co.state=%s "
           "co.rc=%s expire_ticks=%016lx",
           ctx->co.name,
           ctx->co.id,
           ctx->co.f,
           ctx->co.state == CO_STATE_DORMANT ? "DORMANT" :
           ctx->co.state == CO_STATE_RESUMED ? "RESUMED" :
           ctx->co.state == CO_STATE_READ ? "READ" :
           ctx->co.state == CO_STATE_WRITE ? "WRITE" :
           ctx->co.state == CO_STATE_SLEEP ? "SLEEP" :
           ctx->co.state == CO_STATE_SET_RESUME ? "SET_RESUME" :
           ctx->co.state == CO_STATE_SIGNAL_SUBSCRIBE ? "SNGNAL_SUBSCRIBE" :
           ctx->co.state == CO_STATE_JOINWAITQ ? "JOINWAITQ" :
           ctx->co.state == CO_STATE_WAITFOR ? "WAITFOR" :
           "",
           ctx->co.rc == CO_RC_USER_INTERRUPTED ? "USER_INTERRUPTED" :
           ctx->co.rc == CO_RC_TIMEDOUT ? "TIMEDOUT" :
           "OK",
           ctx->expire_ticks
    );

    uc = ctx->co.uc;
    //dump_ucontext(&uc);
    if (DTQUEUE_HEAD(&ctx->sleepq_bucket) != NULL) {
        TRACE("Bucket:");
        for (tmp = DTQUEUE_HEAD(&ctx->sleepq_bucket);
             tmp != NULL;
             tmp = DTQUEUE_NEXT(sleepq_link, tmp)) {
            mrkthr_dump(tmp);
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
            me->expire_ticks = 0;
        } else {
            me->expire_ticks = tsc_now + (uint64_t)(((long double)msec / 1000.) * tsc_freq);
        }
    }

    //CTRACE("msec=%ld expire_ticks=%ld", msec, me->expire_ticks);

    sleepq_insert(me);

    return yield();
}

int
mrkthr_sleep(uint64_t msec)
{
    assert(me != NULL);
    me->co.state = CO_STATE_SLEEP;
    return __sleep(msec);
}

long double
mrkthr_ticks2sec(uint64_t ticks)
{
    return (long double)ticks / (long double)tsc_freq;
}


static void
append_me_to_waitq(mrkthr_waitq_t *waitq)
{
    assert(me != NULL);
    if (me->hosting_waitq != NULL) {
        DTQUEUE_REMOVE(me->hosting_waitq, waitq_link, me);
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
    me->co.state = CO_STATE_JOINWAITQ;
    return yield();
}

static void
resume_waitq_all(mrkthr_waitq_t *waitq)
{
    mrkthr_ctx_t *t;

    while ((t = DTQUEUE_HEAD(waitq)) != NULL) {
        assert(t->hosting_waitq = waitq);
        DTQUEUE_DEQUEUE(waitq, waitq_link);
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
        t->hosting_waitq = NULL;
        set_resume(t);
    }
}


int
mrkthr_join(mrkthr_ctx_t *ctx)
{
    if (!(ctx->co.state & CO_STATE_RESUMABLE)) {
        /* dormant thread, or an attempt to join self ? */
        return CO_RC_JOIN_INVALID;
    }
    return join_waitq(&ctx->waitq);
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

    ctx->co.yield_state = ctx->co.state;
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

    ctx->co.state = CO_STATE_SET_RESUME;
    ctx->expire_ticks = 0;
    sleepq_insert(ctx);
}

/**
 * Send an interrupt signal to the thread.
 */
void
mrkthr_set_interrupt(mrkthr_ctx_t *ctx)
{
    assert(ctx != me);

    //mrkthr_dump(ctx);

    //assert(ctx->co.f != NULL);
    if (ctx->co.f == NULL) {
        CTRACE("Will not interrupt this ctx:");
        mrkthr_dump(ctx);
        return;
    }

    /*
     * We are ignoring all event management rules here.
     */
    ctx->co.rc = CO_RC_USER_INTERRUPTED;
    ctx->expire_ticks = 0;
    sleepq_insert(ctx);
}

/**
 *
 * The kevent backend.
 *
 */

/**
 * Remove an event from the kevents array.
 */
static int
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
        return 0;
    }

    for (kev = array_first(&kevents0, &it);
         kev != NULL;
         kev = array_next(&kevents0, &it)) {

        if (kev->ident == (uintptr_t)fd && kev->filter == filter) {
            //CTRACE("SLOW");
            //KEVENT_DUMP(kev);
            kevent_init(kev);
            /* early return, assume fd/filter is unique in the kevents0 */
            return 0;
        }
    }
    //CTRACE("NOTFOUND");
    //KEVENT_DUMP(kev);
    return 0;
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

static void
sift_sleepq(void)
{
    trie_node_t *trn;
    mrkthr_ctx_t *ctx;

    /* schedule expired mrkthrs */

    for (trn = TRIE_MIN(&the_sleepq);
         trn != NULL;
         trn = TRIE_MIN(&the_sleepq)) {

        ctx = (mrkthr_ctx_t *)(trn->value);
        assert(ctx != NULL);

        //TRACE("Dump sleepq");
        //dump_sleepq();

        update_now();

        //CTRACE(FBBLUE("Processing: delta=%ld (%Lf)"),
        //       ctx->expire_ticks - tsc_now,
        //       mrkthr_ticks2sec(tsc_now - ctx->expire_ticks));
        //mrkthr_dump(ctx);

        if (ctx->expire_ticks < tsc_now) {
            mrkthr_ctx_t *bctx;

            /* remove it as early as here */
            trie_remove_node(&the_sleepq, trn);
            trn = NULL;

            /*
             * Process bucket, must do it *BEFORE* we process
             * the bucket owner
             */

            while ((bctx = DTQUEUE_HEAD(&ctx->sleepq_bucket)) != NULL) {

                //CTRACE(FBGREEN("Resuming expired thread (bucket)"));
                //mrkthr_dump(bctx);

                DTQUEUE_DEQUEUE(&ctx->sleepq_bucket, sleepq_link);

                if (!(bctx->co.state & CO_STATES_RESUMABLE_EXTERNALLY)) {
                    /*
                     * We cannot resume events here that can only be
                     * resumed from within other places of mrkthr_loop().
                     *
                     * All other events not included here are
                     * CO_STATE_READ and CO_STATE_WRITE. This
                     * should never occur.
                     */
                    TRACE(FRED("Have to deliver a %s event "
                               "to co.id=%d that was not scheduled for!"),
                               CO_STATE_STR(bctx->co.state),
                               bctx->co.id);
                    mrkthr_dump(bctx);
                }

                if (resume(bctx) != 0) {
                    //TRACE("Could not resume co %d, discarding ...",
                    //      ctx->co.id);
                }
            }

            /* Finally process bucket owner */
            //CTRACE(FBGREEN("Resuming expired bucket owner"));
            //mrkthr_dump(ctx);

            if (resume(ctx) != 0) {
                //TRACE("Could not resume co %d, discarding ...",
                //      ctx->co.id);
            }

        } else {
            break;
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
    //lldiv_t div;
    long double secs, isecs, nsecs;
    int nempty, nkev;
    trie_node_t *node;
    mrkthr_ctx_t *ctx = NULL;
    array_iter_t it;

    update_now();

    while (!(mflags & CO_FLAG_SHUTDOWN)) {
        //sleep(1);

        //TRACE(FRED("Sifting sleepq ..."));

        /* this will make sure there are no expired ctxes in the sleepq */
        sift_sleepq();

        /* get the first to wake sleeping mrkthr */
        if ((node = TRIE_MIN(&the_sleepq)) != NULL) {
            ctx = node->value;
            assert(ctx != NULL);
            //assert(ctx != NULL);

            if (ctx->expire_ticks > tsc_now) {
                secs = (long double)(ctx->expire_ticks - tsc_now) / tsc_freq;
                nsecs = modfl(secs, &isecs);
                //CTRACE("secs=%Lf isecs=%Lf nsecs=%Lf", secs, isecs, nsecs);
                timeout.tv_sec = isecs;
                timeout.tv_nsec = nsecs * 1000000000;
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

        //TRACE(FRED("nsec_now=%ld tmout=%ld(%ld.%ld) loop..."),
        //      nsec_now,
        //      tmout != NULL ?
        //          tmout->tv_nsec + tmout->tv_sec * 1000000000 : -1,
        //      tmout != NULL ? tmout->tv_sec : -1,
        //      tmout != NULL ? tmout->tv_nsec : -1);
        //array_traverse(&kevents0, (array_traverser_t)mrkthr_dump, NULL);

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

            //TRACE("kevent returned %d", kevres);
            //array_traverse(&kevents1, (array_traverser_t)mrkthr_dump);

            if (kevres == 0) {
                //TRACE("Nothing to process ...");
                if (tmout != NULL) {
                    //TRACE("Timed out.");
                    continue;
                } else {
                    //TRACE("No events, exiting.");
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
                        //TRACE("Error condition for FD %08lx, (%s) skipping ...",
                        //      kev->ident, strerror(kev->data));
                        continue;
                    }

                    //CTRACE("Processing:");
                    //mrkthr_dump(ctx);


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

                    //XXX moved to sift_sleepq()
                    update_now();

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
                    //TRACE("tmout was zero, no nanosleep.");
                    //XXX moved to sift_sleepq()
                    update_now();
                }

                continue;

            } else {
                //TRACE("Nothing to pass to kevent(), breaking the loop ? ...");
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
        TRACE("nread=%ld navail=%ld", nread, navail);
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
        TRACE("nread=%ld navail=%ld", nread, navail);
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
        TRACE("nread=%ld sz=%ld", nread, sz);
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
        TRACE("nrecv=%ld sz=%ld", nrecv, sz);
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
int
mrkthr_signal_init(mrkthr_signal_t *signal, mrkthr_ctx_t *ctx)
{
    signal->owner = ctx;
    return 0;
}

int
mrkthr_signal_fini(mrkthr_signal_t *signal)
{
    signal->owner = NULL;
    return 0;
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

void
mrkthr_signal_send(mrkthr_signal_t *signal)
{
    //CTRACE("signal->owner=%p", signal->owner);
    if (signal->owner != NULL) {

        if (signal->owner->co.state == CO_STATE_SIGNAL_SUBSCRIBE) {
            set_resume(signal->owner);
            return;

        } else {
            CTRACE("Attempt to send signal for thread %d in %s "
                   "state (ignored)",
                   signal->owner->co.id,
                   CO_STATE_STR(signal->owner->co.state));
        }
    }
    //CTRACE("Not resuming orphan signal. See you next time.");
}

/**
 * Condition Variable Primitive.
 */
int
mrkthr_cond_init(mrkthr_cond_t *cond)
{
    DTQUEUE_INIT(&cond->waitq);
    return 0;
}

int
mrkthr_cond_wait(mrkthr_cond_t *cond)
{
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

int
mrkthr_cond_fini(mrkthr_cond_t *cond)
{
    mrkthr_cond_signal_all(cond);
    DTQUEUE_FINI(&cond->waitq);
    return 0;
}

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

    append_me_to_waitq(&ctx->waitq);
    set_resume(ctx);
    me->co.state = CO_STATE_WAITFOR;
    res = __sleep(msec);

    if (me->co.yield_state == CO_STATE_WAITFOR) {
        ctx->co.rc = CO_RC_TIMEDOUT;
        res = MRKTHR_WAIT_TIMEOUT;
    }

    remove_me_from_waitq(&ctx->waitq);
    sleepq_remove(ctx);
    mrkthr_ctx_finalize(ctx);
    push_free_ctx(ctx);

    return res;
}

