# 13 — Terrain from a racing line

> `scripts/engine/terrain_builder.gd` · `shaders/kit_relief.gdshader`.
> Mirrors [C chapter 12](../learn/12-terrain.md).

---

## The problem

A flat ground plane works right up until the circuit has hills. Then the road
either floats above it — you can see under the tarmac — or sinks through it, and
the horizon is a hard line where the plane ends.

You could model the ground in Blender. For a circuit with 18% climbs over a
120-unit loop, that is a lot of hand-sculpting, and every time the racing line
moves the ground has to move with it.

So: **derive the ground from the racing line.** The spline already knows the
height of the track everywhere. Fit a heightfield to it that hugs the road
closely and relaxes to the average height out in the open, and the ground can
never disagree with the tarmac because it is computed from it.

---

## Inverse-distance weighting

Given a query point `(x, z)` and a set of samples with known heights, produce a
height that:

- equals the sample height when you are on top of a sample;
- blends smoothly between samples;
- has **no creases** where the nearest sample changes;
- relaxes to something sensible far from every sample.

Inverse-distance weighting does all four. Weight each sample by a falling
function of its distance, and take the weighted mean:

```gdscript
# scripts/engine/terrain_builder.gd
## Inverse-distance weighting against every spline sample. The 1/(d^4 + k)
## shape makes the ground hug the road closely and relax to the average height
## in the open, with no creases where the nearest sample changes.
static func height_from_spline(spline: TrackSpline, x: float, z: float) -> float:
    var weighted: float = 0.0
    var total: float = 0.0
    for i: int in spline.count:
        var p: Vector3 = spline.positions[i]
        var dx: float = x - p.x
        var dz: float = z - p.z
        var d2: float = dx * dx + dz * dz
        var w: float = 1.0 / (d2 * d2 + 0.45)
        weighted += p.y * w
        total += w
    return weighted / total if total > 0.0 else 0.0
```

Three constants, each doing a job:

**`d2 * d2` is `d⁴`,** not `d²`. The exponent controls how tightly the surface
hugs its samples. With `d²` the influence of distant samples falls off too
slowly and the ground under the road is dragged towards the average height of
the whole circuit — the track sinks into its own hillside. With `d⁴` the nearest
few samples dominate and the road sits on its own ridge.

**`+ 0.45` is the singularity guard.** At `d = 0` the raw weight is infinite. The
constant caps it at 1/0.45 ≈ 2.2 and, more usefully, sets the *radius at which
the weighting starts to care*: `d⁴ = 0.45` at `d ≈ 0.82` units, a bit over one
tile. Inside that radius a sample dominates; outside it, it is one voice among
many.

**No cutoff radius.** Every sample contributes to every query. That is what
guarantees no creases — a cutoff means a sample's contribution jumps to zero at
the boundary, and that discontinuity is visible as a ridge in the shading. It is
also, as the next section explains, why this function is the most expensive
thing in the project.

---

## The cost, and why it changes the design

Count the work for `circuit02`:

- Spline: ~4,000 samples.
- Ground: the circuit is about 120 × 100 units, plus a 9-unit margin each side,
  at a 0.4-unit cell — so roughly 345 × 295 = **102,000 grid samples**.
- Each grid sample loops over every spline sample.

**408 million iterations.** In C that is a second or two at level load, which
the C engine accepts. In GDScript, even fully typed, it is minutes. This is not
a "GDScript is a bit slower" problem; it is an "the approach does not survive
the port" problem.

There are four ways out, and the choice shapes the rest of the chapter.

| Option | Build time | Cost |
|---|---|---|
| **A. Bake at import time** into `LevelData` | Zero at runtime | The bake still has to happen once, somewhere |
| **B. Subsample the spline** — every 8th point | ~50M iterations | Ground is slightly coarser near the road; still slow in GDScript |
| **C. Spatial acceleration** — only sum samples within a radius | ~5M iterations | Reintroduces the creases the no-cutoff design avoids |
| **D. Move the kernel to C** (chapter 16) | ~1 s | A native build per platform |

This project uses **A, with D behind it**: the heightfield is baked into the
level resource by the importer, so the shipped game never runs the fit at all,
and the importer calls the C kernel so that a designer moving a waypoint sees
the ground update in a second rather than in a minute.

That is a genuinely different architecture from the C engine's, and it is the
right one for Godot for a reason worth stating generally:

> Derived data with a stable input belongs in the import step, not in the
> runtime. Godot's import pipeline exists precisely for this, and it turns "too
> slow to compute" into "computed once, on my machine, months ago".

Option B is worth knowing about even so, because during development you want a
version that runs without a native build:

```gdscript
## Every Nth spline sample. The fit barely changes — adjacent samples are 0.25
## units apart and the weighting radius is 0.82 — but the cost falls by the
## stride. 1 for a shipped bake, 8 while iterating without the native kernel.
@export_range(1, 16, 1) var spline_stride: int = 1
```

---

## Building the grid

```gdscript
# scripts/engine/terrain_builder.gd
class_name TerrainBuilder

## Cells per chunk edge. Small enough that light selection is local, large
## enough that the draw-call count stays modest (chapter 11).
const CHUNK_CELLS: int = 8

## Caps the grid so a level with a stray far-away waypoint cannot ask for
## hundreds of megabytes of ground.
const MAX_SAMPLES: int = 512 * 512

class Settings extends RefCounted:
    ## How far past the track the ground extends.
    var margin: float = 9.0
    ## Cells have to be comfortably finer than the road is wide. At 0.9 the
    ## ground was coarser than the 0.69 lane, so on a gradient the linear
    ## surface between samples crossed above the tarmac and ate the track.
    var cell_size: float = 0.4
    ## Pushes the surface under the road to avoid z-fighting with it.
    var sink_below_track: float = 0.06
    var color: Color = Color8(77, 143, 110)

static func build_heightfield(spline: TrackSpline, s: Settings) -> Heightfield:
    var lo := Vector2(INF, INF)
    var hi := Vector2(-INF, -INF)
    for i: int in spline.count:
        var p: Vector3 = spline.positions[i]
        lo = Vector2(minf(lo.x, p.x), minf(lo.y, p.z))
        hi = Vector2(maxf(hi.x, p.x), maxf(hi.y, p.z))
    lo -= Vector2(s.margin, s.margin)
    hi += Vector2(s.margin, s.margin)

    var field := Heightfield.new()
    field.origin = lo
    field.cell_size = s.cell_size
    field.grid_x = maxi(1, int(ceil((hi.x - lo.x) / field.cell_size)))
    field.grid_z = maxi(1, int(ceil((hi.y - lo.y) / field.cell_size)))

    # A stray waypoint at (0, 0, 1e6) would otherwise ask for a grid with more
    # samples than the machine has memory. Coarsen until it fits rather than
    # failing: a bad level should look wrong, not refuse to load.
    while (field.grid_x + 1) * (field.grid_z + 1) > MAX_SAMPLES:
        field.cell_size *= 2.0
        field.grid_x = field.grid_x / 2 + 1
        field.grid_z = field.grid_z / 2 + 1

    field.heights.resize((field.grid_x + 1) * (field.grid_z + 1))
    for iz: int in field.grid_z + 1:
        for ix: int in field.grid_x + 1:
            var x: float = lo.x + float(ix) * field.cell_size
            var z: float = lo.y + float(iz) * field.cell_size
            field.heights[iz * (field.grid_x + 1) + ix] = \
                    height_from_spline(spline, x, z) - s.sink_below_track
    return field
```

**`sink_below_track = 0.06`** is z-fighting insurance. The ground is fitted to
the road's own height, so without a bias the two surfaces are coplanar
everywhere the fit is exact, and the depth buffer picks a winner per pixel per
frame — which looks like the tarmac boiling. Six centimetres at this scale is
a fifth of a car's width: invisible from above, decisive for the depth test.

**The coarsening loop** is the kind of guard that only exists because somebody
shipped a level with a stray waypoint. Note that it degrades rather than fails:
a bad level produces coarse ground, loads, and looks wrong in a way that tells
you what happened.

---

## Normals

```gdscript
static func normal_at(field: Heightfield, ix: int, iz: int) -> Vector3:
    # Central differences on the height field. Cheaper and smoother than
    # taking the cross product of two triangle edges, and — because it uses
    # the grid rather than the mesh — it gives the same normal to the shared
    # corner of four cells, so the surface shades as one continuous sheet.
    var left: float = field.sample(ix - 1, iz)
    var right: float = field.sample(ix + 1, iz)
    var back: float = field.sample(ix, iz - 1)
    var front: float = field.sample(ix, iz + 1)
    var span: float = 2.0 * field.cell_size
    return Vector3((left - right) / span, 1.0, (back - front) / span).normalized()
```

`field.sample()` clamps its indices rather than wrapping or erroring, so the
edge of the grid produces a one-sided difference instead of a special case. The
`1.0` in the Y slot before normalising is the standard heightfield normal: the
surface is a graph `y = f(x, z)`, whose normal is `(-∂f/∂x, 1, -∂f/∂z)`.

---

## The mesh

Godot has two ways to build a mesh from code, and the difference matters at this
size.

**`SurfaceTool`** is the friendly one: `begin`, `set_normal`, `add_vertex`,
`commit`. It also allocates per vertex, deduplicates with a hash, and is
comfortably an order of magnitude slower than the alternative. Use it for a
dozen triangles, not for a hundred thousand.

**`ArrayMesh.add_surface_from_arrays`** takes flat packed arrays and hands them
straight to the rendering server:

```gdscript
static func build_chunk(field: Heightfield, x0: int, z0: int,
                        x1: int, z1: int) -> ArrayMesh:
    var cells: int = (x1 - x0) * (z1 - z0)
    if cells <= 0:
        return null

    # Two triangles per cell, non-indexed: adjacent cells do not share
    # vertices anyway once each carries its own relief colour, and an index
    # buffer that indexes nothing twice is pure overhead.
    var vertex_count: int = cells * 6
    var vertices := PackedVector3Array()
    var normals := PackedVector3Array()
    vertices.resize(vertex_count)
    normals.resize(vertex_count)

    const CORNER_X: PackedInt32Array = [0, 0, 1, 0, 1, 1]
    const CORNER_Z: PackedInt32Array = [0, 1, 1, 0, 1, 0]

    var out: int = 0
    for iz: int in range(z0, z1):
        for ix: int in range(x0, x1):
            for k: int in 6:
                var gx: int = ix + CORNER_X[k]
                var gz: int = iz + CORNER_Z[k]
                vertices[out] = field.vertex_at(gx, gz)
                normals[out] = normal_at(field, gx, gz)
                out += 1

    var arrays: Array = []
    arrays.resize(Mesh.ARRAY_MAX)
    arrays[Mesh.ARRAY_VERTEX] = vertices
    arrays[Mesh.ARRAY_NORMAL] = normals

    var mesh := ArrayMesh.new()
    mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
    return mesh
```

`arrays` must be sized to `Mesh.ARRAY_MAX` with unused slots left `null` — the
index of each slot is what identifies it. This is one of the few places in Godot
4 where an untyped `Array` is unavoidable, because the slots hold different
packed types; chapter 02's rule bends here and nowhere else in the file.

**Winding order** decides which side faces out. Godot uses clockwise winding
when viewed from the front. The `CORNER_X` / `CORNER_Z` tables above produce
upward-facing triangles for a Y-up heightfield; if your ground is invisible from
above and visible from below, swap two entries in each triple.

### Chunking

Eight cells per chunk edge, giving 3.2-unit chunks at a 0.4 cell size, and
roughly 43 × 37 ≈ 1,600 chunks for `circuit02`. Each becomes a
`MeshInstance3D` under a common parent.

That is a lot of nodes, and it is the same tradeoff chapter 11 makes for props:
chunks are culled individually and lit by their own neighbourhood, at the cost
of more draw calls. 1,600 is too many — the C engine gets away with it because
it culls in its own loop before issuing anything, whereas 1,600 Godot nodes are
1,600 entries in the spatial index and 1,600 potential draws.

So the Godot version uses **larger chunks**: 32 cells per edge, giving ~100
chunks of 12.8 units each, of which maybe a dozen are ever on screen. Measure it
on your target; `CHUNK_CELLS` is one constant.

---

## Relief, and where it lives now

The C engine bakes its relief shading into the terrain's **vertex colours**,
because its shader is shared with every other mesh and cannot afford a
per-material branch. Chapter 12 puts it in the shader instead, as a global
uniform plus a per-fragment tint.

Both are defensible and the tradeoff is the usual one:

| | Baked into vertex colours | Computed in the shader |
|---|---|---|
| Fragment cost | None | Small, per pixel |
| Build cost | Per vertex, at bake time | None |
| Changing the look | Rebuild the terrain | Change a uniform, see it live |
| Works on flat props too | No — only what you baked | Yes, the same shader everywhere |

The deciding argument for the shader is the last row. The relief tint has to
apply to the road and the scenery as well as the ground, or the track sits on a
tinted hillside looking like a decal. Chapter 12's `global uniform relief_low`
/ `relief_high` reach every material at once, and the alternative is baking the
same tint into the prop batcher's instance colours — where it would fight the
per-prop tints the level format already uses.

If you do bake it, the C engine's numbers are the ones to copy: exaggerate the
normal by 5, light it from `(0.55, 0.62, 0.42)`, subtract the light's own Y so
flat ground lands at zero, combine 0.55 of the height term with 1.5 of the
hillshade, and let it darken by at most 0.34. Vertex colours multiply, so relief
can only ever subtract light — anything above 1.0 clips to white and flattens
the crests back out.

---

## Bilinear height lookup

The heightfield is queried outside the renderer for exactly two things: placing
scenery on the ground, and letting the Blender tooling match the engine's fit.

```gdscript
## Bilinear height lookup; clamps outside the grid rather than failing.
func height_at(x: float, z: float) -> float:
    var fx: float = (x - origin.x) / cell_size
    var fz: float = (z - origin.y) / cell_size
    var ix: int = int(floor(fx))
    var iz: int = int(floor(fz))
    var tx: float = fx - float(ix)
    var tz: float = fz - float(iz)

    var top: float = lerpf(sample(ix, iz), sample(ix + 1, iz), tx)
    var bottom: float = lerpf(sample(ix, iz + 1), sample(ix + 1, iz + 1), tx)
    return lerpf(top, bottom, tz)
```

`int(floor(fx))`, not `int(fx)`. Truncation rounds towards zero, so at
`fx = -0.3` it gives 0 rather than −1, and the ground has a one-cell seam along
the negative axes. This is a genuinely common bug and it is invisible unless
your circuit crosses the origin — which, since the exporter centres levels
roughly on it, this one does.

Note what does **not** use this: the car. Chapter 08's height comes from
`SplineQuery.position.y`, which is the road, not the ground. The terrain is
scenery.

---

## The GPU alternative

Worth knowing, because for a purely visual heightfield it is genuinely simpler.

Since nothing collides with the terrain, the mesh does not have to exist on the
CPU at all. Bake the heightfield into an `ImageTexture` (one `Image` of
`FORMAT_RF`, 345 × 295 floats), drop a `PlaneMesh` with enough subdivision over
the circuit, and displace it in the vertex shader:

```glsl
uniform sampler2D heightfield : filter_linear, repeat_disable;
uniform vec2 field_origin;
uniform vec2 field_size;

void vertex() {
    vec2 uv = (VERTEX.xz - field_origin) / field_size;
    VERTEX.y = texture(heightfield, uv).r;
}
```

| | CPU `ArrayMesh` chunks | GPU displacement |
|---|---|---|
| Build cost | Seconds | Milliseconds — it is one texture upload |
| Memory | ~100k vertices | One `PlaneMesh` plus a 400 KB texture |
| Normals | Exact, from the grid | Derived in the shader from neighbouring texels, or baked into the texture's other channels |
| LOD | Manual | Free — swap `PlaneMesh.subdivide_*`, or use a mesh LOD |
| Editable at runtime | Rebuild | Write to the texture |
| Works on Compatibility | Yes | Yes — vertex texture fetch is available in GLES3-class hardware |

If you are starting fresh in Godot rather than porting, take the GPU version.
This series ships the CPU version because it is the direct analogue of the C
chapter, because exact grid normals matter for the relief shading, and because
the heightfield has to exist on the CPU anyway for the bake to be storable in
`LevelData`.

---

## Where the ground comes from, end to end

```
levels/circuit02.track  (waypoints, from Blender)
        │
        ▼   import (chapter 04)
LevelData.waypoint_positions
        │
        ▼   TrackSpline.build_from_level (chapter 06)
4,000 samples with heights
        │
        ▼   height_from_spline, per grid sample  ← the expensive step
Heightfield: 345 x 295 heights, baked into LevelData
        │
        ├──►  normal_at  ──►  ArrayMesh chunks  ──►  MeshInstance3D
        │
        └──►  relief range  ──►  global shader uniforms (chapter 12)
```

Every arrow is derived data. Nothing in that chain is authored except the
waypoints at the top, which means a designer moving one waypoint moves the road,
the ground, the normals and the shading together, and none of them can disagree.

That is the property worth protecting when you are tempted to let an artist
sculpt the ground by hand.

---

## Exercises

1. **Feel the exponent.** Change `d2 * d2` to `d2` and load the hilly circuit.
   Where does the road end up relative to the ground, and why does the effect
   get worse on a circuit with a long straight?

2. **Find the crease.** Add a cutoff — skip samples further than 6 units — and
   look at the ground in low sun. Photograph the ridge. Then work out where it
   is, in terms of the cutoff radius and the spline.

3. **Time the fit.** Build the heightfield in pure GDScript with
   `spline_stride` at 1, 4 and 16, timing each. Extrapolate to what stride 1
   would cost on your slowest target machine, and decide whether chapter 16 is
   worth it before reading it.

4. **Coarsen it.** Set `cell_size` to 0.9 — the value that was tried first —
   and drive the steepest climb. Find the place where the ground crosses above
   the tarmac and explain why 0.4 fixes it.

5. **Sink and z-fight.** Set `sink_below_track` to 0.0 and drive a lap. Then set
   it to 0.5. Describe both, and decide what the correct value would be if the
   camera were at ground level instead of overhead.

6. **The GPU version.** Implement the displacement shader above, including
   normals from neighbouring texels. Compare build time, memory
   (`Performance.MEMORY_STATIC`), and the visual result under low sun. Which
   would you ship, and does the answer change on the Pi?

---

Next: [14 — Procedural audio](14-audio.md)
