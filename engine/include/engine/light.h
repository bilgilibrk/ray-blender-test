// Point and spot lights.
//
// Forward shaded: a scene may hold many lights, but only the few most relevant
// to whatever is being drawn are uploaded to the shader. That keeps the
// fragment cost fixed and stays inside the uniform budget of GLES2, which the
// Raspberry Pi console build targets.
#ifndef ENGINE_LIGHT_H
#define ENGINE_LIGHT_H

#include <stdbool.h>

#include "raylib.h"

#include "engine/level.h"

// Lights a scene may hold, and how many can affect a single draw call.
#define LIGHTS_MAX 64
#define LIGHTS_PER_DRAW 8

typedef enum LightType {
    LIGHT_POINT = 0,
    LIGHT_SPOT,
} LightType;

typedef struct Light {
    LightType type;
    Vector3 position;
    Vector3 direction;      // spot only; need not be normalised
    Color color;
    float intensity;
    float range;            // brightness reaches zero here
    float innerConeDeg;     // spot: fully lit inside this half-angle
    float outerConeDeg;     // spot: fades to nothing by this half-angle
    bool enabled;
} Light;

// A point light with sensible cone fields for the shared struct.
Light LightMakePoint(Vector3 position, Color color, float intensity, float range);
Light LightMakeSpot(Vector3 position, Vector3 direction, Color color, float intensity,
                    float range, float innerConeDeg, float outerConeDeg);

typedef struct LightSet {
    Light lights[LIGHTS_MAX];
    int count;
} LightSet;

void LightSetClear(LightSet *set);

// Returns the new light's index, or -1 when the set is full.
int LightSetAdd(LightSet *set, Light light);
Light *LightSetAt(LightSet *set, int index);

// Appends every light declared by a level. Returns how many were added.
int LightSetAddFromLevel(LightSet *set, const Level *level);

// Chooses the lights that matter most to a bounding sphere, strongest first,
// and writes their indices to `out`. Returns how many were written.
int LightSetSelect(const LightSet *set, Vector3 center, float radius, int *out, int maxOut);

// Influence of a light on a bounding sphere; 0 means it cannot reach.
float LightInfluence(const Light *light, Vector3 center, float radius);

#endif // ENGINE_LIGHT_H
