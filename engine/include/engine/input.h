// Action-based input. Keyboard and gamepad both feed the same struct so the
// game never queries a specific device, and an autopilot can substitute for a
// human by writing the same fields.
#ifndef ENGINE_INPUT_H
#define ENGINE_INPUT_H

#include <stdbool.h>

typedef enum InputAction {
    ACTION_PAUSE = 0,
    ACTION_CONFIRM,
    ACTION_RESET_CAR,
    ACTION_TOGGLE_DEBUG,
    ACTION_TOGGLE_CAMERA,
    ACTION_TOGGLE_NIGHT,
    ACTION_SCREENSHOT,
    ACTION_QUIT,
    ACTION_COUNT
} InputAction;

typedef struct InputState {
    float throttle;                 // 0..1
    float brake;                    // 0..1
    float steer;                    // -1 (left) .. +1 (right)
    bool handbrake;
    bool pressed[ACTION_COUNT];     // edge-triggered this frame
    bool gamepadActive;
} InputState;

void InputInit(void);

// Fills `state` from the keyboard and, when present, gamepad 0.
void InputUpdate(InputState *state);

#endif // ENGINE_INPUT_H
