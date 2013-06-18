#ifndef __CR_PARASITE_H__
#define __CR_PARASITE_H__

#define PARASITE_STACK_SIZE	(16 << 10)
#define PARASITE_ARG_SIZE_MIN	( 1 << 12)

#define PARASITE_MAX_SIZE	(64 << 10)

#ifndef __ASSEMBLY__

#include <sys/un.h>
#include <signal.h>

#include "image.h"
#include "util-net.h"

#include "protobuf/vma.pb-c.h"

#define __head __used __section(.head.text)

enum {
	PARASITE_CMD_IDLE		= 0,
	PARASITE_CMD_ACK,

	PARASITE_CMD_INIT,
	PARASITE_CMD_INIT_THREAD,

	/*
	 * These two must be greater than INITs.
	 */
	PARASITE_CMD_DAEMONIZE,
	PARASITE_CMD_DAEMONIZED,

	PARASITE_CMD_CFG_LOG,
	PARASITE_CMD_FINI,
	PARASITE_CMD_FINI_THREAD,

	PARASITE_CMD_MPROTECT_VMAS,
	PARASITE_CMD_DUMPPAGES,

	PARASITE_CMD_DUMP_SIGACTS,
	PARASITE_CMD_DUMP_ITIMERS,
	PARASITE_CMD_DUMP_MISC,
	PARASITE_CMD_DUMP_CREDS,
	PARASITE_CMD_DUMP_THREAD,
	PARASITE_CMD_DRAIN_FDS,
	PARASITE_CMD_GET_PROC_FD,
	PARASITE_CMD_DUMP_TTY,
	PARASITE_CMD_CHECK_VDSO_MARK,

	PARASITE_CMD_MAX,
};

struct ctl_msg {
	unsigned int	cmd;			/* command itself */
	unsigned int	ack;			/* ack on command */
	int		err;			/* error code on reply */
};

#define ctl_msg_cmd(_cmd)		\
	(struct ctl_msg){.cmd = _cmd, }

#define ctl_msg_ack(_cmd, _err)	\
	(struct ctl_msg){.cmd = _cmd, .ack = _cmd, .err = _err, }

struct parasite_init_args {
	int			h_addr_len;
	struct sockaddr_un	h_addr;

	k_rtsigset_t		sig_blocked;

	struct rt_sigframe	*sigframe;
	stack_t			sas;
};

struct parasite_log_args {
	int log_level;
};

struct parasite_vma_entry
{
	unsigned long	start;
	unsigned long	len;
	int		prot;
};

struct parasite_vdso_vma_entry {
	unsigned long	start;
	unsigned long	len;
	unsigned long	proxy_addr;
	int		is_marked;
};

struct parasite_dump_pages_args {
	unsigned int	nr_vmas;
	unsigned int	add_prot;
	unsigned int	off;
	unsigned int	nr_segs;
	unsigned int	nr_pages;
};

static inline struct parasite_vma_entry *pargs_vmas(struct parasite_dump_pages_args *a)
{
	return (struct parasite_vma_entry *)(a + 1);
}

static inline struct iovec *pargs_iovs(struct parasite_dump_pages_args *a)
{
	return (struct iovec *)(pargs_vmas(a) + a->nr_vmas);
}

struct parasite_dump_sa_args {
	rt_sigaction_t sas[SIGMAX];
};

struct parasite_dump_itimers_args {
	struct itimerval real;
	struct itimerval virt;
	struct itimerval prof;
};

/*
 * Misc sfuff, that is too small for separate file, but cannot
 * be read w/o using parasite
 */

struct parasite_dump_misc {
	unsigned long		brk;

	u32 pid;
	u32 sid;
	u32 pgid;
	u32 tls;
	u32 umask;
};

#define PARASITE_MAX_GROUPS	(PAGE_SIZE / sizeof(unsigned int) - 2 * sizeof(unsigned))

struct parasite_dump_creds {
	unsigned int		secbits;
	unsigned int		ngroups;
	unsigned int		groups[PARASITE_MAX_GROUPS];
};

struct parasite_dump_thread {
	unsigned int		*tid_addr;
	pid_t			tid;
	k_rtsigset_t		blocked;
	u32			tls;
	stack_t			sas;
};

#define PARASITE_MAX_FDS	(PAGE_SIZE / sizeof(int))

struct parasite_drain_fd {
	int	nr_fds;
	int	fds[PARASITE_MAX_FDS];
};

static inline int drain_fds_size(struct parasite_drain_fd *dfds)
{
	return sizeof(dfds->nr_fds) + dfds->nr_fds * sizeof(dfds->fds[0]);
}

struct parasite_tty_args {
	int	fd;

	int	sid;
	int	pgrp;
	bool	hangup;

	int	st_pckt;
	int	st_lock;
	int	st_excl;
};

/* the parasite prefix is added by gen_offsets.sh */
#define parasite_sym(pblob, name) ((void *)(pblob) + parasite_blob_offset__##name)

#endif /* !__ASSEMBLY__ */

#endif /* __CR_PARASITE_H__ */
