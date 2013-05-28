#ifndef __CR_SK_QUEUE_H__
#define __CR_SK_QUEUE_H__

#include "asm/types.h"
#include "list.h"
#include "criu.h"
#include "image.h"

extern int read_sk_queues(void);
extern int dump_sk_queue(int sock_fd, int sock_id);
extern void show_sk_queues(int fd);
extern int restore_sk_queue(int fd, unsigned int peer_id);

#endif /* __CR_SK_QUEUE_H__ */
