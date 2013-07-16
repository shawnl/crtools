#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <linux/sockios.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <string.h>

#include "crtools.h"
#include "util.h"
#include "list.h"
#include "log.h"
#include "asm/types.h"
#include "files.h"
#include "sockets.h"
#include "files.h"
#include "sk-inet.h"
#include "netfilter.h"
#include "image.h"
#include "namespaces.h"
#include "xmalloc.h"
#include "config.h"

#include "protobuf.h"
#include "protobuf/tcp-stream.pb-c.h"

#ifndef CONFIG_HAS_TCP_REPAIR
/*
 * It's been reported that both tcp_repair_opt
 * and TCP_ enum already shipped in netinet/tcp.h
 * system header by some distros thus we need a
 * test if we can use predefined ones or provide
 * our own.
 */
struct tcp_repair_opt {
	u32	opt_code;
	u32	opt_val;
};

enum {
	TCP_NO_QUEUE,
	TCP_RECV_QUEUE,
	TCP_SEND_QUEUE,
	TCP_QUEUES_NR,
};
#endif

#ifndef TCP_TIMESTAMP
#define TCP_TIMESTAMP	24
#endif

#ifndef TCPOPT_SACK_PERM
#define TCPOPT_SACK_PERM TCPOPT_SACK_PERMITTED
#endif

static LIST_HEAD(cpt_tcp_repair_sockets);
static LIST_HEAD(rst_tcp_repair_sockets);

static int tcp_repair_on(int fd)
{
	int ret, aux = 1;

	ret = setsockopt(fd, SOL_TCP, TCP_REPAIR, &aux, sizeof(aux));
	if (ret < 0)
		pr_perror("Can't turn TCP repair mode ON");

	return ret;
}

static int refresh_inet_sk(struct inet_sk_desc *sk)
{
	int size;
	struct tcp_info info;

	if (dump_opt(sk->rfd, SOL_TCP, TCP_INFO, &info)) {
		pr_perror("Failed to obtain TCP_INFO");
		return -1;
	}

	switch (info.tcpi_state) {
	case TCP_ESTABLISHED:
	case TCP_CLOSE:
		break;
	default:
		pr_err("Unknown state %d\n", sk->state);
		return -1;
	}

	if (ioctl(sk->rfd, SIOCOUTQ, &size) == -1) {
		pr_perror("Unable to get size of snd queue");
		return -1;
	}

	sk->wqlen = size;

	if (ioctl(sk->rfd, SIOCINQ, &size) == -1) {
		pr_perror("Unable to get size of recv queue");
		return -1;
	}

	sk->rqlen = size;

	return 0;
}

static int tcp_repair_establised(int fd, struct inet_sk_desc *sk)
{
	int ret;

	pr_info("\tTurning repair on for socket %x\n", sk->sd.ino);
	/*
	 * Keep the socket open in criu till the very end. In
	 * case we close this fd after one task fd dumping and
	 * fail we'll have to turn repair mode off
	 */
	sk->rfd = dup(fd);
	if (sk->rfd < 0) {
		pr_perror("Can't save socket fd for repair");
		goto err1;
	}

	if (!(current_ns_mask & CLONE_NEWNET)) {
		ret = nf_lock_connection(sk);
		if (ret < 0)
			goto err2;
	}

	ret = tcp_repair_on(sk->rfd);
	if (ret < 0)
		goto err3;

	list_add_tail(&sk->rlist, &cpt_tcp_repair_sockets);

	ret = refresh_inet_sk(sk);
	if (ret < 0)
		goto err1;

	return 0;

err3:
	if (!(current_ns_mask & CLONE_NEWNET))
		nf_unlock_connection(sk);
err2:
	close(sk->rfd);
err1:
	return -1;
}

static void tcp_unlock_one(struct inet_sk_desc *sk)
{
	int ret;

	list_del(&sk->rlist);

	ret = nf_unlock_connection(sk);
	if (ret < 0)
		pr_perror("Failed to unlock TCP connection");

	tcp_repair_off(sk->rfd);

	/*
	 * tcp_repair_off modifies SO_REUSEADDR so
	 * don't forget to restore original value.
	 */
	restore_opt(sk->rfd, SOL_SOCKET, SO_REUSEADDR, &sk->cpt_reuseaddr);

	close(sk->rfd);
}

void cpt_unlock_tcp_connections(void)
{
	struct inet_sk_desc *sk, *n;

	list_for_each_entry_safe(sk, n, &cpt_tcp_repair_sockets, rlist)
		tcp_unlock_one(sk);
}

/*
 * TCP queues sequences and their relations to the code below
 *
 *       output queue
 * net <----------------------------- sk
 *        ^       ^       ^    seq >>
 *        snd_una snd_nxt write_seq
 *
 *                     input  queue
 * net -----------------------------> sk
 *     << seq   ^       ^
 *              rcv_nxt copied_seq
 *
 *
 * inq_len  = rcv_nxt - copied_seq = SIOCINQ
 * outq_len = write_seq - snd_una  = SIOCOUTQ
 * inq_seq  = rcv_nxt
 * outq_seq = write_seq
 *
 * On restore kernel moves the option we configure with setsockopt,
 * thus we should advance them on the _len value in restore_tcp_seqs.
 *
 */

static int tcp_stream_get_queue(int sk, int queue_id,
		u32 *seq, u32 len, char **bufp)
{
	int ret, aux;
	socklen_t auxl;
	char *buf;

	pr_debug("\tSet repair queue %d\n", queue_id);
	aux = queue_id;
	auxl = sizeof(aux);
	ret = setsockopt(sk, SOL_TCP, TCP_REPAIR_QUEUE, &aux, auxl);
	if (ret < 0)
		goto err_sopt;

	pr_debug("\tGet queue seq\n");
	auxl = sizeof(*seq);
	ret = getsockopt(sk, SOL_TCP, TCP_QUEUE_SEQ, seq, &auxl);
	if (ret < 0)
		goto err_sopt;

	pr_info("\t`- seq %u len %u\n", *seq, len);

	if (len) {
		/*
		 * Try to grab one byte more from the queue to
		 * make sure there are len bytes for real
		 */
		buf = xmalloc(len + 1);
		if (!buf)
			goto err_buf;

		pr_debug("\tReading queue (%d bytes)\n", len);
		ret = recv(sk, buf, len + 1, MSG_PEEK | MSG_DONTWAIT);
		if (ret != len)
			goto err_recv;
	} else
		buf = NULL;

	*bufp = buf;
	return 0;

err_sopt:
	pr_perror("\tsockopt failed");
err_buf:
	return -1;

err_recv:
	pr_perror("\trecv failed (%d, want %d, errno %d)", ret, len, errno);
	xfree(buf);
	goto err_buf;
}

static int tcp_stream_get_options(int sk, TcpStreamEntry *tse)
{
	int ret;
	socklen_t auxl;
	struct tcp_info ti;
	int val;

	auxl = sizeof(ti);
	ret = getsockopt(sk, SOL_TCP, TCP_INFO, &ti, &auxl);
	if (ret < 0)
		goto err_sopt;

	auxl = sizeof(tse->mss_clamp);
	ret = getsockopt(sk, SOL_TCP, TCP_MAXSEG, &tse->mss_clamp, &auxl);
	if (ret < 0)
		goto err_sopt;

	tse->opt_mask = ti.tcpi_options;
	if (ti.tcpi_options & TCPI_OPT_WSCALE) {
		tse->snd_wscale = ti.tcpi_snd_wscale;
		tse->rcv_wscale = ti.tcpi_rcv_wscale;
		tse->has_rcv_wscale = true;
	}

	if (ti.tcpi_options & TCPI_OPT_TIMESTAMPS) {
		auxl = sizeof(val);
		ret = getsockopt(sk, SOL_TCP, TCP_TIMESTAMP, &val, &auxl);
		if (ret < 0)
			goto err_sopt;

		tse->has_timestamp = true;
		tse->timestamp = val;
	}

	pr_info("\toptions: mss_clamp %x wscale %x tstamp %d sack %d\n",
			(int)tse->mss_clamp,
			ti.tcpi_options & TCPI_OPT_WSCALE ? (int)tse->snd_wscale : -1,
			ti.tcpi_options & TCPI_OPT_TIMESTAMPS ? 1 : 0,
			ti.tcpi_options & TCPI_OPT_SACK ? 1 : 0);

	return 0;

err_sopt:
	pr_perror("\tsockopt failed");
	return -1;
}

static int dump_tcp_conn_state(struct inet_sk_desc *sk)
{
	int ret, img_fd;
	TcpStreamEntry tse = TCP_STREAM_ENTRY__INIT;
	char *in_buf, *out_buf;

	/*
	 * Read queue
	 */

	pr_info("Reading inq for socket\n");
	tse.inq_len = sk->rqlen;
	ret = tcp_stream_get_queue(sk->rfd, TCP_RECV_QUEUE,
			&tse.inq_seq, tse.inq_len, &in_buf);
	if (ret < 0)
		goto err_in;

	/*
	 * Write queue
	 */

	pr_info("Reading outq for socket\n");
	tse.outq_len = sk->wqlen;
	ret = tcp_stream_get_queue(sk->rfd, TCP_SEND_QUEUE,
			&tse.outq_seq, tse.outq_len, &out_buf);
	if (ret < 0)
		goto err_out;

	/*
	 * Initial options
	 */

	pr_info("Reading options for socket\n");
	ret = tcp_stream_get_options(sk->rfd, &tse);
	if (ret < 0)
		goto err_opt;

	/*
	 * Push the stuff to image
	 */

	img_fd = open_image(CR_FD_TCP_STREAM, O_DUMP, sk->sd.ino);
	if (img_fd < 0)
		goto err_img;

	ret = pb_write_one(img_fd, &tse, PB_TCP_STREAM);
	if (ret < 0)
		goto err_iw;

	if (in_buf) {
		ret = write_img_buf(img_fd, in_buf, tse.inq_len);
		if (ret < 0)
			goto err_iw;
	}

	if (out_buf) {
		ret = write_img_buf(img_fd, out_buf, tse.outq_len);
		if (ret < 0)
			goto err_iw;
	}

	pr_info("Done\n");
err_iw:
	close(img_fd);
err_img:
err_opt:
	xfree(out_buf);
err_out:
	xfree(in_buf);
err_in:
	return ret;
}

int dump_one_tcp(int fd, struct inet_sk_desc *sk)
{
	if (sk->state != TCP_ESTABLISHED)
		return 0;

	pr_info("Dumping TCP connection\n");

	if (tcp_repair_establised(fd, sk))
		return -1;

	if (dump_tcp_conn_state(sk))
		return -1;

	/*
	 * Socket is left in repair mode, so that at the end it's just
	 * closed and the connection is silently terminated
	 */
	return 0;
}

static int set_tcp_queue_seq(int sk, int queue, u32 seq)
{
	pr_debug("\tSetting %d queue seq to %u\n", queue, seq);

	if (setsockopt(sk, SOL_TCP, TCP_REPAIR_QUEUE, &queue, sizeof(queue)) < 0) {
		pr_perror("Can't set repair queue");
		return -1;
	}

	if (setsockopt(sk, SOL_TCP, TCP_QUEUE_SEQ, &seq, sizeof(seq)) < 0) {
		pr_perror("Can't set queue seq");
		return -1;
	}

	return 0;
}

static int restore_tcp_seqs(int sk, TcpStreamEntry *tse)
{
	if (set_tcp_queue_seq(sk, TCP_RECV_QUEUE,
				tse->inq_seq - tse->inq_len))
		return -1;
	if (set_tcp_queue_seq(sk, TCP_SEND_QUEUE,
				tse->outq_seq - tse->outq_len))
		return -1;

	return 0;
}

static int send_tcp_queue(int sk, int queue, u32 len, int imgfd)
{
	int ret, err = -1;
	char *buf;

	pr_debug("\tRestoring TCP %d queue data %u bytes\n", queue, len);

	if (setsockopt(sk, SOL_TCP, TCP_REPAIR_QUEUE, &queue, sizeof(queue)) < 0) {
		pr_perror("Can't set repair queue");
		return -1;
	}

	buf = xmalloc(len);
	if (!buf)
		return -1;

	if (read_img_buf(imgfd, buf, len) < 0)
		goto err;

	ret = send(sk, buf, len, 0);
	if (ret != len) {
		pr_perror("Can't restore %d queue data (%d), want %d",
				queue, ret, len);
		goto err;
	}

	err = 0;
err:
	xfree(buf);

	return err;
}

static int restore_tcp_queues(int sk, TcpStreamEntry *tse, int fd)
{
	if (restore_prepare_socket(sk))
		return -1;

	if (tse->inq_len &&
			send_tcp_queue(sk, TCP_RECV_QUEUE, tse->inq_len, fd))
		return -1;
	if (tse->outq_len &&
			send_tcp_queue(sk, TCP_SEND_QUEUE, tse->outq_len, fd))
		return -1;

	return 0;
}

static int restore_tcp_opts(int sk, TcpStreamEntry *tse)
{
	struct tcp_repair_opt opts[4];
	int onr = 0;

	pr_debug("\tRestoring TCP options\n");

	if (tse->opt_mask & TCPI_OPT_SACK) {
		pr_debug("\t\tWill turn SAK on\n");
		opts[onr].opt_code = TCPOPT_SACK_PERM;
		opts[onr].opt_val = 0;
		onr++;
	}

	if (tse->opt_mask & TCPI_OPT_WSCALE) {
		pr_debug("\t\tWill set snd_wscale to %u\n", tse->snd_wscale);
		pr_debug("\t\tWill set rcv_wscale to %u\n", tse->rcv_wscale);
		opts[onr].opt_code = TCPOPT_WINDOW;
		opts[onr].opt_val = tse->snd_wscale + (tse->rcv_wscale << 16);
		onr++;
	}

	if (tse->opt_mask & TCPI_OPT_TIMESTAMPS) {
		pr_debug("\t\tWill turn timestamps on\n");
		opts[onr].opt_code = TCPOPT_TIMESTAMP;
		opts[onr].opt_val = 0;
		onr++;
	}

	pr_debug("Will set mss clamp to %u\n", tse->mss_clamp);
	opts[onr].opt_code = TCPOPT_MAXSEG;
	opts[onr].opt_val = tse->mss_clamp;
	onr++;

	if (setsockopt(sk, SOL_TCP, TCP_REPAIR_OPTIONS,
				opts, onr * sizeof(struct tcp_repair_opt)) < 0) {
		pr_perror("Can't repair options");
		return -1;
	}

	if (tse->has_timestamp) {
		if (setsockopt(sk, SOL_TCP, TCP_TIMESTAMP,
				&tse->timestamp, sizeof(tse->timestamp)) < 0) {
			pr_perror("Can't set timestamp");
			return -1;
		}
	}

	return 0;
}

static int restore_tcp_conn_state(int sk, struct inet_sk_info *ii)
{
	int ifd;
	TcpStreamEntry *tse;

	pr_info("Restoring TCP connection id %x ino %x\n", ii->ie->id, ii->ie->ino);

	ifd = open_image(CR_FD_TCP_STREAM, O_RSTR, ii->ie->ino);
	if (ifd < 0)
		goto err;

	if (pb_read_one(ifd, &tse, PB_TCP_STREAM) < 0)
		goto err_c;

	if (restore_tcp_seqs(sk, tse))
		goto err_c;

	if (inet_bind(sk, ii))
		goto err_c;

	if (inet_connect(sk, ii))
		goto err_c;

	if (restore_tcp_opts(sk, tse))
		goto err_c;

	if (restore_tcp_queues(sk, tse, ifd))
		goto err_c;

	tcp_stream_entry__free_unpacked(tse, NULL);
	close(ifd);
	return 0;

err_c:
	tcp_stream_entry__free_unpacked(tse, NULL);
	close(ifd);
err:
	return -1;
}

/*
 * rst_tcp_socks contains sockets in repair mode,
 * which will be off in restorer before resuming.
 */
struct rst_tcp_sock *rst_tcp_socks = NULL;
int rst_tcp_socks_nr = 0;

int rst_tcp_socks_add(int fd, bool reuseaddr)
{
	struct rst_tcp_sock *cur;

	rst_tcp_socks_nr++;
	rst_tcp_socks = xrealloc(rst_tcp_socks, rst_tcp_socks_len());
	if (!rst_tcp_socks)
		return -1;

	pr_debug("Schedule %d socket for repair off\n", fd);
	cur = &rst_tcp_socks[rst_tcp_socks_nr - 1];
	cur->sk = fd;
	cur->reuseaddr = reuseaddr;

	return 0;
}

int restore_one_tcp(int fd, struct inet_sk_info *ii)
{
	pr_info("Restoring TCP connection\n");

	if (tcp_repair_on(fd))
		return -1;

	if (restore_tcp_conn_state(fd, ii))
		return -1;

	return 0;
}

void tcp_locked_conn_add(struct inet_sk_info *ii)
{
	list_add_tail(&ii->rlist, &rst_tcp_repair_sockets);
}

void rst_unlock_tcp_connections(void)
{
	struct inet_sk_info *ii;

	list_for_each_entry(ii, &rst_tcp_repair_sockets, rlist)
		nf_unlock_connection_info(ii);
}

void show_tcp_stream(int fd)
{
	pb_show_plain_pretty(fd, PB_TCP_STREAM, "1:%u 2:%u 3:%u 4:%u");
}

int check_tcp(void)
{
	socklen_t optlen;
	int sk, ret;
	int val;

	sk = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sk < 0) {
		pr_perror("Can't create TCP socket :(");
		return -1;
	}

	ret = tcp_repair_on(sk);
	if (ret)
		goto out;

	optlen = sizeof(val);
	ret = getsockopt(sk, SOL_TCP, TCP_TIMESTAMP, &val, &optlen);
	if (ret)
		pr_perror("Can't get TCP_TIMESTAMP");

out:
	close(sk);

	return ret;
}
