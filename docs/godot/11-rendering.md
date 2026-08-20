# 11 — Rendering

> `scripts/engine/prop_batcher.gd` · `scripts/engine/chase_camera.gd`.
> Mirrors [C chapter 10](../learn/10-rendering.md), most of which Godot has
> already written — and one part of which it emphatically has not.

---

## The problem

`circuit02` has 1,237 props. A naive approach gives you 1,237 draw calls per
frame before you have drawn a single car, and on a Raspberry Pi that is the
whole frame budget.

The C engine solves this by baking every prop into a handful of vertex-coloured
meshes grouped by spatial chunk, turning several hundred `DrawModel` calls into
a dozen `DrawMesh` calls, and doing frustum culling per chunk.

Godot's answer is different in shape, and the interesting part is which half of
the C chapter survives.

---

## What Godot does for you

**Frustum culling, automatically, per `VisualInstance3D`.** Every visual node
has an AABB in the rendering server's spatial index, and anything outside the
camera's frustum is skipped before it reaches the GPU. The C chapter's plane
extraction and box tests — 100 lines — are gone.

**Shadow culling against the light, automatically.** Same index, different
frustum. Also gone.

**Sorting, batching by material, and state changes.** The renderer groups draws
by material and pipeline state for you.

**LOD.** Imported meshes get automatic LODs (chapter 05), and
`visibility_range_begin` / `visibility_range_end` on any `GeometryInstance3D`
give you manual control plus fade transitions.

**Occlusion culling**, if you bake an `OccluderInstance3D`. Worth mentioning
only to say: for a top-down racer it is close to useless. Occlusion culling pays
when large objects hide many others, which is a corridor or a city street. From
a camera 3 units above a flat circuit, almost nothing occludes anything. Do not
spend a day on it.

**What Godot does not do**: turn 1,237 nodes into a small number of draw calls.
Each `MeshInstance3D` is one instance in the server, and one draw call per
material. The spatial index makes culling cheap; it does not make the survivors
cheap.

So the C chapter's batching survives. Its culling does not.

---

## `MultiMesh`: the Godot form of static batching

```gdscript
# scripts/engine/prop_batcher.gd
class_name PropBatcher
extends Node3D

## Bakes every level prop into MultiMeshInstance3D nodes, one per distinct kit
## model. circuit02's 1,237 props use about 30 distinct models, so this turns
## 1,237 draw calls into about 30.
##
## The C engine merges props into per-chunk vertex-coloured meshes instead.
## MultiMesh keeps one copy of the geometry and a buffer of transforms, which
## uploads less, rebuilds faster, and — because the mesh is untouched — keeps
## the imported LODs and shadow meshes working.

@export var kit: KitLibrary

func build(level: LevelData) -> void:
    # Pass 1: count instances per model, so each buffer is allocated once.
    var counts: Dictionary[String, int] = {}
    for i: int in level.prop_count():
        var model: String = level.prop_models[i]
        counts[model] = counts.get(model, 0) + 1

    # Pass 2: create one MultiMesh per model, sized exactly.
    var nodes: Dictionary[String, MultiMeshInstance3D] = {}
    var cursors: Dictionary[String, int] = {}
    for model: String in counts:
        var mesh: Mesh = kit.meshes.get(model, null)
        if mesh == null:
            push_warning("BATCH: no mesh for '%s', %d props skipped"
                    % [model, counts[model]])
            continue

        var mm := MultiMesh.new()
        # Order matters: transform_format and the use_* flags must be set
        # before instance_count, which is when the buffer is allocated.
        mm.transform_format = MultiMesh.TRANSFORM_3D
        mm.use_colors = true
        mm.mesh = mesh
        mm.instance_count = counts[model]

        var node := MultiMeshInstance3D.new()
        node.name = model
        node.multimesh = mm
        add_child(node)
        nodes[model] = node
        cursors[model] = 0

    # Pass 3: fill the transform buffers.
    for i: int in level.prop_count():
        var model: String = level.prop_models[i]
        var node: MultiMeshInstance3D = nodes.get(model, null)
        if node == null:
            continue
        var slot: int = cursors[model]
        cursors[model] = slot + 1

        node.multimesh.set_instance_transform(slot, Transform3D(
                PropBatcher.basis_from_engine_euler(level.prop_rotations[i],
                                                    level.prop_scales[i]),
                level.prop_positions[i]))
        node.multimesh.set_instance_color(slot, level.prop_tints[i])
```

The same **measure, allocate once, fill** shape as chapters 03, 04 and 06.
`instance_count` allocates the buffer, and raising it later reallocates and
copies, so counting first is not optional at this size.

Three details that bite:

**`use_colors` must be set before `instance_count`.** The flags determine the
stride of the instance buffer. Setting one afterwards is either ignored or
throws away the data, depending on version. Set everything, then the count.

**Per-instance colour needs a material that reads it.** A `StandardMaterial3D`
ignores instance colours unless `vertex_color_use_as_albedo = true`. In a custom
shader it arrives as `COLOR` in the vertex stage. If your tints do nothing, this
is why.

**The AABB covers every instance.** A `MultiMeshInstance3D` is culled as a
single unit, and its bounds are the union of all its instances — which for
"every barrier on the circuit" is the whole circuit. So it is **always drawn**,
even when one barrier is on screen.

That last point is the real trade, and it deserves its own section.

---

## Draw calls versus culling

The C engine chunks spatially: props within a 16-unit square are merged, so a
chunk is culled as a unit and only the handful near the camera survive.
`MultiMesh` groups by *model*, so nothing is culled at all.

Which is better depends entirely on the machine:

| | Draw calls | Vertices submitted | Good on |
|---|---|---|---|
| One `MeshInstance3D` per prop | 1,237 | only the visible ones | nothing |
| One `MultiMesh` per model | ~30 | all of them, every frame | desktop GPUs |
| Per model **and** chunk | ~30 × visible chunks | only the visible ones | Pi, mobile |

A desktop GPU does not care about 1,237 instanced barriers; the vertex work is
trivial and the driver overhead is what you were avoiding. On the Compatibility
renderer on a Pi (chapter 17), submitting the whole circuit every frame is
exactly the wrong thing.

So the batcher takes a chunk size, and zero means "do not chunk":

```gdscript
## World units per spatial chunk. 0 disables chunking, which is right on
## desktop: 30 always-drawn MultiMeshes beat 30 x N culled ones, because the
## saving is in draw calls and the vertex cost is not the bottleneck.
## On the Compatibility renderer, 16.0 is a good starting point.
@export var chunk_size: float = 0.0

func _chunk_key(model: String, position: Vector3) -> String:
    if chunk_size <= 0.0:
        return model
    return "%s@%d,%d" % [model,
            int(floor(position.x / chunk_size)),
            int(floor(position.z / chunk_size))]
```

Replacing `model` with `_chunk_key(model, position)` throughout is the entire
change. That is the payoff for having written the batcher as three passes over a
key: the chunking decision is one function.

**Measure before choosing.** `Performance.get_monitor(
Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME)` and the editor's Profiler →
Frame Time graph will tell you which side you are on in about two minutes.

---

## Shadows, and the flat-prop problem

The C chapter has a subtle bug worth porting the fix for. Props with no vertical
extent — grass patches, painted markings, sand — lie *in* the ground rather than
standing on it. They cannot plausibly shadow the surface they are part of, and
because they are coplanar with it they land at the same depth in the shadow map
and speckle it with their own acne.

The C engine batches them separately so the depth pass can skip them. In Godot
it is a property:

```gdscript
    # Flat props are coplanar with the ground: they cannot cast a meaningful
    # shadow, and being at the same depth as the surface they sit on, they
    # produce their own acne in the shadow map.
    if _is_flat(mesh):
        node.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF

static func _is_flat(mesh: Mesh) -> bool:
    return mesh.get_aabb().size.y < 0.02
```

`GeometryInstance3D.cast_shadow` has four settings; `OFF`, `ON`,
`DOUBLE_SIDED`, and `SHADOWS_ONLY`. The last is occasionally useful for a
simplified proxy that casts on behalf of a complex mesh — the same idea as
Godot's own "shadow mesh" import option, done by hand.

While you are there, two more `GeometryInstance3D` properties earn their keep on
a batched circuit:

```gdscript
    # Scenery fades out at distance; road tiles never do, because the track
    # under a distant car must still be there.
    if _is_scenery(model):
        node.visibility_range_end = 45.0
        node.visibility_range_end_margin = 6.0
        node.visibility_range_fade_mode = GeometryInstance3D.VISIBILITY_RANGE_FADE_SELF
```

`VISIBILITY_RANGE_FADE_SELF` dithers the object out over the margin instead of
popping it. `FADE_DEPENDENCIES` is for HLOD hierarchies, which this project does
not have.

---

## Lights

The C engine's per-draw light selection — score every light's influence on a
mesh's bounding sphere, upload the best eight — is chapter 10's most intricate
section, and it exists because forward shading with a fixed uniform budget has
no other option.

Godot's renderers handle this differently, and which one you pick decides how
much of it you need to think about:

| Renderer | Many lights | Notes |
|---|---|---|
| **Forward+** | Clustered. Hundreds of lights, cost proportional to what actually touches each cluster | Desktop default. Nothing to do |
| **Mobile** | Forward, with a per-object light limit | Limits configurable in Project Settings |
| **Compatibility** | Forward, with a tighter per-object limit | The Pi target (chapter 17) |

On Forward+ the whole problem disappears: place 32 lights, let the clusterer
sort it out. On Compatibility, the per-object limits under Project Settings →
`rendering/limits/opengl/` are precisely the C engine's `LIGHTS_PER_DRAW`, and
exceeding them means lights silently drop out per object — which looks like
flickering as the camera moves.

That has a direct consequence for the batcher: **a `MultiMeshInstance3D` is one
object for light-limit purposes.** One MultiMesh covering the whole circuit is
lit by whichever handful of lights the renderer picks for it, which on a night
circuit with 32 lamps is visibly wrong. This is the second argument for
chunking, and on the Compatibility renderer it is the stronger one — a chunk is
lit by its own neighbourhood, which is exactly what the C engine's per-draw
selection achieves.

Headlights are the one dynamic case:

```gdscript
# scripts/game/headlights.gd
## Two SpotLight3Ds parented to the car. Cheap because they move with a node
## rather than being re-uploaded per draw, and because they are switched off
## wholesale during the day rather than being given zero energy — a light with
## zero energy still costs a shadow map if it casts.
func set_night(enabled: bool) -> void:
    left.visible = enabled
    right.visible = enabled
    left.shadow_enabled = enabled and Settings.headlight_shadows
```

---

## The chase camera

This is the part of the C chapter that ports directly, because a camera's feel
is game design.

```gdscript
# scripts/engine/chase_camera.gd
class_name ChaseCamera
extends Node3D

## Follows a target from behind and above. Three separate smoothing rates,
## deliberately: position, yaw and height are three different problems.

@export var camera: Camera3D
@export var target: Node3D

## Horizontal offset behind the focus, world units.
@export var distance: float = 2.4
@export var height: float = 1.9
## How far down the velocity vector to bias the focus.
@export var look_ahead: float = 0.55
## False keeps the map north-up, which some players strongly prefer.
@export var rotate_with_target: bool = true

@export_group("Smoothing rates, 1/s")
@export var position_smoothing: float = 9.0
@export var yaw_smoothing: float = 6.0
## Vertical follow is deliberately slower than the horizontal one. Tracking
## height exactly would cancel the elevation out: the car would sit at the same
## point on screen up a climb and down a descent alike. Trailing it instead
## lets the car ride up the frame as it climbs and sink as it drops.
@export var height_smoothing: float = 3.0
## World units the focus may trail the target by, so the lag stays a lag and
## does not become a disconnection on a long climb.
@export var max_height_lag: float = 0.9

var _focus: Vector3 = Vector3.ZERO
var _yaw: float = 0.0

func _process(delta: float) -> void:
    var car: CarBody = target as CarBody
    var aim: Vector3 = car.global_position \
            + Vector3(car.plane_velocity.x, 0.0, car.plane_velocity.y) * look_ahead

    # Horizontal: fast, so the car stays framed.
    var kp: float = Smoothing.factor(position_smoothing, delta)
    _focus.x = lerpf(_focus.x, aim.x, kp)
    _focus.z = lerpf(_focus.z, aim.z, kp)

    # Vertical: slow, and bounded.
    var kh: float = Smoothing.factor(height_smoothing, delta)
    _focus.y = lerpf(_focus.y, aim.y, kh)
    _focus.y = clampf(_focus.y, aim.y - max_height_lag, aim.y + max_height_lag)

    # Yaw: slower still, and via the shortest way round.
    if rotate_with_target:
        var ky: float = Smoothing.factor(yaw_smoothing, delta)
        _yaw = lerp_angle(_yaw, car.yaw, ky)
    else:
        _yaw = 0.0

    # Speed-reactive framing: pull back and lift slightly with speed, so flat
    # out feels fast and a hairpin feels tight.
    var speed01: float = clampf(car.speed / 6.6, 0.0, 1.0)
    var d: float = distance * (1.0 + 0.18 * speed01)
    var h: float = height * (1.0 + 0.10 * speed01)

    var back := Vector3(sin(_yaw), 0.0, cos(_yaw))    # behind, given -Z forward
    global_position = _focus + back * d + Vector3.UP * h
    look_at(_focus, Vector3.UP)
```

**Three rates, on purpose.** 9.0, 6.0 and 3.0 per second — time constants of
0.11 s, 0.17 s and 0.33 s. Position must be snappy or the car leaves the frame.
Yaw must be slower or the world swings sickeningly on every steering input.
Height must be slower still, for the reason in the comment: a camera that
tracks height perfectly makes a hill invisible.

**`lerp_angle`, not `lerpf`, for the yaw.** It interpolates the shortest way
around the circle, so a car crossing from +179° to −179° does not send the
camera the long way round. This is one of the very few places where a Godot
built-in is exactly right and the C version has to write it out.

**`back = (sin yaw, cos yaw)`.** Chapter 05's convention: forward is
`(−sin, −cos)`, so behind is its negation. If the camera ends up in front of the
car, this line is why.

**`look_at` fails on a degenerate up vector.** If the camera is ever exactly
above the focus — which a top-down mode makes possible — `look_at(_focus,
Vector3.UP)` produces a warning and an identity basis. A strictly top-down
camera should pass `Vector3.FORWARD` as up instead.

The camera runs in `_process`, not `_physics_process`, deliberately: it is
presentation, it wants the display rate, and `Smoothing.factor` makes it
frame-rate independent anyway. See chapter 01 for how it interacts with physics
interpolation.

---

## What to measure

Three numbers, in the editor's Debugger → Monitors, tell you almost everything:

| Monitor | Healthy on this project | What it means when it is not |
|---|---|---|
| `RENDER_TOTAL_DRAW_CALLS_IN_FRAME` | 40–80 | The batcher is not grouping — check for props with unique materials |
| `RENDER_TOTAL_PRIMITIVES_IN_FRAME` | ~200k | Chunking off on a weak GPU, or LODs not generated |
| `TIME_PROCESS` vs `TIME_PHYSICS_PROCESS` | physics larger | If rendering dominates, this chapter; if physics does, chapters 06 and 16 |

That last row is the useful one and it surprises people: on this project the
*simulation* is the expensive half, not the rendering. Chapter 16 acts on that.

---

## Exercises

1. **Count the calls.** Load `circuit02` with the batcher disabled (one
   `MeshInstance3D` per prop) and read
   `RENDER_TOTAL_DRAW_CALLS_IN_FRAME`. Then enable it. Then enable chunking at
   16 units. Record all three, plus frame time, on your fastest and slowest
   machines. Which machine changes its mind about the best option?

2. **See the culling you lost.** Point the camera at empty grass, far from every
   prop, with chunking off. Confirm the draw calls and primitives do not drop.
   Turn chunking on and watch them fall.

3. **Break the instance colours.** Turn off `vertex_color_use_as_albedo` on the
   prop material and describe exactly what you see. Then set `use_colors` after
   `instance_count` and describe what you see then.

4. **The shadow speckle.** Force `cast_shadow = ON` for flat props and look at
   the tarmac in low sun. Photograph the acne. Then fix it with a shadow bias
   instead of by disabling the caster, and explain which fix you would ship.

5. **Camera personality.** Set all three smoothing rates to 20.0 and drive a
   lap. Then set them all to 2.0. Then swap only yaw and height. Describe each
   in one sentence, and decide which of the four you would ship for a
   time-trial mode.

6. **The light limit.** Switch to the Compatibility renderer, load the night
   circuit with 32 lamps, and drive with chunking off. Find the lights popping
   in and out, then turn chunking on and confirm it stops. Explain why, in terms
   of what "one object" means to the renderer.

---

Next: [12 — Shaders, lights and shadows](12-shaders-and-shadows.md)
