#include "game/hud.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "raymath.h"

static const Color kPanel = { 16, 22, 30, 170 };
static const Color kInk = { 240, 246, 252, 255 };
static const Color kDim = { 168, 182, 196, 255 };
static const Color kAccent = { 255, 202, 64, 255 };

static void Panel(Rectangle rect)
{
    DrawRectangleRounded(rect, 0.16f, 6, kPanel);
}

static void TextAt(const char *text, int x, int y, int size, Color color)
{
    DrawText(text, x, y, size, color);
}

// Right-aligned to `x`.
static void TextRight(const char *text, int x, int y, int size, Color color)
{
    DrawText(text, x - MeasureText(text, size), y, size, color);
}

static void TextCentre(const char *text, int cx, int y, int size, Color color)
{
    DrawText(text, cx - MeasureText(text, size) / 2, y, size, color);
}

// ---------------------------------------------------------------------------

static void DrawMinimap(const Race *race, Rectangle area)
{
    const Spline *spline = race->spline;
    if (spline->count < 2) return;

    float minX = 1e30f, maxX = -1e30f, minZ = 1e30f, maxZ = -1e30f;
    for (int i = 0; i < spline->count; i++) {
        Vector3 p = spline->samples[i].position;
        if (p.x < minX) minX = p.x;
        if (p.x > maxX) maxX = p.x;
        if (p.z < minZ) minZ = p.z;
        if (p.z > maxZ) maxZ = p.z;
    }

    float spanX = fmaxf(maxX - minX, 0.001f);
    float spanZ = fmaxf(maxZ - minZ, 0.001f);
    float pad = 10.0f;
    float scale = fminf((area.width - pad * 2) / spanX, (area.height - pad * 2) / spanZ);
    // Centre the track inside the panel.
    float offX = area.x + (area.width - spanX * scale) * 0.5f;
    float offY = area.y + (area.height - spanZ * scale) * 0.5f;

    // World +Z is drawn upwards, so the Y axis is flipped.
    #define MAP_X(wx) (offX + ((wx) - minX) * scale)
    #define MAP_Y(wz) (offY + (spanZ - ((wz) - minZ)) * scale)

    Panel(area);
    for (int i = 0; i < spline->count; i++) {
        Vector3 a = spline->samples[i].position;
        Vector3 b = spline->samples[(i + 1) % spline->count].position;
        DrawLineEx((Vector2){ MAP_X(a.x), MAP_Y(a.z) },
                   (Vector2){ MAP_X(b.x), MAP_Y(b.z) },
                   2.5f, (Color){ 96, 108, 122, 255 });
    }

    // Finish line marker.
    if (race->checkpointCount > 0) {
        Vector3 s = SplinePointAt(spline, race->startDistance);
        DrawCircle((int)MAP_X(s.x), (int)MAP_Y(s.z), 3.5f, kAccent);
    }

    for (int i = race->racerCount - 1; i >= 0; i--) {
        const Racer *r = &race->racers[i];
        float x = MAP_X(r->car.position.x);
        float y = MAP_Y(r->car.position.y);
        if (r->isPlayer) {
            DrawCircle((int)x, (int)y, 5.0f, WHITE);
            DrawCircle((int)x, (int)y, 3.2f, (Color){ 220, 60, 60, 255 });
        } else {
            DrawCircle((int)x, (int)y, 3.2f, r->tint);
        }
    }
    #undef MAP_X
    #undef MAP_Y
}

static void DrawSpeedometer(const Racer *player, const CarTuning *tuning, Rectangle area)
{
    Panel(area);
    float speed01 = Clamp(fabsf(player->car.forwardSpeed) / tuning->topSpeed, 0.0f, 1.0f);

    Rectangle bar = { area.x + 14, area.y + area.height - 26, area.width - 28, 10 };
    DrawRectangleRounded(bar, 1.0f, 4, (Color){ 44, 52, 62, 255 });
    Rectangle fill = bar;
    fill.width = bar.width * speed01;
    Color barColor = (speed01 > 0.85f) ? (Color){ 255, 108, 84, 255 } : kAccent;
    if (fill.width > 2.0f) DrawRectangleRounded(fill, 1.0f, 4, barColor);

    char text[32];
    snprintf(text, sizeof text, "%3d", (int)(fabsf(player->car.forwardSpeed) * HUD_UNITS_TO_KMH));
    TextRight(text, (int)(area.x + area.width - 52), (int)(area.y + 8), 40, kInk);
    TextRight("km/h", (int)(area.x + area.width - 14), (int)(area.y + 26), 16, kDim);

    if (!player->car.onTrack) {
        TextAt("OFF TRACK", (int)(area.x + 14), (int)(area.y + 8), 16,
               (Color){ 255, 150, 90, 255 });
    } else if (player->car.slip > 0.45f) {
        TextAt("DRIFT", (int)(area.x + 14), (int)(area.y + 8), 16, kAccent);
    }
}

// A gradient readout, because from a top-down camera the road ahead gives the
// player very little to go on. car.pitch is the slope the car is sitting on, so
// its tangent is the gradient as a rise over run.
static void DrawGradient(const Racer *player, Rectangle area)
{
    float grade = tanf(player->car.pitch);
    int percent = (int)roundf(grade * 100.0f);

    // Below this the road is flat enough that a readout would only flicker.
    if (percent > -2 && percent < 2) return;

    Panel(area);

    bool climbing = (percent > 0);
    Color tone = climbing ? (Color){ 255, 150, 90, 255 } : (Color){ 130, 220, 255, 255 };

    // A wedge rising or falling in the direction the road goes.
    float cx = area.x + 22.0f, cy = area.y + area.height * 0.5f;
    Vector2 a = { cx - 11.0f, climbing ? cy + 8.0f : cy - 8.0f };
    Vector2 b = { cx + 11.0f, climbing ? cy + 8.0f : cy - 8.0f };
    Vector2 tip = { cx, climbing ? cy - 9.0f : cy + 9.0f };
    // Wound counter-clockwise in screen space either way, or the fill drops out.
    if (climbing) DrawTriangle(tip, a, b, tone);
    else DrawTriangle(a, tip, b, tone);

    char buffer[16];
    snprintf(buffer, sizeof buffer, "%d%%", (percent < 0) ? -percent : percent);
    TextAt(buffer, (int)(area.x + 42), (int)(area.y + 9), 22, tone);
}

static void DrawResults(const Race *race)
{
    int w = GetScreenWidth(), h = GetScreenHeight();
    DrawRectangle(0, 0, w, h, (Color){ 8, 10, 14, 190 });

    int panelW = 460, panelH = 90 + race->racerCount * 30;
    Rectangle panel = { (w - panelW) / 2.0f, (h - panelH) / 2.0f, (float)panelW, (float)panelH };
    DrawRectangleRounded(panel, 0.06f, 8, (Color){ 20, 26, 34, 240 });

    TextCentre("RACE COMPLETE", w / 2, (int)panel.y + 18, 28, kAccent);

    int y = (int)panel.y + 62;
    char buffer[32];
    for (int i = 0; i < race->racerCount; i++) {
        const Racer *r = &race->racers[race->standings[i]];
        Color color = r->isPlayer ? kAccent : kInk;

        snprintf(buffer, sizeof buffer, "%d.", i + 1);
        TextAt(buffer, (int)panel.x + 26, y, 20, color);
        TextAt(r->name, (int)panel.x + 62, y, 20, color);
        TextRight(RaceFormatTime(r->progress.finishTime, buffer, sizeof buffer),
                  (int)(panel.x + panelW - 110), y, 20, color);
        TextRight(RaceFormatTime(r->progress.bestLapTime, buffer, sizeof buffer),
                  (int)(panel.x + panelW - 20), y, 20, kDim);
        y += 30;
    }
    TextCentre("ENTER to race again", w / 2, (int)(panel.y + panelH - 26), 16, kDim);
}

// ---------------------------------------------------------------------------

void HudDraw(const Race *race, bool paused)
{
    int w = GetScreenWidth(), h = GetScreenHeight();
    const Racer *player = &race->racers[race->playerIndex];
    char buffer[48];

    // --- lap and position -------------------------------------------------
    Rectangle top = { 16, 16, 208, 74 };
    Panel(top);
    int lap = player->progress.lap;
    if (lap < 1) lap = 1;
    if (lap > race->totalLaps) lap = race->totalLaps;
    snprintf(buffer, sizeof buffer, "LAP %d/%d", lap, race->totalLaps);
    TextAt(buffer, 30, 26, 24, kInk);

    snprintf(buffer, sizeof buffer, "P%d of %d",
             RacePositionOf(race, race->playerIndex), race->racerCount);
    TextAt(buffer, 30, 56, 20, kAccent);

    // --- timers -------------------------------------------------------------
    Rectangle times = { (float)w - 232, 16, 216, 90 };
    Panel(times);
    TextAt("TIME", w - 218, 24, 14, kDim);
    TextRight(RaceFormatTime(race->elapsed, buffer, sizeof buffer), w - 30, 20, 22, kInk);
    TextAt("LAST", w - 218, 50, 14, kDim);
    TextRight(RaceFormatTime(player->progress.lastLapTime, buffer, sizeof buffer),
              w - 30, 46, 18, kInk);
    TextAt("BEST", w - 218, 74, 14, kDim);
    TextRight(RaceFormatTime(player->progress.bestLapTime, buffer, sizeof buffer),
              w - 30, 70, 18, kAccent);

    DrawMinimap(race, (Rectangle){ 16, (float)h - 186, 170, 170 });
    DrawSpeedometer(player, &race->tuning, (Rectangle){ (float)w - 202, (float)h - 96, 186, 80 });
    DrawGradient(player, (Rectangle){ (float)w - 202, (float)h - 142, 110, 38 });

    // --- countdown ------------------------------------------------------------
    if (race->state == RACE_COUNTDOWN) {
        int count = (int)ceilf(race->countdown - 0.6f);
        if (count > 0) {
            snprintf(buffer, sizeof buffer, "%d", count);
            TextCentre(buffer, w / 2, h / 2 - 60, 96, kAccent);
        } else {
            TextCentre("GO!", w / 2, h / 2 - 60, 96, (Color){ 120, 240, 140, 255 });
        }
    } else if (player->progress.finished && race->state != RACE_FINISHED) {
        TextCentre("FINISHED — waiting for the field", w / 2, h / 2 - 40, 24, kAccent);
    }

    if (race->state == RACE_FINISHED) DrawResults(race);

    if (paused) {
        DrawRectangle(0, 0, w, h, (Color){ 8, 10, 14, 160 });
        TextCentre("PAUSED", w / 2, h / 2 - 40, 48, kInk);
        TextCentre("P resume    R restart    ESC quit", w / 2, h / 2 + 16, 18, kDim);
    }
}

void HudDrawDebug(const Race *race, const HudStats *stats)
{
    const Racer *player = &race->racers[race->playerIndex];
    char buffer[128];
    int y = 116;

    Rectangle panel = { 16, 104, 300, 196 };
    Panel(panel);

    snprintf(buffer, sizeof buffer, "%d fps   %d/%d chunks   %dk tris",
             stats->fps, stats->drawnChunks, stats->totalChunks, stats->triangles / 1000);
    TextAt(buffer, 28, y, 16, kInk); y += 22;

    snprintf(buffer, sizeof buffer, "pos  %.2f, %.2f", player->car.position.x,
             player->car.position.y);
    TextAt(buffer, 28, y, 16, kDim); y += 20;

    snprintf(buffer, sizeof buffer, "vel  %.2f fwd  %.2f lat  slip %.2f",
             player->car.forwardSpeed, player->car.lateralSpeed, player->car.slip);
    TextAt(buffer, 28, y, 16, kDim); y += 20;

    snprintf(buffer, sizeof buffer, "yaw  %.1f deg   rate %.2f",
             player->car.yaw * RAD2DEG, player->car.yawRate);
    TextAt(buffer, 28, y, 16, kDim); y += 20;

    snprintf(buffer, sizeof buffer, "track %s   arc %.2f / %.2f",
             player->car.onTrack ? "on" : "OFF", player->progress.splineDistance,
             race->spline->length);
    TextAt(buffer, 28, y, 16, kDim); y += 20;

    snprintf(buffer, sizeof buffer, "gate %d/%d   lap %d   score %.1f",
             player->progress.nextCheckpoint, race->checkpointCount,
             player->progress.lap, player->progress.score);
    TextAt(buffer, 28, y, 16, kDim); y += 20;

    snprintf(buffer, sizeof buffer, "%d lights   %d skids   %s   audio %s",
             stats->lightCount, stats->skidMarks, stats->night ? "night" : "day",
             stats->audioActive ? "on" : "off");
    TextAt(buffer, 28, y, 16, kDim); y += 20;

    snprintf(buffer, sizeof buffer, "F1 debug  F2 shot  C camera  N night");
    TextAt(buffer, 28, y, 16, kDim);
}
