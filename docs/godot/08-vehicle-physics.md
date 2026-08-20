# 08 — Vehicle physics

> `scripts/game/car_tuning.gd` · `scripts/game/car_body.gd` ·
> `scripts/game/car_surface.gd`. Mirrors
> [C chapter 07](../learn/07-vehicle-physics.md) — the chapter with the least
> Godot in it, because this is the game rather than the engine.

---

## The problem

A car needs to accelerate, brake, steer, slide, and be fun. "Fun" is doing a lot
of work in that sentence, and it is why this is not a physics problem.

A real tyre model — slip angles, load transfer, a Pacejka curve per wheel — is
a week of work, needs data you do not have, and produces a car that spins when
you brush the throttle mid-corner. What an arcade racer wants is a car that
holds a line when you are smooth, slides progressively when you are not, and
whose behaviour you can *state in one sentence*.

Here is the sentence:

> Split the velocity into the car's forward and lateral components each tick.
> The engine and brakes act on the forward part. Tyre grip bleeds off the
> lateral part. Heading comes from a bicycle steering model. Letting the lateral
> component survive when grip is low is what produces drift.

That is the whole model. Everything below is detail.

---

## What Godot does not give you

Worth being explicit, because chapter 07 already dismissed `VehicleBody3D` and
`RigidBody3D`: **nothing in this chapter is an engine feature.** Godot supplies
`delta`, `move_and_slide`, and the maths library. The vehicle is 150 lines you
write, exactly as in C, and the port is close to line-for-line because chapter
05 established that the two engines' conventions differ by exactly π — so every
relative angle, including every sign in this file, is unchanged.

The Godot-specific content is in five places, flagged as they arrive:

1. `delta` is already fixed, so the C engine's fixed-stepper is gone.
2. `move_toward` is the right tool for the two rate limits.
3. The typed maths functions (`clampf`, `lerpf`, `absf`) rather than the generic
   ones.
4. Applying yaw *and* pitch to a `Node3D` without them fighting.
5. Y is not driven by `move_and_slide`.

---

## The tuning

Every number, in one `Resource`, with its units:

```gdscript
# scripts/game/car_tuning.gd
class_name CarTuning
extends Resource

## Tuned for the Kenney kit's scale: one unit is one road tile, the drivable
## lane is 0.69 wide, and a car is roughly 0.30 x 0.60. Nothing here is in
## metres and nothing is in metres per second squared; reading it that way
## will confuse you.

@export_group("Engine")
## Forward acceleration at full throttle, u/s^2.
@export var engine_power: float = 7.2
## u/s^2.
@export var brake_power: float = 13.0
@export var top_speed: float = 6.6
@export var reverse_speed: float = 2.0
@export var drag_linear: float = 0.30
@export var drag_quadratic: float = 0.030

@export_group("Steering")
@export var wheelbase: float = 0.45
## Radians of steering lock when nearly stopped.
@export var max_steer_low_speed: float = 0.62
## Radians of lock at top speed.
@export var max_steer_top_speed: float = 0.24
## How fast the wheels reach the commanded angle, radians per second.
@export var steer_rate: float = 7.0
## Clamp that stops low-speed pirouettes, radians per second.
@export var max_yaw_rate: float = 3.4

@export_group("Grip")
## Lateral velocity decay rate, 1/s.
@export var grip_tarmac: float = 9.5
@export var grip_grass: float = 3.0
## Gravel holds a sliding car better than wet grass does — it is what stops a
## spin rather than letting it run — but nothing about it lets you steer,
## because there is barely any speed left to steer with.
@export var grip_sand: float = 6.0
## Multiplier applied to grip while the handbrake is down.
@export var handbrake_grip: float = 0.22

@export_group("World")
## Pull of a gradient along the road, u/s^2. About 1.5x the engine's own
## acceleration. True gravity at this scale would be nearer 2.4x, which made
## the 18% climbs on the demo circuit a crawl; this keeps hills clearly felt
## but still driveable.
@export var gravity: float = 11.0

@export_group("Body")
@export var half_width: float = 0.148
@export var half_length: float = 0.298

@export_group("Off track")
@export var off_track_speed_scale: float = 0.58
## A gravel trap is meant to end your lap, not to be a slower line through the
## corner. The ceiling takes the speed off as the car ploughs in; the drag is
## what keeps it off, settling full throttle at about 0.95 u/s — a seventh of
## the pace on tarmac, and slow enough that the marshals' six-second rescue is
## the realistic way out.
@export var sand_speed_scale: float = 0.30
## Extra linear drag in gravel, 1/s.
@export var sand_drag: float = 3.5
```

Making this a `Resource` rather than constants buys three things the C version
does not have: a designer can retune without touching code, the inspector shows
the doc comments as tooltips, and a race can hand different cars different
tuning — a handicap system, or a "the AI leader is 4% slower" rubber band, is
now a `duplicate()` and one field.

It also introduces chapter 03's sharing trap. Every car in a race is handed the
*same* `CarTuning` instance and none of them may write to it. If you want a
per-car variation, `duplicate()` first.

---

## Surfaces as a parameter

The car does not know what a gravel trap is. It is told, every tick, what it is
standing on:

```gdscript
# scripts/game/car_surface.gd
class_name CarSurface
extends RefCounted

## What the car is standing on this tick. Driving stays a 2D problem on the XZ
## plane; height and grade ride along so gradients affect speed and the car can
## be drawn sitting on the road.
##
## One instance per car, refilled each tick — see chapter 03.

var grip: float = 9.5           ## lateral velocity decay rate, 1/s
var speed_scale: float = 1.0    ## multiplier on top speed
var drag: float = 0.0           ## extra linear drag from the surface, 1/s
var grade: float = 0.0          ## rise over run along the direction of travel
var height: float = 0.0         ## surface height under the car
```

Five floats, and they are the entire interface between the world and the
vehicle. That is what makes the car testable without a level: chapter 15 drives
one for 600 ticks against a hand-made `CarSurface` and asserts on the resulting
speed, with no spline, no circuit and no scene.

It is also what makes new surfaces free. Ice is
`grip = 1.2, speed_scale = 1.0, drag = 0.0`. Deep water is
`grip = 8.0, speed_scale = 0.25, drag = 6.0`. No new branches anywhere in this
file.

The race director fills it (chapter 10):

```gdscript
# scripts/game/race_director.gd
surface.grip = tuning.grip_tarmac if on_track \
        else (tuning.grip_sand if in_sand else tuning.grip_grass)
surface.speed_scale = 1.0 if on_track \
        else (tuning.sand_speed_scale if in_sand else tuning.off_track_speed_scale)
surface.drag = tuning.sand_drag if in_sand else 0.0
surface.height = query.position.y
# The grade is signed along the centre line, so a car facing back down the
# track has to see it reversed or a climb would push it along.
var alignment: float = forward.dot(Vector2(query.tangent.x, query.tangent.z))
surface.grade = -query.grade if alignment < 0.0 else query.grade
```

That last one is the kind of bug that takes an afternoon: reverse up a hill and
the car accelerates. The grade is a property of the *track*, signed down-track;
the car needs it signed along its own direction of travel.

---

## One tick, in order

```gdscript
# scripts/game/car_body.gd
class_name CarBody
extends CharacterBody3D

@export var tuning: CarTuning

## Simulation state. The car is solved on the XZ plane; y is presentation.
var plane_position: Vector2 = Vector2.ZERO
var plane_velocity: Vector2 = Vector2.ZERO
var yaw: float = 0.0                ## radians; 0 faces -Z, Godot's forward
var steer_angle: float = 0.0
var surface_height: float = 0.0
var pitch: float = 0.0              ## radians, nose-up positive; visual only

## Derived each tick, for audio, effects and the HUD.
var speed: float = 0.0
var forward_speed: float = 0.0
var lateral_speed: float = 0.0
var slip: float = 0.0               ## 0..1, how much the tyres are sliding
var yaw_rate: float = 0.0
var slope_accel: float = 0.0
var on_track: bool = true
var in_sand: bool = false

func forward() -> Vector2:
    return Vector2(-sin(yaw), -cos(yaw))

func right() -> Vector2:
    return Vector2(cos(yaw), -sin(yaw))
```

### Clamp the inputs

```gdscript
func integrate(input: CarInput, surface: CarSurface, delta: float) -> void:
    if delta <= 0.0:
        return

    var throttle: float = clampf(input.throttle, 0.0, 1.0)
    var brake: float = clampf(input.brake, 0.0, 1.0)
    var steer: float = clampf(input.steer, -1.0, 1.0)
```

Clamping at the top of the function rather than trusting callers is not
defensive programming for its own sake. The AI in chapter 09 computes throttle
as `error * 1.6`, which is unbounded by construction; a gamepad axis can read
slightly outside ±1 on some hardware; and a replay file is untrusted input. One
clamp here means none of those can produce a car that accelerates at four times
engine power.

Note `clampf`, not `clamp`. Godot 4 has both: `clamp` is the Variant-generic
version and `clampf` takes and returns `float`. Chapter 02's typed rule applies
— use the typed variants (`clampf`, `clampi`, `absf`, `absi`, `minf`, `maxf`,
`lerpf`, `snappedf`) everywhere in numeric code. They are type-safe *and* they
skip a Variant dispatch, which in a function that runs 720 times a second is not
nothing.

### Decompose the velocity

```gdscript
    var fwd: Vector2 = forward()
    var rgt: Vector2 = right()

    var v_long: float = plane_velocity.dot(fwd)
    var v_lat: float = plane_velocity.dot(rgt)
```

This is the heart of the model. `plane_velocity` is in world space; `v_long` and
`v_lat` are the same vector expressed in the car's frame. Everything from here
to the recombination step works on those two scalars, which is why the whole
model fits in a page: **a two-dimensional vector problem has become two
one-dimensional scalar problems.**

`v_lat` is the sideways speed. On a car with grip it is nearly zero. In a drift
it is large. `slip`, further down, is essentially the ratio of the two.

### Steering: a speed-sensitive lock

```gdscript
    # Lock tightens with speed so the car stays controllable flat out.
    var speed_fraction: float = clampf(absf(v_long) / tuning.top_speed, 0.0, 1.0)
    var max_steer: float = lerpf(tuning.max_steer_low_speed,
                                 tuning.max_steer_top_speed, speed_fraction)
    # A rate limit, not an ease: the wheels turn at a constant rate towards the
    # commanded angle and stop exactly on it. move_toward is exactly this, and
    # unlike `lerp(a, b, delta * k)` it is frame-rate correct by construction.
    steer_angle = move_toward(steer_angle, steer * max_steer,
                              tuning.steer_rate * delta)
```

0.62 radians (36°) of lock parked, 0.24 (14°) at 6.6 u/s. Without the taper, a
flick of the stick at top speed produces a yaw rate the car cannot support, the
lateral velocity spikes, and the car spins. With it, the same input at speed is
a lane change.

`move_toward` is the Godot function for a rate limit. It is the correct choice
here and in exactly one other place (surface height, below); everywhere else in
this project uses `1 - exp(-rate * delta)`. Knowing which you want is worth
stating plainly:

| | Behaviour | Use when |
|---|---|---|
| `move_toward(a, b, rate * delta)` | Constant speed, arrives exactly, in finite time | The thing has a physical maximum rate — a steering rack, a suspension travel |
| `a + (b - a) * (1 - exp(-rate * delta))` | Exponential, never quite arrives | The thing is a smoothing filter — a camera, a readout, an AI's chosen line |

Both are frame-rate independent. `lerp(a, b, 0.1)` is neither.

### Longitudinal forces

```gdscript
    var accel: float = 0.0
    var top_speed: float = tuning.top_speed * surface.speed_scale

    if throttle > 0.0:
        # Taper power near the limit instead of clamping, which would feel
        # abrupt: the car eases up to top speed rather than hitting a wall.
        var headroom: float = 1.0 - clampf(v_long / maxf(top_speed, 0.001), 0.0, 1.0)
        accel += tuning.engine_power * throttle * headroom

    if brake > 0.0:
        if v_long > 0.05:
            accel -= tuning.brake_power * brake
        else:
            # Standing still or already rolling back: brake doubles as reverse.
            var headroom: float = 1.0 - clampf(-v_long / tuning.reverse_speed, 0.0, 1.0)
            accel -= tuning.engine_power * 0.55 * brake * headroom

    accel -= (tuning.drag_linear + surface.drag) * v_long
    accel -= tuning.drag_quadratic * v_long * absf(v_long)
```

**The headroom taper** is why the car does not feel like it hits a wall at top
speed. Engine force falls linearly to zero as `v_long` approaches the limit, so
the approach is asymptotic — which is also what a real engine's torque curve
does against rising drag, arrived at for feel rather than for fidelity.

**`maxf(top_speed, 0.001)`** guards a division. `speed_scale` is data, and a
level or a tuning resource can set it to zero. Without the guard that is an
infinity that propagates into the position and puts the car at NaN, from which
nothing recovers — and NaN positions in Godot produce silent misbehaviour in
the physics server rather than a clean error.

**Brake becomes reverse below 0.05 u/s.** One button, two meanings, decided by
current speed. The 0.05 threshold is small enough that you cannot feel it and
large enough that float noise cannot flicker across it.

**Two drag terms.** Linear drag dominates at low speed and sets how quickly the
car coasts to a stop; quadratic dominates at high speed and, with engine power,
is what actually determines the top speed on the flat. Tuning them separately
lets you change "how draggy it feels when you lift" without changing "how fast
it goes".

### Gravity along the road

```gdscript
    # sin(atan(grade)) resolves the slope into the direction of travel, so a
    # climb costs speed and a descent gives it back.
    var slope: float = surface.grade
    slope_accel = -tuning.gravity * (slope / sqrt(1.0 + slope * slope))
    accel += slope_accel

    v_long += accel * delta

    # A steep enough descent should be able to push past the flat-road limit,
    # which is where the speed on a downhill run comes from.
    var downhill_allowance: float = 1.35 if slope_accel > 0.0 else 1.0
    v_long = clampf(v_long, -tuning.reverse_speed, top_speed * downhill_allowance)
```

`slope / sqrt(1 + slope²)` is `sin(atan(grade))`, computed without the
trigonometry. The grade is rise-over-run — a tangent — and the component of
gravity along a slope is `g·sin θ`. Writing it this way is one divide and one
square root instead of two transcendental calls, in a function that runs 720
times a second, and it is exact rather than approximate.

The `1.35` downhill allowance is the kind of number that only exists because
somebody drove the circuit. Without it, a long descent is capped at the same
speed as the flat and the hill has no payoff.

### The creep killer

```gdscript
    # Kill the last sliver of creep so a stopped car actually stops — but only
    # on the flat, or a car parked on a hill would freeze in mid-air.
    if throttle <= 0.0 and brake <= 0.0 \
            and absf(v_long) < 0.02 and absf(slope_accel) < 0.05:
        v_long = 0.0
```

Linear drag is proportional to speed, so it approaches zero but never reaches
it. Without this, a stationary car drifts forward at 0.003 u/s forever, which
over a three-minute race is 0.5 units — enough to roll out of a pit box, and
enough to keep the audio's engine note from settling.

The `slope_accel` condition is the interesting half: on a hill, gravity is still
acting, and zeroing the velocity there would produce a car glued to a slope.

### Lateral grip

```gdscript
    var lateral_grip: float = surface.grip
    if input.handbrake:
        lateral_grip *= tuning.handbrake_grip
    v_lat *= exp(-lateral_grip * delta)
```

Two lines, and they are the whole drift model.

Grip is a **decay rate**, not a force. `v_lat *= exp(-k·dt)` is the exact
solution to `dv/dt = -k·v` over a step, which means it is frame-rate independent
by construction: 120 ticks of `dt = 1/120` decay exactly as much as 60 of
`dt = 1/60`. The naive `v_lat *= (1 - k·dt)` is the first-order approximation of
the same thing, and at `k = 9.5, dt = 1/120` it is close — but it goes *negative*
for `k·dt > 1`, so a low frame rate makes the car snap sideways.

The numbers say what each surface is:

| Surface | `k` | Half-life of a slide |
|---|---|---|
| Tarmac | 9.5 | 73 ms |
| Gravel | 6.0 | 116 ms |
| Grass | 3.0 | 231 ms |
| Handbrake on tarmac | 2.09 | 331 ms |

`ln 2 / k` is the half-life. Tarmac kills a slide in about a tenth of a second,
which reads as "grippy". The handbrake multiplier of 0.22 turns tarmac into
something slipperier than grass — that is the point of a handbrake turn.

### Heading: the bicycle model

```gdscript
    # Positive steer means right, and turning right rotates `forward` towards
    # right() — which under this yaw convention is a *decreasing* yaw. The
    # negation is what makes A steer left and D steer right.
    yaw_rate = 0.0
    if absf(v_long) > 0.03:
        yaw_rate = -v_long * tan(steer_angle) / tuning.wheelbase
        yaw_rate = clampf(yaw_rate, -tuning.max_yaw_rate, tuning.max_yaw_rate)
    yaw = wrapf(yaw + yaw_rate * delta, -PI, PI)
```

The **bicycle model** collapses a four-wheeled car to two wheels on a centre
line. A car with wheelbase `L` and steering angle `δ` traces a circle of radius
`R = L / tan δ`, and travelling that circle at speed `v` means turning at
`ω = v / R = v · tan δ / L`. That is the formula, and it has three properties
that make it right for an arcade racer:

- **Yaw rate is proportional to speed.** Stationary means no rotation, however
  hard you turn the wheel. That is what a car does, and it is what tank controls
  do not.
- **`tan` grows faster than linearly**, so the last few degrees of lock tighten
  the circle disproportionately — which is what makes a hairpin feel different
  from a sweeper.
- **It is one line**, with one tuning parameter that has a physical meaning.

`> 0.03` gates rotation on moving at all. Without it, `tan(δ)/L` with `v_long`
at float noise still produces a tiny yaw rate, and a parked car slowly rotates.

The `max_yaw_rate` clamp of 3.4 rad/s (195°/s) exists for the low-speed case.
Full lock is `tan(0.62) / 0.45 = 1.59` rad/s per unit of speed, so anything
above about 2.1 u/s at full lock already saturates the clamp — and low speed is
exactly where full lock is available, where a shove from another car adds free
rotation, and where the reverse-gear branch can be pushing the other way. The
clamp costs nothing at racing pace: 6.6 u/s with 0.24 rad of lock gives 3.59
rad/s, so it is barely biting at the very limit, which is where you want a
limit to bite.

**Sign check.** Chapter 05 established that Godot's convention differs from the
C engine's by exactly π. Adding π to a yaw does not change `tan`, does not
change the derivative relationship, and flips *both* `forward` and `right`
together — so this line is character-for-character the C engine's, negation
included. If you find yourself changing a sign here during a port, the mistake
is somewhere else.

### Recombination

```gdscript
    # Recombine in the NEW heading so the car rotates about itself rather than
    # carrying the old frame's velocity direction.
    fwd = forward()
    rgt = right()
    plane_velocity = fwd * v_long + rgt * v_lat
```

Note `forward()` is called again. The car's heading changed two lines ago;
rebuilding the world-space velocity from the *old* frame would mean the car
rotates but its velocity does not, which reads as the car crabbing sideways
through every corner. It is a one-line bug that looks like a grip problem.

### Integration, and the height

```gdscript
    plane_position += plane_velocity * delta
```

...except it does not, because chapter 07 hands the motion to `move_and_slide`
so the sweep happens against the barriers:

```gdscript
func _physics_process(delta: float) -> void:
    integrate(_input, _surface, delta)

    # Y is not part of the sweep: the car's height comes from the spline, not
    # from gravity, and giving move_and_slide a vertical component would make
    # it try to resolve the road as a floor.
    velocity = Vector3(plane_velocity.x, 0.0, plane_velocity.y)
    move_and_slide()
    plane_position = Vector2(global_position.x, global_position.z)

    for i: int in get_slide_collision_count():
        _apply_contact(get_slide_collision(i).get_normal(), 0.15, 0.22)

    _settle_to_surface(_surface, delta)
    _apply_transform()
```

### Settling on the surface

```gdscript
## Fastest the car may be moved vertically to meet the road, u/s. Has to clear
## the steepest climb taken flat out — top speed against an 18% gradient is
## about 1.2 — with enough margin left to swallow a kerb.
const HEIGHT_FOLLOW_RATE: float = 6.0

## Presentation only: the car leans into the gradient and rides the surface
## height. The simulation itself stays flat on the XZ plane.
func _settle_to_surface(surface: CarSurface, delta: float) -> void:
    var pitch_target: float = atan(surface.grade)
    pitch += (pitch_target - pitch) * (1.0 - exp(-12.0 * delta))

    # Height is rate-limited rather than eased. An exponential ease lags by
    # (climb rate / its rate) for as long as the gradient lasts, which is a
    # constant offset, not a transient: it buried the car 4 cm into the tarmac
    # for the whole of an 18% climb and floated it on the way back down. A rate
    # limit tracks any slope below its own ceiling exactly, and still smooths
    # the step from a kerb or a respawn.
    surface_height = move_toward(surface_height, surface.height,
                                 HEIGHT_FOLLOW_RATE * delta)
```

This is the clearest example in the project of the rate-limit-versus-ease
distinction, and it was found the hard way. An exponential filter tracking a
*ramp* input has a steady-state error proportional to the ramp's slope. On a
constant climb, that error never decays — it is not a transient you wait out. A
rate limiter has zero steady-state error for any ramp below its rate.

### Applying the transform

```gdscript
func _apply_transform() -> void:
    global_position = Vector3(plane_position.x, surface_height, plane_position.y)
    # Yaw first, then pitch about the car's own lateral axis. Assigning
    # `rotation` instead would use Godot's YXZ euler order, which pitches about
    # the world X axis and makes a car yawed 90 degrees lean sideways up a hill.
    global_transform.basis = Basis(Vector3.UP, yaw) * Basis(Vector3.RIGHT, pitch)
```

This is the fourth Godot-specific point, and it is a real trap. `rotation.x` and
`rotation.y` look like independent knobs and are not: they are euler angles in a
fixed order, and the order Godot uses (YXZ) is *not* the order that means "yaw
in the world, then pitch in the car's frame" for every yaw. Building the basis
by explicit composition says exactly what you mean.

For an interpolated visual (chapter 01), set this on the interpolated child, not
on the body.

### Derived values

```gdscript
    forward_speed = v_long
    lateral_speed = v_lat
    speed = plane_velocity.length()
    # How sideways the car is going, normalised so it reads 0..1 and does not
    # spike at low speed. The +0.9 floor is what stops a stationary car nudged
    # sideways in the pits from reporting a full-lock drift.
    slip = clampf(absf(v_lat) / (absf(v_long) * 0.45 + 0.9), 0.0, 1.0)
```

`slip` is the most-consumed value in the project and it is not used by the
physics at all: the tyre-scrub audio layer reads it, the skid trails spawn from
it, the HUD's drift indicator shows it, and the AI never looks at it. It exists
purely to let presentation ask "how dramatic is this".

---

## Holding on the grid, and the countdown bug

```gdscript
## Plants the car where it stands, gradient and all, and only settles how it
## sits on the surface.
func hold(surface: CarSurface, delta: float) -> void:
    plane_velocity = Vector2.ZERO
    steer_angle = 0.0
    speed = 0.0
    forward_speed = 0.0
    lateral_speed = 0.0
    yaw_rate = 0.0
    slip = 0.0
    slope_accel = 0.0
    _settle_to_surface(surface, delta)
```

The C series records the bug this function exists to fix, and it is worth
repeating because the instinct that produces it is so reasonable.

The obvious way to hold a grid of cars through a countdown is to feed them
`brake = 1.0`. And the cars drive slowly backwards off the line, because — see
the longitudinal section — **at a standstill the brake doubles as reverse.**

The second-obvious fix is to feed them no input at all. That is what chapter 10
does, and it needs one more thing:

```gdscript
# scripts/game/race_director.gd, during the countdown
racer.input.clear()
# The AI sees a stationary car it cannot move and starts counting towards an
# unstick manoeuvre. Clear it so nobody launches off the line already braking
# and steering backwards.
racer.ai.recover_timer = 0.0
```

Two bugs, one root cause: a system tuned for "the car is racing" being run in a
state where it is not. Worth remembering when adding a pit stop, a formation
lap, or a pause.

---

## The gravel model, as a worked design problem

Three numbers describe a gravel trap and none of them was obvious:

```gdscript
sand_speed_scale = 0.30      # ceiling on top speed
sand_drag = 3.5              # extra linear drag, 1/s
grip_sand = 6.0              # lateral decay, between tarmac and grass
```

The **design requirement** was: a gravel trap should end your lap, not offer a
slower line through the corner. That rules out "just make it a bit slower".

**Why a speed ceiling alone is not enough.** `speed_scale = 0.30` caps top speed
at 2.0 u/s, but the engine still accelerates at 7.2 u/s² up to that cap. A car
that clips a trap barely notices — it is at 2.0 u/s within half a second and out
the other side.

**Why drag is the part that bites.** Extra linear drag of 3.5/s means the
terminal speed at full throttle solves `7.2·headroom = (0.30 + 3.5)·v`, which
settles at about 0.95 u/s — a seventh of racing pace. More importantly, drag
scales with speed, so *arriving* at 6.6 u/s means decelerating hard immediately.
The trap takes the speed off you rather than merely capping it.

**Why grip is 6.0 and not 3.0.** Grass is 3.0 because a car sliding onto grass
should keep sliding — that is the punishment. Gravel is *deep*: it stops a spin
rather than letting it run. But it is higher than tarmac's 9.5 would suggest
being useful, because at 0.95 u/s there is no lateral velocity to speak of, so
the grip number barely matters for driving — it matters for the *arrival*, where
it decides whether a car that spun into the trap keeps spinning.

**And then the escape.** At 0.95 u/s a car can technically drive out, over
several seconds, which is a miserable thirty seconds of gameplay. Hence the
six-second marshals' rescue in chapter 10. The physics makes the trap a
punishment; the rescue makes it a *bounded* punishment.

That chain — requirement, first attempt, why it failed, second parameter, why
that one number — is what tuning actually looks like. Nothing about it can be
derived, and all of it can be written down afterwards.

---

## Exercises

1. **Break the recombination.** Move the `fwd = forward()` / `rgt = right()`
   lines to before the yaw update. Drive a lap and describe what the car does in
   a fast corner. Then explain it in terms of which frame the velocity is
   expressed in.

2. **First order versus exact.** Replace `exp(-k * delta)` with
   `(1.0 - k * delta)` and run at 120 Hz — nearly identical. Now set
   `Engine.physics_ticks_per_second` to 8 and try again. At what tick rate does
   the approximation go negative, and what does the car do on that tick?

3. **Find the steady-state error.** Replace the `move_toward` in
   `_settle_to_surface` with an exponential ease at rate 6.0. Drive up the
   steepest climb on `circuit02` and print `surface.height - surface_height`.
   Confirm it is a constant, not a decaying transient, and predict its size from
   the climb rate.

4. **A third surface.** Add ice: `grip 1.2, speed_scale 1.0, drag 0.0`. You
   should need to touch `car_surface.gd` (one nothing), `car_tuning.gd` (three
   fields) and `race_director.gd` (one branch) — and nothing in this file.
   Confirm that.

5. **Tune the trap.** Set `sand_drag` to 0.0 and drive through a gravel trap at
   speed. Time how long it costs you. Then set `sand_speed_scale` to 1.0 and
   restore the drag. Which of the two numbers is doing the work, and does that
   match what the design requirement asked for?

6. **The pirouette.** Set `max_yaw_rate` to 100.0, drive at 0.4 u/s and apply
   full lock plus handbrake. Describe what happens and why the clamp is
   specified in rad/s rather than as a speed threshold.

---

Next: [09 — The AI driver](09-ai-driver.md)
