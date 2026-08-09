# 12 — Terrain from a racing line

> `engine/include/engine/terrain.h` · `engine/src/terrain.c` — 382 lines.

---

## The problem

Before elevation existed, the ground was one large flat quad
(`RenderGroundPlane`). That works for exactly as long as the track is flat.

Circuit01 climbs 2.25 units between its lowest and highest points; circuit02
climbs 4.58. Against a flat plane at `y = 0`, the Raidillon climb — most of two
road tiles above the start line — would float in the air with a visible gap under
it, and the plunge into the compression would sink beneath the grass.

The header states the fix:

```c
// A flat plane stops working the moment a circuit has hills: the road either
// floats above it or sinks through. This builds a heightfield from the racing
// line instead, so the ground rises and falls with the tarmac and flattens out
// smoothly away from it. Normals are computed from the field, so slopes shade.
```

Note where the height data comes from: **the racing line**. The level file
contains no terrain data at all. There is no heightmap image, no sculpted mesh,
no extra authoring step. The spline already carries elevation (Chapter 05), and
the ground is *derived* from it.

That is a deliberate content-pipeline decision. An author moves a road tile up
in Blender, the racing line follows it, and the ground follows the racing line —
with nothing to keep in sync by hand.

---

## Inverse-distance weighting

The core question: given an arbitrary point `(x, z)` anywhere in the world, what
height should the ground be?

Written out plainly, the answer is this:

```c
float weighted = 0.0f;
float total = 0.0f;
for (int i = 0; i < spline->count; i++) {
    Vector3 p = spline->samples[i].position;
    float dx = x - p.x;
    float dz = z - p.z;
    float d2 = dx * dx + dz * dz;
    float w = 1.0f / (d2 * d2 + 0.45f);
    weighted += p.y * w;
    total += w;
}
return (total > 0.0f) ? weighted / total : 0.0f;
```

That is the definition, and the tests still check the shipped code against it.
`HeightFieldAt` in `engine/src/terrain.c` computes the same sum a good deal
faster; [the section on making it fast](#making-it-fast) explains why it looks
the way it does.

**Inverse-distance weighting** (Shepard's method, 1968) is the standard
scattered-data interpolant. Every known sample votes for the answer, with a
weight that falls off with distance:

```
h(x) = Σ wᵢ·hᵢ / Σ wᵢ        where wᵢ = 1/(dᵢ⁴ + k)
```

Three parameter choices deserve attention.

**Why the fourth power?** `d2 * d2` is `d⁴`. The exponent controls how sharply
influence decays:

- `d¹` — very soft. The ground would be a gentle blur of the whole circuit's
  average and would not follow the road at all.
- `d²` — softer than you want here; the road still floats over its surroundings.
- `d⁴` — nearby samples dominate strongly, so the ground hugs the tarmac, but
  the tail is long enough to relax smoothly to the average in open field.

And `d⁴ = (d²)²` costs one multiply from the squared distance you already have.
**No square root anywhere in the function**, which matters when it runs
hundreds of thousands of times at load.

**Why `+ 0.45` in the denominator?** Two jobs. It prevents division by zero when
the query point coincides with a sample. And it caps the maximum weight, so a
sample is never infinitely authoritative — which is what keeps the surface
smooth *through* the samples rather than spiking at each one.

The Blender track generator mirrors the value:

```python
# tools/blender/build_demo_track.py
# Matches the engine's terrain weighting so scenery sits on the same ground the
# renderer builds. See HeightFieldAt in engine/src/terrain.c.
TERRAIN_FALLOFF = 0.45
```

A duplicated constant across two languages, documented as such — the same
honestly-paid cost as the point-in-box convention in Chapter 04. Without it,
trees placed by the generator would hover above or sink into ground built by the
engine.

**Why every sample, not the nearest few?** Because a nearest-k scheme has
*creases*: as the query point moves, the set of contributing samples changes
discontinuously, and the surface kinks along the boundaries. The comment names
exactly this: *"with no creases where the nearest sample changes."*

The cost is O(gridSamples × splineSamples). Circuit02 builds a 149 × 204 grid
against a 1,467-sample spline, so that is about 45 million iterations at load —
noticeable, once, at startup. Acceptable for a load-time cost, and the
alternative (a spatial index over the spline) would reintroduce the very
discontinuity the method avoids unless done very carefully.

---

## Making it fast

45 million iterations of eight-odd float operations should not take three
seconds. On a Pi it did, and the reason is the one operation in that list which
is not like the others.

```c
float w = 1.0f / (d2 * d2 + 0.45f);
```

Every other float op on a Cortex-A53 is pipelined: issue one per cycle, collect
the answer a few cycles later. Divide is not. The divider is a separate,
non-pipelined unit, and the next divide cannot start until the current one has
retired. Worse, the loop as written above makes that unavoidable — `total += w`
is a chain, each iteration waiting on the last, so the divides are forced into
single file and the loop runs at the divider's *latency* instead of its
throughput. Measured on this hardware: about 55 ns per sample, when the
arithmetic alone is worth perhaps 8.

The fix is to give the hardware more than one thing to do at a time. Split the
running sums into four independent chains, and step four samples per iteration:

```c
float w0 = 0.0f, w1 = 0.0f, w2 = 0.0f, w3 = 0.0f;
float t0 = 0.0f, t1 = 0.0f, t2 = 0.0f, t3 = 0.0f;

int i = 0;
for (; i + 3 < n; i += 4) {
    /* ... four distances, four divides, four pairs of accumulates ... */
}
for (; i < n; i++) {
    /* the leftovers, folded into chain 0 */
}

float weighted = (w0 + w1) + (w2 + w3);
float total = (t0 + t1) + (t2 + t3);
```

Four divides with no dependency between them can overlap. Nothing else changes:
the same samples, the same weights, the same kernel. Only the *order the sum is
accumulated in* is different, which moves the last bit or two of a float and is
why `tests/test_terrain.c` checks the result against a `double`-precision
transcription of the definition rather than against a stored number.

The second half of the win is where the samples are read from. `SplineSample` is
36 bytes — position, tangent, width, distance, grade — and this loop wants 12 of
them. `HeightFieldBuild` copies the positions out once into three flat arrays:

```c
typedef struct HeightField {
    float *x, *y, *z;   // spline sample positions, one array per component
    int count;
    void *storage;      // single allocation backing the three arrays
} HeightField;
```

Three contiguous streams, no stride, and the compiler can see through `restrict`
that they do not alias. Together the two changes take circuit02's height field
from **3.10 s to 1.43 s**, and the ground it produces is the same ground.

**What was deliberately not done.** The obvious next step is a spatial index —
bucket the spline, and for buckets far from the query point substitute a single
aggregate weight for all their samples. It works, and it is worth about another
7x. It also reintroduces creases: the moment a bucket flips from *summed
exactly* to *approximated*, the surface steps. Prototyping put that step at
several millimetres, which is small, but it lands as faint banding once the
relief shading exaggerates the normals five-fold. The exact sum is a
[documented design property](#inverse-distance-weighting) of this terrain, not
an implementation detail to trade away for load time.

---

## Building the grid

```c
TerrainSettings TerrainDefaultSettings(Color groundColor)
{
    TerrainSettings s = {
        .margin = 9.0f,
        // Cells have to be comfortably finer than the road is wide. At 0.9 the
        // ground was coarser than the 0.69 lane, so on a gradient the linear
        // surface between samples crossed above the tarmac and ate the track.
        .cellSize = 0.4f,
        .sinkBelowTrack = 0.06f,
        .color = groundColor,
    };
    return s;
}
```

**The cell size comment records a real bug and its diagnosis.** The heightfield
is piecewise linear between samples. If a cell spans more than the road's width,
the straight line between two samples on either side of the road can pass
*above* the road surface on a gradient — the ground literally swallows the
tarmac. At 0.4 units against a 0.69 lane, at least one sample lands on the road
itself and the interpolation stays below it.

**`sinkBelowTrack = 0.06`** pushes the whole surface 0.06 units below the
blended height, so the ground never z-fights with the road tiles it sits under.
Small enough to be invisible against a 0.69-wide lane, large enough to beat
depth-buffer precision at this scale. The Blender generator mirrors this too
(`TERRAIN_SINK = 0.06`).

```c
bool TerrainBuild(Terrain *terrain, const Spline *spline, const TerrainSettings *settings)
{
    /* ... bounds over all spline samples, expanded by margin ... */
    terrain->origin = (Vector2){ minX, minZ };
    terrain->gridX = (int)ceilf((maxX - minX) / cellSize);
    terrain->gridZ = (int)ceilf((maxZ - minZ) / cellSize);
    if (terrain->gridX < 1) terrain->gridX = 1;
    if (terrain->gridZ < 1) terrain->gridZ = 1;

    while ((long long)(terrain->gridX + 1) * (terrain->gridZ + 1) > TERRAIN_MAX_SAMPLES) {
        terrain->cellSize *= 2.0f;
        cellSize = terrain->cellSize;
        terrain->gridX = terrain->gridX / 2 + 1;
        terrain->gridZ = terrain->gridZ / 2 + 1;
    }

    int sampleCount = (terrain->gridX + 1) * (terrain->gridZ + 1);
    terrain->heights = calloc((size_t)sampleCount, sizeof(float));
    if (!terrain->heights) return false;

    for (int iz = 0; iz <= terrain->gridZ; iz++) {
        for (int ix = 0; ix <= terrain->gridX; ix++) {
            float x = minX + (float)ix * cellSize;
            float z = minZ + (float)iz * cellSize;
            terrain->heights[iz * (terrain->gridX + 1) + ix] =
                HeightFieldAt(&field, x, z) - settings->sinkBelowTrack;
        }
    }
    /* ... */
```

The same defensive doubling loop as `CollisionWorldBuild` (Chapter 06), with the
same `(long long)` cast to stop the comparison overflowing. `TERRAIN_MAX_SAMPLES`
is `512 * 512`.

**`gridX + 1` samples for `gridX` cells** — a fencepost. `n` cells need `n+1`
corner samples. Getting this wrong is the archetypal off-by-one, and it appears
in the sample count, the index arithmetic, and every loop bound (`<=` rather
than `<`).

---

## Normals from the heightfield

```c
static Vector3 NormalAt(const Terrain *terrain, int ix, int iz)
{
    // Central differences on the height field.
    float left = SampleGrid(terrain, ix - 1, iz);
    float right = SampleGrid(terrain, ix + 1, iz);
    float back = SampleGrid(terrain, ix, iz - 1);
    float front = SampleGrid(terrain, ix, iz + 1);
    float span = 2.0f * terrain->cellSize;
    Vector3 n = { (left - right) / span, 1.0f, (back - front) / span };
    return Vector3Normalize(n);
}
```

For a height field `y = f(x, z)`, the surface normal is:

```
n = (−∂f/∂x, 1, −∂f/∂z)
```

Derivation: the surface is the level set of `F(x,y,z) = y − f(x,z) = 0`, whose
gradient is `(−∂f/∂x, 1, −∂f/∂z)`, and the gradient of a level set is normal to
it.

The partial derivatives are estimated by **central differences**:
`∂f/∂x ≈ (f(x+h) − f(x−h)) / 2h`, hence `span = 2 × cellSize`. The negation is
folded into the subtraction order (`left - right` rather than `right - left`).

Central differences are second-order accurate and symmetric, so a sample's
normal does not lean toward its successor. Chapter 05 used the same technique for
spline tangents.

```c
static float SampleGrid(const Terrain *terrain, int ix, int iz)
{
    if (ix < 0) ix = 0;
    if (iz < 0) iz = 0;
    if (ix > terrain->gridX) ix = terrain->gridX;
    if (iz > terrain->gridZ) iz = terrain->gridZ;
    return terrain->heights[iz * (terrain->gridX + 1) + ix];
}
```

**Clamping the index** is what lets `NormalAt` ask for `ix - 1` at the grid edge
without a bounds check at every call site. Edge samples get a one-sided
difference (`f(0) - f(1)` instead of `f(-1) - f(1)`), which halves the effective
span and slightly exaggerates the edge normal — invisible, and the alternative
(special-casing the border) is more code for nothing.

**Clamp-to-edge is the right boundary policy here.** Wrapping would join opposite
sides of the map; returning zero would create a cliff at the border.

---

## Cartographic relief

This is the most interesting part of the file, and it is a rendering problem
solved with a mapmaking technique.

```c
// How much steeper the ground is pretended to be when shading relief. The
// camera looks almost straight down, where a real 17% slope tilts its normal
// by only 10 degrees and lights identically to the flat around it.
#define TERRAIN_RELIEF_EXAGGERATION 5.0f

// Deepest the relief shading is allowed to darken the ground. Vertex colours
// multiply the material colour, so shading can only ever subtract light —
// anything above 1.0 would clip to white and flatten the crests back out.
#define TERRAIN_RELIEF_DEPTH 0.34f

// Relief is lit from much lower than the real sun so that slopes separate into
// a lit face and a shaded one. Points towards the light, not along it.
static Vector3 ReliefLightDirection(void)
{
    return Vector3Normalize((Vector3){ 0.55f, 0.62f, 0.42f });
}
```

The problem, restated: a 17% slope tilts its normal by `atan(0.17) ≈ 9.6°`. Lit
by a near-overhead sun and viewed from a near-overhead camera, that is a change
in `N·L` of about 1.4% — invisible. The elevation is real in the simulation and
absent from the screen.

Photographic realism cannot fix this; the physics is against you. So the ground
borrows two devices from relief maps:

```c
// Seen from almost directly above, a slope's normal
// barely tilts and the scene lighting leaves hills
// invisible. Two cartographic tricks stand in for it,
// baked into the vertex colours:
//
//   height tint — hollows darker, crests lighter, which
//                 carries the broad shape of the land
//   hillshade   — the normal steepened well past
//                 reality and lit from a low angle,
//                 which gives each slope a lit face and
//                 a shaded one
float t = RenderReliefHeight01(v.y);

Vector3 steep = Vector3Normalize((Vector3){
    n.x * TERRAIN_RELIEF_EXAGGERATION, 1.0f,
    n.z * TERRAIN_RELIEF_EXAGGERATION });
// Measured against level ground, so flat terrain lands
// at zero and keeps the material colour untouched.
float hillshade = Vector3DotProduct(steep, reliefLight) - reliefLight.y;

float relief = Clamp(0.55f * (2.0f * t - 1.0f) + 1.5f * hillshade, -1.0f, 1.0f);
float shade = 1.0f - TERRAIN_RELIEF_DEPTH * (1.0f - relief) * 0.5f;
mesh.colors[out * 4 + 0] = (unsigned char)Clamp(255.0f * shade, 0.0f, 255.0f);
/* ... same for g and b, alpha 255 ... */
```

### Height tint (hypsometric tinting)

`RenderReliefHeight01(v.y)` maps the world height into [0,1] across the level's
own range (Chapter 11). `2t − 1` recentres it to [−1, 1] so the *middle*
elevation is neutral and the tint works both ways — hollows darken, crests
lighten. Weighted 0.55.

This is what a physical relief map does with colour ramps, and it carries the
broad shape of the land at a glance.

### Hillshade

`steep` multiplies the normal's horizontal components by 5 and renormalises,
producing the normal a slope *five times steeper* would have. A 9.6° tilt becomes
just over 40°. Now it lights very differently from the flat.

`reliefLight` is `(0.55, 0.62, 0.42)` normalised — a much lower elevation than
the real sun. A low light is what separates terrain into a lit face and a shaded
one; a high light flattens everything, which is why aerial photographs taken at
noon are hard to read and why relief maps are conventionally lit from the
north-west at about 45°.

Subtracting `reliefLight.y` is the calibration step. On perfectly flat ground the
normal is `(0,1,0)` and `dot(steep, light)` is exactly `light.y`, so the
subtraction makes `hillshade` zero there. **A flat level gets no relief shading
at all**, and its material colour survives untouched. Weighted 1.5.

### Combining

The two terms sum, clamp to [−1, 1], and map into a multiplier:

```c
float shade = 1.0f - TERRAIN_RELIEF_DEPTH * (1.0f - relief) * 0.5f;
```

With `relief = +1`, `shade = 1.0`; with `relief = −1`, `shade = 1 − 0.34 = 0.66`.
A 34% swing between the darkest and lightest ground.

**Why it can only darken.** The comment appears twice in the codebase, once in
`terrain.c` and once in `render.c`, because it constrains both:

> Vertex colours multiply the material colour, so shading can only ever subtract
> light — anything above 1.0 would clip to white and flatten the crests back out.

The shader computes `base = fragColor * colDiffuse`. A vertex colour above 1.0 is
not representable in an 8-bit `unsigned char` anyway, and even in float it would
clip against the display. So the whole scheme is built around subtracting from
the brightest point rather than adding to the average.

**And the props use the same range**, via `RenderReliefHeight01` in `EmitProp`
(Chapter 10), so the road darkens into a dip along with the grass beside it. If
only one were tinted, the road would read as a flat ribbon laid over shaded
ground.

---

## Chunked meshes

```c
// Cells per chunk edge. Small enough that light selection is local, large
// enough that the draw-call count stays modest.
#define TERRAIN_CHUNK_CELLS 8
```

8 × 8 cells at 0.4 units is a 3.2-unit chunk — half the static batch's 6-unit
chunk, and for the same two reasons: frustum culling and local light selection.

```c
int vertexCount = cells * 6;   // two triangles per cell, non-indexed
Mesh mesh = { 0 };
mesh.vertexCount = vertexCount;
mesh.triangleCount = cells * 2;
mesh.vertices = MemAlloc((unsigned int)(sizeof(float) * 3 * (size_t)vertexCount));
mesh.normals = MemAlloc((unsigned int)(sizeof(float) * 3 * (size_t)vertexCount));
mesh.colors = MemAlloc((unsigned int)(sizeof(unsigned char) * 4 * (size_t)vertexCount));
/* ... */
const int cornerX[6] = { 0, 0, 1, 0, 1, 1 };
const int cornerZ[6] = { 0, 1, 1, 0, 1, 0 };
for (int k = 0; k < 6; k++) {
    int gx = ix + cornerX[k];
    int gz = iz + cornerZ[k];
    Vector3 v = VertexAt(terrain, gx, gz);
    Vector3 n = NormalAt(terrain, gx, gz);
    /* ... write, extend bounds ... */
}
```

Six vertices per cell — two triangles, non-indexed, sharing no vertices. The
corner tables spell out the winding: `(0,0) (0,1) (1,1)` then `(0,0) (1,1) (1,0)`.

**Why non-indexed?** Consistency with the static batch (Chapter 10), simplicity,
and because the vertex colours differ per corner anyway (each has its own height
and hillshade), so an indexed mesh would share less than it looks.

The memory cost is real: circuit02's terrain runs to a few hundred thousand
triangles. `TerrainBuild` logs it:

```
TERRAIN: 149x204 samples, 494 chunks, 60088 triangles (cell 0.40)
```

Which is a lot of ground — and the frustum test means only a handful of chunks
are drawn per frame. Press `F1` to see.

Note the allocation-failure path `continue`s rather than aborting: a chunk that
cannot be allocated is simply skipped, leaving a hole in the ground rather than
failing the level load. For a cosmetic system that is the right severity.

```c
terrain->material = RenderSceneMaterial();
terrain->material.maps[MATERIAL_MAP_DIFFUSE].color = settings->color;
```

The terrain borrows the renderer's scene material so it picks up the lighting
shader and fog, then overrides the diffuse colour. `TerrainFree` deliberately
does not unload it:

```c
// The material's shader and maps belong to the renderer.
memset(terrain, 0, sizeof(*terrain));
```

**Borrowed resources must not be freed by the borrower**, and saying so at the
free site is how you stop someone "fixing the leak" later.

---

## Bilinear height lookup

```c
float TerrainHeightAt(const Terrain *terrain, float x, float z)
{
    if (!terrain->ready || !terrain->heights) return 0.0f;

    float fx = (x - terrain->origin.x) / terrain->cellSize;
    float fz = (z - terrain->origin.y) / terrain->cellSize;
    int ix = (int)floorf(fx);
    int iz = (int)floorf(fz);
    float tx = fx - (float)ix;
    float tz = fz - (float)iz;

    float h00 = SampleGrid(terrain, ix, iz);
    float h10 = SampleGrid(terrain, ix + 1, iz);
    float h01 = SampleGrid(terrain, ix, iz + 1);
    float h11 = SampleGrid(terrain, ix + 1, iz + 1);

    float top = h00 + (h10 - h00) * tx;
    float bottom = h01 + (h11 - h01) * tx;
    return top + (bottom - top) * tz;
}
```

**Bilinear interpolation**: lerp along X on both edges of the cell, then lerp
between those two results along Z. The canonical way to sample a regular grid,
and exactly what a GPU does for a bilinear-filtered texture fetch.

`floorf` then `fx - ix` gives the integer cell and the fractional position
within it, correct for negative coordinates where a cast would not be.

Interestingly, **nothing in the game currently calls this**. Cars get their
height from the spline query, not from the terrain, because the spline is the
authority on where the road is and the terrain is derived from it. `TerrainHeightAt`
exists for anything that needs ground height *away* from the track — scattered
props, particles, a camera that collides with the landscape.

That is a defensible amount of unused API: it is nine lines, it is the natural
companion to a heightfield, and its absence would be surprising.

---

## Where the ground comes from, end to end

```
Blender: author places road tiles at various heights
    │
    ▼
racing-line curve follows them
    │
    ▼  (exporter)
levels/*.json  "waypoints": [{ "pos": [x, y, z], "width": w }, ...]
    │
    ▼  LevelLoad
Level.waypoints
    │
    ▼  SplineBuild — Catmull-Rom, resample, arc length, grade
Spline.samples[i].position.y
    │
    ├──────────────────────────────► CarSurface.height  (cars sit on the road)
    │                                CarSurface.grade   (gravity along the road)
    │
    ▼  HeightFieldAt — inverse-distance weighting
Terrain.heights[]
    │
    ├──► NormalAt          ──► mesh normals   ──► scene lighting
    ├──► hillshade + tint  ──► vertex colours ──► relief shading
    └──► VertexAt          ──► mesh positions ──► the ground you see
```

One authored quantity — the height of the racing line — feeds the physics, the
ground geometry, the ground shading and the prop shading. Nothing else has to be
authored, and nothing can fall out of sync because there is only one source.

That is the payoff of deriving rather than authoring, and it is worth weighing
against the cost: you cannot make a hill that is *not* near the track. A valley
in the middle of the infield is not expressible. For a racing game that is a
fair trade; for an open-world game it would not be.

---

## Exercises

1. **Change the falloff.** Set the exponent in `HeightFieldAt` to `d²` (use
   `d2 + 0.45f`) and then to `d⁸` (`d2*d2*d2*d2 + 0.45f`). Screenshot each with
   `F2` from the same spot. Describe how the ground meets the road in each case.

2. **See the swallowing bug.** Set `cellSize` to `0.9f` — the value the comment
   says failed — and drive the Raidillon climb on circuit01. Look at the edges
   of the tarmac on the gradient.

3. **Turn relief off.** Set `TERRAIN_RELIEF_DEPTH` and `RELIEF_PROP_DEPTH` to
   `0.0f` and take a screenshot of a hilly section. Compare with one before. Can
   you tell there are hills?

4. **Exaggerate more.** Set `TERRAIN_RELIEF_EXAGGERATION` to 1.0 (no
   exaggeration) and then to 20.0. Where does it stop reading as terrain and
   start reading as an artefact?

5. **Move the relief light.** Change `ReliefLightDirection` to `(0, 1, 0)`
   (straight down). Explain why the hillshade term goes to almost nothing, using
   the `- reliefLight.y` calibration.

6. **Cost the build.** Time `TerrainBuild` on both circuits (wrap it in
   `GetTime()` calls). Compute the iteration count as
   `gridSamples × splineSamples`. Then implement a spatial acceleration: bucket
   the spline samples into a coarse grid and only weight samples within, say, 12
   units. Measure the speedup — and look carefully for the creases the comment
   warns about.

7. **Use `TerrainHeightAt`.** Make the debug overlay show the terrain height and
   the spline height under the player's car, and their difference. Does it match
   `sinkBelowTrack`? Where does it not, and why?

---

Next: [13 — Procedural audio](13-audio.md)
