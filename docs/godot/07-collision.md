# 07 — Collision

> `scripts/engine/obb2.gd` · `scripts/game/car_body.gd` ·
> `scripts/engine/collision_builder.gd`. Mirrors
> [C chapter 06](../learn/06-collision.md), most of which Godot has already
> written — and one part of which it has not.

---

## The problem

Three separate questions get called "collision" and they have three different
answers:

1. **A car must not drive through a barrier.** Hundreds of static boxes per
   circuit, one moving box per car, needs to be robust at 6.6 units per second.
2. **Cars must bump each other.** Six moving boxes, and the *feel* of the bump
   is a game-design decision — an arcade racer wants a shove, not a physically
   correct collision response.
3. **Is the car on tarmac, on grass, or in a gravel trap?** Which is not
   collision at all. It is a spline query and a point-in-box test, and chapter 06
   already answered it.

The C series writes all of this by hand: 2D oriented-box SAT, minimum
translation vectors, a uniform grid broadphase in compressed-sparse-row form,
284 lines. Godot has a complete 3D physics engine, so the question is how much
of that to keep.

The answer is: **question 1 goes to the physics server, question 2 stays hand
written, and question 3 never touches physics at all.** The rest of this chapter
is why.

---

## Why not just use rigid bodies

The obvious Godot answer is `RigidBody3D` cars with box colliders, or
`VehicleBody3D` which is purpose-built. Both are wrong here, and it is worth
knowing why before dismissing them.

**`VehicleBody3D`** is a raycast vehicle: four `VehicleWheel3D`s with spring
suspension, tyre friction and engine torque. It is a reasonable simulator-style
car, and it is a poor arcade car. The handling model this game wants — chapter
08's "lateral velocity decays exponentially, and when grip is low it survives,
which is drift" — is not expressible as wheel friction coefficients. You would
spend a week fighting the suspension to get behaviour you can write in forty
lines.

**`RigidBody3D`** with a box gives you a car that tips over, that spins after a
kerb, that responds to impulses in a physically plausible way — and none of that
is what a top-down racer wants. It also takes control of the transform: you no
longer own the car's yaw, so "the car points where it is steering" becomes a
torque you tune rather than a value you set.

The general principle, which applies well beyond Godot:

> A physics engine is for when you want emergent behaviour you did not
> specify. An arcade vehicle is behaviour you *did* specify, in detail. Using a
> solver to approximate a model you can write directly is backwards.

What the physics engine is genuinely good at here is the boring, hard part:
sweeping a moving shape against hundreds of static ones without tunnelling,
with a spatial index you do not maintain. That is what `CharacterBody3D` gives
you.

---

## The car as a `CharacterBody3D`

```gdscript
# scripts/game/car_body.gd
class_name CarBody
extends CharacterBody3D

## The car owns its own velocity and heading; the physics server is used only
## to sweep that motion against the world and report what it hit. Everything
## about *how* the car moves lives in chapter 08.

func _ready() -> void:
    # The car's height comes from the spline, not from gravity, and it never
    # needs floor/wall/ceiling classification. FLOATING mode skips all of it
    # and stops move_and_slide() from reinterpreting a barrier as a steep
    # floor the car should slide down.
    motion_mode = CharacterBody3D.MOTION_MODE_FLOATING

    # Default 0.001 is written for a metres-and-people scale. A car here is
    # 0.30 units wide, so the default margin is a third of a percent of the
    # car — workable, but tightening it visibly reduces the gap a car leaves
    # when it parks against a barrier.
    safe_margin = 0.0005

    # Barriers only. See the layer table below.
    collision_layer = LAYER_CARS
    collision_mask = LAYER_WALLS
```

The tick then reads, in outline:

```gdscript
func _physics_process(delta: float) -> void:
    # 1. chapter 08: work out plane_velocity and yaw from input and surface
    _integrate(delta)

    # 2. hand the motion to the server. Y is driven separately, from the
    #    spline's surface height, so the swept motion stays horizontal.
    velocity = Vector3(plane_velocity.x, 0.0, plane_velocity.y)
    move_and_slide()

    # 3. read back what happened
    plane_position = Vector2(global_position.x, global_position.z)
    for i: int in get_slide_collision_count():
        var hit: KinematicCollision3D = get_slide_collision(i)
        _apply_contact(hit.get_normal(), 0.15, 0.22)

    # 4. sit on the road
    global_position.y = surface_height
```

Four steps, and each one maps onto something in `race.c`'s per-car block. The
difference is step 2: `CollisionResolveStatic` — 40 lines of iterative
depenetration in C — becomes one call.

### What `move_and_slide` does that the C version does not

`move_and_slide` **sweeps**: it moves the shape along the motion vector,
stopping at the first contact, then slides the remainder along the surface and
repeats (up to `max_slides`, default 6). The C version *teleports* the car by
`velocity * dt` and then pushes it out of anything it ended up inside.

At 120 Hz and 6.6 u/s the difference rarely shows — 5.5 cm of motion against a
0.30-unit car cannot tunnel through a barrier. At 60 Hz against a thin kerb it
absolutely can, and the sweep is why you can lower the tick rate in Godot
without the wall handling falling apart. That is a real robustness win, and it
is free.

### Reading the contact

The C engine's `CarApplyContact` survives unchanged, because it is a game-feel
decision rather than a physics one:

```gdscript
## Cancels the velocity component heading into a surface and scrubs a little
## speed, so wall contact costs time without stopping the car dead.
func _apply_contact(normal_3d: Vector3, restitution: float, scrub: float) -> void:
    var normal := Vector2(normal_3d.x, normal_3d.z)
    if normal.length_squared() < 1e-12:
        return
    normal = normal.normalized()

    var into: float = plane_velocity.dot(normal)
    if into >= 0.0:
        return          # already moving away from the surface

    # Remove the approaching component (plus a little bounce), then scrub the
    # sliding component so scraping a wall costs momentum.
    plane_velocity -= normal * (into * (1.0 + restitution))
    plane_velocity *= (1.0 - scrub)
```

The `into >= 0.0` early-out is not an optimisation. `move_and_slide` can report
a contact on a frame where the car is already leaving the wall, and applying the
response again would add energy. This is the single most common way a
hand-written contact response turns into a car that vibrates against a barrier.

`restitution = 0.15` and `scrub = 0.22` are the C engine's values and they
encode a design decision: hitting a wall should cost you about a fifth of your
speed and almost no bounce. A restitution of 0.6 makes the barriers feel like
bumpers; 0.0 makes them feel like glue.

---

## Building the static world

300 colliders is 300 shapes. It is emphatically **not** 300 nodes:

```gdscript
# scripts/engine/collision_builder.gd
class_name CollisionBuilder

## Builds one StaticBody3D holding every barrier in the level as a child
## CollisionShape3D.
##
## One body, many shapes — not one body per box. A body is a physics-server
## object with a transform, an island, and per-frame bookkeeping; a shape
## inside it is an entry in that body's list. For 300 barriers that is the
## difference between 300 server objects and one.
static func build_walls(level: LevelData, parent: Node3D) -> StaticBody3D:
    var body := StaticBody3D.new()
    body.name = "Walls"
    body.collision_layer = Layers.WALLS
    body.collision_mask = 0        # walls never initiate a query

    for wall: LevelBox in level.colliders:
        var shape := BoxShape3D.new()
        shape.size = Vector3(wall.half_extents.x * 2.0,
                             wall.height,
                             wall.half_extents.y * 2.0)

        var cs := CollisionShape3D.new()
        cs.shape = shape
        cs.transform = Transform3D(
                Basis(Vector3.UP, wall.yaw),
                wall.center + Vector3(0.0, wall.height * 0.5, 0.0))
        body.add_child(cs)

    parent.add_child(body)
    return body
```

Three details that are easy to get wrong:

**`shape.size` is the full extent, not the half.** `BoxShape3D.size` is a
`Vector3` of full dimensions; `LevelBox.half_extents` is half, matching the C
format. Getting this backwards produces barriers of exactly double thickness,
which looks almost right and drives completely wrong.

**The Y offset.** The level format's `center.y` is the *base* of the box and
`height` its full height, because that is what a level editor wants. Godot
shapes are centred on their transform. Hence `+ height * 0.5`.

**`collision_mask = 0`.** A `StaticBody3D` that never moves has no reason to
detect anything. Setting the mask to zero removes it from the broadphase's query
side entirely. With 300 shapes that is free performance for one line.

### Layers and masks

Godot's layer/mask pair is: *`collision_layer` is what I am; `collision_mask` is
what I look for.* Two objects interact when either one's mask includes the
other's layer.

```gdscript
# scripts/engine/layers.gd
class_name Layers

const WALLS: int = 1 << 0
const CARS: int = 1 << 1
const TRIGGERS: int = 1 << 2      # start/finish beam, pit entry
const DEBRIS: int = 1 << 3
```

| Node | Layer | Mask | Reads as |
|---|---|---|---|
| `Walls` (StaticBody3D) | `WALLS` | `0` | "I am a wall. I look for nothing." |
| `CarBody` | `CARS` | `WALLS` | "I am a car. I sweep against walls." |
| Trigger areas | `TRIGGERS` | `CARS` | "I watch for cars." |

Note the car's mask does **not** include `CARS`. Car-versus-car is resolved
explicitly, below, and letting `move_and_slide` also handle it would produce two
responses to one contact.

Name the layers in Project Settings → `layer_names/3d_physics/` so the inspector
shows "Walls" rather than "Layer 1". It takes a minute and pays for itself the
first time somebody else opens the project.

---

## What you still write: car versus car

`CharacterBody3D` does not push other `CharacterBody3D`s — by design; a
character controller's motion is authored, not solved. So the six-car pile-up
into turn one is yours to write, and the C engine's version ports directly.

This is also the part of the C chapter that is most worth keeping for its own
sake, because separating-axis testing on oriented boxes is a genuinely useful
thing to be able to write.

### The oriented box

```gdscript
# scripts/engine/obb2.gd
class_name Obb2
extends RefCounted

## An oriented box on the XZ plane. A top-down racer never needs Y for
## resolution, so car-versus-car stays a 2D problem: two axes instead of
## fifteen, no quaternions, and a result you can reason about on paper.

var center: Vector2 = Vector2.ZERO          ## (x, z)
var half_extents: Vector2 = Vector2.ONE
var yaw: float = 0.0                        ## radians, about +Y

## The box's own axes, matching Convention: forward is -Z at yaw 0.
func axis_forward() -> Vector2:
    return Vector2(-sin(yaw), -cos(yaw))

func axis_right() -> Vector2:
    return Vector2(cos(yaw), -sin(yaw))

## Writes the four corners in order; useful for debug draw and the minimap.
func corners() -> PackedVector2Array:
    var r: Vector2 = axis_right() * half_extents.x
    var f: Vector2 = axis_forward() * half_extents.y
    return PackedVector2Array([
        center - r - f, center + r - f, center + r + f, center - r + f])

## True when `point` lies inside the box: rotate the point into the box's
## frame, then do an axis-aligned test.
func contains(point: Vector2) -> bool:
    var d: Vector2 = point - center
    return absf(d.dot(axis_right())) <= half_extents.x \
            and absf(d.dot(axis_forward())) <= half_extents.y
```

### The separating axis theorem

> Two convex shapes are disjoint if and only if there exists a line onto which
> their projections do not overlap. For two convex polygons it is enough to test
> the axes perpendicular to their edges.

Two rectangles have four edge directions, and opposite edges are parallel, so
there are **four axes to test** — two from each box.

```gdscript
# scripts/engine/manifold.gd
## One class_name per file, so the manifold and the SAT test that produces it
## live next to obb2.gd rather than inside it.
class_name Manifold
extends RefCounted

var hit: bool = false
var normal: Vector2 = Vector2.ZERO      ## unit, points from `a` towards `b`
var depth: float = 0.0                  ## penetration along `normal`

## Separating-axis test. `depth` is the smallest push that separates the pair,
## which is the minimum translation vector.
static func collide(a: Obb2, b: Obb2, out: Manifold) -> bool:
    out.hit = false

    var ax: Vector2 = a.axis_right()
    var az: Vector2 = a.axis_forward()
    var bx: Vector2 = b.axis_right()
    var bz: Vector2 = b.axis_forward()
    var to_b: Vector2 = b.center - a.center

    var best_depth: float = INF
    var best_axis: Vector2 = Vector2.ZERO

    for axis: Vector2 in [ax, az, bx, bz]:
        # Radius of a box along an arbitrary axis: sum of each half-extent
        # times how much that half-extent's own axis leans along it.
        var ra: float = absf(axis.dot(ax)) * a.half_extents.x \
                + absf(axis.dot(az)) * a.half_extents.y
        var rb: float = absf(axis.dot(bx)) * b.half_extents.x \
                + absf(axis.dot(bz)) * b.half_extents.y

        var separation: float = absf(axis.dot(to_b))
        var overlap: float = ra + rb - separation
        if overlap <= 0.0:
            return false        # a separating axis: done, they are disjoint
        if overlap < best_depth:
            best_depth = overlap
            # Point the axis from a towards b so callers can push consistently.
            best_axis = axis if axis.dot(to_b) >= 0.0 else -axis

    out.hit = true
    out.normal = best_axis
    out.depth = best_depth
    return true
```

Two things make this shorter than it looks.

**Early exit on the first separating axis.** Most pairs are disjoint, and most
disjoint pairs are separated on the first axis tested. The average cost of a
non-collision is closer to one axis than four.

**The projection radius trick.** You do not project the corners. A box's radius
along any axis is `Σ |axis · own_axis_i| * half_extent_i` — the sum of how far
each half-extent reaches along that direction. Four dot products instead of
eight corner projections and a min/max scan.

The result is the **minimum translation vector**: the axis with the *smallest*
overlap is the cheapest way out, which is what makes a car nudged into a
barrier's corner pop out sideways rather than lengthwise.

### Resolving the field

```gdscript
# scripts/game/race_director.gd
func _resolve_car_collisions() -> void:
    for i: int in _racers.size():
        for j: int in range(i + 1, _racers.size()):
            var a: CarBody = _racers[i].car
            var b: CarBody = _racers[j].car
            if not Manifold.collide(a.box(), b.box(), _manifold):
                continue

            # Equal masses: split the separation and swap the closing velocity.
            var push: Vector2 = _manifold.normal * (_manifold.depth * 0.5)
            a.plane_position -= push
            b.plane_position += push

            var closing: float = (b.plane_velocity - a.plane_velocity) \
                    .dot(_manifold.normal)
            if closing >= 0.0:
                continue        # already separating

            var impulse: Vector2 = _manifold.normal * (-closing * 0.55)
            a.plane_velocity -= impulse
            b.plane_velocity += impulse
```

Six cars is fifteen pairs. There is no broadphase because fifteen SAT tests per
tick is nothing — 1,800 a second, each a handful of dot products. **The C
engine's uniform grid exists for the 300 static boxes, not for the cars**, and
in Godot the static side is the physics server's problem, so the grid does not
need porting at all.

`0.55` is the coefficient of restitution for the pair, and it is a feel number:
1.0 is an elastic bounce that sends both cars flying, 0.0 is a dead thud that
leaves them stuck together. Partially inelastic is what a real touring-car nudge
looks like.

`_manifold` and the `Obb2`s are members, allocated once — chapter 03's rule. At
15 pairs × 120 Hz, allocating a fresh `Manifold` per test would be 1,800
objects a second for no reason.

---

## What never touches physics

Chapter 04 already made this argument and it is worth restating where the
temptation is strongest.

**Surfaces are not collision.** The obvious Godot design is an `Area3D` per
gravel trap with `body_entered` / `body_exited` signals. It works. It is also:

- 78 monitoring areas on `circuit02`, each in the broadphase every tick;
- stateful — you have to track enter/exit rather than ask "am I in one now",
  and a car that respawns inside a trap may never receive an `entered`;
- ordered relative to the physics tick in ways that make "which surface am I on
  *this* tick" surprisingly fiddly.

The spline query answers it directly, in the same tick, with no state:

```gdscript
var on_track: bool = absf(query.lateral) <= query.half_width
var in_sand: bool = not on_track and _level.in_sandtrap(pos.x, pos.y)
```

**The road is not a collider.** The car's height comes from
`query.position.y`, not from a raycast against road geometry. That is one
interpolation instead of a shape query per car per tick, it cannot fall through
a gap between two road tiles, and it means the terrain (chapter 13) can be a
purely visual mesh with no collision shape at all.

Where you *would* use `Area3D`s, and this project does: things that are genuinely
events. A pit-lane entry beam. A "you crossed the finish line" trigger, if you
were counting laps geometrically — chapter 10 does not, for reasons of its own.

---

## Determinism, honestly

The physics server is not guaranteed to give bit-identical results across
platforms, or across Godot versions, or between the Godot Physics and Jolt
backends (Project Settings → `physics/3d/physics_engine`). Floating-point
contact ordering inside a solver is not something you control.

For this game that is fine — chapter 15's tests assert on *behaviour* (every car
finishes, nobody is stuck, lap times fall in a range) rather than on exact
positions. But it is worth knowing where the line is:

| If you need | Then |
|---|---|
| Replays that play back identically on the same machine | Physics server is fine |
| Replays that play back across platforms | Store inputs *and* positions, or go custom |
| Lockstep networked multiplayer | The physics server cannot do this. Go custom |
| A test that asserts an exact position after 10,000 ticks | Go custom |

"Go custom" means porting the C engine's `CollisionWorld` — the uniform grid and
the iterative depenetration — into `collision_grid.gd`, which is around 150
lines of GDScript over the `Obb2` code you already have. Chapter 16 shows the
C version behind a GDExtension, which is the sensible destination if you get
that far: fully deterministic, faster than the physics server for this specific
shape of problem, and unchanged from a file that already exists in this
repository.

That is a real fork in the road, and most projects should not take it. Take it
when a *requirement* — networking, replays, exact tests — forces you to, not
because hand-written collision sounds satisfying.

---

## Debugging

Two toggles pay for themselves immediately:

- **Debug → Visible Collision Shapes** draws every shape in the running game.
  The first time you run it on an imported circuit you will find at least one
  barrier that is twice the size you thought.
- **`get_slide_collision(i)`** carries more than the normal:
  `get_collider()`, `get_position()`, `get_travel()`, `get_remainder()`.
  Printing `get_collider().name` when a car gets stuck tells you which barrier,
  by name, in one line.

And one custom overlay, since the SAT code is yours:

```gdscript
# scripts/debug/collision_overlay.gd
## Draws every car's Obb2 and the last contact normal, so a wrong yaw
## convention is visible rather than inferred.
func _draw_car_box(car: CarBody) -> void:
    var pts: PackedVector2Array = car.box().corners()
    for i: int in 4:
        _line(pts[i], pts[(i + 1) % 4], Color.CYAN)
    _line(car.plane_position,
          car.plane_position + car.forward() * 0.4, Color.YELLOW)
```

The forward line is the important half. A box is symmetric, so a car whose yaw
convention is π out looks perfectly correct until you draw which way it thinks
it is pointing.

---

## Exercises

1. **Prove the layer table.** Set the car's `collision_mask` to include `CARS`
   as well as `WALLS`, and leave the explicit resolution in place. Drive into
   another car and describe what happens. Which of the two responses is
   `move_and_slide` applying, and why does the pair end up further apart than
   either response alone would put them?

2. **Find the tunnel.** Set `Engine.physics_ticks_per_second = 15` and drive at
   a thin barrier. Then replace `move_and_slide()` with
   `global_position += velocity * delta` plus the C engine's push-out and try
   again. Which one tunnels, and at what tick rate does the other start to?

3. **Port the grid.** Implement `CollisionGrid` from the C chapter — the uniform
   grid in compressed-sparse-row form, plus iterative depenetration — and run a
   six-car race with it instead of `move_and_slide`. Compare: lap times, CPU
   time per tick (`Performance.TIME_PHYSICS_PROCESS`), and whether two runs from
   the same seed produce identical results.

4. **The axis that matters.** Instrument `Manifold.collide` to record which of
   the four axes wins the minimum. Run a race and print the histogram. Explain
   the distribution in terms of how cars actually touch each other.

5. **Scale the margin.** Set `safe_margin` to 0.05 — a sixth of the car's width
   — and park against a barrier. Measure the gap. Then set it to 0.00001 and
   drive along a wall at full speed. Describe both failure modes.

---

Next: [08 — Vehicle physics](08-vehicle-physics.md)
