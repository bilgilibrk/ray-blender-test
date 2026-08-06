// Rubber left on the road: the black streaks a car lays down when it locks a
// wheel under braking or slides a corner sideways.
//
// The marks are a ring of flat quads laid pair by pair as the rear wheels move,
// each fading out on its own clock. Nothing here feeds back into the physics —
// a skid mark is a record of what the car already did.
#ifndef GAME_SKID_H
#define GAME_SKID_H

#include <stdbool.h>

#include "raylib.h"

#include "game/race.h"

// Marks alive at once, across the whole field. The oldest is overwritten when
// the ring wraps, so a long fight through a chicane costs older rubber rather
// than more memory.
#define SKID_MAX_QUADS 1536

// Two rear wheels per car, each laying its own continuous strip.
#define SKID_EMITTERS (RACE_MAX_RACERS * 2)

typedef struct SkidQuad {
    Vector3 corner[4];
    float life;                 // seconds of fade left, 0 when free
    float peak;                 // 0..1, how hard the car was marking when laid
} SkidQuad;

typedef struct SkidEmitter {
    Vector3 last;               // where this wheel last put rubber down
    bool laying;
} SkidEmitter;

typedef struct SkidTrails {
    SkidQuad quads[SKID_MAX_QUADS];
    int next;                   // ring cursor
    SkidEmitter emitters[SKID_EMITTERS];
} SkidTrails;

void SkidInit(SkidTrails *trails);

// Wipes every mark. For a race restart, which teleports the cars.
void SkidClear(SkidTrails *trails);

// Ages the existing marks and lays new ones under any car that is sliding,
// braking hard enough to lock up, or on the handbrake.
void SkidUpdate(SkidTrails *trails, const Race *race, float dt);

void SkidDraw(const SkidTrails *trails);

// How many marks are currently visible, for the debug overlay.
int SkidLiveCount(const SkidTrails *trails);

#endif // GAME_SKID_H
