# 01 — Architecture

> How the code is split, what depends on what, and what happens in one frame.

---

## The problem

A game is a pile of concerns that all want to touch each other. Physics wants to
know about the level. Rendering wants to know about physics. Audio wants to know
about the engine note, which is a fact about a car, which is a fact about
physics. The AI wants to know about the track, the other cars and its own
vehicle.

Left alone, this becomes a single file where everything reaches into everything.
The specific failure mode is not "it looks bad" — it is that **you can no longer
test anything**, because instantiating one system drags in all of them,
including the ones that need a GPU.

## The idea: one hard boundary, drawn where the OS lives

This project draws exactly one architectural line, and draws it hard:

```
game/     knows about racing. Knows nothing about windows, GL, or files.
engine/   knows about windows, GL and files. Knows nothing about racing.
```

Then it adds a second, softer rule inside the engine:

> Nothing below the renderer touches OpenGL.

That is what makes `tests/test_race.c` possible: it loads a real level, builds a
real spline, builds a real collision world, and simulates a complete six-car
race **with no window at all**. Physics, AI, lap counting, collision, run-off,
standings — all of it runs headless, thousands of times faster than real time.

You can verify this claim yourself. `make test` links `engine/`, `game/` (minus
`main.o`) and raylib, and never calls `InitWindow`.

---

## Dependency direction

Arrows point "depends on". There are no cycles.

```
                     ┌──────────────┐
                     │  game/main.c │   wiring + frame loop
                     └──────┬───────┘
              ┌─────────────┼─────────────┬──────────────┐
              ▼             ▼             ▼              ▼
        ┌─────────┐  ┌───────────┐  ┌──────────┐  ┌───────────┐
        │ race.c  │  │  hud.c    │  │ skid.c   │  │  (engine) │
        └────┬────┘  └───────────┘  └──────────┘  └───────────┘
      ┌──────┼───────┐
      ▼      ▼       ▼
   ┌─────┐ ┌────┐ ┌──────────┐
   │car.c│ │ai.c│ │ collide  │
   └─────┘ └──┬─┘ └────┬─────┘
              │        │
              ▼        ▼
          ┌────────────────┐
          │  spline.c      │
          └───────┬────────┘
                  ▼
          ┌────────────────┐
          │   level.c      │
          └───────┬────────┘
            ┌─────┴─────┐
            ▼           ▼
        ┌──────┐   ┌────────┐
        │json.c│   │arena.c │
        └──────┘   └────────┘
```

Read it bottom-up and you get the boot order too: an arena backs the JSON
parser, the parser backs the level loader, the level backs the spline and the
collision world, those back the race, and the race backs everything the player
sees.

The renderer (`render.c`), terrain (`terrain.c`), assets, audio and input hang
off the side. `main.c` is the only file that knows they all exist.

### What each layer may include

| Layer | May include | Must not include |
|---|---|---|
| `engine/arena.h` | `<stddef.h>`, `<stdbool.h>` | anything else, including raylib |
| `engine/json.h` | arena | raylib |
| `engine/level.h` | raylib (for `Vector3`, `Color`), arena | anything from `game/` |
| `engine/spline.h` | raylib, arena, level | anything from `game/` |
| `game/car.h` | raylib, `engine/collide.h` | `engine/render.h` |
| `game/race.h` | engine collide/level/spline, `game/car.h`, `game/ai.h` | `engine/render.h` |
| `game/main.c` | everything | — |

Notice what is *absent*: `game/race.h` does not include `engine/render.h`. The
race does not know it is being drawn. That single omission is what buys the
headless test.

---

## The frame loop

`game/src/main.c` holds the entire loop. Here is its skeleton, in order:

```c
// game/src/main.c, inside main()
while (!WindowShouldClose()) {
    InputState in;
    InputUpdate(&in);                       // 1. poll devices

    /* ... toggles: debug, camera mode, night, pause, reset ... */

    float dt = GetFrameTime();
    if (!paused) {
        int steps = FixedStepperAdvance(&stepper, dt);
        for (int s = 0; s < steps; s++)
            RaceUpdate(&stage.race, drive, stepper.step);   // 2. simulate
        SkidUpdate(&skid, &stage.race, dt);
    }

    UpdateHeadlights(&stage.lights, stage.headlights, &stage.race, night);
    ChaseCameraUpdate(&camera, /* ... */);                  // 3. presentation
    AudioEngineSetMotor(rpm, throttle, slip);

    BeginDrawing();
      RenderBeginShadowPass(focusAheadOfCar);               // 4a. depth pass
        TerrainDraw(...); StaticBatchDraw(...); DrawRacer(...);
      RenderEndShadowPass();

      RenderBeginScene(camera.camera, sky);                 // 4b. colour pass
        TerrainDraw(...); StaticBatchDraw(...);
        SkidDraw(&skid);
        DrawRacer(...);
        if (showDebug) { /* overlays */ }
      RenderEndScene();

      HudDraw(&stage.race, paused, &progress);              // 4c. 2D
    EndDrawing();

    frame++;
    /* screenshots, frame limit */
}
```

Four phases: **poll, simulate, present, draw**. Every game loop you will ever
write is a variation on this. The interesting details are all in phase 2.

### Fixed timestep

Phase 2 does not advance physics by `dt`. It advances it by a **fixed** step,
possibly several times, possibly zero times:

```c
// engine/src/core.c
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
```

The racer runs physics at **120 Hz** (`PHYSICS_HZ` in `main.c`) with at most 8
steps per frame.

**Why this matters.** Variable-timestep integration makes your simulation depend
on frame rate. A car that reaches 6.6 u/s at 60 fps reaches 6.9 at 144 fps and
5.1 on a struggling Pi, because every `v += a*dt` and every `v *= (1 - k*dt)`
compounds differently. Lap times stop being comparable. Physics tuning stops
being meaningful. Replays desync.

With a fixed step, `CarUpdate` always sees `dt == 1/120`, on every machine.

**The two guards** are both about not making a bad frame worse:

- Clamping `dt` to 0.25 s stops a stalled frame (you dragged the window; the
  level was loading) from queueing 300 physics ticks that then take even longer
  and stall the next frame too. This is the classic "spiral of death".
- `maxStepsPerFrame` caps the catch-up. If the machine genuinely cannot keep up,
  the simulation runs in slow motion rather than freezing. The accumulator is
  then dumped so the debt does not accrue forever.

### Where dt still appears

Not everything is fixed-step. Look at what takes raw `dt`:

- `SkidUpdate` — cosmetic, ring-buffered, tolerant of jitter.
- `ChaseCameraUpdate` — smoothing, and deliberately frame-rate-independent (see
  below).
- `AIThink` receives the fixed step, because it is called from inside
  `RaceUpdate`.

The camera's smoothing is worth a close look, because it is the standard fix for
a very common bug:

```c
// engine/src/render.c
static float SmoothFactor(float rate, float dt) { return 1.0f - expf(-rate * dt); }
```

The naive version is `x += (target - x) * 0.1f`, which converges at a speed that
depends entirely on how often you call it — so the camera is snappy at 144 fps
and sluggish at 30. Using `1 - e^(-rate·dt)` makes the *time constant* the
parameter instead of the per-call fraction, and the camera behaves identically
at any frame rate. `rate` has units of 1/second; the value settles to within
1/e (about 37%) of its error after `1/rate` seconds.

---

## The `Stage` pattern

The game can switch circuits mid-run (win a race, the next one loads). That
means every per-level object has to be torn down and rebuilt while the process
lives on. `main.c` groups them:

```c
// game/src/main.c
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
```

and the comment above it states the rule precisely:

> What survives a change of circuit — the camera, the skid marks, the fixed
> stepper, the day/night toggle — deliberately lives outside this.

`StageFree` has a property worth internalising:

```c
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
```

Every `*Free` accepts an all-zero struct. `StageLoad` starts with
`memset(stage, 0, sizeof(*stage))` and can therefore call `StageFree` at any
failure point without tracking how far it got. This is the C equivalent of RAII
plus exceptions, and it costs one `memset`.

The general rule: **make your destructors idempotent and zero-tolerant, and
partial-construction cleanup stops being a source of bugs.**

---

## Why C, and why raylib

Neither choice is neutral, so it is worth being explicit.

**C11** gets you: predictable memory layout (which the static batch depends on),
no hidden allocation, a compile of the whole project in a couple of seconds, and
a language every graphics API speaks natively. It costs you: no destructors, no
generics, no bounds checking. Chapter 02 shows how the arena buys back most of
the first, and Chapter 14 shows how tests buy back some of the third.

**raylib** is a thin layer over GLFW/OpenGL that gives you a window, an input
poll, mesh upload, shader loading, model loading (`.glb`), an audio stream and a
2D drawing API — and gets out of the way. It does *not* give you a scene graph,
an entity system, a material system, a physics engine or an asset pipeline. All
of those are things this project wanted to write, so that is the correct amount
of framework.

The parts of raylib actually used here:

| Used for | Functions |
|---|---|
| Window & loop | `InitWindow`, `WindowShouldClose`, `BeginDrawing` |
| Input | `IsKeyDown`, `IsGamepadAvailable`, `GetGamepadAxisMovement` |
| Meshes | `UploadMesh`, `DrawMesh`, `UnloadMesh`, `GenMeshCube` |
| Models | `LoadModel` (glTF), `GetModelBoundingBox` |
| Shaders | `LoadShaderFromMemory`, `SetShaderValue`, `SetShaderValueV` |
| Low-level GL | `rlLoadFramebuffer`, `rlLoadTextureDepth`, `rlSetMatrixProjection` |
| Maths | `raymath.h` — `Vector3`, `Matrix`, `Clamp`, `Lerp` |
| Audio | `LoadAudioStream`, `SetAudioStreamCallback` |
| Files | `LoadFileData`, `FileExists`, `ExportImage` |

That is a small enough surface that porting to another backend would be a
weekend, not a rewrite.

---

## Global state, and where it is allowed

Two modules keep file-scope singletons:

```c
// engine/src/render.c
static struct { Shader shader; /* ...30 fields... */ bool ready; } g_render;

// engine/src/assets.c
static struct { char root[512]; ModelSlot models[256]; /* ... */ } g_assets;
```

This is a deliberate exception, not sloppiness. Both wrap a resource that is
genuinely a process-wide singleton: there is one GL context and one filesystem.
Threading them through every call site as a parameter would add noise without
adding capability, since you cannot have two.

Everything else — `Level`, `Spline`, `CollisionWorld`, `Race`, `Terrain`,
`StaticBatch`, `LightSet` — is a plain struct the caller owns. You can have two
races in flight, and `tests/test_race.c` does exactly that: it runs the main
six-car race and then spins up dozens of separate single-car `Race` objects to
drop into gravel traps.

**Rule of thumb.** A singleton is defensible when the thing it wraps is
physically singular. It is indefensible when it is merely convenient.

---

## Reading order for the code itself

If you want to read the source rather than these chapters, this order builds
knowledge without forward references:

1. `engine/include/engine/arena.h` + `engine/src/arena.c` — 80 lines, complete.
2. `engine/include/engine/level.h` — the data model everything else consumes.
3. `engine/src/level.c` — how that data model is filled.
4. `engine/include/engine/spline.h` — the geometric query API.
5. `game/include/game/car.h` — the physics state and tuning.
6. `game/src/car.c` — one tick of a vehicle.
7. `game/src/race.c` — how a field of them becomes a race.
8. `engine/src/render.c` — the largest file; read `StaticBatchBuild` first.
9. `game/src/main.c` — the wiring, which now reads as a summary of everything.

---

## Exercises

1. **Trace one value.** Pick the player's throttle. Find every place it is read
   or written, from `IsKeyDown(KEY_W)` to the audio callback's `engineGain`.
   Write the chain down. (There are six hops.)

2. **Break the boundary deliberately.** Add `#include "engine/render.h"` to
   `game/src/race.c` and call `RenderDebugSpline` from inside `RaceUpdate`. Run
   `make test`. Explain the link error you get in terms of this chapter.

3. **Measure the fixed step.** Add a counter to `main.c` that sums the steps
   returned by `FixedStepperAdvance` and prints steps-per-second every 60
   frames. Run with `--no-vsync` and with vsync on. Explain why the number is
   the same both times.

4. **Find the spiral.** Comment out the `if (dt > 0.25f) dt = 0.25f;` clamp,
   then add `if (frame == 100) { for (volatile long i = 0; i < 3000000000L; i++); }`
   to simulate a two-second stall. Describe what happens on frame 101 and why.

---

Next: [02 — Memory: arena allocation](02-memory-arenas.md)
