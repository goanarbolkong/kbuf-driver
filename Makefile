# SPDX-License-Identifier: GPL-2.0
# User-facing entry point. The kernel build system reads the separate Kbuild
# file for the module object layout; this Makefile drives module + userspace
# builds and the quality-gate targets (sparse, checkpatch).

KDIR   ?= /lib/modules/$(shell uname -r)/build
PWD    := $(shell pwd)

CC     := gcc
CFLAGS := -Wall -Wextra -O2 -Iinclude

TEST_SRCS := $(wildcard tests/*.c)
TESTS     := $(TEST_SRCS:.c=)

.PHONY: all modules tests sparse checkpatch clean load unload status dmesg help

all: modules tests

## modules: build kbuf.ko against the running kernel's headers
modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

## tests: build the user-space test programs
tests: $(TESTS)

tests/%: tests/%.c
	$(CC) $(CFLAGS) -o $@ $<

## sparse: static analysis on the module (fix every warning)
sparse:
	$(MAKE) -C $(KDIR) M=$(PWD) C=2 modules

## checkpatch: kernel coding-style check (--strict)
checkpatch:
	$(KDIR)/scripts/checkpatch.pl --strict --no-tree -f \
		src/*.c src/*.h include/*.h

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f $(TESTS)

## load / unload: host insmod is discouraged for experimental builds.
## Prefer scripts/run-qemu.sh. Host load needs MOK signing under Secure Boot.
load:
	sudo insmod kbuf.ko

unload:
	sudo rmmod kbuf

status:
	cat /proc/kbuf_status

dmesg:
	sudo dmesg | tail -30

help:
	@grep -E '^## ' $(MAKEFILE_LIST) | sed 's/## //'
