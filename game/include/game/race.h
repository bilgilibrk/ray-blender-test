// Race session: the field of cars, their progress round the circuit, and the
// countdown/running/finished state machine.
//
// Progress is measured as arc length along the centreline rather than by
// geometric gate crossings, which makes lap counting immune to a car clipping
// the edge of a checkpoint or being shoved sideways through one. The gates are
// still enforced in order, so cutting the course does not advance a lap.
#ifndef GAME_RACE_H
#define GAME_RACE_H

#include <stdbool.h>

#include "raylib.h"

#include "engine/arena.h"
#include "engine/collide.h"
#include "engine/level.h"
#include "engine/spline.h"

#include "game/ai.h"
#include "game/car.h"

#define RACE_MAX_RACERS 8
#define RACE_NAME_MAX 16

typedef struct RaceProgress {
    int lap;                    // times the finish line has been crossed
    int nextCheckpoint;
    int splineHint;
    float splineDistance;
    float lastSplineDistance;
    float score;                // monotonic distance round the loop, for sorting

    float lapStartTime;
    float lastLapTime;
    float bestLapTime;
    float finishTime;
    bool finished;
    int finishPosition;

    float offTrackTime;
    float stuckTime;
} RaceProgress;

typedef struct Racer {
    Car car;
    CarInput input;
    RaceProgress progress;
    AIDriver ai;

    bool isPlayer;
    const char *model;
    Color tint;
    char name[RACE_NAME_MAX];
} Racer;

typedef enum RaceState {
    RACE_COUNTDOWN = 0,
    RACE_RUNNING,
    RACE_FINISHED,
} RaceState;

typedef struct Race {
    const Level *level;
    const Spline *spline;
    const CollisionWorld *collision;
    CarTuning tuning;

    Racer racers[RACE_MAX_RACERS];
    int racerCount;
    int playerIndex;

    RaceState state;
    float countdown;
    float elapsed;
    int totalLaps;
    int finishedCount;

    // Hands the player's car to the AI. Used for attract mode and for driving
    // the game headlessly in tests.
    bool autopilot;

    float *checkpointDistance;  // arc length of each gate
    int checkpointCount;
    float startDistance;        // arc length of the finish line

    int standings[RACE_MAX_RACERS];
    Arena arena;
} Race;

// `racerCount` is clamped to the level's spawn count and RACE_MAX_RACERS.
bool RaceInit(Race *race, const Level *level, const Spline *spline,
              const CollisionWorld *collision, int racerCount);
void RaceFree(Race *race);

// Returns everyone to the grid and restarts the countdown.
void RaceReset(Race *race);

// One fixed simulation step.
void RaceUpdate(Race *race, CarInput playerInput, float dt);

// Puts a car back on the racing line facing the right way.
void RaceRespawn(Race *race, int index);

// 1-based finishing order position of a racer.
int RacePositionOf(const Race *race, int index);

// Formats seconds as m:ss.mmm into `buffer`.
const char *RaceFormatTime(float seconds, char *buffer, int size);

#endif // GAME_RACE_H
