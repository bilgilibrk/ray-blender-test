#include <math.h>
#include <stdio.h>
#include <string.h>

#include "raylib.h"

#include "engine/level.h"
#include "engine/spline.h"

#include "tests.h"

#define SCRATCH_LEVEL "build/test-scratch.level.json"

// A minimal but complete circuit, with one field left open so each case can
// drop something interesting into it.
static bool WriteLevel(const char *waypointX)
{
    FILE *f = fopen(SCRATCH_LEVEL, "w");
    if (!f) return false;
    fprintf(f,
        "{\n"
        "  \"format\": \"kenney-topdown-racer\",\n"
        "  \"name\": \"scratch\",\n"
        "  \"settings\": { \"laps\": 1, \"track_width\": 0.7 },\n"
        "  \"spawns\": [ { \"pos\": [0, 0, 0], \"yaw\": 0 } ],\n"
        "  \"waypoints\": [\n"
        "    { \"pos\": [%s, 0, 0] },\n"
        "    { \"pos\": [0, 0, 10] },\n"
        "    { \"pos\": [-10, 0, 0] },\n"
        "    { \"pos\": [0, 0, -10] }\n"
        "  ]\n"
        "}\n", waypointX);
    fclose(f);
    return true;
}

static bool LoadScratch(void)
{
    Level level;
    bool ok = LevelLoad(&level, SCRATCH_LEVEL);
    if (ok) LevelUnload(&level);
    return ok;
}

void RunLevelTests(void)
{
    int previousLevel = g_verbose ? LOG_INFO : LOG_WARNING;

    // The control: an ordinary coordinate loads.
    CHECK(WriteLevel("10"), "scratch level written");
    CHECK(LoadScratch(), "a well-formed level loads");

    // 1e999 is well-formed JSON but cannot be a double. The parser stops it.
    CHECK(WriteLevel("1e999"), "scratch level written");
    SetTraceLogLevel(LOG_NONE);
    bool tookInfinity = LoadScratch();
    SetTraceLogLevel(previousLevel);
    CHECK(!tookInfinity, "a level with 1e999 in it is rejected");

    // 1e300 is a valid double and survives the parser, but these structs hold
    // floats and it is an infinity in one. Left alone it reaches the spline as
    // a NaN lap length and takes the terrain build down with it, so the level
    // reader has to catch what the parser cannot.
    CHECK(WriteLevel("1e300"), "scratch level written");
    SetTraceLogLevel(LOG_NONE);
    bool tookOverflow = LoadScratch();
    SetTraceLogLevel(previousLevel);
    CHECK(!tookOverflow, "a level whose coordinate overflows a float is rejected");

    // The value that used to get through, all the way to the crash: prove the
    // pipeline behind the loader stays sane on what it does accept.
    CHECK(WriteLevel("10"), "scratch level written");
    Level level;
    if (LevelLoad(&level, SCRATCH_LEVEL)) {
        Spline spline;
        CHECK(SplineBuild(&spline, &level, 0.25f), "spline builds from the scratch level");
        CHECK(isfinite(spline.length) && spline.length > 0.0f,
              "lap length was %.3f", (double)spline.length);
        int broken = 0;
        for (int i = 0; i < spline.count; i++) {
            Vector3 p = spline.samples[i].position;
            if (!isfinite(p.x) || !isfinite(p.y) || !isfinite(p.z)) broken++;
        }
        CHECK(broken == 0, "%d of %d spline samples were not finite", broken, spline.count);
        SplineFree(&spline);
        LevelUnload(&level);
    } else {
        CHECK(false, "the control level failed to load");
    }

    remove(SCRATCH_LEVEL);
}
