// Heads-up display: lap and position readouts, timers, minimap, countdown and
// the end-of-race results table.
#ifndef GAME_HUD_H
#define GAME_HUD_H

#include <stdbool.h>

#include "raylib.h"

#include "game/race.h"

// One kit unit is treated as ~6 m, which puts top speed around 140 km/h.
#define HUD_UNITS_TO_KMH 21.6f

typedef struct HudStats {
    int fps;
    int drawnChunks;
    int totalChunks;
    int triangles;
    int lightCount;
    int skidMarks;
    bool audioActive;
    bool night;
} HudStats;

void HudDraw(const Race *race, bool paused);
void HudDrawDebug(const Race *race, const HudStats *stats);

#endif // GAME_HUD_H
