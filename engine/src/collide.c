#include "engine/collide.h"

#include <math.h>
#include <string.h>

#include "raymath.h"

#define COLLIDE_MAX_GRID_CELLS (1 << 20)

static void Obb2Axes(Obb2 b, Vector2 *ux, Vector2 *uz)
{
    float c = cosf(b.yaw), s = sinf(b.yaw);
    // Yaw about +Y with the engine's left-handed XZ convention.
    *ux = (Vector2){ c, -s };
    *uz = (Vector2){ s, c };
}

void Obb2Corners(Obb2 box, Vector2 out[4])
{
    Vector2 ux, uz;
    Obb2Axes(box, &ux, &uz);
    Vector2 ex = { ux.x * box.halfExtents.x, ux.y * box.halfExtents.x };
    Vector2 ez = { uz.x * box.halfExtents.y, uz.y * box.halfExtents.y };
    out[0] = (Vector2){ box.center.x - ex.x - ez.x, box.center.y - ex.y - ez.y };
    out[1] = (Vector2){ box.center.x + ex.x - ez.x, box.center.y + ex.y - ez.y };
    out[2] = (Vector2){ box.center.x + ex.x + ez.x, box.center.y + ex.y + ez.y };
    out[3] = (Vector2){ box.center.x - ex.x + ez.x, box.center.y - ex.y + ez.y };
}

bool Obb2ContainsPoint(Obb2 box, Vector2 point)
{
    Vector2 ux, uz;
    Obb2Axes(box, &ux, &uz);
    Vector2 d = { point.x - box.center.x, point.y - box.center.y };
    float px = d.x * ux.x + d.y * ux.y;
    float pz = d.x * uz.x + d.y * uz.y;
    return fabsf(px) <= box.halfExtents.x && fabsf(pz) <= box.halfExtents.y;
}

// Half-width of `b` projected onto unit axis `axis`.
static float ProjectRadius(Obb2 b, Vector2 axis)
{
    Vector2 ux, uz;
    Obb2Axes(b, &ux, &uz);
    return fabsf((axis.x * ux.x + axis.y * ux.y)) * b.halfExtents.x +
           fabsf((axis.x * uz.x + axis.y * uz.y)) * b.halfExtents.y;
}

Manifold CollideObb2(Obb2 a, Obb2 b)
{
    Manifold m = { 0 };
    Vector2 delta = { b.center.x - a.center.x, b.center.y - a.center.y };

    Vector2 axes[4];
    Obb2Axes(a, &axes[0], &axes[1]);
    Obb2Axes(b, &axes[2], &axes[3]);

    float bestDepth = 1e30f;
    Vector2 bestAxis = { 1.0f, 0.0f };

    for (int i = 0; i < 4; i++) {
        Vector2 axis = axes[i];
        float len = sqrtf(axis.x * axis.x + axis.y * axis.y);
        if (len < 1e-6f) continue;
        axis.x /= len; axis.y /= len;

        float overlap = ProjectRadius(a, axis) + ProjectRadius(b, axis) -
                        fabsf(delta.x * axis.x + delta.y * axis.y);
        if (overlap <= 0.0f) return m;      // separating axis found

        if (overlap < bestDepth) {
            bestDepth = overlap;
            // Orient the axis so it always points from a towards b.
            float sign = (delta.x * axis.x + delta.y * axis.y) < 0.0f ? -1.0f : 1.0f;
            bestAxis = (Vector2){ axis.x * sign, axis.y * sign };
        }
    }

    m.hit = true;
    m.normal = bestAxis;
    m.depth = bestDepth;
    return m;
}

// --- static broadphase ----------------------------------------------------

static void CellRangeForBox(const CollisionWorld *w, Obb2 box, int *x0, int *y0, int *x1, int *y1)
{
    // Bound the rotated box with an AABB, which is all the grid needs.
    Vector2 corners[4];
    Obb2Corners(box, corners);
    float minX = corners[0].x, maxX = corners[0].x;
    float minY = corners[0].y, maxY = corners[0].y;
    for (int i = 1; i < 4; i++) {
        if (corners[i].x < minX) minX = corners[i].x;
        if (corners[i].x > maxX) maxX = corners[i].x;
        if (corners[i].y < minY) minY = corners[i].y;
        if (corners[i].y > maxY) maxY = corners[i].y;
    }
    *x0 = (int)floorf((minX - w->origin.x) / w->cellSize);
    *y0 = (int)floorf((minY - w->origin.y) / w->cellSize);
    *x1 = (int)floorf((maxX - w->origin.x) / w->cellSize);
    *y1 = (int)floorf((maxY - w->origin.y) / w->cellSize);
    if (*x0 < 0) *x0 = 0;
    if (*y0 < 0) *y0 = 0;
    if (*x1 >= w->gridW) *x1 = w->gridW - 1;
    if (*y1 >= w->gridH) *y1 = w->gridH - 1;
}

bool CollisionWorldBuild(CollisionWorld *world, const LevelCollider *colliders, int count,
                         float cellSize)
{
    memset(world, 0, sizeof(*world));
    if (cellSize <= 0.01f) cellSize = 1.0f;
    world->cellSize = cellSize;
    world->boxCount = count;

    if (count <= 0) {
        // An empty world is legal: queries simply return nothing.
        world->gridW = world->gridH = 1;
        if (!ArenaInit(&world->arena, 256, "collision")) return false;
        world->cellStart = ArenaAlloc(&world->arena, sizeof(int) * 2);
        return true;
    }

    // World bounds from the collider AABBs, padded by one cell.
    float minX = 1e30f, maxX = -1e30f, minZ = 1e30f, maxZ = -1e30f;
    for (int i = 0; i < count; i++) {
        float r = sqrtf(colliders[i].halfExtents.x * colliders[i].halfExtents.x +
                        colliders[i].halfExtents.y * colliders[i].halfExtents.y);
        float cx = colliders[i].center.x, cz = colliders[i].center.z;
        if (cx - r < minX) minX = cx - r;
        if (cx + r > maxX) maxX = cx + r;
        if (cz - r < minZ) minZ = cz - r;
        if (cz + r > maxZ) maxZ = cz + r;
    }
    world->origin = (Vector2){ minX - cellSize, minZ - cellSize };
    world->gridW = (int)ceilf((maxX - minX) / cellSize) + 3;
    world->gridH = (int)ceilf((maxZ - minZ) / cellSize) + 3;
    if (world->gridW < 1) world->gridW = 1;
    if (world->gridH < 1) world->gridH = 1;

    // Guard against a stray collider at a huge coordinate blowing up the grid.
    while ((long long)world->gridW * world->gridH > COLLIDE_MAX_GRID_CELLS) {
        world->cellSize *= 2.0f;
        cellSize = world->cellSize;
        world->gridW = world->gridW / 2 + 1;
        world->gridH = world->gridH / 2 + 1;
    }

    int cells = world->gridW * world->gridH;
    size_t bytes = sizeof(Obb2) * (size_t)count +
                   sizeof(float) * (size_t)count +
                   sizeof(int) * (size_t)(cells + 1) +
                   sizeof(int) * (size_t)count * 8 +       // generous item slots
                   1024;
    if (!ArenaInit(&world->arena, bytes, "collision")) return false;

    world->boxes = ArenaAlloc(&world->arena, sizeof(Obb2) * (size_t)count);
    world->heights = ArenaAlloc(&world->arena, sizeof(float) * (size_t)count);
    for (int i = 0; i < count; i++) {
        world->boxes[i] = (Obb2){
            .center = { colliders[i].center.x, colliders[i].center.z },
            .halfExtents = colliders[i].halfExtents,
            .yaw = colliders[i].yawDeg * DEG2RAD,
        };
        world->heights[i] = colliders[i].height;
    }

    world->cellStart = ArenaAlloc(&world->arena, sizeof(int) * (size_t)(cells + 1));
    if (!world->cellStart) return false;

    // Pass 1: count how many boxes touch each cell.
    for (int i = 0; i < count; i++) {
        int x0, y0, x1, y1;
        CellRangeForBox(world, world->boxes[i], &x0, &y0, &x1, &y1);
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++)
                world->cellStart[y * world->gridW + x + 1]++;
    }
    for (int c = 0; c < cells; c++) world->cellStart[c + 1] += world->cellStart[c];

    int totalItems = world->cellStart[cells];
    world->cellItems = ArenaAlloc(&world->arena, sizeof(int) * (size_t)(totalItems > 0 ? totalItems : 1));
    if (!world->cellItems) {
        TraceLog(LOG_ERROR, "COLLIDE: grid needs %d item slots, arena too small", totalItems);
        return false;
    }

    // Pass 2: fill, using a moving cursor per cell.
    int *cursor = ArenaAlloc(&world->arena, sizeof(int) * (size_t)cells);
    if (!cursor) return false;
    memcpy(cursor, world->cellStart, sizeof(int) * (size_t)cells);
    for (int i = 0; i < count; i++) {
        int x0, y0, x1, y1;
        CellRangeForBox(world, world->boxes[i], &x0, &y0, &x1, &y1);
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++)
                world->cellItems[cursor[y * world->gridW + x]++] = i;
    }

    TraceLog(LOG_INFO, "COLLIDE: %d boxes in %dx%d grid (%d refs, %.1f KB)",
             count, world->gridW, world->gridH, totalItems, world->arena.used / 1024.0);
    return true;
}

void CollisionWorldFree(CollisionWorld *world)
{
    ArenaFree(&world->arena);
    memset(world, 0, sizeof(*world));
}

int CollisionWorldQuery(const CollisionWorld *world, Vector2 center, float radius,
                        int *out, int maxOut)
{
    if (world->boxCount <= 0 || maxOut <= 0) return 0;

    int x0 = (int)floorf((center.x - radius - world->origin.x) / world->cellSize);
    int y0 = (int)floorf((center.y - radius - world->origin.y) / world->cellSize);
    int x1 = (int)floorf((center.x + radius - world->origin.x) / world->cellSize);
    int y1 = (int)floorf((center.y + radius - world->origin.y) / world->cellSize);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= world->gridW) x1 = world->gridW - 1;
    if (y1 >= world->gridH) y1 = world->gridH - 1;

    int found = 0;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            int c = y * world->gridW + x;
            for (int k = world->cellStart[c]; k < world->cellStart[c + 1]; k++) {
                int id = world->cellItems[k];
                // A box spanning several cells appears more than once.
                bool seen = false;
                for (int j = 0; j < found; j++) {
                    if (out[j] == id) { seen = true; break; }
                }
                if (seen) continue;
                out[found++] = id;
                if (found >= maxOut) return found;
            }
        }
    }
    return found;
}

Vector2 CollisionResolveStatic(const CollisionWorld *world, Obb2 *box, int iterations,
                               Vector2 *outDeepestNormal, float *outDeepestDepth)
{
    Vector2 totalPush = { 0 };
    float deepest = 0.0f;
    Vector2 deepestNormal = { 0 };

    if (world->boxCount > 0) {
        int candidates[128];
        float radius = sqrtf(box->halfExtents.x * box->halfExtents.x +
                             box->halfExtents.y * box->halfExtents.y);

        for (int pass = 0; pass < iterations; pass++) {
            int n = CollisionWorldQuery(world, box->center, radius, candidates,
                                        (int)(sizeof candidates / sizeof candidates[0]));
            bool anyHit = false;
            for (int k = 0; k < n; k++) {
                Manifold m = CollideObb2(*box, world->boxes[candidates[k]]);
                if (!m.hit) continue;
                anyHit = true;
                // Normal points box -> obstacle, so push back along -normal.
                box->center.x -= m.normal.x * m.depth;
                box->center.y -= m.normal.y * m.depth;
                totalPush.x -= m.normal.x * m.depth;
                totalPush.y -= m.normal.y * m.depth;
                if (m.depth > deepest) {
                    deepest = m.depth;
                    deepestNormal = (Vector2){ -m.normal.x, -m.normal.y };
                }
            }
            if (!anyHit) break;
        }
    }

    if (outDeepestNormal) *outDeepestNormal = deepestNormal;
    if (outDeepestDepth) *outDeepestDepth = deepest;
    return totalPush;
}
