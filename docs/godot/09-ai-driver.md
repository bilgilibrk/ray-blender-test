# 09 — The AI driver

> `scripts/game/ai_driver.gd` · `scripts/game/ai_neighbour.gd`. Mirrors
> [C chapter 08](../learn/08-ai-driver.md).

---

## The problem

Five opponents have to drive a circuit they have never seen, at a pace that is
beatable but not embarrassing, without hitting the barriers, without hitting
each other, and — the hard part — **looking like drivers rather than like a
train on rails.**

The cheap answers fail in recognisable ways:

- **Follow the centre line at a fixed fraction of top speed.** Every car takes
  the identical line at the identical speed. It is a parade.
- **Follow the centre line, brake when a corner is close.** Better, until a
  hairpin arrives faster than the braking distance and the car ploughs straight
  on into the gravel.
- **Give the AI the same inputs as the player and run a search.** Overkill,
  non-deterministic to tune, and it will still drive into a wall the first time
  the search horizon is a metre short.

What works is the thing real drivers do: **look ahead, decide how hard the road
ahead is, pick a speed you can carry through it, aim at a point down the road,
and steer at it.**

---

## Why not `NavigationServer3D`

Godot has a full navigation stack: bake a `NavigationRegion3D`, drop a
`NavigationAgent3D` on each car, set a target, read `get_next_path_position()`,
and get avoidance between agents for free.

It is the wrong tool, and the reasons generalise:

**The route is not the problem.** Navigation solves "how do I get from A to B
through this geometry". The AI already knows the route — it is the racing line,
and it is the same every lap. Path*finding* is solved before the race starts.
What is unsolved is *pace* and *line*, which navigation has no opinion about.

**Avoidance is tuned for pedestrians.** `NavigationAgent3D`'s avoidance (RVO)
produces agents that politely give way and slow down to avoid contact. A racer
wants cars that hold their line, take a defensive position, and occasionally
trade paint. Racing avoidance is a *lateral offset preference*, not a velocity
obstacle.

**The path is a polyline, so the speed is a step function.** Navigation gives
you the next corner point. It does not give you curvature, and curvature is the
one number this AI is built around.

**It would fight the vehicle model.** `NavigationAgent3D` wants to hand you a
velocity. Chapter 08's car does not accept a velocity, it accepts throttle,
brake and steer. Converting one to the other is inverse dynamics, which is a
harder problem than the one you started with.

Use navigation when the route is genuinely unknown and the geometry is genuinely
complicated: a marshal walking to an incident, a safety car finding the pit
entry from wherever it happens to be. For the race itself, the racing line is
the path and the spline already provides it.

---

## The state

```gdscript
# scripts/game/ai_driver.gd
class_name AIDriver
extends RefCounted

## Waypoint-following opponent driver.
##
## Steers at a point further down the racing line than the car currently is,
## picks a corner speed from how much the line bends ahead, and nudges its
## preferred line sideways to avoid cars it is closing on.
##
## A RefCounted, not a Node: it needs no transform and no tick of its own. It
## is called explicitly from the race director's physics step, which is what
## makes the ordering between "the AI decides" and "the car moves" something
## you can read rather than something you configure.

## 0..1: corner speed, look-ahead and precision.
var skill: float = 0.8
## 0..1: how late it lifts and how close it races.
var aggression: float = 0.7
## Resting lateral offset from the centre line, world units, + is right.
var preferred_offset: float = 0.0
## Smoothed, includes avoidance.
var current_offset: float = 0.0
## Keeps identical drivers from moving identically.
var wobble_phase: float = 0.0
## Counts up while stuck, triggers a reverse.
var recover_timer: float = 0.0

## Own clock, accumulated from the fixed step rather than read from the engine.
## The C version calls GetTime() here, which is the one place its otherwise
## deterministic simulation depends on wall-clock time — two runs of the same
## race diverge slightly. Accumulating delta instead costs one float and makes
## the whole AI reproducible, which chapter 15's tests rely on.
var _clock: float = 0.0

func _init(driver_skill: float, driver_aggression: float,
           offset: float, seed: int) -> void:
    skill = clampf(driver_skill, 0.0, 1.0)
    aggression = clampf(driver_aggression, 0.0, 1.0)
    preferred_offset = offset
    current_offset = offset
    wobble_phase = float(seed % 1000) * 0.0062831853
```

Six floats and a phase. That is the entire driver.

`skill` and `aggression` are the personality, and they are deliberately **two**
knobs rather than one difficulty slider, because they trade differently: a
high-skill low-aggression driver is fast and gives you room; a low-skill
high-aggression one is slow and in your way. Chapter 10 spreads the grid across
both.

`wobble_phase` derives from a seed so that six identical drivers do not produce
six identical lines. It is the cheapest possible way to make a field look
populated, and it costs one `sin` per tick.

---

## Perception: how hard is the road ahead?

```gdscript
## The road ahead is sampled at this spacing, this many times, to measure how
## much it turns. Two fixed probes are not enough: a tight hairpin is shorter
## than the gap between them, so both can land past the corner and report a
## straight road right as the car arrives at it.
const PROBE_STEP: float = 0.55
const PROBE_COUNT: int = 7

func think(car: CarBody, tuning: CarTuning, spline: TrackSpline,
           hint: int, neighbours: Array[AINeighbour], neighbour_count: int,
           delta: float) -> CarInput:
    _clock += delta
    _input.clear()
    if spline.count < 2:
        return _input

    var here := Vector3(car.plane_position.x, car.surface_height,
                        car.plane_position.y)
    spline.closest_into(here, hint, _query)

    # --- how hard is the road ahead -----------------------------------------
    # Walk a set of probes and accumulate how much the track turns. The total
    # catches a corner anywhere in the window; the tightest single step gives
    # the local radius, which is what actually limits corner speed.
    var total_turn: float = 0.0
    var signed_turn: float = 0.0
    var min_radius: float = INF

    spline.sample_into(_query.distance + PROBE_STEP, _probe)
    var previous_dir := Vector2(_probe.tangent.x, _probe.tangent.z)

    for i: int in range(2, PROBE_COUNT + 1):
        spline.sample_into(_query.distance + PROBE_STEP * float(i), _probe)
        var dir := Vector2(_probe.tangent.x, _probe.tangent.z)
        var step: float = previous_dir.angle_to(dir)

        total_turn += absf(step)
        signed_turn += step
        if absf(step) > 1e-4:
            min_radius = minf(min_radius, PROBE_STEP / absf(step))
        previous_dir = dir

    var bend_angle: float = signed_turn                          # + turns right
    var corner01: float = clampf(total_turn / (PI * 0.5), 0.0, 1.0)
```

Seven probes at 0.55 units is a window of **3.85 units ahead** — a bit over half
a second at racing pace, and about six car lengths. That number is the single
most important tuning decision in the file. Too short and the car brakes too
late; too long and it brakes for corners that are still two corners away.

### `Vector2.angle_to`, and why it is the right sign here

The C version writes a helper:

```c
// Angle from `forward` to `target`, positive when the target is to the right.
static float AngleToRight(Vector2 forward, Vector2 target)
{
    return atan2f(Dot2(target, RightOf(forward)), Dot2(target, forward));
}
```

Godot has this built in. `a.angle_to(b)` is `atan2(a.cross(b), a.dot(b))`, and
in the `(x, z)` mapping this project uses, `a.cross(b)` is exactly
`dot(b, RightOf(a))`. So `previous_dir.angle_to(dir)` **is** `AngleToRight`,
with no sign to fix — one of the few places the port gets shorter.

That is a claim worth a test rather than a paragraph:

```gdscript
# tests/test_ai_geometry.gd
func test_angle_to_is_positive_to_the_right() -> void:
    for yaw_degrees: int in [0, 45, 137, 250]:
        var yaw: float = deg_to_rad(float(yaw_degrees))
        var fwd: Vector2 = Convention.forward(yaw)
        var rgt: Vector2 = Convention.right(yaw)
        # A target 30 degrees to the right of forward.
        var target: Vector2 = (fwd * cos(0.5236) + rgt * sin(0.5236)).normalized()
        assert_float(fwd.angle_to(target)).is_equal_approx(0.5236, 1e-4)
```

If that test fails after a refactor, every steering decision in the file has
silently reversed, and nothing else will tell you.

### The three measurements

**`total_turn`** — is there a corner *anywhere* in the window? It sums the
absolute turn, so an S-bend (right then left, netting zero) still reports as
demanding. This is what sets the braking.

**`signed_turn`** — which way? Sums with sign, so the S-bend nets out. This is
what chooses which side of the road to aim at, and netting out through an S is
correct: there is no apex to clip, so stay near the middle.

**`min_radius`** — how fast can this physically be taken? A turn of `θ` radians
over an arc of `PROBE_STEP` implies a radius of `PROBE_STEP / θ`. Taking the
minimum finds the *tightest point* in the window rather than an average, which
matters because a hairpin preceded by a gentle sweeper must be paced for the
hairpin.

`corner01` normalises `total_turn` against 90° so it reads 0 on a straight and 1
in anything at or beyond a right-angle. Every downstream decision uses this
normalised form, so retuning "how much do corners matter" is one constant.

---

## Choosing a line

```gdscript
    # --- pick a racing line ------------------------------------------------
    # Move towards the inside of the corner to clip the apex, scaled by skill.
    # Lateral offsets are measured to the right, so a right-hand corner wants a
    # positive offset.
    var inside_sign: float = 1.0 if bend_angle > 0.0 else -1.0
    var apex_pull: float = corner01 * _query.half_width * 0.55 * (0.4 + 0.6 * skill)
    var wobble: float = sin(_clock * 0.7 + wobble_phase) * 0.02 * (1.0 - skill)
    var want_offset: float = preferred_offset + inside_sign * apex_pull + wobble
```

This is a *simplification* of a real racing line, and it is worth naming what it
leaves out. A proper line is out-in-out: wide on entry, tight at the apex, wide
on exit. This one only does the middle part — it moves towards the inside in
proportion to how much the road is bending *right now*, which naturally produces
something out-in-out-ish, because the bend measurement peaks at the apex.

It is not optimal. It is one line of code, it never produces a line that leaves
the track, and the difference between it and an optimised line is worth maybe
2% of lap time on this circuit — which is less than the spread between skill
levels. Exercise 4.1 in chapter 18 is about closing that gap properly.

`(0.4 + 0.6 * skill)` means a skill-0 driver uses 40% of the available apex pull
and a skill-1 driver all of it. The low-skill car drives a rounder, slower line —
which is what being worse at driving looks like, rather than "the same line
with less throttle".

The `wobble` term is scaled by `(1.0 - skill)`, so the best driver is perfectly
steady and the worst wanders by 2 cm. Two centimetres is nothing mechanically
and is very visible from a chase camera.

### Avoiding the car in front

```gdscript
    # --- avoid cars we are closing on ---------------------------------------
    var fwd: Vector2 = car.forward()
    var rgt: Vector2 = car.right()
    for i: int in neighbour_count:
        var rel: Vector2 = neighbours[i].position - car.plane_position
        var ahead: float = rel.dot(fwd)
        if ahead < 0.05 or ahead > 1.6:
            continue                    # behind us, or too far to matter

        var side: float = rel.dot(rgt)
        if absf(side) > 0.55:
            continue                    # far enough across not to be in the way

        # Steer around the far side: offsets are positive to the right, so a
        # car sitting on our right pushes us left.
        var urgency: float = (1.0 - ahead / 1.6) * (1.0 - 0.35 * aggression)
        var push: float = -1.0 if side >= 0.0 else 1.0
        want_offset += push * urgency * _query.half_width * 0.9

    want_offset = clampf(want_offset, -_query.half_width * 0.85,
                         _query.half_width * 0.85)
```

Three gates, in increasing cost order: behind us, too far, too far across. The
1.6-unit window is under three car lengths — this is a "do not rear-end the car
in front" system, not a strategic one.

**`urgency` grows as the gap closes**, linearly from zero at 1.6 units to one at
contact. Multiplying by `(1 - 0.35 * aggression)` means an aggressive driver
moves over *less*: it will sit closer before yielding. That single term is most
of what makes the field feel like it has personalities.

**The final clamp to ±85% of the half-width** is what stops avoidance from
driving a car off the road to be polite. It is also why two cars side by side
into a corner both stay on the track: neither can be pushed past the edge.

What this deliberately does **not** do: predict where the neighbour will be.
`AINeighbour` carries a velocity and this code ignores it. Extrapolating
positions produces cars that swerve for gaps that never materialise, and the
1.6-unit window is short enough that current position is a good enough proxy.
Exercise 3 revisits that.

### Smoothing the offset

```gdscript
    current_offset += (want_offset - current_offset) * (1.0 - exp(-4.5 * delta))
```

`want_offset` is a step function: it jumps the moment a neighbour enters the
window and jumps back when it leaves. Feeding that straight to the steering
would produce a twitch. The exponential smoother at rate 4.5 gives a time
constant of 0.22 s, which is roughly a human's reaction-plus-hands time and
reads as a deliberate move rather than a flinch.

Chapter 01's `Smoothing.factor` is exactly this expression; it is written out
here because the AI file is otherwise self-contained.

---

## Steering: pure pursuit

```gdscript
    # --- steer towards a point down the line ---------------------------------
    var speed01: float = clampf(car.speed / tuning.top_speed, 0.0, 1.0)
    var look_ahead: float = lerpf(0.55, 1.9, speed01) * (0.85 + 0.3 * skill)
    look_ahead *= 1.0 - 0.25 * corner01

    spline.sample_into(_query.distance + look_ahead, _aim)
    var aim_right := Vector2(-_aim.tangent.z, _aim.tangent.x)
    var target := Vector2(_aim.position.x, _aim.position.z) \
            + aim_right * current_offset

    var to_target: Vector2 = (target - car.plane_position).normalized()
    var steer_angle: float = fwd.angle_to(to_target)
    _input.steer = clampf(steer_angle * (1.7 + 0.8 * skill), -1.0, 1.0)
```

**Pure pursuit** is the classic: pick a point on the path some distance ahead,
steer at it, repeat. It is used in real autonomous vehicles, and its entire
behaviour is governed by one parameter — the look-ahead distance.

- **Too short**: the car chases the line tightly, oscillating around it, because
  every correction overshoots before the next sample.
- **Too long**: the car cuts corners, because it aims at a point across the
  inside of the bend.

So it is scheduled, three ways:

| Term | Range | Why |
|---|---|---|
| `lerpf(0.55, 1.9, speed01)` | 0.55–1.9 u | Faster means look further. At 6.6 u/s, 1.9 units is 0.29 s ahead |
| `(0.85 + 0.3 * skill)` | ×0.85–1.15 | Better drivers look further and are smoother |
| `(1.0 - 0.25 * corner01)` | ×0.75–1.0 | Shorten in corners so the car does not cut the apex |

The final gain, `(1.7 + 0.8 * skill)`, converts an angle in radians to a steer
input in −1..1. At skill 1 a 0.4-radian error saturates the input. Higher gain
means a car that corrects harder and, past a point, oscillates — that is a
proportional controller and it has the stability behaviour of one.

`aim_right` is `tangent × up` again, the same expression as in chapter 06's
`_fill`. The offset is applied in the *aim point's* frame, not the car's, which
is what makes "hold a line 20 cm right of centre" mean the same thing through a
corner as on a straight.

---

## Pace

```gdscript
    # --- pace ----------------------------------------------------------------
    var corner_scale: float = 1.0 - 0.52 * corner01 * (1.15 - 0.3 * aggression)
    var target_speed: float = tuning.top_speed * corner_scale * (0.80 + 0.20 * skill)
```

A right-angle corner (`corner01 == 1`) at aggression 0.7 scales top speed by
`1 - 0.52 * 0.94 = 0.51`. So the AI takes a 90° corner at about half its top
speed, before the physical limit below trims it further.

`(0.80 + 0.20 * skill)` is the flat-out pace ceiling: the worst driver runs at
80% of top speed on the straights. That is a *large* handicap — it is what stops
a six-car field from finishing nose-to-tail — and it is the first number to
change if the difficulty is wrong.

### The physical limit

```gdscript
    # Hard limit from the steering itself: following a radius r needs a yaw
    # rate of v/r, and the car cannot exceed max_yaw_rate. Without this the AI
    # arrives at a tight hairpin far too fast, understeers straight on and gets
    # stuck against whatever is on the outside.
    if min_radius < INF:
        target_speed = minf(target_speed, tuning.max_yaw_rate * min_radius * 0.85)
```

This is the most valuable four lines in the file, and it is the one piece of AI
code derived from the vehicle model rather than tuned.

Following a circle of radius `r` at speed `v` requires yawing at `v / r`.
Chapter 08 clamps yaw rate at `max_yaw_rate`. Therefore the maximum speed at
which the car *can physically follow* a radius `r` is `max_yaw_rate * r`, and no
amount of steering input changes that.

The `0.85` is margin: at exactly the limit the car is on the clamp with nothing
left for a correction, and any disturbance — a kerb, a nudge — puts it wide.

The failure mode without this is specific and recognisable: the AI brakes the
"right" amount for a corner according to `corner01`, arrives at a hairpin whose
radius is 1.2 units at 3.0 u/s, needs 2.5 rad/s of yaw, gets 3.4 but not the
*line*, understeers wide, and beaches itself on the outside barrier. Every lap.
At the same corner.

```gdscript
    # A climb bleeds speed anyway, so do not also brake for the corner beyond it.
    target_speed = maxf(target_speed, 1.1)

    # Running wide costs grip, so ease off until the car is back on line.
    if absf(_query.lateral) > _query.half_width:
        target_speed *= 0.72
```

The 1.1 floor prevents a deadlock: a very tight corner on a climb can produce a
target speed low enough that the car never reaches it, sits at zero throttle,
and stops. A floor of 1.1 u/s means the AI always intends to be moving.

### The throttle/brake controller

```gdscript
    var error: float = target_speed - car.forward_speed
    if error > 0.05:
        _input.throttle = clampf(error * 1.6, 0.0, 1.0)
    elif error < -0.15:
        _input.brake = clampf(-error * 0.85, 0.0, 1.0)
    else:
        _input.throttle = 0.35
```

A proportional controller with a **deadband** and an asymmetric one at that:
+0.05 on the throttle side, −0.15 on the brake side. The asymmetry means the car
tolerates being slightly too fast without braking, which is what a driver does —
braking is expensive and being 0.1 u/s over is not a problem.

The `else` branch is the interesting one. Inside the deadband it applies 35%
throttle rather than zero, which roughly balances drag at racing speed. Zero
throttle in the deadband would make the car decelerate until it left the
deadband, then accelerate back in — a slow oscillation you can see from the
chase camera as a car that surges.

No integral term, no derivative term. An integral would wind up during the
countdown and launch the car; a derivative would amplify the noise from the
probe measurements. For a first-order system with a well-chosen deadband, P is
enough.

---

## Getting unstuck

```gdscript
    # --- unstick --------------------------------------------------------------
    if car.speed < 0.25 and _input.throttle > 0.2:
        recover_timer += delta
    elif car.forward_speed > 0.6:
        recover_timer = 0.0

    if recover_timer > 1.2:
        # Reverse away from whatever we are wedged against, steering back to line.
        _input.throttle = 0.0
        _input.brake = 1.0
        _input.steer = -_input.steer
        if recover_timer > 2.6:
            recover_timer = 0.0

    return _input
```

The detector is "asking for throttle and not moving", which is exactly what
being wedged against a barrier looks like and is not what waiting on the grid
looks like — provided the race director clears the timer during the countdown,
which chapter 08 already flagged.

**The reset conditions are deliberately asymmetric.** The timer accumulates
whenever stuck, but only resets on `forward_speed > 0.6` — actually driving
forwards, not merely moving. A car being pushed sideways by another does not
count as recovered.

**`steer = -steer`** is the whole reversing strategy. Chapter 08's brake-becomes-
reverse means `brake = 1.0` reverses at a standstill, and reversing with
inverted steering swings the nose back towards the line. It is the manoeuvre you
do in a car park, in three lines.

**The 2.6 s reset** ends the reverse whether or not it worked, so the car tries
forward again. Without it, a car wedged in a way reversing cannot fix reverses
forever. With it, the car alternates — and if that also fails, chapter 10's
four-second stuck timer teleports it back to the racing line. Three layers of
increasingly blunt recovery, which is roughly the right number.

---

## What is deliberately absent

Naming the omissions is as useful as explaining the code:

- **No knowledge of other cars' futures.** Positions only, no extrapolation.
- **No knowledge of the barriers.** The AI never queries the collision world. It
  stays on the track by aiming at the racing line and never running wide enough
  to matter — and when that fails, it recovers rather than avoids.
- **No racing-line optimisation.** No offline solve, no curvature-minimising
  path. The line is the centre line plus a bend-proportional offset.
- **No overtaking logic.** Cars pass because one is faster and the other's
  avoidance moves it over. There is no "plan a pass into turn 3".
- **No mistakes.** The AI never locks a wheel, never runs wide deliberately,
  never has a bad lap. Chapter 18 suggests adding some, and it is a much better
  difficulty knob than lowering the pace ceiling.
- **No learning.** Nothing persists between laps or races.

That is roughly 180 lines of GDScript producing a field that races. The lesson
the C series draws is worth repeating: **the intelligence in this AI is entirely
in the *measurements*, not in the decision-making.** `corner01`, `min_radius`
and `lateral` are three well-chosen numbers, and once you have them the
decisions are a handful of multiplications. Time spent finding the right thing
to measure beats time spent on a cleverer controller.

---

## Cost, and the scratch objects

Per car per tick: one `closest_into`, six `sample_into` calls for the probes,
one more for the aim point, and up to five neighbour tests. For six cars at
120 Hz that is roughly **5,800 spline samples a second** on top of the race
director's own queries.

Every one of those writes into a reused object:

```gdscript
var _query := SplineQuery.new()
var _probe := SplineQuery.new()
var _aim := SplineQuery.new()
var _input := CarInput.new()
```

Four allocations per driver, at construction, and none afterwards. Returning a
fresh `CarInput` from `think()` would be 720 objects a second for a record of
four floats — chapter 03's rule, applied where it actually pays.

The caller must know it is being handed a shared object, so say so:

```gdscript
## Produces the control input for one tick. The returned CarInput is owned by
## this driver and is overwritten on the next call — copy it if you need to
## keep it (a replay recorder does; the race director does not).
func think(...) -> CarInput:
```

That comment is load-bearing. A reused return value is a real footgun, and the
only thing that makes it safe is that the contract is stated.

---

## Exercises

1. **Watch the probes.** Draw the seven probe points and the aim point for one
   car as debug spheres. Drive behind it through the fast chicane on
   `circuit02`. Where does the aim point go that surprises you?

2. **Break the physical limit.** Delete the `min_radius` clamp and watch the AI
   at the tightest hairpin. Time how long before a car beaches itself, and
   confirm that raising `corner_scale`'s coefficient does not fix it.

3. **Predictive avoidance.** `AINeighbour` carries a velocity that this code
   ignores. Use it: extrapolate each neighbour 0.3 s and avoid the predicted
   position. Race twenty laps with and without. Count contacts, and count the
   times a car swerves for a gap that was never there.

4. **Two knobs, not one.** Set every driver to `skill = 0.5` and spread
   `aggression` from 0.1 to 1.0. Then do the reverse. Which spread produces a
   more interesting race, and which produces a faster field?

5. **Determinism.** Replace `_clock += delta` with
   `Time.get_ticks_msec() / 1000.0` — the C version's behaviour. Run the same
   headless race twice and diff the finishing times. How large is the
   divergence after three laps, and where does it come from given that the
   wobble is only 2 cm?

6. **Make it slower, believably.** Lower the pace ceiling `(0.80 + 0.20 * skill)`
   until the AI is beatable. Then undo that and instead make the AI occasionally
   miss a braking point (a 5% chance per corner of applying 70% of the target
   brake). Play both. Which is more fun to race against, and which one is more
   fun to *beat*?

---

Next: [10 — Race rules and state](10-race-rules.md)
