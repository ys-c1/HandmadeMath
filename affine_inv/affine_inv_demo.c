/*
 * affine_inv_demo.c
 *
 * Standalone test and benchmark for the affine fast path added to
 * HandmadeMath.h. It uses only Handmade Math and the C/C++ standard
 * library -- no third-party dependencies.
 *
 * It exercises, in one run:
 *
 *   1. Affine recognition (HMM_IsAffineM4) on four small sample matrices:
 *      a pure rotation, a scaling (with translation), a shear, and a
 *      perspective projection.
 *   2. Bit-for-bit comparison between the general inverse
 *      (HMM_InvGeneralM4) and the dispatching inverse
 *      (HMM_InvGeneralM4Fast), plus an M * M^-1 ~= I sanity check.
 *   3. The approximate-affine rule: a matrix whose last row is only close
 *      to (0, 0, 0, 1), with the per-component residuals printed so the
 *      tolerance decision is fully justified.
 *   4. A 100,000-iteration timing comparison of the two paths.
 *
 * The build mode (fast path compiled in or out) is reported at startup.
 * The program exits nonzero if any correctness expectation fails.
 */

#define _POSIX_C_SOURCE 199309L

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../HandmadeMath.h"

#define BENCH_ITERS 100000
#define IDENTITY_EPS 2e-5f

typedef struct Sample
{
    const char *Name;
    HMM_Mat4 Matrix;
} Sample;

static uint32_t BitsOf(float Value)
{
    uint32_t Bits;
    memcpy(&Bits, &Value, sizeof(Bits));
    return Bits;
}

/* ULP-style distance between two floats. -0.0 and +0.0 are the same value
   (they compare equal) and report distance zero; other sign differences are
   reported as a large sentinel. */
static uint32_t UlpDiff(float A, float B)
{
    if (A == B)
    {
        return 0; /* also covers +0.0 vs -0.0 */
    }
    uint32_t BitsA = BitsOf(A);
    uint32_t BitsB = BitsOf(B);
    if ((BitsA ^ BitsB) & 0x80000000u)
    {
        return 0xFFFFFFFFu;
    }
    return BitsA > BitsB ? BitsA - BitsB : BitsB - BitsA;
}

static int BitwiseEqualM4(HMM_Mat4 A, HMM_Mat4 B)
{
    for (int Row = 0; Row < 4; ++Row)
    {
        for (int Column = 0; Column < 4; ++Column)
        {
            if (BitsOf(A.Elements[Row][Column]) != BitsOf(B.Elements[Row][Column]))
            {
                return 0;
            }
        }
    }
    return 1;
}

static uint32_t MaxUlpDiffM4(HMM_Mat4 A, HMM_Mat4 B)
{
    uint32_t Max = 0;
    for (int Row = 0; Row < 4; ++Row)
    {
        for (int Column = 0; Column < 4; ++Column)
        {
            uint32_t Diff = UlpDiff(A.Elements[Row][Column], B.Elements[Row][Column]);
            if (Diff > Max)
            {
                Max = Diff;
            }
        }
    }
    return Max;
}

/* Counts components whose bit patterns differ. Values that are equal as
   floats -- notably -0.0 vs +0.0, which differ only in the sign bit -- are
   not counted, since they are the same value numerically. */
static int CountBitDiffsM4(HMM_Mat4 A, HMM_Mat4 B)
{
    int Count = 0;
    for (int Row = 0; Row < 4; ++Row)
    {
        for (int Column = 0; Column < 4; ++Column)
        {
            float AValue = A.Elements[Row][Column];
            float BValue = B.Elements[Row][Column];
            if (AValue != BValue && BitsOf(AValue) != BitsOf(BValue))
            {
                ++Count;
            }
        }
    }
    return Count;
}

static float MaxIdentityError(HMM_Mat4 Product)
{
    float MaxError = 0.0f;
    for (int Row = 0; Row < 4; ++Row)
    {
        for (int Column = 0; Column < 4; ++Column)
        {
            float Expected = (Row == Column) ? 1.0f : 0.0f;
            float Error = Product.Elements[Row][Column] - Expected;
            if (Error < 0.0f)
            {
                Error = -Error;
            }
            if (Error > MaxError)
            {
                MaxError = Error;
            }
        }
    }
    return MaxError;
}

static void PrintRow(HMM_Mat4 Matrix, int Row)
{
    /* Column-major storage: Elements[column][row]. */
    printf("  [% .6f % .6f % .6f % .6f]\n",
           Matrix.Elements[0][Row], Matrix.Elements[1][Row],
           Matrix.Elements[2][Row], Matrix.Elements[3][Row]);
}

static double SecondsNow(void)
{
    struct timespec Point;
    clock_gettime(CLOCK_MONOTONIC, &Point);
    return (double)Point.tv_sec + (double)Point.tv_nsec * 1e-9;
}

static float BenchmarkGeneral(const Sample *Samples, int Count, HMM_Mat4 *Sink)
{
    double Start = SecondsNow();
    float Checksum = 0.0f;
    for (int Iteration = 0; Iteration < BENCH_ITERS; ++Iteration)
    {
        HMM_Mat4 Inverse = HMM_InvGeneralM4(Samples[Iteration % Count].Matrix);
        Checksum += Inverse.Elements[0][0] + Inverse.Elements[3][3];
    }
    double Elapsed = SecondsNow() - Start;
    *Sink = HMM_InvGeneralM4(Samples[0].Matrix);
    /* Keep the checksum observable so the loop cannot be optimized away. */
    volatile float Guard = Checksum;
    (void)Guard;
    return Elapsed;
}

static float BenchmarkFast(const Sample *Samples, int Count, HMM_Mat4 *Sink)
{
    double Start = SecondsNow();
    float Checksum = 0.0f;
    for (int Iteration = 0; Iteration < BENCH_ITERS; ++Iteration)
    {
        HMM_Mat4 Inverse = HMM_InvGeneralM4Fast(Samples[Iteration % Count].Matrix,
                                                1, HANDMADE_MATH_AFFINE_INV_EPS);
        Checksum += Inverse.Elements[0][0] + Inverse.Elements[3][3];
    }
    double Elapsed = SecondsNow() - Start;
    *Sink = HMM_InvGeneralM4Fast(Samples[0].Matrix, 1, HANDMADE_MATH_AFFINE_INV_EPS);
    volatile float Guard = Checksum;
    (void)Guard;
    return Elapsed;
}

int main(void)
{
    int Failures = 0;

    printf("=== Handmade Math affine-inverse demo ===\n");
#if HANDMADE_MATH_AFFINE_INV
    printf("build: HANDMADE_MATH_AFFINE_INV=1 (fast path compiled in), default eps=%g\n",
           (double)HANDMADE_MATH_AFFINE_INV_EPS);
#else
    printf("build: HANDMADE_MATH_AFFINE_INV=0 (fast path compiled out), default eps=%g\n",
           (double)HANDMADE_MATH_AFFINE_INV_EPS);
#endif

    /* ------------------------------------------------------------------ */
    /* Build the four small sample matrices.                              */
    /* ------------------------------------------------------------------ */

    /* 1. Pure rotation, no translation. */
    HMM_Vec3 Axis = HMM_NormV3(HMM_V3(1.0f, 2.0f, 3.0f));
    HMM_Mat4 Rotation = HMM_Rotate_RH(HMM_AngleDeg(37.0f), Axis);

    /* 2. Scaling composed with a translation. */
    HMM_Mat4 Scaling = HMM_MulM4(HMM_Translate(HMM_V3(3.0f, 4.0f, -5.0f)),
                                 HMM_Scale(HMM_V3(2.0f, -3.0f, 0.5f)));

    /* 3. Shear in the 3x3 block plus a translation. Matrices are
       column-major: Elements[column][row]. */
    HMM_Mat4 Shear = HMM_M4D(1.0f);
    /* shear terms in the upper-left 3x3 block */
    Shear.Elements[1][0] = 0.3f; /* row 0, column 1 */
    Shear.Elements[2][0] = 0.1f; /* row 0, column 2 */
    Shear.Elements[0][1] = 0.2f; /* row 1, column 0 */
    Shear.Elements[2][1] = 0.4f; /* row 1, column 2 */
    Shear.Elements[0][2] = 0.5f; /* row 2, column 0 */
    Shear.Elements[1][2] = 0.1f; /* row 2, column 1 */
    /* translation lives in the last column: Elements[3][0..2] */
    Shear.Elements[3][0] = 1.0f;
    Shear.Elements[3][1] = -2.0f;
    Shear.Elements[3][2] = 0.7f;

    /* 4. Perspective projection: genuinely non-affine (last row != 0,0,0,1). */
    HMM_Mat4 Projection = HMM_Perspective_RH_NO(HMM_AngleDeg(60.0f),
                                                 16.0f / 9.0f, 0.5f, 100.0f);

    Sample Samples[4] = {
        {"rotation (pure)", Rotation},
        {"scaling+translate", Scaling},
        {"shear+translate", Shear},
        {"perspective projection", Projection},
    };
    int AffineExpect[4] = {1, 1, 1, 0};

    /* ------------------------------------------------------------------ */
    /* 1. Recognition.                                                    */
    /* ------------------------------------------------------------------ */

    printf("\n[1] Affine recognition (last row and classification)\n");
    for (int Index = 0; Index < 4; ++Index)
    {
        HMM_Mat4 M = Samples[Index].Matrix;
        HMM_Bool Exact = HMM_IsAffineM4(M, 0.0f);
        HMM_Bool Loose = HMM_IsAffineM4(M, HANDMADE_MATH_AFFINE_INV_EPS);
        printf("  %s\n", Samples[Index].Name);
        for (int Row = 0; Row < 4; ++Row)
        {
            PrintRow(M, Row);
        }
        /* Last row = row 3 of each of the four (column-major) columns. */
        printf("    last row = (%.9g, %.9g, %.9g, %.9g)\n",
               M.Elements[0][3], M.Elements[1][3],
               M.Elements[2][3], M.Elements[3][3]);
        printf("    translation column = (%.9g, %.9g, %.9g, %.9g)\n",
               M.Elements[3][0], M.Elements[3][1],
               M.Elements[3][2], M.Elements[3][3]);
        printf("    affine(eps=0, exact) = %d; affine(eps=%g) = %d; expected affine = %d\n",
               (int)Exact, (double)HANDMADE_MATH_AFFINE_INV_EPS,
               (int)Loose, AffineExpect[Index]);
        if ((int)Exact != AffineExpect[Index] || (int)Loose != AffineExpect[Index])
        {
            printf("    FAIL: recognition mismatch\n");
            ++Failures;
        }
    }

    /* ------------------------------------------------------------------ */
    /* 2. Two-path results: bitwise diffs and correctness.                */
    /* ------------------------------------------------------------------ */

    printf("\n[2] General path vs fast path (16 floats compared per matrix)\n");
    for (int Index = 0; Index < 4; ++Index)
    {
        HMM_Mat4 M = Samples[Index].Matrix;
        HMM_Mat4 General = HMM_InvGeneralM4(M);
        HMM_Mat4 ForcedGeneral = HMM_InvGeneralM4Fast(M, 0, HANDMADE_MATH_AFFINE_INV_EPS);
        HMM_Mat4 Fast = HMM_InvGeneralM4Fast(M, 1, HANDMADE_MATH_AFFINE_INV_EPS);

        if (!BitwiseEqualM4(General, ForcedGeneral))
        {
            printf("  %s: FAIL (UseFastPath=0 must always be bit-identical)\n",
                   Samples[Index].Name);
            ++Failures;
        }

        int Diffs = CountBitDiffsM4(General, Fast);
        uint32_t MaxUlp = MaxUlpDiffM4(General, Fast);
        float IdentityError = MaxIdentityError(HMM_MulM4(M, Fast));

#if HANDMADE_MATH_AFFINE_INV
        const char *Route = AffineExpect[Index] ? "affine fast path" : "general fallback";
#else
        const char *Route = "general fallback (fast path compiled out)";
#endif
        printf("  %-24s route: %-28s bit-diffs: %2d/16  max ULP: %u  |M*M^-1 - I|_inf: %.3g\n",
               Samples[Index].Name, Route, Diffs, MaxUlp, (double)IdentityError);

        if (IdentityError > IDENTITY_EPS)
        {
            printf("    FAIL: inverse does not reconstruct the identity\n");
            ++Failures;
        }
#if !HANDMADE_MATH_AFFINE_INV
        if (Diffs != 0)
        {
            printf("    FAIL: with the fast path compiled out results must be bit-identical\n");
            ++Failures;
        }
#else
        if (!AffineExpect[Index] && Diffs != 0)
        {
            printf("    FAIL: non-affine matrices must use the unchanged general path\n");
            ++Failures;
        }
#endif
    }

    /* ------------------------------------------------------------------ */
    /* 3. Approximate-affine decision, with evidence.                     */
    /* ------------------------------------------------------------------ */

    printf("\n[3] Approximate-affine matrix (last row perturbed)\n");
    HMM_Mat4 NearAffine = Rotation;
    /* Perturb row 3 of columns 0 and 3: Elements[column][row]. */
    NearAffine.Elements[0][3] = 3.0e-7f;
    NearAffine.Elements[3][3] = 1.0f + 5.0e-7f;

    float const Target[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    printf("  rule: affine iff for every column c, |last[c] - target[c]| <= eps,\n");
    printf("        target = (0, 0, 0, 1), last[c] = Elements[c][3] (column-major)\n");
    for (int Column = 0; Column < 4; ++Column)
    {
        float Residual = NearAffine.Elements[Column][3] - Target[Column];
        float Absolute = Residual < 0.0f ? -Residual : Residual;
        printf("    column %d: last=%.9g target=%.1f residual=%+.3g |residual|=%.3g\n",
               Column, (double)NearAffine.Elements[Column][3],
               (double)Target[Column], (double)Residual, (double)Absolute);
    }

    struct ToleranceCase
    {
        float Epsilon;
        int ExpectAffine;
        const char *Reason;
    } ToleranceCases[3] = {
        {0.0f, 0, "exact comparison; nonzero residual fails"},
        {1.0e-8f, 0, "eps smaller than both nonzero residuals (3e-7, 5e-7)"},
        {1.0e-6f, 1, "eps larger than every residual (max 5e-7)"},
    };
    for (int Case = 0; Case < 3; ++Case)
    {
        HMM_Bool IsAffine = HMM_IsAffineM4(NearAffine, ToleranceCases[Case].Epsilon);
        printf("  eps=%-9g -> affine=%d  (%s)\n",
               (double)ToleranceCases[Case].Epsilon, (int)IsAffine,
               ToleranceCases[Case].Reason);
        if ((int)IsAffine != ToleranceCases[Case].ExpectAffine)
        {
            printf("    FAIL: tolerance decision mismatch\n");
            ++Failures;
        }
    }
    /* The exact original rotation must still be affine under every tolerance. */
    if (!HMM_IsAffineM4(Rotation, 0.0f) || !HMM_IsAffineM4(Rotation, 1.0e-8f))
    {
        printf("  FAIL: exact affine matrix rejected\n");
        ++Failures;
    }

    /* ------------------------------------------------------------------ */
    /* 4. Batch timing: 100,000 inversions of affine matrices.            */
    /* ------------------------------------------------------------------ */

    printf("\n[4] Batch timing, %d iterations over the three affine samples\n",
           BENCH_ITERS);
    Sample AffineSamples[3] = {Samples[0], Samples[1], Samples[2]};
    HMM_Mat4 SinkGeneral;
    HMM_Mat4 SinkFast;

    /* Warm up to take first-call and cache effects out of the measurement. */
    (void)BenchmarkGeneral(AffineSamples, 3, &SinkGeneral);
    (void)BenchmarkFast(AffineSamples, 3, &SinkFast);

    double GeneralSeconds = BenchmarkGeneral(AffineSamples, 3, &SinkGeneral);
    double FastSeconds = BenchmarkFast(AffineSamples, 3, &SinkFast);

    /* Both sinks must independently reconstruct the identity. */
    float GeneralIdentityError = MaxIdentityError(
        HMM_MulM4(AffineSamples[0].Matrix, SinkGeneral));
    float FastIdentityError = MaxIdentityError(
        HMM_MulM4(AffineSamples[0].Matrix, SinkFast));

    printf("  HMM_InvGeneralM4     : %8.3f ms total, %7.2f ns/call\n",
           GeneralSeconds * 1e3, GeneralSeconds * 1e9 / (double)BENCH_ITERS);
    printf("  HMM_InvGeneralM4Fast : %8.3f ms total, %7.2f ns/call\n",
           FastSeconds * 1e3, FastSeconds * 1e9 / (double)BENCH_ITERS);
    printf("  speedup: %.2fx (identity error general=%.3g, fast=%.3g)\n",
           GeneralSeconds / FastSeconds,
           (double)GeneralIdentityError, (double)FastIdentityError);

    if (GeneralIdentityError > IDENTITY_EPS || FastIdentityError > IDENTITY_EPS)
    {
        printf("    FAIL: benchmarked inverse does not reconstruct the identity\n");
        ++Failures;
    }

    if (Failures != 0)
    {
        printf("\n%d check(s) FAILED\n", Failures);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
