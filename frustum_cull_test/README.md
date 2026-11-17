# Batch frustum culling tests

Self-contained tests for the frustum culling API added to `HandmadeMath.h`.
No third-party dependencies and no changes to the repo's build scripts:

```
./run_tests.sh
```

The script builds `hmm_frustum_cull_test.c` under C99/C11/C17 and
C++98/C++11/C++17/C++20 (with and without `HANDMADE_MATH_NO_SIMD`), using the
same warning flags as `test/Makefile`, then runs each binary once.

Coverage:

1. Plane extraction: six unit-length, inward-facing planes from a
   view-projection matrix.
2. Three scenario groups for spheres and capsules — fully inside, fully
   outside, and straddling a plane — with the INSIDE / OUTSIDE / INTERSECTS
   distribution printed for each.
3. Batched vs single-volume consistency on randomized volumes across several
   configured batch sizes (1, 2, 7, 16, 256) and with default options,
   including radius-0 and point-capsule degenerates.
4. Bounding-sphere capsule approximation: per-plane interval containment and
   exact agreement for zero-length capsules.
5. Timing of a 100,000-volume batch for spheres and capsules (exact and
   approximate), alongside a single-volume reference loop.
