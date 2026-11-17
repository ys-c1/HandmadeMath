/*
  segment_grid_test.c

  Standalone test + benchmark for the segment closest-point / uniform-grid
  API added to HandmadeMath.h. Not part of the Makefile; build directly:

    cc -std=c99 -O2 -Wall -Wextra segment_grid_test.c -lm -o segment_grid_test

  Optional arguments: [num_segments] [batch_size] [frames]
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../HandmadeMath.h"

static int Failures = 0;

static void Check(int Cond, const char *What)
{
    if (!Cond)
    {
        ++Failures;
        printf("  FAIL: %s\n", What);
    }
}

static int BitEqF(float A, float B)
{
    return memcmp(&A, &B, sizeof(float)) == 0;
}

static int BitEqV2(HMM_Vec2 A, HMM_Vec2 B)
{
    return BitEqF(A.X, B.X) && BitEqF(A.Y, B.Y);
}

static void PrintClosest(const char *Label, HMM_ClosestPoint2D R)
{
    printf("  %-44s -> point=(% .6f, % .6f) dist=%.6f seg=%d\n",
           Label, R.Point.X, R.Point.Y, R.Distance, R.SegmentIndex);
}

/* ------------------------------------------------------------------ */
/* Deterministic RNG (xorshift32) so runs are reproducible.            */
/* ------------------------------------------------------------------ */

static unsigned int RngState = 0x12345678u;

static unsigned int RandU(void)
{
    unsigned int X = RngState;
    X ^= X << 13;
    X ^= X >> 17;
    X ^= X << 5;
    RngState = X;
    return X;
}

static float RandF(float Lo, float Hi)
{
    return Lo + (Hi - Lo) * (float)(RandU() >> 8) * (1.0f / 16777216.0f);
}

/* ------------------------------------------------------------------ */
/* Part 1: single-segment geometry cases                               */
/* ------------------------------------------------------------------ */

static void TestGeometry(void)
{
    printf("== Part 1: single-segment closest point ==\n");

    /* Case 1: foot of the perpendicular falls inside the segment. */
    {
        HMM_Segment2D S;
        HMM_ClosestPoint2D R;
        S.A = HMM_V2(0.0f, 0.0f);
        S.B = HMM_V2(4.0f, 0.0f);
        R = HMM_ClosestPointOnSegment(HMM_V2(1.5f, 2.0f), S);
        PrintClosest("foot inside segment", R);
        Check(BitEqF(R.Point.X, 1.5f) && BitEqF(R.Point.Y, 0.0f), "case1 point");
        Check(BitEqF(R.Distance, 2.0f), "case1 distance");
    }

    /* Case 2: foot falls beyond both endpoints (nearest is an endpoint). */
    {
        HMM_Segment2D S;
        HMM_ClosestPoint2D R1, R2;
        S.A = HMM_V2(0.0f, 0.0f);
        S.B = HMM_V2(4.0f, 0.0f);
        R1 = HMM_ClosestPointOnSegment(HMM_V2(7.0f, 4.0f), S);
        R2 = HMM_ClosestPointOnSegment(HMM_V2(-3.0f, -4.0f), S);
        PrintClosest("beyond endpoint B", R1);
        PrintClosest("beyond endpoint A", R2);
        Check(BitEqV2(R1.Point, HMM_V2(4.0f, 0.0f)) && BitEqF(R1.Distance, 5.0f), "case2 B");
        Check(BitEqV2(R2.Point, HMM_V2(0.0f, 0.0f)) && BitEqF(R2.Distance, 5.0f), "case2 A");
    }

    /* Case 3: degenerate zero-length segment (A == B), treated as a point. */
    {
        HMM_Segment2D S;
        HMM_ClosestPoint2D R;
        S.A = HMM_V2(1.0f, 2.0f);
        S.B = HMM_V2(1.0f, 2.0f);
        R = HMM_ClosestPointOnSegment(HMM_V2(4.0f, 6.0f), S);
        PrintClosest("zero-length segment", R);
        Check(BitEqV2(R.Point, HMM_V2(1.0f, 2.0f)), "case3 point");
        Check(BitEqF(R.Distance, 5.0f), "case3 distance");
    }

    /* Case 4: query point identical to a segment point (distance 0). */
    {
        HMM_Segment2D S, D;
        HMM_ClosestPoint2D R1, R2;
        S.A = HMM_V2(2.0f, 3.0f);
        S.B = HMM_V2(8.0f, 3.0f);
        D.A = HMM_V2(5.0f, 5.0f);
        D.B = HMM_V2(5.0f, 5.0f);
        R1 = HMM_ClosestPointOnSegment(HMM_V2(2.0f, 3.0f), S);
        R2 = HMM_ClosestPointOnSegment(HMM_V2(5.0f, 5.0f), D);
        PrintClosest("query == endpoint A", R1);
        PrintClosest("query == degenerate point", R2);
        Check(BitEqV2(R1.Point, HMM_V2(2.0f, 3.0f)) && BitEqF(R1.Distance, 0.0f), "case4 endpoint");
        Check(BitEqV2(R2.Point, HMM_V2(5.0f, 5.0f)) && BitEqF(R2.Distance, 0.0f), "case4 degenerate");
    }

    /* Empty scene: grid and brute force must agree on the -1 sentinel. */
    {
        HMM_SegmentGrid Grid;
        int Buffer[128];
        HMM_ClosestPoint2D RG, RB;
        int InitOK = HMM_SegmentGridInit(&Grid, 0, 0, 0.0f, 0.0f, 10.0f, 10.0f, 1.0f,
                                         Buffer, 128);
        Check(InitOK == 1, "empty grid init");
        if (InitOK == 1)
        {
            RG = HMM_SegmentGridQuery(&Grid, HMM_V2(1.0f, 1.0f));
            RB = HMM_ClosestPointBruteForce(HMM_V2(1.0f, 1.0f), 0, 0);
            PrintClosest("empty scene (grid)", RG);
            Check(RG.SegmentIndex == -1 && RB.SegmentIndex == -1, "empty scene index");
            Check(BitEqF(RG.Distance, RB.Distance), "empty scene distance");
        }
    }

    printf("\n");
}

/* ------------------------------------------------------------------ */
/* Scene generation                                                    */
/* ------------------------------------------------------------------ */

#define SCENE_MIN 0.0f
#define SCENE_MAX 100.0f

static HMM_Segment2D *MakeScene(int NumSegments)
{
    HMM_Segment2D *Segs = (HMM_Segment2D *)malloc((size_t)NumSegments * sizeof(HMM_Segment2D));
    int I;

    RngState = 0xC0FFEE11u;
    for (I = 0; I < NumSegments; ++I)
    {
        float X = RandF(SCENE_MIN, SCENE_MAX);
        float Y = RandF(SCENE_MIN, SCENE_MAX);
        Segs[I].A = HMM_V2(X, Y);
        if (RandU() % 20 == 0)
        {
            Segs[I].B = Segs[I].A; /* 5% zero-length segments */
        }
        else
        {
            float Angle = RandF(0.0f, 6.2831853f);
            float Len = RandF(0.5f, 5.0f);
            Segs[I].B = HMM_V2(X + Len * HMM_CosF(Angle), Y + Len * HMM_SinF(Angle));
        }
    }
    return Segs;
}

static HMM_Vec2 *MakeQueries(int Count)
{
    HMM_Vec2 *Pts = (HMM_Vec2 *)malloc((size_t)Count * sizeof(HMM_Vec2));
    int I;

    RngState = 0xBADC0DEu;
    for (I = 0; I < Count; ++I)
    {
        /* 10% of queries land outside the grid bounds on purpose. */
        Pts[I] = HMM_V2(RandF(SCENE_MIN - 10.0f, SCENE_MAX + 10.0f),
                        RandF(SCENE_MIN - 10.0f, SCENE_MAX + 10.0f));
    }
    return Pts;
}

static int BuildGrid(HMM_SegmentGrid *Grid, const HMM_Segment2D *Segs, int NumSegments,
                     float CellSize, int **OutBuffer)
{
    int Needed = HMM_SegmentGridBufferSize(Segs, NumSegments,
                                           SCENE_MIN, SCENE_MIN, SCENE_MAX, SCENE_MAX, CellSize);
    int *Buffer;
    if (Needed <= 0)
    {
        return 0;
    }
    Buffer = (int *)malloc((size_t)Needed * sizeof(int));
    if (HMM_SegmentGridInit(Grid, Segs, NumSegments,
                            SCENE_MIN, SCENE_MIN, SCENE_MAX, SCENE_MAX,
                            CellSize, Buffer, Needed) != 1)
    {
        free(Buffer);
        return 0;
    }
    *OutBuffer = Buffer;
    return Needed;
}

/* ------------------------------------------------------------------ */
/* Part 2: grid vs brute force consistency                             */
/* ------------------------------------------------------------------ */

static void TestConsistency(const HMM_Segment2D *Segs, int NumSegments, const HMM_Vec2 *Queries,
                            int NumQueries, const float *CellSizes, int NumCellSizes)
{
    int C;

    printf("== Part 2: grid vs brute force, %d queries x %d segments ==\n",
           NumQueries, NumSegments);

    for (C = 0; C < NumCellSizes; ++C)
    {
        float CellSize = CellSizes[C];
        HMM_SegmentGrid Grid;
        int *Buffer = 0;
        int Needed = BuildGrid(&Grid, Segs, NumSegments, CellSize, &Buffer);
        int Q, Mismatches = 0;
        float MaxDiff = 0.0f;

        Check(Needed > 0, "grid init (consistency)");
        if (Needed <= 0)
        {
            continue;
        }

        for (Q = 0; Q < NumQueries; ++Q)
        {
            HMM_ClosestPoint2D G = HMM_SegmentGridQuery(&Grid, Queries[Q]);
            HMM_ClosestPoint2D B = HMM_ClosestPointBruteForce(Queries[Q], Segs, NumSegments);
            float Diff = HMM_ABS(G.Distance - B.Distance);

            if (!BitEqV2(G.Point, B.Point) || !BitEqF(G.Distance, B.Distance) ||
                G.SegmentIndex != B.SegmentIndex)
            {
                ++Mismatches;
                if (Mismatches <= 3)
                {
                    printf("  mismatch q=%d: grid d=%.8f seg=%d, brute d=%.8f seg=%d\n",
                           Q, G.Distance, G.SegmentIndex, B.Distance, B.SegmentIndex);
                }
            }
            if (Diff > MaxDiff)
            {
                MaxDiff = Diff;
            }
        }

        printf("  cell=%5.2f grid=%3dx%-3d items=%6d buf=%7d ints  mismatches=%d  max|dd|=%g\n",
               CellSize, Grid.Width, Grid.Height, Grid.NumItems, Needed, Mismatches, MaxDiff);
        Check(Mismatches == 0, "grid/brute consistency");

        free(Buffer);
    }
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* Part 3: batch benchmark                                             */
/* ------------------------------------------------------------------ */

static double NowSeconds(void)
{
    return (double)clock() / (double)CLOCKS_PER_SEC;
}

static void Benchmark(const HMM_Segment2D *Segs, int NumSegments, const HMM_Vec2 *Queries,
                      int BatchSize, int Frames, const float *CellSizes, int NumCellSizes)
{
    double T0, T1, BruteMs = 0.0;
    int C, F;
    HMM_ClosestPoint2D *Results =
        (HMM_ClosestPoint2D *)malloc((size_t)BatchSize * sizeof(HMM_ClosestPoint2D));
    volatile float Sink = 0.0f;

    printf("== Part 3: batch benchmark, %d frames x batch %d, %d segments ==\n",
           Frames, BatchSize, NumSegments);

    /* Brute force reference. */
    T0 = NowSeconds();
    for (F = 0; F < Frames; ++F)
    {
        int Q;
        for (Q = 0; Q < BatchSize; ++Q)
        {
            Results[Q] = HMM_ClosestPointBruteForce(Queries[Q], Segs, NumSegments);
        }
        Sink += Results[0].Distance;
    }
    T1 = NowSeconds();
    BruteMs = (T1 - T0) * 1000.0 / (double)Frames;
    printf("  brute force:                 %8.3f ms/batch\n", BruteMs);

    for (C = 0; C < NumCellSizes; ++C)
    {
        float CellSize = CellSizes[C];
        HMM_SegmentGrid Grid;
        int *Buffer = 0;
        int Needed;
        double BuildMs, GridMs;

        T0 = NowSeconds();
        Needed = BuildGrid(&Grid, Segs, NumSegments, CellSize, &Buffer);
        T1 = NowSeconds();
        BuildMs = (T1 - T0) * 1000.0;
        Check(Needed > 0, "grid init (benchmark)");
        if (Needed <= 0)
        {
            continue;
        }

        T0 = NowSeconds();
        for (F = 0; F < Frames; ++F)
        {
            HMM_SegmentGridQueryBatch(&Grid, Queries, Results, BatchSize);
            Sink += Results[0].Distance;
        }
        T1 = NowSeconds();
        GridMs = (T1 - T0) * 1000.0 / (double)Frames;

        printf("  cell=%5.2f: grid %8.3f ms/batch (build %6.2f ms, %d ints)  speedup %6.1fx\n",
               CellSize, GridMs, BuildMs, Needed, BruteMs / GridMs);

        free(Buffer);
    }

    printf("  (sink=%f)\n\n", (float)Sink);
    free(Results);
}

int main(int Argc, char **Argv)
{
    int NumSegments = 3000;
    int BatchSize = 256;
    int Frames = 200;
    int NumQueries;
    HMM_Segment2D *Segs;
    HMM_Vec2 *Queries;
    static const float CellSizes[] = { 0.5f, 1.0f, 2.0f, 4.0f, 8.0f };
    static const float BenchCellSizes[] = { 1.0f, 2.0f, 4.0f };

    if (Argc > 1) NumSegments = atoi(Argv[1]);
    if (Argc > 2) BatchSize = atoi(Argv[2]);
    if (Argc > 3) Frames = atoi(Argv[3]);
    /* Consistency check uses at least 2000 queries; the benchmark uses
       the first BatchSize of them. */
    NumQueries = BatchSize > 2000 ? BatchSize : 2000;

    TestGeometry();

    Segs = MakeScene(NumSegments);
    Queries = MakeQueries(NumQueries);

    TestConsistency(Segs, NumSegments, Queries, NumQueries,
                    CellSizes, (int)(sizeof(CellSizes) / sizeof(CellSizes[0])));

    Benchmark(Segs, NumSegments, Queries, BatchSize, Frames,
              BenchCellSizes, (int)(sizeof(BenchCellSizes) / sizeof(BenchCellSizes[0])));

    free(Segs);
    free(Queries);

    if (Failures == 0)
    {
        printf("ALL CHECKS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", Failures);
    return 1;
}
