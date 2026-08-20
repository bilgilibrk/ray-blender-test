# 17 — Export and platforms

> `export_presets.cfg` · `project.godot` · `tools/export.sh`. Mirrors
> [C chapter 15](../learn/15-build-and-portability.md), which is a 200-line
> Makefile and a lot of `ifeq`.

---

## The problem

The C series ships to Linux, Windows, macOS and a Raspberry Pi console, and pays
for it with a Makefile that detects the host, picks a raylib build per platform,
works around two MSYS2 quirks, and maintains a separate GLES2 shader dialect.

Godot replaces almost all of that with export templates: precompiled engine
binaries for every platform, into which your project is packed. `make` becomes a
button.

What is left is the part the C version handles by *not* having the problem:
choosing a renderer, and being honest about which machines Godot can actually
reach.

---

## Export, in one command

```sh
# tools/export.sh
set -e
mkdir -p build/linux build/windows

godot --headless --path . --export-release "Linux/X11"  build/linux/racer
godot --headless --path . --export-release "Windows"    build/windows/racer.exe
```

The preset names are the ones in `export_presets.cfg`, which the editor writes
when you configure an export. That file **belongs in version control** — it is
project configuration, not a build artefact — with one caveat: it stores
signing credentials and keystore paths for Android and iOS, which do not. Godot
splits those into `export_credentials.cfg`; add that one to `.gitignore`.

`--headless` matters for CI: exporting does not need a display, and without it
the command will try to open a window.

### What the export actually does

1. Collects every resource the project references, converts text resources to
   binary (the *Convert Text Resources To Binary* preset option, on by default),
   and packs them into a `.pck`.
2. Copies the export template for the target platform.
3. Embeds the `.pck` in the executable, or ships it alongside.
4. Copies platform-specific extras — `.gdextension` libraries for the matching
   platform tags, and anything matched by the preset's include filter.

Two consequences worth knowing:

**Anything not *referenced* is not exported.** A scene loaded only by a string
built at runtime — `load("res://levels/" + name + ".track")` — is invisible to
the dependency scanner and will be missing at runtime with a "resource not
found" error that never occurs in the editor. Fix it with the preset's
`include_filter` (`levels/*.track`) or with an explicit `preload` in a manifest
script.

**Only the matching native library ships.** The `.gdextension` file from chapter
16 lists a path per platform-and-architecture tag; the exporter copies the ones
that match the target. If your Linux ARM64 build is missing the extension,
compare the tag in the `.gdextension` with the arch the preset is building.

---

## The three renderers

This is the decision that replaces the C series' GLES2-versus-GL33 shader
dialects, and it is made once per project — Project Settings →
`rendering/renderer/rendering_method`.

| | Forward+ | Mobile | Compatibility |
|---|---|---|---|
| API | Vulkan | Vulkan | OpenGL 3.3 / ES 3.0 / WebGL 2 |
| Lights | Clustered, hundreds | Forward, per-object limits | Forward, tighter limits |
| Shadows | 4 cascades, soft | 4 cascades | Cascades, no soft shadows |
| SDFGI, SSAO, SSR, volumetric fog | Yes | No | No |
| Targets | Desktop GPUs | Phones, tablets, older desktop | Old hardware, Raspberry Pi, the web |

You can override at launch — `--rendering-method gl_compatibility
--rendering-driver opengl3` — which is invaluable for testing, and the project
setting has a `.mobile` and `.web` override so one project can pick per
platform:

```ini
; project.godot
[rendering]

renderer/rendering_method="forward_plus"
renderer/rendering_method.mobile="gl_compatibility"
renderer/rendering_method.web="gl_compatibility"
```

**What this project uses**: Forward+ on desktop, Compatibility everywhere else.
The visual difference on a flat-shaded kit racer is small — no screen-space
effects were being used anyway — and the parts that do differ are exactly the
ones chapters 11 and 12 flag: per-object light limits, and no soft shadows.

That is the whole "two shader dialects" problem, solved by the engine. The
project's one custom shader (chapter 12's relief tint) is fifteen lines of
standard Godot shading language and compiles unchanged on all three.

---

## The Raspberry Pi, honestly

The C engine has a `PLATFORM=drm` build that renders directly to KMS/DRM with
GLES2, on a bare TTY, with no desktop running. It is genuinely a console: boot
to a black screen, run the binary, race.

**Godot 4 cannot do that.** There is no KMS/DRM display driver; Godot needs
X11 or Wayland to open a window. `--headless` runs without a display but also
without rendering, which is chapter 15's world, not a game.

So the Pi story is:

| | C engine | Godot |
|---|---|---|
| Bare TTY, no desktop | `make PLATFORM=drm` | Not possible |
| Under X11/Wayland | Works | Works, Compatibility renderer |
| Pi 5 / Pi 4 (V3D, Mesa) | Comfortable | Playable; measure before promising 60 fps |
| Pi 3 and older | Works on GLES2 | Godot 4 dropped GLES2. Effectively out |

If a bare-metal Pi console is a requirement, that requirement selects the C
engine, and no amount of Godot tuning changes it. This series is upfront about
that in its README and it is worth repeating here rather than burying.

For a Pi running a desktop, the settings that matter most:

```ini
; project.godot — the .pi feature-tag overrides, applied via a custom
; feature tag set on the Pi export preset.
[rendering]
renderer/rendering_method.mobile="gl_compatibility"
anti_aliasing/quality/msaa_3d=0
lights_and_shadows/directional_shadow/soft_shadow_filter_quality=0
textures/default_filters/anisotropic_filtering_level=0

[display]
window/vsync/vsync_mode=1
window/size/viewport_width=1280
window/size/viewport_height=720
```

And in the game itself, the two levers with the largest effect for the least
loss:

- **`SHADING_MODE_PER_VERTEX`** on the kit material (chapter 12). Low-poly flat
  geometry barely notices; the fragment cost drops sharply.
- **Chunked `MultiMesh`** (chapter 11). On desktop, chunking costs more than it
  saves. On the Pi it is the difference between submitting the whole circuit
  every frame and submitting the part you can see.

Both are one line, both are measured with
`Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME` and the frame time graph, and
both are the sort of thing to decide with a number rather than a feeling.

---

## Feature tags

The mechanism that makes per-platform settings work, and the thing that most
repays five minutes of reading.

Any project setting can be suffixed with a feature tag, and the tagged variant
wins when that feature is present: `rendering_method.mobile`,
`window/size/viewport_width.web`, `audio/driver/mix_rate.android`. Built-in tags
cover platform, architecture, debug/release and renderer; custom tags are
declared per export preset.

In code:

```gdscript
if OS.has_feature("pi"):            # custom tag, set on the Pi preset
    prop_batcher.chunk_size = 16.0
    kit_material.shading_mode = BaseMaterial3D.SHADING_MODE_PER_VERTEX

if OS.has_feature("web"):
    Audio.buffer_seconds = 0.25     # browsers are less forgiving of underruns

if OS.is_debug_build():
    debug_overlay.visible = true
```

`OS.is_debug_build()` is the one to prefer over checking `debug` as a feature
tag — it is clearer and it is what stripped assertions key off too.

---

## One input map, four devices

The C engine has `input.h`: an action-based layer so the game never queries a
device, and an autopilot can substitute for a human by writing the same fields.
Godot's `InputMap` is the same idea, done in the editor.

Actions are declared in Project Settings → Input Map, and each carries any
number of events — keys, mouse buttons, joypad buttons, joypad axes — so one
action covers every device without a line of code:

| Action | Keyboard | Gamepad |
|---|---|---|
| `throttle` | `W`, `Up` | Right trigger (axis) |
| `brake` | `S`, `Down` | Left trigger (axis) |
| `steer_left` / `steer_right` | `A` `D`, `Left` `Right` | Left stick X (axis) |
| `handbrake` | `Space` | `A` / cross |
| `pause` | `P`, `Escape` | Start |

```gdscript
# scripts/game/player_input.gd
## Reads the input map into the same CarInput the AI produces, so nothing
## downstream knows or cares which produced it — chapter 10's autopilot is
## exactly this substitution.
func poll(out: CarInput) -> void:
    # get_action_strength reads analogue triggers as 0..1 and digital keys as
    # 0 or 1, so a keyboard and a gamepad feed the same field.
    out.throttle = Input.get_action_strength("throttle")
    out.brake = Input.get_action_strength("brake")
    # get_axis is (negative_action, positive_action) and already applies the
    # deadzone configured per event in the input map.
    out.steer = Input.get_axis("steer_left", "steer_right")
    out.handbrake = Input.is_action_pressed("handbrake")
```

Three details that matter across platforms:

**Deadzones are per action**, set in the Input Map dialog. The default of 0.5 is
far too large for steering — it makes the first half of the stick's travel do
nothing. 0.15 to 0.2 is a sensible starting point for an analogue axis.

**Analogue triggers are axes, not buttons.** Mapping throttle to a trigger's
axis rather than its button is what makes partial throttle possible, and it is
the single biggest difference between a gamepad that feels good and one that
feels like a keyboard.

**Touch needs a different layer entirely.** `Input.get_action_strength` will not
invent a steering axis on a phone. Mobile needs on-screen controls (a
`TouchScreenButton` or a `Control` with `_gui_input`) that write into the same
`CarInput`. Because the boundary is `CarInput` rather than the input system,
adding touch touches one file.

---

## Size, and what is in the 40 MB

A Godot export is roughly 40–60 MB before your content, because the template
contains the whole engine: physics, navigation, rendering backends, the GLTF
importer, WebRTC, the theme system, and everything else you did not use.

The C racer is a 2 MB binary plus assets.

If size matters — a web build, a jam submission, an embedded device — the answer
is a **custom export template**: build the engine yourself with modules
disabled.

```sh
# In a Godot source checkout
scons platform=linuxbsd target=template_release \
      module_navigation_enabled=no \
      module_webrtc_enabled=no \
      module_multiplayer_enabled=no \
      module_gltf_enabled=no \
      deprecated=no
```

`module_gltf_enabled=no` is safe here because glTF is an *import-time* format:
the exported game ships imported meshes, not `.glb` files. Removing the importer
does not remove the models. That kind of reasoning is what makes template
trimming productive rather than a game of guess-and-crash.

Realistically this halves the binary rather than approaching the C engine's
size, and it costs you a Godot build environment plus a rebuild per engine
upgrade. Worth it for the web; rarely worth it for desktop.

---

## The web

The one platform where everything above changes at once:

- **Compatibility renderer only** (WebGL 2).
- **GDExtension needs a matching Web template** and a threads-enabled build.
  This is the most likely place for chapter 16's kernels to be unavailable,
  which is exactly why chapter 16 insists on the GDScript fallback.
- **Audio is stricter.** Browsers refuse to start audio before a user gesture,
  and underruns are more audible. Chapter 14's generator wants a larger
  `buffer_length` here.
- **No filesystem.** `user://` is IndexedDB, asynchronous, and can fail.
- **Download size is the whole first impression.** A 40 MB template is a slow
  first load even before your levels.

None of that is a reason to skip the web — a browser-playable racer is worth a
lot — but it is a second QA target, not a free export.

---

## CI

```yaml
# .github/workflows/export.yml (sketch)
jobs:
  export:
    runs-on: ubuntu-latest
    container: barichello/godot-ci:4.3     # editor + templates preinstalled
    steps:
      - uses: actions/checkout@v4
        with: { submodules: recursive }

      - name: Build native kernels
        run: cd native && scons platform=linux target=template_release

      - name: Import project
        # First run imports every asset and parses every script. With
        # chapter 02's warnings promoted to errors, a type mistake fails here.
        run: godot --headless --path . --quit

      - name: Tests
        run: godot --headless --path . --script res://tests/run_tests.gd

      - name: Export
        run: |
          mkdir -p build/linux
          godot --headless --path . --export-release "Linux/X11" build/linux/racer
```

The import step is doing double duty and it is easy to miss: it populates
`.godot/imported/` (which is gitignored, so CI has to generate it) *and* it is
the type check from chapter 02. A project that imports cleanly with warnings as
errors has no untyped declarations anywhere.

Do not commit `.godot/`. Do commit `.import` files and `export_presets.cfg`.

---

## What the Makefile was actually for

Worth a closing comparison, because it clarifies what an engine buys you.

The C series' Makefile does five things:

1. Detects the host OS and picks a compiler and flags. → **Gone.** Export
   templates are prebuilt.
2. Builds raylib per platform, cleaning between platform switches. → **Gone.**
3. Manages dependency tracking so editing a header rebuilds its dependents. →
   **Gone.** No compilation.
4. Selects a shader dialect per graphics backend. → **Gone.** One shading
   language.
5. Provides a `test` target that links the engine without a window. → **Kept**,
   as chapter 15's `--headless --script`.

Four out of five disappear, and the fifth is easier. In exchange you accept the
engine's decisions about what a build *is*, and you lose the ability to target a
machine the engine does not support — which, for this project, is precisely the
bare-metal Pi console.

That is the trade in one sentence, and it is worth restating whenever somebody
asks whether to write an engine: **you are not choosing between writing more
code and writing less. You are choosing which set of decisions is yours.**

---

## Exercises

1. **Export three ways.** Export the project for your platform with Forward+,
   Mobile and Compatibility. Screenshot the same corner in each and list every
   visible difference. Which of them would a player notice?

2. **Find the missing resource.** Load a level with
   `load("res://levels/" + name + ".track")`, export, and run the exported
   build. Confirm it fails and the editor did not. Then fix it two ways —
   `include_filter` and a preload manifest — and argue for one.

3. **Trim a template.** Build a custom export template with navigation, WebRTC
   and glTF disabled. Measure the size before and after, and confirm the game
   still runs. Which module did you expect to matter most, and did it?

4. **Deadzone.** Set the steering action's deadzone to 0.5, then 0.05, then
   0.18, and drive a lap with a gamepad each time. Describe the feel of each in
   one sentence.

5. **The Pi budget.** On a Pi (or with the Compatibility renderer and a frame
   cap that emulates one), find the frame time for: the default build, with
   per-vertex shading, with chunking at 16 units, and with both. Which single
   change buys the most?

6. **Reproduce the C engine's console build, or prove you cannot.** Try to run
   the exported Godot build on a Pi with no desktop session. Document exactly
   where it fails and what would have to exist in Godot for it to work.

---

Next: [18 — Exercises](18-exercises.md)
