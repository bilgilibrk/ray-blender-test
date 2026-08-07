# 09 — Race rules and state

> `game/include/game/race.h` · `game/src/race.c` — 390 lines.

---

## The problem

"Count the laps" sounds like the easiest thing in the game. It is not. Every
naive implementation has a way to be cheated or corrupted:

- **Trigger volume at the finish line.** A car clipping the corner of the volume
  at 6 u/s can pass through it in less than one 120 Hz tick and miss it entirely.
  A car shoved sideways through it by another car counts a lap it did not drive.
- **Distance travelled.** Doing donuts on the start line accumulates distance
  without progress.
- **Nearest waypoint index.** Jumps discontinuously, and a track that passes
  close to itself will snap to the wrong section (Chapter 05).

And then there is the opposite failure: a system so strict that a legitimate lap
does not count because a car crossed one gate a hand's width off the racing line.

The header states the design:

```c
// game/include/game/race.h
// Progress is measured as arc length along the centreline rather than by
// geometric gate crossings, which makes lap counting immune to a car clipping
// the edge of a checkpoint or being shoved sideways through one. The gates are
// still enforced in order, so cutting the course does not advance a lap.
```

Two mechanisms doing two different jobs. Arc length gives a smooth, continuous,
never-jumping measure of progress. Ordered gates prevent shortcuts. Neither alone
is sufficient; together they are both robust and uncheatable.

---

## The state

```c
typedef struct RaceProgress {
    int lap;                    // times the finish line has been crossed
    int nextCheckpoint;
    int splineHint;
    float splineDistance;
    float lastSplineDistance;
    float score;                // monotonic distance round the loop, for sorting

    float lapStartTime;
    float lastLapTime;
    float bestLapTime;
    float finishTime;
    bool finished;
    int finishPosition;

    float offTrackTime;
    float stuckTime;
} RaceProgress;
```

Note `lastSplineDistance` alongside `splineDistance`. Keeping the previous
value is what turns "where am I" into "did I just cross something", and it is
the whole basis of gate detection below.

```c
typedef struct Race {
    const Level *level;
    const Spline *spline;
    const CollisionWorld *collision;
    CarTuning tuning;

    Racer racers[RACE_MAX_RACERS];
    int racerCount;
    int playerIndex;

    RaceState state;
    float countdown;
    float elapsed;
    int totalLaps;
    int finishedCount;

    bool autopilot;

    float *checkpointDistance;  // arc length of each gate
    int checkpointCount;
    float startDistance;        // arc length of the finish line

    int standings[RACE_MAX_RACERS];
    Arena arena;
} Race;
```

Three `const` pointers to borrowed data — the race reads the level, the spline
and the collision world but owns none of them. `Stage` in `main.c` owns all
four, and the lifetimes coincide, so a raw borrowed pointer is exactly right.

---

## Gates become arc lengths, once

```c
// Resolve each gate to an arc length once; progress is compared against these.
race->checkpointCount = level->checkpointCount;
if (race->checkpointCount > 0) {
    race->checkpointDistance = ArenaAlloc(&race->arena,
                                          sizeof(float) * (size_t)race->checkpointCount);
    for (int i = 0; i < race->checkpointCount; i++) {
        int hint = -1;
        Vector3 p = level->checkpoints[i].position;
        race->checkpointDistance[i] = SplineClosest(spline, p, &hint).distance;
    }
    race->startDistance = race->checkpointDistance[0];
}
```

The level stores gates as world positions with a yaw and a width. The race never
uses that geometry again. Each gate is projected onto the spline once, at init,
and reduced to a single number: its arc length.

`hint = -1` forces a full search — this runs twelve times at init, so cost is
irrelevant and correctness is everything.

**A twelve-float array replaces twelve geometric primitives.** The gate width
does not participate in detection at all; a gate is a *station* on the loop, not
a doorway. That is what makes clipping its edge harmless.

---

## Detecting a crossing

```c
// A gate counts as passed when the car's arc length crosses it moving forward.
// The window rejects the huge jump that a wrap-around or a respawn produces.
#define RACE_GATE_WINDOW 3.0f

static void UpdateCheckpoints(Race *race, Racer *racer)
{
    if (race->checkpointCount <= 0) return;

    RaceProgress *p = &racer->progress;
    float target = race->checkpointDistance[p->nextCheckpoint];

    float now = SplineWrapDelta(race->spline, p->splineDistance, target);
    float before = SplineWrapDelta(race->spline, p->lastSplineDistance, target);

    // Forward zero-crossing of the signed distance to the gate.
    if (before < 0.0f && now >= 0.0f && (now - before) > 0.0f &&
        (now - before) < RACE_GATE_WINDOW) {
        /* ... advance ... */
    }
}
```

This deserves unpacking slowly, because it is doing several things at once.

`SplineWrapDelta(spline, a, b)` (Chapter 05) is the shortest signed difference
`a − b` around the loop, in `(−length/2, +length/2]`. So `now` is "how far past
the gate am I", signed: negative means approaching, positive means past.

Between two ticks the car moves 0.055 units. If `before < 0` (approaching) and
`now ≥ 0` (past), the car crossed the gate during this tick. That is a **zero
crossing of a signed distance function** — the same technique as detecting a
sign change in a root finder.

Why this beats a geometric test:

- **It cannot be missed.** The sign changes exactly once per crossing, no matter
  how fast the car goes. A trigger volume can be tunnelled; a sign change cannot.
- **Lateral position is irrelevant.** The car's arc length is well-defined
  whether it is on the racing line or a metre off it.
- **Direction is intrinsic.** Crossing backwards gives `before > 0, now < 0`,
  which the condition rejects. No separate direction test needed.

### The window

The two extra conditions guard against jumps.

`(now - before) > 0` means the car moved forward. `(now - before) < RACE_GATE_WINDOW`
(3.0 units) means it moved *plausibly* forward — at most about 55 ticks' worth of
movement.

What are they defending against?

- **A respawn.** `RaceRespawn` teleports the car to the nearest point on the
  centre line. If that happens to move it across a gate, the arc length jumps
  and the sign flips. Without the window, a rescue would award a gate.
- **A loop wrap.** `splineDistance` wraps from 94.9 to 0.1 at the end of a lap.
  `SplineWrapDelta` handles most of the arithmetic, but combined with a gate near
  the wrap point the deltas can be large.
- **A stale hint** pinning the query to the wrong side of the track for one tick.

3.0 units is comfortably more than a tick's movement (0.055) and comfortably
less than any teleport.

### Advancing, and counting a lap

```c
bool wasFinishLine = (p->nextCheckpoint == 0);
p->nextCheckpoint = (p->nextCheckpoint + 1) % race->checkpointCount;

if (wasFinishLine) {
    if (p->lap > 0) {
        p->lastLapTime = race->elapsed - p->lapStartTime;
        if (p->bestLapTime <= 0.0f || p->lastLapTime < p->bestLapTime) {
            p->bestLapTime = p->lastLapTime;
        }
    }
    p->lap++;
    p->lapStartTime = race->elapsed;

    if (p->lap > race->totalLaps && !p->finished) {
        p->finished = true;
        p->finishTime = race->elapsed;
        p->finishPosition = ++race->finishedCount;
    }
}
```

**Only one gate is ever tested per tick** — `nextCheckpoint`. That is the whole
anti-shortcut mechanism. Cut across the infield from gate 3 to gate 9 and your
arc length duly increases, but gate 4 is never crossed, so `nextCheckpoint`
stays at 4 and you can lap the circuit forever without the counter moving.

**`if (p->lap > 0)`** skips the lap-time calculation on the first crossing. Cars
start behind the line, so the first crossing begins lap 1 rather than completing
one — there is no previous lap to time.

**`p->lap > race->totalLaps`** — with `totalLaps = 3`, crossing the line for the
fourth time (`lap` becoming 4) finishes the race. The counter counts *crossings*,
not completed laps, so the finish is at `totalLaps + 1`. `tests/test_race.c`
pins this exactly:

```c
CHECK(!p->finished || p->lap == race.totalLaps + 1,
      "car %d finished on lap counter %d, expected %d", i, p->lap, race.totalLaps + 1);
```

An off-by-one here is the kind of bug that produces "the race ends a lap early"
reports, and this assertion is why it cannot recur.

**`++race->finishedCount`** — pre-increment, so the first finisher gets position
1. Compact, and easy to get wrong.

---

## Scoring and standings

```c
// Distance travelled round the loop measured from the finish line.
static float LapRelative(const Race *race, float distance)
{
    float rel = distance - race->startDistance;
    if (rel < 0.0f) rel += race->spline->length;
    return rel;
}
```

Rebases arc length so the finish line is zero. Without it, a circuit whose gate 0
sits at arc length 40 would have cars "restarting" their progress mid-lap.

```c
p->score = (float)p->lap * race->spline->length + LapRelative(race, q.distance);
```

**A single monotonic number**: laps completed times the loop length, plus
progress within the current lap. A car on lap 2, 30 units round a 95-unit
circuit, scores `2 × 95 + 30 = 220`.

Now sorting the field is a numeric comparison, and it is correct across lap
boundaries — which the raw arc length is not, since a leader just past the line
(0.5) has a *smaller* arc length than a backmarker approaching it (94.0).

The grid setup uses a negative score deliberately:

```c
p->score = LapRelative(race, q.distance) - race->spline->length;   // still behind the line
```

Cars start *behind* the finish line, so before the race their progress is
negative. Without this, a car sitting on the grid at arc length 94 would score 94
and appear to be leading.

### The comparator

```c
static int CompareStandings(const Race *race, int a, int b)
{
    const RaceProgress *pa = &race->racers[a].progress;
    const RaceProgress *pb = &race->racers[b].progress;

    if (pa->finished != pb->finished) return pa->finished ? -1 : 1;
    if (pa->finished && pb->finished) return pa->finishPosition - pb->finishPosition;
    if (pa->score > pb->score) return -1;
    if (pa->score < pb->score) return 1;
    return a - b;
}
```

Four tiers, in order: finished beats unfinished; among finishers, finishing
order; among runners, score; ties broken by index.

**The index tiebreak matters.** Without it, `CompareStandings` returns 0 for
equal scores, and the sort's behaviour on ties depends on the current array
order — so two cars with identical progress would swap places every frame and
the position indicator would flicker. Returning `a - b` makes the order *total*,
so the sort is deterministic. This is exactly why `std::sort` has `stable_sort`
as a separate function.

```c
static void SortStandings(Race *race)
{
    // Insertion sort: the field is tiny and nearly ordered every frame.
    for (int i = 1; i < race->racerCount; i++) {
        int value = race->standings[i];
        int j = i - 1;
        while (j >= 0 && CompareStandings(race, race->standings[j], value) > 0) {
            race->standings[j + 1] = race->standings[j];
            j--;
        }
        race->standings[j + 1] = value;
    }
}
```

Insertion sort on at most 8 elements, called 120 times a second. This is the
right algorithm for two reasons, and the comment gives both:

- **Tiny n.** Quicksort's overhead exceeds its advantage below roughly 16
  elements. Every good `sort` implementation switches to insertion sort at that
  size internally.
- **Nearly sorted.** Positions change rarely. Insertion sort is **adaptive**:
  its cost is O(n + inversions), so an already-sorted array costs one pass of
  comparisons. Quicksort does not care and pays O(n log n) regardless.

`race->standings` persists between frames, holding last frame's order, which is
what makes "nearly sorted" true. That is deliberate — reinitialising it to
`0..n-1` each frame would throw the property away.

---

## Placing the grid

```c
static void PlaceOnGrid(Race *race, int index)
{
    Racer *racer = &race->racers[index];
    const Level *level = race->level;

    Vector2 position = { 0.0f, 0.0f };
    Vector3 spawn3D = { 0.0f, 0.0f, 0.0f };
    float yaw = 0.0f;
    if (level->spawnCount > 0) {
        const LevelSpawn *spawn = &level->spawns[index % level->spawnCount];
        spawn3D = spawn->position;
        position = (Vector2){ spawn->position.x, spawn->position.z };
        yaw = spawn->yawDeg * DEG2RAD;
    }

    CarInit(&racer->car, position, yaw);

    RaceProgress *p = &racer->progress;
    memset(p, 0, sizeof(*p));
    p->splineHint = -1;

    Vector3 here = { position.x, spawn3D.y, position.y };
    SplineQuery q = SplineClosest(race->spline, here, &p->splineHint);
    racer->car.height = q.position.y;
    racer->car.pitch = atanf(q.grade);
    p->splineDistance = q.distance;
    p->lastSplineDistance = q.distance;
    p->score = LapRelative(race, q.distance) - race->spline->length;
}
```

Three details worth noting.

**`index % level->spawnCount`** wraps if there are more cars than spawns. Two
cars would share a slot and immediately collide — but `RaceInit` prevents that:

```c
if (racerCount > RACE_MAX_RACERS) racerCount = RACE_MAX_RACERS;
if (level->spawnCount > 0 && racerCount > level->spawnCount) racerCount = level->spawnCount;
if (racerCount < 1) racerCount = 1;
```

`--racers 20` on a six-spawn circuit silently gives you six. The modulo is
belt-and-braces.

**`lastSplineDistance = splineDistance`** on init. If it were left at 0, the
first tick would compute a delta of `distance - 0`, which could look like a
crossing.

**`pitch = atanf(q.grade)`** — the car sits pitched to the road immediately
rather than easing into it over the first half-second.

`tests/test_race.c` checks the spawns themselves are sane:

```c
CHECK(fabsf(q.lateral) <= q.halfWidth,
      "spawn %d is %.2f off the centre line but the track half width is %.2f", /* ... */);

// The grid must face down-track, not into the scenery.
float want = atan2f(q.tangent.x, q.tangent.z) * RAD2DEG;
float diff = fmodf(level.spawns[i].yawDeg - want + 540.0f, 360.0f) - 180.0f;
CHECK(fabsf(diff) < 25.0f, "spawn %d faces %.1f deg, track runs at %.1f deg", /* ... */);
```

`fmodf(a - b + 540, 360) - 180` is the wrap-to-shortest idiom for degrees:
add 540 (= 360 + 180) to force positivity, take the modulus, subtract 180. The
result lands in [−180, 180). Chapter 05's `SplineWrapDelta` in another space.

---

## Rescue

```c
#define RACE_OFFTRACK_RESPAWN 6.0f      // seconds stranded before a rescue
#define RACE_STUCK_RESPAWN 4.0f

// ... in RaceUpdate:
if (race->state == RACE_RUNNING && !p->finished) {
    p->offTrackTime = onTrack ? 0.0f : (p->offTrackTime + dt);
    p->stuckTime = (racer->car.speed < 0.3f) ? (p->stuckTime + dt) : 0.0f;
    if (p->offTrackTime > RACE_OFFTRACK_RESPAWN || p->stuckTime > RACE_STUCK_RESPAWN) {
        RaceRespawn(race, i);
    }
}
```

Two independent timers, each reset the moment its condition clears. Six seconds
off track, or four seconds barely moving, triggers a marshal rescue.

```c
void RaceRespawn(Race *race, int index)
{
    Racer *racer = &race->racers[index];
    Vector3 here = { racer->car.position.x, racer->car.height, racer->car.position.y };
    SplineQuery q = SplineClosest(race->spline, here, &racer->progress.splineHint);

    racer->car.position = (Vector2){ q.position.x, q.position.z };
    racer->car.height = q.position.y;
    racer->car.pitch = atanf(q.grade);
    racer->car.velocity = (Vector2){ 0.0f, 0.0f };
    racer->car.yaw = atan2f(q.tangent.x, q.tangent.z);
    racer->car.steerAngle = 0.0f;
    racer->car.speed = 0.0f;
    racer->progress.offTrackTime = 0.0f;
    racer->progress.stuckTime = 0.0f;
    racer->ai.recoverTimer = 0.0f;
}
```

Snap to the **nearest** point on the centre line, facing down-track, stationary.

**Nearest, not "last gate"** — so a rescue never gains or loses a position. Arc
length is essentially unchanged, so `score` is unchanged, so standings are
unchanged. Compare a naive "teleport to the last checkpoint", which would move a
car backwards and could cost it places for an incident that was not its fault.

**Velocity is zeroed** — a rescue is a penalty. You lose your momentum, which on
this scale is several seconds.

**`ai.recoverTimer = 0.0f`** — clearing another system's state. Without it, a
just-rescued AI would still be mid-reverse-manoeuvre and immediately drive
backwards off the line. This is the same coupling the countdown handling deals
with (Chapter 07), and it is the kind of cross-system detail that only shows up
in play.

Note the interaction with the gate window: a respawn can move a car across a
gate. `RACE_GATE_WINDOW` is what makes that harmless.

---

## The state machine

```c
typedef enum RaceState {
    RACE_COUNTDOWN = 0,
    RACE_RUNNING,
    RACE_FINISHED,
} RaceState;
```

```c
void RaceUpdate(Race *race, CarInput playerInput, float dt)
{
    if (race->state == RACE_COUNTDOWN) {
        race->countdown -= dt;
        if (race->countdown <= 0.0f) {
            race->countdown = 0.0f;
            race->state = RACE_RUNNING;
        }
    } else if (race->state == RACE_RUNNING) {
        race->elapsed += dt;
    }

    bool locked = (race->state == RACE_COUNTDOWN);
    /* ... per-car update ... */

    ResolveCarCollisions(race);
    SortStandings(race);

    if (race->state == RACE_RUNNING && race->finishedCount >= race->racerCount) {
        race->state = RACE_FINISHED;
    }
}
```

Three states, two transitions, both one-way. No transition table, no callbacks —
at this size, `if` statements are the state machine, and adding machinery would
make it harder to read, not easier.

**`elapsed` only advances while running.** Every lap time and finish time is
derived from it, so the countdown is automatically excluded from the clock.

`RACE_FINISHED` does not stop the simulation. Cars keep being updated, keep
colliding, keep coasting:

```c
if (p->finished) {
    // Coast to a stop rather than freezing mid-track.
    racer->input.throttle = 0.0f;
    racer->input.brake = 0.35f;
}
```

Freezing a car the instant it crosses the line would look broken — cars stopped
dead mid-corner while the results screen fades in. A gentle 35% brake is a
slowing-down lap.

---

## The order of operations in one tick

`RaceUpdate`'s per-car loop is carefully sequenced, and the sequence is the
subtle part:

```
for each car:
  1. decide inputs      (locked? player? AI?)
  2. query the spline   -> onTrack, inSand, grade, height
  3. build CarSurface
  4. CarUpdate or CarHold
  5. resolve static collision, apply contact
  6. query the spline AGAIN -> updated arc length
  7. lastSplineDistance = splineDistance; splineDistance = new
  8. UpdateCheckpoints
  9. score
 10. rescue timers
then:
 11. ResolveCarCollisions   (all pairs)
 12. SortStandings
 13. check for race end
```

**The spline is queried twice per car per tick.** Step 2 decides what surface the
car is driving on *before* it moves; step 6 measures where it ended up *after*
moving and after collision resolution.

Skipping step 6 and reusing step 2's answer would mean lap progress lagged one
tick behind position — and, worse, would miss a gate crossing caused by
collision resolution pushing a car forward.

Both queries share `p->splineHint`, so the second is nearly free (Chapter 05).

**Car-versus-car resolution happens after every car has moved** (step 11), not
inside the loop. Resolving pairs mid-loop would make the result depend on
iteration order — car 0 would be resolved against car 1's *old* position but car
1 against car 0's *new* one. Batching it makes the tick order-independent, which
matters for determinism.

---

## Inputs, and the autopilot switch

```c
if (locked) {
    racer->input = (CarInput){ 0 };
    racer->ai.recoverTimer = 0.0f;
} else if (racer->isPlayer && !race->autopilot && !p->finished) {
    racer->input = playerInput;
} else {
    int n = 0;
    for (int j = 0; j < race->racerCount; j++) {
        if (j == i) continue;
        neighbours[n].position = race->racers[j].car.position;
        neighbours[n].velocity = race->racers[j].car.velocity;
        n++;
    }
    racer->input = AIThink(&racer->ai, &racer->car, &race->tuning, race->spline,
                           &p->splineHint, neighbours, n, dt);
    /* ... */
}
```

The `else` branch catches three cases: AI cars, the player's car under
`--autopilot`, and the player's car after finishing. All three want the same
thing, so they share the branch.

`race->autopilot` is a single bool that turns the game into a self-driving demo.
`tests/test_race.c` sets it and drives the whole field:

```c
Race race;
CHECK(RaceInit(&race, &level, &spline, &collision, 6), "race init");
race.autopilot = true;
```

**A one-line switch that makes the game testable.** Design for it early; it is
very hard to retrofit.

The neighbour list is rebuilt per car per tick — an O(n²) copy over at most 8
cars, 56 vector copies. Cheaper to rebuild than to maintain incrementally, and
the exclusion of self (`if (j == i) continue`) is what makes the AI's avoidance
loop simple.

---

## Time formatting

```c
const char *RaceFormatTime(float seconds, char *buffer, int size)
{
    if (seconds <= 0.0f || seconds >= 5999.0f) {
        snprintf(buffer, (size_t)size, "--:--.---");
        return buffer;
    }
    int minutes = (int)(seconds / 60.0f);
    float rest = seconds - (float)minutes * 60.0f;
    snprintf(buffer, (size_t)size, "%d:%06.3f", minutes, (double)rest);
    return buffer;
}
```

`%06.3f` is worth knowing: six characters total, three after the point, zero
padded. `7.5` becomes `07.500`, which is what a lap timer should show — not
`7.500`.

The sentinel for "no time yet" is `--:--.---` rather than `0:00.000`, so an
un-set best lap is visibly absent rather than looking like an impossibly fast
one.

Returning `buffer` lets the call nest inside a `printf` argument list, which is
what `tests/test_race.c` and `hud.c` both do.

---

## The campaign

`main.c` layers progression over the race:

```c
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
```

```c
bool won = (stage.race.racers[stage.race.playerIndex].progress.finishPosition == 1);
bool advance = (stage.race.state == RACE_FINISHED) && won && (current + 1 < circuitCount);
```

And then a comment that documents a bug class:

```c
// Recomputed from the current circuit rather than reusing `advance`,
// which is stale the moment the player has moved on: `current` has
// already stepped and current + 1 may be off the end of the campaign.
bool nextExists = (current + 1 < circuitCount);
```

`advance` is computed near the top of the frame, before the input handling that
may increment `current`. Reusing it below would read a value that describes the
*previous* circuit — and index `kCampaignNames` out of bounds on the last one.

**Derived booleans computed early and consumed late are a reliable source of
bugs.** Either recompute at the point of use, or compute at the point of use in
the first place.

---

## Exercises

1. **Try to cheat.** With `--debug` on, drive across the infield of circuit01,
   skipping several gates, and cross the finish line. Watch `nextCheckpoint` in
   the overlay. Then drive the lap properly and watch it advance.

2. **Remove the window.** Delete the `(now - before) < RACE_GATE_WINDOW` term.
   Drive to just before a gate, press `R`, and see whether the gate is awarded.
   Construct the exact sequence that makes it happen.

3. **Break the score.** Change `p->score` to just `LapRelative(race, q.distance)`
   without the lap term. Run `--autopilot` and watch the position indicator as
   the leader crosses the line. Explain in one sentence.

4. **Remove the tiebreak.** Change the final `return a - b;` in
   `CompareStandings` to `return 0;`. Start a race and watch positions during
   the countdown, when every car has near-identical scores.

5. **Add a penalty.** Detect a car that crosses a gate while more than
   `2 × halfWidth` off the racing line, and add 2 seconds to its `elapsed`-based
   finish time. Where in the tick does this belong? What has to change in
   `CompareStandings`?

6. **Sector times.** The gates are already ordered arc lengths. Record the time
   at each and show the three best sector times on the HUD. What has to be added
   to `RaceProgress`, and what has to be reset in `RaceReset`?

7. **Determinism.** Run `--autopilot --frames 7200` twice and compare the final
   log line. Are they identical? Find the one call in `ai.c` that makes the
   answer depend on wall-clock time, and replace it with something derived from
   `race->elapsed`.

---

Next: [10 — Rendering: batching and culling](10-rendering.md)
