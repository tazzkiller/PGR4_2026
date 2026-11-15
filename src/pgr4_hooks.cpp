// Vendored from pgr4-native (MIT, see pgr4-native/VERSION and LICENSE): games/pgr4/src/pgr4_hooks.cpp,
// with this project's additions at the end.
// Game patches for Project Gotham Racing 4, Title Update #2: the mid-asm hooks the manifest
// (pgr4recompiled_manifest.toml) places in the generated code.

#include <rex/audio/audio_media_player.h>
#include <rex/audio/audio_system.h>
#include <rex/audio/xma/decoder.h>
#include <rex/cvar.h>
#include <rex/runtime.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/ppc/context.h>
#include <rex/system/flags.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <windows.h>

#include <atomic>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <vector>
#include <unordered_map>
#include <string>
#include <cstring>
#include <map>
#include <mutex>
#include <array>
#include <algorithm>
#include <cmath>
#include <bit>
#include <cstdint>

REXCVAR_DEFINE_BOOL(pgr4_60fps, false, "PGR4",
                    "Present every vblank instead of the game's interval (30 FPS) when the native "
                    "renderer does not pace the frames")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// The native renderer (src/pgr4_d3d_trace.cpp).
uint32_t Pgr4PresentationInterval(uint32_t guest_interval);
bool Pgr4FrameRateUnlocked();
float Pgr4MotionBlurStep(float step);
float Pgr4FrameSeconds();

bool Pgr4Enable60Fps() { return REXCVAR_GET(pgr4_60fps); }

void Pgr4PresentationIntervalHook(PPCRegister& r10) { r10.u64 = Pgr4PresentationInterval(r10.u32); }

void Pgr4MotionBlurStepHook(PPCRegister& f0) { f0.f64 = double(Pgr4MotionBlurStep(float(f0.f64))); }

// Local addition: the menus' key repeat (sub_8230F758, per pad button) times a hold and its
// repeats in seconds (0.2 s apart, 0.05 s after 0.5 s held) but adds a fixed 1/60 s per frame,
// so unlocked it repeated as many times faster as the frame rate is over 60. The step it loads
// (lfs f13 from 0x82001170) becomes the frame's measured time step.
void Pgr4MenuRepeatStepHook(PPCRegister& f13) { f13.f64 = double(Pgr4FrameSeconds()); }

// The cockpit camera's head smoothing (sub_827DA5C0): slerp(old, target, HeadAmount) each frame
// was tuned on the console's 30 FPS. At 120 fps it caught up four times as often (68 % of the gap
// per 1/30 s instead of 25 %): the head stuck to the car body and its every shake reached the
// image. Per real frame, 1 - (1 - amount)^(frame time x 30) catches up as on the console.
REXCVAR_DEFINE_BOOL(pgr4_cockpit_head_fix, true, "PGR4",
                    "The cockpit camera's head smoothing as on the console's 30 FPS at any frame "
                    "rate (false: the game's per-frame factor)");
void Pgr4DashHeadHook(PPCRegister& r10) {
  if (!REXCVAR_GET(pgr4_cockpit_head_fix)) return;
  static uint32_t copy = 0;  // a 16-byte aligned stand-in for the variable, value at +32
  auto* memory = rex::system::kernel_memory();
  if (!copy) {
    const uint32_t block = memory->SystemHeapAlloc(96);
    if (!block) return;
    copy = (block + 15) & ~15u;
  }
  auto load = [&](uint32_t a) {
    const uint8_t* p = memory->TranslateVirtual<const uint8_t*>(a);
    return std::bit_cast<float>(uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 |
                                uint32_t(p[2]) << 8 | p[3]);
  };
  const float amount = load(r10.u32 + 32);
  float per_frame = amount;
  if (amount > 0.0f && amount < 1.0f)
    per_frame = 1.0f - std::pow(1.0f - amount, Pgr4FrameSeconds() * 30.0f);
  const uint32_t bits = std::bit_cast<uint32_t>(per_frame);
  uint8_t* out = memory->TranslateVirtual<uint8_t*>(copy + 32);
  for (int i = 0; i < 4; ++i) out[i] = uint8_t(bits >> (24 - 8 * i));
  r10.u64 = copy;
}

// The cockpit's free look (right stick), per 30 FPS frame like the head: the turn step scaled by
// the frames of 1/30 s, the catch-up factor made per real frame.
void Pgr4DashLookStepHook(PPCRegister& f0) {
  if (REXCVAR_GET(pgr4_cockpit_head_fix)) f0.f64 *= double(Pgr4FrameSeconds()) * 30.0;
}
// The look's catch-up softened: the game's (25 % of the gap per 1/30 s, 90 % of the stick's angle
// in 0.27 s) felt violent next to the photo mode's camera turning at a speed.
REXCVAR_DEFINE_DOUBLE(pgr4_cockpit_look_speed, 0.2, "PGR4",
                      "How fast the cockpit's free look (right stick) reaches the stick's angle "
                      "(1: the game's, 0.2: 90 % of it in about 1.5 s)");
void Pgr4DashLookLerpHook(PPCRegister& f13) {
  if (!REXCVAR_GET(pgr4_cockpit_head_fix)) return;
  const double amount = f13.f64 * std::clamp(REXCVAR_GET(pgr4_cockpit_look_speed), 0.05, 1.0);
  if (amount > 0.0 && amount < 1.0)
    f13.f64 = 1.0 - std::pow(1.0 - amount, double(Pgr4FrameSeconds()) * 30.0);
}

// The camera shake's three angles (sub_827DD588) times this: its noise follows real time and its
// intensity the car's motion per second, so at a high frame rate the shake is the game's own,
// seen sharp instead of blurred at 30 FPS.
REXCVAR_DEFINE_DOUBLE(pgr4_camera_shake_scale, 1.0, "PGR4",
                      "The camera shake's strength on bumpy roads (1: the game's, 0: none)");
void Pgr4CameraShakeScaleHook(PPCRegister& f30, PPCRegister& f29, PPCRegister& f28) {
  const double scale = REXCVAR_GET(pgr4_camera_shake_scale);
  if (scale == 1.0) return;
  f30.f64 *= scale;
  f29.f64 *= scale;
  f28.f64 *= scale;
}

// Native 16:10 (development.md): the manifest hooks every instruction that loads the 720 of the
// game's hardcoded 1280x720 frame; each gets the new render height instead. The boundary hooks
// below cover the copies the loads cannot: structs the game fills itself and hands to the D3D
// (viewport, scissor and resolve rects) and the surface creators whose packed sizes the
// viewport clamp reads back (sub_826C6100).
REXCVAR_DECLARE(int32_t, pgr4_render_height);  // native/renderer.cpp: the FFX effects size from it
REXCVAR_DECLARE(std::string, pgr4_native_capture_dir);  // native/draw.cpp: development logs

void Pgr4RenderHeightHook(PPCRegister& r) {
  if (r.u32 == 720) r.u64 = uint32_t(REXCVAR_GET(pgr4_render_height));
}

uint32_t GuestLoad32(uint32_t a) {
  return __builtin_bswap32(*rex::system::kernel_memory()->TranslateVirtual<const uint32_t*>(a));
}

void GuestStore32(uint32_t a, uint32_t v) {
  *rex::system::kernel_memory()->TranslateVirtual<uint32_t*>(a) = __builtin_bswap32(v);
}

// The D3D structs that carry a 1280x720 frame: Resolve's rect (r5 of sub_826CBEE0), the int
// SetViewport's (r4 of sub_826C6378) and SetScissorRect's (r4 of sub_826C59D8), both
// {x1, y1, x2, y2}. A 1280x720 one becomes the render height.
void Pgr4ResolveRectHook(PPCRegister& r5) {
  if (!r5.u32) return;
  if (GuestLoad32(r5.u32 + 8) == 1280 && GuestLoad32(r5.u32 + 12) == 720)
    GuestStore32(r5.u32 + 12, uint32_t(REXCVAR_GET(pgr4_render_height)));
}

void Pgr4ViewportHook(PPCRegister& r4) {
  if (!r4.u32) return;
  if (GuestLoad32(r4.u32 + 8) == 1280 && GuestLoad32(r4.u32 + 12) == 720)
    GuestStore32(r4.u32 + 12, uint32_t(REXCVAR_GET(pgr4_render_height)));
}

// The float viewport setter (sub_826C63F8, the one the scene's views use): r4 points at
// {X, Y, W, H, MinZ, MaxZ} floats.
void Pgr4ViewportFHook(PPCRegister& r4) {
  if (!r4.u32) return;
  constexpr uint32_t kW = 0x44A00000;  // 1280.0f
  constexpr uint32_t kH = 0x44340000;  // 720.0f
  if (GuestLoad32(r4.u32 + 8) == kW && GuestLoad32(r4.u32 + 12) == kH) {
    const float h = float(REXCVAR_GET(pgr4_render_height));
    GuestStore32(r4.u32 + 12, std::bit_cast<uint32_t>(h));
  }
}

// The vertex block's c45, the HUD's orthographic projection, uploaded with the block by the
// state apply (sub_82871A20): the game maps its 640x480 HUD space into the central 960x720 of
// its 720-tall frame (4:3-safe, the pixel scale 1.5). With the frame pgr4_render_height tall,
// the y scale and offset still divide by 720 and every c45 draw (the dials, the HUD text, the
// menus) stretches by height/720. Rewrite the game's own value to the same mapping of the
// taller frame: the central 960x720 of it, centered, as the 960-wide area already is.
void Pgr4HudOrthoHook(PPCRegister& r5) {
  const float h = float(REXCVAR_GET(pgr4_render_height));
  if (h == 720.0f || !r5.u32) return;   // the 16:10 patch is off
  const uint32_t c = r5.u32 + 45 * 16;  // c45 of the 256-register block
  if (std::bit_cast<float>(GuestLoad32(c)) != 3.0f / 1280.0f ||
      std::bit_cast<float>(GuestLoad32(c + 4)) != -3.0f / 720.0f ||
      std::bit_cast<float>(GuestLoad32(c + 8)) != -0.75f ||
      std::bit_cast<float>(GuestLoad32(c + 12)) != 1.0f)
    return;  // not the game's HUD ortho (another mode's): leave it
  GuestStore32(c + 4, std::bit_cast<uint32_t>(-3.0f / h));
  GuestStore32(c + 12, std::bit_cast<uint32_t>(720.0f / h));
}

// SetScissorRect (fn_826C59D8): r4 points at {x1, y1, x2, y2}.
void Pgr4ScissorRectHook(PPCRegister& r4) {
  if (!r4.u32) return;
  if (GuestLoad32(r4.u32 + 8) == 1280 && GuestLoad32(r4.u32 + 12) == 720)
    GuestStore32(r4.u32 + 12, uint32_t(REXCVAR_GET(pgr4_render_height)));
}

namespace {

// The walking camera's call to its collision callback (the bctrl at 0x827DD51C).
constexpr uint32_t kGarageWalkReturn = 0x827DD520;
// The photo mode's camera (mode 513, sub_827D2400: its bctrl at 0x827D2C78), whose D-pad up and
// down moved only now and then above 30 FPS, depending on the camera's angle.
constexpr uint32_t kPhotoCameraReturn = 0x827D2C7C;
// The callback makes no calls between its entry and the step count but the register save helper,
// so the caller recorded at entry is still this frame's.
thread_local bool t_garage_walk_call = false;

constexpr uint32_t kCarPreviewCellLimit = 1024;
constexpr uint32_t kCarPreviewChainLimit = 4096;
thread_local uint32_t t_car_preview_cells = 0;
thread_local uint32_t t_car_preview_chain = 0;

}  // namespace

void Pgr4GarageCollisionCallerHook(PPCRegister& r12) {
  t_garage_walk_call = r12.u32 == kGarageWalkReturn || r12.u32 == kPhotoCameraReturn;
}

// The step count is trunc(distance * 20), and a count of zero skips the move. The callback itself
// rejects effectively zero moves first (squared length under its epsilon), so at unlocked frame
// rates the sub-5 cm moves of a short frame get one step instead of none, for the walking and
// photo cameras; other collision users and 30 FPS keep the game's count.
void Pgr4GarageCollisionStepsHook(PPCRegister& r29) {
  if (r29.s32 == 0 && t_garage_walk_call && Pgr4FrameRateUnlocked()) r29.s64 = 1;
}

void Pgr4CarPreviewEnterHook() { t_car_preview_cells = 0; }

bool Pgr4CarPreviewCellHook(PPCRegister& r30, PPCRegister& r31) {
  if (++t_car_preview_cells <= kCarPreviewCellLimit) [[likely]]
    return false;
  static std::atomic_bool logged = false;
  if (!logged.exchange(true))
    REXLOG_WARN(
        "PGR4 car preview: visibility search passed {} cells (row {}, column {}); ending it",
        kCarPreviewCellLimit, r30.u32, r31.u32);
  return true;
}

void Pgr4CarPreviewChainStartHook() { t_car_preview_chain = 0; }

bool Pgr4CarPreviewChainHook(PPCRegister& r3, PPCRegister& r10) {
  if (++t_car_preview_chain <= kCarPreviewChainLimit) [[likely]]
    return false;
  static std::atomic_bool logged = false;
  if (!logged.exchange(true))
    REXLOG_WARN(
        "PGR4 car preview: visibility list passed {} nodes (node {:#x}, cell {:#x}); "
        "skipping the rest",
        kCarPreviewChainLimit, r3.u32, r10.u32);
  return true;
}

// --- Additions (not from pgr4-native): Xenia Canary's game patches for this title update
// (github.com/xenia-canary/game-patches, "4D5307F9 - Project Gotham Racing 4 (TU2).patch.toml").
// Xenia rewrites the instructions in memory; recompiled code ignores that, so each patch is a
// mid-asm hook after the instruction (config/tu2.toml). sub_829DEA18 / sub_829D6FB0: li r11,1 is
// the "enabled" byte (stb r11,96(r1)) of the option registered through sub_82297010.
REXCVAR_DEFINE_BOOL(pgr4_disable_motion_blur, false, "PGR4",
                    "Disable motion blur (Xenia patch by illusion)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(pgr4_disable_hdr, false, "PGR4",
                    "Disable HDR; avoids flickering when upscaling (Xenia patch by remco, "
                    "SuperType1)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

void Pgr4MotionBlurFlagHook(PPCRegister& r11) {
  if (REXCVAR_GET(pgr4_disable_motion_blur)) r11.u64 = 0;
}

void Pgr4HdrFlagHook(PPCRegister& r11) {
  if (REXCVAR_GET(pgr4_disable_hdr)) r11.u64 = 0;
}

// --- The cockpit view (local additions). Big-endian guest floats.
namespace {
float BeF(const uint8_t* p) {
  return std::bit_cast<float>(uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 |
                              p[3]);
}
void PutBeF(uint8_t* p, float f) {
  const uint32_t v = std::bit_cast<uint32_t>(f);
  for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (24 - 8 * i));
}
uint64_t g_frame_no = 0;  // the game's frames (sub_824C6FD8)
std::unordered_map<uint32_t, uint64_t> g_drawn_records;  // vehicle record -> the frame it was drawn in
}  // namespace
void Pgr4EyeFollowsBody(uint8_t* base, uint32_t camera);
void Pgr4AutopilotCar(uint32_t record, uint64_t frame);  // src/pgr4_autopilot.cpp

// --- Crowds (local addition): the radius where their spectators turn 3D, written into each
// layout as the game makes it (at a track's load). Not each frame: a layout freed when the race
// ends would be written into after (the game crashed loading the next race).
REXCVAR_DEFINE_DOUBLE(pgr4_crowd_3d_radius, 5.0, "PGR4",
                      "The crowds' radius where their blocks of spectators turn from flat sprites "
                      "to 3D characters (layout +52; the game's 2.5, about 40-60 m, made them pop "
                      "in close). The game keeps 880 3D characters at most: too large, some disappear");
// The crowds' layout constructor (sub_824043E8, r3 the layout, +52 its 3D radius: 2.5).
REX_EXTERN(__imp__sub_824043E8);
REX_HOOK_RAW(sub_824043E8) {
  const uint32_t layout = ctx.r3.u32;
  __imp__sub_824043E8(ctx, base);
  PutBeF(base + layout + 52, float(std::clamp(REXCVAR_GET(pgr4_crowd_3d_radius), 0.5, 20.0)));
  REXLOG_INFO("PGR4 crowds: layout {:08X}, 3D radius {:.2f}", layout, BeF(base + layout + 52));
}

// The garage's arcade machines are only scenery (Geometry Wars, gw.xex, is not recompiled): for
// Ingame\GarageHUD.lua, Lua's ArcadeActive() (sub_825EAEB8, which pushes a flag) always answers false,
// so no IDS_A_PLAY_GW, and LaunchGW (sub_825CC2B8) does nothing.
REX_EXTERN(sub_82506638);  // lua_pushboolean(L, b)
REX_HOOK_RAW(sub_825EAEB8) {
  ctx.r4.u64 = 0;
  sub_82506638(ctx, base);
  ctx.r3.u64 = 1;  // one result
}
REX_HOOK_RAW(sub_825CC2B8) {
  (void)base;
  ctx.r3.u64 = 0;  // no results
}

REX_EXTERN(__imp__sub_824C6FD8);
extern float g_pgr4_camera_eye[3];  // pgr4-native draw.cpp
extern float g_pgr4_car_speed;
extern float g_pgr4_car_side[3];
extern float g_pgr4_car_back[3];
extern float g_pgr4_car_pos[3];
extern bool g_pgr4_game_paused;
extern float g_pgr4_car_yaw_rate;
REXCVAR_DEFINE_BOOL(pgr4_ui_clock_probe, false, "PGR4",
                    "Development: the UI's frame time and the game clock's, in the log");
void Pgr4BringWindowToFront();  // src/pgr4_menu.cpp
bool Pgr4Precompiling();       // src/pgr4_profile.cpp
REX_HOOK_RAW(sub_824C6FD8) {
  ++g_frame_no;
  __imp__sub_824C6FD8(ctx, base);
  // The game's clock (seconds, stands still in the pause and the photo mode; measured 04/11/2026):
  // unchanged for some frames, paused.
  {
    static float last_game_clock = 0.0f;
    static int same_game_clock = 0;
    const float game_clock = BeF(base + 0x82A8131C + 24);
    same_game_clock = game_clock == last_game_clock ? same_game_clock + 1 : 0;
    last_game_clock = game_clock;
    g_pgr4_game_paused = same_game_clock >= 6;
  }
  // The speed of the car nearest the camera, for the rain's running drops (pgr4-native draw.cpp);
  // no car drawn lately: 0 (the menus).
  {
    float best = 1e12f, speed = 0.0f;
    uint32_t car = 0;
    for (const auto& [record, frame] : g_drawn_records) {
      if (frame + 2 < g_frame_no) continue;
      float d2 = 0.0f, v2 = 0.0f;
      for (int k = 0; k < 3; ++k) {
        const float e = BeF(base + record + 736 + 48 + 4 * k) - g_pgr4_camera_eye[k];
        const float v = BeF(base + record + 880 + 4 * k);
        d2 += e * e;
        v2 += v * v;
      }
      if (d2 < best) best = d2, speed = std::sqrt(v2), car = record;
    }
    g_pgr4_car_speed = speed;
    // In a bend, over some 50 km/h: towards the bend's inside, the car's right times how hard it
    // turns (the turn about the height, +912 + 4, times the speed: 1 at 10 m/s2).
    float side[3] = {}, back[3] = {};
    if (car) {
      const uint8_t* pose = base + car + 736;
      for (int k = 0; k < 3; ++k) g_pgr4_car_pos[k] = BeF(pose + 48 + 4 * k);
      const float fx = BeF(pose + 32), fz = BeF(pose + 40);
      const float len = std::max(std::sqrt(fx * fx + fz * fz), 1e-3f);
      const float ramp = std::clamp((speed - 14.0f) / 11.0f, 0.0f, 1.0f);
      const float turn = std::clamp(BeF(base + car + 912 + 4) * speed / 10.0f, -1.0f, 1.0f) * ramp;
      g_pgr4_car_yaw_rate = BeF(base + car + 912 + 4);
      side[0] = fz / len * turn;
      side[2] = -fx / len * turn;
      // The wind of the car's speed: backwards, from some 45 km/h, strongest over 170 km/h.
      const float wind = std::clamp((speed - 12.0f) / 35.0f, 0.0f, 1.0f) * 1.4f;
      for (int k = 0; k < 3; ++k) back[k] = BeF(pose + 32 + 4 * k) * wind;  // the pose's z axis: towards the back
    }
    if (!car)
      for (int k = 0; k < 3; ++k) g_pgr4_car_pos[k] = 1e6f;
    for (int k = 0; k < 3; ++k) {
      g_pgr4_car_side[k] = side[k];
      g_pgr4_car_back[k] = back[k];
    }
  }
  // The window in front once more after a second of frames, if it was not shown yet when the
  // app asked (src/pgr4_app.h).
  if (g_frame_no == 60 && !Pgr4Precompiling()) Pgr4BringWindowToFront();
  if (REXCVAR_GET(pgr4_ui_clock_probe) && g_frame_no % 300 == 0) {
    const uint8_t* ui = base + 0x82CAAA64;
    const uint8_t* game = base + 0x82A8131C;
    REXLOG_INFO("PGR4 clocks: frame {:.5f} s; UI +4..+28 {:.5f} {:.5f} {:.5f} {:.5f} {:.5f} {:.5f} {:.5f}; "
                "game {:.5f} {:.5f} {:.5f} {:.5f} {:.5f} {:.5f} {:.5f}",
                Pgr4FrameSeconds(), BeF(ui + 4), BeF(ui + 8), BeF(ui + 12), BeF(ui + 16), BeF(ui + 20),
                BeF(ui + 24), BeF(ui + 28), BeF(game + 4), BeF(game + 8), BeF(game + 12),
                BeF(game + 16), BeF(game + 20), BeF(game + 24), BeF(game + 28));
  }
}

// The cockpit camera (sub_827DA5C0, r3: +16 the eye, +32 its orientation).
REX_EXTERN(__imp__sub_827DA5C0);
REX_HOOK_RAW(sub_827DA5C0) {
  const uint32_t camera = ctx.r3.u32;
  __imp__sub_827DA5C0(ctx, base);
  Pgr4EyeFollowsBody(base, camera);
}

// The cars' drawn body (sub_82416BA0: the vehicle record's drawn pose, +12560, from its pose at
// +736, directly or carried forward by sub_82417B50). +736 turns with a jitter of about 0.001 rad
// from frame to frame that the record's own pose next to it (+800) and the physics do not have.
// Seen from outside it is under a pixel; in the cockpit view the camera holds steady (the head
// smooths it) while the cockpit around it, drawn from that pose, shook by several pixels a frame.
// The drawn pose takes +800's orientation with +736's position; +736 is put back after.
// The cockpit camera's eye (sub_827DA5C0, +16) is computed after the cars' drawn poses (+12560),
// from the car position the camera hangs from (+400) and the driver's head turned by an
// orientation that changes with the 60 Hz steps: in a bend the eye jumped sideways by about 9 mm
// every other frame against the drawn car (0.35 m to the side of its centre), and the wheel and the
// mirror, 0.6 m away, shook by some 20 px. The eye is put back at the drawn car's pose plus its place
// in the car averaged over 0.3 s: the head's motion kept, the frame-to-frame jumps gone.
REXCVAR_DEFINE_BOOL(pgr4_cockpit_body_follow, true, "PGR4",
                    "In the cockpit view, the eye held to the drawn car (the cockpit steady around it)");
namespace {
std::unordered_map<uint32_t, std::array<float, 3>> g_eye_places;  // camera -> eye in the car's axes
}  // namespace
void Pgr4EyeFollowsBody(uint8_t* base, uint32_t camera) {
  if (!REXCVAR_GET(pgr4_cockpit_body_follow)) return;
  float eye[3];
  for (int k = 0; k < 3; ++k) eye[k] = BeF(base + camera + 16 + 4 * k);
  uint32_t best = 0;
  float best_d2 = 2.5f * 2.5f;
  // The car drawn this frame (the cars are updated before the cameras).
  for (const auto& [record, frame] : g_drawn_records) {
    if (frame != g_frame_no) continue;
    float d2 = 0;
    for (int k = 0; k < 3; ++k) {
      const float e = BeF(base + record + 12560 + 48 + 4 * k) - eye[k];
      d2 += e * e;
    }
    if (d2 < best_d2) best_d2 = d2, best = record;
  }
  if (!best) return;
  const uint8_t* m = base + best + 12560;  // rows: x, y, z axes, position
  float axes[3][3], pos[3], place[3];
  for (int r = 0; r < 3; ++r)
    for (int k = 0; k < 3; ++k) axes[r][k] = BeF(m + 16 * r + 4 * k);
  for (int k = 0; k < 3; ++k) pos[k] = BeF(m + 48 + 4 * k);
  for (int r = 0; r < 3; ++r) {
    float d = 0;
    for (int k = 0; k < 3; ++k) d += (eye[k] - pos[k]) * axes[r][k];
    place[r] = d;
  }
  auto [it, fresh] = g_eye_places.try_emplace(camera);
  auto& avg = it->second;
  const float a = fresh ? 1.0f : 1.0f - std::exp(-Pgr4FrameSeconds() / 0.3f);
  for (int r = 0; r < 3; ++r) avg[r] += (place[r] - avg[r]) * a;
  for (int k = 0; k < 3; ++k) {
    float v = pos[k];
    for (int r = 0; r < 3; ++r) v += avg[r] * axes[r][k];
    PutBeF(base + camera + 16 + 4 * k, v);
  }
}
REXCVAR_DEFINE_BOOL(pgr4_steady_body_pose, true, "PGR4",
                    "The cars drawn with their steady orientation (no frame-to-frame jitter of the "
                    "body, which shook the cockpit view)");
namespace {
std::unordered_map<uint32_t, std::array<float, 3>> g_body_offsets;  // body - eye, averaged
struct StepTurn {
  double last[3][3], prev[3][3];  // rows: the x, y, z axes
  bool has_last = false, has_prev = false;
  uint64_t frame = 0;  // g_frame_no when last used: a record unseen for a while is another car
};
std::unordered_map<uint32_t, StepTurn> g_step_turns;
// The record's +800 orientation (rows at `steady`) turned on by `ahead` seconds of the turn between
// its last two values, written as the rows at `out`.
void CarryTurnTo(uint8_t* out, uint8_t* base, uint32_t record, const uint8_t* steady, float ahead) {
  double now[3][3];
  for (int r = 0; r < 3; ++r)
    for (int k = 0; k < 3; ++k) now[r][k] = BeF(steady + 16 * r + 4 * k);
  StepTurn& st = g_step_turns[record];
  // A record not drawn for over a second (60 frames or more) may be another car now, as at a new
  // race: its last turns forgotten, not mixed with the new car's.
  if (st.frame + 60 < g_frame_no) st.has_last = st.has_prev = false;
  st.frame = g_frame_no;
  bool changed = !st.has_last;
  for (int r = 0; r < 3 && !changed; ++r)
    for (int k = 0; k < 3 && !changed; ++k) changed = now[r][k] != st.last[r][k];
  if (changed) {
    if (st.has_last) std::memcpy(st.prev, st.last, sizeof(st.prev));
    st.has_prev = st.has_last;
    std::memcpy(st.last, now, sizeof(st.last));
    st.has_last = true;
  }
  const double h = BeF(base + 0x82001170);  // the physics step, 1/60 s
  if (!st.has_prev || !(h > 0) || !(ahead > 0)) return;
  const double frac = std::min(double(ahead) / h, 2.0);
  // R = A^T (columns: the axes); the last step's turn D = R_last R_prev^T as an axis and an angle,
  // applied for frac of a step: R_out = D^frac R_last.
  double d[3][3];
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      double v = 0;
      for (int k = 0; k < 3; ++k) v += st.last[k][i] * st.prev[k][j];
      d[i][j] = v;
    }
  const double c = std::clamp((d[0][0] + d[1][1] + d[2][2] - 1.0) / 2.0, -1.0, 1.0);
  const double angle = std::acos(c);
  double axis[3] = {d[2][1] - d[1][2], d[0][2] - d[2][0], d[1][0] - d[0][1]};
  const double n = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
  if (!(angle > 1e-7 && angle < 0.5 && n > 1e-12)) return;
  for (double& a : axis) a /= n;
  const double t = angle * frac, ct = std::cos(t), sn = std::sin(t), C = 1 - ct;
  const double x = axis[0], y = axis[1], z = axis[2];
  const double q[3][3] = {{ct + x * x * C, x * y * C - z * sn, x * z * C + y * sn},
                          {y * x * C + z * sn, ct + y * y * C, y * z * C - x * sn},
                          {z * x * C - y * sn, z * y * C + x * sn, ct + z * z * C}};
  for (int r = 0; r < 3; ++r)
    for (int i = 0; i < 3; ++i) {
      double v = 0;
      for (int k = 0; k < 3; ++k) v += q[i][k] * st.last[r][k];
      PutBeF(out + 16 * r + 4 * i, float(v));
    }
}
void CarryTurn(uint8_t* base, uint32_t record, const uint8_t* steady, float ahead) {
  CarryTurnTo(base + record + 736, base, record, steady, ahead);
}
struct SteadyBody {
  uint8_t* pose;
  uint8_t saved[64];
  bool active = false;
  // `ahead`: the time the caller carries the pose forward by its velocity (+880), 0 for none.
  SteadyBody(uint8_t* base, uint32_t record, float ahead) : pose(base + record + 736) {
    if (record < 0x40000000) return;
    std::memcpy(saved, pose, 64);
    if (REXCVAR_GET(pgr4_steady_body_pose)) {
      // Only an orthonormal-looking +800 (its x axis of unit length), and only close to +736's
      // own orientation: the jitter it removes is about 0.001 rad, and the car's other parts
      // (driver, wipers, a loose bumper) are placed from +736 elsewhere, so a +800 that is
      // further off (a crash, a reset, a car whose +800 has not followed yet) would draw the
      // body apart from them. Each axis within 0.03 rad (cos 0.99955).
      const uint8_t* steady = base + record + 800;
      const float x0 = BeF(steady), x1 = BeF(steady + 4), x2 = BeF(steady + 8);
      const float len = x0 * x0 + x1 * x1 + x2 * x2;
      float worst = 1.0f;
      for (int r = 0; r < 3; ++r) {
        float d = 0;
        for (int k = 0; k < 3; ++k) d += BeF(steady + 16 * r + 4 * k) * BeF(pose + 16 * r + 4 * k);
        worst = std::min(worst, d);
      }
      const bool close = worst > 0.99955f;
      if (len > 0.98f && len < 1.02f && !close) {
        static int logged = 0;
        if (logged < 20) {
          ++logged;
          REXLOG_INFO("PGR4 steady pose: car {:08X} skipped, +800 {:.3f} rad from +736", record,
                      std::acos(std::clamp(worst, -1.0f, 1.0f)));
        }
      }
      if (len > 0.98f && len < 1.02f && close) {
        std::memcpy(pose, steady, 48);  // rows x, y, z (with their w)
        active = true;
        // +800 turns only on the 60 Hz steps: carried forward like the position, by the turn
        // between its last two orientations for `ahead` of a step (in a bend, 0.5 degree a step).
        CarryTurn(base, record, steady, ahead);
      }
    }
  }
  ~SteadyBody() {
    if (active) std::memcpy(pose, saved, 64);
  }
};
}  // namespace
// Tests: the car the camera is on held where it is, for looking at it at leisure (the game lets a
// stopped car creep back, 0.015 km/h, also under Xenia). Its pose and speeds written over, before
// each drawing of it. Overlay, Tests tab; not kept in the settings.
REXCVAR_DEFINE_BOOL(pgr4_freeze_car, false, "PGR4", "Tests: the car held where it is");
namespace {
uint32_t g_frozen_car = 0;
float g_frozen_at[3] = {};
void HoldFrozenCar(uint8_t* base, uint32_t record) {
  if (!REXCVAR_GET(pgr4_freeze_car)) {
    g_frozen_car = 0;
    return;
  }
  if (!g_frozen_car) {
    // The car nearest the camera, at the moment it is asked.
    float best = 1e12f;
    for (const auto& [car, frame] : g_drawn_records) {
      if (frame + 2 < g_frame_no) continue;
      float d2 = 0.0f;
      for (int k = 0; k < 3; ++k) {
        const float e = BeF(base + car + 736 + 48 + 4 * k) - g_pgr4_camera_eye[k];
        d2 += e * e;
      }
      if (d2 < best) best = d2, g_frozen_car = car;
    }
    if (!g_frozen_car) return;
    for (int k = 0; k < 3; ++k) g_frozen_at[k] = BeF(base + g_frozen_car + 736 + 48 + 4 * k);
    REXLOG_INFO("PGR4 freeze: car {:08X} at {:.2f} {:.2f} {:.2f}", g_frozen_car, g_frozen_at[0],
                g_frozen_at[1], g_frozen_at[2]);
  }
  if (record != g_frozen_car) return;
  for (int k = 0; k < 3; ++k) {
    PutBeF(base + record + 736 + 48 + 4 * k, g_frozen_at[k]);
    PutBeF(base + record + 800 + 48 + 4 * k, g_frozen_at[k]);
    PutBeF(base + record + 880 + 4 * k, 0.0f);
    PutBeF(base + record + 912 + 4 * k, 0.0f);
  }
}
}  // namespace
REX_EXTERN(__imp__sub_82417B50);
REX_HOOK_RAW(sub_82417B50) {
  const uint32_t record = ctx.r3.u32;
  if (record >= 0x40000000) HoldFrozenCar(base, record);
  {
    SteadyBody steady(base, record, float(ctx.f1.f64));
    __imp__sub_82417B50(ctx, base);
  }
  if (record >= 0x40000000) {
    g_drawn_records[record] = g_frame_no;
    Pgr4AutopilotCar(record, g_frame_no);
  }
}
// Development: the route's file the game loads (src/pgr4_autopilot.cpp).
void Pgr4AutopilotRouteString(const uint8_t* base, uint32_t string);
void Pgr4RaceWeather(const uint8_t* base);  // src/pgr4_autopilot.cpp
bool Pgr4FreeCamBefore(uint8_t* base, uint32_t camera, uint8_t saved[8]);  // src/pgr4_freecam.cpp
void Pgr4FreeCamAfter(uint8_t* base, uint32_t camera, const uint8_t saved[8]);
REX_EXTERN(__imp__sub_827D4688);
REX_HOOK_RAW(sub_827D4688) {
  const uint32_t camera = ctx.r3.u32;
  uint8_t saved[8];
  const bool free_camera = Pgr4FreeCamBefore(base, camera, saved);
  __imp__sub_827D4688(ctx, base);
  if (free_camera) Pgr4FreeCamAfter(base, camera, saved);
}
REX_EXTERN(__imp__sub_8264A358);
REX_HOOK_RAW(sub_8264A358) {
  Pgr4RaceWeather(base);
  Pgr4AutopilotRouteString(base, ctx.r4.u32);
  __imp__sub_8264A358(ctx, base);
}
REX_EXTERN(__imp__sub_82416BA0);
REX_HOOK_RAW(sub_82416BA0) {
  // Called with no pose (r4 = 0) it takes +736 itself; from sub_82417B50 +736 is already steady.
  const uint32_t record = ctx.r3.u32;
  if (record >= 0x40000000) {
    Pgr4AutopilotCar(record, g_frame_no);
    HoldFrozenCar(base, record);
  }
  if (ctx.r4.u32) {
    __imp__sub_82416BA0(ctx, base);
    return;
  }
  {
    SteadyBody steady(base, record, 0.0f);
    __imp__sub_82416BA0(ctx, base);
  }
  if (record >= 0x40000000) g_drawn_records[record] = g_frame_no;
}

// pgr4_net_start_probe: the network race start check (sub_8265C268, NetRaceCore.RaceStartSynchronized:
// true when every player of the race, ids at list+520 and count at list+528 with list = r3+64, has a
// state (+16 of sub_8266C398(r3+48, id)) of 17 or more) and the local loaded state's broadcast
// (sub_825D5140, SetRaceLoadedState): logged when what it sees changes.
REXCVAR_DEFINE_BOOL(pgr4_net_start_probe, false, "PGR4",
                    "Development: what the network race start check sees, in the log");
namespace {
uint32_t Be32(const uint8_t* base, uint32_t a) {
  return uint32_t(base[a]) << 24 | uint32_t(base[a + 1]) << 16 | uint32_t(base[a + 2]) << 8 |
         base[a + 3];
}
}  // namespace
REX_EXTERN(__imp__sub_8265C268);
REX_EXTERN(sub_8266C398);
REX_HOOK_RAW(sub_8265C268) {
  if (!REXCVAR_GET(pgr4_net_start_probe)) {
    __imp__sub_8265C268(ctx, base);
    return;
  }
  const uint32_t core = ctx.r3.u32;
  const PPCContext saved = ctx;
  std::string seen = fmt::format("flags {} {} {}", base[core], base[core + 1], base[core + 7]);
  const uint32_t list = Be32(base, core + 64);
  if (list) {
    const uint32_t count = Be32(base, list + 528);
    seen += fmt::format(", {} players:", count);
    for (uint32_t i = 0; i < count && i < 16; ++i) {
      const uint8_t id = base[list + 520 + i];
      ctx.r3.u64 = Be32(base, core + 48);
      ctx.r4.u64 = id;
      sub_8266C398(ctx, base);
      const uint32_t player = ctx.r3.u32;
      seen += player ? fmt::format(" [id {} state {}]", id, int32_t(Be32(base, player + 16)))
                     : fmt::format(" [id {} none]", id);
    }
  }
  ctx = saved;
  __imp__sub_8265C268(ctx, base);
  seen += fmt::format(" -> {}", ctx.r3.u32);
  static std::string last;
  if (seen != last) {
    last = seen;
    REXLOG_INFO("PGR4 race start check: {}", seen);
  }
}
// pgr4_audio_occlusion_probe: the vehicles' audio occlusion (sub_826A12A8, from their occlusion
// queries: 0 heard, 1 hidden), 8 vehicles per viewport at 0x82C8A2E0, read by the car and bike
// audio (sub_826A10D0); logged when a value moves by 0.1 or more, with the frame number.
REXCVAR_DEFINE_BOOL(pgr4_audio_occlusion_probe, false, "PGR4",
                    "Development: the vehicles' audio occlusion values, in the log");
REX_EXTERN(__imp__sub_826A12A8);
REX_HOOK_RAW(sub_826A12A8) {
  __imp__sub_826A12A8(ctx, base);
  if (!REXCVAR_GET(pgr4_audio_occlusion_probe)) return;
  constexpr uint32_t kOcclusion = 0x82C8A2E0;
  static bool started = false;
  if (!std::exchange(started, true)) REXLOG_INFO("PGR4 audio occlusion: probe on");
  static float last[16] = {};
  bool moved = false;
  float now[16];
  for (int i = 0; i < 16; ++i) {
    now[i] = BeF(base + kOcclusion + 4 * i);
    moved |= std::fabs(now[i] - last[i]) >= 0.1f;
  }
  if (!moved) return;
  std::memcpy(last, now, sizeof(now));
  REXLOG_INFO("PGR4 audio occlusion (frame {}): {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} "
              "{:.2f} | {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f}",
              g_frame_no, now[0], now[1], now[2], now[3], now[4], now[5], now[6], now[7], now[8],
              now[9], now[10], now[11], now[12], now[13], now[14], now[15]);
}

// pgr4_audio_param_probe: the vehicles' sound parameters (sub_8268ED20(sound, ?, index, value),
// which the car and bike audio call each frame: revs, throttle, speeds, occlusion 30...), the
// latest value of each by sound object, logged every 30 game frames.
REXCVAR_DEFINE_BOOL(pgr4_audio_param_probe, false, "PGR4",
                    "Development: the vehicles' sound parameters, in the log");
// pgr4_audio_resampler: the game's Audio.PerfTweaks.NewResampler (0x82C9517C, its value at +32),
// read by sub_82818120 / sub_82818148: 1 the vector resamplers (sub_828173B8, sub_828176E0), 0
// the scalar ones (sub_828172D8, sub_828175D0); -1 leaves the game's. Development test.
REXCVAR_DEFINE_INT32(pgr4_audio_resampler, -1, "PGR4",
                     "Development: the game's audio resampler (-1 its own, 0 scalar, 1 vector)");
// pgr4_audio_filter_probe: the mixers' low-pass coefficients (sub_828397A8(mixer, f1): f1 the
// voice's rate ratio, cutoff 0.5 / f1 above 0.5; b at +64, a at +68 of the mixer), the latest of
// each mixer logged with the audio parameters (every 30 frames) when it changed.
REXCVAR_DEFINE_BOOL(pgr4_audio_filter_probe, false, "PGR4",
                    "Development: the audio mixers' low-pass inputs and coefficients, in the log");
REX_EXTERN(__imp__sub_828397A8);
REX_HOOK_RAW(sub_828397A8) {
  const uint32_t mixer = ctx.r3.u32;
  const double ratio = ctx.f1.f64;
  __imp__sub_828397A8(ctx, base);
  if (!REXCVAR_GET(pgr4_audio_filter_probe)) return;
  struct Seen {
    double ratio;
    float b, a;
  };
  static std::mutex mutex;
  static std::map<uint32_t, Seen> seen;
  static uint64_t dumped = 0;
  std::lock_guard<std::mutex> lock(mutex);
  seen[mixer] = {ratio, BeF(base + mixer + 64), BeF(base + mixer + 68)};
  if (g_frame_no < dumped + 30) return;
  dumped = g_frame_no;
  std::string line;
  for (const auto& [m, s] : seen)
    line += fmt::format(" {:08X}:r{:.3f}/b{:.4f}/a{:.4f}", m, s.ratio, s.b, s.a);
  REXLOG_INFO("PGR4 audio filters (frame {}):{}", g_frame_no, line);
  seen.clear();
}

// pgr4_audio_compressor_probe: the game's compressors (sub_8280AA48(dsp, samples), once per 256
// sample block: RMS level +48, threshold +60, ratio +64, attack +52, release +56, gain applied
// +72 toward 1), the latest of each logged every 30 game frames.
REXCVAR_DEFINE_BOOL(pgr4_audio_compressor_probe, false, "PGR4",
                    "Development: the game's audio compressors' level and gain, in the log");
REX_EXTERN(__imp__sub_8280AA48);
REX_HOOK_RAW(sub_8280AA48) {
  const uint32_t dsp = ctx.r3.u32;
  __imp__sub_8280AA48(ctx, base);
  if (!REXCVAR_GET(pgr4_audio_compressor_probe)) return;
  struct Seen {
    float rms, threshold, ratio, attack, release, gain, min_gain;
  };
  static std::mutex mutex;
  static std::map<uint32_t, Seen> seen;
  static uint64_t dumped = 0;
  const uint8_t* d = base + dsp;
  std::lock_guard<std::mutex> lock(mutex);
  auto [it, added] = seen.try_emplace(dsp);
  const float gain = BeF(d + 72);
  it->second = {BeF(d + 48), BeF(d + 60), BeF(d + 64), BeF(d + 52), BeF(d + 56), gain,
                added ? gain : std::min(it->second.min_gain, gain)};
  if (g_frame_no < dumped + 30) return;
  dumped = g_frame_no;
  std::string line;
  for (const auto& [m, s] : seen)
    line += fmt::format(" {:08X}: rms {:.3f} thr {:.3f} ratio {:.3f} att {:.3f} rel {:.3f} gain "
                        "{:.3f} (min {:.3f}) |",
                        m, s.rms, s.threshold, s.ratio, s.attack, s.release, s.gain, s.min_gain);
  REXLOG_INFO("PGR4 audio compressors (frame {}):{}", g_frame_no, line);
  seen.clear();
}

// pgr4_audio_mixer_probe: the mixers (sub_828387A8, sub_828391F8, sub_82839B80, sub_8283A3C0)
// and voices (sub_82806450) the audio thread runs, by their object (r3): every half second, the
// first 512 bytes of each read as floats, and the ones that held still over the two previous
// reads then moved by a factor of 1.6 or more (volumes falling or rising) logged.
REXCVAR_DEFINE_BOOL(pgr4_audio_mixer_probe, false, "PGR4",
                    "Development: sudden changes in the audio mixers' and voices' fields, in the log");
namespace {
constexpr uint32_t kProbeBytes = 512;
struct MixerSnapshot {
  std::array<float, kProbeBytes / 4> before{}, last{};
  uint32_t reads = 0;
};
std::mutex g_mixer_mutex;
std::map<uint32_t, char> g_mixer_kinds;  // object -> 'm' mixer, 'v' voice
std::map<uint32_t, MixerSnapshot> g_mixer_snapshots;
std::chrono::steady_clock::time_point g_mixer_read;

void NoteMixer(uint8_t* base, uint32_t object, char kind) {
  if (!REXCVAR_GET(pgr4_audio_mixer_probe) || object < 0x40000000) return;
  std::lock_guard<std::mutex> lock(g_mixer_mutex);
  g_mixer_kinds[object] = kind;
  const auto now = std::chrono::steady_clock::now();
  if (now - g_mixer_read < std::chrono::milliseconds(500)) return;
  g_mixer_read = now;
  // The voices' candidate volume fields (+100, +372, +376, +380, +500), every read.
  {
    std::string line;
    for (const auto& [obj, k] : g_mixer_kinds) {
      if (k != 'v') continue;
      const uint8_t* v = base + obj;
      line += fmt::format(" {:08X}: {:.3f} {:.3f} {:.3f} {:.3f} {:.3f} |", obj, BeF(v + 100),
                          BeF(v + 372), BeF(v + 376), BeF(v + 380), BeF(v + 500));
    }
    REXLOG_INFO("PGR4 audio voices (frame {}):{}", g_frame_no, line);
  }
  std::string changes;
  for (const auto& [obj, k] : g_mixer_kinds) {
    MixerSnapshot& s = g_mixer_snapshots[obj];
    std::array<float, kProbeBytes / 4> current;
    for (uint32_t i = 0; i < current.size(); ++i) current[i] = BeF(base + obj + 4 * i);
    if (s.reads >= 2) {
      for (uint32_t i = 0; i < current.size(); ++i) {
        const float a = s.last[i], b = current[i];
        const bool sane = std::isfinite(a) && std::isfinite(b) && std::fabs(a) <= 16.0f &&
                          std::fabs(b) <= 16.0f && std::max(std::fabs(a), std::fabs(b)) >= 0.05f;
        if (!sane || s.before[i] != a || a == b) continue;
        const float lo = std::min(std::fabs(a), std::fabs(b)), hi = std::max(std::fabs(a), std::fabs(b));
        if (lo * 1.6f > hi) continue;
        changes += fmt::format(" {}{:08X}+{}: {:.3f} -> {:.3f} |", k, obj, 4 * i, a, b);
      }
    }
    s.before = s.last;
    s.last = current;
    ++s.reads;
  }
  if (!changes.empty())
    REXLOG_INFO("PGR4 audio mixers (frame {}, {} objects):{}", g_frame_no, g_mixer_kinds.size(),
                changes.size() > 3000 ? changes.substr(0, 3000) + " ..." : changes);
  g_mixer_kinds.clear();
}
}  // namespace
REX_EXTERN(__imp__sub_828387A8);
REX_HOOK_RAW(sub_828387A8) {
  NoteMixer(base, ctx.r3.u32, 'm');
  __imp__sub_828387A8(ctx, base);
}
REX_EXTERN(__imp__sub_828391F8);
REX_HOOK_RAW(sub_828391F8) {
  NoteMixer(base, ctx.r3.u32, 'm');
  __imp__sub_828391F8(ctx, base);
}
REX_EXTERN(__imp__sub_82839B80);
REX_HOOK_RAW(sub_82839B80) {
  NoteMixer(base, ctx.r3.u32, 'm');
  __imp__sub_82839B80(ctx, base);
}
REX_EXTERN(__imp__sub_8283A3C0);
REX_HOOK_RAW(sub_8283A3C0) {
  NoteMixer(base, ctx.r3.u32, 'm');
  __imp__sub_8283A3C0(ctx, base);
}
REX_EXTERN(__imp__sub_82806450);
// pgr4_audio_voice_probe (15/11/2026, the kudos counter's tick inaudible though decoded): for
// 40 ms after the UI's HUD_Kudos_Up or HUD_Kudos_Bank (sub_826A03A8), every voice the audio
// thread runs whose object points at an XMA context (a word in its first 2 KB inside the
// context array, ContextArrayAddress, 64 bytes a context) logged with the context and its
// candidate gain fields; the first voices' 512 bytes dumped, to compare the two sounds.
REXCVAR_DEFINE_BOOL(pgr4_audio_voice_probe, false, "PGR4",
                    "Development: the voices playing the kudos sounds, in the log");
std::atomic<int64_t> g_ui_sound_at{0};    // steady_clock ns of the last kudos UI sound
std::atomic<uint32_t> g_ui_sound_index{0};
std::atomic<uint32_t> g_tick_xma{0};  // the XMA object of the last HUD_Kudos_Up's voice
REXCVAR_DEFINE_DOUBLE(pgr4_audio_tick_boost, 0.0, "PGR4",
                      "Development: the kudos tick voice's output multiplied by this (0 off)");
REX_HOOK_RAW(sub_82806450) {
  NoteMixer(base, ctx.r3.u32, 'v');
  // The tick's voice (its XMA object at +80 the one started right after HUD_Kudos_Up, g_tick_xma):
  // its state before the render and the 256 floats it rendered into r4.
  const uint32_t voice = ctx.r3.u32, out = ctx.r4.u32, params = ctx.r5.u32;
  bool trace = false;
  int64_t since = 0;
  std::string before;
  if (REXCVAR_GET(pgr4_audio_voice_probe) && g_tick_xma.load() &&
      GuestLoad32(voice + 80) == g_tick_xma.load()) {
    since = std::chrono::steady_clock::now().time_since_epoch().count() - g_ui_sound_at.load();
    static std::atomic<int> lines{0};
    trace = since >= 0 && since < 60'000'000 && lines.fetch_add(1) < 3000;
    if (trace) {
      const uint8_t* v = base + voice;
      before = fmt::format(
          "b28 {} b29 {} b36 {} b72 {} b73 {} b7C {} w50 {} w5C {} w90 {} wAC {} f64 {:.3f} "
          "vol {:.3f}..{:.3f} t {:.3f}",
          v[28], v[29], v[36], v[0x72], v[0x73], v[0x7C], GuestLoad32(voice + 0x50),
          GuestLoad32(voice + 0x5C), GuestLoad32(voice + 0x90), GuestLoad32(voice + 0xAC),
          BeF(v + 0x64), BeF(v + 0x68), BeF(v + 0x6C), params ? BeF(base + params + 12) : -1.0f);
    }
  }
  __imp__sub_82806450(ctx, base);
  // Development test (15/11/2026): the tick voice's rendered block multiplied, to tell a tick too
  // quiet from one thrown away after its voice.
  if (const double boost = REXCVAR_GET(pgr4_audio_tick_boost);
      boost > 0 && g_tick_xma.load() && GuestLoad32(voice + 80) == g_tick_xma.load()) {
    for (uint32_t i = 0; i < 256; ++i)
      PutBeF(base + out + 4 * i,
             std::clamp(float(BeF(base + out + 4 * i) * boost), -1.0f, 1.0f));
  }
  if (trace) {
    double sum = 0, peak = 0;
    for (uint32_t i = 0; i < 256; ++i) {
      const double s = BeF(base + out + 4 * i);
      sum += s * s;
      peak = std::max(peak, std::abs(s));
    }
    const double rms = std::sqrt(sum / 256);
    REXLOG_INFO("PGR4 audio tick voice +{:.1f}ms {:08X}: {} -> ret {} out rms {:.0f} dB peak {:.0f} "
                "dB, after b36 {} b73 {} wAC {}",
                since / 1e6, voice, before, int32_t(ctx.r3.u32),
                rms > 0 ? 20 * std::log10(rms) : -99.0, peak > 0 ? 20 * std::log10(peak) : -99.0,
                base[voice + 36], base[voice + 0x73], GuestLoad32(voice + 0xAC));
  }
}

// pgr4_audio_event_probe: the vehicle sound's event calls into the sound system: sub_82689A60
// (sound, event: method +112), sub_82689A20 (sound, index, f1: method +104), sub_8268EE28
// (vehicle sound, ?, index), each call logged with the game frame.
REXCVAR_DEFINE_BOOL(pgr4_audio_event_probe, false, "PGR4",
                    "Development: the vehicle sound's event calls, in the log");
REXCVAR_DEFINE_DOUBLE(pgr4_audio_max_wind_speed, 0.0, "PGR4",
                      "Development: cap on the speed (m/s) the wind sound gets; 0 none");
REX_EXTERN(__imp__sub_82689A60);
REX_HOOK_RAW(sub_82689A60) {
  if (REXCVAR_GET(pgr4_audio_event_probe))
    REXLOG_INFO("PGR4 audio event A60 (frame {}): sound {:08X} event {}", g_frame_no,
                ctx.r3.u32, int32_t(ctx.r4.u32));
  __imp__sub_82689A60(ctx, base);
}
REX_EXTERN(__imp__sub_82689A20);
REX_HOOK_RAW(sub_82689A20) {
  // Development: cap on the value of index 0 (the speed, on the wind sound) passed this way.
  if (const double cap = REXCVAR_GET(pgr4_audio_max_wind_speed); cap > 0 && ctx.r4.u32 == 0)
    ctx.f1.f64 = std::min(ctx.f1.f64, cap);
  if (REXCVAR_GET(pgr4_audio_event_probe))
    REXLOG_INFO("PGR4 audio event A20 (frame {}): sound {:08X} index {} value {:.3f}", g_frame_no,
                ctx.r3.u32, int32_t(ctx.r4.u32), ctx.f1.f64);
  __imp__sub_82689A20(ctx, base);
}
REX_EXTERN(__imp__sub_8268EE28);
REX_HOOK_RAW(sub_8268EE28) {
  if (REXCVAR_GET(pgr4_audio_event_probe))
    REXLOG_INFO("PGR4 audio event E28 (frame {}): sound {:08X} r4 {} index {}", g_frame_no,
                ctx.r3.u32, int32_t(ctx.r4.u32), int32_t(ctx.r5.u32));
  __imp__sub_8268EE28(ctx, base);
}

// pgr4_audio_script_probe (15/11/2026, the kudos counter's tick inaudible): the sound scripts'
// interpreter, sub_82830998(script, instance): one instruction a call (sub_82831158 loops while
// the state's +264 is set). State at *(script+16) + *(instance+44): float stack from +0, program
// counter +256, stack height +260, compare flags +265/+266/+267; code at *(script+20) +
// *(*(script+8)+12), its length at *(*(script+12) + *(*(script+8)+12) + 8). Each instruction
// run within 40 ms of a kudos UI sound logged with its operand and the stack after it.
REXCVAR_DEFINE_BOOL(pgr4_audio_script_probe, false, "PGR4",
                    "Development: the sound scripts' instructions after a kudos sound, in the log");
extern std::atomic<int64_t> g_ui_sound_at;
REX_EXTERN(__imp__sub_82830998);
REX_EXTERN(sub_827E32F8);  // a sound component's value (object, component, instance state, 0)
REX_HOOK_RAW(sub_82830998) {
  bool trace = false;
  uint32_t state = 0, code = 0, pc = 0, script = ctx.r3.u32, instance = ctx.r4.u32;
  if (REXCVAR_GET(pgr4_audio_script_probe)) {
    const int64_t since =
        std::chrono::steady_clock::now().time_since_epoch().count() - g_ui_sound_at.load();
    static std::atomic<int> lines{0};
    if (since >= 0 && since < 40'000'000 && lines.load() < 4000) {
      ++lines;
      trace = true;
      const uint32_t shared = GuestLoad32(GuestLoad32(script + 8) + 12);
      state = GuestLoad32(script + 16) + GuestLoad32(instance + 44);
      code = GuestLoad32(script + 20) + shared;
      pc = GuestLoad32(state + 256);
    }
  }
  __imp__sub_82830998(ctx, base);
  if (trace && GuestLoad32(code + 4 * pc) == 0x1000 && GuestLoad32(code + 4 * pc + 4) == 0x65) {
    // HUD_Kudos_UpScript just played HUD_Kudos_UpSound (UI.dat component 101): the values of
    // the components around it, read as the interpreter's op 0x1001 does (sub_827E32F8(object,
    // component, instance state, 0) -> f1).
    const PPCContext saved = ctx;
    std::string values;
    for (uint32_t component : {52u, 97u, 98u, 99u, 100u, 101u, 10u, 163u}) {
      ctx.r3.u64 = GuestLoad32(script + 8);
      ctx.r4.u64 = component;
      ctx.r5.u64 = GuestLoad32(instance + 44);
      ctx.r6.u64 = 0;
      sub_827E32F8(ctx, base);
      values += fmt::format(" {}={:.3f}", component, ctx.f1.f64);
    }
    ctx = saved;
    REXLOG_INFO("PGR4 audio script values after HUD_Kudos_UpSound:{}", values);
  }
  if (trace) {
    const uint32_t sp = GuestLoad32(state + 260);
    std::string stack;
    for (uint32_t i = 0; i < std::min(sp, 8u); ++i)
      stack += fmt::format(" {:.3f}", BeF(base + state + 4 * i));
    REXLOG_INFO("PGR4 audio script {:08X}/{:08X} pc {}: op {:04X} {:08X} -> pc {} sp {} [{} ] flags "
                "{}{}{} run {}",
                script, instance, pc, GuestLoad32(code + 4 * pc), GuestLoad32(code + 4 * pc + 4),
                GuestLoad32(state + 256), sp, stack, base[state + 265], base[state + 266],
                base[state + 267], base[state + 264]);
  }
}

// pgr4_audio_xaudio_probe (15/11/2026, the kudos tick): the title's XAudio source voice step,
// sub_8291AD10(voice): its XMA context at +0xB8 (sub_829146D8, sub_82915028), flags at +0xAC (1
// playing?, 2 start, 4 ramp, 0x10), outputs +0x38, volume +0x70, ramp count +0xAE, state +0x124.
// Each call within 40 ms of a kudos UI sound logged before and after, with its return.
REXCVAR_DEFINE_BOOL(pgr4_audio_xaudio_probe, false, "PGR4",
                    "Development: the XAudio voices stepped after a kudos sound, in the log");
REX_EXTERN(__imp__sub_8291AD10);
REX_HOOK_RAW(sub_8291AD10) {
  bool trace = false;
  const uint32_t voice = ctx.r3.u32;
  std::string before;
  if (REXCVAR_GET(pgr4_audio_xaudio_probe)) {
    const int64_t since =
        std::chrono::steady_clock::now().time_since_epoch().count() - g_ui_sound_at.load();
    static std::atomic<int> lines{0};
    if (since >= 0 && since < 40'000'000 && lines.load() < 6000) {
      ++lines;
      trace = true;
      before = fmt::format("+{}ms ctx {:08X} flags {:02X} outs {} vol {:.3f} ramp {} state {}",
                           since / 1000000, GuestLoad32(voice + 0xB8), base[voice + 0xAC],
                           base[voice + 0x38], BeF(base + voice + 0x70),
                           uint16_t(base[voice + 0xAE] << 8 | base[voice + 0xAF]),
                           base[voice + 0x124]);
    }
  }
  __imp__sub_8291AD10(ctx, base);
  if (trace)
    REXLOG_INFO("PGR4 audio xaudio {:08X}: {} -> ret {} flags {:02X} vol {:.3f} state {}", voice,
                before, int32_t(ctx.r3.u32), base[voice + 0xAC], BeF(base + voice + 0x70),
                base[voice + 0x124]);
}

// pgr4_audio_xaudio_probe, continued: the title's XMA helpers (r3 the context) and the XAudio
// voice methods (vtable 0x82246458, r3 the voice, its context at +0xB8), each call within 40 ms
// of a kudos UI sound logged with its caller (lr) and return.
namespace {
void NoteXAudioCall(const char* name, uint32_t fn, PPCContext& ctx, uint8_t* base,
                    void (*call)(PPCContext&, uint8_t*)) {
  bool trace = false;
  int64_t since = 0;
  const uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, lr = uint32_t(ctx.lr);
  if ((REXCVAR_GET(pgr4_audio_voice_probe) || REXCVAR_GET(pgr4_audio_tick_boost) > 0) &&
      fn == 0x82914C58 && lr == 0x8280634C &&
      g_ui_sound_index.load() == 5) {
    // A voice started (sub_82806180) within 15 ms of HUD_Kudos_Up: the tick's.
    const int64_t s =
        std::chrono::steady_clock::now().time_since_epoch().count() - g_ui_sound_at.load();
    if (s >= 0 && s < 15'000'000 && g_tick_xma.exchange(r3) != r3)
      REXLOG_INFO("PGR4 audio tick voice: XMA object {:08X}", r3);
  }
  if (REXCVAR_GET(pgr4_audio_xaudio_probe)) {
    since = std::chrono::steady_clock::now().time_since_epoch().count() - g_ui_sound_at.load();
    static std::atomic<int> lines{0};
    trace = since >= 0 && since < 40'000'000 && lines.fetch_add(1) < 8000;
  }
  call(ctx, base);
  if (trace) {
    const bool voice = fn >= 0x82916000;
    REXLOG_INFO("PGR4 audio xcall +{:.1f}ms {} {:08X}({:08X}, {:08X}){} from {:08X} -> {:08X}",
                since / 1e6, name, fn, r3, r4,
                voice ? fmt::format(" ctx {:08X} state {} flags {:02X}", GuestLoad32(r3 + 0xB8),
                                    base[r3 + 0x124], base[r3 + 0xAC])
                      : std::string(),
                lr, ctx.r3.u32);
  }
}
}  // namespace
#define PGR4_XAUDIO_PROBE(addr, name)                                   \
  REX_EXTERN(__imp__sub_##addr);                                        \
  REX_HOOK_RAW(sub_##addr) {                                            \
    NoteXAudioCall(name, 0x##addr, ctx, base, &__imp__sub_##addr);      \
  }
PGR4_XAUDIO_PROBE(82915170, "xma kick?")
PGR4_XAUDIO_PROBE(829150A8, "xma 50A8")
PGR4_XAUDIO_PROBE(82915028, "xma 5028")
PGR4_XAUDIO_PROBE(829146D8, "xma 46D8")
PGR4_XAUDIO_PROBE(82914C58, "xma 4C58")
PGR4_XAUDIO_PROBE(82914E78, "xma 4E78")
PGR4_XAUDIO_PROBE(829152C0, "xma 52C0")
PGR4_XAUDIO_PROBE(829197E0, "voice stop")
PGR4_XAUDIO_PROBE(829198E0, "voice state")
PGR4_XAUDIO_PROBE(82919A30, "voice 9A30")
PGR4_XAUDIO_PROBE(82919B60, "voice 9B60")
PGR4_XAUDIO_PROBE(82919C78, "voice 9C78")
PGR4_XAUDIO_PROBE(8291A560, "voice A560")
PGR4_XAUDIO_PROBE(8291A7E8, "voice A7E8")
PGR4_XAUDIO_PROBE(8291A9A0, "voice A9A0")
PGR4_XAUDIO_PROBE(8291ABA0, "voice ABA0")
PGR4_XAUDIO_PROBE(8291B790, "voice B790")
PGR4_XAUDIO_PROBE(829195F0, "voice 95F0")
#undef PGR4_XAUDIO_PROBE

// pgr4_audio_ui_probe (15/11/2026, a player's kudos without sound): the UI sound object's events
// (GAME:\game\audio\UI.dat, registered by name in sub_826A0870 through sub_826A0770, the handle
// at object + (index + 0x111) * 4), played by sub_826A03A8(object, index) when the object is on
// (+0x14), loaded (+0x440) and the event's handle is not 0.
REXCVAR_DEFINE_BOOL(pgr4_audio_ui_probe, false, "PGR4",
                    "Development: the UI sound events the game plays (kudos, countdown), in the log");
// Local change (15/11/2026, Tazz): the kudos counter's tick (HUD_Kudos_Up, index 5) at the
// console's 30 FPS pace. The HUD's counter steps once per so many game frames, and the game runs
// at the real frame rate (up to the screen's: 165 Hz and more), so it asked for its tick
// rate / 30 times too often. Each request adds 30 x the frame's duration; a tick plays when that
// reaches 1: every request at 30 FPS, one in two at 60, about one in 5.5 at 165.
REXCVAR_DEFINE_BOOL(pgr4_kudos_tick_30fps_pace, true, "PGR4",
                    "The kudos counter's tick played at the console's 30 FPS pace at any frame rate");
REX_EXTERN(__imp__sub_826A03A8);
REX_HOOK_RAW(sub_826A03A8) {
  if (ctx.r4.u32 == 5 && REXCVAR_GET(pgr4_kudos_tick_30fps_pace)) {
    static double credit = 1.0;
    credit = std::min(credit + std::clamp(double(Pgr4FrameSeconds()) * 30.0, 0.0, 1.0), 2.0);
    if (credit < 1.0) return;
    credit -= 1.0;
  }
  if (REXCVAR_GET(pgr4_audio_ui_probe)) {
    static const char* const kNames[] = {
        "UI_Move", "UI_Confirm", "UI_Back", "HUD_Countdown", "HUD_Go", "HUD_Kudos_Up",
        "HUD_Kudos_Bank", "HUD_Kudos_Lose", "KudosCircleComplete", "KudosCircleComplete5",
        "KudosWordsSwoosh", "KudosStarSprayStart", "KudosStarSprayStop", "TimeSheetPopUp",
        "TwoDCrowd", "KudosStarSpray", "KudosAwardText", "ConeHitPositive", "CheckPointPassed",
        "ConeHitNegative", "EliminationWarning", "Eliminated", "GateOff", "FastestLap",
        "KudosChallengeTargetAchieved", "SpeedChallengePassCamera", "TimeKudosTimerStart",
        "TimeKudosTimerStop", "TestTrackTopSpeed", "TestTrackFigure8Drift", "NetSurvivorBeenHit",
        "NetSurvivorHitting", "CoOpKudosMode", "CoOpKudosCarsLocked", "MenuMovement",
        "CarPurchasedOrUnlocked", "AwardsGiven", "MedalsAchieved", "TrophiesAchieved",
        "UIPauseActionIn", "UIResumeActionIn", "PostRaceWinStart", "PostRaceWinStop",
        "PostRaceLoseStart", "PostRaceLoseStop", "LoadingScreenStart", "LoadingScreenStop",
        "EnteringUISection", "LeavingUISection", "UIPhotoClickActionIn",
        "UIMissedConeGateActionIn", "RaceEnd", "EnableMusic", "DisableMusic", "CalanderRedOut",
        "LoadingScreenPanelOut", "LoadingScreenPanelIn", "TodaysEventCircle", "CongratsTrophy",
        "FailedMajor", "UI_Move2", "CongratsTrophyExit", "FailedMajorExit"};
    const uint32_t object = ctx.r3.u32, index = ctx.r4.u32;
    if (index == 5 || index == 6) {
      g_ui_sound_index = index;
      g_ui_sound_at = std::chrono::steady_clock::now().time_since_epoch().count();
    }
    const uint8_t on = *rex::system::kernel_memory()->TranslateVirtual<const uint8_t*>(object + 0x14);
    REXLOG_INFO("PGR4 audio ui (frame {}): {} ({}) on {} loaded {:08X} handle {:08X}", g_frame_no,
                index < std::size(kNames) ? kNames[index] : "?", index, on,
                GuestLoad32(object + 0x440), GuestLoad32(object + (index + 0x111) * 4));
  }
  __imp__sub_826A03A8(ctx, base);
}

// pgr4_audio_queue_probe: the sound system's message queue (sub_82807E30(queue, type, data,
// bytes) posts, 0 when the ring is full and the message dropped): every half second, the posts,
// the drops, the ring's capacity (+4, words) and fill (+16 write - +12 read).
REXCVAR_DEFINE_BOOL(pgr4_audio_queue_probe, false, "PGR4",
                    "Development: the audio message queue's posts, drops and fill, in the log");
REX_EXTERN(__imp__sub_82807E30);
REX_HOOK_RAW(sub_82807E30) {
  const uint32_t queue = ctx.r3.u32;
  __imp__sub_82807E30(ctx, base);
  if (!REXCVAR_GET(pgr4_audio_queue_probe)) return;
  static std::mutex mutex;
  static std::map<uint32_t, std::pair<uint32_t, uint32_t>> counts;  // queue -> posts, drops
  static uint32_t max_fill = 0;
  static std::chrono::steady_clock::time_point logged;
  std::lock_guard<std::mutex> lock(mutex);
  auto& [posts, drops] = counts[queue];
  ++posts;
  drops += ctx.r3.u32 == 0;
  const uint32_t capacity = Be32(base, queue + 4), read = Be32(base, queue + 12),
                 write = Be32(base, queue + 16);
  const uint32_t fill = write >= read ? write - read : capacity - read + write;
  max_fill = std::max(max_fill, fill);
  const auto now = std::chrono::steady_clock::now();
  if (now - logged < std::chrono::milliseconds(500)) return;
  logged = now;
  std::string line;
  for (const auto& [q, c] : counts) line += fmt::format(" {:08X}: posts {} drops {} |", q, c.first, c.second);
  REXLOG_INFO("PGR4 audio queue (frame {}): fill {} max {} of {} words:{}", g_frame_no, fill,
              max_fill, capacity, line);
  counts.clear();
  max_fill = 0;
}

REXCVAR_DEFINE_BOOL(pgr4_audio_debug, false, "PGR4",
                    "Development: the game's own audio debug displays, on during races");
// The sound graphs' Gradient component (sub_827E33B0(component, instance)): the change of an input
// (RPMGradient, ThrottleGradient, GroundSpeedGradient...) since the last audio update, times the
// update rate (u16 at instance +12) and a constant. Its state is at component +16 plus instance +44:
// +0 the last value, +4 the gradient; component +4 is the name.
// pgr4_audio_gradient_probe logs each one's value and gradient range every 30 game frames;
// pgr4_audio_gradient_zero forces the gradients to 0 (development tests).
REXCVAR_DEFINE_BOOL(pgr4_audio_gradient_probe, false, "PGR4",
                    "Development: the sound graphs' gradients (RPM, throttle, speed), in the log");
REXCVAR_DEFINE_BOOL(pgr4_audio_gradient_zero, false, "PGR4",
                    "Development: the sound graphs' gradients forced to 0");
REX_EXTERN(__imp__sub_827E33B0);
REX_HOOK_RAW(sub_827E33B0) {
  const uint32_t component = ctx.r3.u32, instance = ctx.r4.u32;
  __imp__sub_827E33B0(ctx, base);
  const bool probe = REXCVAR_GET(pgr4_audio_gradient_probe), zero = REXCVAR_GET(pgr4_audio_gradient_zero);
  if (!probe && !zero) return;
  auto be32 = [&](uint32_t a) { return __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + a)); };
  uint8_t* state = base + be32(component + 16) + be32(instance + 44);
  if (zero) PutBeF(state + 4, 0.0f);
  if (!probe) return;
  // The instance's whole state block (instance +44, the components' outputs), every 100 ms: the
  // floats that moved by a factor of 2 or more since the last read, with their offset.
  {
    static std::mutex block_mutex;
    static std::map<uint32_t, std::vector<float>> blocks;
    static std::chrono::steady_clock::time_point read;
    std::lock_guard<std::mutex> lock(block_mutex);
    const auto now = std::chrono::steady_clock::now();
    if (now - read >= std::chrono::milliseconds(100)) {
      read = now;
      constexpr uint32_t kBlockBytes = 0x2000;
      const uint32_t block = be32(instance + 44);
      std::vector<float> current(kBlockBytes / 4);
      for (uint32_t i = 0; i < current.size(); ++i) current[i] = BeF(base + block + 4 * i);
      // The raw block too, for offline study: a record per read of the wall-clock time (ms since
      // the epoch, u64), the instance (u32), then the 8 KB as the guest has them (big-endian).
      {
        static std::ofstream dump("audio_state.bin", std::ios::binary | std::ios::trunc);
        const uint64_t ms = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::system_clock::now().time_since_epoch())
                                         .count());
        dump.write(reinterpret_cast<const char*>(&ms), 8);
        dump.write(reinterpret_cast<const char*>(&instance), 4);
        dump.write(reinterpret_cast<const char*>(base + block), kBlockBytes);
        dump.flush();
      }
      std::vector<float>& last = blocks[instance];
      std::string changes;
      for (uint32_t i = 0; i < current.size() && !last.empty(); ++i) {
        const float a = last[i], b = current[i];
        if (!std::isfinite(a) || !std::isfinite(b) || std::fabs(a) > 1e4f || std::fabs(b) > 1e4f)
          continue;
        const float lo = std::min(std::fabs(a), std::fabs(b)), hi = std::max(std::fabs(a), std::fabs(b));
        if (hi < 0.05f || lo * 2.0f > hi) continue;
        changes += fmt::format(" +{:X}: {:.3f}->{:.3f}", 4 * i, a, b);
      }
      last = std::move(current);
      if (!changes.empty())
        REXLOG_INFO("PGR4 audio state {:08X} (block {:08X}):{}", instance, block,
                    changes.size() > 4000 ? changes.substr(0, 4000) + " ..." : changes);
    }
  }
  struct Range { std::string name; uint32_t rate = 0, calls = 0; float value = 0, lo = 1e30f, hi = -1e30f; };
  static std::mutex mutex;
  static std::map<std::pair<uint32_t, uint32_t>, Range> ranges;
  static uint64_t dumped = 0;
  std::lock_guard<std::mutex> lock(mutex);
  Range& r = ranges[{instance, component}];
  if (r.name.empty()) r.name = reinterpret_cast<const char*>(base + be32(component + 4));
  r.rate = *reinterpret_cast<uint16_t*>(base + instance + 12);
  r.rate = uint16_t((r.rate >> 8) | (r.rate << 8));
  r.value = BeF(state);
  const float g = BeF(state + 4);
  r.lo = std::min(r.lo, g);
  r.hi = std::max(r.hi, g);
  ++r.calls;
  if (g_frame_no >= dumped + 30) {
    dumped = g_frame_no;
    for (const auto& [key, e] : ranges)
      REXLOG_INFO("PGR4 audio gradient (frame {}) {:08X} {}: value {:.3f} gradient {:.3f}..{:.3f} "
                  "rate {} calls {}",
                  g_frame_no, key.first, e.name, e.value, e.lo, e.hi, e.rate, e.calls);
    ranges.clear();
  }
}

REXCVAR_DEFINE_BOOL(pgr4_audio_disable_reverb, false, "PGR4",
                    "Development: the game's Audio.DisableReverb on during races");
REXCVAR_DEFINE_DOUBLE(pgr4_audio_max_revs, 0.0, "PGR4",
                      "Development: cap on the revs (0..1) the vehicle sound gets; 0 none");
REXCVAR_DEFINE_DOUBLE(pgr4_audio_revs_jitter, 0.0, "PGR4",
                      "Development: +- wobble on the revs the vehicle sound gets; 0 none");
REXCVAR_DEFINE_DOUBLE(pgr4_audio_max_speed, 0.0, "PGR4",
                      "Development: cap on the speed (m/s) the vehicle sound gets; 0 none");
REX_EXTERN(__imp__sub_8268ED20);
REX_HOOK_RAW(sub_8268ED20) {
  // Development: the game's Audio.DisableReverb (0x82C94CFC, flag at +32), read by the mix
  // (sub_82801120).
  if (REXCVAR_GET(pgr4_audio_disable_reverb)) base[0x82C94CFC + 32] = 1;
  if (REXCVAR_GET(pgr4_audio_debug)) {
    // Once: the global sound system (0x82C89A4C) and its methods, for the message queue's code.
    static bool listed = false;
    if (!std::exchange(listed, true)) {
      const uint32_t system = __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + 0x82C89A4C));
      const uint32_t vtable = system ? __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + system)) : 0;
      std::string methods;
      for (uint32_t i = 0; vtable && i < 48; ++i)
        methods += fmt::format(" +{}:{:08X}", 4 * i,
                               __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + vtable + 4 * i)));
      REXLOG_INFO("PGR4 audio system {:08X} vtable {:08X}:{}", system, vtable, methods);
    }
    // The game's audio debug tweaks (flag at +32). Only the system panel
    // (System.ShowProfileBarsAndSystemStats, 0x82C89A54: instances, muted instances, generators,
    // update times) draws in the retail build. The others (Audio.RenderDebug 0x82A84F90,
    // Components.ShowGradientVals 0x82AB4BA0, Cars.Show* 0x82C89D20.., Bikes.Show* 0x82C89AAC..)
    // show nothing, and the per-object panels' trigger (ObjectDebug.InitiateCarDebug 0x82C89F3C,
    // InitiateBikeDebug 0x82C89B3C) crashes the game at the race start.
    base[0x82A84F90 + 32] = 1;  // Audio.RenderDebug, which the panel needs
    base[0x82C89A54 + 32] = 1;
  }
  // Development caps on the parameters the vehicle sound gets: revs (index 0, 0..1) and speed
  // (index 11, m/s); 0 leaves them alone.
  if (const double cap = REXCVAR_GET(pgr4_audio_max_revs); cap > 0 && ctx.r5.u32 == 0)
    ctx.f1.f64 = std::min(ctx.f1.f64, cap);
  // Development: the revs moved by +-jitter on alternate frames (steady revs as the trigger?).
  if (const double jitter = REXCVAR_GET(pgr4_audio_revs_jitter); jitter > 0 && ctx.r5.u32 == 0)
    ctx.f1.f64 = std::max(0.0, ctx.f1.f64 + ((g_frame_no & 1) ? jitter : -jitter));
  if (const double cap = REXCVAR_GET(pgr4_audio_max_speed); cap > 0 && ctx.r5.u32 == 11)
    ctx.f1.f64 = std::min(ctx.f1.f64, cap);
  if (const int32_t resampler = REXCVAR_GET(pgr4_audio_resampler); resampler >= 0) {
    uint8_t& flag = base[0x82C9517C + 32];
    static bool logged = false;
    if (!std::exchange(logged, true))
      REXLOG_INFO("PGR4 audio resampler: the game's {}, set {}", flag, resampler);
    flag = uint8_t(resampler);
  }
  // p25: the switch the car audio sends only when it flips (a ray cast into the scenery, hit
  // closer than 50 m -> 0, else 1, remembered at vehicle +288).
  if (REXCVAR_GET(pgr4_audio_param_probe) && ctx.r5.u32 == 25)
    REXLOG_INFO("PGR4 audio p25 (frame {}): sound {:08X} -> {:.1f}", g_frame_no, ctx.r3.u32,
                ctx.f1.f64);
  if (REXCVAR_GET(pgr4_audio_param_probe)) {
    static std::map<uint32_t, std::map<uint32_t, float>> values;
    static uint64_t dumped = 0;
    values[ctx.r3.u32][ctx.r5.u32] = float(ctx.f1.f64);
    if (g_frame_no >= dumped + 30) {
      dumped = g_frame_no;
      for (const auto& [sound, params] : values) {
        std::string line;
        for (const auto& [index, value] : params) line += fmt::format(" p{}={:.3f}", index, value);
        REXLOG_INFO("PGR4 audio params (frame {}) {:08X}:{}", g_frame_no, sound, line);
      }
      values.clear();
      // Camera 0 (0x82C91D00, the listener): its step's displacement (+208) over the time step
      // (+284), the speed the audio takes from it (sub_827D4F88), and its interpolation (+272,
      // +276: the update's fraction and time).
      const uint8_t* cam = base + 0x82C91D00;
      const float dx = BeF(cam + 208), dy = BeF(cam + 212), dz = BeF(cam + 216);
      const float dt = BeF(cam + 284), len = std::sqrt(dx * dx + dy * dy + dz * dz);
      REXLOG_INFO("PGR4 audio camera: step {:.3f} m / dt {:.5f} s = {:.2f} m/s (update {:.3f} "
                  "{:.5f}, +280 {:.5f})",
                  len, dt, dt > 0 ? len / dt : 0.0f, BeF(cam + 272), BeF(cam + 276),
                  BeF(cam + 280));
    }
  }
  __imp__sub_8268ED20(ctx, base);
}

REX_EXTERN(__imp__sub_825D5140);
REX_HOOK_RAW(sub_825D5140) {
  if (REXCVAR_GET(pgr4_net_start_probe)) REXLOG_INFO("PGR4 race loaded state set (SetRaceLoadedState)");
  __imp__sub_825D5140(ctx, base);
}

// The music's volume. PGR4 plays its music through the system's music player (XMP playlists) and,
// on the console, captures the player's output (XMPCaptureOutput) to mix it in its own engine; it
// never calls XMPSetVolume. The SDK, as Xenia Canary, leaves the capture off (it muted some titles):
// the music went straight to the speakers at a fixed volume. The game drives that captured voice's
// volume through its music player's SetVolume(float) (sub_827FFBB8, a voice parameter set by
// sub_82913C48), called by the sound manager (vtable slot 39, sub_827E3190) with the profile's
// "Music volume" or "Menu music volume", its fades and its ducking: the same value is given to the
// system's music player, after the game's own call (the idea of beatrixzy's PGR4-Recomp,
// src/music_volume_hook.cpp; 12/11/2026). Replaces reading the profile's settings each frame.
REXCVAR_DEFINE_BOOL(pgr4_music_volume, true, "PGR4",
                    "The game's music volume (its settings, fades and ducking) applied to the "
                    "music player");
REX_EXTERN(__imp__sub_827FFBB8);
REX_HOOK_RAW(sub_827FFBB8) {
  const float asked = float(ctx.f1.f64);
  const float volume = std::clamp(asked, 0.0f, 1.0f);
  // Investigation (14/11/2026: the music very low in a race at 5 in both settings): every value the
  // game asks for and who asks (the sound manager's caller), the first 400 changes.
  {
    static float last = -1.0f;
    static uint32_t last_lr = 0, lines = 0;
    const uint32_t lr = uint32_t(ctx.lr);
    if ((asked != last || lr != last_lr) && lines < 400 && !REXCVAR_GET(pgr4_native_capture_dir).empty()) {
      ++lines;
      REXLOG_INFO("PGR4 music volume asked: {:.3f} from {:08X} (frame {})", asked, lr, g_frame_no);
    }
    last = asked;
    last_lr = lr;
  }
  __imp__sub_827FFBB8(ctx, base);
  if (!REXCVAR_GET(pgr4_music_volume)) return;
  auto* runtime = rex::system::kernel_state()->emulator();
  auto* audio = runtime ? static_cast<rex::audio::AudioSystem*>(runtime->audio_system()) : nullptr;
  rex::audio::AudioMediaPlayer* player = audio ? audio->media_player() : nullptr;
  if (!player) {
    static bool warned = false;
    if (!warned) REXLOG_WARN("PGR4 music volume: no music player");
    warned = true;
    return;
  }
  // The title's volume (AudioMediaPlayer::SetTitleVolume, local SDK change, 14/11/2026), which a
  // change of playback controller does not put back to another controller's: as XMP's own volume it
  // was (the race's music quieter than the options' preview at the same setting).
  if (player->GetTitleVolume() != volume) player->SetTitleVolume(volume);
}

// The music the game captures (XMPCaptureOutput, local SDK change, 14/11/2026): its callback,
// sub_828008B0, submits the block to its music voice (sub_82913DC0, return 0x82800930) only when the
// voice's state says it is ready (bit 0x20 of sub_82913D68's state), and drops it otherwise. A block
// not submitted goes back to the player (AudioMediaPlayer::ReturnCaptureBlock), which hands it again:
// the voice sets the pace, as on the console, and nothing is lost.
namespace {
thread_local bool t_music_block_submitted = false;
}  // namespace
REX_EXTERN(__imp__sub_82913DC0);
REX_HOOK_RAW(sub_82913DC0) {
  if (uint32_t(ctx.lr) == 0x82800930) t_music_block_submitted = true;
  __imp__sub_82913DC0(ctx, base);
}
REX_EXTERN(__imp__sub_828008B0);
REX_HOOK_RAW(sub_828008B0) {
  t_music_block_submitted = false;
  __imp__sub_828008B0(ctx, base);
  // The voice's pace, logged every 5 s in development (capture dir set): blocks taken and refused a
  // second (48 kHz float stereo, 256 frames a block: 187.5 taken a second when the format is the
  // voice's; measured so on 14/11/2026).
  if (!REXCVAR_GET(pgr4_native_capture_dir).empty()) {
    static uint32_t taken = 0, refused = 0;
    static auto since = std::chrono::steady_clock::now();
    (t_music_block_submitted ? taken : refused)++;
    const auto now = std::chrono::steady_clock::now();
    if (const double s = std::chrono::duration<double>(now - since).count(); s >= 5.0) {
      REXLOG_INFO("PGR4 music capture: {:.1f} blocks taken a second, {:.1f} refused", taken / s, refused / s);
      taken = refused = 0;
      since = now;
    }
  }
  if (t_music_block_submitted) return;
  auto* runtime = rex::system::kernel_state()->emulator();
  auto* audio = runtime ? static_cast<rex::audio::AudioSystem*>(runtime->audio_system()) : nullptr;
  if (rex::audio::AudioMediaPlayer* player = audio ? audio->media_player() : nullptr)
    player->ReturnCaptureBlock();
}

// The game's language follows the launcher's. PGR4 keeps the language chosen in its Options >
// General in the profile (title setting 0x63E83FFF, written by sub_825BC6C0) and, at start
// (sub_825BC5B0), puts it back over the console's: the launcher's choice (user_language and
// user_country in pgr4.toml) only ever applied to a new profile. The saved one is now left out, so
// the console's language, the launcher's, applies; a language chosen in the game (UIGameData.
// SetLanguage, sub_825C2200) is written back to pgr4.toml, so the launcher shows it and the next
// start keeps it. The game's languages are its loc\ folders, by code (the language table at
// 0x82A4228C: +0 its entries, 92 bytes each starting with the code, +8 the current one).
REXCVAR_DEFINE_BOOL(pgr4_launcher_language, true, "PGR4",
                    "The game's language is the launcher's (user_language), not the one saved in "
                    "the profile; a language chosen in the game is written back")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REX_EXTERN(__imp__sub_825BC5B0);
REX_HOOK_RAW(sub_825BC5B0) {
  if (!REXCVAR_GET(pgr4_launcher_language)) {
    __imp__sub_825BC5B0(ctx, base);
    return;
  }
  ctx.r3.u64 = 0;  // no language in the profile
}
namespace {
void WriteTomlSettingTo(const std::filesystem::path& path, const std::string& key,
                        const std::string& value);
// pgr4.toml beside the executable (and the project's copy, as the launcher): `key = value` set
// (added when missing), other lines kept.
void WriteTomlSetting(const std::string& key, const std::string& value) {
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::vector<std::filesystem::path> paths = {std::filesystem::path(exe).parent_path() /
                                              "pgr4.toml"};
#ifdef PGR4_SOURCE_TOML
  // Local change (31/10/2026): only from the build folder. A copy elsewhere (the desktop
  // release) wrote its own folders into the project's, which the next build copied back.
  if (std::error_code ec; std::filesystem::exists(PGR4_SOURCE_TOML) &&
      std::filesystem::equivalent(paths.front().parent_path(), PGR4_BUILD_DIR, ec))
    paths.push_back(PGR4_SOURCE_TOML);
#endif
  for (const std::filesystem::path& path : paths) WriteTomlSettingTo(path, key, value);
}
void WriteTomlSettingTo(const std::filesystem::path& path, const std::string& key,
                        const std::string& value) {
  std::vector<std::string> lines;
  {
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);) lines.push_back(line);
  }
  bool found = false;
  for (std::string& line : lines) {
    const size_t start = line.find_first_not_of(" \t");
    if (start == std::string::npos || line.compare(start, key.size(), key) != 0) continue;
    const size_t eq = line.find_first_not_of(" \t", start + key.size());
    if (eq == std::string::npos || line[eq] != '=') continue;
    line = key + " = " + value;
    found = true;
  }
  if (!found) lines.push_back(key + " = " + value);
  std::ofstream out(path, std::ios::trunc);
  for (const std::string& line : lines) out << line << '\n';
}
}  // namespace
REX_EXTERN(__imp__sub_825C2200);
REX_HOOK_RAW(sub_825C2200) {
  __imp__sub_825C2200(ctx, base);
  if (!REXCVAR_GET(pgr4_launcher_language)) return;
  constexpr uint32_t kLanguages = 0x82A4228C;
  const uint32_t entries = Be32(base, kLanguages), count = Be32(base, kLanguages + 4);
  const int32_t current = int32_t(Be32(base, kLanguages + 8));
  if (!entries || current < 0 || uint32_t(current) >= count) return;
  const std::string code(reinterpret_cast<const char*>(base + entries + uint32_t(current) * 92));
  // The launcher's values (src/launcher/launcher.cpp, kLanguages): Czech and Hungarian are English
  // on a console of their country.
  static const struct {
    const char* code;
    int language, country;
  } kCodes[] = {{"en", 1, 0},  {"fr", 4, 0},  {"de", 3, 0},  {"es", 5, 0},  {"it", 6, 0},
                {"cz", 1, 23}, {"hu", 1, 42}, {"pl", 11, 0}, {"ru", 12, 0}, {"ja", 2, 0},
                {"ko", 7, 0},  {"tc", 8, 0},  {"sc", 10, 0}};
  for (const auto& c : kCodes) {
    if (code != c.code) continue;
    WriteTomlSetting("user_language", std::to_string(c.language));
    const uint32_t country = REXCVAR_GET(user_country);
    if (c.country)
      WriteTomlSetting("user_country", std::to_string(c.country));
    else if (country == 23 || country == 42)
      WriteTomlSetting("user_country", "103");
    REXLOG_INFO("PGR4 language: {} chosen in the game, written to pgr4.toml", code);
    return;
  }
  REXLOG_WARN("PGR4 language: {} chosen in the game has no launcher language", code);
}

// --- Cars' projected sun shadow (local addition, after PGR4:ReGPU's car_shadow.cpp, MIT): only the
// blob under the car showed, not its 512x512 shadow map drawn onto the road (psCarShadow mode 0).
// The update's frustum test (sub_82437230, r3 the manager: cars at +0, their count at +40) sets
// car+324 when the car's shadow is in view; the renderer's per-view reset (sub_82437158, right
// before the shadow pass) clears it, so the pass (sub_82438418, r3 the manager: cars at +44, their
// count at +84) always read 0 and drew the blob. The update's result is kept and put back before
// the pass. pgr4_car_shadow_fix false (live): the game's behaviour, for comparison.
REXCVAR_DEFINE_BOOL(pgr4_car_shadow_fix, true, "PGR4",
                    "The cars' projected sun shadow on the road (the game's per-view reset hid it, "
                    "leaving only the blob under the car)");
namespace {
std::mutex g_car_shadow_mutex;
std::unordered_map<uint32_t, uint8_t> g_car_shadow_in_view;  // car -> car+324 after the update
}  // namespace
REX_EXTERN(__imp__sub_82437230);
REX_HOOK_RAW(sub_82437230) {
  const uint32_t mgr = ctx.r3.u32;
  __imp__sub_82437230(ctx, base);
  if (!REXCVAR_GET(pgr4_car_shadow_fix)) return;
  const uint32_t n = Be32(base, mgr + 40);
  std::lock_guard<std::mutex> lock(g_car_shadow_mutex);
  for (uint32_t i = 0; i < n && i < 16; ++i)
    if (const uint32_t car = Be32(base, mgr + i * 4)) g_car_shadow_in_view[car] = base[car + 324];
}
REX_EXTERN(__imp__sub_82438418);
REX_HOOK_RAW(sub_82438418) {
  if (REXCVAR_GET(pgr4_car_shadow_fix)) {
    const uint32_t mgr = ctx.r3.u32, n = Be32(base, mgr + 84);
    std::lock_guard<std::mutex> lock(g_car_shadow_mutex);
    for (uint32_t i = 0; i < n && i < 16; ++i)
      if (const uint32_t car = Be32(base, mgr + 44 + i * 4))
        if (const auto it = g_car_shadow_in_view.find(car);
            it != g_car_shadow_in_view.end() && it->second && !base[car + 324]) {
          base[car + 324] = 1;
          static bool logged = false;
          if (!logged) {
            logged = true;
            REXLOG_INFO("PGR4 car shadow: projected-shadow flag put back before the shadow pass");
          }
        }
  }
  __imp__sub_82438418(ctx, base);
}

// Local change (pgr4 project): the draw distance tests, back from docs/essais_distance_affichage for the
// overlay's tab (06/11/2026), all at the game's values by default.
extern std::atomic<int32_t> g_pgr4_world_max_lod;
extern std::atomic<int32_t> g_pgr4_force_world_max_lod;
// --- Scenery draw distance (local addition, back for a test): the view setup's cull preset
// (sub_823CA868(mode, view) copies 7 floats from its per-zone table to 0x82AA1CD0, [0] the
// instances' cull and LOD distance scale): the main view's (view 0) times pgr4_draw_distance; the
// mirror's, reflections' and cube's kept (their cost).
REXCVAR_DEFINE_DOUBLE(pgr4_draw_distance, 1.0, "PGR4",
                      "The main view's draw distance of the scenery's instances (props, trees): "
                      "culled and simplified this many times farther (1: the game's)");
constexpr uint32_t kViewCullScale = 0x82AA1CD0;
REX_EXTERN(__imp__sub_823CA868);
// Test: pgr4_draw_distance_mask, which of the 7 floats are scaled (bit i: float i; 1: the
// instances' only, as before). Each new preset (mode and its values) is logged.
REXCVAR_DEFINE_INT32(pgr4_draw_distance_mask, 1, "PGR4",
                     "Test: which of the view's 7 cull preset floats pgr4_draw_distance scales "
                     "(bit i: float i; 1: the instances' distance only)");
REX_HOOK_RAW(sub_823CA868) {
  const uint32_t mode = ctx.r3.u32, view = ctx.r4.u32;
  __imp__sub_823CA868(ctx, base);
  {
    static std::mutex mutex;
    static std::unordered_map<uint64_t, bool> seen;
    float v[7];
    for (int i = 0; i < 7; ++i) v[i] = BeF(base + kViewCullScale + 4 * i);
    uint64_t key = uint64_t(mode) << 32 | view;
    for (float f : v) key = key * 1000003u ^ std::hash<float>{}(f);
    std::lock_guard<std::mutex> lock(mutex);
    if (seen.size() < 64 && seen.emplace(key, true).second)
      REXLOG_INFO("PGR4 cull preset: mode {} view {}: {} {} {} {} {} {} {}", mode, view, v[0], v[1],
                  v[2], v[3], v[4], v[5], v[6]);
  }
  const double scale = REXCVAR_GET(pgr4_draw_distance);
  if (view != 0 || scale == 1.0) return;
  const uint32_t mask = uint32_t(REXCVAR_GET(pgr4_draw_distance_mask));
  for (int i = 0; i < 7; ++i)
    if (mask & (1u << i))
      PutBeF(base + kViewCullScale + 4 * i,
             BeF(base + kViewCullScale + 4 * i) * float(std::clamp(scale, 0.25, 8.0)));
}


// --- Precomputed visibility widened (local addition): the track is cut into PVS cells (813 in
// St Petersburg, numbered in no order along the track), each with the scenery tree's nodes seen
// from it (2 bits a node: visible, wholly visible). Objects such as the banners over the track
// popped in when the car entered the first cell listing them. sub_823CD430(world, &pos, &pos2)
// finds the camera's cell (sub_823CCD28 -> sub_823D3470(locator 0x82A1D220, &pos), the cell or
// -1) and sub_823CD238(world, cell) puts it into a cache slot (world+0xEFF0,
// up to 8, LRU), decoding it with sub_823CD090(world, node, &data, &bit cursor), a walk of the
// tree that sets or clears each node's two bits of that slot and goes into the children of the
// partly visible ones; the renderer tests the current slot's bits only. When a cell is decoded,
// the cells at points ahead of the camera (its motion's direction, every 20 m up to
// pgr4_pvs_ahead) are decoded into the same slot as well, keeping the bits already set: the slot
// holds the union, cached with it.
REXCVAR_DEFINE_DOUBLE(pgr4_pvs_ahead, 0.0, "PGR4",
                      "Precomputed visibility: the objects seen from the track this many metres "
                      "ahead of the camera drawn as well (0: the game's)");
namespace {
bool g_pvs_union = false;    // sub_823CD090 keeps the slot's bits already set
uint32_t g_pvs_decodes = 0;  // sub_823CD090 calls so far (a decode happened)
uint32_t g_pvs_nodes = 0, g_pvs_kept = 0, g_pvs_lit = 0;  // the union's nodes, for the log
float g_pvs_pos[3] = {}, g_pvs_dir[3] = {};
bool g_pvs_has_pos = false, g_pvs_has_dir = false;
constexpr uint32_t kPvsLocator = 0x82A1D220;
}  // namespace
REX_EXTERN(__imp__sub_823CD090);
REX_HOOK_RAW(sub_823CD090) {
  ++g_pvs_decodes;
  if (!g_pvs_union) {
    __imp__sub_823CD090(ctx, base);
    return;
  }
  const uint32_t world = ctx.r3.u32, node = ctx.r4.u32;
  const uint32_t slot = Be32(base, world + 0xEFF0);
  const uint32_t mask = Be32(base, node);
  const uint32_t bit = 2 * slot, half = mask + (bit >> 4) * 2;
  const uint16_t before = uint16_t(base[half] << 8 | base[half + 1]);
  __imp__sub_823CD090(ctx, base);
  // The two bits of this slot as the cells before left them, kept (a visible node stays so).
  const uint16_t keep = before & uint16_t(3u << (bit & 15));
  const uint16_t decoded = uint16_t(base[half] << 8 | base[half + 1]);
  const uint16_t after = decoded | keep;
  ++g_pvs_nodes;
  if ((decoded & ~keep) & uint16_t(3u << (bit & 15))) ++g_pvs_lit;
  if (keep & ~decoded) ++g_pvs_kept;
  base[half] = uint8_t(after >> 8);
  base[half + 1] = uint8_t(after);
}
// The camera's position each frame, its direction of motion from the last (kept while it stops).
REX_EXTERN(__imp__sub_823CD430);
REX_HOOK_RAW(sub_823CD430) {
  if (const uint32_t pos = ctx.r4.u32) {
    float p[3] = {BeF(base + pos), BeF(base + pos + 4), BeF(base + pos + 8)};
    if (g_pvs_has_pos) {
      const float d[3] = {p[0] - g_pvs_pos[0], p[1] - g_pvs_pos[1], p[2] - g_pvs_pos[2]};
      const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
      // Moving (not a camera cut): the direction, smoothed.
      if (len > 0.05f && len < 20.0f) {
        for (int i = 0; i < 3; ++i) g_pvs_dir[i] = g_pvs_dir[i] * 0.8f + d[i] / len * 0.2f;
        g_pvs_has_dir = true;
      }
    }
    std::memcpy(g_pvs_pos, p, sizeof(p));
    g_pvs_has_pos = true;
  }
  __imp__sub_823CD430(ctx, base);
}
REX_EXTERN(__imp__sub_823CD238);
REX_EXTERN(sub_823CD090);
REX_EXTERN(sub_823D3470);
REX_HOOK_RAW(sub_823CD238) {
  const uint32_t world = ctx.r3.u32;
  const int32_t cell = int32_t(ctx.r4.u32);
  const uint32_t decodes = g_pvs_decodes;
  __imp__sub_823CD238(ctx, base);
  const double ahead = std::clamp(REXCVAR_GET(pgr4_pvs_ahead), 0.0, 1000.0);
  // A decode (the cell was not cached): the cells ahead into the same slot.
  if (ahead <= 0.0 || cell < 0 || g_pvs_decodes == decodes || !g_pvs_has_dir) return;
  const PPCContext saved = ctx;
  const int32_t count = int32_t(Be32(base, world + 61412));
  const uint32_t cells = Be32(base, world + 61404);
  const float len = std::sqrt(g_pvs_dir[0] * g_pvs_dir[0] + g_pvs_dir[1] * g_pvs_dir[1] +
                              g_pvs_dir[2] * g_pvs_dir[2]);
  if (len < 0.1f) return;
  std::vector<int32_t> done{cell};
  const uint32_t own_nodes = g_pvs_decodes - decodes;
  uint32_t root_bits = 0;
  {
    const uint32_t slot = Be32(base, world + 0xEFF0), mask = Be32(base, world + 24);
    const uint32_t bit = 2 * slot, half = mask + (bit >> 4) * 2;
    root_bits = (uint32_t(base[half] << 8 | base[half + 1]) >> (bit & 15)) & 3;
  }
  g_pvs_nodes = g_pvs_kept = g_pvs_lit = 0;
  for (double d = 20.0; d <= ahead + 0.01; d += 20.0) {
    // The cell at that point (sub_823D3470 with the locator the game uses, a position on the
    // guest stack).
    ctx = saved;
    ctx.r1.u64 = saved.r1.u32 - 256;
    const uint32_t vec = ctx.r1.u32 + 96;
    for (int i = 0; i < 3; ++i)
      PutBeF(base + vec + 4 * i, g_pvs_pos[i] + g_pvs_dir[i] / len * float(d));
    PutBeF(base + vec + 12, 1.0f);
    ctx.r3.u64 = kPvsLocator;
    ctx.r4.u64 = vec;
    sub_823D3470(ctx, base);
    const int32_t n = int32_t(ctx.r3.u32);
    if (n < 0 || n >= count || std::find(done.begin(), done.end(), n) != done.end()) continue;
    done.push_back(n);
    // As sub_823CD238 calls it: the root node (world+24), the cell's record (40 bytes each, its
    // data pointer at +24) and a bit cursor from 0.
    ctx = saved;
    ctx.r1.u64 = saved.r1.u32 - 256;
    const uint32_t cursor = ctx.r1.u32 + 80;
    std::memset(base + cursor, 0, 4);
    ctx.r3.u64 = world;
    ctx.r4.u64 = world + 24;
    ctx.r5.u64 = cells + uint32_t(n) * 40 + 24;
    ctx.r6.u64 = cursor;
    g_pvs_union = true;
    sub_823CD090(ctx, base);
    g_pvs_union = false;
  }
  ctx = saved;
  static uint32_t logged = 0;
  if (logged < 40) {
    ++logged;
    std::string list;
    for (size_t i = 1; i < done.size(); ++i) list += " " + std::to_string(done[i]);
    REXLOG_INFO("PGR4 PVS: cell {} of {} (slot {}, its own decode {} nodes, root bits {}), cells ahead added:{} "
                "({} nodes walked, {} bits newly set, {} kept)",
                cell, count, Be32(base, world + 0xEFF0), own_nodes, root_bits, list, g_pvs_nodes, g_pvs_lit,
                g_pvs_kept);
  }
}

// Test: the per-cell visible set (sub_8238BD38, after the PVS update: two cached entries of 96
// bytes from world+61560, each its cell, the camera then and a frame stamp, the set made by
// sub_8238C398 walking the scenery tree) computed again every frame from the camera now: both
// entries' cells set to -2 after it, so that the next frame finds none.
REXCVAR_DEFINE_BOOL(pgr4_pvs_refresh, false, "PGR4",
                    "Test: the visible set of the camera's PVS cell made again every frame (not "
                    "only when entering the cell)");
REX_EXTERN(__imp__sub_8238BD38);
REX_HOOK_RAW(sub_8238BD38) {
  const uint32_t world = ctx.r3.u32;
  __imp__sub_8238BD38(ctx, base);
  // The world's highest scenery LOD (sub_823D67A0), read and forced: tunables.h.
  if (world) {
    if (const int32_t forced = g_pgr4_force_world_max_lod.load(std::memory_order_relaxed); forced >= 0) {
      base[world + 0x728] = 0, base[world + 0x729] = 0, base[world + 0x72A] = 0;
      base[world + 0x72B] = uint8_t(forced);
    }
    g_pgr4_world_max_lod.store(int32_t(Be32(base, world + 0x728)), std::memory_order_relaxed);
  }
  if (!REXCVAR_GET(pgr4_pvs_refresh) || !world) return;
  for (uint32_t e = 0; e < 2; ++e) {
    const uint32_t a = world + 61560 + 96 * e;
    base[a] = 0xFF;
    base[a + 1] = 0xFF;
    base[a + 2] = 0xFF;
    base[a + 3] = 0xFE;
  }
}
