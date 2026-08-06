// Drives a whole race with no window. Everything below the renderer is free of
// OpenGL, so the physics, AI and lap logic can be simulated far faster than
// real time and asserted on.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "raylib.h"

#include "engine/collide.h"
#include "engine/level.h"
#include "engine/spline.h"

#include "game/car.h"
#include "game/race.h"

#include "tests.h"

// Every circuit the game ships. All of them have to be driveable, not just the
// one the player starts on.
static const char *kCircuits[] = {
    "levels/circuit01.level.json",
    "levels/circuit02.level.json",
};

#define STEP (1.0f / 120.0f)



typedef struct SimResult {
    float wallTime;
    int finished;
    float bestLap;
    float worstOffTrackFraction;
    float leaderAverageSpeed;
} SimResult;

// Runs until every car finishes or `limitSeconds` of simulated time elapses.
static SimResult Simulate(Race *race, float limitSeconds, bool trace)
{
    SimResult result = { 0 };
    result.bestLap = 0.0f;

    int steps = (int)(limitSeconds / STEP);
    int offTrackTicks[RACE_MAX_RACERS] = { 0 };
    int tickCount = 0;
    float speedSum = 0.0f;
    float nextTrace = 0.0f;
    CarInput idle = { 0 };

    for (int i = 0; i < steps; i++) {
        RaceUpdate(race, idle, STEP);
        tickCount++;

        for (int r = 0; r < race->racerCount; r++) {
            if (!race->racers[r].car.onTrack) offTrackTicks[r]++;
        }
        speedSum += race->racers[race->standings[0]].car.speed;

        if (trace && race->elapsed >= nextTrace && race->state == RACE_RUNNING) {
            const Racer *r = &race->racers[0];
            printf("    t=%5.1f  spd=%5.2f  thr=%.2f brk=%.2f str=%+.2f  %s  "
                   "arc=%6.2f lap=%d gate=%2d  rec=%.2f\n",
                   (double)race->elapsed, (double)r->car.speed,
                   (double)r->input.throttle, (double)r->input.brake, (double)r->input.steer,
                   r->car.onTrack ? "on " : "OFF", (double)r->progress.splineDistance,
                   r->progress.lap, r->progress.nextCheckpoint, (double)r->ai.recoverTimer);
            nextTrace += 1.0f;
        }
        if (race->state == RACE_FINISHED) break;
    }

    result.wallTime = race->elapsed;
    for (int r = 0; r < race->racerCount; r++) {
        if (race->racers[r].progress.finished) result.finished++;
        float best = race->racers[r].progress.bestLapTime;
        if (best > 0.0f && (result.bestLap <= 0.0f || best < result.bestLap)) result.bestLap = best;
        float fraction = (tickCount > 0) ? (float)offTrackTicks[r] / (float)tickCount : 0.0f;
        if (fraction > result.worstOffTrackFraction) result.worstOffTrackFraction = fraction;
    }
    result.leaderAverageSpeed = (tickCount > 0) ? speedSum / (float)tickCount : 0.0f;
    return result;
}

static void RunCircuitTests(const char *path)
{
    printf("  --- %s\n", path);

    Level level;
    if (!LevelLoad(&level, path)) {
        CHECK(false, "could not load %s (run `make level` first)", path);
        return;
    }

    CHECK(level.propCount > 100, "level has %d props, expected a full track", level.propCount);
    CHECK(level.waypointCount >= 3, "level has %d waypoints", level.waypointCount);
    CHECK(level.spawnCount >= 1, "level has %d spawns", level.spawnCount);
    CHECK(level.checkpointCount >= 3, "level has %d checkpoints", level.checkpointCount);

    Spline spline;
    CHECK(SplineBuild(&spline, &level, 0.22f), "spline build");
    CHECK(spline.length > 10.0f, "loop length %.2f looks wrong", (double)spline.length);

    CollisionWorld collision;
    CHECK(CollisionWorldBuild(&collision, level.colliders, level.colliderCount, 2.0f),
          "collision build");

    // No collider should sit on the racing surface: that would wall off the track.
    int blocking = 0;
    for (int i = 0; i < level.colliderCount; i++) {
        int hint = -1;
        SplineQuery q = SplineClosest(&spline, level.colliders[i].center, &hint);
        float reach = sqrtf(level.colliders[i].halfExtents.x * level.colliders[i].halfExtents.x +
                            level.colliders[i].halfExtents.y * level.colliders[i].halfExtents.y);
        if (fabsf(q.lateral) - reach < q.halfWidth * 0.8f) {
            blocking++;
            if (g_verbose) {
                printf("    collider %d at (%.2f, %.2f) is %.2f from the centre line "
                       "(half width %.2f, reach %.2f)\n", i, (double)level.colliders[i].center.x,
                       (double)level.colliders[i].center.z, (double)fabsf(q.lateral),
                       (double)q.halfWidth, (double)reach);
            }
        }
    }
    CHECK(blocking == 0, "%d collider(s) intrude on the racing surface", blocking);

    // --- how fast can the circuit actually be driven? -------------------------
    // Corner radius caps speed at maxYawRate * radius, so a layout can be
    // accidentally undriveable no matter how good the AI is. Report the profile.
    {
        CarTuning tuning = CarDefaultTuning();
        const float step = 0.55f;
        float worstRadius = 1e30f;
        float worstAt = 0.0f;
        float slowSum = 0.0f;
        int slowSamples = 0;

        for (float d = 0.0f; d < spline.length; d += 0.25f) {
            SplineSample a = SplineSampleAt(&spline, d);
            SplineSample b = SplineSampleAt(&spline, d + step);
            float turn = fabsf(atan2f(a.tangent.z * b.tangent.x - a.tangent.x * b.tangent.z,
                                      a.tangent.x * b.tangent.x + a.tangent.z * b.tangent.z));
            float radius = (turn > 1e-4f) ? step / turn : 1e30f;
            if (radius < worstRadius) { worstRadius = radius; worstAt = d; }

            float cap = tuning.maxYawRate * radius;
            if (cap < tuning.topSpeed) { slowSum += cap; slowSamples++; }
        }
        printf("  tightest radius %.2f at arc %.1f (caps speed at %.2f u/s); "
               "%d%% of the lap is corner-limited\n",
               (double)worstRadius, (double)worstAt,
               (double)(tuning.maxYawRate * worstRadius),
               (int)(100.0f * (float)slowSamples / (spline.length / 0.25f)));

        CHECK(tuning.maxYawRate * worstRadius > 1.8f,
              "tightest corner (radius %.2f) caps speed at %.2f u/s — undriveable",
              (double)worstRadius, (double)(tuning.maxYawRate * worstRadius));
    }

    // --- spawn sanity -------------------------------------------------------
    for (int i = 0; i < level.spawnCount; i++) {
        int hint = -1;
        SplineQuery q = SplineClosest(&spline, level.spawns[i].position, &hint);
        CHECK(fabsf(q.lateral) <= q.halfWidth,
              "spawn %d is %.2f off the centre line but the track half width is %.2f",
              i, (double)fabsf(q.lateral), (double)q.halfWidth);

        // The grid must face down-track, not into the scenery.
        float want = atan2f(q.tangent.x, q.tangent.z) * RAD2DEG;
        float diff = fmodf(level.spawns[i].yawDeg - want + 540.0f, 360.0f) - 180.0f;
        CHECK(fabsf(diff) < 25.0f, "spawn %d faces %.1f deg, track runs at %.1f deg",
              i, (double)level.spawns[i].yawDeg, (double)want);
    }

    // --- full race ------------------------------------------------------------
    Race race;
    CHECK(RaceInit(&race, &level, &spline, &collision, 6), "race init");
    race.autopilot = true;

    if (g_verbose) printf("  telemetry (car 0):\n");
    SimResult run = Simulate(&race, 400.0f, g_verbose);

    printf("  %d/%d finished, race time %.1fs, best lap %.2fs, leader avg speed %.2f u/s, "
           "worst off-track %.0f%%\n",
           run.finished, race.racerCount, (double)run.wallTime, (double)run.bestLap,
           (double)run.leaderAverageSpeed, (double)(run.worstOffTrackFraction * 100.0f));

    CHECK(run.finished == race.racerCount, "only %d of %d cars finished within 400s",
          run.finished, race.racerCount);
    CHECK(run.leaderAverageSpeed > 2.5f,
          "leader averaged %.2f u/s — the field is crawling", (double)run.leaderAverageSpeed);
    CHECK(run.bestLap > 5.0f && run.bestLap < 60.0f,
          "best lap of %.2fs is outside the plausible range", (double)run.bestLap);
    CHECK(run.worstOffTrackFraction < 0.25f,
          "a car spent %.0f%% of the race off track", (double)(run.worstOffTrackFraction * 100.0f));

    // Every finisher should have completed exactly the required number of laps.
    for (int i = 0; i < race.racerCount; i++) {
        const RaceProgress *p = &race.racers[i].progress;
        CHECK(!p->finished || p->lap == race.totalLaps + 1,
              "car %d finished on lap counter %d, expected %d",
              i, p->lap, race.totalLaps + 1);
    }

    // Finishing positions must be unique and cover 1..N.
    int seen[RACE_MAX_RACERS + 1] = { 0 };
    for (int i = 0; i < race.racerCount; i++) {
        int pos = race.racers[i].progress.finishPosition;
        if (pos >= 1 && pos <= race.racerCount) seen[pos]++;
    }
    for (int pos = 1; pos <= race.racerCount; pos++) {
        CHECK(seen[pos] == 1, "finishing position %d was awarded %d times", pos, seen[pos]);
    }

    RaceFree(&race);
    CollisionWorldFree(&collision);
    SplineFree(&spline);
    LevelUnload(&level);
}

void RunRaceTests(void)
{
    for (int i = 0; i < (int)(sizeof kCircuits / sizeof kCircuits[0]); i++) {
        RunCircuitTests(kCircuits[i]);
    }
}
