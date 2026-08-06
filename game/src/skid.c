#include "game/skid.h"

#include <math.h>
#include <string.h>

#include "raymath.h"

#include "game/car.h"

// How long a mark takes to fade out completely.
#define SKID_LIFE 8.0f

// Darkest a fresh mark laid at full strength gets. Short of opaque: rubber
// stains tarmac, it does not paint it.
#define SKID_MAX_ALPHA 150.0f

// Rear axle placement, in world units, to match the drawn car. The tuning's
// halfLength is 0.298 and halfWidth 0.148, so this sits the strips just inside
// the back corners of the body.
#define SKID_AXLE_BACK 0.17f
#define SKID_TRACK_HALF 0.115f
#define SKID_TYRE_HALF_WIDTH 0.035f

// Laid a shade above the road, or the quads z-fight the tarmac they sit on.
#define SKID_LIFT 0.015f

// Distance a wheel must travel before another quad is laid. Short enough that a
// tight corner still reads as a curve, long enough not to burn the ring buffer
// on a car creeping along.
#define SKID_STEP 0.09f

// Below this the tyres are gripping, not sliding, and leave nothing behind.
// Measured against the AI, which peaks at 0.298 through the quickest corners
// and sits far below that everywhere else: this puts rubber down where the
// field is genuinely scrubbing for grip and nowhere else. A player leaning on
// the handbrake goes well past it.
#define SKID_SLIP_FLOOR 0.22f

// Brake input past which a wheel is treated as locked.
#define SKID_BRAKE_FLOOR 0.55f

void SkidInit(SkidTrails *trails)
{
    memset(trails, 0, sizeof(*trails));
}

void SkidClear(SkidTrails *trails)
{
    for (int i = 0; i < SKID_MAX_QUADS; i++) trails->quads[i].life = 0.0f;
    for (int i = 0; i < SKID_EMITTERS; i++) trails->emitters[i].laying = false;
}

int SkidLiveCount(const SkidTrails *trails)
{
    int live = 0;
    for (int i = 0; i < SKID_MAX_QUADS; i++) {
        if (trails->quads[i].life > 0.0f) live++;
    }
    return live;
}

// How hard this car is marking the road right now, 0..1.
static float MarkStrength(const Racer *racer)
{
    // Rubber only stains tarmac; off the track a car is throwing grass around.
    if (!racer->car.onTrack) return 0.0f;
    if (fabsf(racer->car.forwardSpeed) < 0.6f) return 0.0f;

    // Sliding sideways is the main source: slip is already the ratio of lateral
    // to forward motion, which is exactly what scrubs a tyre.
    float slide = (racer->car.slip - SKID_SLIP_FLOOR) / (1.0f - SKID_SLIP_FLOOR);
    slide = Clamp(slide, 0.0f, 1.0f);

    // Locking up under brakes is the other. Only while actually moving forward,
    // or a car reversing off the line would lay marks.
    float lock = 0.0f;
    if (racer->input.brake > SKID_BRAKE_FLOOR && racer->car.forwardSpeed > 1.2f) {
        lock = Clamp((racer->input.brake - SKID_BRAKE_FLOOR) / (1.0f - SKID_BRAKE_FLOOR),
                     0.0f, 1.0f);
    }
    if (racer->input.handbrake) lock = 1.0f;

    return fmaxf(slide, lock);
}

static void LayQuad(SkidTrails *trails, Vector3 from, Vector3 to, float strength)
{
    float dx = to.x - from.x, dz = to.z - from.z;
    float len = sqrtf(dx * dx + dz * dz);
    if (len < 1e-5f) return;

    // Square the strip off across the direction of travel, so a curving skid
    // stays the width of the tyre all the way round.
    Vector3 across = { -dz / len * SKID_TYRE_HALF_WIDTH, 0.0f,
                        dx / len * SKID_TYRE_HALF_WIDTH };

    // Wound so the pair of triangles below face upwards, which is the way round
    // rlgl's culling keeps for a quad lying on the ground.
    SkidQuad *q = &trails->quads[trails->next];
    q->corner[0] = (Vector3){ from.x - across.x, from.y, from.z - across.z };
    q->corner[1] = (Vector3){ from.x + across.x, from.y, from.z + across.z };
    q->corner[2] = (Vector3){ to.x + across.x, to.y, to.z + across.z };
    q->corner[3] = (Vector3){ to.x - across.x, to.y, to.z - across.z };
    q->life = SKID_LIFE;
    q->peak = strength;

    trails->next = (trails->next + 1) % SKID_MAX_QUADS;
}

void SkidUpdate(SkidTrails *trails, const Race *race, float dt)
{
    if (dt <= 0.0f) return;

    for (int i = 0; i < SKID_MAX_QUADS; i++) {
        if (trails->quads[i].life > 0.0f) trails->quads[i].life -= dt;
    }

    for (int i = 0; i < race->racerCount && i < RACE_MAX_RACERS; i++) {
        const Racer *racer = &race->racers[i];
        float strength = MarkStrength(racer);

        Vector2 forward = CarForward(&racer->car);
        Vector2 right = CarRight(&racer->car);

        for (int side = 0; side < 2; side++) {
            SkidEmitter *emitter = &trails->emitters[i * 2 + side];
            float lateral = side ? SKID_TRACK_HALF : -SKID_TRACK_HALF;
            Vector3 wheel = {
                racer->car.position.x - forward.x * SKID_AXLE_BACK + right.x * lateral,
                racer->car.height + SKID_LIFT,
                racer->car.position.y - forward.y * SKID_AXLE_BACK + right.y * lateral,
            };

            if (strength <= 0.0f) {
                emitter->laying = false;
                continue;
            }
            // The first tick of a skid only anchors the strip. Drawing from
            // wherever the wheel happened to be last would streak a mark across
            // everything between here and the previous skid.
            if (!emitter->laying) {
                emitter->laying = true;
                emitter->last = wheel;
                continue;
            }

            float dx = wheel.x - emitter->last.x;
            float dz = wheel.z - emitter->last.z;
            if (dx * dx + dz * dz < SKID_STEP * SKID_STEP) continue;

            LayQuad(trails, emitter->last, wheel, strength);
            emitter->last = wheel;
        }
    }
}

void SkidDraw(const SkidTrails *trails)
{
    for (int i = 0; i < SKID_MAX_QUADS; i++) {
        const SkidQuad *q = &trails->quads[i];
        if (q->life <= 0.0f) continue;

        float fade = Clamp(q->life / SKID_LIFE, 0.0f, 1.0f);
        unsigned char alpha = (unsigned char)(SKID_MAX_ALPHA * q->peak * fade);
        if (alpha == 0) continue;

        Color ink = { 26, 24, 28, alpha };
        DrawTriangle3D(q->corner[0], q->corner[1], q->corner[2], ink);
        DrawTriangle3D(q->corner[0], q->corner[2], q->corner[3], ink);
    }
}
