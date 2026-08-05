#include <math.h>
#include <stdio.h>
#include <string.h>

#include "raylib.h"

#include "raymath.h"

#include "engine/light.h"

#include "tests.h"

void RunLightTests(void)
{
    LightSet set;
    LightSetClear(&set);
    CHECK(set.count == 0, "a cleared set is empty");

    // --- construction -------------------------------------------------------
    Light point = LightMakePoint((Vector3){ 1, 2, 3 }, RED, 2.0f, 5.0f);
    CHECK(point.type == LIGHT_POINT, "point light type");
    CHECK(point.range == 5.0f, "point light range");

    Light spot = LightMakeSpot((Vector3){ 0, 1, 0 }, (Vector3){ 0, -4, 0 }, WHITE,
                               1.0f, 8.0f, 20.0f, 35.0f);
    CHECK(spot.type == LIGHT_SPOT, "spot light type");
    CHECK(fabsf(spot.direction.y + 1.0f) < 1e-5f, "spot direction is normalised");

    // An inner cone wider than the outer would divide by zero in the shader.
    Light pinched = LightMakeSpot((Vector3){ 0 }, (Vector3){ 0, -1, 0 }, WHITE,
                                  1.0f, 4.0f, 60.0f, 30.0f);
    CHECK(pinched.innerConeDeg < pinched.outerConeDeg,
          "inner cone %.1f was not clamped below outer %.1f",
          (double)pinched.innerConeDeg, (double)pinched.outerConeDeg);

    // A zero range would make the shader's attenuation divide by zero.
    Light degenerate = LightMakePoint((Vector3){ 0 }, WHITE, 1.0f, 0.0f);
    CHECK(degenerate.range > 0.0f, "zero range was clamped");

    // --- influence ------------------------------------------------------------
    Light lamp = LightMakePoint((Vector3){ 0, 0, 0 }, WHITE, 1.0f, 10.0f);
    CHECK(LightInfluence(&lamp, (Vector3){ 0, 0, 0 }, 0.0f) > 0.99f, "full at the centre");
    CHECK(LightInfluence(&lamp, (Vector3){ 20, 0, 0 }, 0.0f) == 0.0f, "zero beyond the range");
    CHECK(LightInfluence(&lamp, (Vector3){ 10, 0, 0 }, 0.0f) == 0.0f, "zero exactly at the range");

    float near = LightInfluence(&lamp, (Vector3){ 2, 0, 0 }, 0.0f);
    float far = LightInfluence(&lamp, (Vector3){ 7, 0, 0 }, 0.0f);
    CHECK(near > far, "influence falls off with distance (%.3f vs %.3f)",
          (double)near, (double)far);

    // A large object reaches closer to the light than its centre suggests.
    CHECK(LightInfluence(&lamp, (Vector3){ 12, 0, 0 }, 4.0f) > 0.0f,
          "a big bounding sphere should still be reached");

    lamp.enabled = false;
    CHECK(LightInfluence(&lamp, (Vector3){ 0, 0, 0 }, 0.0f) == 0.0f, "disabled lights are dark");

    // --- selection ---------------------------------------------------------------
    LightSetClear(&set);
    // Ten lights in a row, brightest nearest the origin.
    for (int i = 0; i < 10; i++) {
        LightSetAdd(&set, LightMakePoint((Vector3){ (float)i, 0, 0 }, WHITE, 1.0f, 100.0f));
    }
    CHECK(set.count == 10, "ten lights added");

    int chosen[LIGHTS_PER_DRAW];
    int found = LightSetSelect(&set, (Vector3){ 0, 0, 0 }, 0.0f, chosen, LIGHTS_PER_DRAW);
    CHECK(found == LIGHTS_PER_DRAW, "selection is capped at %d, got %d", LIGHTS_PER_DRAW, found);
    CHECK(chosen[0] == 0, "nearest light should be first, got %d", chosen[0]);

    // Results must be ordered strongest first, and free of duplicates.
    bool ordered = true, unique = true;
    for (int i = 1; i < found; i++) {
        float a = LightInfluence(&set.lights[chosen[i - 1]], (Vector3){ 0, 0, 0 }, 0.0f);
        float b = LightInfluence(&set.lights[chosen[i]], (Vector3){ 0, 0, 0 }, 0.0f);
        if (b > a) ordered = false;
        for (int j = 0; j < i; j++) {
            if (chosen[j] == chosen[i]) unique = false;
        }
    }
    CHECK(ordered, "selection is not sorted strongest first");
    CHECK(unique, "selection returned the same light twice");

    // Nothing in range means nothing selected.
    LightSetClear(&set);
    LightSetAdd(&set, LightMakePoint((Vector3){ 0, 0, 0 }, WHITE, 1.0f, 2.0f));
    CHECK(LightSetSelect(&set, (Vector3){ 50, 0, 0 }, 0.0f, chosen, LIGHTS_PER_DRAW) == 0,
          "distant query selects nothing");

    // --- capacity -----------------------------------------------------------------
    LightSetClear(&set);
    SetTraceLogLevel(LOG_NONE);     // the overflow warning is expected
    int accepted = 0;
    for (int i = 0; i < LIGHTS_MAX + 16; i++) {
        if (LightSetAdd(&set, LightMakePoint((Vector3){ 0 }, WHITE, 1.0f, 1.0f)) >= 0) accepted++;
    }
    SetTraceLogLevel(g_verbose ? LOG_INFO : LOG_WARNING);
    CHECK(accepted == LIGHTS_MAX, "accepted %d lights, capacity is %d", accepted, LIGHTS_MAX);
    CHECK(set.count == LIGHTS_MAX, "set overflowed to %d", set.count);
    CHECK(LightSetAt(&set, LIGHTS_MAX) == NULL, "out-of-range index returns NULL");
    CHECK(LightSetAt(&set, -1) == NULL, "negative index returns NULL");

    // --- lights carried by the demo level ---------------------------------------
    Level level;
    if (LevelLoad(&level, "levels/circuit01.level.json")) {
        CHECK(level.lightCount > 0, "the demo level should ship lights, found %d",
              level.lightCount);

        LightSetClear(&set);
        int added = LightSetAddFromLevel(&set, &level);
        CHECK(added == level.lightCount, "added %d of %d level lights", added, level.lightCount);

        int spots = 0;
        for (int i = 0; i < set.count; i++) {
            CHECK(set.lights[i].range > 0.0f, "level light %d has zero range", i);
            CHECK(set.lights[i].intensity > 0.0f, "level light %d is black", i);
            if (set.lights[i].type == LIGHT_SPOT) {
                spots++;
                CHECK(set.lights[i].innerConeDeg < set.lights[i].outerConeDeg,
                      "level spot %d has an inverted cone", i);
                CHECK(fabsf(Vector3Length(set.lights[i].direction) - 1.0f) < 1e-4f,
                      "level spot %d direction is not unit length", i);
            }
        }
        CHECK(spots > 0, "expected at least one spot light in the demo level");
        LevelUnload(&level);
    }
}
