# 04 — Build at `-O3`

**Files:** `Makefile`, `docs/learn/15-build-and-portability.md`
**Result:** terrain height field 1423 ms → 587 ms, whole test suite 10.60 s →
9.66 s, for byte-identical output. One line.

---

## Why this is a separate change

Improvement 01 restructured the terrain height-field loop and noted that
compiling that one file at `-O3` was worth a further 2.4x, but declined to take
it — an optimisation-level change affects every file in the project, and
smuggling it into a change about one loop would have hidden it.

So here it is on its own, with the measurements that justify it.

## The measurements

All on the target hardware: Raspberry Pi, Cortex-A53, 1.2 GHz, GCC, project
objects only (raylib is built by its own Makefile and is untouched).

| | `-O2` | `-O3` | |
|---|---:|---:|---|
| terrain height field, circuit02 | 1423 ms | 587 ms | **2.4x** |
| terrain height field, circuit01 | 245 ms | 97 ms | **2.5x** |
| whole test suite | 10.60 s | 9.66 s | **9%** |
| `racer` binary | 2.1 MB | 2.2 MB | +5% |

The test suite is a fair proxy for the game's CPU cost: it is dominated by two
complete six-car races — physics at 120 Hz, AI, collision, spline queries — with
no rendering in it at all. 9% there is 9% of the simulation budget.

Combined with improvement 01, circuit02's height field goes from **3.10 s to
0.59 s**, a 5.3x that costs nothing in fidelity: this is still the exact sum
over every spline sample that the terrain chapter argues for.

## Correctness

Race telemetry is byte-identical between `-O2` and `-O3` across both circuits —
same finishing times, same best laps, same off-track fractions to the digit.
That is the check that matters here. `-O3` enables no unsafe floating-point
transformation (that would be `-ffast-math`, which is not used and should not
be); it is more inlining, more unrolling and more aggressive scheduling.

`make test` reports 414 checks, 0 failures at both levels.

## The honest caveat

**`-O3` is not a free win in general**, and this change does not claim it is.
While benchmarking improvement 01 I measured variants of the same terrain kernel
where `-O3` was distinctly *worse*:

| terrain kernel variant | `-O2 -funroll-loops` | `-O3` |
|---|---:|---:|
| 4 accumulator chains (shipped) | 8.4 ns/sample | 12.7 |
| 8 accumulator chains | 9.6 | 27.0 |
| 16 accumulator chains | 10.0 | 40.4 |

Past four chains, `-O3`'s scheduling spills the accumulators and the loop falls
off a cliff. The flag is justified by measurements of *this code as it currently
stands*, not by a belief that higher optimisation is better. The Makefile
comment says so, so that the next person to touch a hot path knows to re-measure
rather than assume.

## The escape hatch

The level is a variable rather than a literal:

```makefile
OPT      ?= -O3
CFLAGS   := -std=c11 $(OPT) -g $(WARNINGS) ...
```

So checking the claim, or building for a debugger or a sanitiser, is:

```sh
make OPT=-O2
make OPT="-O0 -fsanitize=address"
```

`-g` is unchanged and unconditional: a crash without symbols is a waste of
everyone's time, and that was already the project's stated position.
