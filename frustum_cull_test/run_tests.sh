#!/usr/bin/env bash
#
# Builds and runs the batch frustum culling tests under every C/C++ standard
# the repo's own test/Makefile uses, plus a HANDMADE_MATH_NO_SIMD build.
# Does not touch the repo build scripts.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BUILD="$HERE/build"
SRC="$HERE/hmm_frustum_cull_test.c"
WARN="-Wall -Wextra -Wno-missing-braces -Wno-missing-field-initializers -Wfloat-equal"
CC="${CC:-cc}"
CXX="${CXX:-c++}"

mkdir -p "$BUILD"

run_c () {
    std="$1"; shift
    bin="$BUILD/hmm_fc_$(echo "$std$*" | tr -cd '[:alnum:]')"
    echo "--- CC  $std $* ---"
    "$CC" $WARN -O2 -std="$std" "$@" -I"$ROOT" "$SRC" -lm -o "$bin"
    "$bin"
}

run_cxx () {
    std="$1"; shift
    bin="$BUILD/hmm_fc_$(echo "$std$*" | tr -cd '[:alnum:]')"
    echo "--- CXX $std $* ---"
    "$CXX" $WARN -O2 -std="$std" "$@" -I"$ROOT" "$SRC" -lm -o "$bin"
    "$bin"
}

run_c   c99
run_c   c99 -DHANDMADE_MATH_NO_SIMD
run_c   c11
run_c   c17
run_cxx gnu++98
run_cxx gnu++98 -DHANDMADE_MATH_NO_SIMD
run_cxx c++11
run_cxx c++17
run_cxx c++20

echo
echo "All frustum culling test builds passed."
