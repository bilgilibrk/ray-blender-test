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
    float gripSand;
    float handbrakeGrip;        // multiplier applied to grip when handbraking

    // Pull of a gradient along the road, in world units per second squared.
    // Exaggerated relative to true gravity at this scale so that hills are
    // something you feel rather than something you measure.
    float gravity;

    float halfWidth;            // collision box, world units
    float halfLength;
    float offTrackSpeedScale;   // top-speed multiplier off the tarmac
    float sandSpeedScale;       // top-speed multiplier in a gravel trap
    float sandDrag;             // extra linear drag there, 1/s
} CarTuning;

CarTuning CarDefaultTuning(void);

typedef struct CarInput {
    float throttle;             // 0..1
    float brake;                // 0..1
    float steer;                // -1..1
    bool handbrake;
} CarInput;

// What the car is standing on this tick. Driving stays a 2D problem on the XZ
// plane; height and grade ride along so gradients affect speed and the car can
// be drawn sitting on the road.
typedef struct CarSurface {
    float grip;                 // lateral velocity decay rate, 1/s
    float speedScale;           // multiplier on top speed
    float drag;                 // extra linear drag from the surface itself, 1/s
    float grade;                // rise over run along the direction of travel
    float height;               // surface height under the car
} CarSurface;

typedef struct Car {
    Vector2 position;           // world (x, z)
    Vector2 velocity;           // world (x, z)
    float yaw;                  // radians; 0 faces +Z
    float steerAngle;

    float height;               // follows the track surface
    float pitch;                // radians, nose-up positive; visual only

    // Derived each tick, useful for audio, effects and the HUD.
    float speed;
    float forwardSpeed;
    float lateralSpeed;
    float slip;                 // 0..1, how much the tyres are sliding
    float yawRate;
    float slopeAccel;           // gravity's contribution this tick, for the HUD
    bool onTrack;
    bool inSand;                // bogged down in a run-off trap
} Car;

void CarInit(Car *car, Vector2 position, float yaw);

// Advances one fixed step against the surface the car is standing on.
void CarUpdate(Car *car, const CarTuning *tuning, CarInput input,
               const CarSurface *surface, float dt);

// Plants the car where it stands, gradient and all, and only settles how it
// sits on the surface. Holding a car with a full brake input does not work:
// at a standstill the brake doubles as reverse and the car drives off backwards.
void CarHold(Car *car, const CarSurface *surface, float dt);

// Collision box in world space.
Obb2 CarBox(const Car *car, const CarTuning *tuning);

// Cancels the velocity component heading into a surface and scrubs a little
// speed, so wall contact costs time without stopping the car dead.
void CarApplyContact(Car *car, Vector2 normal, float restitution, float scrub);

Vector2 CarForward(const Car *car);
Vector2 CarRight(const Car *car);

#endif // GAME_CAR_H
