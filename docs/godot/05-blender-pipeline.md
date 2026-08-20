# 05 — The Blender pipeline

> `tools/blender/io_kenney_racing.py` · `addons/track_importer/` ·
> `scripts/engine/convention.gd`. Mirrors
> [C chapter 04](../learn/04-level-pipeline.md), which had to write the whole
> chain by hand.

---

## The problem

You have an engine and no content. Somebody has to place several hundred road
tiles, barriers, trees and grandstands, draw a racing line, mark a starting
grid, and light the scene.

The C series answers this by turning Blender into the level editor: a Python
add-on walks the scene and writes JSON, and the engine loads the same `.glb`
files Blender is displaying, so the viewport *is* the game's geometry.

Godot changes the calculus, because Godot has a 3D editor of its own. So there
are now two viable pipelines, and the choice between them is a real one:

```
A.  Blender scene ──(io_kenney_racing.py)──► levels/*.track ──(import)──► LevelData
    │
    └── the C engine reads the same .track file. One source, two engines.

B.  Godot scene (circuit.tscn) ──(@tool bake)──► LevelData
    │
    └── authored where it is played. Nothing else can read it.
```

This repository uses **A**, because the C engine and the Godot port must load
the same circuits and the exporter already exists. A Godot-only project should
use **B**, and this chapter covers both — the coordinate-space section applies
to either.

---

## What Godot imports for you

The single biggest thing the C series had to build was the exporter's model
handling: name a `.glb`, load it at runtime, cache it, draw it. Godot's importer
does all of it, and more than you probably want.

**Drop a `.glb` in the project** and Godot converts it to a `PackedScene` in
`.godot/imported/`, with a `.import` file next to the source recording the
settings. The source file stays authoritative; the imported artefact is a build
product and belongs in `.gitignore`.

**Drop a `.blend` in the project** and Godot will do the same, by invoking
Blender itself — you have to point it at the executable in Project Settings →
`filesystem/import/blender/blender_path`, and it needs Blender 3.0+. This is
genuinely convenient: saving in Blender re-imports in Godot within a second, no
export step. It also means every machine that opens the project needs Blender
installed, and a CI box that exports the game needs it too. This project keeps
`.blend` files outside the Godot project folder and imports `.glb` exports, so
the build has one fewer dependency.

**What you get per imported file**, worth knowing because the defaults are not
always what a racer wants:

| Import setting | Default | What this project uses |
|---|---|---|
| Meshes → Ensure Tangents | on | off for kit props — untextured flat shading needs no tangents, and they cost bytes per vertex |
| Meshes → Generate LODs | on | on for scenery, off for road tiles (they are already a couple of triangles) |
| Meshes → Create Shadow Meshes | on | on |
| Materials → Extract | in-scene | extract to `.tres` once, so all 400 tree instances share one material |
| Skins, Animations | on | off — nothing in the kit animates |

Set these in the **Advanced Import Settings** dialog (double-click the file in
the FileSystem dock), not by editing `.import` files by hand.

### Object-name suffixes

Godot's scene importer reads suffixes on Blender object names and acts on them.
The useful ones here:

| Suffix | Effect |
|---|---|
| `-noimp` | Skip this object entirely |
| `-col` | Also generate a trimesh `StaticBody3D` collision sibling |
| `-colonly` | Import *only* as collision, no visible mesh |
| `-convcol` / `-convcolonly` | As above, but a convex shape |
| `-occ` / `-occonly` | Generate an occluder |

`-colonly` is how you would author invisible walls in pipeline B: a box in
Blender named `wall-colonly` becomes a collider and nothing else, which is
exactly a `LevelCollider` with no matching prop. The full list is in Godot's
"Importing 3D scenes" documentation and it grows between versions.

### Never edit an imported scene

The rule that saves the most pain: **an imported `.glb` scene is regenerated
from source on every re-import, and your edits to it are lost.** To attach a
script, add a child, or change a material per-instance, right-click the imported
scene in the FileSystem dock → **New Inherited Scene**, and save that as
`scenes/props/tree.tscn`. The inherited scene keeps a live link to the import,
so re-exporting from Blender updates the geometry and keeps your additions.

---

## Coordinate spaces, precisely

This is where content pipelines go to die, so it is worth being exact. There are
three frames in play and only one real conversion between them.

**Blender is Z-up, right-handed.** X right, Y forward (into the screen), Z up.

**glTF is Y-up, right-handed, with -Z forward.** X right, Y up, Z toward the
viewer.

**Godot is glTF's frame.** X right, Y up, -Z forward. `Node3D`'s local forward
is `-basis.z`; `look_at()` points `-Z` at the target.

The Blender→glTF conversion is a rotation of −90° about X:

```
Blender (x, y, z)  →  Y-up (x, z, −y)
```

The C engine's exporter implements this by hand:

```python
# tools/blender/io_kenney_racing.py
BLENDER_TO_ENGINE = Matrix(((1, 0, 0), (0, 0, 1), (0, -1, 0)))

def to_engine_point(v):
    return [round(v.x, 5), round(v.z, 5), round(-v.y, 5)]
```

and Godot's glTF importer implements the same thing for meshes. **They agree.**
The C engine's "engine space" and Godot's world space are the same axes, in the
same handedness, with the same units. A position exported for the C engine drops
into Godot unchanged, which is why chapter 04's importer copies
`prop_positions` straight across.

### The one difference: which way is zero yaw

The two engines disagree about a single thing, and it is a convention rather
than a coordinate system:

| | Forward at yaw 0 | Forward as a function of yaw | Right |
|---|---|---|---|
| C engine | `+Z` | `(sin θ, cos θ)` | `(−cos θ, sin θ)` |
| Godot | `−Z` | `(−sin θ, −cos θ)` | `(cos θ, −sin θ)` |

Look at the middle column. `(−sin θ, −cos θ) = (sin(θ + π), cos(θ + π))`.

> **The two conventions differ by exactly π.**

That single fact is worth more than any amount of case-by-case sign chasing,
because it has two immediate consequences:

**1. All *relative* maths is identical.** A steering angle, a yaw rate, an angle
between two directions, a dot product with the car's right vector — none of them
change, because adding π to both operands cancels. Chapter 08's vehicle model is
a line-for-line port of `car.c` for exactly this reason, including the
much-commented sign on the yaw rate.

**2. Only *absolute* conversions change**, and there are precisely four of them.
They live in one file so they cannot drift:

```gdscript
# scripts/engine/convention.gd
class_name Convention

## Conversions between the C engine's yaw convention (zero faces +Z, as
## written by tools/blender/io_kenney_racing.py) and Godot's (zero faces -Z,
## the direction of -basis.z). They differ by exactly pi, so every relative
## angle is shared and only these functions know about it.

## Godot yaw that faces the given XZ direction. Note atan2(-dx, -dz), not
## atan2(dx, dz): the latter is the C engine's, and using it here points every
## car and every gate exactly backwards.
static func yaw_from_direction(dx: float, dz: float) -> float:
    return atan2(-dx, -dz)

## Unit forward vector on the XZ plane for a Godot yaw. Equal to -basis.z with
## y dropped; written out so the vehicle model can stay in 2D.
static func forward(yaw: float) -> Vector2:
    return Vector2(-sin(yaw), -cos(yaw))

## The car's right on the XZ plane: +basis.x. forward x up, which is what a
## viewer looking along forward with +Y up sees as screen right.
static func right(yaw: float) -> Vector2:
    return Vector2(cos(yaw), -sin(yaw))

## Engine-space yaw (degrees, +Z zero) to Godot yaw (radians, -Z zero).
static func yaw_from_engine_degrees(degrees: float) -> float:
    return deg_to_rad(degrees) + PI

## Engine-space XYZ euler in degrees to Godot's, for prop rotations.
static func euler_from_engine(degrees: Vector3) -> Vector3:
    return Vector3(deg_to_rad(degrees.x),
                   deg_to_rad(degrees.y) + PI,
                   deg_to_rad(degrees.z))
```

Test it, because a sign error here is invisible until a circuit is half built:

```gdscript
# tests/test_convention.gd
func test_forward_matches_node3d() -> void:
    var node := Node3D.new()
    for degrees: int in [0, 37, 90, 180, 271]:
        var yaw: float = deg_to_rad(float(degrees))
        node.rotation = Vector3(0.0, yaw, 0.0)
        var from_basis: Vector3 = -node.transform.basis.z
        var from_helper: Vector2 = Convention.forward(yaw)
        assert_float(from_helper.x).is_equal_approx(from_basis.x, 1e-5)
        assert_float(from_helper.y).is_equal_approx(from_basis.z, 1e-5)
    node.free()

func test_yaw_from_direction_round_trips() -> void:
    for degrees: int in [0, 37, 90, 180, 271]:
        var yaw: float = wrapf(deg_to_rad(float(degrees)), -PI, PI)
        var f: Vector2 = Convention.forward(yaw)
        assert_float(Convention.yaw_from_direction(f.x, f.y)) \
                .is_equal_approx(yaw, 1e-5)
```

The first test is the important one: it checks the helper against Godot's own
`Node3D` rather than against another piece of your maths. That is the difference
between testing a convention and testing your belief about it.

### Prop rotations: euler order

The C level format stores an XYZ euler triple applied as `Rx·Ry·Rz`, matching
raymath's `MatrixRotateXYZ`. **Godot's `Node3D.rotation` uses YXZ order** by
default (`rotation_order`, `EULER_ORDER_YXZ`).

For the kit this rarely matters: almost every prop is pure yaw, where every
euler order agrees. For the few that are not — a banked barrier, a tilted sign —
the orders give different results, and the fix is not to convert euler triples
but to stop using them:

```gdscript
# scripts/engine/prop_batcher.gd
## Build the basis in the exporter's order explicitly rather than assigning
## Node3D.rotation, whose euler order is YXZ and would silently disagree for
## any prop with more than one non-zero angle.
static func basis_from_engine_euler(radians: Vector3, scale: Vector3) -> Basis:
    var b: Basis = Basis(Vector3.RIGHT, radians.x) \
            * Basis(Vector3.UP, radians.y) \
            * Basis(Vector3.BACK, radians.z)
    return b.scaled(scale)
```

`Basis(axis, angle)` composes explicitly, so the order is the one you wrote.
This is the sort of thing that produces a bug report reading "the pit wall signs
are rotated wrong but only on circuit 2", and it is much cheaper to get right
once than to find later.

---

## Pipeline A: keep Blender as the editor

Nothing about the Blender side changes. `io_kenney_racing.py` walks the scene,
classifies objects by name and collection, and writes JSON:

| Blender object | Becomes |
|---|---|
| Any mesh from the kit | A prop: model name, position, rotation, scale, tint |
| Object in the colliders collection | A `LevelBox` in `colliders` |
| Object in the sandtraps collection | A `LevelBox` in `sandtraps` |
| A curve or ordered empties | The waypoint ring, with per-point width |
| Empties named `spawn.*` | Grid slots |
| Empties named `gate.*` | Checkpoints, in name order |
| Blender lamp objects | Point and spot lights |

The only Godot-side change is the file extension — `.track` rather than
`.level.json`, so chapter 04's `EditorImportPlugin` can claim it without
hijacking every JSON file in the project.

### Resolving a model name to a mesh

The C engine's `assets.c` keeps a name-keyed cache and calls `LoadModel` on
demand. In Godot, the batcher (chapter 11) needs `Mesh` resources, not scenes,
so the lookup is built once as a resource:

```gdscript
# scripts/data/kit_library.gd
@tool
class_name KitLibrary
extends Resource

## Maps a kit model name — "roadStraight", with no directory and no extension,
## exactly as the exporter writes it — to the Mesh the batcher instances.
##
## Baking it into a resource means the running game does no directory scanning
## and no scene instantiation just to reach a mesh.

@export var meshes: Dictionary[String, Mesh] = {}

@export_tool_button("Rescan assets/models")
var rescan: Callable = _rescan

func _rescan() -> void:
    meshes.clear()
    var dir: DirAccess = DirAccess.open("res://assets/models")
    if dir == null:
        push_error("KIT: assets/models missing")
        return
    for file: String in dir.get_files():
        if not file.ends_with(".glb"):
            continue
        var scene: PackedScene = load("res://assets/models/%s" % file) as PackedScene
        if scene == null:
            continue
        var mesh: Mesh = _first_mesh(scene)
        if mesh == null:
            push_warning("KIT: '%s' contains no MeshInstance3D" % file)
            continue
        meshes[file.get_basename()] = mesh
    print("KIT: %d models" % meshes.size())

static func _first_mesh(scene: PackedScene) -> Mesh:
    var root: Node = scene.instantiate()
    var found: Mesh = null
    for node: Node in _walk(root):
        var mi: MeshInstance3D = node as MeshInstance3D
        if mi != null and mi.mesh != null:
            found = mi.mesh
            break
    root.free()      # not queue_free: this instance was never in the tree
    return found
```

Three things in there are Godot-specific and easy to get wrong:

- **`@export_tool_button`** (Godot 4.4+) puts a button in the inspector. On
  earlier versions use an `@export var rescan: bool` that resets itself in its
  setter — uglier, same effect.
- **`root.free()`, not `queue_free()`.** An instantiated scene that was never
  added to the tree has no frame to be freed at the end of; `queue_free` on it
  leaves an orphan the debugger will report at exit.
- **`Dictionary[String, Mesh]`** needs Godot 4.4. Before that, use an
  `Array[String]` and a parallel `Array[Mesh]` — clumsier, but typed, which an
  untyped `Dictionary` is not.

---

## Pipeline B: author the circuit in Godot

If nothing else has to read your levels, this is the better workflow, because
the editor showing you the track *is* the engine that will run it — the same
property the C series wants from Blender, obtained more directly.

The circuit becomes a scene:

```
Circuit (Node3D)                     circuit.tscn
├── Props (Node3D)                   inherited kit scenes, placed by hand
├── CentreLine (Path3D)              the racing line, as a Curve3D
├── Walls (Node3D)                   StaticBody3D + BoxShape3D each
├── Sandtraps (Node3D)               Area3D + BoxShape3D each
├── Grid (Node3D)                    Marker3D per spawn slot
├── Gates (Node3D)                   Marker3D per checkpoint, in order
└── Lights (Node3D)                  OmniLight3D / SpotLight3D
```

Everything here is a stock node, which means the editor gizmos work: you drag a
`Path3D` point and the racing line moves, with a curve handle at each end.
Compared with authoring a waypoint ring in Blender this is a substantial
upgrade, and it is the main argument for pipeline B.

Then a `@tool` script bakes the scene to a `LevelData` so the runtime keeps the
same flat, cheap data model:

```gdscript
# scripts/data/circuit_baker.gd
@tool
class_name CircuitBaker
extends Node3D

## Bakes this scene into a LevelData resource. The scene is the editable
## source; the resource is what the game loads. Baking rather than walking the
## scene at runtime keeps the game's data model identical between pipeline A
## and pipeline B — the race director cannot tell which one produced its level.

@export_file("*.tres") var output_path: String = "res://levels/circuit03.tres"
@export var laps: int = 3
@export var default_track_width: float = 0.69

@export_tool_button("Bake to LevelData")
var bake_button: Callable = bake

func bake() -> void:
    var level := LevelData.new()
    level.display_name = name
    level.laps = laps
    level.default_track_width = default_track_width

    _bake_props(level, $Props)
    _bake_centre_line(level, $CentreLine)
    level.colliders = _bake_boxes($Walls)
    level.sandtraps = _bake_boxes($Sandtraps)
    level.spawns = _bake_spawns($Grid)
    level.checkpoints = _bake_gates($Gates)
    level.lights = _bake_lights($Lights)

    var error: Error = ResourceSaver.save(level, output_path)
    if error != OK:
        push_error("BAKE: could not save '%s' (%d)" % [output_path, error])
        return
    print("BAKE: %s — %d props, %d waypoints, %d gates"
            % [level.display_name, level.prop_count(),
               level.waypoint_count(), level.checkpoints.size()])

func _bake_centre_line(level: LevelData, path: Path3D) -> void:
    # Curve3D can hand back a polyline directly; take that rather than the
    # control points, so the waypoint ring matches what the editor drew.
    # Chapter 06 resamples it again for uniform arc-length queries.
    var curve: Curve3D = path.curve
    var points: PackedVector3Array = curve.tessellate(5, 4.0)
    level.waypoint_positions = points
    level.waypoint_widths.resize(points.size())
    level.waypoint_widths.fill(default_track_width)

func _bake_boxes(root: Node3D) -> Array[LevelBox]:
    var out: Array[LevelBox] = []
    for child: Node in root.get_children():
        var body: Node3D = child as Node3D
        var shape: CollisionShape3D = body.get_node_or_null("CollisionShape3D") \
                as CollisionShape3D
        var box: BoxShape3D = (shape.shape if shape != null else null) as BoxShape3D
        if box == null:
            push_warning("BAKE: '%s' has no BoxShape3D, skipped" % child.name)
            continue
        var entry := LevelBox.new()
        entry.center = body.global_position
        entry.half_extents = Vector2(box.size.x * 0.5, box.size.z * 0.5)
        entry.height = box.size.y
        # The node's own basis is the truth; the yaw scalar is derived from it
        # here, in one place, so a rotated wall and its data always agree.
        entry.yaw = body.global_rotation.y
        out.append(entry)
    return out
```

`_get_configuration_warnings` is worth adding to a baker like this:

```gdscript
func _get_configuration_warnings() -> PackedStringArray:
    var warnings: PackedStringArray = []
    var line: Path3D = get_node_or_null("CentreLine") as Path3D
    if line == null:
        warnings.append("No CentreLine (Path3D) child: the circuit cannot be driven.")
    elif line.curve.point_count < 3:
        warnings.append("CentreLine needs at least 3 points.")
    if get_node_or_null("Grid") == null:
        warnings.append("No Grid child: cars will all start at the origin.")
    return warnings
```

That yellow triangle in the scene tree is Godot's version of the C series'
tolerant-loader warnings, and it appears *while you are authoring* rather than
when you press play. It is one of the genuinely nicer things about the editor
and the engine being the same program.

---

## The separation of concerns, restated for nodes

Pipeline B has one trap the JSON pipeline does not, and it is the same trap
chapter 04 warns about from the other direction.

In Blender, art and collision are separate lists because the exporter puts them
in separate lists. In Godot it is tempting to let the art *be* the collision:
name the road mesh `road-col` and let the importer generate a trimesh collider.
Then drivability is derived from art, and:

- Every gap between two road tiles becomes a physical ledge.
- Widening the lane means moving geometry.
- The car collides with the road it is driving on, at 120 Hz, against a trimesh
  of several thousand triangles per tile.

Keep the four concerns separate exactly as the C format does. **The road is not
a collider. The lane is the spline plus a width, and nothing else.** Chapter 07
builds the collision world from the `colliders` list only, and chapter 08's car
never queries physics to know whether it is on tarmac — it asks the spline.

---

## Exercises

1. **Verify the frames.** Put an empty at Blender coordinates (1, 2, 3), export,
   import, and read `global_position` in Godot. Confirm it is (1, 3, −2). Then
   rotate it 30° about Blender's Z and confirm what happens to `rotation.y`.

2. **Break the convention deliberately.** Change `Convention.yaw_from_direction`
   to `atan2(dx, dz)` — the C engine's version — and run a race. Describe
   exactly what the grid does at the start, and why chapter 04's auto-generated
   gates make it worse rather than better.

3. **The euler order.** Author a prop in Blender with 20° of X and 40° of Y
   rotation. Export it, then place it in Godot twice: once by assigning
   `rotation` directly and once through `basis_from_engine_euler`. Measure the
   angle between the two forward vectors. Now do it with only a Y rotation and
   explain the difference.

4. **Bake and diff.** Take `circuit01.track`, import it, then build the same
   circuit as a `.tscn` in pipeline B and bake it. Compare the two `LevelData`s
   field by field with a script. Which fields can never match, and does it
   matter?

5. **Re-import survival.** Attach a script to an imported `.glb` scene directly,
   save, then touch the source file to force a re-import. Confirm the script is
   gone. Redo it with an inherited scene. This is a two-minute exercise that
   will save you a day.

---

Next: [06 — Splines and arc length](06-splines.md)
