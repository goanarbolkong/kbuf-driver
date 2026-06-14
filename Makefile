# SPDX-License-Identifier: GPL-2.0
# User-facing entry point. The kernel build system reads the separate Kbuild
# file for the module object layout; this Makefile drives module + userspace
# builds and the quality-gate targets (sparse, checkpatch).

KDIR   ?= /lib/modules/$(shell uname -r)/build
PWD    := $(shell pwd)

CC     := gcc
CFLAGS := -Wall -Wextra -O2 -Iinclude
CXX    := g++
CXXFLAGS := -std=c++20 -Wall -Wextra -O2 -Iinclude

TEST_SRCS := $(wildcard tests/*.c)
TESTS     := $(TEST_SRCS:.c=)
BENCH_SRCS := $(wildcard bench/*.c)
BENCHES   := $(BENCH_SRCS:.c=)

# kbuf++ GoogleTest binaries (built only after `make gtest`).
GTEST_LIB := third_party/libgtest.a
GTEST_INC := $(firstword $(wildcard third_party/googletest-*/googletest/include))
CPP_SRCS  := $(wildcard tests/*.cpp)
CPP_TESTS := $(CPP_SRCS:.cpp=)

.PHONY: all modules tests bench cpp gtest sparse checkpatch verif verif-smoke clean load unload status dmesg help

all: modules tests

## modules: build kbuf.ko against the running kernel's headers
modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

## tests: build the user-space test programs
tests: $(TESTS)

tests/%: tests/%.c
	$(CC) $(CFLAGS) -o $@ $<

## bench: build the throughput benchmark(s)
bench: $(BENCHES)

bench/%: bench/%.c
	$(CC) $(CFLAGS) -o $@ $< -lm

## gtest: fetch + statically build GoogleTest into third_party/
gtest:
	./scripts/fetch-googletest.sh

## cpp: build the kbuf++ GoogleTest binaries (run `make gtest` first)
cpp: $(CPP_TESTS)

tests/%: tests/%.cpp $(GTEST_LIB)
	$(CXX) $(CXXFLAGS) -I$(GTEST_INC) -static -o $@ $< $(GTEST_LIB) -lpthread

## sparse: static analysis on the module (fix every warning)
sparse:
	$(MAKE) -C $(KDIR) M=$(PWD) C=2 modules

## checkpatch: kernel coding-style check (--strict).
## Excluded: include/libkbuf.h (user-space header: uint64_t, not kernel u64)
## and src/kbuf_trace.h (TRACE_EVENT macro DSL with its own prescribed layout).
CHECKPATCH_FILES := $(filter-out src/kbuf_trace.h,$(wildcard src/*.c src/*.h)) \
		    include/kbuf.h
checkpatch:
	$(KDIR)/scripts/checkpatch.pl --strict --no-tree -f $(CHECKPATCH_FILES)

## verif: full pytest verification suite under QEMU (one boot per test)
verif:
	python3 -m pytest verif --junitxml=.qemu/verif.xml

## verif-smoke: single-boot sanity check (fast; what CI runs without KVM)
verif-smoke:
	python3 -m pytest verif -m smoke --junitxml=.qemu/verif-smoke.xml

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f $(TESTS) $(BENCHES)

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
