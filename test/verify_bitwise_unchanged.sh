#!/bin/sh
# Verifies that the batch normal-matrix work is purely additive: builds a small
# dump program against the pre-change header (git HEAD) and the working tree
# header, then compares every byte of pre-existing API output.
#
# Usage: ./test/verify_bitwise_unchanged.sh [path-to-header-at-HEAD]
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM

mkdir -p "$work/orig" "$work/new" "$work/src"

if [ "$#" -ge 1 ]; then
    cp "$1" "$work/orig/HandmadeMath.h"
else
    git -C "$root" show HEAD:HandmadeMath.h > "$work/orig/HandmadeMath.h"
fi
cp "$root/HandmadeMath.h" "$work/new/HandmadeMath.h"
cp "$here/normal_matrix_bitwise_dump.c" "$work/src/dump.c"

CC=${CC:-cc}

for variant in orig new; do
    "$CC" -std=c11 -O2 -I"$work/$variant" "$work/src/dump.c" -lm -o "$work/dump_$variant"
    "$work/dump_$variant" > "$work/out_$variant.txt"
done

if cmp -s "$work/out_orig.txt" "$work/out_new.txt"; then
    echo "PASS: pre-existing API output is bitwise identical ($(wc -l < "$work/out_orig.txt") records)"
    exit 0
fi

echo "FAIL: pre-existing API output changed"
diff "$work/out_orig.txt" "$work/out_new.txt" | head -20
exit 1
