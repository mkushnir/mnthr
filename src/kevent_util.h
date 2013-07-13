#ifndef KEVENT_UTIL_H
#define KEVENT_UTIL_H

#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>

#include "mrkcommon/dumpm.h"

#ifdef __cplusplus
extern "C" {
#endif

int kevent_dump(void *);
#define KEVENT_DUMP(kev) kevent_dump((kev))

const char * kevent_filter_str(int);
int kevent_init(void *data);
int kevent_fini(void *data);
int kevent_isempty(struct kevent *);
void kevent_copy(struct kevent *, struct kevent *);

/* misc */
int kev_write(struct kevent *, unsigned char *, ssize_t);
int kev_read(struct kevent *, unsigned char *, ssize_t);

#ifdef __cplusplus
}
#endif

#endif
