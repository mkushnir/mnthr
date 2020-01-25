#ifndef MNTHR_PRIVATE_H
#define MNTHR_PRIVATE_H

#ifdef HAVE_CONFIG_H
#   include "config.h"
#endif

#include <stdint.h> /* UINTMAX_MAX */

#include <netinet/in.h>
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE
#endif
#include <ucontext.h>

#include <sys/param.h> /* PAGE_SIZE */

#ifndef PAGE_SIZE
#   ifdef HAVE_SYS_USER_H
#       include <sys/user.h>
#   else
#       error "PAGE_SIZE cannot be defined"
#   endif
#endif

#include <sys/socket.h>
#include <sys/types.h>

#include <mndiag.h>

#include <mncommon/dtqueue.h>
#include <mncommon/stqueue.h>
#include <mncommon/rbt.h>
#include <mncommon/util.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * PAGE_SIZE is known to be defined using sysconf on some systems
 */
#ifndef HAVE_PAGE_SIZE_CONSTANT
#   ifdef PAGE_SIZE
#       undef PAGE_SIZE
#   endif
#   define PAGE_SIZE 4096
#endif

#ifndef STACKSIZE
#   define STACKSIZE (PAGE_SIZE * 8)
#endif


#ifndef HAVE_SF_HDTR
struct sf_hdtr {
    struct iovec *headers;
    int hdr_cnt;
    struct iovec *trailers;
    int trl_cnt;
};
#endif


struct _mnthr_ctx;

typedef DTQUEUE(_mnthr_ctx, mnthr_waitq_t);
#define MNTHR_WAITQ_T_DEFINED

struct _mnthr_ctx {
    struct _co {
        ucontext_t uc;
        char *stack;
        int64_t id;
        char name[8];
        int (*f)(int, void *[]);
        void **argv;
        /* weakref */
        void *cld;
        int argc;
        unsigned abac;

#       define CO_STATE_DORMANT 0x01
#       define CO_STATE_RESUMED 0x02
#       define CO_STATE_READ 0x04
#       define CO_STATE_WRITE 0x08
#       define CO_STATE_SLEEP 0x10
#       define CO_STATE_OTHER_POLLER 0x20
#       define CO_STATE_SET_RESUME 0x40
#       define CO_STATE_SET_INTERRUPT 0x80
#       define CO_STATE_SIGNAL_SUBSCRIBE 0x100
#       define CO_STATE_JOIN 0x200
#       define CO_STATE_JOIN_INTERRUPTED 0x400
#       define CO_STATE_CONDWAIT 0x800
#       define CO_STATE_WAITFOR 0x1000
#       define CO_STATE_PEEK 0x2000

#       define CO_STATES_RESUMABLE_EXTERNALLY (CO_STATE_SLEEP |                \
                                               CO_STATE_SET_RESUME |           \
                                               CO_STATE_SET_INTERRUPT |        \
                                               CO_STATE_SIGNAL_SUBSCRIBE |     \
                                               CO_STATE_JOIN |                 \
                                               CO_STATE_JOIN_INTERRUPTED |     \
                                               CO_STATE_CONDWAIT |             \
                                               CO_STATE_WAITFOR  |             \
                                               CO_STATE_PEEK)                  \


#       define CO_STATE_RESUMABLE (CO_STATE_READ |                     \
                                   CO_STATE_WRITE |                    \
                                   CO_STATE_OTHER_POLLER |             \
                                   CO_STATES_RESUMABLE_EXTERNALLY)     \


#       define CO_STATE_STR(st) (                                      \
            (st) == CO_STATE_DORMANT ? "DORMANT" :                     \
            (st) == CO_STATE_RESUMED ? "RESUMED" :                     \
            (st) == CO_STATE_READ ? "READ" :                           \
            (st) == CO_STATE_WRITE ? "WRITE" :                         \
            (st) == CO_STATE_OTHER_POLLER ? "OTHER_POLLER" :           \
            (st) == CO_STATE_SLEEP ? "SLEEP" :                         \
            (st) == CO_STATE_SET_RESUME ? "SET_RESUME" :               \
            (st) == CO_STATE_SET_INTERRUPT ? "SET_INTERRUPT" :         \
            (st) == CO_STATE_SIGNAL_SUBSCRIBE ? "SIGNAL_SUBSCRIBE" :   \
            (st) == CO_STATE_JOIN ? "JOIN" :                           \
            (st) == CO_STATE_JOIN_INTERRUPTED ? "JOIN_INTERRUPTED" :   \
            (st) == CO_STATE_CONDWAIT ? "CONDWAIT" :                   \
            (st) == CO_STATE_WAITFOR ? "WAITFOR" :                     \
            (st) == CO_STATE_PEEK ? "PEEK" :                           \
            "<unknown>")                                               \


        unsigned int state;

        /*
         * Thread return code, MNTHR_CO_RC_* or user-defined, can be set
         * publicly using:
         *  - mnthr_set_retval()
         *  - mnthr_signal_error()
         *  - mnthr_signal_error_and_join()
         *
         * When setting, it is recommended to restrict to non-negative
         * values in order to prevent clashing with library's error codes
         * (which are all negative).
         *
         * This code is the return value of the following:
         *  - mnthr_signal_subscribe()
         *  - mnthr_join() + library code MNTHR_JOIN_FAILURE
         *  - mnthr_set_interrupt_and_join() + library code
         *    MNTHR_JOIN_FAILURE
         *  - mnthr_signal_error_and_join()
         *  - mnthr_cond_wait()
         *  - mnthr_yield()
         *  - mnthr_sleep_ticks()
         *  - mnthr_sleep()
         *  - mnthr_set_interrupt_and_join_with_timeout() + library codes
         *      MNTHR_JOIN_FAILURE, MNTHR_WAIT_TIMEOUT
         *  - mnthr_signal_subscribe_with_timeout() + library codes
         *      MNTHR_WAIT_TIMEOUT
         *  - mnthr_wait_for() + library code MNTHR_WAIT_TIMEOUT
         *
         * The following finctions return -1 on error, so the thread
         * return code can only be expected using mnthr_get_retval():
         *  - mnthr_stat_wait()
         *  - mnthr_get_rbuflen()
         *  - mnthr_wait_for_read()
         *  - mnthr_get_wbuflen()
         *  - mnthr_wait_for_write()
         *  - mnthr_wait_for_events()
         *
         */
        int rc;
    } co;

    /*
     * Expiration timestamp in the nsecs from the Epoch.
     * UINTMAX_MAX if forever. 0 - undefined (can never enter sleepq),
     * 1 - resume now.
     */
     uint64_t expire_ticks;
#   define MNTHR_SLEEP_UNDEFINED (0ul)
#   define MNTHR_SLEEP_RESUME_NOW (1ul)
#   define MNTHR_SLEEP_FOREVER (UINTMAX_MAX)

    void (*sleepq_enqueue)(struct _mnthr_ctx *);

    /*
     * Sleep list bucket.
     *
     * An instance of mnthr_ctx_t can be placed in a sleep list in its
     * corresponding position based on the "expire_ticks" key. If another
     * instance is going to be placed in the sleep list under the same
     * key, it cannot be placed there because the keys must be unique. It
     * is then placed in a bucket. A bucket is implemented as a part of
     * the mnthr_ctx_t structure. Any member of the sleep list can be
     * a "bucket owner", and can subsequently hold other instances under
     * the same key. This design resembles a multimap.
     */
    mnthr_waitq_t sleepq_bucket;

    DTQUEUE_ENTRY(_mnthr_ctx, sleepq_link);

    /*
     * Wait queue this ctx is a host of.
     */
    mnthr_waitq_t waitq;

    /*
     * Membership of this ctx in an other wait queue.
     */
    DTQUEUE_ENTRY(_mnthr_ctx, waitq_link);
    mnthr_waitq_t *hosting_waitq;

    /*
     * Membership of this ctx in free_ctxes.
     */
    DTQUEUE_ENTRY(_mnthr_ctx, free_link);

    /*
     * Membership of this ctx in runq.
     */
    STQUEUE_ENTRY(_mnthr_ctx, runq_link);

    /*
     * event lookup in kevents0,
     * specifically for mnthr_clear_event()
     */
    union {
        struct {
            int ident;
            int filter; /* special case CO_STATE_OTHER_POLLER */
            int idx;
        } kev;
        void *ev;
    } pdata;

};

//#define MNTHR_ST_DELETE
// NOTE_DELETE
// IN_DELETE_SELF
//#define MNTHR_ST_WRITE
// NOTE_WRITE
// IN_CREATE, IN_DELETE, IN_MOVED_FROM, IN_MOVED_TO
//#define MNTHR_ST_EXTEND
//NOTE_EXTEND
//
//#define MNTHR_ST_ATTRIB
// NOTE_ATTRIB
// IN_ATTRIB
//#define MNTHR_ST_LINK
// NOTE_LINK
//
//#define MNTHR_ST_RENAME
// NOTE_RENAME
//
//#define MNTHR_ST_REVOKE
// NOTE_REVOKE
//


#ifdef USE_EV
struct _ev_item;
#endif

struct _mnthr_stat {
#ifdef USE_EV
    struct _ev_item *ev;
#endif
#ifdef USE_KEVENT
    char *path;
    int fd;
#endif
};

struct _mnthr_profile {
    const char *name;
    int id;
    uint64_t n;
    uint64_t running_aggr;
    uint64_t start;
    uint64_t min;
    uint64_t max;
    long double avg;
};

#define MNTHR_DEFAULT_WBUFLEN (1024*1024)

#define CO_FLAG_INITIALIZED 0x01
#define CO_FLAG_SHUTDOWN 0x02
extern int mnthr_flags;
extern struct _mnthr_ctx *me;
extern ucontext_t main_uc;
extern mnbtrie_t the_sleepq;

int yield(void);
void push_free_ctx(struct _mnthr_ctx *);
void sleepq_remove(struct _mnthr_ctx *);
void set_resume_fast(struct _mnthr_ctx *);
void mnthr_ctx_finalize(struct _mnthr_ctx *);

uint64_t poller_usec2ticks_absolute(uint64_t);
uint64_t poller_msec2ticks_absolute(uint64_t);
uint64_t poller_ticks_absolute(uint64_t);
void poller_clear_event(struct _mnthr_ctx *);
void poller_init(void);
void poller_fini(void);
int poller_resume(struct _mnthr_ctx *);
void poller_sift_sleepq(void);
void poller_mnthr_ctx_init(struct _mnthr_ctx *);

#ifdef __cplusplus
}
#endif

#include "mnthr.h"
#endif
