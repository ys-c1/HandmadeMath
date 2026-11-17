#!/bin/sh
# Builds and runs the standalone batch normal-matrix verification/benchmark.
# Additive only: it compiles test/normal_matrix_test.c and does not touch the
# repository build scripts or the existing test sources.
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build="$here/build/normal_matrix"

mkdir -p "$build"

CC=${CC:-cc}
CXX=${CXX:-c++}

echo "== C11, SIMD =="
"$CC" -std=c11 -O2 -Wall -Wextra -Werror -I"$here" \
    "$here/normal_matrix_test.c" -lm -o "$build/normal_matrix_c11"
"$build/normal_matrix_c11"

echo
echo "== C11, no SIMD =="
"$CC" -std=c11 -O2 -Wall -Wextra -Werror -DHANDMADE_MATH_NO_SIMD -I"$here" \
    "$here/normal_matrix_test.c" -lm -o "$build/normal_matrix_c11_nosimd"
"$build/normal_matrix_c11_nosimd"

echo
echo "== C++11 =="
"$CXX" -std=c++11 -O2 -Wall -Wextra -Werror -I"$here" -x c++ \
    "$here/normal_matrix_test.c" -lm -o "$build/normal_matrix_cpp11"
"$build/normal_matrix_cpp11"
