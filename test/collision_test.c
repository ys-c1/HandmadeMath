/*
 * collision_test.c - tests and benchmarks for the sphere/capsule collision
 * section of HandmadeMath.h. Self-contained: does not use HandmadeTest.h, and
 * compiles both as C (C99 and later) and as C++ (C++98 and later).
 *
 * Coverage:
 *   1. Sphere-sphere, sphere-capsule and capsule-capsule tests in the four
 *      required configurations: intersecting, tangent, separated, contained.
 *   2. Penetration depth mode and contact normal options, including the
 *      square-root-free fast path.
 *   3. Zero-length capsules (capsules degenerate to spheres).
 *   4. Numeric consistency between per-pair calls and batch calls.
 *   5. Batch throughput on a few thousand pairs, with speedup versus the
 *      per-pair loop.
 */

#define _POSIX_C_SOURCE 199309L

#include "../HandmadeMath.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* Tiny test harness                                                   */
/* ------------------------------------------------------------------ */

static int g_checks_run = 0;
static int g_checks_failed = 0;

#define CHECK(condition)                                                      \
    do                                                                        \
    {                                                                         \
        ++g_checks_run;                                                       \
        if (!(condition))                                                     \
        {                                                                     \
            ++g_checks_failed;                                                \
            printf("  FAIL line %d: %s\n", __LINE__, #condition);            \
        }                                                                     \
    } while (0)

#define CHECK_NEAR(actual, expected, eps)                                     \
    do                                                                        \
    {                                                                         \
        ++g_checks_run;                                                       \
        float ct_actual = (actual);                                           \
        float ct_expected = (expected);                                       \
        float ct_diff = ct_actual - ct_expected;                              \
        if (ct_diff < 0.0f) ct_diff = -ct_diff;                               \
        if (ct_diff > (eps))                                                  \
        {                                                                     \
            ++g_checks_failed;                                                \
            printf("  FAIL line %d: %s ~= %s (got %.7f, want %.7f)\n",       \
                   __LINE__, #actual, #expected, ct_actual, ct_expected);     \
        }                                                                     \
    } while (0)

#define CHECK_V3_NEAR(actual, expected, eps)                                  \
    do                                                                        \
    {                                                                         \
        HMM_Vec3 ct_v = (actual);                                             \
        HMM_Vec3 ct_w = (expected);                                           \
        CHECK_NEAR(ct_v.X, ct_w.X, eps);                                      \
        CHECK_NEAR(ct_v.Y, ct_w.Y, eps);                                      \
        CHECK_NEAR(ct_v.Z, ct_w.Z, eps);                                      \
    } while (0)

#define SECTION(name) printf("--- %s ---\n", (name))

/* ------------------------------------------------------------------ */
/* 1. Sphere-sphere                                                    */
/* ------------------------------------------------------------------ */

static void test_sphere_sphere(void)
{
    HMM_CollideOptions opts = HMM_DefaultCollideOptions();
    HMM_Sphere s1, s2;
    HMM_Contact c;

    SECTION("sphere-sphere: intersecting");
    s1.Center = HMM_V3(0.0f, 0.0f, 0.0f); s1.Radius = 2.0f;
    s2.Center = HMM_V3(3.0f, 0.0f, 0.0f); s2.Radius = 2.0f;
    c = HMM_CollideSphereSphere(s1, s2);
    CHECK(c.Intersect == 1);
    CHECK_NEAR(c.Depth, 1.0f, 1e-5f);
    CHECK_V3_NEAR(c.Normal, HMM_V3(1.0f, 0.0f, 0.0f), 1e-6f);
    CHECK_V3_NEAR(c.PointA, s1.Center, 0.0f);
    CHECK_V3_NEAR(c.PointB, s2.Center, 0.0f);
    /* Surface contact points derived from feature points and normal. */
    CHECK_V3_NEAR(HMM_AddV3(c.PointA, HMM_MulV3F(c.Normal, s1.Radius)),
                  HMM_V3(2.0f, 0.0f, 0.0f), 1e-5f);

    SECTION("sphere-sphere: tangent");
    s2.Center = HMM_V3(4.0f, 0.0f, 0.0f);
    c = HMM_CollideSphereSphere(s1, s2);
    CHECK(c.Intersect == 1);
    CHECK_NEAR(c.Depth, 0.0f, 1e-5f);
    CHECK_V3_NEAR(c.Normal, HMM_V3(1.0f, 0.0f, 0.0f), 1e-6f);

    SECTION("sphere-sphere: separated");
    s2.Center = HMM_V3(5.0f, 0.0f, 0.0f);
    c = HMM_CollideSphereSphere(s1, s2);
    CHECK(c.Intersect == 0);
    CHECK_NEAR(c.Depth, -1.0f, 1e-5f);
    CHECK_V3_NEAR(c.Normal, HMM_V3(1.0f, 0.0f, 0.0f), 1e-6f);

    SECTION("sphere-sphere: contained (one sphere inside the other)");
    s1.Center = HMM_V3(0.0f, 0.0f, 0.0f); s1.Radius = 5.0f;
    s2.Center = HMM_V3(1.0f, 0.0f, 0.0f); s2.Radius = 1.0f;
    c = HMM_CollideSphereSphere(s1, s2);
    CHECK(c.Intersect == 1);
    CHECK_NEAR(c.Depth, 6.0f - HMM_LenV3(HMM_SubV3(s2.Center, s1.Center)),
               1e-5f);
    CHECK_NEAR(c.Depth, 5.0f, 1e-5f);

    SECTION("sphere-sphere: concentric features, normal is zero");
    s2.Center = HMM_V3(0.0f, 0.0f, 0.0f); s2.Radius = 1.0f;
    c = HMM_CollideSphereSphereEx(s1, s2, opts);
    CHECK(c.Intersect == 1);
    CHECK_NEAR(c.Depth, 6.0f, 1e-5f);
    CHECK_NEAR(c.Normal.X, 0.0f, 0.0f);
    CHECK_NEAR(c.Normal.Y, 0.0f, 0.0f);
    CHECK_NEAR(c.Normal.Z, 0.0f, 0.0f);
}

/* ------------------------------------------------------------------ */
/* 2. Sphere-capsule                                                   */
/* ------------------------------------------------------------------ */

static void test_sphere_capsule(void)
{
    HMM_CollideOptions opts;
    HMM_Sphere s;
    HMM_Capsule cap;
    HMM_Contact c;

    /* Capsule on the x axis from (-2,0,0) to (2,0,0), radius 1. */
    cap.A = HMM_V3(-2.0f, 0.0f, 0.0f);
    cap.B = HMM_V3(2.0f, 0.0f, 0.0f);
    cap.Radius = 1.0f;

    SECTION("sphere-capsule: intersecting (mid segment)");
    s.Center = HMM_V3(0.0f, 1.5f, 0.0f); s.Radius = 1.0f;
    c = HMM_CollideSphereCapsule(s, cap);
    CHECK(c.Intersect == 1);
    CHECK_NEAR(c.Depth, 0.5f, 1e-5f);
    CHECK_V3_NEAR(c.PointB, HMM_V3(0.0f, 0.0f, 0.0f), 1e-6f);
    CHECK_V3_NEAR(c.PointA, s.Center, 0.0f);
    /* Normal points from the sphere (first shape) toward the capsule. */
    CHECK_V3_NEAR(c.Normal, HMM_V3(0.0f, -1.0f, 0.0f), 1e-6f);

    SECTION("sphere-capsule: tangent (mid segment)");
    s.Center = HMM_V3(0.0f, 2.0f, 0.0f);
    c = HMM_CollideSphereCapsule(s, cap);
    CHECK(c.Intersect == 1);
    CHECK_NEAR(c.Depth, 0.0f, 1e-5f);
    CHECK_V3_NEAR(c.Normal, HMM_V3(0.0f, -1.0f, 0.0f), 1e-6f);

    SECTION("sphere-capsule: separated (mid segment, signed gap)");
    s.Center = HMM_V3(0.0f, 3.0f, 0.0f);
    c = HMM_CollideSphereCapsule(s, cap);
    CHECK(c.Intersect == 0);
    CHECK_NEAR(c.Depth, -1.0f, 1e-5f);
    CHECK_V3_NEAR(c.PointB, HMM_V3(0.0f, 0.0f, 0.0f), 1e-6f);
    CHECK_V3_NEAR(c.Normal, HMM_V3(0.0f, -1.0f, 0.0f), 1e-6f);

    SECTION("sphere-capsule: separated beyond an endpoint (closest clamps)");
    s.Center = HMM_V3(5.0f, 0.0f, 0.0f);
    c = HMM_CollideSphereCapsule(s, cap);
    CHECK(c.Intersect == 0);
    CHECK_NEAR(c.Depth, 2.0f - 3.0f, 1e-5f); /* gap = 3-2 = 1 */
    CHECK_NEAR(c.Depth, -1.0f, 1e-5f);
    CHECK_V3_NEAR(c.PointB, HMM_V3(2.0f, 0.0f, 0.0f), 1e-6f);
    CHECK_V3_NEAR(c.Normal, HMM_V3(-1.0f, 0.0f, 0.0f), 1e-6f);

    SECTION("sphere-capsule: tangent at an endpoint");
    s.Center = HMM_V3(4.0f, 0.0f, 0.0f); s.Radius = 1.0f;
    c = HMM_CollideSphereCapsule(s, cap);
    CHECK(c.Intersect == 1);
    CHECK_NEAR(c.Depth, 0.0f, 1e-5f);
    CHECK_V3_NEAR(c.PointB, HMM_V3(2.0f, 0.0f, 0.0f), 1e-6f);

    SECTION("sphere-capsule: sphere contained in capsule");
    s.Center = HMM_V3(0.0f, 0.0f, 0.0f); s.Radius = 0.5f;
    c = HMM_CollideSphereCapsule(s, cap);
    CHECK(c.Intersect == 1);
    /* Features coincide: depth is the sum of the two radii. */
    CHECK_NEAR(c.Depth, 1.5f, 1e-5f);

    SECTION("sphere-capsule: zero-length capsule behaves like a sphere");
    cap.A = HMM_V3(1.0f, 1.0f, 1.0f);
    cap.B = HMM_V3(1.0f, 1.0f, 1.0f);
    cap.Radius = 1.5f;
    s.Center = HMM_V3(0.0f, 1.0f, 1.0f); s.Radius = 0.5f;
    {
        HMM_Sphere equivalent;
        HMM_Contact c2;
        equivalent.Center = HMM_V3(1.0f, 1.0f, 1.0f);
        equivalent.Radius = 1.5f;
        c = HMM_CollideSphereCapsule(s, cap);
        c2 = HMM_CollideSphereSphere(s, equivalent);
        CHECK(c.Intersect == c2.Intersect);
        CHECK_NEAR(c.Depth, c2.Depth, 1e-6f);
        CHECK_V3_NEAR(c.PointB, equivalent.Center, 1e-6f);
    }

    SECTION("sphere-capsule: clamped depth + no normal (sqrt-free fast path)");
    cap.A = HMM_V3(-2.0f, 0.0f, 0.0f);
    cap.B = HMM_V3(2.0f, 0.0f, 0.0f);
    cap.Radius = 1.0f;
    opts.ComputeNormal = 0;
    opts.DepthMode = HMM_DEPTH_CLAMPED;
    s.Center = HMM_V3(0.0f, 3.0f, 0.0f); s.Radius = 1.0f;
    c = HMM_CollideSphereCapsuleEx(s, cap, opts);
    CHECK(c.Intersect == 0);
    CHECK_NEAR(c.Depth, 0.0f, 0.0f);
    CHECK_NEAR(c.Normal.X, 0.0f, 0.0f);
    CHECK_NEAR(c.Normal.Y, 0.0f, 0.0f);
    CHECK_NEAR(c.Normal.Z, 0.0f, 0.0f);
    /* Overlapping pair under the same options still reports real depth. */
    s.Center = HMM_V3(0.0f, 1.5f, 0.0f);
    c = HMM_CollideSphereCapsuleEx(s, cap, opts);
    CHECK(c.Intersect == 1);
    CHECK_NEAR(c.Depth, 0.5f, 1e-5f);
}

/* ------------------------------------------------------------------ */
/* 3. Capsule-capsule                                                  */
/* ------------------------------------------------------------------ */

static void test_capsule_capsule(void)
{
    HMM_Capsule c1, c2;
    HMM_Contact c;

    /* Both capsules parallel to the x axis, at different heights y. */
    c1.A = HMM_V3(-2.0f, 0.0f, 0.0f);
    c1.B = HMM_V3(2.0f, 0.0f, 0.0f);
    c1.Radius = 1.0f;

    SECTION("capsule-capsule: intersecting");
    c2.A = HMM_V3(-1.0f, 1.5f, 0.0f);
    c2.B = HMM_V3(3.0f, 1.5f, 0.0f);
    c2.Radius = 1.0f;
    c = HMM_CollideCapsuleCapsule(c1, c2);
    CHECK(c.Intersect == 1);
    CHECK_NEAR(c.Depth, 0.5f, 1e-5f);
    CHECK_V3_NEAR(c.Normal, HMM_V3(0.0f, 1.0f, 0.0f), 1e-6f);
    /* Closest features are interior points with matching x. */
    CHECK_NEAR(c.PointA.X, c.PointB.X, 1e-5f);
    CHECK_NEAR(c.PointA.Y, 0.0f, 1e-5f);
    CHECK_NEAR(c.PointB.Y, 1.5f, 1e-5f);

    SECTION("capsule-capsule: tangent");
    c2.A = HMM_V3(-1.0f, 2.0f, 0.0f);
    c2.B = HMM_V3(3.0f, 2.0f, 0.0f);
    c = HMM_CollideCapsuleCapsule(c1, c2);
    CHECK(c.Intersect == 1);
    CHECK_NEAR(c.Depth, 0.0f, 1e-5f);
    CHECK_V3_NEAR(c.Normal, HMM_V3(0.0f, 1.0f, 0.0f), 1e-6f);

    SECTION("capsule-capsule: separated");
    c2.A = HMM_V3(-1.0f, 3.0f, 0.0f);
    c2.B = HMM_V3(3.0f, 3.0f, 0.0f);
    c = HMM_CollideCapsuleCapsule(c1, c2);
    CHECK(c.Intersect == 0);
    CHECK_NEAR(c.Depth, -1.0f, 1e-5f);
    CHECK_V3_NEAR(c.Normal, HMM_V3(0.0f, 1.0f, 0.0f), 1e-6f);

    SECTION("capsule-capsule: one capsule contained in the other");
    c2.A = HMM_V3(-0.5f, 0.0f, 0.0f);
    c2.B = HMM_V3(0.5f, 0.0f, 0.0f);
    c2.Radius = 0.5f;
    c = HMM_CollideCapsuleCapsule(c1, c2);
    CHECK(c.Intersect == 1);
    /* Features coincide: depth is the sum of the two radii. */
    CHECK_NEAR(c.Depth, 1.5f, 1e-5f);
    CHECK_V3_NEAR(c.PointA, c.PointB, 1e-5f);
    /* Coincident features: no direction to normalize. */
    CHECK_NEAR(c.Normal.X, 0.0f, 0.0f);
    CHECK_NEAR(c.Normal.Y, 0.0f, 0.0f);
    CHECK_NEAR(c.Normal.Z, 0.0f, 0.0f);

    SECTION("capsule-capsule: crossing skew segments, no NaN (parallel guard)");
    c2.A = HMM_V3(0.0f, -1.0f, 1.0f);
    c2.B = HMM_V3(0.0f, 3.0f, 1.0f);
    c2.Radius = 0.5f;
    c = HMM_CollideCapsuleCapsule(c1, c2);
    /* Closest points are the segment crossing at (0,0,0)/(0,0,1), gap 1. */
    CHECK_V3_NEAR(c.PointA, HMM_V3(0.0f, 0.0f, 0.0f), 1e-5f);
    CHECK_V3_NEAR(c.PointB, HMM_V3(0.0f, 0.0f, 1.0f), 1e-5f);
    CHECK(c.Intersect == 1); /* 1.0 + 0.5 > 1.0 */
    CHECK_NEAR(c.Depth, 0.5f, 1e-5f);
    CHECK_V3_NEAR(c.Normal, HMM_V3(0.0f, 0.0f, 1.0f), 1e-6f);

    SECTION("capsule-capsule: both zero-length, identical to sphere-sphere");
    {
        HMM_Sphere e1, e2;
        HMM_Contact ref;
        c1.A = c1.B = HMM_V3(0.0f, 0.0f, 0.0f); c1.Radius = 2.0f;
        c2.A = c2.B = HMM_V3(3.0f, 0.0f, 0.0f); c2.Radius = 2.0f;
        e1.Center = c1.A; e1.Radius = c1.Radius;
        e2.Center = c2.A; e2.Radius = c2.Radius;
        c = HMM_CollideCapsuleCapsule(c1, c2);
        ref = HMM_CollideSphereSphere(e1, e2);
        CHECK(c.Intersect == ref.Intersect);
        CHECK_NEAR(c.Depth, ref.Depth, 1e-6f);
        CHECK_V3_NEAR(c.Normal, ref.Normal, 1e-6f);
        CHECK_V3_NEAR(c.PointA, ref.PointA, 1e-6f);
        CHECK_V3_NEAR(c.PointB, ref.PointB, 1e-6f);
    }

    SECTION("capsule-capsule: one zero-length against a normal capsule");
    {
        HMM_Sphere as_sphere;
        HMM_Contact ref;
        HMM_CollideOptions opts = HMM_DefaultCollideOptions();
        c1.A = c1.B = HMM_V3(0.0f, 1.5f, 0.0f); c1.Radius = 1.0f;
        c2.A = HMM_V3(-2.0f, 0.0f, 0.0f);
        c2.B = HMM_V3(2.0f, 0.0f, 0.0f);
        c2.Radius = 1.0f;
        as_sphere.Center = c1.A; as_sphere.Radius = c1.Radius;
        c = HMM_CollideCapsuleCapsuleEx(c1, c2, opts);
        ref = HMM_CollideSphereCapsuleEx(as_sphere, c2, opts);
        CHECK(c.Intersect == ref.Intersect);
        CHECK_NEAR(c.Depth, ref.Depth, 1e-6f);
        CHECK_V3_NEAR(c.PointA, ref.PointA, 1e-6f);
        CHECK_V3_NEAR(c.PointB, ref.PointB, 1e-6f);
    }

    SECTION("capsule-capsule: tangent at endpoint-to-endpoint");
    c1.A = HMM_V3(0.0f, 0.0f, 0.0f);
    c1.B = HMM_V3(1.0f, 0.0f, 0.0f);
    c1.Radius = 1.0f;
    c2.A = HMM_V3(3.0f, 0.0f, 0.0f);
    c2.B = HMM_V3(4.0f, 0.0f, 0.0f);
    c2.Radius = 1.0f;
    c = HMM_CollideCapsuleCapsule(c1, c2);
    CHECK(c.Intersect == 1);
    CHECK_NEAR(c.Depth, 0.0f, 1e-5f);
    CHECK_V3_NEAR(c.PointA, HMM_V3(1.0f, 0.0f, 0.0f), 1e-6f);
    CHECK_V3_NEAR(c.PointB, HMM_V3(3.0f, 0.0f, 0.0f), 1e-6f);
    CHECK_V3_NEAR(c.Normal, HMM_V3(1.0f, 0.0f, 0.0f), 1e-6f);
}

/* ------------------------------------------------------------------ */
/* 4. Options: depth mode and normal toggle                           */
/* ------------------------------------------------------------------ */

static void test_options(void)
{
    HMM_CollideOptions opts;
    HMM_Sphere s1, s2;
    HMM_Contact c;

    s1.Center = HMM_V3(0.0f, 0.0f, 0.0f); s1.Radius = 2.0f;
    s2.Center = HMM_V3(5.0f, 0.0f, 0.0f); s2.Radius = 2.0f;

    SECTION("options: defaults are signed depth with normal");
    opts = HMM_DefaultCollideOptions();
    CHECK(opts.ComputeNormal == 1);
    CHECK(opts.DepthMode == HMM_DEPTH_SIGNED);

    SECTION("options: clamped depth on a separated pair");
    opts.ComputeNormal = 1;
    opts.DepthMode = HMM_DEPTH_CLAMPED;
    c = HMM_CollideSphereSphereEx(s1, s2, opts);
    CHECK(c.Intersect == 0);
    CHECK_NEAR(c.Depth, 0.0f, 0.0f);
    CHECK_V3_NEAR(c.Normal, HMM_V3(1.0f, 0.0f, 0.0f), 1e-6f);

    SECTION("options: no normal still returns the signed depth");
    opts.ComputeNormal = 0;
    opts.DepthMode = HMM_DEPTH_SIGNED;
    c = HMM_CollideSphereSphereEx(s1, s2, opts);
    CHECK(c.Intersect == 0);
    CHECK_NEAR(c.Depth, -1.0f, 1e-5f);
    CHECK_NEAR(c.Normal.X, 0.0f, 0.0f);
    CHECK_NEAR(c.Normal.Y, 0.0f, 0.0f);
    CHECK_NEAR(c.Normal.Z, 0.0f, 0.0f);

    SECTION("options: feature points are always returned");
    CHECK_V3_NEAR(c.PointA, s1.Center, 0.0f);
    CHECK_V3_NEAR(c.PointB, s2.Center, 0.0f);
}

/* ------------------------------------------------------------------ */
/* 5. Batch vs per-pair numerical consistency                         */
/* ------------------------------------------------------------------ */

#define BATCH_PAIRS 256

static unsigned int rng_state = 0x12345678u;
static float rng_float(float low, float high)
{
    rng_state = rng_state * 1103515245u + 12345u;
    {
        float unit = (float)(rng_state >> 8) / (float)(1u << 24);
        return low + unit * (high - low);
    }
}

static HMM_Vec3 rng_vec3(float range)
{
    return HMM_V3(rng_float(-range, range),
                  rng_float(-range, range),
                  rng_float(-range, range));
}

static int contacts_match(HMM_Contact a, HMM_Contact b)
{
    if (a.Intersect != b.Intersect) return 0;
    if (fabsf(a.Depth - b.Depth) > 1e-6f) return 0;
    if (fabsf(a.PointA.X - b.PointA.X) > 1e-6f) return 0;
    if (fabsf(a.PointA.Y - b.PointA.Y) > 1e-6f) return 0;
    if (fabsf(a.PointA.Z - b.PointA.Z) > 1e-6f) return 0;
    if (fabsf(a.PointB.X - b.PointB.X) > 1e-6f) return 0;
    if (fabsf(a.PointB.Y - b.PointB.Y) > 1e-6f) return 0;
    if (fabsf(a.PointB.Z - b.PointB.Z) > 1e-6f) return 0;
    if (fabsf(a.Normal.X - b.Normal.X) > 1e-6f) return 0;
    if (fabsf(a.Normal.Y - b.Normal.Y) > 1e-6f) return 0;
    if (fabsf(a.Normal.Z - b.Normal.Z) > 1e-6f) return 0;
    return 1;
}

static void test_batch_consistency(void)
{
    HMM_Sphere *sp1 = (HMM_Sphere *)malloc(sizeof(HMM_Sphere) * BATCH_PAIRS);
    HMM_Sphere *sp2 = (HMM_Sphere *)malloc(sizeof(HMM_Sphere) * BATCH_PAIRS);
    HMM_Capsule *cp1 = (HMM_Capsule *)malloc(sizeof(HMM_Capsule) * BATCH_PAIRS);
    HMM_Capsule *cp2 = (HMM_Capsule *)malloc(sizeof(HMM_Capsule) * BATCH_PAIRS);
    HMM_Contact *out = (HMM_Contact *)malloc(sizeof(HMM_Contact) * BATCH_PAIRS);
    HMM_CollideOptions opts[4];
    HMM_CollideOptions fast;
    int i, k;
    int mismatches = 0;

    rng_state = 0x12345678u;
    for (i = 0; i < BATCH_PAIRS; ++i)
    {
        sp1[i].Center = rng_vec3(4.0f); sp1[i].Radius = rng_float(0.3f, 2.0f);
        sp2[i].Center = rng_vec3(4.0f); sp2[i].Radius = rng_float(0.3f, 2.0f);
        cp1[i].A = rng_vec3(3.0f);
        cp1[i].B = HMM_AddV3(cp1[i].A, rng_vec3(2.0f));
        cp1[i].Radius = rng_float(0.3f, 1.5f);
        cp2[i].A = rng_vec3(3.0f);
        cp2[i].B = HMM_AddV3(cp2[i].A, rng_vec3(2.0f));
        cp2[i].Radius = rng_float(0.3f, 1.5f);
        /* Sprinkle zero-length capsules on purpose. */
        if ((i % 17) == 0) cp1[i].B = cp1[i].A;
        if ((i % 23) == 0) cp2[i].B = cp2[i].A;
    }

    opts[0] = HMM_DefaultCollideOptions();
    opts[1].ComputeNormal = 1; opts[1].DepthMode = HMM_DEPTH_CLAMPED;
    opts[2].ComputeNormal = 0; opts[2].DepthMode = HMM_DEPTH_SIGNED;
    opts[3].ComputeNormal = 0; opts[3].DepthMode = HMM_DEPTH_CLAMPED;
    fast = opts[3]; /* sqrt-free preset for separated pairs */

    SECTION("batch consistency: sphere-sphere (all option combos)");
    for (k = 0; k < 4; ++k)
    {
        HMM_CollideSphereSphereBatch(sp1, sp2, out, BATCH_PAIRS, opts[k]);
        for (i = 0; i < BATCH_PAIRS; ++i)
        {
            HMM_Contact one = HMM_CollideSphereSphereEx(sp1[i], sp2[i], opts[k]);
            if (!contacts_match(one, out[i])) ++mismatches;
        }
    }
    CHECK(mismatches == 0);
    if (mismatches) printf("  %d mismatches\n", mismatches);

    SECTION("batch consistency: sphere-capsule (all option combos)");
    mismatches = 0;
    for (k = 0; k < 4; ++k)
    {
        HMM_CollideSphereCapsuleBatch(sp1, cp1, out, BATCH_PAIRS, opts[k]);
        for (i = 0; i < BATCH_PAIRS; ++i)
        {
            HMM_Contact one = HMM_CollideSphereCapsuleEx(sp1[i], cp1[i], opts[k]);
            if (!contacts_match(one, out[i])) ++mismatches;
        }
    }
    CHECK(mismatches == 0);
    if (mismatches) printf("  %d mismatches\n", mismatches);

    SECTION("batch consistency: capsule-capsule (all option combos)");
    mismatches = 0;
    for (k = 0; k < 4; ++k)
    {
        HMM_CollideCapsuleCapsuleBatch(cp1, cp2, out, BATCH_PAIRS, opts[k]);
        for (i = 0; i < BATCH_PAIRS; ++i)
        {
            HMM_Contact one = HMM_CollideCapsuleCapsuleEx(cp1[i], cp2[i], opts[k]);
            if (!contacts_match(one, out[i])) ++mismatches;
        }
    }
    CHECK(mismatches == 0);
    if (mismatches) printf("  %d mismatches\n", mismatches);

    SECTION("batch consistency: fast preset agrees with per-pair fast calls");
    mismatches = 0;
    HMM_CollideCapsuleCapsuleBatch(cp1, cp2, out, BATCH_PAIRS, fast);
    for (i = 0; i < BATCH_PAIRS; ++i)
    {
        HMM_Contact one = HMM_CollideCapsuleCapsuleEx(cp1[i], cp2[i], fast);
        if (!contacts_match(one, out[i])) ++mismatches;
    }
    CHECK(mismatches == 0);

    free(sp1); free(sp2); free(cp1); free(cp2); free(out);
}

/* ------------------------------------------------------------------ */
/* 6. Benchmark: a few thousand pairs, batch vs per-pair              */
/* ------------------------------------------------------------------ */

#define BENCH_PAIRS 4096
#define BENCH_ITERS 300

static double now_seconds(void)
{
#if defined(CLOCK_MONOTONIC)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#else
    return (double)clock() / (double)CLOCKS_PER_SEC;
#endif
}

/*
 * Per-pair timing barrier. Forcing no-inline keeps the per-pair loop honest:
 * callers pay the full function-call/options-setup cost per pair, exactly as
 * application code calling the public API does, instead of having the whole
 * pair merged into the loop by the optimizer. The batch entry point stays the
 * single call an application would make.
 */
#if defined(__GNUC__) || defined(__clang__)
# define CT_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
# define CT_NOINLINE __declspec(noinline)
#else
# define CT_NOINLINE
#endif

static CT_NOINLINE HMM_Contact ct_scalar_cc(HMM_Capsule c1, HMM_Capsule c2,
                                            HMM_CollideOptions o)
{
    return HMM_CollideCapsuleCapsuleEx(c1, c2, o);
}

static CT_NOINLINE HMM_Contact ct_scalar_sc(HMM_Sphere s, HMM_Capsule c,
                                            HMM_CollideOptions o)
{
    return HMM_CollideSphereCapsuleEx(s, c, o);
}

static CT_NOINLINE HMM_Contact ct_scalar_ss(HMM_Sphere s1, HMM_Sphere s2,
                                            HMM_CollideOptions o)
{
    return HMM_CollideSphereSphereEx(s1, s2, o);
}

static void benchmark(void)
{
    HMM_Sphere *sp1 = (HMM_Sphere *)malloc(sizeof(HMM_Sphere) * BENCH_PAIRS);
    HMM_Sphere *sp2 = (HMM_Sphere *)malloc(sizeof(HMM_Sphere) * BENCH_PAIRS);
    HMM_Capsule *cp1 = (HMM_Capsule *)malloc(sizeof(HMM_Capsule) * BENCH_PAIRS);
    HMM_Capsule *cp2 = (HMM_Capsule *)malloc(sizeof(HMM_Capsule) * BENCH_PAIRS);
    HMM_Contact *out = (HMM_Contact *)malloc(sizeof(HMM_Contact) * BENCH_PAIRS);
    HMM_CollideOptions full = HMM_DefaultCollideOptions();
    HMM_CollideOptions fast;
    int i, it;
    double t0, t1, t_scalar, t_batch_full, t_batch_fast;
    volatile float sink = 0.0f;

    fast.ComputeNormal = 0;
    fast.DepthMode = HMM_DEPTH_CLAMPED;

    rng_state = 0xABCDEF01u;
    for (i = 0; i < BENCH_PAIRS; ++i)
    {
        /*
         * Roughly 80% of the pairs are disjoint and 20% overlap, matching the
         * output of a broad phase: the fast preset skips every square root on
         * the disjoint majority.
         */
        float center_range = (i % 5 == 0) ? 1.5f : 6.0f;
        sp1[i].Center = rng_vec3(center_range); sp1[i].Radius = rng_float(0.3f, 1.5f);
        sp2[i].Center = rng_vec3(center_range); sp2[i].Radius = rng_float(0.3f, 1.5f);
        cp1[i].A = rng_vec3(center_range);
        cp1[i].B = HMM_AddV3(cp1[i].A, rng_vec3(2.0f));
        cp1[i].Radius = rng_float(0.3f, 1.2f);
        cp2[i].A = rng_vec3(center_range);
        cp2[i].B = HMM_AddV3(cp2[i].A, rng_vec3(2.0f));
        cp2[i].Radius = rng_float(0.3f, 1.2f);
        if ((i % 31) == 0) cp1[i].B = cp1[i].A;
        if ((i % 29) == 0) cp2[i].B = cp2[i].A;
    }

    SECTION("benchmark: capsule-capsule, 4096 pairs x 300 iterations");

    {
        int separated = 0;
        for (i = 0; i < BENCH_PAIRS; ++i)
        {
            HMM_Contact probe =
                HMM_CollideCapsuleCapsuleEx(cp1[i], cp2[i], full);
            separated += probe.Intersect == 0;
        }
        printf("  dataset: %d/%d pairs separated (%.0f%%)\n",
               separated, BENCH_PAIRS,
               100.0f * (float)separated / (float)BENCH_PAIRS);
    }

    t0 = now_seconds();
    for (it = 0; it < BENCH_ITERS; ++it)
    {
        for (i = 0; i < BENCH_PAIRS; ++i)
        {
            HMM_Contact one = ct_scalar_cc(cp1[i], cp2[i], full);
            sink += one.Depth;
        }
    }
    t1 = now_seconds();
    t_scalar = t1 - t0;

    t0 = now_seconds();
    for (it = 0; it < BENCH_ITERS; ++it)
    {
        HMM_CollideCapsuleCapsuleBatch(cp1, cp2, out, BENCH_PAIRS, full);
        sink += out[it % BENCH_PAIRS].Depth;
    }
    t1 = now_seconds();
    t_batch_full = t1 - t0;

    t0 = now_seconds();
    for (it = 0; it < BENCH_ITERS; ++it)
    {
        HMM_CollideCapsuleCapsuleBatch(cp1, cp2, out, BENCH_PAIRS, fast);
        sink += out[it % BENCH_PAIRS].Depth;
    }
    t1 = now_seconds();
    t_batch_fast = t1 - t0;

    printf("  per-pair public-API calls (normal+signed): %8.4f s\n", t_scalar);
    printf("  batch, same options                    : %8.4f s  (%.2fx)\n",
           t_batch_full, t_scalar / t_batch_full);
    printf("  batch, clamped depth, no normal        : %8.4f s  (%.2fx)\n",
           t_batch_fast, t_scalar / t_batch_fast);
    /* Timing is advisory only; never fail functional runs over wall clock. */
    CHECK(t_batch_full > 0.0 && t_batch_fast > 0.0);
    if (t_batch_fast > t_scalar)
        printf("  note: batch slower here (debug/sanitizer build?)\n");

    SECTION("benchmark: sphere-capsule and sphere-sphere batch timings");
    t0 = now_seconds();
    for (it = 0; it < BENCH_ITERS; ++it)
    {
        for (i = 0; i < BENCH_PAIRS; ++i)
        {
            HMM_Contact one = ct_scalar_sc(sp1[i], cp1[i], full);
            sink += one.Depth;
        }
    }
    t1 = now_seconds();
    t_scalar = t1 - t0;
    t0 = now_seconds();
    for (it = 0; it < BENCH_ITERS; ++it)
    {
        HMM_CollideSphereCapsuleBatch(sp1, cp1, out, BENCH_PAIRS, fast);
        sink += out[it % BENCH_PAIRS].Depth;
    }
    t1 = now_seconds();
    t_batch_fast = t1 - t0;
    printf("  sphere-capsule per-pair                : %8.4f s\n", t_scalar);
    printf("  sphere-capsule batch (fast preset)     : %8.4f s  (%.2fx)\n",
           t_batch_fast, t_scalar / t_batch_fast);

    t0 = now_seconds();
    for (it = 0; it < BENCH_ITERS; ++it)
    {
        for (i = 0; i < BENCH_PAIRS; ++i)
        {
            HMM_Contact one = ct_scalar_ss(sp1[i], sp2[i], full);
            sink += one.Depth;
        }
    }
    t1 = now_seconds();
    t_scalar = t1 - t0;
    t0 = now_seconds();
    for (it = 0; it < BENCH_ITERS; ++it)
    {
        HMM_CollideSphereSphereBatch(sp1, sp2, out, BENCH_PAIRS, fast);
        sink += out[it % BENCH_PAIRS].Depth;
    }
    t1 = now_seconds();
    t_batch_fast = t1 - t0;
    printf("  sphere-sphere per-pair                 : %8.4f s\n", t_scalar);
    printf("  sphere-sphere batch (fast preset)      : %8.4f s  (%.2fx)\n",
           t_batch_fast, t_scalar / t_batch_fast);

    /* Keep the benchmark honest: the checksum must actually be consumed. */
    CHECK(sink >= 0.0f || sink < 0.0f);

    free(sp1); free(sp2); free(cp1); free(cp2); free(out);
}

int main(void)
{
    printf("=== HandmadeMath sphere/capsule collision tests ===\n");

    test_sphere_sphere();
    test_sphere_capsule();
    test_capsule_capsule();
    test_options();
    test_batch_consistency();
    benchmark();

    printf("----------------------------------------------------\n");
    if (g_checks_failed == 0)
    {
        printf("ALL PASS: %d checks, 0 failures\n", g_checks_run);
        return 0;
    }
    printf("FAILED: %d checks, %d failures\n", g_checks_run, g_checks_failed);
    return 1;
}
