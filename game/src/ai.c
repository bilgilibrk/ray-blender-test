#include "game/ai.h"

#include <math.h>
#include <string.h>

#include "raymath.h"

// How far ahead the corner-severity probes sit, in world units.
#define AI_PROBE_NEAR 0.9f
#define AI_PROBE_FAR  3.4f

void AIDriverInit(AIDriver *ai, float skill, float aggression, float preferredOffset,
                  unsigned int seed)
{
    memset(ai, 0, sizeof(*ai));
    ai->skill = Clamp(skill, 0.0f, 1.0f);
    ai->aggression = Clamp(aggression, 0.0f, 1.0f);
    ai->preferredOffset = preferredOffset;
    ai->currentOffset = preferredOffset;
    ai->wobblePhase = (float)(seed % 1000) * 0.0062831853f;
}

// The car's right-hand direction for a given heading. Matches CarRight(): for
// yaw t, forward is (sin t, cos t) and right is (cos t, -sin t).
static Vector2 RightOf(Vector2 dir)
{
    return (Vector2){ dir.y, -dir.x };
}

static float Dot2(Vector2 a, Vector2 b) { return a.x * b.x + a.y * b.y; }

// Angle from `forward` to `target`, positive when the target is to the right.
// Expressed in the car's own frame so it feeds straight into the steer input,
// whose positive direction is also "right".
static float AngleToRight(Vector2 forward, Vector2 target)
{
    return atan2f(Dot2(target, RightOf(forward)), Dot2(target, forward));
}

CarInput AIThink(AIDriver *ai, const Car *car, const CarTuning *tuning,
                 const Spline *spline, int *hint,
                 const AINeighbour *neighbours, int neighbourCount, float dt)
{
    CarInput input = { 0 };
    if (spline->count < 2) return input;

    Vector3 here = { car->position.x, 0.0f, car->position.y };
    SplineQuery q = SplineClosest(spline, here, hint);

    // --- how hard is the next corner ---------------------------------------
    SplineSample near = SplineSampleAt(spline, q.distance + AI_PROBE_NEAR);
    SplineSample far = SplineSampleAt(spline, q.distance + AI_PROBE_FAR);
    Vector2 nearDir = { near.tangent.x, near.tangent.z };
    Vector2 farDir = { far.tangent.x, far.tangent.z };
    float bendAngle = AngleToRight(nearDir, farDir);   // positive: track turns right
    float corner01 = Clamp(fabsf(bendAngle) / (PI * 0.5f), 0.0f, 1.0f);

    // --- choose a racing line ------------------------------------------------
    // Move towards the inside of the corner to clip the apex, scaled by skill.
    // Lateral offsets are measured to the left, so a right-hand corner wants a
    // negative offset.
    float insideSign = (bendAngle > 0.0f) ? -1.0f : 1.0f;
    float apexPull = corner01 * q.halfWidth * 0.55f * (0.4f + 0.6f * ai->skill);
    float wobble = sinf((float)GetTime() * 0.7f + ai->wobblePhase) * 0.02f * (1.0f - ai->skill);
    float wantOffset = ai->preferredOffset + insideSign * apexPull + wobble;

    // --- avoid cars we are closing on ----------------------------------------
    Vector2 forward = CarForward(car);
    for (int i = 0; i < neighbourCount; i++) {
        Vector2 rel = { neighbours[i].position.x - car->position.x,
                        neighbours[i].position.y - car->position.y };
        float ahead = rel.x * forward.x + rel.y * forward.y;
        if (ahead < 0.05f || ahead > 1.6f) continue;

        Vector2 right = CarRight(car);
        float side = rel.x * right.x + rel.y * right.y;
        if (fabsf(side) > 0.55f) continue;

        // Steer around the far side: offsets are positive to the left, so a car
        // sitting on our right pushes us left.
        float urgency = (1.0f - ahead / 1.6f) * (1.0f - 0.35f * ai->aggression);
        float push = (side >= 0.0f) ? 1.0f : -1.0f;
        wantOffset += push * urgency * q.halfWidth * 0.9f;
    }
    wantOffset = Clamp(wantOffset, -q.halfWidth * 0.85f, q.halfWidth * 0.85f);

    float blend = 1.0f - expf(-4.5f * dt);
    ai->currentOffset += (wantOffset - ai->currentOffset) * blend;

    // --- steer towards a point down the line ----------------------------------
    float speed01 = Clamp(car->speed / tuning->topSpeed, 0.0f, 1.0f);
    float lookAhead = Lerp(0.55f, 1.9f, speed01) * (0.85f + 0.3f * ai->skill);
    lookAhead *= 1.0f - 0.25f * corner01;

    SplineSample aim = SplineSampleAt(spline, q.distance + lookAhead);
    Vector2 aimLeft = { -aim.tangent.z, aim.tangent.x };
    Vector2 targetPoint = { aim.position.x + aimLeft.x * ai->currentOffset,
                            aim.position.z + aimLeft.y * ai->currentOffset };

    Vector2 toTarget = { targetPoint.x - car->position.x, targetPoint.y - car->position.y };
    float toTargetLen = sqrtf(toTarget.x * toTarget.x + toTarget.y * toTarget.y);
    if (toTargetLen > 1e-5f) {
        toTarget.x /= toTargetLen;
        toTarget.y /= toTargetLen;
    }
    float steerAngle = AngleToRight(forward, toTarget);
    input.steer = Clamp(steerAngle * (1.7f + 0.8f * ai->skill), -1.0f, 1.0f);

    // --- pace -------------------------------------------------------------------
    float cornerScale = 1.0f - 0.52f * corner01 * (1.15f - 0.3f * ai->aggression);
    float targetSpeed = tuning->topSpeed * cornerScale * (0.80f + 0.20f * ai->skill);

    // Running wide costs grip, so ease off until the car is back on line.
    if (fabsf(q.lateral) > q.halfWidth) targetSpeed *= 0.72f;

    float error = targetSpeed - car->forwardSpeed;
    if (error > 0.05f) {
        input.throttle = Clamp(error * 1.6f, 0.0f, 1.0f);
    } else if (error < -0.15f) {
        input.brake = Clamp(-error * 0.85f, 0.0f, 1.0f);
    } else {
        input.throttle = 0.35f;
    }

    // --- unstick --------------------------------------------------------------
    if (car->speed < 0.25f && input.throttle > 0.2f) {
        ai->recoverTimer += dt;
    } else if (car->forwardSpeed > 0.6f) {
        ai->recoverTimer = 0.0f;
    }
    if (ai->recoverTimer > 1.2f) {
        // Reverse away from whatever we are wedged against, steering back to line.
        input.throttle = 0.0f;
        input.brake = 1.0f;
        input.steer = -input.steer;
        if (ai->recoverTimer > 2.6f) ai->recoverTimer = 0.0f;
    }

    return input;
}
