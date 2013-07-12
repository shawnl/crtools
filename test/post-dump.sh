#!/bin/sh

[ $CRTOOLS_SCRIPT_ACTION == post-dump ] && {
	#
	# Special code to inform CRIU that
	# it should turn off repair mode
	# on sockets.
        exit 32
}
