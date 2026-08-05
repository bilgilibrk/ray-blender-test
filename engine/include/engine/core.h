// Engine bootstrap: window, subsystems and the fixed-timestep helper.
#ifndef ENGINE_CORE_H
#define ENGINE_CORE_H

#include <stdbool.h>

#include "raylib.h"

typedef struct EngineConfig {
    const char *title;
    int width;
    int height;
    int targetFPS;          // 0 leaves the frame rate uncapped
    bool vsync;
    bool msaa;
    bool fullscreen;
    bool audio;
    const char *assetRoot;  // defaults to "assets"
} EngineConfig;

EngineConfig EngineDefaultConfig(void);

// Brings up the window, renderer, assets and (optionally) audio.
bool EngineInit(const EngineConfig *config);
void EngineShutdown(void);

// Physics runs on a fixed step so handling is identical regardless of frame
// rate; rendering still happens once per frame.
typedef struct FixedStepper {
    float step;             // seconds per simulation tick
    float accumulator;
    int maxStepsPerFrame;   // clamp so a stall cannot spiral
} FixedStepper;

void FixedStepperInit(FixedStepper *stepper, float hz, int maxStepsPerFrame);

// Adds `dt` and returns how many fixed steps to run this frame.
int FixedStepperAdvance(FixedStepper *stepper, float dt);

// Writes a PNG next to the executable and logs the path.
void EngineScreenshot(const char *path);

#endif // ENGINE_CORE_H
