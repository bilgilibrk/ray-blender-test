#include "engine/core.h"

#include <string.h>

#include "engine/assets.h"
#include "engine/audio.h"
#include "engine/input.h"
#include "engine/render.h"

EngineConfig EngineDefaultConfig(void)
{
    EngineConfig cfg = {
        .title = "raylib engine",
        .width = 1280,
        .height = 720,
        .targetFPS = 60,
        .vsync = true,
        .msaa = true,
        .fullscreen = false,
        .audio = true,
        .assetRoot = "assets",
    };
    return cfg;
}

bool EngineInit(const EngineConfig *config)
{
    unsigned int flags = 0;
    if (config->vsync) flags |= FLAG_VSYNC_HINT;
    if (config->msaa) flags |= FLAG_MSAA_4X_HINT;
    if (config->fullscreen) flags |= FLAG_FULLSCREEN_MODE;
    SetConfigFlags(flags);

    InitWindow(config->width, config->height, config->title);
    if (!IsWindowReady()) {
        TraceLog(LOG_ERROR, "CORE: window creation failed");
        return false;
    }
    if (config->targetFPS > 0) SetTargetFPS(config->targetFPS);

    // Escape is handled as a game action so pause menus can intercept it.
    SetExitKey(KEY_NULL);

    AssetsInit(config->assetRoot ? config->assetRoot : "assets");
    InputInit();

    if (!RenderInit()) {
        TraceLog(LOG_ERROR, "CORE: renderer init failed");
        AssetsShutdown();
        CloseWindow();
        return false;
    }
    if (config->audio) AudioEngineInit();   // silent fallback is fine

    TraceLog(LOG_INFO, "CORE: engine ready (%dx%d)", GetScreenWidth(), GetScreenHeight());
    return true;
}

void EngineShutdown(void)
{
    AudioEngineShutdown();
    RenderShutdown();
    AssetsShutdown();
    if (IsWindowReady()) CloseWindow();
}

void FixedStepperInit(FixedStepper *stepper, float hz, int maxStepsPerFrame)
{
    if (hz <= 0.0f) hz = 60.0f;
    stepper->step = 1.0f / hz;
    stepper->accumulator = 0.0f;
    stepper->maxStepsPerFrame = (maxStepsPerFrame > 0) ? maxStepsPerFrame : 5;
}

int FixedStepperAdvance(FixedStepper *stepper, float dt)
{
    // A huge dt (window drag, level load) would otherwise queue up hundreds of
    // ticks; drop the excess instead of stalling further.
    if (dt > 0.25f) dt = 0.25f;
    stepper->accumulator += dt;

    int steps = 0;
    while (stepper->accumulator >= stepper->step && steps < stepper->maxStepsPerFrame) {
        stepper->accumulator -= stepper->step;
        steps++;
    }
    if (stepper->accumulator > stepper->step * (float)stepper->maxStepsPerFrame) {
        stepper->accumulator = 0.0f;
    }
    return steps;
}

void EngineScreenshot(const char *path)
{
    // Not TakeScreenshot: that discards any directory in the name and always
    // writes beside the executable, which breaks scripted capture runs.
    Image image = LoadImageFromScreen();
    if (ExportImage(image, path)) {
        TraceLog(LOG_INFO, "CORE: screenshot written to %s", path);
    } else {
        TraceLog(LOG_ERROR, "CORE: could not write screenshot to %s", path);
    }
    UnloadImage(image);
}
