#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Full unload/reload cycle with a non-default module parameter.
rc=0
rmmod kbuf || { echo "initial rmmod failed"; rc=1; }
insmod /kbuf.ko ndevices=2 || { echo "reload insmod failed"; rc=1; }
n=$(ls -d /dev/kbuf[0-9]* 2>/dev/null | wc -l)
[ "$n" -eq 2 ] || { echo "ndevices=2 not honored (got $n)"; rc=1; }
rmmod kbuf || { echo "final rmmod failed"; rc=1; }
exit $rc
