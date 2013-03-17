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
 * Basic thread locking primitive, mrkthr_event_t event.
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
 *  - port to Linux epoll
 */
#include <sys/types.h>
#include <sys/param.h> /* PAGE_SIZE */
#include <sys/mman.h>
#include <sys/stdint.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/tree.h>

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "diag.h"
#include "mrkcommon/dumpm.h"
#include "mrkcommon/array.h"
#include "mrkcommon/list.h"
#include "kevent_util.h"
/* Experimental trie use */
#include "mrkcommon/trie.h"
#include "mrkthr_private.h"

typedef int (*writer_t) (int, int, int);

#define CO_FLAG_INITIALIZED 0x01
static int mflags;

static ucontext_t main_uc;
static char main_stack[SSIZE];

static int q0 = -1;
static array_t kevents0;
static array_t kevents1;

static list_t ctxes;
static mrkthr_ctx_t *me;
static array_t free_ctxes;

/*
 * Sleep list holds threads that are waiting for resume
 * in the future. It's prioritized by the thread's expire_ticks.
 */
#ifdef USE_RBT
RB_HEAD(sleepq, _mrkthr_ctx);
static struct sleepq the_sleepq = RB_INITIALIZER();
#else
static trie_t the_sleepq;
#endif

static uint64_t tsc_freq;
static uint64_t tsc_zero, tsc_now;
static uint64_t nsec_zero, nsec_now;

#ifdef USE_RBT
static int64_t sleepq_cmp(struct _mrkthr_ctx *, struct _mrkthr_ctx *);
#endif

static int mrkthr_ctx_init(mrkthr_ctx_t *);
static int mrkthr_ctx_fini(mrkthr_ctx_t *);
static struct kevent *new_event(int, int, int *);
static int co_init(struct _co *);
static int co_fini(struct _co *);
static void resume_waitq_all(array_t *);
static int discard_event(int, int);
static void push_free_ctx(mrkthr_ctx_t *);
static mrkthr_ctx_t *pop_free_ctx(void);

#ifdef USE_RBT
RB_PROTOTYPE_STATIC(sleepq, _mrkthr_ctx, sleepq_link, sleepq_cmp);
static int64_t
sleepq_cmp(struct _mrkthr_ctx *a, struct _mrkthr_ctx *b)
{
    return (int64_t)(b->expire_ticks) - (int64_t)(a->expire_ticks);
}
#endif

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
    //CTRACE("nsec_zero=%ld", nsec_zero/1000000000);
    return 0;
}

#define update_now update_now_tsc

uint64_t
mrkthr_get_now(void)
{
    return nsec_now;
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

#ifndef USE_RBT
UNUSED static int
dump_sleepq_node(trie_node_t *node, UNUSED void *udata)
{
    mrkthr_ctx_t *ctx = (mrkthr_ctx_t *)node->value;
    mrkthr_dump(ctx, NULL);
    return 0;
}
#endif

UNUSED static void
dump_sleepq()
{
#ifndef USE_RBT
    trie_traverse(&the_sleepq, dump_sleepq_node, NULL);
#else
    mrkthr_ctx_t *sle;

    RB_FOREACH(sle, sleepq, &the_sleepq) {
        mrkthr_dump(sle, NULL);
        if (sle->sleepq_bucket.head != NULL) {
            mrkthr_ctx_t *slbe;
            for (slbe = sle->sleepq_bucket.head;
                 slbe != NULL;
                 slbe = slbe->sleepq_bucket_entry.next) {
                mrkthr_dump(slbe, NULL);
            }
        }
    }
#endif
}

/* Sleep list */

#ifdef USE_RBT
RB_GENERATE_STATIC(sleepq, _mrkthr_ctx, sleepq_link, sleepq_cmp);
#endif

static void
sleepq_bucket_remove(struct _mrkthr_ctx_list *bucket, mrkthr_ctx_t *ctx)
{
    if (ctx->sleepq_bucket_entry.prev != NULL) {

        ctx->sleepq_bucket_entry.prev->sleepq_bucket_entry.next =
            ctx->sleepq_bucket_entry.next;

    } else {
        bucket->head = ctx->sleepq_bucket_entry.next;
    }

    if (ctx->sleepq_bucket_entry.next != NULL) {

        ctx->sleepq_bucket_entry.next->sleepq_bucket_entry.prev =
            ctx->sleepq_bucket_entry.prev;

    } else {
        bucket->tail = ctx->sleepq_bucket_entry.prev;
    }

    ctx->sleepq_bucket_entry.prev = NULL;
    ctx->sleepq_bucket_entry.next = NULL;
}

static void
sleepq_handle_remove(mrkthr_ctx_t *sle, mrkthr_ctx_t *ctx)
{
#ifndef USE_RBT
    trie_node_t *node;
#endif
    /*
     * ctx is either the sle itself, or it is
     * in the sle.sleepq_bucket.
     *
     * If it is the sle, and has a non-empty
     * bucket, must transfer bucket ownership to
     * the first item in the bucket.
     */
    if (sle->sleepq_bucket.head != NULL) {

        if (sle == ctx) {
            /* we are going to remove a bucket owner */

            mrkthr_ctx_t *new_bucket_owner = sle->sleepq_bucket.head;

            sleepq_bucket_remove(&sle->sleepq_bucket, new_bucket_owner);
            new_bucket_owner->sleepq_bucket = sle->sleepq_bucket;
            sle->sleepq_bucket.head = NULL;
            sle->sleepq_bucket.tail = NULL;

#ifdef USE_RBT
            RB_REMOVE(sleepq, &the_sleepq, sle);
            RB_INSERT(sleepq, &the_sleepq, new_bucket_owner);
#else
            node = trie_find_exact(&the_sleepq, sle->expire_ticks);
            assert(node != NULL && node->value == sle);
            node->value = new_bucket_owner;
#endif

        } else {
            /* we are removing from the bucket */
            sleepq_bucket_remove(&sle->sleepq_bucket, ctx);
        }

    } else {
        assert(sle == ctx);
#ifdef USE_RBT
        RB_REMOVE(sleepq, &the_sleepq, sle);
#else
        node = trie_find_exact(&the_sleepq, sle->expire_ticks);
        node->value = NULL;
        trie_node_remove(node);
#endif
    }
}

static void
sleepq_remove(mrkthr_ctx_t *ctx)
{
#ifndef USE_RBT
    trie_node_t *node;
#endif
    mrkthr_ctx_t *sle;

#ifdef USE_RBT
    if ((sle = RB_FIND(sleepq, &the_sleepq, ctx)) != NULL) {
#else
    if ((node = trie_find_exact(&the_sleepq, ctx->expire_ticks)) != NULL) {
        sle = (mrkthr_ctx_t *)(node->value);
        assert(sle != NULL);
#endif
        sleepq_handle_remove(sle, ctx);
    }
}

static void
sleepq_enqueue(mrkthr_ctx_t *ctx)
{
#ifndef USE_RBT
    trie_node_t *node;
#endif
    mrkthr_ctx_t *tmp;

    //CTRACE(FGREEN("SL enqueing"));
    //mrkthr_dump(ctx, NULL);

#ifdef USE_RBT
    if ((tmp = RB_INSERT(sleepq, &the_sleepq, ctx)) != NULL) {
#else
    if ((node = trie_add_node(&the_sleepq, ctx->expire_ticks)) == NULL) {
        FAIL("trie_add_node");
    }
    tmp = (mrkthr_ctx_t *)(node->value);
    if (tmp != NULL) {
#endif
        //TRACE("while enqueing, found bucket:");
        //mrkthr_dump(tmp, NULL);

        if (tmp->sleepq_bucket.tail == NULL) {
            tmp->sleepq_bucket.head = ctx;
            tmp->sleepq_bucket.tail = ctx;
            ctx->sleepq_bucket_entry.prev = NULL;
            ctx->sleepq_bucket_entry.next = NULL;
        } else {
            //TRACE("tmp=%p", tmp);
            //mrkthr_dump(tmp, NULL);
            //D8(tmp->sleepq_bucket.tail, 1024);
            //TRACE("tmp->sleepq_bucket.tail=%p", tmp->sleepq_bucket.tail);
            tmp->sleepq_bucket.tail->sleepq_bucket_entry.next = ctx;
            ctx->sleepq_bucket_entry.prev = tmp->sleepq_bucket.tail;
            ctx->sleepq_bucket_entry.next = NULL;
            tmp->sleepq_bucket.tail = ctx;
        }

        //TRACE("After adding to the bucket:");
        //mrkthr_dump(tmp, NULL);
    }
#ifndef USE_RBT
    else {
        node->value = ctx;
    }
#endif
}

/*
 * Module init/fini
 */
int
mrkthr_init(void)
{
    size_t sz;

    if (array_init(&free_ctxes, sizeof(mrkthr_ctx_t *), 0,
                  NULL, NULL) != 0) {
        FAIL("array_init");
    }

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
#ifdef USE_RBT
    RB_INIT(&the_sleepq);
#else
    trie_init(&the_sleepq);
#endif
    mflags |= CO_FLAG_INITIALIZED;

    sz = sizeof(tsc_freq);
    if (sysctlbyname("machdep.tsc_freq", &tsc_freq, &sz, NULL, 0) != 0) {
        FAIL("sysctlbyname");
    }

    if (wallclock_init() != 0) {
        FAIL("wallclock_init");
    }

    return 0;
}

int
mrkthr_fini(void)
{
    me = NULL;
    mflags &= ~CO_FLAG_INITIALIZED;
    array_fini(&kevents0);
    array_fini(&kevents1);
    list_fini(&ctxes);
    array_fini(&free_ctxes);
    close(q0);
    return 0;
}

/*
 * mrkthr_ctx management
 */
static int
co_init(struct _co *co)
{
    /* leave co->uc, co->stack for future use */
    co->id = -1;
    *co->name = '\0';
    co->f = NULL;
    co->argc = 0;
    co->argv = NULL;
    co->state = CO_STATE_DORMANT;
    co->rc = 0;
    return 0;
}

static int
co_fini(struct _co *co)
{
    /* leave co->uc, co->stack for future use */
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
    return 0;
}

static int
mrkthr_ctx_init(mrkthr_ctx_t *ctx)
{
    ctx->co.stack = MAP_FAILED;
    ctx->co.uc.uc_link = NULL;
    ctx->co.uc.uc_stack.ss_sp = NULL;
    ctx->co.uc.uc_stack.ss_size = 0;
    //sigfillset(&ctx->co.uc.uc_sigmask);
    co_init(&ctx->co);
    ctx->sleepq_bucket.head = NULL;
    ctx->sleepq_bucket.tail = NULL;
    ctx->sleepq_bucket_entry.prev = NULL;
    ctx->sleepq_bucket_entry.next = NULL;
    ctx->expire_ticks = 0;
    if (array_init(&ctx->waitq, sizeof(mrkthr_ctx_t *), 0,
                   NULL, NULL) != 0) {
        FAIL("array_init");
    }
    ctx->_idx0 = -1;
    return 0;
}

static int
mrkthr_ctx_fini(mrkthr_ctx_t *ctx)
{
    if (ctx->co.stack != MAP_FAILED) {
        munmap(ctx->co.stack, SSIZE);
        ctx->co.stack = MAP_FAILED;
    }
    ctx->co.uc.uc_link = NULL;
    ctx->co.uc.uc_stack.ss_sp = NULL;
    ctx->co.uc.uc_stack.ss_size = 0;
    co_fini(&ctx->co);
    push_free_ctx(ctx);
    sleepq_remove(ctx);
    resume_waitq_all(&ctx->waitq);
    array_fini(&ctx->waitq);
    ctx->_idx0 = -1;
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
    mrkthr_ctx_t **pctx = NULL;
    if ((pctx = array_incr(&free_ctxes)) == NULL) {
        FAIL("array_incr");
    } else {
        *pctx = ctx;
    }

}

static mrkthr_ctx_t *
pop_free_ctx(void)
{
    mrkthr_ctx_t *ctx = NULL;
    mrkthr_ctx_t **pctx = NULL;
    array_iter_t ait;

    if ((pctx = array_last(&free_ctxes, &ait)) != NULL) {
        ctx = *pctx;
        if (array_decr(&free_ctxes) != 0) {
            FAIL("array_decr");
        }
    } else {
        if ((ctx = list_incr(&ctxes)) == NULL) {
            FAIL("list_incr");
        }
    }
    return ctx;
}

/**
 * Return a new mrkthr_ctx_t instance. The new instance doesn't have to
 * be freed, and should be treated as an opaque object. It's internally
 * reclaimed as soon as the worker function returns.
 */
mrkthr_ctx_t *
mrkthr_new(const char *name, cofunc f, int argc, ...)
{
    int i;
    list_iter_t it;
    va_list ap;
    mrkthr_ctx_t *ctx = NULL;


    assert(mflags & CO_FLAG_INITIALIZED);
    ctx = pop_free_ctx();

    assert(ctx!= NULL);
    assert(ctx->co.id == -1);
    
    /* Thread id is actually an index into the ctxes list */
    ctx->co.id = it.iter;

    if (name != NULL) {
        strncpy(ctx->co.name, name, sizeof(ctx->co.name));
    } else {
        ctx->co.name[0] = '\0';
    }

    if (ctx->co.stack == MAP_FAILED) {
        if ((ctx->co.stack = mmap(NULL, SSIZE, PROT_READ|PROT_WRITE,
                                  MAP_ANON, -1, 0)) == MAP_FAILED) {
            TRRETNULL(MRKTHR_CTX_NEW + 1);
        }
        if (mprotect(ctx->co.stack, PAGE_SIZE, PROT_NONE) != 0) {
            FAIL("mprotect");
        }
        ctx->co.uc.uc_stack.ss_sp = ctx->co.stack;
        ctx->co.uc.uc_stack.ss_size = SSIZE;
    }

    ctx->co.uc.uc_link = &main_uc;

    ctx->co.f = f;

    if (argc > 0) {
        ctx->co.argc = argc;
        if ((ctx->co.argv = malloc(sizeof(void *) * ctx->co.argc)) == NULL) {
            FAIL("malloc");
        }
        va_start(ap, argc);
        for (i = 0; i < ctx->co.argc; ++i) {
            ctx->co.argv[i] = va_arg(ap, void *);
            //CTRACE("ctx->co.argv[%d]=%p", i, ctx->co.argv[i]);
        }
        va_end(ap);
    }

    if (_getcontext(&ctx->co.uc) != 0) {
        TRRETNULL(MRKTHR_CTX_NEW + 2);
    }
    makecontext(&ctx->co.uc, (void(*)(void))f, 2, ctx->co.argc, ctx->co.argv);

    return ctx;
}

int
mrkthr_dump(const mrkthr_ctx_t *ctx, UNUSED void *udata)
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
           ctx->co.state == CO_STATE_EVENT_READ ? "EVENT_READ" :
           ctx->co.state == CO_STATE_EVENT_WRITE ? "EVENT_WRITE" :
           ctx->co.state == CO_STATE_EVENT_SLEEP ? "EVENT_SLEEP" :
           ctx->co.state == CO_STATE_EVENT_ACQUIRE ? "EVENT_ACQUIRE" :
           ctx->co.state == CO_STATE_EVENT_JOINWAITQ ? "EVENT_JOINWAITQ" :
           ctx->co.state == CO_STATE_EVENT_RESUME ? "EVENT_RESUME" :
           ctx->co.state == CO_STATE_EVENT_INTERRUPT ? "EVENT_INTERRUPT" :
           "",
           ctx->co.rc == CO_RC_USER_INTERRUPTED ? "USER_INTERRUPTED" : "OK",
           ctx->expire_ticks
    );

    uc = ctx->co.uc;
    //dump_ucontext(&uc);
    if (ctx->sleepq_bucket.head != NULL) {
        TRACE("Bucket:");
        for (tmp = ctx->sleepq_bucket.head;
             tmp != NULL;
             tmp = tmp->sleepq_bucket_entry.next) {
            mrkthr_dump(tmp, NULL);
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
const mrkthr_ctx_t *
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
    //mrkthr_dump(me, NULL);
    res = swapcontext(&me->co.uc, &main_uc);
    if(res != 0) {
        CTRACE("swapcontext() error");
        return setcontext(&main_uc);
    }
    //CTRACE("back from yield ?");
    return me->co.rc;
}

int
mrkthr_sleep(uint64_t msec)
{
    assert(me != NULL);

    me->co.state = CO_STATE_EVENT_SLEEP;

    if (msec == mrkthr_SLEEP_FOREVER) {
        me->expire_ticks = mrkthr_SLEEP_FOREVER;
    } else {
        if (msec == 0) {
            me->expire_ticks = 0;
        } else {
            me->expire_ticks = tsc_now + (uint64_t)(((long double)msec / 1000.) * tsc_freq);
        }
    }

    //CTRACE("msec=%ld expire_ticks=%ld", msec, me->expire_ticks);

    sleepq_enqueue(me);

    return yield();
}

long double
mrkthr_ticks2sec(uint64_t ticks)
{
    return (long double)ticks / (long double)tsc_freq;
}


/**
 * Sleep until the target ctx has exited.
 */
static int
join_waitq(array_t *waitq)
{
    mrkthr_ctx_t **t;
    array_iter_t it;

    assert(me != NULL);

    /* find first empty slot */
    for (t = array_first(waitq, &it);
         t != NULL;
         t = array_next(waitq, &it)) {

        if (*t == MAP_FAILED) {
            break;
        }
    }

    if (t == NULL) {
        if ((t = array_incr(waitq)) == NULL) {
            FAIL("array_incr");
        }
    }

    *t = me;
    me->co.state = CO_STATE_EVENT_JOINWAITQ;
    return yield();
}

static void
resume_waitq_all(array_t *waitq)
{
    mrkthr_ctx_t **t;
    array_iter_t it;

    for (t = array_first(waitq, &it);
         t != NULL;
         t = array_next(waitq, &it)) {

        if (*t != MAP_FAILED) {
            mrkthr_set_resume(*t);
            *t = MAP_FAILED;
        }
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
    //mrkthr_dump(ctx, NULL);

    /*
     * Can only be the result of yield or start, ie, the state cannot be
     * dormant or resumed.
     */
    if (!(ctx->co.state & CO_STATE_RESUMABLE)) {
        /* This is an error (currently no reason is known, though) */
        resume_waitq_all(&ctx->waitq);
        co_fini(&ctx->co);
        /* not sure if we can push it here ... */
        push_free_ctx(ctx);
        TRRET(RESUME + 1);
    }

    ctx->co.state = CO_STATE_RESUMED;

    me = ctx;

    res = swapcontext(&main_uc, &me->co.uc);
    //CTRACE("swapcontext returned %d (yield was called?)", res);

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
        //mrkthr_dump(ctx, NULL);
        resume_waitq_all(&ctx->waitq);
        co_fini(&ctx->co);
        push_free_ctx(ctx);
        //TRRET(RESUME + 2);
        return RESUME + 2;

    } else {
        CTRACE("Unknown case:");
        mrkthr_dump(ctx, NULL);
        FAIL("resume");
    }

    return res;
}

void
mrkthr_set_resume(mrkthr_ctx_t *ctx)
{
    assert(ctx != me);

    //mrkthr_dump(ctx, NULL);

    //assert(ctx->co.f != NULL);
    if (ctx->co.f == NULL) {
        CTRACE("Will not resume this ctx:");
        mrkthr_dump(ctx, NULL);
        return;
    }

    ctx->co.state = CO_STATE_EVENT_RESUME;
    ctx->expire_ticks = 0;
    //ctx->expire_ticks = tsc_now;
    sleepq_enqueue(ctx);
}

/**
 * Send an interrupt signal to the thread.
 */
void
mrkthr_set_interrupt(mrkthr_ctx_t *ctx)
{
    assert(ctx != me);

    //mrkthr_dump(ctx, NULL);

    //assert(ctx->co.f != NULL);
    if (ctx->co.f == NULL) {
        CTRACE("Will not interrupt this ctx:");
        mrkthr_dump(ctx, NULL);
        return;
    }

    ctx->co.rc = CO_RC_USER_INTERRUPTED;
    ctx->expire_ticks = 0;
    //ctx->expire_ticks = tsc_now;
    sleepq_enqueue(ctx);
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
process_sleep_resume_list(void)
{
#ifndef USE_RBT
    trie_node_t *node;
#endif
    mrkthr_ctx_t *ctx;

    /* schedule expired mrkthrs */

#ifdef USE_RBT
    for (ctx = RB_MIN(sleepq, &the_sleepq);
         ctx != NULL;
         ctx = RB_MIN(sleepq, &the_sleepq)) {
#else
    for (node = TRIE_MIN(&the_sleepq);
         node != NULL;
         node = TRIE_MIN(&the_sleepq)) {

        ctx = (mrkthr_ctx_t *)(node->value);
        assert(ctx != NULL);
#endif

        //TRACE("Dump sleepq");
        //dump_sleepq();

        update_now();

        //CTRACE(FBBLUE("Processing: delta=%ld (%Lf)"), ctx->expire_ticks - tsc_now, mrkthr_ticks2sec(tsc_now - ctx->expire_ticks));
        //mrkthr_dump(ctx, NULL);

        if (ctx->expire_ticks < tsc_now) {

            /* dequeue it as early as here */
#ifdef USE_RBT
            RB_REMOVE(sleepq, &the_sleepq, ctx);
#else
            trie_node_remove(node);
            node = NULL;
#endif

            /*
             * Process bucket, must do it *BEFORE* we process
             * the bucket owner
             */

            while (ctx->sleepq_bucket.head != NULL) {
                mrkthr_ctx_t *tmp = ctx->sleepq_bucket.head;

                //CTRACE(FBGREEN("Resuming expired thread (bucket)"));
                //mrkthr_dump(tmp, NULL);

                sleepq_bucket_remove(&ctx->sleepq_bucket, tmp);

                if (resume(tmp) != 0) {
                    //TRACE("Could not resume co %d, discarding ...",
                    //      ctx->co.id);
                }
            }

            /* Finally process bucket owner */
            //CTRACE(FBGREEN("Resuming expired bucket owner"));
            //mrkthr_dump(ctx, NULL);

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
#ifndef USE_RBT
    trie_node_t *node;
#endif
    mrkthr_ctx_t *ctx = NULL;
    array_iter_t it;

    update_now();

    while (1) {
        //sleep(1);

        //TRACE(FRED("Processing sleep/resume lists ..."));

        /* this will make sure there are no expired ctxes in the sleepq */
        process_sleep_resume_list();

        /* get the first to wake sleeping mrkthr */
#ifdef USE_RBT
        if ((ctx = RB_MIN(sleepq, &the_sleepq)) != NULL) {
#else
        if ((node = TRIE_MIN(&the_sleepq)) != NULL) {
            ctx = node->value;
            assert(ctx != NULL);
#endif
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
                    //mrkthr_dump(ctx, NULL);


                    if (ctx != NULL) {

                        /* only clear_event() makes use of it */
                        ctx->_idx0 = -1;

                        switch (kev->filter) {

                        case EVFILT_READ:

                            if (ctx->co.f != NULL) {

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

                    //XXX moved to process_sleep_resume_list()
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
                    //XXX moved to process_sleep_resume_list()
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
        me->co.state = CO_STATE_EVENT_READ;
        res = yield();
        //CTRACE("yield returned %d", res);
        if (res != 0) {
            return -1;
        }

        if ((kev = result_event(fd, EVFILT_READ)) != NULL) {
            return (ssize_t)(kev->data);
        }
    }
//    CTRACE("after---");
//    KEVENT_DUMP(kev);
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

    //CTRACE("o=%ld n=%ld", *offset, navail);

    if (navail == 0) {
        /* EOF ? */
        TRRET(MRKTHR_ACCEPT_ALL + 2);
    }
    while (nread < navail) {
        tmp = *buf + (*offset + nread);
        tmp->addrlen = sizeof(union _mrkthr_addr);
        //CTRACE("nread=%ld", nread);
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
    //CTRACE("Returning %ld", *offset);

    return 0;
}

/**
 * Allocate enough space in *buf beyond *offset for a single read
 * operation and read into that location from ctx->in.
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

    //CTRACE("o=%ld n=%ld", *offset, navail);

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
 * Perform a single read from ctx->in into buf.
 * Return the number of bytes read or -1 in case of error.
 */
ssize_t
mrkthr_read_allb(int fd, char *buf)
{
    ssize_t navail;
    ssize_t nread;

    assert(me != NULL);

    if ((navail = mrkthr_get_rbuflen(fd)) <= 0) {
        return -1;
    }

    if ((nread = read(fd, buf, navail)) == -1) {
        perror("read");
        return -1;
    }

    if (nread < navail) {
        TRACE("nread=%ld navail=%ld", nread, navail);
        if (nread == 0) {
            return -1;
        }
    }
    return nread;
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
        me->co.state = CO_STATE_EVENT_WRITE;
        res = yield();
        //CTRACE("yield returned %d", res);
        if (res != 0) {
            return -1;
        }

        if ((kev = result_event(fd, EVFILT_WRITE)) != NULL) {
            return (ssize_t)(kev->data);
        }
        //TRACE("after---");
        //KEVENT_DUMP(kev);
    }
    return -1;
}

/**
 * Write buflen bytes from buf into ctx->fd.
 */
int
mrkthr_write_all(int fd, const char *buf, size_t buflen)
{
    ssize_t navail;
    ssize_t nwritten;
    off_t remaining = buflen;

    assert(me != NULL);
    //CTRACE("About ro write");
    //D16(buf, buflen);

    while (remaining > 0) {
        if ((navail = mrkthr_get_wbuflen(fd)) <= 0) {
            TRRET(MRKTHR_WRITE_ALL + 1);
        }
        //CTRACE("navail=%ld", navail);

        if ((nwritten = write(fd, buf + buflen - remaining,
                              MIN(navail, remaining))) == -1) {

            TRRET(MRKTHR_WRITE_ALL + 2);
        }
        //CTRACE("remaining=%ld nwritten=%ld", remaining, nwritten);
        remaining -= nwritten;
    }
    //CTRACE("Wrote all, exiting.");
    return 0;
}

/**
 * Event Primitive.
 */
int
mrkthr_event_init(mrkthr_event_t *event, mrkthr_ctx_t *ctx)
{
    event->owner = ctx;
    return 0;
}

int
mrkthr_event_acquire(UNUSED mrkthr_event_t *event)
{
    assert(event->owner == me);

    //CTRACE("holding on ...");
    me->co.state = CO_STATE_EVENT_ACQUIRE;
    return yield();
}

void
mrkthr_event_release(mrkthr_event_t *event)
{
    //CTRACE("event->owner=%p", event->owner);
    if (event->owner != NULL) {
        mrkthr_set_resume(event->owner);
        return;
    }
    //CTRACE("Not resuming orphan event. See you next time.");
}

/**
 * Condition Variable Primitive.
 */
int
mrkthr_cond_init(mrkthr_cond_t *cond)
{
    if (array_init(&cond->waitq, sizeof(mrkthr_ctx_t *), 0,
                   NULL, NULL) != 0) {
        FAIL("array_init");
    }
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

int
mrkthr_cond_fini(mrkthr_cond_t *cond)
{
    mrkthr_cond_signal_all(cond);
    array_fini(&cond->waitq);
    return 0;
}

