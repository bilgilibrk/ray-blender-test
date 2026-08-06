// Top-down racer built on the engine layer, using the Kenney Racing Kit.
//
//   racer [options]
//     --level PATH        level JSON to load (default levels/circuit01.level.json)
//     --racers N          size of the field, 1..8
//     --width/--height N  window size
//     --fullscreen        borderless fullscreen
//     --no-audio          skip the audio device
//     --no-vsync          uncap the frame rate
//     --autopilot         let the AI drive the player's car
//     --debug             start with the debug overlay on
//     --night             start at night, with the placed lights and headlights
//     --frames N          quit after N frames (for automated runs)
//     --shots a,b,c       screenshot on those frame numbers
//     --shot-prefix P     screenshot filename prefix (default "shot")

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "raylib.h"
#include "raymath.h"

#include "engine/assets.h"
#include "engine/audio.h"
#include "engine/collide.h"
#include "engine/core.h"
#include "engine/input.h"
#include "engine/level.h"
#include "engine/light.h"
#include "engine/render.h"
#include "engine/spline.h"
#include "engine/terrain.h"

#include "game/car.h"
#include "game/hud.h"
#include "game/race.h"

#define PHYSICS_HZ 120.0f
#define CAR_SCALE 0.40f
#define SPLINE_SPACING 0.22f
#define BATCH_CHUNK_SIZE 6.0f
#define MAX_SHOTS 16

// Headlight placement, in car-local units.
#define HEADLIGHT_FORWARD 0.30f
#define HEADLIGHT_SIDE 0.10f
#define HEADLIGHT_HEIGHT 0.17f

typedef struct Options {
    const char *levelPath;
    const char *shotPrefix;
    int racers;
    int width, height;
    bool fullscreen;
    bool audio;
    bool vsync;
    bool autopilot;
    bool debug;
    bool night;
    int frameLimit;
    int shots[MAX_SHOTS];
    int shotCount;
} Options;

static Options DefaultOptions(void)
{
    Options o = {
        .levelPath = "levels/circuit01.level.json",
        .shotPrefix = "shot",
        .racers = 6,
        .width = 1280,
        .height = 720,
        .fullscreen = false,
        .audio = true,
        .vsync = true,
        .autopilot = false,
        .debug = false,
        .night = false,
        .frameLimit = 0,
        .shotCount = 0,
    };
    return o;
}

static void ParseShots(Options *options, const char *list)
{
    const char *p = list;
    while (*p && options->shotCount < MAX_SHOTS) {
        char *end = NULL;
        long value = strtol(p, &end, 10);
        if (end == p) break;
        if (value > 0) options->shots[options->shotCount++] = (int)value;
        p = (*end == ',') ? end + 1 : end;
    }
}

static bool ParseArgs(Options *options, int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        bool hasNext = (i + 1 < argc);

        if (!strcmp(a, "--level") && hasNext) options->levelPath = argv[++i];
        else if (!strcmp(a, "--racers") && hasNext) options->racers = atoi(argv[++i]);
        else if (!strcmp(a, "--width") && hasNext) options->width = atoi(argv[++i]);
        else if (!strcmp(a, "--height") && hasNext) options->height = atoi(argv[++i]);
        else if (!strcmp(a, "--shot-prefix") && hasNext) options->shotPrefix = argv[++i];
        else if (!strcmp(a, "--frames") && hasNext) options->frameLimit = atoi(argv[++i]);
        else if (!strcmp(a, "--shots") && hasNext) ParseShots(options, argv[++i]);
        else if (!strcmp(a, "--fullscreen")) options->fullscreen = true;
        else if (!strcmp(a, "--no-audio")) options->audio = false;
        else if (!strcmp(a, "--no-vsync")) options->vsync = false;
        else if (!strcmp(a, "--autopilot")) options->autopilot = true;
        else if (!strcmp(a, "--debug")) options->debug = true;
        else if (!strcmp(a, "--night")) options->night = true;
        else {
            fprintf(stderr, "racer: unknown option '%s'\n", a);
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------

// Places a car model so its footprint centre sits on the car's position and its
// wheels rest on the ground, whatever origin the artist happened to use.
static void DrawRacer(const Racer *racer, float scale)
{
    Model *model = AssetsGetModel(racer->model);
    BoundingBox bounds = AssetsGetModelBounds(racer->model);

    float cx = (bounds.min.x + bounds.max.x) * 0.5f;
    float cz = (bounds.min.z + bounds.max.z) * 0.5f;
    float lift = -bounds.min.y;

    float s = sinf(racer->car.yaw), c = cosf(racer->car.yaw);
    float offsetX = (c * cx + s * cz) * scale;
    float offsetZ = (-s * cx + c * cz) * scale;

    Vector3 position = { racer->car.position.x - offsetX,
                         racer->car.height + lift * scale,
                         racer->car.position.y - offsetZ };

    // Pitch about the car's own lateral axis, so apply it before the yaw. An
    // XYZ euler triple cannot express that ordering, hence the explicit matrix.
    // car.pitch is the slope the car sits on, positive uphill. A positive
    // rotation about +X drops the nose, so it is negated here.
    Matrix transform = MatrixMultiply(
        MatrixMultiply(MatrixScale(scale, scale, scale),
                       MatrixMultiply(MatrixRotateX(-racer->car.pitch),
                                      MatrixRotateY(racer->car.yaw))),
        MatrixTranslate(position.x, position.y, position.z));
    RenderModelTransform(model, transform, racer->tint);
}

static void DrawShadow(const Racer *racer, const CarTuning *tuning)
{
    // A flat quad under the car reads better than a real shadow at this scale.
    Vector2 corners[4];
    Obb2 box = CarBox(&racer->car, tuning);
    box.halfExtents.x *= 1.05f;
    box.halfExtents.y *= 1.05f;
    Obb2Corners(box, corners);

    // Lift each corner along the slope so the patch lies on the road rather
    // than cutting into a hill.
    Vector2 forward = CarForward(&racer->car);
    float slope = tanf(racer->car.pitch);
    Vector3 lifted[4];
    for (int i = 0; i < 4; i++) {
        float along = (corners[i].x - racer->car.position.x) * forward.x +
                      (corners[i].y - racer->car.position.y) * forward.y;
        lifted[i] = (Vector3){ corners[i].x,
                               racer->car.height + along * slope + 0.012f,
                               corners[i].y };
    }

    Color shade = { 20, 30, 24, 90 };
    DrawTriangle3D(lifted[0], lifted[1], lifted[2], shade);
    DrawTriangle3D(lifted[0], lifted[2], lifted[3], shade);
}

// --- lighting ---------------------------------------------------------------

typedef struct Headlights {
    int left;
    int right;
} Headlights;

static Color ScaleColor(Color color, float factor)
{
    return (Color){
        (unsigned char)Clamp(color.r * factor, 0.0f, 255.0f),
        (unsigned char)Clamp(color.g * factor, 0.0f, 255.0f),
        (unsigned char)Clamp(color.b * factor, 0.0f, 255.0f),
        color.a,
    };
}

// Dims the key light and sky so the placed lights and headlights carry the
// scene. Prop colours are untouched — the shader darkens them.
static RenderSettings NightSettings(RenderSettings day)
{
    RenderSettings night = day;
    night.sunIntensity = day.sunIntensity * 0.10f;
    night.sunColor = (Color){ 150, 170, 220, 255 };   // cool moonlight
    night.ambient = ScaleColor(day.ambient, 0.28f);
    night.skyColor = ScaleColor(day.skyColor, 0.16f);
    night.fogDensity = day.fogDensity * 1.5f;
    return night;
}

// Placed lights are dimmed rather than switched off during the day: a street
// lamp does not light much at noon, but leaving a hint of the pool visible
// makes it obvious the level has lighting in it at all.
#define LIGHT_DAY_SCALE 0.30f

static void ApplyLightMode(LightSet *lights, const float *baseIntensity, int levelLightCount,
                           bool night)
{
    float scale = night ? 1.0f : LIGHT_DAY_SCALE;
    for (int i = 0; i < levelLightCount && i < lights->count; i++) {
        lights->lights[i].intensity = baseIntensity[i] * scale;
    }
}

static void AddHeadlights(LightSet *lights, Headlights *out, int count)
{
    for (int i = 0; i < count; i++) {
        Light beam = LightMakeSpot((Vector3){ 0 }, (Vector3){ 0, 0, 1 },
                                   (Color){ 255, 244, 214, 255 }, 2.4f, 4.6f, 14.0f, 30.0f);
        beam.enabled = false;   // switched on with night mode
        out[i].left = LightSetAdd(lights, beam);
        out[i].right = LightSetAdd(lights, beam);
    }
}

// Re-aims each car's pair of beams from its current transform.
static void UpdateHeadlights(LightSet *lights, const Headlights *slots, const Race *race,
                             bool enabled)
{
    for (int i = 0; i < race->racerCount; i++) {
        const Car *car = &race->racers[i].car;
        Vector2 forward = CarForward(car);
        Vector2 right = CarRight(car);
        // Aim slightly down so the beam lands on the road ahead.
        Vector3 direction = { forward.x, -0.22f + sinf(car->pitch), forward.y };

        for (int side = 0; side < 2; side++) {
            int index = side ? slots[i].right : slots[i].left;
            Light *light = LightSetAt(lights, index);
            if (!light) continue;

            float lateral = side ? HEADLIGHT_SIDE : -HEADLIGHT_SIDE;
            light->position = (Vector3){
                car->position.x + forward.x * HEADLIGHT_FORWARD + right.x * lateral,
                car->height + HEADLIGHT_HEIGHT,
                car->position.y + forward.y * HEADLIGHT_FORWARD + right.y * lateral,
            };
            light->direction = Vector3Normalize(direction);
            light->enabled = enabled;
        }
    }
}

// ---------------------------------------------------------------------------

int main(int argc, char **argv)
{
    Options options = DefaultOptions();
    if (!ParseArgs(&options, argc, argv)) return 2;

    EngineConfig config = EngineDefaultConfig();
    config.title = "Kenney Top-Down Racer";
    config.width = options.width;
    config.height = options.height;
    config.fullscreen = options.fullscreen;
    config.vsync = options.vsync;
    config.audio = options.audio;
    config.assetRoot = "assets";
    if (!EngineInit(&config)) return 1;

    Level level;
    if (!LevelLoad(&level, options.levelPath)) {
        EngineShutdown();
        return 1;
    }

    Spline spline;
    if (!SplineBuild(&spline, &level, SPLINE_SPACING)) {
        LevelUnload(&level);
        EngineShutdown();
        return 1;
    }

    CollisionWorld collision;
    if (!CollisionWorldBuild(&collision, level.colliders, level.colliderCount, 2.0f)) {
        TraceLog(LOG_ERROR, "MAIN: collision build failed");
        SplineFree(&spline);
        LevelUnload(&level);
        EngineShutdown();
        return 1;
    }

    RenderSettings daySettings = RenderDefaultSettings(&level);
    RenderSettings nightSettings = NightSettings(daySettings);
    bool night = options.night;
    RenderSetSettings(night ? &nightSettings : &daySettings);

    StaticBatch batch;
    if (!StaticBatchBuild(&batch, &level, BATCH_CHUNK_SIZE)) {
        TraceLog(LOG_ERROR, "MAIN: static batch build failed");
    }

    Terrain terrain;
    TerrainSettings terrainSettings = TerrainDefaultSettings(level.groundColor);
    if (!TerrainBuild(&terrain, &spline, &terrainSettings)) {
        TraceLog(LOG_ERROR, "MAIN: terrain build failed");
    }

    Race race;
    if (!RaceInit(&race, &level, &spline, &collision, options.racers)) {
        TraceLog(LOG_ERROR, "MAIN: race init failed");
        StaticBatchFree(&batch);
        CollisionWorldFree(&collision);
        SplineFree(&spline);
        LevelUnload(&level);
        EngineShutdown();
        return 1;
    }
    race.autopilot = options.autopilot;

    // Level lights first, then two headlight slots per car appended after them.
    static LightSet lights;
    LightSetClear(&lights);
    LightSetAddFromLevel(&lights, &level);
    static float baseIntensity[LIGHTS_MAX];
    int levelLightCount = lights.count;
    for (int i = 0; i < levelLightCount; i++) baseIntensity[i] = lights.lights[i].intensity;

    Headlights headlights[RACE_MAX_RACERS] = { 0 };
    AddHeadlights(&lights, headlights, race.racerCount);
    ApplyLightMode(&lights, baseIntensity, levelLightCount, night);
    RenderSetLights(&lights);
    TraceLog(LOG_INFO, "MAIN: %d lights (%d from level, %d headlights)",
             lights.count, level.lightCount, race.racerCount * 2);

    const Racer *player = &race.racers[race.playerIndex];
    ChaseCamera camera;
    ChaseCameraInit(&camera,
                    (Vector3){ player->car.position.x, player->car.height,
                               player->car.position.y },
                    player->car.yaw);

    FixedStepper stepper;
    FixedStepperInit(&stepper, PHYSICS_HZ, 8);

    bool showDebug = options.debug;
    bool paused = false;
    int frame = 0;
    int nextShot = 0;

    while (!WindowShouldClose()) {
        InputState in;
        InputUpdate(&in);

        if (in.pressed[ACTION_TOGGLE_DEBUG]) showDebug = !showDebug;
        if (in.pressed[ACTION_TOGGLE_CAMERA]) camera.rotateWithTarget = !camera.rotateWithTarget;
        if (in.pressed[ACTION_TOGGLE_NIGHT]) {
            night = !night;
            RenderSetSettings(night ? &nightSettings : &daySettings);
            ApplyLightMode(&lights, baseIntensity, levelLightCount, night);
        }
        if (in.pressed[ACTION_QUIT]) break;
        if (in.pressed[ACTION_PAUSE]) paused = !paused;
        if (in.pressed[ACTION_RESET_CAR] && !paused) {
            if (race.state == RACE_FINISHED) RaceReset(&race);
            else RaceRespawn(&race, race.playerIndex);
        }
        if (race.state == RACE_FINISHED && in.pressed[ACTION_CONFIRM]) RaceReset(&race);

        CarInput drive = {
            .throttle = in.throttle,
            .brake = in.brake,
            .steer = in.steer,
            .handbrake = in.handbrake,
        };

        float dt = GetFrameTime();
        // A headless run has no vsync to pace it, so use the fixed step directly.
        if (options.frameLimit > 0) dt = 1.0f / 60.0f;

        if (!paused) {
            int steps = FixedStepperAdvance(&stepper, dt);
            for (int s = 0; s < steps; s++) RaceUpdate(&race, drive, stepper.step);
        }

        UpdateHeadlights(&lights, headlights, &race, night);

        player = &race.racers[race.playerIndex];
        float speed01 = Clamp(player->car.speed / race.tuning.topSpeed, 0.0f, 1.0f);
        ChaseCameraUpdate(&camera,
                          (Vector3){ player->car.position.x, player->car.height,
                                     player->car.position.y },
                          player->car.yaw, speed01, dt);

        // Engine note tracks revs; tyre scrub follows slip while on the ground.
        if (AudioEngineAvailable()) {
            float rpm = Clamp(fabsf(player->car.forwardSpeed) / race.tuning.topSpeed, 0.0f, 1.0f);
            if (race.state == RACE_COUNTDOWN) rpm = 0.35f + 0.25f * sinf((float)GetTime() * 9.0f);
            AudioEngineSetMotor(rpm, player->input.throttle,
                                player->car.slip * (player->car.onTrack ? 1.0f : 0.6f));
        }

        BeginDrawing();
        RenderBeginScene(camera.camera, level.skyColor);
            TerrainDraw(&terrain, camera.camera);
            StaticBatchDraw(&batch, camera.camera);
            for (int i = 0; i < race.racerCount; i++) DrawShadow(&race.racers[i], &race.tuning);
            for (int i = 0; i < race.racerCount; i++) DrawRacer(&race.racers[i], CAR_SCALE);
            if (showDebug) {
                RenderDebugSpline(&spline, (Color){ 90, 220, 255, 160 });
                RenderDebugCheckpoints(&level, (Color){ 255, 210, 90, 200 });
                RenderDebugColliders(&level, (Color){ 255, 80, 120, 90 });
            }
        RenderEndScene();

        HudDraw(&race, paused);
        if (showDebug) {
            HudStats stats = {
                .fps = GetFPS(),
                .drawnChunks = batch.drawnLastFrame,
                .totalChunks = batch.chunkCount,
                .triangles = batch.totalTriangles,
                .audioActive = AudioEngineAvailable(),
                .lightCount = lights.count,
                .night = night,
            };
            HudDrawDebug(&race, &stats);
        }
        EndDrawing();

        frame++;
        if (in.pressed[ACTION_SCREENSHOT]) {
            EngineScreenshot(TextFormat("%s-%04d.png", options.shotPrefix, frame));
        }
        while (nextShot < options.shotCount && options.shots[nextShot] <= frame) {
            EngineScreenshot(TextFormat("%s-%04d.png", options.shotPrefix, frame));
            nextShot++;
        }
        if (options.frameLimit > 0 && frame >= options.frameLimit) break;
    }

    // Report something useful when a headless run ends.
    if (options.frameLimit > 0) {
        char buffer[32];
        const Racer *p = &race.racers[race.playerIndex];
        TraceLog(LOG_INFO, "MAIN: %d frames, elapsed %.1fs, player lap %d, gate %d, "
                           "best %s, pos %d",
                 frame, race.elapsed, p->progress.lap, p->progress.nextCheckpoint,
                 RaceFormatTime(p->progress.bestLapTime, buffer, sizeof buffer),
                 RacePositionOf(&race, race.playerIndex));
    }

    RaceFree(&race);
    TerrainFree(&terrain);
    StaticBatchFree(&batch);
    CollisionWorldFree(&collision);
    SplineFree(&spline);
    LevelUnload(&level);
    EngineShutdown();
    return 0;
}
