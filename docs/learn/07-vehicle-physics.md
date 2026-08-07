# 07 — Vehicle physics

> `game/include/game/car.h` · `game/src/car.c` — 243 lines.
> Tests: the surface runs in `tests/test_race.c`.

---

## The problem

A car should feel like a car. It should accelerate, brake, turn where you point
it, slide when you ask too much of the tyres, and lose speed uphill. It must do
all of that in 240 lines that run 720 times a second (6 cars × 120 Hz) on a
Raspberry Pi.

A real vehicle simulation — Pacejka tyre curves, load transfer, a differential,
a gearbox, suspension — is thousands of lines and needs a hundred tuning
parameters you cannot reason about. It is also, for a top-down arcade racer,
*wrong*: it produces a car that spins under provocation and demands smooth
inputs, when what you want is a car that slides predictably and rewards being
thrown at a corner.

The answer is an **arcade model**: physically motivated but not physically
derived. Every term exists because it produces a behaviour, and the behaviours
were chosen first.

---

## The core idea

The header states it in five lines:

```c
// game/include/game/car.h
// Arcade vehicle physics for a top-down racer.
//
// Velocity is split into the car's forward and lateral components each tick:
// the engine and brakes act on the forward part, tyre grip bleeds off the
// lateral part, and heading comes from a bicycle steering model. Letting the
// lateral component survive when grip is low is what produces drift.
```

Read that again, because the whole file is an elaboration of it.

Velocity is a world-space 2D vector. Once per tick it is decomposed into the
car's own frame:

```
v_long = v · forward     how fast the car is going where it points
v_lat  = v · right       how fast it is sliding sideways
```

Then:

- **Longitudinal** is driven by engine, brakes, drag and gravity.
- **Lateral** decays exponentially at a rate set by tyre grip.
- **Heading** changes according to a bicycle model.
- The two components are recombined **in the new heading**.

That last step is the trick. A car with high grip has `v_lat ≈ 0`, so recombining
in the new heading means the velocity vector rotates with the car — it goes
exactly where it points. A car with low grip keeps its `v_lat`, so it continues
to slide in roughly the old direction while the body rotates. That is a drift,
and it emerges from one number.

---

## The state and the tuning

```c
typedef struct Car {
    Vector2 position;           // world (x, z)
    Vector2 velocity;           // world (x, z)
    float yaw;                  // radians; 0 faces +Z
    float steerAngle;

    float height;               // follows the track surface
    float pitch;                // radians, nose-up positive; visual only

    // Derived each tick, useful for audio, effects and the HUD.
    float speed;
    float forwardSpeed;
    float lateralSpeed;
    float slip;                 // 0..1, how much the tyres are sliding
    float yawRate;
    float slopeAccel;           // gravity's contribution this tick, for the HUD
    bool onTrack;
    bool inSand;                // bogged down in a run-off trap
} Car;
```

Four fields of true state (`position`, `velocity`, `yaw`, `steerAngle`), two of
presentation state (`height`, `pitch`), and seven derived values recomputed
every tick.

Publishing the derived values is a small design decision with a large payoff.
`slip` is computed in `CarUpdate` and then read by the HUD, the audio engine and
the skid-mark system — none of which has to know how it was derived, and none of
which recomputes it. If it were private, each consumer would need its own
approximation and they would drift apart.

The tuning is a separate struct, so it can be swapped per car:

```c
CarTuning CarDefaultTuning(void)
{
    // Tuned for the Kenney kit's scale: one unit is one road tile, the drivable
    // lane is 0.69 wide, and a car is roughly 0.30 x 0.60.
    CarTuning t = {
        .enginePower = 7.2f,
        .brakePower = 13.0f,
        .topSpeed = 6.6f,
        .reverseSpeed = 2.0f,
        .dragLinear = 0.30f,
        .dragQuadratic = 0.030f,

        .wheelbase = 0.45f,
        .maxSteerLowSpeed = 0.62f,
        .maxSteerTopSpeed = 0.24f,
        .steerRate = 7.0f,
        .maxYawRate = 3.4f,

        .gripTarmac = 9.5f,
        .gripGrass = 3.0f,
        .gripSand = 6.0f,
        .handbrakeGrip = 0.22f,

        .gravity = 11.0f,

        .halfWidth = 0.148f,
        .halfLength = 0.298f,
        .offTrackSpeedScale = 0.58f,
        .sandSpeedScale = 0.30f,
        .sandDrag = 3.5f,
    };
    return t;
}
```

Twenty-two numbers, all in kit-tile units. Brakes (13.0) are nearly twice as
strong as the engine (7.2), which is true of real cars and matters for how a lap
feels.

---

## Surfaces as a parameter

```c
// What the car is standing on this tick. Driving stays a 2D problem on the XZ
// plane; height and grade ride along so gradients affect speed and the car can
// be drawn sitting on the road.
typedef struct CarSurface {
    float grip;                 // lateral velocity decay rate, 1/s
    float speedScale;           // multiplier on top speed
    float drag;                 // extra linear drag from the surface itself, 1/s
    float grade;                // rise over run along the direction of travel
    float height;               // surface height under the car
} CarSurface;
```

`CarUpdate` does not know what tarmac is. It is handed five numbers describing
whatever the car is standing on. `race.c` decides which:

```c
// game/src/race.c
CarSurface surface = {
    .grip = onTrack ? race->tuning.gripTarmac
                    : (inSand ? race->tuning.gripSand : race->tuning.gripGrass),
    .speedScale = onTrack ? 1.0f
                          : (inSand ? race->tuning.sandSpeedScale
                                    : race->tuning.offTrackSpeedScale),
    .drag = inSand ? race->tuning.sandDrag : 0.0f,
    .grade = (alignment < 0.0f) ? -q.grade : q.grade,
    .height = q.position.y,
};
```

Adding ice would be one more branch here and zero changes to `car.c`. And,
critically, it makes the physics **testable in isolation**:

```c
// tests/test_race.c
static void RunSurfaceTests(void)
{
    CarTuning tuning = CarDefaultTuning();
    CarSurface tarmac = { .grip = tuning.gripTarmac, .speedScale = 1.0f };
    CarSurface grass  = { .grip = tuning.gripGrass, .speedScale = tuning.offTrackSpeedScale };
    CarSurface sand   = { .grip = tuning.gripSand, .speedScale = tuning.sandSpeedScale,
                          .drag = tuning.sandDrag };
    /* ... drive a car across each for one second ... */
}
```

No level, no track, no spline. Just the tyre model against the ground. The real
output from `make test`:

```
a second at full throttle from 6.60 u/s: tarmac 5.04 u/s (5.60 units),
                                         grass 3.23 (3.42), gravel 0.96 (1.10)
```

Those three numbers *are* the surface design, made measurable.

---

## One tick, in order

### Clamp the inputs

```c
input.throttle = Clamp(input.throttle, 0.0f, 1.0f);
input.brake = Clamp(input.brake, 0.0f, 1.0f);
input.steer = Clamp(input.steer, -1.0f, 1.0f);
```

Inputs arrive from a keyboard, an analogue stick, or the AI. The AI computes
`Clamp(error * 1.6f, ...)` and could in principle produce anything. Clamping at
the boundary means nothing downstream needs to worry.

### Decompose the velocity

```c
Vector2 forward = CarForward(car);
Vector2 right = CarRight(car);

float vLong = car->velocity.x * forward.x + car->velocity.y * forward.y;
float vLat = car->velocity.x * right.x + car->velocity.y * right.y;
```

```c
Vector2 CarForward(const Car *car)
{
    return (Vector2){ sinf(car->yaw), cosf(car->yaw) };
}

// Right-hand side of the car: forward x up, which is the same axis a viewer
// looking along `forward` with +Y up sees as screen right. Yaw grows the other
// way (+90 degrees takes +Z towards +X, i.e. towards the car's left), so this
// is not simply forward rotated by +90.
Vector2 CarRight(const Car *car)
{
    return (Vector2){ -cosf(car->yaw), sinf(car->yaw) };
}
```

`forward = (sin yaw, cos yaw)` means **yaw 0 faces +Z**, not +X. This is the
convention the whole codebase uses (see `atan2f(x, z)` everywhere in Chapter 04)
and it exists because the engine is Y-up with Z as the "forward" ground axis.

The comment on `CarRight` deserves attention, because it documents a genuine
trap: **you cannot get the car's right by adding 90° to its yaw.** Substitute and
see — `forward(yaw + 90°) = (cos yaw, −sin yaw)`, which is exactly `−CarRight`.
Adding a quarter turn of yaw points you *left*. `CarRight` is instead the vector
`(−cos yaw, sin yaw)`, which happens to be `forward` turned a quarter of the way
round in the opposite sense to yaw.

Get this wrong and steering inverts — which, per `git log`, actually happened
once (`e504167 Steer right when the input says right`).

---

### Steering: a speed-sensitive lock

```c
// Lock tightens with speed so the car stays controllable flat out.
float speedFraction = Clamp(fabsf(vLong) / tuning->topSpeed, 0.0f, 1.0f);
float maxSteer = Lerp(tuning->maxSteerLowSpeed, tuning->maxSteerTopSpeed, speedFraction);
float target = input.steer * maxSteer;
float steerStep = tuning->steerRate * dt;
if (fabsf(target - car->steerAngle) <= steerStep) car->steerAngle = target;
else car->steerAngle += (target > car->steerAngle) ? steerStep : -steerStep;
```

**Speed-sensitive lock.** At a standstill the wheels can turn 0.62 rad (36°); at
top speed only 0.24 rad (14°). This is not how a real steering rack works — a
real one has a fixed ratio. It is a game-design device, and a near-universal one:
without it, full lock at top speed spins the car instantly and the vehicle is
undriveable at speed.

**Rate limiting rather than snapping.** The wheels take `0.62/7.0 ≈ 0.09 s` to
reach full lock. With a keyboard, `input.steer` is a step function from 0 to 1;
without the rate limit the car would snap into a turn. The limiter turns a
digital input into something that feels analogue.

Note the *rate limit* (a fixed step per tick) rather than an *exponential ease*.
The distinction returns below, and it matters more than it looks.

---

### Longitudinal forces

```c
float accel = 0.0f;
float topSpeed = tuning->topSpeed * surface->speedScale;

if (input.throttle > 0.0f) {
    // Taper power near the limit instead of clamping, which would feel abrupt.
    float headroom = 1.0f - Clamp(vLong / fmaxf(topSpeed, 0.001f), 0.0f, 1.0f);
    accel += tuning->enginePower * input.throttle * headroom;
}
```

**The headroom taper** replaces a hard speed cap. At `vLong = 0` headroom is 1
and the engine gives full 7.2; at `vLong = topSpeed` headroom is 0 and it gives
nothing. In between the car eases up to its limit rather than accelerating hard
and then hitting a wall.

Physically this stands in for aerodynamic drag and the falling torque curve of a
real engine at high revs. Mathematically it is a first-order lag whose steady
state is exactly `topSpeed`.

```c
if (input.brake > 0.0f) {
    if (vLong > 0.05f) {
        accel -= tuning->brakePower * input.brake;
    } else {
        // Standing still or already rolling back: brake doubles as reverse.
        float headroom = 1.0f - Clamp(-vLong / tuning->reverseSpeed, 0.0f, 1.0f);
        accel -= tuning->enginePower * 0.55f * input.brake * headroom;
    }
}
```

**One key does brake and reverse.** Above 0.05 u/s forward it brakes; at or
below, it becomes reverse at 55% engine power with its own headroom taper against
`reverseSpeed`.

This one-button convention is standard in arcade racers, and it has a
consequence that bites twice elsewhere in this codebase — see "the countdown
bug" below.

```c
accel -= (tuning->dragLinear + surface->drag) * vLong;
accel -= tuning->dragQuadratic * vLong * fabsf(vLong);
```

**Two drag terms.**

- *Linear* (`−k·v`) dominates at low speed. Physically it is rolling resistance
  and driveline loss. It is also what makes `sandDrag = 3.5` so brutal: in
  gravel the total linear drag is `0.30 + 3.5 = 3.8`, more than twelve times the
  tarmac value.
- *Quadratic* (`−k·v·|v|`) dominates at high speed. Physically it is air
  resistance. `|v|` rather than `v` so the term opposes motion in both directions.

At top speed the quadratic term contributes `0.030 × 6.6² ≈ 1.31` and the linear
`0.30 × 6.6 ≈ 1.98`, so both matter.

You can solve for the true terminal velocity: set `accel = 0` with full throttle,
and the headroom taper means the engine gives nothing at `topSpeed`, so drag
wins and the car settles below it. The test measures exactly this:

```
a second at full throttle from 6.60 u/s: tarmac 5.04 u/s
```

and asserts the interpretation:

```c
// Top speed is a ceiling the drag never quite lets the engine hold, so the
// tarmac case settles below it — but nowhere near the other two.
CHECK(onTarmac > tuning.topSpeed * 0.7f, "the tarmac case is broken: %.2f u/s", (double)onTarmac);
```

---

### Gravity along the road

```c
// Gravity along the road. sin(atan(grade)) resolves the slope into the
// direction of travel, so a climb costs speed and a descent gives it back.
float slope = surface->grade;
car->slopeAccel = -tuning->gravity * (slope / sqrtf(1.0f + slope * slope));
accel += car->slopeAccel;
```

The maths. `grade` is rise over run, so the slope angle is `θ = atan(grade)`.
The component of gravity along a slope of angle θ is `g·sin(θ)`. And there is a
neat identity:

```
sin(atan(x)) = x / √(1 + x²)
```

Draw the right triangle: opposite `x`, adjacent `1`, hypotenuse `√(1+x²)`. So
`sin = x/√(1+x²)`, with no trig call at all. Two multiplies, an add, a `sqrt`
and a divide, instead of `atanf` then `sinf`.

The sign: positive grade means climbing, and a climb should slow you, hence the
leading minus.

`car->slopeAccel` is stored because the HUD shows the gradient, and because the
next line uses it:

```c
vLong += accel * dt;
// A steep enough descent should be able to push past the flat-road limit,
// which is where the speed on a downhill run comes from.
float downhillAllowance = (car->slopeAccel > 0.0f) ? 1.35f : 1.0f;
vLong = Clamp(vLong, -tuning->reverseSpeed, topSpeed * downhillAllowance);
```

Without the allowance, a car would hit the same ceiling downhill as on the flat,
and a plunge would feel identical to a straight. 35% extra is enough to feel and
not enough to make descents dominate a lap.

### The exaggerated gravity

```c
// About 1.5x the engine's own acceleration. True gravity at this scale
// would be nearer 2.4x, which made the 18% climbs on the demo circuit a
// crawl; this keeps hills clearly felt but still driveable.
.gravity = 11.0f,
```

This is where the model is openly unphysical, and the comment says why.

At this scale, a genuine `9.81 m/s²` — with one unit being a road tile of a few
metres — works out to roughly 2.4× the engine's acceleration. Circuit01 has 18%
climbs. `11.0 × 0.18/√(1+0.18²) ≈ 1.95 u/s²` against an engine that gives 7.2 at
zero speed but much less near the top; at true gravity the climb would simply
beat the engine and cars would grind to walking pace.

The fix is not to make the engine stronger (that would ruin the flat sections)
or the hills shallower (that would ruin the layout). It is to weaken gravity
until the *feel* is right. **This is what "arcade" means as an engineering
decision: when physical fidelity and the experience conflict, the experience
wins, and you write down why.**

### The creep killer

```c
// Kill the last sliver of creep so a stopped car actually stops.
if (input.throttle <= 0.0f && input.brake <= 0.0f &&
    fabsf(vLong) < 0.02f && fabsf(car->slopeAccel) < 0.05f) {
    vLong = 0.0f;   // only settle to a stop on the flat
}
```

Exponential drag never reaches zero. A car left alone approaches 0 asymptotically
and creeps forever at 0.001 u/s, which shows on the HUD, keeps the engine note
alive, and stops `stuckTime` from behaving sensibly.

The `fabsf(car->slopeAccel) < 0.05f` condition is the subtle part. A car parked
on a hill *should* roll away — that is gravity working. Only cars on genuinely
flat ground are snapped to rest.

---

### Lateral grip

```c
float lateralGrip = surface->grip;
if (input.handbrake) lateralGrip *= tuning->handbrakeGrip;
vLat *= expf(-lateralGrip * dt);
```

Three lines. This is the heart of the model.

**Exponential decay.** `v(t) = v₀·e^(−k·t)`. Solving `dv/dt = −k·v`. The half-life
is `ln(2)/k`:

| Surface | k | Half-life | What it feels like |
|---|---|---|---|
| Tarmac | 9.5 | 73 ms | grippy; the car goes where it points |
| Gravel | 6.0 | 116 ms | holds a slide, but there is no speed to slide with |
| Grass | 3.0 | 231 ms | loose; the car washes wide |
| Handbrake on tarmac | 9.5 × 0.22 = 2.09 | 332 ms | full drift |

**Why exponential rather than a friction force?** A Coulomb friction model
(`F = μ·N`, constant magnitude opposing sliding) is more physical, and it is
harder to tune and worse to drive: it removes lateral velocity at a constant
rate, so a small slide stops abruptly while a big one persists. Exponential decay
removes a constant *fraction*, so slides of all sizes decay with the same
character. Predictability beats fidelity for a control surface the player is
holding.

**Why `expf` rather than `1 - k*dt`?** Frame-rate independence — the same
argument as the camera smoothing in Chapter 01. With a fixed 120 Hz step you
could get away with the linear approximation, but `expf` is exact for any `dt`,
which matters because the tests call `CarUpdate` directly and could use any step.

**The handbrake as a multiplier** (0.22) rather than an absolute value means it
composes with the surface. Handbraking on grass gives `3.0 × 0.22 = 0.66`, an
enormous slide. That is emergent and correct: the handbrake reduces grip by a
proportion whatever you are standing on.

---

### Heading: the bicycle model

```c
// Positive steer means right, and turning right rotates `forward` towards
// CarRight() — which under this yaw convention is a *decreasing* yaw. The
// negation is what makes A steer left and D steer right.
float yawRate = 0.0f;
if (fabsf(vLong) > 0.03f) {
    yawRate = -vLong * tanf(car->steerAngle) / tuning->wheelbase;
    yawRate = Clamp(yawRate, -tuning->maxYawRate, tuning->maxYawRate);
}
car->yawRate = yawRate;
car->yaw += yawRate * dt;
```

**The kinematic bicycle model.** Collapse a four-wheeled car to two wheels on a
centreline, separated by the wheelbase `L`. If the front wheel is steered by
angle δ, the car traces a circle of radius:

```
R = L / tan(δ)
```

Derivation: the rear wheel rolls along its own axis, the front wheel along its
steered axis. The instantaneous centre of rotation is where the perpendiculars
to both meet. With the rear wheel's perpendicular being the rear axle line and
the front's being rotated by δ, trigonometry gives `R = L/tan(δ)` measured from
the rear axle.

Travelling at speed `v` around radius `R` gives angular velocity:

```
ω = v / R = v · tan(δ) / L
```

Which is the line of code, with the negation for the yaw convention.

This model is the workhorse of vehicle robotics — it is what most autonomous
driving path planners assume — because it captures the essential non-holonomic
constraint (a car cannot move sideways) with two parameters.

**`yawRate ∝ vLong`** has a consequence worth stating: **a stationary car cannot
turn.** Steering does nothing at rest, exactly as in a real car. The
`fabsf(vLong) > 0.03f` guard makes it explicit and avoids dividing tiny numbers.

**`maxYawRate = 3.4` rad/s** clamps low-speed pirouettes: at 1 u/s with full
lock, `tan(0.62)/0.45 ≈ 1.6`, so `ω = 1.6 rad/s` — fine. But the speed-sensitive
lock plus this clamp together keep the car from spinning on the spot.

The clamp also has a second life: the AI uses it to derive a **physical speed
limit** for a corner, and `tests/test_race.c` uses it to decide whether a circuit
is driveable at all:

```c
// tests/test_race.c
CHECK(tuning.maxYawRate * worstRadius > 1.8f,
      "tightest corner (radius %.2f) caps speed at %.2f u/s — undriveable", /* ... */);
```

Since `ω = v/R` and `ω ≤ maxYawRate`, the fastest you can go round radius `R` is
`v = maxYawRate × R`. Real output:

```
tightest radius 1.31 at arc 63.8 (caps speed at 4.44 u/s); 18% of the lap is corner-limited
```

One tuning constant, three uses. That is a good sign that it is a real parameter
of the model rather than a fudge factor.

---

### Recombination

```c
// Recombine in the new heading so the car rotates about itself rather than
// carrying the old frame's velocity direction.
forward = CarForward(car);
right = CarRight(car);
car->velocity.x = forward.x * vLong + right.x * vLat;
car->velocity.y = forward.y * vLong + right.y * vLat;

car->position.x += car->velocity.x * dt;
car->position.y += car->velocity.y * dt;
```

`CarForward` is called **again**, after `yaw` has been updated. That is the whole
drift mechanism.

- High grip: `vLat ≈ 0`, so `velocity ≈ forward × vLong`. The velocity vector
  rotates exactly with the body. The car goes where it points.
- Low grip: `vLat` survives. The body has rotated but a large lateral component
  remains, so the car continues broadly along its old path while facing
  elsewhere. That *is* a slide.

The comment names the alternative: recombining in the *old* frame would rotate
the body without rotating the velocity, so the car would slide sideways in a
straight line whatever you did with the wheel.

Note the integration order: velocity is updated, then position is integrated
with the *new* velocity. That is **semi-implicit (symplectic) Euler**, and it is
the standard choice for games — more stable than explicit Euler for oscillatory
systems, and one line of difference.

---

### Derived values

```c
car->forwardSpeed = vLong;
car->lateralSpeed = vLat;
car->speed = sqrtf(car->velocity.x * car->velocity.x + car->velocity.y * car->velocity.y);
car->slip = Clamp(fabsf(vLat) / (fabsf(vLong) * 0.45f + 0.9f), 0.0f, 1.0f);
```

`slip` is the interesting one. Conceptually it is "lateral over forward" — the
slip angle's tangent — but a naive ratio explodes at low speed (a car creeping
sideways at 0.1 u/s would read as fully sliding). The `+ 0.9f` in the denominator
softens that; the `0.45f` scales how much forward speed is needed before a given
lateral counts as a slide.

`slip` then drives three consumers, none of which does its own maths:

- Audio: `AudioEngineSetMotor(rpm, throttle, slip * (onTrack ? 1.0f : 0.6f))` —
  tyre scrub volume.
- Skid marks: `slide = (slip - SKID_SLIP_FLOOR) / (1 - SKID_SLIP_FLOOR)`.
- The debug HUD.

The skid threshold shows how such a constant is chosen honestly:

```c
// game/src/skid.c
// Below this the tyres are gripping, not sliding, and leave nothing behind.
// Measured against the AI, which peaks at 0.298 through the quickest corners
// and sits far below that everywhere else: this puts rubber down where the
// field is genuinely scrubbing for grip and nowhere else. A player leaning on
// the handbrake goes well past it.
#define SKID_SLIP_FLOOR 0.22f
```

Not guessed — measured, against a known reference behaviour, with the reasoning
recorded.

---

## Elevation, and a rate limit that had to replace an ease

The simulation is 2D. Height and pitch are presentation, updated separately:

```c
// Presentation only: the car leans into the gradient and rides the surface
// height. The simulation itself stays flat on the XZ plane.
static void SettleToSurface(Car *car, const CarSurface *surface, float dt)
{
    float pitchTarget = atanf(surface->grade);
    car->pitch += (pitchTarget - car->pitch) * (1.0f - expf(-12.0f * dt));

    // Height is rate-limited rather than eased. An exponential ease lags by
    // (climb rate / its rate) for as long as the gradient lasts, which is a
    // constant offset, not a transient: it buried the car 4cm into the tarmac
    // for the whole of an 18% climb and floated it on the way back down. A rate
    // limit tracks any slope below its own ceiling exactly, and still smooths
    // the step from a kerb or a respawn.
    float step = CAR_HEIGHT_FOLLOW_RATE * dt;
    float delta = surface->height - car->height;
    if (fabsf(delta) <= step) car->height = surface->height;
    else car->height += (delta > 0.0f) ? step : -step;
}
```

This comment is the best short explanation of exponential-ease-versus-rate-limit
you are likely to read, and the distinction generalises well beyond cars.

**Exponential ease** (`x += (target − x) × k`) has zero steady-state error when
the target is *constant*. But against a target moving at constant velocity `u`,
it settles to a **constant lag** of `u/rate`. It never catches up. Climbing at
18% at 6 u/s, the surface height rises at about 1.06 u/s; with a smoothing rate
of 12, that is a permanent 0.09-unit lag — the car sunk into the road for the
whole climb, and floating above it all the way down.

**Rate limiting** (`x += clamp(target − x, −step, +step)`) tracks any target
whose speed is below the rate **exactly**, with zero steady-state error, and
still smooths a discontinuity.

The rule: *use an exponential ease for a target that settles; use a rate limit
for a target that moves.*

```c
// Fastest the car may be moved vertically to meet the road, in units a second.
// Has to clear the steepest climb taken flat out — top speed against an 18%
// gradient is about 1.2 — with enough margin left to swallow a kerb.
#define CAR_HEIGHT_FOLLOW_RATE 6.0f
```

Six is five times the worst case, so the limit never binds in normal driving and
only smooths genuine steps.

Pitch keeps the exponential ease, correctly: pitch settles at a constant on a
constant gradient, so there is no moving target to lag behind.

### The grade must be signed by heading

```c
// game/src/race.c
// The grade is signed along the centre line, so a car facing back down
// the track has to see it reversed or a climb would push it along.
Vector2 forward = CarForward(&racer->car);
float alignment = forward.x * q.tangent.x + forward.y * q.tangent.z;
CarSurface surface = {
    /* ... */
    .grade = (alignment < 0.0f) ? -q.grade : q.grade,
};
```

The spline's grade is measured *down-track*. A car facing backwards — spun,
reversing out of gravel — is going the other way, so an uphill section is a
descent for it. Without the flip, a spun car on a climb would be accelerated by
gravity.

The `alignment` dot product is the standard "am I facing with or against this
direction" test.

---

## `CarHold`, and the countdown bug

```c
// Plants the car where it stands, gradient and all, and only settles how it
// sits on the surface. Holding a car with a full brake input does not work:
// at a standstill the brake doubles as reverse and the car drives off backwards.
void CarHold(Car *car, const CarSurface *surface, float dt)
{
    if (dt <= 0.0f) return;
    car->velocity = (Vector2){ 0.0f, 0.0f };
    car->steerAngle = 0.0f;
    /* ... zero every derived value ... */
    SettleToSurface(car, surface, dt);
}
```

Read the comment against the brake code from earlier. During the countdown the
natural implementation is "apply full brakes". But full brake at a standstill is
full reverse, so the entire grid reversed off the line during the three-second
countdown. `git log` records the fix: `4b75022 Hold the grid still during the
countdown instead of braking it`.

`race.c` closes the loop with a second, related fix:

```c
// game/src/race.c
if (locked) {
    // Held on the line. Note this is *no* input rather than a full
    // brake: at a standstill the brake doubles as reverse, so braking
    // through the countdown drove the whole grid backwards off the line.
    racer->input = (CarInput){ 0 };
    // The AI sees a stationary car it cannot move and starts counting
    // towards an unstick manoeuvre. Clear it so nobody launches off the
    // line already braking and steering backwards.
    racer->ai.recoverTimer = 0.0f;
}
```

Two systems, two bugs, one root cause: a shared control that means different
things in different states. Worth remembering when you design an input mapping.

---

## The gravel model as a worked design problem

The README states the goal precisely:

> Gravel is deliberately not a slower line through the corner. Top speed falls
> to 30% and a heavy drag holds full throttle to about 0.95 u/s, a seventh of
> the pace on tarmac; you crawl out sideways or the marshals lift you back to
> the line after six seconds.

Three parameters cooperate — grip of 6.0 (between tarmac's 9.5 and grass's 3.0),
a speed ceiling of `6.6 × 0.30 = 1.98` u/s, and an extra 3.5 of linear drag on
top of the tuning's own 0.30:

```c
// game/src/car.c, inside CarDefaultTuning()
        // Gravel holds a sliding car better than wet grass does — it is what
        // stops a spin rather than letting it run — but nothing about it lets
        // you steer, because there is barely any speed left to steer with.
        .gripSand = 6.0f,
        /* ... */
        // A gravel trap is meant to end your lap, not to be a slower line
        // through the corner. The ceiling takes the speed off as the car
        // ploughs in; the drag is what keeps it off, settling full throttle at
        // about 0.95 u/s — a seventh of the pace on tarmac, and slow enough
        // that the marshals' six-second rescue is the realistic way out.
        .sandSpeedScale = 0.30f,
        .sandDrag = 3.5f,
```

**Why grip is *higher* than grass** is counter-intuitive and the comment
explains:

> Gravel holds a sliding car better than wet grass does — it is what stops a
> spin rather than letting it run — but nothing about it lets you steer, because
> there is barely any speed left to steer with.

Grip only affects lateral decay. With `vLong` pinned near 0.95, the bicycle model
gives almost no yaw rate anyway. So high grip makes gravel *arrest* a slide
(realistic, and it stops cars pinballing) without making it drivable.

**Why both a speed cap and a drag.** The comment separates their jobs:

> The ceiling takes the speed off as the car ploughs in; the drag is what keeps
> it off, settling full throttle at about 0.95 u/s.

The ceiling is the transient — it kills entry speed immediately. The drag is the
steady state — it fights the engine's headroom term to an equilibrium well below
the ceiling. Solve `enginePower × headroom = 3.8 × v` with `topSpeed = 1.98` and
you land near 0.95.

**And it is all asserted:**

```c
// tests/test_race.c
CHECK(inGravel < 1.2f, "a car flat out in gravel still does %.2f u/s", (double)inGravel);
CHECK(inGravel < onGrass * 0.5f, "gravel (%.2f u/s) is barely slower than grass (%.2f u/s)", /* ... */);
CHECK(farGravel < 1.6f, "a car crossed %.2f units of gravel in a second", (double)farGravel);
// ...but it does not stop dead, or a trap would be a wall.
CHECK(inGravel > 0.3f, "gravel brought the car to a standstill (%.2f u/s)", (double)inGravel);
```

Note the last one. Bounded on *both* sides. A trap that stopped a car dead would
be a wall, which the design explicitly rejects. Design intent, in an assertion.

---

## Exercises

1. **Feel the grip.** Set `gripTarmac` to 2.0 and drive. Then 30.0. Describe
   each. Then run `make test` on both and read the lap times — which numbers
   move?

2. **Prove the drift mechanism.** In `CarUpdate`, move the recombination *before*
   the yaw update (use the old `forward`/`right`). Drive. Why does the car now
   slide sideways in a straight line?

3. **Ice.** Add `gripIce`, `iceSpeedScale` and `iceDrag` to `CarTuning`, and a
   `bool inIce` path in `race.c`'s surface selection triggered by a new
   `"icepatches"` array in the level format. Then add a surface test asserting
   that ice is fast but uncontrollable. Which existing test would catch you
   putting ice on the racing line?

4. **Real gravity.** Set `gravity = 17.3f` (roughly the 2.4× the comment cites).
   Run `make test` and read the lap times and leader average speed for both
   circuits. Which assertion fails first?

5. **Reproduce the ease bug.** Replace the rate limit in `SettleToSurface` with
   `car->height += delta * (1.0f - expf(-6.0f * dt));`. Drive up the Raidillon
   climb on circuit01 and watch the car against the road. Then compute the
   predicted lag from `climbRate / rate` and check it against what you see.

6. **The bicycle model's limit.** For each of the tuning's steering values,
   compute the turning radius `R = wheelbase/tan(δ)` at low-speed lock and at
   top-speed lock. Compare with the circuit's tightest radius of 1.31. What does
   that tell you about whether the hairpin can be taken at speed?

7. **Add load transfer.** Under braking, weight moves to the front wheels, so
   grip should rise slightly under braking and fall under acceleration. Add a
   term to `lateralGrip` proportional to `input.brake - input.throttle`. Does the
   car feel better? Do lap times improve? Is it worth the parameter?

---

Next: [08 — The AI driver](08-ai-driver.md)
