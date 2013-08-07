#ifndef MRKTHR_H
#define MRKTHR_H

#include <netinet/in.h>
#include <sys/socket.h>

#include "mrkcommon/dtqueue.h"
#include "mrkcommon/dumpm.h"
#include "mrkcommon/util.h"
#include "mrkcommon/bytestream.h"

#ifdef __cplusplus
extern "C" {
#endif


#define CTRACE(s, ...) TRACE("[% 4d] " s, mrkthr_id(), ##__VA_ARGS__)

typedef int (*cofunc)(int, void *[]);
typedef struct _mrkthr_ctx mrkthr_ctx_t;

#ifndef MRKTHR_WAITQ_T_DEFINED
typedef DTQUEUE(_mrkthr_ctx, mrkthr_waitq_t);
#define MRKTHR_WAITQ_T_DEFINED
#endif

#define MRKTHR_WAIT_TIMEOUT (-1)
#define MRKTHR_JOIN_FAILURE (-2)

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

typedef struct _mrkthr_rwlock {
    mrkthr_cond_t cond;
    int fwriter:1;
    unsigned nreaders;
} mrkthr_rwlock_t;

int mrkthr_init(void);
int mrkthr_fini(void);
int mrkthr_loop(void);
int wallclock_init(void);
void mrkthr_shutdown(void);
size_t mrkthr_compact_sleepq(size_t);
size_t mrkthr_get_sleepq_length(void);
size_t mrkthr_get_sleepq_volume(void);
void mrkthr_dump_all_ctxes(void);
void mrkthr_dump_sleepq(void);

int mrkthr_dump(const mrkthr_ctx_t *);
const char *mrkthr_strerror(int);
mrkthr_ctx_t *mrkthr_new(const char *, cofunc, int, ...);
mrkthr_ctx_t *mrkthr_spawn(const char *name, cofunc f, int argc, ...);
PRINTFLIKE(2, 3) int mrkthr_set_name(mrkthr_ctx_t *, const char *, ...);
mrkthr_ctx_t *mrkthr_me(void);
int mrkthr_id(void);
int mrkthr_sleep(uint64_t);
long double mrkthr_ticks2sec(uint64_t);
int mrkthr_join(mrkthr_ctx_t *);
void mrkthr_run(mrkthr_ctx_t *);
void mrkthr_set_interrupt(mrkthr_ctx_t *);
int mrkthr_set_interrupt_and_join(mrkthr_ctx_t *);
int mrkthr_is_dead(mrkthr_ctx_t *);

ssize_t mrkthr_get_rbuflen(int);
int mrkthr_accept_all(int, mrkthr_socket_t **, off_t *);
int mrkthr_read_all(int, char **, off_t *);
ssize_t mrkthr_read_allb(int, char *, ssize_t);
ssize_t mrkthr_recvfrom_allb(int, void * restrict, ssize_t, int, struct sockaddr * restrict, socklen_t * restrict);
ssize_t mrkthr_get_wbuflen(int);
int mrkthr_write_all(int, const char *, size_t);
int mrkthr_sendto_all(int, const void *, size_t, int, const struct sockaddr *, socklen_t);

void mrkthr_signal_init(mrkthr_signal_t *, mrkthr_ctx_t *);
void mrkthr_signal_fini(mrkthr_signal_t *);
int mrkthr_signal_has_owner(mrkthr_signal_t *);
int mrkthr_signal_subscribe(mrkthr_signal_t *);
void mrkthr_signal_send(mrkthr_signal_t *);

void mrkthr_cond_init(mrkthr_cond_t *);
int mrkthr_cond_wait(mrkthr_cond_t *);
void mrkthr_cond_signal_all(mrkthr_cond_t *);
void mrkthr_cond_signal_one(mrkthr_cond_t *);
void mrkthr_cond_fini(mrkthr_cond_t *);

void mrkthr_sema_init(mrkthr_sema_t *, int);
int mrkthr_sema_acquire(mrkthr_sema_t *);
void mrkthr_sema_release(mrkthr_sema_t *);
void mrkthr_sema_fini(mrkthr_sema_t *);

void mrkthr_rwlock_init(mrkthr_rwlock_t *);
int mrkthr_rwlock_acquire_read(mrkthr_rwlock_t *);
void mrkthr_rwlock_release_read(mrkthr_rwlock_t *);
int mrkthr_rwlock_acquire_write(mrkthr_rwlock_t *);
void mrkthr_rwlock_release_write(mrkthr_rwlock_t *);
void mrkthr_rwlock_fini(mrkthr_rwlock_t *);


uint64_t mrkthr_get_now(void);
uint64_t mrkthr_get_now_precise(void);
uint64_t mrkthr_get_now_ticks(void);
uint64_t mrkthr_get_now_ticks_precise(void);

int mrkthr_wait_for(uint64_t, const char *, cofunc, int, ...);

ssize_t mrkthr_bytestream_read_more(bytestream_t *, int, ssize_t);
ssize_t mrkthr_bytestream_write(bytestream_t *, int, size_t);

#ifdef __cplusplus
}
#endif

#endif
