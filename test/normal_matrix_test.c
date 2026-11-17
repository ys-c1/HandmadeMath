/*
  Standalone verification + benchmark for the batch normal-matrix API added to
  HandmadeMath.h. Not part of the repository build; build and run with
  ./run_normal_matrix_test.sh.

  Deliverables produced by this program:
    * normal-direction error for four transform groups
      (uniform scale, non-uniform scale, shear, degenerate)
    * bitwise difference between the batch API and per-element evaluation
    * elapsed time for a 100,000-element batch
    * explicit, NaN-free handling of degenerate transforms
*/

#include "../HandmadeMath.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NM_MAX_SAMPLES 16
#define NM_BENCH_COUNT 100000

#if defined(HANDMADE_MATH__USE_SSE)
# define NM_SIMD_LABEL "timing (SSE path)"
#elif defined(HANDMADE_MATH__USE_NEON)
# define NM_SIMD_LABEL "timing (NEON path)"
#else
# define NM_SIMD_LABEL "timing (scalar path)"
#endif

static int g_failures = 0;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("      FAIL: %s (line %d)\n", (msg), __LINE__); \
            ++g_failures; \
        } \
    } while (0)

/* ------------------------------ double reference ------------------------- */

/* Independent 3x3 inverse-transpose in double precision. A is row-major. */
static void nm_ref_normal_matrix(const double A[3][3], double N[3][3])
{
    double Det = A[0][0] * (A[1][1] * A[2][2] - A[1][2] * A[2][1])
               - A[0][1] * (A[1][0] * A[2][2] - A[1][2] * A[2][0])
               + A[0][2] * (A[1][0] * A[2][1] - A[1][1] * A[2][0]);
    double Inv[3][3];
    int Row, Col;

    Inv[0][0] = (A[1][1] * A[2][2] - A[1][2] * A[2][1]) / Det;
    Inv[0][1] = (A[0][2] * A[2][1] - A[0][1] * A[2][2]) / Det;
    Inv[0][2] = (A[0][1] * A[1][2] - A[0][2] * A[1][1]) / Det;
    Inv[1][0] = (A[1][2] * A[2][0] - A[1][0] * A[2][2]) / Det;
    Inv[1][1] = (A[0][0] * A[2][2] - A[0][2] * A[2][0]) / Det;
    Inv[1][2] = (A[0][2] * A[1][0] - A[0][0] * A[1][2]) / Det;
    Inv[2][0] = (A[1][0] * A[2][1] - A[1][1] * A[2][0]) / Det;
    Inv[2][1] = (A[0][1] * A[2][0] - A[0][0] * A[2][1]) / Det;
    Inv[2][2] = (A[0][0] * A[1][1] - A[0][1] * A[1][0]) / Det;

    for (Row = 0; Row < 3; ++Row)
    {
        for (Col = 0; Col < 3; ++Col)
        {
            N[Row][Col] = Inv[Col][Row]; /* transpose */
        }
    }
}

/* HMM stores columns; convert the 3x3 linear part to row-major doubles. */
static void nm_hmm_to_rows(HMM_Mat4 M, double A[3][3])
{
    int Row, Col;
    for (Row = 0; Row < 3; ++Row)
    {
        for (Col = 0; Col < 3; ++Col)
        {
            A[Row][Col] = (double)M.Elements[Col][Row];
        }
    }
}

static double nm_dir_error_deg(HMM_Vec3 Result, const double Ref[3])
{
    double Len = sqrt(Ref[0] * Ref[0] + Ref[1] * Ref[1] + Ref[2] * Ref[2]);
    double Dot, Cos;

    if (!(Len > 0.0))
    {
        return -1.0;
    }

    Dot = ((double)Result.X * Ref[0] + (double)Result.Y * Ref[1] + (double)Result.Z * Ref[2])
        / (Len * (double)HMM_LenV3(Result));
    Cos = Dot;
    if (Cos > 1.0) Cos = 1.0;
    if (Cos < -1.0) Cos = -1.0;

    return acos(Cos) * (180.0 / 3.14159265358979323846);
}

/* ------------------------------ helpers ---------------------------------- */

static HMM_Bool nm_vec_finite(HMM_Vec3 V)
{
    /* Ordered comparisons reject NaN and infinity without float equality. */
    return (HMM_Bool)(V.X <= 3.0e38f && V.X >= -3.0e38f
                      && V.Y <= 3.0e38f && V.Y >= -3.0e38f
                      && V.Z <= 3.0e38f && V.Z >= -3.0e38f);
}

static HMM_Bool nm_mat_finite(HMM_Mat3 M)
{
    int Index;
    for (Index = 0; Index < 3; ++Index)
    {
        if (!nm_vec_finite(M.Columns[Index])) return 0;
    }
    return 1;
}

static HMM_Bool nm_bits_equal(float A, float B)
{
    return (HMM_Bool)(memcmp(&A, &B, sizeof(float)) == 0);
}

static HMM_Bool nm_vec_bits_equal(HMM_Vec3 A, HMM_Vec3 B)
{
    return (HMM_Bool)(nm_bits_equal(A.X, B.X) && nm_bits_equal(A.Y, B.Y) && nm_bits_equal(A.Z, B.Z));
}

static HMM_Bool nm_mat_bits_equal(HMM_Mat3 A, HMM_Mat3 B)
{
    int Index;
    for (Index = 0; Index < 3; ++Index)
    {
        if (!nm_vec_bits_equal(A.Columns[Index], B.Columns[Index])) return 0;
    }
    return 1;
}

static HMM_Mat4 nm_from_linear(HMM_Mat3 Linear)
{
    HMM_Mat4 Result = HMM_M4D(1.0f);
    int Index;
    for (Index = 0; Index < 3; ++Index)
    {
        Result.Columns[Index] = HMM_V4V(Linear.Columns[Index], 0.0f);
    }
    return Result;
}

static HMM_Vec3 nm_rotate_apply(HMM_Mat4 R, HMM_Vec3 V)
{
    HMM_Mat3 Rotation;
    int Index;
    for (Index = 0; Index < 3; ++Index)
    {
        Rotation.Columns[Index] = R.Columns[Index].XYZ;
    }
    return HMM_MulM3V3(Rotation, V);
}

/* ------------------------------ samples ---------------------------------- */

typedef struct nm_samples
{
    HMM_Mat4 Transforms[NM_MAX_SAMPLES];
    HMM_Vec3 Normals[NM_MAX_SAMPLES];
    HMM_Bool HasReference[NM_MAX_SAMPLES];
    HMM_Mat4 Rotations[NM_MAX_SAMPLES]; /* reference rotation, if any */
} nm_samples;

static void nm_make_uniform(nm_samples *S)
{
    int Index;
    for (Index = 0; Index < NM_MAX_SAMPLES; ++Index)
    {
        float Angle = 0.2f * (float)Index;
        float Scale = 0.5f + 0.25f * (float)(Index % 6);
        HMM_Mat4 R = HMM_Rotate_RH(Angle, HMM_V3(0.3f, 1.0f, -0.2f));
        S->Rotations[Index] = R;
        S->Transforms[Index] = HMM_MulM4(R, HMM_Scale(HMM_V3(Scale, Scale, Scale)));
        S->Normals[Index] = HMM_NormV3(HMM_V3(1.0f, -2.0f, 0.5f + 0.1f * (float)Index));
        S->HasReference[Index] = 1;
    }
}

static void nm_make_nonuniform(nm_samples *S)
{
    int Index;
    for (Index = 0; Index < NM_MAX_SAMPLES; ++Index)
    {
        float Angle = 0.15f * (float)Index;
        HMM_Mat4 R = HMM_Rotate_RH(Angle, HMM_V3(1.0f, 2.0f, 0.5f));
        HMM_Vec3 Scale = HMM_V3(2.0f + 0.1f * (float)Index, 0.5f + 0.05f * (float)Index, 4.0f - 0.1f * (float)Index);
        S->Rotations[Index] = R;
        S->Transforms[Index] = HMM_MulM4(R, HMM_Scale(Scale));
        S->Normals[Index] = HMM_NormV3(HMM_V3(0.2f, 1.0f, 0.3f - 0.05f * (float)Index));
        S->HasReference[Index] = 1;
    }
}

static void nm_make_shear(nm_samples *S)
{
    int Index;
    for (Index = 0; Index < NM_MAX_SAMPLES; ++Index)
    {
        float Sh = 0.1f * (float)(Index + 1);
        HMM_Mat3 Linear = {0};
        HMM_Mat4 R = HMM_Rotate_RH(0.05f * (float)Index, HMM_V3(0.0f, 0.0f, 1.0f));
        Linear.Columns[0] = HMM_V3(1.0f, Sh, 0.0f);
        Linear.Columns[1] = HMM_V3(0.0f, 1.0f, 0.0f);
        Linear.Columns[2] = HMM_V3(0.0f, 0.0f, 1.0f);
        S->Rotations[Index] = R;
        S->Transforms[Index] = HMM_MulM4(R, nm_from_linear(Linear));
        S->Normals[Index] = HMM_NormV3(HMM_V3(0.0f, 1.0f, 0.2f * (float)(Index % 3)));
        S->HasReference[Index] = 1;
    }
}

static void nm_make_degenerate(nm_samples *S)
{
    int Index;
    for (Index = 0; Index < NM_MAX_SAMPLES; ++Index)
    {
        float Angle = 0.1f * (float)Index;
        HMM_Mat4 R = HMM_Rotate_RH(Angle, HMM_V3(0.0f, 1.0f, 0.0f));
        HMM_Vec3 Scale = HMM_V3(1.0f, 1.0f, (Index % 3 == 0) ? 0.0f : 0.0f);
        S->Rotations[Index] = R;

        if (Index % 4 == 3)
        {
            /* Fully collapsed: no direction survives. Only finiteness and the
               degenerate flag are meaningful. */
            S->Transforms[Index] = HMM_Scale(HMM_V3(0.0f, 0.0f, 0.0f));
            S->Normals[Index] = HMM_V3(0.0f, 1.0f, 0.0f);
            S->HasReference[Index] = 0;
        }
        else
        {
            /* One collapsed axis; use a normal in the surviving plane so the
               regularized approximation has a meaningful reference. */
            S->Transforms[Index] = HMM_MulM4(R, HMM_Scale(Scale));
            S->Normals[Index] = HMM_NormV3(HMM_V3(1.0f, 0.3f * (float)(Index % 3), 0.0f));
            S->HasReference[Index] = 1;
        }
    }
}

/* ------------------------------ group report ----------------------------- */

static void nm_report_group(const char *Name, nm_samples *S, HMM_NormalMatrixConfig Config)
{
    HMM_Mat3 PerElement[NM_MAX_SAMPLES];
    HMM_Vec3 PerElementVectors[NM_MAX_SAMPLES];
    HMM_Mat3 BatchMatrices[NM_MAX_SAMPLES];
    HMM_Vec3 BatchVectors[NM_MAX_SAMPLES];
    HMM_NormalMatrixStatus PerStatus[NM_MAX_SAMPLES];
    HMM_NormalMatrixStatus BatchStatus[NM_MAX_SAMPLES];
    double MaxError = 0.0;
    int BitDifferences = 0;
    int Degenerate = 0;
    int NotFinite = 0;
    int Index;

    for (Index = 0; Index < NM_MAX_SAMPLES; ++Index)
    {
        PerElement[Index] = HMM_NormalMatrixM4(S->Transforms[Index], Config, &PerStatus[Index]);
        PerElementVectors[Index] = HMM_NormalM4(S->Normals[Index], S->Transforms[Index], Config, 0);
        if (!nm_mat_finite(PerElement[Index]) || !nm_vec_finite(PerElementVectors[Index]))
        {
            ++NotFinite;
        }
        if (PerStatus[Index] == HMM_NormalMatrixStatus_Degenerate)
        {
            ++Degenerate;
        }
    }

    Config.BatchSize = 5;
    (void)HMM_NormalMatrixBatchM4(BatchMatrices, S->Transforms, NM_MAX_SAMPLES, Config, BatchStatus);
    (void)HMM_NormalBatchM4(BatchVectors, S->Normals, S->Transforms, NM_MAX_SAMPLES, Config, 0);

    for (Index = 0; Index < NM_MAX_SAMPLES; ++Index)
    {
        if (!nm_mat_bits_equal(PerElement[Index], BatchMatrices[Index])
            || !nm_vec_bits_equal(PerElementVectors[Index], BatchVectors[Index]))
        {
            ++BitDifferences;
        }

        if (S->HasReference[Index])
        {
            double A[3][3];
            double N[3][3];
            double Ref[3];
            HMM_Vec3 Rotated;
            double Error;

            nm_hmm_to_rows(S->Transforms[Index], A);
            nm_ref_normal_matrix(A, N);
            Ref[0] = N[0][0] * (double)S->Normals[Index].X
                   + N[0][1] * (double)S->Normals[Index].Y
                   + N[0][2] * (double)S->Normals[Index].Z;
            Ref[1] = N[1][0] * (double)S->Normals[Index].X
                   + N[1][1] * (double)S->Normals[Index].Y
                   + N[1][2] * (double)S->Normals[Index].Z;
            Ref[2] = N[2][0] * (double)S->Normals[Index].X
                   + N[2][1] * (double)S->Normals[Index].Y
                   + N[2][2] * (double)S->Normals[Index].Z;

            if (PerStatus[Index] == HMM_NormalMatrixStatus_Degenerate && !(fabs(Ref[0]) + fabs(Ref[1]) + fabs(Ref[2]) > 0.0))
            {
                /* Reference vanished along the collapse; compare with the
                   rotation applied to the surviving normal instead. */
                Rotated = nm_rotate_apply(S->Rotations[Index], S->Normals[Index]);
                Ref[0] = (double)Rotated.X;
                Ref[1] = (double)Rotated.Y;
                Ref[2] = (double)Rotated.Z;
            }

            Error = nm_dir_error_deg(PerElementVectors[Index], Ref);
            if (Error > MaxError)
            {
                MaxError = Error;
            }
        }
    }

    printf("  %-16s dir_error=%8.4f deg  bitwise_diffs=%d  degenerate=%2d/%d  non_finite=%d\n",
           Name, MaxError, BitDifferences, Degenerate, NM_MAX_SAMPLES, NotFinite);

    CHECK(BitDifferences == 0, "batch differs bitwise from per-element");
    CHECK(NotFinite == 0, "non-finite output produced");
    if (strcmp(Name, "degenerate") == 0)
    {
        CHECK(Degenerate == NM_MAX_SAMPLES, "degenerate input not detected");
        CHECK(MaxError < 0.05, "degenerate approximation not usable");
    }
    else
    {
        CHECK(Degenerate == 0, "regular transform misreported as degenerate");
        CHECK(MaxError < 0.05, "normal direction error too large");
    }
}

/* ------------------------------ benchmark -------------------------------- */

static void nm_benchmark(void)
{
    HMM_Mat4 *Transforms = (HMM_Mat4 *)malloc(sizeof(HMM_Mat4) * NM_BENCH_COUNT);
    HMM_Vec3 *Normals = (HMM_Vec3 *)malloc(sizeof(HMM_Vec3) * NM_BENCH_COUNT);
    HMM_Mat3 *OutMatrices = (HMM_Mat3 *)malloc(sizeof(HMM_Mat3) * NM_BENCH_COUNT);
    HMM_Vec3 *OutVectors = (HMM_Vec3 *)malloc(sizeof(HMM_Vec3) * NM_BENCH_COUNT);
    HMM_Mat3 *RefMatrices = (HMM_Mat3 *)malloc(sizeof(HMM_Mat3) * NM_BENCH_COUNT);
    HMM_Vec3 *RefVectors = (HMM_Vec3 *)malloc(sizeof(HMM_Vec3) * NM_BENCH_COUNT);
    HMM_NormalMatrixStatus *Status = (HMM_NormalMatrixStatus *)malloc(sizeof(HMM_NormalMatrixStatus) * NM_BENCH_COUNT);
    HMM_NormalMatrixConfig Config = HMM_NormalMatrixConfigDefault();
    int BatchSizes[3];
    int ExpectedDegenerate = 0;
    int Index;
    int BatchIndex;

    if (!Transforms || !Normals || !OutMatrices || !OutVectors || !RefMatrices || !RefVectors || !Status)
    {
        printf("  allocation failed\n");
        ++g_failures;
        free(Transforms); free(Normals); free(OutMatrices);
        free(OutVectors); free(RefMatrices); free(RefVectors); free(Status);
        return;
    }

    for (Index = 0; Index < NM_BENCH_COUNT; ++Index)
    {
        float Angle = (float)(Index % 1000) * 0.001f;
        HMM_Vec3 Scale = HMM_V3(1.0f + 0.001f * (float)(Index % 97),
                                1.0f + 0.001f * (float)(Index % 53),
                                (Index % 211 == 0) ? 0.0f : 1.0f + 0.001f * (float)(Index % 31));
        Transforms[Index] = HMM_MulM4(HMM_Rotate_RH(Angle, HMM_V3(1.0f, 0.5f, 0.25f)), HMM_Scale(Scale));
        Normals[Index] = HMM_NormV3(HMM_V3((float)(Index % 7) - 3.0f, 1.0f, (float)(Index % 5) - 2.0f));

        if (Index % 211 == 0)
        {
            ++ExpectedDegenerate;
        }
    }

    /* Per-element reference for the bitwise comparison. */
    for (Index = 0; Index < NM_BENCH_COUNT; ++Index)
    {
        RefMatrices[Index] = HMM_NormalMatrixM4(Transforms[Index], Config, 0);
        RefVectors[Index] = HMM_NormalM4(Normals[Index], Transforms[Index], Config, 0);
    }

    BatchSizes[0] = 0;      /* whole batch */
    BatchSizes[1] = 64;
    BatchSizes[2] = 1024;

    printf("  %s\n", NM_SIMD_LABEL);
    for (BatchIndex = 0; BatchIndex < 3; ++BatchIndex)
    {
        int Repetition;
        int Degenerate = 0;
        int BitDifferences = 0;
        double BestMatrixMs = 1.0e30;
        double BestVectorMs = 1.0e30;

        for (Repetition = 0; Repetition < 3; ++Repetition)
        {
            clock_t Start = clock();
            Degenerate = HMM_NormalMatrixBatchM4(OutMatrices, Transforms, NM_BENCH_COUNT, Config, Status);
            {
                double Elapsed = 1000.0 * (double)(clock() - Start) / (double)CLOCKS_PER_SEC;
                if (Elapsed < BestMatrixMs) BestMatrixMs = Elapsed;
            }

            Start = clock();
            (void)HMM_NormalBatchM4(OutVectors, Normals, Transforms, NM_BENCH_COUNT, Config, 0);
            {
                double Elapsed = 1000.0 * (double)(clock() - Start) / (double)CLOCKS_PER_SEC;
                if (Elapsed < BestVectorMs) BestVectorMs = Elapsed;
            }
        }

        for (Index = 0; Index < NM_BENCH_COUNT; ++Index)
        {
            if (!nm_mat_bits_equal(OutMatrices[Index], RefMatrices[Index])
                || !nm_vec_bits_equal(OutVectors[Index], RefVectors[Index]))
            {
                ++BitDifferences;
            }
        }

        printf("    batch_size=%-6d matrices: %7.3f ms (%6.2f M/s)  vectors: %7.3f ms  bitwise_diffs=%d  degenerate=%d\n",
               BatchSizes[BatchIndex], BestMatrixMs,
               (double)NM_BENCH_COUNT / (BestMatrixMs * 1000.0),
               BestVectorMs, BitDifferences, Degenerate);

        CHECK(BitDifferences == 0, "100k batch differs bitwise from per-element");
        CHECK(Degenerate == ExpectedDegenerate, "unexpected degenerate count in benchmark");
    }

    free(Transforms); free(Normals); free(OutMatrices);
    free(OutVectors); free(RefMatrices); free(RefVectors); free(Status);
}

/* ------------------------------ main ------------------------------------- */

int main(void)
{
    nm_samples Samples;
    HMM_NormalMatrixConfig Config = HMM_NormalMatrixConfigDefault();

    printf("Batch normal-matrix verification\n");
    printf("  ============================================================\n");

    Config.Normalize = 1;

    nm_make_uniform(&Samples);
    nm_report_group("uniform", &Samples, Config);

    nm_make_nonuniform(&Samples);
    nm_report_group("non-uniform", &Samples, Config);

    nm_make_shear(&Samples);
    nm_report_group("shear", &Samples, Config);

    nm_make_degenerate(&Samples);
    nm_report_group("degenerate", &Samples, Config);

    printf("  ============================================================\n");

    /* Config edge cases: zero normal stays zero, normalization toggles. */
    {
        HMM_Mat4 Transform = HMM_Scale(HMM_V3(2.0f, 2.0f, 2.0f));
        HMM_Vec3 Zero = HMM_V3(0.0f, 0.0f, 0.0f);
        HMM_Vec3 Unit = HMM_V3(0.0f, 0.0f, 1.0f);
        HMM_Vec3 Result;

        Config.Normalize = 1;
        Result = HMM_NormalM4(Zero, Transform, Config, 0);
        CHECK(nm_vec_finite(Result) && Result.X == 0.0f && Result.Y == 0.0f && Result.Z == 0.0f,
              "zero normal did not stay zero");

        Config.Normalize = 0;
        Result = HMM_NormalM4(Unit, Transform, Config, 0);
        CHECK(nm_vec_finite(Result) && fabs(HMM_LenV3(Result) - 0.5) < 1e-6,
              "non-normalized length wrong");

        Config.Normalize = 1;
        Result = HMM_NormalM4(Unit, Transform, Config, 0);
        CHECK(fabs(HMM_LenV3(Result) - 1.0f) < 1e-6f, "normalized length wrong");
    }

    /* A merely small (but invertible) scale must not be called degenerate. */
    {
        HMM_NormalMatrixStatus Status;
        HMM_Mat3 N = HMM_NormalMatrixM4(HMM_Scale(HMM_V3(0.01f, 0.01f, 0.01f)), Config, &Status);
        CHECK(Status == HMM_NormalMatrixStatus_Ok, "small invertible scale flagged degenerate");
        CHECK(fabs(N.Elements[0][0] - 100.0f) < 0.01f, "small scale inverse wrong");
    }

    /* Batch size must not change results. */
    {
        nm_samples Local;
        int Sizes[4];
        int SizeIndex;

        nm_make_nonuniform(&Local);
        Sizes[0] = 1; Sizes[1] = 3; Sizes[2] = 7; Sizes[3] = 0;

        {
            HMM_Mat3 Reference[NM_MAX_SAMPLES];
            Config.BatchSize = 0;
            Config.Normalize = 1;
            (void)HMM_NormalMatrixBatchM4(Reference, Local.Transforms, NM_MAX_SAMPLES, Config, 0);

            for (SizeIndex = 0; SizeIndex < 4; ++SizeIndex)
            {
                HMM_Mat3 Candidate[NM_MAX_SAMPLES];
                int Index;
                Config.BatchSize = Sizes[SizeIndex];
                (void)HMM_NormalMatrixBatchM4(Candidate, Local.Transforms, NM_MAX_SAMPLES, Config, 0);
                for (Index = 0; Index < NM_MAX_SAMPLES; ++Index)
                {
                    CHECK(nm_mat_bits_equal(Reference[Index], Candidate[Index]),
                          "batch size changed results");
                }
            }
        }
    }

    /* Empty batch is a no-op. */
    {
        Config.BatchSize = 4;
        CHECK(HMM_NormalMatrixBatchM4(0, 0, 0, Config, 0) == 0, "empty batch not handled");
    }

    printf("  ============================================================\n");
    nm_benchmark();

    printf("  ============================================================\n");
    if (g_failures == 0)
    {
        printf("RESULT: PASS\n");
        return 0;
    }

    printf("RESULT: FAIL (%d checks failed)\n", g_failures);
    return 1;
}
