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
#include "engine/render.h"
#include "engine/spline.h"

#include "game/car.h"
#include "game/hud.h"
#include "game/race.h"

#define PHYSICS_HZ 120.0f
#define CAR_SCALE 0.40f
#define SPLINE_SPACING 0.22f
#define BATCH_CHUNK_SIZE 6.0f
#define MAX_SHOTS 16

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
                         lift * scale,
                         racer->car.position.y - offsetZ };
    RenderModelEuler(model, position, (Vector3){ 0.0f, racer->car.yaw * RAD2DEG, 0.0f },
                     (Vector3){ scale, scale, scale }, racer->tint);
}

static void DrawShadow(const Racer *racer, const CarTuning *tuning)
{
    // A flat quad under the car reads better than a real shadow at this scale.
    Vector2 corners[4];
    Obb2 box = CarBox(&racer->car, tuning);
    box.halfExtents.x *= 1.05f;
    box.halfExtents.y *= 1.05f;
    Obb2Corners(box, corners);

    Color shade = { 20, 30, 24, 90 };
    DrawTriangle3D((Vector3){ corners[0].x, 0.012f, corners[0].y },
                   (Vector3){ corners[1].x, 0.012f, corners[1].y },
                   (Vector3){ corners[2].x, 0.012f, corners[2].y }, shade);
    DrawTriangle3D((Vector3){ corners[0].x, 0.012f, corners[0].y },
                   (Vector3){ corners[2].x, 0.012f, corners[2].y },
                   (Vector3){ corners[3].x, 0.012f, corners[3].y }, shade);
}

static Vector3 LevelCentroid(const Spline *spline)
{
    Vector3 sum = { 0 };
    if (spline->count == 0) return sum;
    for (int i = 0; i < spline->count; i++) {
        sum.x += spline->samples[i].position.x;
        sum.z += spline->samples[i].position.z;
    }
    sum.x /= (float)spline->count;
    sum.z /= (float)spline->count;
    return sum;
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

    RenderSettings settings = RenderDefaultSettings(&level);
    RenderSetSettings(&settings);

    StaticBatch batch;
    if (!StaticBatchBuild(&batch, &level, BATCH_CHUNK_SIZE)) {
        TraceLog(LOG_ERROR, "MAIN: static batch build failed");
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

    const Racer *player = &race.racers[race.playerIndex];
    ChaseCamera camera;
    ChaseCameraInit(&camera, (Vector3){ player->car.position.x, 0.0f, player->car.position.y },
                    player->car.yaw);

    Vector3 groundCentre = LevelCentroid(&spline);
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

        player = &race.racers[race.playerIndex];
        float speed01 = Clamp(player->car.speed / race.tuning.topSpeed, 0.0f, 1.0f);
        ChaseCameraUpdate(&camera,
                          (Vector3){ player->car.position.x, 0.0f, player->car.position.y },
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
            RenderGroundPlane((Vector3){ groundCentre.x, -0.02f, groundCentre.z }, 260.0f,
                              level.groundColor);
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
    StaticBatchFree(&batch);
    CollisionWorldFree(&collision);
    SplineFree(&spline);
    LevelUnload(&level);
    EngineShutdown();
    return 0;
}
