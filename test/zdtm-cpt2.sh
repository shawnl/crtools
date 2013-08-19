#!/bin/sh

#
# A trivial script to start/stop tests used in CRIU zdtm engine.
# I used it for converter but might be worth for testing if test
# passes start/stop cycle but fails in CRIU itself.

basedir="test/zdtm/live"

error_nopid()
{
	echo "No $1.pid found"
	exit 1
}

start_test()
{
	local testname=$1
	local filename=`basename $testname`
	local directory=`dirname $testname`
	local pid=-1

	#
	# drop any running instance
	killall -9 $filename > /dev/null 2>&1

	#
	# cleanup previous data
	make -s -C $basedir/$directory $filename.cleanout

	#
	# finally run it
	make -s -C $basedir/$directory $filename.pid
	pid=`cat $basedir/$testname.pid` || error_nopid $testname

	echo "test '$testname' running with pid '$pid'"
}

stop_test()
{
	local testname=$1
	local filename=`basename $testname`
	local directory=`dirname $testname`
	local pid=-1

	#
	# get running pid
	pid=`cat $basedir/$testname.pid` || error_nopid $testname

	#
	# finalize it and read results
	kill $pid > /dev/null 2>&1
	sltime=1
	for i in `seq 10`; do
		test -f $basedir/$testname.out && break
		echo Waiting...
		sleep 0.$sltime
		[ $sltime -lt 9 ] && sltime=$((sltime+1))
	done
	cat $basedir/$testname.out
}

if [ $# -ge 2 ]; then
	case $1 in
	start)
		shift
		for i in $@; do
			start_test $i
		done
		;;
	stop)
		shift
		for i in $@; do
			stop_test $i
		done
		;;
	*)
		echo "(start|stop) test-name"
		;;
	esac
fi
