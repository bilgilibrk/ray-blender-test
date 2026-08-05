// Arcade vehicle physics for a top-down racer.
//
// Velocity is split into the car's forward and lateral components each tick:
// the engine and brakes act on the forward part, tyre grip bleeds off the
// lateral part, and heading comes from a bicycle steering model. Letting the
// lateral component survive when grip is low is what produces drift.
#ifndef GAME_CAR_H
#define GAME_CAR_H

#include <stdbool.h>

#include "raylib.h"

#include "engine/collide.h"

typedef struct CarTuning {
    float enginePower;          // forward acceleration at full throttle, u/s^2
    float brakePower;           // u/s^2
    float topSpeed;             // u/s
    float reverseSpeed;         // u/s
    float dragLinear;
    float dragQuadratic;

    float wheelbase;
    float maxSteerLowSpeed;     // radians of steering lock when nearly stopped
    float maxSteerTopSpeed;     // radians of lock at top speed
    float steerRate;            // how fast the wheels reach the commanded angle
    float maxYawRate;           // clamp that stops low-speed pirouettes

    float gripTarmac;           // lateral velocity decay rate, 1/s
    float gripGrass;
    float handbrakeGrip;        // multiplier applied to grip when handbraking

    float halfWidth;            // collision box, world units
    float halfLength;
    float offTrackSpeedScale;   // top-speed multiplier off the tarmac
} CarTuning;

CarTuning CarDefaultTuning(void);

typedef struct CarInput {
    float throttle;             // 0..1
    float brake;                // 0..1
    float steer;                // -1..1
    bool handbrake;
} CarInput;

typedef struct Car {
    Vector2 position;           // world (x, z)
    Vector2 velocity;           // world (x, z)
    float yaw;                  // radians; 0 faces +Z
    float steerAngle;

    // Derived each tick, useful for audio, effects and the HUD.
    float speed;
    float forwardSpeed;
    float lateralSpeed;
    float slip;                 // 0..1, how much the tyres are sliding
    float yawRate;
    bool onTrack;
} Car;

void CarInit(Car *car, Vector2 position, float yaw);

// Advances one fixed step. `grip` and `speedScale` come from the surface the
// car is standing on, so callers blend tarmac and grass however they like.
void CarUpdate(Car *car, const CarTuning *tuning, CarInput input,
               float grip, float speedScale, float dt);

// Collision box in world space.
Obb2 CarBox(const Car *car, const CarTuning *tuning);

// Cancels the velocity component heading into a surface and scrubs a little
// speed, so wall contact costs time without stopping the car dead.
void CarApplyContact(Car *car, Vector2 normal, float restitution, float scrub);

Vector2 CarForward(const Car *car);
Vector2 CarRight(const Car *car);

#endif // GAME_CAR_H
