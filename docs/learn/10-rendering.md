# 10 — Rendering: batching and culling

> `engine/include/engine/render.h` · `engine/src/render.c` — 996 lines, the
> largest file in the project. Shadows and shaders are Chapter 11.

---

## The problem

Circuit01 has 732 props. Circuit02 has 1,237. The obvious way to draw them is a
loop:

```c
for (int i = 0; i < level->propCount; i++) {
    RenderModelEuler(AssetsGetModel(level->props[i].model), /* ... */);
}
```

That is 1,237 `DrawModel` calls per frame. Each one binds a shader, uploads a
model matrix, sets uniforms, binds a vertex array and issues a draw. On a
desktop GPU it is wasteful. On a Raspberry Pi's VideoCore, driving GLES2 over a
DRM/KMS surface, it is a slideshow.

The bottleneck is not triangles. Circuit02's props total 35,848 triangles,
which any GPU eats without noticing. The bottleneck is **draw calls** — CPU-side state
changes and command submission, one per object, with the GPU idling between
them.

---

## The idea: static batching

The kit's models are untextured, flat-shaded geometry. Every mesh's material is
a single diffuse colour. And every prop is *static* — a road tile never moves.

Those two facts together mean the per-object state is only: the vertex positions
(fixable by pre-transforming), the normals (same), and the colour (movable into
a vertex attribute).

So:

> At load time, transform every prop's triangles into world space, bake its
> material colour into per-vertex colours, and concatenate everything into a few
> large meshes grouped by spatial region.

1,237 draw calls become a few dozen. And because each mesh covers a compact
region of space, most of them can be rejected outright by a frustum test.

`render.h` states it:

```c
// The Kenney kit is untextured flat-shaded geometry, so every static prop in a
// level is baked once into vertex-coloured meshes grouped by spatial chunk.
// That turns a few hundred DrawModel calls into a handful of DrawMesh calls
// and makes frustum culling cheap, which matters on a Raspberry Pi.
```

**What you give up:** you cannot move, hide or recolour an individual prop after
baking. For a racing circuit that is not a loss. If you needed destructible
scenery, static batching would be the wrong tool.

---

## Chunking

```c
typedef struct ChunkKey { int x, z, decal; } ChunkKey;
```

```c
ChunkKey key = { (int)floorf(p->position.x / chunkSize),
                 (int)floorf(p->position.z / chunkSize),
                 PropIsDecal(p) ? 1 : 0 };
```

`main.c` uses `BATCH_CHUNK_SIZE = 6.0f`, so props are bucketed into 6×6-unit
cells on the XZ plane.

**Why chunk at all?** One giant mesh would be one draw call — even better. But
then the frustum test is all-or-nothing: the camera sees a corner of the
circuit, the test passes, and the GPU processes every triangle in the level. And
per-draw light selection (below) would have to pick eight lights for the entire
world.

Chunking trades a few more draw calls for the ability to reject most of the
geometry and to light each region locally. 6 units is a bit larger than one
screenful of a top-down camera, which is roughly the right granularity.

`floorf` rather than a cast, for the same reason as Chapter 06: truncation folds
negative coordinates onto the wrong cell.

### The `decal` flag

The third key component splits flat props into separate chunks:

```c
// engine/include/engine/render.h
typedef struct BatchChunk {
    Mesh mesh;
    BoundingBox bounds;
    // Props with no vertical extent — grass patches, painted markings — lie in
    // the ground rather than standing on it, and cannot plausibly shadow the
    // surface they are part of. Worse, being coplanar with it they land at the
    // same depth in the shadow map and speckle it with their own acne. They are
    // batched separately from everything else so the depth pass can skip them.
    bool castsShadow;
} BatchChunk;
```

```c
// Vertical extent under which a prop counts as lying in the ground rather than
// standing on it. The kit's grass patch is a single quad exactly 0 tall; the
// shortest thing that should still cast is a kerb, an order of magnitude up.
#define PROP_DECAL_HEIGHT 0.02f

// Height of a prop once placed, so a flat one can be told from a standing one.
static bool PropIsDecal(const LevelProp *p)
{
    BoundingBox local = AssetsGetModelBounds(p->model);
    Matrix world = PropMatrix(p);
    float lowest = 1e30f, highest = -1e30f;
    for (int i = 0; i < 8; i++) {
        Vector3 corner = { (i & 1) ? local.max.x : local.min.x,
                           (i & 2) ? local.max.y : local.min.y,
                           (i & 4) ? local.max.z : local.min.z };
        float y = Vector3Transform(corner, world).y;
        if (y < lowest) lowest = y;
        if (y > highest) highest = y;
    }
    return (highest - lowest) <= PROP_DECAL_HEIGHT;
}
```

Two things here.

**The bit-trick corner enumeration.** `(i & 1)`, `(i & 2)`, `(i & 4)` for
`i ∈ [0,8)` enumerates all eight sign combinations of min/max. This is the
standard idiom for iterating a box's corners, and it appears again in the
frustum test below.

Note it transforms all eight corners rather than transforming the box's min and
max, because a *rotated* box's extremes are not the transforms of its extremes.

**Why the flag exists** is a shadow-mapping detail covered fully in Chapter 11.
In brief: a flat quad lying on the ground is coplanar with what it would shadow,
so in the depth map it lands at essentially the same depth as the ground, and
the comparison flickers — a speckled mess called shadow acne. Rather than fixing
it with bias (which would need to be huge), such props are excluded from the
depth pass entirely. Which they can be, because a zero-height object casts
nothing.

The threshold is justified against a real asset: the kit's grass patch is
exactly 0 tall, and a kerb — the shortest thing that should cast — is an order of
magnitude taller.

---

## The three-pass bake

```c
bool StaticBatchBuild(StaticBatch *batch, const Level *level, float chunkSize)
```

The same measure-allocate-fill shape as Chapters 02, 05 and 06.

### Pass 1: bucket and count

```c
int maxChunks = level->propCount + 8;
ChunkBuild *chunks = calloc((size_t)maxChunks, sizeof(ChunkBuild));
int *propChunk = calloc((size_t)level->propCount, sizeof(int));

int chunkCount = 0;
for (int i = 0; i < level->propCount; i++) {
    const LevelProp *p = &level->props[i];
    ChunkKey key = { /* ... */ };
    int ci = FindChunk(chunks, chunkCount, key);
    int verts = PropVertexCount(p);

    if (ci >= 0 && chunks[ci].vertexCount + verts > BATCH_MAX_VERTS_PER_CHUNK) {
        ci = -1;   // full: start another chunk with the same key
        for (int j = 0; j < chunkCount; j++) {
            if (chunks[j].key.x == key.x && chunks[j].key.z == key.z &&
                chunks[j].key.decal == key.decal &&
                chunks[j].vertexCount + verts <= BATCH_MAX_VERTS_PER_CHUNK) { ci = j; break; }
        }
    }
    if (ci < 0) {
        if (chunkCount >= maxChunks) { ci = 0; }
        else { ci = chunkCount++; chunks[ci].key = key; }
    }
    chunks[ci].vertexCount += verts;
    propChunk[i] = ci;
}
```

`FindChunk` is a linear scan over the chunk list. With a few dozen chunks and a
thousand props that is ~30,000 integer comparisons at load — microseconds. A
hash map would be more code for no measurable gain.

**Chunk splitting.** `BATCH_MAX_VERTS_PER_CHUNK` is 120,000 (non-indexed, so a
triangle-soup cap). If a 6×6 cell contains a dense cluster of grandstands, the
chunk fills and a second one is created with the same key. That works because
the key is used only for grouping, never for lookup after the build — `propChunk[i]`
records the answer.

**`propChunk[i]` is the memo.** The assignment computed here is reused in pass 2,
so the (surprisingly expensive) `PropIsDecal` and `FindChunk` work is done once.

`maxChunks = propCount + 8` is the worst case: every prop in its own chunk. The
`if (chunkCount >= maxChunks) { ci = 0; }` fallback cannot fire, but costs
nothing and means a future change to the key cannot cause an overrun.

### Pass 2: allocate and emit

```c
for (int c = 0; c < chunkCount; c++) {
    int n = chunks[c].vertexCount;
    chunks[c].vertices = MemAlloc((unsigned int)(sizeof(float) * 3 * (size_t)n));
    chunks[c].normals  = MemAlloc((unsigned int)(sizeof(float) * 3 * (size_t)n));
    chunks[c].colors   = MemAlloc((unsigned int)(sizeof(unsigned char) * 4 * (size_t)n));
    chunks[c].bounds.min = (Vector3){ 1e30f, 1e30f, 1e30f };
    chunks[c].bounds.max = (Vector3){ -1e30f, -1e30f, -1e30f };
    if (!chunks[c].vertices || !chunks[c].normals || !chunks[c].colors) {
        /* ... free everything allocated so far, bail ... */
    }
}
for (int i = 0; i < level->propCount; i++) EmitProp(&chunks[propChunk[i]], &level->props[i]);
```

`MemAlloc` (raylib's allocator) rather than the arena, because these pointers are
handed to `UploadMesh` and become raylib's to free. Chapter 02's rule about
matching the allocator to the ownership.

The bounds are seeded **inverted** — min at +∞, max at −∞ — so the first
`if (p.x < min.x)` always takes. Seeding to zero would include the origin in
every chunk's bounds, and a chunk at the far side of the map would report a
bounding box stretching back to (0,0,0), defeating the frustum cull.

### Emitting one prop

```c
static void EmitProp(ChunkBuild *chunk, const LevelProp *prop)
{
    Model *model = AssetsGetModel(prop->model);
    Matrix world = PropMatrix(prop);
    // Normals need the inverse-transpose to survive non-uniform scaling.
    Matrix normalMat = MatrixTranspose(MatrixInvert(world));

    for (int mi = 0; mi < model->meshCount; mi++) {
        Mesh *mesh = &model->meshes[mi];
        if (!mesh->vertices) continue;

        Color matColor = WHITE;
        int matIndex = model->meshMaterial ? model->meshMaterial[mi] : 0;
        if (model->materials && matIndex >= 0 && matIndex < model->materialCount) {
            matColor = model->materials[matIndex].maps[MATERIAL_MAP_DIFFUSE].color;
        }
        unsigned char r = (unsigned char)((matColor.r * prop->tint.r) / 255);
        /* ... g, b, a ... */

        int indexCount = mesh->triangleCount * 3;
        for (int k = 0; k < indexCount; k++) {
            int vi = mesh->indices ? (int)mesh->indices[k] : k;
            if (vi >= mesh->vertexCount) continue;

            Vector3 p = { mesh->vertices[vi*3+0], mesh->vertices[vi*3+1], mesh->vertices[vi*3+2] };
            p = Vector3Transform(p, world);

            Vector3 n = { 0.0f, 1.0f, 0.0f };
            if (mesh->normals) {
                n = (Vector3){ mesh->normals[vi*3+0], mesh->normals[vi*3+1], mesh->normals[vi*3+2] };
                n = Vector3Normalize(Vector3Transform(n, normalMat));
            }

            // Tint by world height, matching the ground, so the road darkens
            // into a dip and lightens over a crest instead of reading as one
            // flat ribbon from above.
            float shade = 1.0f - RELIEF_PROP_DEPTH * (1.0f - RenderReliefHeight01(p.y));

            int o = chunk->written;
            if (o >= chunk->vertexCount) return;   // sizing pass guarantees space
            /* ... write position, normal, colour; extend bounds ... */
        }
    }
}
```

Four ideas worth extracting.

**The inverse-transpose for normals.** A normal is not a direction you can
transform with the model matrix. Under non-uniform scaling — squash a sphere
flat — the surface tilts one way and a naively transformed normal tilts the
other. The correct transform is `(M⁻¹)ᵀ`.

Why: a normal `n` satisfies `n · t = 0` for every tangent `t` on the surface.
After transforming tangents by `M`, you need `n' · (M t) = 0`. Writing
`n' = A n`, that is `(A n)ᵀ M t = nᵀ Aᵀ M t = 0`, which holds for all `t` iff
`Aᵀ M = I`, i.e. `A = (M⁻¹)ᵀ`.

For a pure rotation, `(M⁻¹)ᵀ = M`, so it costs nothing to be correct. For
anything scaled, it is the difference between correct lighting and subtly wrong
lighting.

**Index expansion into a triangle soup.** `vi = mesh->indices ? indices[k] : k`
dereferences the index buffer, and each vertex is written out in full. The
result is unindexed: three vertices per triangle, duplicates and all.

That costs memory — roughly 3× for a well-shared mesh. It buys concatenation:
merging indexed meshes means rebasing every index by the running vertex count,
tracking it per source mesh, and handling overflow past 65,535. For flat-shaded
geometry where most vertices need distinct normals anyway, sharing was low to
begin with.

**Material colour folded into vertex colour.** `matColor × propTint / 255` is
computed once per mesh, then written to every vertex. This is why batching works
at all: the only per-object state left was the colour, and it has just become a
vertex attribute. The `/255` is fixed-point multiplication of two 0–255 values.

**Bounds computed during emission**, not from the source model's box, because
after transformation the true extent may differ from the transform of the
original box.

The `if (o >= chunk->vertexCount) return;` is a belt-and-braces bound. Pass 1
guarantees space; if a future change breaks that, this stops the overrun rather
than corrupting the heap.

### Pass 3: upload

```c
int live = 0;
for (int c = 0; c < chunkCount; c++) {
    if (chunks[c].written < 3) {
        MemFree(chunks[c].vertices); MemFree(chunks[c].normals); MemFree(chunks[c].colors);
        continue;
    }
    Mesh mesh = { 0 };
    mesh.vertexCount = chunks[c].written;
    mesh.triangleCount = chunks[c].written / 3;
    mesh.vertices = chunks[c].vertices;
    mesh.normals = chunks[c].normals;
    mesh.colors = chunks[c].colors;
    UploadMesh(&mesh, false);

    batch->chunks[live].mesh = mesh;
    batch->chunks[live].bounds = chunks[c].bounds;
    batch->chunks[live].castsShadow = (chunks[c].key.decal == 0);
    batch->totalTriangles += mesh.triangleCount;
    live++;
}
batch->chunkCount = live;
```

Chunks with fewer than three vertices — no complete triangle — are dropped and
their buffers freed. The `live` cursor compacts the array. `UploadMesh(&mesh, false)`
with `false` meaning "not dynamic": a static VBO, uploaded once, which is what
lets the driver put it in the fastest memory it has.

The log line tells you it worked:

```
RENDER: baked 732 props into 105 chunks (21480 triangles)
```

732 draw calls became 105 (229 on circuit02), of which the frustum test leaves
only the handful in view each frame. Press `F1` in game to see `drawnLastFrame`
live.

---

## Frustum culling

### Extracting the planes

```c
// Gribb-Hartmann plane extraction from a view-projection matrix.
static Frustum FrustumFromMatrix(Matrix m)
{
    Frustum f;
    f.planes[0] = (Vector4){ m.m3 + m.m0, m.m7 + m.m4, m.m11 + m.m8,  m.m15 + m.m12 }; // left
    f.planes[1] = (Vector4){ m.m3 - m.m0, m.m7 - m.m4, m.m11 - m.m8,  m.m15 - m.m12 }; // right
    f.planes[2] = (Vector4){ m.m3 + m.m1, m.m7 + m.m5, m.m11 + m.m9,  m.m15 + m.m13 }; // bottom
    f.planes[3] = (Vector4){ m.m3 - m.m1, m.m7 - m.m5, m.m11 - m.m9,  m.m15 - m.m13 }; // top
    f.planes[4] = (Vector4){ m.m3 + m.m2, m.m7 + m.m6, m.m11 + m.m10, m.m15 + m.m14 }; // near
    f.planes[5] = (Vector4){ m.m3 - m.m2, m.m7 - m.m6, m.m11 - m.m10, m.m15 - m.m14 }; // far
    for (int i = 0; i < 6; i++) {
        Vector4 p = f.planes[i];
        float len = sqrtf(p.x * p.x + p.y * p.y + p.z * p.z);
        if (len > 1e-6f) {
            f.planes[i] = (Vector4){ p.x / len, p.y / len, p.z / len, p.w / len };
        }
    }
    return f;
}
```

This looks like magic and is not.

A point is inside the view volume iff, after the view-projection transform, its
clip coordinates satisfy `−w ≤ x ≤ w`, `−w ≤ y ≤ w`, `−w ≤ z ≤ w`.

Take the left plane. `x ≥ −w` rearranges to `x + w ≥ 0`. And `x` and `w` are
rows of the matrix dotted with the point:

```
x = m0·px + m4·py + m8·pz  + m12
w = m3·px + m7·py + m11·pz + m15
```

So `x + w ≥ 0` is:

```
(m0+m3)·px + (m4+m7)·py + (m8+m11)·pz + (m12+m15) ≥ 0
```

which is exactly `ax + by + cz + d ≥ 0` — a plane equation, with the
coefficients being row-sums of the matrix. That is the first line of the
function. Right is `w − x ≥ 0`, giving differences instead of sums. And so on for
all six.

Published by Gribb and Hartmann in 2001, and it is the standard method because
it needs no knowledge of the camera at all: it works for perspective,
orthographic, oblique, or any projection you can express as a matrix.

**Normalisation** divides all four coefficients by the length of `(a,b,c)`.
After that, `ax+by+cz+d` is the *signed distance* from the plane in world units,
not just a sign. This code only uses the sign — but normalising is cheap, done
once per frame, and makes the planes usable for anything else later.

### Testing a box

```c
bool RenderFrustumTestBox(const Frustum *f, BoundingBox box)
{
    for (int i = 0; i < 6; i++) {
        Vector4 p = f->planes[i];
        // Positive vertex: the corner furthest along the plane normal.
        Vector3 v = { p.x >= 0 ? box.max.x : box.min.x,
                      p.y >= 0 ? box.max.y : box.min.y,
                      p.z >= 0 ? box.max.z : box.min.z };
        if (p.x * v.x + p.y * v.y + p.z * v.z + p.w < 0.0f) return false;
    }
    return true;
}
```

The **positive vertex** (or "p-vertex") optimisation.

To reject a box against a plane you must show *every* corner is outside. But you
only need to check the one corner **furthest along the plane's normal** — if
even that one is on the negative side, all eight are. And the sign of each
normal component tells you which corner that is, component by component: if
`p.x ≥ 0`, the furthest-along-x corner is `max.x`.

Three comparisons and a dot product instead of eight transformed corners. Per
plane, per box, per frame.

**This test is conservative.** A box can pass all six planes and still be
outside the frustum — the classic case is a large box near a corner of the view
volume, outside the frustum but not entirely outside any single plane. That
produces a false positive: the chunk is drawn unnecessarily. Harmless. A false
*negative* would pop geometry out of view, and cannot happen here.

`if (len > 1e-6f)` in the normaliser and the ordering of the tests both matter
less than one property: **the cheapest test is the one that most often rejects.**
For a chase camera looking down, the near and far planes reject least; left and
right reject most. The loop tests all six regardless, which at six planes is not
worth reordering.

### Culling against the sun

```c
Frustum RenderFrustumFromCamera(Camera3D camera)
{
    // Filling the shadow map, what matters is whether the sun can see a chunk,
    // not whether the player can — a hill behind the camera still casts onto
    // the road ahead of it. Overriding here keeps every caller's draw loop the
    // same in both passes.
    if (g_render.shadowPass) return FrustumFromMatrix(g_render.lightVP);
    /* ... build view * projection from the camera ... */
}
```

A small piece of API design worth noticing. During the shadow pass the relevant
frustum is the *sun's*, not the camera's. Rather than making every caller pass a
flag or a different matrix, the function returns the right frustum for the
current pass.

The result is that `main.c`'s two passes are textually identical draw sequences:

```c
RenderBeginShadowPass(focus);
    TerrainDraw(&stage.terrain, camera.camera);
    StaticBatchDraw(&stage.batch, camera.camera);
    for (...) DrawRacer(...);
RenderEndShadowPass();

RenderBeginScene(camera.camera, stage.level.skyColor);
    TerrainDraw(&stage.terrain, camera.camera);
    StaticBatchDraw(&stage.batch, camera.camera);
    /* ... */
```

Same calls, same arguments. `render.h` states the contract: *"Issue the same
draws as the main pass between these two: whatever is drawn is what casts."*

**Hide the mode in the state, not in every call site.** The alternative — a
`bool isShadowPass` parameter threaded through `StaticBatchDraw`, `TerrainDraw`,
`RenderDrawLitMesh`, `RenderModelTransform` — would touch every function for no
gain in clarity.

---

## Per-draw light selection

The scene may hold 64 lights (`LIGHTS_MAX`); circuit02 has 32 placed lamps plus
2 headlights per car. The shader can afford 8 (`LIGHTS_PER_DRAW`).

```c
// engine/include/engine/light.h
// Forward shaded: a scene may hold many lights, but only the few most relevant
// to whatever is being drawn are uploaded to the shader. That keeps the
// fragment cost fixed and stays inside the uniform budget of GLES2, which the
// Raspberry Pi console build targets.
#define LIGHTS_MAX 64
#define LIGHTS_PER_DRAW 8
```

Two constraints, and GLES2 sharpens both. Fragment cost scales with the loop
bound. And uniform storage is scarce: the GLES2 spec guarantees only 16 fragment
uniform vectors (`GL_MAX_FRAGMENT_UNIFORM_VECTORS`), while three `vec4` arrays of
8 lights already costs 24, plus the sun, fog and shadow uniforms on top. Real
devices expose far more than the guaranteed floor — which is why this works — but
8 is already leaning on that, and 64 would need 192 vectors for the light arrays
alone.

### Scoring influence

```c
float LightInfluence(const Light *light, Vector3 center, float radius)
{
    if (!light->enabled || light->intensity <= 0.0f) return 0.0f;

    float distance = Vector3Distance(light->position, center) - radius;
    if (distance < 0.0f) distance = 0.0f;
    if (distance >= light->range) return 0.0f;

    // Matches the shader's falloff so selection and shading agree.
    float falloff = 1.0f - distance / light->range;
    return light->intensity * falloff * falloff;
}
```

The comment is the important line. The shader computes:

```glsl
float atten = 1.0 - dist/range;
atten *= atten;
```

and `LightInfluence` computes the same thing. If they disagreed, the selector
could drop a light the shader would have rendered brightly, and objects would
visibly change lighting as the camera moved.

**When a selection heuristic and a rendering formula must agree, write them from
the same expression and say so in both places.**

`distance - radius` measures to the *nearest point* of the bounding sphere, so a
lamp just outside a large chunk scores as if it were at the chunk's edge, which
is where it will actually light.

The early `distance >= range` return is what makes an unlit level free: with no
lights in range, `LightSetSelect` returns 0 and the shader's loop exits on its
first iteration.

### Selecting the top 8

```c
int LightSetSelect(const LightSet *set, Vector3 center, float radius, int *out, int maxOut)
{
    float best[LIGHTS_PER_DRAW];
    int count = 0;
    if (maxOut > LIGHTS_PER_DRAW) maxOut = LIGHTS_PER_DRAW;

    for (int i = 0; i < set->count; i++) {
        float score = LightInfluence(&set->lights[i], center, radius);
        if (score <= 0.0f) continue;

        // Insertion sort into a short strongest-first list.
        int slot = count;
        if (count == maxOut) {
            if (score <= best[count - 1]) continue;
            slot = count - 1;
        } else {
            count++;
        }
        while (slot > 0 && best[slot - 1] < score) {
            best[slot] = best[slot - 1];
            out[slot] = out[slot - 1];
            slot--;
        }
        best[slot] = score;
        out[slot] = i;
    }
    return count;
}
```

A **bounded insertion sort** — a top-k selection. O(n·k) with n=64 and k=8, but
the `score <= 0` early-out rejects most lights immediately (they are out of
range), and `score <= best[count-1]` rejects most of the rest without shifting.

A full sort would be O(n log n) and would need somewhere to put 64 scores. This
needs 8 floats of stack.

### Uploading

```c
static void UploadLightsFor(Vector3 center, float radius)
{
    int count = 0;
    int chosen[LIGHTS_PER_DRAW];
    if (g_render.lights) {
        count = LightSetSelect(g_render.lights, center, radius, chosen, LIGHTS_PER_DRAW);
    }

    // Skip the upload when nothing is lit and nothing was lit last time.
    if (count == 0 && g_render.uploadedCount == 0) return;

    float posRange[LIGHTS_PER_DRAW * 4] = { 0 };
    float colors[LIGHTS_PER_DRAW * 4] = { 0 };
    float dirs[LIGHTS_PER_DRAW * 4] = { 0 };

    for (int i = 0; i < count; i++) {
        const Light *light = &g_render.lights->lights[chosen[i]];
        posRange[i*4+0] = light->position.x; /* ... */
        posRange[i*4+3] = light->range;

        // Premultiply intensity so the shader does one multiply fewer.
        colors[i*4+0] = light->color.r / 255.0f * light->intensity;
        /* ... */
        // -2 marks a point light, which the shader tests for to skip the cone.
        colors[i*4+3] = (light->type == LIGHT_SPOT)
            ? cosf(light->outerConeDeg * DEG2RAD) : -2.0f;

        dirs[i*4+0] = light->direction.x; /* ... */
        dirs[i*4+3] = cosf(light->innerConeDeg * DEG2RAD);
    }
    /* ... SetShaderValueV for each ... */
    g_render.uploadedCount = count;
}
```

Four packing decisions, each saving GPU work:

**Three `vec4` arrays instead of a struct array.** GLSL 100 has no structs in
uniform arrays worth relying on, and packing into `vec4`s matches how uniform
registers are allocated — a `vec3` still occupies a full register on most
hardware, so the `w` slot is free real estate.

**`w` slots carry extra data.** `posRange.w` is the range. `lightColor.w` is
`cos(outerCone)`. `lightDir.w` is `cos(innerCone)`. Three values that would
otherwise need three more arrays.

**Cosines precomputed on the CPU.** The shader compares `dot(-L, dir)` against
`cos(cone)` — comparing cosines rather than taking `acos` per fragment. Doing
the `cosf` once per light per draw instead of once per fragment is thousands of
times fewer trig calls.

**`-2.0` as a sentinel.** A cosine is in [−1, 1], so −2 is unreachable and can
mean "this is a point light". The shader tests `if (cosOuter > -1.5)` to decide
whether to apply a cone. One float instead of a separate type array.

**Intensity premultiplied into colour** — the shader multiplies colour by
attenuation and N·L anyway, so folding intensity in costs nothing and saves a
per-fragment multiply.

The `if (count == 0 && uploadedCount == 0) return;` skips the whole upload when
neither this draw nor the last had lights, which is the common case in daylight
away from the pit lane.

### Where it is called from

```c
void RenderDrawLitMesh(Mesh mesh, Material material, Matrix transform, BoundingBox bounds)
{
    // In the depth pass only the silhouette matters, so the lighting work and
    // the material both go away.
    if (g_render.shadowPass) {
        DrawMesh(mesh, g_render.depthMaterial, transform);
        return;
    }

    Vector3 center = { (bounds.min.x + bounds.max.x) * 0.5f, /* ... */ };
    UploadLightsFor(center, Vector3Distance(center, bounds.max));
    DrawMesh(mesh, material, transform);
}
```

Once per draw call. A batch chunk gets the lights near that chunk; the terrain
gets the lights near that terrain chunk; a car gets the lights near that car,
including its own headlights.

The radius is the bounding sphere of the box — centre to a corner.

---

## Drawing a model with a tint, without corrupting it

```c
void RenderModelTransform(Model *model, Matrix m, Color tint)
{
    /* ... depth-pass early-out ... */

    // Approximate extent, used only to decide which lights are worth uploading.
    Vector3 position = { m.m12, m.m13, m.m14 };
    float reach = 1.2f * fmaxf(fabsf(m.m0) + fabsf(m.m4) + fabsf(m.m8),
                               fabsf(m.m2) + fabsf(m.m6) + fabsf(m.m10));
    UploadLightsFor(position, reach);

    for (int i = 0; i < model->meshCount; i++) {
        int matIndex = model->meshMaterial ? model->meshMaterial[i] : 0;
        // Material is a shallow struct whose `maps` array is shared with the
        // cached model, so tint by swapping the value in place and putting the
        // original back. Copying the struct and writing through it would
        // permanently darken the cached model a little more every frame.
        Material *mat = &model->materials[matIndex];
        Shader savedShader = mat->shader;
        Color base = mat->maps[MATERIAL_MAP_DIFFUSE].color;

        mat->shader = g_render.shader;
        mat->maps[MATERIAL_MAP_DIFFUSE].color = (Color){
            (unsigned char)((base.r * tint.r) / 255), /* ... */
        };
        DrawMesh(model->meshes[i], *mat, m);

        mat->maps[MATERIAL_MAP_DIFFUSE].color = base;
        mat->shader = savedShader;
    }
}
```

The comment records a real and instructive bug.

`Material` looks like a value type, but its `maps` field is a **pointer** to a
shared array. So `Material copy = *mat; copy.maps[0].color = tinted;` writes
through the shared pointer and modifies the cached model. Since the tint is a
multiply, the model gets darker every frame — six cars sharing one red model
would multiply the colour six times per frame until it is black within seconds.

Save, modify, draw, restore. Correct, and honest about why.

**The lesson generalises:** in C, a struct containing pointers is not a value. A
shallow copy shares everything the pointers reach. This is exactly the bug C++
copy constructors exist to prevent, and in C you must notice it yourself.

`reach` deserves a note. `m.m0, m.m4, m.m8` are the first row of the matrix;
`m.m2, m.m6, m.m10` the third. Summing absolute values gives an L1 bound on how
far the transform can stretch a unit vector along X and Z. It is a rough
overestimate, which is exactly right for a light-selection radius (err outward,
as always).

---

## The chase camera

```c
void ChaseCameraInit(ChaseCamera *cam, Vector3 focus, float yaw)
{
    /* ... */
    // High and only slightly behind: a top-down racer needs to show the corner
    // you are about to take, not the back of your own car.
    cam->distance = 3.4f;
    cam->height = 7.4f;
    cam->lookAhead = 1.7f;
    cam->rotateWithTarget = true;
    cam->positionSmoothing = 9.0f;
    cam->yawSmoothing = 5.0f;
    // At racing speed an 18% gradient climbs about 0.7 units a second, so this
    // rate settles into roughly a third of a unit of lag — enough to see, well
    // short of shoving the car out of frame.
    cam->heightSmoothing = 2.0f;
    cam->maxHeightLag = 0.6f;
    /* ... */
}
```

Height 7.4 against distance 3.4 — more than twice as high as it is far back.
That is the top-down look.

### Three smoothing rates, on purpose

```c
void ChaseCameraUpdate(ChaseCamera *cam, Vector3 target, float targetYaw, float speed01, float dt)
{
    float kp = SmoothFactor(cam->positionSmoothing, dt);
    cam->focus.x += (target.x - cam->focus.x) * kp;
    cam->focus.z += (target.z - cam->focus.z) * kp;

    // Height trails on its own slower rate, then is capped so a long descent
    // cannot leave the camera buried in the hill it just came down.
    float kh = SmoothFactor(cam->heightSmoothing, dt);
    cam->focus.y += (target.y - cam->focus.y) * kh;
    if (cam->maxHeightLag > 0.0f) {
        float lag = Clamp(cam->focus.y - target.y, -cam->maxHeightLag, cam->maxHeightLag);
        cam->focus.y = target.y + lag;
    }

    if (cam->rotateWithTarget) {
        float ky = SmoothFactor(cam->yawSmoothing, dt);
        cam->yaw += WrapAngle(targetYaw - cam->yaw) * ky;
        cam->yaw = WrapAngle(cam->yaw);
    } else {
        cam->yaw = 0.0f;
    }
    /* ... */
}
```

Position 9.0, yaw 5.0, height 2.0. The header explains the last:

```c
// Vertical follow is deliberately slower than the horizontal one. Tracking
// height exactly would cancel the elevation out: the car would sit at the
// same point on screen up a climb and down a descent alike. Trailing it
// instead lets the car ride up the frame as it climbs and sink as it drops.
```

This is a genuinely clever piece of design. A camera that tracks height perfectly
makes elevation *invisible* — the car stays at the same screen position whether
it is climbing Raidillon or plunging into Eau Rouge. Making the camera lag turns
elevation into something the player can see.

Chapter 07 established that an exponential ease against a moving target settles
to a constant lag — the *bug* in the car's height following. Here the same
behaviour is the *feature*. Same maths, opposite intent, and the difference is
whether the lag is wanted.

Then `maxHeightLag` caps it at 0.6 units so a long descent cannot bury the
camera in the hillside.

**`WrapAngle(targetYaw - cam->yaw)`** is essential. Interpolating from 3.1 rad to
−3.1 rad without wrapping takes the long way round: the camera spins 355° instead
of 5°. Chapter 05's wrap idiom, in radians.

### Speed-reactive framing

```c
// Pull back and rise a little with speed so fast sections read further ahead.
float dist = cam->distance * (1.0f + 0.30f * speed01);
float height = cam->height * (1.0f + 0.16f * speed01);
Vector3 forward = { sinf(cam->yaw), 0.0f, cosf(cam->yaw) };

Vector3 look = {
    cam->focus.x + forward.x * cam->lookAhead * speed01,
    cam->focus.y,
    cam->focus.z + forward.z * cam->lookAhead * speed01,
};

cam->camera.target = look;
cam->camera.position = (Vector3){ look.x - forward.x * dist, look.y + height,
                                  look.z - forward.z * dist };
```

Three speed-dependent adjustments, all scaled by `speed01`:

- **Pull back 30%** — more of the world in frame at speed.
- **Rise 16%** — a slightly more overhead view.
- **Bias the look-at point up to 1.7 units ahead** — the car sits lower in frame
  at speed, giving more road to look at.

All three go to zero at a standstill, so a parked car is centred and close.
Every racing game does some version of this, and it is worth being explicit that
these are three separate knobs rather than one "zoom" value.

---

## The ground quad

```c
void RenderGroundPlane(Vector3 center, float size, Color color)
{
    // Drawn as a real mesh rather than through rlgl's immediate mode: the
    // immediate batch runs raylib's default shader, so the ground would ignore
    // the sun and every placed light and stay flat bright at night.
    if (g_render.groundMesh.vertexCount == 0) return;
    /* ... scale a unit quad, build bounds, RenderDrawLitMesh ... */
}
```

A trap worth knowing about in any engine with an immediate-mode layer. raylib's
`DrawPlane` batches into an internal buffer flushed with raylib's *own* shader.
Anything drawn that way bypasses your lighting entirely — and the failure is
subtle in daylight and glaring at night.

The fix is to make a real `Mesh` and go through the same `RenderDrawLitMesh`
path as everything else. Note the same reasoning does *not* apply to the skid
marks and debug overlays, which use `DrawTriangle3D` and `DrawLine3D`
deliberately — they are meant to be unlit.

```c
// A single quad is enough: lighting is evaluated per fragment, so a large
// ground plane still picks up every nearby lamp.
static Mesh MakeGroundQuad(void)
```

Worth stating because it is not true of vertex lighting. With per-vertex
lighting a large quad would need subdividing to sample the light field; with
per-fragment lighting, six vertices is enough for any size.

---

## Exercises

1. **Watch the cull.** Press `F1` in game. `drawnLastFrame / chunkCount` is the
   cull ratio. Drive a lap and note the range. Then set `BATCH_CHUNK_SIZE` to
   1.0 and to 40.0 and compare both the ratio and the FPS. Explain the shape of
   the tradeoff.

2. **Disable culling.** Make `RenderFrustumTestBox` always return `true`. Measure
   FPS on both circuits with `--no-vsync`. How much did culling buy?

3. **Break the normal transform.** In `EmitProp`, use `world` instead of
   `normalMat` for normals. Find a prop in a level file, give it a non-uniform
   `"scale"` like `[2, 1, 1]`, and compare. Why does everything else look fine?

4. **Reproduce the material bug.** In `RenderModelTransform`, replace the
   save/restore with `Material copy = *mat; copy.maps[...].color = tinted;
   DrawMesh(model->meshes[i], copy, m);`. Run and watch the cars over ten
   seconds. Explain in terms of what `maps` is.

5. **Count the lights.** Instrument `UploadLightsFor` with counters for calls
   and for total lights uploaded. Run a night lap of circuit02 (32 lamps + 12
   headlights). What is the average lights-per-draw? How does it compare to
   uploading all 44 every time?

6. **Camera feel.** Set `heightSmoothing` to 9.0 (matching position). Drive
   circuit01's Raidillon climb. Then set it to 0.5. Describe both, and relate
   them to the constant-lag analysis in Chapter 07.

7. **Add per-chunk sorting.** Chunks are drawn in bake order. Sort them
   front-to-back by distance from the camera before drawing. Does FPS improve?
   (Hint: early-Z rejection.) Does it help more or less on the Pi than on a
   desktop?

---

Next: [11 — Shaders and shadow mapping](11-shaders-and-shadows.md)
