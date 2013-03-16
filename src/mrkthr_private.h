#ifndef MRKTHR_PRIVATE_H
#define MRKTHR_PRIVATE_H

#include <sys/param.h> /* PAGE_SIZE */
#include <sys/event.h>
#include <sys/limits.h> /* ULONG_MAX*/
#include <sys/socket.h>
#include <sys/tree.h>
#include <sys/types.h>

#include <netinet/in.h>
#include <ucontext.h>

#include "mrkcommon/array.h"
#include "mrkcommon/rbt.h"
#include "mrkcommon/util.h"

#define SSIZE (PAGE_SIZE * 1024)

struct _mrkthr_ctx;

struct _mrkthr_ctx_list {
    struct _mrkthr_ctx *head;
    struct _mrkthr_ctx *tail;
};

struct _mrkthr_ctx_list_entry {
    struct _mrkthr_ctx *prev;
    struct _mrkthr_ctx *next;
};

struct _mrkthr_ctx {
    struct _co {
        ucontext_t uc;
        char *stack;
        int id;
        char name[64];
        int (*f)(int, void *[]);
        int argc;
        void **argv;
#       define CO_STATE_DORMANT 0x01
#       define CO_STATE_RESUMED 0x02
#       define CO_STATE_EVENT_READ 0x04
#       define CO_STATE_EVENT_WRITE 0x08
#       define CO_STATE_EVENT_SLEEP 0x10
#       define CO_STATE_EVENT_ACQUIRE 0x20
#       define CO_STATE_EVENT_RESUME 0x40
#       define CO_STATE_EVENT_JOINWAITQ 0x80
#       define CO_STATE_EVENT_INTERRUPT 0x100
#       define CO_STATE_RESUMABLE (CO_STATE_EVENT_READ | CO_STATE_EVENT_WRITE | CO_STATE_EVENT_SLEEP | CO_STATE_EVENT_ACQUIRE | CO_STATE_EVENT_RESUME | CO_STATE_EVENT_JOINWAITQ | CO_STATE_EVENT_INTERRUPT)
        unsigned int state;
#       define CO_RC_USER_INTERRUPTED 0x01
        unsigned char rc;
        /* not really in rc */
#       define CO_RC_JOIN_INVALID 0x02
    } co;

    /*
     * Expiration timestamp in the nsecs from the Epoch.
     * ULONG_MAX if forever. 0 - resume now.
     */
     uint64_t expire_ticks;
#   define mrkthr_SLEEP_FOREVER (ULONG_MAX)

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
     */
#ifdef USE_RBT
    RB_ENTRY(_mrkthr_ctx) sleepq_link;
#endif
    struct _mrkthr_ctx_list sleepq_bucket;
    struct _mrkthr_ctx_list_entry sleepq_bucket_entry;

    array_t waitq;
    /*
     * event lookup in kevents0,
     * specifically for mrkthr_clear_event()
     */
    int _idx0;

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

#include "mrkthr.h"
#endif
