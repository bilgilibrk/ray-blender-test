# 04 — Level format and the Blender pipeline

> `engine/include/engine/level.h` · `engine/src/level.c` ·
> `tools/blender/io_kenney_racing.py` · `tools/blender/build_demo_track.py`

---

## The problem

You have a game engine and no content. Somebody has to place several hundred
road tiles, barriers, trees and grandstands, draw a racing line, mark a starting
grid, and light the scene. Doing that in code is miserable — you cannot see what
you are doing. Doing it in a bespoke editor means writing an editor first.

So: **use a tool that already exists.** Blender is a free, universal 3D editor
with a Python API and a viewport that can show exactly the geometry the game
will render. Turn it into the level editor by writing an add-on.

This chapter is about the whole chain:

```
Kenney .glb models
        │
        ├──────────────────────────────► loaded by the engine at runtime
        │                                (assets/models/*.glb)
        ▼
Blender scene  ──(io_kenney_racing.py)──►  levels/*.level.json
   (levels/*.blend)                              │
        ▲                                        ▼
        └──(build_demo_track.py generates)──  LevelLoad()  ──► Level struct
```

The key property: **Blender and the engine load the same `.glb` files.** The
viewport is not an approximation of the game; it is the same geometry with
different lighting. What you place is what ships.

---

## The data model

Start with what the engine actually wants, in `engine/include/engine/level.h`:

```c
// engine/include/engine/level.h
typedef struct Level {
    char name[LEVEL_NAME_MAX];
    int laps;
    Color skyColor;
    Color groundColor;
    Vector3 sunDirection;
    Color sunColor;
    float sunIntensity;
    Color ambientColor;
    float defaultTrackWidth;

    LevelProp *props;             int propCount;
    LevelCollider *colliders;     int colliderCount;
    LevelSandtrap *sandtraps;     int sandtrapCount;
    LevelSpawn *spawns;           int spawnCount;
    LevelWaypoint *waypoints;     int waypointCount;
    LevelCheckpoint *checkpoints; int checkpointCount;
    LevelLight *lights;           int lightCount;

    Arena arena;                  // owns every array above
} Level;
```

Seven parallel arrays of plain structs and one arena that owns them all. There
is no scene graph, no entity hierarchy, no component system. Why not?

Because **nothing in this game needs one.** A prop never moves. A collider never
parents to another collider. There is no "attach this trailer to that car"
relationship anywhere. A hierarchy would be machinery with no user.

This is worth internalising: the fashionable general-purpose architectures
(scene graphs, ECS) solve problems of dynamism and composition. If your content
is static, arrays of structs are not a simplification — they are the correct
data structure, and they iterate faster.

### The separation of concerns inside a level

The single most important design decision in this format is that **art,
solidity, drivability and run-off are four independent lists.**

| Concept | List | What it means |
|---|---|---|
| Art | `props` | A `.glb` drawn at a transform. Purely visual. |
| Solidity | `colliders` | A yawed box you cannot drive through. Invisible. |
| Drivability | `waypoints` | The centre line plus a width. Inside it is tarmac. |
| Run-off | `sandtraps` | A yawed box that is slow but not solid. Invisible. |
| Progress | `checkpoints` | Ordered gates. |
| Grid | `spawns` | Where cars start. |
| Light | `lights` | Point and spot lamps. |

None is derived from another. That has consequences in both directions.

**What it buys.** You can put a road tile down without it being drivable. You
can make an invisible wall (a collider with no prop). You can make gravel that
is drawn one way and felt another. You can widen the track without moving a
single tile. Each concern is authored where it is most convenient.

**What it costs.** The four can disagree. A barrier can end up on the racing
line. A sand trap can be drawn where no box is. The art can say "corner" where
the waypoints say "straight".

The project's answer to that cost is *not* to derive one from another. It is to
**test the relationship**. From `tests/test_race.c`:

```c
// No collider should sit on the racing surface: that would wall off the track.
CHECK(blocking == 0, "%d collider(s) intrude on the racing surface", blocking);

CHECK(onLine == 0, "%d centre-line sample(s) sit in a sand trap", onLine);
CHECK(adrift == 0, "%d sand trap(s) are nowhere near the track", adrift);
CHECK(unreachable == 0, "%d sand trap(s) cannot be reached by running wide", unreachable);
CHECK(solidInTrap == 0, "%d collider(s) stand inside a sand trap", solidInTrap);
CHECK(sandPieces > 0, "%d sand traps but no sand drawn anywhere", level.sandtrapCount);
```

The `level.h` comment states this explicitly:

> A trap is drawn by the kit's sand pieces and felt through these boxes. The two
> are authored together and neither is derived from the other, so art and
> physics can disagree; the level tests are what keep them honest.

That is a general pattern worth naming: **when two representations of the same
thing must stay consistent, and unifying them would cost you flexibility you
need, make the consistency a tested invariant instead.** Chapter 14 goes deeper.

---

## The file format

```jsonc
{
  "format": "kenney-topdown-racer",
  "version": 1,
  "name": "Ardennes Circuit",
  "settings": {
    "laps": 3,
    "track_width": 0.69,          // drivable width, world units
    "sky_color":     [124, 176, 214, 255],
    "ground_color":  [77, 143, 110, 255],
    "ambient_color": [88, 90, 100, 255],
    "sun_direction": [-0.45, -1.0, -0.35],
    "sun_color":     [255, 250, 235, 255],
    "sun_intensity": 0.62
  },
  "props": [
    { "model": "roadStraight",     // assets/models/<model>.glb
      "pos":   [0, 0, 0],
      "rot":   [0, 90, 0],         // optional, XYZ euler degrees (Rx·Ry·Rz)
      "scale": [1, 1, 1],          // optional
      "tint":  [255, 255, 255, 255] }
  ],
  "colliders":  [ { "pos": [0, 0.06, 0], "half": [0.125, 0.06],
                    "height": 0.13, "yaw": 90 } ],
  "sandtraps":  [ { "pos": [3.7, 0, 12.1], "half": [0.265, 0.296], "yaw": 22.5 } ],
  "lights":     [ { "type": "spot", "pos": [-1.4, 2.3, 8.5], "dir": [1, -1.5, 0],
                    "color": [255, 242, 217, 255], "intensity": 3.4, "range": 5.2,
                    "cone": [23.65, 43.0] } ],
  "spawns":     [ { "pos": [0.15, 0, 6], "yaw": 0 } ],
  "waypoints":  [ { "pos": [0.15, -1.5, 0], "width": 0.69 } ],
  "checkpoints":[ { "pos": [0.15, 0, 8], "yaw": 0, "width": 1.1 } ]
}
```

The two shipping circuits, for scale:

| | props | colliders | sand traps | waypoints | gates | lights |
|---|---|---|---|---|---|---|
| `circuit01` (Ardennes) | 732 | 182 | 36 | 212 | 12 | 15 |
| `circuit02` (Eifel) | 1237 | 300 | 78 | 489 | 12 | 32 |

### Format design choices

**Text, not binary.** You can `git diff` a level. You can hand-edit one to test
a hypothesis. You can `grep` for a model name. The parse cost — a few
milliseconds for a 250 KB file — is irrelevant at load time. Binary formats earn
their keep when you are streaming hundreds of megabytes; a racing circuit is not
that.

**A `format` string and a `version` number.** The loader checks the first and
*warns* rather than fails:

```c
// engine/src/level.c
const char *format = JsonStringField(root, "format", NULL);
if (format && strcmp(format, LEVEL_FORMAT_ID) != 0) {
    TraceLog(LOG_WARNING, "LEVEL: '%s' declares format '%s', expected '%s'",
             path, format, LEVEL_FORMAT_ID);
}
```

Note `JsonStringField(root, "format", NULL)` — a file with *no* format field is
accepted silently. The check exists to catch "you handed me the wrong kind of
JSON", not to gate-keep. Being loud but not fatal is usually the right stance
for a tool you also use by hand.

**Almost everything is optional.** From the README: *"Only `waypoints` (three or
more) is truly required."* A level with nothing but a waypoint ring loads,
builds a spline, auto-generates checkpoints, and can be driven — invisible, but
driveable. `tests/test_spline.c` exploits exactly this to build test levels in
six lines with no file at all.

---

## The loader

`LevelLoad` in `engine/src/level.c` is 200 lines of very flat code. Its shape:

```c
bool LevelLoad(Level *level, const char *path)
{
    memset(level, 0, sizeof(*level));                    // 1. zero

    unsigned char *text = LoadFileData(path, &fileSize); // 2. read
    if (!text || fileSize <= 0) { /* error */ return false; }

    Arena scratch = { 0 };                               // 3. parse into scratch
    ArenaInit(&scratch, fileSize * 12 + (1u << 20), "level-scratch");
    JsonValue *root = JsonParse(&scratch, text, fileSize, err, sizeof err);
    UnloadFileData(text);
    if (!root || root->type != JSON_OBJECT) { /* error */ return false; }

    ArenaInit(&level->arena, EstimateLevelBytes(root, autoChecks), "level");  // 4. size + alloc

    /* 5. copy each array out of the tree into the level arena */

    ArenaFree(&scratch);                                 // 6. drop the tree
    return true;
}
```

Chapter 02 covered the two-arena part. Three loader details are worth their own
look.

### Skipping bad props without leaving a hole

```c
level->propCount = JsonCount(jProps);
if (level->propCount > 0) {
    level->props = ArenaAlloc(&level->arena, sizeof(LevelProp) * (size_t)level->propCount);
}
int written = 0;
for (int i = 0; i < level->propCount; i++) {
    const JsonValue *o = JsonAt(jProps, i);
    const char *model = JsonStringField(o, "model", NULL);
    if (!model || !model[0]) {
        TraceLog(LOG_WARNING, "LEVEL: prop %d has no model, skipped", i);
        continue;
    }
    LevelProp *p = &level->props[written++];
    /* ... fill ... */
}
level->propCount = written;
```

Allocate for the optimistic count, fill with a separate `written` cursor, then
correct the count. The array may have unused tail slots — a few hundred bytes of
arena, reclaimed with everything else. The alternative (two passes to count
valid props first) would double the JSON walking to save nothing.

Note the log line. A silently dropped prop is a mystery; a warned one is a bug
report.

### Numbers a float cannot hold

The last thing `LevelLoad` does before reporting success is sweep everything it
just built:

```c
static bool LevelCheckNumbers(const Level *level)
{
    /* ... every position, extent, angle, width, intensity and range ... */
    for (int i = 0; i < level->waypointCount; i++) {
        if (!Vec3Finite(level->waypoints[i].position) || !isfinite(level->waypoints[i].width)) {
            TraceLog(LOG_ERROR, "LEVEL: waypoint %d has a coordinate that is not a number", i);
            return false;
        }
    }
    /* ... */
}
```

This looks like belt-and-braces over a parser that already refuses numbers it
cannot represent (Chapter 03), and it is not. The parser works in `double`.
These structs hold `float`. **`1e300` is well-formed JSON, an entirely ordinary
`double`, and an infinity in a `float`** — so it passes every check the parser
can reasonably make and still arrives here broken.

The reason to catch it *here*, rather than let it go, is how far it travels
otherwise. One infinite waypoint gives the spline a bounding box of
`[-inf, +inf]`; two thirds of its samples come out non-finite; the lap length
becomes NaN; and the terrain build, which derives its grid dimensions from that
box, segfaults. The backtrace points at `TerrainBuild` and says nothing at all
about a level file, which is where the mistake actually is.

The cost is one pass over a few thousand floats at load. The benefit is an error
message with an index in it.

### Auto-generated checkpoints

If a level ships no gates, the loader manufactures twelve:

```c
} else if (autoChecks > 0) {
    // Spread gates evenly along the centreline and face each one down-track.
    for (int i = 0; i < autoChecks; i++) {
        int wi = (int)((long long)i * waypointCount / autoChecks);
        int next = (wi + 1) % waypointCount;
        Vector3 a = level->waypoints[wi].position;
        Vector3 b = level->waypoints[next].position;
        level->checkpoints[i].position = a;
        level->checkpoints[i].yawDeg = atan2f(b.x - a.x, b.z - a.z) * RAD2DEG;
        level->checkpoints[i].width = level->waypoints[wi].width * 2.0f;
    }
}
```

Two things to notice.

`(long long)i * waypointCount / autoChecks` casts to 64-bit *before* multiplying.
With 489 waypoints and 12 gates the product is trivially small, but the habit
matters: `i * count` overflowing a 32-bit int is a classic and silent bug in
index arithmetic.

`atan2f(b.x - a.x, b.z - a.z)` — note the argument order. The usual `atan2(y, x)`
gives an angle from the +X axis. Here it is `atan2(dx, dz)`, giving an angle from
the **+Z** axis, because that is the engine's zero yaw. Whenever you see
`atan2f(something.x, something.z)` in this codebase, it is converting a direction
into engine yaw. It appears in `RaceRespawn`, in the spawn-facing test, and here.

### Point-in-box, matching an invariant by hand

```c
bool LevelInSandtrap(const Level *level, float x, float z)
{
    for (int i = 0; i < level->sandtrapCount; i++) {
        const LevelSandtrap *s = &level->sandtraps[i];
        // Into the box's own frame. The axes are the ones Obb2 uses — yaw about
        // +Y on the engine's left-handed XZ plane — so a trap and a collider
        // written with the same yaw cover the same ground.
        float c = cosf(s->yawDeg * DEG2RAD);
        float sn = sinf(s->yawDeg * DEG2RAD);
        float dx = x - s->center.x;
        float dz = z - s->center.z;
        float localX = dx * c - dz * sn;
        float localZ = dx * sn + dz * c;
        if (fabsf(localX) <= s->halfExtents.x && fabsf(localZ) <= s->halfExtents.y) return true;
    }
    return false;
}
```

The technique — rotate the *point* into the box's frame, then do an
axis-aligned test — is the standard way to test a point against an oriented box,
and you will see it again in `Obb2ContainsPoint` in Chapter 06.

The comment is the important part. This function duplicates the rotation
convention rather than calling `Obb2ContainsPoint`, because `level.h` must not
depend on `collide.h` (see the layering rules in Chapter 01). Duplicated
conventions drift. So the comment pins it, and the same convention is
*re-implemented a third time* in Python:

```python
# tools/blender/io_kenney_racing.py
def box_contains(entry, point):
    """Point-in-box on the engine's XZ plane. Mirrors LevelInSandtrap()."""
    theta = math.radians(entry["yaw"])
    c, s = math.cos(theta), math.sin(theta)
    dx = point[0] - entry["pos"][0]
    dz = point[1] - entry["pos"][2]
    return (abs(dx * c - dz * s) <= entry["half"][0] and
            abs(dx * s + dz * c) <= entry["half"][1])
```

Three implementations of one convention, in two languages. This is a real cost
of the layering, honestly paid: each is documented as mirroring the others, and
the level tests would catch a divergence because they exercise both the C and
the Python side against the same shipped level.

**The linear scan** deserves a defence, since it is O(traps) per query with 78
traps on circuit02. The header explains:

> Linear over the list: a circuit carries a few dozen boxes and only cars that
> are already off the tarmac ever ask.

And `race.c` enforces that second clause:

```c
// game/src/race.c
bool onTrack = fabsf(q.lateral) <= q.halfWidth;
// Only a car that has already left the tarmac can be in the gravel, so
// the lane is what decides drivability and the trap list is consulted
// second — which also keeps the scan off the hot path for a whole field
// that is where it should be.
bool inSand = !onTrack && LevelInSandtrap(race->level, ...);
```

`&&` short-circuits. In a normal racing lap, `LevelInSandtrap` is called zero
times per tick. The "slow" algorithm runs only in the situation where it does
not matter. Choosing where an algorithm runs is often a better optimisation than
improving the algorithm.

---

## Coordinate spaces: Blender vs the engine

This is where content pipelines go to die, so it is worth being precise.

**Blender is Z-up, right-handed.** X right, Y forward (into the screen), Z up.

**glTF — and therefore the engine — is Y-up.** X right, Y up, Z toward the
viewer.

The conversion is a rotation of −90° about X:

```
Blender (x, y, z)  →  engine (x, z, −y)
```

Implemented twice, once for points and once for bases:

```python
# tools/blender/io_kenney_racing.py
BLENDER_TO_ENGINE = Matrix(((1, 0, 0), (0, 0, 1), (0, -1, 0)))

def to_engine_point(v):
    return [round(v.x, 5), round(v.z, 5), round(-v.y, 5)]
```

For a rotation *basis*, a change of coordinates is a similarity transform:

```python
engine_basis = BLENDER_TO_ENGINE @ basis @ BLENDER_TO_ENGINE.transposed()
```

If `C` maps Blender coordinates to engine coordinates and `M` is a rotation
expressed in Blender's frame, then the same rotation in the engine's frame is
`C·M·Cᵀ`. (`Cᵀ` rather than `C⁻¹` because `C` is orthogonal, so they are equal.)
The intuition: to apply `M` to an engine vector, first convert it back to
Blender space (`Cᵀ`), rotate (`M`), then convert forward again (`C`).

Getting this wrong is the single most common bug in an exporter, and it usually
manifests as "everything is mirrored" or "rotations go the wrong way only for
some objects".

### Euler extraction, and why it needs a special case

The engine's `LevelProp.rotationDeg` is an XYZ euler triple, applied as
`Rx·Ry·Rz` (matching raymath's `MatrixRotateXYZ`). Converting a rotation matrix
back to euler angles is standard but fiddly:

```python
def decompose_engine(matrix_world):
    position = to_engine_point(matrix_world.translation)
    basis = matrix_world.to_3x3()
    engine_basis = BLENDER_TO_ENGINE @ basis @ BLENDER_TO_ENGINE.transposed()

    scale = [engine_basis.col[i].length for i in range(3)]
    rot = engine_basis.copy()
    for i in range(3):
        if scale[i] > 1e-9:
            rot.col[i] = engine_basis.col[i] / scale[i]

    # Guard against a mirrored (negative determinant) basis, which euler
    # extraction cannot represent; flip X and record it in the scale.
    if rot.determinant() < 0.0:
        rot.col[0] = -rot.col[0]
        scale[0] = -scale[0]

    if _is_pure_yaw(rot):
        # The overwhelmingly common case for a top-down track. Handled directly
        # because the generic asin branch below picks the wrong solution at
        # exactly 180 degrees, turning a half-turn into [180, 0, 180].
        x, y, z = 0.0, math.atan2(rot[0][2], rot[0][0]), 0.0
    else:
        sy = max(-1.0, min(1.0, rot[0][2]))
        y = math.asin(sy)
        if abs(math.cos(y)) > 1e-6:
            x = math.atan2(-rot[1][2], rot[2][2])
            z = math.atan2(-rot[0][1], rot[0][0])
        else:
            # Gimbal lock: only x+z is observable, so pin z and fold it into x.
            x = math.atan2(rot[1][0], rot[1][1])
            z = 0.0
    /* ... */
```

Four separate hazards handled in twenty lines:

1. **Scale must come out first.** A basis with scale baked in is not a rotation
   matrix, and `asin` of one of its elements is meaningless. Normalising each
   column extracts the scale and leaves the rotation.

2. **Negative determinant means a mirror.** A mirrored transform is not a
   rotation at all — no euler triple represents it. Flipping one axis and
   recording a negative scale expresses the same overall transform in a form the
   engine can apply.

3. **Pure yaw is special-cased.** Almost every object in a top-down racer is
   rotated only about the up axis. The generic branch is correct for those in
   principle, but at exactly 180° it picks a valid-but-ugly solution:
   `[180, 0, 180]` rotates the same way as `[0, 180, 0]` but produces
   floating-point noise and unreadable level files. Special-casing the common
   path makes 90% of the exported rotations exactly `[0, 90, 0]`.

4. **Gimbal lock.** When `cos(y) ≈ 0` — pitch at ±90° — the X and Z rotations
   become the same rotation, and only their sum is recoverable. The code pins
   `z = 0` and folds everything into `x`, which is the conventional resolution.

This is the standard argument for storing quaternions instead. The format uses
eulers because a human reading `"rot": [0, 90, 0]` in a level file immediately
knows what it means, and `[0, 0.707, 0, 0.707]` does not. That readability is
worth twenty lines of extraction code paid once in the exporter.

### Yaw measured from a direction, not from an euler

For spawns and checkpoints, the exporter refuses to use euler decomposition at
all:

```python
def engine_yaw_degrees(obj):
    """Yaw about the engine's +Y axis, in degrees.

    Measured from where the object's forward axis actually points rather than
    from an euler triple: euler decomposition has several valid answers for the
    same orientation, and picking the wrong one would silently spin spawn and
    checkpoint gates. The engine's +Z is Blender's -Y.
    """
    forward = obj.matrix_world.to_3x3() @ Vector((0.0, -1.0, 0.0))
    return math.degrees(math.atan2(forward.x, -forward.y))
```

Take the object's forward vector, transform it, and read its angle. There is
only one answer. This is more robust than any decomposition, and it is the right
technique whenever you need one angle out of a full orientation.

Again note `atan2(x, ...)` rather than `atan2(y, x)` — measuring from +Z.

### The origin-offset wrinkle

From the README, and it will bite you if you author a track by hand:

> **Kit models do not have their origins at the centre of their geometry.**
> Kenney's exporter baked a per-model offset into each glTF node — road tiles,
> for instance, put their cell corner a constant `(-0.35, -0.65)` from the
> origin. This cannot be fixed by re-centring meshes in Blender, because the
> engine loads the original `.glb`; `build_demo_track.py` compensates at
> placement time instead (see `place_centred`).

```python
# tools/blender/io_kenney_racing.py
# The kit's tiles all share this cell origin offset, in Blender XY.
GRID_OFFSET = (-0.35, 0.65)
GRID_SIZE = 1.0
```

The general lesson: **third-party assets have conventions you did not choose.**
You can fight them (re-export everything, and now your pipeline has a
preprocessing step and a divergence from upstream) or you can measure them once
and compensate at the point of use. This project measures.

`main.c` does the same for cars, whose origins are also not centred:

```c
// game/src/main.c
// Places a car model so its footprint centre sits on the car's position and its
// wheels rest on the ground, whatever origin the artist happened to use.
static void DrawRacer(const Racer *racer, float scale)
{
    BoundingBox bounds = AssetsGetModelBounds(racer->model);
    float cx = (bounds.min.x + bounds.max.x) * 0.5f;
    float cz = (bounds.min.z + bounds.max.z) * 0.5f;
    /* ... rotate that offset by the car's yaw and subtract it ... */
```

Measure the bounding box at load, derive the offset, apply it. Works for any
model from any artist.

---

## The Blender add-on

`tools/blender/io_kenney_racing.py` is 987 lines and does four jobs.

### 1. Tagging with custom properties

Blender lets you attach arbitrary key/value data to any object. The add-on uses
seven keys:

```python
PROP_PREFAB = "kr_prefab"      # str: kit model name, marks an object as a prop
PROP_TYPE = "kr_type"          # str: spawn | checkpoint | racingline | collider | sand
PROP_INDEX = "kr_index"        # int: ordering for spawns and checkpoints
PROP_WIDTH = "kr_width"        # float: gate or track width override
PROP_SOLID = "kr_solid"        # bool: emit a collider for this prop
PROP_INTENSITY = "kr_intensity"  # float: overrides the power-derived brightness
PROP_RANGE = "kr_range"          # float: overrides the light's reach
```

This is the whole trick that turns a general 3D editor into a level editor. You
do not need a custom file format inside Blender — you need a convention for
saying "this cube is a sand trap" that survives a save and a reload. Custom
properties are that convention, and they show up in Blender's own Object
Properties panel, so a user can inspect and fix them without your UI.

### 2. Placement operators

`Add Prefab` imports a `.glb`, applies its transform, snaps it to the kit grid.
`Rotate ⟳ / ⟲` turns by a quarter. `Add Spawn`, `Add Checkpoint`, `Add Sand Trap`
drop tagged empties. `Add Racing Line` creates a closed Bézier curve.

The snap is the interesting one:

```python
def snap_value(value, offset):
    return round((value - offset) / GRID_SIZE) * GRID_SIZE + offset
```

Snapping to a grid *offset* by the kit's baked-in origin, so a snapped tile
lands on a cell boundary rather than 0.35 units off it.

### 3. The export

`build_level_dict` walks the scene once and sorts objects into the seven output
lists by their `kr_type` / `kr_prefab` tags. Three parts are worth study.

**Auto-colliders by name prefix:**

```python
SOLID_PREFIXES = (
    "barrier", "fence", "rail", "tent", "grandStand", "pits", "tree",
    "lightPost", "lightRed", "lightColored", "bannerTower", "billboard",
    "radarEquipment", "pylon", "flag", "overhead", "camera",
)

def is_solid_prefab(name):
    return any(name.startswith(prefix) for prefix in SOLID_PREFIXES)
```

A heuristic, and an unashamed one. Anything whose kit name starts with
"barrier" gets a collider automatically; road tiles do not. `kr_solid` overrides
it either way. This is a good trade: it makes the common case zero-effort and
leaves an escape hatch. The failure mode (a new kit piece that should be solid
but is not in the list) is visible the first time you drive through a tent.

**The safety net that drops blocking colliders:**

```python
# Drop any collider that would sit on the racing surface. Scenery placement
# is fiddly on a circuit that runs close to itself, and one box on the line
# is enough to wedge the whole field; better to lose the collision than the
# race. The count is reported so the mistake is still visible.
blocked = 0
if waypoint_ring:
    kept = []
    for box in colliders:
        reach = math.hypot(box["half"][0], box["half"][1])
        if distance_to_ring((box["pos"][0], box["pos"][2]), waypoint_ring) - reach \
                < settings.track_width * 0.5 + 0.12:
            blocked += 1
            continue
        kept.append(box)
    colliders = kept
if blocked and report:
    report({"WARNING"}, f"{blocked} collider(s) dropped for intruding on the track")
```

This is the exporter enforcing at author time the same invariant
`tests/test_race.c` checks at test time — belt and braces, deliberately. The
comment states the priority ordering: a missing collision is a cosmetic bug, a
collider on the racing line is a race-ending one.

Note it warns rather than silently fixing. Silent auto-repair is how you end up
with a level that works only because a tool keeps rescuing it.

**Light conversion, with unit translation:**

```python
def light_to_dict(obj):
    data = obj.data
    if data.type == "SUN":
        return None     # handled as the level's directional light instead

    intensity = obj.get(PROP_INTENSITY)
    if intensity is None:
        intensity = max(data.energy, 0.0) / LIGHT_WATTS_PER_UNIT   # 100.0

    reach = obj.get(PROP_RANGE)
    if reach is None:
        reach = data.cutoff_distance if getattr(data, "use_custom_distance", False) \
            else LIGHT_DEFAULT_RANGE

    entry = { "type": "spot" if data.type == "SPOT" else "point", /* ... */ }
    if data.type == "SPOT":
        # spot_size is the full cone angle; the engine wants half-angles, and
        # spot_blend is the fraction of the cone taken up by the soft edge.
        outer = math.degrees(data.spot_size) * 0.5
        inner = outer * (1.0 - data.spot_blend)
        entry["dir"] = light_beam_direction(obj)
        entry["cone"] = [round(inner, 3), round(outer, 3)]
    return entry
```

Blender speaks watts and full cone angles; the engine speaks unitless intensity
and half-angles. Somebody has to translate, and the exporter is the right place
— it keeps Blender's own UI (Power, Spot Size, Blend) as the authoring controls,
which is exactly what a user of Blender expects to work.

A `SUN` lamp becomes the level's key light rather than a placed light. One scene
concept, two different destinations, because the engine treats directional light
completely differently from point lights (Chapter 11).

### 4. Validation

```python
class KR_OT_validate(Operator):
    def execute(self, context):
        data = build_level_dict(context)
        problems = []
        if not data["waypoints"]:
            problems.append("no racing line")
        elif len(data["waypoints"]) < 3:
            problems.append("racing line has fewer than 3 points")
        if not data["spawns"]:
            problems.append("no spawn points")
        if not data["props"]:
            problems.append("no track pieces")
        /* ... report ... */
```

Cheap, and it turns "the level loads but the game crashes" into "the button said
no racing line". Every content pipeline should have this button.

---

## Generating a circuit from a list of moves

`tools/blender/build_demo_track.py` builds both shipping circuits from scratch,
headlessly:

```sh
make level      # blender --background --python tools/blender/build_demo_track.py
```

A circuit is a list of moves:

```python
TRACK_ARDENNES = [
    ("s", 13, 0.00, "Start/finish straight"),
    ("t", "E", 2, 0.0, "La Source (entry)"),
    ("s", 1, 0.00, ""),
    ("t", "S", 2, 0.0, "La Source (exit)"),
    ("s", 7, -1.25, "Plunge towards the compression"),
    ("t", "E", 2, 0.0, "Eau Rouge (left)"),
    ("s", 2, -0.25, "Compression"),
    ("t", "S", 2, 0.0, "Eau Rouge (right)"),
    ("s", 10, 1.70, "Raidillon climb"),
    /* ... */
    ("s", None, 0.00, "Blanchimont"),
    ("t", "N", 2, 0.0, "Bus stop (left)"),
    ("s", None, 0.00, "Run to the line"),
]
```

`("s", tiles, rise, label)` is a straight; `("t", heading, cells, rise, label)`
is a corner into a new compass heading. This is a turtle-graphics program, and
it is *legible* — you can read the Ardennes layout as a description of a lap.

### Closing the loop

Two straights are left as `None` and solved:

```python
def solve_track(moves):
    """Fill in the two unspecified straight lengths so the walk closes.

    Walks the moves with the unknowns at zero, then cancels the leftover
    displacement using the two adjustable straights. They must lie on
    perpendicular headings for the residual to be separable.
    """
```

The maths is a two-variable linear system that happens to be trivial because the
headings are axis-aligned. Walk the loop with the unknowns at zero; you end up
at some position `pos` instead of the origin. The residual `-pos` must be
cancelled by the two adjustable straights, each of which contributes
`length × heading`. Because the headings are perpendicular, projecting the
residual onto each gives its length directly:

```python
residual = (-pos[0], -pos[1])
length_a = residual[0] * dir_a[0] + residual[1] * dir_a[1]
length_b = residual[0] * dir_b[0] + residual[1] * dir_b[1]
```

Then three assertions that are the real value of the function:

```python
if heading != HEADINGS["N"]:
    raise RuntimeError(f"track ends heading {heading}, expected north")
if abs(turns) != 4:
    raise RuntimeError(f"net rotation is {turns * 90} degrees, a simple loop needs +/-360")
/* ... */
if abs(length - tiles) > 1e-6:
    raise RuntimeError(f"adjustable straight {index} solved to {length:.3f}, not a whole "
                       f"number of tiles")
if tiles < 1:
    raise RuntimeError(f"adjustable straight {index} solved to {tiles} tiles; the layout "
                       f"does not leave room to close")
```

A generator that can produce a broken track is worse than no generator. These
turn "the circuit has a subtle gap somewhere" into a message naming the move.

The `abs(turns) != 4` check is a small piece of topology: a simple closed loop
made of right-angle turns has a total turning number of exactly ±360°. If your
layout doubles back on itself you get ±0 or ±720, and the check catches it
before anything is placed.

### Closing the elevation

The same idea in one dimension:

```python
def balance_elevation(moves):
    """Spread any leftover height across the straights so the loop closes."""
    total_rise = sum(m[2] if m[0] == "s" else m[3] for m in moves)
    tiles = sum(m[1] for m in moves if m[0] == "s")
    if abs(total_rise) < 1e-9 or tiles == 0:
        return moves

    per_tile = total_rise / tiles
    print(f"[solve] elevation was {total_rise:+.3f} over the lap; "
          f"trimming {per_tile:+.4f} per tile to close it")
```

You cannot author a lap that climbs and never comes back down — it must return
to its start height. Rather than making the author balance the numbers by hand,
the generator distributes the error evenly across every straight tile. An author
writes the *character* of each section (Eau Rouge plunges, Raidillon climbs) and
the tool makes it consistent.

Corners stay level, and the README explains why:

> A quarter arc cannot be pitched about a single axis without twisting it, so
> gradients live on the straights and the track crests before turning in.

Which is also, conveniently, how a lot of real circuits feel.

### Verification passes

After building, the script checks its own work:

```python
def verify_line_on_tiles(centre_line, tiles_placed):   # every centre-line point is on a road tile
def verify_tile_heights(straight_cells, centre_line):  # tiles match the line's elevation
def verify_loop_clearance(centre_line):                # the loop does not touch itself
```

The last one matters more than it sounds. A track that runs within `LOOP_CLEARANCE`
(1.30 units) of itself would let the spline's nearest-point query snap to the
wrong side of the circuit, and a car would teleport a lap forward. The generator
refuses to emit such a layout. Chapter 05 covers the runtime half of that defence.

---

## Exercises

1. **Hand-write a level.** Create `levels/tiny.level.json` with only a
   `waypoints` array — four points in a square, 20 units on a side. Run
   `./build/desktop/racer --level levels/tiny.level.json --racers 2`. It should
   drive. How many checkpoints did the loader generate, and where?

2. **Break the format check.** Change `"format"` in `circuit01.level.json` to
   `"something-else"`. What happens, and where in `level.c` does it happen? Now
   delete the field entirely. Explain the difference.

3. **Trace one prop.** Pick any prop in `circuit01.level.json`. Find which
   Blender object produced it (open `levels/circuit01.blend`, or read
   `build_demo_track.py`), then find where its `.glb` gets loaded
   (`assets.c`), and where its triangles end up (`render.c`,
   `StaticBatchBuild`).

4. **Add a level field.** Add `"fog_density"` to `settings`, read it in
   `LevelLoad` into a new `Level` field, and use it in
   `RenderDefaultSettings`. Which four files do you touch? Does
   `EstimateLevelBytes` need changing, and why not?

5. **Find a violated invariant.** Edit `circuit01.level.json` to move one
   collider onto the racing line — take a barrier's `pos` and set it to a
   nearby waypoint's `pos`. Run `make test`. Read the failure message. Now do
   the same with a sand trap.

6. **Export by hand.** Write a Python script (no Blender) that reads
   `circuit01.level.json`, translates every `pos` by `[0, 0, 5]`, and writes
   `circuit01-shifted.level.json`. Does it still pass `make test` if you add it
   to `kCircuits`? Which invariant breaks first, and why?

---

Next: [05 — Splines and arc length](05-splines.md)
