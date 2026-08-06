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
#include "engine/light.h"
#include "engine/spline.h"

typedef struct RenderSettings {
    Vector3 sunDirection;   // direction the light travels
    Color skyColor;
    Color groundColor;
    Color ambient;
    Color sunColor;
    float sunIntensity;     // 0 leaves only ambient and the placed lights
    float fogDensity;       // 0 disables distance fog
} RenderSettings;

bool RenderInit(void);
void RenderShutdown(void);
void RenderSetSettings(const RenderSettings *settings);
RenderSettings RenderGetSettings(void);
RenderSettings RenderDefaultSettings(const Level *level);

// Point and spot lights used by later draws. The set is borrowed, not copied,
// so lights can be moved between frames (headlights, flashing beacons) without
// telling the renderer again. Pass NULL to light with the sun alone.
void RenderSetLights(const LightSet *lights);

// --- relief shading ----------------------------------------------------------
//
// A camera looking almost straight down cannot show elevation on its own: a
// 17% slope tilts its normal by ten degrees and so lights almost exactly like
// the flat beside it. Everything baked into a mesh is therefore tinted by how
// high it sits, dips darkening and crests lightening, which is what lets a
// climb read as a climb from above.
//
// Set the level's height range once, before building the terrain or the static
// batch, so the road and the ground either side of it shade together.
void RenderSetReliefRange(float lowest, float highest);

// Where `y` sits in that range: 0 at the bottom, 1 at the top. Returns 0.5 —
// a neutral tint — until a range has been set.
float RenderReliefHeight01(float y);

// --- culling and lit draws --------------------------------------------------

typedef struct Frustum {
    Vector4 planes[6];      // left, right, bottom, top, near, far
} Frustum;

Frustum RenderFrustumFromCamera(Camera3D camera);
bool RenderFrustumTestBox(const Frustum *frustum, BoundingBox box);

// Uploads the lights that reach `bounds`, then draws. Anything built out of
// chunks (the static batch, the terrain) goes through here so each chunk is lit
// by its own neighbourhood rather than by the scene's brightest lights.
void RenderDrawLitMesh(Mesh mesh, Material material, Matrix transform, BoundingBox bounds);

// A material set up to render with the scene shader. Meshes built by other
// modules use this so they pick up lighting and fog.
Material RenderSceneMaterial(void);

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

// Draws a model under an arbitrary transform. Needed when a rotation cannot be
// written as an XYZ euler triple in the order the level format uses — a car
// pitched about its own lateral axis after yawing, for instance.
void RenderModelTransform(Model *model, Matrix transform, Color tint);

// A large flat quad under the track so gaps between tiles are not the void.
void RenderGroundPlane(Vector3 center, float size, Color color);

// --- debug -------------------------------------------------------------------

void RenderDebugColliders(const Level *level, Color color);
void RenderDebugSpline(const Spline *spline, Color color);
void RenderDebugCheckpoints(const Level *level, Color color);

#endif // ENGINE_RENDER_H
