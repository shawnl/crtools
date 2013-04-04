#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>

#include <sys/types.h>
#include <sys/stat.h>

#include <fcntl.h>
#include <arpa/inet.h>

#include <google/protobuf-c/protobuf-c.h>

#include "compiler.h"
#include "xmalloc.h"
#include "types.h"
#include "log.h"

#include "protobuf.h"

#include "../../../protobuf/inventory.pb-c.h"
#include "../../../protobuf/regfile.pb-c.h"
#include "../../../protobuf/eventfd.pb-c.h"
#include "../../../protobuf/eventpoll.pb-c.h"
#include "../../../protobuf/signalfd.pb-c.h"
#include "../../../protobuf/fsnotify.pb-c.h"
#include "../../../protobuf/core.pb-c.h"
#include "../../../protobuf/mm.pb-c.h"
#include "../../../protobuf/pipe.pb-c.h"
#include "../../../protobuf/fifo.pb-c.h"
#include "../../../protobuf/fdinfo.pb-c.h"
#include "../../../protobuf/pipe-data.pb-c.h"
#include "../../../protobuf/pstree.pb-c.h"
#include "../../../protobuf/sa.pb-c.h"
#include "../../../protobuf/sk-unix.pb-c.h"
#include "../../../protobuf/sk-inet.pb-c.h"
#include "../../../protobuf/packet-sock.pb-c.h"
#include "../../../protobuf/sk-packet.pb-c.h"
#include "../../../protobuf/creds.pb-c.h"
#include "../../../protobuf/itimer.pb-c.h"
#include "../../../protobuf/utsns.pb-c.h"
#include "../../../protobuf/ipc-var.pb-c.h"
#include "../../../protobuf/ipc-shm.pb-c.h"
#include "../../../protobuf/ipc-msg.pb-c.h"
#include "../../../protobuf/ipc-sem.pb-c.h"
#include "../../../protobuf/fs.pb-c.h"
#include "../../../protobuf/remap-file-path.pb-c.h"
#include "../../../protobuf/ghost-file.pb-c.h"
#include "../../../protobuf/mnt.pb-c.h"
#include "../../../protobuf/netdev.pb-c.h"
#include "../../../protobuf/tcp-stream.pb-c.h"
#include "../../../protobuf/tty.pb-c.h"
#include "../../../protobuf/file-lock.pb-c.h"
#include "../../../protobuf/rlimit.pb-c.h"
#include "../../../protobuf/vma.pb-c.h"
#include "../../../protobuf/pagemap.pb-c.h"
#include "../../../protobuf/siginfo.pb-c.h"
#include "../../../protobuf/sk-netlink.pb-c.h"

typedef size_t (*pb_getpksize_t)(void *obj);
typedef size_t (*pb_pack_t)(void *obj, void *where);
typedef void  *(*pb_unpack_t)(void *allocator, size_t size, void *from);
typedef void   (*pb_free_t)(void *obj, void *allocator);

struct cr_pb_message_desc {
	pb_getpksize_t				getpksize;
	pb_pack_t				pack;
	pb_unpack_t				unpack;
	pb_free_t				free;
	const ProtobufCMessageDescriptor	*pb_desc;
};

#define PB_PACK_TYPECHECK(__o, __fn)	({ if (0) __fn##__pack(__o, NULL); (pb_pack_t)&__fn##__pack; })
#define PB_GPS_TYPECHECK(__o, __fn)	({ if (0) __fn##__get_packed_size(__o); (pb_getpksize_t)&__fn##__get_packed_size; })
#define PB_UNPACK_TYPECHECK(__op, __fn)	({ if (0) *__op = __fn##__unpack(NULL, 0, NULL); (pb_unpack_t)&__fn##__unpack; })
#define PB_FREE_TYPECHECK(__o, __fn)	({ if (0) __fn##__free_unpacked(__o, NULL); (pb_free_t)&__fn##__free_unpacked; })

/*
 * This should be explicitly "called" to do type-checking
 */

#define CR_PB_MDESC_INIT(__var, __type, __name)	do {				\
		__var.getpksize	= PB_GPS_TYPECHECK((__type *)NULL, __name);	\
		__var.pack	= PB_PACK_TYPECHECK((__type *)NULL, __name);	\
		__var.unpack	= PB_UNPACK_TYPECHECK((__type **)NULL, __name);	\
		__var.free	= PB_FREE_TYPECHECK((__type *)NULL, __name);	\
		__var.pb_desc	= &__name##__descriptor;			\
	} while (0)

static struct cr_pb_message_desc cr_pb_descs[PB_MAX];

#define CR_PB_DESC(__type, __vtype, __ftype)					\
	CR_PB_MDESC_INIT(cr_pb_descs[PB_##__type],				\
			 __vtype##Entry,					\
			 __ftype##_entry)

void pb_init(void)
{
	CR_PB_DESC(INVENTORY,				Inventory,	inventory);
	CR_PB_DESC(FDINFO,				Fdinfo,		fdinfo);
	CR_PB_DESC(REG_FILES,				RegFile,	reg_file);
	CR_PB_DESC(EVENTFD,				EventfdFile,	eventfd_file);
	CR_PB_DESC(EVENTPOLL,				EventpollFile,	eventpoll_file);
	CR_PB_DESC(EVENTPOLL_TFD,			EventpollTfd,	eventpoll_tfd);
	CR_PB_DESC(SIGNALFD,				Signalfd,	signalfd);
	CR_PB_DESC(INOTIFY,				InotifyFile,	inotify_file);
	CR_PB_DESC(INOTIFY_WD,				InotifyWd,	inotify_wd);
	CR_PB_DESC(FANOTIFY,				FanotifyFile,	fanotify_file);
	CR_PB_DESC(FANOTIFY_MARK,			FanotifyMark,	fanotify_mark);
	CR_PB_DESC(CORE,				Core,		core);
	CR_PB_DESC(IDS,					TaskKobjIds,	task_kobj_ids);
	CR_PB_DESC(MM,					Mm,		mm);
	CR_PB_DESC(VMAS,				Vma,		vma);
	CR_PB_DESC(PIPES,				Pipe,		pipe);
	CR_PB_DESC(PIPES_DATA,				PipeData,	pipe_data);
	CR_PB_DESC(FIFO,				Fifo,		fifo);
	CR_PB_DESC(PSTREE,				Pstree,		pstree);
	CR_PB_DESC(SIGACT,				Sa,		sa);
	CR_PB_DESC(UNIXSK,				UnixSk,		unix_sk);
	CR_PB_DESC(INETSK,				InetSk,		inet_sk);
	CR_PB_DESC(SK_QUEUES,				SkPacket,	sk_packet);
	CR_PB_DESC(ITIMERS,				Itimer,		itimer);
	CR_PB_DESC(CREDS,				Creds,		creds);
	CR_PB_DESC(UTSNS,				Utsns,		utsns);
	CR_PB_DESC(IPCNS_VAR,				IpcVar,		ipc_var);
	CR_PB_DESC(IPCNS_SHM,				IpcShm,		ipc_shm);
	/* There's no _entry suffix in this one :( */
	CR_PB_MDESC_INIT(cr_pb_descs[PB_IPCNS_MSG],	IpcMsg, ipc_msg);
	CR_PB_DESC(IPCNS_MSG_ENT,			IpcMsg,		ipc_msg);
	CR_PB_DESC(IPCNS_SEM,				IpcSem,		ipc_sem);
	CR_PB_DESC(FS,					Fs,		fs);
	CR_PB_DESC(REMAP_FPATH,				RemapFilePath,	remap_file_path);
	CR_PB_DESC(GHOST_FILE,				GhostFile,	ghost_file);
	CR_PB_DESC(TCP_STREAM,				TcpStream,	tcp_stream);
	CR_PB_DESC(MOUNTPOINTS,				Mnt,		mnt);
	CR_PB_DESC(NETDEV,				NetDevice,	net_device);
	CR_PB_DESC(PACKETSK,				PacketSock,	packet_sock);
	CR_PB_DESC(TTY,					TtyFile,	tty_file);
	CR_PB_DESC(TTY_INFO,				TtyInfo,	tty_info);
	CR_PB_DESC(FILE_LOCK,				FileLock,	file_lock);
	CR_PB_DESC(RLIMIT,				Rlimit,		rlimit);
	CR_PB_MDESC_INIT(cr_pb_descs[PB_PAGEMAP_HEAD],	PagemapHead,	pagemap_head);
	CR_PB_DESC(PAGEMAP,				Pagemap,	pagemap);
	CR_PB_DESC(SIGINFO,				Siginfo,	siginfo);
	CR_PB_DESC(NETLINKSK,				NetlinkSk,	netlink_sk);
}

static struct cr_pb_message_desc cr_pb_descs[PB_MAX];

/*
 * Writes PB record (header + packed object pointed by @obj)
 * to file @fd, using @getpksize to get packed size and @pack
 * to implement packing
 *
 *  0 on success
 * -1 on error
 */
int pb_write_one(int fd, void *obj, int type)
{
	u8 local[1024];
	void *buf = (void *)&local;
	u32 size, packed;
	int ret = -1;

	if (!cr_pb_descs[type].pb_desc) {
		pr_err("Wrong object requested %d\n", type);
		return -1;
	}

	size = cr_pb_descs[type].getpksize(obj);
	if (size > (u32)sizeof(local)) {
		buf = xmalloc(size);
		if (!buf)
			goto err;
	}

	packed = cr_pb_descs[type].pack(obj, buf);
	if (packed != size) {
		pr_err("Failed packing PB object %p\n", obj);
		goto err;
	}

	ret = write(fd, &size, sizeof(size));
	if (ret != sizeof(size)) {
		ret = -1;
		pr_perror("Can't write %d bytes", (int)sizeof(size));
		goto err;
	}

	ret = write(fd, buf, size);
	if (ret != size) {
		ret = -1;
		pr_perror("Can't write %d bytes", size);
		goto err;
	}

	ret = 0;
err:
	if (buf != (void *)&local)
		xfree(buf);
	return ret;
}