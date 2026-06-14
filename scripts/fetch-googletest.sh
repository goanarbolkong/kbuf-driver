#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Fetch and statically build GoogleTest for the kbuf++ test suite.
#
# GoogleTest is not vendored in the repo; this script downloads a pinned
# release into third_party/ (gitignored) and builds libgtest.a + a main, with
# no CMake and no system install. The kbuf++ GoogleTest binary links against
# the result and runs statically inside the QEMU guest like every other test.
#
# Idempotent: re-running with the library already present is a no-op.
set -eu

VERSION=v1.15.2
URL="https://github.com/google/googletest/archive/refs/tags/${VERSION}.tar.gz"

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TP="$ROOT/third_party"
SRC="$TP/googletest-${VERSION#v}/googletest"
LIB="$TP/libgtest.a"

if [ -f "$LIB" ] && [ -d "$SRC" ]; then
	echo "googletest already built: $LIB"
	exit 0
fi

mkdir -p "$TP"
if [ ! -d "$SRC" ]; then
	echo "fetching googletest ${VERSION} ..."
	curl -fsSL "$URL" | tar -xz -C "$TP"
fi

echo "building libgtest.a (static) ..."
CXX=${CXX:-g++}
CXXFLAGS="-std=c++17 -O2 -I$SRC/include -I$SRC"
$CXX $CXXFLAGS -c "$SRC/src/gtest-all.cc"  -o "$TP/gtest-all.o"
$CXX $CXXFLAGS -c "$SRC/src/gtest_main.cc" -o "$TP/gtest_main.o"
ar rcs "$LIB" "$TP/gtest-all.o" "$TP/gtest_main.o"

echo "done: $LIB"
echo "include dir: $SRC/include"
