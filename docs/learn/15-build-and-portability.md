# 15 — Build system and portability

> `Makefile` — 173 lines. `tools/setup.sh`, `.gitmodules`.

---

## The problem

The project must build on:

- Linux with X11 or Wayland (Arch, Debian, Fedora)
- Windows, via MSYS2 MinGW64
- macOS (untested, but the rules are there)
- A Raspberry Pi rendering straight to DRM/KMS with no window system at all

The last two targets are the hard ones. The Pi build uses a *different graphics
API* (GLES2 instead of OpenGL 3.3), a *different platform backend*
(`PLATFORM_DRM` instead of `PLATFORM_DESKTOP`), and links a completely different
set of libraries.

And raylib must be built from source for each, because a prebuilt binary encodes
those choices.

---

## Why a Makefile

No CMake. No Meson. No zig. From the Makefile's own header:

```makefile
# raylib is a git submodule pinned to its 5.5 tag and is built from source by
# this Makefile, through raylib's own Makefile — no CMake, no zig. First time:
#
#   git submodule update --init
```

For a project of this size — 24 `.c` files, one library, four platform
configurations — a Makefile is 173 readable lines with no generator step, no
build directory ceremony, and no tool to install. `make` is already on every
machine that can compile C.

The scaling limit is real: a hundred targets, cross-compilation matrices,
generated sources, or a dependency graph you cannot hold in your head all argue
for a generator. This project has none of those.

---

## Host detection

```makefile
# Host detection. Windows sets OS in the environment even under MSYS2, where
# uname also works; everywhere else uname is the answer.
ifeq ($(OS),Windows_NT)
  HOST := Windows
else
  HOST := $(shell uname -s)
endif
```

Two mechanisms because neither works everywhere. `uname` is unavailable in a
bare `cmd.exe`; `$(OS)` is a Windows-only environment variable. Checking `$(OS)`
first and falling back to `uname` covers MSYS2 (where both work), Linux, and
macOS.

`:=` rather than `=` matters here: `:=` expands once, `=` re-expands on every
use — so `HOST = $(shell uname -s)` would fork a process every time `HOST` is
referenced.

---

## Two axes: platform and host

```makefile
PLATFORM ?= desktop
BUILD    := build/$(PLATFORM)
```

`?=` assigns only if not already set, so `make PLATFORM=drm` overrides it.

**`PLATFORM` is the target** (desktop or drm). **`HOST` is the machine you are
compiling on.** They are independent, and conflating them is a classic build
bug.

```makefile
ifeq ($(PLATFORM),drm)
  # Renders straight to /dev/dri with no window system. Linux only.
  ifneq ($(HOST),Linux)
    $(error PLATFORM=drm needs Linux with DRM/KMS; this host is $(HOST). Use the default \
            desktop build instead)
  endif
  RAYLIB_PLATFORM := PLATFORM_DRM
  RAYLIB_GRAPHICS := GRAPHICS_API_OPENGL_ES2
  PLATFORM_CFLAGS := -DENGINE_PLATFORM_DRM
  PLATFORM_LDLIBS := -lEGL -lGLESv2 -lgbm -ldrm -lm -lpthread -ldl -lrt
else
  RAYLIB_PLATFORM := PLATFORM_DESKTOP
  RAYLIB_GRAPHICS := GRAPHICS_API_OPENGL_33
  PLATFORM_CFLAGS :=
  ifeq ($(HOST),Windows)
    PLATFORM_LDLIBS := -lopengl32 -lgdi32 -lwinmm
    EXE := .exe
  else ifeq ($(HOST),Darwin)
    # Untested here; these are raylib's documented macOS frameworks.
    PLATFORM_LDLIBS := -framework OpenGL -framework Cocoa -framework IOKit \
                       -framework CoreVideo -framework CoreAudio
  else
    PLATFORM_LDLIBS := -lGL -lX11 -lXrandr -lXi -lXcursor -lXinerama \
                       -lm -lpthread -ldl -lrt
  endif
endif
```

**`$(error ...)` for an impossible combination.** `PLATFORM=drm` on Windows
cannot work — there is no `/dev/dri`. Failing at configure time with a message
that names the host and suggests the alternative is far better than a wall of
link errors twenty seconds later.

**`-DENGINE_PLATFORM_DRM`** is the one define that reaches the C code, and it
does exactly one thing:

```c
// engine/src/render.c
#if defined(ENGINE_PLATFORM_DRM)
    #define ENGINE_GLSL_VERSION 100
#else
    #define ENGINE_GLSL_VERSION 330
#endif
```

Which selects the GLSL dialect (Chapter 11). **That is the only platform
conditional in 3,400 lines of engine source.** Every other platform difference is
handled by the build system choosing different libraries and a
differently-configured raylib.

That is the goal to aim for: platform differences belong in the build, not
scattered through the source as conditionals.

**`EXE := .exe`** is appended to target names so `build/desktop/racer.exe` is
what Windows produces. Everywhere else it expands to nothing.

---

## Per-platform raylib

```makefile
RAYLIB_LIB := $(BUILD)/libraylib.a

# raylib compiles its objects in its own source directory and does not tag them
# by platform, so a desktop build and a DRM build would otherwise mix. Cleaning
# first costs a full rebuild, but only when this library is actually missing.
$(RAYLIB_LIB): $(RAYLIB_SRC)/raylib.h
	@mkdir -p $(BUILD)
	@echo "building raylib for $(RAYLIB_PLATFORM) ($(RAYLIB_GRAPHICS))"
	"$(MAKE)" -C $(RAYLIB_SRC) clean $(RAYLIB_MAKEFLAGS)
	"$(MAKE)" -C $(RAYLIB_SRC) $(RAYLIB_MAKEFLAGS)
```

The problem: raylib's own Makefile writes `.o` files next to its sources and
does not distinguish platforms. Build desktop, then build DRM, and the DRM link
picks up desktop objects — producing a binary that fails at startup with an
inscrutable GL error.

The fix is a `clean` before every build of the library. That is expensive, but
the rule's prerequisite is `$(BUILD)/libraylib.a`, which exists per platform. So
the clean-and-rebuild only happens when *that platform's* library is missing.
Switching back and forth between desktop and DRM costs one rebuild each way, and
repeated builds of the same platform cost nothing.

```makefile
# RAYLIB_RELEASE_PATH must be absolute: raylib's Makefile runs from its own
# directory. Objects stay in raylib's tree, which its .gitignore already covers,
# so building leaves the submodule clean.
RAYLIB_MAKEFLAGS := PLATFORM=$(RAYLIB_PLATFORM) GRAPHICS=$(RAYLIB_GRAPHICS) \
                    RAYLIB_RELEASE_PATH="$(abspath $(BUILD))" $(RAYLIB_SHELL)
```

`$(abspath ...)` because `make -C` changes directory, so a relative path would
resolve against raylib's source directory rather than the project root.

"Building leaves the submodule clean" is a genuinely important property: a
`git status` that shows the submodule as modified after every build trains you
to ignore `git status`.

### Two Windows-specific workarounds

```makefile
# raylib picks cmd.exe for its shell whenever OS=Windows_NT, which is wrong in
# an MSYS2 shell; its `clean` would then call `del`.
ifeq ($(HOST),Windows)
  RAYLIB_SHELL := PLATFORM_SHELL=sh
endif
```

Upstream's heuristic is right for a native Windows build and wrong under MSYS2,
where the shell is `sh` and `del` does not exist. Overriding the variable is the
supported escape hatch.

```makefile
# Both this and $(MAKE) are quoted at every use below. On Windows they routinely
# contain spaces and brackets — "C:/Program Files (x86)/GnuWin32/bin/make" — and
# an unquoted expansion is a shell syntax error, not a missing-file error.
```

`git log` records this as its own commit: `c6e069d Quote $(MAKE) so the raylib
build survives Windows toolchain paths`.

The failure mode is worth internalising. `C:/Program Files (x86)/.../make -C ...`
unquoted is parsed by the shell as the command `C:/Program` with arguments
`Files`, `(x86)/...` — and `(` is a shell metacharacter, so you get a *syntax
error*, not "command not found". Nothing in the message mentions the path.

**Quote every variable that could hold a path. On Windows, that is all of them.**

### A helpful error for a missing submodule

```makefile
# Present only when the submodule has not been checked out.
$(RAYLIB_SRC)/raylib.h:
	@echo ""
	@echo "raylib sources are missing — vendor/raylib is a git submodule."
	@echo "Run:  git submodule update --init"
	@echo ""
	@exit 1

submodule:
	git submodule update --init
```

A rule for a file that should already exist. If `vendor/raylib/src/raylib.h` is
present, `make` never runs it. If it is absent — the overwhelmingly common
first-time failure, because `git clone` without `--recurse-submodules` leaves
the directory empty — this fires instead of a hundred `#include` errors.

**When a prerequisite can plausibly be missing, give it a rule that explains
itself.**

---

## Compiler flags

```makefile
WARNINGS := -Wall -Wextra -Wno-unused-parameter -Wshadow -Wpointer-arith -Wcast-align \
            -Wstrict-prototypes -Wmissing-prototypes
```

Beyond the usual `-Wall -Wextra`:

| Flag | Catches |
|---|---|
| `-Wshadow` | a local hiding an outer variable of the same name |
| `-Wpointer-arith` | arithmetic on `void*`, a GNU extension |
| `-Wcast-align` | a cast that increases alignment requirements — a real crash on ARM |
| `-Wstrict-prototypes` | `f()` where `f(void)` was meant |
| `-Wmissing-prototypes` | a non-static function with no prototype, i.e. one that should have been `static` |

`-Wcast-align` matters specifically on this project's primary development
machine — an aarch64 Pi, where unaligned access is not merely slow.

`-Wmissing-prototypes` is the interesting one: it enforces the discipline that
every function is either declared in a header or marked `static`. That is what
keeps the module boundaries in Chapter 01 real rather than aspirational.

`-Wno-unused-parameter` is switched off because callback signatures routinely
ignore arguments — `MotorStreamCallback` does not use every parameter it is
handed.

```makefile
# -MMD -MP emit a .d file per object listing the headers it used, so editing a
# header rebuilds everything that includes it. Without this, changing a struct
# leaves stale objects reading fields at the wrong offsets.
CFLAGS   := -std=c11 -O2 -g $(WARNINGS) -MMD -MP $(PLATFORM_CFLAGS) \
            -Iengine/include -Igame/include -I$(RAYLIB_SRC)
```

```makefile
-include $(ENGINE_OBJ:.o=.d) $(GAME_OBJ:.o=.d)
```

**Automatic header dependencies**, and the comment names exactly why it matters.
Add a field to the middle of `Car` and rebuild only `car.o`: every other object
still has the old offsets compiled in. The result is not a compile error — it is
a program that reads `velocity` where `yaw` now lives. Hours of debugging for a
missing flag.

`-MMD` emits a `.d` file per object listing the headers it included (the second
`M` skips system headers). `-MP` adds a phony target for each header so that
*deleting* a header does not break the build with "no rule to make target".

`-include` (with the leading dash) includes them if they exist and stays quiet
if they do not — necessary for the very first build, when no `.d` files exist
yet.

**`-O2 -g` together.** Optimised *and* with debug symbols. The physics runs at
120 Hz for six cars and the terrain build is 160 million iterations, so `-O0`
would be painful; and a crash without symbols is a waste of everyone's time.
Stepping through optimised code is imperfect but backtraces are exact.

---

## Sources and objects

```makefile
ENGINE_SRC := $(wildcard engine/src/*.c)
GAME_SRC   := $(wildcard game/src/*.c)
TEST_SRC   := $(wildcard tests/*.c)

ENGINE_OBJ := $(ENGINE_SRC:%.c=$(BUILD)/%.o)
GAME_OBJ   := $(GAME_SRC:%.c=$(BUILD)/%.o)
# Game code minus its entry point, so tests can drive a whole race in-process.
GAME_LIB   := $(filter-out $(BUILD)/game/src/main.o,$(GAME_OBJ))
```

`$(wildcard)` means adding a `.c` file requires no Makefile edit. The usual
objection — that a stale generated file gets silently compiled in — does not
apply to a hand-maintained source tree.

`$(SRC:%.c=$(BUILD)/%.o)` is substitution reference syntax: it maps
`engine/src/foo.c` to `build/desktop/engine/src/foo.o`. Mirroring the source
tree under `build/` means no name collisions between `engine/src/render.c` and a
hypothetical `game/src/render.c`.

The `filter-out` line is Chapter 14's whole foundation, in one expression.

```makefile
$(BUILD)/%.o: %.c | $(RAYLIB_SRC)/raylib.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
```

The `|` marks an **order-only prerequisite**: `raylib.h` must exist before
compiling, but its timestamp does not force a rebuild. Without the `|`, checking
out a newer raylib would rebuild every object in the project.

`@mkdir -p $(dir $@)` creates the output directory on demand — simpler than
declaring directory targets, and idempotent.

---

## Targets

```makefile
.PHONY: all run clean test level engine shaders raylib submodule

all: $(TARGET)

# Engine objects only — handy while the game layer is in flux.
engine: $(ENGINE_OBJ)

test: $(TEST_BIN)
	./$(TEST_BIN)

# Validates both GLSL variants. The GLES2 one cannot be exercised by running the
# game on a machine with no display, so it is checked statically instead.
shaders:
	@tools/check_shaders.sh

run: $(TARGET)
	./$(TARGET)

level:
	blender --background --python tools/blender/build_demo_track.py

clean:
	rm -rf build
	@if [ -f $(RAYLIB_SRC)/Makefile ]; then \
	    "$(MAKE)" -C $(RAYLIB_SRC) clean $(RAYLIB_MAKEFLAGS) >/dev/null 2>&1 || true; \
	fi
```

**`.PHONY`** declares targets that are not files. Without it, a file named
`test` in the working directory would make `make test` report "up to date".

**`clean` guards and forgives.** `if [ -f ... ]` skips raylib's clean when the
submodule is absent; `|| true` means a failure there does not fail the whole
target. Cleaning is a best-effort operation and should never be the thing that
blocks you.

**`level` regenerates content through Blender.** A content pipeline step as a
first-class build target — so regenerating both circuits is `make level`, not a
paragraph in a wiki.

---

## The setup script

```sh
# tools/setup.sh
# One-time setup: check out the raylib submodule, report any missing system
# packages, and fetch the Kenney kit if the models are not already present.
#
# raylib itself is built by the top-level Makefile, from raylib's own Makefile.
# Nothing here compiles anything.
set -euo pipefail
```

**"Nothing here compiles anything"** is the key design statement. The script
does not duplicate the build; it prepares the environment and gets out of the
way. Setup scripts that also build inevitably drift from the real build.

```sh
if command -v pacman >/dev/null 2>&1; then
    PKGS="base-devel mesa libx11 libxrandr libxi libxcursor libxinerama alsa-lib"
    [[ -e /dev/dri ]] && PKGS="$PKGS libdrm mesa"
    echo "Arch: sudo pacman -S --needed $PKGS"
elif command -v apt-get >/dev/null 2>&1; then
    MISSING=()
    for pkg in build-essential libgl1-mesa-dev /* ... */; do
        dpkg -s "$pkg" >/dev/null 2>&1 || MISSING+=("$pkg")
    done
    if (( ${#MISSING[@]} )); then
        echo "Debian/Ubuntu/Raspberry Pi OS:"
        echo "  sudo apt-get install -y --no-install-recommends ${MISSING[*]}"
    else
        echo "all Debian packages present"
    fi
elif command -v dnf >/dev/null 2>&1; then
    /* ... */
else
    echo "unrecognised distribution — install a C toolchain plus the GL, X11 and ALSA headers"
fi
```

Two things worth copying.

**It detects the package manager, not the distribution.** `command -v pacman` is
more robust than parsing `/etc/os-release`, and it works on derivatives the
author never heard of.

**It prints the command rather than running it.** No `sudo` from a setup script.
The user sees exactly what would be installed and decides. On Debian it goes
further and checks each package with `dpkg -s`, so the suggested command lists
only what is genuinely missing.

The fallback branch is honest: an unrecognised distribution gets a description
of what is needed rather than silence.

`set -euo pipefail` — exit on error, error on unset variable, and propagate
failures through pipes. Three flags that turn shell from a footgun into
something you can rely on.

---

## The DRM path

```
make PLATFORM=drm
./build/drm/racer
```

Rendering directly to `/dev/dri/card0` with no X server, no Wayland compositor,
no window manager. The game becomes the display, like a console.

The README is scrupulous about the caveats:

> This needs a display connected — it opens `/dev/dri/card0` directly and will
> report `No suitable DRM connector found` on a headless machine. Run it from
> the console rather than over SSH, or the process cannot become DRM master.

> The DRM target compiles, links and initialises GLES2 here, but it has **not**
> been run against a real panel in this repository's development environment
> (no monitor attached). The desktop path is fully exercised.

**Stating what is untested is part of documenting it.** A reader who hits a
problem on DRM now knows they are in territory the author could not verify,
rather than assuming their setup is broken.

"DRM master" is worth a note: only one process may control a DRM device's mode
setting at a time, and only from a session that owns the VT. Over SSH there is
no VT, so the call fails. This surprises everyone once.

The two build trees are kept apart:

> The desktop and DRM builds each get their own raylib, configured differently
> (OpenGL 3.3 vs GLES2) and kept in `build/<platform>/libraylib.a`, so you can
> switch between them without a full rebuild.

---

## What GLES2 costs

The DRM target constrains the renderer in ways the desktop one does not, and
they are scattered through the earlier chapters:

| Constraint | Where it shows up |
|---|---|
| GLSL 100 syntax | `scene_shader.inc`'s macro layer (Ch. 11) |
| No default float precision | the `precision highp/mediump` block (Ch. 11) |
| Loop bounds must be constant | `for (i < MAX_LIGHTS) { if (i >= lightCount) break; }` (Ch. 11) |
| ~16 guaranteed fragment uniform vectors | `LIGHTS_PER_DRAW = 8` (Ch. 10) |
| 8 guaranteed texture units | the shadow map bound to slot 1 (Ch. 11) |
| `OES_depth_texture` optional | `shadowsAvailable`, blob-shadow fallback (Ch. 11) |
| Weak fill rate | static batching, frustum culling (Ch. 10) |

None of these are gratuitous. Each is a documented GLES2 guarantee, and each
produced a specific design decision that the desktop build also lives with —
usually at no cost, occasionally at a small one.

**Targeting the weakest platform first tends to produce a better design for all
of them.** Eight lights per draw is not a limitation anyone notices; it is just
a bound that made the light-selection code necessary, and light selection is
better than uploading 64 lights everywhere.

---

## Exercises

1. **Prove the dependency tracking works.** Run `make`. Add a field to the
   middle of `struct Car` in `car.h`. Run `make` again and watch which files
   recompile. Now delete `-MMD -MP` from `CFLAGS` and the `-include` line, run
   `make clean && make`, and repeat. What recompiles the second time?

2. **Switch platforms.** Run `make`, then `make PLATFORM=drm`, then `make`
   again. Watch which steps rebuild each time and explain it in terms of
   `$(BUILD)/libraylib.a`.

3. **Trigger the error.** Run `make PLATFORM=drm HOST=Windows`. Read the
   message. Then find a way to produce the same class of failure without the
   `$(error)` guard and compare how comprehensible it is.

4. **Count the ifdefs.** Search `engine/` and `game/` for `#if defined` and
   `#ifdef`. How many are platform conditionals? Compare with a codebase you
   know that handles portability in the source.

5. **Add a target.** Write `make sanitize` that rebuilds into
   `build/asan/` with `-fsanitize=address,undefined -O1 -g` and runs the tests.
   (Note: `MEMORY.md` records that ASan is unavailable on this particular Pi —
   so also make the target fail with a clear message when the sanitiser
   runtime is missing, rather than a link error.)

6. **Cross-compile.** Add a `PLATFORM=web` branch using Emscripten
   (`PLATFORM_WEB`, `GRAPHICS_API_OPENGL_ES2`). Which of the GLES2 constraints
   in the table above already apply? What in `main.c` would have to change for a
   browser's event loop?

7. **Time the build.** `time make clean && time make -j$(nproc)`. Where does the
   time go — raylib, or the project? What does that suggest about whether the
   `clean` before each raylib build is worth optimising?

---

Next: [16 — Exercises](16-exercises.md)
