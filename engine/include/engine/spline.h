// Closed centreline built from a level's waypoints.
//
// The waypoint loop is smoothed with Catmull-Rom and resampled at a roughly
// uniform spacing, which makes arc-length queries (lap progress, AI look-ahead,
// off-track tests) a simple walk over line segments.
#ifndef ENGINE_SPLINE_H
#define ENGINE_SPLINE_H

#include <stdbool.h>

#include "raylib.h"

#include "engine/arena.h"
#include "engine/level.h"

typedef struct SplineSample {
    Vector3 position;
    Vector3 tangent;        // unit and horizontal, points down-track
    float width;            // drivable half-width is width * 0.5f
    float distance;         // arc length from sample 0, measured on the ground
    float grade;            // rise over run: +0.1 climbs one unit every ten
} SplineSample;

typedef struct Spline {
    SplineSample *samples;
    int count;
    float length;           // total loop length
    Arena arena;
} Spline;

// Resamples the level's waypoint loop. `spacing` is the target distance between
// samples in world units. Returns false if the level has fewer than 3 waypoints.
bool SplineBuild(Spline *spline, const Level *level, float spacing);
void SplineFree(Spline *spline);

// Nearest point on the centreline. `hintIndex` is the caller's previous result
// (pass a negative value for a full search) and is updated in place; the
// windowed search around it keeps per-frame queries cheap.
typedef struct SplineQuery {
    Vector3 position;       // closest point on the centreline, including height
    Vector3 tangent;        // horizontal
    float distance;         // arc length of that point (lap progress)
    float lateral;          // signed offset from the centreline, + is left
    float halfWidth;        // drivable half-width there
    float grade;            // slope of the track under this point
    int index;              // sample index the result came from
} SplineQuery;

SplineQuery SplineClosest(const Spline *spline, Vector3 point, int *hintIndex);

// Point on the centreline at an arc length, wrapping around the loop.
Vector3 SplinePointAt(const Spline *spline, float distance);
SplineSample SplineSampleAt(const Spline *spline, float distance);

// Shortest signed difference `a - b` around the loop, in (-length/2, length/2].
float SplineWrapDelta(const Spline *spline, float a, float b);

#endif // ENGINE_SPLINE_H
