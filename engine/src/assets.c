#include "engine/assets.h"

#include <stdio.h>
#include <string.h>

#define ASSETS_MAX_MODELS   256
#define ASSETS_MAX_TEXTURES 64
#define ASSETS_MAX_SOUNDS   64
#define ASSETS_NAME_MAX     64

typedef struct ModelSlot {
    char name[ASSETS_NAME_MAX];
    Model model;
    BoundingBox bounds;
    bool used;
    bool placeholder;
} ModelSlot;

typedef struct TextureSlot {
    char name[ASSETS_NAME_MAX];
    Texture2D texture;
    bool used;
} TextureSlot;

typedef struct SoundSlot {
    char name[ASSETS_NAME_MAX];
    Sound sound;
    bool used;
} SoundSlot;

static struct {
    char root[512];
    ModelSlot models[ASSETS_MAX_MODELS];
    TextureSlot textures[ASSETS_MAX_TEXTURES];
    SoundSlot sounds[ASSETS_MAX_SOUNDS];
    int modelCount;
    bool ready;
} g_assets;

static unsigned int HashName(const char *s)
{
    unsigned int h = 2166136261u;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return h;
}

void AssetsInit(const char *root)
{
    memset(&g_assets, 0, sizeof(g_assets));
    snprintf(g_assets.root, sizeof g_assets.root, "%s", root ? root : "assets");
    g_assets.ready = true;
}

void AssetsShutdown(void)
{
    for (int i = 0; i < ASSETS_MAX_MODELS; i++) {
        if (g_assets.models[i].used) UnloadModel(g_assets.models[i].model);
    }
    for (int i = 0; i < ASSETS_MAX_TEXTURES; i++) {
        if (g_assets.textures[i].used) UnloadTexture(g_assets.textures[i].texture);
    }
    for (int i = 0; i < ASSETS_MAX_SOUNDS; i++) {
        if (g_assets.sounds[i].used) UnloadSound(g_assets.sounds[i].sound);
    }
    memset(&g_assets, 0, sizeof(g_assets));
}

// Linear-probing lookup. Returns the slot for `name`, inserting when `insert`
// is set, or NULL when the table is full.
static ModelSlot *FindModelSlot(const char *name, bool insert)
{
    unsigned int start = HashName(name) % ASSETS_MAX_MODELS;
    for (int probe = 0; probe < ASSETS_MAX_MODELS; probe++) {
        ModelSlot *slot = &g_assets.models[(start + probe) % ASSETS_MAX_MODELS];
        if (slot->used) {
            if (strncmp(slot->name, name, ASSETS_NAME_MAX - 1) == 0) return slot;
            continue;
        }
        if (!insert) return NULL;
        snprintf(slot->name, ASSETS_NAME_MAX, "%s", name);
        slot->used = true;
        g_assets.modelCount++;
        return slot;
    }
    return NULL;
}

Model *AssetsGetModel(const char *name)
{
    static Model empty;
    if (!g_assets.ready || !name || !name[0]) return &empty;

    ModelSlot *slot = FindModelSlot(name, true);
    if (!slot) {
        TraceLog(LOG_ERROR, "ASSETS: model table full, cannot load '%s'", name);
        return &empty;
    }
    if (slot->model.meshCount > 0 || slot->placeholder) return &slot->model;

    const char *path = TextFormat("%s/models/%s.glb", g_assets.root, name);
    if (FileExists(path)) {
        slot->model = LoadModel(path);
    }
    if (slot->model.meshCount == 0) {
        // Keep running with an obvious stand-in rather than crashing on a typo.
        TraceLog(LOG_WARNING, "ASSETS: missing model '%s', using placeholder", name);
        slot->model = LoadModelFromMesh(GenMeshCube(0.5f, 0.5f, 0.5f));
        slot->model.materials[0].maps[MATERIAL_MAP_DIFFUSE].color = MAGENTA;
        slot->placeholder = true;
    }
    slot->bounds = GetModelBoundingBox(slot->model);
    return &slot->model;
}

BoundingBox AssetsGetModelBounds(const char *name)
{
    AssetsGetModel(name);   // ensure loaded
    ModelSlot *slot = FindModelSlot(name, false);
    if (!slot) return (BoundingBox){ { 0, 0, 0 }, { 0, 0, 0 } };
    return slot->bounds;
}

Texture2D *AssetsGetTexture(const char *name)
{
    static Texture2D empty;
    if (!g_assets.ready || !name) return &empty;

    unsigned int start = HashName(name) % ASSETS_MAX_TEXTURES;
    for (int probe = 0; probe < ASSETS_MAX_TEXTURES; probe++) {
        TextureSlot *slot = &g_assets.textures[(start + probe) % ASSETS_MAX_TEXTURES];
        if (slot->used) {
            if (strncmp(slot->name, name, ASSETS_NAME_MAX - 1) == 0) return &slot->texture;
            continue;
        }
        snprintf(slot->name, ASSETS_NAME_MAX, "%s", name);
        slot->used = true;
        slot->texture = LoadTexture(TextFormat("%s/textures/%s", g_assets.root, name));
        return &slot->texture;
    }
    TraceLog(LOG_ERROR, "ASSETS: texture table full, cannot load '%s'", name);
    return &empty;
}

Sound *AssetsGetSound(const char *name)
{
    static Sound empty;
    if (!g_assets.ready || !name) return &empty;

    unsigned int start = HashName(name) % ASSETS_MAX_SOUNDS;
    for (int probe = 0; probe < ASSETS_MAX_SOUNDS; probe++) {
        SoundSlot *slot = &g_assets.sounds[(start + probe) % ASSETS_MAX_SOUNDS];
        if (slot->used) {
            if (strncmp(slot->name, name, ASSETS_NAME_MAX - 1) == 0) return &slot->sound;
            continue;
        }
        snprintf(slot->name, ASSETS_NAME_MAX, "%s", name);
        slot->used = true;
        slot->sound = LoadSound(TextFormat("%s/audio/%s", g_assets.root, name));
        return &slot->sound;
    }
    TraceLog(LOG_ERROR, "ASSETS: sound table full, cannot load '%s'", name);
    return &empty;
}

int AssetsModelCount(void) { return g_assets.modelCount; }
