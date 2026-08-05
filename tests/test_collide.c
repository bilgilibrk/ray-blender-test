#include <math.h>
#include <stdio.h>
#include <string.h>

#include "raylib.h"

#include "engine/collide.h"

#include "tests.h"

static Obb2 Box(float x, float z, float hx, float hz, float yawDeg)
{
    return (Obb2){ .center = { x, z }, .halfExtents = { hx, hz }, .yaw = yawDeg * DEG2RAD };
}

void RunCollisionTests(void)
{
    // --- separating axis ---------------------------------------------------
    CHECK(!CollideObb2(Box(0, 0, 1, 1, 0), Box(3, 0, 1, 1, 0)).hit, "clearly apart");
    CHECK(!CollideObb2(Box(0, 0, 1, 1, 0), Box(2.01f, 0, 1, 1, 0)).hit, "just apart");

    Manifold m = CollideObb2(Box(0, 0, 1, 1, 0), Box(1.5f, 0, 1, 1, 0));
    CHECK(m.hit, "overlapping boxes register");
    CHECK(fabsf(m.depth - 0.5f) < 1e-4f, "penetration %.4f, expected 0.5", (double)m.depth);
    CHECK(m.normal.x > 0.99f, "normal should point along +X, got (%.2f, %.2f)",
          (double)m.normal.x, (double)m.normal.y);

    // The normal always points from the first box towards the second.
    Manifold flipped = CollideObb2(Box(1.5f, 0, 1, 1, 0), Box(0, 0, 1, 1, 0));
    CHECK(flipped.normal.x < -0.99f, "reversed normal should point along -X, got %.2f",
          (double)flipped.normal.x);

    // A rotated box separates on an axis an AABB test would miss.
    CHECK(!CollideObb2(Box(0, 0, 1, 0.1f, 0), Box(0, 1.0f, 1, 0.1f, 0)).hit,
          "thin boxes stacked in Z stay apart");
    CHECK(CollideObb2(Box(0, 0, 1, 0.1f, 0), Box(0, 0.9f, 0.1f, 1, 0)).hit,
          "crossed thin boxes overlap");

    // A 45-degree square's corner reaches further than its half extent.
    CHECK(CollideObb2(Box(0, 0, 1, 1, 0), Box(2.2f, 0, 1, 1, 45)).hit,
          "rotated corner should reach across the gap");

    // --- point containment --------------------------------------------------
    CHECK(Obb2ContainsPoint(Box(0, 0, 1, 2, 0), (Vector2){ 0.5f, 1.5f }), "point inside");
    CHECK(!Obb2ContainsPoint(Box(0, 0, 1, 2, 0), (Vector2){ 1.5f, 0.0f }), "point outside");
    CHECK(Obb2ContainsPoint(Box(0, 0, 1, 2, 90), (Vector2){ 1.9f, 0.0f }),
          "rotating the box moves its long axis");

    // --- corners ---------------------------------------------------------------
    Vector2 corners[4];
    Obb2Corners(Box(5, 7, 1, 2, 0), corners);
    float minX = corners[0].x, maxX = corners[0].x, minZ = corners[0].y, maxZ = corners[0].y;
    for (int i = 1; i < 4; i++) {
        if (corners[i].x < minX) minX = corners[i].x;
        if (corners[i].x > maxX) maxX = corners[i].x;
        if (corners[i].y < minZ) minZ = corners[i].y;
        if (corners[i].y > maxZ) maxZ = corners[i].y;
    }
    CHECK(fabsf(minX - 4.0f) < 1e-4f && fabsf(maxX - 6.0f) < 1e-4f, "corner X span");
    CHECK(fabsf(minZ - 5.0f) < 1e-4f && fabsf(maxZ - 9.0f) < 1e-4f, "corner Z span");

    // --- broadphase ---------------------------------------------------------------
    enum { GRID = 12 };
    LevelCollider colliders[GRID * GRID];
    int count = 0;
    for (int z = 0; z < GRID; z++) {
        for (int x = 0; x < GRID; x++) {
            colliders[count].center = (Vector3){ (float)x * 2.0f, 0.0f, (float)z * 2.0f };
            colliders[count].halfExtents = (Vector2){ 0.4f, 0.4f };
            colliders[count].height = 1.0f;
            colliders[count].yawDeg = 0.0f;
            count++;
        }
    }

    CollisionWorld world;
    CHECK(CollisionWorldBuild(&world, colliders, count, 2.0f), "grid world builds");

    int hits[64];
    int found = CollisionWorldQuery(&world, (Vector2){ 0.0f, 0.0f }, 0.5f, hits,
                                    (int)(sizeof hits / sizeof hits[0]));
    CHECK(found >= 1, "query at a collider found %d", found);

    // Results must be unique even though a box can span several cells.
    found = CollisionWorldQuery(&world, (Vector2){ 5.0f, 5.0f }, 6.0f, hits,
                                (int)(sizeof hits / sizeof hits[0]));
    bool duplicated = false;
    for (int i = 0; i < found; i++) {
        for (int j = i + 1; j < found; j++) {
            if (hits[i] == hits[j]) duplicated = true;
        }
    }
    CHECK(!duplicated, "broadphase returned the same box twice");

    CHECK(CollisionWorldQuery(&world, (Vector2){ 500.0f, 500.0f }, 1.0f, hits, 64) == 0,
          "query far outside the grid finds nothing");

    // --- resolution ------------------------------------------------------------
    // A box dropped on top of an obstacle must end up clear of every obstacle.
    Obb2 mover = Box(0.1f, 0.1f, 0.3f, 0.3f, 0.0f);
    Vector2 normal = { 0 };
    float depth = 0.0f;
    CollisionResolveStatic(&world, &mover, 4, &normal, &depth);
    CHECK(depth > 0.0f, "resolver reported no contact for an overlapping box");

    int stillHit = 0;
    int candidates[64];
    int n = CollisionWorldQuery(&world, mover.center, 1.5f, candidates, 64);
    for (int i = 0; i < n; i++) {
        if (CollideObb2(mover, world.boxes[candidates[i]]).hit) stillHit++;
    }
    CHECK(stillHit == 0, "box still overlaps %d obstacle(s) after resolution", stillHit);

    CollisionWorldFree(&world);

    // An empty world must be usable, not a crash.
    CollisionWorld empty;
    CHECK(CollisionWorldBuild(&empty, NULL, 0, 1.0f), "empty world builds");
    CHECK(CollisionWorldQuery(&empty, (Vector2){ 0, 0 }, 5.0f, hits, 64) == 0,
          "empty world returns nothing");
    Obb2 free_ = Box(0, 0, 1, 1, 0);
    CollisionResolveStatic(&empty, &free_, 2, &normal, &depth);
    CHECK(free_.center.x == 0.0f && free_.center.y == 0.0f, "empty world moved the box");
    CollisionWorldFree(&empty);
}
