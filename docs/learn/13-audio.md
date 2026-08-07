# 13 — Procedural audio

> `engine/include/engine/audio.h` · `engine/src/audio.c` — 145 lines.

---

## The problem

```c
// engine/include/engine/audio.h
// The Kenney kit ships no sound, so the engine note, tyre scrub and countdown
// beeps are synthesised into a single streamed voice. Everything degrades to a
// silent no-op when no audio device is available (headless boxes, most Pis at
// a bare TTY), so callers never need to check first.
```

Two constraints, and they point the same way.

**There are no sound files.** The art kit is CC0 models with no audio. Sourcing,
licensing and shipping engine recordings is a whole task; and a recorded engine
loop needs pitch-shifting and crossfading to track revs convincingly, which is
its own signal-processing problem.

**A car engine is a periodic waveform whose pitch tracks a number you already
have.** That is precisely what a synthesiser is for. Twenty lines of oscillator
gives you a note that responds instantly and continuously to `forwardSpeed`,
with no sample library and no interpolation artefacts.

---

## The architecture: a callback on another thread

```c
#define AUDIO_RATE       22050
#define AUDIO_BUFFER     1024

// Written by the game thread, read by the audio callback. Single-writer floats:
// a torn read would at worst make one buffer sound slightly stale.
static struct {
    volatile float rpm01;
    volatile float load;
    volatile float skid;
    volatile float volume;
    volatile float blipFreq;
    volatile int blipSamples;      // remaining samples of the current blip
    AudioStream stream;
    bool available;
} g_audio;
```

The audio device calls `MotorStreamCallback` from its own thread whenever it
needs another buffer. At 22,050 Hz with a 1,024-sample buffer that is about
every 46 ms — and it is a **hard real-time deadline**. Miss it and you get an
audible click or dropout.

That means the callback may not: allocate, take a lock the game thread might
hold, do file I/O, or block on anything.

### Why 22,050 Hz

Half of CD rate. By Nyquist it reproduces frequencies up to 11 kHz — plenty for
an engine note whose fundamental peaks around 260 Hz plus a few harmonics, and
for filtered noise. It halves the CPU cost of the callback, which is the point
on a Pi.

### The concurrency story, stated honestly

The comment is the design document:

> Written by the game thread, read by the audio callback. Single-writer floats:
> a torn read would at worst make one buffer sound slightly stale.

This is not a lock-free queue and it is not formally correct C11 — `volatile`
in C is about preventing compiler *elision* of accesses, not about
inter-thread ordering, and a 32-bit float store is atomic on ARM and x86 in
practice but not by the standard's guarantee.

So why is it acceptable? Because the analysis of the worst case is:

- Each field is written by exactly one thread (the game) and read by exactly one
  (audio). No read-modify-write, so no lost updates.
- A float is 32 bits and naturally aligned on both target architectures, so a
  tear would require a non-atomic 32-bit store — which neither ARM nor x86-64
  does.
- Even granting a tear, the consequence is one 46 ms buffer synthesised with a
  slightly wrong RPM. Inaudible.

**The right amount of rigour depends on the cost of being wrong.** For a
lock-free ring buffer carrying financial transactions, `volatile` would be
malpractice. For the pitch of an engine note, a mutex would be worse: taking a
lock in an audio callback risks priority inversion and an actual dropout, which
is a far more serious failure than a stale buffer.

The proper modern answer is `_Atomic float` with `memory_order_relaxed`, which
costs nothing on these architectures and is standards-clean. That is exercise 6.

`blipSamples` is the one field written by *both* threads — the game sets it,
the callback decrements it. It is a counter that only needs to reach zero
eventually, and a lost decrement lengthens a beep by 45 microseconds.

---

## Synthesis

```c
static void MotorStreamCallback(void *buffer, unsigned int frames)
{
    short *out = (short *)buffer;

    static float phase1 = 0.0f, phase2 = 0.0f, phase3 = 0.0f;
    static float blipPhase = 0.0f;
    static float noiseLp = 0.0f;
    static unsigned int rng = 12345u;

    float rpm = g_audio.rpm01;
    float load = g_audio.load;
    float skid = g_audio.skid;
    float volume = g_audio.volume;
```

**The shared state is snapshotted once per buffer**, not read per sample. That
gives every sample in the buffer a consistent view, avoids 1,024 volatile reads,
and means the parameters change at buffer boundaries — a 46 ms quantisation that
is below the threshold of perception for a continuously-varying engine note.

**`static` locals hold the oscillator phases across calls.** Phase must be
continuous between buffers: restart it at zero each time and you get a
discontinuity in the waveform every 46 ms, which is an audible buzz at 21 Hz.
This is the single most common bug in first-attempt audio callbacks.

### The oscillators

```c
// Idle around 48 Hz, redline near 260 Hz.
float base = 48.0f + rpm * 212.0f;
float inc1 = base / AUDIO_RATE;
float inc2 = (base * 2.02f) / AUDIO_RATE;      // slightly detuned octave
float inc3 = (base * 0.5f) / AUDIO_RATE;       // sub

float engineGain = 0.16f + 0.30f * load + 0.10f * rpm;
```

```c
// Band-limited-ish sawtooth: cheap, and the grit suits an engine note.
static float Saw(float phase) { return phase * 2.0f - 1.0f; }
```

```c
for (unsigned int i = 0; i < frames; i++) {
    phase1 += inc1; if (phase1 >= 1.0f) phase1 -= 1.0f;
    phase2 += inc2; if (phase2 >= 1.0f) phase2 -= 1.0f;
    phase3 += inc3; if (phase3 >= 1.0f) phase3 -= 1.0f;

    float engine = (Saw(phase1) * 0.55f + Saw(phase2) * 0.25f + Saw(phase3) * 0.35f);
    engine *= engineGain;
```

**Phase accumulation.** `phase` runs 0→1 and wraps; `inc = frequency / sampleRate`
is how far it advances per sample. This is the foundation of every digital
oscillator: keep a normalised phase, add an increment, wrap.

Subtracting 1.0 rather than `fmodf` — the increment is always less than 1 for
audible frequencies, so one subtraction suffices, and it avoids a function call
per oscillator per sample.

**A sawtooth** is `2·phase − 1`: a linear ramp from −1 to +1 with a hard drop.
It is rich in harmonics (amplitude falling as 1/n), which is why it sounds
buzzy — and buzzy is what an engine sounds like. A sine would sound like a
theremin.

Strictly this is *not* band-limited: the hard discontinuity produces harmonics
above Nyquist that alias back down as inharmonic tones. The comment says
"band-limited-**ish**", which is honest. Proper anti-aliasing (PolyBLEP, or a
wavetable per octave) is real work, and at these low fundamentals the aliased
content sits under the intended harmonics and reads as grit. For an engine that
is a feature.

**Three detuned oscillators.**

- The fundamental at `base`.
- An octave at `base × 2.02` — deliberately 2.02 rather than 2.00. An exact
  octave phase-locks and fuses into one tone; 1% sharp produces a slow beat that
  makes the note sound thick and mechanical rather than synthetic. This is
  "detuning" and it is how every analogue synth patch gets its width.
- A sub at `base × 0.5`, an octave down, for weight.

Mixed 0.55 / 0.25 / 0.35.

**The gain envelope** `0.16 + 0.30·load + 0.10·rpm` means the engine is quiet at
idle, loudest under throttle, and slightly louder at high revs. `load` is the
throttle input, so lifting off audibly reduces the note without changing its
pitch — which is exactly what a real engine does and is a surprisingly strong
cue.

### Tyre scrub

```c
// Tyre scrub: white noise through a one-pole low-pass.
float noise = FastRandom(&rng);
noiseLp += (noise - noiseLp) * 0.35f;
float scrub = noiseLp * skid * 0.34f;
```

```c
static float FastRandom(unsigned int *state)
{
    *state = *state * 1664525u + 1013904223u;
    return ((float)((*state >> 9) & 0x7FFFFF) / (float)0x7FFFFF) * 2.0f - 1.0f;
}
```

**A linear congruential generator** with the Numerical Recipes constants
(multiplier 1664525, increment 1013904223). Two arithmetic operations, no state
beyond a `uint32`, and it is inlined into the loop. Its statistical quality is
poor — the low bits are notoriously non-random, which is why the code takes bits
23–9 via `>> 9` — but for audio noise "poor" is inaudible.

Note the callback needs its own RNG rather than `rand()`, because `rand()` is
not thread-safe and shares state with whatever the game thread is doing.

**A one-pole low-pass filter**, in one line:

```
y[n] = y[n-1] + α·(x[n] − y[n-1])
```

This is an exponential moving average, and it is a first-order IIR low-pass. Its
cutoff is:

```
f_c = −ln(1 − α) · f_s / 2π  ≈ (for small α)  α·f_s / 2π
```

With `α = 0.35` and `f_s = 22050`, that is roughly 1.5 kHz. White noise becomes
"pink-ish" — the hiss loses its top end and gains the rushing quality of rubber
on tarmac rather than radio static.

Recognise this line. It is the same maths as the camera smoothing in Chapter 01
and the offset blending in Chapter 08 — an exponential filter, applied in three
completely different domains.

### The countdown blip

```c
float blip = 0.0f;
if (g_audio.blipSamples > 0) {
    blipPhase += g_audio.blipFreq / AUDIO_RATE;
    if (blipPhase >= 1.0f) blipPhase -= 1.0f;
    // Fade the tail so the tone does not click when it ends.
    float env = (g_audio.blipSamples < 900) ? (float)g_audio.blipSamples / 900.0f : 1.0f;
    blip = sinf(blipPhase * 2.0f * PI) * 0.42f * env;
    g_audio.blipSamples--;
} else {
    blipPhase = 0.0f;
}
```

A sine, because a beep should be a clean tone rather than a buzz.

**The envelope is the important detail.** Cutting a sine off mid-cycle leaves a
step discontinuity in the waveform, which contains energy at every frequency —
an audible **click**. Ramping the amplitude to zero over the last 900 samples
(41 ms) removes it.

This is why every synthesiser has an ADSR envelope, and why the *release* stage
is not optional. Any abrupt amplitude change in a digital signal is a click.

`blipPhase = 0.0f` in the `else` branch resets for the next beep, so each starts
at zero amplitude — the attack side of the same problem.

### Mix and output

```c
float sample = (engine + scrub + blip) * volume;
if (sample > 1.0f) sample = 1.0f;
if (sample < -1.0f) sample = -1.0f;
out[i] = (short)(sample * 32000.0f);
```

**Hard clipping** at ±1. Three summed sources can exceed unity; without a
clamp, converting to `short` would *wrap* — +1.2 becoming a large negative
number — which is a violent, unmistakable crackle. Clipping merely distorts,
which on an engine note is barely noticeable.

The proper answer is a limiter or a soft-clip curve like `tanh`. Two comparisons
is the right amount of machinery here.

**32000 rather than 32767** leaves about 0.2 dB of headroom below full scale, so
rounding cannot overflow the 16-bit range.

---

## Initialisation, and failing quietly

```c
bool AudioEngineInit(void)
{
    memset((void *)&g_audio, 0, sizeof(g_audio));
    g_audio.volume = 0.7f;

    InitAudioDevice();
    if (!IsAudioDeviceReady()) {
        TraceLog(LOG_WARNING, "AUDIO: no output device, running silent");
        return false;
    }

    SetAudioStreamBufferSizeDefault(AUDIO_BUFFER);
    g_audio.stream = LoadAudioStream(AUDIO_RATE, 16, 1);
    if (!IsAudioStreamValid(g_audio.stream)) {
        TraceLog(LOG_WARNING, "AUDIO: could not create engine stream, running silent");
        CloseAudioDevice();
        return false;
    }

    SetAudioStreamCallback(g_audio.stream, MotorStreamCallback);
    PlayAudioStream(g_audio.stream);
    g_audio.available = true;
    return true;
}
```

`LoadAudioStream(22050, 16, 1)` — 22 kHz, 16-bit, **mono**. Mono because the
game has no spatial audio: one engine note, from your own car, positioned
nowhere.

Every failure path warns and returns false with `available` still false. And
then every setter is a no-op:

```c
void AudioEngineSetMotor(float rpm01, float load, float skid)
{
    if (!g_audio.available) return;
    g_audio.rpm01 = Clamp(rpm01, 0.0f, 1.0f);
    /* ... */
}
```

The header's promise — *"callers never need to check first"* — is kept by the
implementation, so calling code has no branches.

`core.c` treats it as optional:

```c
// engine/src/core.c
if (config->audio) AudioEngineInit();   // silent fallback is fine
```

Note the return value is deliberately ignored. Audio failing is not an error
worth propagating; the game runs fine without it, which matters on a headless
Pi, in CI, and under `--no-audio`.

`main.c` does check, but only to skip work:

```c
// Engine note tracks revs; tyre scrub follows slip while on the ground.
if (AudioEngineAvailable()) {
    float rpm = Clamp(fabsf(player->car.forwardSpeed) / stage.race.tuning.topSpeed, 0.0f, 1.0f);
    if (stage.race.state == RACE_COUNTDOWN) {
        rpm = 0.35f + 0.25f * sinf((float)GetTime() * 9.0f);
    }
    AudioEngineSetMotor(rpm, player->input.throttle,
                        player->car.slip * (player->car.onTrack ? 1.0f : 0.6f));
}
```

Three mappings worth noting:

- **`rpm` from `forwardSpeed`, not `speed`.** A car sliding sideways at 5 u/s
  with 1 u/s of forward motion should sound slow, because its wheels are barely
  turning in the direction of travel. Using `speed` would make a drift sound
  like a straight.
- **The countdown revs.** `0.35 + 0.25·sin(t · 9)` blips the engine between 10%
  and 60% at about 1.4 Hz while the grid waits. Four lines of pure atmosphere.
- **Off-track scrub is scaled to 0.6.** Sliding on grass makes less of a
  screeching noise than sliding on tarmac.

---

## What this buys, and what it costs

**Buys:**

- No audio assets to source, license, convert or ship.
- Instant, continuous response to game state — no crossfade seams, no pitch
  quantisation.
- 145 lines and a few KB of code, versus megabytes of samples.
- Runs identically on every platform, including a Pi at a bare TTY.

**Costs:**

- It sounds synthesised. A recorded V8 has formants, exhaust resonance and
  mechanical noise this does not.
- No spatial audio, no Doppler, no other cars.
- The aliasing is real, even if it is disguised as grit.

For a top-down arcade racer made of untextured flat-shaded kit models, the
synthesised note is stylistically consistent with the visuals. Wrapping a
photoreal engine recording around this art would sit oddly.

**Match the fidelity of each subsystem to the others.** A game with one
photoreal element and nine stylised ones reads as broken; a game that is
consistently stylised reads as designed.

---

## Exercises

1. **Hear the phase bug.** Move `phase1`, `phase2`, `phase3` from `static` into
   plain locals initialised to zero. Drive and listen. Compute the frequency of
   the artefact from `AUDIO_RATE / AUDIO_BUFFER`.

2. **Hear the click.** Remove the `env` term from the blip (use `1.0f`). Trigger
   a blip — add `AudioEngineBlip(880.0f, 0.15f)` on a key press. Listen to the
   end of the tone.

3. **Detune.** Change `base * 2.02f` to exactly `base * 2.0f`. Listen carefully
   at a steady speed. Then try `2.10f`. Describe the beat frequency in each case
   and relate it to the difference in Hz.

4. **Filter cutoff.** Change the one-pole coefficient from `0.35f` to `0.02f`
   and then to `0.9f`. Compute the approximate cutoff in each case and describe
   the resulting scrub sound.

5. **Add a gearbox.** Real engines drop revs on a shift. Divide the speed range
   into five bands and map `rpm` to a sawtooth within each, so the note rises and
   snaps back. Where should this live — in `audio.c`, or in `main.c`'s mapping?
   Argue for one.

6. **Do the concurrency properly.** Replace `volatile float` with
   `_Atomic float` and use `atomic_store_explicit(..., memory_order_relaxed)`
   and the matching load. Confirm it still compiles under `-std=c11` and
   measure whether the callback's cost changes. Explain what the standard now
   guarantees that it did not before.

7. **Wire up the blips.** `AudioEngineBlip` exists and nothing calls it. Add
   countdown beeps: a 660 Hz tone at 3, 2 and 1, and an 880 Hz tone at go. Where
   in `race.c` or `main.c` does the edge detection belong, and how do you avoid
   firing it every tick of the second?

---

Next: [14 — Testing a game without a window](14-testing.md)
