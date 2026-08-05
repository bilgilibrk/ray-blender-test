// Name-keyed cache for models, textures and sounds.
//
// Models are looked up by bare kit name ("roadStraight") and resolved against
// the configured model directory. Every asset is loaded at most once and freed
// together by AssetsShutdown.
#ifndef ENGINE_ASSETS_H
#define ENGINE_ASSETS_H

#include <stdbool.h>

#include "raylib.h"

// `root` is the assets directory, e.g. "assets". Safe to call once at startup.
void AssetsInit(const char *root);
void AssetsShutdown(void);

// Returns a cached model, loading "<root>/models/<name>.glb" on first use.
// On failure returns a unit-cube placeholder so the game keeps running, and
// logs once per missing name.
Model *AssetsGetModel(const char *name);

// Local-space bounding box of a model, cached alongside it.
BoundingBox AssetsGetModelBounds(const char *name);

Texture2D *AssetsGetTexture(const char *name);   // "<root>/textures/<name>"
Sound *AssetsGetSound(const char *name);         // "<root>/audio/<name>"

int AssetsModelCount(void);

#endif // ENGINE_ASSETS_H
