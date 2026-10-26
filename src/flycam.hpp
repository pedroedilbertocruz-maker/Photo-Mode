// Free-flying photo camera and time-of-day stepping. Pure math with no game dependencies, so it
// can be unit tested on its own.
//
// World conventions follow the game: +Y up. Yaw is measured in the XZ plane with
// yaw = atan2(dz, dx) (the same convention as Dusklight's debug fly camera).
#pragma once

namespace photo {

struct Input {
    float move_x = 0.0f;   // main stick, -1..1 (right +)
    float move_y = 0.0f;   // main stick, -1..1 (forward +)
    float look_x = 0.0f;   // C stick, -1..1 (right +)
    float look_y = 0.0f;   // C stick, -1..1 (up +)
    float rise = 0.0f;     // R trigger, 0..1
    float sink = 0.0f;     // L trigger, 0..1
    float roll = 0.0f;     // -1..1 (clockwise +)
    float zoom = 0.0f;     // -1..1 (+ narrows the field of view)
    bool fast = false;
};

struct Tuning {
    float move_speed = 900.0f;     // world units per second
    float fast_multiplier = 5.0f;
    float look_speed = 2.0f;       // radians per second at full deflection
    float roll_speed = 60.0f;      // degrees per second
    float zoom_speed = 30.0f;      // field-of-view degrees per second
    float min_fovy = 5.0f;
    float max_fovy = 120.0f;
    float deadzone = 0.12f;
    bool invert_y = false;
};

struct Camera {
    float eye[3] = {0.0f, 0.0f, 0.0f};
    float yaw = 0.0f;    // radians
    float pitch = 0.0f;  // radians, clamped short of straight up/down
    float fovy = 60.0f;  // degrees
    float bank = 0.0f;   // degrees
};

// Builds a camera looking from eye toward center.
Camera from_look_at(const float eye[3], const float center[3], float fovy, float bank);

// Integrates one frame of input over dt seconds.
void update(Camera& cam, const Input& input, const Tuning& tuning, float dt);

// Point the camera looks at, `distance` units ahead of the eye.
void center(const Camera& cam, float distance, float out[3]);

// Unit forward vector.
void forward(const Camera& cam, float out[3]);

// Applies a radial deadzone to a stick and rescales the remainder to 0..1.
void apply_deadzone(float& x, float& y, float deadzone);

// Game clock helpers. The game stores time of day in degrees: 15 per hour, 0..360.
float advance_time_degrees(float degrees, float hours_delta);

}  // namespace photo
