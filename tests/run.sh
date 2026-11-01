#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
if [ "${WSL_DISTRO_NAME:-}" = ubuntu-noble ]; then
    echo 'Use the explicitly selected Nikon distribution.' >&2
    exit 1
fi
TEST_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TEST_BUILD=${WONDERLANTERN_TEST_BUILD:-"$TEST_ROOT/.build/tests"}
mkdir -p "$TEST_BUILD"
cd "$TEST_ROOT"
python3 -B -m unittest discover -s tests -p 'test_*.py' -v
cc -std=c11 -O1 -g -fsanitize=address,undefined -Itests/include tests/test_gpu.c $(pkg-config --cflags --libs glib-2.0) -lm -o "$TEST_BUILD/gpu"
cc -std=c11 -O1 -g -fsanitize=address,undefined tests/test_isp.c $(pkg-config --cflags --libs glib-2.0) -lm -o "$TEST_BUILD/isp"
ASAN_OPTIONS=detect_leaks=1 "$TEST_BUILD/gpu"
ASAN_OPTIONS=detect_leaks=1 "$TEST_BUILD/isp"
cc -std=c11 -O1 -g -fsanitize=address,undefined tests/test_shoulder_observer.c -o "$TEST_BUILD/shoulder"
ASAN_OPTIONS=detect_leaks=1 "$TEST_BUILD/shoulder"
