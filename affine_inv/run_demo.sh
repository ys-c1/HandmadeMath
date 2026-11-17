#!/bin/sh
# One-shot build-and-run for the affine-inverse demo. Uses only the
# compilers already required by the repository and the C standard library;
# no new third-party code and no network access.
#
# Builds and runs four variants:
#   1. C99, default build (fast path compiled out)
#   2. C99 with HANDMADE_MATH_AFFINE_INV=1 (fast path compiled in)
#   3. Same as 2 but with SIMD disabled
#   4. C++98 with the fast path compiled in (header is compiled as C++ too)
set -e

cd "$(dirname "$0")"
mkdir -p build
CC="${CC:-cc}"
CXX="${CXX:-c++}"
CFLAGS="-std=c99 -O2 -Wall -Wextra -Wno-missing-braces"
CXXFLAGS="-std=c++98 -O2 -Wall -Wextra -Wno-missing-braces"
LDLIBS="-lm"

echo "== variant 1: C99, fast path compiled out =="
$CC $CFLAGS affine_inv_demo.c $LDLIBS -o build/demo_c99_general
./build/demo_c99_general

echo
echo "== variant 2: C99, fast path compiled in =="
$CC $CFLAGS -DHANDMADE_MATH_AFFINE_INV=1 affine_inv_demo.c $LDLIBS -o build/demo_c99_fast
./build/demo_c99_fast

echo
echo "== variant 3: C99, fast path in, SIMD disabled =="
$CC $CFLAGS -DHANDMADE_MATH_AFFINE_INV=1 -DHANDMADE_MATH_NO_SIMD affine_inv_demo.c $LDLIBS -o build/demo_c99_fast_nosimd
./build/demo_c99_fast_nosimd

echo
echo "== variant 4: C++98, fast path compiled in =="
$CXX $CXXFLAGS -DHANDMADE_MATH_AFFINE_INV=1 -x c++ affine_inv_demo.c $LDLIBS -o build/demo_cpp98_fast
./build/demo_cpp98_fast
