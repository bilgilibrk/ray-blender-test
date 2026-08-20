# 14 — Procedural audio

> `scripts/engine/procedural_audio.gd` · `scenes/audio/audio.tscn`.
> Mirrors [C chapter 13](../learn/13-audio.md).

---

## The problem

The Kenney kit ships no sound. A racing game with no engine note is not a
racing game — the engine is most of the feedback loop that tells you what the
car is doing, and it is the difference between "the car is moving" and "the car
is *working*".

You could buy or record samples. But an engine note is not a sample: it is a
pitch that follows revs, a timbre that follows load, and a texture that follows
what the tyres are doing. Playing a looped sample at varying pitch sounds like a
looped sample at varying pitch.

So: **synthesise it.** Three detuned sawtooth oscillators whose frequency
follows the car's speed, a noise layer gated by tyre slip, and a sine blip for
the countdown. About 60 lines, no assets, and it responds to every input the
physics produces.

---

## The architecture, and the one big difference

This is where the Godot port diverges most sharply from the C, and it is worth
being precise because the difference is a genuine trade rather than an
improvement.

**raylib** hands you a callback. `SetAudioStreamCallback(stream, MotorStreamCallback)`
registers a function that the audio device thread calls whenever it needs
another buffer. The game thread never blocks on audio, and audio never blocks on
the game. In exchange you have a genuine cross-thread data race — the C chapter
handles it with `volatile` floats and an argument about torn reads being
harmless.

**Godot** hands you a queue. `AudioStreamGenerator` gives you a playback object
with a buffer; you ask how much room there is and push frames into it, **from
whatever thread you like — in practice, from `_process` on the main thread.**

```gdscript
func _process(_delta: float) -> void:
    var frames: int = _playback.get_frames_available()
    if frames > 0:
        _fill(frames)
        _playback.push_buffer(_scratch)
```

| | raylib callback | Godot generator |
|---|---|---|
| Who calls the synth | The audio device thread | You, from `_process` |
| Cross-thread state | A real data race, handled with `volatile` | None: the synth reads game state directly |
| Failure mode | Glitch if the callback is slow | **Underrun if a frame is slow** |
| Latency control | Device buffer size | `buffer_length`, and how eagerly you fill |

That third row is the trade. Godot's design removes an entire class of
concurrency bug and replaces it with a coupling: **if a frame takes longer than
the audio buffer holds, you hear it.** A 100 ms level-load hitch with a 50 ms
buffer is an audible click.

The mitigation is not clever, it is just explicit:

```gdscript
# scripts/engine/procedural_audio.gd
## Seconds of audio the generator holds. Godot's default is 0.5, which is a
## lot of latency for an engine note that has to respond to the throttle.
## 0.12 is a reasonable compromise: about seven frames at 60 fps, so a single
## slow frame is absorbed, and the note still tracks the revs closely enough
## that you cannot hear the lag.
const BUFFER_SECONDS: float = 0.12
```

And, for the one place where a hitch is guaranteed — loading a circuit — fade
the voice out first rather than trying to survive it. A deliberate fade sounds
like a design decision; an underrun sounds like a bug.

If you genuinely need the callback model, `AudioStreamPlayback` can be driven
from a `Thread` you own, and then you are back to the C chapter's concurrency
problem with none of its `volatile` guarantees. Do not, unless something forces
you.

---

## Setting it up

```gdscript
# scripts/engine/procedural_audio.gd
class_name ProceduralAudio
extends Node

## Engine note, tyre scrub and countdown beeps, synthesised into one streamed
## voice. Everything degrades to a silent no-op when there is no output device
## (headless CI, a Pi at a bare TTY), so callers never need to check first.

const MIX_RATE: float = 22050.0
const BUFFER_SECONDS: float = 0.12

@onready var _player: AudioStreamPlayer = $EnginePlayer

var _playback: AudioStreamGeneratorPlayback = null
var _scratch := PackedVector2Array()

# Written by the game, read by the synth. No volatile, no atomics: with the
# generator model both happen on the main thread.
var rpm01: float = 0.0
var load01: float = 0.0
var skid01: float = 0.0
var volume: float = 0.7

# Oscillator state, carried across buffers. Continuity here is what stops the
# note clicking at every buffer boundary.
var _phase1: float = 0.0
var _phase2: float = 0.0
var _phase3: float = 0.0
var _blip_phase: float = 0.0
var _blip_frequency: float = 0.0
var _blip_samples: int = 0
var _noise_lp: float = 0.0
var _rng: int = 12345

func _ready() -> void:
    var generator := AudioStreamGenerator.new()
    generator.mix_rate = MIX_RATE
    generator.buffer_length = BUFFER_SECONDS
    _player.stream = generator
    _player.play()
    _playback = _player.get_stream_playback() as AudioStreamGeneratorPlayback
    if _playback == null:
        push_warning("AUDIO: no generator playback, running silent")
        return
    # One allocation, reused every frame. At 22 kHz a 60 fps frame is ~368
    # frames of audio; size for a slow frame and push only what fits.
    _scratch.resize(int(MIX_RATE * BUFFER_SECONDS) + 1)
```

### Why 22,050 Hz

Half of CD rate. An engine note whose fundamental tops out near 260 Hz has
essentially nothing above 8 kHz that matters, and the sawtooth's aliasing at
22 kHz is *part of the sound* — it adds grit that reads as mechanical noise
rather than as a defect.

The cost of halving the rate is halving the synthesis work, which on a Pi is a
real consideration.

One Godot-specific caveat: the audio server mixes at
`audio/driver/mix_rate` (44,100 by default), so a 22,050 Hz generator is
**resampled**. That is cheap and inaudible for this material, but it means the
saving is only in your synthesis loop, not in the mixer. If you want the
generator to run natively, set the project's mix rate to match — and then every
other sound in the game is 22 kHz too, which is probably not what you want.

---

## The synthesis loop

```gdscript
func _process(_delta: float) -> void:
    if _playback == null:
        return
    var available: int = _playback.get_frames_available()
    if available <= 0:
        return
    var count: int = mini(available, _scratch.size())
    _fill(count)
    # push_buffer takes the whole array, so resize the view rather than
    # pushing frame by frame: one call across the boundary instead of 368.
    _playback.push_buffer(_scratch.slice(0, count))

func _fill(count: int) -> void:
    # Idle around 48 Hz, redline near 260 Hz.
    var base: float = 48.0 + rpm01 * 212.0
    var inc1: float = base / MIX_RATE
    var inc2: float = (base * 2.02) / MIX_RATE      # slightly detuned octave
    var inc3: float = (base * 0.5) / MIX_RATE       # sub
    var engine_gain: float = 0.16 + 0.30 * load01 + 0.10 * rpm01

    for i: int in count:
        _phase1 = fmod(_phase1 + inc1, 1.0)
        _phase2 = fmod(_phase2 + inc2, 1.0)
        _phase3 = fmod(_phase3 + inc3, 1.0)

        var engine: float = (_saw(_phase1) * 0.55
                + _saw(_phase2) * 0.25
                + _saw(_phase3) * 0.35) * engine_gain

        # Tyre scrub: white noise through a one-pole low-pass.
        _noise_lp += (_random() - _noise_lp) * 0.35
        var scrub: float = _noise_lp * skid01 * 0.34

        var blip: float = 0.0
        if _blip_samples > 0:
            _blip_phase = fmod(_blip_phase + _blip_frequency / MIX_RATE, 1.0)
            # Fade the tail so the tone does not click when it ends.
            var env: float = minf(1.0, float(_blip_samples) / 900.0)
            blip = sin(_blip_phase * TAU) * 0.42 * env
            _blip_samples -= 1
        else:
            _blip_phase = 0.0

        var sample: float = clampf((engine + scrub + blip) * volume, -1.0, 1.0)
        _scratch[i] = Vector2(sample, sample)

## Band-limited-ish sawtooth: cheap, and the grit suits an engine note.
static func _saw(phase: float) -> float:
    return phase * 2.0 - 1.0

## Linear congruential generator. Deterministic, one multiply and one add, and
## nothing in the audio path should be calling into the engine's RNG 22,050
## times a second.
func _random() -> float:
    _rng = (_rng * 1664525 + 1013904223) & 0xFFFFFFFF
    return float((_rng >> 9) & 0x7FFFFF) / float(0x7FFFFF) * 2.0 - 1.0
```

### The oscillators

**Three sawtooths, detuned.** A single saw at 150 Hz sounds like a synthesiser.
Three at `f`, `2.02f` and `0.5f` sound like an engine, and the reason is the
`2.02` — an *exactly* doubled octave phase-locks with the fundamental and fuses
into one timbre, while 1% sharp beats against it at about 3 Hz, which the ear
reads as mechanical roughness.

The sub at `0.5f` supplies the body. Without it the note is thin and buzzy;
with it there is something under the car.

**`engine_gain = 0.16 + 0.30 * load + 0.10 * rpm`.** Idle is quiet but audible.
Load — throttle — is the biggest term, so lifting off is immediately audible
even at constant revs. Revs contribute a little, so a high-revving coast is
still louder than an idle.

**Phase continuity across buffers is mandatory.** `_phase1` is a member, not a
local. Resetting it per buffer produces a click at every boundary — 60 clicks a
second, which sounds like a buzz, and takes a surprisingly long time to
diagnose because it scales with frame rate.

### Tyre scrub

```gdscript
_noise_lp += (_random() - _noise_lp) * 0.35
var scrub: float = _noise_lp * skid01 * 0.34
```

White noise is too bright to be a tyre. The one-pole low-pass at coefficient
0.35 rolls off the top and leaves something closer to a hiss on tarmac. The
coefficient is a smoothing factor per *sample*, so at 22,050 Hz the corner
frequency is around 1.5 kHz — high enough to be present, low enough not to be
sand.

`skid01` is chapter 08's `slip`, unmodified. That is why `slip` exists.

### The blip

One sine, an amplitude envelope on the tail, and a sample counter. `_blip_samples`
is decremented in the loop, so the duration is exact regardless of frame rate.
The 900-sample fade is 41 ms — short enough not to be heard as a fade, long
enough to prevent the click that ending a non-zero-crossing sine produces.

```gdscript
## Fires a short tone; used for the countdown and lap chimes.
func blip(frequency: float, seconds: float) -> void:
    _blip_frequency = frequency
    _blip_samples = int(seconds * MIX_RATE)
```

Godot has an alternative worth knowing: `AudioStreamPolyphonic` (4.3+) lets one
player hold several concurrent one-shots, so the blips could be a pre-generated
`AudioStreamWAV` played through a separate polyphonic player. That would let two
blips overlap, which this design cannot. For a countdown that ticks once a
second it does not matter; for a "car alongside" warning that can retrigger, it
would.

---

## Wiring it to the game

```gdscript
# scripts/main.gd
func _process(_delta: float) -> void:
    var car: CarBody = race.player_car()
    # Revs from speed, not from a gearbox: this car has no gears, so the note
    # rises with road speed and dips only when the tyres let go.
    Audio.rpm01 = clampf(car.speed / tuning.top_speed, 0.0, 1.0)
    Audio.load01 = race.player_input.throttle
    Audio.skid01 = car.slip
```

```gdscript
# scripts/main.gd, in _ready()
race.countdown_tick.connect(_on_countdown_tick)

func _on_countdown_tick(seconds_remaining: int) -> void:
    if seconds_remaining > 0:
        Audio.blip(660.0, 0.12)
    else:
        Audio.blip(990.0, 0.45)      # the go tone, higher and longer
```

The signal from chapter 10 is doing exactly what it was designed for: the race
knows nothing about audio, and the audio knows nothing about the race.

Note the assignment happens in `_process`, not `_physics_process`. The mixer is
not tick-locked, the values are cosmetic, and updating them 120 times a second
would be 120 writes for 60 frames of output.

---

## What Godot adds: buses, effects and 3D

Everything so far is a port. These three are things the C engine does not have
and would be real work to add.

### Buses

`AudioServer` has a bus graph, editable in the editor's Audio tab. This project
uses four:

```
Master
├── Engine     (the procedural voice)
├── Tyres
└── UI         (countdown, menu)
```

Which buys: one volume slider per category in the options menu, ducking the
engine under the results screen, and — the interesting one — effects per bus.

```gdscript
## A low-pass on the engine bus, opened by speed. At low revs the note is
## muffled, as though heard through bodywork; flat out it opens up. This is
## two lines and does more for the sense of speed than any amount of synth
## tuning.
func _update_engine_filter(speed01: float) -> void:
    var filter: AudioEffectLowPassFilter = AudioServer.get_bus_effect(
            _engine_bus, 0) as AudioEffectLowPassFilter
    filter.cutoff_hz = lerpf(1200.0, 9000.0, speed01)
```

Doing that in the C engine means writing a filter into the synthesis loop and
mixing it per sample. Here it is a property on an effect that the mixer applies
in optimised C++.

### Positional audio for opponents

`AudioStreamPlayer3D` gives attenuation, panning and doppler for free:

```gdscript
# scripts/game/opponent_audio.gd
## Each AI car carries its own engine voice. Five extra procedural voices at
## 22 kHz is real CPU, so opponents use a much cheaper source: a short looped
## sample, pitched by speed. Only the player's car is fully synthesised.
func _ready() -> void:
    unit_size = 6.0
    max_distance = 24.0
    attenuation_model = AudioStreamPlayer3D.ATTENUATION_INVERSE_DISTANCE
    doppler_tracking = AudioStreamPlayer3D.DOPPLER_TRACKING_PHYSICS_STEP
```

`DOPPLER_TRACKING_PHYSICS_STEP` is the right choice for a body moved in
`_physics_process` — the other option, `IDLE_STEP`, samples the velocity at the
display rate and produces doppler jitter on a fixed-step body.

The comment records a real decision. Six fully synthesised voices at 22 kHz is
six times the per-sample loop, in GDScript, on the main thread, every frame. The
player's car is the one you are listening to; the others need to be *there*, not
detailed.

### Failing quietly

The C engine checks `IsAudioDeviceReady()` and sets an `available` flag that
every entry point tests. Godot's audio server always exists — with no device it
uses a dummy driver — so `_player.play()` on a headless machine is a no-op that
costs nothing and reports nothing.

That is convenient and slightly dangerous: the synthesis loop still runs. On a
headless CI box grinding through chapter 15's race tests, that is real CPU spent
generating samples nobody will hear. Hence:

```gdscript
func _ready() -> void:
    # No point synthesising into a dummy device: the CI machine runs a whole
    # race per test and the audio would be pure overhead.
    if DisplayServer.get_name() == "headless":
        set_process(false)
        return
```

---

## Exercises

1. **Hear the underrun.** Add `OS.delay_msec(200)` to `_process` once every 300
   frames. Listen. Then raise `BUFFER_SECONDS` to 0.5 and listen again. Which
   would you ship, and what does the higher value cost you when you lift off the
   throttle?

2. **Kill the detune.** Change `2.02` to `2.0` and drive. Describe the
   difference in one sentence, then try `2.1` and `2.005`. Which sounds most
   like an engine, and can you say why in terms of beat frequency?

3. **Break the phase.** Reset `_phase1`, `_phase2` and `_phase3` to zero at the
   top of `_fill`. Listen at 60 fps, then at 144 fps with vsync off. Explain why
   the artefact changes pitch with frame rate.

4. **Push frame by frame.** Replace `push_buffer` with a loop of `push_frame`
   calls and measure `_process` time with the profiler. How much of the cost is
   the synthesis and how much is crossing into the engine 368 times?

5. **Gears.** The car has no gearbox, so revs track speed linearly. Add five
   ratios: `rpm01` becomes the fractional part of `speed / top_speed * 5.0`,
   with a short gain dip at each change. Does it sound better? Does it make the
   car *feel* faster, and is that the same question?

6. **Move it off the main thread.** Drive the generator from a `Thread`, with a
   mutex around the three input floats. Measure the frame time saved and count
   the ways you can now deadlock. Then decide whether the C engine's `volatile`
   approach was reasonable after all.

---

Next: [15 — Testing without a window](15-testing.md)
