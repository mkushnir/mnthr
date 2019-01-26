#ifndef MRKTHR_H
#define MRKTHR_H

#include <stdbool.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <netdb.h>

#include <mndiag.h>

#include <mrkcommon/dtqueue.h>
#include <mrkcommon/dumpm.h>
#include <mrkcommon/util.h>
#include <mrkcommon/bytestream.h>


#ifdef __cplusplus
extern "C" {
#endif

void mndiag_mrkthr_str(int, char *, size_t);

#define CTRACE(s, ...)                                         \
    do {                                                       \
        struct timeval _mrkthr_ts;                             \
        gettimeofday(&_mrkthr_ts, NULL);                       \
        struct tm *_mrkthr_tm = localtime(&_mrkthr_ts.tv_sec); \
        char _mrkthr_ss[64];                                   \
        strftime(_mrkthr_ss,                                   \
                 sizeof(_mrkthr_ss),                           \
                 "%Y-%m-%d %H:%M:%S",                          \
                 _mrkthr_tm);                                  \
        TRACE("%s.%06ld [% 4d] " s,                            \
              _mrkthr_ss,                                      \
              (long)_mrkthr_ts.tv_usec,                        \
              mrkthr_id(), ##__VA_ARGS__);                     \
    } while (0)                                                \


typedef int (*mrkthr_cofunc_t)(int, void *[]);
typedef struct _mrkthr_ctx mrkthr_ctx_t;

#ifndef MRKTHR_WAITQ_T_DEFINED
typedef DTQUEUE(_mrkthr_ctx, mrkthr_waitq_t);
#define MRKTHR_WAITQ_T_DEFINED
#endif

#define MRKTHRET(rv) mrkthr_set_retval(rv); return rv

#define MRKTHR_WAIT_TIMEOUT \
    MNDIAG_PUBLIC_CODE(MNDIAG_LIBRARY_MRKTHR, 129, 1)


#define MRKTHR_JOIN_FAILURE \
    MNDIAG_PUBLIC_CODE(MNDIAG_LIBRARY_MRKTHR, 129, 2)


#define MRKTHR_RWLOCK_TRY_ACQUIRE_READ_FAIL \
    MNDIAG_PUBLIC_CODE(MNDIAG_LIBRARY_MRKTHR, 129, 3)


#define MRKTHR_RWLOCK_TRY_ACQUIRE_WRITE_FAIL \
    MNDIAG_PUBLIC_CODE(MNDIAG_LIBRARY_MRKTHR, 129, 4)


#define MRKTHR_SEMA_TRY_ACQUIRE_FAIL \
    MNDIAG_PUBLIC_CODE(MNDIAG_LIBRARY_MRKTHR, 129, 5)


/* These calls are cancellation points */
#define MRKTHR_CPOINT

union _mrkthr_addr {
    struct sockaddr sa;
    struct sockaddr_in sa4;
    struct sockaddr_in6 sa6;
};
typedef struct _mrkthr_socket {
    int fd;
    union _mrkthr_addr addr;
    socklen_t addrlen;
} mrkthr_socket_t;

typedef struct _mrkthr_signal {
    struct _mrkthr_ctx *owner;
} mrkthr_signal_t;

typedef struct _mrkthr_cond {
    mrkthr_waitq_t waitq;
} mrkthr_cond_t;

typedef struct _mrkthr_sema {
    mrkthr_cond_t cond;
    int n;
    int i;
} mrkthr_sema_t;

typedef struct _mrkthr_inverted_sema {
    mrkthr_cond_t cond;
    int n;
    int i;
} mrkthr_inverted_sema_t;

typedef struct _mrkthr_rwlock {
    mrkthr_cond_t cond;
    unsigned nreaders;
    bool fwriter;
} mrkthr_rwlock_t;

int mrkthr_init(void);
int mrkthr_fini(void);
int mrkthr_loop(void);

void mrkthr_shutdown(void);
size_t mrkthr_compact_sleepq(size_t);
size_t mrkthr_get_sleepq_length(void);
size_t mrkthr_get_sleepq_volume(void);
void mrkthr_dump_all_ctxes(void);
void mrkthr_dump_sleepq(void);
size_t mrkthr_gc(void);
size_t mrkthr_ctx_sizeof(void);
size_t mrkthr_set_stacksize(size_t);

int mrkthr_dump(const mrkthr_ctx_t *);
mrkthr_ctx_t *mrkthr_new(const char *, mrkthr_cofunc_t, int, ...);
#define MRKTHR_NEW(name, f, ...)    \
    mrkthr_new(name, f, MRKASZ(__VA_ARGS__), ##__VA_ARGS__)
mrkthr_ctx_t *mrkthr_spawn(const char *name, mrkthr_cofunc_t, int, ...);
#define MRKTHR_SPAWN(name, f, ...)  \
    mrkthr_spawn(name, f, MRKASZ(__VA_ARGS__), ##__VA_ARGS__)
mrkthr_ctx_t *mrkthr_new_sig(const char *, mrkthr_cofunc_t, int, ...);
mrkthr_ctx_t *mrkthr_spawn_sig(const char *, mrkthr_cofunc_t, int, ...);
#define MRKTHR_SPAWN_SIG(name, f, ...)  \
    mrkthr_spawn_sig(name, f, MRKASZ(__VA_ARGS__), ##__VA_ARGS__)
PRINTFLIKE(2, 3) int mrkthr_set_name(mrkthr_ctx_t *, const char *, ...);
mrkthr_ctx_t *mrkthr_me(void);
int mrkthr_id(void);

#define MRKTHR_CO_RC_EXITED \
    MNDIAG_PUBLIC_CODE(MNDIAG_LIBRARY_MRKTHR, 130, 1)

#define MRKTHR_CO_RC_USER_INTERRUPTED   \
    MNDIAG_PUBLIC_CODE(MNDIAG_LIBRARY_MRKTHR, 130, 2)

#define MRKTHR_CO_RC_TIMEDOUT   \
    MNDIAG_PUBLIC_CODE(MNDIAG_LIBRARY_MRKTHR, 130, 3)

#define MRKTHR_CO_RC_SIMULTANEOUS   \
    MNDIAG_PUBLIC_CODE(MNDIAG_LIBRARY_MRKTHR, 130, 4)

#define MRKTHR_CO_RC_POLLER \
    MNDIAG_PUBLIC_CODE(MNDIAG_LIBRARY_MRKTHR, 130, 5)

#define MRKTHR_CO_RC_STR(rc) (                                         \
     (rc) == 0 ? "OK" :                                                \
     (rc) == (int)MRKTHR_CO_RC_EXITED ? "EXITED" :                     \
     (rc) == (int)MRKTHR_CO_RC_USER_INTERRUPTED ? "USER_INTERRUPTED" : \
     (rc) == (int)MRKTHR_CO_RC_TIMEDOUT ? "TIMEDOUT" :                 \
     (rc) == (int)MRKTHR_CO_RC_SIMULTANEOUS ? "SIMULTANEOUS" :         \
     (rc) == (int)MRKTHR_CO_RC_POLLER ? "POLLER" :                     \
     "UD"                                                              \
 )                                                                     \


#define MRKTHR_IS_CO_RC(rc)                            \
    (((rc) &                                           \
      (MNDIAG_BIT_GLOBAL |                             \
       MNDIAG_BIT_PUBLIC |                             \
       MNDIAG_BIT_LIBRARY |                            \
       MNDIAG_BIT_CLASS)) ==                           \
     MNDIAG_PUBLIC_CODE(MNDIAG_LIBRARY_MRKTHR, -2, 0)) \


int mrkthr_get_retval(void);
int mrkthr_set_retval(int);
void *mrkthr_set_cld(void *);
void *mrkthr_get_cld(void);
bool mrkthr_is_runnable(mrkthr_ctx_t *);

void mrkthr_set_prio(mrkthr_ctx_t *, int);
void mrkthr_incabac(mrkthr_ctx_t *);
void mrkthr_decabac(mrkthr_ctx_t *);
MRKTHR_CPOINT int mrkthr_sleep(uint64_t);
MRKTHR_CPOINT int mrkthr_sleep_usec(uint64_t);
MRKTHR_CPOINT int mrkthr_sleep_ticks(uint64_t);
MRKTHR_CPOINT int mrkthr_yield(void);
MRKTHR_CPOINT int mrkthr_giveup(void);
long double mrkthr_ticks2sec(uint64_t);
long double mrkthr_ticksdiff2sec(int64_t);
uint64_t mrkthr_msec2ticks(uint64_t);
MRKTHR_CPOINT int mrkthr_join(mrkthr_ctx_t *);
void mrkthr_run(mrkthr_ctx_t *);
void mrkthr_set_interrupt(mrkthr_ctx_t *);
MRKTHR_CPOINT int mrkthr_set_interrupt_and_join(mrkthr_ctx_t *);
MRKTHR_CPOINT int mrkthr_set_interrupt_and_join_with_timeout(mrkthr_ctx_t *, uint64_t);
int mrkthr_is_dead(mrkthr_ctx_t *);

int mrkthr_socket(const char *, const char *, int, int);
int mrkthr_socket_bind(const char *, const char *, int);
MRKTHR_CPOINT int mrkthr_socket_connect(const char *, const char *, int);
MRKTHR_CPOINT int mrkthr_connect(int, const struct sockaddr *, socklen_t);
MRKTHR_CPOINT ssize_t mrkthr_get_rbuflen(int);
MRKTHR_CPOINT int mrkthr_wait_for_read(int);
MRKTHR_CPOINT int mrkthr_wait_for_write(int);
#define MRKTHR_WAIT_EVENT_READ (0x01)
#define MRKTHR_WAIT_EVENT_WRITE (0x02)
MRKTHR_CPOINT int mrkthr_wait_for_events(int, int *);
MRKTHR_CPOINT int mrkthr_accept_all(int, mrkthr_socket_t **, off_t *);
MRKTHR_CPOINT int mrkthr_accept_all2(int, mrkthr_socket_t **, off_t *);
MRKTHR_CPOINT int mrkthr_read_all(int, char **, off_t *);
MRKTHR_CPOINT ssize_t mrkthr_read_allb(int, char *, ssize_t);
MRKTHR_CPOINT ssize_t mrkthr_read_allb_et(int, char *, ssize_t);
MRKTHR_CPOINT ssize_t mrkthr_recv_allb(int, char *, ssize_t, int);
MRKTHR_CPOINT ssize_t mrkthr_recvfrom_allb(int,
                                          void * restrict,
                                          ssize_t,
                                          int,
                                          struct sockaddr * restrict,
                                          socklen_t * restrict);
MRKTHR_CPOINT ssize_t mrkthr_get_wbuflen(int);
MRKTHR_CPOINT int mrkthr_write_all(int, const char *, size_t);
MRKTHR_CPOINT int mrkthr_write_all_et(int, const char *, size_t);
MRKTHR_CPOINT int mrkthr_send_all(int, const char *, size_t, int);
MRKTHR_CPOINT int mrkthr_sendto_all(int,
                                   const void *,
                                   size_t,
                                   int,
                                   const struct sockaddr *,
                                   socklen_t);
#if 0
MRKTHR_CPOINT int mrkthr_sendfile_np(int,
                                     int,
                                     off_t,
                                     size_t,
                                     struct sf_hdtr *,
                                     off_t *,
                                     int);
#else
MRKTHR_CPOINT int mrkthr_sendfile(int,
                                  int,
                                  off_t *,
                                  size_t);

#endif

/*
 *
 */
#define MRKTHR_ST_UNKNOWN   (0x00000)
#define MRKTHR_ST_DELETE    (0x10000)
#define MRKTHR_ST_WRITE     (0x20000)
#define MRKTHR_ST_ATTRIB    (0x40000)
typedef struct _mrkthr_stat mrkthr_stat_t;
mrkthr_stat_t *mrkthr_stat_new(const char *path);
void mrkthr_stat_destroy(mrkthr_stat_t **);
MRKTHR_CPOINT int mrkthr_stat_wait(mrkthr_stat_t *);

void mrkthr_signal_init(mrkthr_signal_t *, mrkthr_ctx_t *);
#define MRKTHR_SIGNAL_INIT(signal) mrkthr_signal_init((signal), NULL)
void mrkthr_signal_fini(mrkthr_signal_t *);
int mrkthr_signal_has_owner(mrkthr_signal_t *);
mrkthr_ctx_t *mrkthr_signal_get_owner(mrkthr_signal_t *);
MRKTHR_CPOINT int mrkthr_signal_subscribe(mrkthr_signal_t *);
MRKTHR_CPOINT int mrkthr_signal_subscribe_with_timeout(mrkthr_signal_t *,
                                                      uint64_t);
void mrkthr_signal_send(mrkthr_signal_t *);
void mrkthr_signal_error(mrkthr_signal_t *, int);
MRKTHR_CPOINT int mrkthr_signal_error_and_join(mrkthr_signal_t *, int);
#define mrkthr_signal_unsubscribe(signal) mrkthr_signal_fini((signal));

void mrkthr_cond_init(mrkthr_cond_t *);
MRKTHR_CPOINT int mrkthr_cond_wait(mrkthr_cond_t *);
void mrkthr_cond_signal_all(mrkthr_cond_t *);
void mrkthr_cond_signal_one(mrkthr_cond_t *);
void mrkthr_cond_fini(mrkthr_cond_t *);

void mrkthr_sema_init(mrkthr_sema_t *, int);
MRKTHR_CPOINT int mrkthr_sema_acquire(mrkthr_sema_t *);
int mrkthr_sema_try_acquire(mrkthr_sema_t *);
void mrkthr_sema_release(mrkthr_sema_t *);
void mrkthr_sema_fini(mrkthr_sema_t *);

void mrkthr_inverted_sema_init(mrkthr_inverted_sema_t *, int);
void mrkthr_inverted_sema_acquire(mrkthr_inverted_sema_t *);
MRKTHR_CPOINT int mrkthr_inverted_sema_wait(mrkthr_inverted_sema_t *);
void mrkthr_inverted_sema_release(mrkthr_inverted_sema_t *);
void mrkthr_inverted_sema_fini(mrkthr_inverted_sema_t *);

void mrkthr_rwlock_init(mrkthr_rwlock_t *);
MRKTHR_CPOINT int mrkthr_rwlock_acquire_read(mrkthr_rwlock_t *);
MRKTHR_CPOINT int mrkthr_rwlock_try_acquire_read(mrkthr_rwlock_t *);
void mrkthr_rwlock_release_read(mrkthr_rwlock_t *);
MRKTHR_CPOINT int mrkthr_rwlock_acquire_write(mrkthr_rwlock_t *);
MRKTHR_CPOINT int mrkthr_rwlock_try_acquire_write(mrkthr_rwlock_t *);
void mrkthr_rwlock_release_write(mrkthr_rwlock_t *);
void mrkthr_rwlock_fini(mrkthr_rwlock_t *);


uint64_t mrkthr_get_now_nsec(void);
#define MRKTHR_GET_NOW_SEC() \
    (mrkthr_get_now_nsec() / 1000000000)
#define MRKTHR_GET_NOW_FSEC() \
    ((double)mrkthr_get_now(_nsed) / 1000000000.0)
#define MRKTHR_GET_NOW_MSEC() \
    (mrkthr_get_now_nsec() / 1000000)
#define MRKTHR_GET_NOW_USEC() \
    (mrkthr_get_now_nsec() / 1000)

uint64_t mrkthr_get_now_nsec_precise(void);
#define MRKTHR_GET_NOW_SEC_PRECISE() \
    (mrkthr_get_now_nsec_precise() / 1000000000)
#define MRKTHR_GET_NOW_FSEC_PRECISE() \
    ((double)mrkthr_get_now_nsec_precise() / 1000000000.0)
#define MRKTHR_GET_NOW_MSEC_PRECISE() \
    (mrkthr_get_now_nsec_precise() / 1000000)
#define MRKTHR_GET_NOW_USEC_PRECISE() \
    (mrkthr_get_now_nsec_precise() / 1000)

uint64_t mrkthr_get_now_ticks(void);
#define MRKTHR_GET_NOW_TICKS_SEC() \
    (mrkthr_get_now_ticks() / 1000000000)
#define MRKTHR_GET_NOW_TICKS_FSEC() \
    ((double)mrkthr_get_now_ticks() / 1000000000.0)

uint64_t mrkthr_get_now_ticks_precise(void);
#define MRKTHR_GET_NOW_TICKS_PRECISE_SEC() \
    (mrkthr_get_now_ticks_precise() / 1000000000)
#define MRKTHR_GET_NOW_TICKS_PRECISE_FSEC() \
    ((double)mrkthr_get_now_ticks_precise() / 1000000000.0)

MRKTHR_CPOINT int mrkthr_wait_for(uint64_t, const char *, mrkthr_cofunc_t, int, ...);
#define MRKTHR_WAIT_FOR(timeout, name, f, ...)  \
    mrkthr_wait_for(timeout, name, f, MRKASZ(__VA_ARGS__), ##__VA_ARGS__)
MRKTHR_CPOINT int mrkthr_peek(mrkthr_ctx_t *, uint64_t);

MRKTHR_CPOINT ssize_t mrkthr_bytestream_read_more(mnbytestream_t *, void *, ssize_t);
MRKTHR_CPOINT ssize_t mrkthr_bytestream_read_more_et(mnbytestream_t *, void *, ssize_t);
MRKTHR_CPOINT ssize_t mrkthr_bytestream_write(mnbytestream_t *, void *, size_t);
MRKTHR_CPOINT ssize_t mrkthr_bytestream_write_et(mnbytestream_t *, void *, size_t);

#ifdef __cplusplus
}
#endif

#endif
