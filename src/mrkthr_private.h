#ifndef MRKTHR_PRIVATE_H
#define MRKTHR_PRIVATE_H

#include <sys/param.h> /* PAGE_SIZE */
#include <sys/limits.h> /* ULONG_MAX*/
#include <sys/socket.h>
#include <sys/types.h>

#include <netinet/in.h>
#include <ucontext.h>

#include "mrkcommon/dtqueue.h"
#include "mrkcommon/stqueue.h"
#include "mrkcommon/rbt.h"
#include "mrkcommon/util.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STACKSIZE (PAGE_SIZE * 1024)

struct _mrkthr_ctx;

struct _mrkthr_ctx_list {
    struct _mrkthr_ctx *head;
    struct _mrkthr_ctx *tail;
};

struct _mrkthr_ctx_list_entry {
    struct _mrkthr_ctx *prev;
    struct _mrkthr_ctx *next;
};

typedef DTQUEUE(_mrkthr_ctx, mrkthr_waitq_t);
#define MRKTHR_WAITQ_T_DEFINED

struct _mrkthr_ctx {
    struct _co {
        ucontext_t uc;
        char *stack;
        int id;
        char name[8];
        int (*f)(int, void *[]);
        int argc;
        void **argv;
#       define CO_STATE_DORMANT 0x01
#       define CO_STATE_RESUMED 0x02
#       define CO_STATE_READ 0x04
#       define CO_STATE_WRITE 0x08
#       define CO_STATE_SLEEP 0x10
#       define CO_STATE_SET_RESUME 0x20
#       define CO_STATE_SET_INTERRUPT 0x40
#       define CO_STATE_SIGNAL_SUBSCRIBE 0x80
#       define CO_STATE_JOIN 0x100
#       define CO_STATE_JOIN_INTERRUPTED 0x200
#       define CO_STATE_CONDWAIT 0x400
#       define CO_STATE_WAITFOR 0x800

#       define CO_STATES_RESUMABLE_EXTERNALLY (CO_STATE_SLEEP |                \
                                               CO_STATE_SET_RESUME |           \
                                               CO_STATE_SET_INTERRUPT |        \
                                               CO_STATE_SIGNAL_SUBSCRIBE |     \
                                               CO_STATE_JOIN |                 \
                                               CO_STATE_JOIN_INTERRUPTED |     \
                                               CO_STATE_CONDWAIT |             \
                                               CO_STATE_WAITFOR)               \


#       define CO_STATE_RESUMABLE (CO_STATE_READ |                     \
                                   CO_STATE_WRITE |                    \
                                   CO_STATES_RESUMABLE_EXTERNALLY)     \


#       define CO_STATE_STR(st) (                                      \
            (st) == CO_STATE_DORMANT ? "DORMANT" :                     \
            (st) == CO_STATE_RESUMED ? "RESUMED" :                     \
            (st) == CO_STATE_READ ? "READ" :                           \
            (st) == CO_STATE_WRITE ? "WRITE" :                         \
            (st) == CO_STATE_SLEEP ? "SLEEP" :                         \
            (st) == CO_STATE_SET_RESUME ? "SET_RESUME" :               \
            (st) == CO_STATE_SET_INTERRUPT ? "SET_INTERRUPT" :         \
            (st) == CO_STATE_SIGNAL_SUBSCRIBE ? "SIGNAL_SUBSCRIBE" :   \
            (st) == CO_STATE_JOIN ? "JOIN" :                           \
            (st) == CO_STATE_JOIN_INTERRUPTED ? "JOIN_INTERRUPTED" :   \
            (st) == CO_STATE_CONDWAIT ? "CONDWAIT" :                   \
            (st) == CO_STATE_WAITFOR ? "WAITFOR" :                     \
            "<unknown>")                                               \


        unsigned int state;
#       define CO_RC_USER_INTERRUPTED 0x01
#       define CO_RC_TIMEDOUT 0x02
#       define CO_RC_SIMULTANEOUS 0x03
#       define CO_RC_STR(rc) (                                         \
            (rc) == 0 ? "OK" :                                         \
            (rc) == CO_RC_USER_INTERRUPTED ? "USER_INTERRUPTED" :      \
            (rc) == CO_RC_TIMEDOUT ? "TIMEDOUT" :                      \
            (rc) == CO_RC_SIMULTANEOUS ? "SIMULTANEOUS" :              \
            "UD"                                                       \
        )                                                              \

        unsigned char rc;
    } co;

    /*
     * Expiration timestamp in the nsecs from the Epoch.
     * ULONG_MAX if forever. 0 - resume now.
     */
     uint64_t expire_ticks;
#   define MRKTHR_SLEEP_FOREVER (ULONG_MAX)

    /*
     * Sleep list bucket.
     *
     * An instance of mrkthr_ctx_t can be placed in a sleep list in its
     * corresponding position based on the "expire_ticks" key. If another
     * instance is going to be placed in the sleep list under the same
     * key, it cannot be placed there because the keys must be unique. It
     * is then placed in a bucket. A bucket is implemented as a part of
     * the mrkthr_ctx_t structure. Any member of the sleep list can be
     * a "bucket owner", and can subsequently hold other instances under
     * the same key. This design resembles a multimap.
     *
     * UPD: This design can be improved by using <mrkcommon/dtqueue.h>
     */
    mrkthr_waitq_t sleepq_bucket;

    DTQUEUE_ENTRY(_mrkthr_ctx, sleepq_link);

    /*
     * Wait queue this ctx is a host of.
     */
    mrkthr_waitq_t waitq;

    /*
     * Membership of this ctx in an other wait queue.
     */
    DTQUEUE_ENTRY(_mrkthr_ctx, waitq_link);
    mrkthr_waitq_t *hosting_waitq;

    /*
     * Membership of this ctx in free_ctxes.
     */
    STQUEUE_ENTRY(_mrkthr_ctx, free_link);

    /*
     * Membership of this ctx in runq.
     */
    STQUEUE_ENTRY(_mrkthr_ctx, runq_link);

    /*
     * event lookup in kevents0,
     * specifically for mrkthr_clear_event()
     */
    union {
        int _kevent;
        void *_ev;
    } pdata;

};

struct _mrkthr_profile {
    const char *name;
    int id;
    uint64_t n;
    uint64_t running_aggr;
    uint64_t start;
    uint64_t min;
    uint64_t max;
    long double avg;
};


#define CO_FLAG_INITIALIZED 0x01
#define CO_FLAG_SHUTDOWN 0x02
extern int mflags;
extern struct _mrkthr_ctx *me;
extern ucontext_t main_uc;
extern trie_t the_sleepq;

int yield(void);
void push_free_ctx(struct _mrkthr_ctx *);
void sleepq_remove(struct _mrkthr_ctx *);
void mrkthr_ctx_finalize(struct _mrkthr_ctx *);
int dump_ctx_traverser(struct _mrkthr_ctx *, void *);

uint64_t poller_msec2ticks_absolute(uint64_t);
uint64_t poller_ticks_absolute(uint64_t);
void poller_clear_event(struct _mrkthr_ctx *);
void poller_init(void);
void poller_fini(void);
int poller_resume(struct _mrkthr_ctx *);
void poller_sift_sleepq(void);
void poller_mrkthr_ctx_init(struct _mrkthr_ctx *);

#ifdef __cplusplus
}
#endif

#include "mrkthr.h"
#endif
