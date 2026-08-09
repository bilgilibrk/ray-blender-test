#include <math.h>
#include <stdio.h>
#include <string.h>

#include "raylib.h"
#include "raymath.h"

#include "engine/terrain.h"

#include "tests.h"

// A waypoint ring, tilted so the loop climbs and descends. Height queries are
// only interesting on a track that is not flat.
static bool MakeSlopedRing(Level *level, int count, float radius, float rise)
{
    memset(level, 0, sizeof(*level));
    if (!ArenaInit(&level->arena, sizeof(LevelWaypoint) * (size_t)count + 256, "test")) {
        return false;
    }
    level->waypoints = ArenaAlloc(&level->arena, sizeof(LevelWaypoint) * (size_t)count);
    level->waypointCount = count;
    level->defaultTrackWidth = 1.0f;
    level->laps = 1;
    for (int i = 0; i < count; i++) {
        float a = 2.0f * PI * (float)i / (float)count;
        level->waypoints[i].position =
            (Vector3){ cosf(a) * radius, sinf(a) * rise, sinf(a) * radius };
        level->waypoints[i].width = 1.0f;
    }
    return true;
}

// The definition the shipped query has to keep matching: inverse-distance
// weighting against every sample, summed straight down the list.
static float ReferenceHeight(const Spline *spline, float x, float z)
{
    double weighted = 0.0, total = 0.0;
    for (int i = 0; i < spline->count; i++) {
        Vector3 p = spline->samples[i].position;
        double dx = (double)x - p.x;
        double dz = (double)z - p.z;
        double d2 = dx * dx + dz * dz;
        double w = 1.0 / (d2 * d2 + 0.45);
        weighted += p.y * w;
        total += w;
    }
    return (total > 0.0) ? (float)(weighted / total) : 0.0f;
}

void RunTerrainTests(void)
{
    Level level;
    CHECK(MakeSlopedRing(&level, 24, 10.0f, 2.0f), "sloped ring level allocated");

    Spline spline;
    CHECK(SplineBuild(&spline, &level, 0.25f), "spline builds from the sloped ring");

    HeightField field;
    CHECK(HeightFieldBuild(&field, &spline), "height field builds");
    CHECK(field.count == spline.count, "height field kept %d of %d samples",
          field.count, spline.count);

    // --- agrees with the definition ----------------------------------------
    // The shipped query splits the sum into four chains, which reorders the
    // additions. That must stay a rounding-level difference and nothing more.
    float worst = 0.0f;
    float worstAt[2] = { 0.0f, 0.0f };
    for (int iz = -30; iz <= 30; iz++) {
        for (int ix = -30; ix <= 30; ix++) {
            float x = (float)ix * 0.5f;
            float z = (float)iz * 0.5f;
            float got = HeightFieldAt(&field, x, z);
            float want = ReferenceHeight(&spline, x, z);
            float error = fabsf(got - want);
            if (error > worst) { worst = error; worstAt[0] = x; worstAt[1] = z; }
        }
    }
    CHECK(worst < 1e-4f, "height field drifts from the reference by %.2e at (%.1f, %.1f)",
          (double)worst, (double)worstAt[0], (double)worstAt[1]);

    // --- hugs the road -------------------------------------------------------
    // On the racing line itself the ground should sit within a few centimetres
    // of the tarmac, or the road visibly floats.
    float worstOnTrack = 0.0f;
    for (int i = 0; i < spline.count; i += 7) {
        Vector3 p = spline.samples[i].position;
        float error = fabsf(HeightFieldAt(&field, p.x, p.z) - p.y);
        if (error > worstOnTrack) worstOnTrack = error;
    }
    CHECK(worstOnTrack < 0.15f, "ground leaves the road by up to %.3f units",
          (double)worstOnTrack);

    // --- relaxes away from the road -------------------------------------------
    // Far from everything the field must settle towards the mean height of the
    // loop rather than running away with the nearest sample.
    float centre = HeightFieldAt(&field, 0.0f, 0.0f);
    CHECK(fabsf(centre) < 0.35f, "middle of the ring sat at %.3f, expected near 0",
          (double)centre);

    // --- no creases -----------------------------------------------------------
    // Neighbouring grid nodes must not jump: a step here shows up as a visible
    // seam once the ground is shaded.
    float worstStep = 0.0f;
    for (int iz = -24; iz <= 24; iz++) {
        for (int ix = -24; ix < 24; ix++) {
            float z = (float)iz * 0.4f;
            float a = HeightFieldAt(&field, (float)ix * 0.4f, z);
            float b = HeightFieldAt(&field, (float)(ix + 1) * 0.4f, z);
            float step = fabsf(b - a);
            if (step > worstStep) worstStep = step;
        }
    }
    CHECK(worstStep < 0.5f, "adjacent ground samples differ by up to %.3f units",
          (double)worstStep);

    // A query landing exactly on a sample must not divide by zero.
    Vector3 onSample = spline.samples[0].position;
    float here = HeightFieldAt(&field, onSample.x, onSample.z);
    CHECK(isfinite(here), "height on top of a sample was not finite");

    HeightFieldFree(&field);
    CHECK(field.count == 0 && field.storage == NULL, "freeing clears the height field");

    SplineFree(&spline);
    LevelUnload(&level);
}
