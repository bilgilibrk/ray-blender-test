# 11 — Shaders and shadow mapping

> `engine/src/scene_shader.inc` · the shadow half of `engine/src/render.c` ·
> `tools/check_shaders.sh`

---

## Two GLSL dialects from one source

The desktop build targets OpenGL 3.3 (GLSL 330). The Raspberry Pi console build
targets GLES2 (GLSL 100). These are genuinely different languages:

| | GLSL 100 (GLES2) | GLSL 330 |
|---|---|---|
| Vertex inputs | `attribute` | `in` |
| Vertex outputs | `varying` | `out` |
| Fragment inputs | `varying` | `in` |
| Fragment output | `gl_FragColor` | a declared `out vec4` |
| Texture sampling | `texture2D()` | `texture()` |
| Precision | must be declared | defaults exist |

Maintaining two copies of a fragment shader is a guarantee that they will drift,
and the drift will be discovered on the platform you cannot easily test.

The fix is preprocessor-level:

```c
// engine/src/scene_shader.inc
#if ENGINE_GLSL_VERSION == 100
    #define GLSL_VERT_HEADER "#version 100\n"
    #define GLSL_FRAG_HEADER "#version 100\n" \
                             "#ifdef GL_FRAGMENT_PRECISION_HIGH\n" \
                             "precision highp float;\n" \
                             "#else\n" \
                             "precision mediump float;\n" \
                             "#endif\n"
    #define GLSL_ATTRIBUTE   "attribute"
    #define GLSL_VARY_OUT    "varying"
    #define GLSL_VARY_IN     "varying"
    #define GLSL_FRAG_DECL   ""
    #define GLSL_FRAG_COLOR  "gl_FragColor"
    #define GLSL_TEXTURE     "texture2D"
#else
    #define GLSL_VERT_HEADER "#version 330\n"
    #define GLSL_FRAG_HEADER "#version 330\n"
    #define GLSL_ATTRIBUTE   "in"
    #define GLSL_VARY_OUT    "out"
    #define GLSL_VARY_IN     "in"
    #define GLSL_FRAG_DECL   "out vec4 finalColor;\n"
    #define GLSL_FRAG_COLOR  "finalColor"
    #define GLSL_TEXTURE     "texture"
#endif
```

The shader body is one string, assembled from adjacent string literals:

```c
static const char *kVertexShader =
    GLSL_VERT_HEADER
    GLSL_ATTRIBUTE " vec3 vertexPosition;\n"
    GLSL_ATTRIBUTE " vec3 vertexNormal;\n"
    GLSL_ATTRIBUTE " vec4 vertexColor;\n"
    "uniform mat4 mvp;\n"
    "uniform mat4 matModel;\n"
    "uniform mat4 matNormal;\n"
    GLSL_VARY_OUT " vec4 fragColor;\n"
    GLSL_VARY_OUT " vec3 fragNormal;\n"
    GLSL_VARY_OUT " vec3 fragWorld;\n"
    "void main() {\n"
    "    fragColor = vertexColor;\n"
    "    fragNormal = normalize(vec3(matNormal*vec4(vertexNormal, 0.0)));\n"
    "    fragWorld = vec3(matModel*vec4(vertexPosition, 1.0));\n"
    "    gl_Position = mvp*vec4(vertexPosition, 1.0);\n"
    "}\n";
```

C's adjacent-string-literal concatenation means `"a" "b"` is `"ab"` at compile
time, with no runtime cost. The `#define`s slot into the same sequence.

The header states the goal:

> The GLSL 330 and GLSL 100 variants share one body; only the qualifiers and the
> output differ. Keeping them as one string stops the desktop and GLES2 paths
> from drifting apart.

**The precision block** is GLES-specific and mandatory: GLES2 fragment shaders
have no default float precision, so a shader without a `precision` declaration
fails to compile. The `#ifdef GL_FRAGMENT_PRECISION_HIGH` guard requests `highp`
where the hardware has it and falls back to `mediump` where it does not, and the
comment says why it matters here:

> The shadow compare comes out of a perspective divide near 1.0 and is tested
> against a bias of about 0.0016, which is finer than mediump can resolve there.

`mediump` guarantees only about 10 bits of mantissa. Near 1.0 that gives a
resolution around 0.001 — the same order as the bias. Shadow comparisons would
become noise.

**Why embed the source rather than load a `.glsl` file?**

> Embedded rather than loaded from disk so a level can be rendered without any
> files beyond the models themselves.

No shader directory to ship, no path to get wrong, no possibility of a mismatch
between the binary and the shader files beside it.

### Passing a C constant into GLSL

```c
#define STR(x) #x
#define XSTR(x) STR(x)

static const char *kFragmentShader =
    GLSL_FRAG_HEADER
    "#define MAX_LIGHTS " XSTR(LIGHTS_PER_DRAW) "\n"
    /* ... */
```

The two-level stringify macro. `#x` turns a macro *parameter* into a string
literal, but it does not expand the argument first — so `STR(LIGHTS_PER_DRAW)`
would give the literal text `"LIGHTS_PER_DRAW"`. Wrapping in a second macro
forces expansion before stringification, so `XSTR(LIGHTS_PER_DRAW)` gives `"8"`.

The payoff is that `LIGHTS_PER_DRAW` is defined once in `light.h` and used by
the C selector, the C upload arrays and the GLSL loop bound. They cannot
disagree.

---

## The scene shader

### Vertex stage

```glsl
fragColor  = vertexColor;
fragNormal = normalize(vec3(matNormal * vec4(vertexNormal, 0.0)));
fragWorld  = vec3(matModel * vec4(vertexPosition, 1.0));
gl_Position = mvp * vec4(vertexPosition, 1.0);
```

Four lines, three outputs.

**`vec4(normal, 0.0)` versus `vec4(position, 1.0)`.** The `w` component is the
difference between a *direction* and a *point*. With `w = 1`, the matrix's
translation column applies; with `w = 0` it does not. A normal must not be
translated — moving a surface does not change which way it faces. This is the
whole point of homogeneous coordinates and it is easy to get wrong.

`matNormal` is the inverse-transpose from Chapter 10, supplied by raylib because
`render.c` registers the location:

```c
g_render.shader.locs[SHADER_LOC_MATRIX_NORMAL] = GetShaderLocation(g_render.shader, "matNormal");
```

**`fragWorld`** is the world-space position, needed by the fragment stage for
point-light distances, fog and the shadow lookup. Computing it in the vertex
shader and interpolating is much cheaper than reconstructing it per fragment.

### Fragment stage — lighting

```glsl
vec3 n = normalize(fragNormal);
vec3 sunL = -normalize(sunDir);
float ndl = max(dot(n, sunL), 0.0);
// Ambient, key light, and a weak sky term so upward faces lift a little.
vec3 lighting = ambient.rgb + sunColor.rgb*sunColor.a*ndl*SunReach(n, sunL)
              + ambient.rgb*0.35*max(n.y, 0.0);
```

`normalize(fragNormal)` again — interpolating unit vectors across a triangle
does not preserve unit length, so a per-fragment renormalise is required.

`ndl` is **Lambertian diffuse**: brightness proportional to the cosine of the
angle between the surface normal and the direction to the light, clamped at zero
so back-faces are dark rather than negative.

`sunL = -sunDir` because `sunDir` is the direction the light *travels* and the
lighting equation wants the direction *to* the light.

The **sky term** `ambient.rgb * 0.35 * max(n.y, 0.0)` is a one-line hemisphere
approximation: upward-facing surfaces get a little extra, as if lit by the sky
dome. It costs one multiply and does a lot to keep flat-shaded geometry from
looking like cardboard.

```glsl
for (int i = 0; i < MAX_LIGHTS; i++) {
    if (i >= lightCount) break;
    vec3 toLight = lightPosRange[i].xyz - fragWorld;
    float dist = length(toLight);
    float range = lightPosRange[i].w;
    if (dist >= range) continue;
    vec3 L = toLight/max(dist, 0.0001);
    // Squared linear falloff: cheap, and reaches exactly zero at the range.
    float atten = 1.0 - dist/range;
    atten *= atten;
    float cosOuter = lightColor[i].w;
    if (cosOuter > -1.5) {
        float cd = dot(-L, normalize(lightDir[i].xyz));
        float cosInner = lightDir[i].w;
        atten *= clamp((cd - cosOuter)/max(cosInner - cosOuter, 0.001), 0.0, 1.0);
    }
    lighting += lightColor[i].rgb*max(dot(n, L), 0.0)*atten;
}
```

**`for (i = 0; i < MAX_LIGHTS; i++) { if (i >= lightCount) break; }`** rather
than `i < lightCount` directly. GLSL 100 requires loop bounds to be
compile-time constants so the compiler can unroll. The dynamic break inside is
permitted and does the same job.

**Squared linear falloff.** Physically correct is inverse-square, `1/d²`, which
never reaches zero — so a light influences every fragment in the scene forever
and you need an arbitrary cutoff that produces a visible edge.

`(1 − d/r)²` reaches exactly zero at `d = r`, smoothly (its derivative is also
zero there, which is why it is squared rather than linear). No discontinuity, no
arbitrary cutoff, and the range becomes a meaningful authoring parameter.

And it is the same formula `LightInfluence` uses on the CPU, as Chapter 10
stressed.

**The cone test.** For a spot light, `cd` is the cosine of the angle between the
fragment direction and the beam axis. Then:

```glsl
atten *= clamp((cd - cosOuter)/max(cosInner - cosOuter, 0.001), 0.0, 1.0);
```

A linear ramp in *cosine space* between the inner and outer cones: 1 inside the
inner cone, 0 outside the outer, smooth between. Comparing cosines avoids any
inverse trig. Note that cosine decreases as angle increases, so `cosInner > cosOuter`
and the ratio has the right sign.

**`if (cosOuter > -1.5)`** is the point/spot discriminator from Chapter 10:
`-2.0` is not a valid cosine, so it is a safe sentinel.

**`max(dist, 0.0001)`** guards a fragment exactly at a light's position.

### Fog

```glsl
vec4 base = fragColor*colDiffuse;
vec3 lit = base.rgb*lighting;
float d = length(cameraPos - fragWorld)*fogDensity;
float f = clamp(1.0 - exp(-d*d), 0.0, 1.0);
finalColor = vec4(mix(lit, fogColor.rgb, f), base.a);
```

**Exponential-squared fog**, `1 − e^(−(d·k)²)`. Compared with linear fog it has
no start/end parameters and no visible onset — it ramps in gently and saturates
smoothly. The squaring makes near distances almost fog-free and far distances
saturate faster than plain exponential.

`fogColor` is set to the sky colour in `RenderSetSettings`, so distant geometry
fades into the horizon rather than into a grey band.

**`base = fragColor * colDiffuse`** is where the two colour sources multiply.
`fragColor` is the vertex colour — for batched props, the baked material colour
times the prop tint times the relief shade (Chapter 10). `colDiffuse` is the
material uniform — used by the terrain for its ground colour and by
`RenderModelTransform` for car tints.

**Alpha passes through unfogged.** `base.a` is used directly, so a translucent
object does not become opaque in the distance.

---

## Shadow mapping

### The idea

> Render the scene from the light's point of view, storing depth. Then, when
> shading a fragment, transform it into the light's space and compare its depth
> against the stored value. If the stored depth is nearer, something is between
> the fragment and the light — it is in shadow.

Simple to state, and famously fiddly in practice. Everything below is about the
fiddliness.

### A depth-only framebuffer

```c
// A framebuffer with a depth texture and no colour attachment. raylib's own
// LoadRenderTexture gives depth as a renderbuffer, which cannot be sampled.
static void InitShadowMap(void)
{
    g_render.depthShader = LoadShaderFromMemory(kDepthVertexShader, kDepthFragmentShader);
    if (g_render.depthShader.id == 0) {
        TraceLog(LOG_WARNING, "RENDER: depth shader failed to compile, shadows off");
        return;
    }
    /* ... */
    RenderTexture2D map = { 0 };
    map.id = rlLoadFramebuffer();
    if (map.id == 0) { /* warn, return */ }
    map.texture.width = SHADOW_MAP_SIZE;
    map.texture.height = SHADOW_MAP_SIZE;

    rlEnableFramebuffer(map.id);
    map.depth.id = rlLoadTextureDepth(SHADOW_MAP_SIZE, SHADOW_MAP_SIZE, false);
    map.depth.format = 19;      // DEPTH_COMPONENT_24BIT
    rlFramebufferAttach(map.id, map.depth.id, RL_ATTACHMENT_DEPTH, RL_ATTACHMENT_TEXTURE2D, 0);

    bool complete = rlFramebufferComplete(map.id);
    rlDisableFramebuffer();

    if (!complete) {
        // GLES2 without OES_depth_texture lands here: rlLoadTextureDepth falls
        // back to a renderbuffer, which will not attach as a texture.
        TraceLog(LOG_WARNING, "RENDER: shadow map incomplete, shadows off");
        rlUnloadFramebuffer(map.id);
        return;
    }

    g_render.shadowMap = map;
    g_render.shadowsAvailable = true;
    g_render.shadowsEnabled = true;
}
```

**Renderbuffer versus texture.** A renderbuffer is write-only storage for
rendering; a texture can be sampled by a shader. raylib's convenience
`LoadRenderTexture` allocates depth as a renderbuffer, which is faster but
useless here — the whole point is to read the depth back. Hence the manual
`rlLoadTextureDepth` + `rlFramebufferAttach`.

**No colour attachment at all.** Nothing is written but depth, so a colour buffer
would be wasted bandwidth on a Pi. This is legal in GL as long as you do not
sample from a colour attachment.

**Graceful degradation.** Every failure path warns and returns with
`shadowsAvailable` still false. GLES2 without `OES_depth_texture` is a real
configuration — plenty of embedded GPUs — and the game must run there. It falls
back to painted blob shadows:

```c
// game/src/main.c
// The painted blob stands in only when there is no real shadow to
// cast one; drawing both would double up under every car.
if (!RenderShadowsEnabled()) {
    for (int i = 0; i < stage.race.racerCount; i++) {
        DrawShadow(&stage.race.racers[i], &stage.race.tuning);
    }
}
```

`render.h` documents the contract:

> False when the depth attachment could not be made sampleable, which is the
> case on GLES2 hardware without OES_depth_texture. Draws still work; there are
> just no shadows to draw.

**Feature detection at runtime with a working fallback** is the right shape for
anything GPU-dependent.

### The depth-pass shader

```c
static const char *kDepthVertexShader =
    GLSL_VERT_HEADER
    GLSL_ATTRIBUTE " vec3 vertexPosition;\n"
    "uniform mat4 mvp;\n"
    "void main() {\n"
    "    gl_Position = mvp*vec4(vertexPosition, 1.0);\n"
    "}\n";

static const char *kDepthFragmentShader =
    GLSL_FRAG_HEADER
    GLSL_FRAG_DECL
    "void main() {\n"
    "    " GLSL_FRAG_COLOR " = vec4(1.0);\n"
    "}\n";
```

The vertex shader does the minimum: position to clip space. No normals, no
colour, no world position — the depth buffer is filled by the rasteriser, not by
the shader.

The fragment shader exists only because a program needs one to link. The comment
is candid:

> There is no colour attachment to write to, so the fragment shader exists
> purely to be a legal one.

---

### Fitting the light's view volume

```c
// Square, and big enough that a car 0.6 units long still spans tens of texels
// across the box below.
#define SHADOW_MAP_SIZE 2048

// Half-width of the world box the map covers, centred on the focus. Has to
// outrun what the chase camera can see down a straight, or shadows would pop
// in at the top of the screen.
#define SHADOW_BOX_EXTENT 17.0f

// Depth range along the sun. Wide enough that a caster well above or below the
// focus still lands inside, but no wider: the whole range shares the depth
// buffer's precision, and it is that precision the bias below is measured in.
#define SHADOW_BOX_DEPTH 44.0f

// How dark a fully occluded fragment's key light goes. Short of 1 so shadows
// stay translucent rather than turning the ground to a silhouette.
#define SHADOW_STRENGTH 0.72f
```

Every one of these is a tradeoff, and the comments state which.

The circuits are 95 and 200+ units long. A shadow map covering the whole level
at 2048² would give texels several units across — larger than a car. So the map
covers a **34-unit box that follows the player**. Geometry outside is simply lit,
which is fine because it is off screen.

`SHADOW_BOX_EXTENT = 17` must exceed what the camera can see. `main.c` also
leads the box:

```c
// How far ahead of the car the shadow box is centred. The chase camera shows
// much more road in front than behind, so a box centred on the car itself
// spends half its resolution on tarmac nobody is looking at.
#define SHADOW_LEAD 4.0f
```

**Depth range is a precision budget.** A 24-bit depth buffer spread over 44
units gives about 2.6 µunits per step — but depth is non-linear and the bias
below is measured in *normalised* depth, so widening the range directly costs
shadow accuracy. Too narrow and a caster falls outside and stops casting. 44 is
the compromise.

### Texel snapping

```c
void RenderBeginShadowPass(Vector3 focus)
{
    if (!RenderShadowsEnabled()) return;

    Vector3 dir = Vector3Normalize(g_render.settings.sunDirection);
    // A sun pointing straight down would make the usual up vector degenerate.
    Vector3 up = (fabsf(dir.y) > 0.999f) ? (Vector3){ 0.0f, 0.0f, 1.0f }
                                         : (Vector3){ 0.0f, 1.0f, 0.0f };
    float back = SHADOW_BOX_DEPTH * 0.5f;

    // Snap the box to whole shadow texels. Without this the map is re-rasterised
    // against a slightly different grid every frame and every shadow edge in the
    // scene crawls as the car moves.
    Matrix rough = MatrixLookAt(Vector3Subtract(focus, Vector3Scale(dir, back)), focus, up);
    float texel = (2.0f * SHADOW_BOX_EXTENT) / (float)SHADOW_MAP_SIZE;
    Vector3 inLight = Vector3Transform(focus, rough);
    inLight.x = floorf(inLight.x / texel) * texel;
    inLight.y = floorf(inLight.y / texel) * texel;
    // Snapping happens in the plane across the sun, which moving the eye along
    // the sun cannot disturb, so one round trip is enough to settle it.
    Vector3 snapped = Vector3Transform(inLight, MatrixInvert(rough));

    Matrix view = MatrixLookAt(Vector3Subtract(snapped, Vector3Scale(dir, back)), snapped, up);
    Matrix proj = MatrixOrtho(-SHADOW_BOX_EXTENT, SHADOW_BOX_EXTENT,
                              -SHADOW_BOX_EXTENT, SHADOW_BOX_EXTENT,
                              0.01f, SHADOW_BOX_DEPTH);
    g_render.lightVP = MatrixMultiply(view, proj);

    g_render.shadowPass = true;
    BeginTextureMode(g_render.shadowMap);
    rlClearScreenBuffers();
    rlEnableDepthTest();
    rlSetMatrixProjection(proj);
    rlSetMatrixModelview(view);
}
```

**Texel snapping is the single most important trick for a moving shadow map**,
and it is worth understanding exactly.

The map is a 2048² grid over a 34-unit box. The box follows the car, so it moves
by fractions of a texel every frame. Each frame, the same static geometry — a
barrier, a tree — is rasterised against a *slightly different* grid, so which
texels it covers changes by one here and there. The shadow's edge shifts by a
texel and then shifts back. Across a whole scene that reads as every shadow edge
shimmering, and it is far more distracting than a slightly blurrier shadow.

The fix: quantise the box's position to whole texels. Transform the focus into
light space, `floor` its X and Y to a multiple of the texel size, transform back,
and build the real matrices around the snapped point. Now the grid is *the same
grid* frame to frame; it advances in whole-texel jumps and the edges are stable.

The comment about "one round trip is enough" is a correctness argument: snapping
happens in the two axes perpendicular to the sun, and the eye is then moved
along the sun axis, which cannot change the perpendicular components. So the
snapped point stays snapped and no iteration is needed.

**Orthographic projection** because the sun is a directional light — its rays are
parallel, so there is no perspective. `MatrixOrtho` with symmetric bounds.

**The degenerate up vector.** `MatrixLookAt` fails when the view direction is
parallel to the up vector. A sun pointing straight down (`dir.y = -1`) with
`up = (0,1,0)` is exactly that case, producing a matrix full of `NaN`. Switching
to `(0,0,1)` fixes it. Cheap insurance against a level that specifies a noon sun.

### The comparison

```glsl
// How much of the sun reaches this fragment: 1 fully lit, 0 fully shadowed.
// Only the key light is occluded — ambient, the sky term and the placed
// lamps all still reach into the shadow, which is what keeps it readable
// rather than a black hole.
float SunReach(vec3 n, vec3 l) {
    if (shadowStrength <= 0.0) return 1.0;
    vec4 lp = lightVP*vec4(fragWorld, 1.0);
    vec3 proj = lp.xyz/lp.w*0.5 + 0.5;
    // Outside the map the sun is unoccluded: the box only covers the play area
    // around the camera, and anything beyond it must not be shadowed by chance.
    if (proj.z > 1.0 || proj.x < 0.0 || proj.x > 1.0 ||
        proj.y < 0.0 || proj.y > 1.0) return 1.0;
    // Slope-scaled bias: a surface edge-on to the sun crosses many depth texels
    // within one of its own, and needs far more slack than one facing it. The
    // floor is set above the 0.06-unit lip where the ground meets the tarmac,
    // which is otherwise close enough to the bias to strobe in and out of
    // shadow along every kerb in the level.
    float bias = max(0.0035*(1.0 - dot(n, l)), 0.0016);
    float lit = 0.0;
    // 3x3 PCF, which softens the edge enough to hide the texel grid.
    for (int x = -1; x <= 1; x++) {
        for (int y = -1; y <= 1; y++) {
            vec2 at = proj.xy + vec2(float(x), float(y))*shadowTexel;
            float d = texture(shadowMap, at).r;
            lit += (proj.z - bias > d) ? 0.0 : 1.0;
        }
    }
    return mix(1.0, lit/9.0, shadowStrength);
}
```

Six ideas.

**`lp.xyz/lp.w*0.5 + 0.5`.** The perspective divide takes clip space to NDC,
which is [−1, 1]. Texture coordinates and depth are [0, 1]. The `*0.5 + 0.5`
remaps. (For an orthographic projection `w` is already 1, but writing the divide
keeps the code correct if the projection ever changes.)

**Out-of-range means lit, not shadowed.** Critical. The box covers only the area
around the player; sampling outside it would clamp or wrap to arbitrary texels
and produce shadows in random places across the rest of the circuit. Returning
1.0 means "beyond the shadow map, assume sunlit", which is exactly the correct
default.

**Slope-scaled bias** is the classic fix for **shadow acne**. Here is the
problem: the depth map stores one depth per texel, but a texel covers a finite
area of the surface. On a surface angled away from the light, the true depth
varies a lot across that area. So half the fragments in the texel have depth
greater than the stored sample and shadow themselves — producing dark stripes
across every lit surface.

The fix is to subtract a small bias before comparing. But a *constant* bias is
wrong in both directions: too small for steeply-angled surfaces (acne remains),
too large for face-on surfaces (shadows detach from their casters — "peter
panning").

`0.0035 * (1 − dot(n, l))` scales with the angle: near zero when the surface
faces the light, up to 0.0035 when edge-on. Exactly proportional to how much the
depth varies across a texel.

The floor of `0.0016` is justified against a specific piece of level geometry:
the 0.06-unit lip where tarmac meets ground. Without the floor, that step is
close enough to the computed bias that the comparison flickers, and every kerb
in the level strobes.

**3×3 PCF** (Percentage Closer Filtering) samples nine texels and averages the
binary results. Note it averages the *comparisons*, not the depths — averaging
depths and then comparing once would give a wrong answer at any depth
discontinuity. The result is a 4-texel-wide soft edge instead of a hard
staircase.

Nine texture fetches per fragment is the main cost of shadows here. On a Pi that
is measurable, which is why `--no-shadows` exists.

**`mix(1.0, lit/9.0, shadowStrength)`** with strength 0.72 means a fully
shadowed fragment still gets 28% of the sun. The comment explains: fully black
shadows read as holes in the ground rather than as shade. And it is only the
*key light* that is attenuated — ambient, the sky term and every placed lamp are
computed outside `SunReach` and reach into shadows unimpeded. That is physically
motivated (real shadows are lit by skylight and bounce) and is what makes night
headlights work normally under a shadow.

### Binding the shadow map by hand

```c
// Bound by hand: the sampler is not one of the material's own maps, so
// raylib will not bind it for us on each DrawMesh. Slot 1 rather than
// anything higher because GLES2 only guarantees eight texture units, and
// DrawMesh leaves it alone: it touches a slot only where the material has
// a texture, and nothing in this scene fills a map beyond the diffuse.
int slot = 1;
rlActiveTextureSlot(slot);
rlEnableTexture(g_render.shadowMap.depth.id);
rlSetUniform(g_render.locShadowMap, &slot, SHADER_UNIFORM_INT, 1);
rlActiveTextureSlot(0);
```

A good example of the kind of detail that separates working GPU code from
almost-working GPU code. raylib binds textures that belong to a `Material`'s
`maps` array; the shadow map does not, so it must be bound manually — once per
frame in `RenderBeginScene`, since nothing else disturbs slot 1.

The reasoning about *which* slot is spelled out: GLES2 guarantees only 8 texture
units, and the analysis of what `DrawMesh` does confirms slot 1 is safe.

---

## Two passes, one draw sequence

```c
// game/src/main.c
// Depth pass first: the same geometry, seen from the sun. Centred a
// little ahead of the car so the box covers the road being driven into
// rather than the one already behind.
Vector2 lead = CarForward(&player->car);
RenderBeginShadowPass((Vector3){ player->car.position.x + lead.x * SHADOW_LEAD,
                                player->car.height,
                                player->car.position.y + lead.y * SHADOW_LEAD });
    TerrainDraw(&stage.terrain, camera.camera);
    StaticBatchDraw(&stage.batch, camera.camera);
    for (int i = 0; i < stage.race.racerCount; i++) {
        DrawRacer(&stage.race.racers[i], CAR_SCALE);
    }
RenderEndShadowPass();
```

Chapter 10 covered how the mode is hidden in `g_render.shadowPass` so the same
calls do the right thing in both passes. The mechanism, in three places:

```c
// StaticBatchDraw: skip decal chunks
if (shadowPass && !batch->chunks[i].castsShadow) continue;

// RenderDrawLitMesh: swap the material, skip lighting
if (g_render.shadowPass) { DrawMesh(mesh, g_render.depthMaterial, transform); return; }

// RenderFrustumFromCamera: cull against the sun
if (g_render.shadowPass) return FrustumFromMatrix(g_render.lightVP);
```

And what is *not* drawn in the depth pass is as deliberate:

```c
// Rubber lies in the road surface, so it goes down after the road
// and before the cars, and never in the depth pass above, where a
// flat decal has nothing to cast and only fights the tarmac.
SkidDraw(&skid);
```

Same reasoning as the decal chunks: a flat quad coplanar with the ground would
add acne and cast nothing.

The ordering in the colour pass is: terrain, props, skid marks, blob shadows (if
no real ones), cars, debug overlays. Skid marks after the road they lie on and
before the cars that lay them.

---

## Relief shading: seeing hills from above

A problem specific to a top-down camera, and the solution is cartographic rather
than photographic.

```c
// engine/include/engine/render.h
// A camera looking almost straight down cannot show elevation on its own: a
// 17% slope tilts its normal by ten degrees and so lights almost exactly like
// the flat beside it. Everything baked into a mesh is therefore tinted by how
// high it sits, dips darkening and crests lightening, which is what lets a
// climb read as a climb from above.
void RenderSetReliefRange(float lowest, float highest);
float RenderReliefHeight01(float y);
```

The observation is exact: `atan(0.17) ≈ 9.6°`. A normal tilted ten degrees from
vertical, lit by a sun that is also nearly overhead, produces an `N·L` within a
couple of percent of the flat ground beside it. Elevation is real in the
simulation and invisible on screen.

Two devices, both baked into vertex colours at load time.

**Height tint**, applied to props in `EmitProp`:

```c
// engine/src/render.c
#define RELIEF_PROP_DEPTH 0.30f

float shade = 1.0f - RELIEF_PROP_DEPTH * (1.0f - RenderReliefHeight01(p.y));
```

The lowest point of the track is 30% darker than the highest, with everything in
between interpolated. It reads as aerial perspective and it carries the broad
shape of the land.

The comment explains the ceiling:

> Vertex colours multiply the material colour, so this can only ever subtract
> light — going above 1.0 would clip to white and flatten the crests back out.

**Hillshade**, applied to terrain in `terrain.c` (Chapter 12), fakes a low sun
against exaggerated normals to give each slope a lit face and a shaded one.

`main.c` sets the range before either mesh is baked:

```c
// The track's own elevation range drives the relief tint, and both the
// batch and the terrain have to be told before they bake their vertices.
float lowest = 1e30f, highest = -1e30f;
for (int i = 0; i < stage->spline.count; i++) { /* ... min/max ... */ }
RenderSetReliefRange(lowest, highest);
```

Per-level normalisation, so a flat circuit is not tinted at all and a hilly one
uses the full range. `RenderReliefHeight01` returns a neutral 0.5 when no range
is set.

---

## Validating shaders you cannot run

```sh
# tools/check_shaders.sh
# Compiles the scene shader for both graphics backends and validates the GLSL.
#
# The DRM/GLES2 target needs a display to run, so on a headless machine this is
# the only thing standing between a typo in the GLSL 100 variant and a build
# that fails at startup on real hardware.
```

The problem: the GLES2 shader is a string in a C file. Nothing compiles it until
the game runs on a Pi with a monitor attached. A typo would ship.

The solution is neat. Generate a tiny C program that includes the same
`scene_shader.inc` with the DRM define set, and simply prints the strings to
files:

```c
#include "engine/light.h"
#if defined(ENGINE_PLATFORM_DRM)
    #define ENGINE_GLSL_VERSION 100
#else
    #define ENGINE_GLSL_VERSION 330
#endif
#include "scene_shader.inc"

int main(int argc, char **argv)
{
    /* ... fputs(kVertexShader, v); fputs(kFragmentShader, f); ... */
}
```

Then run `glslangValidator` on the output:

```sh
for target in desktop drm; do
    define=""
    [[ $target == drm ]] && define="-DENGINE_PLATFORM_DRM"
    gcc -std=c11 $define -I... -o "$WORK/dump_$target" "$WORK/dump.c"
    "$WORK/dump_$target" "$WORK/$target.vert" "$WORK/$target.frag"
    if glslangValidator "$WORK/$target.vert" "$WORK/$target.frag" > "$WORK/$target.log" 2>&1; then
        echo "ok"
    else
        echo "FAILED"; sed 's/^/    /' "$WORK/$target.log"; status=1
    fi
done
```

The generated program is the *same preprocessor path* the engine uses, so what
is validated is exactly what would ship. And it degrades politely:

```sh
if ! command -v glslangValidator >/dev/null 2>&1; then
    echo "glslangValidator not found — skipping (sudo apt-get install glslang-tools)"
    exit 0
fi
```

`make shaders` runs it.

**The general technique — extract the artefact through the real build path, then
validate it with a static tool — applies to anything you cannot execute in CI.**
Shaders, SQL, regexes, config schemas, generated code.

---

## Exercises

1. **See the acne.** Set the bias floor in `SunReach` to `0.0` and the slope
   term to `0.0`. Run and look at the ground. Then set the bias to `0.02` and
   look at where the shadows sit relative to the cars. Name both artefacts.

2. **See the crawl.** Delete the texel-snapping block in `RenderBeginShadowPass`
   and build `view` directly from `focus`. Drive slowly along a straight and
   watch the shadow edges of the barriers.

3. **PCF cost.** Change the 3×3 loop to a single tap, then to 5×5. Measure FPS
   with `--no-vsync` on each. Compare the visual difference at 1080p and at
   `--width 640 --height 360`.

4. **Break the GLES2 shader.** Introduce a deliberate typo inside the shared
   shader body — say `texture2D` where `GLSL_TEXTURE` should be. Run
   `make shaders`. Which target fails, and why does the desktop build still run?

5. **Shadow box size.** Set `SHADOW_BOX_EXTENT` to 6.0, then 60.0. Describe what
   goes wrong in each case and relate it to texel density.

6. **Add a second cascade.** Real engines use cascaded shadow maps: several
   boxes at different scales, chosen per fragment by depth. Sketch what would
   have to change in `RenderBeginShadowPass`, in the uniforms, and in
   `SunReach`. What does the GLES2 uniform budget say about whether it fits?

7. **Prove the sky term matters.** Remove `ambient.rgb*0.35*max(n.y, 0.0)` from
   the lighting sum and take a screenshot with `F2`. Compare with one before.
   What specifically got worse?

---

Next: [12 — Terrain from a racing line](12-terrain.md)
