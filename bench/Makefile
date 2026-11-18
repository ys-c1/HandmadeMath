# Benchmark + bit-exactness harness for batch 4x4 matrix multiplication.
# Self-contained: libc only, no third-party dependencies.

CC ?= cc
CFLAGS_COMMON = -std=c11 -O2 -Wall -Wextra -Wpedantic -ffp-contract=off
# -ffp-contract=off guarantees the scalar reference kernel's separate
# mul/add statements are never fused into FMA, keeping every build's
# float operation order bit-identical to the naive triple loop.

COUNT ?= 1000000
RUNS  ?= 5

.PHONY: all clean bench bench_no_simd sweep

all: bench bench_no_simd

mat4_bench: mat4_bench.c ../HandmadeMath.h
	$(CC) $(CFLAGS_COMMON) -msse2 mat4_bench.c -lm -o mat4_bench

mat4_bench_no_simd: mat4_bench.c ../HandmadeMath.h
	$(CC) $(CFLAGS_COMMON) -DHANDMADE_MATH_NO_SIMD mat4_bench.c -lm \
		-o mat4_bench_no_simd

bench: mat4_bench
	./mat4_bench $(COUNT) $(RUNS)

bench_no_simd: mat4_bench_no_simd
	./mat4_bench_no_simd $(COUNT) $(RUNS)

# Tile-size sweep: same count, fewer runs, compile one binary per tile.
sweep:
	@for tile in 8 16 32 64 128 256 512; do \
		echo "=== tile $$tile ==="; \
		$(CC) $(CFLAGS_COMMON) -msse2 -DHMM_MATMUL_BATCH_TILE=$$tile \
			mat4_bench.c -lm -o mat4_bench_tile_$$tile; \
		./mat4_bench_tile_$$tile $(COUNT) 3 | sed -n '/Summary/,$$p'; \
	done

clean:
	rm -f mat4_bench mat4_bench_no_simd mat4_bench_tile_*
