// Top-down racer built on the engine layer, using the Kenney Racing Kit.
//
//   racer [options]
//     --level PATH        play this one circuit instead of the campaign
//     --racers N          size of the field, 1..8
//     --width/--height N  window size
//     --fullscreen        borderless fullscreen
//     --no-audio          skip the audio device
//     --no-vsync          uncap the frame rate
//     --no-shadows        skip the sun's shadow pass
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
#include "game/skid.h"

#define PHYSICS_HZ 120.0f
#define CAR_SCALE 0.40f

// Cars are drawn nosed up or down harder than the road really is. A 17% climb
// is only ten degrees of pitch, and ten degrees seen from a camera that is
// itself looking down at sixty is very nearly nothing. Only the drawn model is
// exaggerated — the shadow and the headlights still use the true angle.
#define CAR_PITCH_EXAGGERATION 2.2f

// How far ahead of the car the shadow box is centred. The chase camera shows
// much more road in front than behind, so a box centred on the car itself
// spends half its resolution on tarmac nobody is looking at.
#define SHADOW_LEAD 4.0f
#define SPLINE_SPACING 0.22f
#define BATCH_CHUNK_SIZE 6.0f
#define MAX_SHOTS 16

// Headlight placement, in car-local units.
#define HEADLIGHT_FORWARD 0.30f
#define HEADLIGHT_SIDE 0.10f
#define HEADLIGHT_HEIGHT 0.17f

// The circuits in the order the player meets them. Winning a race opens the
// next; losing offers the same one again. The names are for the results screen
// and are kept beside the paths so the two cannot drift apart.
static const char *kCampaign[] = {
    "levels/circuit01.level.json",
    "levels/circuit02.level.json",
};
static const char *kCampaignNames[] = {
    "ARDENNES CIRCUIT",
    "EIFEL NORDSCHLEIFE",
};
#define CAMPAIGN_COUNT ((int)(sizeof kCampaign / sizeof kCampaign[0]))

typedef struct Options {
    const char *levelPath;
    const char *shotPrefix;
    int racers;
    int width, height;
    bool fullscreen;
    bool audio;
    bool vsync;
    bool shadows;
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
        .levelPath = NULL,          // NULL means play the campaign
        .shotPrefix = "shot",
        .racers = 6,
        .width = 1280,
        .height = 720,
        .fullscreen = false,
        .audio = true,
        .vsync = true,
        .shadows = true,
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
        else if (!strcmp(a, "--no-shadows")) options->shadows = false;
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

    float s = sinf(racer->car.yaw), c = cosf(racer->car.yaw);
    float offsetX = (c * cx + s * cz) * scale;
    float offsetZ = (-s * cx + c * cz) * scale;

    // Pitch about the car's own lateral axis, so apply it before the yaw. An
    // XYZ euler triple cannot express that ordering, hence the explicit matrix.
    // car.pitch is the slope the car sits on, positive uphill. A positive
    // rotation about +X drops the nose, so it is negated here.
    float pitch = racer->car.pitch * CAR_PITCH_EXAGGERATION;

    // The model turns about its own origin, so pitching it swings one end below
    // that origin — and the harder it is pitched, the further. Lift by where the
    // pitched bounding box actually bottoms out rather than by its flat height,
    // or the tail digs into the road on every gradient. Rotating about X by
    // -pitch sends a corner to y*cos(pitch) + z*sin(pitch); the lowest one is
    // whichever end of the car the slope has dropped.
    float cp = cosf(pitch), sp = sinf(pitch);
    float lowest = bounds.min.y * cp + ((sp > 0.0f) ? bounds.min.z : bounds.max.z) * sp;
    float lift = -lowest;

    Vector3 position = { racer->car.position.x - offsetX,
                         racer->car.height + lift * scale,
                         racer->car.position.y - offsetZ };
    Matrix transform = MatrixMultiply(
        MatrixMultiply(MatrixScale(scale, scale, scale),
                       MatrixMultiply(MatrixRotateX(-pitch),
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

// ---------------------------------------------------------------------------
// One circuit's worth of state
// ---------------------------------------------------------------------------

// Everything tied to a single circuit, grouped rather than left as locals so a
// level can be torn down and another built in its place while the game runs.
// What survives a change of circuit - the camera, the skid marks, the fixed
// stepper, the day/night toggle - deliberately lives outside this.
typedef struct Stage {
    Level level;
    Spline spline;
    CollisionWorld collision;
    StaticBatch batch;
    Terrain terrain;
    Race race;

    LightSet lights;
    float baseIntensity[LIGHTS_MAX];
    int levelLightCount;
    Headlights headlights[RACE_MAX_RACERS];

    RenderSettings day;
    RenderSettings dark;
} Stage;

// Safe on a half-built stage: every one of these tolerates a zeroed struct,
// which is what lets StageLoad bail out through it at any point.
static void StageFree(Stage *stage)
{
    RaceFree(&stage->race);
    TerrainFree(&stage->terrain);
    StaticBatchFree(&stage->batch);
    CollisionWorldFree(&stage->collision);
    SplineFree(&stage->spline);
    LevelUnload(&stage->level);
    memset(stage, 0, sizeof(*stage));
}

static bool StageLoad(Stage *stage, const char *path, const Options *options, bool night)
{
    memset(stage, 0, sizeof(*stage));

    if (!LevelLoad(&stage->level, path)) return false;
    if (!SplineBuild(&stage->spline, &stage->level, SPLINE_SPACING)) {
        StageFree(stage);
        return false;
    }
    if (!CollisionWorldBuild(&stage->collision, stage->level.colliders,
                             stage->level.colliderCount, 2.0f)) {
        TraceLog(LOG_ERROR, "MAIN: collision build failed");
        StageFree(stage);
        return false;
    }

    stage->day = RenderDefaultSettings(&stage->level);
    stage->dark = NightSettings(stage->day);
    RenderSetSettings(night ? &stage->dark : &stage->day);

    // The track's own elevation range drives the relief tint, and both the
    // batch and the terrain have to be told before they bake their vertices.
    float lowest = 1e30f, highest = -1e30f;
    for (int i = 0; i < stage->spline.count; i++) {
        float y = stage->spline.samples[i].position.y;
        if (y < lowest) lowest = y;
        if (y > highest) highest = y;
    }
    RenderSetReliefRange(lowest, highest);
    TraceLog(LOG_INFO, "MAIN: %s climbs %.2f units (%.2f to %.2f)",
             stage->level.name, (double)(highest - lowest), (double)lowest, (double)highest);

    if (!StaticBatchBuild(&stage->batch, &stage->level, BATCH_CHUNK_SIZE)) {
        TraceLog(LOG_ERROR, "MAIN: static batch build failed");
    }

    TerrainSettings terrainSettings = TerrainDefaultSettings(stage->level.groundColor);
    if (!TerrainBuild(&stage->terrain, &stage->spline, &terrainSettings)) {
        TraceLog(LOG_ERROR, "MAIN: terrain build failed");
    }

    if (!RaceInit(&stage->race, &stage->level, &stage->spline, &stage->collision,
                  options->racers)) {
        TraceLog(LOG_ERROR, "MAIN: race init failed");
        StageFree(stage);
        return false;
    }
    stage->race.autopilot = options->autopilot;

    // Level lights first, then two headlight slots per car appended after them.
    LightSetClear(&stage->lights);
    LightSetAddFromLevel(&stage->lights, &stage->level);
    stage->levelLightCount = stage->lights.count;
    for (int i = 0; i < stage->levelLightCount; i++) {
        stage->baseIntensity[i] = stage->lights.lights[i].intensity;
    }
    AddHeadlights(&stage->lights, stage->headlights, stage->race.racerCount);
    ApplyLightMode(&stage->lights, stage->baseIntensity, stage->levelLightCount, night);
    RenderSetLights(&stage->lights);

    TraceLog(LOG_INFO, "MAIN: %s, %d lights (%d from level, %d headlights)",
             stage->level.name, stage->lights.count, stage->level.lightCount,
             stage->race.racerCount * 2);
    return true;
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

    // --level pins the game to one circuit; otherwise it is the campaign, and
    // winning opens the next one.
    const char *single[1] = { options.levelPath };
    const char **circuits = options.levelPath ? single : kCampaign;
    int circuitCount = options.levelPath ? 1 : CAMPAIGN_COUNT;
    int current = 0;

    bool night = options.night;

    static Stage stage;
    if (!StageLoad(&stage, circuits[current], &options, night)) {
        EngineShutdown();
        return 1;
    }

    if (!options.shadows) RenderSetShadowsEnabled(false);
    else if (!RenderShadowsAvailable()) {
        TraceLog(LOG_WARNING, "MAIN: no shadow map on this GPU, falling back to blob shadows");
    }

    const Racer *player = &stage.race.racers[stage.race.playerIndex];
    ChaseCamera camera;
    ChaseCameraInit(&camera,
                    (Vector3){ player->car.position.x, player->car.height,
                               player->car.position.y },
                    player->car.yaw);

    static SkidTrails skid;
    SkidInit(&skid);

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
            RenderSetSettings(night ? &stage.dark : &stage.day);
            ApplyLightMode(&stage.lights, stage.baseIntensity, stage.levelLightCount, night);
        }
        if (in.pressed[ACTION_QUIT]) break;
        if (in.pressed[ACTION_PAUSE]) paused = !paused;

        // Winning opens the next circuit; anything less is an invitation to
        // have another go at this one.
        bool won = (stage.race.racers[stage.race.playerIndex].progress.finishPosition == 1);
        bool advance = (stage.race.state == RACE_FINISHED) && won &&
                       (current + 1 < circuitCount);

        if (in.pressed[ACTION_RESET_CAR] && !paused) {
            if (stage.race.state == RACE_FINISHED) {
                RaceReset(&stage.race);
                SkidClear(&skid);
            } else {
                RaceRespawn(&stage.race, stage.race.playerIndex);
            }
        }
        if (stage.race.state == RACE_FINISHED && in.pressed[ACTION_CONFIRM]) {
            if (advance) {
                current++;
                StageFree(&stage);
                if (!StageLoad(&stage, circuits[current], &options, night)) {
                    TraceLog(LOG_ERROR, "MAIN: could not open %s", circuits[current]);
                    break;
                }
                player = &stage.race.racers[stage.race.playerIndex];
                ChaseCameraInit(&camera,
                                (Vector3){ player->car.position.x, player->car.height,
                                           player->car.position.y },
                                player->car.yaw);
            } else {
                RaceReset(&stage.race);
            }
            SkidClear(&skid);
        }

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
            for (int s = 0; s < steps; s++) RaceUpdate(&stage.race, drive, stepper.step);
            SkidUpdate(&skid, &stage.race, dt);
        }

        UpdateHeadlights(&stage.lights, stage.headlights, &stage.race, night);

        player = &stage.race.racers[stage.race.playerIndex];
        float speed01 = Clamp(player->car.speed / stage.race.tuning.topSpeed, 0.0f, 1.0f);
        ChaseCameraUpdate(&camera,
                          (Vector3){ player->car.position.x, player->car.height,
                                     player->car.position.y },
                          player->car.yaw, speed01, dt);

        // Engine note tracks revs; tyre scrub follows slip while on the ground.
        if (AudioEngineAvailable()) {
            float rpm = Clamp(fabsf(player->car.forwardSpeed) / stage.race.tuning.topSpeed,
                              0.0f, 1.0f);
            if (stage.race.state == RACE_COUNTDOWN) {
                rpm = 0.35f + 0.25f * sinf((float)GetTime() * 9.0f);
            }
            AudioEngineSetMotor(rpm, player->input.throttle,
                                player->car.slip * (player->car.onTrack ? 1.0f : 0.6f));
        }

        BeginDrawing();

        // Depth pass first: the same geometry, seen from the sun. Centred a
        // little ahead of the car so the box covers the road being driven into
        // rather than the one already behind.
        Vector2 lead = CarForward(&player->car);
        RenderBeginShadowPass((Vector3){ player->car.position.x + lead.x * SHADOW_LEAD,
                                        player->car.height,
                                        player->car.position.y + lead.y * SHADOW_LEAD });
            TerrainDraw(&stage.terrain, camera.camera);
            StaticBatchDraw(&stage.batch, camera.camera);
            for (int i = 0; i < stage.race.racerCount; i++) {
                DrawRacer(&stage.race.racers[i], CAR_SCALE);
            }
        RenderEndShadowPass();

        RenderBeginScene(camera.camera, stage.level.skyColor);
            TerrainDraw(&stage.terrain, camera.camera);
            StaticBatchDraw(&stage.batch, camera.camera);
            // Rubber lies in the road surface, so it goes down after the road
            // and before the cars, and never in the depth pass above, where a
            // flat decal has nothing to cast and only fights the tarmac.
            SkidDraw(&skid);
            // The painted blob stands in only when there is no real shadow to
            // cast one; drawing both would double up under every car.
            if (!RenderShadowsEnabled()) {
                for (int i = 0; i < stage.race.racerCount; i++) {
                    DrawShadow(&stage.race.racers[i], &stage.race.tuning);
                }
            }
            for (int i = 0; i < stage.race.racerCount; i++) {
                DrawRacer(&stage.race.racers[i], CAR_SCALE);
            }
            if (showDebug) {
                RenderDebugSpline(&stage.spline, (Color){ 90, 220, 255, 160 });
                RenderDebugCheckpoints(&stage.level, (Color){ 255, 210, 90, 200 });
                RenderDebugColliders(&stage.level, (Color){ 255, 80, 120, 90 });
            }
        RenderEndScene();

        // Recomputed from the current circuit rather than reusing `advance`,
        // which is stale the moment the player has moved on: `current` has
        // already stepped and current + 1 may be off the end of the campaign.
        bool nextExists = (current + 1 < circuitCount);
        HudProgress progress = {
            .hasNext = nextExists,
            .unlockedNext = nextExists && won && stage.race.state == RACE_FINISHED,
            .nextName = (nextExists && !options.levelPath) ? kCampaignNames[current + 1] : NULL,
        };
        HudDraw(&stage.race, paused, &progress);
        if (showDebug) {
            HudStats stats = {
                .fps = GetFPS(),
                .drawnChunks = stage.batch.drawnLastFrame,
                .totalChunks = stage.batch.chunkCount,
                .triangles = stage.batch.totalTriangles,
                .audioActive = AudioEngineAvailable(),
                .lightCount = stage.lights.count,
                .skidMarks = SkidLiveCount(&skid),
                .night = night,
            };
            HudDrawDebug(&stage.race, &stats);
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
        const Racer *p = &stage.race.racers[stage.race.playerIndex];
        TraceLog(LOG_INFO, "MAIN: %d frames, elapsed %.1fs, player lap %d, gate %d, "
                           "best %s, pos %d",
                 frame, stage.race.elapsed, p->progress.lap, p->progress.nextCheckpoint,
                 RaceFormatTime(p->progress.bestLapTime, buffer, sizeof buffer),
                 RacePositionOf(&stage.race, stage.race.playerIndex));
    }

    StageFree(&stage);
    EngineShutdown();
    return 0;
}
