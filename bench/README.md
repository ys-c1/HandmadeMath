# Batch 4x4 matrix multiply: benchmark and bit-exactness harness

Self-contained, libc-only harness (no third-party code, no network) for the
`HMM_MulM4Batch` API added to `HandmadeMath.h`.

## What it does

`mat4_bench.c` contains, in one binary:

1. **Bit-exactness check** — 1000 random matrix pairs (24-bit-mantissa floats
   in `[-1,1)`, so real rounding is exercised). The optimized batch result,
   the per-item library call `HMM_MulM4`, and a plain `(i, column, row, k)`
   naive triple loop are compared **raw uint32 bit-for-bit**. It also checks
   the deliberately 4-byte-misaligned SSE path, zero/identity inputs, and odd
   counts that straddle tile and interleave boundaries.
2. **Throughput** — 1,000,000 products timed over several runs, reporting
   GFLOPS using the standard `2 * 4 * 4 * 4 = 128` FLOPs per 4x4 product.
   Best run wins. A checksum is accumulated across runs so the optimizer
   cannot eliminate the work.

## Build and run

```sh
make            # builds both mat4_bench (SSE) and mat4_bench_no_simd
make bench      # SSE build,   1M pairs, 5 runs
make bench_no_simd
make sweep      # tile-size sweep (8 .. 512)
make clean

# custom size / run count:
./mat4_bench [count] [runs]
```

`-ffp-contract=off` is set so the naive scalar reference can never fuse a
multiply/add into an FMA; every build therefore keeps the exact same float
operation order. `HMM_MATMUL_BATCH_TILE` may be overridden at compile time.

## Tile size

The default batch tile is 64 matrices (`HMM_MATMUL_BATCH_TILE` in
`HandmadeMath.h`). One tile's three arrays (left/right/out) are
`3 * 64 * 64 B = 12 KiB`, which fits comfortably in even a 32 KiB L1d (the
common minimum on x86). On this streaming workload, where each matrix is read
once and written once, measured throughput is essentially flat from tile 8 up
to effectively no tiling — the sweep documents that; 64 is chosen as the
safe L1-resident default rather than the peak of noisy measurements.
