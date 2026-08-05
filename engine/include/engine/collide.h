// Collision on the XZ plane. A top-down racer never needs the Y axis for
// resolution, so everything here is 2D: oriented boxes solved with SAT, plus a
// uniform grid over the level's static colliders.
#ifndef ENGINE_COLLIDE_H
#define ENGINE_COLLIDE_H

#include <stdbool.h>

#include "raylib.h"

#include "engine/arena.h"
#include "engine/level.h"

typedef struct Obb2 {
    Vector2 center;         // (x, z)
    Vector2 halfExtents;
    float yaw;              // radians, about +Y
} Obb2;

typedef struct Manifold {
    bool hit;
    Vector2 normal;         // unit, points from `a` towards `b`
    float depth;            // penetration along `normal`
} Manifold;

// Separating-axis test. `depth` is the smallest push that separates the pair.
Manifold CollideObb2(Obb2 a, Obb2 b);

// True when `point` lies inside the box.
bool Obb2ContainsPoint(Obb2 box, Vector2 point);

// Writes the four corners in CCW order; useful for debug draw and minimaps.
void Obb2Corners(Obb2 box, Vector2 out[4]);

// --- static broadphase ----------------------------------------------------

typedef struct CollisionWorld {
    Obb2 *boxes;
    float *heights;
    int boxCount;

    // Uniform grid in compressed-row form: cellStart[c]..cellStart[c+1] indexes
    // into cellItems, which holds indices into `boxes`.
    int *cellStart;
    int *cellItems;
    int gridW, gridH;
    float cellSize;
    Vector2 origin;         // world position of cell (0,0)

    Arena arena;
} CollisionWorld;

bool CollisionWorldBuild(CollisionWorld *world, const LevelCollider *colliders, int count,
                         float cellSize);
void CollisionWorldFree(CollisionWorld *world);

// Collects boxes whose cells overlap the query disc. Returns the number written
// to `out`, never more than `maxOut`.
int CollisionWorldQuery(const CollisionWorld *world, Vector2 center, float radius,
                        int *out, int maxOut);

// Pushes `box` out of every static collider it overlaps, up to `iterations`
// passes. Returns the accumulated correction and, when non-NULL, the deepest
// contact normal seen (useful for scrubbing speed on wall scrapes).
Vector2 CollisionResolveStatic(const CollisionWorld *world, Obb2 *box, int iterations,
                               Vector2 *outDeepestNormal, float *outDeepestDepth);

#endif // ENGINE_COLLIDE_H
