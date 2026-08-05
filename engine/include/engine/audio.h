// Procedural audio.
//
// The Kenney kit ships no sound, so the engine note, tyre scrub and countdown
// beeps are synthesised into a single streamed voice. Everything degrades to a
// silent no-op when no audio device is available (headless boxes, most Pis at
// a bare TTY), so callers never need to check first.
#ifndef ENGINE_AUDIO_H
#define ENGINE_AUDIO_H

#include <stdbool.h>

bool AudioEngineInit(void);
void AudioEngineShutdown(void);
bool AudioEngineAvailable(void);

// `rpm01` and `load` are 0..1; `skid` drives the tyre-scrub noise layer.
void AudioEngineSetMotor(float rpm01, float load, float skid);

// Master gain, 0..1.
void AudioEngineSetVolume(float volume);

// Fires a short tone; used for the countdown and lap chimes.
void AudioEngineBlip(float frequency, float seconds);

#endif // ENGINE_AUDIO_H
