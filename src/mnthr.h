#ifndef MNTHR_H
#define MNTHR_H

#include <stdbool.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <netdb.h>

#include <mndiag.h>

#include <mncommon/dtqueue.h>
#include <mncommon/dumpm.h>
#include <mncommon/util.h>
#include <mncommon/bytestream.h>


#ifdef __cplusplus
extern "C" {
#endif

void mndiag_mnthr_str(int, char *, size_t);

#define CTRACE(s, ...)                                         \
    do {                                                       \
        struct timeval _mnthr_ts;                              \
        gettimeofday(&_mnthr_ts, NULL);                        \
        struct tm *_mnthr_tm = localtime(&_mnthr_ts.tv_sec);   \
        char _mnthr_ss[64];                                    \
        strftime(_mnthr_ss,                                    \
                 sizeof(_mnthr_ss),                            \
                 "%Y-%m-%d %H:%M:%S",                          \
                 _mnthr_tm);                                   \
        TRACE("%s.%06ld [% 4d] " s,                            \
              _mnthr_ss,                                       \
              (long)_mnthr_ts.tv_usec,                         \
              mnthr_id(), ##__VA_ARGS__);                      \
    } while (0)                                                \


typedef int (*mnthr_cofunc_t)(int, void *[]);
typedef struct _mnthr_ctx mnthr_ctx_t;

#ifndef MNTHR_WAITQ_T_DEFINED
typedef DTQUEUE(_mnthr_ctx, mnthr_waitq_t);
#define MNTHR_WAITQ_T_DEFINED
#endif

#define MNTHRET(rv) mnthr_set_retval(rv); return rv

#define MNTHR_WAIT_TIMEOUT \
    MNDIAG_PUBLIC_CODE(MNDIAG_LIBRARY_MNTHR, 129, 1)


#define MNTHR_JOIN_FAILURE \
    MNDIAG_PUBLIC_CODE(MNDIAG_LIBRARY_MNTHR, 129, 2)


#define MNTHR_RWLOCK_TRY_ACQUIRE_READ_FAIL \
    MNDIAG_PUBLIC_CODE(MNDIAG_LIBRARY_MNTHR, 129, 3)


#define MNTHR_RWLOCK_TRY_ACQUIRE_WRITE_FAIL \
    MNDIAG_PUBLIC_CODE(MNDIAG_LIBRARY_MNTHR, 129, 4)


#define MNTHR_SEMA_TRY_ACQUIRE_FAIL \
    MNDIAG_PUBLIC_CODE(MNDIAG_LIBRARY_MNTHR, 129, 5)


/* These calls are cancellation points */
#define MNTHR_CPOINT

union _mnthr_addr {
    struct sockaddr sa;
    struct sockaddr_in sa4;
    struct sockaddr_in6 sa6;
};
typedef struct _mnthr_socket {
    int fd;
    union _mnthr_addr addr;
    socklen_t addrlen;
} mnthr_socket_t;

typedef struct _mnthr_signal {
    struct _mnthr_ctx *owner;
} mnthr_signal_t;

typedef struct _mnthr_cond {
    mnthr_waitq_t waitq;
} mnthr_cond_t;

typedef struct _mnthr_sema {
    mnthr_cond_t cond;
    int n;
    int i;
} mnthr_sema_t;

typedef struct _mnthr_inverted_sema {
    mnthr_cond_t cond;
    int n;
    int i;
} mnthr_inverted_sema_t;

typedef struct _mnthr_rwlock {
    mnthr_cond_t cond;
    unsigned nreaders;
    bool fwriter;
} mnthr_rwlock_t;

typedef struct _mnthr_gen {
    mnthr_signal_t s0, s1;
    void *udata;
} mnthr_gen_t;

int mnthr_init(void);
int mnthr_fini(void);
int mnthr_loop(void);

void mnthr_shutdown(void);
bool mnthr_shutting_down(void);
size_t mnthr_compact_sleepq(size_t);
size_t mnthr_get_sleepq_length(void);
size_t mnthr_get_sleepq_volume(void);
void mnthr_dump_all_ctxes(void);
void mnthr_dump_sleepq(void);
size_t mnthr_gc(void);
size_t mnthr_ctx_sizeof(void);
size_t mnthr_set_stacksize(size_t);

int mnthr_dump(const mnthr_ctx_t *);
mnthr_ctx_t *mnthr_new(const char *, mnthr_cofunc_t, int, ...);
#define MNTHR_NEW(name, f, ...)    \
    mnthr_new(name, f, MNASZ(__VA_ARGS__), ##__VA_ARGS__)
mnthr_ctx_t *mnthr_spawn(const char *name, mnthr_cofunc_t, int, ...);
#define MNTHR_SPAWN(name, f, ...)  \
    mnthr_spawn(name, f, MNASZ(__VA_ARGS__), ##__VA_ARGS__)
mnthr_ctx_t *mnthr_new_sig(const char *, mnthr_cofunc_t, int, ...);
mnthr_ctx_t *mnthr_spawn_sig(const char *, mnthr_cofunc_t, int, ...);
#define MNTHR_SPAWN_SIG(name, f, ...)  \
    mnthr_spawn_sig(name, f, MNASZ(__VA_ARGS__), ##__VA_ARGS__)
PRINTFLIKE(2, 3) int mnthr_set_name(mnthr_ctx_t *, const char *, ...);
mnthr_ctx_t *mnthr_me(void);
int mnthr_id(void);

#define MNTHR_CO_RC_EXITED \
    MNDIAG_PUBLIC_CODE(MNDIAG_LIBRARY_MNTHR, 130, 1)

#define MNTHR_CO_RC_USER_INTERRUPTED   \
    MNDIAG_PUBLIC_CODE(MNDIAG_LIBRARY_MNTHR, 130, 2)

#define MNTHR_CO_RC_TIMEDOUT   \
    MNDIAG_PUBLIC_CODE(MNDIAG_LIBRARY_MNTHR, 130, 3)

#define MNTHR_CO_RC_SIMULTANEOUS   \
    MNDIAG_PUBLIC_CODE(MNDIAG_LIBRARY_MNTHR, 130, 4)

#define MNTHR_CO_RC_POLLER \
    MNDIAG_PUBLIC_CODE(MNDIAG_LIBRARY_MNTHR, 130, 5)

#define MNTHR_CO_RC_STR(rc) (                                          \
     (rc) == 0 ? "OK" :                                                \
     (rc) == (int)MNTHR_CO_RC_EXITED ? "EXITED" :                      \
     (rc) == (int)MNTHR_CO_RC_USER_INTERRUPTED ? "USER_INTERRUPTED" :  \
     (rc) == (int)MNTHR_CO_RC_TIMEDOUT ? "TIMEDOUT" :                  \
     (rc) == (int)MNTHR_CO_RC_SIMULTANEOUS ? "SIMULTANEOUS" :          \
     (rc) == (int)MNTHR_CO_RC_POLLER ? "POLLER" :                      \
     "UD"                                                              \
 )                                                                     \


#define MNTHR_IS_CO_RC(rc)                             \
    (((rc) &                                           \
      (MNDIAG_BIT_GLOBAL |                             \
       MNDIAG_BIT_PUBLIC |                             \
       MNDIAG_BIT_LIBRARY |                            \
       MNDIAG_BIT_CLASS)) ==                           \
     MNDIAG_PUBLIC_CODE(MNDIAG_LIBRARY_MNTHR, -2, 0))  \


int mnthr_get_retval(void);
int mnthr_set_retval(int);
void *mnthr_set_cld(void *);
void *mnthr_get_cld(void);
bool mnthr_is_runnable(mnthr_ctx_t *);

void mnthr_set_prio(mnthr_ctx_t *, int);
void mnthr_incabac(mnthr_ctx_t *);
void mnthr_decabac(mnthr_ctx_t *);
MNTHR_CPOINT int mnthr_sleep(uint64_t);
MNTHR_CPOINT int mnthr_sleep_usec(uint64_t);
MNTHR_CPOINT int mnthr_sleep_ticks(uint64_t);
MNTHR_CPOINT int mnthr_yield(void);
MNTHR_CPOINT int mnthr_giveup(void);
long double mnthr_ticks2sec(uint64_t);
long double mnthr_ticksdiff2sec(int64_t);
uint64_t mnthr_msec2ticks(uint64_t);
MNTHR_CPOINT int mnthr_join(mnthr_ctx_t *);
void mnthr_run(mnthr_ctx_t *);
void mnthr_set_interrupt(mnthr_ctx_t *);
MNTHR_CPOINT int mnthr_set_interrupt_and_join(mnthr_ctx_t *);
MNTHR_CPOINT int mnthr_set_interrupt_and_join_with_timeout(mnthr_ctx_t *, uint64_t);
int mnthr_is_dead(mnthr_ctx_t *);

int mnthr_socket(const char *, const char *, int, int);
int mnthr_socket_bind(const char *, const char *, int);
MNTHR_CPOINT int mnthr_socket_connect(const char *, const char *, int);
MNTHR_CPOINT int mnthr_connect(int, const struct sockaddr *, socklen_t);
MNTHR_CPOINT ssize_t mnthr_get_rbuflen(int);
MNTHR_CPOINT int mnthr_wait_for_read(int);
MNTHR_CPOINT int mnthr_wait_for_write(int);
#define MNTHR_WAIT_EVENT_READ (0x01)
#define MNTHR_WAIT_EVENT_WRITE (0x02)
MNTHR_CPOINT int mnthr_wait_for_events(int, int *);
MNTHR_CPOINT int mnthr_accept_all(int, mnthr_socket_t **, off_t *);
MNTHR_CPOINT int mnthr_accept_all2(int, mnthr_socket_t **, off_t *);
MNTHR_CPOINT int mnthr_read_all(int, char **, off_t *);
MNTHR_CPOINT ssize_t mnthr_read_allb(int, char *, ssize_t);
MNTHR_CPOINT ssize_t mnthr_read_allb_et(int, char *, ssize_t);
MNTHR_CPOINT ssize_t mnthr_recv_allb(int, char *, ssize_t, int);
MNTHR_CPOINT ssize_t mnthr_recvfrom_allb(int,
                                          void * restrict,
                                          ssize_t,
                                          int,
                                          struct sockaddr * restrict,
                                          socklen_t * restrict);
MNTHR_CPOINT ssize_t mnthr_get_wbuflen(int);
MNTHR_CPOINT int mnthr_write_all(int, const char *, size_t);
MNTHR_CPOINT int mnthr_write_all_et(int, const char *, size_t);
MNTHR_CPOINT int mnthr_send_all(int, const char *, size_t, int);
MNTHR_CPOINT int mnthr_sendto_all(int,
                                   const void *,
                                   size_t,
                                   int,
                                   const struct sockaddr *,
                                   socklen_t);
#if 0
MNTHR_CPOINT int mnthr_sendfile_np(int,
                                     int,
                                     off_t,
                                     size_t,
                                     struct sf_hdtr *,
                                     off_t *,
                                     int);
#else
MNTHR_CPOINT int mnthr_sendfile(int,
                                  int,
                                  off_t *,
                                  size_t);

#endif

/*
 *
 */
#define MNTHR_ST_UNKNOWN   (0x00000)
#define MNTHR_ST_DELETE    (0x10000)
#define MNTHR_ST_WRITE     (0x20000)
#define MNTHR_ST_ATTRIB    (0x40000)
typedef struct _mnthr_stat mnthr_stat_t;
mnthr_stat_t *mnthr_stat_new(const char *path);
void mnthr_stat_destroy(mnthr_stat_t **);
MNTHR_CPOINT int mnthr_stat_wait(mnthr_stat_t *);

void mnthr_signal_init(mnthr_signal_t *, mnthr_ctx_t *);
#define MNTHR_SIGNAL_INIT(signal) mnthr_signal_init((signal), NULL)
void mnthr_signal_fini(mnthr_signal_t *);
int mnthr_signal_has_owner(mnthr_signal_t *);
mnthr_ctx_t *mnthr_signal_get_owner(mnthr_signal_t *);
MNTHR_CPOINT int mnthr_signal_subscribe(mnthr_signal_t *);
MNTHR_CPOINT int mnthr_signal_subscribe_with_timeout(mnthr_signal_t *,
                                                      uint64_t);
void mnthr_signal_send(mnthr_signal_t *);
void mnthr_signal_error(mnthr_signal_t *, int);
MNTHR_CPOINT int mnthr_signal_error_and_join(mnthr_signal_t *, int);
#define mnthr_signal_unsubscribe(signal) mnthr_signal_fini((signal));

void mnthr_cond_init(mnthr_cond_t *);
MNTHR_CPOINT int mnthr_cond_wait(mnthr_cond_t *);
void mnthr_cond_signal_all(mnthr_cond_t *);
void mnthr_cond_signal_one(mnthr_cond_t *);
void mnthr_cond_fini(mnthr_cond_t *);

void mnthr_sema_init(mnthr_sema_t *, int);
MNTHR_CPOINT int mnthr_sema_acquire(mnthr_sema_t *);
int mnthr_sema_try_acquire(mnthr_sema_t *);
void mnthr_sema_release(mnthr_sema_t *);
void mnthr_sema_fini(mnthr_sema_t *);

void mnthr_inverted_sema_init(mnthr_inverted_sema_t *, int);
void mnthr_inverted_sema_acquire(mnthr_inverted_sema_t *);
MNTHR_CPOINT int mnthr_inverted_sema_wait(mnthr_inverted_sema_t *);
void mnthr_inverted_sema_release(mnthr_inverted_sema_t *);
void mnthr_inverted_sema_fini(mnthr_inverted_sema_t *);

void mnthr_rwlock_init(mnthr_rwlock_t *);
MNTHR_CPOINT int mnthr_rwlock_acquire_read(mnthr_rwlock_t *);
MNTHR_CPOINT int mnthr_rwlock_try_acquire_read(mnthr_rwlock_t *);
void mnthr_rwlock_release_read(mnthr_rwlock_t *);
MNTHR_CPOINT int mnthr_rwlock_acquire_write(mnthr_rwlock_t *);
MNTHR_CPOINT int mnthr_rwlock_try_acquire_write(mnthr_rwlock_t *);
void mnthr_rwlock_release_write(mnthr_rwlock_t *);
void mnthr_rwlock_fini(mnthr_rwlock_t *);

void mnthr_gen_init(mnthr_gen_t *);
void mnthr_gen_fini(mnthr_gen_t *);
int mnthr_gen_yield(mnthr_gen_t *, void *);
int mnthr_gen_signal(mnthr_gen_t *, int);

#define MNTHR_GEN_RET(gen, res) mnthr_signal_error(&(gen)->s0, (res)); return (res)

#define MNTHR_GEN_YIELD(gen, udata) mnthr_gen_yield(gen, (void *)(udata))

#define MNTHR_GEN_WHILE(res, gen, __a1)                        \
while (((res) = mnthr_signal_subscribe(&(gen)->s0)) == 0)      \
{                                                              \
    __a1;                                                      \
    mnthr_signal_send(&(gen)->s1);                             \
}                                                              \

#define MNTHR_GEN_SIGNAL(gen, rc) mnthr_gen_signal(gen, rc)


uint64_t mnthr_get_now_nsec(void);
#define MNTHR_GET_NOW_SEC() \
    (mnthr_get_now_nsec() / 1000000000)
#define MNTHR_GET_NOW_FSEC() \
    ((double)mnthr_get_now(_nsed) / 1000000000.0)
#define MNTHR_GET_NOW_MSEC() \
    (mnthr_get_now_nsec() / 1000000)
#define MNTHR_GET_NOW_USEC() \
    (mnthr_get_now_nsec() / 1000)

uint64_t mnthr_get_now_nsec_precise(void);
#define MNTHR_GET_NOW_SEC_PRECISE() \
    (mnthr_get_now_nsec_precise() / 1000000000)
#define MNTHR_GET_NOW_FSEC_PRECISE() \
    ((double)mnthr_get_now_nsec_precise() / 1000000000.0)
#define MNTHR_GET_NOW_MSEC_PRECISE() \
    (mnthr_get_now_nsec_precise() / 1000000)
#define MNTHR_GET_NOW_USEC_PRECISE() \
    (mnthr_get_now_nsec_precise() / 1000)

uint64_t mnthr_get_now_ticks(void);
#define MNTHR_GET_NOW_TICKS_SEC() \
    (mnthr_get_now_ticks() / 1000000000)
#define MNTHR_GET_NOW_TICKS_FSEC() \
    ((double)mnthr_get_now_ticks() / 1000000000.0)

uint64_t mnthr_get_now_ticks_precise(void);
#define MNTHR_GET_NOW_TICKS_PRECISE_SEC() \
    (mnthr_get_now_ticks_precise() / 1000000000)
#define MNTHR_GET_NOW_TICKS_PRECISE_FSEC() \
    ((double)mnthr_get_now_ticks_precise() / 1000000000.0)

MNTHR_CPOINT int mnthr_wait_for(uint64_t, const char *, mnthr_cofunc_t, int, ...);
#define MNTHR_WAIT_FOR(timeout, name, f, ...)  \
    mnthr_wait_for(timeout, name, f, MNASZ(__VA_ARGS__), ##__VA_ARGS__)
MNTHR_CPOINT int mnthr_peek(mnthr_ctx_t *, uint64_t);

MNTHR_CPOINT ssize_t mnthr_bytestream_read_more(mnbytestream_t *, void *, ssize_t);
MNTHR_CPOINT ssize_t mnthr_bytestream_read_more_et(mnbytestream_t *, void *, ssize_t);
MNTHR_CPOINT ssize_t mnthr_bytestream_write(mnbytestream_t *, void *, size_t);
MNTHR_CPOINT ssize_t mnthr_bytestream_write_et(mnbytestream_t *, void *, size_t);

#ifdef __cplusplus
}
#endif

#endif
