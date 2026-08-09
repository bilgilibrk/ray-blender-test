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

// The height field the ground is fitted to, kept separate from the meshes.
//
// It is inverse-distance weighting against every spline sample, which is a
// quadratic amount of work: one evaluation per grid node, each touching the
// whole racing line. That is the single most expensive part of loading a
// circuit, so the sample positions are held here split into three flat arrays
// rather than read back out of the spline's interleaved structs.
//
// Separating it from TerrainBuild also means it can be evaluated with no GPU,
// which is what lets the tests check the ground without a window.
typedef struct HeightField {
    float *x, *y, *z;   // spline sample positions, one array per component
    int count;
    void *storage;      // single allocation backing the three arrays
} HeightField;

bool HeightFieldBuild(HeightField *field, const Spline *spline);
void HeightFieldFree(HeightField *field);

// Ground height at a point, before the terrain's sinkBelowTrack is applied.
float HeightFieldAt(const HeightField *field, float x, float z);

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
