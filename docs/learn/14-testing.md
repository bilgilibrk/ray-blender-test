# 14 — Testing a game without a window

> `tests/` — 900 lines across six files. `tools/check_shaders.sh`.

---

## The problem

Games are notoriously hard to test. The usual excuses:

- "It needs a GPU." Most of it does not.
- "The behaviour is emergent." That is an argument for testing it, not against.
- "There's no right answer, it's a feel thing." Feel has measurable
  consequences — lap times, top speeds, how far a car travels in a second.
- "The content changes constantly." Then test properties of content, not
  specific content.

This project answers all four. `make test` runs **348 checks in a couple of
seconds** with no window, no GPU and no display:

```
json
spline
collision
light
race
  a second at full throttle from 6.60 u/s: tarmac 5.04 u/s (5.60 units),
                                           grass 3.23 (3.42), gravel 0.96 (1.10)
  --- levels/circuit01.level.json
  36 sand traps over 6 sand pieces
  tightest radius 1.31 at arc 63.8 (caps speed at 4.44 u/s); 18% of the lap is corner-limited
  6/6 finished, race time 88.0s, best lap 23.30s, leader avg speed 3.05 u/s,
      worst off-track 16%, 0.0 car-seconds in gravel
  ran wide into 30 trap stretches: worst got 1.40 units in a second,
      best was still doing 1.02 u/s
  --- levels/circuit02.level.json
  78 sand traps over 13 sand pieces
  tightest radius 1.31 at arc 95.0 (caps speed at 4.47 u/s); 22% of the lap is corner-limited
  6/6 finished, race time 114.8s, best lap 54.49s, leader avg speed 3.59 u/s,
      worst off-track 5%, 0.0 car-seconds in gravel
  ran wide into 65 trap stretches: worst got 1.37 units in a second,
      best was still doing 1.00 u/s

348 checks, 0 failures — ok
```

Read that output as a document. It is not just pass/fail — it is a report on how
the game currently plays.

---

## The architectural precondition

None of this is possible without the layering from Chapter 01:

```makefile
# Makefile
# Tests run the engine and the race simulation with no window: nothing below the
# renderer touches OpenGL. raylib is still linked for TraceLog, file IO and maths.
$(TEST_BIN): $(TEST_SRC) $(ENGINE_OBJ) $(GAME_LIB) $(RAYLIB_LIB)
```

```makefile
# Game code minus its entry point, so tests can drive a whole race in-process.
GAME_LIB   := $(filter-out $(BUILD)/game/src/main.o,$(GAME_OBJ))
```

The `filter-out` is the whole trick. `main.c` holds the window, the frame loop
and the rendering; everything else — `car.c`, `ai.c`, `race.c`, `hud.c`,
`skid.c` — links into the test binary and runs headless.

`hud.c` and `render.c` are linked too. They are never *called*, so their GL
functions are never reached, and the linker is happy because raylib provides the
symbols.

**If you want testable game logic, keep the frame loop in a file by itself.**
That one decision is worth more than any testing framework.

---

## The harness

```c
// tests/tests.h
// Shared harness for the engine and gameplay tests.
extern int g_failures;
extern int g_checks;

#define CHECK(cond, ...) do {                       \
    g_checks++;                                     \
    if (!(cond)) {                                  \
        g_failures++;                               \
        printf("  FAIL: ");                         \
        printf(__VA_ARGS__);                        \
        printf("   [%s:%d]\n", __FILE__, __LINE__); \
    }                                               \
} while (0)

extern int g_verbose;

void RunJsonTests(void);
void RunSplineTests(void);
void RunCollisionTests(void);
void RunLightTests(void);
void RunRaceTests(void);
```

Twenty-eight lines. No framework, no dependency, no build step.

Design details worth stealing:

**`do { ... } while (0)`** is the standard multi-statement macro idiom. It makes
`if (x) CHECK(...); else ...` parse correctly — a bare `{ }` block would leave a
stray semicolon and break the `else`.

**Variadic message with `printf` semantics.** So a failure is not
`assertion failed: blocking == 0` but:

```
FAIL: 3 collider(s) intrude on the racing surface   [tests/test_race.c:128]
```

The message carries the *value*, which is usually the whole diagnosis.

**It does not abort.** `CHECK` records and continues. So one broken invariant
does not hide the next twenty, and you see the full picture in one run. For a
test suite where later checks do not depend on earlier ones, this is strictly
better than `assert`.

```c
// tests/main.c
int main(int argc, char **argv)
{
    // The engine logs a lot at INFO; tests only care about problems.
    SetTraceLogLevel(LOG_WARNING);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--verbose")) { SetTraceLogLevel(LOG_INFO); g_verbose = 1; }
    }

    printf("json\n");      RunJsonTests();
    printf("spline\n");    RunSplineTests();
    printf("collision\n"); RunCollisionTests();
    printf("light\n");     RunLightTests();
    printf("race\n");      RunRaceTests();

    printf("\n%d checks, %d failures — %s\n", g_checks, g_failures,
           g_failures ? "FAILED" : "ok");
    return g_failures ? 1 : 0;
}
```

The order is bottom-up: JSON before spline before race. If the JSON parser is
broken, its failures appear first and everything after is explained.

Exit code 1 on failure, so `make test` fails the build and CI notices.

---

## Four kinds of test

The suite is not homogeneous. It contains four genuinely different testing
strategies, and knowing which is which is the useful part.

### 1. Unit tests with known answers

`test_json.c`, `test_collide.c`, `test_light.c`, and part of `test_spline.c`.

The technique: **choose inputs whose correct output you can derive by hand.**

```c
// tests/test_collide.c
Manifold m = CollideObb2(Box(0, 0, 1, 1, 0), Box(1.5f, 0, 1, 1, 0));
CHECK(fabsf(m.depth - 0.5f) < 1e-4f, "penetration %.4f, expected 0.5", (double)m.depth);
```

Two unit boxes 1.5 apart overlap by exactly 0.5. Arithmetic, not observation.

```c
// tests/test_spline.c
// A smoothed 32-gon of radius 10 should measure close to a circle.
float circumference = 2.0f * PI * 10.0f;
CHECK(fabsf(spline.length - circumference) < circumference * 0.02f, /* ... */);
```

A circle's circumference is 2πr. The 2% tolerance covers the polygon
approximation and the smoothing.

```c
// tests/test_light.c
Light lamp = LightMakePoint((Vector3){ 0, 0, 0 }, WHITE, 1.0f, 10.0f);
CHECK(LightInfluence(&lamp, (Vector3){ 0, 0, 0 }, 0.0f) > 0.99f, "full at the centre");
CHECK(LightInfluence(&lamp, (Vector3){ 20, 0, 0 }, 0.0f) == 0.0f, "zero beyond the range");
CHECK(LightInfluence(&lamp, (Vector3){ 10, 0, 0 }, 0.0f) == 0.0f, "zero exactly at the range");
```

Note the third: **exactly** at the range, an exact zero. That is the boundary
case the falloff formula was designed to hit, so it is asserted exactly rather
than with a tolerance.

**The anti-pattern this avoids:** running the code, seeing what it produces, and
asserting that. Such a test locks in current behaviour including its bugs, and
tells you nothing about correctness.

### 2. Robustness tests

Hostile and degenerate input, asserting only "does not crash, does not accept".

```c
// tests/test_json.c
const char *bad[] = {
    "{", "[", "{\"a\":}", "{\"a\" 1}", "[1,]", "{,}", "\"unterminated",
    "{\"a\":1} trailing", "tru", "[1 2]", "{\"a\":1,}", "",
};
```

Chapter 03 broke down what each targets. Alongside them:

```c
// Deep nesting must hit the depth guard rather than smash the stack.
char deep[4096];
memset(deep, '[', sizeof deep - 1);
CHECK(P(&tmp, deep, err, sizeof err) == NULL, "deep nesting rejected");

// Exhausting the arena must fail cleanly.
Arena tiny;
ArenaInit(&tiny, 64, "tiny");
CHECK(P(&tiny, doc, err, sizeof err) == NULL, "arena exhaustion handled");
```

And the API-tolerance checks:

```c
// Accessors must tolerate wrong types and NULL.
CHECK(JsonNumber(JsonGet(root, "name"), 42.0) == 42.0, "number of string -> fallback");
CHECK(JsonCount(NULL) == 0, "count of NULL");
CHECK(JsonGet(NULL, "x") == NULL, "get from NULL");
```

These are cheap, they never break on refactoring, and they cover the entire
class of "level file is corrupt or hand-edited wrong".

Chapter 02 made the point that **allocation failure is testable here precisely
because the allocator is a parameter**. That is a design decision paying a
testing dividend.

### 3. Invariants over content

This is the most interesting category, and the one most transferable to your own
projects.

The two circuits are generated by a Python script and contain 732 and 1,237
props. Nobody can review that by hand, and no fixed expected-value test survives
a regeneration. So the tests assert **properties that must hold for any valid
circuit**:

```c
// tests/test_race.c
// No collider should sit on the racing surface: that would wall off the track.
int blocking = 0;
for (int i = 0; i < level.colliderCount; i++) {
    int hint = -1;
    SplineQuery q = SplineClosest(&spline, level.colliders[i].center, &hint);
    float reach = sqrtf(/* ... circumradius ... */);
    if (fabsf(q.lateral) - reach < q.halfWidth * 0.8f) {
        blocking++;
        if (g_verbose) { printf("    collider %d at (%.2f, %.2f) is %.2f from the centre line ...\n", /* ... */); }
    }
}
CHECK(blocking == 0, "%d collider(s) intrude on the racing surface", blocking);
```

The README says this one earned its keep: *"That last check is what caught
scenery being placed on the racing line."*

Note the `g_verbose` branch printing the *identity and position* of each
offender. A count tells you something is wrong; the list tells you what to fix.

The run-off block is the fullest expression of the technique. Chapter 04
explained that art and physics are authored separately and neither derives from
the other, so these tests are what hold them together:

```c
// The art and the physics regions are authored side by side and neither is
// derived from the other, so this is where they are held to each other: the
// gravel must start off the tarmac and stop within reach of it. A trap
// covering the lane would be felt only through a bug; one out in the field
// would be gravel nobody can reach.
CHECK(level.sandtrapCount > 0, "the circuit has no run-off gravel at all");
CHECK(sandPieces > 0, "%d sand traps but no sand drawn anywhere", level.sandtrapCount);
CHECK(solidInTrap == 0, "%d collider(s) stand inside a sand trap", solidInTrap);
CHECK(onLine == 0, "%d centre-line sample(s) sit in a sand trap", onLine);
CHECK(adrift == 0, "%d sand trap(s) are nowhere near the track", adrift);
CHECK(unreachable == 0, "%d sand trap(s) cannot be reached by running wide", unreachable);
```

Six invariants, each a sentence of design intent:

| Invariant | The design rule it encodes |
|---|---|
| `sandtrapCount > 0` | every circuit has run-off |
| `sandPieces > 0` | if it is felt, it is drawn |
| `solidInTrap == 0` | nothing solid inside gravel — a barrier before the trap has slowed you is just a wall |
| `onLine == 0` | gravel never covers the racing line |
| `adrift == 0` | gravel is beside the track, not in a field |
| `unreachable == 0` | running wide here actually puts you in it |

And the reachability test is genuinely clever — it does not check geometry, it
checks *consequence*:

```c
// And the thing that actually matters: leaving the road here has
// to put you in it. Probe just off the tarmac, on the line joining
// the centre line to the trap.
Vector3 from = q.position;
float dx = level.sandtraps[i].center.x - from.x;
float dz = level.sandtraps[i].center.z - from.z;
float len = sqrtf(dx * dx + dz * dz);
if (len > 1e-4f) {
    float probe = q.halfWidth + 0.25f;
    if (!LevelInSandtrap(&level, from.x + dx/len * probe, from.z + dz/len * probe)) {
        unreachable++;
    }
}
```

Step off the road toward the trap and ask whether you are in gravel. That is the
player's actual experience, expressed as an assertion.

The driveability check is in the same spirit:

```c
// Corner radius caps speed at maxYawRate * radius, so a layout can be
// accidentally undriveable no matter how good the AI is. Report the profile.
CHECK(tuning.maxYawRate * worstRadius > 1.8f,
      "tightest corner (radius %.2f) caps speed at %.2f u/s — undriveable", /* ... */);
```

This uses the vehicle model (Chapter 07) to derive a hard bound and applies it
to the *content*. A generated layout with a corner too tight to drive fails the
test with a message naming the arc length where it is.

**The general technique: when content is generated or voluminous, test the
relationships it must satisfy rather than its values.** Write each invariant as
a sentence of design intent first, then encode it.

### 4. Simulation tests

The headline feature. `test_race.c` runs an entire six-car race:

```c
// Drives a whole race with no window. Everything below the renderer is free of
// OpenGL, so the physics, AI and lap logic can be simulated far faster than
// real time and asserted on.
#define STEP (1.0f / 120.0f)

static SimResult Simulate(Race *race, float limitSeconds, bool trace)
{
    int steps = (int)(limitSeconds / STEP);
    /* ... */
    for (int i = 0; i < steps; i++) {
        RaceUpdate(race, idle, STEP);
        tickCount++;
        for (int r = 0; r < race->racerCount; r++) {
            if (!race->racers[r].car.onTrack) offTrackTicks[r]++;
            if (race->racers[r].car.inSand) result.sandTicks++;
        }
        speedSum += race->racers[race->standings[0]].car.speed;
        /* ... optional telemetry ... */
        if (race->state == RACE_FINISHED) break;
    }
    /* ... */
}
```

`race.autopilot = true` hands the player's car to the AI (Chapter 09), and the
loop calls `RaceUpdate` at the same fixed 120 Hz the game uses. 400 simulated
seconds is 48,000 ticks, and it runs in well under a second.

**Nothing is mocked.** The real level, the real spline, the real collision world,
the real physics, the real AI. The only thing absent is the renderer.

The assertions are behavioural:

```c
CHECK(run.finished == race.racerCount, "only %d of %d cars finished within 400s", /* ... */);
CHECK(run.leaderAverageSpeed > 2.5f, "leader averaged %.2f u/s — the field is crawling", /* ... */);
CHECK(run.bestLap > 5.0f && run.bestLap < 60.0f, "best lap of %.2fs is outside the plausible range", /* ... */);
CHECK(run.worstOffTrackFraction < 0.25f, "a car spent %.0f%% of the race off track", /* ... */);
```

Each is a **band**, not a value. `bestLap` between 5 and 60 seconds; leader
average above 2.5 u/s; off-track under 25%.

That is what makes them robust. Tuning `enginePower` from 7.2 to 7.4 changes
every number in the run and breaks no test. Tuning it to 0.7 breaks all of them.
**The tests catch "the game is broken", not "the game changed."**

Then the bookkeeping invariants, which *are* exact:

```c
// Every finisher should have completed exactly the required number of laps.
CHECK(!p->finished || p->lap == race.totalLaps + 1, /* ... */);

// Finishing positions must be unique and cover 1..N.
int seen[RACE_MAX_RACERS + 1] = { 0 };
for (int i = 0; i < race.racerCount; i++) {
    int pos = race.racers[i].progress.finishPosition;
    if (pos >= 1 && pos <= race.racerCount) seen[pos]++;
}
for (int pos = 1; pos <= race.racerCount; pos++) {
    CHECK(seen[pos] == 1, "finishing position %d was awarded %d times", pos, seen[pos]);
}
```

Lap counting and position assignment are *logic*, not feel, so they get exact
assertions. **Know which of your outputs are tuning and which are correctness,
and assert them differently.**

---

## Testing the physics in isolation

```c
// Drives one car across a given surface for a second, flat out, from racing
// speed, and reports where it ended up. No level and no track: just the tyre
// model against the ground it is standing on.
static void SurfaceRun(const CarTuning *tuning, const CarSurface *surface,
                       float *outSpeed, float *outTravel)
{
    Car car;
    CarInit(&car, (Vector2){ 0.0f, 0.0f }, 0.0f);          // facing +Z
    car.velocity = (Vector2){ 0.0f, tuning->topSpeed };
    car.forwardSpeed = tuning->topSpeed;

    CarInput flatOut = { .throttle = 1.0f };
    for (int i = 0; i < 120; i++) CarUpdate(&car, tuning, flatOut, surface, STEP);

    *outSpeed = car.speed;
    *outTravel = car.position.y;
}
```

Six lines of setup, because `CarUpdate` takes its surface as a parameter
(Chapter 07). No level, no spline, no race. This is what "designing for
testability" actually looks like in practice — not a framework, but a function
signature that does not require a world.

Facing +Z means `position.y` (the world's Z) *is* the distance travelled. A
small setup choice that removes a distance calculation and a source of error
from the test.

Then the bounds encode the design:

```c
// Gravel is meant to be the end of your lap, not a slower line through the
// corner: a car cannot drive out of it at anything better than a walk.
CHECK(inGravel < 1.2f, "a car flat out in gravel still does %.2f u/s", (double)inGravel);
CHECK(inGravel < onGrass * 0.5f, "gravel (%.2f u/s) is barely slower than grass (%.2f u/s)", /* ... */);
// ...but it does not stop dead, or a trap would be a wall.
CHECK(inGravel > 0.3f, "gravel brought the car to a standstill (%.2f u/s)", (double)inGravel);
```

**Bounded on both sides**, with the design rationale in the comment and the
measured value in the message. This is executable design documentation.

---

## Composing the two: the run-wide test

The last block in `test_race.c` combines surface physics with level geometry:

```c
// The surface tests above prove gravel is slow and the geometry tests prove
// the boxes sit beside the road. This is the two of them together, in the
// level the game ships: put a car off the road at every trap, flat out, and
// it should be walking a second later and still be in there.
```

For each of the 36 (or 78) traps: build a one-car `Race`, skip the countdown,
place the car just off the tarmac pointing along the track at 85% of top speed,
hold full throttle for 120 ticks, and measure.

```c
Race drive;
if (!RaceInit(&drive, &level, &spline, &collision, 1)) break;
drive.state = RACE_RUNNING;         // no countdown to sit through
drive.countdown = 0.0f;
/* ... place the car just outside the lane, aimed at the trap ... */

// A box at the end of a band points its car straight out of the
// gravel, because that is where the corner stops having any. That
// is the geometry doing its job, not a fault, so those are left out
// rather than asserted on.
if (!LevelInSandtrap(&level, start.x + forward.x * 0.5f, start.y + forward.y * 0.5f)) {
    RaceFree(&drive);
    continue;
}

float entry = drive.tuning.topSpeed * 0.85f;
car->velocity = (Vector2){ forward.x * entry, forward.y * entry };

CarInput flatOut = { .throttle = 1.0f };
float slowest = 1e30f;
for (int t = 0; t < 120; t++) {
    RaceUpdate(&drive, flatOut, STEP);
    // After a quarter second, by which time the gravel has had its
    // say. A car may well crawl out the far end of a band before
    // the second is up — the trap has still done its job.
    if (t > 30 && car->speed < slowest) slowest = car->speed;
}
```

Three details that separate a good test from a flaky one.

**Dozens of independent `Race` objects.** The `Race` struct is caller-owned
(Chapter 01), so the test can build and tear down one per trap.

**Boxes at the end of a band are skipped, with a stated reason.** A trap at the
edge of a corner legitimately points its car out of the gravel. Rather than
loosening the assertion for everyone, those cases are excluded and the exclusion
is justified. And then the exclusion itself is bounded:

```c
CHECK(tested >= level.sandtrapCount / 2, "only %d of %d traps were long enough to "
      "test — the bands have gone to pieces", tested, level.sandtrapCount);
```

If the skip ever swallowed most of the traps, the test would say so — closing
the loophole that skips usually open.

**Only samples after a quarter second.** The car enters at 5.6 u/s and takes a
moment to bog down; measuring from tick 0 would report the entry speed.

The assertions:

```c
// Grass over the same second is worth about 3.4 units, tarmac 5.6.
CHECK(worstTravel < 2.0f, "a car crossed %.2f units of gravel in a second", (double)worstTravel);
CHECK(bestGetaway < 1.3f, "one trap never slowed its car below %.2f u/s", (double)bestGetaway);
```

Note the comment gives the *reference values from the other tests* — 3.4 for
grass, 5.6 for tarmac — so the reader can judge whether 2.0 is a meaningful
threshold. Real output: `worst got 1.40 units in a second`.

And note the *worst* case is asserted, not the average. 36 traps, and the one
that performs worst must still pass. Averages hide the failure you care about.

---

## Tests as tuning instrument

```
tightest radius 1.31 at arc 63.8 (caps speed at 4.44 u/s); 18% of the lap is corner-limited
6/6 finished, race time 88.0s, best lap 23.30s, leader avg speed 3.05 u/s
ran wide into 30 trap stretches: worst got 1.40 units in a second
```

The README makes this explicit:

> Tuning lives in `CarDefaultTuning()` in `game/src/car.c`; `make test` reports
> lap times, so it doubles as a tuning loop.

Change `enginePower`, run `make test`, read the lap time. Two seconds per
iteration, deterministic, both circuits, no driving required.

That is a **much** faster feedback loop than playing the game, and it means
tuning decisions are made against numbers rather than impressions. You still
have to play it to check the feel — but you arrive at the playtest with the
numbers already in range.

And `--verbose` gives per-second telemetry when something is wrong:

```c
printf("    t=%5.1f  spd=%5.2f  thr=%.2f brk=%.2f str=%+.2f  %s  "
       "arc=%6.2f lap=%d gate=%2d  rec=%.2f\n", /* ... */);
```

Speed, throttle, brake, steer, on/off track, arc length, lap, gate, recover
timer — enough to diagnose an AI that gets stuck without attaching a debugger.

---

## What is not tested

Being honest about the gaps is part of understanding a test suite.

- **Rendering.** No golden-image tests, no pixel comparisons. `--shots` exists
  for manual screenshot capture but nothing compares them.
- **The HUD.** Linked, never called.
- **Input.** `input.c` is a thin mapping over raylib and is not exercised.
- **Audio.** The callback is never invoked; correctness is judged by ear.
- **The Blender tools.** 2,200 lines of Python with no tests of their own —
  though `build_demo_track.py` has extensive internal assertions
  (`verify_line_on_tiles`, `verify_tile_heights`, `verify_loop_clearance`,
  `solve_track`'s four `RuntimeError`s), and its *output* is thoroughly tested
  by `test_race.c`.
- **The DRM/GLES2 path.** Cannot be run headless. Mitigated by
  `tools/check_shaders.sh`, below.

The unifying principle: **the things that are tested are the things where a bug
is invisible until it matters.** A rendering bug is obvious the moment you look
at the screen. A lap-counting bug that only fires when a car is nudged sideways
through a gate at 6 u/s is not.

---

## Validating what you cannot run

Chapter 11 covered `tools/check_shaders.sh` in detail. The summary, because it
is a testing technique as much as a graphics one:

The GLES2 shader is a string in a C file. Nothing compiles it until the game
runs on a Pi with a monitor attached. So the script generates a tiny C program
that includes the *same* `scene_shader.inc` through the *same* preprocessor
path, dumps the resulting GLSL to files, and runs `glslangValidator` on them —
for both targets.

```
desktop  #version 330 ok
drm      #version 100 ok
```

**Extract the artefact through the real build path, then validate it statically.**
Applies to shaders, SQL, regexes, config schemas, and generated code of any kind.

It degrades politely when the validator is absent, so it never blocks a build on
a machine without `glslang-tools`.

---

## Determinism, and the one crack in it

The simulation is very nearly deterministic: fixed timestep, no threading in the
simulation path, no `rand()`.

There is one exception, in `ai.c`:

```c
float wobble = sinf((float)GetTime() * 0.7f + ai->wobblePhase) * 0.02f * (1.0f - ai->skill);
```

`GetTime()` is wall-clock seconds since `InitWindow`. In a headless test that
still advances in real time, so two runs of `make test` produce slightly
different wobble and therefore slightly different lap times.

The tests survive it because they assert bands rather than values — which is
the design working. But it means the suite is not bit-reproducible, and a
failure at the edge of a band could be intermittent.

The fix is to derive the wobble from simulated time (`race->elapsed`) rather
than wall time. That is exercise 5, and Chapter 09's exercise 7.

**Guard your determinism deliberately.** In a simulation, every read of a
wall clock, an uninitialised value, or a global RNG is a crack.

---

## Exercises

1. **Break something and watch.** Change `gripTarmac` from 9.5 to 2.0. Run
   `make test`. Which checks fail, in which order, and do the messages tell you
   what happened? Now change `maxYawRate` to 1.0 and do the same.

2. **Add an invariant.** Every checkpoint should be on the racing line. Write
   the check — project each gate onto the spline and assert `|lateral| <= halfWidth`
   — and run it against both circuits. Does it pass? If not, is the test wrong or
   the content?

3. **Test the standings.** Add a check inside `Simulate` that the `standings`
   array is a permutation of `0..racerCount-1` on every tick. Run it. What would
   break this, and why is it worth checking every tick rather than once?

4. **Find the flakiness.** Run `make test` ten times and record `best lap` for
   circuit01 each time. How much does it vary? Trace the variation to
   `GetTime()` in `ai.c`.

5. **Fix the determinism.** Change `AIThink` to take a time parameter and pass
   `race->elapsed`. Re-run exercise 4 — is it now identical every time? What
   else in the codebase reads a wall clock during simulation?

6. **A golden image test.** Use `--frames 120 --shots 120 --shot-prefix golden`
   to capture a deterministic screenshot, commit it, then write a script that
   re-captures and compares. What makes this fragile, and what would you have to
   pin down (driver, resolution, MSAA) for it to be useful?

7. **Test the exporter.** Write a Python test for `decompose_engine` in
   `io_kenney_racing.py`: for a set of rotation matrices, decompose to euler,
   recompose with the same `Rx·Ry·Rz` convention, and assert the result matches
   the input to within a tolerance. Include the 180° case the special-case
   branch exists for.

8. **Property-based JSON.** Write a generator that produces random *valid* JSON
   documents, serialise them, parse them back, and assert the tree matches.
   Then mutate one random byte and assert the parser either accepts it or
   rejects it cleanly — never crashes.

---

Next: [15 — Build system and portability](15-build-and-portability.md)
