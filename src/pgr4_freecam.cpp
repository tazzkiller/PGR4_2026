// A free camera (local addition, pgr4 project), to look at the game from anywhere. F5 turns it on and
// off, in the game's window, when pgr4_free_camera is set (pgr4.default.toml: it is, in a release). While it is on, player 1's controller moves it and the game gets none of it:
//   left stick: forward, back and sideways; right stick: look around
//   triggers: down (left), up (right); A held: four times as fast; X held: a quarter
// The game's cameras: 4 of 1680 bytes at 0x82C91D00, +16 the eye, +32 and +48 the orientation
// (quaternion x y z w, from the world to the camera: its conjugate turns the z axis to where the
// camera looks, measured in the game against the car it follows), +496 the mode. The first is the view. sub_827D4688 works them out each
// frame, by the mode, then derives the rest from them (the view is made there): so the camera is
// written before it, and for that call the mode is 0, which works out nothing.
#include <rex/cvar.h>
#include <rex/logging.h>

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

REXCVAR_DEFINE_BOOL(pgr4_free_camera, false, "PGR4",
                    "F5 turns a free camera on and off, moved with the controller");

namespace {
constexpr uint32_t kViewCamera = 0x82C91D00;
using Clock = std::chrono::steady_clock;

float BeF(const uint8_t* p) {
  uint32_t v = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
void PutBeF(uint8_t* p, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (24 - 8 * i));
}
// The quaternion's turn of the z axis.
void TurnZ(const float q[4], float out[3]) {
  out[0] = 2.0f * (q[0] * q[2] + q[3] * q[1]);
  out[1] = 2.0f * (q[1] * q[2] - q[3] * q[0]);
  out[2] = 1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]);
}

std::mutex g_mutex;
bool g_on = false, g_entering = false, g_key_held = false, g_ready = false;
float g_eye[3] = {};
float g_yaw = 0.0f, g_pitch = 0.0f, g_sign = 1.0f;
Clock::time_point g_last;

bool OurWindowInFront() {
  HWND front = GetForegroundWindow();
  DWORD pid = 0;
  if (front) GetWindowThreadProcessId(front, &pid);
  return pid == GetCurrentProcessId();
}
float Axis(int v) {
  const float a = float(v) / 32767.0f;
  const float dead = 0.14f;
  if (std::abs(a) < dead) return 0.0f;
  return (a > 0 ? a - dead : a + dead) / (1.0f - dead);
}
}  // namespace

// After the game's XamInputGetState for user 0 (pgr4_profile.cpp): true when the camera took the
// controller.
bool Pgr4FreeCamInput(uint8_t* base, uint32_t state) {
  if (!REXCVAR_GET(pgr4_free_camera)) return false;
  std::lock_guard<std::mutex> lock(g_mutex);
  const bool down = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
  if (down && !g_key_held && OurWindowInFront()) {
    g_on = !g_on;
    g_entering = g_on;
    g_ready = false;
    g_last = Clock::now();
    REXLOG_INFO("PGR4 free camera: {}", g_on ? "on" : "off");
  }
  g_key_held = down;
  if (!g_on) return false;
  const uint8_t* s = base + state + 4;
  const uint16_t buttons = uint16_t(s[0]) << 8 | s[1];
  const float left = float(s[2]) / 255.0f, right = float(s[3]) / 255.0f;
  auto stick = [&](int at) { return Axis(int16_t(uint16_t(s[at]) << 8 | s[at + 1])); };
  const float lx = stick(4), ly = stick(6), rx = stick(8), ry = stick(10);
  const auto now = Clock::now();
  const float dt = std::min(std::chrono::duration<float>(now - g_last).count(), 0.1f);
  g_last = now;
  if (g_ready) {
    float speed = 12.0f;
    if (buttons & 0x1000) speed *= 4.0f;  // A
    if (buttons & 0x4000) speed *= 0.25f;  // X
    const float look = (buttons & 0x4000) ? 0.5f : 1.4f;
    g_yaw += rx * look * dt;
    g_pitch = std::clamp(g_pitch - g_sign * ry * look * dt, -1.5f, 1.5f);
    const float cp = std::cos(g_pitch), sp = std::sin(g_pitch), cy = std::cos(g_yaw), sy = std::sin(g_yaw);
    const float f[3] = {g_sign * sy * cp, -g_sign * sp, g_sign * cy * cp};
    // To the right of the way we look, y up (seen in the game: heading -x, right is +z).
    float r[3] = {f[2], 0.0f, -f[0]};
    const float rl = std::max(std::sqrt(r[0] * r[0] + r[2] * r[2]), 1e-4f);
    r[0] /= rl, r[2] /= rl;
    for (int k = 0; k < 3; ++k) g_eye[k] += (f[k] * ly + r[k] * lx) * speed * dt;
    g_eye[1] += (right * right - left * left) * speed * dt;
  }
  // The game sees a controller left alone.
  std::memset(base + state + 4, 0, 12);
  return true;
}

// Before sub_827D4688 (pgr4_hooks.cpp): true if the mode was put aside.
bool Pgr4FreeCamBefore(uint8_t* base, uint32_t camera, uint8_t saved[8]) {
  if (camera != kViewCamera || !REXCVAR_GET(pgr4_free_camera)) return false;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_on) return false;
  uint8_t* c = base + camera;
  if (g_entering) {
    // From where the game's camera is, looking where it looks.
    float q[4], z[3];
    for (int k = 0; k < 3; ++k) g_eye[k] = BeF(c + 16 + 4 * k);
    for (int k = 0; k < 3; ++k) q[k] = -BeF(c + 32 + 4 * k);
    q[3] = BeF(c + 44);
    TurnZ(q, z);
    g_sign = 1.0f;
    g_yaw = std::atan2(z[0], z[2]);
    g_pitch = -std::asin(std::clamp(z[1], -1.0f, 1.0f));
    g_entering = false;
    g_ready = true;
    REXLOG_INFO("PGR4 free camera: from {:.1f} {:.1f} {:.1f}, looking {:.2f} {:.2f} {:.2f}", g_eye[0], g_eye[1], g_eye[2], z[0], z[1], z[2]);
  }
  const float hy = g_yaw * 0.5f, hp = g_pitch * 0.5f;
  const float cy = std::cos(hy), sy = std::sin(hy), cp = std::cos(hp), sp = std::sin(hp);
  // Turn about y, then about the camera's x: no roll.
  const float q[4] = {-(cy * sp), -(sy * cp), sy * sp, cy * cp};  // the conjugate, as the game keeps it
  for (int k = 0; k < 3; ++k) PutBeF(c + 16 + 4 * k, g_eye[k]);
  for (int k = 0; k < 4; ++k) {
    PutBeF(c + 32 + 4 * k, q[k]);
    PutBeF(c + 48 + 4 * k, q[k]);
  }
  std::memcpy(saved, c + 496, 4);
  std::memcpy(saved + 4, c + 504, 4);
  std::memset(c + 496, 0, 4);
  return true;
}
void Pgr4FreeCamAfter(uint8_t* base, uint32_t camera, const uint8_t saved[8]) {
  uint8_t* c = base + camera;
  std::memcpy(c + 496, saved, 4);
  std::memcpy(c + 504, saved + 4, 4);
}
