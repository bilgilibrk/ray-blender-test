// Ground that follows the track's elevation.
//
// A flat plane stops working the moment a circuit has hills: the road either
// floats above it or sinks through. This builds a heightfield from the racing
// line instead, so the ground rises and falls with the tarmac and flattens out
// smoothly away from it. Normals are computed from the field, so slopes shade.
//
// Split into chunks for the same reasons the static batch is: frustum culling,
// and so each piece of ground is lit by the lamps near it rather than by the
// brightest lights anywhere in the level.
#ifndef ENGINE_TERRAIN_H
#define ENGINE_TERRAIN_H

#include <stdbool.h>

#include "raylib.h"

#include "engine/render.h"
#include "engine/spline.h"

typedef struct TerrainChunk {
    Mesh mesh;
    BoundingBox bounds;
} TerrainChunk;

typedef struct Terrain {
    TerrainChunk *chunks;
    int chunkCount;
    Material material;

    float *heights;         // (gridX + 1) * (gridZ + 1) samples
    int gridX, gridZ;
    float cellSize;
    Vector2 origin;         // world XZ of sample (0, 0)

    int triangleCount;
    int drawnLastFrame;
    bool ready;
} Terrain;

typedef struct TerrainSettings {
    float margin;           // how far past the track the ground extends
    float cellSize;         // world units between height samples
    float sinkBelowTrack;   // pushes the surface under the road to avoid z-fighting
    Color color;
} TerrainSettings;

TerrainSettings TerrainDefaultSettings(Color groundColor);

// Fits a heightfield to the spline. Returns false only on allocation failure.
bool TerrainBuild(Terrain *terrain, const Spline *spline, const TerrainSettings *settings);
void TerrainFree(Terrain *terrain);

void TerrainDraw(Terrain *terrain, Camera3D camera);

// Bilinear height lookup; returns 0 outside the grid.
float TerrainHeightAt(const Terrain *terrain, float x, float z);

#endif // ENGINE_TERRAIN_H
