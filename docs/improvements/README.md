# Improvements

One document per change, written after the change and kept with it.

Where [`docs/learn/`](../learn/README.md) explains why the code is the shape it
is, these explain why it *changed* shape: what was measured, what was tried and
rejected, and what the change costs. They exist because the interesting part of
most of these was not the diff.

Each one is the companion to a single commit.

| | change | effect |
|---|---|---|
| [01](01-terrain-height-field.md) | Terrain height field, restructured | circuit02 load 3.10 s → 1.43 s, identical ground |
| [02](02-reject-numbers-that-are-not-numbers.md) | Strict JSON numbers, and a finiteness check on levels | a typo'd exponent is an error, not a segfault |
| [03](03-a-clock-the-simulation-owns.md) | AI wobble driven by simulated time | the race is reproducible; the tests now run the shipped AI |
| [04](04-build-at-o3.md) | `-O3` | terrain 2.4x, simulation 9%, byte-identical output |

---

## What these have in common

Three of the four came from measuring rather than reading, and the fourth came
from a probe that fed the parser input nobody had tried.

**Measure the target, not the idea of it.** The terrain loop was slow for a
reason that is invisible in the source: one non-pipelined divide on an in-order
core. No amount of staring at `1.0f / (d2 * d2 + 0.45f)` suggests that the fix
is four accumulators.

**Write down what you rejected.** Improvement 01 spends more words on the
spatial index that was *not* adopted than on the change that was, because the
next person to look at that function will have the same idea and deserves the
measurements.

**A test that cannot reach a path passes forever.** Improvement 03 is the sharp
version: the AI's wobble was switched off in every headless test run there had
ever been, and the suite reported its lap times as though they were the game's.
The bug was not in the assertion. It was in what the harness made unreachable.

**Correct the documentation in place.** Two of these disproved a specific
sentence in `docs/learn/`. Those sentences were rewritten with the real
mechanism rather than quietly deleted — an explanation that was wrong is worth
more as a corrected one than as an absence.
