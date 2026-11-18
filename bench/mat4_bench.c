/*
 * mat4_bench.c -- benchmark and bit-exactness check for batch 4x4 matmul.
 *
 * Usage:
 *   mat4_bench [count] [runs]
 *     count : number of matrix pairs in the batch (default 1000000)
 *     runs  : timed runs per implementation (default 5)
 *
 * No third-party code, no network: libc only.
 */
#define _POSIX_C_SOURCE 200112L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../HandmadeMath.h"

/* ------------------------------------------------------------------ */
/* Deterministic PRNG (xorshift32) and float generation               */
/* ------------------------------------------------------------------ */

static unsigned int RngState = 0x12345678u;

static unsigned int NextU32(void)
{
    unsigned int x = RngState;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    RngState = x;
    return x;
}

/* Uniform floats in [-1, 1) with a 24-bit mantissa: exercises real
 * float rounding instead of tiny integers that never round. */
static float NextFloat(void)
{
    return ((float)(NextU32() & 0x00FFFFFFu) / (float)0x01000000u) * 2.0f
           - 1.0f;
}

static void FillRandom(HMM_Mat4 *M, int Count)
{
    int i, c, r;
    for (i = 0; i < Count; ++i)
    {
        for (c = 0; c < 4; ++c)
        {
            for (r = 0; r < 4; ++r)
            {
                M[i].Elements[c][r] = NextFloat();
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Naive reference: plain (i, j, k) triple loop over the batch.       */
/* First product is assigned (not added to zero) so that signed-zero  */
/* behavior matches the library's scalar kernel exactly.              */
/* ------------------------------------------------------------------ */

static void NaiveBatch(const HMM_Mat4 *Left, const HMM_Mat4 *Right,
                       HMM_Mat4 *Out, int Count)
{
    int i, Column, Row, k;

    for (i = 0; i < Count; ++i)
    {
        for (Column = 0; Column < 4; ++Column)
        {
            for (Row = 0; Row < 4; ++Row)
            {
                Out[i].Elements[Column][Row] =
                    Left[i].Elements[0][Row] * Right[i].Elements[Column][0];
                for (k = 1; k < 4; ++k)
                {
                    Out[i].Elements[Column][Row] +=
                        Left[i].Elements[k][Row] * Right[i].Elements[Column][k];
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Bit-exact comparison                                               */
/* ------------------------------------------------------------------ */

static int BitsEqual(const HMM_Mat4 *A, const HMM_Mat4 *B)
{
    int c, r;
    for (c = 0; c < 4; ++c)
    {
        for (r = 0; r < 4; ++r)
        {
            union { float f; unsigned int u; } ua, ub;
            ua.f = A->Elements[c][r];
            ub.f = B->Elements[c][r];
            if (ua.u != ub.u)
            {
                return 0;
            }
        }
    }
    return 1;
}

static unsigned int Checksum(const HMM_Mat4 *M, int Count)
{
    unsigned int Accum = 0;
    const unsigned int *P = (const unsigned int *)M;
    size_t n = (size_t)Count * (sizeof(HMM_Mat4) / sizeof(unsigned int));
    size_t i;
    for (i = 0; i < n; ++i)
    {
        Accum ^= P[i] + 0x9e3779b9u + (Accum << 6) + (Accum >> 2);
    }
    return Accum;
}

/* ------------------------------------------------------------------ */
/* Timing                                                             */
/* ------------------------------------------------------------------ */

static double NowSeconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* Standard convention: mul and add each count as one FLOP, so a 4x4
 * matrix product is 2 * 4 * 4 * 4 = 128 FLOPs. */
#define FLOPS_PER_MATMUL 128.0

typedef void (*BatchFn)(const HMM_Mat4 *, const HMM_Mat4 *, HMM_Mat4 *, int);

static double Bench(BatchFn Fn, const HMM_Mat4 *A, const HMM_Mat4 *B,
                    HMM_Mat4 *C, int Count, int Runs, const char *Name,
                    unsigned int *Sink)
{
    double Best = 1e30;
    int r;

    Fn(A, B, C, Count); /* warmup */
    *Sink += Checksum(C, Count);

    for (r = 0; r < Runs; ++r)
    {
        double t0 = NowSeconds();
        Fn(A, B, C, Count);
        double t1 = NowSeconds();
        double Secs = t1 - t0;
        double Gflops = ((double)Count * FLOPS_PER_MATMUL) / Secs / 1e9;
        if (Secs < Best)
        {
            Best = Secs;
        }
        *Sink += Checksum(C, Count);
        printf("  %-22s run %d: %8.4f s  %7.2f GFLOPS\n",
               Name, r + 1, Secs, Gflops);
    }
    return Best;
}

static void NaiveWrap(const HMM_Mat4 *A, const HMM_Mat4 *B, HMM_Mat4 *C,
                      int Count)
{
    NaiveBatch(A, B, C, Count);
}

static void LibWrap(const HMM_Mat4 *A, const HMM_Mat4 *B, HMM_Mat4 *C,
                    int Count)
{
    int i;
    for (i = 0; i < Count; ++i)
    {
        C[i] = HMM_MulM4(A[i], B[i]);
    }
}

static void BatchWrap(const HMM_Mat4 *A, const HMM_Mat4 *B, HMM_Mat4 *C,
                      int Count)
{
    HMM_MulM4Batch(A, B, C, Count);
}

static HMM_Mat4 *AlignedAllocMat4(int Count)
{
    void *p = 0;
    if (posix_memalign(&p, 64, (size_t)Count * sizeof(HMM_Mat4)) != 0)
    {
        return 0;
    }
    return (HMM_Mat4 *)p;
}

int main(int argc, char **argv)
{
    const int Count = (argc > 1) ? atoi(argv[1]) : 1000000;
    const int Runs  = (argc > 2) ? atoi(argv[2]) : 5;
    const int CorrectnessPairs = 1000;
    HMM_Mat4 *A, *B, *Ref, *Opt, *Lib, *Mis;
    HMM_Mat4 *MisA, *MisB; /* deliberately 4-byte aligned (misaligned) */
    HMM_Mat4 *MisABase, *MisBBase; /* original aligned pointers for free() */
    unsigned int Sink = 0;
    int i, Failures;
    double TNaive, TLib, TBatch;

    if (Count < CorrectnessPairs)
    {
        fprintf(stderr, "count must be >= %d\n", CorrectnessPairs);
        return 2;
    }

    A   = AlignedAllocMat4(Count);
    B   = AlignedAllocMat4(Count);
    Ref = AlignedAllocMat4(Count);
    Opt = AlignedAllocMat4(Count);
    Lib = AlignedAllocMat4(Count);
    Mis = AlignedAllocMat4(Count);
    /* +1 matrix of slack, then offset by one float -> 4-byte (mis)aligned */
    MisABase = AlignedAllocMat4(Count + 1);
    MisBBase = AlignedAllocMat4(Count + 1);
    MisA = (HMM_Mat4 *)((char *)MisABase + sizeof(float));
    MisB = (HMM_Mat4 *)((char *)MisBBase + sizeof(float));
    if (!A || !B || !Ref || !Opt || !Lib || !Mis || !MisABase || !MisBBase)
    {
        fprintf(stderr, "allocation failed\n");
        return 2;
    }

    FillRandom(A, Count);
    FillRandom(B, Count);
    /* memcpy (not struct assignment): the destination is deliberately
     * 4-byte aligned, and the compiler may lower HMM_Mat4 struct copies
     * to aligned movaps, which would fault there. */
    memcpy(MisA, A, (size_t)Count * sizeof(HMM_Mat4));
    memcpy(MisB, B, (size_t)Count * sizeof(HMM_Mat4));

#ifdef HANDMADE_MATH__USE_SSE
    printf("SIMD: SSE enabled   | tile = %d\n", HMM_MATMUL_BATCH_TILE);
#elif defined(HANDMADE_MATH__USE_NEON)
    printf("SIMD: NEON (scalar batch kernel) | tile = %d\n",
           HMM_MATMUL_BATCH_TILE);
#else
    printf("SIMD: disabled (scalar fallback) | tile = %d\n",
           HMM_MATMUL_BATCH_TILE);
#endif
    printf("Batch size: %d matrix pairs, %d timed runs each\n\n", Count, Runs);

    /* -------------------------------------------------------------- */
    /* 1. Bit-exactness: 1000 random pairs, aligned + unaligned paths */
    /* -------------------------------------------------------------- */
    printf("Correctness: %d random matrix pairs, raw uint32 bit comparison\n",
           CorrectnessPairs);

    NaiveBatch(A, B, Ref, CorrectnessPairs);
    HMM_MulM4Batch(A, B, Opt, CorrectnessPairs);
    for (i = 0; i < CorrectnessPairs; ++i)
    {
        Lib[i] = HMM_MulM4(A[i], B[i]);
    }

    Failures = 0;
    for (i = 0; i < CorrectnessPairs; ++i)
    {
        if (!BitsEqual(&Ref[i], &Opt[i]))
        {
            printf("  MISMATCH batch vs naive at %d\n", i);
            ++Failures;
        }
        if (!BitsEqual(&Ref[i], &Lib[i]))
        {
            printf("  MISMATCH HMM_MulM4 vs naive at %d\n", i);
            ++Failures;
        }
    }

    /* Unaligned inputs: forces the unaligned SSE load path. */
    HMM_MulM4Batch(MisA, MisB, Mis, CorrectnessPairs);
    for (i = 0; i < CorrectnessPairs; ++i)
    {
        if (!BitsEqual(&Ref[i], &Mis[i]))
        {
            printf("  MISMATCH unaligned batch vs naive at %d\n", i);
            ++Failures;
        }
    }

    /* Edge cases: zeros (signed zero), identity, odd counts/tiles. */
    {
        HMM_Mat4 Zero = HMM_M4();
        HMM_Mat4 Identity = HMM_M4D(1.0f);
        HMM_Mat4 EOut, ERef;
        /* Fixed sizes, independent of HMM_MATMUL_BATCH_TILE, so an
         * oversized tile cannot make these allocations huge. The set
         * still straddles the default tile boundary (63/64/65) and the
         * two-product interleave remainder. */
        const int OddCounts[] = {1, 2, 3, 7, 31, 33, 63, 64, 65, 127, 129};
        size_t e;

        NaiveBatch(&Zero, &Identity, &ERef, 1);
        HMM_MulM4Batch(&Zero, &Identity, &EOut, 1);
        if (!BitsEqual(&ERef, &EOut))
        {
            printf("  MISMATCH zero*identity edge case\n");
            ++Failures;
        }
        NaiveBatch(&Identity, &Zero, &ERef, 1);
        HMM_MulM4Batch(&Identity, &Zero, &EOut, 1);
        if (!BitsEqual(&ERef, &EOut))
        {
            printf("  MISMATCH identity*zero edge case\n");
            ++Failures;
        }
        for (e = 0; e < sizeof(OddCounts) / sizeof(OddCounts[0]); ++e)
        {
            int n = OddCounts[e];
            HMM_Mat4 *ea = AlignedAllocMat4(n);
            HMM_Mat4 *eb = AlignedAllocMat4(n);
            HMM_Mat4 *eo = AlignedAllocMat4(n);
            HMM_Mat4 *er = AlignedAllocMat4(n);
            int j;
            if (!ea || !eb || !eo || !er)
            {
                fprintf(stderr, "edge-case allocation failed for n=%d\n", n);
                free(ea); free(eb); free(eo); free(er);
                return 2;
            }
            for (j = 0; j < n; ++j)
            {
                ea[j] = A[j];
                eb[j] = B[j];
            }
            NaiveBatch(ea, eb, er, n);
            HMM_MulM4Batch(ea, eb, eo, n);
            for (j = 0; j < n; ++j)
            {
                if (!BitsEqual(&er[j], &eo[j]))
                {
                    printf("  MISMATCH odd-count %d at %d\n", n, j);
                    ++Failures;
                }
            }
            free(ea); free(eb); free(eo); free(er);
        }
    }

    if (Failures == 0)
    {
        printf("  PASS: all %d random pairs + unaligned path + edge cases "
               "bit-identical to naive\n\n", CorrectnessPairs);
    }
    else
    {
        printf("  FAIL: %d mismatches\n\n", Failures);
        free(A); free(B); free(Ref); free(Opt); free(Lib); free(Mis); free(MisABase); free(MisBBase);
        return 1;
    }

    /* -------------------------------------------------------------- */
    /* 2. Performance: naive triple loop vs library vs optimized batch */
    /* -------------------------------------------------------------- */
    printf("Performance (%d pairs, best of %d runs):\n", Count, Runs);
    TNaive = Bench(NaiveWrap, A, B, Ref, Count, Runs, "naive triple loop", &Sink);
    TLib   = Bench(LibWrap,   A, B, Lib, Count, Runs, "HMM_MulM4 per item", &Sink);
    TBatch = Bench(BatchWrap, A, B, Opt, Count, Runs, "HMM_MulM4Batch", &Sink);

    {
        double GNaive = ((double)Count * FLOPS_PER_MATMUL) / TNaive / 1e9;
        double GLib   = ((double)Count * FLOPS_PER_MATMUL) / TLib   / 1e9;
        double GBatch = ((double)Count * FLOPS_PER_MATMUL) / TBatch / 1e9;

        printf("\nSummary (best run):\n");
        printf("  %-22s %7.2f GFLOPS\n", "naive triple loop", GNaive);
        printf("  %-22s %7.2f GFLOPS  (%.2fx vs naive)\n",
               "HMM_MulM4 per item", GLib, TNaive / TLib);
        printf("  %-22s %7.2f GFLOPS  (%.2fx vs naive)\n",
               "HMM_MulM4Batch", GBatch, TNaive / TBatch);
        if (TNaive / TBatch >= 3.0)
        {
            printf("  TARGET >= 3x: PASS\n");
        }
        else
        {
            printf("  TARGET >= 3x: FAIL\n");
        }
    }

    printf("\nchecksum sink: 0x%08x (prevents dead-code elimination)\n", Sink);
    free(A); free(B); free(Ref); free(Opt); free(Lib); free(Mis); free(MisABase); free(MisBBase);
    return 0;
}
