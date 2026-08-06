#include "engine/spline.h"

#include <math.h>
#include <string.h>

#include "raymath.h"

// How far either side of the hint a windowed nearest-sample search looks.
#define SPLINE_SEARCH_WINDOW 24

// Centripetal Catmull-Rom, evaluated with Barry-Goldman.
//
// The uniform form assumes the control points are evenly spaced. A track's are
// not — a hand-drawn racing line, or an exporter that thins arcs harder than
// straights, leaves spans of very different lengths — and where the spacing
// jumps the uniform curve overshoots. That shows up as a phantom hairpin in the
// curvature, which the AI dutifully brakes for. Knot spacing of sqrt(distance)
// removes it, and also guarantees no cusps or self-intersections within a span.
static Vector3 CatmullRom(Vector3 p0, Vector3 p1, Vector3 p2, Vector3 p3, float t)
{
    const float alpha = 0.5f;   // centripetal
    float t0 = 0.0f;
    float t1 = t0 + powf(fmaxf(Vector3Distance(p0, p1), 1e-5f), alpha);
    float t2 = t1 + powf(fmaxf(Vector3Distance(p1, p2), 1e-5f), alpha);
    float t3 = t2 + powf(fmaxf(Vector3Distance(p2, p3), 1e-5f), alpha);

    float tt = t1 + (t2 - t1) * t;   // t arrives normalised across the middle span

    Vector3 a1 = Vector3Lerp(p0, p1, (tt - t0) / (t1 - t0));
    Vector3 a2 = Vector3Lerp(p1, p2, (tt - t1) / (t2 - t1));
    Vector3 a3 = Vector3Lerp(p2, p3, (tt - t2) / (t3 - t2));
    Vector3 b1 = Vector3Lerp(a1, a2, (tt - t0) / (t2 - t0));
    Vector3 b2 = Vector3Lerp(a2, a3, (tt - t1) / (t3 - t1));
    return Vector3Lerp(b1, b2, (tt - t1) / (t2 - t1));
}

static float LerpF(float a, float b, float t) { return a + (b - a) * t; }

bool SplineBuild(Spline *spline, const Level *level, float spacing)
{
    memset(spline, 0, sizeof(*spline));
    int n = level->waypointCount;
    if (n < 3) {
        TraceLog(LOG_ERROR, "SPLINE: level '%s' has %d waypoints, need at least 3", level->name, n);
        return false;
    }
    if (spacing <= 0.0001f) spacing = 0.25f;

    // Pass 1: subdivide each waypoint span enough that segments stay near
    // `spacing`, and measure the resulting arc length.
    int *subdiv = (int *)MemAlloc((unsigned int)(sizeof(int) * (size_t)n));
    if (!subdiv) return false;

    int total = 0;
    for (int i = 0; i < n; i++) {
        Vector3 a = level->waypoints[i].position;
        Vector3 b = level->waypoints[(i + 1) % n].position;
        float chord = Vector3Distance(a, b);
        int steps = (int)ceilf(chord / spacing);
        if (steps < 1) steps = 1;
        if (steps > 512) steps = 512;
        subdiv[i] = steps;
        total += steps;
    }

    if (!ArenaInit(&spline->arena, sizeof(SplineSample) * (size_t)(total + 1) + 256, "spline")) {
        MemFree(subdiv);
        return false;
    }
    spline->samples = ArenaAlloc(&spline->arena, sizeof(SplineSample) * (size_t)total);
    spline->count = total;

    // Pass 2: evaluate the smoothed curve.
    int out = 0;
    for (int i = 0; i < n; i++) {
        Vector3 p0 = level->waypoints[(i - 1 + n) % n].position;
        Vector3 p1 = level->waypoints[i].position;
        Vector3 p2 = level->waypoints[(i + 1) % n].position;
        Vector3 p3 = level->waypoints[(i + 2) % n].position;
        float w1 = level->waypoints[i].width;
        float w2 = level->waypoints[(i + 1) % n].width;

        for (int s = 0; s < subdiv[i]; s++) {
            float t = (float)s / (float)subdiv[i];
            spline->samples[out].position = CatmullRom(p0, p1, p2, p3, t);
            spline->samples[out].width = LerpF(w1, w2, t);
            out++;
        }
    }
    MemFree(subdiv);

    // Pass 3: arc lengths, tangents and grades, all on the closed loop.
    // Distances are measured on the ground plane so that lap progress, AI
    // look-ahead and gate spacing do not stretch on a climb.
    float acc = 0.0f;
    for (int i = 0; i < total; i++) {
        spline->samples[i].distance = acc;
        Vector3 a = spline->samples[i].position;
        Vector3 b = spline->samples[(i + 1) % total].position;
        acc += sqrtf((b.x - a.x) * (b.x - a.x) + (b.z - a.z) * (b.z - a.z));
    }
    spline->length = acc;

    for (int i = 0; i < total; i++) {
        Vector3 prev = spline->samples[(i - 1 + total) % total].position;
        Vector3 next = spline->samples[(i + 1) % total].position;
        Vector3 dir = Vector3Subtract(next, prev);

        float rise = dir.y;
        dir.y = 0.0f;
        float run = Vector3Length(dir);
        spline->samples[i].tangent = (run > 1e-6f) ? Vector3Scale(dir, 1.0f / run)
                                                   : (Vector3){ 0, 0, 1 };
        spline->samples[i].grade = (run > 1e-6f) ? rise / run : 0.0f;
    }

    TraceLog(LOG_INFO, "SPLINE: %d samples, loop length %.2f units", total, spline->length);
    return true;
}

void SplineFree(Spline *spline)
{
    ArenaFree(&spline->arena);
    memset(spline, 0, sizeof(*spline));
}

// Projects `point` onto the segment starting at sample `i`, returning the
// parameter along it clamped to [0,1] and the squared distance.
static float ProjectOnSegment(const Spline *s, int i, Vector3 point, float *outSqDist)
{
    int j = (i + 1) % s->count;
    Vector3 a = s->samples[i].position;
    Vector3 b = s->samples[j].position;
    Vector3 ab = Vector3Subtract(b, a);
    ab.y = 0.0f;
    Vector3 ap = Vector3Subtract(point, a);
    ap.y = 0.0f;

    float denom = ab.x * ab.x + ab.z * ab.z;
    float t = (denom > 1e-9f) ? (ap.x * ab.x + ap.z * ab.z) / denom : 0.0f;
    t = Clamp(t, 0.0f, 1.0f);

    float dx = ap.x - ab.x * t;
    float dz = ap.z - ab.z * t;
    *outSqDist = dx * dx + dz * dz;
    return t;
}

SplineQuery SplineClosest(const Spline *spline, Vector3 point, int *hintIndex)
{
    SplineQuery q = { 0 };
    if (spline->count < 2) return q;

    int best = 0;
    float bestT = 0.0f;
    float bestSq = 1e30f;

    int hint = (hintIndex && *hintIndex >= 0) ? *hintIndex % spline->count : -1;

    // Fine sweep around the caller's last answer, which is where a car that
    // moved normally will be.
    if (hint >= 0) {
        for (int k = -SPLINE_SEARCH_WINDOW; k <= SPLINE_SEARCH_WINDOW; k++) {
            int i = ((hint + k) % spline->count + spline->count) % spline->count;
            float sq;
            float t = ProjectOnSegment(spline, i, point, &sq);
            if (sq < bestSq) { bestSq = sq; best = i; bestT = t; }
        }
    }

    // Coarse sweep of the whole loop. Without this a stale hint — after a
    // respawn, or on a circuit whose two halves run close together — can pin
    // the query to the wrong side of the track, and no distance threshold
    // reliably detects that. The stride keeps it cheap enough to run always.
    int stride = spline->count / 64;
    if (stride < 1) stride = 1;

    int coarseBest = -1;
    float coarseSq = bestSq;
    for (int i = 0; i < spline->count; i += stride) {
        float sq;
        float t = ProjectOnSegment(spline, i, point, &sq);
        if (sq < coarseSq) { coarseSq = sq; coarseBest = i; bestT = t; }
    }

    // Refine around the coarse winner when it beats the windowed result.
    if (coarseBest >= 0) {
        bestSq = coarseSq;
        best = coarseBest;
        for (int k = -stride; k <= stride; k++) {
            int i = ((coarseBest + k) % spline->count + spline->count) % spline->count;
            float sq;
            float t = ProjectOnSegment(spline, i, point, &sq);
            if (sq < bestSq) { bestSq = sq; best = i; bestT = t; }
        }
    }

    int next = (best + 1) % spline->count;
    Vector3 a = spline->samples[best].position;
    Vector3 b = spline->samples[next].position;

    q.index = best;
    q.position = Vector3Lerp(a, b, bestT);      // carries the surface height
    q.tangent = spline->samples[best].tangent;
    q.halfWidth = LerpF(spline->samples[best].width, spline->samples[next].width, bestT) * 0.5f;
    q.grade = LerpF(spline->samples[best].grade, spline->samples[next].grade, bestT);

    // Horizontal, to match how sample distances were accumulated.
    float segLen = sqrtf((b.x - a.x) * (b.x - a.x) + (b.z - a.z) * (b.z - a.z));
    q.distance = spline->samples[best].distance + segLen * bestT;
    if (q.distance >= spline->length) q.distance -= spline->length;

    // Sign the lateral offset using the track's left vector (tangent x up).
    Vector3 left = { -q.tangent.z, 0.0f, q.tangent.x };
    Vector3 rel = Vector3Subtract(point, q.position);
    q.lateral = rel.x * left.x + rel.z * left.z;

    if (hintIndex) *hintIndex = best;
    return q;
}

SplineSample SplineSampleAt(const Spline *spline, float distance)
{
    SplineSample out = { 0 };
    if (spline->count < 2 || spline->length <= 0.0f) return out;

    distance = fmodf(distance, spline->length);
    if (distance < 0.0f) distance += spline->length;

    // Samples are near-uniform, so start from the proportional guess and walk.
    int i = (int)((distance / spline->length) * (float)spline->count) % spline->count;
    for (int guard = 0; guard < spline->count; guard++) {
        float d0 = spline->samples[i].distance;
        int j = (i + 1) % spline->count;
        float d1 = (j == 0) ? spline->length : spline->samples[j].distance;
        if (distance < d0) { i = (i - 1 + spline->count) % spline->count; continue; }
        if (distance > d1) { i = j; continue; }

        float t = (d1 > d0) ? (distance - d0) / (d1 - d0) : 0.0f;
        out.position = Vector3Lerp(spline->samples[i].position, spline->samples[j].position, t);
        out.tangent = spline->samples[i].tangent;
        out.width = LerpF(spline->samples[i].width, spline->samples[j].width, t);
        out.grade = LerpF(spline->samples[i].grade, spline->samples[j].grade, t);
        out.distance = distance;
        return out;
    }
    return spline->samples[0];
}

Vector3 SplinePointAt(const Spline *spline, float distance)
{
    return SplineSampleAt(spline, distance).position;
}

float SplineWrapDelta(const Spline *spline, float a, float b)
{
    float half = spline->length * 0.5f;
    float d = fmodf(a - b, spline->length);
    if (d > half) d -= spline->length;
    if (d < -half) d += spline->length;
    return d;
}
