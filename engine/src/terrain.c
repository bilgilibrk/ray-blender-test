#include "engine/terrain.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "raymath.h"

// Cells per chunk edge. Small enough that light selection is local, large
// enough that the draw-call count stays modest.
#define TERRAIN_CHUNK_CELLS 8

// Caps the grid so a level with a stray far-away waypoint cannot ask for
// hundreds of megabytes of ground.
#define TERRAIN_MAX_SAMPLES (512 * 512)

// How much steeper the ground is pretended to be when shading relief. The
// camera looks almost straight down, where a real 17% slope tilts its normal
// by only 10 degrees and lights identically to the flat around it.
#define TERRAIN_RELIEF_EXAGGERATION 5.0f

// Deepest the relief shading is allowed to darken the ground. Vertex colours
// multiply the material colour, so shading can only ever subtract light —
// anything above 1.0 would clip to white and flatten the crests back out.
#define TERRAIN_RELIEF_DEPTH 0.34f

// Relief is lit from much lower than the real sun so that slopes separate into
// a lit face and a shaded one. Points towards the light, not along it.
static Vector3 ReliefLightDirection(void)
{
    return Vector3Normalize((Vector3){ 0.55f, 0.62f, 0.42f });
}

TerrainSettings TerrainDefaultSettings(Color groundColor)
{
    TerrainSettings s = {
        .margin = 9.0f,
        // Cells have to be comfortably finer than the road is wide. At 0.9 the
        // ground was coarser than the 0.69 lane, so on a gradient the linear
        // surface between samples crossed above the tarmac and ate the track.
        .cellSize = 0.4f,
        .sinkBelowTrack = 0.06f,
        .color = groundColor,
    };
    return s;
}

// Softening term in the weight. Also the reason the kernel never divides by
// zero when a query lands exactly on a sample.
#define HEIGHTFIELD_SOFTEN 0.45f

bool HeightFieldBuild(HeightField *field, const Spline *spline)
{
    memset(field, 0, sizeof(*field));
    if (spline->count < 1) return false;

    // One allocation, carved into three so the query reads three contiguous
    // streams instead of striding a 36-byte SplineSample for 12 useful bytes.
    size_t n = (size_t)spline->count;
    float *storage = (float *)calloc(n * 3, sizeof(float));
    if (!storage) return false;

    field->storage = storage;
    field->x = storage;
    field->y = storage + n;
    field->z = storage + n * 2;
    field->count = spline->count;
    for (int i = 0; i < spline->count; i++) {
        Vector3 p = spline->samples[i].position;
        field->x[i] = p.x;
        field->y[i] = p.y;
        field->z[i] = p.z;
    }
    return true;
}

void HeightFieldFree(HeightField *field)
{
    free(field->storage);
    memset(field, 0, sizeof(*field));
}

// Inverse-distance weighting against every spline sample. The 1/(d^4 + k) shape
// makes the ground hug the road closely and relax to the average height in the
// open, with no creases where the nearest sample changes.
float HeightFieldAt(const HeightField *field, float x, float z)
{
    const float *restrict px = field->x;
    const float *restrict py = field->y;
    const float *restrict pz = field->z;
    int n = field->count;

    // Four independent accumulator chains rather than one.
    //
    // The kernel needs a divide per sample, and a divide is the one float op
    // that is not pipelined: on the Pi's in-order Cortex-A53 the next one
    // cannot start until the running one retires, so a single chain of them
    // costs the divider's latency per sample instead of its throughput. Four
    // sums in flight give the hardware something to overlap and are worth
    // about 3.5x here — measured, and the whole reason this loop is written
    // out rather than left as the obvious three lines.
    //
    // All this changes about the result is the order the weights are summed
    // in, which moves the last bit or two of a float and nothing else.
    float w0 = 0.0f, w1 = 0.0f, w2 = 0.0f, w3 = 0.0f;
    float t0 = 0.0f, t1 = 0.0f, t2 = 0.0f, t3 = 0.0f;

    int i = 0;
    for (; i + 3 < n; i += 4) {
        float ax = x - px[i + 0], az = z - pz[i + 0];
        float bx = x - px[i + 1], bz = z - pz[i + 1];
        float cx = x - px[i + 2], cz = z - pz[i + 2];
        float dx = x - px[i + 3], dz = z - pz[i + 3];

        float a2 = ax * ax + az * az;
        float b2 = bx * bx + bz * bz;
        float c2 = cx * cx + cz * cz;
        float d2 = dx * dx + dz * dz;

        float wa = 1.0f / (a2 * a2 + HEIGHTFIELD_SOFTEN);
        float wb = 1.0f / (b2 * b2 + HEIGHTFIELD_SOFTEN);
        float wc = 1.0f / (c2 * c2 + HEIGHTFIELD_SOFTEN);
        float wd = 1.0f / (d2 * d2 + HEIGHTFIELD_SOFTEN);

        w0 += py[i + 0] * wa; t0 += wa;
        w1 += py[i + 1] * wb; t1 += wb;
        w2 += py[i + 2] * wc; t2 += wc;
        w3 += py[i + 3] * wd; t3 += wd;
    }
    for (; i < n; i++) {
        float ex = x - px[i], ez = z - pz[i];
        float e2 = ex * ex + ez * ez;
        float w = 1.0f / (e2 * e2 + HEIGHTFIELD_SOFTEN);
        w0 += py[i] * w;
        t0 += w;
    }

    float weighted = (w0 + w1) + (w2 + w3);
    float total = (t0 + t1) + (t2 + t3);
    return (total > 0.0f) ? weighted / total : 0.0f;
}

static float SampleGrid(const Terrain *terrain, int ix, int iz)
{
    if (ix < 0) ix = 0;
    if (iz < 0) iz = 0;
    if (ix > terrain->gridX) ix = terrain->gridX;
    if (iz > terrain->gridZ) iz = terrain->gridZ;
    return terrain->heights[iz * (terrain->gridX + 1) + ix];
}

static Vector3 NormalAt(const Terrain *terrain, int ix, int iz)
{
    // Central differences on the height field.
    float left = SampleGrid(terrain, ix - 1, iz);
    float right = SampleGrid(terrain, ix + 1, iz);
    float back = SampleGrid(terrain, ix, iz - 1);
    float front = SampleGrid(terrain, ix, iz + 1);
    float span = 2.0f * terrain->cellSize;
    Vector3 n = { (left - right) / span, 1.0f, (back - front) / span };
    return Vector3Normalize(n);
}

static Vector3 VertexAt(const Terrain *terrain, int ix, int iz)
{
    return (Vector3){
        terrain->origin.x + (float)ix * terrain->cellSize,
        SampleGrid(terrain, ix, iz),
        terrain->origin.y + (float)iz * terrain->cellSize,
    };
}

bool TerrainBuild(Terrain *terrain, const Spline *spline, const TerrainSettings *settings)
{
    memset(terrain, 0, sizeof(*terrain));
    if (spline->count < 2) return false;

    float cellSize = (settings->cellSize > 0.05f) ? settings->cellSize : 0.9f;
    terrain->cellSize = cellSize;

    float minX = 1e30f, maxX = -1e30f, minZ = 1e30f, maxZ = -1e30f;
    for (int i = 0; i < spline->count; i++) {
        Vector3 p = spline->samples[i].position;
        if (p.x < minX) minX = p.x;
        if (p.x > maxX) maxX = p.x;
        if (p.z < minZ) minZ = p.z;
        if (p.z > maxZ) maxZ = p.z;
    }
    minX -= settings->margin; maxX += settings->margin;
    minZ -= settings->margin; maxZ += settings->margin;

    terrain->origin = (Vector2){ minX, minZ };
    terrain->gridX = (int)ceilf((maxX - minX) / cellSize);
    terrain->gridZ = (int)ceilf((maxZ - minZ) / cellSize);
    if (terrain->gridX < 1) terrain->gridX = 1;
    if (terrain->gridZ < 1) terrain->gridZ = 1;

    while ((long long)(terrain->gridX + 1) * (terrain->gridZ + 1) > TERRAIN_MAX_SAMPLES) {
        terrain->cellSize *= 2.0f;
        cellSize = terrain->cellSize;
        terrain->gridX = terrain->gridX / 2 + 1;
        terrain->gridZ = terrain->gridZ / 2 + 1;
    }

    int sampleCount = (terrain->gridX + 1) * (terrain->gridZ + 1);
    terrain->heights = calloc((size_t)sampleCount, sizeof(float));
    if (!terrain->heights) return false;

    HeightField field;
    if (!HeightFieldBuild(&field, spline)) {
        free(terrain->heights);
        terrain->heights = NULL;
        return false;
    }
    for (int iz = 0; iz <= terrain->gridZ; iz++) {
        for (int ix = 0; ix <= terrain->gridX; ix++) {
            float x = minX + (float)ix * cellSize;
            float z = minZ + (float)iz * cellSize;
            terrain->heights[iz * (terrain->gridX + 1) + ix] =
                HeightFieldAt(&field, x, z) - settings->sinkBelowTrack;
        }
    }
    HeightFieldFree(&field);

    // --- build chunk meshes ---------------------------------------------------
    int chunksX = (terrain->gridX + TERRAIN_CHUNK_CELLS - 1) / TERRAIN_CHUNK_CELLS;
    int chunksZ = (terrain->gridZ + TERRAIN_CHUNK_CELLS - 1) / TERRAIN_CHUNK_CELLS;
    terrain->chunks = calloc((size_t)(chunksX * chunksZ), sizeof(TerrainChunk));
    if (!terrain->chunks) {
        free(terrain->heights);
        terrain->heights = NULL;
        return false;
    }

    Vector3 reliefLight = ReliefLightDirection();

    int live = 0;
    for (int cz = 0; cz < chunksZ; cz++) {
        for (int cx = 0; cx < chunksX; cx++) {
            int x0 = cx * TERRAIN_CHUNK_CELLS;
            int z0 = cz * TERRAIN_CHUNK_CELLS;
            int x1 = x0 + TERRAIN_CHUNK_CELLS;
            int z1 = z0 + TERRAIN_CHUNK_CELLS;
            if (x1 > terrain->gridX) x1 = terrain->gridX;
            if (z1 > terrain->gridZ) z1 = terrain->gridZ;
            int cells = (x1 - x0) * (z1 - z0);
            if (cells <= 0) continue;

            int vertexCount = cells * 6;   // two triangles per cell, non-indexed
            Mesh mesh = { 0 };
            mesh.vertexCount = vertexCount;
            mesh.triangleCount = cells * 2;
            mesh.vertices = MemAlloc((unsigned int)(sizeof(float) * 3 * (size_t)vertexCount));
            mesh.normals = MemAlloc((unsigned int)(sizeof(float) * 3 * (size_t)vertexCount));
            mesh.colors = MemAlloc((unsigned int)(sizeof(unsigned char) * 4 * (size_t)vertexCount));
            if (!mesh.vertices || !mesh.normals || !mesh.colors) {
                MemFree(mesh.vertices);
                MemFree(mesh.normals);
                MemFree(mesh.colors);
                continue;
            }

            BoundingBox bounds = { { 1e30f, 1e30f, 1e30f }, { -1e30f, -1e30f, -1e30f } };
            int out = 0;
            for (int iz = z0; iz < z1; iz++) {
                for (int ix = x0; ix < x1; ix++) {
                    const int cornerX[6] = { 0, 0, 1, 0, 1, 1 };
                    const int cornerZ[6] = { 0, 1, 1, 0, 1, 0 };
                    for (int k = 0; k < 6; k++) {
                        int gx = ix + cornerX[k];
                        int gz = iz + cornerZ[k];
                        Vector3 v = VertexAt(terrain, gx, gz);
                        Vector3 n = NormalAt(terrain, gx, gz);

                        mesh.vertices[out * 3 + 0] = v.x;
                        mesh.vertices[out * 3 + 1] = v.y;
                        mesh.vertices[out * 3 + 2] = v.z;
                        mesh.normals[out * 3 + 0] = n.x;
                        mesh.normals[out * 3 + 1] = n.y;
                        mesh.normals[out * 3 + 2] = n.z;

                        // Seen from almost directly above, a slope's normal
                        // barely tilts and the scene lighting leaves hills
                        // invisible. Two cartographic tricks stand in for it,
                        // baked into the vertex colours:
                        //
                        //   height tint — hollows darker, crests lighter, which
                        //                 carries the broad shape of the land
                        //   hillshade   — the normal steepened well past
                        //                 reality and lit from a low angle,
                        //                 which gives each slope a lit face and
                        //                 a shaded one
                        float t = RenderReliefHeight01(v.y);

                        Vector3 steep = Vector3Normalize((Vector3){
                            n.x * TERRAIN_RELIEF_EXAGGERATION, 1.0f,
                            n.z * TERRAIN_RELIEF_EXAGGERATION });
                        // Measured against level ground, so flat terrain lands
                        // at zero and keeps the material colour untouched.
                        float hillshade = Vector3DotProduct(steep, reliefLight) - reliefLight.y;

                        float relief = Clamp(0.55f * (2.0f * t - 1.0f) + 1.5f * hillshade,
                                             -1.0f, 1.0f);
                        float shade = 1.0f - TERRAIN_RELIEF_DEPTH * (1.0f - relief) * 0.5f;
                        mesh.colors[out * 4 + 0] = (unsigned char)Clamp(255.0f * shade, 0.0f, 255.0f);
                        mesh.colors[out * 4 + 1] = (unsigned char)Clamp(255.0f * shade, 0.0f, 255.0f);
                        mesh.colors[out * 4 + 2] = (unsigned char)Clamp(255.0f * shade, 0.0f, 255.0f);
                        mesh.colors[out * 4 + 3] = 255;
                        out++;

                        if (v.x < bounds.min.x) bounds.min.x = v.x;
                        if (v.y < bounds.min.y) bounds.min.y = v.y;
                        if (v.z < bounds.min.z) bounds.min.z = v.z;
                        if (v.x > bounds.max.x) bounds.max.x = v.x;
                        if (v.y > bounds.max.y) bounds.max.y = v.y;
                        if (v.z > bounds.max.z) bounds.max.z = v.z;
                    }
                }
            }

            UploadMesh(&mesh, false);
            terrain->chunks[live].mesh = mesh;
            terrain->chunks[live].bounds = bounds;
            terrain->triangleCount += mesh.triangleCount;
            live++;
        }
    }
    terrain->chunkCount = live;

    terrain->material = RenderSceneMaterial();
    terrain->material.maps[MATERIAL_MAP_DIFFUSE].color = settings->color;
    terrain->ready = true;

    TraceLog(LOG_INFO, "TERRAIN: %dx%d samples, %d chunks, %d triangles (cell %.2f)",
             terrain->gridX + 1, terrain->gridZ + 1, terrain->chunkCount,
             terrain->triangleCount, (double)terrain->cellSize);
    return true;
}

void TerrainFree(Terrain *terrain)
{
    for (int i = 0; i < terrain->chunkCount; i++) UnloadMesh(terrain->chunks[i].mesh);
    free(terrain->chunks);
    free(terrain->heights);
    // The material's shader and maps belong to the renderer.
    memset(terrain, 0, sizeof(*terrain));
}

void TerrainDraw(Terrain *terrain, Camera3D camera)
{
    terrain->drawnLastFrame = 0;
    if (!terrain->ready) return;

    Frustum frustum = RenderFrustumFromCamera(camera);
    for (int i = 0; i < terrain->chunkCount; i++) {
        if (!RenderFrustumTestBox(&frustum, terrain->chunks[i].bounds)) continue;
        RenderDrawLitMesh(terrain->chunks[i].mesh, terrain->material, MatrixIdentity(),
                          terrain->chunks[i].bounds);
        terrain->drawnLastFrame++;
    }
}

float TerrainHeightAt(const Terrain *terrain, float x, float z)
{
    if (!terrain->ready || !terrain->heights) return 0.0f;

    float fx = (x - terrain->origin.x) / terrain->cellSize;
    float fz = (z - terrain->origin.y) / terrain->cellSize;
    int ix = (int)floorf(fx);
    int iz = (int)floorf(fz);
    float tx = fx - (float)ix;
    float tz = fz - (float)iz;

    float h00 = SampleGrid(terrain, ix, iz);
    float h10 = SampleGrid(terrain, ix + 1, iz);
    float h01 = SampleGrid(terrain, ix, iz + 1);
    float h11 = SampleGrid(terrain, ix + 1, iz + 1);

    float top = h00 + (h10 - h00) * tx;
    float bottom = h01 + (h11 - h01) * tx;
    return top + (bottom - top) * tz;
}
