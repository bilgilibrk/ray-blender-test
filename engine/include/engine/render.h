// Scene rendering.
//
// The Kenney kit is untextured flat-shaded geometry, so every static prop in a
// level is baked once into vertex-coloured meshes grouped by spatial chunk.
// That turns a few hundred DrawModel calls into a handful of DrawMesh calls
// and makes frustum culling cheap, which matters on a Raspberry Pi.
#ifndef ENGINE_RENDER_H
#define ENGINE_RENDER_H

#include <stdbool.h>

#include "raylib.h"

#include "engine/level.h"
#include "engine/spline.h"

typedef struct RenderSettings {
    Vector3 sunDirection;   // direction the light travels
    Color skyColor;
    Color groundColor;
    Color ambient;
    float fogDensity;       // 0 disables distance fog
} RenderSettings;

bool RenderInit(void);
void RenderShutdown(void);
void RenderSetSettings(const RenderSettings *settings);
RenderSettings RenderDefaultSettings(const Level *level);

// --- static geometry -------------------------------------------------------

typedef struct BatchChunk {
    Mesh mesh;
    BoundingBox bounds;
} BatchChunk;

typedef struct StaticBatch {
    BatchChunk *chunks;
    int chunkCount;
    Material material;
    int totalTriangles;
    int drawnLastFrame;
    bool ready;
} StaticBatch;

// Bakes every level prop into chunk meshes. `chunkSize` is in world units.
bool StaticBatchBuild(StaticBatch *batch, const Level *level, float chunkSize);
void StaticBatchFree(StaticBatch *batch);
void StaticBatchDraw(StaticBatch *batch, Camera3D camera);

// --- camera ----------------------------------------------------------------

typedef struct ChaseCamera {
    Camera3D camera;
    Vector3 focus;          // smoothed point being looked at
    float yaw;              // smoothed heading, radians
    float distance;         // horizontal offset behind the focus
    float height;
    float lookAhead;        // how far down the velocity vector to bias the focus
    bool rotateWithTarget;  // false keeps the map north-up
    float positionSmoothing;
    float yawSmoothing;
} ChaseCamera;

void ChaseCameraInit(ChaseCamera *cam, Vector3 focus, float yaw);
void ChaseCameraUpdate(ChaseCamera *cam, Vector3 target, float targetYaw, float speed01, float dt);

// --- immediate draws ---------------------------------------------------------

void RenderBeginScene(Camera3D camera, Color background);
void RenderEndScene(void);

// Draws a model with an XYZ euler rotation in degrees.
void RenderModelEuler(Model *model, Vector3 position, Vector3 rotationDeg, Vector3 scale,
                      Color tint);

// A large flat quad under the track so gaps between tiles are not the void.
void RenderGroundPlane(Vector3 center, float size, Color color);

// --- debug -------------------------------------------------------------------

void RenderDebugColliders(const Level *level, Color color);
void RenderDebugSpline(const Spline *spline, Color color);
void RenderDebugCheckpoints(const Level *level, Color color);

#endif // ENGINE_RENDER_H
