#!/bin/bash
# Build and run the sphere/capsule collision test under every language
# standard the library supports, plus a sanitizer build. Does not touch the
# existing Makefile-based test suite.
set -u

cd "$(dirname "$0")"

CC="${CC:-cc}"
CXX="${CXX:-c++}"
BUILD_DIR=./build_collision
SRC=./collision_test.c
WARN="-Wall -Wextra -Wpedantic -Wno-missing-braces -Wno-missing-field-initializers"

mkdir -p "$BUILD_DIR"
status=0

run() {
    # $1 = output binary, rest = compiler command line
    local out="$1"; shift
    echo
    echo "== $out =="
    if "$@" -lm -o "$BUILD_DIR/$out"; then
        "$BUILD_DIR/$out" || status=1
    else
        echo "COMPILE FAILED: $out"
        status=1
    fi
}

# C builds: C99/C11/C17, with and without SIMD.
run c99           "$CC"  $WARN -std=c99  "$SRC"
run c99_no_simd   "$CC"  $WARN -std=c99  -DHANDMADE_MATH_NO_SIMD "$SRC"
run c11           "$CC"  $WARN -std=c11  "$SRC"
run c17           "$CC"  $WARN -std=c17  "$SRC"
# Optimized build: same checks, plus representative benchmark speedup numbers.
run c11_o2        "$CC"  $WARN -std=c11 -O2 "$SRC"

# C++ builds: old and new standard, with and without SIMD.
run cpp98         "$CXX" $WARN -std=c++98 "$SRC"
run cpp98_no_simd "$CXX" $WARN -std=c++98 -DHANDMADE_MATH_NO_SIMD "$SRC"
run cpp17         "$CXX" $WARN -std=c++17 "$SRC"

# Sanitizer build (only if the compiler accepts the flags).
cat > "$BUILD_DIR/san_probe.c" <<'EOF'
int main(void) { return 0; }
EOF
if "$CC" $WARN -std=c11 -fsanitize=address,undefined \
        "$BUILD_DIR/san_probe.c" -o "$BUILD_DIR/san_probe" 2>/dev/null; then
    run c11_san "$CC" $WARN -std=c11 -fsanitize=address,undefined -O1 "$SRC"
else
    echo
    echo "== sanitizer build skipped (compiler does not support it) =="
fi

echo
if [ "$status" -eq 0 ]; then
    echo "ALL COLLISION BUILDS AND TESTS PASSED"
else
    echo "COLLISION TESTS FAILED"
fi
exit "$status"
