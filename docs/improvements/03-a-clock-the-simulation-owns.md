# 03 — A clock the simulation owns

**Files:** `game/src/ai.c`, `game/include/game/ai.h`, `game/src/race.c`,
`tests/test_race.c`
**Result:** the race is reproducible, and the headless tests now exercise the
same AI the game does. `make test` telemetry moves as a consequence, and that
movement is the point.

---

## The bug the book already knew about

`docs/learn/14-testing.md` had a section called *"Determinism, and the one crack
in it"*, naming this line in `ai.c`:

```c
float wobble = sinf((float)GetTime() * 0.7f + ai->wobblePhase) * 0.02f * (1.0f - ai->skill);
```

Fixing it is exercise 5 of that chapter and exercise 7 of Chapter 09. This is
that fix — plus a correction, because the chapter's account of the symptom was
wrong in a way worth writing down.

## What the chapter said, and what was actually happening

The chapter said:

> `GetTime()` is wall-clock seconds since `InitWindow`. In a headless test that
> still advances in real time, so two runs of `make test` produce slightly
> different wobble and therefore slightly different lap times. The tests survive
> it because they assert bands rather than values.

The first sentence is right. The rest is not. `GetTime()` is `glfwGetTime()`,
which returns **zero until GLFW is initialised**, and the test binary never
opens a window. Measured directly:

```
GetTime() with no window: 0.000000  ... after a busy second: 0.000000
```

So `sinf(0 * 0.7f + phase)` was a constant. The suite was never flaky. The
wobble was **switched off** for every headless test that has ever run.

That is the worse of the two failures. A flaky test announces itself; this one
passed quietly while exercising a configuration the shipped game never uses, and
reported its lap times as if they were the game's.

In the game, where a window does exist, the original diagnosis was correct and
the bug was real: a fixed-timestep simulation reading a variable-rate clock.
`FixedStepperAdvance` can run several 120 Hz substeps per rendered frame, and
every one of them saw the *same* `GetTime()` — so the wobble was sampled at the
frame rate, and the same race run twice was not the same race.

## The fix

A clock on the driver, advanced by the `dt` it is already handed:

```c
ai->clock += dt;
/* ... */
float wobble = sinf(ai->clock * 0.7f + ai->wobblePhase) * 0.02f * (1.0f - ai->skill);
```

`RaceReset` now calls `AIDriverReset`, so restarting a circuit replays it rather
than continuing it. Without that, pressing restart gave a different race from
the one just driven — a second, smaller version of the same bug.

The wobble's *design* is untouched: still a 0.02-unit sway at 0.7 rad/s scaled
by lack of skill, still phase-spread per driver. Only its source of time changed.

## What this does to the test numbers

Turning the wobble on in the headless tests changes what they measure:

| circuit01 | before | after |
|---|---:|---:|
| race time | 88.0 s | 75.6 s |
| best lap | 23.30 s | 23.29 s |
| leader average speed | 3.05 u/s | 3.53 u/s |
| worst off-track | 16% | 4% |

**Nothing got better.** The simulation is the same simulation it always was in
the game; the tests were measuring a different one. The old column is what a
field of AI drivers does with the wobble disabled — a configuration that has
never shipped. Circuit02 barely moves (114.8 → 114.7 s), which fits: the swing
on circuit01 comes from one car in the old run spending 16% of the race off the
track, and small perturbations decide whether that happens.

All the existing band assertions still pass unchanged. They were written as
bands for exactly this reason.

## How it is verified

`RunDeterminismTests` in `tests/test_race.c`:

- the driver clock advances by exactly the ticks handed to it (240 ticks of
  `STEP` must move it by `240 × STEP`) — this is what pins the source of time;
- the same race, run twice from `RaceReset` with real wall-clock time
  deliberately burned in between, must end with every car in *exactly* the same
  position — not within a tolerance, bit-identical;
- `RaceReset` must zero every driver's clock.

Note what this test cannot do: it would still pass if the wobble went back to
`GetTime()`, because headless it is always zero. Chapter 14's exercise 4 has
been rewritten to make that trap the exercise, since the honest lesson here is
that a test which cannot reach a code path will pass quietly forever.

`make test` goes from 402 to **414 checks, 0 failures**.

## Documentation corrected

- `docs/learn/14-testing.md` — the "one crack" section rewritten with the real
  mechanism; exercises 4 and 5 replaced, since the old ones asked the reader to
  measure a flakiness that was never there.
- `docs/learn/08-ai-driver.md` — the wobble snippet and an explanation of
  `ai->clock`.
- `docs/learn/09-race-rules.md` — exercise 7 repointed at what *else* could
  break determinism.
- `docs/learn/16-exercises.md` — the replay exercise no longer cites this as an
  open crack.
