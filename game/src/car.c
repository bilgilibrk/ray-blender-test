#include "game/car.h"

#include <math.h>
#include <string.h>

#include "raymath.h"

CarTuning CarDefaultTuning(void)
{
    // Tuned for the Kenney kit's scale: one unit is one road tile, the drivable
    // lane is 0.69 wide, and a car is roughly 0.30 x 0.60.
    CarTuning t = {
        .enginePower = 7.2f,
        .brakePower = 13.0f,
        .topSpeed = 6.6f,
        .reverseSpeed = 2.0f,
        .dragLinear = 0.30f,
        .dragQuadratic = 0.030f,

        .wheelbase = 0.45f,
        .maxSteerLowSpeed = 0.62f,
        .maxSteerTopSpeed = 0.24f,
        .steerRate = 7.0f,
        .maxYawRate = 3.4f,

        .gripTarmac = 9.5f,
        .gripGrass = 3.0f,
        .handbrakeGrip = 0.22f,

        // About 1.5x the engine's own acceleration. True gravity at this scale
        // would be nearer 2.4x, which made the 18% climbs on the demo circuit a
        // crawl; this keeps hills clearly felt but still driveable.
        .gravity = 11.0f,

        .halfWidth = 0.148f,
        .halfLength = 0.298f,
        .offTrackSpeedScale = 0.58f,
    };
    return t;
}

Vector2 CarForward(const Car *car)
{
    return (Vector2){ sinf(car->yaw), cosf(car->yaw) };
}

// Right-hand side of the car, consistent with the engine's yaw convention
// (yaw +90 degrees turns +Z towards +X).
Vector2 CarRight(const Car *car)
{
    return (Vector2){ cosf(car->yaw), -sinf(car->yaw) };
}

void CarInit(Car *car, Vector2 position, float yaw)
{
    memset(car, 0, sizeof(*car));
    car->position = position;
    car->yaw = yaw;
    car->onTrack = true;
}

// Presentation only: the car leans into the gradient and rides the surface
// height. The simulation itself stays flat on the XZ plane.
static void SettleToSurface(Car *car, const CarSurface *surface, float dt)
{
    float pitchTarget = atanf(surface->grade);
    car->pitch += (pitchTarget - car->pitch) * (1.0f - expf(-12.0f * dt));
    car->height += (surface->height - car->height) * (1.0f - expf(-18.0f * dt));
}

Obb2 CarBox(const Car *car, const CarTuning *tuning)
{
    return (Obb2){
        .center = car->position,
        .halfExtents = { tuning->halfWidth, tuning->halfLength },
        .yaw = car->yaw,
    };
}

void CarUpdate(Car *car, const CarTuning *tuning, CarInput input,
               const CarSurface *surface, float dt)
{
    if (dt <= 0.0f) return;

    input.throttle = Clamp(input.throttle, 0.0f, 1.0f);
    input.brake = Clamp(input.brake, 0.0f, 1.0f);
    input.steer = Clamp(input.steer, -1.0f, 1.0f);

    Vector2 forward = CarForward(car);
    Vector2 right = CarRight(car);

    float vLong = car->velocity.x * forward.x + car->velocity.y * forward.y;
    float vLat = car->velocity.x * right.x + car->velocity.y * right.y;

    // --- steering ----------------------------------------------------------
    // Lock tightens with speed so the car stays controllable flat out.
    float speedFraction = Clamp(fabsf(vLong) / tuning->topSpeed, 0.0f, 1.0f);
    float maxSteer = Lerp(tuning->maxSteerLowSpeed, tuning->maxSteerTopSpeed, speedFraction);
    float target = input.steer * maxSteer;
    float steerStep = tuning->steerRate * dt;
    if (fabsf(target - car->steerAngle) <= steerStep) car->steerAngle = target;
    else car->steerAngle += (target > car->steerAngle) ? steerStep : -steerStep;

    // --- longitudinal ------------------------------------------------------
    float accel = 0.0f;
    float topSpeed = tuning->topSpeed * surface->speedScale;

    if (input.throttle > 0.0f) {
        // Taper power near the limit instead of clamping, which would feel abrupt.
        float headroom = 1.0f - Clamp(vLong / fmaxf(topSpeed, 0.001f), 0.0f, 1.0f);
        accel += tuning->enginePower * input.throttle * headroom;
    }
    if (input.brake > 0.0f) {
        if (vLong > 0.05f) {
            accel -= tuning->brakePower * input.brake;
        } else {
            // Standing still or already rolling back: brake doubles as reverse.
            float headroom = 1.0f - Clamp(-vLong / tuning->reverseSpeed, 0.0f, 1.0f);
            accel -= tuning->enginePower * 0.55f * input.brake * headroom;
        }
    }
    accel -= tuning->dragLinear * vLong;
    accel -= tuning->dragQuadratic * vLong * fabsf(vLong);

    // Gravity along the road. sin(atan(grade)) resolves the slope into the
    // direction of travel, so a climb costs speed and a descent gives it back.
    float slope = surface->grade;
    car->slopeAccel = -tuning->gravity * (slope / sqrtf(1.0f + slope * slope));
    accel += car->slopeAccel;

    vLong += accel * dt;
    // A steep enough descent should be able to push past the flat-road limit,
    // which is where the speed on a downhill run comes from.
    float downhillAllowance = (car->slopeAccel > 0.0f) ? 1.35f : 1.0f;
    vLong = Clamp(vLong, -tuning->reverseSpeed, topSpeed * downhillAllowance);
    // Kill the last sliver of creep so a stopped car actually stops.
    if (input.throttle <= 0.0f && input.brake <= 0.0f &&
        fabsf(vLong) < 0.02f && fabsf(car->slopeAccel) < 0.05f) {
        vLong = 0.0f;   // only settle to a stop on the flat
    }

    // --- lateral grip -------------------------------------------------------
    float lateralGrip = surface->grip;
    if (input.handbrake) lateralGrip *= tuning->handbrakeGrip;
    vLat *= expf(-lateralGrip * dt);

    // --- heading ------------------------------------------------------------
    float yawRate = 0.0f;
    if (fabsf(vLong) > 0.03f) {
        yawRate = vLong * tanf(car->steerAngle) / tuning->wheelbase;
        yawRate = Clamp(yawRate, -tuning->maxYawRate, tuning->maxYawRate);
    }
    car->yawRate = yawRate;
    car->yaw += yawRate * dt;
    if (car->yaw > PI) car->yaw -= 2.0f * PI;
    if (car->yaw < -PI) car->yaw += 2.0f * PI;

    // Recombine in the new heading so the car rotates about itself rather than
    // carrying the old frame's velocity direction.
    forward = CarForward(car);
    right = CarRight(car);
    car->velocity.x = forward.x * vLong + right.x * vLat;
    car->velocity.y = forward.y * vLong + right.y * vLat;

    car->position.x += car->velocity.x * dt;
    car->position.y += car->velocity.y * dt;

    SettleToSurface(car, surface, dt);

    car->forwardSpeed = vLong;
    car->lateralSpeed = vLat;
    car->speed = sqrtf(car->velocity.x * car->velocity.x + car->velocity.y * car->velocity.y);
    car->slip = Clamp(fabsf(vLat) / (fabsf(vLong) * 0.45f + 0.9f), 0.0f, 1.0f);
}

void CarHold(Car *car, const CarSurface *surface, float dt)
{
    if (dt <= 0.0f) return;

    car->velocity = (Vector2){ 0.0f, 0.0f };
    car->steerAngle = 0.0f;
    car->speed = 0.0f;
    car->forwardSpeed = 0.0f;
    car->lateralSpeed = 0.0f;
    car->yawRate = 0.0f;
    car->slip = 0.0f;
    car->slopeAccel = 0.0f;

    SettleToSurface(car, surface, dt);
}

void CarApplyContact(Car *car, Vector2 normal, float restitution, float scrub)
{
    float len = sqrtf(normal.x * normal.x + normal.y * normal.y);
    if (len < 1e-6f) return;
    normal.x /= len;
    normal.y /= len;

    float into = car->velocity.x * normal.x + car->velocity.y * normal.y;
    if (into >= 0.0f) return;   // already moving away from the surface

    // Remove the approaching component (plus a little bounce) and scrub the
    // sliding component so scraping a wall costs momentum.
    float remove = into * (1.0f + restitution);
    car->velocity.x -= normal.x * remove;
    car->velocity.y -= normal.y * remove;
    car->velocity.x *= (1.0f - scrub);
    car->velocity.y *= (1.0f - scrub);

    car->speed = sqrtf(car->velocity.x * car->velocity.x + car->velocity.y * car->velocity.y);
}
