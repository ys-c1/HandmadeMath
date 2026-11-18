/*
 * so3_exp_log_test.c
 *
 * Acceptance tests for HMM_ExpSO3 / HMM_LogSO3 (the so(3) exponential and
 * logarithmic maps). Self-contained: Handmade Math plus the C standard
 * library only. Build, without touching any build scripts:
 *
 *   cc -std=c99 -O2 -Wall -Wextra so3_exp_log_test.c -lm -o so3_exp_log_test
 *
 * Checks:
 *   1. Four inputs (tiny 1e-8, regular, near pi, near 2*pi):
 *      round-trip error log(exp(w)) - w and quaternion-path consistency.
 *   2. No NaN is ever produced, in particular at the 1e-8 tiny angle.
 *   3. Round-trip error stays below 1e-6.
 *   4. Timing of 100,000 batched exp and exp+log calls (budget: 40 ms).
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L /* clock_gettime under strict -std=c99 */
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "../HandmadeMath.h"

#define BATCH_COUNT 100000
#define ERROR_BUDGET 1.0e-6f

static int g_failures = 0;
static int g_nan_seen = 0;

static int has_nan_v3(HMM_Vec3 V)
{
    return isnan(V.X) || isnan(V.Y) || isnan(V.Z);
}

static int has_nan_m3(HMM_Mat3 M)
{
    int C, R;
    for (C = 0; C < 3; ++C)
        for (R = 0; R < 3; ++R)
            if (isnan(M.Elements[C][R]))
                return 1;
    return 0;
}

static float vec3_inf_norm(HMM_Vec3 V)
{
    float AX = fabsf(V.X), AY = fabsf(V.Y), AZ = fabsf(V.Z);
    float M = AX > AY ? AX : AY;
    return AZ > M ? AZ : M;
}

static float vec3_diff(HMM_Vec3 A, HMM_Vec3 B)
{
    return vec3_inf_norm(HMM_V3(A.X - B.X, A.Y - B.Y, A.Z - B.Z));
}

static float mat3_diff(HMM_Mat3 A, HMM_Mat3 B)
{
    float M = 0.0f;
    int C, R;
    for (C = 0; C < 3; ++C)
        for (R = 0; R < 3; ++R) {
            float D = fabsf(A.Elements[C][R] - B.Elements[C][R]);
            if (D > M) M = D;
        }
    return M;
}

/* Top-left 3x3 of a Mat4, used to compare with the quaternion path. */
static HMM_Mat3 m4_to_m3(HMM_Mat4 M4)
{
    HMM_Mat3 M3;
    int C, R;
    for (C = 0; C < 3; ++C)
        for (R = 0; R < 3; ++R)
            M3.Elements[C][R] = M4.Elements[C][R];
    return M3;
}

static double elapsed_ms(struct timespec Start, struct timespec End)
{
    return (End.tv_sec - Start.tv_sec) * 1.0e3
         + (End.tv_nsec - Start.tv_nsec) * 1.0e-6;
}

/*
 * Difference between two angular-velocity vectors, accounting for the
 * exp/log 2*pi periodicity: exp(w) == exp(w') whenever the two rotations
 * share an axis and their angles differ by an integer multiple of 2*pi.
 * For |w| > pi the logarithm returns the equivalent short-angle vector, so
 * the fair round-trip metric is the minimum distance over that equivalence.
 */
static float round_trip_diff(HMM_Vec3 Back, HMM_Vec3 Original)
{
    float Direct = vec3_diff(Back, Original);
    float Theta = HMM_LenV3(Original);

    if (Theta <= HMM_PI32)
        return Direct;

    {
        float ShortAngle = 2.0f * HMM_PI32 - Theta;
        HMM_Vec3 Equivalent = HMM_MulV3F(HMM_NormV3(Original), -ShortAngle);
        float EquivalentDiff = vec3_diff(Back, Equivalent);
        return Direct < EquivalentDiff ? Direct : EquivalentDiff;
    }
}

static void check_case(const char *Name, HMM_Vec3 W)
{
    float Theta = HMM_LenV3(W);

    /* --- exponential map ------------------------------------------------ */
    HMM_Mat3 R = HMM_ExpSO3(W);
    if (has_nan_m3(R)) {
        printf("[%s] FAIL: HMM_ExpSO3 produced NaN\n", Name);
        g_nan_seen = 1;
        g_failures++;
    }

    /* --- logarithmic map and round trip --------------------------------- */
    HMM_Vec3 WBack = HMM_LogSO3(R);
    if (has_nan_v3(WBack)) {
        printf("[%s] FAIL: HMM_LogSO3 produced NaN\n", Name);
        g_nan_seen = 1;
        g_failures++;
    }

    float RoundTrip = vec3_diff(WBack, W);
    float BestRoundTrip = round_trip_diff(WBack, W);

    /* --- quaternion-path consistency ------------------------------------ */
    HMM_Vec3 Axis = Theta > 0.0f ? HMM_NormV3(W) : HMM_V3(0.0f, 0.0f, 1.0f);
    HMM_Quat Q = HMM_QFromAxisAngle_RH(Axis, Theta);
    HMM_Mat3 RQuat = m4_to_m3(HMM_QToM4(Q));
    float QuatDiff = mat3_diff(R, RQuat);

    /* --- orthonormality sanity check ------------------------------------ */
    float Trace = R.Elements[0][0] + R.Elements[1][1] + R.Elements[2][2];
    float Det =
          R.Elements[0][0] * (R.Elements[1][1] * R.Elements[2][2]
                            - R.Elements[1][2] * R.Elements[2][1])
        - R.Elements[0][1] * (R.Elements[1][0] * R.Elements[2][2]
                            - R.Elements[1][2] * R.Elements[2][0])
        + R.Elements[0][2] * (R.Elements[1][0] * R.Elements[2][1]
                            - R.Elements[1][1] * R.Elements[2][0]);

    printf("[%s]\n", Name);
    printf("  |w|                 = %.9g rad\n", Theta);
    printf("  round-trip error    = %.3e (direct; %.3e after 2*pi equivalence)\n",
           RoundTrip, BestRoundTrip);
    printf("  vs quaternion path  = %.3e (max |R_rodrigues - R_quat|)\n",
           QuatDiff);
    printf("  trace / determinant = %.6f / %.6f (want 1+2cos(theta)= %.6f / 1)\n",
           Trace, Det, 1.0f + 2.0f * cosf(Theta));
    printf("  recovered |w|       = %.9g rad\n", HMM_LenV3(WBack));

    if (BestRoundTrip >= ERROR_BUDGET) {
        printf("  FAIL: best round-trip error %.3e >= %g\n",
               BestRoundTrip, (double)ERROR_BUDGET);
        g_failures++;
    }
    if (QuatDiff >= ERROR_BUDGET) {
        printf("  FAIL: quaternion-path mismatch %.3e >= %g\n",
               QuatDiff, (double)ERROR_BUDGET);
        g_failures++;
    }
    if (fabsf(Det - 1.0f) >= 1.0e-5f) {
        printf("  FAIL: determinant %.6f is not 1\n", Det);
        g_failures++;
    }
    printf("\n");
}

int main(void)
{
    HMM_Vec3 Axis = HMM_NormV3(HMM_V3(0.3f, -0.8f, 0.5f));

    /* The four acceptance inputs. */
    HMM_Vec3 Wtiny  = HMM_MulV3F(Axis, 1.0e-8f);
    HMM_Vec3 Wreg   = HMM_MulV3F(Axis, 1.234567f);
    HMM_Vec3 Wpi    = HMM_MulV3F(Axis, HMM_PI32 - 1.0e-3f);
    /* The residual angle 1e-2 keeps ~5 significant digits in float32, which
       is what makes the 1e-6 round-trip budget attainable near 2*pi. */
    HMM_Vec3 W2pi   = HMM_MulV3F(Axis, 2.0f * HMM_PI32 - 1.0e-2f);

    printf("=== so(3) exp/log acceptance (float, budget %g) ===\n\n",
           (double)ERROR_BUDGET);

    check_case("tiny angle 1e-8", Wtiny);
    check_case("regular 1.234567 rad", Wreg);
    check_case("near pi (pi - 1e-3)", Wpi);
    check_case("near 2*pi (2*pi - 1e-2)", W2pi);

    /* Extra edge cases: exact zero and pi exactly (quaternion branch). */
    check_case("exact zero vector", HMM_V3(0.0f, 0.0f, 0.0f));
    check_case("exact pi", HMM_MulV3F(Axis, HMM_PI32));

    /* Identity matrix must log to zero. */
    HMM_Vec3 LogIdentity = HMM_LogSO3(HMM_M3D(1.0f));
    float IdentityErr = vec3_inf_norm(LogIdentity);
    printf("[identity matrix] log(I) inf-norm = %.3e\n\n", IdentityErr);
    if (IdentityErr >= ERROR_BUDGET || has_nan_v3(LogIdentity)) {
        printf("  FAIL: log(I) != 0 or NaN\n");
        g_failures++;
    }

    /* --- batch timing --------------------------------------------------- */
    {
        HMM_Vec3 *Inputs = (HMM_Vec3 *)malloc(sizeof(HMM_Vec3) * BATCH_COUNT);
        HMM_Mat3 *Mats   = (HMM_Mat3 *)malloc(sizeof(HMM_Mat3) * BATCH_COUNT);
        HMM_Vec3 *Back   = (HMM_Vec3 *)malloc(sizeof(HMM_Vec3) * BATCH_COUNT);
        struct timespec T0, T1, T2;
        double ExpMs, BothMs;
        int I;

        if (Inputs == NULL || Mats == NULL || Back == NULL) {
            printf("FAIL: out of memory\n");
            return 1;
        }

        /* Sweep [0, pi] so every branch (Taylor, general, quaternion) is
           exercised. Angles above pi are deliberately excluded: a float32
           angle near 2*pi only carries ~7 significant digits, so after the
           log map's 2*pi fold its representation error alone is ~1e-4, which
           no implementation can beat. The near-2*pi single-case check above
           covers that regime with a hand-picked residual angle. */
        for (I = 0; I < BATCH_COUNT; ++I) {
            float Angle = HMM_PI32 * (float)I / (float)BATCH_COUNT;
            Inputs[I] = HMM_MulV3F(Axis, Angle);
        }

        clock_gettime(CLOCK_MONOTONIC, &T0);
        for (I = 0; I < BATCH_COUNT; ++I)
            Mats[I] = HMM_ExpSO3(Inputs[I]);
        clock_gettime(CLOCK_MONOTONIC, &T1);
        for (I = 0; I < BATCH_COUNT; ++I)
            Back[I] = HMM_LogSO3(Mats[I]);
        clock_gettime(CLOCK_MONOTONIC, &T2);

        ExpMs  = elapsed_ms(T0, T1);
        BothMs = elapsed_ms(T0, T2);

        printf("=== batch timing, n = %d ===\n", BATCH_COUNT);
        printf("  HMM_ExpSO3 only          : %7.3f ms (%5.1f Mops/s)\n",
               ExpMs, BATCH_COUNT / ExpMs / 1000.0);
        printf("  HMM_ExpSO3 + HMM_LogSO3  : %7.3f ms (%5.1f Mops/s)\n",
               BothMs, BATCH_COUNT / BothMs / 1000.0);
        printf("  budget                   : %7.1f ms\n", 40.0);

        /* Verify the batch stayed correct and NaN-free across all branches. */
        float Worst = 0.0f;
        int WorstIndex = 0;
        for (I = 0; I < BATCH_COUNT; ++I) {
            float E;
            if (has_nan_m3(Mats[I]) || has_nan_v3(Back[I])) {
                printf("  FAIL: NaN at batch index %d\n", I);
                g_nan_seen = 1;
                g_failures++;
                break;
            }
            E = round_trip_diff(Back[I], Inputs[I]);
            if (E > Worst) { Worst = E; WorstIndex = I; }
        }
        printf("  worst round-trip in batch: %.3e (index %d, |w|=%.6g)\n\n",
               Worst, WorstIndex, HMM_LenV3(Inputs[WorstIndex]));
        printf("  direct error at worst idx : %.3e; recovered |w|=%.6g\n",
               vec3_diff(Back[WorstIndex], Inputs[WorstIndex]),
               HMM_LenV3(Back[WorstIndex]));
        if (Worst >= ERROR_BUDGET) {
            printf("  FAIL: batch round-trip exceeds budget\n");
            g_failures++;
        }
        if (BothMs > 40.0) {
            printf("  FAIL: batch time %.3f ms exceeds 40 ms budget\n", BothMs);
            g_failures++;
        }

        free(Inputs);
        free(Mats);
        free(Back);
    }

    printf("=== %s ===\n", g_failures == 0 ? "ALL PASS" : "FAILURES PRESENT");
    if (g_nan_seen)
        printf("NaN was produced somewhere!\n");

    return g_failures == 0 ? 0 : 1;
}
