# 12 — Shaders, lights and shadows

> `scenes/env/day.tres` · `scenes/env/night.tres` ·
> `shaders/kit_relief.gdshader`. Mirrors
> [C chapter 11](../learn/11-shaders-and-shadows.md), which writes a shadow
> mapper from scratch.

---

## The problem

Three separate things, which the C chapter has to solve together because it owns
the whole pipeline:

1. **Lighting.** Flat-shaded untextured geometry needs a sun, an ambient term
   and a handful of placed lamps, with a fragment cost that does not grow with
   the number of lights in the level.
2. **Shadows.** A directional shadow map sharp enough to matter on a circuit far
   larger than one screenful, without acne and without detaching from its
   caster.
3. **Reading elevation from almost directly above.** A camera looking down at a
   17% slope sees a normal tilted ten degrees, which lights almost exactly like
   the flat beside it. Hills become invisible.

Godot solves (1) and (2). It does not solve (3), and (3) turns out to be the
most interesting third of the chapter.

---

## What Godot solves

**Shadow mapping** is a `DirectionalLight3D` with `shadow_enabled = true`. Four
parallel-split cascades, blur, soft shadows from an angular diameter, and a
`shadow_pancake_size` for the near plane problem — all parameters. The C
chapter's 300 lines of framebuffer setup, view-volume fitting, texel snapping
and PCF comparison collapse into an inspector panel.

**Forward lighting with many lights** is chapter 11's table: Forward+ clusters
them, Mobile and Compatibility have per-object limits. Either way you place
lights and stop thinking about uniform budgets.

**Fog, ambient, sky, tonemapping** are one `Environment` resource on a
`WorldEnvironment` node, swappable at runtime.

**Two shader dialects from one source** — the C chapter's GLSL 100 versus 330
problem — is gone. You write Godot's shading language once and the engine emits
whatever the backend needs.

What remains yours: the tuning, which at this project's scale is not optional,
and the relief shading, which is a game-specific idea no engine has.

---

## The scale problem, which is the whole tuning story

Godot's rendering defaults are written for a metres-and-people scale: a person
is 1.8 units, a room is 4, a street is 30. This project's scale is *one unit per
road tile*, so a car is 0.30 × 0.60 units and the entire circuit is about 120
units across.

Every default that has a length in it is therefore wrong by roughly an order of
magnitude, and the symptoms are confusing because nothing errors.

```gdscript
# scenes/env/day_light.gd — the values that matter, and why
func configure(sun: DirectionalLight3D) -> void:
    sun.shadow_enabled = true

    # Default 100.0. Our whole circuit is ~120 units across and the camera sees
    # maybe 25. Anything past that is wasting cascade resolution on geometry no
    # one can see; halving this roughly doubles the effective shadow texels per
    # unit.
    sun.directional_shadow_max_distance = 32.0

    # Default 4 splits. With a max distance of 32 and a camera that never tilts
    # far from overhead, the depth range on screen is small, so two splits are
    # plenty and each gets more resolution.
    sun.directional_shadow_mode = \
            DirectionalLight3D.SHADOW_PARALLEL_2_SPLITS
    sun.directional_shadow_split_1 = 0.15

    # Default bias 0.03 with normal bias 2.0 (Godot 4 defaults are tuned for
    # metre-scale geometry). A car is 0.30 wide; 0.03 of bias is a tenth of the
    # car, which detaches its shadow visibly — "peter-panning". These are
    # roughly an order of magnitude down, matching the scale.
    sun.shadow_bias = 0.004
    sun.shadow_normal_bias = 0.2

    # Angular diameter drives soft-shadow width. 0.5 degrees is the real sun;
    # larger reads as overcast and hides the remaining acne.
    sun.light_angular_distance = 0.6
```

**Bias is a tradeoff with exactly two failure modes**, and knowing which you are
looking at is the whole skill:

| Symptom | Cause | Fix |
|---|---|---|
| Striped or speckled self-shadowing on flat surfaces | Bias too small: a surface shadows itself due to depth quantisation | Raise `shadow_bias` |
| Shadows detached from their object, floating away from the base | Bias too large: the comparison is offset past the contact point | Lower `shadow_bias`, raise `shadow_normal_bias` |

`shadow_normal_bias` offsets along the surface normal instead of along the light
ray, which handles grazing angles — a low sun across a flat circuit, which is
this game's most common lighting — without the detachment that plain depth bias
causes. Prefer raising it before raising `shadow_bias`.

Two more scale-sensitive settings worth checking on any small-scale project:

- **`Camera3D.near`.** The default 0.05 is fine here, but it interacts with
  shadow precision: a small near plane spends depth precision you needed
  elsewhere. This project uses 0.1.
- **`Environment.fog_density`** and any `fog_depth_begin` / `fog_depth_end`,
  which are all in world units and default to metre-scale distances.

---

## Materials for the kit

The Kenney kit is untextured, flat-shaded, vertex-coloured geometry. The
material wants to be as cheap as it looks:

```gdscript
var material := StandardMaterial3D.new()
material.vertex_color_use_as_albedo = true          # per-instance tints (ch 11)
material.specular_mode = BaseMaterial3D.SPECULAR_DISABLED
material.diffuse_mode = BaseMaterial3D.DIFFUSE_LAMBERT
material.roughness = 1.0
material.metallic = 0.0
# Untextured flat geometry has no need for per-pixel normals, and vertex
# lighting is a large win on the Compatibility renderer (chapter 17).
material.shading_mode = BaseMaterial3D.SHADING_MODE_PER_VERTEX
```

`SHADING_MODE_PER_VERTEX` is the one to try early on a Pi. On low-poly kit
geometry the visual difference is small — most faces are flat and a few
hundred triangles — and the fragment cost falls sharply.

The kit's materials come from the glTF import. Chapter 05 recommends extracting
them to `.tres` at import time; the reason is here: 400 tree instances sharing
one material is one pipeline state, and 400 embedded copies of an identical
material is 400.

---

## The Godot shading language, briefly

If you know GLSL, the differences that matter:

```glsl
// shaders/kit_relief.gdshader
shader_type spatial;
render_mode diffuse_lambert, specular_disabled, world_vertex_coords;

// Set once per level from GDScript, shared by the terrain and the props so
// the road and the ground either side of it shade together. Declared under
// Project Settings -> Shader Globals.
global uniform float relief_low;
global uniform float relief_high;

uniform vec3 dip_tint : source_color = vec3(0.72, 0.74, 0.80);
uniform vec3 crest_tint : source_color = vec3(1.14, 1.12, 1.05);

varying float relief;

void vertex() {
    // world_vertex_coords puts VERTEX in world space, so this is the world
    // height without a matrix multiply of our own.
    relief = clamp((VERTEX.y - relief_low) / max(relief_high - relief_low, 0.001),
                   0.0, 1.0);
}

void fragment() {
    vec3 tint = mix(dip_tint, crest_tint, relief);
    ALBEDO = COLOR.rgb * tint;
    ROUGHNESS = 1.0;
    SPECULAR = 0.0;
}
```

| GLSL | Godot |
|---|---|
| `gl_Position`, `gl_FragColor` | `VERTEX` / `POSITION`, and `ALBEDO` / `EMISSION` / `ALPHA` fed to the engine's lighting |
| `uniform mat4 mvp` | `MODEL_MATRIX`, `VIEW_MATRIX`, `PROJECTION_MATRIX` provided |
| `varying vec3 v_normal;` in both stages | `varying` declared once at file scope |
| `#ifdef GL_ES` dialect switching | Nothing. One source, all backends |
| Custom lighting | Optional `void light()` function |
| Textures bound by hand | `uniform sampler2D tex : source_color, filter_linear_mipmap;` |

The important conceptual difference: a `spatial` shader normally **contributes
to** the engine's lighting rather than replacing it. You write `ALBEDO`,
`NORMAL`, `ROUGHNESS`; the engine does the lights, the shadows and the fog. That
is why the shader above is fifteen lines while the C engine's equivalent is
two hundred — the two hundred lines are the part Godot kept.

Adding `render_mode unshaded` opts out entirely, and then you are back to
writing the lighting yourself. Almost never what you want.

---

## Relief shading: the part Godot does not do

The problem, restated: this game is played from close to overhead. Lambert
shading of a surface whose normal has tilted ten degrees is nearly identical to
the flat beside it. So a circuit with 18% climbs *reads as flat*, the player
cannot see the hill they are about to lose speed on, and the elevation work in
chapters 08 and 13 is invisible.

Cartographers solved this in the nineteenth century, and the two techniques are
`relief` above and hillshading below.

### Hypsometric tinting

Tint by absolute height: dips cool and dark, crests warm and light. That is the
`mix(dip_tint, crest_tint, relief)` line, and the two colours are deliberately
not a simple brightness ramp — the dip is shifted blue and the crest yellow,
because the eye reads warm-forward and cool-back far more strongly than it reads
a 10% luminance difference.

The height range must be set **once per level, before building the terrain or
the batch**, so the road and the ground either side of it agree:

```gdscript
# scripts/main.gd, after loading a level
func _apply_relief_range(spline: TrackSpline) -> void:
    var lowest: float = INF
    var highest: float = -INF
    for p: Vector3 in spline.positions:
        lowest = minf(lowest, p.y)
        highest = maxf(highest, p.y)
    # A margin, so the extremes of the track are not pinned at pure tint.
    var margin: float = maxf(0.5, (highest - lowest) * 0.15)
    RenderingServer.global_shader_parameter_set("relief_low", lowest - margin)
    RenderingServer.global_shader_parameter_set("relief_high", highest + margin)
```

**Global shader parameters** are the Godot feature that makes this clean: one
value, declared in Project Settings → Shader Globals, readable by every shader
in the project with no per-material plumbing. The C engine passes the range to
each material by hand and has a comment about keeping them in sync; here there
is nothing to keep in sync.

Set them via `RenderingServer.global_shader_parameter_set`, not by editing the
project settings at runtime.

### Hillshade

Tinting shows *height*. It does not show *slope*, so a long even ramp looks like
a gradient wash rather than a hill. Hillshade adds a second, artificial light
from a fixed direction — traditionally the north-west — that responds only to
the surface normal:

```glsl
// Fixed-direction relief light, in addition to the scene's real sun. Purely a
// legibility aid: it does not move with the sun, does not cast, and is not
// affected by night. Without it a long even ramp reads as a colour wash rather
// than as a slope.
uniform vec3 hillshade_dir = vec3(0.55, 0.62, 0.42);
uniform float hillshade_strength : hint_range(0.0, 1.0) = 0.25;
// The normal is steepened well past reality before the relief light hits it.
// A real 17% slope tilts its normal by ten degrees, which is not enough of a
// difference to see; exaggerating by five gives each slope a lit face and a
// shaded one. Chapter 13 uses the same constant when it bakes the ground.
uniform float relief_exaggeration = 5.0;

void fragment() {
    vec3 n = normalize(vec3(world_normal.x * relief_exaggeration, 1.0,
                            world_normal.z * relief_exaggeration));
    // Measured against level ground, so flat terrain lands at zero and keeps
    // the material colour untouched.
    vec3 l = normalize(hillshade_dir);
    float shade = (dot(n, l) - l.y) * 0.5 + 0.5;
    vec3 tint = mix(dip_tint, crest_tint, relief);
    ALBEDO = COLOR.rgb * tint * mix(1.0, shade, hillshade_strength);
}
```

`* 0.5 + 0.5` remaps the dot product from [−1, 1] to [0, 1] so that a
back-facing slope darkens rather than clamping to black — this is a legibility
aid, not a light, and losing detail in the "shadow" would defeat it.

`hillshade_strength` at 0.25 is deliberately weak. Turn it to 1.0 to see what
you are doing, then turn it back down until it is felt rather than seen; past
about 0.35 it starts fighting the real sun and the scene looks flat-lit from two
directions at once.

`NORMAL` in `fragment()` is in **view** space, which is why the snippet above
reads `world_normal` — a `varying vec3` written in `vertex()` as
`NORMAL * mat3(MODEL_MATRIX)` (or simply `NORMAL` under
`render_mode world_vertex_coords`). A fixed world-space relief light that
silently swings with the camera is one of the easier mistakes to make here, and
it looks like a bug in the terrain rather than in the shader.

---

## Day and night

Two `Environment` resources and two light configurations, swapped:

```gdscript
# scripts/engine/time_of_day.gd
class_name TimeOfDay
extends Node

@export var world_environment: WorldEnvironment
@export var sun: DirectionalLight3D
@export var day: Environment
@export var night: Environment

## The level's lamps are placed for night. During the day they are switched
## off wholesale rather than dimmed: an OmniLight3D with zero energy still
## occupies a slot in the renderer's per-object light list, and on the
## Compatibility renderer that slot is scarce (chapter 11).
@export var lamps: Node3D

func set_night(enabled: bool) -> void:
    world_environment.environment = night if enabled else day
    sun.light_energy = 0.08 if enabled else 0.62
    sun.light_color = Color8(150, 170, 220) if enabled else Color8(255, 250, 235)
    sun.shadow_enabled = not enabled     # moonlight casting shadows reads oddly
    for lamp: Node in lamps.get_children():
        (lamp as Light3D).visible = enabled
```

The `Environment` carries sky, ambient, fog and tonemap:

| Property | Day | Night | Why |
|---|---|---|---|
| `background_mode` | Sky | Color | A night sky texture is wasted on a top-down camera |
| `ambient_light_source` | Sky | Color | Ambient from a dark colour, not from a bright sky |
| `ambient_light_energy` | 0.35 | 0.12 | The floor on how dark a shadow gets |
| `fog_enabled` | true | true | Distance fade at the edge of the visible circuit |
| `fog_density` | 0.008 | 0.02 | Units, so metre-scale defaults are far too strong |
| `tonemap_mode` | Filmic | Filmic | Consistent so a headlight does not clip differently at night |

`fog_density` is the one that bites at this scale. Godot's default of 0.01 is
per world unit; on a circuit 120 units across that is already noticeable, and
the metre-scale intuition ("0.01 is nothing") is wrong by the same factor as
everything else in this chapter.

Cross-fading between the two — rather than snapping — is a `Tween` over the
light energy plus a swap of the `Environment` at the midpoint, when the scene is
darkest and the change is least visible.

---

## Validating shaders you cannot run

The C chapter has a genuine problem here: its GLES2 shader variant cannot be
exercised on a headless machine, so it compiles both dialects statically in CI.

Godot's version of the problem is smaller but real: **shaders are compiled by
the rendering driver, and `--headless` has no rendering driver.** So
`godot --headless --quit` parses every script and imports every resource but
does *not* prove your shaders compile — and a shader that compiles under
Forward+ can still fail or behave differently under Compatibility.

Three things that do work:

**1. The editor reports compile errors immediately**, in the shader editor and
in the Errors panel. This catches syntax and most semantic errors the moment you
save.

**2. Run a smoke scene with the target driver.** A scene containing one quad per
material, one frame, then quit:

```sh
# tools/check_shaders.sh
# Renders one frame of a scene containing every material in the project, under
# each renderer we ship. Shader compilation happens in the driver, so this is
# the only thing standing between a Compatibility-only shader error and a
# build that fails at startup on the Pi.
set -e
for driver in vulkan opengl3; do
    godot --path . --rendering-driver "$driver" \
          --quit-after 2 res://tests/smoke/materials.tscn
done
```

On a headless Linux box this needs a virtual display for the `opengl3` pass —
`xvfb-run -a` in front of the command — because a GL context needs a display
even when nothing is shown.

**3. Ship the shader cache.** Godot compiles pipelines lazily by default, which
means the first time a material appears on screen the game stutters. Project
Settings → `rendering/shader_compiler/` has the knobs; the short version is that
running the smoke scene at startup, or before a race, warms them.

---

## Exercises

1. **Find both bias failures.** Set `shadow_bias` to 0.0 and photograph the
   acne. Set it to 0.2 and photograph the peter-panning. Then find the smallest
   value with no visible acne, and check it again with the sun low.

2. **Cascade budget.** Set `directional_shadow_max_distance` to 200, then 32,
   then 12. Screenshot the shadow edge under a car at each. At what point does
   the far scenery lose its shadow entirely, and does that matter for this
   camera?

3. **Turn off the relief.** Set `hillshade_strength` to 0 and both tints to
   white, then drive the hilly circuit. Time a lap. Turn them back on and time
   another five. Is the difference in your lap time, or only in your confidence?

4. **A third cartographic technique.** Add contour lines: darken fragments whose
   world height is within a small epsilon of a multiple of 0.25 units. Is it
   better or worse than hypsometric tinting for reading a slope at speed, and
   why?

5. **Scale audit.** Go through `Environment` and `DirectionalLight3D` and list
   every property whose units are world units. Predict which defaults are wrong
   at 1-unit-per-tile scale before checking each one.

6. **The compatibility gap.** Run the game under `--rendering-driver opengl3`
   and compare a screenshot with the Forward+ one. List every visible
   difference, then work out which are shader features and which are lighting
   limits from chapter 11's table.

---

Next: [13 — Terrain from a racing line](13-terrain.md)
