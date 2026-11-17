/*
 * camera_path_smoothing.c
 *
 * Curvature-constrained interpolation for hand-placed camera paths.
 * Hand-placed control points produce hard corners; this smooths the
 * Catmull-Rom spline through them so the delivered path stays under a
 * curvature bound.
 *
 * Pipeline (three stages):
 *   1. Measure  - resample the path the way it will be delivered and
 *                 compute the max curvature (Menger/circumcircle
 *                 curvature of consecutive sample triples).
 *   2. Enforce  - iterate until the delivered output is under the bound.
 *                 Each iteration is one curve-shortening step: redistribute
 *                 the working points uniformly by arc length, relax them
 *                 toward their neighbours (Laplacian), and, for closed
 *                 loops, rescale to the original perimeter so the loop
 *                 cannot collapse.  Two mechanisms, selected by config:
 *                 * allow_move_original != 0: the working points ARE the
 *                   original control points; the spline keeps
 *                   interpolating the adjusted polygon.
 *                 * allow_move_original == 0: the original control points
 *                   stay untouched; the working points are intermediate
 *                   points inserted by densifying the spline, and only
 *                   they are adjusted.  (A hard corner that must be
 *                   interpolated cannot be smoothed by insertion alone -
 *                   its curvature scales with 1/spacing - so the inserted
 *                   points form the delivered path and may leave the
 *                   original waypoints.)
 *                 The lowest-curvature state seen is kept, so an
 *                 unsatisfiable bound still reports the closest result.
 *   3. Resample - emit the final path uniformly, N samples per segment
 *                 (per spline segment when moving control points, uniform
 *                 by arc length over the refined polyline otherwise).
 *
 * Stage 2 measures curvature on the would-be delivered output (same
 * resampling as stage 3), so the bound constrains exactly what is
 * reported and delivered.
 *
 * Configurable (SmoothCfg): curvature upper bound, samples per segment,
 * whether original control points may move, iteration budget, relax step.
 *
 * Reported per path: max curvature before/after, deviation from the
 * original control points (control-point displacement when they may
 * move, distance from the original points to the delivered path
 * otherwise), segment-count change, elapsed time.  If the bound cannot
 * be met, the closest achievable curvature is reported instead.
 *
 * Standalone script: not part of the Makefile builds, links only against
 * HandmadeMath.h (no new dependencies, no network).
 *
 * Build & run (from the test/ directory):
 *   cc -std=c99 -Wall -Wextra camera_path_smoothing.c -lm -o camera_path_smoothing
 *   ./camera_path_smoothing
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../HandmadeMath.h"

#define MAX_POLY 16384      /* point budget for the working polyline */

typedef struct {
    float kappa_max;          /* curvature upper bound (1 / unit length)   */
    int   samples_per_segment;/* output resampling density (stage 3)       */
    int   allow_move_original;/* nonzero: original control points may move */
    int   max_iterations;     /* enforcement budget (stage 2)              */
    float relax;              /* Laplacian step per sweep, (0, 1]          */
} SmoothCfg;

typedef struct {
    const char *name;
    int closed;
    int seg_before, seg_after;
    float kappa_before, kappa_after; /* max curvature of resampled paths  */
    float dev_max, dev_mean;         /* deviation from original points    */
    int iterations;
    int satisfied;
    double elapsed_ms;
} Report;

/* ------------------------------------------------------------------ */
/* Spline + polyline primitives                                        */
/* ------------------------------------------------------------------ */

/* Uniform Catmull-Rom segment, t in [0, 1]. */
static HMM_Vec3 cr_eval(HMM_Vec3 p0, HMM_Vec3 p1, HMM_Vec3 p2, HMM_Vec3 p3, float t)
{
    float t2 = t * t;
    float t3 = t2 * t;
    HMM_Vec3 a = HMM_MulV3F(p1, 2.0f);
    HMM_Vec3 b = HMM_MulV3F(HMM_SubV3(p2, p0), t);
    HMM_Vec3 c = HMM_MulV3F(HMM_SubV3(HMM_AddV3(HMM_MulV3F(p0, 2.0f), HMM_MulV3F(p2, 4.0f)),
                                      HMM_AddV3(HMM_MulV3F(p1, 5.0f), p3)), t2);
    HMM_Vec3 d = HMM_MulV3F(HMM_AddV3(HMM_SubV3(HMM_MulV3F(p1, 3.0f), p0),
                                      HMM_SubV3(p3, HMM_MulV3F(p2, 3.0f))), t3);
    return HMM_MulV3F(HMM_AddV3(HMM_AddV3(a, b), HMM_AddV3(c, d)), 0.5f);
}

static void segment_neighbours(const HMM_Vec3 *pts, int n, int closed, int seg,
                               HMM_Vec3 *p0, HMM_Vec3 *p1, HMM_Vec3 *p2, HMM_Vec3 *p3)
{
    if (closed) {
        *p0 = pts[(seg - 1 + n) % n];
        *p1 = pts[seg];
        *p2 = pts[(seg + 1) % n];
        *p3 = pts[(seg + 2) % n];
    } else {
        *p0 = pts[seg > 0 ? seg - 1 : 0];
        *p1 = pts[seg];
        *p2 = pts[seg + 1];
        *p3 = pts[seg + 2 < n ? seg + 2 : n - 1];
    }
}

/* Menger curvature of the circumcircle through a, b, c. */
static float menger_curvature(HMM_Vec3 a, HMM_Vec3 b, HMM_Vec3 c)
{
    HMM_Vec3 ab = HMM_SubV3(b, a);
    HMM_Vec3 ac = HMM_SubV3(c, a);
    float lab = HMM_LenV3(ab);
    float lbc = HMM_LenV3(HMM_SubV3(c, b));
    float lac = HMM_LenV3(ac);
    float denom = lab * lbc * lac;
    if (denom < 1e-20f) {
        return 0.0f;
    }
    return 2.0f * HMM_LenV3(HMM_Cross(ab, ac)) / denom;
}

static float polyline_max_curvature(const HMM_Vec3 *pts, int count, int closed)
{
    float best = 0.0f;
    int lim = closed ? count : count - 2;
    int i;

    for (i = 0; i < lim; ++i) {
        float kappa = menger_curvature(pts[i], pts[(i + 1) % count], pts[(i + 2) % count]);
        if (kappa > best) {
            best = kappa;
        }
    }
    return best;
}

/* One Jacobi Laplacian sweep; open-path endpoints stay pinned.  The
 * stride widens the neighbourhood: cycling strides gives multi-scale
 * diffusion, so long-wavelength error decays in O(1) sweeps instead of
 * O(n^2). */
static void laplacian_sweep(HMM_Vec3 *pts, HMM_Vec3 *tmp, int n, int closed,
                            int stride, float relax)
{
    int i;
    for (i = 0; i < n; ++i) {
        int ip, in;
        HMM_Vec3 mid;
        if (closed) {
            ip = (i - stride + n) % n;
            in = (i + stride) % n;
        } else {
            ip = i - stride < 0 ? 0 : i - stride;
            in = i + stride > n - 1 ? n - 1 : i + stride;
        }
        mid = HMM_MulV3F(HMM_AddV3(pts[ip], pts[in]), 0.5f);
        tmp[i] = HMM_LerpV3(pts[i], relax, mid);
    }
    if (!closed) {
        tmp[0] = pts[0];
        tmp[n - 1] = pts[n - 1];
    }
    memcpy(pts, tmp, (size_t)n * sizeof(*pts));
}

static float polyline_perimeter(const HMM_Vec3 *pts, int n, int closed)
{
    float total = 0.0f;
    int segs = closed ? n : n - 1;
    int i;
    for (i = 0; i < segs; ++i) {
        total += HMM_LenV3(HMM_SubV3(pts[(i + 1) % n], pts[i]));
    }
    return total;
}

/* Rescale about the centroid so a closed loop cannot collapse. */
static void preserve_perimeter(HMM_Vec3 *pts, int n, float target_perim)
{
    float perim = polyline_perimeter(pts, n, 1);
    HMM_Vec3 centroid = HMM_V3(0.0f, 0.0f, 0.0f);
    float scale;
    int i;

    if (perim < 1e-12f) {
        return;
    }
    scale = target_perim / perim;
    for (i = 0; i < n; ++i) {
        centroid = HMM_AddV3(centroid, pts[i]);
    }
    centroid = HMM_MulV3F(centroid, 1.0f / (float)n);
    for (i = 0; i < n; ++i) {
        pts[i] = HMM_AddV3(centroid, HMM_MulV3F(HMM_SubV3(pts[i], centroid), scale));
    }
}

/* ------------------------------------------------------------------ */
/* Resampling                                                          */
/* ------------------------------------------------------------------ */

/* Sample the spline, per_seg points per segment. Returns point count. */
static int sample_spline(const HMM_Vec3 *pts, int n, int closed, int per_seg,
                         HMM_Vec3 *out, int out_cap)
{
    int segs = closed ? n : n - 1;
    int count = 0;
    int s, k;

    for (s = 0; s < segs; ++s) {
        HMM_Vec3 p0, p1, p2, p3;
        segment_neighbours(pts, n, closed, s, &p0, &p1, &p2, &p3);
        for (k = 0; k < per_seg; ++k) {
            if (count >= out_cap) {
                return count;
            }
            out[count++] = cr_eval(p0, p1, p2, p3, (float)k / (float)per_seg);
        }
    }
    if (!closed && count < out_cap) {
        out[count++] = pts[n - 1];
    }
    return count;
}

/* Resample a polyline uniformly by arc length. Returns point count. */
static int resample_polyline_arclength(const HMM_Vec3 *pts, int n, int closed,
                                       int want, HMM_Vec3 *out)
{
    double *accum = (double *)malloc((size_t)n * sizeof(*accum));
    double total = 0.0;
    int segs = closed ? n : n - 1;
    int i, w;

    for (i = 0; i < segs; ++i) {
        accum[i] = total;
        total += (double)HMM_LenV3(HMM_SubV3(pts[(i + 1) % n], pts[i]));
    }
    if (total <= 0.0) {
        out[0] = pts[0];
        free(accum);
        return 1;
    }
    {
        int seg = 0;
        for (w = 0; w < want; ++w) {
            double s = (closed ? (double)w / (double)want
                               : (double)w / (double)(want - 1)) * total;
            double t;
            while (seg < segs - 1 && accum[seg + 1] < s) {
                seg++;
            }
            {
                double seg_len = (seg + 1 < segs ? accum[seg + 1] : total) - accum[seg];
                t = seg_len > 0.0 ? (s - accum[seg]) / seg_len : 0.0;
            }
            out[w] = HMM_LerpV3(pts[seg], (float)t, pts[(seg + 1) % n]);
        }
    }
    free(accum);
    return want;
}

/* In-place arc-length redistribution: same count, uniform spacing. */
static void redistribute_arclength(HMM_Vec3 *pts, HMM_Vec3 *tmp, int n, int closed)
{
    resample_polyline_arclength(pts, n, closed, n, tmp);
    memcpy(pts, tmp, (size_t)n * sizeof(*pts));
}

/* ------------------------------------------------------------------ */
/* Stage 2: enforce the curvature bound                                */
/*                                                                     */
/* move_mode != 0: working points are the control points; curvature    */
/* is measured on the spline through them, sampled N per segment.      */
/* move_mode == 0: working points are inserted intermediate points;    */
/* curvature is measured on their arc-length-uniform resample.         */
/* ------------------------------------------------------------------ */

static float measure_output(const HMM_Vec3 *pts, int n, int closed,
                            int move_mode, int want, HMM_Vec3 *buf, int buf_cap)
{
    int count;
    if (move_mode) {
        count = sample_spline(pts, n, closed, want, buf, buf_cap);
    } else {
        count = resample_polyline_arclength(pts, n, closed, want, buf);
    }
    return polyline_max_curvature(buf, count, closed);
}

static int enforce_curvature(HMM_Vec3 *pts, int n, int closed,
                             const SmoothCfg *cfg, int move_mode, int out_want,
                             int *out_iterations, HMM_Vec3 *buf, int buf_cap)
{
    float best_kappa = 1e30f;
    float perim = closed ? polyline_perimeter(pts, n, 1) : 0.0f;
    HMM_Vec3 *snap = (HMM_Vec3 *)malloc((size_t)MAX_POLY * sizeof(*snap));
    HMM_Vec3 *tmp = (HMM_Vec3 *)malloc((size_t)MAX_POLY * sizeof(*tmp));
    HMM_Vec3 *red = (HMM_Vec3 *)malloc((size_t)MAX_POLY * sizeof(*red));
    int satisfied = 0;
    int iter;

    *out_iterations = 0;
    for (iter = 0; iter < cfg->max_iterations; ++iter) {
        float kappa;
        if (move_mode) {
            kappa = measure_output(pts, n, closed, 1, cfg->samples_per_segment,
                                   buf, buf_cap);
        } else {
            kappa = measure_output(pts, n, closed, 0, out_want, buf, buf_cap);
        }
        if (kappa < best_kappa) {
            best_kappa = kappa;
            memcpy(snap, pts, (size_t)n * sizeof(*snap));
        }
        if (kappa <= cfg->kappa_max) {
            satisfied = 1;
            break;
        }
        *out_iterations = iter + 1;

        /* One curve-shortening step: redistribute, then a Laplacian
         * sweep with a cycling stride (multi-scale diffusion). */
        {
            static const int STRIDES[6] = { 1, 2, 4, 8, 16, 32 };
            int stride = STRIDES[iter % 6];
            int cap = n / 4 > 1 ? n / 4 : 1;
            if (stride > cap) {
                stride = cap;
            }
            redistribute_arclength(pts, red, n, closed);
            laplacian_sweep(pts, tmp, n, closed, stride, cfg->relax);
        }
        if (closed) {
            preserve_perimeter(pts, n, perim);
        }
    }
    if (!satisfied) {
        memcpy(pts, snap, (size_t)n * sizeof(*pts)); /* closest result */
    }
    free(snap);
    free(tmp);
    free(red);
    return satisfied;
}

/* ------------------------------------------------------------------ */
/* Scenario runner                                                     */
/* ------------------------------------------------------------------ */

static double now_ms(void)
{
    return 1000.0 * (double)clock() / (double)CLOCKS_PER_SEC;
}

static void run_scenario(const char *name, const HMM_Vec3 *ctrl, int n, int closed,
                         const SmoothCfg *cfg, Report *rep)
{
    double t0 = now_ms();
    HMM_Vec3 *work = (HMM_Vec3 *)malloc((size_t)MAX_POLY * sizeof(*work));
    HMM_Vec3 *output = (HMM_Vec3 *)malloc((size_t)MAX_POLY * sizeof(*output));
    int segs = closed ? n : n - 1;
    int work_n = n;
    int out_want = closed ? segs * cfg->samples_per_segment
                          : segs * cfg->samples_per_segment + 1;
    int out_count = 0;
    int iterations = 0;
    int i;

    memcpy(work, ctrl, (size_t)n * sizeof(*work));

    rep->name = name;
    rep->closed = closed;
    rep->seg_before = segs;

    /* Stage 1: curvature of the raw path, measured exactly like the
     * delivered output will be (resampled at the configured density). */
    rep->kappa_before = measure_output(ctrl, n, closed, 1, cfg->samples_per_segment,
                                       output, MAX_POLY);

    /* Stage 2. */
    if (cfg->allow_move_original) {
        rep->satisfied = enforce_curvature(work, work_n, closed, cfg, 1, out_want,
                                           &iterations, output, MAX_POLY);
    } else {
        /* Insert intermediate points: densify the spline into a polyline
         * at the output density.  The original control points are never
         * touched; only the inserted points are adjusted. */
        work_n = sample_spline(ctrl, n, closed, cfg->samples_per_segment,
                               work, MAX_POLY);
        rep->satisfied = enforce_curvature(work, work_n, closed, cfg, 0, out_want,
                                           &iterations, output, MAX_POLY);
    }
    rep->iterations = iterations;
    rep->seg_after = closed ? work_n : work_n - 1;

    /* Deviation from the original control points. */
    rep->dev_max = 0.0f;
    rep->dev_mean = 0.0f;
    if (cfg->allow_move_original) {
        for (i = 0; i < n; ++i) {
            float d = HMM_LenV3(HMM_SubV3(work[i], ctrl[i]));
            if (d > rep->dev_max) {
                rep->dev_max = d;
            }
            rep->dev_mean += d;
        }
        rep->dev_mean /= (float)n;
    } else {
        /* Distance from each hand-placed point to the refined polyline. */
        for (i = 0; i < n; ++i) {
            float best = 1e30f;
            int j;
            for (j = 0; j < work_n; ++j) {
                float d = HMM_LenV3(HMM_SubV3(work[j], ctrl[i]));
                if (d < best) {
                    best = d;
                }
            }
            if (best > rep->dev_max) {
                rep->dev_max = best;
            }
            rep->dev_mean += best;
        }
        rep->dev_mean /= (float)n;
    }

    /* Stage 3: the delivered, uniformly resampled path. */
    if (cfg->allow_move_original) {
        out_count = sample_spline(work, work_n, closed, cfg->samples_per_segment,
                                  output, MAX_POLY);
    } else {
        out_count = resample_polyline_arclength(work, work_n, closed, out_want, output);
    }
    rep->kappa_after = polyline_max_curvature(output, out_count, closed);
    /* The bound is judged on the delivered output, not the estimate. */
    rep->satisfied = rep->satisfied && (rep->kappa_after <= cfg->kappa_max * 1.001f);

    rep->elapsed_ms = now_ms() - t0;

    free(work);
    free(output);
}

/* ------------------------------------------------------------------ */
/* Sample paths + report                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
    /* 1. Straight line: nothing to do, must stay untouched. */
    HMM_Vec3 straight[] = {
        HMM_V3(0.0f, 0.0f, 0.0f), HMM_V3(1.0f, 0.0f, 0.0f),
        HMM_V3(2.0f, 0.0f, 0.0f), HMM_V3(3.0f, 0.0f, 0.0f),
    };
    /* 2. S-curve: gentle lateral swings (with a slight 3D wobble). */
    HMM_Vec3 s_curve[] = {
        HMM_V3(0.0f, 0.0f, 0.0f),  HMM_V3(1.0f, 0.8f, 0.2f),
        HMM_V3(2.0f, 0.0f, 0.0f),  HMM_V3(3.0f, -0.8f, -0.2f),
        HMM_V3(4.0f, 0.0f, 0.0f),
    };
    /* 3. Sharp 90-degree turn. */
    HMM_Vec3 sharp[] = {
        HMM_V3(0.0f, 0.0f, 0.0f), HMM_V3(2.0f, 0.0f, 0.0f),
        HMM_V3(2.0f, 1.0f, 0.0f), HMM_V3(4.0f, 1.0f, 0.0f),
    };
    /* 4. Closed loop: rounded-square camera orbit. */
    HMM_Vec3 loop[] = {
        HMM_V3(2.0f, 0.0f, 0.0f),  HMM_V3(1.0f, 1.0f, 0.0f),
        HMM_V3(0.0f, 1.0f, 0.0f),  HMM_V3(-1.0f, 1.0f, 0.0f),
        HMM_V3(-2.0f, 0.0f, 0.0f), HMM_V3(-1.0f, -1.0f, 0.0f),
        HMM_V3(0.0f, -1.0f, 0.0f), HMM_V3(1.0f, -1.0f, 0.0f),
    };

    SmoothCfg cfgs[6];
    Report reports[6];
    int n_reports = 0;
    int failures = 0;
    int i;

    /*            kappa_max  N  move  iters  relax */
    cfgs[0].kappa_max = 0.50f; cfgs[0].samples_per_segment = 8;
    cfgs[0].allow_move_original = 1; cfgs[0].max_iterations = 1000; cfgs[0].relax = 0.5f;
    cfgs[1].kappa_max = 1.20f; cfgs[1].samples_per_segment = 16;
    cfgs[1].allow_move_original = 1; cfgs[1].max_iterations = 1000; cfgs[1].relax = 0.5f;
    cfgs[2].kappa_max = 1.00f; cfgs[2].samples_per_segment = 24;
    cfgs[2].allow_move_original = 1; cfgs[2].max_iterations = 1000; cfgs[2].relax = 0.5f;
    cfgs[3].kappa_max = 1.00f; cfgs[3].samples_per_segment = 24;
    cfgs[3].allow_move_original = 0; cfgs[3].max_iterations = 3000; cfgs[3].relax = 0.5f;
    cfgs[4].kappa_max = 1.00f; cfgs[4].samples_per_segment = 32;
    cfgs[4].allow_move_original = 1; cfgs[4].max_iterations = 1000; cfgs[4].relax = 0.5f;
    /* Unsatisfiable on purpose: a closed curve of this perimeter cannot
     * stay under kappa 0.30 (total curvature of any closed curve >= 2 pi),
     * and the original points are pinned.  Must report the closest result. */
    cfgs[5].kappa_max = 0.30f; cfgs[5].samples_per_segment = 32;
    cfgs[5].allow_move_original = 0; cfgs[5].max_iterations = 3000; cfgs[5].relax = 0.5f;

    run_scenario("straight",         straight, 4, 0, &cfgs[0], &reports[n_reports++]);
    run_scenario("s-curve",          s_curve,  5, 0, &cfgs[1], &reports[n_reports++]);
    run_scenario("sharp-turn/move",  sharp,    4, 0, &cfgs[2], &reports[n_reports++]);
    run_scenario("sharp-turn/fix",   sharp,    4, 0, &cfgs[3], &reports[n_reports++]);
    run_scenario("closed-loop/move", loop,     8, 1, &cfgs[4], &reports[n_reports++]);
    run_scenario("closed-loop/impossible", loop, 8, 1, &cfgs[5], &reports[n_reports++]);

    printf("%-24s | %-14s | %-9s | %-17s | %-14s | %-9s | %s\n",
           "path", "cfg kmax/mv/N", "segments", "max curvature", "ctrl-pt dev", "time", "status");
    printf("-------------------------+----------------+-----------+-------------------+----------------+-----------+----------\n");
    for (i = 0; i < n_reports; ++i) {
        const Report *r = &reports[i];
        printf("%-24s | %5.2f/%s/%-2d | %3d -> %-4d | %7.4f -> %-7.4f | %5.3f / %-5.3f | %7.2f ms | %s\n",
               r->name, (double)cfgs[i].kappa_max, cfgs[i].allow_move_original ? "y" : "n",
               cfgs[i].samples_per_segment,
               r->seg_before, r->seg_after,
               (double)r->kappa_before, (double)r->kappa_after,
               (double)r->dev_max, (double)r->dev_mean,
               r->elapsed_ms,
               r->satisfied ? "OK" : "CLOSEST (bound unreachable)");
    }
    printf("(dev = control-point displacement for mv=y, distance from the\n");
    printf(" original points to the delivered path for mv=n; max / mean)\n");

    /* Self-checks: the run doubles as a test. */
    for (i = 0; i < n_reports; ++i) {
        const Report *r = &reports[i];
        if (r->satisfied && r->kappa_after > cfgs[i].kappa_max * 1.001f) {
            printf("FAIL: %s reports satisfied but kappa %.4f > bound %.4f\n",
                   r->name, (double)r->kappa_after, (double)cfgs[i].kappa_max);
            failures++;
        }
        if (r->kappa_after > r->kappa_before * 1.001f + 1e-6f) {
            printf("FAIL: %s ended worse than it started (%.4f -> %.4f)\n",
                   r->name, (double)r->kappa_before, (double)r->kappa_after);
            failures++;
        }
    }
    if (reports[0].kappa_after > 1e-3f || reports[0].dev_max > 1e-6f ||
        reports[0].seg_after != reports[0].seg_before) {
        printf("FAIL: straight path was altered (kappa %.6f, dev %.6f, segs %d -> %d)\n",
               (double)reports[0].kappa_after, (double)reports[0].dev_max,
               reports[0].seg_before, reports[0].seg_after);
        failures++;
    }
    if (!reports[3].satisfied || reports[3].seg_after <= reports[3].seg_before) {
        printf("FAIL: fixed-control-point variant should satisfy the bound via inserted points\n");
        failures++;
    }
    if (reports[5].satisfied || reports[5].kappa_after >= reports[5].kappa_before) {
        printf("FAIL: impossible bound must report an improved closest result\n");
        failures++;
    }

    printf("SELF-CHECK: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
