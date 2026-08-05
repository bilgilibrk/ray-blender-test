#include "engine/light.h"

#include <math.h>
#include <string.h>

#include "raymath.h"

Light LightMakePoint(Vector3 position, Color color, float intensity, float range)
{
    Light light = {
        .type = LIGHT_POINT,
        .position = position,
        .direction = { 0.0f, -1.0f, 0.0f },
        .color = color,
        .intensity = intensity,
        .range = (range > 0.001f) ? range : 0.001f,
        .innerConeDeg = 0.0f,
        .outerConeDeg = 0.0f,
        .enabled = true,
    };
    return light;
}

Light LightMakeSpot(Vector3 position, Vector3 direction, Color color, float intensity,
                    float range, float innerConeDeg, float outerConeDeg)
{
    // Keep inner strictly inside outer or the falloff divides by zero.
    if (outerConeDeg < 1.0f) outerConeDeg = 1.0f;
    if (outerConeDeg > 89.0f) outerConeDeg = 89.0f;
    if (innerConeDeg > outerConeDeg - 0.5f) innerConeDeg = outerConeDeg - 0.5f;
    if (innerConeDeg < 0.0f) innerConeDeg = 0.0f;

    Light light = {
        .type = LIGHT_SPOT,
        .position = position,
        .direction = Vector3Normalize(direction),
        .color = color,
        .intensity = intensity,
        .range = (range > 0.001f) ? range : 0.001f,
        .innerConeDeg = innerConeDeg,
        .outerConeDeg = outerConeDeg,
        .enabled = true,
    };
    return light;
}

void LightSetClear(LightSet *set)
{
    memset(set, 0, sizeof(*set));
}

int LightSetAdd(LightSet *set, Light light)
{
    if (set->count >= LIGHTS_MAX) {
        TraceLog(LOG_WARNING, "LIGHT: set is full (%d), dropping light", LIGHTS_MAX);
        return -1;
    }
    if (light.range <= 0.001f) light.range = 0.001f;
    set->lights[set->count] = light;
    return set->count++;
}

Light *LightSetAt(LightSet *set, int index)
{
    if (index < 0 || index >= set->count) return NULL;
    return &set->lights[index];
}

int LightSetAddFromLevel(LightSet *set, const Level *level)
{
    int added = 0;
    for (int i = 0; i < level->lightCount; i++) {
        const LevelLight *source = &level->lights[i];
        Light light = source->isSpot
            ? LightMakeSpot(source->position, source->direction, source->color,
                            source->intensity, source->range,
                            source->innerConeDeg, source->outerConeDeg)
            : LightMakePoint(source->position, source->color, source->intensity, source->range);
        if (LightSetAdd(set, light) >= 0) added++;
    }
    if (added > 0) TraceLog(LOG_INFO, "LIGHT: %d light(s) from level", added);
    return added;
}

float LightInfluence(const Light *light, Vector3 center, float radius)
{
    if (!light->enabled || light->intensity <= 0.0f) return 0.0f;

    float distance = Vector3Distance(light->position, center) - radius;
    if (distance < 0.0f) distance = 0.0f;
    if (distance >= light->range) return 0.0f;

    // Matches the shader's falloff so selection and shading agree.
    float falloff = 1.0f - distance / light->range;
    return light->intensity * falloff * falloff;
}

int LightSetSelect(const LightSet *set, Vector3 center, float radius, int *out, int maxOut)
{
    if (maxOut <= 0) return 0;

    float best[LIGHTS_PER_DRAW];
    int count = 0;
    if (maxOut > LIGHTS_PER_DRAW) maxOut = LIGHTS_PER_DRAW;

    for (int i = 0; i < set->count; i++) {
        float score = LightInfluence(&set->lights[i], center, radius);
        if (score <= 0.0f) continue;

        // Insertion sort into a short strongest-first list.
        int slot = count;
        if (count == maxOut) {
            if (score <= best[count - 1]) continue;
            slot = count - 1;
        } else {
            count++;
        }
        while (slot > 0 && best[slot - 1] < score) {
            best[slot] = best[slot - 1];
            out[slot] = out[slot - 1];
            slot--;
        }
        best[slot] = score;
        out[slot] = i;
    }
    return count;
}
