// Waypoint-following opponent driver.
//
// Steers at a point further down the racing line than the car currently is,
// picks a corner speed from how much the line bends ahead, and nudges its
// preferred line sideways to avoid cars it is closing on.
#ifndef GAME_AI_H
#define GAME_AI_H

#include "raylib.h"

#include "engine/spline.h"

#include "game/car.h"

typedef struct AIDriver {
    float skill;            // 0..1: corner speed, look-ahead and precision
    float aggression;       // 0..1: how late it lifts and how close it races
    float preferredOffset;  // resting lateral offset from the centreline
    float currentOffset;    // smoothed, includes avoidance
    float wobblePhase;      // keeps identical drivers from moving identically
    float recoverTimer;     // counts up while stuck, triggers a reverse
} AIDriver;

void AIDriverInit(AIDriver *ai, float skill, float aggression, float preferredOffset,
                  unsigned int seed);

// A neighbour the driver should avoid, in world space.
typedef struct AINeighbour {
    Vector2 position;
    Vector2 velocity;
} AINeighbour;

// Produces the control input for one tick. `hint` is the caller's cached
// spline index for this car and is updated in place.
CarInput AIThink(AIDriver *ai, const Car *car, const CarTuning *tuning,
                 const Spline *spline, int *hint,
                 const AINeighbour *neighbours, int neighbourCount, float dt);

#endif // GAME_AI_H
