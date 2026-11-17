#!/bin/sh
# Build and run the HSH spatial-hash tests and benchmarks as both C and C++.
# The build uses only the compilers already used by this repository; no new
# third-party code and no network access are involved.
set -e

cd "$(dirname "$0")"
mkdir -p build

WARN="-g -Wall -Wextra -Wpedantic -Wno-missing-braces -Wno-missing-field-initializers -Wfloat-equal"

echo "== Compiling hsh_test as C99 =="
cc $WARN -std=c99 hsh_test.c -lm -o build/hsh_test_c99
echo "== Running C99 build =="
./build/hsh_test_c99

echo
echo "== Compiling hsh_test as C11 =="
cc $WARN -std=c11 hsh_test.c -lm -o build/hsh_test_c11
echo "== Running C11 build =="
./build/hsh_test_c11

echo
echo "== Compiling hsh_test as C++98 =="
c++ $WARN -std=c++98 hsh_test.c -lm -o build/hsh_test_cpp98
echo "== Running C++98 build =="
./build/hsh_test_cpp98

echo
echo "== Compiling hsh_test as C++11 =="
c++ $WARN -std=c++11 hsh_test.c -lm -o build/hsh_test_cpp11
echo "== Running C++11 build =="
./build/hsh_test_cpp11

echo
echo "All HSH builds finished."
