#include "engine/level.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "raymath.h"

#include "engine/json.h"

#define LEVEL_FORMAT_ID "kenney-topdown-racer"

// Aim for this many auto-generated gates when a level ships no explicit ones.
#define LEVEL_AUTO_CHECKPOINTS 12

static Vector3 ReadVec3(const JsonValue *obj, const char *key, Vector3 fallback)
{
    float v[3] = { fallback.x, fallback.y, fallback.z };
    JsonFloatsField(obj, key, v, 3);
    return (Vector3){ v[0], v[1], v[2] };
}

static Color ReadColor(const JsonValue *obj, const char *key, Color fallback)
{
    float v[4] = { fallback.r, fallback.g, fallback.b, fallback.a };
    const JsonValue *arr = JsonGet(obj, key);
    // Alpha is optional, so accept both 3- and 4-element colours.
    if (!JsonFloats(arr, v, 4)) JsonFloats(arr, v, 3);
    Color c;
    c.r = (unsigned char)Clamp(v[0], 0.0f, 255.0f);
    c.g = (unsigned char)Clamp(v[1], 0.0f, 255.0f);
    c.b = (unsigned char)Clamp(v[2], 0.0f, 255.0f);
    c.a = (unsigned char)Clamp(v[3], 0.0f, 255.0f);
    return c;
}

// Worst-case bytes the final level arena needs, including alignment slack.
static size_t EstimateLevelBytes(const JsonValue *root, int autoCheckpoints)
{
    const JsonValue *props = JsonGet(root, "props");
    const JsonValue *colliders = JsonGet(root, "colliders");
    const JsonValue *spawns = JsonGet(root, "spawns");
    const JsonValue *waypoints = JsonGet(root, "waypoints");
    const JsonValue *checkpoints = JsonGet(root, "checkpoints");

    size_t bytes = 0;
    bytes += sizeof(LevelProp) * (size_t)JsonCount(props);
    bytes += sizeof(LevelCollider) * (size_t)JsonCount(colliders);
    bytes += sizeof(LevelSpawn) * (size_t)JsonCount(spawns);
    bytes += sizeof(LevelWaypoint) * (size_t)JsonCount(waypoints);
    bytes += sizeof(LevelCheckpoint) * (size_t)(JsonCount(checkpoints) + autoCheckpoints);

    for (int i = 0; i < JsonCount(props); i++) {
        bytes += strlen(JsonStringField(JsonAt(props, i), "model", "")) + 1;
    }
    // 16-byte alignment padding per array, plus one per interned string.
    bytes += 16 * (size_t)(8 + JsonCount(props));
    return bytes + 1024;
}

bool LevelLoad(Level *level, const char *path)
{
    memset(level, 0, sizeof(*level));

    int fileSize = 0;
    unsigned char *text = LoadFileData(path, &fileSize);
    if (!text || fileSize <= 0) {
        TraceLog(LOG_ERROR, "LEVEL: cannot read '%s'", path);
        return false;
    }

    // The JSON tree is scratch: it is parsed, copied out, and thrown away, so it
    // lives in its own arena rather than bloating the level for its whole life.
    Arena scratch = { 0 };
    size_t scratchBytes = (size_t)fileSize * 12 + (1u << 20);
    if (!ArenaInit(&scratch, scratchBytes, "level-scratch")) {
        TraceLog(LOG_ERROR, "LEVEL: out of memory parsing '%s'", path);
        UnloadFileData(text);
        return false;
    }

    char err[256];
    JsonValue *root = JsonParse(&scratch, (const char *)text, (size_t)fileSize, err, sizeof err);
    UnloadFileData(text);
    if (!root || root->type != JSON_OBJECT) {
        TraceLog(LOG_ERROR, "LEVEL: '%s': %s", path, root ? "root is not an object" : err);
        ArenaFree(&scratch);
        return false;
    }

    const char *format = JsonStringField(root, "format", NULL);
    if (format && strcmp(format, LEVEL_FORMAT_ID) != 0) {
        TraceLog(LOG_WARNING, "LEVEL: '%s' declares format '%s', expected '%s'",
                 path, format, LEVEL_FORMAT_ID);
    }

    const JsonValue *settings  = JsonGet(root, "settings");
    const JsonValue *jProps    = JsonGet(root, "props");
    const JsonValue *jCollide  = JsonGet(root, "colliders");
    const JsonValue *jSpawns   = JsonGet(root, "spawns");
    const JsonValue *jWaypts   = JsonGet(root, "waypoints");
    const JsonValue *jChecks   = JsonGet(root, "checkpoints");

    int waypointCount = JsonCount(jWaypts);
    int explicitChecks = JsonCount(jChecks);
    int autoChecks = (explicitChecks == 0 && waypointCount >= 3) ? LEVEL_AUTO_CHECKPOINTS : 0;

    if (!ArenaInit(&level->arena, EstimateLevelBytes(root, autoChecks), "level")) {
        TraceLog(LOG_ERROR, "LEVEL: out of memory building '%s'", path);
        ArenaFree(&scratch);
        return false;
    }

    snprintf(level->name, sizeof level->name, "%s", JsonStringField(root, "name", "untitled"));
    level->laps = (int)JsonNumberField(settings, "laps", 3);
    level->defaultTrackWidth = (float)JsonNumberField(settings, "track_width", 0.72);
    level->skyColor = ReadColor(settings, "sky_color", (Color){ 124, 176, 214, 255 });
    level->groundColor = ReadColor(settings, "ground_color", (Color){ 104, 152, 84, 255 });
    level->sunDirection = Vector3Normalize(
        ReadVec3(settings, "sun_direction", (Vector3){ -0.45f, -1.0f, -0.35f }));
    if (level->laps < 1) level->laps = 1;

    // --- props ------------------------------------------------------------
    level->propCount = JsonCount(jProps);
    if (level->propCount > 0) {
        level->props = ArenaAlloc(&level->arena, sizeof(LevelProp) * (size_t)level->propCount);
    }
    int written = 0;
    for (int i = 0; i < level->propCount; i++) {
        const JsonValue *o = JsonAt(jProps, i);
        const char *model = JsonStringField(o, "model", NULL);
        if (!model || !model[0]) {
            TraceLog(LOG_WARNING, "LEVEL: prop %d has no model, skipped", i);
            continue;
        }
        LevelProp *p = &level->props[written++];
        p->model = ArenaStrDup(&level->arena, model, strlen(model));
        p->position = ReadVec3(o, "pos", (Vector3){ 0 });
        p->rotationDeg = ReadVec3(o, "rot", (Vector3){ 0 });
        p->scale = ReadVec3(o, "scale", (Vector3){ 1, 1, 1 });
        p->tint = ReadColor(o, "tint", WHITE);
    }
    level->propCount = written;

    // --- colliders ---------------------------------------------------------
    level->colliderCount = JsonCount(jCollide);
    if (level->colliderCount > 0) {
        level->colliders = ArenaAlloc(&level->arena,
                                      sizeof(LevelCollider) * (size_t)level->colliderCount);
    }
    for (int i = 0; i < level->colliderCount; i++) {
        const JsonValue *o = JsonAt(jCollide, i);
        LevelCollider *c = &level->colliders[i];
        c->center = ReadVec3(o, "pos", (Vector3){ 0 });
        float half[2] = { 0.5f, 0.5f };
        JsonFloatsField(o, "half", half, 2);
        c->halfExtents = (Vector2){ fabsf(half[0]), fabsf(half[1]) };
        c->height = (float)JsonNumberField(o, "height", 0.5);
        c->yawDeg = (float)JsonNumberField(o, "yaw", 0.0);
    }

    // --- spawns ------------------------------------------------------------
    level->spawnCount = JsonCount(jSpawns);
    if (level->spawnCount > 0) {
        level->spawns = ArenaAlloc(&level->arena, sizeof(LevelSpawn) * (size_t)level->spawnCount);
    }
    for (int i = 0; i < level->spawnCount; i++) {
        const JsonValue *o = JsonAt(jSpawns, i);
        level->spawns[i].position = ReadVec3(o, "pos", (Vector3){ 0 });
        level->spawns[i].yawDeg = (float)JsonNumberField(o, "yaw", 0.0);
    }

    // --- waypoints ----------------------------------------------------------
    level->waypointCount = waypointCount;
    if (waypointCount > 0) {
        level->waypoints = ArenaAlloc(&level->arena,
                                      sizeof(LevelWaypoint) * (size_t)waypointCount);
    }
    for (int i = 0; i < waypointCount; i++) {
        const JsonValue *o = JsonAt(jWaypts, i);
        level->waypoints[i].position = ReadVec3(o, "pos", (Vector3){ 0 });
        level->waypoints[i].width = (float)JsonNumberField(o, "width", level->defaultTrackWidth);
    }

    // --- checkpoints ---------------------------------------------------------
    if (explicitChecks > 0) {
        level->checkpointCount = explicitChecks;
        level->checkpoints = ArenaAlloc(&level->arena,
                                        sizeof(LevelCheckpoint) * (size_t)explicitChecks);
        for (int i = 0; i < explicitChecks; i++) {
            const JsonValue *o = JsonAt(jChecks, i);
            level->checkpoints[i].position = ReadVec3(o, "pos", (Vector3){ 0 });
            level->checkpoints[i].yawDeg = (float)JsonNumberField(o, "yaw", 0.0);
            level->checkpoints[i].width =
                (float)JsonNumberField(o, "width", level->defaultTrackWidth * 2.0f);
        }
    } else if (autoChecks > 0) {
        // Spread gates evenly along the centreline and face each one down-track.
        level->checkpoints = ArenaAlloc(&level->arena,
                                        sizeof(LevelCheckpoint) * (size_t)autoChecks);
        level->checkpointCount = autoChecks;
        for (int i = 0; i < autoChecks; i++) {
            int wi = (int)((long long)i * waypointCount / autoChecks);
            int next = (wi + 1) % waypointCount;
            Vector3 a = level->waypoints[wi].position;
            Vector3 b = level->waypoints[next].position;
            level->checkpoints[i].position = a;
            level->checkpoints[i].yawDeg = atan2f(b.x - a.x, b.z - a.z) * RAD2DEG;
            level->checkpoints[i].width = level->waypoints[wi].width * 2.0f;
        }
        TraceLog(LOG_INFO, "LEVEL: generated %d checkpoints from centreline", autoChecks);
    }

    TraceLog(LOG_INFO,
             "LEVEL: '%s' loaded — %d props, %d colliders, %d spawns, %d waypoints, %d checkpoints "
             "(%.1f/%.1f KB arena)",
             level->name, level->propCount, level->colliderCount, level->spawnCount,
             level->waypointCount, level->checkpointCount,
             level->arena.used / 1024.0, level->arena.capacity / 1024.0);

    ArenaFree(&scratch);
    return true;
}

void LevelUnload(Level *level)
{
    ArenaFree(&level->arena);
    memset(level, 0, sizeof(*level));
}
