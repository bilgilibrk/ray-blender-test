// Level data as produced by the Blender exporter (tools/blender/io_kenney_racing).
//
// Coordinates are engine-space: X right, Y up, Z forward, one unit == one kit
// tile. The exporter converts from Blender's Z-up space, so nothing downstream
// has to think about handedness.
#ifndef ENGINE_LEVEL_H
#define ENGINE_LEVEL_H

#include <stdbool.h>

#include "raylib.h"

#include "engine/arena.h"

#define LEVEL_NAME_MAX 64

// A placed piece of kit art. Purely visual: drivability comes from the
// centreline, and solidity from the collider list.
typedef struct LevelProp {
    const char *model;      // kit model name, no directory and no extension
    Vector3 position;
    Vector3 rotationDeg;    // XYZ euler in degrees
    Vector3 scale;
    Color tint;
} LevelProp;

// An axis-aligned-in-local-space box, yawed about Y. Solved on the XZ plane.
typedef struct LevelCollider {
    Vector3 center;
    Vector2 halfExtents;    // half width (X) and half depth (Z) before yaw
    float height;
    float yawDeg;
} LevelCollider;

// Run-off surface: a box on the XZ plane, yawed about Y, inside which a car is
// wading through gravel rather than sitting on grass. Nothing solid, so a box
// that overlaps the tarmac costs nothing — the drivable lane always wins — but
// the exporter keeps them clear of it anyway.
//
// A trap is drawn by the kit's sand pieces and felt through these boxes. The
// two are authored together and neither is derived from the other, so art and
// physics can disagree; the level tests are what keep them honest.
typedef struct LevelSandtrap {
    Vector3 center;
    Vector2 halfExtents;    // half width (X) and half depth (Z) before yaw
    float yawDeg;
} LevelSandtrap;

typedef struct LevelSpawn {
    Vector3 position;
    float yawDeg;
} LevelSpawn;

// A light placed in the editor. Blender's own lamp objects export to these.
typedef struct LevelLight {
    Vector3 position;
    Vector3 direction;      // spot only
    Color color;
    float intensity;
    float range;
    float innerConeDeg;
    float outerConeDeg;
    bool isSpot;
} LevelLight;

// Ordered centreline of the circuit; the loop is implicitly closed.
typedef struct LevelWaypoint {
    Vector3 position;
    float width;            // drivable width at this point
} LevelWaypoint;

// A gate the cars must cross in order. Index 0 doubles as the finish line.
typedef struct LevelCheckpoint {
    Vector3 position;
    float yawDeg;           // gate faces along +Z when yaw is 0
    float width;
} LevelCheckpoint;

typedef struct Level {
    char name[LEVEL_NAME_MAX];
    int laps;
    Color skyColor;
    Color groundColor;
    Vector3 sunDirection;
    Color sunColor;
    float sunIntensity;
    Color ambientColor;
    float defaultTrackWidth;

    LevelProp *props;             int propCount;
    LevelCollider *colliders;     int colliderCount;
    LevelSandtrap *sandtraps;     int sandtrapCount;
    LevelSpawn *spawns;           int spawnCount;
    LevelWaypoint *waypoints;     int waypointCount;
    LevelCheckpoint *checkpoints; int checkpointCount;
    LevelLight *lights;           int lightCount;

    Arena arena;                  // owns every array above
} Level;

// Reads a level JSON file. On failure returns false and logs the reason.
bool LevelLoad(Level *level, const char *path);
void LevelUnload(Level *level);

// True when (x, z) is inside any of the level's sand traps. Linear over the
// list: a circuit carries a few dozen boxes and only cars that are already off
// the tarmac ever ask.
bool LevelInSandtrap(const Level *level, float x, float z);

#endif // ENGINE_LEVEL_H
