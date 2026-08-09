# 02 — A typo in a level file should not segfault the engine

**Files:** `engine/src/json.c`, `engine/src/level.c`, `tests/test_json.c`,
`tests/test_level.c`
**Result:** a malformed number is refused with a line and column, instead of
crashing the terrain build several hundred milliseconds later.

---

## The bug

Take `levels/circuit01.level.json`, and change one waypoint's `x` from `10` to
`1e999` — the sort of thing a slipped finger or a script with a bad format
string produces. Before this change:

```
LevelLoad:  accepted
  waypoint[5].x = inf
SplineBuild: accepted
  spline length = nan, samples = 1654
  non-finite spline samples: 1030 of 1654
Segmentation fault
```

The load succeeded. The spline succeeded. The crash landed in `TerrainBuild`,
whose grid dimensions come from a bounding box that is now `[-inf, +inf]`, and
the backtrace says nothing about a level file at all.

## Why it happened

`ParseNumber` delegated the whole job to `strtod`:

```c
char tmp[64];
size_t n = (size_t)(ps->end - ps->p);
if (n > sizeof(tmp) - 1) n = sizeof(tmp) - 1;
memcpy(tmp, ps->p, n);
tmp[n] = '\0';

double v = strtod(tmp, &endp);
if (endp == tmp) { Fail(ps, "invalid number"); return false; }
```

`strtod` is a C parser, not a JSON one, and it was doing the validating as well
as the converting. Probing the shipped parser:

| input | JSON? | before | after |
|---|---|---|---|
| `+1` | no | accepted as `1` | rejected |
| `0x10` | no | accepted as `16` | rejected |
| `0x1p8` | no | accepted as `256` | rejected |
| `01` | no | accepted as `1` | rejected |
| `1.` | no | accepted as `1` | rejected |
| `1e999` | yes | **accepted as `inf`** | rejected |
| `-1.5e-3` | yes | accepted | accepted |
| 400-digit literal | yes | mis-read or bogus syntax error | accepted, exact |

The `64` byte buffer was a second, quieter problem. It copied "up to 63 bytes"
and let `strtod` find the end of the number, so a longer literal was cut — and
then either converted to a *different* number, or reported as a syntax error
several characters past the one that actually caused it.

## What changed

**One.** A `ScanNumber` that measures the token against JSON's actual grammar
before conversion:

```
-? ( 0 | [1-9][0-9]* ) ( . [0-9]+ )? ( [eE] [+-]? [0-9]+ )?
```

`strtod` still does the conversion — correctly-rounded decimal-to-binary is
genuinely hard and the standard library has a tested implementation. It just no
longer decides what a number *is*. Scanning first also yields the token's
length, which is what lets the buffer be the right size: the stack copy for the
ordinary case, the arena for anything longer.

**Two.** Overflow is now a failure rather than a rounding:

```c
if (!isfinite(v)) { Fail(ps, "number is too large to represent"); return false; }
```

Underflow is deliberately left alone — `1e-999` lands on zero, which is a fair
answer.

**Three.** A second layer in `level.c`, because the first one cannot be enough.
`1e300` is well-formed JSON and a perfectly ordinary `double`; it is an infinity
only once narrowed to the `float` that `LevelWaypoint` actually stores.
`LevelCheckNumbers` sweeps the finished level for non-finite values and fails
the load naming the offender:

```
ERROR: LEVEL: waypoint 5 has a coordinate that is not a number
ERROR: LEVEL: '/tmp/bad2.level.json' rejected
```

## A leniency that was reversed

`docs/learn/03-json-parser.md` described the leading `+` as "a deliberate
leniency, consistent with allowing comments". Comments are kept — hand-editing a
level file is a thing people do in this project, and `//` earns its place. The
`+` is not, because it was not really a decision: it fell out of handing the
parse to `strtod`, and the same slack is what admitted `0x1p8` and `inf`.

Checked before removing it: no file in `levels/` uses a leading `+`, and the
Blender exporter rounds to plain decimals, so nothing that exists emits one.
Exponent signs (`2e+3`) are untouched — those *are* JSON.

The same chapter claimed overflow yielding `HUGE_VAL` was "acceptable for level
data". That is the sentence this change disproves, and it has been corrected in
place rather than quietly dropped.

## How it is verified

- `tests/test_json.c`: 12 non-JSON numbers that must be rejected, 10 valid ones
  that must parse to exact expected values, and a 400-digit literal that must
  convert exactly rather than being truncated.
- `tests/test_level.c` (new): writes a scratch level with `1e999` (caught by the
  parser), then with `1e300` (caught by the level reader), then with a normal
  coordinate, and checks the spline built from the last one is finite
  throughout.

`make test` goes from 358 to **402 checks, 0 failures**. Both shipped circuits
load unchanged and the six-car race telemetry is byte-identical.
