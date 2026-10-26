#include "flycam.hpp"

#include <algorithm>
#include <cmath>

namespace photo {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kMaxPitch = kPi / 2.0f - 0.05f;

float wrap_angle(float radians) {
    float w = std::fmod(radians, 2.0f * kPi);
    return w < 0.0f ? w + 2.0f * kPi : w;
}

}  // namespace

Camera from_look_at(const float eye[3], const float center[3], float fovy, float bank) {
    Camera cam;
    cam.eye[0] = eye[0];
    cam.eye[1] = eye[1];
    cam.eye[2] = eye[2];
    const float dx = center[0] - eye[0];
    const float dy = center[1] - eye[1];
    const float dz = center[2] - eye[2];
    const float horizontal = std::sqrt(dx * dx + dz * dz);
    cam.yaw = wrap_angle(std::atan2(dz, dx));
    cam.pitch = std::clamp(std::atan2(dy, horizontal), -kMaxPitch, kMaxPitch);
    cam.fovy = std::isfinite(fovy) && fovy > 0.0f ? fovy : 60.0f;
    cam.bank = std::isfinite(bank) ? bank : 0.0f;
    return cam;
}

void forward(const Camera& cam, float out[3]) {
    out[0] = std::cos(cam.yaw) * std::cos(cam.pitch);
    out[1] = std::sin(cam.pitch);
    out[2] = std::sin(cam.yaw) * std::cos(cam.pitch);
}

void center(const Camera& cam, float distance, float out[3]) {
    float f[3];
    forward(cam, f);
    for (int i = 0; i < 3; ++i) {
        out[i] = cam.eye[i] + f[i] * distance;
    }
}

void apply_deadzone(float& x, float& y, float deadzone) {
    const float length = std::sqrt(x * x + y * y);
    if (length <= deadzone || length <= 0.0f) {
        x = 0.0f;
        y = 0.0f;
        return;
    }
    const float scaled = std::min((length - deadzone) / (1.0f - deadzone), 1.0f);
    x = x / length * scaled;
    y = y / length * scaled;
}

void update(Camera& cam, const Input& in, const Tuning& t, float dt) {
    if (!(dt > 0.0f)) {
        return;
    }
    dt = std::min(dt, 0.1f);  // avoid jumps after hitches

    float moveX = in.move_x;
    float moveY = in.move_y;
    float lookX = in.look_x;
    float lookY = in.look_y;
    apply_deadzone(moveX, moveY, t.deadzone);
    apply_deadzone(lookX, lookY, t.deadzone);

    // Look. Squared response gives fine control near the center of the stick.
    const float lookRespX = lookX * std::fabs(lookX);
    const float lookRespY = lookY * std::fabs(lookY) * (t.invert_y ? -1.0f : 1.0f);
    // Slow rotation down when zoomed in so framing stays precise.
    const float zoomScale = std::clamp(cam.fovy / 60.0f, 0.15f, 1.5f);
    cam.yaw = wrap_angle(cam.yaw + lookRespX * t.look_speed * zoomScale * dt);
    cam.pitch = std::clamp(cam.pitch + lookRespY * t.look_speed * zoomScale * dt, -kMaxPitch, kMaxPitch);

    // Move relative to where the camera looks; triggers move straight up/down.
    float f[3];
    forward(cam, f);
    const float rightX = -std::sin(cam.yaw);
    const float rightZ = std::cos(cam.yaw);
    const float speed = t.move_speed * (in.fast ? t.fast_multiplier : 1.0f) * dt;
    const float vertical = std::clamp(in.rise, 0.0f, 1.0f) - std::clamp(in.sink, 0.0f, 1.0f);
    cam.eye[0] += (f[0] * moveY + rightX * moveX) * speed;
    cam.eye[1] += (f[1] * moveY + vertical) * speed;
    cam.eye[2] += (f[2] * moveY + rightZ * moveX) * speed;

    cam.bank += std::clamp(in.roll, -1.0f, 1.0f) * t.roll_speed * dt;
    cam.bank = std::fmod(cam.bank + 540.0f, 360.0f) - 180.0f;  // keep in -180..180

    cam.fovy = std::clamp(cam.fovy - std::clamp(in.zoom, -1.0f, 1.0f) * t.zoom_speed * dt,
        t.min_fovy, t.max_fovy);
}

float advance_time_degrees(float degrees, float hoursDelta) {
    float d = std::fmod(degrees + hoursDelta * 15.0f, 360.0f);
    return d < 0.0f ? d + 360.0f : d;
}

}  // namespace photo
