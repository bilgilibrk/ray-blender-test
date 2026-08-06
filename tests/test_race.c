// Drives a whole race with no window. Everything below the renderer is free of
// OpenGL, so the physics, AI and lap logic can be simulated far faster than
// real time and asserted on.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    int sandTicks;              // car-ticks spent in a gravel trap
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
            if (race->racers[r].car.inSand) result.sandTicks++;
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

    // --- sand traps ----------------------------------------------------------
    // The art and the physics regions are authored side by side and neither is
    // derived from the other, so this is where they are held to each other: the
    // gravel must start off the tarmac and stop within reach of it. A trap
    // covering the lane would be felt only through a bug; one out in the field
    // would be gravel nobody can reach.
    {
        int onLine = 0, adrift = 0, unreachable = 0;
        for (int i = 0; i < spline.count; i++) {
            Vector3 p = spline.samples[i].position;
            if (LevelInSandtrap(&level, p.x, p.z)) onLine++;
        }
        for (int i = 0; i < level.sandtrapCount; i++) {
            int hint = -1;
            SplineQuery q = SplineClosest(&spline, level.sandtraps[i].center, &hint);
            float lateral = fabsf(q.lateral);
            float reach = sqrtf(level.sandtraps[i].halfExtents.x *
                                    level.sandtraps[i].halfExtents.x +
                                level.sandtraps[i].halfExtents.y *
                                    level.sandtraps[i].halfExtents.y);
            if (lateral - reach > q.halfWidth * 1.6f || lateral > 1.4f) {
                adrift++;
                if (g_verbose) {
                    printf("    sand trap %d at (%.2f, %.2f) is %.2f from the centre line\n",
                           i, (double)level.sandtraps[i].center.x,
                           (double)level.sandtraps[i].center.z, (double)lateral);
                }
            }

            // And the thing that actually matters: leaving the road here has
            // to put you in it. Probe just off the tarmac, on the line joining
            // the centre line to the trap.
            Vector3 from = q.position;
            float dx = level.sandtraps[i].center.x - from.x;
            float dz = level.sandtraps[i].center.z - from.z;
            float len = sqrtf(dx * dx + dz * dz);
            if (len > 1e-4f) {
                float probe = q.halfWidth + 0.25f;
                if (!LevelInSandtrap(&level, from.x + dx / len * probe,
                                     from.z + dz / len * probe)) {
                    unreachable++;
                }
            }
        }
        // Nothing solid may stand in a trap. A barrier or a lamp post inside
        // the gravel is a wall reached before the gravel has slowed anybody
        // down, which is the one arrangement worse than having no trap at all.
        int solidInTrap = 0;
        for (int i = 0; i < level.colliderCount; i++) {
            const LevelCollider *c = &level.colliders[i];
            Obb2 box = { .center = { c->center.x, c->center.z },
                         .halfExtents = c->halfExtents,
                         .yaw = c->yawDeg * DEG2RAD };
            Vector2 corners[4];
            Obb2Corners(box, corners);
            bool hit = LevelInSandtrap(&level, box.center.x, box.center.y);
            for (int k = 0; k < 4 && !hit; k++) {
                hit = LevelInSandtrap(&level, corners[k].x, corners[k].y);
            }
            if (hit) solidInTrap++;
        }

        // And every trap should have gravel drawn on it. The art is placed by
        // the same hand as the boxes but through a different mechanism, so a
        // level with one and not the other is a real possibility.
        int sandPieces = 0;
        for (int i = 0; i < level.propCount; i++) {
            if (strstr(level.props[i].model, "Sand")) sandPieces++;
        }

        printf("  %d sand traps over %d sand pieces\n", level.sandtrapCount, sandPieces);
        CHECK(level.sandtrapCount > 0, "the circuit has no run-off gravel at all");
        CHECK(sandPieces > 0, "%d sand traps but no sand drawn anywhere",
              level.sandtrapCount);
        CHECK(solidInTrap == 0, "%d collider(s) stand inside a sand trap", solidInTrap);
        CHECK(onLine == 0, "%d centre-line sample(s) sit in a sand trap", onLine);
        CHECK(adrift == 0, "%d sand trap(s) are nowhere near the track", adrift);
        CHECK(unreachable == 0, "%d sand trap(s) cannot be reached by running wide",
              unreachable);
    }

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
           "worst off-track %.0f%%, %.1f car-seconds in gravel\n",
           run.finished, race.racerCount, (double)run.wallTime, (double)run.bestLap,
           (double)run.leaderAverageSpeed, (double)(run.worstOffTrackFraction * 100.0f),
           (double)(run.sandTicks * STEP));

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

    // --- running wide into a trap ---------------------------------------------
    // The surface tests above prove gravel is slow and the geometry tests prove
    // the boxes sit beside the road. This is the two of them together, in the
    // level the game ships: put a car off the road at every trap, flat out, and
    // it should be walking a second later and still be in there.
    if (level.sandtrapCount > 0) {
        int tested = 0;
        float worstTravel = 0.0f;
        float bestGetaway = 0.0f;   // the least any car was slowed to
        for (int i = 0; i < level.sandtrapCount; i++) {
            Race drive;
            if (!RaceInit(&drive, &level, &spline, &collision, 1)) break;
            drive.state = RACE_RUNNING;         // no countdown to sit through
            drive.countdown = 0.0f;

            int hint = -1;
            SplineQuery q = SplineClosest(&spline, level.sandtraps[i].center, &hint);
            float dx = level.sandtraps[i].center.x - q.position.x;
            float dz = level.sandtraps[i].center.z - q.position.z;
            float len = sqrtf(dx * dx + dz * dz);
            if (len < 1e-4f) { RaceFree(&drive); continue; }

            float out = q.halfWidth + 0.3f;
            Vector2 start = { q.position.x + dx / len * out, q.position.z + dz / len * out };
            Car *car = &drive.racers[0].car;
            CarInit(car, start, atan2f(q.tangent.x, q.tangent.z));
            car->height = q.position.y;
            Vector2 forward = CarForward(car);

            // A box at the end of a band points its car straight out of the
            // gravel, because that is where the corner stops having any. That
            // is the geometry doing its job, not a fault, so those are left out
            // rather than asserted on.
            if (!LevelInSandtrap(&level, start.x + forward.x * 0.5f,
                                 start.y + forward.y * 0.5f)) {
                RaceFree(&drive);
                continue;
            }

            float entry = drive.tuning.topSpeed * 0.85f;
            car->velocity = (Vector2){ forward.x * entry, forward.y * entry };

            CarInput flatOut = { .throttle = 1.0f };
            float slowest = 1e30f;
            for (int t = 0; t < 120; t++) {
                RaceUpdate(&drive, flatOut, STEP);
                // After a quarter second, by which time the gravel has had its
                // say. A car may well crawl out the far end of a band before
                // the second is up — the trap has still done its job.
                if (t > 30 && car->speed < slowest) slowest = car->speed;
            }

            float travel = sqrtf((car->position.x - start.x) * (car->position.x - start.x) +
                                 (car->position.y - start.y) * (car->position.y - start.y));
            if (travel > worstTravel) worstTravel = travel;
            if (slowest > bestGetaway) bestGetaway = slowest;
            tested++;
            RaceFree(&drive);
        }
        printf("  ran wide into %d trap stretches: worst got %.2f units in a second, "
               "best was still doing %.2f u/s\n",
               tested, (double)worstTravel, (double)bestGetaway);
        CHECK(tested >= level.sandtrapCount / 2, "only %d of %d traps were long enough to "
              "test — the bands have gone to pieces", tested, level.sandtrapCount);
        // Grass over the same second is worth about 3.4 units, tarmac 5.6.
        CHECK(worstTravel < 2.0f, "a car crossed %.2f units of gravel in a second",
              (double)worstTravel);
        CHECK(bestGetaway < 1.3f, "one trap never slowed its car below %.2f u/s",
              (double)bestGetaway);
    }

    RaceFree(&race);
    CollisionWorldFree(&collision);
    SplineFree(&spline);
    LevelUnload(&level);
}

// Drives one car across a given surface for a second, flat out, from racing
// speed, and reports where it ended up. No level and no track: just the tyre
// model against the ground it is standing on.
static void SurfaceRun(const CarTuning *tuning, const CarSurface *surface,
                       float *outSpeed, float *outTravel)
{
    Car car;
    CarInit(&car, (Vector2){ 0.0f, 0.0f }, 0.0f);          // facing +Z
    car.velocity = (Vector2){ 0.0f, tuning->topSpeed };
    car.forwardSpeed = tuning->topSpeed;

    CarInput flatOut = { .throttle = 1.0f };
    for (int i = 0; i < 120; i++) CarUpdate(&car, tuning, flatOut, surface, STEP);

    *outSpeed = car.speed;
    *outTravel = car.position.y;
}

static void RunSurfaceTests(void)
{
    CarTuning tuning = CarDefaultTuning();
    CarSurface tarmac = { .grip = tuning.gripTarmac, .speedScale = 1.0f };
    CarSurface grass = { .grip = tuning.gripGrass, .speedScale = tuning.offTrackSpeedScale };
    CarSurface sand = { .grip = tuning.gripSand, .speedScale = tuning.sandSpeedScale,
                        .drag = tuning.sandDrag };

    float onTarmac, onGrass, inGravel, farTarmac, farGrass, farGravel;
    SurfaceRun(&tuning, &tarmac, &onTarmac, &farTarmac);
    SurfaceRun(&tuning, &grass, &onGrass, &farGrass);
    SurfaceRun(&tuning, &sand, &inGravel, &farGravel);

    printf("  a second at full throttle from %.2f u/s: tarmac %.2f u/s (%.2f units), "
           "grass %.2f (%.2f), gravel %.2f (%.2f)\n",
           (double)tuning.topSpeed, (double)onTarmac, (double)farTarmac,
           (double)onGrass, (double)farGrass, (double)inGravel, (double)farGravel);

    // Gravel is meant to be the end of your lap, not a slower line through the
    // corner: a car cannot drive out of it at anything better than a walk.
    CHECK(inGravel < 1.2f, "a car flat out in gravel still does %.2f u/s", (double)inGravel);
    CHECK(inGravel < onGrass * 0.5f,
          "gravel (%.2f u/s) is barely slower than grass (%.2f u/s)",
          (double)inGravel, (double)onGrass);
    CHECK(farGravel < 1.6f, "a car crossed %.2f units of gravel in a second",
          (double)farGravel);
    // ...but it does not stop dead, or a trap would be a wall.
    CHECK(inGravel > 0.3f, "gravel brought the car to a standstill (%.2f u/s)",
          (double)inGravel);
    // Top speed is a ceiling the drag never quite lets the engine hold, so the
    // tarmac case settles below it — but nowhere near the other two.
    CHECK(onTarmac > tuning.topSpeed * 0.7f,
          "the tarmac case is broken: %.2f u/s", (double)onTarmac);
}

void RunRaceTests(void)
{
    RunSurfaceTests();
    for (int i = 0; i < (int)(sizeof kCircuits / sizeof kCircuits[0]); i++) {
        RunCircuitTests(kCircuits[i]);
    }
}
