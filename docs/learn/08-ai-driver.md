# 08 — The AI driver

> `game/include/game/ai.h` · `game/src/ai.c` — 177 lines.

---

## The problem

Five opponents have to drive a circuit competitively, without cheating, using
exactly the same physics and the same three inputs as the player: throttle,
brake, steer.

That last constraint is the important one. It is very easy to write an "AI" that
moves a car along a path at a scripted speed. That car is not racing — it cannot
be bumped off line, cannot lock up, cannot lose grip, cannot make a mistake, and
will look wrong the moment it interacts with the player.

Here, `AIThink` returns a `CarInput`:

```c
// game/include/game/ai.h
CarInput AIThink(AIDriver *ai, const Car *car, const CarTuning *tuning,
                 const Spline *spline, int *hint,
                 const AINeighbour *neighbours, int neighbourCount, float dt);
```

The same struct the keyboard produces. `race.c` feeds it into the same
`CarUpdate`. There is literally no path by which an AI car can do something a
player could not.

That symmetry is also what makes `--autopilot` work: hand the player's car to
`AIThink` and the whole game drives itself, which is how the test suite
simulates a full race with no window (Chapter 14).

---

## The state

```c
// game/include/game/ai.h
typedef struct AIDriver {
    float skill;            // 0..1: corner speed, look-ahead and precision
    float aggression;       // 0..1: how late it lifts and how close it races
    float preferredOffset;  // resting lateral offset from the centreline
    float currentOffset;    // smoothed, includes avoidance
    float wobblePhase;      // keeps identical drivers from moving identically
    float recoverTimer;     // counts up while stuck, triggers a reverse
} AIDriver;
```

Six floats. No path plan, no state machine, no behaviour tree, no lookup table.
The entire driver is a pure function of the car's current state and the track
geometry, plus two smoothed values and one timer.

`race.c` spreads the field:

```c
// game/src/race.c
// Spread the field: later cars start slightly slower so the grid order
// is not simply reversed by lap one.
float skill = 0.92f - 0.055f * (float)i;
float aggression = 0.75f - 0.05f * (float)i;
float offset = ((i % 2) ? 0.10f : -0.10f);
AIDriverInit(&racer->ai, skill, aggression, offset, (unsigned int)(i * 7919 + 13));
```

Skill runs 0.92 down to 0.535 across eight cars. The alternating offset staggers
them left and right of the centre line so the grid does not form a single-file
queue. `7919` is prime, which spreads the wobble phases.

---

## Perception: how hard is the road ahead?

```c
// The road ahead is sampled at this spacing, this many times, to measure how
// much it turns. Two fixed probes are not enough: a tight hairpin is shorter
// than the gap between them, so both can land past the corner and report a
// straight road right as the car arrives at it.
#define AI_PROBE_STEP 0.55f
#define AI_PROBE_COUNT 7
```

The comment describes a real bug and its cause. With two probes at, say, 1.0 and
2.5 units ahead, a hairpin occupying units 1.2–1.8 falls entirely between them.
Both probes report a straight road. The AI arrives flat out.

Seven probes at 0.55 spacing cover 3.85 units of road with no gaps large enough
to hide a corner.

```c
float totalTurn = 0.0f;
float signedTurn = 0.0f;
float minRadius = 1e30f;

SplineSample probe = SplineSampleAt(spline, q.distance + AI_PROBE_STEP);
Vector2 previousDir = { probe.tangent.x, probe.tangent.z };

for (int i = 2; i <= AI_PROBE_COUNT; i++) {
    probe = SplineSampleAt(spline, q.distance + AI_PROBE_STEP * (float)i);
    Vector2 dir = { probe.tangent.x, probe.tangent.z };
    float step = AngleToRight(previousDir, dir);

    totalTurn += fabsf(step);
    signedTurn += step;
    if (fabsf(step) > 1e-4f) {
        float radius = AI_PROBE_STEP / fabsf(step);
        if (radius < minRadius) minRadius = radius;
    }
    previousDir = dir;
}
```

Three different measurements are extracted from one walk, and each answers a
different question.

### `totalTurn` — is there a corner anywhere in the window?

Sum of absolute turns. It catches a corner regardless of where in the 3.85-unit
window it sits, and it does not cancel out. An S-bend that turns 40° right then
40° left has `totalTurn = 80°` — correctly reported as "there is a lot of
cornering coming".

```c
float corner01 = Clamp(totalTurn / (PI * 0.5f), 0.0f, 1.0f);
```

Normalised against 90°: a full right-angle corner inside the window gives 1.0.

### `signedTurn` — which way?

Sum of signed turns. The S-bend above gives 0 — correctly, since it has no net
direction. Used only to decide which side the apex is on:

```c
float bendAngle = signedTurn;                      // positive: turns right
float insideSign = (bendAngle > 0.0f) ? 1.0f : -1.0f;
```

### `minRadius` — how fast can this physically be taken?

The tightest single step. Because each step covers `AI_PROBE_STEP` of arc and
turns by `step` radians, the local radius is `arc/angle`:

```c
float radius = AI_PROBE_STEP / fabsf(step);
```

This is the definition of curvature (`κ = dθ/ds`, `R = 1/κ`) evaluated as a
finite difference. It matters that this uses the *minimum* rather than the
average: an average would be diluted by the straight either side of a hairpin,
and the car would arrive too fast.

### The angle helper

```c
// The car's right-hand direction for a given heading. Matches CarRight(): for
// a heading (x, z) on the XZ plane, right is (-z, x).
static Vector2 RightOf(Vector2 dir)
{
    return (Vector2){ -dir.y, dir.x };
}

// Angle from `forward` to `target`, positive when the target is to the right.
// Expressed in the car's own frame so it feeds straight into the steer input,
// whose positive direction is also "right".
static float AngleToRight(Vector2 forward, Vector2 target)
{
    return atan2f(Dot2(target, RightOf(forward)), Dot2(target, forward));
}
```

`atan2(perpendicular component, parallel component)` is the standard way to get
a **signed** angle between two vectors. `acos(a·b)` gives you the magnitude but
never the sign, so it cannot tell left from right — and left from right is the
entire output of a steering controller.

The comment ties the sign convention to `CarRight()` explicitly. Chapter 07
showed how easy it is to get this backwards.

---

## Choosing a line

### The apex

```c
// Move towards the inside of the corner to clip the apex, scaled by skill.
// Lateral offsets are measured to the right, so a right-hand corner wants a
// positive offset.
float insideSign = (bendAngle > 0.0f) ? 1.0f : -1.0f;
float apexPull = corner01 * q.halfWidth * 0.55f * (0.4f + 0.6f * ai->skill);
float wobble = sinf((float)GetTime() * 0.7f + ai->wobblePhase) * 0.02f * (1.0f - ai->skill);
float wantOffset = ai->preferredOffset + insideSign * apexPull + wobble;
```

The target offset from the centre line is three terms:

1. **`preferredOffset`** — this driver's habitual position, ±0.10.
2. **`insideSign × apexPull`** — move toward the inside of whatever corner is
   coming. Scaled by `corner01` (no corner, no pull), by `halfWidth` (so it works
   on any track width), and by skill (0.4 at skill 0, 1.0 at skill 1).
3. **`wobble`** — a slow sine, amplitude `0.02 × (1 − skill)`, phased per driver.

Note this is a *simplification* of a real racing line. A proper one is
out-in-out: wide on entry, clip the apex, drift wide on exit. This only does the
apex part. The result is still convincingly race-like, because the apex is the
part a viewer notices, and it is one term instead of a trajectory optimiser.

**The wobble is a design device, not noise.** A perfect AI is unsettling — its
cars track a line with machine precision, which reads as fake even when a viewer
cannot say why. A 0.02-unit sway at 0.7 rad/s, scaled by *lack* of skill, makes
the back of the field look human and leaves the leaders sharp.

`ai->wobblePhase = (float)(seed % 1000) * 0.0062831853f` spreads phases across
2π (`0.00628 ≈ 2π/1000`), so cars do not sway in unison.

### Avoiding the car in front

```c
Vector2 forward = CarForward(car);
for (int i = 0; i < neighbourCount; i++) {
    Vector2 rel = { neighbours[i].position.x - car->position.x,
                    neighbours[i].position.y - car->position.y };
    float ahead = rel.x * forward.x + rel.y * forward.y;
    if (ahead < 0.05f || ahead > 1.6f) continue;

    Vector2 right = CarRight(car);
    float side = rel.x * right.x + rel.y * right.y;
    if (fabsf(side) > 0.55f) continue;

    // Steer around the far side: offsets are positive to the right, so a car
    // sitting on our right pushes us left.
    float urgency = (1.0f - ahead / 1.6f) * (1.0f - 0.35f * ai->aggression);
    float push = (side >= 0.0f) ? -1.0f : 1.0f;
    wantOffset += push * urgency * q.halfWidth * 0.9f;
}
wantOffset = Clamp(wantOffset, -q.halfWidth * 0.85f, q.halfWidth * 0.85f);
```

Project every neighbour into the car's own frame — `ahead` and `side` — and
filter twice:

- `ahead < 0.05` skips cars behind or beside. **You do not steer around
  something you have already passed.** Without this the AI would swerve for a
  car it just overtook.
- `ahead > 1.6` skips distant cars. About a quarter-second of closing at racing
  speed.
- `|side| > 0.55` skips cars on a different part of the track. Not in your path,
  not your problem.

**`urgency` grows as the gap closes** — 0 at 1.6 units, 1 at contact.

**Aggression reduces the avoidance.** An aggressive driver (0.75) applies only
`1 − 0.35 × 0.75 = 0.74` of the dodge, so it holds its line and leans on the
other car. That is a good example of a personality parameter expressed as a
physical behaviour rather than as a mode.

**The final clamp to ±0.85 half-width** keeps the AI on the tarmac. It will move
across the lane but not off it, which prevents the avoidance term from driving a
car into the gravel to dodge a wobble.

### Smoothing the offset

```c
float blend = 1.0f - expf(-4.5f * dt);
ai->currentOffset += (wantOffset - ai->currentOffset) * blend;
```

`wantOffset` is recomputed every tick and can jump — a neighbour crosses the
`ahead > 1.6` threshold, or a corner enters the probe window. Feeding a step
change into the steering controller gives a twitch. The exponential ease
(Chapter 01's `1 − e^(−rate·dt)` again, rate 4.5, time constant ~0.22 s) turns
each jump into a lane change.

Here an ease is right rather than a rate limit, because the target settles
(Chapter 07's rule).

---

## Steering: pure pursuit

```c
float speed01 = Clamp(car->speed / tuning->topSpeed, 0.0f, 1.0f);
float lookAhead = Lerp(0.55f, 1.9f, speed01) * (0.85f + 0.3f * ai->skill);
lookAhead *= 1.0f - 0.25f * corner01;

SplineSample aim = SplineSampleAt(spline, q.distance + lookAhead);
Vector2 aimRight = RightOf((Vector2){ aim.tangent.x, aim.tangent.z });
Vector2 targetPoint = { aim.position.x + aimRight.x * ai->currentOffset,
                        aim.position.z + aimRight.y * ai->currentOffset };

Vector2 toTarget = { targetPoint.x - car->position.x, targetPoint.y - car->position.y };
/* ... normalise ... */
float steerAngle = AngleToRight(forward, toTarget);
input.steer = Clamp(steerAngle * (1.7f + 0.8f * ai->skill), -1.0f, 1.0f);
```

**Pure pursuit** is the classic path-following controller from robotics: pick a
point on the path some distance ahead, steer toward it, repeat. It has been the
workhorse of autonomous vehicles since the 1980s because it is three lines and
it works.

The look-ahead distance is everything:

- **Too short** → the controller chases the path point-by-point and oscillates.
  The car weaves.
- **Too long** → the controller cuts corners, because it aims at a point beyond
  the bend and drives the chord.

So it is scaled three ways:

1. **By speed** — `Lerp(0.55, 1.9, speed01)`. Faster cars need to look further,
   because they cover more ground before the steering takes effect. This makes
   the *time* horizon roughly constant, which is the standard formulation.
2. **By skill** — a better driver looks further ahead (`0.85 + 0.3 × skill`).
3. **Shortened in corners** — `× (1 − 0.25 × corner01)`. In a tight corner the
   long look-ahead would aim past the apex and cut. Pulling it in makes the car
   follow the bend.

Then the target is offset laterally by `currentOffset` using the *aim point's*
right vector — so on a curve, the offset is perpendicular to the track there,
not to the track here.

**The steering gain** `1.7 + 0.8 × skill` converts an angle in radians into a
normalised steer input. It is a proportional controller: `steer = K × error`. A
better driver has a higher gain, so it corrects more decisively.

No integral term (no steady-state offset to eliminate), no derivative term (the
look-ahead already provides the phase lead a D-term would). Pure pursuit's
geometry does the anticipation.

---

## Pace

```c
float cornerScale = 1.0f - 0.52f * corner01 * (1.15f - 0.3f * ai->aggression);
float targetSpeed = tuning->topSpeed * cornerScale * (0.80f + 0.20f * ai->skill);
```

The heuristic layer: slow down in proportion to how much cornering is ahead. A
full 90° corner in the window cuts the target by roughly half. Aggression
reduces the reduction; skill raises the baseline.

These four constants are pure tuning, and `make test` reports lap times so they
can be tuned empirically:

```
6/6 finished, race time 88.0s, best lap 23.30s, leader avg speed 3.05 u/s
```

### The physical limit

```c
// Hard limit from the steering itself: following a radius r needs a yaw
// rate of v/r, and the car cannot exceed maxYawRate. Without this the AI
// arrives at a tight hairpin far too fast, understeers straight on and gets
// stuck against whatever is on the outside.
if (minRadius < 1e29f) {
    float physicalLimit = tuning->maxYawRate * minRadius * 0.85f;
    if (physicalLimit < targetSpeed) targetSpeed = physicalLimit;
}
```

This is the best line in the file, because it is not a heuristic — it is derived
from the vehicle model.

Chapter 07 established `ω = v/R` from the bicycle model, and `ω ≤ maxYawRate`
from the clamp. Therefore:

```
v ≤ maxYawRate × R
```

Exceed it and the car physically cannot follow the corner. It understeers
straight on, into the gravel or the barrier — which is precisely the failure the
comment describes.

The `0.85` is a safety margin: the model is kinematic and ignores the lateral
slide that grip permits, so leaving 15% keeps the car inside its real envelope.

Note this **overrides** the heuristic rather than blending with it. A hard
physical limit should be a `min`, not a term.

```c
// A climb bleeds speed anyway, so do not also brake for the corner beyond it.
targetSpeed = fmaxf(targetSpeed, 1.1f);

// Running wide costs grip, so ease off until the car is back on line.
if (fabsf(q.lateral) > q.halfWidth) targetSpeed *= 0.72f;
```

A floor of 1.1 u/s stops the AI from talking itself into a standstill on a
climb into a corner, where the gradient and the corner scale multiply.

The off-track reduction is a feedback term: once the car has run wide it has less
grip, so asking for less speed helps it get back.

### The throttle/brake controller

```c
float error = targetSpeed - car->forwardSpeed;
if (error > 0.05f) {
    input.throttle = Clamp(error * 1.6f, 0.0f, 1.0f);
} else if (error < -0.15f) {
    input.brake = Clamp(-error * 0.85f, 0.0f, 1.0f);
} else {
    input.throttle = 0.35f;
}
```

A proportional controller with a **deadband** between −0.15 and +0.05.

Without the deadband, an AI sitting exactly at its target speed would alternate
throttle and brake every tick — 120 times a second — which produces a car that
audibly and visibly hunts, and lays skid marks it should not. The deadband
replaces that with a steady 35% throttle, roughly enough to hold speed against
drag.

The asymmetry (0.05 above, 0.15 below) biases toward *not* braking. Braking is
more disruptive than coasting, so the controller tolerates being a little fast
before it intervenes.

The gains differ too: throttle 1.6, brake 0.85. Brakes are nearly twice as
strong as the engine (13.0 vs 7.2), so a lower gain gives comparable authority.

---

## Getting unstuck

```c
if (car->speed < 0.25f && input.throttle > 0.2f) {
    ai->recoverTimer += dt;
} else if (car->forwardSpeed > 0.6f) {
    ai->recoverTimer = 0.0f;
}
if (ai->recoverTimer > 1.2f) {
    // Reverse away from whatever we are wedged against, steering back to line.
    input.throttle = 0.0f;
    input.brake = 1.0f;
    input.steer = -input.steer;
    if (ai->recoverTimer > 2.6f) ai->recoverTimer = 0.0f;
}
```

The whole controller above assumes the car can move. Wedged against a barrier,
it cannot: `error` stays large, throttle stays at 1.0, and the car sits there
revving.

The detector is *"asking for throttle but not moving"* — both conditions, which
is what distinguishes stuck from stopped. A car legitimately at rest during the
countdown has no throttle and does not trigger it.

The recovery exploits the brake-is-reverse behaviour from Chapter 07: full brake
at a standstill reverses. And `input.steer = -input.steer` inverts the steering,
which — because reversing inverts the geometric relationship between steering
and path — turns the car back toward the line as it backs away. That is the same
reason you turn the wheel the "wrong" way when reversing a car into a space.

The timer resets at 2.6 s, so recovery is attempted in 1.4-second bursts with
1.2-second gaps. Continuous reversing would just drive backwards down the track.

**The hysteresis is asymmetric on purpose:** the timer accumulates below 0.25 u/s
but only resets above 0.6 u/s. A single band would let a car oscillating around
the threshold flicker in and out of recovery.

And there is a layered defence — `race.c` teleports anything still stuck after 4
seconds (`RACE_STUCK_RESPAWN`), so a failure of this recovery is not fatal to the
race. Chapter 09.

---

## What is deliberately absent

Worth listing, because the absences are the design:

- **No path planning.** No A*, no trajectory optimisation, no precomputed
  racing line. The line is computed fresh each tick from local geometry.
- **No knowledge of other cars' intentions.** Neighbours are positions and
  velocities, and only the velocity field is passed (unused). No prediction.
- **No memory of the track.** Lap 3 is driven exactly like lap 1. A learning AI
  would be a different and much larger project.
- **No difficulty rubber-banding.** Skill is fixed at init; nobody speeds up
  because the player is ahead.
- **No state machine.** `recoverTimer` is the only piece of behavioural state,
  and it is a float.

The result is 177 lines that produce a six-car race in which every car finishes,
lap times are within a few seconds of each other, and nobody is off track more
than 16% of the time — all asserted by `tests/test_race.c`.

**A reactive controller with good perception beats a planner with poor
perception.** Most of this file is perception (the probe walk) and most of the
rest is one classic controller.

---

## Exercises

1. **Watch it think.** Run `make test --verbose` and read the per-second
   telemetry for car 0: speed, throttle, brake, steer, on/off track, arc length,
   lap, gate, recover timer. Find the corners in the numbers.

2. **Break the probes.** Set `AI_PROBE_COUNT` to 2. Run `make test`. Which
   assertion fails, and on which circuit? Now set it to 20 — what happens, and
   why is more not better?

3. **Delete the physical limit.** Comment out the `minRadius` block. Run
   `make test` and read the off-track percentage and finish count. Then drive
   with `--autopilot --debug` and watch the AI at the hairpin.

4. **Remove the deadband.** Change the `else` branch to `input.throttle = 0.0f`
   and narrow the thresholds to ±0.0. Drive with `--autopilot` and watch the AI
   cars' skid marks and listen to the engine note.

5. **Add out-in-out.** Extend the line selection so the car is pushed *outward*
   before a corner and allowed to run wide on exit. You will need to know
   whether the corner is ahead or behind — the probe walk gives you that. Does it
   lower lap times? Does it look better?

6. **Predictive avoidance.** `AINeighbour` carries a `velocity` the code ignores.
   Use it: extrapolate each neighbour forward by ~0.3 s before computing `ahead`
   and `side`. Does overtaking improve? Does anything get worse?

7. **Tune for a target.** Change the four pace constants to make the AI lap
   circuit01 in 21 seconds instead of 23.3, without increasing the off-track
   percentage above 20%. `make test` is your feedback loop. Record what you
   changed and what it cost.

---

Next: [09 — Race rules and state](09-race-rules.md)
