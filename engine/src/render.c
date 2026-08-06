#include "engine/render.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "raymath.h"
#include "rlgl.h"

#include "engine/assets.h"
#include "engine/light.h"

#if defined(ENGINE_PLATFORM_DRM)
    #define ENGINE_GLSL_VERSION 100
#else
    #define ENGINE_GLSL_VERSION 330
#endif

// Non-indexed chunk meshes, so this is a triangle-soup vertex cap.
#define BATCH_MAX_VERTS_PER_CHUNK 120000

static struct {
    Shader shader;
    int locSunDir;
    int locSunColor;
    int locAmbient;
    int locFogColor;
    int locFogDensity;
    int locCameraPos;
    int locLightCount;
    int locLightPosRange;
    int locLightColor;
    int locLightDir;

    RenderSettings settings;
    const LightSet *lights;     // borrowed; the caller owns the storage
    int uploadedCount;

    Mesh groundMesh;            // unit quad on XZ, scaled per draw
    Material sceneMaterial;
    float reliefLow, reliefHigh;
    bool ready;
} g_render;

// How far the height tint may darken a baked prop. Vertex colours multiply the
// material colour, so this can only ever subtract light — going above 1.0 would
// clip to white and flatten the crests back out.
#define RELIEF_PROP_DEPTH 0.30f

void RenderSetReliefRange(float lowest, float highest)
{
    g_render.reliefLow = lowest;
    g_render.reliefHigh = highest;
}

float RenderReliefHeight01(float y)
{
    float span = g_render.reliefHigh - g_render.reliefLow;
    if (span <= 1e-5f) return 0.5f;
    return Clamp((y - g_render.reliefLow) / span, 0.0f, 1.0f);
}

// A single quad is enough: lighting is evaluated per fragment, so a large
// ground plane still picks up every nearby lamp.
static Mesh MakeGroundQuad(void)
{
    Mesh mesh = { 0 };
    mesh.vertexCount = 6;
    mesh.triangleCount = 2;
    mesh.vertices = MemAlloc(sizeof(float) * 3 * 6);
    mesh.normals = MemAlloc(sizeof(float) * 3 * 6);
    if (!mesh.vertices || !mesh.normals) {
        MemFree(mesh.vertices);
        MemFree(mesh.normals);
        return (Mesh){ 0 };
    }

    const float corners[6][2] = {
        { -0.5f, -0.5f }, { -0.5f, 0.5f }, { 0.5f, 0.5f },
        { -0.5f, -0.5f }, { 0.5f, 0.5f }, { 0.5f, -0.5f },
    };
    for (int i = 0; i < 6; i++) {
        mesh.vertices[i * 3 + 0] = corners[i][0];
        mesh.vertices[i * 3 + 1] = 0.0f;
        mesh.vertices[i * 3 + 2] = corners[i][1];
        mesh.normals[i * 3 + 0] = 0.0f;
        mesh.normals[i * 3 + 1] = 1.0f;
        mesh.normals[i * 3 + 2] = 0.0f;
    }
    UploadMesh(&mesh, false);
    return mesh;
}

// Shader source lives in its own file so tools/check_shaders.sh can compile the
// GLES2 variant through glslangValidator: the DRM target cannot be run on a
// machine with no display, and a shader that fails to compile would break it
// entirely with nothing else to catch it.
#include "scene_shader.inc"

bool RenderInit(void)
{
    g_render.shader = LoadShaderFromMemory(kVertexShader, kFragmentShader);
    if (g_render.shader.id == 0) {
        TraceLog(LOG_ERROR, "RENDER: scene shader failed to compile");
        return false;
    }
    g_render.shader.locs[SHADER_LOC_MATRIX_MODEL] = GetShaderLocation(g_render.shader, "matModel");
    g_render.shader.locs[SHADER_LOC_MATRIX_NORMAL] = GetShaderLocation(g_render.shader, "matNormal");
    g_render.locSunDir     = GetShaderLocation(g_render.shader, "sunDir");
    g_render.locSunColor   = GetShaderLocation(g_render.shader, "sunColor");
    g_render.locAmbient    = GetShaderLocation(g_render.shader, "ambient");
    g_render.locFogColor   = GetShaderLocation(g_render.shader, "fogColor");
    g_render.locFogDensity = GetShaderLocation(g_render.shader, "fogDensity");
    g_render.locCameraPos  = GetShaderLocation(g_render.shader, "cameraPos");

    g_render.locLightCount    = GetShaderLocation(g_render.shader, "lightCount");
    g_render.locLightPosRange = GetShaderLocation(g_render.shader, "lightPosRange");
    g_render.locLightColor    = GetShaderLocation(g_render.shader, "lightColor");
    g_render.locLightDir      = GetShaderLocation(g_render.shader, "lightDir");

    RenderSettings def = {
        .sunDirection = Vector3Normalize((Vector3){ -0.45f, -1.0f, -0.35f }),
        .skyColor = (Color){ 124, 176, 214, 255 },
        .groundColor = (Color){ 104, 152, 84, 255 },
        .ambient = (Color){ 88, 90, 100, 255 },
        .sunColor = (Color){ 255, 250, 235, 255 },
        .sunIntensity = 0.62f,
        .fogDensity = 0.012f,
    };
    RenderSetSettings(&def);

    g_render.groundMesh = MakeGroundQuad();
    g_render.sceneMaterial = LoadMaterialDefault();
    g_render.sceneMaterial.shader = g_render.shader;

    g_render.ready = true;
    return true;
}

void RenderShutdown(void)
{
    if (g_render.groundMesh.vertexCount > 0) UnloadMesh(g_render.groundMesh);
    // Detach first: the material does not own the shader.
    g_render.sceneMaterial.shader = (Shader){ 0 };
    if (g_render.sceneMaterial.maps) UnloadMaterial(g_render.sceneMaterial);
    if (g_render.shader.id != 0) UnloadShader(g_render.shader);
    memset(&g_render, 0, sizeof(g_render));
}

RenderSettings RenderDefaultSettings(const Level *level)
{
    RenderSettings s = {
        .sunDirection = level->sunDirection,
        .skyColor = level->skyColor,
        .groundColor = level->groundColor,
        .ambient = level->ambientColor,
        .sunColor = level->sunColor,
        .sunIntensity = level->sunIntensity,
        .fogDensity = 0.010f,
    };
    return s;
}

void RenderSetSettings(const RenderSettings *settings)
{
    g_render.settings = *settings;
    if (g_render.shader.id == 0) return;

    Vector3 sun = Vector3Normalize(settings->sunDirection);
    float ambient[4] = { settings->ambient.r / 255.0f, settings->ambient.g / 255.0f,
                         settings->ambient.b / 255.0f, 1.0f };
    float sunColor[4] = { settings->sunColor.r / 255.0f, settings->sunColor.g / 255.0f,
                          settings->sunColor.b / 255.0f, settings->sunIntensity };
    float fog[4] = { settings->skyColor.r / 255.0f, settings->skyColor.g / 255.0f,
                     settings->skyColor.b / 255.0f, 1.0f };

    SetShaderValue(g_render.shader, g_render.locSunDir, &sun, SHADER_UNIFORM_VEC3);
    SetShaderValue(g_render.shader, g_render.locSunColor, sunColor, SHADER_UNIFORM_VEC4);
    SetShaderValue(g_render.shader, g_render.locAmbient, ambient, SHADER_UNIFORM_VEC4);
    SetShaderValue(g_render.shader, g_render.locFogColor, fog, SHADER_UNIFORM_VEC4);
    SetShaderValue(g_render.shader, g_render.locFogDensity, &settings->fogDensity,
                   SHADER_UNIFORM_FLOAT);
}

RenderSettings RenderGetSettings(void)
{
    return g_render.settings;
}

// ---------------------------------------------------------------------------
// Light uploads
// ---------------------------------------------------------------------------

void RenderSetLights(const LightSet *lights)
{
    g_render.lights = lights;
}

// Uploads the lights that reach a bounding sphere. Called once per draw call,
// so a car is lit by the lamps near the car and a batch chunk by the lamps near
// that chunk, without either paying for the whole scene.
static void UploadLightsFor(Vector3 center, float radius)
{
    if (g_render.shader.id == 0) return;

    int count = 0;
    int chosen[LIGHTS_PER_DRAW];
    if (g_render.lights) {
        count = LightSetSelect(g_render.lights, center, radius, chosen, LIGHTS_PER_DRAW);
    }

    // Skip the upload when nothing is lit and nothing was lit last time.
    if (count == 0 && g_render.uploadedCount == 0) return;

    float posRange[LIGHTS_PER_DRAW * 4] = { 0 };
    float colors[LIGHTS_PER_DRAW * 4] = { 0 };
    float dirs[LIGHTS_PER_DRAW * 4] = { 0 };

    for (int i = 0; i < count; i++) {
        const Light *light = &g_render.lights->lights[chosen[i]];
        posRange[i * 4 + 0] = light->position.x;
        posRange[i * 4 + 1] = light->position.y;
        posRange[i * 4 + 2] = light->position.z;
        posRange[i * 4 + 3] = light->range;

        // Premultiply intensity so the shader does one multiply fewer.
        colors[i * 4 + 0] = light->color.r / 255.0f * light->intensity;
        colors[i * 4 + 1] = light->color.g / 255.0f * light->intensity;
        colors[i * 4 + 2] = light->color.b / 255.0f * light->intensity;
        // -2 marks a point light, which the shader tests for to skip the cone.
        colors[i * 4 + 3] = (light->type == LIGHT_SPOT)
            ? cosf(light->outerConeDeg * DEG2RAD) : -2.0f;

        dirs[i * 4 + 0] = light->direction.x;
        dirs[i * 4 + 1] = light->direction.y;
        dirs[i * 4 + 2] = light->direction.z;
        dirs[i * 4 + 3] = cosf(light->innerConeDeg * DEG2RAD);
    }

    SetShaderValue(g_render.shader, g_render.locLightCount, &count, SHADER_UNIFORM_INT);
    if (count > 0) {
        SetShaderValueV(g_render.shader, g_render.locLightPosRange, posRange,
                        SHADER_UNIFORM_VEC4, count);
        SetShaderValueV(g_render.shader, g_render.locLightColor, colors,
                        SHADER_UNIFORM_VEC4, count);
        SetShaderValueV(g_render.shader, g_render.locLightDir, dirs,
                        SHADER_UNIFORM_VEC4, count);
    }
    g_render.uploadedCount = count;
}

// ---------------------------------------------------------------------------
// Static batching
// ---------------------------------------------------------------------------

typedef struct ChunkKey { int x, z; } ChunkKey;

typedef struct ChunkBuild {
    ChunkKey key;
    int vertexCount;
    int written;
    float *vertices;
    float *normals;
    unsigned char *colors;
    BoundingBox bounds;
} ChunkBuild;

static Matrix PropMatrix(const LevelProp *p)
{
    Matrix s = MatrixScale(p->scale.x, p->scale.y, p->scale.z);
    Matrix r = MatrixRotateXYZ((Vector3){ p->rotationDeg.x * DEG2RAD,
                                          p->rotationDeg.y * DEG2RAD,
                                          p->rotationDeg.z * DEG2RAD });
    Matrix t = MatrixTranslate(p->position.x, p->position.y, p->position.z);
    return MatrixMultiply(MatrixMultiply(s, r), t);
}

// Vertex count a prop contributes, expanded to a triangle soup.
static int PropVertexCount(const LevelProp *p)
{
    Model *m = AssetsGetModel(p->model);
    int total = 0;
    for (int i = 0; i < m->meshCount; i++) total += m->meshes[i].triangleCount * 3;
    return total;
}

static int FindChunk(ChunkBuild *chunks, int count, ChunkKey key)
{
    for (int i = 0; i < count; i++) {
        if (chunks[i].key.x == key.x && chunks[i].key.z == key.z) return i;
    }
    return -1;
}

// Appends one prop's triangles into a chunk's arrays.
static void EmitProp(ChunkBuild *chunk, const LevelProp *prop)
{
    Model *model = AssetsGetModel(prop->model);
    Matrix world = PropMatrix(prop);
    // Normals need the inverse-transpose to survive non-uniform scaling.
    Matrix normalMat = MatrixTranspose(MatrixInvert(world));

    for (int mi = 0; mi < model->meshCount; mi++) {
        Mesh *mesh = &model->meshes[mi];
        if (!mesh->vertices) continue;

        Color matColor = WHITE;
        int matIndex = model->meshMaterial ? model->meshMaterial[mi] : 0;
        if (model->materials && matIndex >= 0 && matIndex < model->materialCount) {
            matColor = model->materials[matIndex].maps[MATERIAL_MAP_DIFFUSE].color;
        }
        unsigned char r = (unsigned char)((matColor.r * prop->tint.r) / 255);
        unsigned char g = (unsigned char)((matColor.g * prop->tint.g) / 255);
        unsigned char b = (unsigned char)((matColor.b * prop->tint.b) / 255);
        unsigned char a = (unsigned char)((matColor.a * prop->tint.a) / 255);

        int indexCount = mesh->triangleCount * 3;
        for (int k = 0; k < indexCount; k++) {
            int vi = mesh->indices ? (int)mesh->indices[k] : k;
            if (vi >= mesh->vertexCount) continue;

            Vector3 p = { mesh->vertices[vi * 3 + 0],
                          mesh->vertices[vi * 3 + 1],
                          mesh->vertices[vi * 3 + 2] };
            p = Vector3Transform(p, world);

            Vector3 n = { 0.0f, 1.0f, 0.0f };
            if (mesh->normals) {
                n = (Vector3){ mesh->normals[vi * 3 + 0],
                               mesh->normals[vi * 3 + 1],
                               mesh->normals[vi * 3 + 2] };
                n = Vector3Normalize(Vector3Transform(n, normalMat));
            }

            // Tint by world height, matching the ground, so the road darkens
            // into a dip and lightens over a crest instead of reading as one
            // flat ribbon from above.
            float shade = 1.0f - RELIEF_PROP_DEPTH * (1.0f - RenderReliefHeight01(p.y));

            int o = chunk->written;
            if (o >= chunk->vertexCount) return;   // sizing pass guarantees space
            chunk->vertices[o * 3 + 0] = p.x;
            chunk->vertices[o * 3 + 1] = p.y;
            chunk->vertices[o * 3 + 2] = p.z;
            chunk->normals[o * 3 + 0] = n.x;
            chunk->normals[o * 3 + 1] = n.y;
            chunk->normals[o * 3 + 2] = n.z;
            chunk->colors[o * 4 + 0] = (unsigned char)((float)r * shade);
            chunk->colors[o * 4 + 1] = (unsigned char)((float)g * shade);
            chunk->colors[o * 4 + 2] = (unsigned char)((float)b * shade);
            chunk->colors[o * 4 + 3] = a;
            chunk->written++;

            if (p.x < chunk->bounds.min.x) chunk->bounds.min.x = p.x;
            if (p.y < chunk->bounds.min.y) chunk->bounds.min.y = p.y;
            if (p.z < chunk->bounds.min.z) chunk->bounds.min.z = p.z;
            if (p.x > chunk->bounds.max.x) chunk->bounds.max.x = p.x;
            if (p.y > chunk->bounds.max.y) chunk->bounds.max.y = p.y;
            if (p.z > chunk->bounds.max.z) chunk->bounds.max.z = p.z;
        }
    }
}

bool StaticBatchBuild(StaticBatch *batch, const Level *level, float chunkSize)
{
    memset(batch, 0, sizeof(*batch));
    if (level->propCount <= 0) {
        batch->ready = true;
        return true;
    }
    if (chunkSize < 1.0f) chunkSize = 8.0f;

    // Pass 1: bucket props by chunk and measure each chunk's vertex count.
    // Splitting an oversized chunk is handled by giving it a unique key.
    int maxChunks = level->propCount + 8;
    ChunkBuild *chunks = calloc((size_t)maxChunks, sizeof(ChunkBuild));
    int *propChunk = calloc((size_t)level->propCount, sizeof(int));
    if (!chunks || !propChunk) { free(chunks); free(propChunk); return false; }

    int chunkCount = 0;
    for (int i = 0; i < level->propCount; i++) {
        const LevelProp *p = &level->props[i];
        ChunkKey key = { (int)floorf(p->position.x / chunkSize),
                         (int)floorf(p->position.z / chunkSize) };
        int ci = FindChunk(chunks, chunkCount, key);
        int verts = PropVertexCount(p);

        if (ci >= 0 && chunks[ci].vertexCount + verts > BATCH_MAX_VERTS_PER_CHUNK) {
            ci = -1;   // full: start another chunk with the same key
            for (int j = 0; j < chunkCount; j++) {
                if (chunks[j].key.x == key.x && chunks[j].key.z == key.z &&
                    chunks[j].vertexCount + verts <= BATCH_MAX_VERTS_PER_CHUNK) { ci = j; break; }
            }
        }
        if (ci < 0) {
            if (chunkCount >= maxChunks) { ci = 0; }
            else { ci = chunkCount++; chunks[ci].key = key; }
        }
        chunks[ci].vertexCount += verts;
        propChunk[i] = ci;
    }

    // Pass 2: allocate and fill.
    for (int c = 0; c < chunkCount; c++) {
        int n = chunks[c].vertexCount;
        chunks[c].vertices = MemAlloc((unsigned int)(sizeof(float) * 3 * (size_t)n));
        chunks[c].normals  = MemAlloc((unsigned int)(sizeof(float) * 3 * (size_t)n));
        chunks[c].colors   = MemAlloc((unsigned int)(sizeof(unsigned char) * 4 * (size_t)n));
        chunks[c].bounds.min = (Vector3){ 1e30f, 1e30f, 1e30f };
        chunks[c].bounds.max = (Vector3){ -1e30f, -1e30f, -1e30f };
        if (!chunks[c].vertices || !chunks[c].normals || !chunks[c].colors) {
            TraceLog(LOG_ERROR, "RENDER: out of memory baking static batch");
            for (int k = 0; k <= c; k++) {
                MemFree(chunks[k].vertices); MemFree(chunks[k].normals); MemFree(chunks[k].colors);
            }
            free(chunks); free(propChunk);
            return false;
        }
    }
    for (int i = 0; i < level->propCount; i++) EmitProp(&chunks[propChunk[i]], &level->props[i]);

    // Pass 3: upload.
    batch->chunks = calloc((size_t)chunkCount, sizeof(BatchChunk));
    if (!batch->chunks) { free(chunks); free(propChunk); return false; }

    int live = 0;
    for (int c = 0; c < chunkCount; c++) {
        if (chunks[c].written < 3) {
            MemFree(chunks[c].vertices); MemFree(chunks[c].normals); MemFree(chunks[c].colors);
            continue;
        }
        Mesh mesh = { 0 };
        mesh.vertexCount = chunks[c].written;
        mesh.triangleCount = chunks[c].written / 3;
        mesh.vertices = chunks[c].vertices;
        mesh.normals = chunks[c].normals;
        mesh.colors = chunks[c].colors;
        UploadMesh(&mesh, false);

        batch->chunks[live].mesh = mesh;
        batch->chunks[live].bounds = chunks[c].bounds;
        batch->totalTriangles += mesh.triangleCount;
        live++;
    }
    batch->chunkCount = live;

    batch->material = LoadMaterialDefault();
    batch->material.shader = g_render.shader;
    batch->ready = true;

    free(chunks);
    free(propChunk);
    TraceLog(LOG_INFO, "RENDER: baked %d props into %d chunks (%d triangles)",
             level->propCount, batch->chunkCount, batch->totalTriangles);
    return true;
}

void StaticBatchFree(StaticBatch *batch)
{
    for (int i = 0; i < batch->chunkCount; i++) UnloadMesh(batch->chunks[i].mesh);
    free(batch->chunks);
    // The material's shader is owned by the renderer, so detach before unloading.
    batch->material.shader = (Shader){ 0 };
    if (batch->material.maps) UnloadMaterial(batch->material);
    memset(batch, 0, sizeof(*batch));
}

// --- frustum culling --------------------------------------------------------

// Gribb-Hartmann plane extraction from a view-projection matrix.
static Frustum FrustumFromMatrix(Matrix m)
{
    Frustum f;
    f.planes[0] = (Vector4){ m.m3 + m.m0, m.m7 + m.m4, m.m11 + m.m8,  m.m15 + m.m12 }; // left
    f.planes[1] = (Vector4){ m.m3 - m.m0, m.m7 - m.m4, m.m11 - m.m8,  m.m15 - m.m12 }; // right
    f.planes[2] = (Vector4){ m.m3 + m.m1, m.m7 + m.m5, m.m11 + m.m9,  m.m15 + m.m13 }; // bottom
    f.planes[3] = (Vector4){ m.m3 - m.m1, m.m7 - m.m5, m.m11 - m.m9,  m.m15 - m.m13 }; // top
    f.planes[4] = (Vector4){ m.m3 + m.m2, m.m7 + m.m6, m.m11 + m.m10, m.m15 + m.m14 }; // near
    f.planes[5] = (Vector4){ m.m3 - m.m2, m.m7 - m.m6, m.m11 - m.m10, m.m15 - m.m14 }; // far
    for (int i = 0; i < 6; i++) {
        Vector4 p = f.planes[i];
        float len = sqrtf(p.x * p.x + p.y * p.y + p.z * p.z);
        if (len > 1e-6f) {
            f.planes[i] = (Vector4){ p.x / len, p.y / len, p.z / len, p.w / len };
        }
    }
    return f;
}

bool RenderFrustumTestBox(const Frustum *f, BoundingBox box)
{
    for (int i = 0; i < 6; i++) {
        Vector4 p = f->planes[i];
        // Positive vertex: the corner furthest along the plane normal.
        Vector3 v = { p.x >= 0 ? box.max.x : box.min.x,
                      p.y >= 0 ? box.max.y : box.min.y,
                      p.z >= 0 ? box.max.z : box.min.z };
        if (p.x * v.x + p.y * v.y + p.z * v.z + p.w < 0.0f) return false;
    }
    return true;
}

Frustum RenderFrustumFromCamera(Camera3D camera)
{
    float aspect = (float)GetScreenWidth() / (float)GetScreenHeight();
    Matrix view = GetCameraMatrix(camera);
    Matrix proj = (camera.projection == CAMERA_ORTHOGRAPHIC)
        ? MatrixOrtho(-camera.fovy / 2 * aspect, camera.fovy / 2 * aspect,
                      -camera.fovy / 2, camera.fovy / 2, 0.01f, 1000.0f)
        : MatrixPerspective(camera.fovy * DEG2RAD, aspect, 0.01f, 1000.0f);
    return FrustumFromMatrix(MatrixMultiply(view, proj));
}

void RenderDrawLitMesh(Mesh mesh, Material material, Matrix transform, BoundingBox bounds)
{
    Vector3 center = { (bounds.min.x + bounds.max.x) * 0.5f,
                       (bounds.min.y + bounds.max.y) * 0.5f,
                       (bounds.min.z + bounds.max.z) * 0.5f };
    UploadLightsFor(center, Vector3Distance(center, bounds.max));
    DrawMesh(mesh, material, transform);
}

Material RenderSceneMaterial(void)
{
    return g_render.sceneMaterial;
}

void StaticBatchDraw(StaticBatch *batch, Camera3D camera)
{
    batch->drawnLastFrame = 0;
    if (!batch->ready || batch->chunkCount == 0) return;

    Frustum frustum = RenderFrustumFromCamera(camera);
    for (int i = 0; i < batch->chunkCount; i++) {
        if (!RenderFrustumTestBox(&frustum, batch->chunks[i].bounds)) continue;
        RenderDrawLitMesh(batch->chunks[i].mesh, batch->material, MatrixIdentity(),
                          batch->chunks[i].bounds);
        batch->drawnLastFrame++;
    }
}

// ---------------------------------------------------------------------------
// Camera
// ---------------------------------------------------------------------------

void ChaseCameraInit(ChaseCamera *cam, Vector3 focus, float yaw)
{
    memset(cam, 0, sizeof(*cam));
    cam->focus = focus;
    cam->yaw = yaw;
    // High and only slightly behind: a top-down racer needs to show the corner
    // you are about to take, not the back of your own car.
    cam->distance = 3.4f;
    cam->height = 7.4f;
    cam->lookAhead = 1.7f;
    cam->rotateWithTarget = true;
    cam->positionSmoothing = 9.0f;
    cam->yawSmoothing = 5.0f;

    cam->camera.up = (Vector3){ 0.0f, 1.0f, 0.0f };
    cam->camera.fovy = 46.0f;
    cam->camera.projection = CAMERA_PERSPECTIVE;
    cam->camera.target = focus;
    cam->camera.position = (Vector3){ focus.x, focus.y + cam->height, focus.z - cam->distance };
}

// Framerate-independent exponential smoothing.
static float SmoothFactor(float rate, float dt) { return 1.0f - expf(-rate * dt); }

static float WrapAngle(float a)
{
    while (a > PI) a -= 2.0f * PI;
    while (a < -PI) a += 2.0f * PI;
    return a;
}

void ChaseCameraUpdate(ChaseCamera *cam, Vector3 target, float targetYaw, float speed01, float dt)
{
    if (dt <= 0.0f) dt = 1.0f / 60.0f;

    float kp = SmoothFactor(cam->positionSmoothing, dt);
    cam->focus.x += (target.x - cam->focus.x) * kp;
    cam->focus.y += (target.y - cam->focus.y) * kp;
    cam->focus.z += (target.z - cam->focus.z) * kp;

    if (cam->rotateWithTarget) {
        float ky = SmoothFactor(cam->yawSmoothing, dt);
        cam->yaw += WrapAngle(targetYaw - cam->yaw) * ky;
        cam->yaw = WrapAngle(cam->yaw);
    } else {
        cam->yaw = 0.0f;
    }

    // Pull back and rise a little with speed so fast sections read further ahead.
    float dist = cam->distance * (1.0f + 0.30f * speed01);
    float height = cam->height * (1.0f + 0.16f * speed01);
    Vector3 forward = { sinf(cam->yaw), 0.0f, cosf(cam->yaw) };

    Vector3 look = {
        cam->focus.x + forward.x * cam->lookAhead * speed01,
        cam->focus.y,
        cam->focus.z + forward.z * cam->lookAhead * speed01,
    };

    cam->camera.target = look;
    cam->camera.position = (Vector3){ look.x - forward.x * dist, look.y + height,
                                      look.z - forward.z * dist };
}

// ---------------------------------------------------------------------------
// Immediate-mode helpers
// ---------------------------------------------------------------------------

void RenderBeginScene(Camera3D camera, Color background)
{
    ClearBackground(background);
    BeginMode3D(camera);
    if (g_render.shader.id != 0) {
        float camPos[3] = { camera.position.x, camera.position.y, camera.position.z };
        SetShaderValue(g_render.shader, g_render.locCameraPos, camPos, SHADER_UNIFORM_VEC3);
    }
}

void RenderEndScene(void)
{
    EndMode3D();
}

void RenderModelEuler(Model *model, Vector3 position, Vector3 rotationDeg, Vector3 scale,
                      Color tint)
{
    Matrix m = MatrixMultiply(
        MatrixMultiply(MatrixScale(scale.x, scale.y, scale.z),
                       MatrixRotateXYZ((Vector3){ rotationDeg.x * DEG2RAD,
                                                  rotationDeg.y * DEG2RAD,
                                                  rotationDeg.z * DEG2RAD })),
        MatrixTranslate(position.x, position.y, position.z));
    RenderModelTransform(model, m, tint);
}

void RenderModelTransform(Model *model, Matrix m, Color tint)
{
    if (!model || model->meshCount == 0) return;

    // Approximate extent, used only to decide which lights are worth uploading.
    Vector3 position = { m.m12, m.m13, m.m14 };
    float reach = 1.2f * fmaxf(fabsf(m.m0) + fabsf(m.m4) + fabsf(m.m8),
                               fabsf(m.m2) + fabsf(m.m6) + fabsf(m.m10));
    UploadLightsFor(position, reach);

    for (int i = 0; i < model->meshCount; i++) {
        int matIndex = model->meshMaterial ? model->meshMaterial[i] : 0;
        // Material is a shallow struct whose `maps` array is shared with the
        // cached model, so tint by swapping the value in place and putting the
        // original back. Copying the struct and writing through it would
        // permanently darken the cached model a little more every frame.
        Material *mat = &model->materials[matIndex];
        Shader savedShader = mat->shader;
        Color base = mat->maps[MATERIAL_MAP_DIFFUSE].color;

        mat->shader = g_render.shader;
        mat->maps[MATERIAL_MAP_DIFFUSE].color = (Color){
            (unsigned char)((base.r * tint.r) / 255),
            (unsigned char)((base.g * tint.g) / 255),
            (unsigned char)((base.b * tint.b) / 255),
            (unsigned char)((base.a * tint.a) / 255),
        };
        DrawMesh(model->meshes[i], *mat, m);

        mat->maps[MATERIAL_MAP_DIFFUSE].color = base;
        mat->shader = savedShader;
    }
}

void RenderGroundPlane(Vector3 center, float size, Color color)
{
    // Drawn as a real mesh rather than through rlgl's immediate mode: the
    // immediate batch runs raylib's default shader, so the ground would ignore
    // the sun and every placed light and stay flat bright at night.
    if (g_render.groundMesh.vertexCount == 0) return;

    Matrix transform = MatrixMultiply(MatrixScale(size, 1.0f, size),
                                      MatrixTranslate(center.x, center.y, center.z));
    BoundingBox bounds = {
        { center.x - size * 0.5f, center.y, center.z - size * 0.5f },
        { center.x + size * 0.5f, center.y, center.z + size * 0.5f },
    };

    Material material = g_render.sceneMaterial;
    material.maps[MATERIAL_MAP_DIFFUSE].color = color;
    RenderDrawLitMesh(g_render.groundMesh, material, transform, bounds);
}

// ---------------------------------------------------------------------------
// Debug visualisation
// ---------------------------------------------------------------------------

void RenderDebugColliders(const Level *level, Color color)
{
    for (int i = 0; i < level->colliderCount; i++) {
        const LevelCollider *c = &level->colliders[i];
        rlPushMatrix();
        rlTranslatef(c->center.x, c->center.y + c->height * 0.5f, c->center.z);
        rlRotatef(c->yawDeg, 0.0f, 1.0f, 0.0f);
        DrawCubeWires((Vector3){ 0, 0, 0 }, c->halfExtents.x * 2.0f, c->height,
                      c->halfExtents.y * 2.0f, color);
        rlPopMatrix();
    }
}

void RenderDebugSpline(const Spline *spline, Color color)
{
    for (int i = 0; i < spline->count; i++) {
        const SplineSample *a = &spline->samples[i];
        const SplineSample *b = &spline->samples[(i + 1) % spline->count];
        Vector3 lift = { 0.0f, 0.05f, 0.0f };
        DrawLine3D(Vector3Add(a->position, lift), Vector3Add(b->position, lift), color);

        // Every eighth sample, show the drivable width as a cross-track tick.
        if (i % 8 == 0) {
            Vector3 left = { -a->tangent.z, 0.0f, a->tangent.x };
            Vector3 hw = Vector3Scale(left, a->width * 0.5f);
            DrawLine3D(Vector3Add(Vector3Add(a->position, hw), lift),
                       Vector3Add(Vector3Subtract(a->position, hw), lift), Fade(color, 0.5f));
        }
    }
}

void RenderDebugCheckpoints(const Level *level, Color color)
{
    for (int i = 0; i < level->checkpointCount; i++) {
        const LevelCheckpoint *cp = &level->checkpoints[i];
        float yaw = cp->yawDeg * DEG2RAD;
        // Gate faces down-track, so its bar runs across the track.
        Vector3 across = { cosf(yaw), 0.0f, -sinf(yaw) };
        Vector3 half = Vector3Scale(across, cp->width * 0.5f);
        Vector3 a = Vector3Subtract(cp->position, half);
        Vector3 b = Vector3Add(cp->position, half);
        Color c = (i == 0) ? WHITE : color;
        DrawLine3D(a, b, c);
        DrawLine3D(Vector3Add(a, (Vector3){ 0, 0.6f, 0 }), Vector3Add(b, (Vector3){ 0, 0.6f, 0 }), c);
        DrawLine3D(a, Vector3Add(a, (Vector3){ 0, 0.6f, 0 }), c);
        DrawLine3D(b, Vector3Add(b, (Vector3){ 0, 0.6f, 0 }), c);
    }
}
