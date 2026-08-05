#include <math.h>
#include <stdio.h>
#include <string.h>

#include "raylib.h"
#include "raymath.h"

#include "engine/spline.h"

#include "tests.h"

// Builds a level holding just a waypoint ring of the given radius.
static bool MakeRingLevel(Level *level, int count, float radius, float width)
{
    memset(level, 0, sizeof(*level));
    if (!ArenaInit(&level->arena, sizeof(LevelWaypoint) * (size_t)count + 256, "test")) return false;
    level->waypoints = ArenaAlloc(&level->arena, sizeof(LevelWaypoint) * (size_t)count);
    level->waypointCount = count;
    level->defaultTrackWidth = width;
    level->laps = 3;
    for (int i = 0; i < count; i++) {
        float a = 2.0f * PI * (float)i / (float)count;
        level->waypoints[i].position = (Vector3){ cosf(a) * radius, 0.0f, sinf(a) * radius };
        level->waypoints[i].width = width;
    }
    return true;
}

void RunSplineTests(void)
{
    Level level;
    CHECK(MakeRingLevel(&level, 32, 10.0f, 2.0f), "ring level allocated");

    Spline spline;
    CHECK(SplineBuild(&spline, &level, 0.5f), "spline builds from a ring");

    // A smoothed 32-gon of radius 10 should measure close to a circle.
    float circumference = 2.0f * PI * 10.0f;
    CHECK(fabsf(spline.length - circumference) < circumference * 0.02f,
          "loop length %.2f, expected about %.2f", (double)spline.length, (double)circumference);

    // Every sample should sit on the ring.
    float worst = 0.0f;
    for (int i = 0; i < spline.count; i++) {
        Vector3 p = spline.samples[i].position;
        float r = sqrtf(p.x * p.x + p.z * p.z);
        float error = fabsf(r - 10.0f);
        if (error > worst) worst = error;
    }
    CHECK(worst < 0.05f, "samples deviate from the ring by up to %.3f", (double)worst);

    // Tangents must be unit length and perpendicular to the radius.
    float worstDot = 0.0f;
    for (int i = 0; i < spline.count; i++) {
        Vector3 p = spline.samples[i].position;
        Vector3 t = spline.samples[i].tangent;
        CHECK(fabsf(Vector3Length(t) - 1.0f) < 0.001f, "tangent %d is not unit length", i);
        float radial = (p.x * t.x + p.z * t.z) / 10.0f;
        if (fabsf(radial) > worstDot) worstDot = fabsf(radial);
    }
    CHECK(worstDot < 0.05f, "tangents lean off the circle by up to %.3f", (double)worstDot);

    // --- closest-point queries -------------------------------------------
    // A point outside the ring projects back onto it at the same bearing.
    int hint = -1;
    SplineQuery q = SplineClosest(&spline, (Vector3){ 12.0f, 0.0f, 0.0f }, &hint);
    CHECK(fabsf(q.position.x - 10.0f) < 0.05f && fabsf(q.position.z) < 0.05f,
          "closest point to (12,0) was (%.2f, %.2f)", (double)q.position.x, (double)q.position.z);
    CHECK(fabsf(fabsf(q.lateral) - 2.0f) < 0.05f,
          "lateral offset %.2f, expected 2", (double)fabsf(q.lateral));
    CHECK(fabsf(q.halfWidth - 1.0f) < 0.01f, "half width %.2f, expected 1", (double)q.halfWidth);

    // The sign of `lateral` must distinguish inside from outside.
    int hintIn = -1;
    SplineQuery inner = SplineClosest(&spline, (Vector3){ 8.0f, 0.0f, 0.0f }, &hintIn);
    CHECK((inner.lateral > 0.0f) != (q.lateral > 0.0f),
          "inside and outside give the same lateral sign (%.2f vs %.2f)",
          (double)inner.lateral, (double)q.lateral);

    // A stale hint from the far side of the loop must still find the right spot.
    int staleHint = spline.count / 2;
    SplineQuery recovered = SplineClosest(&spline, (Vector3){ 12.0f, 0.0f, 0.0f }, &staleHint);
    CHECK(fabsf(recovered.position.x - 10.0f) < 0.05f,
          "a stale hint broke the query: got x=%.2f", (double)recovered.position.x);

    // --- arc-length sampling ------------------------------------------------
    Vector3 quarter = SplinePointAt(&spline, spline.length * 0.25f);
    Vector3 start = SplinePointAt(&spline, 0.0f);
    CHECK(fabsf(Vector3Distance(quarter, start) - 10.0f * sqrtf(2.0f)) < 0.2f,
          "quarter-lap chord was %.2f, expected %.2f",
          (double)Vector3Distance(quarter, start), (double)(10.0f * sqrtf(2.0f)));

    // Sampling past the end wraps around rather than clamping.
    Vector3 wrapped = SplinePointAt(&spline, spline.length * 1.25f);
    CHECK(Vector3Distance(wrapped, quarter) < 0.05f, "arc length did not wrap");

    // --- wrap-around differences -------------------------------------------
    float small = SplineWrapDelta(&spline, 1.0f, spline.length - 1.0f);
    CHECK(fabsf(small - 2.0f) < 0.001f,
          "wrapped delta across the seam was %.3f, expected 2", (double)small);
    float backwards = SplineWrapDelta(&spline, spline.length - 1.0f, 1.0f);
    CHECK(fabsf(backwards + 2.0f) < 0.001f,
          "reverse wrapped delta was %.3f, expected -2", (double)backwards);

    SplineFree(&spline);
    LevelUnload(&level);

    // Too few waypoints must fail rather than produce a degenerate spline.
    // The rejection logs an error, which is the expected result here.
    Level tiny;
    CHECK(MakeRingLevel(&tiny, 2, 5.0f, 1.0f), "two-point level allocated");
    Spline bad;
    int previousLevel = g_verbose ? LOG_INFO : LOG_WARNING;
    SetTraceLogLevel(LOG_NONE);
    bool built = SplineBuild(&bad, &tiny, 0.5f);
    SetTraceLogLevel(previousLevel);
    CHECK(!built, "spline rejects a 2-waypoint loop");
    LevelUnload(&tiny);
}
