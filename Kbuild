# SPDX-License-Identifier: GPL-2.0
# Kbuild spec for the kbuf module. The kernel build system reads this file when
# invoked with M=$(PWD); the user-facing entry point is the top-level Makefile.

obj-m := kbuf.o
kbuf-y := src/kbuf_main.o src/kbuf_ring.o src/kbuf_proc.o src/kbuf_ioctl.o \
	  src/kbuf_mmap.o src/kbuf_debugfs.o src/kbuf_ctl.o src/kbuf_dmabuf.o

ccflags-y := -I$(src)/include -I$(src)/src
