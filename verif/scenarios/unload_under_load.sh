#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# An open fd holds a module reference: rmmod must fail with the fd open
# and succeed once it is closed.
rc=0
sleep 30 < /dev/kbuf0 &
holder=$!
sleep 1
if rmmod kbuf 2>/dev/null; then
	echo "rmmod unexpectedly succeeded while an fd was open"
	rc=1
fi
kill "$holder" 2>/dev/null
wait "$holder" 2>/dev/null
sleep 1
rmmod kbuf || { echo "rmmod after fd close failed"; rc=1; }
exit $rc
