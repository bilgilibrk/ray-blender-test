# 01 — Terrain height field: 2.2x faster, same ground

**Files:** `engine/src/terrain.c`, `engine/include/engine/terrain.h`,
`tests/test_terrain.c`
**Result:** circuit02's height field drops from **3.10 s to 1.43 s**; circuit01
from 0.53 s to 0.24 s. Output unchanged except in the last bit or two of a
float.

---

## What was wrong

Loading a circuit was slow, and effectively all of it was one function.
Measured on the target hardware (Raspberry Pi, Cortex-A53, 1.2 GHz):

| stage | circuit01 | circuit02 |
|---|---:|---:|
| `LevelLoad` (file + JSON) | 25 ms | 62 ms |
| `SplineBuild` | 1 ms | 2 ms |
| **terrain height field** | **530 ms** | **3096 ms** |

The height field fits the ground to the racing line by inverse-distance
weighting: for every node of the terrain grid, sum a weight over every sample of
the spline. Circuit02 is a 149 × 204 grid against a 1,467-sample spline — about
45 million iterations.

45 million iterations of a handful of float operations should cost a fraction of
a second, not three. At ~70 ns per sample the loop was running about an order of
magnitude slower than its arithmetic.

## Why it was slow

One operation in the kernel is not like the others:

```c
float w = 1.0f / (d2 * d2 + 0.45f);
```

On an in-order Cortex-A53 the divider is a separate, non-pipelined unit: the
next divide cannot begin until the current one retires. And the loop guaranteed
that would happen, because `total += w` is a serial dependency chain — every
iteration waits for the previous one. The divides ran in single file, so the
loop cost the divider's *latency* per sample rather than its throughput.

A microbenchmark on the same machine, isolating the kernel:

| variant | ns/sample | vs baseline |
|---|---:|---:|
| original, one accumulator chain | 67.8 | 1.00x |
| 2 independent chains | 23.9 | 2.84x |
| **4 independent chains** | **18.9** | **3.60x** |
| 8 independent chains | 28.2 | 2.41x |
| Newton-Raphson reciprocal instead of divide | 101.8 | 0.67x |

Four is the sweet spot: enough divides in flight to keep the unit busy, few
enough that the working set stays in registers. Replacing the divide with a
reciprocal approximation is much *worse* — the extra multiplies cost more than
the divide they avoid.

## What changed

Two things, neither of which alters the interpolation.

**1. Four independent accumulator chains.** The sum is split four ways and
recombined at the end. This is a reassociation of a floating-point sum, so
results move by a bit or two and nothing else.

**2. Positions in flat arrays.** `SplineSample` is 36 bytes and this loop wants
12 of them. `HeightFieldBuild` copies the positions once into three contiguous
`float` arrays behind a single allocation, so the query reads three clean
streams that `restrict` proves cannot alias.

The height field also became a named type (`HeightField`) separate from
`TerrainBuild`. That is what makes it testable: `TerrainBuild` uploads meshes
and needs a GPU, the height field does not, so it can now be exercised headless.

## Why not the obvious 7x

The natural next step is a spatial index: bucket the spline and, for buckets far
from the query point, substitute one aggregate weight for all their samples.
This was prototyped in two forms — a flat uniform bucket grid, and a two-level
hierarchy with centroid-and-second-moment aggregates.

It works. Best configurations reached **7.4x** over the original.

It was **not adopted.** The moment a bucket flips from *summed exactly* to
*approximated*, the surface steps. Sweeping the parameters against an exact
reference over every grid node of both circuits put the worst step at 3–30 mm
depending on configuration, and only ~3x was available at the ≤1 mm end. Terrain
relief shading exaggerates the surface normal five-fold precisely to make gentle
slopes readable, which turns a millimetre-scale step between adjacent nodes into
visible banding on flat ground.

Crease-freedom is a stated design property of this terrain, not an incidental
one — `docs/learn/12-terrain.md` argues for summing every sample on exactly
these grounds. Trading it for load time is a different decision from making the
existing code faster, and it is not this change's to make.

The two-level hierarchy is also recorded here as a dead end: it was no better
than the flat grid at equal accuracy, because the `d⁻⁴` kernel is sharp enough
that nodes must be very distant before an aggregate is admissible, and the
coarse level almost never qualified.

## Also available, deliberately deferred

Compiling `engine/src/terrain.c` at `-O3` instead of the project's `-O2` takes
the same code from 1.43 s to **0.59 s** — a further 2.4x, exact, for a build
flag. That is a project-wide decision about optimisation level, so it belongs in
its own change rather than smuggled in here.

## How it is verified

`tests/test_terrain.c` (10 new checks) transcribes the interpolation from the
documentation in `double` precision and compares the shipped query against it
over a 61 × 61 grid of points, requiring agreement to 1e-4. It also checks that
the ground hugs the road, relaxes to the mean height away from it, has no large
steps between adjacent nodes, and stays finite when a query lands exactly on a
sample.

`make test` reports **358 checks, 0 failures**, and the six-car race telemetry
for both circuits is byte-identical to before the change.
