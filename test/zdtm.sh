#!/bin/sh

ZP="zdtm/live"

TEST_LIST="
static/pipe00
static/pipe01
static/busyloop00
static/cwd00
static/env00
static/maps00
static/maps01
static/maps02
static/maps04
static/mprotect00
static/mtime_mmap
static/sleeping00
static/write_read00
static/write_read01
static/write_read02
static/write_read10
static/wait00
static/vdso00
static/sched_prio00
static/sched_policy00
static/file_shared
static/timers
static/futex
static/futex-rl
static/xids00
static/groups
static/pthread00
static/pthread01
static/umask00
streaming/pipe_loop00
streaming/pipe_shared00
transition/file_read
static/sockets00
static/sockets01
static/sock_opts00
static/sock_opts01
static/sockets_spair
static/sockets_dgram
static/socket_queues
static/sk-unix-unconn
static/pid00
static/pstree
static/caps00
static/cmdlinenv00
static/socket_listen
static/socket_listen6
static/packet_sock
static/socket_udp
static/sock_filter
static/socket6_udp
static/socket_udplite
static/selfexe00
static/unlink_fstat00
static/unlink_fstat02
static/unlink_fstat03
static/eventfs00
static/signalfd00
static/inotify00
static/fanotify00
static/unbound_sock
static/fifo-rowo-pair
static/fifo-ghost
static/fifo
static/fifo_wronly
static/zombie00
static/rlimits00
transition/fork
static/pty00
static/pty01
static/pty04
static/tty02
static/tty03
static/child_opened_proc
static/cow01
static/fpu00
static/fpu01
static/mmx00
static/sse00
static/sse20
static/fdt_shared
static/file_locks00
static/file_locks01
static/sigpending
static/sigaltstack
static/sk-netlink
static/proc-self
"
# Duplicate list with ns/ prefix
TEST_LIST=$TEST_LIST$(echo $TEST_LIST | tr ' ' '\n' | sed 's#^#ns/#')

# These ones are not in ns
TEST_LIST="$TEST_LIST
static/file_fown
static/socket-ext
static/socket-tcp
static/socket-tcp6
streaming/socket-tcp
streaming/socket-tcp6
static/socket-tcpbuf
static/socket-tcpbuf6
static/pty03
"

MNT_TEST_LIST="
static/mountpoints
"

# These ones are in ns
TEST_LIST="$TEST_LIST
ns/static/session00
ns/static/session01
"

UTS_TEST_LIST="
static/utsname
"

IPC_TEST_LIST="
static/ipc_namespace
static/shm
static/msgque
static/sem
transition/ipc
"

TEST_CR_KERNEL="
static/sigpending
static/sk-netlink
"

TEST_SUID_LIST="
pid00
caps00
maps01
groups
sched_prio00
sched_policy00
sock_opts00
sock_opts01
cmdlinenv00
packet_sock
fanotify00
sk-netlink
"

CRIU=$(readlink -f `dirname $0`/../criu)
CRIU_CPT=$CRIU
TMP_TREE=""

test -x $CRIU || {
	echo "$CRIU is unavailable"
	exit 1
}

ARGS=""

PID=""
PIDNS=""

ITERATIONS=1
EXCLUDE_PATTERN=""
CLEANUP=0
PAGE_SERVER=0
PS_PORT=12345

check_mainstream()
{
	local -a ver_arr
	local ver_str=`uname -r`

	$CRIU check && return 0
	MAINSTREAM_KERNEL=1

	cat >&2 <<EOF
============================= WARNING =============================
Not all C/R features are commited in the meainstream kernel.
Linux C/R can be cloned from:
git://git.kernel.org/pub/scm/linux/kernel/git/gorcunov/linux-cr.git
===================================================================
EOF

	ver_arr=(`echo ${ver_str//./ }`)

	[ "${ver_arr[0]}" -gt 3 ] && return 0
	[[ "${ver_arr[0]}" -eq 3 && "${ver_arr[1]}" -ge 8 ]] && return 0

	echo "A version of kernel should be greater or equal to 3.8"

	return 1;
}

exit_callback()
{
	echo $@
	[ -n "$ZDTM_ROOT" ] && {
		umount -l "$ZDTM_ROOT"
		rmdir "$ZDTM_ROOT"
	}

	[[ -n "$ZDTM_FAILED" && -n "$DUMP_ARCHIVE" ]] && tar -czf $DUMP_ARCHIVE dump
	[ -n "$TMPFS_DUMP" ] &&
		umount -l "$TMPFS_DUMP"
}
trap exit_callback EXIT

construct_root()
{
	local root=$1
	local test_path=$2
	local ps_path=`type -P ps`
	local libdir=$root/lib
	local libdir2=$root/lib64

	mkdir $root/bin
	cp $ps_path $root/bin

	mkdir $libdir $libdir2
	for i in `ldd $test_path $ps_path | grep -P '^\s' | awk '{ print $1 }' | grep -v vdso`; do
		local lib=`basename $i`
		[ -f $libdir/$lib ] && continue ||
		[ -f $i ] && cp $i $libdir && cp $i $libdir2 && continue ||
		[ -f /lib64/$i ] && cp /lib64/$i $libdir && cp /lib64/$i $libdir2 && continue ||
		[ -f /usr/lib64/$i ] && cp /usr/lib64/$i $libdir && cp /usr/lib64/$i $libdir2 && continue ||
		[ -f /lib/x86_64-linux-gnu/$i ] && cp /lib/x86_64-linux-gnu/$i $libdir && cp /lib/x86_64-linux-gnu/$i $libdir2 && continue ||
		[ -f /lib/arm-linux-gnueabi/$i ] && cp /lib/arm-linux-gnueabi/$i $libdir && cp /lib/arm-linux-gnueabi/$i $libdir2 && continue || echo "Failed at " $i && return 1
	done
}

start_test()
{
	local tdir=$1
	local tname=$2
	export ZDTM_ROOT
	TPID=`readlink -f $tdir`/$tname.init.pid

	killall -9 $tname > /dev/null 2>&1
	make -C $tdir $tname.cleanout

	unset ZDTM_UID
	unset ZDTM_GID

	echo $TEST_SUID_LIST | grep $tname || {
		export ZDTM_UID=18943
		export ZDTM_GID=58467
		chown $ZDTM_UID:$ZDTM_GID $tdir
	}

	if [ -z "$PIDNS" ]; then
		make -C $tdir $tname.pid
		PID=`cat $test.pid` || return 1
	else
		if [ -z "$ZDTM_ROOT" ]; then
			mkdir dump
			ZDTM_ROOT=`mktemp -d /tmp/criu-root.XXXXXX`
			ZDTM_ROOT=`readlink -f $ZDTM_ROOT`
			mount --bind . $ZDTM_ROOT || return 1
		fi
		make -C $tdir $tname
		construct_root $ZDTM_ROOT $tdir/$tname || return 1
	(	export ZDTM_NEWNS=1
		export ZDTM_PIDFILE=$TPID
		cd $ZDTM_ROOT
		rm -f $ZDTM_PIDFILE
		make -C $tdir $tname.pid || {
			echo ERROR: fail to start $tdir/$tname
			return 1;
		}
	)

		PID=`cat "$TPID"`
		ps -p $PID || return 1
	fi
}

stop_test()
{
	kill $PID
}

save_fds()
{
	test -n "$PIDNS" && return 0
	ls -l /proc/$1/fd | sed 's/\(-> \(pipe\|socket\)\):.*/\1/' | awk '{ print $9,$10,$11; }' > $2
}

diff_fds()
{
	test -n "$PIDNS" && return 0
	if ! diff -up $1 $2; then
		echo ERROR: Sets of descriptors are differ:
		echo $1
		echo $2
		return 1
	fi
}

run_test()
{
	local test=$1
	local linkremap=

	[ -n "$EXCLUDE_PATTERN" ] && echo $test | grep "$EXCLUDE_PATTERN" && return 0

	#
	# add option for unlinked files test
	if [[ $1 =~ "unlink_" ]]; then
		linkremap="--link-remap"
	fi

	[ -n "$MAINSTREAM_KERNEL" ] && echo $TEST_CR_KERNEL | grep -q ${test#ns/} && {
		echo "Skip $test"
		return 0
	}

	expr "$test" : 'ns/' > /dev/null && PIDNS=1 || PIDNS=""
	test=${ZP}/${test#ns/}

	shift
	local args=$*
	local tname=`basename $test`
	local tdir=`dirname $test`
	DUMP_PATH=""

	echo "Execute $test"

	start_test $tdir $tname || return 1

	local ddump
	kill -s 0 "$PID" || {
		echo "Get a wrong pid '$PID'"
		return 1
	}

	if [ -n "$PIDNS" ]; then
		[ -z "$CR_IP_TOOL" ] && CR_IP_TOOL=ip
		$CR_IP_TOOL a help 2>&1 | grep -q showdump || {
			cat >&2 <<EOF
The util "ip" is incompatible. The good one can be cloned from
git://git.criu.org/iproute2. It should be compiled and a path
to ip is written in \$CR_IP_TOOL.
EOF
			exit 1;
		}
		args="-n uts -n ipc -n net -n pid -n mnt --root $ZDTM_ROOT --pidfile $TPID $args"
	fi

	for i in `seq $ITERATIONS`; do

	ddump=dump/$tname/$PID/$i
	DUMP_PATH=`pwd`/$ddump
	echo Dump $PID
	mkdir -p $ddump

	if [ $PAGE_SERVER -eq 1 ]; then
		$CRIU page-server -D $ddump -o page_server.log -v4 --port $PS_PORT --daemon
		PS_PID=$!
		opts="--page-server --address 127.0.0.1 --port $PS_PORT"
	fi


	save_fds $PID  $ddump/dump.fd
	setsid $CRIU_CPT dump $opts --file-locks --tcp-established $linkremap \
		-x --evasive-devices -D $ddump -o dump.log -v4 -t $PID $args $ARGS || {
		echo WARNING: process $tname is left running for your debugging needs
		return 1
	}

	if [ $PAGE_SERVER -eq 1 ]; then
		wait $PS_PID
	fi

	if expr " $ARGS" : ' -s' > /dev/null; then
		save_fds $PID  $ddump/dump.fd.after
		diff_fds $ddump/dump.fd $ddump/dump.fd.after || return 1
		killall -CONT $tname
		if [[ $linkremap ]]; then
			echo "remove ./$tdir/link_remap.*"
			rm -f ./$tdir/link_remap.*
		fi
	else
		# Wait while tasks are dying, otherwise PIDs would be busy.
		for i in $ddump/core-*.img; do
			local pid

			[ -n "$PIDNS" ] && break;

			pid=`expr "$i" : '.*/core-\([0-9]*\).img'`
			while :; do
				kill -0 $pid > /dev/null 2>&1 || break;
				echo Waiting the process $pid
				sleep 0.1
			done
		done

		echo Restore
		setsid $CRIU restore --file-locks --tcp-established -x -D $ddump -o restore.log -v4 -d $args || return 2

		for i in `seq 5`; do
			save_fds $PID  $ddump/restore.fd
			diff_fds $ddump/dump.fd $ddump/restore.fd && break
			sleep 0.2
		done
		[ $i -eq 5 ] && return 2;
		[ -n "$PIDNS" ] && PID=`cat $TPID`
	fi

	done

	echo Check results $PID
	stop_test $tdir $tname
	sltime=1
	for i in `seq 50`; do
		test -f $test.out && break
		echo Waiting...
		sleep 0.$sltime
		[ $sltime -lt 9 ] && sltime=$((sltime+1))
	done
	cat $test.out
	cat $test.out | grep -q PASS || return 2
	[ "$CLEANUP" -ne 0 ] && rm -rf `dirname $ddump`
	return 0
}

case_error()
{
	test=${ZP}/${1#ns/}
	local test_log=`pwd`/$test.out

	ZDTM_FAILED=1

	echo "Test: $test"
	echo "====================== ERROR ======================"

	if [ -n "$DUMP_PATH" ]; then
		[ -e "$DUMP_PATH/dump.log" ] && {
			echo "Dump log   : $DUMP_PATH/dump.log"
			cat $DUMP_PATH/dump.log* | grep Error
		}
		[ -e "$DUMP_PATH/restore.log" ] && {
			echo "Restore log: $DUMP_PATH/restore.log"
			cat $DUMP_PATH/restore.log* | grep Error
		}
	fi
	[ -e "$test_log" ] &&
		echo "Output file: $test_log"
	[ -n "$HEAD" ] &&
		echo "The initial HEAD was $HEAD"
	exit 1
}

checkout()
{
	local commit=`git describe $1` &&
	TMP_TREE=`dirname $CRIU`/criu.$commit &&
	mkdir -p $TMP_TREE &&
	git --git-dir `dirname $CRIU`/.git archive $commit . | tar -x -C $TMP_TREE &&
	make -C $TMP_TREE -j 32
}

cd `dirname $0` || exit 1

while :; do
	if [ "$1" = "-d" ]; then
		ARGS="-s"
		shift
		continue
	fi
	if [ "$1" = "-i" ]; then
		shift
		ITERATIONS=$1
		shift
		continue
	fi
	if [ "$1" = "-b" ]; then
		shift
		checkout $1 || exit 1
		CRIU_CPT=$TMP_TREE/criu
		shift
		continue
	fi
	if [ "$1" = "-c" ]; then
		shift
		checkout $1 || exit 1
		shift
		$TMP_TREE/test/zdtm.sh "$@"
		exit
	fi
	if [ "$1" = "-p" ]; then
		shift
		PAGE_SERVER=1
		continue;
	fi
	if [ "$1" = "-C" ]; then
		shift
		CLEANUP=1
		continue;
	fi
	if [ "$1" = "-x" ]; then
		shift
		EXCLUDE_PATTERN=$1
		shift
		continue;
	fi
	if [ "$1" = "-t" ]; then
		shift
		TMPFS_DUMP=dump
		[ -d dump ] || mkdir $TMPFS_DUMP
		mount -t tmpfs none $TMPFS_DUMP || exit 1
		continue;
	fi
	if [ "$1" = "-a" ]; then
		shift
		DUMP_ARCHIVE=$1
		shift
		continue;
	fi
	break;
done

if [ $# -eq 0 ]; then

	check_mainstream || exit 1

	for t in $TEST_LIST; do
		run_test $t || case_error $t
	done
	for t in $UTS_TEST_LIST; do
		run_test $t -n uts || case_error $t
	done
	for t in $MNT_TEST_LIST; do
		run_test $t -n mnt || case_error $t
	done
	for t in $IPC_TEST_LIST; do
		run_test $t -n ipc || case_error $t
	done
elif [ "$1" = "-l" ]; then
	echo $TEST_LIST $UTS_TEST_LIST $MNT_TEST_LIST $IPC_TEST_LIST | tr ' ' '\n'
elif [ "$1" = "-h" ]; then
	cat >&2 <<EOF
This script is used for executing unit tests.
Usage:
zdtm.sh [OPTIONS]
zdtm.sh [OPTIONS] [TEST NAME]
Options:
	-l : Show list of tests.
	-d : Dump a test process and check that this process can continue working.
	-i : Number of ITERATIONS of dump/restore
	-p : Test page server
	-C : Delete dump files if a test completed successfully
	-b <commit> : Check backward compatibility
	-x <PATTERN>: Exclude pattern
	-t : mount tmpfs for dump files
	-a <FILE>.tar.gz : save archive with dump files and logs
EOF
elif [ "${1:0:1}" = '-' ]; then
	echo "unrecognized option $1"
else
	if echo $UTS_TEST_LIST | fgrep -qw $1; then
		run_test $1 -n uts || case_error $1
	elif echo $MNT_TEST_LIST | fgrep -qw $1; then
		run_test $1 -n mnt || case_error $1
	elif echo $IPC_TEST_LIST | fgrep -qw $1; then
		run_test $1 -n ipc || case_error $1
	elif echo $FILE_LOCK_TEST_LIST | fgrep -qw $1; then
		run_test $1 -l || case_error $1
	else
		run_test $1 || case_error $1
	fi
fi

[ -n "$TMP_TREE" ] && rm -rf $TMP_TREE || exit 0
