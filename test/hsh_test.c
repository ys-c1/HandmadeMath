/*
  hsh_test.c - correctness tests and benchmarks for the HSH spatial hash in
  HandmadeMath.h.

  No third-party code is used: the RNG is a small LCG, timing uses
  clock_gettime, and resident-set sampling reads /proc/self/status.

  Build (see run_hsh_bench.sh):
    cc -std=c99 -Wall -Wextra -I.. hsh_test.c -lm -o hsh_test
  The same source is also compiled as C++ to check the header's C++ side.

  The program exits non-zero if any correctness test fails. Benchmarks run
  after the tests and only print measurements.
*/

#define _POSIX_C_SOURCE 199309L

#include "../HandmadeMath.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* ------------------------------------------------------------------------- */
/* Tiny test harness                                                         */
/* ------------------------------------------------------------------------- */

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        g_checks += 1;                                                    \
        if (!(cond))                                                      \
        {                                                                 \
            g_failures += 1;                                              \
            printf("    [FAIL] %s (line %d)\n", #cond, __LINE__);        \
        }                                                                 \
    } while (0)

#define CHECK_EQ_INT(actual, expected)                                    \
    do {                                                                  \
        int hsh_actual_ = (int)(actual);                                  \
        int hsh_expected_ = (int)(expected);                              \
        g_checks += 1;                                                    \
        if (hsh_actual_ != hsh_expected_)                                 \
        {                                                                 \
            g_failures += 1;                                              \
            printf("    [FAIL] %s == %s: got %d, expected %d (line %d)\n",\
                   #actual, #expected, hsh_actual_, hsh_expected_,        \
                   __LINE__);                                             \
        }                                                                 \
    } while (0)

/* ------------------------------------------------------------------------- */
/* Deterministic RNG and timing                                              */
/* ------------------------------------------------------------------------- */

static unsigned int g_rng = 0x12345678u;

static float frand(void)
{
    g_rng = g_rng * 1664525u + 1013904223u;
    return (float)(g_rng >> 8) / (float)(1u << 24);
}

static float frand_range(float lo, float hi)
{
    return lo + (hi - lo) * frand();
}

/* Standard normal via Box-Muller. */
static float grand(void)
{
    float u1 = frand();
    float u2 = frand();
    if (u1 < 1e-7f)
    {
        u1 = 1e-7f;
    }
    return sqrtf(-2.0f * logf(u1)) * cosf(6.28318530718f * u2);
}

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static long read_rss_kb(void)
{
    FILE *f = fopen("/proc/self/status", "r");
    long kb = -1;
    char line[256];
    if (f == NULL)
    {
        return -1;
    }
    while (fgets(line, sizeof(line), f) != NULL)
    {
        if (strncmp(line, "VmRSS:", 6) == 0)
        {
            sscanf(line + 6, "%ld", &kb);
            break;
        }
    }
    fclose(f);
    return kb;
}

static long read_hwm_kb(void)
{
    FILE *f = fopen("/proc/self/status", "r");
    long kb = -1;
    char line[256];
    if (f == NULL)
    {
        return -1;
    }
    while (fgets(line, sizeof(line), f) != NULL)
    {
        if (strncmp(line, "VmHWM:", 6) == 0)
        {
            sscanf(line + 6, "%ld", &kb);
            break;
        }
    }
    fclose(f);
    return kb;
}

/* ------------------------------------------------------------------------- */
/* Reference oracle                                                          */
/* ------------------------------------------------------------------------- */

typedef struct RefObject
{
    float x, y, r;
} RefObject;

static void ref_cell_range(const RefObject *o, float cell_size,
                           int *min_cx, int *min_cy,
                           int *max_cx, int *max_cy)
{
    float inv = 1.0f / cell_size;
    float fx0 = (o->x - o->r) * inv;
    float fx1 = (o->x + o->r) * inv;
    float fy0 = (o->y - o->r) * inv;
    float fy1 = (o->y + o->r) * inv;
    *min_cx = (int)floorf(fx0);
    *max_cx = (int)floorf(fx1);
    *min_cy = (int)floorf(fy0);
    *max_cy = (int)floorf(fy1);
}

/* Exact candidate oracle: pair (i, j) is a candidate iff some occupied cell
   of i is within neighborhood cells of some occupied cell of j. */
static int ref_is_candidate(const RefObject *objs, int i, int j,
                            float cell_size, int neighborhood)
{
    int aminx, aminy, amaxx, amaxy;
    int bminx, bminy, bmaxx, bmaxy;
    int ax, ay;
    ref_cell_range(&objs[i], cell_size, &aminx, &aminy, &amaxx, &amaxy);
    ref_cell_range(&objs[j], cell_size, &bminx, &bminy, &bmaxx, &bmaxy);
    for (ay = aminy; ay <= amaxy; ++ay)
    {
        for (ax = aminx; ax <= amaxx; ++ax)
        {
            int bx, by;
            for (by = bminy; by <= bmaxy; ++by)
            {
                for (bx = bminx; bx <= bmaxx; ++bx)
                {
                    if (abs(ax - bx) <= neighborhood &&
                        abs(ay - by) <= neighborhood)
                    {
                        return 1;
                    }
                }
            }
        }
    }
    return 0;
}

static int ref_pair_cmp(const void *a, const void *b)
{
    const HSH_Pair *pa = (const HSH_Pair *)a;
    const HSH_Pair *pb = (const HSH_Pair *)b;
    if (pa->a != pb->a)
    {
        return pa->a - pb->a;
    }
    return pa->b - pb->b;
}

/* Build the full oracle pair set for a small scene. */
static int ref_all_pairs(const RefObject *objs, int n, float cell_size,
                         int neighborhood, HSH_Pair *out)
{
    int i, j, count = 0;
    for (i = 0; i < n; ++i)
    {
        for (j = i + 1; j < n; ++j)
        {
            if (ref_is_candidate(objs, i, j, cell_size, neighborhood))
            {
                out[count].a = i;
                out[count].b = j;
                count += 1;
            }
        }
    }
    qsort(out, (size_t)count, sizeof(HSH_Pair), ref_pair_cmp);
    return count;
}

/* Number of pairs whose circles genuinely overlap (the narrow-phase truth). */
static int ref_true_overlaps(const RefObject *objs, int n)
{
    int i, j, count = 0;
    for (i = 0; i < n; ++i)
    {
        for (j = i + 1; j < n; ++j)
        {
            float dx = objs[i].x - objs[j].x;
            float dy = objs[i].y - objs[j].y;
            float rr = objs[i].r + objs[j].r;
            if (dx * dx + dy * dy <= rr * rr)
            {
                count += 1;
            }
        }
    }
    return count;
}

/* Compare a query result against the oracle; returns 1 when identical. */
static int pairs_match_oracle(const HSH_Pair *got, int got_count,
                              const HSH_Pair *want, int want_count)
{
    HSH_Pair *sorted;
    int i, ok;
    if (got_count != want_count)
    {
        printf("    [FAIL] pair count: got %d, expected %d\n",
               got_count, want_count);
        return 0;
    }
    sorted = (HSH_Pair *)malloc(sizeof(HSH_Pair) * (size_t)got_count);
    memcpy(sorted, got, sizeof(HSH_Pair) * (size_t)got_count);
    qsort(sorted, (size_t)got_count, sizeof(HSH_Pair), ref_pair_cmp);
    ok = 1;
    for (i = 0; i < got_count; ++i)
    {
        if (sorted[i].a != want[i].a || sorted[i].b != want[i].b)
        {
            printf("    [FAIL] pair %d: got (%d,%d), expected (%d,%d)\n",
                   i, sorted[i].a, sorted[i].b, want[i].a, want[i].b);
            ok = 0;
            break;
        }
        if (i > 0 &&
            sorted[i].a == sorted[i - 1].a &&
            sorted[i].b == sorted[i - 1].b)
        {
            printf("    [FAIL] duplicate pair (%d,%d)\n",
                   sorted[i].a, sorted[i].b);
            ok = 0;
            break;
        }
    }
    free(sorted);
    return ok;
}

static HSH_Result query_all(HSH_Grid *g, HSH_Pair **out, int *out_count)
{
    HSH_Pair dummy;
    HSH_Result rc;
    int required = 0;
    rc = HSH_QueryPairs(g, &dummy, 0, &required);
    if (rc != HSH_ERROR_PAIR_CAPACITY)
    {
        *out_count = required;
        *out = NULL;
        return rc;
    }
    *out = (HSH_Pair *)malloc(sizeof(HSH_Pair) *
                              ((size_t)required + 1));
    rc = HSH_QueryPairs(g, *out, required, out_count);
    return rc;
}

/* ------------------------------------------------------------------------- */
/* Correctness tests                                                         */
/* ------------------------------------------------------------------------- */

static void test_invalid_configs(void)
{
    HSH_Grid *g = NULL;
    HSH_Config cfg;
    HSH_Result rc;

    printf("Config validation:\n");

    rc = HSH_ValidateConfig(NULL);
    CHECK_EQ_INT(rc, HSH_ERROR_INVALID_ARGUMENT);

    cfg = HSH_DefaultConfig(8);
    cfg.cell_size = 0.0f;
    CHECK_EQ_INT(HSH_Init(&g, &cfg), HSH_ERROR_INVALID_CELL_SIZE);
    CHECK(g == NULL);

    cfg = HSH_DefaultConfig(8);
    cfg.cell_size = -1.0f;
    CHECK_EQ_INT(HSH_Init(&g, &cfg), HSH_ERROR_INVALID_CELL_SIZE);

    cfg = HSH_DefaultConfig(8);
    cfg.cell_size = 0.0f / 0.0f; /* NaN */
    CHECK_EQ_INT(HSH_Init(&g, &cfg), HSH_ERROR_INVALID_CELL_SIZE);

    cfg = HSH_DefaultConfig(8);
    cfg.cell_size = 1.0f / 0.0f; /* +inf */
    CHECK_EQ_INT(HSH_Init(&g, &cfg), HSH_ERROR_INVALID_CELL_SIZE);

    cfg = HSH_DefaultConfig(8);
    cfg.neighborhood = -1;
    CHECK_EQ_INT(HSH_Init(&g, &cfg), HSH_ERROR_INVALID_NEIGHBORHOOD);

    cfg = HSH_DefaultConfig(8);
    cfg.bucket_count = 0;
    CHECK_EQ_INT(HSH_Init(&g, &cfg), HSH_ERROR_INVALID_BUCKET_COUNT);

    cfg = HSH_DefaultConfig(8);
    cfg.max_objects = 0;
    CHECK_EQ_INT(HSH_Init(&g, &cfg), HSH_ERROR_INVALID_MAX_OBJECTS);

    cfg = HSH_DefaultConfig(8);
    cfg.max_cell_span = 0;
    CHECK_EQ_INT(HSH_Init(&g, &cfg), HSH_ERROR_INVALID_MAX_SPAN);

    cfg = HSH_DefaultConfig(8);
    cfg.strategy = (HSH_UpdateStrategy)99;
    CHECK_EQ_INT(HSH_Init(&g, &cfg), HSH_ERROR_INVALID_STRATEGY);

    HSH_Free(NULL); /* must not crash */
    printf("    config validation checks done\n");
}

static void test_basic_scene_matches_oracle(void)
{
    /* Hand-made 3x3 layout with a few extra objects. cell size 10. */
    static const RefObject objs[] = {
        {  5.0f,   5.0f, 0.5f},  /* 0: cell (0,0)   */
        {  5.8f,   5.0f, 0.5f},  /* 1: same cell, really overlaps 0 */
        { 25.0f,   5.0f, 0.5f},  /* 2: cell (2,0)   */
        {  5.0f,  15.0f, 0.5f},  /* 3: cell (0,1)   */
        { 15.0f,  15.0f, 0.5f},  /* 4: cell (1,1)   */
        { -5.0f,  -5.0f, 0.5f},  /* 5: cell (-1,-1) */
        {  9.9f,   9.9f, 2.0f},  /* 6: spans cells around (0,0) */
        { 50.0f,  50.0f, 0.5f}   /* 7: isolated, cell (5,5) */
    };
    enum { N = (int)(sizeof(objs) / sizeof(objs[0])) };
    HSH_Config cfg = HSH_DefaultConfig(N);
    HSH_Grid *g = NULL;
    HSH_Pair *got = NULL;
    HSH_Pair want[64];
    int got_count = 0, want_count = 0, i;
    HSH_Stats stats;

    printf("Basic scene vs brute-force oracle (neighborhood 1 and 0):\n");
    cfg.cell_size = 10.0f;
    cfg.neighborhood = 1;
    cfg.bucket_count = 31;
    cfg.max_cell_span = 4;
    CHECK_EQ_INT(HSH_Init(&g, &cfg), HSH_OK);

    for (i = 0; i < N; ++i)
    {
        CHECK_EQ_INT(HSH_Insert(g, i, objs[i].x, objs[i].y, objs[i].r),
                     HSH_OK);
    }
    CHECK_EQ_INT(HSH_Insert(g, 0, 1.0f, 1.0f, 0.5f), HSH_ERROR_FULL);
    CHECK_EQ_INT(HSH_Insert(g, N, 1.0f, 1.0f, 0.5f),
                 HSH_ERROR_INVALID_ARGUMENT);

    CHECK_EQ_INT(query_all(g, &got, &got_count), HSH_OK);
    want_count = ref_all_pairs(objs, N, 10.0f, 1, want);
    CHECK(pairs_match_oracle(got, got_count, want, want_count));
    free(got);

    /* Every truly overlapping pair must be among the candidates. */
    {
        int true_hits = ref_true_overlaps(objs, N);
        int covered = 0, k, m;
        CHECK(true_hits > 0);
        for (k = 0; k < N; ++k)
        {
            for (m = k + 1; m < N; ++m)
            {
                float dx = objs[k].x - objs[m].x;
                float dy = objs[k].y - objs[m].y;
                float rr = objs[k].r + objs[m].r;
                if (dx * dx + dy * dy <= rr * rr)
                {
                    CHECK(ref_is_candidate(objs, k, m, 10.0f, 1) == 1);
                    covered += 1;
                }
            }
        }
        CHECK_EQ_INT(covered, true_hits);
    }

    HSH_GetStats(g, &stats);
    CHECK_EQ_INT(stats.object_count, N);
    CHECK(stats.cell_count > 0);
    CHECK(stats.entry_count >= N);
    CHECK(stats.total_bytes > 0);

    /* Neighborhood 0: only objects sharing a cell pair up. */
    HSH_Reset(g);
    g->cfg.neighborhood = 0;
    for (i = 0; i < N; ++i)
    {
        HSH_Insert(g, i, objs[i].x, objs[i].y, objs[i].r);
    }
    CHECK_EQ_INT(query_all(g, &got, &got_count), HSH_OK);
    want_count = ref_all_pairs(objs, N, 10.0f, 0, want);
    CHECK(pairs_match_oracle(got, got_count, want, want_count));
    free(got);

    HSH_Free(&g);
    CHECK(g == NULL);
}

static void test_multi_cell_object_no_duplicates(void)
{
    /* One wide box spanning a 2x2 block of cells plus point objects in each
       corner cell. Each pair must be reported exactly once. */
    static const RefObject objs[] = {
        {15.0f, 15.0f, 6.0f},  /* 0: spans cells (0..1, 0..1) */
        { 5.0f,  5.0f, 0.5f},  /* 1 */
        {15.0f,  5.0f, 0.5f},  /* 2 */
        { 5.0f, 15.0f, 0.5f},  /* 3 */
        {15.0f, 15.0f, 0.5f},  /* 4 */
        {35.0f, 35.0f, 0.5f}   /* 5: far away, cell (3,3) */
    };
    enum { N = (int)(sizeof(objs) / sizeof(objs[0])) };
    HSH_Config cfg = HSH_DefaultConfig(N);
    HSH_Grid *g = NULL;
    HSH_Pair *got = NULL;
    HSH_Pair want[64];
    int got_count = 0, want_count = 0, i;

    printf("Multi-cell object dedup and span limits:\n");
    cfg.cell_size = 10.0f;
    cfg.neighborhood = 1;
    cfg.bucket_count = 17;
    cfg.max_cell_span = 3;
    CHECK_EQ_INT(HSH_Init(&g, &cfg), HSH_OK);
    for (i = 0; i < N; ++i)
    {
        CHECK_EQ_INT(HSH_Insert(g, i, objs[i].x, objs[i].y, objs[i].r),
                     HSH_OK);
    }
    CHECK_EQ_INT(query_all(g, &got, &got_count), HSH_OK);
    want_count = ref_all_pairs(objs, N, 10.0f, 1, want);
    CHECK(pairs_match_oracle(got, got_count, want, want_count));
    free(got);
    HSH_Free(&g);

    /* Same scene with a span limit of 1: the wide box must be rejected. */
    cfg.max_cell_span = 1;
    CHECK_EQ_INT(HSH_Init(&g, &cfg), HSH_OK);
    CHECK_EQ_INT(HSH_Insert(g, 0, objs[0].x, objs[0].y, objs[0].r),
                 HSH_ERROR_SPAN_TOO_LARGE);
    CHECK_EQ_INT(HSH_Insert(g, 1, objs[1].x, objs[1].y, objs[1].r),
                 HSH_OK);
    HSH_Free(&g);
}

static void test_incremental_moves(void)
{
    static const RefObject start[] = {
        { 5.0f,  5.0f, 0.5f},
        { 7.0f,  5.0f, 0.5f},
        {25.0f, 25.0f, 0.5f}
    };
    enum { N = (int)(sizeof(start) / sizeof(start[0])) };
    HSH_Config cfg = HSH_DefaultConfig(N);
    HSH_Grid *g = NULL;
    HSH_Pair *got = NULL;
    HSH_Pair want[32];
    RefObject cur[3];
    int got_count = 0, want_count = 0, i;
    HSH_Stats stats;

    printf("Incremental move update:\n");
    cfg.cell_size = 10.0f;
    cfg.neighborhood = 1;
    cfg.bucket_count = 13;
    cfg.max_cell_span = 2;
    CHECK_EQ_INT(HSH_Init(&g, &cfg), HSH_OK);
    for (i = 0; i < N; ++i)
    {
        cur[i] = start[i];
        HSH_Insert(g, i, cur[i].x, cur[i].y, cur[i].r);
    }

    /* Move object 0 one cell to the right, next to object 2's old region. */
    HSH_BeginFrame(g);
    cur[0].x = 17.0f;
    CHECK_EQ_INT(HSH_Move(g, 0, cur[0].x, cur[0].y, cur[0].r), HSH_OK);
    HSH_GetStats(g, &stats);
    CHECK_EQ_INT(stats.moved_objects, 1);
    CHECK(stats.touched_cells >= 2);
    CHECK_EQ_INT(query_all(g, &got, &got_count), HSH_OK);
    want_count = ref_all_pairs(cur, N, 10.0f, 1, want);
    CHECK(pairs_match_oracle(got, got_count, want, want_count));
    free(got);

    /* Move inside the same cell: no membership work. */
    HSH_BeginFrame(g);
    cur[0].x = 17.5f;
    HSH_Move(g, 0, cur[0].x, cur[0].y, cur[0].r);
    HSH_GetStats(g, &stats);
    CHECK_EQ_INT(stats.moved_objects, 0);
    CHECK_EQ_INT(stats.touched_cells, 0);

    /* Negative-coordinate move across the origin boundary. */
    HSH_BeginFrame(g);
    cur[1].x = -0.5f;
    cur[1].y = -0.5f;
    HSH_Move(g, 1, cur[1].x, cur[1].y, cur[1].r);
    CHECK_EQ_INT(query_all(g, &got, &got_count), HSH_OK);
    want_count = ref_all_pairs(cur, N, 10.0f, 1, want);
    CHECK(pairs_match_oracle(got, got_count, want, want_count));
    free(got);

    /* Bad moves are rejected and leave the grid usable. */
    CHECK_EQ_INT(HSH_Move(g, 0, 0.0f / 0.0f, 0.0f, 0.5f),
                 HSH_ERROR_INVALID_ARGUMENT);
    CHECK_EQ_INT(HSH_Move(g, N, 0.0f, 0.0f, 0.5f),
                 HSH_ERROR_INVALID_ARGUMENT);
    HSH_Free(&g);
}

static void test_rebuild_matches_incremental(void)
{
    enum { N = 24 };
    HSH_Config cfg_inc = HSH_DefaultConfig(N);
    HSH_Config cfg_reb = HSH_DefaultConfig(N);
    HSH_Grid *inc = NULL;
    HSH_Grid *reb = NULL;
    HSH_Pair *p_inc = NULL;
    HSH_Pair *p_reb = NULL;
    RefObject objs[N];
    int n_inc = 0, n_reb = 0, i, frame;

    printf("Rebuild strategy matches incremental strategy:\n");
    cfg_inc.cell_size = 10.0f;
    cfg_reb.cell_size = 10.0f;
    cfg_inc.bucket_count = 37;
    cfg_reb.bucket_count = 37;
    cfg_inc.max_cell_span = 3;
    cfg_reb.max_cell_span = 3;
    cfg_reb.strategy = HSH_UPDATE_REBUILD;
    CHECK_EQ_INT(HSH_Init(&inc, &cfg_inc), HSH_OK);
    CHECK_EQ_INT(HSH_Init(&reb, &cfg_reb), HSH_OK);

    for (i = 0; i < N; ++i)
    {
        objs[i].x = frand_range(-30.0f, 30.0f);
        objs[i].y = frand_range(-30.0f, 30.0f);
        objs[i].r = frand_range(0.5f, 4.0f);
        CHECK_EQ_INT(HSH_Insert(inc, i, objs[i].x, objs[i].y, objs[i].r),
                     HSH_OK);
        CHECK_EQ_INT(HSH_Insert(reb, i, objs[i].x, objs[i].y, objs[i].r),
                     HSH_OK);
    }

    for (frame = 0; frame < 6; ++frame)
    {
        HSH_BeginFrame(inc);
        for (i = 0; i < N; ++i)
        {
            objs[i].x += frand_range(-12.0f, 12.0f);
            objs[i].y += frand_range(-12.0f, 12.0f);
            HSH_Move(inc, i, objs[i].x, objs[i].y, objs[i].r);
            HSH_Move(reb, i, objs[i].x, objs[i].y, objs[i].r);
        }
        CHECK_EQ_INT(HSH_Rebuild(reb), HSH_OK);
        CHECK_EQ_INT(query_all(inc, &p_inc, &n_inc), HSH_OK);
        CHECK_EQ_INT(query_all(reb, &p_reb, &n_reb), HSH_OK);
        CHECK_EQ_INT(n_inc, n_reb);
        qsort(p_inc, (size_t)n_inc, sizeof(HSH_Pair), ref_pair_cmp);
        qsort(p_reb, (size_t)n_reb, sizeof(HSH_Pair), ref_pair_cmp);
        CHECK(memcmp(p_inc, p_reb,
                     sizeof(HSH_Pair) * (size_t)n_inc) == 0);
        free(p_inc);
        free(p_reb);
    }
    HSH_Free(&inc);
    HSH_Free(&reb);
}

static void test_capacity_and_reset(void)
{
    HSH_Config cfg = HSH_DefaultConfig(4);
    HSH_Grid *g = NULL;
    HSH_Pair pairs[2];
    HSH_Stats stats;
    int count = 0;

    printf("Pair capacity error and reset reuse:\n");
    cfg.cell_size = 10.0f;
    cfg.bucket_count = 7;
    CHECK_EQ_INT(HSH_Init(&g, &cfg), HSH_OK);
    HSH_Insert(g, 0, 1.0f, 1.0f, 0.5f);
    HSH_Insert(g, 1, 2.0f, 1.0f, 0.5f);
    HSH_Insert(g, 2, 1.0f, 2.0f, 0.5f);

    CHECK_EQ_INT(HSH_QueryPairs(g, pairs, 2, &count),
                 HSH_ERROR_PAIR_CAPACITY);
    CHECK(count >= 3); /* required count is reported even when truncated */

    HSH_Reset(g);
    HSH_GetStats(g, &stats);
    CHECK_EQ_INT(stats.object_count, 0);
    CHECK_EQ_INT(stats.cell_count, 0);
    CHECK_EQ_INT(stats.entry_count, 0);

    /* Storage is reused after reset. */
    CHECK_EQ_INT(HSH_Insert(g, 0, 99.0f, 99.0f, 0.5f), HSH_OK);
    CHECK_EQ_INT(HSH_QueryPairs(g, pairs, 2, &count), HSH_OK);
    CHECK_EQ_INT(count, 0);
    HSH_Free(&g);
}

static void test_random_no_false_negatives(void)
{
    /* Randomized invariant test over several configurations and frames:
       every genuinely overlapping pair must always appear in the query. */
    enum { RN = 60, RFRAMES = 30, TRIALS = 8 };
    int trial;
    printf("Randomized no-false-negative trials:\n");
    for (trial = 0; trial < TRIALS; ++trial)
    {
        HSH_Config cfg = HSH_DefaultConfig(RN);
        HSH_Grid *g = NULL;
        RefObject objs[RN];
        HSH_Pair *got = NULL;
        int got_count = 0;
        int i, frame;
        float cell_size = 5.0f + (float)(trial % 5) * 5.0f;
        int neighborhood = trial % 3;

        cfg.cell_size = cell_size;
        cfg.neighborhood = neighborhood;
        cfg.bucket_count = 53 + trial * 7;
        cfg.max_cell_span = 6;
        CHECK_EQ_INT(HSH_Init(&g, &cfg), HSH_OK);

        for (i = 0; i < RN; ++i)
        {
            objs[i].x = frand_range(-40.0f, 40.0f);
            objs[i].y = frand_range(-40.0f, 40.0f);
            objs[i].r = frand_range(0.5f, 6.0f);
            HSH_Result rc = HSH_Insert(g, i, objs[i].x, objs[i].y,
                                       objs[i].r);
            CHECK(rc == HSH_OK || rc == HSH_ERROR_SPAN_TOO_LARGE);
            if (rc == HSH_ERROR_SPAN_TOO_LARGE)
            {
                /* Keep only objects the configuration accepts. */
                objs[i].r = 0.5f;
                CHECK_EQ_INT(HSH_Insert(g, i, objs[i].x, objs[i].y,
                                        objs[i].r), HSH_OK);
            }
        }

        for (frame = 0; frame < RFRAMES; ++frame)
        {
            int k, m;
            HSH_BeginFrame(g);
            for (i = 0; i < RN; ++i)
            {
                objs[i].x += frand_range(-8.0f, 8.0f);
                objs[i].y += frand_range(-8.0f, 8.0f);
                HSH_Move(g, i, objs[i].x, objs[i].y, objs[i].r);
            }
            CHECK_EQ_INT(query_all(g, &got, &got_count), HSH_OK);

            /* Mark candidate pairs, then require every true overlap. */
            {
                char *is_cand = (char *)calloc(
                    (size_t)RN * RN, 1);
                int p;
                for (p = 0; p < got_count; ++p)
                {
                    is_cand[got[p].a * RN + got[p].b] = 1;
                }
                for (k = 0; k < RN; ++k)
                {
                    for (m = k + 1; m < RN; ++m)
                    {
                        float dx = objs[k].x - objs[m].x;
                        float dy = objs[k].y - objs[m].y;
                        float rr = objs[k].r + objs[m].r;
                        if (dx * dx + dy * dy <= rr * rr)
                        {
                            CHECK(is_cand[k * RN + m] == 1);
                        }
                    }
                }
                free(is_cand);
            }
            free(got);
        }
        HSH_Free(&g);
    }
}

static void test_memory_stable_across_frames(void)
{
    enum { N = 40, FRAMES = 2000 };
    HSH_Config cfg = HSH_DefaultConfig(N);
    HSH_Grid *g = NULL;
    RefObject objs[N];
    HSH_Stats stats;
    size_t bytes_first = 0, bytes_last = 0;
    unsigned long bytes_ul_first, bytes_ul_last;
    long rss_first = 0, rss_last = 0;
    int i, frame;
    int cells_first = 0, cells_last = 0;
    int entries_first = 0, entries_last = 0;

    printf("Memory stability over %d frames:\n", FRAMES);
    cfg.cell_size = 10.0f;
    cfg.bucket_count = 67;
    cfg.max_cell_span = 4;
    CHECK_EQ_INT(HSH_Init(&g, &cfg), HSH_OK);
    for (i = 0; i < N; ++i)
    {
        objs[i].x = frand_range(-50.0f, 50.0f);
        objs[i].y = frand_range(-50.0f, 50.0f);
        objs[i].r = frand_range(0.5f, 3.0f);
        HSH_Insert(g, i, objs[i].x, objs[i].y, objs[i].r);
    }

    for (frame = 0; frame < FRAMES; ++frame)
    {
        HSH_BeginFrame(g);
        for (i = 0; i < N; ++i)
        {
            objs[i].x += frand_range(-15.0f, 15.0f);
            objs[i].y += frand_range(-15.0f, 15.0f);
            HSH_Move(g, i, objs[i].x, objs[i].y, objs[i].r);
        }
        if (frame == 0)
        {
            HSH_GetStats(g, &stats);
            bytes_first = stats.total_bytes;
            bytes_ul_first = (unsigned long)bytes_first;
            cells_first = stats.cell_count;
            entries_first = stats.entry_count;
            rss_first = read_rss_kb();
        }
        if (frame == FRAMES - 1)
        {
            HSH_GetStats(g, &stats);
            bytes_last = stats.total_bytes;
            bytes_ul_last = (unsigned long)bytes_last;
            cells_last = stats.cell_count;
            entries_last = stats.entry_count;
            rss_last = read_rss_kb();
        }
    }

    printf("    frame 1   : total_bytes=%lu cells=%d entries=%d VmRSS=%ld KB\n",
           bytes_ul_first, cells_first, entries_first, rss_first);
    printf("    frame %d: total_bytes=%lu cells=%d entries=%d VmRSS=%ld KB\n",
           FRAMES, bytes_ul_last, cells_last, entries_last, rss_last);
    /* The configured footprint is the stability contract: it must not depend
       on the frame count. RSS may shift by a small constant from allocator
       warm-up but must not grow with the number of frames. */
    CHECK_EQ_INT((int)bytes_first, (int)bytes_last);
    CHECK(rss_last <= rss_first + 512);
    CHECK(cells_last <= cfg.max_objects * cfg.max_cell_span *
                       cfg.max_cell_span);
    CHECK(entries_last <= cfg.max_objects * cfg.max_cell_span *
                         cfg.max_cell_span);
    HSH_GetStats(g, &stats);
    CHECK_EQ_INT(stats.object_count, N);
    HSH_Free(&g);
}

/* ------------------------------------------------------------------------- */
/* Benchmarks                                                                */
/* ------------------------------------------------------------------------- */

#define BENCH_N 20000
#define BENCH_FRAMES 5
#define BENCH_CELL 10.0f
#define BENCH_BUCKETS 4099

typedef enum ScenarioKind
{
    SC_UNIFORM = 0,
    SC_CLUSTER = 1,
    SC_STRIP = 2,
    SC_HIGHSPEED = 3
} ScenarioKind;

static const char *scenario_name(ScenarioKind k)
{
    switch (k)
    {
    case SC_UNIFORM: return "uniform   ";
    case SC_CLUSTER: return "clustered ";
    case SC_STRIP: return "long-strip";
    case SC_HIGHSPEED: return "high-speed";
    default: return "?";
    }
}

static void scenario_position(ScenarioKind k, int id, float *x, float *y)
{
    switch (k)
    {
    case SC_UNIFORM:
    case SC_HIGHSPEED:
        *x = frand() * 500.0f;
        *y = frand() * 500.0f;
        break;
    case SC_CLUSTER:
    {
        int cluster = id % 8;
        float cx = 62.5f + (float)(cluster % 4) * 125.0f;
        float cy = 125.0f + (float)(cluster / 4) * 250.0f;
        *x = cx + grand() * 20.0f;
        *y = cy + grand() * 20.0f;
        break;
    }
    case SC_STRIP:
        *x = frand() * 4000.0f;
        *y = frand() * 10.0f;
        break;
    }
}

static double benchmark_brute(const RefObject *objs, int n,
                              int *out_true_hits, double *out_seconds)
{
    double t0 = now_seconds();
    double comparisons = 0;
    int hits = 0;
    int i, j;
    for (i = 0; i < n; ++i)
    {
        for (j = i + 1; j < n; ++j)
        {
            float dx = objs[i].x - objs[j].x;
            float dy = objs[i].y - objs[j].y;
            float rr = objs[i].r + objs[j].r;
            comparisons += 1.0;
            if (dx * dx + dy * dy <= rr * rr)
            {
                hits += 1;
            }
        }
    }
    *out_seconds = now_seconds() - t0;
    *out_true_hits = hits;
    return comparisons;
}

static void run_scenario(ScenarioKind kind, HSH_UpdateStrategy strategy,
                         float cell_size, int neighborhood,
                         long *out_candidates, double *out_update_s,
                         double *out_query_s)
{
    HSH_Config cfg = HSH_DefaultConfig(BENCH_N);
    HSH_Grid *g = NULL;
    RefObject *objs;
    HSH_Pair *pairs = NULL;
    int pair_count = 0;
    int i, frame;
    double update_total = 0.0;
    double query_total = 0.0;
    double t0, t1;
    long cand_total = 0;

    cfg.cell_size = cell_size;
    cfg.neighborhood = neighborhood;
    cfg.bucket_count = BENCH_BUCKETS;
    cfg.max_cell_span = 8;
    cfg.strategy = strategy;
    if (HSH_Init(&g, &cfg) != HSH_OK)
    {
        printf("    grid init failed for scenario %d\n", (int)kind);
        exit(2);
    }
    objs = (RefObject *)malloc(sizeof(RefObject) * BENCH_N);

    for (i = 0; i < BENCH_N; ++i)
    {
        scenario_position(kind, i, &objs[i].x, &objs[i].y);
        objs[i].r = 0.5f;
        HSH_Insert(g, i, objs[i].x, objs[i].y, objs[i].r);
    }

    for (frame = 0; frame < BENCH_FRAMES; ++frame)
    {
        HSH_BeginFrame(g);
        t0 = now_seconds();
        for (i = 0; i < BENCH_N; ++i)
        {
            if (kind == SC_HIGHSPEED)
            {
                float vx = frand_range(-150.0f, 150.0f);
                float vy = frand_range(-150.0f, 150.0f);
                objs[i].x = fmodf(objs[i].x + vx + 500.0f, 500.0f);
                objs[i].y = fmodf(objs[i].y + vy + 500.0f, 500.0f);
            }
            else
            {
                objs[i].x += frand_range(-3.0f, 3.0f);
                objs[i].y += frand_range(-3.0f, 3.0f);
                if (objs[i].x < 0.0f) { objs[i].x += 500.0f; }
                if (objs[i].x > 500.0f) { objs[i].x -= 500.0f; }
                if (objs[i].y < 0.0f) { objs[i].y += 500.0f; }
                if (objs[i].y > 500.0f) { objs[i].y -= 500.0f; }
            }
            HSH_Move(g, i, objs[i].x, objs[i].y, objs[i].r);
        }
        if (strategy == HSH_UPDATE_REBUILD)
        {
            HSH_Rebuild(g);
        }
        t1 = now_seconds();
        update_total += t1 - t0;

        t0 = now_seconds();
        {
            int required = 0;
            HSH_Pair dummy;
            HSH_Result rc = HSH_QueryPairs(g, &dummy, 0, &required);
            if (rc != HSH_ERROR_PAIR_CAPACITY)
            {
                pair_count = required;
            }
            else
            {
                free(pairs);
                pairs = (HSH_Pair *)malloc(
                    sizeof(HSH_Pair) * ((size_t)required + 1));
                HSH_QueryPairs(g, pairs, required, &pair_count);
            }
        }
        t1 = now_seconds();
        query_total += t1 - t0;
        cand_total += pair_count;
    }

    *out_candidates = cand_total / BENCH_FRAMES;
    *out_update_s = update_total / BENCH_FRAMES;
    *out_query_s = query_total / BENCH_FRAMES;
    free(pairs);
    free(objs);
    HSH_Free(&g);
}

/* Per-scenario cell-size tuning: the fixed cell=10 benchmark above is not
   optimal for every distribution. This sweep shows the cell size at which
   each scene minimizes query cost, which is the practical tuning knob. */
static void run_scenario_tuning(void)
{
    static const ScenarioKind kinds[4] = {
        SC_UNIFORM, SC_CLUSTER, SC_STRIP, SC_HIGHSPEED
    };
    static const float cells[4] = { 2.0f, 5.0f, 10.0f, 20.0f };
    int k, ci;
    printf("\n");
    printf("Per-scenario cell-size tuning (neighborhood=1, N=%d):\n",
           BENCH_N);
    printf("scenario    | cell_size | candidates/frame | "
           "update s/frame | query s/frame\n");
    printf("------------+-----------+------------------+"
           "----------------+---------------\n");
    for (k = 0; k < 4; ++k)
    {
        for (ci = 0; ci < 4; ++ci)
        {
            long candidates = 0;
            double upd = 0.0, qry = 0.0;
            g_rng = 0x12345678u + (unsigned int)k;
            run_scenario(kinds[k], HSH_UPDATE_INCREMENTAL,
                         cells[ci], 1, &candidates, &upd, &qry);
            printf("%s | %9.1f | %16ld | %14.4f | %13.4f\n",
                   scenario_name(kinds[k]), cells[ci], candidates,
                   upd, qry);
        }
    }
}

/* Cell size / neighborhood sweep on the uniform scene: shows how the two
   configurable parameters trade candidate count for update cost. */
static void run_config_sweep(void)
{
    static const float cells[3] = { 5.0f, 10.0f, 20.0f };
    int ci, nb;
    printf("\n");
    printf("Config sweep on the uniform scene (N=%d):\n", BENCH_N);
    printf("cell_size | neighborhood | candidates/frame | "
           "update s/frame | query s/frame\n");
    printf("----------+--------------+------------------+"
           "----------------+---------------\n");
    for (ci = 0; ci < 3; ++ci)
    {
        for (nb = 0; nb <= 2; ++nb)
        {
            long candidates = 0;
            double upd = 0.0, qry = 0.0;
            g_rng = 0x12345678u;
            run_scenario(SC_UNIFORM, HSH_UPDATE_INCREMENTAL,
                         cells[ci], nb, &candidates, &upd, &qry);
            printf("%9.1f | %12d | %16ld | %14.4f | %13.4f\n",
                   cells[ci], nb, candidates, upd, qry);
        }
    }
}

static void run_benchmarks(void)
{
    ScenarioKind kinds[4] = {
        SC_UNIFORM, SC_CLUSTER, SC_STRIP, SC_HIGHSPEED
    };
    int k;
    double brute_comparisons =
        (double)BENCH_N * (BENCH_N - 1) / 2.0;

    printf("\n");
    printf("==================== Benchmarks (N=%d, %d timed frames) "
           "====================\n", BENCH_N, BENCH_FRAMES);
    printf("cell_size=%.1f neighborhood=1 buckets=%d radius=0.5\n",
           BENCH_CELL, BENCH_BUCKETS);
    printf("\n");
    printf("scenario    | hash candidates/frame | brute comparisons/frame | "
           "true overlaps | brute time (s)\n");
    printf("------------+------------------------+"
           "--------------------------+"
           "---------------+----------------\n");

    for (k = 0; k < 4; ++k)
    {
        RefObject *objs =
            (RefObject *)malloc(sizeof(RefObject) * BENCH_N);
        int i, true_hits = 0;
        double brute_s = 0.0;
        long candidates_inc = 0, candidates_reb = 0;
        double upd_inc = 0.0, qry_inc = 0.0, upd_reb = 0.0, qry_reb = 0.0;

        g_rng = 0x12345678u + (unsigned int)k;
        for (i = 0; i < BENCH_N; ++i)
        {
            scenario_position(kinds[k], i, &objs[i].x, &objs[i].y);
            objs[i].r = 0.5f;
        }
        benchmark_brute(objs, BENCH_N, &true_hits, &brute_s);

        g_rng = 0x12345678u + (unsigned int)k;
        run_scenario(kinds[k], HSH_UPDATE_INCREMENTAL, BENCH_CELL, 1,
                     &candidates_inc, &upd_inc, &qry_inc);
        g_rng = 0x12345678u + (unsigned int)k;
        run_scenario(kinds[k], HSH_UPDATE_REBUILD, BENCH_CELL, 1,
                     &candidates_reb, &upd_reb, &qry_reb);
        CHECK_EQ_INT((int)(candidates_inc == candidates_reb), 1);

        printf("%s | %22ld | %24.0f | %13d | %14.4f\n",
               scenario_name(kinds[k]), candidates_inc,
               brute_comparisons, true_hits, brute_s);
        printf("  incremental: update %.4f s/frame, query %.4f s/frame"
               " -> query speedup x%.1f, end-to-end x%.1f\n",
               upd_inc, qry_inc,
               brute_s / qry_inc,
               brute_s / (upd_inc + qry_inc));
        printf("  rebuild    : update %.4f s/frame, query %.4f s/frame"
               " -> query speedup x%.1f, end-to-end x%.1f\n",
               upd_reb, qry_reb,
               brute_s / qry_reb,
               brute_s / (upd_reb + qry_reb));
        free(objs);
    }

    run_config_sweep();
    run_scenario_tuning();

    printf("\n");
    printf("Memory (one grid, N=%d, max_cell_span=8):\n", BENCH_N);
    {
        HSH_Config cfg = HSH_DefaultConfig(BENCH_N);
        HSH_Grid *g = NULL;
        HSH_Stats st;
        cfg.cell_size = BENCH_CELL;
        cfg.bucket_count = BENCH_BUCKETS;
        cfg.max_cell_span = 8;
        if (HSH_Init(&g, &cfg) == HSH_OK)
        {
            HSH_GetStats(g, &st);
            printf("  configured pool size: %lu bytes (%.2f MB); this is the\n"
                   "  full high-water mark: no operation grows it, so it is\n"
                   "  independent of frame count.\n",
                   (unsigned long)st.total_bytes,
                   (double)st.total_bytes / (1024.0 * 1024.0));
            printf("  process VmRSS with the grid resident: %ld KB (%.2f MB)\n",
                   read_rss_kb(), (double)read_rss_kb() / 1024.0);
            printf("  process VmHWM (includes this benchmark's pair buffers): "
                   "%ld KB (%.2f MB)\n",
                   read_hwm_kb(), (double)read_hwm_kb() / 1024.0);
            HSH_Free(&g);
        }
    }
}

int main(void)
{
    printf("Running HSH spatial-hash tests\n\n");

    test_invalid_configs();
    test_basic_scene_matches_oracle();
    test_multi_cell_object_no_duplicates();
    test_incremental_moves();
    test_rebuild_matches_incremental();
    test_capacity_and_reset();
    test_random_no_false_negatives();
    test_memory_stable_across_frames();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures != 0)
    {
        printf("TESTS FAILED\n");
        return 1;
    }
    printf("ALL TESTS PASSED\n");

    if (getenv("HSH_SKIP_BENCH") != NULL)
    {
        printf("\n(HSH_SKIP_BENCH set: skipping benchmarks)\n");
        return 0;
    }

    run_benchmarks();
    return 0;
}
