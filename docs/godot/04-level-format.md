# 04 — Level data without a parser

> `scripts/data/level_data.gd` · `addons/track_importer/` — the 400 lines of
> recursive descent from [C chapter 03](../learn/03-json-parser.md) that you do
> not write, and the data model from
> [C chapter 04](../learn/04-level-pipeline.md) that you still do.

---

## The problem

Levels are data. Data needs a format. The format needs a reader.

The C engine writes its own JSON parser — 401 lines, recursive descent, arena
allocated — for three stated reasons: allocation control, comments, and that a
JSON grammar fits on a postcard so writing one teaches you something.

Two of those three reasons evaporate in Godot. You cannot control allocation, and
there is nothing to teach you because `Resource` serialisation is already
written. The third — comments in hand-edited files — still bites, and this
chapter ends up caring about it more than you would expect.

The real question is not "which parser". It is: **what is the source of truth
for a circuit, and who is allowed to edit it?**

---

## The data model

Start where the C series starts: with what the game actually wants.

```gdscript
# scripts/data/level_data.gd
class_name LevelData
extends Resource

## A complete circuit. Produced by the Blender exporter (chapter 05) and
## imported into this form; consumed by the spline, the collision grid, the
## prop batcher and the race director.
##
## Coordinates are Godot-space: X right, Y up, -Z forward, one unit == one
## Kenney kit tile. Positions come straight from the exporter, which already
## writes Y-up; yaw is shifted by pi to Godot's forward axis at import time,
## so nothing downstream has to think about handedness.

@export var display_name: String = "Untitled"
@export var laps: int = 3
@export var default_track_width: float = 0.69

@export_group("Environment")
@export var sky_color: Color = Color8(124, 176, 214)
@export var ground_color: Color = Color8(77, 143, 110)
@export var ambient_color: Color = Color8(88, 90, 100)
@export var sun_direction: Vector3 = Vector3(-0.45, -1.0, 0.35)
@export var sun_color: Color = Color8(255, 250, 235)
@export_range(0.0, 4.0, 0.01) var sun_intensity: float = 0.62

## --- bulk arrays, written by the importer -------------------------------
## Parallel, not an array of records: circuit02 has 1,237 props, and 1,237
## Resource objects would be 1,237 separate allocations to load, save, and
## walk with a pointer chase each. See chapter 03.
@export var prop_models: PackedStringArray = []
@export var prop_positions: PackedVector3Array = []
@export var prop_rotations: PackedVector3Array = []   # XYZ euler, radians
@export var prop_scales: PackedVector3Array = []
@export var prop_tints: PackedColorArray = []

@export var waypoint_positions: PackedVector3Array = []
@export var waypoint_widths: PackedFloat32Array = []

## --- record arrays, edited by hand ---------------------------------------
## Tens of entries, each one something a designer moves individually. The
## inspector can show these; it cannot show a PackedVector3Array usefully.
@export var colliders: Array[LevelBox] = []
@export var sandtraps: Array[LevelBox] = []
@export var spawns: Array[LevelSpawn] = []
@export var checkpoints: Array[LevelGate] = []
@export var lights: Array[LevelLight] = []

func prop_count() -> int:
    return prop_models.size()

func waypoint_count() -> int:
    return waypoint_positions.size()
```

Seven concepts, exactly as in C, and the same headline design decision:

> **Art, solidity, drivability and run-off are four independent lists.**

| Concept | Field | What it means |
|---|---|---|
| Art | `prop_*` | A mesh drawn at a transform. Purely visual. |
| Solidity | `colliders` | A yawed box you cannot drive through. Invisible. |
| Drivability | `waypoint_*` | The centre line plus a width. Inside it is tarmac. |
| Run-off | `sandtraps` | A yawed box that is slow but not solid. Invisible. |
| Progress | `checkpoints` | Ordered gates. |
| Grid | `spawns` | Where cars start. |
| Light | `lights` | Point and spot lamps. |

None is derived from another. What that buys and costs is unchanged from the C
series — you can lay a road tile that is not drivable, make an invisible wall,
draw gravel that is felt somewhere slightly different — and so is the answer to
the cost: **do not derive one from another, test the relationship.** Chapter 15
ports those tests.

### Why two storage strategies in one resource

The split above is the only genuinely new decision in this chapter, so it is
worth defending.

`prop_positions` as a `PackedVector3Array` is one flat buffer of 3,711 floats.
As `Array[LevelProp]` it would be 1,237 `Resource` objects, each with its own
refcount, each serialised as a separate `[sub_resource]` block in the `.tres`,
each loaded and freed individually. Loading `circuit02` goes from "read one
buffer" to "construct 1,237 objects", and the `.tres` file goes from a few
hundred kilobytes to a few megabytes.

But `colliders` as packed arrays would be unusable: a collider is a position, a
half-extent, a height and a yaw — four different shapes of data that would need
four parallel arrays with no way to see one collider as a unit, and the
inspector would show you four lists of numbers.

So the rule this project uses:

> **Bulk, generated, uniform → parallel packed arrays. Few, hand-edited,
> heterogeneous → an `Array` of small `Resource` records.**

The records themselves are unremarkable:

```gdscript
# scripts/data/level_box.gd
class_name LevelBox
extends Resource

## A box on the XZ plane, yawed about Y. Used for both solid colliders and
## run-off traps: the difference is which list it is in, not what it is.
@export var center: Vector3 = Vector3.ZERO
@export var half_extents: Vector2 = Vector2(0.5, 0.5)   # X and Z, before yaw
@export var height: float = 0.13
@export var yaw: float = 0.0                            # radians, about +Y
```

Note `yaw` is stored in **radians**, unlike the C format's degrees. Degrees are
for humans reading a JSON file; the importer converts once and nothing
downstream ever calls `deg_to_rad` in a loop. The same applies to prop
rotations. Every angle inside the running game is radians, and the only two
places degrees exist are the exporter's output and the inspector's display.

---

## The format: what `.tres` gives you for free

Save that resource and Godot writes this:

```ini
[gd_resource type="Resource" script_class="LevelData" load_steps=6 format=3]

[ext_resource type="Script" path="res://scripts/data/level_data.gd" id="1"]

[sub_resource type="Resource" id="LevelBox_a1"]
script = ExtResource("2")
center = Vector3(0, 0.06, 0)
half_extents = Vector2(0.125, 0.06)
height = 0.13
yaw = 1.5708

[resource]
script = ExtResource("1")
display_name = "Ardennes Circuit"
laps = 3
prop_models = PackedStringArray("roadStraight", "roadCorner", ...)
prop_positions = PackedVector3Array(0, 0, 0, 1, 0, 0, ...)
colliders = [SubResource("LevelBox_a1")]
```

You get, without writing a line of parser:

- **A text format you can `git diff`.** Same property the C series values in
  JSON, for the same reason.
- **Type-checked deserialisation.** A field whose type does not match is
  reported by the resource loader, not discovered when the game reads it.
- **Editor integration.** Double-click the `.tres` and every field is in the
  inspector, with the ranges and groups from your `@export` annotations.
- **A binary variant for free.** The export preset's *Convert text resources to
  binary* option (on by default) turns every `.tres` into a `.res` at export
  time. You get the diffable format in the repository and the fast one in the
  build, with no second code path.

And you give up:

- **Comments.** `.tres` has no comment syntax and Godot rewrites the file
  wholesale when it saves, so anything you add by hand is lost the next time the
  editor touches it. The C format's `// drivable width, world units` annotations
  have nowhere to go. What replaces them is the `##` doc comment on the
  `@export`, which shows up as an inspector tooltip — arguably better placed,
  but only visible in the editor, not in the file.
- **Hand-editability in practice.** You *can* edit a `.tres`, but sub-resource
  IDs and `load_steps` counts make it unpleasant, and getting it wrong produces
  a resource that fails to load with a terse message.
- **Portability.** A `.tres` is only readable by Godot. The C engine in this
  repository cannot load one. If both engines must read the same circuit — which
  in this repository they must — `.tres` cannot be the source of truth.

That last point is decisive here, and it is what the rest of the chapter is
about.

---

## Keeping JSON as the source of truth

The Blender exporter in `tools/blender/io_kenney_racing.py` already writes
`levels/*.level.json`, the C engine already reads it, and the level tests
already validate it. Replacing it with a Godot-only format would mean
maintaining an exporter per engine and losing the shared test suite.

So: **JSON stays the source of truth; `LevelData` is a build artefact.** Godot
has a first-class mechanism for exactly this shape — a source file that is not
itself a resource, converted at import time into one that is.

```gdscript
# addons/track_importer/track_importer.gd
@tool
class_name TrackImporter
extends EditorImportPlugin

## Converts the C engine's circuit JSON into a LevelData resource.
##
## The source file stays the thing Blender writes and the C engine reads;
## Godot imports it the same way it imports a .png, caching the result in
## .godot/imported/. Re-exporting from Blender re-imports automatically.

func _get_importer_name() -> String:
    return "racer.track"

func _get_visible_name() -> String:
    return "Racing circuit"

func _get_recognized_extensions() -> PackedStringArray:
    # NOT "json": an import plugin claims every file with the extension it
    # names, and stealing every .json in the project is antisocial. The
    # exporter writes levels/*.track, which is JSON with a distinct suffix.
    return PackedStringArray(["track"])

func _get_save_extension() -> String:
    return "res"

func _get_resource_type() -> String:
    return "Resource"

func _get_preset_count() -> int:
    return 1

func _get_preset_name(_index: int) -> String:
    return "Default"

func _get_import_options(_path: String, _preset: int) -> Array[Dictionary]:
    return [{
        "name": "auto_checkpoints",
        "default_value": 12,
        "property_hint": PROPERTY_HINT_RANGE,
        "hint_string": "0,32,1",
    }]

func _get_option_visibility(_path: String, _option: StringName,
                            _options: Dictionary) -> bool:
    return true

func _get_priority() -> float:
    return 1.0

func _get_import_order() -> int:
    return 0

func _import(source_file: String, save_path: String, options: Dictionary,
             _platform_variants: Array[String],
             _gen_files: Array[String]) -> Error:
    var text: String = FileAccess.get_file_as_string(source_file)
    if text.is_empty():
        push_error("TRACK: '%s' is empty or unreadable" % source_file)
        return ERR_FILE_CANT_READ

    var parsed: Variant = JSON.parse_string(text)
    if not (parsed is Dictionary):
        push_error("TRACK: '%s' is not a JSON object" % source_file)
        return ERR_FILE_CORRUPT

    var level: LevelData = LevelJson.to_level_data(
            parsed as Dictionary, source_file, int(options["auto_checkpoints"]))
    if level == null:
        return ERR_FILE_CORRUPT

    return ResourceSaver.save(level, "%s.%s" % [save_path, _get_save_extension()])
```

Registered by a two-line plugin:

```gdscript
# addons/track_importer/plugin.gd
@tool
extends EditorPlugin

var _importer: TrackImporter = null

func _enter_tree() -> void:
    _importer = TrackImporter.new()
    add_import_plugin(_importer)

func _exit_tree() -> void:
    remove_import_plugin(_importer)
    _importer = null
```

What this buys, and it is a lot:

- `preload("res://levels/circuit01.track")` returns a fully-built `LevelData`.
  The game never sees JSON, never parses at runtime, and never pays for it.
- Re-exporting from Blender triggers a re-import automatically — the editor
  watches the file.
- The parse cost moves to import time, so shipping is the binary `.res`.
- The C engine and Godot read the same file.

The one wrinkle is the extension. `_get_recognized_extensions` claims *every*
file with that suffix in the project, so returning `"json"` would hijack every
JSON file you ever add. Giving circuits their own extension is the clean fix;
`.track` here, and the Blender exporter writes it.

---

## The tolerant loader

The conversion itself is the part that corresponds to `LevelLoad`, and its
design goals are identical: **accept anything reasonable, warn about anything
odd, fail only on the genuinely broken.**

```gdscript
# scripts/data/level_json.gd
class_name LevelJson

const FORMAT_ID: String = "kenney-topdown-racer"

## Every field is optional except waypoints. A file with nothing but a
## waypoint ring loads, builds a spline, gets auto-generated gates, and can
## be driven — invisible, but drivable. The spline tests rely on this to
## build circuits in six lines with no file at all.
static func to_level_data(root: Dictionary, source: String,
                          auto_checkpoints: int) -> LevelData:
    var format: String = _string(root, "format", "")
    if not format.is_empty() and format != FORMAT_ID:
        # Loud but not fatal: this catches "you handed me the wrong kind of
        # JSON", it does not gate-keep. A file with no format field at all is
        # accepted silently.
        push_warning("TRACK: '%s' declares format '%s', expected '%s'"
                % [source, format, FORMAT_ID])

    var level := LevelData.new()
    var settings: Dictionary = _dict(root, "settings")
    level.display_name = _string(root, "name", source.get_file())
    level.laps = _int(settings, "laps", 3)
    level.default_track_width = _float(settings, "track_width", 0.69)
    level.sky_color = _color(settings, "sky_color", Color8(124, 176, 214))
    # ... the rest of settings ...

    _read_props(level, _array(root, "props"))
    _read_waypoints(level, _array(root, "waypoints"), level.default_track_width)

    if level.waypoint_count() < 3:
        push_error("TRACK: '%s' has %d waypoints, need at least 3"
                % [source, level.waypoint_count()])
        return null

    level.colliders = _read_boxes(_array(root, "colliders"), true)
    level.sandtraps = _read_boxes(_array(root, "sandtraps"), false)
    level.spawns = _read_spawns(_array(root, "spawns"))
    level.lights = _read_lights(_array(root, "lights"))
    _read_gates(level, _array(root, "checkpoints"), auto_checkpoints)
    return level
```

Three details carry over from `level.c` more or less verbatim.

### Typed accessors at the boundary

```gdscript
static func _float(source: Dictionary, key: String, fallback: float) -> float:
    var value: Variant = source.get(key, null)
    if value is float or value is int:
        return float(value)
    return fallback

static func _vec3(value: Variant, fallback: Vector3) -> Vector3:
    if not (value is Array):
        return fallback
    var array: Array = value as Array
    if array.size() < 3:
        return fallback
    return Vector3(float(array[0]), float(array[1]), float(array[2]))
```

These are `JsonFloatField` and friends from `engine/src/json.h`, and they exist
for the same two reasons: a default per field means the format is optional by
construction, and — the reason chapter 02 cares — **this is the only file in the
project where a `Variant` is allowed to exist.** Everything above these
functions is typed. This is the boundary, and it is deliberately narrow.

Note `value is float or value is int`. JSON has one number type; GDScript's
parser gives you `int` for `3` and `float` for `3.0`. A level with `"laps": 3`
and one with `"laps": 3.0` must behave the same, and forgetting this produces a
fallback value that looks like a mysterious content bug.

### Skipping bad props without leaving a hole

```gdscript
static func _read_props(level: LevelData, raw: Array) -> void:
    var count: int = raw.size()
    level.prop_models.resize(count)
    level.prop_positions.resize(count)
    level.prop_rotations.resize(count)
    level.prop_scales.resize(count)
    level.prop_tints.resize(count)

    var written: int = 0
    for i: int in count:
        if not (raw[i] is Dictionary):
            continue
        var entry: Dictionary = raw[i] as Dictionary
        var model: String = _string(entry, "model", "")
        if model.is_empty():
            # A silently dropped prop is a mystery; a warned one is a bug report.
            push_warning("TRACK: prop %d has no model, skipped" % i)
            continue
        level.prop_models[written] = model
        # Positions need no conversion: the exporter already writes Y-up,
        # which is Godot's space too. Only yaw shifts — see chapter 05.
        level.prop_positions[written] = _vec3(entry.get("pos"), Vector3.ZERO)
        level.prop_rotations[written] = Convention.euler_from_engine(
                _vec3(entry.get("rot"), Vector3.ZERO))
        level.prop_scales[written] = _vec3(entry.get("scale"), Vector3.ONE)
        level.prop_tints[written] = _color_from(entry.get("tint"), Color.WHITE)
        written += 1

    # Correct the length once, at the end. The alternative — two passes to
    # count the valid props first — doubles the walking to save nothing.
    level.prop_models.resize(written)
    level.prop_positions.resize(written)
    level.prop_rotations.resize(written)
    level.prop_scales.resize(written)
    level.prop_tints.resize(written)
```

Allocate for the optimistic count, fill with a separate `written` cursor, then
shrink. Identical to the C, and `resize()` down does not reallocate, so the
correction is free.

### Auto-generated gates

If a level ships no checkpoints, manufacture twelve:

```gdscript
static func _read_gates(level: LevelData, raw: Array, auto_count: int) -> void:
    if raw.size() > 0:
        for entry: Variant in raw:
            # ... read explicit gates ...
        return
    if auto_count <= 0:
        return

    var n: int = level.waypoint_count()
    for i: int in auto_count:
        # Spread gates evenly along the centre line and face each one down-track.
        @warning_ignore("integer_division")
        var wi: int = i * n / auto_count
        var a: Vector3 = level.waypoint_positions[wi]
        var b: Vector3 = level.waypoint_positions[(wi + 1) % n]

        var gate := LevelGate.new()
        gate.position = a
        # Godot's forward is -Z, so a direction (dx, dz) becomes
        # atan2(-dx, -dz). The C engine's zero yaw faces +Z and writes
        # atan2(dx, dz); the two differ by exactly pi, and chapter 05 traces
        # why that is the only difference between the two conventions.
        gate.yaw = atan2(-(b.x - a.x), -(b.z - a.z))
        gate.width = level.waypoint_widths[wi] * 2.0
        level.checkpoints.append(gate)
```

The `@warning_ignore("integer_division")` is doing real work: `i * n / auto_count`
must floor, and chapter 02 turned that warning on precisely so that every
deliberate use is marked. The C version's concern — casting to 64-bit before
multiplying so `i * count` cannot overflow — does not apply, because GDScript's
`int` is already 64-bit.

---

## Point-in-box, and the convention that must not drift

```gdscript
# scripts/data/level_data.gd
## True when (x, z) is inside any run-off trap. Linear over the list: a
## circuit carries a few dozen boxes and only cars that have already left the
## tarmac ever ask — see race_director.gd, where the `and` short-circuits.
func in_sandtrap(x: float, z: float) -> bool:
    for trap: LevelBox in sandtraps:
        var c: float = cos(trap.yaw)
        var s: float = sin(trap.yaw)
        var dx: float = x - trap.center.x
        var dz: float = z - trap.center.z
        # Rotate the point into the box's frame, then do an axis-aligned test.
        if absf(dx * c - dz * s) <= trap.half_extents.x \
                and absf(dx * s + dz * c) <= trap.half_extents.y:
            return true
    return false
```

The C series notes that this rotation convention is implemented three times — in
`level.c`, in `collide.c` and in the Python exporter — because the layering
forbids `level.h` from including `collide.h`, and it pins them together with
comments because duplicated conventions drift.

In Godot it is implemented twice: here, and in the Python exporter. Chapter 07's
collision code uses Godot's own `Shape3D` for the solid boxes, which has its own
convention (a `BoxShape3D` inside a `Transform3D`, no yaw scalar anywhere). So
the drift risk is not lower, it has just moved: the thing that can now disagree
is *a yaw scalar in the data* versus *a basis in a transform*. Chapter 05
converts between them in exactly one function, for exactly this reason.

**The linear scan** deserves the same defence it gets in C. 78 traps × 6 cars ×
120 Hz would be 56,000 box tests a second if every car asked every tick. They do
not, because `race_director.gd` reads:

```gdscript
var on_track: bool = absf(query.lateral) <= query.half_width
# Only a car that has already left the tarmac can be in the gravel, so the
# lane decides drivability and the trap list is consulted second — which keeps
# the scan off the hot path for a whole field that is where it should be.
var in_sand: bool = not on_track and _level.in_sandtrap(pos.x, pos.z)
```

In a normal racing lap, `in_sandtrap` is called zero times per tick. Choosing
*where* an algorithm runs is often a better optimisation than improving the
algorithm — and chapter 16 revisits this one when deciding what to move to C.

---

## When to skip all of this

Three cases where the import pipeline above is the wrong answer:

**Levels authored in the Godot editor.** If the circuit is a `.tscn` you built
by dragging nodes around, `LevelData` is redundant: the scene *is* the level and
`Node3D` transforms are the data. Chapter 05 covers this path, which is what you
would do in a Godot-only project.

**User-generated content loaded at runtime.** `.tres` and `.res` can execute
code paths you do not control when they reference scripts, so loading one a
player downloaded is a security problem. JSON parsed by the tolerant loader
above is data and only data. If players share circuits, ship the JSON path in
the game, not just the importer.

**Anything you need to hand-edit while testing a hypothesis.** The C series
makes this point and it is still true: being able to open a level, change one
number, and reload is worth real money during tuning. Keep the runtime JSON
loader working even if the shipped path is the imported resource, and let a
command-line flag choose:

```gdscript
# scripts/main.gd
func _resolve_level(name: String) -> LevelData:
    # --level-json levels/circuit01.track reloads from source, uncached, so a
    # tuning change is one keypress away from being on screen.
    var override: String = _cli_value("--level-json")
    if not override.is_empty():
        var text: String = FileAccess.get_file_as_string(override)
        return LevelJson.to_level_data(JSON.parse_string(text) as Dictionary,
                override, 12)
    return load("res://levels/%s.track" % name) as LevelData
```

---

## Exercises

1. **Measure the two storage strategies.** Build a `LevelData` with props as
   `Array[LevelProp]` records instead of packed arrays. Compare: `.tres` file
   size, load time (`Time.get_ticks_usec()` around `ResourceLoader.load`), and
   `Performance.OBJECT_COUNT` after loading. Use `circuit02` — 1,237 props is
   where the difference shows.

2. **Break the format check.** Change `"format"` in a `.track` file to
   `"something-else"` and re-import. Where does the warning appear, and does the
   level still load? Now delete the `"waypoints"` array. What is different about
   how that failure is reported, and why is that the right distinction?

3. **Round-trip it.** Write the inverse of `LevelJson.to_level_data`: a `@tool`
   script that takes a `LevelData` and writes `.track` JSON. Import
   `circuit01.track`, export it again, and diff. Every difference is either a
   float formatting artefact or a bug — which are which?

4. **Comments, recovered.** `.tres` cannot hold comments. Add a
   `@export_multiline var notes: String` to `LevelData` and populate it from a
   `"notes"` key in the JSON. Is that better or worse than the C format's
   inline `//` comments, and for whom?

5. **The security exercise.** Write a `.tres` that references a script, and load
   it with `ResourceLoader.load`. Then load the same data through the JSON path.
   Explain, concretely, what a malicious circuit could do in each case.

---

Next: [05 — The Blender pipeline](05-blender-pipeline.md)
