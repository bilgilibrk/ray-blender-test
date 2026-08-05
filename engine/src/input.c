#include "engine/input.h"

#include <math.h>
#include <string.h>

#include "raylib.h"

#define GAMEPAD_DEADZONE 0.18f

static float ApplyDeadzone(float v)
{
    if (fabsf(v) < GAMEPAD_DEADZONE) return 0.0f;
    // Rescale so the usable range still spans the full 0..1.
    float sign = (v < 0.0f) ? -1.0f : 1.0f;
    return sign * (fabsf(v) - GAMEPAD_DEADZONE) / (1.0f - GAMEPAD_DEADZONE);
}

void InputInit(void)
{
    // Nothing to set up: raylib polls devices for us. Kept for symmetry and to
    // give the module a place to load a rebinding table later.
}

void InputUpdate(InputState *state)
{
    memset(state, 0, sizeof(*state));

    // --- keyboard ---------------------------------------------------------
    if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP)) state->throttle = 1.0f;
    if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN)) state->brake = 1.0f;
    if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT)) state->steer -= 1.0f;
    if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) state->steer += 1.0f;
    if (IsKeyDown(KEY_SPACE)) state->handbrake = true;

    state->pressed[ACTION_PAUSE]         = IsKeyPressed(KEY_P);
    state->pressed[ACTION_CONFIRM]       = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE);
    state->pressed[ACTION_RESET_CAR]     = IsKeyPressed(KEY_R);
    state->pressed[ACTION_TOGGLE_DEBUG]  = IsKeyPressed(KEY_F1);
    state->pressed[ACTION_TOGGLE_CAMERA] = IsKeyPressed(KEY_C);
    state->pressed[ACTION_TOGGLE_NIGHT]  = IsKeyPressed(KEY_N);
    state->pressed[ACTION_SCREENSHOT]    = IsKeyPressed(KEY_F2);
    state->pressed[ACTION_QUIT]          = IsKeyPressed(KEY_ESCAPE);

    // --- gamepad ----------------------------------------------------------
    if (!IsGamepadAvailable(0)) return;
    state->gamepadActive = true;

    float steer = ApplyDeadzone(GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_X));
    if (steer != 0.0f) state->steer = steer;

    // Triggers rest at -1 on most pads, so remap to 0..1.
    float rt = (GetGamepadAxisMovement(0, GAMEPAD_AXIS_RIGHT_TRIGGER) + 1.0f) * 0.5f;
    float lt = (GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_TRIGGER) + 1.0f) * 0.5f;
    if (rt > 0.05f) state->throttle = rt;
    if (lt > 0.05f) state->brake = lt;

    if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_RIGHT_FACE_DOWN)) state->throttle = 1.0f;
    if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_RIGHT_FACE_LEFT)) state->brake = 1.0f;
    if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_RIGHT_TRIGGER_1)) state->handbrake = true;

    if (IsGamepadButtonPressed(0, GAMEPAD_BUTTON_MIDDLE_RIGHT)) state->pressed[ACTION_PAUSE] = true;
    if (IsGamepadButtonPressed(0, GAMEPAD_BUTTON_RIGHT_FACE_DOWN)) state->pressed[ACTION_CONFIRM] = true;
    if (IsGamepadButtonPressed(0, GAMEPAD_BUTTON_RIGHT_FACE_UP)) state->pressed[ACTION_RESET_CAR] = true;

    state->steer = (state->steer < -1.0f) ? -1.0f : (state->steer > 1.0f) ? 1.0f : state->steer;
}
