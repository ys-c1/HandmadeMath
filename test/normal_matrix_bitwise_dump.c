/* Dumps raw bytes of results from the pre-existing HandmadeMath API so the
   original and modified headers can be compared bit-for-bit. */
#include <stdio.h>
#include <string.h>

#include "HandmadeMath.h"

static void dump(const char *Tag, const void *Data, size_t Size)
{
    size_t Index;
    const unsigned char *Bytes = (const unsigned char *)Data;

    printf("%s", Tag);
    for (Index = 0; Index < Size; ++Index)
    {
        printf("%02x", (unsigned)Bytes[Index]);
    }
    printf("\n");
}

static HMM_Mat3 extract3(HMM_Mat4 Matrix)
{
    HMM_Mat3 Result = {0};
    int Column, Row;
    for (Column = 0; Column < 3; ++Column)
    {
        for (Row = 0; Row < 3; ++Row)
        {
            Result.Elements[Column][Row] = Matrix.Elements[Column][Row];
        }
    }
    return Result;
}

int main(void)
{
    int Index;

    for (Index = 0; Index < 8; ++Index)
    {
        char Tag[64];
        float Alpha = 0.13f * (float)Index;
        HMM_Mat4 Rotation = HMM_Rotate_RH(Alpha, HMM_V3(0.3f, 1.0f, -0.7f));
        HMM_Mat4 Scale = HMM_Scale(HMM_V3(1.0f + 0.3f * (float)Index,
                                         (Index == 3) ? 0.0f : 0.5f + 0.1f * (float)Index,
                                         2.0f - 0.1f * (float)Index));
        HMM_Mat4 Transform = HMM_MulM4(Rotation, Scale);
        HMM_Vec3 Normal = HMM_NormV3(HMM_V3((float)Index - 2.0f, 1.0f, 0.5f));
        HMM_Mat3 Linear = extract3(Transform);
        HMM_Mat3 Inverse = HMM_InvGeneralM3(Linear);
        HMM_Mat3 Transposed = HMM_TransposeM3(Inverse);
        HMM_Mat3 Product = HMM_MulM3(Linear, Inverse);
        HMM_Mat4 GeneralInverse = HMM_InvGeneralM4(Transform);
        HMM_Vec3 MappedNormal = HMM_MulM3V3(Transposed, Normal);
        HMM_Vec3 MappedModel = HMM_MulM3V3(Linear, Normal);
        float Determinant = HMM_DeterminantM3(Linear);
        HMM_Mat4 Quat = HMM_QToM4(HMM_QFromAxisAngle_RH(HMM_V3(1.0f, 2.0f, 3.0f), Alpha));
        HMM_Vec3 Rotated = HMM_RotateV3AxisAngle_RH(Normal, HMM_V3(1.0f, 2.0f, 3.0f), Alpha);

        snprintf(Tag, sizeof(Tag), "inverse%d ", Index);
        dump(Tag, &Inverse, sizeof(Inverse));
        snprintf(Tag, sizeof(Tag), "transpose%d ", Index);
        dump(Tag, &Transposed, sizeof(Transposed));
        snprintf(Tag, sizeof(Tag), "product%d ", Index);
        dump(Tag, &Product, sizeof(Product));
        snprintf(Tag, sizeof(Tag), "geninv%d ", Index);
        dump(Tag, &GeneralInverse, sizeof(GeneralInverse));
        snprintf(Tag, sizeof(Tag), "mappedn%d ", Index);
        dump(Tag, &MappedNormal, sizeof(MappedNormal));
        snprintf(Tag, sizeof(Tag), "mappedm%d ", Index);
        dump(Tag, &MappedModel, sizeof(MappedModel));
        snprintf(Tag, sizeof(Tag), "det%d ", Index);
        dump(Tag, &Determinant, sizeof(Determinant));
        snprintf(Tag, sizeof(Tag), "quat%d ", Index);
        dump(Tag, &Quat, sizeof(Quat));
        snprintf(Tag, sizeof(Tag), "rotated%d ", Index);
        dump(Tag, &Rotated, sizeof(Rotated));
    }

    return 0;
}
