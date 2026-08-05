#include "engine/audio.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "raylib.h"
#include "raymath.h"

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

static float FastRandom(unsigned int *state)
{
    *state = *state * 1664525u + 1013904223u;
    return ((float)((*state >> 9) & 0x7FFFFF) / (float)0x7FFFFF) * 2.0f - 1.0f;
}

// Band-limited-ish sawtooth: cheap, and the grit suits an engine note.
static float Saw(float phase) { return phase * 2.0f - 1.0f; }

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

    // Idle around 48 Hz, redline near 260 Hz.
    float base = 48.0f + rpm * 212.0f;
    float inc1 = base / AUDIO_RATE;
    float inc2 = (base * 2.02f) / AUDIO_RATE;      // slightly detuned octave
    float inc3 = (base * 0.5f) / AUDIO_RATE;       // sub

    float engineGain = 0.16f + 0.30f * load + 0.10f * rpm;

    for (unsigned int i = 0; i < frames; i++) {
        phase1 += inc1; if (phase1 >= 1.0f) phase1 -= 1.0f;
        phase2 += inc2; if (phase2 >= 1.0f) phase2 -= 1.0f;
        phase3 += inc3; if (phase3 >= 1.0f) phase3 -= 1.0f;

        float engine = (Saw(phase1) * 0.55f + Saw(phase2) * 0.25f + Saw(phase3) * 0.35f);
        engine *= engineGain;

        // Tyre scrub: white noise through a one-pole low-pass.
        float noise = FastRandom(&rng);
        noiseLp += (noise - noiseLp) * 0.35f;
        float scrub = noiseLp * skid * 0.34f;

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

        float sample = (engine + scrub + blip) * volume;
        if (sample > 1.0f) sample = 1.0f;
        if (sample < -1.0f) sample = -1.0f;
        out[i] = (short)(sample * 32000.0f);
    }
}

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
    TraceLog(LOG_INFO, "AUDIO: procedural engine voice at %d Hz", AUDIO_RATE);
    return true;
}

void AudioEngineShutdown(void)
{
    if (g_audio.available) {
        StopAudioStream(g_audio.stream);
        UnloadAudioStream(g_audio.stream);
    }
    if (IsAudioDeviceReady()) CloseAudioDevice();
    memset((void *)&g_audio, 0, sizeof(g_audio));
}

bool AudioEngineAvailable(void) { return g_audio.available; }

void AudioEngineSetMotor(float rpm01, float load, float skid)
{
    if (!g_audio.available) return;
    g_audio.rpm01 = Clamp(rpm01, 0.0f, 1.0f);
    g_audio.load = Clamp(load, 0.0f, 1.0f);
    g_audio.skid = Clamp(skid, 0.0f, 1.0f);
}

void AudioEngineSetVolume(float volume)
{
    g_audio.volume = Clamp(volume, 0.0f, 1.0f);
}

void AudioEngineBlip(float frequency, float seconds)
{
    if (!g_audio.available) return;
    g_audio.blipFreq = frequency;
    g_audio.blipSamples = (int)(seconds * AUDIO_RATE);
}
