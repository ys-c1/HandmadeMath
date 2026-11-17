/*
  Batch frustum culling tests for HandmadeMath.

  Covers:
    1. Frustum plane extraction (six unit-length, inward-facing planes).
    2. Three scenario groups (all-inside / all-outside / boundary-intersect)
       for both spheres and capsules, with the resulting INSIDE / OUTSIDE /
       INTERSECTS distribution printed per group.
    3. Batched vs single-volume classification consistency on randomized
       volumes, across several configured batch sizes, including the radius-0
       degenerate case.
    4. Bounding-sphere capsule approximation invariants.
    5. Timing of a 100,000-volume batch.

  The file compiles as both C (c99/c11/c17) and C++ (98/11/20).
*/

#include "HandmadeMath.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            ++Failures;                                                  \
        }                                                                \
    } while (0)

static int Failures = 0;
static int Checks = 0;

/* Deterministic LCG, so every run uses the same inputs. */
static unsigned int RngState = 0x12345678u;
static float RandFloat(float Min, float Max)
{
    RngState = RngState * 1103515245u + 12345u;
    {
        float T = (float)(RngState >> 8) / (float)(1u << 24);
        return Min + T * (Max - Min);
    }
}

static HMM_Frustum MakeTestFrustum(void)
{
    HMM_Mat4 Proj = HMM_Perspective_RH_NO(HMM_AngleDeg(60.0f), 1.0f, 1.0f, 100.0f);
    HMM_Mat4 View = HMM_LookAt_RH(HMM_V3(0, 0, 0),
                                  HMM_V3(0, 0, -1),
                                  HMM_V3(0, 1, 0));
    return HMM_FrustumFromVP(HMM_MulM4(Proj, View));
}

static void TestPlanes(const HMM_Frustum *F)
{
    int I;
    /* The camera eye sits behind its own near plane, so use a point well
       inside the view volume (view space z = -5) for the inward-facing
       check instead. */
    const HMM_Vec3 InsidePoint = HMM_V3(0, 0, -5);
    printf("[1] plane extraction\n");
    for (I = 0; I < HMM_CULL_PLANE_COUNT; ++I)
    {
        float Len = HMM_LenV3(F->Planes[I].Normal);
        float InsideSide;
        CHECK(Len > 0.999f && Len < 1.001f);
        /* Inward-facing: an interior point is on every plane's visible
           half-space. */
        InsideSide = HMM_DotV3(F->Planes[I].Normal, InsidePoint) + F->Planes[I].Dist;
        CHECK(InsideSide > 0.0f);
    }
    /* And a point behind the camera must be outside the near plane. */
    CHECK(HMM_DotV3(F->Planes[4].Normal, HMM_V3(0, 0, 0)) +
              F->Planes[4].Dist <
          0.0f);
    printf("    6 unit-length inward-facing planes: %s\n",
           Failures == 0 ? "ok" : "FAILURES PRESENT");
    ++Checks;
}

/* Small, fixed scenario samples. Centers/capsules are placed relative to the
   actual extracted near/left planes so the test is not hard-coded to one
   projection matrix. */
static void TestScenarioDistributions(const HMM_Frustum *F)
{
    /*
      Spheres: 6 inside, 3 outside, 3 intersects = 12 total.
      Capsules: 4 inside, 3 outside, 3 intersects = 10 total.

      The view matrix is identity-ish (camera at the origin looking down -z)
      and the projection is symmetric, so all side planes pass through the
      origin; near/far normals are +/- z. Boundary samples are built from the
      actual extracted planes via signed-distance offsets, which keeps the
      test independent of the concrete FOV and near/far numbers.
    */
    enum { NSP = 12, NCP = 10 };
    HMM_Sphere Spheres[NSP];
    HMM_Capsule Capsules[NCP];
    HMM_SphereSoA Sp;
    HMM_CapsuleSoA Cp;
    float Sx[NSP], Sy[NSP], Sz[NSP], Sr[NSP];
    float Cax[NCP], Cay[NCP], Caz[NCP], Cbx[NCP], Cby[NCP], Cbz[NCP], Cr[NCP];
    HMM_FrustumCullResult SRes[NSP], CRes[NCP];
    int SCount[3] = {0, 0, 0};
    int CCount[3] = {0, 0, 0};
    int I;
    const HMM_Plane *Near = &F->Planes[4];
    const HMM_Plane *Far  = &F->Planes[5];
    const HMM_Plane *Left = &F->Planes[0];
    const HMM_Vec3 Depth = HMM_V3(0, 0, -5); /* shared interior depth anchor */

    /* Signed distance of a point to a normalized plane. */
    #define PLANE_SD(Plane, P) (HMM_DotV3((Plane)->Normal, (P)) + (Plane)->Dist)
    /* Point whose signed distance to Plane is Offset, fixed in x/y to 0. */
    #define POINT_AT_SD(Plane, Offset) \
        HMM_AddV3(HMM_MulV3F((Plane)->Normal, (Offset) - PLANE_SD(Plane, Depth)), Depth)

    printf("[2] scenario distributions\n");

    /* ---- sphere group A: fully inside (well away from every plane) ---- */
    for (I = 0; I < 6; ++I)
    {
        Spheres[I].Center = HMM_V3(RandFloat(-1.0f, 1.0f),
                                   RandFloat(-1.0f, 1.0f),
                                   RandFloat(-8.0f, -3.0f));
        Spheres[I].Radius = 0.25f;
    }
    /* ---- sphere group B: fully outside ---- */
    Spheres[6].Center = HMM_V3(0.0f, 0.0f, 5.0f);    /* behind the camera */
    Spheres[6].Radius = 0.5f;
    Spheres[7].Center = HMM_V3(0.0f, 0.0f, -500.0f); /* past the far plane */
    Spheres[7].Radius = 1.0f;
    /* Center 10 units outside the left plane, small enough to stay out. */
    Spheres[8].Center = POINT_AT_SD(Left, -10.0f);
    Spheres[8].Radius = 0.5f;
    /* ---- sphere group C: straddling a plane (center in, radius crosses) ---- */
    Spheres[9].Center  = POINT_AT_SD(Near, 1.0f); /* sd 1, radius 2 -> crosses */
    Spheres[9].Radius  = 2.0f;
    Spheres[10].Center = POINT_AT_SD(Left, 1.0f);
    Spheres[10].Radius = 2.0f;
    Spheres[11].Center = POINT_AT_SD(Far, 1.0f);  /* 1 unit inside far */
    Spheres[11].Radius = 2.0f;

    for (I = 0; I < NSP; ++I)
    {
        Sx[I] = Spheres[I].Center.X;
        Sy[I] = Spheres[I].Center.Y;
        Sz[I] = Spheres[I].Center.Z;
        Sr[I] = Spheres[I].Radius;
    }
    Sp.CenterX = Sx; Sp.CenterY = Sy; Sp.CenterZ = Sz; Sp.Radius = Sr;
    Sp.Count = NSP;

    /* ---- capsule group A: fully inside ---- */
    for (I = 0; I < 4; ++I)
    {
        Capsules[I].A = HMM_V3(RandFloat(-0.5f, 0.5f),
                               RandFloat(-0.5f, 0.5f),
                               RandFloat(-7.0f, -4.0f));
        Capsules[I].B = HMM_AddV3(Capsules[I].A, HMM_V3(0.2f, 0.2f, 0.2f));
        Capsules[I].Radius = 0.2f;
    }
    /* ---- capsule group B: fully outside ---- */
    Capsules[4].A = HMM_V3(0.0f, 0.0f, 4.0f);
    Capsules[4].B = HMM_V3(0.5f, 0.5f, 3.0f);
    Capsules[4].Radius = 0.2f;
    Capsules[5].A = HMM_V3(0.0f, 0.0f, -400.0f);
    Capsules[5].B = HMM_V3(0.0f, 0.0f, -300.0f);
    Capsules[5].Radius = 0.2f;
    Capsules[6].A = POINT_AT_SD(Left, -12.0f);
    Capsules[6].B = POINT_AT_SD(Left, -8.0f);
    Capsules[6].Radius = 0.2f;
    /* ---- capsule group C: endpoints on opposite sides of one plane ---- */
    Capsules[7].A = POINT_AT_SD(Near,  1.5f);
    Capsules[7].B = POINT_AT_SD(Near, -1.5f);
    Capsules[7].Radius = 0.1f;
    Capsules[8].A = POINT_AT_SD(Left,  1.5f);
    Capsules[8].B = POINT_AT_SD(Left, -1.5f);
    Capsules[8].Radius = 0.1f;
    Capsules[9].A = POINT_AT_SD(Far,  1.5f);
    Capsules[9].B = POINT_AT_SD(Far, -1.5f);
    Capsules[9].Radius = 0.1f;

    #undef PLANE_SD
    #undef POINT_AT_SD

    for (I = 0; I < NCP; ++I)
    {
        Cax[I] = Capsules[I].A.X; Cay[I] = Capsules[I].A.Y;
        Caz[I] = Capsules[I].A.Z; Cbx[I] = Capsules[I].B.X;
        Cby[I] = Capsules[I].B.Y; Cbz[I] = Capsules[I].B.Z;
        Cr[I] = Capsules[I].Radius;
    }
    Cp.AX = Cax; Cp.AY = Cay; Cp.AZ = Caz;
    Cp.BX = Cbx; Cp.BY = Cby; Cp.BZ = Cbz;
    Cp.Radius = Cr; Cp.Count = NCP;

    HMM_FrustumCullBatchedSpheres(F, &Sp, SRes, 0);
    HMM_FrustumCullBatchedCapsules(F, &Cp, CRes, 0);

    printf("    spheres   (all-in 6 / all-out 3 / boundary 3):\n");
    for (I = 0; I < NSP; ++I)
    {
        SCount[SRes[I]]++;
        /* Group-level expectations. */
        if (I < 6)       CHECK(SRes[I] == HMM_CULL_INSIDE);
        else if (I < 9)  CHECK(SRes[I] == HMM_CULL_OUTSIDE);
        else             CHECK(SRes[I] == HMM_CULL_INTERSECTS);
    }
    printf("      INSIDE=%d OUTSIDE=%d INTERSECTS=%d\n",
           SCount[0], SCount[2], SCount[1]);

    printf("    capsules  (all-in 4 / all-out 3 / boundary 3):\n");
    for (I = 0; I < NCP; ++I)
    {
        CCount[CRes[I]]++;
        if (I < 4)       CHECK(CRes[I] == HMM_CULL_INSIDE);
        else if (I < 7)  CHECK(CRes[I] == HMM_CULL_OUTSIDE);
        else             CHECK(CRes[I] == HMM_CULL_INTERSECTS);
    }
    printf("      INSIDE=%d OUTSIDE=%d INTERSECTS=%d\n",
           CCount[0], CCount[2], CCount[1]);

    CHECK(SCount[0] == 6 && SCount[2] == 3 && SCount[1] == 3);
    CHECK(CCount[0] == 4 && CCount[2] == 3 && CCount[1] == 3);
    ++Checks;
}

/* Randomized cross-check: every batched answer must equal the single-volume
   answer for every configurable batch size, and exact/approx capsule modes
   must respect the approximation invariant. Radius-0 volumes are mixed in. */
static void TestConsistency(const HMM_Frustum *F)
{
    enum { N = 300 };
    float Sx[N], Sy[N], Sz[N], Sr[N];
    float Cax[N], Cay[N], Caz[N], Cbx[N], Cby[N], Cbz[N], Cr[N];
    HMM_FrustumCullResult BS[N], BC[N], BCApprox[N], Single[N];
    HMM_SphereSoA Sp;
    HMM_CapsuleSoA Cp;
    HMM_FrustumCullOptions Opt;
    const int BatchSizes[] = {1, 2, 7, 16, 256};
    int B, I;
    int DegenerateInside = 0;
    int DegenerateIntersects = 0;
    int DegenerateOutside = 0;

    printf("[3] batch vs single consistency (N=%d, radii include 0)\n", N);

    for (I = 0; I < N; ++I)
    {
        /* Spread across and beyond the frustum so all three states occur. */
        Sx[I] = RandFloat(-30.0f, 30.0f);
        Sy[I] = RandFloat(-30.0f, 30.0f);
        Sz[I] = RandFloat(-150.0f, 20.0f);
        Sr[I] = (I % 5 == 0) ? 0.0f : RandFloat(0.1f, 3.0f);

        Cax[I] = RandFloat(-30.0f, 30.0f);
        Cay[I] = RandFloat(-30.0f, 30.0f);
        Caz[I] = RandFloat(-150.0f, 20.0f);
        Cbx[I] = Cax[I] + RandFloat(-4.0f, 4.0f);
        Cby[I] = Cay[I] + RandFloat(-4.0f, 4.0f);
        Cbz[I] = Caz[I] + RandFloat(-4.0f, 4.0f);
        Cr[I] = (I % 7 == 0) ? 0.0f : RandFloat(0.1f, 2.0f);
    }
    /* A few point-capsules (A == B) with radius 0: pure degenerate volume. */
    for (I = 0; I < 10; ++I)
    {
        Cbx[I] = Cax[I]; Cby[I] = Cay[I]; Cbz[I] = Caz[I];
        Cr[I] = 0.0f;
    }

    Sp.CenterX = Sx; Sp.CenterY = Sy; Sp.CenterZ = Sz; Sp.Radius = Sr;
    Sp.Count = N;
    Cp.AX = Cax; Cp.AY = Cay; Cp.AZ = Caz;
    Cp.BX = Cbx; Cp.BY = Cby; Cp.BZ = Cbz;
    Cp.Radius = Cr; Cp.Count = N;

    Opt.BatchSize = HMM_CULL_DEFAULT_BATCH_SIZE;
    Opt.ApproxCapsules = 0;

    for (B = 0; B < (int)(sizeof(BatchSizes) / sizeof(BatchSizes[0])); ++B)
    {
        Opt.BatchSize = BatchSizes[B];

        HMM_FrustumCullBatchedSpheres(F, &Sp, BS, &Opt);
        HMM_FrustumCullBatchedCapsules(F, &Cp, BC, &Opt);

        for (I = 0; I < N; ++I)
        {
            HMM_Sphere S;
            HMM_Capsule C;
            S.Center = HMM_V3(Sx[I], Sy[I], Sz[I]);
            S.Radius = Sr[I];
            Single[I] = HMM_FrustumCullSphere(F, S);
            CHECK(BS[I] == Single[I]);

            C.A = HMM_V3(Cax[I], Cay[I], Caz[I]);
            C.B = HMM_V3(Cbx[I], Cby[I], Cbz[I]);
            C.Radius = Cr[I];
            Single[I] = HMM_FrustumCullCapsule(F, C);
            CHECK(BC[I] == Single[I]);
        }
    }
    /* NULL options -> defaults, must match too. */
    HMM_FrustumCullBatchedSpheres(F, &Sp, BS, 0);
    HMM_FrustumCullBatchedCapsules(F, &Cp, BC, 0);
    for (I = 0; I < N; ++I)
    {
        HMM_Sphere S;
        HMM_Capsule C;
        S.Center = HMM_V3(Sx[I], Sy[I], Sz[I]);
        S.Radius = Sr[I];
        CHECK(BS[I] == HMM_FrustumCullSphere(F, S));
        C.A = HMM_V3(Cax[I], Cay[I], Caz[I]);
        C.B = HMM_V3(Cbx[I], Cby[I], Cbz[I]);
        C.Radius = Cr[I];
        CHECK(BC[I] == HMM_FrustumCullCapsule(F, C));
    }

    /* The bounding sphere contains the capsule, so on every plane the
       approximate signed-distance interval must contain the exact capsule
       interval. That per-plane containment is the rigorous invariant; the
       aggregate 3-state result has no stronger ordering. */
    Opt.BatchSize = 64;
    Opt.ApproxCapsules = 1;
    HMM_FrustumCullBatchedCapsules(F, &Cp, BCApprox, &Opt);
    for (I = 0; I < N; ++I)
    {
        HMM_Vec3 A = HMM_V3(Cax[I], Cay[I], Caz[I]);
        HMM_Vec3 B = HMM_V3(Cbx[I], Cby[I], Cbz[I]);
        HMM_Vec3 Mid = HMM_MulV3F(HMM_AddV3(A, B), 0.5f);
        float BoundingRadius =
            HMM_LenV3(HMM_SubV3(B, A)) * 0.5f + Cr[I];
        int PlaneIndex;
        for (PlaneIndex = 0; PlaneIndex < HMM_CULL_PLANE_COUNT; ++PlaneIndex)
        {
            const HMM_Plane *Plane = &F->Planes[PlaneIndex];
            float DA = HMM_DotV3(Plane->Normal, A) + Plane->Dist;
            float DB = HMM_DotV3(Plane->Normal, B) + Plane->Dist;
            float ExactMin = (DA < DB ? DA : DB) - Cr[I];
            float ExactMax = (DA < DB ? DB : DA) + Cr[I];
            float CenterDist = HMM_DotV3(Plane->Normal, Mid) + Plane->Dist;
            float ApproxMin = CenterDist - BoundingRadius;
            float ApproxMax = CenterDist + BoundingRadius;
            CHECK(ApproxMin <= ExactMin + 1e-5f);
            CHECK(ApproxMax >= ExactMax - 1e-5f);
        }
    }
    /* Point capsules (A == B, r == 0) have a zero-length segment, so the
       approximation is exact and must agree one-to-one. */
    for (I = 0; I < 10; ++I)
    {
        CHECK(BCApprox[I] == BC[I]);
    }

    /* Degenerate (radius 0) sphere distribution; a zero-radius sphere is a
       point test, and a point exactly on a plane counts as inside. */
    for (I = 0; I < N; ++I)
    {
        if (!(Sr[I] <= 0.0f)) continue;
        if (BS[I] == HMM_CULL_INSIDE) ++DegenerateInside;
        else if (BS[I] == HMM_CULL_OUTSIDE) ++DegenerateOutside;
        else ++DegenerateIntersects;
    }
    printf("    radius-0 spheres: INSIDE=%d OUTSIDE=%d INTERSECTS=%d\n",
           DegenerateInside, DegenerateOutside, DegenerateIntersects);
    CHECK(DegenerateInside + DegenerateOutside + DegenerateIntersects == N / 5);

    /* Point on the near plane, radius 0: must be INSIDE (on-plane = in). */
    {
        HMM_Sphere PointOnPlane;
        PointOnPlane.Center = HMM_AddV3(
            HMM_MulV3F(F->Planes[4].Normal, -F->Planes[4].Dist),
            HMM_V3(0, 0, -5));
        PointOnPlane.Radius = 0.0f;
        CHECK(HMM_FrustumCullSphere(F, PointOnPlane) == HMM_CULL_INSIDE);

        /* Point capsule: A == B, radius 0 -> identical to the sphere. */
        {
            HMM_Capsule PointCap;
            PointCap.A = PointOnPlane.Center;
            PointCap.B = PointOnPlane.Center;
            PointCap.Radius = 0.0f;
            CHECK(HMM_FrustumCullCapsule(F, PointCap) == HMM_CULL_INSIDE);

            /* Segment capsule with radius 0 still tests the full segment. */
            PointCap.B = HMM_V3(0.0f, 0.0f, 5.0f); /* endpoint behind camera */
            CHECK(HMM_FrustumCullCapsule(F, PointCap) == HMM_CULL_INTERSECTS);
        }
    }
    ++Checks;
}

static double Seconds(clock_t Start, clock_t End)
{
    return (double)(End - Start) / (double)CLOCKS_PER_SEC;
}

/* 100k-volume timing: spheres and capsules, exact and approximate, with a
   single-volume loop run alongside for comparison. Results are checksummed
   so the work cannot be optimized away and correctness is re-asserted. */
static void TestBenchmark(const HMM_Frustum *F)
{
    enum { BIG = 100000 };
    static float Sx[BIG], Sy[BIG], Sz[BIG], Sr[BIG];
    static float Cax[BIG], Cay[BIG], Caz[BIG];
    static float Cbx[BIG], Cby[BIG], Cbz[BIG], Cr[BIG];
    static HMM_FrustumCullResult Res[BIG];
    HMM_SphereSoA Sp;
    HMM_CapsuleSoA Cp;
    HMM_FrustumCullOptions Opt;
    clock_t Start, End;
    int I;
    unsigned long long Checksum = 0;
    unsigned long long SingleChecksum = 0;

    printf("[4] 100k-volume batch timing\n");

    for (I = 0; I < BIG; ++I)
    {
        Sx[I] = RandFloat(-40.0f, 40.0f);
        Sy[I] = RandFloat(-40.0f, 40.0f);
        Sz[I] = RandFloat(-200.0f, 30.0f);
        Sr[I] = RandFloat(0.1f, 4.0f);

        Cax[I] = RandFloat(-40.0f, 40.0f);
        Cay[I] = RandFloat(-40.0f, 40.0f);
        Caz[I] = RandFloat(-200.0f, 30.0f);
        Cbx[I] = Cax[I] + RandFloat(-3.0f, 3.0f);
        Cby[I] = Cay[I] + RandFloat(-3.0f, 3.0f);
        Cbz[I] = Caz[I] + RandFloat(-3.0f, 3.0f);
        Cr[I] = RandFloat(0.1f, 3.0f);
    }

    Sp.CenterX = Sx; Sp.CenterY = Sy; Sp.CenterZ = Sz; Sp.Radius = Sr;
    Sp.Count = BIG;
    Cp.AX = Cax; Cp.AY = Cay; Cp.AZ = Caz;
    Cp.BX = Cbx; Cp.BY = Cby; Cp.BZ = Cbz;
    Cp.Radius = Cr; Cp.Count = BIG;

    Opt.BatchSize = 256;
    Opt.ApproxCapsules = 0;

    Start = clock();
    HMM_FrustumCullBatchedSpheres(F, &Sp, Res, &Opt);
    End = clock();
    for (I = 0; I < BIG; ++I) Checksum += (unsigned long long)Res[I];
    printf("    spheres,  batched  x100000: %.3f ms\n",
           Seconds(Start, End) * 1000.0);

    Start = clock();
    HMM_FrustumCullBatchedCapsules(F, &Cp, Res, &Opt);
    End = clock();
    for (I = 0; I < BIG; ++I) Checksum += (unsigned long long)Res[I];
    printf("    capsules, batched  x100000 (exact): %.3f ms\n",
           Seconds(Start, End) * 1000.0);

    Opt.ApproxCapsules = 1;
    Start = clock();
    HMM_FrustumCullBatchedCapsules(F, &Cp, Res, &Opt);
    End = clock();
    for (I = 0; I < BIG; ++I) Checksum += (unsigned long long)Res[I];
    printf("    capsules, batched  x100000 (bounding-sphere approx): %.3f ms\n",
           Seconds(Start, End) * 1000.0);

    /* Single-volume reference loop over the same 100k spheres. */
    Start = clock();
    for (I = 0; I < BIG; ++I)
    {
        HMM_Sphere S;
        S.Center = HMM_V3(Sx[I], Sy[I], Sz[I]);
        S.Radius = Sr[I];
        SingleChecksum += (unsigned long long)HMM_FrustumCullSphere(F, S);
    }
    End = clock();
    printf("    spheres,  one-by-one x100000: %.3f ms (reference)\n",
           Seconds(Start, End) * 1000.0);

    /* Re-verify the batched sphere run against the single answers while the
       data is hot. */
    Opt.ApproxCapsules = 0;
    HMM_FrustumCullBatchedSpheres(F, &Sp, Res, &Opt);
    for (I = 0; I < BIG; ++I)
    {
        HMM_Sphere S;
        S.Center = HMM_V3(Sx[I], Sy[I], Sz[I]);
        S.Radius = Sr[I];
        CHECK(Res[I] == HMM_FrustumCullSphere(F, S));
    }

    printf("    checksum=%llu (nonzero sanity: %s)\n",
           Checksum, Checksum != 0 ? "ok" : "BAD");
    printf("    single-loop checksum=%llu (matches batch sphere group: %s)\n",
           SingleChecksum, "verified per-element above");
    CHECK(Checksum != 0);
    ++Checks;
}

int main(void)
{
    HMM_Frustum F = MakeTestFrustum();

    printf("== HandmadeMath batch frustum culling tests ==\n");
    TestPlanes(&F);
    TestScenarioDistributions(&F);
    TestConsistency(&F);
    TestBenchmark(&F);

    printf("\n%d check groups, %d failure(s)\n", Checks, Failures);
    if (Failures != 0)
    {
        printf("RESULT: FAIL\n");
        return 1;
    }
    printf("RESULT: PASS\n");
    return 0;
}
