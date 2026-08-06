#include "game/race.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "raymath.h"

#define RACE_COUNTDOWN_SECONDS 3.6f
#define RACE_OFFTRACK_RESPAWN 6.0f      // seconds stranded before a rescue
#define RACE_STUCK_RESPAWN 4.0f

// A gate counts as passed when the car's arc length crosses it moving forward.
// The window rejects the huge jump that a wrap-around or a respawn produces.
#define RACE_GATE_WINDOW 3.0f

static const char *kCarModels[] = {
    "raceCarRed", "raceCarWhite", "raceCarGreen", "raceCarOrange",
};

static const char *kDriverNames[RACE_MAX_RACERS] = {
    "YOU", "VOSS", "IKEDA", "MARCH", "DELGADO", "KOVAC", "ABIOLA", "RENN",
};

static const Color kCarTints[RACE_MAX_RACERS] = {
    { 255, 255, 255, 255 }, { 236, 220, 120, 255 }, { 150, 205, 255, 255 },
    { 255, 170, 120, 255 }, { 200, 160, 255, 255 }, { 160, 240, 190, 255 },
    { 255, 150, 190, 255 }, { 190, 190, 200, 255 },
};

const char *RaceFormatTime(float seconds, char *buffer, int size)
{
    if (seconds <= 0.0f || seconds >= 5999.0f) {
        snprintf(buffer, (size_t)size, "--:--.---");
        return buffer;
    }
    int minutes = (int)(seconds / 60.0f);
    float rest = seconds - (float)minutes * 60.0f;
    snprintf(buffer, (size_t)size, "%d:%06.3f", minutes, (double)rest);
    return buffer;
}

// Distance travelled round the loop measured from the finish line.
static float LapRelative(const Race *race, float distance)
{
    float rel = distance - race->startDistance;
    if (rel < 0.0f) rel += race->spline->length;
    return rel;
}

static void PlaceOnGrid(Race *race, int index)
{
    Racer *racer = &race->racers[index];
    const Level *level = race->level;

    Vector2 position = { 0.0f, 0.0f };
    Vector3 spawn3D = { 0.0f, 0.0f, 0.0f };
    float yaw = 0.0f;
    if (level->spawnCount > 0) {
        const LevelSpawn *spawn = &level->spawns[index % level->spawnCount];
        spawn3D = spawn->position;
        position = (Vector2){ spawn->position.x, spawn->position.z };
        yaw = spawn->yawDeg * DEG2RAD;
    }

    CarInit(&racer->car, position, yaw);

    RaceProgress *p = &racer->progress;
    memset(p, 0, sizeof(*p));
    p->splineHint = -1;
    p->bestLapTime = 0.0f;

    Vector3 here = { position.x, spawn3D.y, position.y };
    SplineQuery q = SplineClosest(race->spline, here, &p->splineHint);
    racer->car.height = q.position.y;
    racer->car.pitch = atanf(q.grade);
    p->splineDistance = q.distance;
    p->lastSplineDistance = q.distance;
    p->score = LapRelative(race, q.distance) - race->spline->length;   // still behind the line
}

bool RaceInit(Race *race, const Level *level, const Spline *spline,
              const CollisionWorld *collision, int racerCount)
{
    memset(race, 0, sizeof(*race));
    race->level = level;
    race->spline = spline;
    race->collision = collision;
    race->tuning = CarDefaultTuning();
    race->totalLaps = level->laps;
    race->playerIndex = 0;

    if (racerCount > RACE_MAX_RACERS) racerCount = RACE_MAX_RACERS;
    if (level->spawnCount > 0 && racerCount > level->spawnCount) racerCount = level->spawnCount;
    if (racerCount < 1) racerCount = 1;
    race->racerCount = racerCount;

    if (!ArenaInit(&race->arena, sizeof(float) * (size_t)(level->checkpointCount + 1) + 256,
                   "race")) {
        return false;
    }

    // Resolve each gate to an arc length once; progress is compared against these.
    race->checkpointCount = level->checkpointCount;
    if (race->checkpointCount > 0) {
        race->checkpointDistance = ArenaAlloc(&race->arena,
                                              sizeof(float) * (size_t)race->checkpointCount);
        for (int i = 0; i < race->checkpointCount; i++) {
            int hint = -1;
            Vector3 p = level->checkpoints[i].position;
            race->checkpointDistance[i] = SplineClosest(spline, p, &hint).distance;
        }
        race->startDistance = race->checkpointDistance[0];
    }

    for (int i = 0; i < racerCount; i++) {
        Racer *racer = &race->racers[i];
        racer->isPlayer = (i == race->playerIndex);
        racer->model = kCarModels[i % (int)(sizeof kCarModels / sizeof kCarModels[0])];
        racer->tint = kCarTints[i];
        snprintf(racer->name, sizeof racer->name, "%s", kDriverNames[i]);

        // Spread the field: later cars start slightly slower so the grid order
        // is not simply reversed by lap one.
        float skill = 0.92f - 0.055f * (float)i;
        float aggression = 0.75f - 0.05f * (float)i;
        float offset = ((i % 2) ? 0.10f : -0.10f);
        AIDriverInit(&racer->ai, skill, aggression, offset, (unsigned int)(i * 7919 + 13));

        race->standings[i] = i;
    }

    RaceReset(race);
    TraceLog(LOG_INFO, "RACE: %d cars, %d laps, %d gates, lap length %.1f",
             race->racerCount, race->totalLaps, race->checkpointCount, spline->length);
    return true;
}

void RaceFree(Race *race)
{
    ArenaFree(&race->arena);
    memset(race, 0, sizeof(*race));
}

void RaceReset(Race *race)
{
    for (int i = 0; i < race->racerCount; i++) PlaceOnGrid(race, i);
    race->state = RACE_COUNTDOWN;
    race->countdown = RACE_COUNTDOWN_SECONDS;
    race->elapsed = 0.0f;
    race->finishedCount = 0;
}

void RaceRespawn(Race *race, int index)
{
    Racer *racer = &race->racers[index];
    Vector3 here = { racer->car.position.x, racer->car.height, racer->car.position.y };
    SplineQuery q = SplineClosest(race->spline, here, &racer->progress.splineHint);

    racer->car.position = (Vector2){ q.position.x, q.position.z };
    racer->car.height = q.position.y;
    racer->car.pitch = atanf(q.grade);
    racer->car.velocity = (Vector2){ 0.0f, 0.0f };
    racer->car.yaw = atan2f(q.tangent.x, q.tangent.z);
    racer->car.steerAngle = 0.0f;
    racer->car.speed = 0.0f;
    racer->progress.offTrackTime = 0.0f;
    racer->progress.stuckTime = 0.0f;
    racer->ai.recoverTimer = 0.0f;
}

// Advances gate order and lap count from the car's arc-length progress.
static void UpdateCheckpoints(Race *race, Racer *racer)
{
    if (race->checkpointCount <= 0) return;

    RaceProgress *p = &racer->progress;
    float target = race->checkpointDistance[p->nextCheckpoint];

    float now = SplineWrapDelta(race->spline, p->splineDistance, target);
    float before = SplineWrapDelta(race->spline, p->lastSplineDistance, target);

    // Forward zero-crossing of the signed distance to the gate.
    if (before < 0.0f && now >= 0.0f && (now - before) > 0.0f &&
        (now - before) < RACE_GATE_WINDOW) {
        bool wasFinishLine = (p->nextCheckpoint == 0);
        p->nextCheckpoint = (p->nextCheckpoint + 1) % race->checkpointCount;

        if (wasFinishLine) {
            if (p->lap > 0) {
                p->lastLapTime = race->elapsed - p->lapStartTime;
                if (p->bestLapTime <= 0.0f || p->lastLapTime < p->bestLapTime) {
                    p->bestLapTime = p->lastLapTime;
                }
            }
            p->lap++;
            p->lapStartTime = race->elapsed;

            if (p->lap > race->totalLaps && !p->finished) {
                p->finished = true;
                p->finishTime = race->elapsed;
                p->finishPosition = ++race->finishedCount;
            }
        }
    }
}

static void ResolveCarCollisions(Race *race)
{
    for (int i = 0; i < race->racerCount; i++) {
        for (int j = i + 1; j < race->racerCount; j++) {
            Car *a = &race->racers[i].car;
            Car *b = &race->racers[j].car;

            Manifold m = CollideObb2(CarBox(a, &race->tuning), CarBox(b, &race->tuning));
            if (!m.hit) continue;

            // Equal masses: split the separation and swap the closing velocity.
            float push = m.depth * 0.5f;
            a->position.x -= m.normal.x * push;
            a->position.y -= m.normal.y * push;
            b->position.x += m.normal.x * push;
            b->position.y += m.normal.y * push;

            Vector2 rel = { b->velocity.x - a->velocity.x, b->velocity.y - a->velocity.y };
            float closing = rel.x * m.normal.x + rel.y * m.normal.y;
            if (closing >= 0.0f) continue;

            float impulse = -closing * 0.55f;   // partially inelastic
            a->velocity.x -= m.normal.x * impulse;
            a->velocity.y -= m.normal.y * impulse;
            b->velocity.x += m.normal.x * impulse;
            b->velocity.y += m.normal.y * impulse;
        }
    }
}

static int CompareStandings(const Race *race, int a, int b)
{
    const RaceProgress *pa = &race->racers[a].progress;
    const RaceProgress *pb = &race->racers[b].progress;

    if (pa->finished != pb->finished) return pa->finished ? -1 : 1;
    if (pa->finished && pb->finished) return pa->finishPosition - pb->finishPosition;
    if (pa->score > pb->score) return -1;
    if (pa->score < pb->score) return 1;
    return a - b;
}

static void SortStandings(Race *race)
{
    // Insertion sort: the field is tiny and nearly ordered every frame.
    for (int i = 1; i < race->racerCount; i++) {
        int value = race->standings[i];
        int j = i - 1;
        while (j >= 0 && CompareStandings(race, race->standings[j], value) > 0) {
            race->standings[j + 1] = race->standings[j];
            j--;
        }
        race->standings[j + 1] = value;
    }
}

int RacePositionOf(const Race *race, int index)
{
    for (int i = 0; i < race->racerCount; i++) {
        if (race->standings[i] == index) return i + 1;
    }
    return race->racerCount;
}

void RaceUpdate(Race *race, CarInput playerInput, float dt)
{
    if (race->state == RACE_COUNTDOWN) {
        race->countdown -= dt;
        if (race->countdown <= 0.0f) {
            race->countdown = 0.0f;
            race->state = RACE_RUNNING;
        }
    } else if (race->state == RACE_RUNNING) {
        race->elapsed += dt;
    }

    bool locked = (race->state == RACE_COUNTDOWN);

    // Neighbour list for the AI, rebuilt each tick.
    AINeighbour neighbours[RACE_MAX_RACERS];

    for (int i = 0; i < race->racerCount; i++) {
        Racer *racer = &race->racers[i];
        RaceProgress *p = &racer->progress;

        // --- decide inputs --------------------------------------------------
        if (locked) {
            // Held on the line. Note this is *no* input rather than a full
            // brake: at a standstill the brake doubles as reverse, so braking
            // through the countdown drove the whole grid backwards off the line.
            racer->input = (CarInput){ 0 };
            // The AI sees a stationary car it cannot move and starts counting
            // towards an unstick manoeuvre. Clear it so nobody launches off the
            // line already braking and steering backwards.
            racer->ai.recoverTimer = 0.0f;
        } else if (racer->isPlayer && !race->autopilot && !p->finished) {
            racer->input = playerInput;
        } else {
            int n = 0;
            for (int j = 0; j < race->racerCount; j++) {
                if (j == i) continue;
                neighbours[n].position = race->racers[j].car.position;
                neighbours[n].velocity = race->racers[j].car.velocity;
                n++;
            }
            racer->input = AIThink(&racer->ai, &racer->car, &race->tuning, race->spline,
                                   &p->splineHint, neighbours, n, dt);
            if (p->finished) {
                // Coast to a stop rather than freezing mid-track.
                racer->input.throttle = 0.0f;
                racer->input.brake = 0.35f;
            }
        }
        // --- surface --------------------------------------------------------
        Vector3 here = { racer->car.position.x, racer->car.height, racer->car.position.y };
        SplineQuery q = SplineClosest(race->spline, here, &p->splineHint);
        bool onTrack = fabsf(q.lateral) <= q.halfWidth;
        racer->car.onTrack = onTrack;

        // The grade is signed along the centre line, so a car facing back down
        // the track has to see it reversed or a climb would push it along.
        Vector2 forward = CarForward(&racer->car);
        float alignment = forward.x * q.tangent.x + forward.y * q.tangent.z;
        CarSurface surface = {
            .grip = onTrack ? race->tuning.gripTarmac : race->tuning.gripGrass,
            .speedScale = onTrack ? 1.0f : race->tuning.offTrackSpeedScale,
            .grade = (alignment < 0.0f) ? -q.grade : q.grade,
            .height = q.position.y,
        };

        if (locked) CarHold(&racer->car, &surface, dt);
        else CarUpdate(&racer->car, &race->tuning, racer->input, &surface, dt);

        // --- static collision -------------------------------------------------
        Obb2 box = CarBox(&racer->car, &race->tuning);
        Vector2 normal = { 0 };
        float depth = 0.0f;
        Vector2 push = CollisionResolveStatic(race->collision, &box, 3, &normal, &depth);
        if (depth > 0.0f) {
            racer->car.position = box.center;
            CarApplyContact(&racer->car, normal, 0.15f, 0.22f);
        }
        (void)push;

        // --- progress ---------------------------------------------------------
        here = (Vector3){ racer->car.position.x, racer->car.height, racer->car.position.y };
        q = SplineClosest(race->spline, here, &p->splineHint);
        p->lastSplineDistance = p->splineDistance;
        p->splineDistance = q.distance;

        if (race->state == RACE_RUNNING && !p->finished) {
            UpdateCheckpoints(race, racer);
        }
        p->score = (float)p->lap * race->spline->length + LapRelative(race, q.distance);

        // --- rescue -------------------------------------------------------------
        if (race->state == RACE_RUNNING && !p->finished) {
            p->offTrackTime = onTrack ? 0.0f : (p->offTrackTime + dt);
            p->stuckTime = (racer->car.speed < 0.3f) ? (p->stuckTime + dt) : 0.0f;
            if (p->offTrackTime > RACE_OFFTRACK_RESPAWN || p->stuckTime > RACE_STUCK_RESPAWN) {
                RaceRespawn(race, i);
            }
        }
    }

    ResolveCarCollisions(race);
    SortStandings(race);

    if (race->state == RACE_RUNNING && race->finishedCount >= race->racerCount) {
        race->state = RACE_FINISHED;
    }
}
