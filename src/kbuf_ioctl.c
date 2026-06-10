// SPDX-License-Identifier: GPL-2.0
/*
 * kbuf_ioctl.c - ioctl dispatch for the kbuf device.
 *
 * The UAPI (command numbers and structs) is committed in include/kbuf.h. The
 * command handlers are implemented in Phase 3; this file currently validates
 * the command encoding and reports recognised-but-unimplemented commands with
 * -ENOSYS, keeping v1 runtime behaviour unchanged.
 */
#include <linux/errno.h>
#include <linux/fs.h>

#include "kbuf_internal.h"

long kbuf_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	if (_IOC_TYPE(cmd) != KBUF_IOC_MAGIC)
		return -ENOTTY;
	if (_IOC_NR(cmd) == 0 || _IOC_NR(cmd) > KBUF_IOC_MAXNR)
		return -ENOTTY;

	/*
	 * UAPI commands KBUF_IOCRESIZE/GSTATS/RESET/SMODE are declared in
	 * include/kbuf.h; their handlers arrive in Phase 3 onward. Until then
	 * every recognised command is reported as unsupported.
	 */
	return -ENOTTY;
}
