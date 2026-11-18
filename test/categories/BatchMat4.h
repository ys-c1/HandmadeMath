#include "../HandmadeTest.h"
#include <string.h>

/* Bit-level float equality: the batch function must match HMM_MulM4 not
 * just numerically, but bit-for-bit (same float operation order). */
static int BatchTest_BitsEqual(float A, float B)
{
    union { float f; unsigned int u; } ua, ub;
    ua.f = A;
    ub.f = B;
    return ua.u == ub.u;
}

static void BatchTest_Fill(HMM_Mat4 *M, int Count, float Seed)
{
    int i, Column, Row;
    float v = Seed;
    for (i = 0; i < Count; ++i)
    {
        for (Column = 0; Column < 4; ++Column)
        {
            for (Row = 0; Row < 4; ++Row)
            {
                /* Fractional, distinct values so row/column swaps and
                 * rounding-order changes cannot hide. */
                M[i].Elements[Column][Row] = v;
                v = v * 1.000001f + 0.25f;
            }
        }
    }
}

TEST(Multiplication, MulM4BatchMatchesMulM4)
{
    /* Counts chosen to exercise full, partial and empty tiles, plus
     * the odd-element interleave remainder. */
    const int Counts[] = {1, 2, 3, 7, 63, 64, 65, 127, 128, 129};
    const int MaxCount = 129;
    HMM_Mat4 Left[129];
    HMM_Mat4 Right[129];
    HMM_Mat4 BatchOut[129];
    HMM_Mat4 RefOut[129];
    int t, i, Column, Row;

    BatchTest_Fill(Left, MaxCount, 0.5f);
    BatchTest_Fill(Right, MaxCount, -0.375f);

    for (t = 0; t < (int)(sizeof(Counts) / sizeof(Counts[0])); ++t)
    {
        int Count = Counts[t];

        for (i = 0; i < Count; ++i)
        {
            RefOut[i] = HMM_MulM4(Left[i], Right[i]);
        }
        HMM_MulM4Batch(Left, Right, BatchOut, Count);

        for (i = 0; i < Count; ++i)
        {
            for (Column = 0; Column < 4; ++Column)
            {
                for (Row = 0; Row < 4; ++Row)
                {
                    EXPECT_TRUE(BatchTest_BitsEqual(
                        BatchOut[i].Elements[Column][Row],
                        RefOut[i].Elements[Column][Row]));
                }
            }
        }
    }
}

TEST(Multiplication, MulM4BatchEdgeCases)
{
    HMM_Mat4 In[3];
    HMM_Mat4 Out[3];
    HMM_Mat4 Sentinel;
    int i;

    BatchTest_Fill(In, 3, 1.25f);
    Sentinel = HMM_M4D(7.0f);

    /* Zero and negative counts must do nothing and must not crash. */
    for (i = 0; i < 3; ++i)
    {
        Out[i] = Sentinel;
    }
    HMM_MulM4Batch(In, In, Out, 0);
    HMM_MulM4Batch(In, In, Out, -4);
    for (i = 0; i < 3; ++i)
    {
        EXPECT_M4_EQ(Out[i], Sentinel);
    }

    /* NULL pointers must be ignored rather than dereferenced. */
    HMM_MulM4Batch(0, In, Out, 3);
    HMM_MulM4Batch(In, 0, Out, 3);
    HMM_MulM4Batch(In, In, 0, 3);
}

#ifdef HANDMADE_MATH__USE_SSE
TEST(Multiplication, MulM4BatchUnaligned)
{
    enum { Count = 65 };
    /* Typed HMM_Mat4 storage (not a char array cast to HMM_Mat4*) so the
     * misaligned accesses stay through a compatible effective type. The
     * extra matrix leaves room for the one-float offset below. */
    HMM_Mat4 LeftStore[Count + 1];
    HMM_Mat4 RightStore[Count + 1];
    HMM_Mat4 AlignedLeft[Count];
    HMM_Mat4 AlignedRight[Count];
    HMM_Mat4 Out[Count];
    HMM_Mat4 RefOut[Count];
    HMM_Mat4 *Left;
    HMM_Mat4 *Right;
    int i, Column, Row;

    BatchTest_Fill(AlignedLeft, Count, -1.5f);
    BatchTest_Fill(AlignedRight, Count, 0.625f);

    /* Offset by one float: 4-byte aligned, 16-byte misaligned. This
     * forces the unaligned load/store path inside the SSE kernel. */
    Left = (HMM_Mat4 *)((char *)LeftStore + sizeof(float));
    Right = (HMM_Mat4 *)((char *)RightStore + sizeof(float));
    for (i = 0; i < Count; ++i)
    {
        /* memcpy, not struct assignment: the destination is deliberately
         * misaligned and an HMM_Mat4 copy could be lowered to movaps. */
        memcpy(&Left[i], &AlignedLeft[i], sizeof(HMM_Mat4));
        memcpy(&Right[i], &AlignedRight[i], sizeof(HMM_Mat4));
        RefOut[i] = HMM_MulM4(AlignedLeft[i], AlignedRight[i]);
    }

    HMM_MulM4Batch(Left, Right, Out, Count);

    for (i = 0; i < Count; ++i)
    {
        for (Column = 0; Column < 4; ++Column)
        {
            for (Row = 0; Row < 4; ++Row)
            {
                EXPECT_TRUE(BatchTest_BitsEqual(
                    Out[i].Elements[Column][Row],
                    RefOut[i].Elements[Column][Row]));
            }
        }
    }
}

TEST(Multiplication, MulM4BatchSSEKernel)
{
    HMM_Mat4 Left[2];
    HMM_Mat4 Right[2];
    HMM_Mat4 AlignedOut[2];
    enum { Count = 2 };
    /* Typed storage with one extra matrix of slack for the 4-byte offset. */
    HMM_Mat4 OutStore[Count + 1];
    HMM_Mat4 *UnalignedOut =
        (HMM_Mat4 *)((char *)OutStore + sizeof(float));
    int i;

    BatchTest_Fill(Left, Count, 2.5f);
    BatchTest_Fill(Right, Count, -0.125f);

    HMM_MulM4Batch__SSEKernel(&Left[0], &Right[0], &AlignedOut[0], 1);
    HMM_MulM4Batch__SSEKernel(&Left[1], &Right[1], &AlignedOut[1], 1);
    /* The microkernel computes one product per call, so exercise the
     * unaligned store path once per element. */
    HMM_MulM4Batch__SSEKernel(&Left[0], &Right[0], &UnalignedOut[0], 0);
    HMM_MulM4Batch__SSEKernel(&Left[1], &Right[1], &UnalignedOut[1], 0);

    for (i = 0; i < Count; ++i)
    {
        HMM_Mat4 Ref = HMM_MulM4(Left[i], Right[i]);
        int Column, Row;
        for (Column = 0; Column < 4; ++Column)
        {
            for (Row = 0; Row < 4; ++Row)
            {
                EXPECT_TRUE(BatchTest_BitsEqual(
                    AlignedOut[i].Elements[Column][Row],
                    Ref.Elements[Column][Row]));
                EXPECT_TRUE(BatchTest_BitsEqual(
                    UnalignedOut[i].Elements[Column][Row],
                    Ref.Elements[Column][Row]));
            }
        }
    }
}
#else
TEST(Multiplication, MulM4BatchScalarKernel)
{
    HMM_Mat4 Left[2];
    HMM_Mat4 Right[2];
    HMM_Mat4 Out[2];
    int i;

    BatchTest_Fill(Left, 2, 2.5f);
    BatchTest_Fill(Right, 2, -0.125f);

    HMM_MulM4Batch__ScalarKernel(&Left[0], &Right[0], &Out[0]);
    HMM_MulM4Batch__ScalarKernel(&Left[1], &Right[1], &Out[1]);

    for (i = 0; i < 2; ++i)
    {
        HMM_Mat4 Ref = HMM_MulM4(Left[i], Right[i]);
        int Column, Row;
        for (Column = 0; Column < 4; ++Column)
        {
            for (Row = 0; Row < 4; ++Row)
            {
                EXPECT_TRUE(BatchTest_BitsEqual(
                    Out[i].Elements[Column][Row],
                    Ref.Elements[Column][Row]));
            }
        }
    }
}
#endif
