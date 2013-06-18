#include <sys/mman.h>
#include <errno.h>
#include <signal.h>
#include <linux/limits.h>
#include <sys/mount.h>
#include <stdarg.h>
#include <sys/ioctl.h>

#include "syscall.h"
#include "parasite.h"
#include "lock.h"
#include "vdso.h"
#include "log.h"

#include <string.h>

#include "asm/types.h"
#include "asm/parasite.h"
#include "asm/restorer.h"

static int tsock = -1;

static struct rt_sigframe *sigframe;

#ifndef SPLICE_F_GIFT
#define SPLICE_F_GIFT	0x08
#endif

static int mprotect_vmas(struct parasite_dump_pages_args *args)
{
	struct parasite_vma_entry *vmas, *vma;
	int ret = 0, i;

	vmas = pargs_vmas(args);
	for (i = 0; i < args->nr_vmas; i++) {
		vma = vmas + i;
		ret = sys_mprotect((void *)vma->start, vma->len, vma->prot | args->add_prot);
		if (ret) {
			pr_err("mprotect(%08lx, %lu) failed with code %d\n",
						vma->start, vma->len, ret);
			break;
		}
	}

	return ret;
}

static int dump_pages(struct parasite_dump_pages_args *args)
{
	int p, ret;
	struct iovec *iovs;

	p = recv_fd(tsock);
	if (p < 0)
		return -1;

	iovs = pargs_iovs(args);
	ret = sys_vmsplice(p, &iovs[args->off], args->nr_segs,
				SPLICE_F_GIFT | SPLICE_F_NONBLOCK);
	if (ret != PAGE_SIZE * args->nr_pages) {
		sys_close(p);
		pr_err("Can't splice pages ti pipe (%d/%d)", ret, args->nr_pages);
		return -1;
	}

	sys_close(p);
	return 0;
}

static int dump_sigact(struct parasite_dump_sa_args *da)
{
	int sig, ret = 0;

	for (sig = 1; sig <= SIGMAX; sig++) {
		int i = sig - 1;

		if (sig == SIGKILL || sig == SIGSTOP)
			continue;

		ret = sys_sigaction(sig, NULL, &da->sas[i], sizeof(k_rtsigset_t));
		if (ret < 0) {
			pr_err("sys_sigaction failed\n");
			break;
		}
	}

	return ret;
}

static int dump_itimers(struct parasite_dump_itimers_args *args)
{
	int ret;

	ret = sys_getitimer(ITIMER_REAL, &args->real);
	if (!ret)
		ret = sys_getitimer(ITIMER_VIRTUAL, &args->virt);
	if (!ret)
		ret = sys_getitimer(ITIMER_PROF, &args->prof);

	if (ret)
		pr_err("getitimer failed\n");

	return ret;
}

static int dump_misc(struct parasite_dump_misc *args)
{
	args->brk = sys_brk(0);

	args->pid = sys_getpid();
	args->sid = sys_getsid();
	args->pgid = sys_getpgid(0);
	args->tls = arch_get_tls();
	args->umask = sys_umask(0);
	sys_umask(args->umask); /* never fails */

	return 0;
}

static int dump_creds(struct parasite_dump_creds *args)
{
	int ret;

	args->secbits = sys_prctl(PR_GET_SECUREBITS, 0, 0, 0, 0);

	ret = sys_getgroups(0, NULL);
	if (ret < 0)
		goto grps_err;

	args->ngroups = ret;
	if (args->ngroups >= PARASITE_MAX_GROUPS) {
		pr_err("Too many groups in task %d\n", (int)args->ngroups);
		return -1;
	}

	ret = sys_getgroups(args->ngroups, args->groups);
	if (ret < 0)
		goto grps_err;

	if (ret != args->ngroups) {
		pr_err("Groups changed on the fly %d -> %d\n",
				args->ngroups, ret);
		return -1;
	}

	return 0;

grps_err:
	pr_err("Error calling getgroups (%d)\n", ret);
	return -1;
}

static int drain_fds(struct parasite_drain_fd *args)
{
	int ret;

	ret = send_fds(tsock, NULL, 0,
		       args->fds, args->nr_fds, true);
	if (ret)
		pr_err("send_fds failed\n");

	return ret;
}

static int dump_thread(struct parasite_dump_thread *args)
{
	pid_t tid = sys_gettid();
	int ret;

	ret = sys_prctl(PR_GET_TID_ADDRESS, (unsigned long) &args->tid_addr, 0, 0, 0);
	if (ret)
		return ret;

	args->tid = tid;
	args->tls = arch_get_tls();

	return 0;
}

static int init_thread(struct parasite_dump_thread *args)
{
	k_rtsigset_t to_block;
	pid_t tid = sys_gettid();
	int ret;

	ksigfillset(&to_block);
	ret = sys_sigprocmask(SIG_SETMASK, &to_block,
			      &args->blocked,
			      sizeof(k_rtsigset_t));
	if (ret)
		return -1;

	ret = sys_prctl(PR_GET_TID_ADDRESS, (unsigned long) &args->tid_addr, 0, 0, 0);
	if (ret)
		goto err;

	args->tid = tid;
	args->tls = arch_get_tls();

	ret = sys_sigaltstack(NULL, &args->sas);
	if (ret)
		goto err;

	return ret;
err:
	sys_sigprocmask(SIG_SETMASK, &args->blocked,
				NULL, sizeof(k_rtsigset_t));
	return ret;
}

static int fini_thread(struct parasite_dump_thread *args)
{
	return sys_sigprocmask(SIG_SETMASK, &args->blocked,
				NULL, sizeof(k_rtsigset_t));
}

static int init(struct parasite_init_args *args)
{
	k_rtsigset_t to_block;
	int ret;

	sigframe = args->sigframe;

	ksigfillset(&to_block);
	ret = sys_sigprocmask(SIG_SETMASK, &to_block,
			      &args->sig_blocked,
			      sizeof(k_rtsigset_t));
	if (ret)
		return -1;

	ret = sys_sigaltstack(NULL, &args->sas);
	if (ret)
		goto err;

	tsock = sys_socket(PF_UNIX, SOCK_STREAM, 0);
	if (tsock < 0) {
		ret = tsock;
		goto err;
	}

	ret = sys_connect(tsock, (struct sockaddr *)&args->h_addr, args->h_addr_len);
	if (ret < 0)
		goto err;

	return 0;
err:
	sys_sigprocmask(SIG_SETMASK, &args->sig_blocked,
				NULL, sizeof(k_rtsigset_t));
	return ret;
}

static char proc_mountpoint[] = "proc.crtools";
static int parasite_get_proc_fd()
{
	int ret, fd = -1;
	char buf[2];

	ret = sys_readlink("/proc/self", buf, sizeof(buf));
	if (ret < 0 && ret != -ENOENT) {
		pr_err("Can't readlink /proc/self\n");
		return ret;
	}

	/* Fast path -- if /proc belongs to this pidns */
	if (ret == 1 && buf[0] == '1') {
		fd = sys_open("/proc", O_RDONLY, 0);
		goto out_send_fd;
	}

	if (sys_mkdir(proc_mountpoint, 0700)) {
		pr_err("Can't create a directory\n");
		return -1;
	}

	if (sys_mount("proc", proc_mountpoint, "proc", MS_MGC_VAL, NULL)) {
		pr_err("mount failed\n");
		goto out_rmdir;
	}

	fd = sys_open(proc_mountpoint, O_RDONLY, 0);

	if (sys_umount2(proc_mountpoint, MNT_DETACH)) {
		pr_err("Can't umount procfs\n");
		return -1;
	}

out_rmdir:
	if (sys_rmdir(proc_mountpoint)) {
		pr_err("Can't remove directory\n");
		return -1;
	}

out_send_fd:
	if (fd < 0)
		return fd;
	ret = send_fd(tsock, NULL, 0, fd);
	sys_close(fd);
	return ret;
}

static inline int tty_ioctl(int fd, int cmd, int *arg)
{
	int ret;

	ret = sys_ioctl(fd, cmd, (unsigned long)arg);
	if (ret < 0) {
		if (ret != -ENOTTY)
			return ret;
		*arg = 0;
	}
	return 0;
}

static int parasite_dump_tty(struct parasite_tty_args *args)
{
	int ret;

#ifndef TIOCGPKT
# define TIOCGPKT	_IOR('T', 0x38, int)
#endif

#ifndef TIOCGPTLCK
# define TIOCGPTLCK	_IOR('T', 0x39, int)
#endif

#ifndef TIOCGEXCL
# define TIOCGEXCL	_IOR('T', 0x40, int)
#endif

	ret = tty_ioctl(args->fd, TIOCGSID, &args->sid);
	if (ret < 0)
		goto err;

	ret = tty_ioctl(args->fd, TIOCGPGRP, &args->pgrp);
	if (ret < 0)
		goto err;

	ret = tty_ioctl(args->fd, TIOCGPKT, &args->st_pckt);
	if (ret < 0)
		goto err;

	ret = tty_ioctl(args->fd, TIOCGPTLCK, &args->st_lock);
	if (ret < 0)
		goto err;

	ret = tty_ioctl(args->fd, TIOCGEXCL, &args->st_excl);
	if (ret < 0)
		goto err;

	args->hangup = false;
	return 0;

err:
	if (ret != -EIO) {
		pr_err("TTY: Can't get sid/pgrp: %d\n", ret);
		return -1;
	}

	/* kernel reports EIO for get ioctls on pair-less ptys */
	args->sid = 0;
	args->pgrp = 0;
	args->st_pckt = 0;
	args->st_lock = 0;
	args->st_excl = 0;
	args->hangup = true;

	return 0;
}

static int parasite_cfg_log(struct parasite_log_args *args)
{
	int ret;

	ret = recv_fd(tsock);
	if (ret >= 0) {
		log_set_fd(ret);
		log_set_loglevel(args->log_level);
		ret = 0;
	}

	return ret;
}

static int parasite_check_vdso_mark(struct parasite_vdso_vma_entry *args)
{
	struct vdso_mark *m = (void *)args->start;

	if (is_vdso_mark(m)) {
		args->is_marked = 1;
		args->proxy_addr = m->proxy_addr;
	} else {
		args->is_marked = 0;
		args->proxy_addr = VDSO_BAD_ADDR;
	}

	return 0;
}

static int __parasite_daemon_reply_ack(unsigned int cmd, int err)
{
	struct ctl_msg m;
	int ret;

	m = ctl_msg_ack(cmd, err);
	ret = sys_sendto(tsock, &m, sizeof(m), 0, NULL, 0);
	if (ret != sizeof(m)) {
		pr_err("Sent only %d bytes while %d expected\n",
			ret, (int)sizeof(m));
		return -1;
	}

	pr_debug("__sent ack msg: %d %d %d\n",
		 m.cmd, m.ack, m.err);

	return 0;
}

static int __parasite_daemon_wait_msg(struct ctl_msg *m)
{
	int ret;

	pr_debug("Daemon wais for command\n");

	while (1) {
		*m = (struct ctl_msg){ };
		ret = sys_recvfrom(tsock, m, sizeof(*m), MSG_WAITALL, NULL, 0);
		if (ret != sizeof(*m)) {
			pr_err("Trimmed message received (%d/%d)\n",
			       (int)sizeof(*m), ret);
			return -1;
		}

		pr_debug("__fetched msg: %d %d %d\n",
			 m->cmd, m->ack, m->err);
		return 0;
	}

	return -1;
}

static int fini()
{
	unsigned long new_sp;

	new_sp = (long)sigframe + SIGFRAME_OFFSET;
	pr_debug("%ld: new_sp=%lx ip %lx\n", sys_gettid(),
		  new_sp, RT_SIGFRAME_REGIP(sigframe));

	sys_close(tsock);
	log_set_fd(-1);

	ARCH_RT_SIGRETURN(new_sp);

	BUG();

	return -1;
}

static noinline __used int noinline parasite_daemon(void *args)
{
	struct ctl_msg m = { };
	int ret = -1;

	pr_debug("Running daemon thread leader\n");

	/* Reply we're alive */
	if (__parasite_daemon_reply_ack(PARASITE_CMD_DAEMONIZE, 0))
		goto out;

	ret = 0;

	while (1) {
		if (__parasite_daemon_wait_msg(&m))
			break;

		if (ret && m.cmd != PARASITE_CMD_FINI) {
			pr_err("Command rejected\n");
			continue;
		}

		switch (m.cmd) {
		case PARASITE_CMD_FINI:
			ret = fini();
			sys_close(tsock);
			/*
			 * No ACK here since we're getting out.
			 */
			break;
		case PARASITE_CMD_DUMP_THREAD:
			ret = dump_thread(args);
			break;
		case PARASITE_CMD_DUMPPAGES:
			ret = dump_pages(args);
			break;
		case PARASITE_CMD_MPROTECT_VMAS:
			ret = mprotect_vmas(args);
			break;
		case PARASITE_CMD_DUMP_SIGACTS:
			ret = dump_sigact(args);
			break;
		case PARASITE_CMD_DUMP_ITIMERS:
			ret = dump_itimers(args);
			break;
		case PARASITE_CMD_DUMP_MISC:
			ret = dump_misc(args);
			break;
		case PARASITE_CMD_DUMP_CREDS:
			ret = dump_creds(args);
			break;
		case PARASITE_CMD_DRAIN_FDS:
			ret = drain_fds(args);
			break;
		case PARASITE_CMD_GET_PROC_FD:
			ret = parasite_get_proc_fd();
			break;
		case PARASITE_CMD_DUMP_TTY:
			ret = parasite_dump_tty(args);
			break;
		case PARASITE_CMD_CHECK_VDSO_MARK:
			ret = parasite_check_vdso_mark(args);
			break;
		default:
			pr_err("Unknown command in parasite daemon thread leader: %d\n", m.cmd);
			ret = -1;
			break;
		}

		if (__parasite_daemon_reply_ack(m.cmd, ret))
			break;

		if (ret) {
			pr_err("Close the control socket for writing\n");
			sys_shutdown(tsock, SHUT_WR);
		}
	}

out:
	fini();

	return 0;
}

int __used parasite_service(unsigned int cmd, void *args)
{
	pr_info("Parasite cmd %d/%x process\n", cmd, cmd);

	switch (cmd) {
	case PARASITE_CMD_INIT:
		return init(args);
	case PARASITE_CMD_INIT_THREAD:
		return init_thread(args);
	case PARASITE_CMD_FINI_THREAD:
		return fini_thread(args);
	case PARASITE_CMD_CFG_LOG:
		return parasite_cfg_log(args);
	case PARASITE_CMD_DAEMONIZE:
		return parasite_daemon(args);
	}

	pr_err("Unknown command to parasite: %d\n", cmd);
	return -EINVAL;
}
