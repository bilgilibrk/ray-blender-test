# 16 — Writing the hot paths in C

> `native/` — plain C11 kernels behind a minimal GDExtension. No C-series
> equivalent: the C series is *entirely* this chapter.

---

## The rule

> C is a conclusion, not a starting point.

Nothing gets written in C because it sounds like it needs to be fast. It gets
written in C after a profiler names it, and after the cheaper fixes — a better
algorithm, a search hint, moving the work to import time — have been tried.

That is not a moral position, it is an economic one. A GDExtension costs you: a
build toolchain, a compile step, a binary per platform per architecture per
build type, a debugging story that involves attaching to a running Godot, and a
class of crash (segfault) that GDScript cannot produce. Those costs are real and
they are permanent.

The benefit is a constant factor on a small amount of code. Take it where the
factor is large and the code is small.

---

## Profile first

Three tools, in the order you should reach for them.

**1. The editor profiler.** Debugger → Profiler, with *Measure* set to Frame
Time, and the Script Functions list sorted by self time. This tells you which
GDScript functions dominate, which is the question you actually have. It needs
the game to be running from the editor with the debugger attached, and it adds
overhead — so it tells you *proportions*, not absolute times.

**2. `Performance` monitors**, for the coarse split:

```gdscript
# scripts/debug/perf_overlay.gd
func _process(_delta: float) -> void:
    label.text = "phys %.2f ms  proc %.2f ms  draws %d" % [
        Performance.get_monitor(Performance.TIME_PHYSICS_PROCESS) * 1000.0,
        Performance.get_monitor(Performance.TIME_PROCESS) * 1000.0,
        Performance.get_monitor(Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME)]
```

If `TIME_PHYSICS_PROCESS` dominates, this chapter. If `TIME_PROCESS` dominates,
look at the HUD and the camera. If neither does and the frame is still long, it
is rendering, and chapter 11 is where to go.

**3. A microbenchmark for the specific function**, run headless so the editor
overhead is out of the picture:

```gdscript
# tests/bench/bench_spline.gd
## Run: godot --headless --path . --script res://tests/bench/bench_spline.gd
extends SceneTree

func _init() -> void:
    var level: LevelData = load("res://levels/circuit02.track") as LevelData
    var spline := TrackSpline.new()
    spline.build_from_level(level, 0.25)
    var out := SplineQuery.new()
    var point := Vector3(4.0, 0.0, -11.0)

    var start: int = Time.get_ticks_usec()
    for i: int in 100_000:
        spline.closest_into(point, out.index, out)
    var micros: int = Time.get_ticks_usec() - start
    print("closest_into: %.3f us/call" % (float(micros) / 100_000.0))
    quit()
```

Run that on your **slowest target**, not your development machine. A Pi and a
desktop will not agree about what is worth porting, and the Pi is the one with a
frame budget.

---

## The three candidates, and what the arithmetic says

Before measuring, it is worth counting — the counts alone eliminate two of the
five things you might have guessed.

| Candidate | Work per second | Verdict |
|---|---|---|
| `TrackSpline.closest_into` | 6 cars × 2 calls × 120 Hz × ~240 segment projections ≈ **350,000 projections/s** | **Port it** |
| `TrackSpline.sample_into` (AI probes) | 6 × 7 × 120 ≈ 5,000 calls/s, each a short walk | Port it — same file, nearly free to include |
| `TerrainBuilder.height_from_spline` | 102,000 grid samples × 4,000 spline samples ≈ **408,000,000 iterations**, once per bake | **Port it** |
| `Manifold.collide` (car vs car) | 15 pairs × 120 Hz = **1,800 tests/s**, ~16 float ops each | Leave it |
| `CarBody.integrate` | 6 × 120 = 720 calls/s, ~60 float ops each | Leave it |
| `AIDriver.think` | 720 calls/s — but most of its cost *is* the spline | Leave it; porting the spline fixes it |

Two of those are worth dwelling on, because they are the ones people get wrong.

**Car-versus-car collision feels like it should be hot.** It is a nested loop
doing separating-axis tests, which sounds expensive. It is 1,800 tests a second
of about sixteen floating-point operations — under 30,000 operations per second,
which is nothing. The nested loop is `O(n²)` over **six**.

**The vehicle model feels like it should be hot.** It is the physics! It runs at
120 Hz! It is 720 calls a second of straight-line arithmetic. Porting it would
save perhaps 3% of the physics tick and cost you the ability to tune handling
without a rebuild, which is the worst trade in the project.

The spline dominates by three orders of magnitude, and the terrain bake
dominates everything. That is the whole answer, and it took counting rather than
measuring to see it.

---

## The architecture: plain C, wrapped thinly

Godot's GDExtension interface is a **C ABI** — `gdextension_interface.h` is a C
header, and a pure-C extension is possible. It is also a lot of manual
marshalling: every class, method, property and type has to be described through
function pointers by hand.

The official binding, `godot-cpp`, is C++ and does all of that for you with
macros. So the architecture this project uses is:

```
native/
  src/
    kernels/
      spline_kernel.c      plain C11. No Godot headers. No allocation.
      spline_kernel.h
      terrain_kernel.c
      terrain_kernel.h
    racer_kernels.cpp      ~120 lines of godot-cpp glue
    racer_kernels.h
    register_types.cpp
  godot-cpp/               submodule
  SConstruct
  racer.gdextension
```

The kernels are **plain C that knows nothing about Godot**, operating on flat
arrays of floats. That buys four things:

1. **It is C**, which is the requirement.
2. **It compiles standalone**, so the existing `tests/` harness in this
   repository can test it with no engine at all.
3. **The C engine in this repository can use the identical file.** `spline.c`
   and `spline_kernel.c` are solving the same problem in the same layout; one
   kernel can serve both engines.
4. **The binding layer stays small enough to read**, which is what keeps the
   maintenance cost honest.

### The kernel

```c
/* native/src/kernels/spline_kernel.c
 *
 * Nearest point on a resampled closed centre line. No Godot types, no
 * allocation, no globals: everything arrives as a pointer to a flat buffer
 * the caller owns. That is what lets this file be compiled into the raylib
 * engine, into the GDExtension, and into a standalone test binary without
 * changing a line.
 */
#include "spline_kernel.h"

#include <math.h>

static float project_on_segment(const float *positions, int count, int i,
                                float px, float pz, float *out_sq)
{
    int j = (i + 1) % count;
    float ax = positions[i * 3 + 0], az = positions[i * 3 + 2];
    float bx = positions[j * 3 + 0], bz = positions[j * 3 + 2];

    float abx = bx - ax, abz = bz - az;
    float apx = px - ax, apz = pz - az;

    float denom = abx * abx + abz * abz;
    float t = 0.0f;
    if (denom > 1e-9f) {
        t = (apx * abx + apz * abz) / denom;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    }
    float dx = apx - abx * t;
    float dz = apz - abz * t;
    *out_sq = dx * dx + dz * dz;
    return t;
}

void spline_closest(const SplineKernelView *s, float px, float pz, int hint,
                    SplineKernelResult *out)
{
    int best = 0, count = s->count;
    float best_t = 0.0f, best_sq = 1e30f, sq, t;

    /* Fine sweep around the caller's last answer. */
    if (hint >= 0) {
        int h = hint % count;
        for (int k = -SPLINE_SEARCH_WINDOW; k <= SPLINE_SEARCH_WINDOW; k++) {
            int i = ((h + k) % count + count) % count;
            t = project_on_segment(s->positions, count, i, px, pz, &sq);
            if (sq < best_sq) { best_sq = sq; best = i; best_t = t; }
        }
    }

    /* Coarse sweep of the whole loop: a stale hint produces a plausible small
     * distance on the wrong part of the track, which no threshold detects. */
    int stride = count / 64;
    if (stride < 1) stride = 1;
    int coarse_best = -1;
    float coarse_sq = best_sq;
    for (int i = 0; i < count; i += stride) {
        t = project_on_segment(s->positions, count, i, px, pz, &sq);
        if (sq < coarse_sq) { coarse_sq = sq; coarse_best = i; best_t = t; }
    }
    if (coarse_best >= 0) {
        best_sq = coarse_sq;
        best = coarse_best;
        for (int k = -stride; k <= stride; k++) {
            int i = ((coarse_best + k) % count + count) % count;
            t = project_on_segment(s->positions, count, i, px, pz, &sq);
            if (sq < best_sq) { best_sq = sq; best = i; best_t = t; }
        }
    }

    /* ... assemble position, tangent, distance, lateral, half-width, grade ... */
    out->index = best;
}
```

Line for line, that is `closest_into` from chapter 06. Which is the point:
**write it in GDScript first, with tests, then transliterate.** The GDScript
version is the specification and the differential test below is what proves the
port.

```c
/* native/src/kernels/spline_kernel.h */
#ifndef SPLINE_KERNEL_H
#define SPLINE_KERNEL_H

#ifdef __cplusplus
extern "C" {
#endif

#define SPLINE_SEARCH_WINDOW 24

/* A borrowed view of the spline's arrays. The caller owns every pointer and
 * guarantees they outlive the call; nothing here allocates or frees. */
typedef struct SplineKernelView {
    const float *positions;   /* count * 3 */
    const float *tangents;    /* count * 3 */
    const float *widths;      /* count */
    const float *distances;   /* count */
    const float *grades;      /* count */
    int count;
    float length;
} SplineKernelView;

typedef struct SplineKernelResult {
    float position[3];
    float tangent[3];
    float distance, lateral, half_width, grade;
    int index;
} SplineKernelResult;

void spline_closest(const SplineKernelView *s, float px, float pz, int hint,
                    SplineKernelResult *out);

#ifdef __cplusplus
}
#endif
#endif /* SPLINE_KERNEL_H */
```

`extern "C"` around the header is what lets the C++ wrapper include it without
name mangling. Forgetting it produces a link error that reads as a missing
symbol and is completely mystifying the first time.

### The wrapper

```cpp
// native/src/racer_kernels.cpp
#include "racer_kernels.h"
#include "kernels/spline_kernel.h"

#include <godot_cpp/core/class_db.hpp>

using namespace godot;

void RacerKernels::_bind_methods() {
    ClassDB::bind_method(D_METHOD("set_spline", "positions", "tangents",
                                  "widths", "distances", "grades", "length"),
                         &RacerKernels::set_spline);
    ClassDB::bind_method(D_METHOD("closest_batch", "points", "hints"),
                         &RacerKernels::closest_batch);
}

void RacerKernels::set_spline(const PackedVector3Array &positions,
                              const PackedVector3Array &tangents,
                              const PackedFloat32Array &widths,
                              const PackedFloat32Array &distances,
                              const PackedFloat32Array &grades,
                              double length) {
    // Keep our own references so the copy-on-write buffers cannot be freed or
    // reallocated under us between calls. Cheap: a CoW handle, not a copy.
    _positions = positions;
    _tangents = tangents;
    _widths = widths;
    _distances = distances;
    _grades = grades;

    _view.count = static_cast<int>(positions.size());
    _view.length = static_cast<float>(length);
    _refresh_pointers();
}

// One call, every car. See "choosing the boundary" below: the marshalling cost
// is per call, so the batch size is what decides whether this is a win.
PackedFloat32Array RacerKernels::closest_batch(const PackedVector3Array &points,
                                               const PackedInt32Array &hints) {
    const int n = static_cast<int>(points.size());
    PackedFloat32Array out;
    out.resize(n * RESULT_STRIDE);

    const Vector3 *in = points.ptr();
    const int32_t *hint = hints.ptr();
    float *dst = out.ptrw();

    SplineKernelResult r;
    for (int i = 0; i < n; i++) {
        spline_closest(&_view, static_cast<float>(in[i].x),
                       static_cast<float>(in[i].z), hint[i], &r);
        dst[i * RESULT_STRIDE + 0] = r.position[0];
        // ... pack the rest ...
        dst[i * RESULT_STRIDE + 9] = static_cast<float>(r.index);
    }
    return out;
}
```

### The two marshalling facts that matter

**`ptr()` and `ptrw()` give you the raw buffer with no copy.** A
`PackedVector3Array` passed as `const PackedVector3Array&` shares its
copy-on-write buffer; `ptr()` is a pointer into it. That is why chapter 03
insisted on packed arrays rather than `Array[SplineSample]`: this is where the
decision pays off, and an `Array` of `RefCounted` records here would mean a
`Variant` unwrap and a virtual property fetch per element.

**`real_t` is not always `float`.** In a standard Godot build `Vector3` holds
three 32-bit floats and you can legitimately treat the buffer as a `const
float*`. In a **double-precision build** (`scons precision=double`) it holds
doubles, and that cast is silently wrong. Guard it:

```cpp
static_assert(sizeof(godot::Vector3) == 3 * sizeof(float),
              "This extension assumes a single-precision build. Rebuild "
              "godot-cpp with precision=double and convert explicitly.");
```

A `static_assert` turns a subtle garbage-data bug into a compile error, which is
exactly the trade you want.

### Choosing the boundary

This is the design decision that decides whether the port is worth anything.

Every call across the GDExtension boundary costs: a method lookup, a `Variant`
pack and unpack per argument, and a return-value marshal. It is not enormous —
sub-microsecond — but it is not zero, and it is *per call*.

So do not port `project_on_segment`. Calling it 350,000 times a second across
the boundary would cost more in marshalling than it saves in arithmetic — the
classic failed optimisation, and it is easy to arrive at by "porting the hot
function" without looking at how it is called.

Port **the largest unit that is still pure**. Here that is
`closest_batch`: one call per physics tick, handling every car, doing ~1,500
segment projections inside a single C loop. 120 boundary crossings a second
instead of 350,000.

The general rule:

> Move the boundary outward until the work per call is large compared with the
> cost of the call. Then stop — every step outward drags more logic into C,
> where it is harder to change.

Getting the terrain kernel right is the same idea in the extreme: one call bakes
the entire heightfield.

---

## The build

```python
# native/SConstruct
#!/usr/bin/env python
import os

env = SConscript("godot-cpp/SConstruct")

env.Append(CPPPATH=["src/"])
# SCons compiles .c with the C compiler and .cpp with C++ from the same
# environment, which is exactly what we want: the kernels stay C11.
env.Append(CFLAGS=["-std=c11"])

sources = Glob("src/*.cpp") + Glob("src/kernels/*.c")

library = env.SharedLibrary(
    "../addons/racer_native/bin/libracer{}{}".format(env["suffix"],
                                                     env["SHLIBSUFFIX"]),
    source=sources,
)
Default(library)
```

```ini
; addons/racer_native/racer.gdextension
[configuration]
entry_symbol = "racer_library_init"
compatibility_minimum = "4.3"
reloadable = true

[libraries]
linux.debug.x86_64 =    "res://addons/racer_native/bin/libracer.linux.template_debug.x86_64.so"
linux.release.x86_64 =  "res://addons/racer_native/bin/libracer.linux.template_release.x86_64.so"
linux.debug.arm64 =     "res://addons/racer_native/bin/libracer.linux.template_debug.arm64.so"
linux.release.arm64 =   "res://addons/racer_native/bin/libracer.linux.template_release.arm64.so"
windows.debug.x86_64 =  "res://addons/racer_native/bin/libracer.windows.template_debug.x86_64.dll"
windows.release.x86_64 = "res://addons/racer_native/bin/libracer.windows.template_release.x86_64.dll"
macos.debug =           "res://addons/racer_native/bin/libracer.macos.template_debug.framework"
macos.release =         "res://addons/racer_native/bin/libracer.macos.template_release.framework"
```

```cpp
// native/src/register_types.cpp
extern "C" GDExtensionBool GDE_EXPORT racer_library_init(
        GDExtensionInterfaceGetProcAddress p_get_proc_address,
        const GDExtensionClassLibraryPtr p_library,
        GDExtensionInitialization *r_initialization) {
    godot::GDExtensionBinding::InitObject init(p_get_proc_address, p_library,
                                               r_initialization);
    init.register_initializer(initialize_racer_module);
    init.register_terminator(uninitialize_racer_module);
    init.set_minimum_library_initialization_level(
            MODULE_INITIALIZATION_LEVEL_SCENE);
    return init.init();
}
```

Build it:

```sh
cd native
git submodule update --init            # godot-cpp
scons platform=linux target=template_debug arch=arm64
scons platform=linux target=template_release arch=arm64
```

**The build matrix is the real cost.** Every platform you ship needs a build,
and cross-compiling for all of them from one machine takes setup. On the
Raspberry Pi target in chapter 17 you can build natively on the Pi — slowly, but
without a toolchain — which is often the pragmatic answer for a hobby project.

`reloadable = true` lets the editor unload and reload the library when it
changes, so you do not have to restart Godot after every build. It is a Godot
4.2+ feature and it is worth having on during development.

---

## Using it, with a GDScript fallback

The extension must be optional. A contributor without a toolchain, a platform
you have not built for, and the editor on first checkout all need the game to
run.

```gdscript
# scripts/engine/track_spline.gd
## Native acceleration, if the extension is present. Everything below has a
## GDScript path that produces identical results — see the differential test
## in tests/test_spline_native.gd — so the game runs either way and the only
## difference is speed.
var _native: Object = null

func _init() -> void:
    if ClassDB.class_exists("RacerKernels"):
        _native = ClassDB.instantiate("RacerKernels")

func closest_into(point: Vector3, hint: int, out: SplineQuery) -> void:
    if _native != null:
        _closest_native(point, hint, out)
    else:
        _closest_gdscript(point, hint, out)
```

`ClassDB.class_exists` is the check that does not fail when the extension is
missing. Writing `RacerKernels.new()` directly is a parse error without the
extension, which means the *script* fails to load and takes the whole game with
it.

For the batch path, the race director calls once per tick:

```gdscript
# scripts/game/race_director.gd
func _query_all_cars() -> void:
    for i: int in _racers.size():
        _query_points[i] = _car_point(_racers[i].car)
        _query_hints[i] = _racers[i].progress.spline_hint
    var packed: PackedFloat32Array = _spline.closest_batch(_query_points,
                                                           _query_hints)
    for i: int in _racers.size():
        _unpack_query(packed, i, _queries[i])
```

Three preallocated arrays, one boundary crossing, six results.

---

## Proving the port

Two kinds of test, and you want both.

**Differential tests** assert that the two implementations agree:

```gdscript
# tests/test_spline_native.gd
## The GDScript implementation is the specification. Any divergence is a bug
## in the port, and this test is what makes it safe to change either one.
static func run(t: TestHarness) -> void:
    if not ClassDB.class_exists("RacerKernels"):
        print("SKIP native differential test: extension not built")
        return

    t.suite("spline native vs gdscript")
    var spline := TrackSpline.new()
    spline.build_from_level(load("res://levels/circuit02.track"), 0.25)

    var rng := RandomNumberGenerator.new()
    rng.seed = 20260820
    var a := SplineQuery.new()
    var b := SplineQuery.new()

    for i: int in 5000:
        var p := Vector3(rng.randf_range(-60.0, 60.0), 0.0,
                         rng.randf_range(-60.0, 60.0))
        var hint: int = rng.randi_range(-1, spline.count - 1)
        spline._closest_gdscript(p, hint, a)
        spline._closest_native(p, hint, b)
        t.near(a.distance, b.distance, 1e-3, "distance at sample %d" % i)
        t.near(a.lateral, b.lateral, 1e-3, "lateral at sample %d" % i)
        t.check(a.index == b.index, "index at sample %d" % i)
```

Random points including stale hints, a fixed seed so a failure is reproducible,
and a tolerance that reflects float32 versus GDScript's float64 rather than
demanding bit equality.

**Standalone C tests** reuse the existing harness in `tests/`:

```c
/* tests/test_spline_kernel.c — links spline_kernel.c and nothing else. */
CHECK(fabsf(result.lateral - 1.0f) < 0.05f,
      "one unit inside a ten-unit ring is one unit right");
```

That the kernel compiles and tests without Godot at all is the strongest
argument for keeping it as plain C rather than writing it in C++ against
godot-cpp types. A crash in a pure C kernel is debuggable with `gdb` and a
100-line driver, not by attaching to a game engine.

---

## Threading, which is nearly free once the kernel is pure

A function with no globals, no allocation and no Godot types is trivially
parallel. The terrain bake is embarrassingly so — every grid sample is
independent:

```cpp
// One row of the heightfield per task. WorkerThreadPool is Godot's own pool,
// so this composes with whatever else the engine is doing rather than
// oversubscribing the machine with threads of our own.
void RacerKernels::_bake_rows(void *userdata, uint32_t row) {
    BakeContext *ctx = static_cast<BakeContext *>(userdata);
    terrain_bake_row(&ctx->view, ctx->heights, ctx->grid_x, row,
                     ctx->origin_x, ctx->origin_z, ctx->cell_size);
}
```

On a four-core Pi that is close to a 4× saving on top of whatever the C port
already bought, for about ten lines. It is available *because* the kernel was
written as a pure function over borrowed buffers — the same property that made
it testable.

---

## When not to do any of this

Five situations where the answer is not C:

**The work can move to import time.** Chapter 13's terrain bake is only in this
chapter because a designer wants to iterate on waypoints. The *shipped* game
does not run it at all. Import-time work has no runtime cost and no build
matrix.

**A better algorithm is available.** Chapter 06's windowed search is a 100×
improvement from one idea. There is no constant factor in C that competes with
removing 97% of the work.

**The work belongs on the GPU.** Chapter 13's displacement shader is the same
computation, done by hardware that is already idle in a top-down racer.

**It is called rarely.** Chapter 08's vehicle model is 720 calls a second. Even
a 20× speedup on it is invisible, and the cost — losing hot tuning — is real.

**You would have to port a lot to port anything.** If the hot function reads
`Node` properties, emits signals or touches `Resource`s, the boundary is in the
wrong place. Refactor it into a pure function first, in GDScript, with tests.
Then decide. Often the refactor alone is a large part of the speedup, because
pure functions over packed arrays are also the fastest thing GDScript can do.

### And the alternatives to C specifically

| Option | Good for | Cost |
|---|---|---|
| **C via GDExtension** | Numeric kernels, reusing existing C | Build matrix, segfaults |
| **C++ via godot-cpp** | Anything needing Godot types deeply | Same, plus C++ |
| **C# (.NET build)** | Broad, gradual speedups; whole subsystems | A different Godot build, larger export, no C# on the web |
| **Compute shaders** | Massively parallel, GPU-shaped work | Latency, readback, not available on Compatibility |
| **`WorkerThreadPool` in GDScript** | Parallelising work you already have | GDScript per-thread cost is unchanged |

For this project's two hot spots — a numeric kernel over flat float buffers, and
a bake — plain C is the right tool, and it happens to also be a file this
repository already has.

---

## Exercises

1. **Count before you measure.** For each of the six candidates in the table
   above, compute the operations per second yourself before reading the verdict
   column. Then benchmark two of them and see whether the counting predicted the
   ranking.

2. **Get the boundary wrong on purpose.** Expose `project_on_segment` as a bound
   method and call it 350,000 times from GDScript. Compare with the batched
   version and with the pure GDScript version. Which of the three is slowest,
   and by how much?

3. **Build it.** Set up `godot-cpp`, build the extension for your platform, and
   confirm `ClassDB.class_exists("RacerKernels")` is true. Then delete the
   binary and confirm the game still runs on the GDScript path.

4. **Break the precision assumption.** Rebuild godot-cpp with
   `precision=double` and remove the `static_assert`. Describe the failure. Is
   it a crash, or is it worse than a crash?

5. **Thread the bake.** Parallelise `terrain_bake_row` with
   `WorkerThreadPool.add_group_task`. Measure on one core and on all of them.
   Then find the race condition you would have introduced if the kernel had kept
   any state between rows.

6. **The honest comparison.** Time the full terrain bake three ways: GDScript
   with `spline_stride = 8`, GDScript with stride 1, and the C kernel with
   stride 1. Then decide which one you would actually ship, given that the
   shipped game bakes at import time and never runs any of them.

---

Next: [17 — Export and platforms](17-export-and-platforms.md)
