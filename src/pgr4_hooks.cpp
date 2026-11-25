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
#include <set>
#include <mutex>
#include <thread>
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

// Local change (20/11/2026, the frame-rate audit): the console frames' count (sub_824C6FD8 below:
// the kudos tick, the collisions' and the crowd's console frames) and the music's fades counted
// real time by Pgr4FrameSeconds, an average over about 1/2 s made for the motion blur's strength:
// after a hitch or a change of frame rate they were wrong for that long. pgr4_game_frame_time gives
// them the last measured step, unsmoothed (real time, the pause too). Read at each call.
// Not for the cockpit's head (pgr4_cockpit_head_fix): given the game's frame time (0x82A81330,
// read in its slerp) the whole cockpit view jumped from frame to frame at 72 frames a second,
// standing still on a bike (captures 20/11/2026, alternated: 1.6-1.7 mean change between frames
// without, 3.2-3.4 with; 1.2 with the head fix off), so it keeps Pgr4FrameSeconds until the time
// its camera update is given is read in the code.
REXCVAR_DEFINE_BOOL(pgr4_game_frame_time, true, "PGR4",
                    "The console frames' count and the music's fades follow the last frame's real "
                    "time instead of a 1/2 s average");
float Pgr4FrameStepSeconds();  // pgr4-native pgr4_d3d_trace.cpp

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
// pgr4-native fidelityfx.cpp: --pgr4_render_height, 0 resolved from the screen's shape.
namespace pgr4::native {
uint32_t FfxSceneHeight();
}
REXCVAR_DECLARE(std::string, pgr4_native_capture_dir);  // native/draw.cpp: development logs

void Pgr4RenderHeightHook(PPCRegister& r) {
  if (r.u32 == 720) r.u64 = pgr4::native::FfxSceneHeight();
}

// Local change (25/11/2026, test: Tazz, « on peux ajouter le vertical ? »): the game's own side by
// side split screen (docs/audit_ecran_partage.md). With two viewports, sub_824C4B80 lays the views
// out at a race's load from player 1's profile option VerticalSplitScreen (profile + 8 + 0x2054B,
// read by the lbzx at 0x824C535C: 0 the top and bottom halves, else the left and right halves,
// 640x720 each, a rear-view mirror each), and sub_8261BDA8 places each player's messages from the
// same byte (its lbzx at 0x8261C0D4). No menu shows the option. On: both reads see it set, the
// profile itself left as it is (nothing saved); off: the profile's value, the game as it was. A
// choice of layout, the player's: off by default (on in the development pgr4.toml for the test).
// Read at each race's load (the layout) and at each message.
REXCVAR_DEFINE_BOOL(pgr4_split_side_by_side, false, "PGR4",
                    "Two players' split screen side by side (left and right halves), the game's "
                    "own layout behind its hidden VerticalSplitScreen option; off: top and bottom");
void Pgr4SideBySideHook(PPCRegister& r) {
  // The layout's read, logged with the profile's own value (the views' layout is made once a race).
  static uint32_t logged = 0;
  if (logged < 8) {
    ++logged;
    REXLOG_INFO("PGR4 split screen: the layout's VerticalSplitScreen read {} in the profile, {}",
                r.u32 & 0xFF, REXCVAR_GET(pgr4_split_side_by_side) ? "side by side forced" : "kept");
  }
  if (REXCVAR_GET(pgr4_split_side_by_side)) r.u64 = 1;
}
void Pgr4SideBySideTextHook(PPCRegister& r) {
  if (REXCVAR_GET(pgr4_split_side_by_side)) r.u64 = 1;
}
// ... and the scripts': GetOptionsVars (sub_825E59E8, its lbzx at 0x825E5F24) hands the option to
// Lua as set (25/11/2026, the test's HUD kept the top and bottom layout: whether the scripts lay
// it out from the option); SetOptionsVars (sub_825E2388, its stbx r11,r29,r10 at 0x825E296C) then
// writes back the profile's own value, not Lua's (the profile and the save as they were).
// ... without the rear-view mirrors, as the game's top and bottom layout (25/11/2026, Tazz: « on
// suit ce que le jeu faisait d'origine »): its branch (loc_824C54F0) stores 0 in each view's
// descriptor's mirror byte (+20: stb r24,100(r1), r24 0); the side by side branch stores 1
// (stb r19,100(r1) at 0x824C53C0 for view 0, 0x824C547C for view 1), and its two mirrors, never
// finished, share one texture (0x82A94708) and render player 1's car as the focus (their view
// number +32 left 0): player 2's driver in both mirrors. The byte set to 0 after each store, in
// memory (r19 stays: the branch uses it afterwards). Read at each race's load.
REXCVAR_DEFINE_BOOL(pgr4_split_side_by_side_no_mirror, true, "PGR4",
                    "Side by side split screen: no rear-view mirrors, as the game's top and bottom "
                    "layout; off: the side by side layout's own mirrors");
void Pgr4SideBySideMirrorHook(PPCRegister& r1) {
  if (!REXCVAR_GET(pgr4_split_side_by_side) || !REXCVAR_GET(pgr4_split_side_by_side_no_mirror)) return;
  *rex::system::kernel_memory()->TranslateVirtual<uint8_t*>(r1.u32 + 100) = 0;
  static uint32_t logged = 0;
  if (logged < 4) {
    ++logged;
    REXLOG_INFO("PGR4 split screen: side by side view without its rear-view mirror");
  }
}
// Local change (25/11/2026, Tazz: « Oui, le HUD »): the HUD in the side by side views. No game
// script reads VerticalSplitScreen (work/hud_audit: the 247 scripts' constants): the HUD keeps its
// top and bottom bands. With this on, the scripts see the option set (GetOptionsVars, below) and
// our Lua after Screens\GUIBank.lua (src/pgr4_menu.cpp HudSideBySideLua) gives each player the left
// or right half in GetViewportDetails (the 2D items through AdjustPositionAndSizeForViewports, the
// 3D gauges through SendViewportDetails), as the game's 3-4 viewport branch does its quarters;
// Screens\HudHelper.lua's kudos stars' bank_x follows the half; the 3D gauges' x within their
// rectangle is pgr4_split_side_by_side_gauge_x (Pgr4SideBySideGaugeXHook). Read at each call.
REXCVAR_DEFINE_BOOL(pgr4_split_side_by_side_hud, true, "PGR4",
                    "Side by side split screen: each player's HUD in their half; off: the game's "
                    "top and bottom bands");
// The 3D gauges' x in their rectangle (sub_8245E7E0's split screen branch: the engine variable
// Vehicle.HUD.Setup.HorizSplitLayoutX, 0.825, made for a wide band): 0.65 keeps their right margin
// in a 640-wide half as it is in the band (work/hud_audit/layout_calc_out.txt).
REXCVAR_DEFINE_DOUBLE(pgr4_split_side_by_side_gauge_x, 0.65, "PGR4",
                      "Side by side split screen: the speed and rev gauges' x in their half (the "
                      "game's 0.825 for its wide bands)");
bool Pgr4SideBySideHud() {
  return REXCVAR_GET(pgr4_split_side_by_side) && REXCVAR_GET(pgr4_split_side_by_side_hud);
}
void Pgr4SideBySideLuaHook(PPCRegister& r) {
  // The scripts see the side by side layout only for its HUD (the views' layout is the engine's,
  // Pgr4SideBySideHook; no script read it before).
  if (Pgr4SideBySideHud()) r.u64 = 1;
}
// sub_8245E7E0's split screen branch: f1 = HorizSplitLayoutX loaded (lfs f1,32(r29) at 0x8245E96C
// before bl sub_8245C2A8, at 0x8245E980 before sub_8245C310), the solo branch's own (0x8245E9B4,
// 0x8245E9E8) left as they are.
void Pgr4SideBySideGaugeXHook(PPCRegister& f1) {
  if (!Pgr4SideBySideHud()) return;
  f1.f64 = REXCVAR_GET(pgr4_split_side_by_side_gauge_x);
}
// The probe: SendViewportDetailVariables' (sub_825CE0E0) store of a view's rectangle for the 3D
// gauges (0x82AAD3EC + r10, stfsx at 0x825CE1D8..E8: sizeX f29, sizeY f28, topLeftX f31, topLeftY
// f30, heightRatio f1), in the interface's 640x480 units; logged at each call (a race's load).
// Development only (pgr4_native_capture_dir set, never in the release).
void Pgr4ViewportDetailProbeHook(PPCRegister& r10, PPCRegister& f29, PPCRegister& f28,
                                 PPCRegister& f31, PPCRegister& f30, PPCRegister& f1) {
  static uint32_t logged = 0;
  if (logged >= 64 || REXCVAR_GET(pgr4_native_capture_dir).empty()) return;
  ++logged;
  REXLOG_INFO("PGR4 split screen HUD: view {} rectangle x {:.2f} y {:.2f} size {:.2f} x {:.2f}, height "
              "ratio {:.3f}{}",
              r10.u32 / 20, f31.f64, f30.f64, f29.f64, f28.f64, f1.f64,
              Pgr4SideBySideHud() ? " (side by side HUD)" : "");
}
void Pgr4SideBySideStoreHook(PPCRegister& r11, PPCRegister& r29, PPCRegister& r10) {
  if (!REXCVAR_GET(pgr4_split_side_by_side)) return;
  const uint8_t own = *rex::system::kernel_memory()->TranslateVirtual<const uint8_t*>(r29.u32 + r10.u32);
  static uint32_t logged = 0;
  if (logged < 8 && (r11.u32 & 0xFF) != own) {
    ++logged;
    REXLOG_INFO("PGR4 split screen: the scripts' VerticalSplitScreen {} not saved, the profile's {} kept",
                r11.u32 & 0xFF, own);
  }
  r11.u64 = own;
}

// The scene's surface pair creators fn_826C8B18 and sub_822B3FC8 (r4: the height). A frame taller
// than 720 needs taller surfaces: sub_826C6100 clamps every viewport to the bound target's packed
// size, which would clamp it back to 720. A shorter frame (21:9) keeps the game's 720 surfaces,
// which hold it, and so does every other 720-tall surface they make: the videos' target, whose
// 1280x720 Bink frame is resolved whole into the texture the menus show (at the render height
// its draw was cut to that many rows, the rest of the picture missing).
void Pgr4SurfaceHeightHook(PPCRegister& r4) {
  if (r4.u32 == 720 && pgr4::native::FfxSceneHeight() > 720) r4.u64 = pgr4::native::FfxSceneHeight();
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
    GuestStore32(r5.u32 + 12, pgr4::native::FfxSceneHeight());
}

void Pgr4ViewportHook(PPCRegister& r4) {
  if (!r4.u32) return;
  if (GuestLoad32(r4.u32 + 8) == 1280 && GuestLoad32(r4.u32 + 12) == 720)
    GuestStore32(r4.u32 + 12, pgr4::native::FfxSceneHeight());
}

// The float viewport setter (sub_826C63F8, the one the scene's views use): r4 points at
// {X, Y, W, H, MinZ, MaxZ} floats.
void Pgr4ViewportFHook(PPCRegister& r4) {
  if (!r4.u32) return;
  constexpr uint32_t kW = 0x44A00000;  // 1280.0f
  constexpr uint32_t kH = 0x44340000;  // 720.0f
  if (GuestLoad32(r4.u32 + 8) == kW && GuestLoad32(r4.u32 + 12) == kH) {
    const float h = float(pgr4::native::FfxSceneHeight());
    GuestStore32(r4.u32 + 12, std::bit_cast<uint32_t>(h));
  }
}

// The vertex block's c45, the HUD's orthographic projection, uploaded with the block by the
// state apply (sub_82871A20). Left as the game wrote it: the menus' and the HUD's draws also take
// their c45 by other paths (a draw log of the title screen showed the game's value after this
// block was rewritten), so the native renderer fits it to the frame in the constants every draw
// uploads instead (pgr4-native draw.cpp, FitHudOrtho). The manifest's hook stays, doing nothing.
void Pgr4HudOrthoHook(PPCRegister&) {}

// The display's aspect: the game takes 16:9 (4:3 without its widescreen flag, [0x82A93860 +
// 180]) from one constant, 0x820010A8, at all 15 of its views' setups (cameras, sub_82323160:
// pixel aspect = aspect x height / width), and its projection (sub_822C2410 ->
// sub_826E32F0, XMMatrixPerspectiveFovRH's maths) divides the vertical field of view's scale by
// width / height x that pixel aspect. With the frame 1280 x render height the 16:9 squeezes the
// picture by height / 720; the frame's own aspect instead keeps the camera's vertical field of
// view and widens the horizontal one with the frame (Hor+: 21:9 shows more of the sides, the
// same height). Written once, before the game's first instruction (Pgr4App::OnPreLaunchModule):
// the constant is in read-only data, its page opened for the write.
void Pgr4ApplyRenderAspect() {
  const uint32_t h = pgr4::native::FfxSceneHeight();
  if (h == 720) return;
  constexpr uint32_t kWideAspect = 0x820010A8;
  if (GuestLoad32(kWideAspect) != std::bit_cast<uint32_t>(16.0f / 9.0f)) {
    REXLOG_WARN("PGR4 aspect: 0x{:08X} is not the game's 16:9, left as it is", kWideAspect);
    return;
  }
  auto* heap = rex::system::kernel_memory()->LookupHeap(kWideAspect);
  if (!heap) return;
  const uint32_t page = kWideAspect & ~(heap->page_size() - 1);
  uint32_t old = 0;
  heap->Protect(page, heap->page_size(),
                rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, &old);
  const float aspect = 1280.0f / float(h);
  GuestStore32(kWideAspect, std::bit_cast<uint32_t>(aspect));
  if (old) heap->Protect(page, heap->page_size(), old);
  REXLOG_INFO("PGR4 aspect: {:.4f} for the 1280x{} frame (16:9 is 1.7778)", aspect, h);
}

// A frame taller than 16:9 (16:10) keeps the 16:9's horizontal field of view and shows more above
// and below (Vert+), as a wider one keeps the vertical and shows more of the sides: the game's
// projection (sub_826E32F0(f1 vertical field of view, f2 aspect, f3 near, f4 far), whose x scale is
// the y scale / f2) gets, for the frame's views (f2 the aspect Pgr4ApplyRenderAspect wrote), a
// vertical field of view whose tangent is the game's x height / 720.
REX_EXTERN(__imp__sub_826E32F0);
REX_HOOK_RAW(sub_826E32F0) {
  static const uint32_t h = pgr4::native::FfxSceneHeight();
  if (h > 720 && std::abs(ctx.f2.f64 - 1280.0 / h) < 1e-4 && ctx.f1.f64 > 0.0 && ctx.f1.f64 < 3.0) {
    const double game = ctx.f1.f64;
    ctx.f1.f64 = 2.0 * std::atan(std::tan(game * 0.5) * double(h) / 720.0);
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 4)
      REXLOG_INFO("PGR4 aspect: vertical field of view {:.2f} -> {:.2f} degrees (caller {:08X})",
                  game * 57.29578, ctx.f1.f64 * 57.29578, uint32_t(ctx.lr));
  }
  __imp__sub_826E32F0(ctx, base);
}

// SetScissorRect (fn_826C59D8): r4 points at {x1, y1, x2, y2}.
void Pgr4ScissorRectHook(PPCRegister& r4) {
  if (!r4.u32) return;
  if (GuestLoad32(r4.u32 + 8) == 1280 && GuestLoad32(r4.u32 + 12) == 720)
    GuestStore32(r4.u32 + 12, pgr4::native::FfxSceneHeight());
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
// Local change (23/11/2026): read at each frame by pgr4-native (renderer.cpp ApplyMotionBlurRule:
// a race and its replay only, the menus, the pause and the photo mode keeping the game's blur);
// the hook below gives the game's start its value.
REXCVAR_DEFINE_BOOL(pgr4_disable_motion_blur, false, "PGR4",
                    "Disable motion blur in a race and its replay (Xenia patch by illusion)");
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
// A guest address's bytes on the host. The recompiled code (REX_PHYS_HOST_OFFSET, Windows) puts the
// 4 KB physical heap, 0xE0000000 up, 0x1000 further on: base + address alone is a page short there.
// The pose and bone buffers live there (20/11/2026).
uint8_t* GuestBytes(uint8_t* base, uint32_t a) { return base + a + (a >= 0xE0000000u ? 0x1000u : 0u); }
const uint8_t* GuestBytes(const uint8_t* base, uint32_t a) {
  return base + a + (a >= 0xE0000000u ? 0x1000u : 0u);
}
void PutBeF(uint8_t* p, float f) {
  const uint32_t v = std::bit_cast<uint32_t>(f);
  for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (24 - 8 * i));
}
uint64_t g_frame_no = 0;  // the game's frames (sub_824C6FD8)
// sub_824C6FD8: the game frames' lengths (Pgr4FrameSeconds) in console frames, carried over, one
// at most a game frame. For what the console did once per frame (the kudos counter's tick).
double g_console_credit = 0.0;
// This game frame starts a console frame (g_console_credit reached 1): the audio's once-a-console-
// frame events (the collisions, the crowd's delay after a fall) go out on those frames only.
std::atomic<bool> g_console_frame_start{true};
// The kudos counter's tick asked for during a console frame (sub_826A03A8, index 5), played at
// the start of the next (sub_824C6FD8): its sound object (sub_826A03A8's r3), 0 for none.
std::atomic<uint32_t> g_kudos_tick_object{0};
thread_local bool g_kudos_tick_playing = false;  // sub_826A03A8 called for it: played, not kept
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
REXCVAR_DEFINE_BOOL(pgr4_cockpit_input_watch, false, "PGR4",
                    "Development: note which code writes the view camera's head target and place "
                    "(+320 to +447; page watch, once a run)");
namespace {
void WriteWatchStart(uint8_t* base, uint32_t table, uint32_t count);  // below
void WriteWatchReport(std::initializer_list<uint32_t> pages);
}  // namespace
REXCVAR_DEFINE_BOOL(pgr4_ui_clock_probe, false, "PGR4",
                    "Development: the UI's frame time and the game clock's, in the log");
void Pgr4BringWindowToFront();  // src/pgr4_menu.cpp
bool Pgr4Precompiling();       // src/pgr4_profile.cpp
REX_EXTERN(sub_826A03A8);  // a UI sound (object, index): hooked below
void Pgr4CadenceFrame(const uint8_t* base);  // below: the frame-rate audit's counts
REX_HOOK_RAW(sub_824C6FD8) {
  ++g_frame_no;
  {
    // A frame of 1/30 s, give or take 3 %, is one console frame (at a 30 FPS limit, every frame).
    const double seconds = REXCVAR_GET(pgr4_game_frame_time) ? double(Pgr4FrameStepSeconds())
                                                              : double(Pgr4FrameSeconds());
    const double step = std::clamp(seconds * 30.0, 0.0, 1.0);
    g_console_credit += step > 0.97 ? 1.0 : step;
    g_console_frame_start.store(g_console_credit >= 1.0, std::memory_order_relaxed);
    if (g_console_credit >= 1.0) {
      g_console_credit = std::min(g_console_credit - 1.0, 0.999);
      // A console frame starts: the kudos tick asked for during the last one, once. This thread
      // plays UI sounds through sub_826A03A8 itself (EnableMusic, every frame).
      if (const uint32_t object = g_kudos_tick_object.exchange(0)) {
        const PPCContext saved = ctx;
        ctx.r3.u64 = object;
        ctx.r4.u64 = 5;
        g_kudos_tick_playing = true;
        sub_826A03A8(ctx, base);
        g_kudos_tick_playing = false;
        ctx = saved;
      }
    }
  }
  Pgr4CadenceFrame(base);  // development: the frame-rate audit's counts
  // pgr4_cockpit_input_watch: which code writes the view camera's +320 to +447 (its head target
  // +320, the place it hangs from +400, the head +416; sub_827DA5C0 reads them), the page watch
  // once a run, reported every 2 s.
  if (REXCVAR_GET(pgr4_cockpit_input_watch)) {
    WriteWatchStart(base, 0x82C91D00 + 320, 2);
    static auto last = std::chrono::steady_clock::now();
    if (std::chrono::steady_clock::now() - last > std::chrono::seconds(2)) {
      last = std::chrono::steady_clock::now();
      WriteWatchReport({0, 1});
    }
  }
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
  // Local change (18/11/2026): back as well (ahead < 0: the physics ahead of the game's time, as
  // in a race's last step of a frame; pgr4_vehicle_pose_ahead).
  if (!st.has_prev || !(h > 0) || ahead == 0.0f) return;
  const double frac = std::clamp(double(ahead) / h, -2.0, 2.0);
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
// Development (17/11/2026, the menu's drifting car moving by whole 1/60 s steps at 44 frames a
// second): which of the two functions makes each car's drawn pose, and with what time past the
// step, in the log for 120 calls from when it is switched on.
REXCVAR_DEFINE_BOOL(pgr4_vehicle_pedal_watch, false, "PGR4",
                    "Development, with pgr4_vehicle_lights_probe: note for 60 s which code writes a "
                    "car's controls from its first braking in a replay (page watch)");
REXCVAR_DEFINE_BOOL(pgr4_vehicle_lights_probe, false, "PGR4",
                    "Development: log what each car's brake and reverse lights are based on at each "
                    "sub_82445CB8 call (mode, pedal, average, flag, gear, steps)");
REXCVAR_DEFINE_BOOL(pgr4_vehicle_pose_probe, false, "PGR4",
                    "Development: log which function draws each car's pose (extrapolated or the "
                    "step's) and the time past the step");
namespace {
int g_pose_probe_left = 0;
// With the physics clock (sub_824C6398: steps of 0x82001170 while (n - 1) h <= t - x, x and n at
// 0x82A17640 and +4, the game's time t at 0x82A8131C + 24) and the caller.
void PoseProbe(const uint8_t* base, const char* what, uint32_t record, double ahead, uint64_t lr) {
  static bool was = false;
  const bool on = REXCVAR_GET(pgr4_vehicle_pose_probe);
  if (on && !was) g_pose_probe_left = 120;
  was = on;
  if (!on || g_pose_probe_left <= 0) return;
  --g_pose_probe_left;
  const float x = BeF(base + 0x82A17640), t = BeF(base + 0x82A8131C + 24),
              h = BeF(base + 0x82001170);
  const uint8_t* np = base + 0x82A17644;
  const int32_t n = int32_t(uint32_t(np[0]) << 24 | uint32_t(np[1]) << 16 | uint32_t(np[2]) << 8 | np[3]);
  REXLOG_INFO("PGR4 pose probe: frame {} car {:08X} {} ahead {:.5f} s, frame {:.5f} s, caller {:08X}, "
              "physics x {:.6f} n {} t {:.5f} h {:.6f}: x + (n-1)h - t = {:.5f}",
              g_frame_no, record, what, ahead, Pgr4FrameSeconds(), uint32_t(lr), x, n, t, h,
              x + (n - 1) * h - t);
}
}  // namespace
// Local change (18/11/2026, Tazz: the cars jerking at 44 frames a second, the menu's drifting car
// gripping and spinning by turns): the physics steps 1/60 s at a time (sub_824C6398, step count n
// at 0x82A17644, the game's time t at 0x82A8131C + 24), and every car's drawn pose was its last
// step's (sub_82437C18 -> sub_82416BA0 without a pose; measured in the menus and in a race): one
// step a frame, two every third at 44 frames a second, 0.074 / 0.073 / 0.147 m and a rear wheel's
// 58 / 58 / 117 degrees. The game's own extrapolation, sub_82417B50 (the pose carried forward by
// the car's velocities, as its photo shutter's sub-frames use it), now makes it, carried by the
// time since the pose the game wrote (g_pose_time): every car drawn at the game's time. On by
// default (Tazz, 18/11/2026).
REXCVAR_DEFINE_BOOL(pgr4_vehicle_pose_extend, true, "PGR4",
                    "The game's own mix of a car's last two physics steps carried on past its range "
                    "to the game's time, instead of the velocity carry");
REXCVAR_DEFINE_BOOL(pgr4_vehicle_pose_ahead, true, "PGR4",
                    "Every car's drawn pose carried from the game's pose to the game's time "
                    "(smooth at frame rates other than 60 and 30)");
namespace {
bool g_pose_carrying = false;  // sub_82417B50 called from sub_82416BA0's hook below
// The r5 of the update's own sub_82416BA0 call, for the carried one. sub_82417B50 (the photo
// shutter's) passes 1, a preview: sub_82416BA0 then returns after placing the car, before the
// rest of the frame's car update (skid marks and the like: they vanished with the carry, Tazz,
// 18/11/2026). The update passes 0 (0x82437C78).
uint64_t g_carry_r5 = 0;
// The game's time of the pose sub_82445CB8 last wrote at each address (a car record's +736), and
// the frame it wrote it in. It writes one of three (read 18/11/2026): below its range (fraction
// (x + (n - 1) h - t) / x under 0x82258A6C's 0.001, or 0 when Dynamics.AllowAdjustToFramerate at
// 0x82A81378 is 0) the body's last step pose (its vfunc +120), of time x + (n - 1) h; above it
// (over 0x8218BCC0's 0.999) the step before (+128), of time (n - 1) h; between, the two mixed by
// the fraction (sub_822C4340), of time t itself. The carry below takes the pose from that time to
// t: none for a mixed pose (a race's, every frame), a step's carried. Before, the carry assumed a
// step's pose at n h: a mixed one (the menus mix one frame in a few between 60 and 120 frames a
// second) was carried a second time, up to 2 cm behind (probe, 18/11/2026), and a car the game
// mixed in the last 30 frames was not carried at all (its cars stepped again at 81 frames a
// second, Tazz, 18/11/2026).
// The target is the game's time at that call, not at the car's update: in the menus the mix comes
// first in the frame and both are the same, in a race the car's update comes first and draws the
// pose mixed at the end of the frame before (a frame behind, smooth: the game's own), so carried to
// the update's time every car of a race was moved a frame on by its velocities (seen 18/11/2026).
struct PoseTime {
  double time, target;
  uint64_t frame;
};
std::unordered_map<uint32_t, PoseTime> g_pose_time;
// pgr4_vehicle_pose_extend (18/11/2026; on by default, Tazz: « c'est parfait », the front wheels no
// longer jump): the game's mix let past its range,
// for a fraction within [kExtendLow, kExtendHigh]: its straight line between the two steps carried
// on to the game's time (sub_822C4340: positions (1 - f) A + f B, rotations a normalized linear
// mix, both fine out of 0..1), the range's bounds loaded at 0x82445DE8 and 0x82445E2C taken out
// (Pgr4PoseLowBoundHook, Pgr4PoseHighBoundHook). Everything the function then derives from the
// pose (+800, which the wheels start from, and the rest) and the car's own update are the game's.
// The velocity carry below left the frame where a new step comes in 6 % short in the menus, the
// car's velocities not landing where the next step puts it (an every-frame carry from the step,
// tried the same day, was worse: 4.07 to 4.85 m/s for 4.12 to 4.52).
constexpr float kExtendLow = -1.0f, kExtendHigh = 2.0f;
// Local change (20/11/2026, the frame-rate audit): in the menus the mix comes before the frame's
// steps, so the game's time is up to as many steps past the last as the frame will run: one above
// 60 frames a second (a fraction down to -1), two from 30 to 60 (down to -2). Below -1 the pose
// fell back to the velocity carry (sub_82417B50, 6 % short when a step came in; one frame in three
// at 44 frames a second), and the wheels to the last step's pose. pgr4_vehicle_pose_extend_two_steps
// lets the mix carry on to -2: the console's 30 FPS and everything above mixed the same way. Read
// at each call.
REXCVAR_DEFINE_BOOL(pgr4_vehicle_pose_extend_two_steps, true, "PGR4",
                    "The cars' mix carried on up to two steps past the last (menus from 30 to 60 "
                    "frames a second) instead of one");
float ExtendLow() { return REXCVAR_GET(pgr4_vehicle_pose_extend_two_steps) ? -2.0f : kExtendLow; }
thread_local bool t_pose_extend = false;  // set around sub_82445CB8's call
// Local change (18/11/2026, Tazz: the hubcaps flicker, seen in DLSS-G's Depth/MVec Alignment view
// once every 8 frames at 68 frames a second): sub_82445CB8 mixes a car's body between its last two
// physics steps (sub_822C4340, fraction f), then, for a car (physics +16 == 0), copies each
// wheel's values from the physics as they are (loop at 0x824460F8: physics wheels at +1240, 368
// bytes each; drawn wheels at *(pose + 960), 160 bytes each, *(pose + 968) of them), its steering
// angle (+0 into +80: 0.52 rad held through the menu car's drift, probe) and its spin angle (-8
// into +84) among them, which sub_82441040 turns the wheel by, the steering about the axis at the
// wheel's +144, then the spin about its axle (lfs f0,84(r24), the rotation about +656). The
// body followed the game's time, the wheels' spin only the steps: in a frame without a step
// (frames past 60 a second: one in 8 at 68, one in 2 at 120) they stood still and turned twice as
// far in the next, against the body (and a wheel's motion vectors with it). pgr4_vehicle_wheel_spin_mix
// mixes the spin angle by the same fraction as the body, between its last step's value and the
// step before's (kept here, as the physics keeps only the last): f = 0 the last step, 1 the one
// before, the game's mix and pgr4_vehicle_pose_extend's alike. A step's per-step turn may be more
// than half a turn (250 rad/s at 300 km/h: 4.2 rad a step), so the turn between two steps is the
// one nearest the turn of the step before (any whole turns added). Read at each call.
struct WheelSpin {
  float last = 0.0f, before = 0.0f, turn = 0.0f;  // the angle at the last step, at the one before
  bool has_before = false;
  // pgr4_vehicle_wheel_pose_mix: the wheel's place (+0, +32), axis (+16, a unit vector) and
  // steering (+80) at the last step and the one before (kPoseFloats, in kPoseOffsets' order).
  float pose_last[10] = {}, pose_before[10] = {};
};
// The wheel's values sub_82441040 places it by, besides the spin, that sub_82445CB8 also copies
// from the last step: its place +0 (and +32, the one a debug byte picks), its axis +16 (the
// suspension's and camber's: 3 degrees a step at most, probe in a race, 18/11/2026) and its
// steering +80 (4.3 degrees a step at most); measured in a race, 2.2 cm a step at most. Not +124
// (unchanged in the race probed).
constexpr int kPoseFloats = 10;
constexpr int kPoseOffsets[kPoseFloats] = {0, 4, 8, 16, 20, 24, 32, 36, 40, 80};
struct CarSpin {
  int32_t n = 0;  // the physics step count at `last`
  uint32_t wheels = 0;
  uint64_t frame = 0;
  WheelSpin wheel[8];
};
std::unordered_map<uint32_t, CarSpin> g_wheel_spin;  // by pose (a car record's +736)
}  // namespace
REXCVAR_DEFINE_BOOL(pgr4_vehicle_wheel_spin_mix, true, "PGR4",
                    "Each car wheel's spin angle mixed between its last two physics steps by the "
                    "fraction the game mixes the body by");
// Local change (18/11/2026, Tazz: « vérifie aussi pour les pneus et le reste »): the same for the
// wheel's place, axis and steering (WheelSpin::pose_last). They step as the spin did, by less (2.2
// cm, 3 and 4.3 degrees a step at most in a race). The tracks (sub_8243FC38) and the rest of the
// car's update read the wheel where sub_82441040 then places it: at the body's time too. Read at
// each call.
REXCVAR_DEFINE_BOOL(pgr4_vehicle_wheel_pose_mix, true, "PGR4",
                    "Each car wheel's place, axis and steering mixed between its last two physics "
                    "steps by the fraction the game mixes the body by");
namespace {
uint32_t WheelBeU32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
// After sub_82445CB8 (its `pose`, physics step count `n`, the fraction `f` it drew the body at:
// 0 the last step, 1 the one before, past them with pgr4_vehicle_pose_extend).
void MixWheelSpin(uint8_t* base, uint32_t pose, int32_t n, float f) {
  const uint32_t wheels = WheelBeU32(base + pose + 968);
  const uint32_t array = WheelBeU32(base + pose + 960);
  if (!wheels || wheels > 8 || array < 0x40000000) return;
  constexpr float kTwoPi = 6.28318530718f;
  CarSpin& car = g_wheel_spin[pose];
  // A car seen again after a while, another car in the record, or a jump in the steps (a replay's
  // cut, a reset): no step before yet.
  const bool fresh = car.wheels != wheels || g_frame_no > car.frame + 2 || n < car.n || n > car.n + 8;
  const int32_t steps = fresh ? 0 : n - car.n;
  for (uint32_t i = 0; i < wheels; ++i) {
    uint8_t* angle = base + array + 160 * i + 84;  // the spin
    const float now = BeF(angle);  // the game's: the last step's
    WheelSpin& w = car.wheel[i];
    uint8_t* const entry = base + array + 160 * i;
    float pose_now[kPoseFloats];
    bool pose_finite = true;
    for (int k = 0; k < kPoseFloats; ++k) {
      pose_now[k] = BeF(entry + kPoseOffsets[k]);
      pose_finite = pose_finite && std::isfinite(pose_now[k]);
    }
    if (fresh || !std::isfinite(now) || !pose_finite) {
      w = {now, now, 0.0f, false};
      std::copy(pose_now, pose_now + kPoseFloats, w.pose_last);
      std::copy(pose_now, pose_now + kPoseFloats, w.pose_before);
      continue;
    }
    if (steps > 0) {
      // The turn over these steps nearest `steps` times the last step's, whole turns added.
      const float expected = w.turn * float(steps);
      float d = now - w.last;
      d += kTwoPi * std::round((expected - d) / kTwoPi);
      w.turn = d / float(steps);
      w.before = now - w.turn;  // the step before the last (the others' turns alike)
      w.has_before = true;
      // The others alike: the step before the last, the steps between evenly.
      for (int k = 0; k < kPoseFloats; ++k)
        w.pose_before[k] = pose_now[k] - (pose_now[k] - w.pose_last[k]) / float(steps);
    }
    w.last = now;
    std::copy(pose_now, pose_now + kPoseFloats, w.pose_last);
    if (w.has_before && REXCVAR_GET(pgr4_vehicle_wheel_pose_mix)) {
      float mixed[kPoseFloats];
      for (int k = 0; k < kPoseFloats; ++k)
        mixed[k] = pose_now[k] - f * (pose_now[k] - w.pose_before[k]);
      // The axis kept a unit vector when it was one (the game's rotations are mixed and normalized
      // alike, sub_822C4200).
      const float len = std::sqrt(mixed[3] * mixed[3] + mixed[4] * mixed[4] + mixed[5] * mixed[5]);
      const float len_now = std::sqrt(pose_now[3] * pose_now[3] + pose_now[4] * pose_now[4] +
                                      pose_now[5] * pose_now[5]);
      if (len > 1e-6f && std::abs(len_now - 1.0f) < 1e-3f)
        for (int k = 3; k < 6; ++k) mixed[k] /= len;
      for (int k = 0; k < kPoseFloats; ++k) PutBeF(entry + kPoseOffsets[k], mixed[k]);
    }
    if (!w.has_before || !REXCVAR_GET(pgr4_vehicle_wheel_spin_mix)) continue;
    // (1 - f) last + f before, as sub_822C4340 mixes the body's position.
    PutBeF(angle, now - f * w.turn);
    if (i == 0 && REXCVAR_GET(pgr4_vehicle_pose_probe) && g_pose_probe_left > 0)
      REXLOG_INFO("PGR4 pose probe: frame {} pose {:08X} wheel 0 spin: step {} angle {:.4f}, turn "
                  "{:.4f} a step, fraction {:.4f}, drawn {:.4f} (steering +80 {:.4f}, +88 {:.4f})",
                  g_frame_no, pose, n, now, w.turn, f, now - f * w.turn,
                  BeF(base + array + 80), BeF(base + array + 88));
  }
  // Development (pgr4_vehicle_pose_probe): the wheels' other values sub_82445CB8 copies from the
  // last step and sub_82441040 draws by (the position vectors +0, +16, +32, the steering +80, +124),
  // their change since the probe's last look, to see whether they also step.
  if (REXCVAR_GET(pgr4_vehicle_pose_probe) && g_pose_probe_left > 0) {
    // The brake lights' switch sub_82415138 reads (Vehicle.Lights.UpdateFromSorcery, light types
    // 0x1F to 0x21): the car record's +0x47E, off when +0x404 is -1 and +0x480 is set.
    const uint8_t* record = base + pose - 736;
    REXLOG_INFO("PGR4 pose probe: frame {} pose {:08X} brake lights: +0x47E {} +0x480 {} +0x404 {}",
                g_frame_no, pose, record[0x47E], record[0x480],
                int32_t(WheelBeU32(record + 0x404)));
    static std::unordered_map<uint32_t, std::array<float, 22>> seen;
    for (uint32_t i = 0; i < wheels && i < 2; ++i) {
      const uint8_t* e = base + array + 160 * (i == 0 ? 0 : wheels - 1);
      static constexpr int kOff[11] = {0, 4, 8, 16, 20, 24, 32, 36, 40, 80, 124};
      auto& s = seen[pose];
      float d[11];
      for (int k = 0; k < 11; ++k) {
        const float v = BeF(e + kOff[k]);
        d[k] = v - s[11 * i + k];
        s[11 * i + k] = v;
      }
      REXLOG_INFO("PGR4 pose probe: frame {} pose {:08X} wheel {} step {} change: +0 {:.4f} {:.4f} "
                  "{:.4f}, +16 {:.4f} {:.4f} {:.4f}, +32 {:.4f} {:.4f} {:.4f}, +80 {:.4f}, +124 {:.4f} "
                  "(+0 at {:.3f} {:.3f} {:.3f})",
                  g_frame_no, pose, i == 0 ? 0 : wheels - 1, n, d[0], d[1], d[2], d[3], d[4], d[5],
                  d[6], d[7], d[8], d[9], d[10], BeF(e), BeF(e + 4), BeF(e + 8));
    }
  }
  car.n = n;
  car.wheels = wheels;
  car.frame = g_frame_no;
}
}  // namespace
// Local change (18/11/2026, Tazz: the far opponents' bike wheels change from one frame to the
// next): a vehicle's parts are drawn with their simplified model (sub_8242D560, the part's second
// index set) when sub_824230A8 (vehicle, part group, &low) sets `low`: its height on screen
// (vehicle data +0x31D4, a fraction of the image's height, sub_82438768 by sub_8243B748: the
// bounding radius projected) at 0.05 or less (the constant at 0x82001290; a debug override at
// 0x82A1E288 when 0x82AA69E2 is set; always `low` in mode 4 at 0x82AAC630, and for part groups 1, 2,
// 5 and 6 when the data's +0x321F is set). 0.05 was 36 pixels on the console's 720 lines; at 2160
// lines it is 108, and a bike's simplified wheel (its tyre not covering the rim: the road seen
// through) popped in plain sight. pgr4_vehicle_lod_by_pixels keeps the switch at the console's size
// in pixels: 0.05 times 720 / the render's height (720 times pgr4_native_resolution_scale), the
// same at 720 lines and below. Read at each call.
REXCVAR_DEFINE_BOOL(pgr4_vehicle_lod_by_pixels, true, "PGR4",
                    "Vehicles switch to their simplified models at the console's size in pixels "
                    "(the game's 5 % of the image's height times 720 / the render's height)");
REXCVAR_DECLARE(int32_t, pgr4_native_resolution_scale);  // pgr4-native renderer.cpp
REX_EXTERN(__imp__sub_824230A8);
REX_HOOK_RAW(sub_824230A8) {
  const uint32_t vehicle = ctx.r3.u32, group = ctx.r4.u32 & 0xFF, out = ctx.r5.u32;
  __imp__sub_824230A8(ctx, base);
  const int32_t scale = REXCVAR_GET(pgr4_native_resolution_scale);
  if (!REXCVAR_GET(pgr4_vehicle_lod_by_pixels) || scale <= 1) return;
  // The game's forced cases (its branch to 0x824230B8), kept.
  if (WheelBeU32(base + 0x82AAC630) == 4) return;
  const uint32_t data = WheelBeU32(base + vehicle + 0xA0);
  if (base[data + 0x321F] != 0 && (group == 1 || group == 2 || group == 5 || group == 6)) return;
  const float size = base[0x82AA69E2] != 0 ? BeF(base + 0x82A1E288) : BeF(base + data + 0x31D4);
  const float threshold = BeF(base + 0x82001290) / float(scale);
  base[out] = (size > threshold) ? 0 : 1;  // as the game: not above the threshold, NaN included
}

// Diagnostic (18/11/2026, Shanghai's far buildings drawn white): sub_823B1F50 binds an instance's
// lightmap (sampler 14) with the entry sub_823B3A78 (world, record) returns: the record's page
// (world +0x28, 64 bytes each, at the record's +2) when its first halfword is below 0x8000, else
// the world's fallback (+0x98). Drawn with the fallback, psSceneDiffuse_Night's g_textureSet_14 is
// false and the building gets 1.0 for its lightmap, times 6: white. pgr4_lightmap_probe logs every 2
// seconds how often the fallback was returned, which pages, their entries' first 16 bytes and the
// fallback's texture. The game's result is unchanged.
REXCVAR_DEFINE_BOOL(pgr4_lightmap_probe, false, "PGR4",
                    "Log the lightmap pages missing when the scene's buildings are drawn "
                    "(sub_823B3A78's fallback), every 2 seconds (diagnostic)");
REXCVAR_DEFINE_BOOL(pgr4_lightmap_write_probe, false, "PGR4",
                    "With pgr4_lightmap_probe: note for 60 s which code writes the scene's texture "
                    "page table (diagnostic)");
namespace {
struct LightmapProbe {
  std::mutex mutex;
  std::map<uint64_t, uint32_t> missing;  // page << 32 | caller's return address -> fallbacks
  uint32_t calls = 0, fallbacks = 0, world = 0;
  std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
} g_lightmap_probe;

// Diagnostic (19/11/2026): which code writes the page table (pgr4_lightmap_write_probe). Its host
// pages are made read-only for kWatchSeconds; a write faults, the handler notes the instruction (its
// offset in pgr4.exe, for llvm-symbolizer) and the entry's offset, lets that one instruction run
// (the page writable, the trap flag set) and protects the page again at the single step. Reads are
// untouched. Removed by its own thread after kWatchSeconds, whatever happens to the race (a file
// read by the system into a protected page would fail).
struct PageWrite {
  uint64_t rva;
  uint32_t guest;  // guest address written
  uint32_t value;  // the written word (4-byte aligned) after the write
};
struct WriteWatch {
  std::atomic<bool> on{false};
  uintptr_t lo = 0, hi = 0;          // host pages protected
  uint32_t table = 0, count = 0;     // guest table, entries
  uint8_t* base = nullptr;
  uint32_t host_offset = 0;          // the physical heap's (GuestBytes)
  uintptr_t image = 0;
  static constexpr uint32_t kMax = 8192;
  PageWrite writes[kMax];
  std::atomic<uint32_t> n{0};
  std::atomic<uint32_t> faults{0};
  PVOID handler = nullptr;
  static constexpr int kWatchSeconds = 60;
} g_write_watch;
thread_local uintptr_t t_watch_page = 0;
thread_local uint32_t t_watch_slot = ~0u;

LONG CALLBACK WriteWatchHandler(EXCEPTION_POINTERS* e) {
  auto& w = g_write_watch;
  const DWORD code = e->ExceptionRecord->ExceptionCode;
  if (code == EXCEPTION_SINGLE_STEP && t_watch_page) {
    DWORD old;
    VirtualProtect(reinterpret_cast<void*>(t_watch_page), 4096,
                   w.on.load() ? PAGE_READONLY : PAGE_READWRITE, &old);
    if (t_watch_slot != ~0u) {
      PageWrite& r = w.writes[t_watch_slot];
      // The word written (4-byte aligned), after the write.
      r.value = WheelBeU32(GuestBytes(w.base, r.guest & ~3u));
    }
    t_watch_page = 0;
    t_watch_slot = ~0u;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (code != EXCEPTION_ACCESS_VIOLATION || e->ExceptionRecord->NumberParameters < 2 ||
      e->ExceptionRecord->ExceptionInformation[0] != 1)
    return EXCEPTION_CONTINUE_SEARCH;
  const uintptr_t at = e->ExceptionRecord->ExceptionInformation[1];
  if (at < w.lo || at >= w.hi) return EXCEPTION_CONTINUE_SEARCH;
  w.faults.fetch_add(1, std::memory_order_relaxed);
  const uintptr_t page = at & ~uintptr_t(4095);
  DWORD old;
  VirtualProtect(reinterpret_cast<void*>(page), 4096, PAGE_READWRITE, &old);
  const uint32_t guest = uint32_t(at - reinterpret_cast<uintptr_t>(w.base) - w.host_offset);
  t_watch_slot = ~0u;
  if (guest >= w.table && guest < w.table + w.count * 64) {
    const uint32_t i = w.n.fetch_add(1, std::memory_order_relaxed);
    if (i < WriteWatch::kMax) {
      w.writes[i] = {uint64_t(e->ContextRecord->Rip - w.image), guest, 0};
      t_watch_slot = i;
    }
  }
  t_watch_page = page;
  e->ContextRecord->EFlags |= 0x100;  // trap flag: back here after the write
  return EXCEPTION_CONTINUE_EXECUTION;
}

void WriteWatchProtect(DWORD protect) {
  auto& w = g_write_watch;
  DWORD old;
  for (uintptr_t p = w.lo; p < w.hi; p += 4096)
    VirtualProtect(reinterpret_cast<void*>(p), 4096, protect, &old);
}

void WriteWatchStart(uint8_t* base, uint32_t table, uint32_t count) {
  auto& w = g_write_watch;
  if (w.handler || !count) return;  // once a run
  w.base = base;
  w.table = table;
  w.count = count;
  w.image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  w.host_offset = uint32_t(GuestBytes(base, table) - (base + table));
  w.lo = reinterpret_cast<uintptr_t>(GuestBytes(base, table)) & ~uintptr_t(4095);
  w.hi = (reinterpret_cast<uintptr_t>(GuestBytes(base, table) + count * 64) + 4095) & ~uintptr_t(4095);
  w.handler = AddVectoredExceptionHandler(1, WriteWatchHandler);
  w.on = true;
  WriteWatchProtect(PAGE_READONLY);
  REXLOG_INFO("PGR4 lightmap write probe: table {:08X}, {} entries, {} host pages read-only for {} s",
              table, count, (w.hi - w.lo) / 4096, WriteWatch::kWatchSeconds);
  std::thread([] {
    std::this_thread::sleep_for(std::chrono::seconds(WriteWatch::kWatchSeconds));
    g_write_watch.on = false;
    WriteWatchProtect(PAGE_READWRITE);
    REXLOG_INFO("PGR4 lightmap write probe: stopped ({} faults, {} table writes)",
                g_write_watch.faults.load(), g_write_watch.n.load());
  }).detach();
}

// The writes noted since the last report: by instruction and field, then those to `pages`.
void WriteWatchReport(std::initializer_list<uint32_t> pages) {
  auto& w = g_write_watch;
  static uint32_t reported = 0;
  const uint32_t n = std::min(w.n.load(), WriteWatch::kMax);
  if (n == reported) return;
  std::map<std::pair<uint64_t, uint32_t>, uint32_t> by;  // (rva, field offset) -> writes
  std::string watched;
  for (uint32_t i = reported; i < n; ++i) {
    const PageWrite& r = w.writes[i];
    const uint32_t entry = (r.guest - w.table) / 64, field = (r.guest - w.table) % 64;
    ++by[{r.rva, field}];
    for (const uint32_t p : pages)
      if (entry == p && watched.size() < 600) {
        char s[80];
        std::snprintf(s, sizeof(s), " [%u+%u by %llX -> %08X]", entry, field,
                      (unsigned long long)r.rva, r.value);
        watched += s;
      }
  }
  std::string sites;
  for (const auto& [k, c] : by) {
    if (sites.size() > 900) break;
    char s[64];
    std::snprintf(s, sizeof(s), " %llX+%u x%u", (unsigned long long)k.first, k.second, c);
    sites += s;
  }
  REXLOG_INFO("PGR4 lightmap write probe: {} table writes ({} faults); by code+field:{}; watched:{}",
              n - reported, w.faults.load(), sites, watched);
  reported = n;
}
// Page loads given up (pgr4_lightmap_probe): sub_82384598 loads a page into its entry's back half
// (+0x20) and, failing, sets it free with sub_8237E7A8(back half, 2); sub_822B4818(size, &out, pool)
// is its allocation (pools 1-4), null when the pool is full.
struct LoadFailures {
  std::map<uint64_t, uint32_t> given_up;     // entry << 32 | caller -> times
  std::map<uint64_t, uint32_t> alloc_failed;  // pool << 32 | size -> times
} g_load_failures;
}  // namespace
REX_EXTERN(__imp__sub_8237E7A8);
REX_HOOK_RAW(sub_8237E7A8) {
  const uint32_t half = ctx.r3.u32, state = ctx.r4.u32, caller = uint32_t(ctx.lr);
  __imp__sub_8237E7A8(ctx, base);
  if (!REXCVAR_GET(pgr4_lightmap_probe) || state != 2) return;
  std::lock_guard lock(g_lightmap_probe.mutex);
  const uint32_t world = g_lightmap_probe.world;
  const uint32_t table = world ? WheelBeU32(base + world + 0x28) : 0;
  const uint32_t entry = table && half >= table + 0x20 ? (half - 0x20 - table) / 64 : 0xFFFFFFFF;
  ++g_load_failures.given_up[uint64_t(entry) << 32 | caller];
}
REX_EXTERN(__imp__sub_822B4818);
REX_HOOK_RAW(sub_822B4818) {
  const uint32_t size = ctx.r3.u32, pool = ctx.r5.u32;
  __imp__sub_822B4818(ctx, base);
  if (!REXCVAR_GET(pgr4_lightmap_probe) || ctx.r3.u32 != 0) return;
  std::lock_guard lock(g_lightmap_probe.mutex);
  ++g_load_failures.alloc_failed[uint64_t(pool) << 32 | size];
}
// Local change (19/11/2026, Tazz: Shanghai's far buildings flashing white with ENHANCED DETAILS high
// or ultra, with Xenos too): the scene's textures and lightmaps stream into one heap, 0x82A80A7C,
// made at boot by sub_822B4D88 with a fixed 147 MB (sub_822B5A58(heap, 4 KB blocks, 0x9300000):
// sub_822B50C8 -> sub_8230BEB0, one contiguous physical allocation, 64 KB aligned; the game stops
// if it fails). Forced to the scenery's highest LOD (10, needing 139.3 MB by the game's own table,
// world +0xEC), Shanghai asks for more than the heap holds: page loads (sub_82384598) fail on
// allocation, a blinking sign's second lightmap (page 15279) never loads, and sub_823B3A78's
// fallback (no texture) draws the building at full light, times 6: white. Measured in a race: the
// emulated console's physical memory keeps 207 MB never given out (132 MB in the menus).
// pgr4_scene_texture_pool_extra_mb adds that many MB to the heap at its creation; everything else
// reads its size from the heap (+0x3C: sub_823D67A0's LOD choice, sub_823C67C8's streaming budget).
// Read at boot only. 96 (243 MB): two players' split screen asks 187.8 MB for LOD 10 by the game's
// table; measured in Shanghai and Quebec at two in storms, peaks of 215.8 and 214.5 MB, no failed
// allocation; the physical memory left is then 36 MB in the menus, 110 MB in a race (19/11/2026).
REXCVAR_DEFINE_INT32(pgr4_scene_texture_pool_extra_mb, 96, "PGR4",
                     "MB added to the scene's texture heap (the game's 147 MB) at boot: more "
                     "scenery detail loaded without running out (0: the game's size)")
    .range(0, 96)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REX_EXTERN(__imp__sub_822B5A58);
REX_HOOK_RAW(sub_822B5A58) {
  const uint32_t heap = ctx.r3.u32;
  const int32_t extra = std::clamp(REXCVAR_GET(pgr4_scene_texture_pool_extra_mb), 0, 96);
  const bool grow = heap == 0x82A80A7C && ctx.r5.u32 == 0x9300000 && extra > 0;
  if (grow) ctx.r5.u64 = 0x9300000u + (uint32_t(extra) << 20);
  __imp__sub_822B5A58(ctx, base);
  if (heap == 0x82A80A7C)
    REXLOG_INFO("PGR4 scene texture heap: {:.0f} MB at {:08X} (the game's 147 MB{})",
                WheelBeU32(base + heap + 0x3C) / 1048576.0, WheelBeU32(base + heap + 4),
                grow ? fmt::format(", plus {} MB", extra) : std::string());
}

// Local change (19/11/2026, Tazz: the brake lights flicker in replays). sub_822E5BF8 (method 0x18
// of a car's table, 0x82002740: sub_822DC398 on *(car +808)) sets the car's controls to zero:
// pedals and steering 0.0, buttons 0. sub_824C6398, the frame's simulation, calls it twice for the
// cars whose byte +0x3C is set (their controls fed from outside: the replay's cars): once at the
// frame's start in sub_82303770 (return address 0x823037FC), then at each physics step in
// sub_82303370, right before sub_824F3010 decodes the step's recorded controls (sub_824E2A40).
// At the console's 30 FPS every frame ran two steps, so the frame's clear was always followed by a
// step's clear and decode. Above 60 FPS a frame often runs none: the controls stayed zero for the
// frame, and sub_82445CB8 drew the brake lights from a 0 pedal (off one frame in three at 95 FPS,
// measured). pgr4_replay_controls_hold skips the frame's clear when the frame runs no step (the
// game's test, (n - 1) h <= t - x, n at 0x82A17644, x 0x82A17640, h 0x82001170, t 0x82A81334):
// the controls stay the last step's, as on the console between two steps. Frames with steps are
// unchanged. Read at each call.
REXCVAR_DEFINE_BOOL(pgr4_replay_controls_hold, true, "PGR4",
                    "Replays: the cars' controls (brake, steering) kept on frames without a physics "
                    "step instead of cleared (the brake lights flickered above 60 FPS)");
REX_EXTERN(__imp__sub_822E5BF8);
REX_HOOK_RAW(sub_822E5BF8) {
  if (uint32_t(ctx.lr) == 0x823037FC && REXCVAR_GET(pgr4_replay_controls_hold)) {
    const uint8_t* np = base + 0x82A17644;
    const int32_t n =
        int32_t(uint32_t(np[0]) << 24 | uint32_t(np[1]) << 16 | uint32_t(np[2]) << 8 | np[3]);
    const double h = double(BeF(base + 0x82001170));
    const float due = float(double(int64_t(n - 1)) * h);  // as sub_824C6398 rounds it
    if (!(due <= BeF(base + 0x82A81334) - BeF(base + 0x82A17640))) return;  // no step this frame
  }
  if (REXCVAR_GET(pgr4_vehicle_lights_probe)) {
    static std::mutex m;
    static std::set<uint64_t> seen;
    const uint32_t caller = uint32_t(ctx.lr), object = ctx.r3.u32;
    std::lock_guard lock(m);
    if (seen.insert(uint64_t(caller) << 32 | WheelBeU32(base + object)).second)
      REXLOG_INFO("PGR4 controls clear: from {:08X}, object {:08X} (table {:08X}), controls {:08X}",
                  caller, object, WheelBeU32(base + object), WheelBeU32(base + object + 808));
  }
  __imp__sub_822E5BF8(ctx, base);
}

REX_EXTERN(__imp__sub_823B3A78);
REX_HOOK_RAW(sub_823B3A78) {
  const uint32_t world = ctx.r3.u32, record = ctx.r4.u32, caller = uint32_t(ctx.lr);
  __imp__sub_823B3A78(ctx, base);
  if (!REXCVAR_GET(pgr4_lightmap_probe)) return;
  auto& p = g_lightmap_probe;
  std::lock_guard lock(p.mutex);
  ++p.calls;
  p.world = world;
  if (ctx.r3.u32 == world + 0x98) {
    ++p.fallbacks;
    const uint64_t page = uint32_t(base[record + 2]) << 8 | base[record + 3];
    ++p.missing[page << 32 | caller];
  }
  // The page watch on the table of the world whose fallback was returned (sub_823B3A78 serves several
  // worlds: the first one seen held 2520 entries rewritten every frame, not the scene's).
  if (ctx.r3.u32 == world + 0x98 && REXCVAR_GET(pgr4_lightmap_write_probe) &&
      (uint32_t(base[record + 2]) << 8 | base[record + 3]) == 15279)
    WriteWatchStart(base, WheelBeU32(base + world + 0x28), WheelBeU32(base + world + 0x20));
  const auto now = std::chrono::steady_clock::now();
  if (now - p.last < std::chrono::seconds(2)) return;
  p.last = now;
  const uint32_t table = WheelBeU32(base + world + 0x28);
  if (REXCVAR_GET(pgr4_lightmap_write_probe))
    WriteWatchReport({15279, 11654, 12608, 17811, 17815, 17817});
  std::string pages;
  int shown = 0;
  for (const auto& [key, n] : p.missing) {
    if (shown++ == 12) break;
    const uint32_t page = uint32_t(key >> 32);
    const uint8_t* e = base + table + page * 64;
    char s[128];
    std::snprintf(s, sizeof(s), " %u from %08X (x%u: %02X%02X %02X%02X %08X %08X %08X)", page,
                  uint32_t(key), n, e[0], e[1], e[2], e[3], WheelBeU32(e + 4), WheelBeU32(e + 8),
                  WheelBeU32(e + 16));
    pages += s;
  }
  // 0x82A1BCE0: the lightmap set sub_823B44F8's case 0xCC picks (its record's 2 * set + 1).
  // The guest's physical memory not given out (the emulator's physical heap, 4 KB pages), the
  // scene's texture pool (heap 0x82A80A7C: size +0x3C, in use +0x40, peak +0x44) and the
  // world's highest scenery LOD (+0x728, sub_823D67A0's choice unless ENHANCED DETAILS forces it).
  auto* physical = rex::system::kernel_memory()->GetPhysicalHeap();
  const double free_mb = double(physical->GetUnreservedPageCount()) * physical->page_size() / 1048576.0;
  REXLOG_INFO("PGR4 lightmap probe: {} lookups, {} fallbacks, {} missing; fallback texture {:08X}; "
              "set {}; physical free {:.1f} of {:.0f} MB; pool {:.1f}/{:.1f} MB (peak {:.1f}); max LOD {};{}",
              p.calls, p.fallbacks, p.missing.size(), WheelBeU32(base + world + 0x98),
              int32_t(WheelBeU32(base + 0x82A1BCE0)), free_mb,
              double(physical->GetTotalPageCount()) * physical->page_size() / 1048576.0,
              WheelBeU32(base + 0x82A80AB8) / 1048576.0, WheelBeU32(base + 0x82A80ABC) / 1048576.0,
              WheelBeU32(base + 0x82A80AC0) / 1048576.0,
              WheelBeU32(base + WheelBeU32(base + 0x82A985E8) + 0x728), pages);
  p.calls = p.fallbacks = 0;
  p.missing.clear();
  auto& f = g_load_failures;
  if (!f.given_up.empty() || !f.alloc_failed.empty()) {
    std::string s;
    for (const auto& [k, n] : f.given_up) {
      if (s.size() > 700) break;
      char t[64];
      std::snprintf(t, sizeof(t), " page %d from %08X x%u", int32_t(k >> 32), uint32_t(k), n);
      s += t;
    }
    s += "; allocations failed:";
    for (const auto& [k, n] : f.alloc_failed) {
      if (s.size() > 1400) break;
      char t[64];
      std::snprintf(t, sizeof(t), " pool %u size %u x%u", uint32_t(k >> 32), uint32_t(k), n);
      s += t;
    }
    REXLOG_INFO("PGR4 lightmap probe: loads given up:{}", s);
    f.given_up.clear();
    f.alloc_failed.clear();
  }
}
REX_EXTERN(__imp__sub_82445CB8);
// The vehicle's physics and where it keeps its velocities (pgr4_character_accel_steps, below).
void Pgr4NoteVehiclePhysics(const uint8_t* base, uint32_t pose, uint32_t physics);
REX_HOOK_RAW(sub_82445CB8) {
  const uint32_t pose = ctx.r3.u32;
  Pgr4NoteVehiclePhysics(base, pose, ctx.r4.u32);
  // Its fraction, as it computes it (g_pose_time above). In the menus the update comes before the
  // frame's physics steps: the fraction is mostly below its range, the pose a step's.
  const float x = BeF(base + 0x82A17640), t = BeF(base + 0x82A8131C + 24),
              h = BeF(base + 0x82001170), lo = BeF(base + 0x82258A6C), hi = BeF(base + 0x8218BCC0);
  const uint8_t* np = base + 0x82A17644;
  const int32_t n = int32_t(uint32_t(np[0]) << 24 | uint32_t(np[1]) << 16 | uint32_t(np[2]) << 8 | np[3]);
  const float fraction =
      base[0x82A81378] == 0 ? 0.0f : x > 0 ? (x + float(n - 1) * h - t) / x : 0.0f;
  const bool extend = REXCVAR_GET(pgr4_vehicle_pose_extend) && base[0x82A81378] != 0 &&
                      (fraction < lo || fraction > hi) && fraction >= ExtendLow() &&
                      fraction <= kExtendHigh;
  const bool mixed = extend || (!(fraction < lo) && !(fraction > hi));
  // A car's (physics +16 == 0: sub_82445CB8's wheel loop at 0x824460F8; a bike's goes elsewhere).
  const bool car_wheels = WheelBeU32(base + ctx.r4.u32 + 16) == 0;
  t_pose_extend = extend;
  const uint32_t physics = ctx.r4.u32;
  __imp__sub_82445CB8(ctx, base);
  t_pose_extend = false;
  // Diagnostic (19/11/2026, Tazz: the brake and reverse lights flicker in replays): what
  // sub_82445CB8 based the lights on, each call: its mode (physics +20; 1: the brake through
  // sub_82446D78's average of its last calls, *(pose +944): ring at +0, count +8), the brake
  // pedal (*(physics +808) +8, against f29), the brake lights' flag it wrote (pose +414, the car
  // record's +0x47E), the gear (physics +2702 into pose +292) and the frame's physics steps.
  if (car_wheels && REXCVAR_GET(pgr4_vehicle_lights_probe)) {
    const uint32_t filter = WheelBeU32(base + pose + 944);
    float average = -1.0f;
    if (filter >= 0x00010000) {
      const uint32_t ring = WheelBeU32(base + filter), count = WheelBeU32(base + filter + 8);
      if (ring && count > 0 && count <= 256) {
        float sum = BeF(base + filter + 16);
        for (uint32_t i = 0; i < count; ++i) sum += BeF(base + ring + 4 * i);
        average = sum / float(count);
      }
    }
    REXLOG_INFO("PGR4 lights probe: frame {} pose {:08X} steps {} mode {} pedal {:.3f} average {:.3f} "
                "brake flag {} gear {}",
                g_frame_no, pose, n, int32_t(WheelBeU32(base + physics + 20)),
                BeF(base + WheelBeU32(base + physics + 808) + 8), average, base[pose + 414],
                int32_t(int8_t(base[physics + 2702])));
    // pgr4_vehicle_pedal_watch: the page watch (WriteWatchStart) on this car's controls
    // (*(physics +808), 64 bytes) from the first time its pedal falls from braking to 0 on a call
    // without a new physics step (seen in replays only), to find what sets it back to 0.
    const uint32_t controls = WheelBeU32(base + physics + 808);
    static std::unordered_map<uint32_t, std::pair<float, int32_t>> last_pedal;  // pose -> pedal, step
    const float pedal = BeF(base + controls + 8);
    auto& lp = last_pedal[pose];
    if (REXCVAR_GET(pgr4_vehicle_pedal_watch) && lp.first > 0.5f && pedal == 0.0f && lp.second == n) {
      WriteWatchStart(base, controls, 1);
      REXLOG_INFO("PGR4 pedal watch: controls {:08X} of pose {:08X} (pedal {:.3f} -> 0 at step {})",
                  controls, pose, lp.first, n);
    }
    lp = {pedal, n};
    static auto last_report = std::chrono::steady_clock::now();
    if (REXCVAR_GET(pgr4_vehicle_pedal_watch) &&
        std::chrono::steady_clock::now() - last_report > std::chrono::seconds(2)) {
      last_report = std::chrono::steady_clock::now();
      WriteWatchReport({0});
    }
  }
  // Local change: the wheels' spin at the body's time (pgr4_vehicle_wheel_spin_mix).
  if (car_wheels) MixWheelSpin(base, pose, n, mixed ? fraction : fraction < lo ? 0.0f : 1.0f);
  // Development (pgr4_vehicle_pose_probe): a bike's wheels (sub_82445CB8's branch at 0x82446298:
  // the spin carried on at each call, +84 += +88 times the game's step at 0x82A81330; the place
  // +0 and the steering +80 from the last step's pose), each call.
  if (!car_wheels && REXCVAR_GET(pgr4_vehicle_pose_probe) && g_pose_probe_left > 0) {
    const uint32_t array = WheelBeU32(base + pose + 960), wheels = WheelBeU32(base + pose + 968);
    if (array >= 0x40000000 && wheels && wheels <= 4)
      for (uint32_t i = 0; i < wheels && i < 2; ++i) {
        const uint8_t* e = base + array + 160 * i;
        REXLOG_INFO("PGR4 pose probe: frame {} bike pose {:08X} wheel {} step {} fraction {:.4f}: "
                    "spin +84 {:.4f} rate +88 {:.3f} dt {:.5f}, steering +80 {:.4f}, place +0 {:.4f} "
                    "{:.4f} {:.4f}, +16 {:.4f} {:.4f} {:.4f}",
                    g_frame_no, pose, i, n, fraction, BeF(e + 84), BeF(e + 88),
                    BeF(base + 0x82A81330), BeF(e + 80), BeF(e), BeF(e + 4), BeF(e + 8), BeF(e + 16),
                    BeF(e + 20), BeF(e + 24));
      }
  }
  const double last_step = double(x) + double(n - 1) * double(h);
  g_pose_time[pose] = {mixed            ? double(t)
                       : fraction < lo  ? last_step
                                        : last_step - double(x),
                       double(t), g_frame_no};
  if (REXCVAR_GET(pgr4_vehicle_pose_probe) && g_pose_probe_left > 0) {
    --g_pose_probe_left;
    REXLOG_INFO("PGR4 pose probe: frame {} game interpolation sub_82445CB8 into {:08X}: fraction "
                "{:.4f}, range {:.4f} to {:.4f}, {}", g_frame_no, pose, fraction, lo, hi,
                extend ? "mixed past its range" : mixed ? "mixed" : "a step's pose");
  }
}
// sub_82445CB8's range bounds (lfs f0 of 0x82258A6C's 0.001 at 0x82445DE8, of 0x8218BCC0's 0.999
// at 0x82445E2C, each compared with the fraction f31 next), out of the way while t_pose_extend.
void Pgr4PoseLowBoundHook(PPCRegister& f0) {
  if (t_pose_extend) f0.f64 = double(ExtendLow()) - 1.0;
}
void Pgr4PoseHighBoundHook(PPCRegister& f0) {
  if (t_pose_extend) f0.f64 = double(kExtendHigh) + 1.0;
}
REX_EXTERN(sub_82417B50);
REX_EXTERN(__imp__sub_82417B50);
REX_HOOK_RAW(sub_82417B50) {
  const uint32_t record = ctx.r3.u32;
  PoseProbe(base, "extrapolated (sub_82417B50)", record, ctx.f1.f64, ctx.lr);
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
extern std::atomic<uint32_t> g_pgr4_view_mode;  // draw.cpp: development, the volumetric fog's probe
namespace pgr4::native {
void NoteGameCameraCut(const float eye[3], const char* why, bool again);  // pgr4-native upscaler.cpp
}
namespace {
// The view camera's cuts (local addition, 18/11/2026; --pgr4_native_game_camera_cuts), read
// around its update: its mode (+496, the last update's at +500) or its sub-camera (+516, +520)
// changing, or the replay's zone switch byte (0x82C922CC: 1 at a change of zone; a change of
// sub-camera within a zone does not set it). Compared with this hook's own last values too, as
// the game may compare and copy within the update. The eye (+16) goes to the renderer.
uint32_t g_cut_mode = 0, g_cut_sub = 0;
bool g_cut_zone = false, g_cut_known = false;
uint64_t g_cut_frame = UINT64_MAX;  // the game frame of the last cut
uint32_t BeU32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void NoteCameraCut(const uint8_t* base, bool zone_before) {
  constexpr uint32_t kView = 0x82C91D00, kZoneSwitch = 0x82C922CC;
  const uint8_t* c = base + kView;
  const uint32_t mode = BeU32(c + 496), last_mode = BeU32(c + 500);
  g_pgr4_view_mode.store(mode, std::memory_order_relaxed);  // development: the fog probe
  const uint32_t sub = BeU32(c + 516), last_sub = BeU32(c + 520);
  const bool zone = zone_before || base[kZoneSwitch] == 1;
  const char* why = nullptr;
  if (g_cut_known) {
    if (mode != g_cut_mode || mode != last_mode) why = "mode";
    else if (zone && !g_cut_zone) why = "zone";
    else if (sub != g_cut_sub || sub != last_sub) why = "sub-camera";
  }
  static int logged = 0;  // local diagnostic
  if (why && logged < 400) {
    ++logged;
    REXLOG_INFO("PGR4 camera cut: frame {} {} (mode {} -> {}, game's last {}; sub-camera {:08X} "
                "-> {:08X}, game's last {:08X}; zone byte {})",
                g_frame_no, why, g_cut_mode, mode, last_mode, g_cut_sub, sub, last_sub, int(zone));
  }
  const float eye[3] = {BeF(c + 16), BeF(c + 20), BeF(c + 24)};
  if (why) {
    pgr4::native::NoteGameCameraCut(eye, why, false);
    g_cut_frame = g_frame_no;
  } else if (g_cut_frame == g_frame_no) {
    // Updated again in the cut's frame (a replay's zone change was seen to move the eye twice).
    static int again_logged = 0;
    if (again_logged < 100) {
      ++again_logged;
      REXLOG_INFO("PGR4 camera cut: frame {} camera updated again, eye {:.2f} {:.2f} {:.2f}",
                  g_frame_no, eye[0], eye[1], eye[2]);
    }
    pgr4::native::NoteGameCameraCut(eye, "", true);
  }
  g_cut_mode = mode;
  g_cut_sub = sub;
  g_cut_zone = zone;
  g_cut_known = true;
}
}  // namespace
REXCVAR_DECLARE(bool, pgr4_native_game_camera_cuts);
REX_HOOK_RAW(sub_827D4688) {
  const uint32_t camera = ctx.r3.u32;
  uint8_t saved[8];
  const bool free_camera = Pgr4FreeCamBefore(base, camera, saved);
  const bool cuts = camera == 0x82C91D00 && !free_camera && REXCVAR_GET(pgr4_native_game_camera_cuts);
  const bool zone_before = cuts && base[0x82C922CC] == 1;
  __imp__sub_827D4688(ctx, base);
  if (free_camera) Pgr4FreeCamAfter(base, camera, saved);
  if (cuts) {
    NoteCameraCut(base, zone_before);
  } else if (camera == 0x82C91D00) {
    // Local change (24/11/2026): the view mode kept without the cuts too (draw.cpp
    // pgr4_native_car_shadow_fog_views reads it), as NoteCameraCut stores it.
    g_pgr4_view_mode.store(BeU32(base + 0x82C91D00 + 496), std::memory_order_relaxed);
  }
}
REX_EXTERN(__imp__sub_8264A358);
namespace pgr4::native {
void PrewarmAtRaceLoad();  // pgr4-native draw.cpp (pgr4_native_prewarm_relaxed)
}
REX_HOOK_RAW(sub_8264A358) {
  Pgr4RaceWeather(base);
  Pgr4AutopilotRouteString(base, ctx.r4.u32);
  // Local change (22/11/2026): the loading waits for the prewarm's end (its fast mode), so the race
  // starts with nothing left to prepare.
  pgr4::native::PrewarmAtRaceLoad();
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
  PoseProbe(base, ctx.r4.u32 ? "given pose (sub_82416BA0)" : "step pose (sub_82416BA0)", record,
            0.0, ctx.lr);
  // Local change (18/11/2026): the drawn pose carried to the game's time (pgr4_vehicle_pose_ahead),
  // from the time of the pose sub_82445CB8 wrote at +736 this frame or the last (g_pose_time): a
  // step's carried by the time since it, a mixed one (already at the game's time) left the game's.
  // No such pose: the game's.
  const auto pose_time = g_pose_time.find(record + 736);
  if (!ctx.r4.u32 && record >= 0x40000000 && !g_pose_carrying &&
      REXCVAR_GET(pgr4_vehicle_pose_ahead) && pose_time != g_pose_time.end() &&
      pose_time->second.frame + 1 >= g_frame_no) {
    const float t = BeF(base + 0x82A8131C + 24), h = BeF(base + 0x82001170);
    const uint8_t* np = base + 0x82A17644;
    const int32_t n =
        int32_t(uint32_t(np[0]) << 24 | uint32_t(np[1]) << 16 | uint32_t(np[2]) << 8 | np[3]);
    const double ahead = pose_time->second.target - pose_time->second.time;
    // Back too: in a race the update runs at each physics step, and at the frame's last the
    // physics is often ahead of the game's time (measured -0.5 to -17 ms); carried forward only,
    // those frames kept the step's pose and every car shook between the two (Tazz, 18/11/2026).
    if (h > 0 && std::abs(ahead) > 1e-6 && ahead >= -2.0 * h && ahead <= 3.0 * h) {
      const PPCContext saved = ctx;
      ctx.f1.f64 = ahead;
      g_carry_r5 = saved.r5.u64;
      g_pose_carrying = true;
      sub_82417B50(ctx, base);
      g_pose_carrying = false;
      ctx = saved;
      if (REXCVAR_GET(pgr4_vehicle_pose_probe) && g_pose_probe_left > 0) {
        const uint8_t* step = base + record + 736 + 48;
        const uint8_t* drawn = base + record + 12560 + 48;
        const uint8_t* vel = base + record + 880;
        REXLOG_INFO("PGR4 pose probe: frame {} car {:08X} ahead {:.5f} n {} t {:.5f} step {:.3f} {:.3f} "
                    "{:.3f} drawn {:.3f} {:.3f} {:.3f} vel {:.3f} {:.3f} {:.3f}",
                    g_frame_no, record, ahead, n, t, BeF(step), BeF(step + 4), BeF(step + 8),
                    BeF(drawn), BeF(drawn + 4), BeF(drawn + 8), BeF(vel), BeF(vel + 4), BeF(vel + 8));
      }
      return;
    }
  }
  if (ctx.r4.u32) {
    if (g_pose_carrying) ctx.r5.u64 = g_carry_r5;  // the update's, not sub_82417B50's preview
    __imp__sub_82416BA0(ctx, base);
    return;
  }
  {
    SteadyBody steady(base, record, 0.0f);
    __imp__sub_82416BA0(ctx, base);
  }
  if (record >= 0x40000000) g_drawn_records[record] = g_frame_no;
}

// The carried pose's wheels (local change, 18/11/2026). sub_82416BA0 updates each wheel (its
// +392 of 992 bytes at +384) with sub_82441040(wheel, tracks, pose): given no pose, tracks is the
// game's (1, or 0 while byte +61 of 0x82A235D8 is set with its +56 mode not 0) and the wheel lays
// its skid marks (sub_8243FC38, Vehicle.Effects.Tracks); given one, as for the carried pose, 0:
// no skid marks (Tazz, 18/11/2026). The carried call's wheels get the game's own value.
REX_EXTERN(__imp__sub_82441040);
REX_HOOK_RAW(sub_82441040) {
  if (g_pose_carrying && ctx.r5.u32 != 0) {
    const uint8_t* state = base + 0x82A235D8;
    const uint32_t mode = uint32_t(state[56]) << 24 | uint32_t(state[57]) << 16 |
                          uint32_t(state[58]) << 8 | state[59];
    ctx.r4.u64 = (mode == 0 || state[61] == 0) ? 1 : 0;
  }
  // The probe: what each path places the wheel from, its parent's (+748) +800 without a pose, the
  // pose given with one, against the parent's step (+736) and drawn (+12560) poses.
  if (REXCVAR_GET(pgr4_vehicle_pose_probe) && g_pose_probe_left > 0) {
    --g_pose_probe_left;
    const uint8_t* w = base + ctx.r3.u32;
    const uint32_t parent = uint32_t(w[748]) << 24 | uint32_t(w[749]) << 16 |
                            uint32_t(w[750]) << 8 | w[751];
    const uint8_t* pr = base + parent;
    const uint8_t* given = ctx.r5.u32 ? base + ctx.r5.u32 : nullptr;
    REXLOG_INFO("PGR4 pose probe: frame {} wheel {:08X} {} tracks {} parent {:08X}: +800 {:.3f} "
                "{:.3f} {:.3f}, +736 {:.3f} {:.3f} {:.3f}, +12560 {:.3f} {:.3f} {:.3f}, given {:.3f} "
                "{:.3f} {:.3f}",
                g_frame_no, ctx.r3.u32, g_pose_carrying ? "carried" : "game's", int(ctx.r4.u32),
                parent, BeF(pr + 800 + 48), BeF(pr + 800 + 52), BeF(pr + 800 + 56),
                BeF(pr + 736 + 48), BeF(pr + 736 + 52), BeF(pr + 736 + 56), BeF(pr + 12560 + 48),
                BeF(pr + 12560 + 52), BeF(pr + 12560 + 56), given ? BeF(given + 48) : 0.0f,
                given ? BeF(given + 52) : 0.0f, given ? BeF(given + 56) : 0.0f);
  }
  __imp__sub_82441040(ctx, base);
}

// The drivers' and riders' ragdolls at any frame rate (local change, 18/11/2026, Tazz: the driver
// jumps in a replay's cockpit view, more with FSR FG). Both were tuned on the console's 30 frames a
// second, two physics steps of 1/60 s a frame; probe in a replay at 86 frames a second:
// - the acceleration a character gets from its vehicle (sub_8249C3E0, called once a frame): its
//   velocities (+112, +124 of the record at +116) minus the last call's (+48, +64 of r3), over the
//   frame's time (+132). The velocities move with the physics steps, not with the frames: the
//   driver got 0 to 19.7 m/s^2 frame to frame for an even 8.8 (then times
//   Character.Ragdoll.VehicleForceScale, 18). Then mixed into the last by
//   Character.Ragdoll.VehicleAccelSmoothFactor (0.7, f1) a call: a call's weight, so the
//   smoothing's time grew shorter with the frame rate.
//   pgr4_character_accel_window: the acceleration over the console's frame, the velocities now
//   less those 1/30 s ago (a character's own history, by its time), handed to the game through
//   the last call's velocities (v - a dt); the weight as a time, 1 - (1 - f1)^(dt 30) (0.7 at 30
//   frames a second, 0.34 at 86). The game's forces, noise and scale are its own.
// - the ragdoll's step (sub_824B6438): 1 / (Character.Ragdoll.NumStepsPerFrame x 30) s a call,
//   the frame's real time it is given (f1) unused: 2.9 times too fast at 86 frames a second.
//   pgr4_character_ragdoll_time: the 30 it loads (0x821AC658, at 0x824B6480) made 1 / f1, so a
//   call steps the frame's time (Pgr4RagdollRateHook); at most 1/30 s (the console's, under 30
//   frames a second), at least 1 ms.
// Both on by default (Tazz, 18/11/2026: « le pilote c'est bon ça marche »). Measured in a replay's
// cockpit view, the smoothed acceleration's change frame to frame: 35 % of it at 73 frames a
// second without, 9 % with both; 24 % and 16 % at 30.
REXCVAR_DEFINE_BOOL(pgr4_character_accel_window, true, "PGR4",
                    "Characters' ragdolls: the vehicle's acceleration over the console's 1/30 s and "
                    "its smoothing by time, at any frame rate");
REXCVAR_DEFINE_BOOL(pgr4_character_ragdoll_time, true, "PGR4",
                    "Characters' ragdolls: each step the frame's real time instead of 1/30 s");
// Local change (20/11/2026, a player: the driver jitters in the vehicle selection at 72 frames a
// second, not at 60 or 90). pgr4_character_accel_window takes the vehicle's velocities 1/30 s ago
// between the two frame samples around then: right for velocities that move on at each frame, not
// for those that move on at the 1/60 s physics steps (a staircase). Those it got right only where
// 1/30 s is a whole number of frames (60, 90, 120 frames a second); at 72 (2.4 frames) the old
// velocity fell inside a stair and the acceleration swung with a 6-frame period (5 steps in 6
// frames), times Character.Ragdoll.VehicleForceScale (18).
// What writes them (probes and a page watch, 20/11/2026, 72 frames a second, a car and a bike, the
// menus and a race): sub_82484C60 (r4 the character's vehicle body, r5 the vehicle), called after
// the frame's vehicle update (sub_82437C18) and before the frame's steps (the race's sub_824C6398,
// the menus' sub_82639A28), writes the body's velocities (+112) as the vehicle's +880 (its velocity,
// as pgr4_freeze_car zeroes it) turned by its rows at +12560, +12576, +12592. The vehicle update
// writes those rows at every frame (238 frames of 238, none in a step: they turn with the drawn
// car). +880 is no step's either: the pose mix (sub_82445CB8, in that update) copies it once a
// frame from the physics' velocity getter (its table's +112; +912 from +108). That field is sampled
// at the end of each step's physics (sub_823058C8, both loops); that the step writes it is what
// the match below checks at every frame. So the body's velocities are a staircase turned by a
// rotation that moves on at every frame: on a frame without a step they barely moved, then jumped
// (4 to 5 times the change of the other frames).
// pgr4_character_accel_steps: their change over the console's frame (1/30 s, two steps), each part
// where it moves: rows1 v1 - rows0 v0 = rows1 (v1 - v0) + (rows1 - rows0) v0, with v1 the
// physics' velocity at the step the body has and v0 that of two steps before it, both sampled at
// the end of each step (a step counted without physics, past 6 a frame or with the steps' flag
// off, holds the one before), and rows0 the rows 1/30 s of the game's time (0x82A8131C +24)
// before, between the two frames around then. A bike's rider (sub_824817D8 -> sub_82475A30, no
// sub_82484C60) has the physics' velocity as it is: then simply v1 - v0. The angular velocities
// are the physics' as they are for both (sub_82475A30 copies +912; sub_82484C60 turns only the
// velocity): w1 - w0. Then the same at any frame rate. Only when the body's velocities are that
// (to 1e-4): else, velocities that move on only at the steps as the vehicle
// update's step count saw them, else the window over time. With pgr4_character_accel_window;
// read at each call.
REXCVAR_DEFINE_BOOL(pgr4_character_accel_steps, true, "PGR4",
                    "Characters' ragdolls: the vehicle's acceleration from its velocities at the "
                    "physics steps and its turn over time (1/30 s), the same at any frame rate");
REXCVAR_DEFINE_BOOL(pgr4_character_velocity_watch, false, "PGR4",
                    "Development, with pgr4_character_accel_probe: note which code writes the "
                    "probed character's vehicle velocities (page watch, once a run)");
REXCVAR_DEFINE_BOOL(pgr4_character_accel_probe, false, "PGR4",
                    "Development: log each character's vehicle velocity, acceleration and where it "
                    "came from, for 240 calls from when it is switched on");
namespace {
struct VelocitySample {
  double time;
  float v[3], w[3];
};
struct VelocityHistory {
  double clock = 0.0;
  std::vector<VelocitySample> samples;  // oldest first, the last 0.2 s
};
std::unordered_map<uint32_t, VelocityHistory> g_character_velocities;  // by sub_8249C3E0's r3
struct VehicleStep {
  int32_t n;  // the step count after the step
  float v[3];
  float w[3];
};
// Each vehicle's physics (sub_82445CB8's r4 for its pose, the vehicle + 736) and where that keeps
// its velocity and angular velocity: the offsets the getters at its table's +112 and +108 load
// (li r11,N; lvx128 v0,r4,r11; stvx128 v0,r0,r3; blr: 944 and 960 for table 0x82002740, 464 and
// 480 for 0x82006F00, read 20/11/2026). The step's physics writes them; sub_82445CB8 copies them
// to the vehicle's +880 and +912 once a frame.
struct VehiclePhysics {
  uint32_t physics = 0, v_at = 0, w_at = 0;
};
std::mutex g_vehicle_physics_mutex;
std::unordered_map<uint32_t, VehiclePhysics> g_vehicle_physics;  // by vehicle
struct RowsSample {
  double time;  // the game's time
  float m[9];
};
// A character's vehicle body as sub_82475A30 (its velocities copied as they are) and sub_82484C60
// (a car's velocity turned by the vehicle's rows after it) last wrote it: the vehicle, the frames,
// the vehicle's rows (the last 0.2 s of the game's time), and the vehicle's physics' velocities at
// the end of each step (the last 12).
struct BodyVehicle {
  uint32_t vehicle = 0;
  uint64_t frame = 0, rows_frame = 0;  // the last frames sub_82475A30 and sub_82484C60 wrote it
  std::vector<RowsSample> rows;
  std::vector<VehicleStep> steps;  // from its physics
};
std::mutex g_body_vehicles_mutex;
std::unordered_map<uint32_t, BodyVehicle> g_body_vehicles;  // by body
std::atomic<int32_t> g_vehicle_update_n{0};  // the step count when sub_82437C18 began
// Each character's velocities as the vehicle update's step count saw them (one sample a frame whose
// update came after a new step), and the last frame they moved on while that count did not.
struct UpdateSteps {
  std::vector<VehicleStep> samples;  // the last 8 steps
  uint64_t moved_between = 0;
};
std::unordered_map<uint32_t, UpdateSteps> g_character_update_steps;  // by sub_8249C3E0's r3
int g_accel_probe_left = 0;
int32_t StepCount(const uint8_t* base) {
  const uint8_t* np = base + 0x82A17644;
  return int32_t(uint32_t(np[0]) << 24 | uint32_t(np[1]) << 16 | uint32_t(np[2]) << 8 | np[3]);
}
uint32_t BeU32At(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
// The last sample at or before step n (none: nullptr).
const VehicleStep* VehicleStepAt(const std::vector<VehicleStep>& steps, int32_t n) {
  for (auto it = steps.rbegin(); it != steps.rend(); ++it)
    if (it->n <= n) return &*it;
  return nullptr;
}
bool SameVector(const float a[3], const float b[3]) {
  return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
}
// The rows at time t, between the two samples around it (the oldest while the history is
// shorter); the time they are at.
double RowsAt(const std::vector<RowsSample>& rows, double t, float out[9]) {
  if (rows.front().time >= t) {
    std::copy(rows.front().m, rows.front().m + 9, out);
    return rows.front().time;
  }
  size_t i = 1;
  while (i + 1 < rows.size() && rows[i].time < t) ++i;
  const RowsSample& a = rows[i - 1];
  const RowsSample& b = rows[i];
  const double f = b.time > a.time ? std::clamp((t - a.time) / (b.time - a.time), 0.0, 1.0) : 1.0;
  for (int k = 0; k < 9; ++k) out[k] = float(a.m[k] + (b.m[k] - a.m[k]) * f);
  return a.time + (b.time - a.time) * f;
}
}  // namespace
void Pgr4NoteVehiclePhysics(const uint8_t* base, uint32_t pose, uint32_t physics) {
  if (!REXCVAR_GET(pgr4_character_accel_window) || !REXCVAR_GET(pgr4_character_accel_steps)) return;
  if (pose < 0x40000000 + 736 || physics < 0x40000000) return;
  const uint32_t vehicle = pose - 736;
  std::lock_guard lock(g_vehicle_physics_mutex);
  const auto known = g_vehicle_physics.find(vehicle);
  if (known != g_vehicle_physics.end() && known->second.physics == physics) return;
  if (g_vehicle_physics.size() > 64) g_vehicle_physics.clear();  // vehicles long gone
  // The getter's load offset, as its code has it: li r11,N; lvx128 v0,r4,r11; ...; blr at +12.
  auto loaded = [&](uint32_t getter) -> uint32_t {
    if (getter < 0x82000000 || getter >= 0x83000000) return 0;
    const uint32_t li = BeU32At(base + getter), ret = BeU32At(base + getter + 12);
    return (li >> 16) == 0x3960 && ret == 0x4E800020 ? (li & 0xFFFF) : 0;
  };
  const uint32_t table = BeU32At(base + physics);
  VehiclePhysics& q = g_vehicle_physics[vehicle];
  q = {physics, loaded(BeU32At(base + table + 112)), loaded(BeU32At(base + table + 108))};
  if (g_accel_probe_left > 0)
    REXLOG_INFO("PGR4 accel probe: vehicle {:08X} physics {:08X} table {:08X}: velocity at +{}, "
                "angular velocity at +{}", vehicle, physics, table, q.v_at, q.w_at);
}
// Local change (20/11/2026, the frame-rate audit; replaces pgr4_character_ragdoll_time when on):
// pgr4_character_ragdoll_time stepped the ragdoll by the frame's time, one step a frame. Its
// physics is tuned on the step (read 20/11/2026): sub_824B4938 multiplies every servo's and
// limit's gain and damping by 1/h, sub_824B2760 damps the velocities by 0.99 a step, the
// constraints' biases are per step: with steps of 1/72 or 1/144 s the driver moved differently.
// pgr4_character_ragdoll_steps keeps the console's: sub_824B6438 (r3 the ragdoll, f1 the frame's
// time, one call a frame per character, on the characters' thread) runs once per 1/30 s of the
// frames' time (0, 1, 2 or more a frame, from an accumulator; 4 at most), each a step of 1/30 s as
// on the console. A frame with no time (the pause, f1 = 0) runs the game's call as it is (its
// step without the integration). For the drawing (sub_824B44F8, r3 the ragdoll: it reads each
// body's place +0 and quaternion +16, bodies at ragdoll + 23216 + 80 i, their count at +35520),
// the bodies are mixed between the last two steps by the fraction of a step the accumulator holds
// (places straight, quaternions mixed and normalized), then put back: the pose of one step before
// the frame's time, always between two steps the physics reached. Carried past the last step
// instead (as pgr4_vehicle_pose_extend carries a car), the rider jerked at each step (Tazz,
// 20/11/2026: « le ragdoll a de petites saccades »): its servos turn it every step, not on a
// straight line. The ragdoll is in its vehicle's frame (the rider's root body at 0, 0, 0 in every
// step, riding and through many falls, probe 20/11/2026), so the step's lag is only the rider's
// motion on the vehicle. Read at each call.
REXCVAR_DEFINE_BOOL(pgr4_character_ragdoll_steps, true, "PGR4",
                    "Characters' ragdolls: steps of 1/30 s as on the console (as many as the time "
                    "needs), drawn between the last two");
namespace {
constexpr uint32_t kRagdollBodies = 23216, kRagdollBodyCount = 35520, kRagdollStride = 80;
constexpr double kRagdollStep = 1.0 / 30.0;
struct RagdollSteps {
  uint32_t count = 0;
  uint64_t frame = 0;
  double time = 0.0;  // the frames' time not stepped yet
  bool has_before = false;
  std::vector<float> before, last;  // places and quaternions (7 floats a body), the last two steps
};
std::mutex g_ragdolls_mutex;
std::unordered_map<uint32_t, RagdollSteps> g_ragdolls;  // by ragdoll
uint32_t RagdollCount(const uint8_t* base, uint32_t ragdoll) {
  return std::min<uint32_t>(BeU32At(base + ragdoll + kRagdollBodyCount), 64);
}
void ReadBodies(const uint8_t* base, uint32_t ragdoll, uint32_t count, std::vector<float>& out) {
  out.resize(size_t(count) * 7);
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* b = base + ragdoll + kRagdollBodies + kRagdollStride * i;
    for (int k = 0; k < 3; ++k) out[7 * i + k] = BeF(b + 4 * k);
    for (int k = 0; k < 4; ++k) out[7 * i + 3 + k] = BeF(b + 16 + 4 * k);
  }
}
}  // namespace
void Pgr4RagdollRateHook(PPCRegister& f0, PPCRegister& f1) {
  if (REXCVAR_GET(pgr4_character_ragdoll_steps)) return;  // the game's 1/30 s
  if (!REXCVAR_GET(pgr4_character_ragdoll_time) || !(f1.f64 > 0.0)) return;
  f0.f64 = 1.0 / std::clamp(f1.f64, 0.001, 1.0 / 30.0);
}
namespace {
bool PoseProbeLine();  // pgr4_character_pose_probe (below)
}  // namespace
namespace {
void PoseStatsRagdollStep(uint32_t ragdoll, double dt, int steps);
void PoseStatsRagdollDrawn(const uint8_t* base, uint32_t ragdoll, uint32_t pose, double weight,
                           char how, float f);
void PoseStatsCharacter(const uint8_t* base, uint32_t perso, double dt);
void PoseStatsBones(uint8_t* base, uint32_t skeleton, uint32_t out, const std::vector<float>& now);
}  // namespace
REX_EXTERN(__imp__sub_824B6438);
void Pgr4CadenceCount(int i);  // the frame-rate audit's counts (below)
REX_HOOK_RAW(sub_824B6438) {
  Pgr4CadenceCount(80);
  const uint32_t ragdoll = ctx.r3.u32;
  if (PoseProbeLine())
    REXLOG_INFO("PGR4 pose probe: frame {} ragdoll step call (sub_824B6438) ragdoll {:08X} r4 {:08X} "
                "f1 {:.5f}", g_frame_no, ragdoll, ctx.r4.u32, ctx.f1.f64);
  const double dt = ctx.f1.f64;
  if (!REXCVAR_GET(pgr4_character_ragdoll_steps) || ragdoll < 0x40000000 || !(dt > 0.0)) {
    if (ragdoll >= 0x40000000) {
      std::lock_guard lock(g_ragdolls_mutex);
      if (!REXCVAR_GET(pgr4_character_ragdoll_steps)) g_ragdolls.erase(ragdoll);
    }
    if (ragdoll >= 0x40000000) PoseStatsRagdollStep(ragdoll, dt, -1);
    __imp__sub_824B6438(ctx, base);
    return;
  }
  const uint32_t count = RagdollCount(base, ragdoll);
  RagdollSteps r;
  {
    std::lock_guard lock(g_ragdolls_mutex);
    if (g_ragdolls.size() > 64) g_ragdolls.clear();  // ragdolls long gone
    r = g_ragdolls[ragdoll];
  }
  // Another body count, or none stepped for half a second: a step now, no carry until two.
  if (r.count != count || g_frame_no > r.frame + 30) r = {count, 0, kRagdollStep, false, {}, {}};
  r.frame = g_frame_no;
  r.time += std::min(dt, 0.25);
  const PPCContext in = ctx;
  int steps = 0;
  while (r.time >= kRagdollStep - 1e-9 && steps < 4) {
    ReadBodies(base, ragdoll, count, r.before);
    ctx = in;
    __imp__sub_824B6438(ctx, base);
    r.time -= kRagdollStep;
    ++steps;
  }
  if (steps == 4) r.time = std::min(r.time, kRagdollStep);  // far behind: no catching up
  if (steps) {
    ReadBodies(base, ragdoll, count, r.last);
    r.has_before = true;
  } else {
    ctx = in;
  }
  if (g_accel_probe_left > 0)
    REXLOG_INFO("PGR4 ragdoll steps: frame {} ragdoll {:08X} dt {:.5f} steps {} time left {:.5f} "
                "bodies {} first body at {:.3f} {:.3f} {:.3f}",
                g_frame_no, ragdoll, dt, steps, r.time, count, count ? r.last[0] : 0.0f,
                count ? r.last[1] : 0.0f, count ? r.last[2] : 0.0f);
  PoseStatsRagdollStep(ragdoll, dt, steps);
  std::lock_guard lock(g_ragdolls_mutex);
  g_ragdolls[ragdoll] = std::move(r);
}
// Development (20/11/2026, the AI riders drawn in two poses after some loads, a race's intro and
// the pause included, also before today's changes): each character's update on the characters'
// thread (sub_82474DA8, r3 the character, f1 its time; the counter +1760 that sub_82496680 moves
// on when the time is not 0; the two poses' buffers +2440 and +2436), the ragdoll node
// (sub_8249CCD8, r4 the record: +136 / +140 against the state pick its branch, +292 resets, +128
// the pose, +132 the time, the bit 0 of *(+120) after) and the ragdoll pose's drawing
// (sub_824B44F8, r3 the ragdoll, r4 the pose, f1 the weight), 900 lines from when it is switched
// on.
REXCVAR_DEFINE_BOOL(pgr4_character_pose_probe, false, "PGR4",
                    "Development: log the characters' updates, ragdoll nodes and ragdoll pose "
                    "drawings for 900 lines from when it is switched on");
namespace {
std::atomic<int> g_pose_probe_lines{0};
bool PoseProbeLine() {
  // The detail lines: 300 every 2 s (re-armed by the summary), so the probe never goes quiet.
  return REXCVAR_GET(pgr4_character_pose_probe) && g_pose_probe_lines.fetch_sub(1) > 0;
}
// The continuous summary (pgr4_character_pose_probe), every 2 s, no budget: for each ragdoll its
// step calls (stepped, run as is), its drawings (interpolated by pgr4_character_ragdoll_steps, drawn
// as they are and why), the interpolation's fraction, the drawn pose and weight, and how far the drawn
// bodies move between drawings; for each character its updates; for each skeleton its bone outputs
// (sub_82493138). "Back and forth": an output far from the last one but near the one before (the
// change from two back under 0.3 of the change from the last, the last over 0.005), which steady
// motion never does. A skeleton doing it in over a third of its outputs is flagged FAULTY and the first
// one's output buffer page-watched (once a run) to name its writers.
struct BackAndForth {
  std::vector<float> prev, prev2;
  uint64_t calls = 0, back = 0, same_frame = 0, last_frame = ~0ull;
  float d1_max = 0.0f, d1_sum = 0.0f, back_d1_max = 0.0f;
  void Add(const std::vector<float>& now, uint64_t frame) {
    ++calls;
    if (frame == last_frame) ++same_frame;
    last_frame = frame;
    if (prev.size() == now.size()) {
      float d1 = 0.0f, d2 = -1.0f;
      for (size_t i = 0; i < now.size(); ++i) d1 = std::max(d1, std::abs(now[i] - prev[i]));
      if (prev2.size() == now.size()) {
        d2 = 0.0f;
        for (size_t i = 0; i < now.size(); ++i) d2 = std::max(d2, std::abs(now[i] - prev2[i]));
      }
      d1_max = std::max(d1_max, d1);
      d1_sum += d1;
      if (d2 >= 0.0f && d1 > 0.005f && d2 < 0.3f * d1) ++back, back_d1_max = std::max(back_d1_max, d1);
    }
    prev2 = std::move(prev);
    prev = now;
  }
  void Reset() { calls = back = same_frame = 0, d1_max = d1_sum = back_d1_max = 0.0f; }
  std::string Text() const {
    return fmt::format("{} ({} in a frame already) move max {:.4f} mean {:.4f}, back and forth {} "
                       "(largest {:.4f})", calls, same_frame, d1_max, calls ? d1_sum / calls : 0.0f,
                       back, back_d1_max);
  }
};
struct RagdollStats {
  uint64_t step_calls = 0, steps = 0, as_is = 0, zero_dt = 0, ours = 0, other = 0, no_history = 0,
           off = 0;
  float f_min = 2.0f, f_max = -1.0f, w_min = 2.0f, w_max = -1.0f;
  double dt_sum = 0.0;
  std::set<uint32_t> poses;
  BackAndForth drawn;
};
struct CharacterStats {
  uint64_t calls = 0, same_frame = 0, last_frame = ~0ull, zero_dt = 0;
  double dt_sum = 0.0;
  uint32_t counter_first = 0, counter_last = 0, a = 0, b = 0;
};
struct SkeletonStats {
  std::set<uint32_t> outs;
  BackAndForth bones;
  uint32_t last_out = 0;
};
std::mutex g_pose_stats_mutex;
std::map<uint32_t, RagdollStats> g_pose_ragdolls;
std::map<uint32_t, CharacterStats> g_pose_characters;
std::map<uint32_t, SkeletonStats> g_pose_skeletons;
void PoseStatsSummary(uint8_t* base) {
  static auto last = std::chrono::steady_clock::now();
  const auto now = std::chrono::steady_clock::now();
  if (now - last < std::chrono::seconds(2)) return;
  last = now;
  g_pose_probe_lines = 300;
  uint32_t faulty_out = 0, faulty_skeleton = 0;
  for (auto& [ragdoll, r] : g_pose_ragdolls) {
    if (!r.step_calls && !r.drawn.calls) continue;
    std::string poses;
    for (const uint32_t p : r.poses) poses += fmt::format(" {:08X}", p);
    REXLOG_INFO("PGR4 pose summary: frame {} ragdoll {:08X}: step calls {} (stepped {} times; run as "
                "is {}, time 0 in {}) time {:.3f} s; drawn {}; interpolated {} (fraction {:.3f}..{:.3f}), "
                "as they are: bodies set otherwise {}, no history {}, steps off {}; weight "
                "{:.3f}..{:.3f}; poses{}", g_frame_no, ragdoll, r.step_calls, r.steps, r.as_is,
                r.zero_dt, r.dt_sum, r.drawn.Text(), r.ours, r.f_min, r.f_max, r.other, r.no_history,
                r.off, r.w_min, r.w_max, poses);
    BackAndForth keep = std::move(r.drawn);
    keep.Reset();
    r = {};
    r.drawn = std::move(keep);
  }
  for (auto& [perso, c] : g_pose_characters) {
    if (!c.calls) continue;
    REXLOG_INFO("PGR4 pose summary: frame {} character {:08X}: updates {} ({} in a frame already, time "
                "0 in {}) time {:.3f} s, counter {} -> {}, poses A {:08X} B {:08X}", g_frame_no, perso,
                c.calls, c.same_frame, c.zero_dt, c.dt_sum, c.counter_first, c.counter_last, c.a, c.b);
    const uint64_t lf = c.last_frame;
    c = {};
    c.last_frame = lf;
  }
  for (auto& [skeleton, k] : g_pose_skeletons) {
    if (!k.bones.calls) continue;
    std::string outs;
    for (const uint32_t o : k.outs) outs += fmt::format(" {:08X}", o);
    const bool faulty = k.bones.calls >= 20 && k.bones.back * 3 > k.bones.calls;
    REXLOG_INFO("PGR4 pose summary: frame {} skeleton {:08X}: bone outputs {}; outputs{}{}", g_frame_no,
                skeleton, k.bones.Text(), outs, faulty ? " FAULTY" : "");
    if (faulty && !faulty_out) faulty_out = k.last_out, faulty_skeleton = skeleton;
    k.bones.Reset();
    k.outs.clear();
  }
  static bool watched = false;
  if (faulty_out && !watched) {
    watched = true;
    REXLOG_INFO("PGR4 pose summary: skeleton {:08X} goes back and forth: its output {:08X} watched",
                faulty_skeleton, faulty_out);
    const uint32_t count = std::min<uint32_t>(BeU32At(GuestBytes(base, faulty_skeleton + 4)), 256);
    WriteWatchStart(base, faulty_out, (24 * count + 63) / 64);
  }
  WriteWatchReport({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19});
}
void PoseStatsRagdollStep(uint32_t ragdoll, double dt, int steps) {
  if (!REXCVAR_GET(pgr4_character_pose_probe)) return;
  std::lock_guard lock(g_pose_stats_mutex);
  RagdollStats& r = g_pose_ragdolls[ragdoll];
  ++r.step_calls;
  if (steps < 0) ++r.as_is; else r.steps += uint64_t(steps);
  if (!(dt > 0.0)) ++r.zero_dt;
  r.dt_sum += dt;
}
// how: 'i' interpolated, 'o' bodies set otherwise, 'n' no history, 'x' steps off.
void PoseStatsRagdollDrawn(const uint8_t* base, uint32_t ragdoll, uint32_t pose, double weight,
                           char how, float f) {
  if (!REXCVAR_GET(pgr4_character_pose_probe)) return;
  std::vector<float> bodies;
  ReadBodies(base, ragdoll, RagdollCount(base, ragdoll), bodies);
  std::lock_guard lock(g_pose_stats_mutex);
  RagdollStats& r = g_pose_ragdolls[ragdoll];
  r.drawn.Add(bodies, g_frame_no);
  if (how == 'i') ++r.ours, r.f_min = std::min(r.f_min, f), r.f_max = std::max(r.f_max, f);
  else if (how == 'o') ++r.other;
  else if (how == 'n') ++r.no_history;
  else ++r.off;
  r.w_min = std::min(r.w_min, float(weight));
  r.w_max = std::max(r.w_max, float(weight));
  if (r.poses.size() < 8) r.poses.insert(pose);
}
void PoseStatsCharacter(const uint8_t* base, uint32_t perso, double dt) {
  if (!REXCVAR_GET(pgr4_character_pose_probe)) return;
  std::lock_guard lock(g_pose_stats_mutex);
  CharacterStats& c = g_pose_characters[perso];
  const uint32_t counter = BeU32At(base + perso + 1760);
  if (!c.calls) c.counter_first = counter;
  c.counter_last = counter;
  ++c.calls;
  if (c.last_frame == g_frame_no) ++c.same_frame;
  c.last_frame = g_frame_no;
  if (dt == 0.0) ++c.zero_dt;
  c.dt_sum += dt;
  c.a = BeU32At(base + perso + 2440);
  c.b = BeU32At(base + perso + 2436);
}
void PoseStatsBones(uint8_t* base, uint32_t skeleton, uint32_t out, const std::vector<float>& now) {
  if (!REXCVAR_GET(pgr4_character_pose_probe)) return;
  std::lock_guard lock(g_pose_stats_mutex);
  SkeletonStats& k = g_pose_skeletons[skeleton];
  k.bones.Add(now, g_frame_no);
  if (k.outs.size() < 8) k.outs.insert(out);
  k.last_out = out;
  PoseStatsSummary(base);
}
}  // namespace
REX_EXTERN(__imp__sub_82474DA8);
REX_HOOK_RAW(sub_82474DA8) {
  const uint32_t perso = ctx.r3.u32;
  const double dt = ctx.f1.f64;
  const uint32_t counter_before = BeU32At(base + perso + 1760);
  const uint64_t frame_in = g_frame_no;
  __imp__sub_82474DA8(ctx, base);
  if (perso >= 0x40000000) PoseStatsCharacter(base, perso, dt);
  if (REXCVAR_GET(pgr4_character_pose_probe) && dt == 0.0 && perso >= 0x40000000) {
    // While paused: each 4-byte word of the character (+0 to +3200) that changed since the last
    // call, its old and new value (as a float and in hex), the first 16.
    static std::mutex m;
    static std::unordered_map<uint32_t, std::vector<uint8_t>> snapshots;
    std::lock_guard lock(m);
    auto& snap = snapshots[perso];
    const uint8_t* now = base + perso;
    if (snap.size() == 3200) {
      std::string changed;
      int n = 0, total = 0;
      for (uint32_t o = 0; o < 3200; o += 4) {
        if (std::memcmp(snap.data() + o, now + o, 4) == 0) continue;
        ++total;
        if (n++ < 16)
          changed += fmt::format(" +{} {:08X}->{:08X} ({:.4g}->{:.4g})", o, BeU32At(snap.data() + o),
                                 BeU32At(now + o), BeF(snap.data() + o), BeF(now + o));
      }
      if (total && PoseProbeLine())
        REXLOG_INFO("PGR4 pose probe: frame {} paused character {:08X}: {} words changed:{}",
                    g_frame_no, perso, total, changed);
    }
    snap.assign(now, now + 3200);
  }
  if (PoseProbeLine()) {
    // The two poses' bone buffers (24 bytes a bone): their first 64 bones hashed, to see a still
    // character (the pause) drawn from bones that change.
    auto bones = [&](uint32_t at) -> uint64_t {
      const uint32_t buf = BeU32At(base + perso + at);
      if (buf < 0x40000000) return 0;
      uint64_t h = 1469598103934665603ull;  // FNV-1a
      const uint8_t* b = GuestBytes(base, buf);
      for (uint32_t i = 0; i < 24 * 52; ++i) h = (h ^ b[i]) * 1099511628211ull;
      return h;
    };
    REXLOG_INFO("PGR4 pose probe: frame {} -> {} character {:08X} dt {:.5f} counter {} -> {} pose A "
                "{:08X} {:016X} B {:08X} {:016X}", frame_in, g_frame_no, perso, dt, counter_before,
                BeU32At(base + perso + 1760), BeU32At(base + perso + 2440), bones(2440),
                BeU32At(base + perso + 2436), bones(2436));
  }
}
namespace {
uint64_t PoseHash(const uint8_t* base, uint32_t at, uint32_t size) {
  if (at < 0x40000000 || !size) return 0;
  uint64_t h = 1469598103934665603ull;  // FNV-1a
  const uint8_t* p = GuestBytes(base, at);
  for (uint32_t i = 0; i < size; ++i) h = (h ^ p[i]) * 1099511628211ull;
  return h;
}
// A call on the characters' path: its arguments, and for the ragdoll node's branches (r3 the
// state, r4 the record) the state's first word against the record's +136 and +140.
void PoseTrace(const uint8_t* base, const PPCContext& ctx, const char* what, bool branch) {
  if (!PoseProbeLine()) return;
  if (branch)
    REXLOG_INFO("PGR4 pose probe: frame {} {} state {:08X} (*state {}) record {:08X} +136 {} +140 {} "
                "+292 {} f1 {:.5f}", g_frame_no, what, ctx.r3.u32, BeU32At(base + ctx.r3.u32),
                ctx.r4.u32, BeU32At(base + ctx.r4.u32 + 136), BeU32At(base + ctx.r4.u32 + 140),
                base[ctx.r4.u32 + 292], ctx.f1.f64);
  else
    REXLOG_INFO("PGR4 pose probe: frame {} {} r3 {:08X} r4 {:08X} r5 {:08X} r6 {:08X} f1 {:.5f}",
                g_frame_no, what, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.f1.f64);
}
}  // namespace
#define PGR4_POSE_TRACE(name, what, branch)                                                      REX_EXTERN(__imp__##name);                                                                     REX_HOOK_RAW(name) {                                                                             PoseTrace(base, ctx, what, branch);                                                            __imp__##name(ctx, base);                                                                    }
// The ragdoll node's branches (sub_8249CCD8): the first pass, a reset, the normal one (by the
// body's flag), the second pass of the frame (the pose reused, no step).
PGR4_POSE_TRACE(sub_8249C098, "branch C098 (reset, body flag)", true)
PGR4_POSE_TRACE(sub_8249C6B0, "branch C6B0 (reset)", true)
PGR4_POSE_TRACE(sub_8249C1F0, "branch C1F0 (normal, body flag)", true)
PGR4_POSE_TRACE(sub_8249C9E0, "branch C9E0 (normal)", true)
PGR4_POSE_TRACE(sub_8249BFC0, "branch BFC0 (reused)", true)
PGR4_POSE_TRACE(sub_82496680, "counter step (sub_82496680)", false)
PGR4_POSE_TRACE(sub_824968C8, "pose graph (sub_824968C8)", false)
PGR4_POSE_TRACE(sub_82474950, "pose buffers reset (sub_82474950)", false)
// The ragdoll pose's weight (sub_8249FED8 returns it in f1).
REX_EXTERN(__imp__sub_8249FED8);
REX_HOOK_RAW(sub_8249FED8) {
  const uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32;
  __imp__sub_8249FED8(ctx, base);
  if (PoseProbeLine())
    REXLOG_INFO("PGR4 pose probe: frame {} ragdoll weight (sub_8249FED8) r3 {:08X} r4 {:08X} -> {:.4f}",
                g_frame_no, r3, r4, ctx.f1.f64);
}
// The bones made for the drawing (sub_82493138: r3 the skeleton, its bone count at +4; r4 the
// output, 24 bytes a bone, hashed after).
REX_EXTERN(__imp__sub_82493138);
REX_HOOK_RAW(sub_82493138) {
  const uint32_t skeleton = ctx.r3.u32, out = ctx.r4.u32;
  const uint32_t count = skeleton >= 0x40000000 ? std::min<uint32_t>(BeU32At(base + skeleton + 4), 256) : 0;
  __imp__sub_82493138(ctx, base);
  if (!REXCVAR_GET(pgr4_character_pose_probe) || out < 0x40000000 || !count) return;
  // The output as floats (6 a bone), against this skeleton's last two outputs.
  static std::mutex m;
  static std::unordered_map<uint32_t, std::pair<std::vector<float>, std::vector<float>>> last;
  std::vector<float> now(size_t(count) * 6);
  const uint8_t* const bones = GuestBytes(base, out);
  for (size_t i = 0; i < now.size(); ++i) now[i] = BeF(bones + 4 * uint32_t(i));
  std::lock_guard lock(m);
  auto& [prev, prev2] = last[skeleton];
  float d1 = 0.0f, d2 = 0.0f;
  size_t at1 = 0;
  for (size_t i = 0; i < now.size(); ++i) {
    if (prev.size() == now.size() && std::abs(now[i] - prev[i]) > d1) d1 = std::abs(now[i] - prev[i]), at1 = i;
    if (prev2.size() == now.size()) d2 = std::max(d2, std::abs(now[i] - prev2[i]));
  }
  if (PoseProbeLine())
    REXLOG_INFO("PGR4 pose probe: frame {} bones out (sub_82493138) skeleton {:08X} bones {} out {:08X} "
                "{:016X} change from the last {:.5f} (bone {} float {}: {:.5f} -> {:.5f}), from the one "
                "before {:.5f}", g_frame_no, skeleton, count, out, PoseHash(base, out, 24 * count), d1,
                at1 / 6, at1 % 6, prev.size() == now.size() ? prev[at1] : 0.0f, now[at1], d2);
  PoseStatsBones(base, skeleton, out, now);
  prev2 = std::move(prev);
  prev = std::move(now);
}
REX_EXTERN(__imp__sub_8249CCD8);
REX_HOOK_RAW(sub_8249CCD8) {
  const uint32_t rec = ctx.r4.u32;
  const uint8_t* r = base + rec;
  const uint32_t r136 = BeU32At(r + 136), r140 = BeU32At(r + 140), r128 = BeU32At(r + 128);
  const float dt = BeF(r + 132);
  const uint8_t r292 = r[292];
  __imp__sub_8249CCD8(ctx, base);
  if (PoseProbeLine())
    REXLOG_INFO("PGR4 pose probe: frame {} ragdoll node record {:08X} dt {:.5f} +136 {} +140 {} +292 {} "
                "+128 {:08X} ragdoll pose bit {}", g_frame_no, rec, dt, r136, r140, r292, r128,
                BeU32At(r + 120) >= 0x40000000 ? BeU32At(base + BeU32At(r + 120)) & 1 : 9u);
}
// The drawing of a ragdoll's pose (see pgr4_character_ragdoll_steps above).
REX_EXTERN(__imp__sub_824B44F8);
REX_HOOK_RAW(sub_824B44F8) {
  const uint32_t ragdoll = ctx.r3.u32;
  if (REXCVAR_GET(pgr4_character_pose_probe) && ragdoll >= 0x40000000) {
    static std::mutex m;
    static std::unordered_map<uint32_t, std::vector<float>> last;
    const uint32_t count = RagdollCount(base, ragdoll);
    std::vector<float> now;
    ReadBodies(base, ragdoll, count, now);
    std::lock_guard lock(m);
    auto& prev = last[ragdoll];
    float d = 0.0f;
    if (prev.size() == now.size())
      for (size_t i = 0; i < now.size(); ++i) d = std::max(d, std::abs(now[i] - prev[i]));
    if (PoseProbeLine())
      REXLOG_INFO("PGR4 pose probe: frame {} ragdoll pose drawn: ragdoll {:08X} pose {:08X} weight {:.3f} "
                  "bodies {} change from the last drawing {:.5f}", g_frame_no, ragdoll, ctx.r4.u32,
                  ctx.f1.f64, count, d);
    prev = std::move(now);
  }
  std::vector<uint8_t> kept;
  char how = 'x';
  float how_f = 0.0f;
  if (REXCVAR_GET(pgr4_character_ragdoll_steps) && ragdoll >= 0x40000000) {
    how = 'n';
    std::lock_guard lock(g_ragdolls_mutex);
    const auto it = g_ragdolls.find(ragdoll);
    const uint32_t count = RagdollCount(base, ragdoll);
    // Only while the bodies are still our last step's, bit for bit: a ragdoll the game set
    // otherwise since (a reset, a load giving its address to another rider, the race's intro
    // where it draws riders without stepping them) is drawn as it is, and its history dropped.
    bool ours = it != g_ragdolls.end() && it->second.has_before && it->second.count == count &&
                it->second.last.size() == size_t(count) * 7;
    if (ours) {
      std::vector<float> now;
      ReadBodies(base, ragdoll, count, now);
      ours = std::memcmp(now.data(), it->second.last.data(), now.size() * sizeof(float)) == 0;
      if (!ours) g_ragdolls.erase(it), how = 'o';
    }
    if (ours) {
      const RagdollSteps& r = it->second;
      const float f = float(std::clamp(r.time / kRagdollStep, 0.0, 1.0));
      how = 'i';
      how_f = f;
      kept.assign(base + ragdoll + kRagdollBodies,
                  base + ragdoll + kRagdollBodies + kRagdollStride * count);
      for (uint32_t i = 0; i < count; ++i) {
        uint8_t* b = base + ragdoll + kRagdollBodies + kRagdollStride * i;
        const float* a = &r.before[7 * i];
        const float* l = &r.last[7 * i];
        for (int k = 0; k < 3; ++k) PutBeF(b + 4 * k, a[k] + (l[k] - a[k]) * f);
        // The quaternion between the last two steps, f of the way, the nearer sign.
        float dot = 0.0f;
        for (int k = 0; k < 4; ++k) dot += a[3 + k] * l[3 + k];
        const float s = dot < 0.0f ? -1.0f : 1.0f;
        float q[4], len = 0.0f;
        for (int k = 0; k < 4; ++k) {
          q[k] = s * a[3 + k] + (l[3 + k] - s * a[3 + k]) * f;
          len += q[k] * q[k];
        }
        if (!(len > 1e-12f)) continue;
        len = std::sqrt(len);
        for (int k = 0; k < 4; ++k) PutBeF(b + 16 + 4 * k, q[k] / len);
      }
    }
  }
  if (ragdoll >= 0x40000000) PoseStatsRagdollDrawn(base, ragdoll, ctx.r4.u32, ctx.f1.f64, how, how_f);
  __imp__sub_824B44F8(ctx, base);
  if (!kept.empty()) std::memcpy(base + ragdoll + kRagdollBodies, kept.data(), kept.size());
}
REX_EXTERN(__imp__sub_8249C3E0);
REX_HOOK_RAW(sub_8249C3E0) {
  const uint32_t state = ctx.r3.u32;
  uint8_t* const st = base + state;
  const uint8_t* const r = base + ctx.r4.u32;
  const uint32_t body_at = BeU32At(r + 116);
  const uint8_t* const body = base + body_at;
  const float dt = BeF(r + 132);
  // The game's own reset (another vehicle, or its +292 set): no history to go by.
  const bool same = BeU32At(st) == BeU32At(r + 140) && r[292] == 0;
  static bool probe_was = false;
  const bool probe_on = REXCVAR_GET(pgr4_character_accel_probe);
  if (probe_on && !probe_was) g_accel_probe_left = 240;
  probe_was = probe_on;
  if (!REXCVAR_GET(pgr4_character_accel_window) || !same || !(dt > 1e-5f) || dt > 0.25f) {
    g_character_velocities.erase(state);
    g_character_update_steps.erase(state);
    __imp__sub_8249C3E0(ctx, base);
    return;
  }
  float v[3], w[3];
  for (int k = 0; k < 3; ++k) {
    v[k] = BeF(body + 112 + 4 * k);
    w[k] = BeF(body + 124 + 4 * k);
  }
  // Over time: the velocities 1/30 s ago, between the two frame samples around then (the oldest
  // while the history is shorter).
  VelocityHistory& hist = g_character_velocities[state];
  const bool v_changed = hist.samples.empty() || !SameVector(hist.samples.back().v, v);
  const bool w_changed = hist.samples.empty() || !SameVector(hist.samples.back().w, w);
  hist.clock += dt;
  hist.samples.push_back({hist.clock, {v[0], v[1], v[2]}, {w[0], w[1], w[2]}});
  while (hist.samples.size() > 2 && hist.samples[1].time < hist.clock - 0.2)
    hist.samples.erase(hist.samples.begin());
  double av[3] = {}, aw[3] = {};
  bool window_ok = false;
  {
    const double target = hist.clock - 1.0 / 30.0;
    float old_v[3], old_w[3];
    double span = 0.0;
    if (hist.samples.front().time >= target) {
      span = hist.clock - hist.samples.front().time;
      std::copy(hist.samples.front().v, hist.samples.front().v + 3, old_v);
      std::copy(hist.samples.front().w, hist.samples.front().w + 3, old_w);
    } else {
      span = 1.0 / 30.0;
      size_t i = 1;
      while (hist.samples[i].time < target) ++i;
      const VelocitySample& a = hist.samples[i - 1];
      const VelocitySample& b = hist.samples[i];
      const double f = b.time > a.time ? (target - a.time) / (b.time - a.time) : 1.0;
      for (int k = 0; k < 3; ++k) {
        old_v[k] = float(a.v[k] + (b.v[k] - a.v[k]) * f);
        old_w[k] = float(a.w[k] + (b.w[k] - a.w[k]) * f);
      }
    }
    window_ok = span > 1e-4;
    if (window_ok)
      for (int k = 0; k < 3; ++k) {
        av[k] = (v[k] - old_v[k]) / span;
        aw[k] = (w[k] - old_w[k]) / span;
      }
  }
  // From the vehicle's steps and rows (see above).
  bool from_steps = false, w_from_steps = false, rows_turned = false, update_steps = false,
       moved_between = false;
  uint32_t vehicle = 0;
  int32_t step = 0;
  double rows_span = 0.0;
  const char* why = "";
  if (REXCVAR_GET(pgr4_character_accel_steps)) {
    const double h = double(BeF(base + 0x82001170));
    const int32_t un = g_vehicle_update_n.load();
    {
      std::lock_guard lock(g_body_vehicles_mutex);
      const auto it = g_body_vehicles.find(body_at);
      if (it == g_body_vehicles.end() || it->second.frame != g_frame_no) {
        why = " (no vehicle wrote it this frame)";
      } else if (!(h > 0.0)) {
        why = " (no step length)";
      } else {
        const BodyVehicle& bv = it->second;
        vehicle = bv.vehicle;
        const VehicleStep* now_step = VehicleStepAt(bv.steps, un);
        const VehicleStep* before = VehicleStepAt(bv.steps, un - 2);
        auto close = [](double a, double b) { return std::abs(a - b) <= 1e-4 * (1.0 + std::abs(b)); };
        if (!now_step || !before) {
          why = " (steps not sampled yet)";
        } else {
          step = now_step->n;
          // The velocities: the physics' as they are (sub_82475A30: a bike's), or turned by the
          // vehicle's rows (sub_82484C60 after it: a car's).
          bool raw = true, turned = !bv.rows.empty() && bv.rows_frame == g_frame_no;
          const float* m1 = turned ? bv.rows.back().m : nullptr;
          for (int k = 0; k < 3; ++k) {
            raw = raw && close(now_step->v[k], v[k]);
            if (turned)
              turned = close(m1[3 * k] * now_step->v[0] + m1[3 * k + 1] * now_step->v[1] +
                                 m1[3 * k + 2] * now_step->v[2],
                             v[k]);
          }
          if (turned) {
            from_steps = true;
            rows_turned = true;
            float m0[9];
            const double t1 = bv.rows.back().time;
            rows_span = t1 - RowsAt(bv.rows, t1 - 1.0 / 30.0, m0);
            for (int k = 0; k < 3; ++k) {
              double a = 0.0;
              for (int j = 0; j < 3; ++j) {
                a += m1[3 * k + j] * (now_step->v[j] - before->v[j]) / (2.0 * h);
                if (rows_span > 1e-4)
                  a += (m1[3 * k + j] - m0[3 * k + j]) / rows_span * before->v[j];
              }
              av[k] = a;
            }
          } else if (raw) {
            from_steps = true;
            for (int k = 0; k < 3; ++k) av[k] = (now_step->v[k] - before->v[k]) / (2.0 * h);
          } else {
            why = " (not its physics' velocity)";
          }
          // The angular velocities: the physics' as they are (both).
          w_from_steps = from_steps;
          for (int k = 0; w_from_steps && k < 3; ++k) w_from_steps = close(now_step->w[k], w[k]);
          if (w_from_steps)
            for (int k = 0; k < 3; ++k) aw[k] = (now_step->w[k] - before->w[k]) / (2.0 * h);
        }
      }
    }
    // Velocities that move on only at the steps and are not their vehicle's +880 turned: the steps
    // as the vehicle update's count saw them, one a frame, a step it did not see (two in a frame,
    // below 60 frames a second) between its neighbours. Those that moved on while that count did
    // not, in the last 120 frames, move on at each frame: the window over time.
    UpdateSteps& u = g_character_update_steps[state];
    VehicleStep sample{un, {v[0], v[1], v[2]}};
    if (u.samples.empty() || un < u.samples.back().n || un > u.samples.back().n + 16) {
      u.samples = {sample};
    } else if (un > u.samples.back().n) {
      u.samples.push_back(sample);
      while (u.samples.front().n < un - 8) u.samples.erase(u.samples.begin());
    } else if (!SameVector(u.samples.back().v, v)) {
      u.moved_between = g_frame_no;
    }
    moved_between = u.moved_between && g_frame_no < u.moved_between + 120;
    if (!from_steps && !moved_between && h > 0.0) {
      const VehicleStep& at = u.samples.back();
      const int32_t target = at.n - 2;
      const VehicleStep& oldest = u.samples.front();
      float old_v[3];
      int32_t span = 0;
      if (oldest.n >= target) {
        span = at.n - oldest.n;
        std::copy(oldest.v, oldest.v + 3, old_v);
      } else {
        span = 2;
        size_t i = 1;
        while (u.samples[i].n < target) ++i;
        const VehicleStep& a = u.samples[i - 1];
        const VehicleStep& b = u.samples[i];
        const double f = double(target - a.n) / double(b.n - a.n);
        for (int k = 0; k < 3; ++k) old_v[k] = float(a.v[k] + (b.v[k] - a.v[k]) * f);
      }
      if (span > 0) {
        update_steps = true;
        for (int k = 0; k < 3; ++k) av[k] = (at.v[k] - old_v[k]) / (double(span) * h);
      }
    }
  }
  if (from_steps || update_steps || window_ok) {
    // The last call's velocities such that (velocities now - last) / dt is that acceleration.
    for (int k = 0; k < 3; ++k) {
      PutBeF(st + 48 + 4 * k, float(v[k] - av[k] * dt));
      PutBeF(st + 64 + 4 * k, float(w[k] - aw[k] * dt));
    }
    const double weight = ctx.f1.f64;
    if (weight > 0.0 && weight < 1.0) ctx.f1.f64 = 1.0 - std::pow(1.0 - weight, double(dt) * 30.0);
  }
  if (g_accel_probe_left > 0) {
    --g_accel_probe_left;
    // pgr4_character_velocity_watch: which code writes these velocities (the page watch on
    // body +112, 64 bytes, once a run; its writes reported each probed call).
    if (REXCVAR_GET(pgr4_character_velocity_watch)) {
      WriteWatchStart(base, body_at + 112, 1);
      WriteWatchReport({0});
    }
    REXLOG_INFO("PGR4 accel probe: frame {} state {:08X} n {} update n {} dt {:.5f} v {:.6f} {:.6f} "
                "{:.6f}{} angular {:.6f} {:.6f} {:.6f}{} acceleration {:.3f} {:.3f} {:.3f} from {}",
                g_frame_no, state, StepCount(base), g_vehicle_update_n.load(), dt, v[0], v[1], v[2],
                v_changed ? "" : " (unchanged)", w[0], w[1], w[2], w_changed ? "" : " (unchanged)",
                av[0], av[1], av[2],
                from_steps ? fmt::format("vehicle {:08X} step {}, {} (angular {})", vehicle, step,
                                         rows_turned ? fmt::format("turned, rows over {:.4f} s",
                                                                   rows_span)
                                                     : std::string("as they are"),
                                         w_from_steps ? "too" : "over time")
                : update_steps ? fmt::format("the update's steps{}", why)
                               : fmt::format("time{}{}",
                                             moved_between ? ", moved between steps" : "", why));
  }
  __imp__sub_8249C3E0(ctx, base);
}
// The frame's vehicle update: the step count when it begins (see above).
REX_EXTERN(__imp__sub_82437C18);
REX_HOOK_RAW(sub_82437C18) {
  g_vehicle_update_n = StepCount(base);
  __imp__sub_82437C18(ctx, base);
}
// The character's vehicle body written from the vehicle (see above): the vehicle and its rows, by
// the game's time.
REX_EXTERN(__imp__sub_82484C60);
REX_HOOK_RAW(sub_82484C60) {
  const uint32_t body = ctx.r4.u32, car = ctx.r5.u32;
  if (REXCVAR_GET(pgr4_character_accel_window) && REXCVAR_GET(pgr4_character_accel_steps) &&
      body >= 0x40000000 && car >= 0x40000000) {
    const double t = double(BeF(base + 0x82A8131C + 24));
    RowsSample rows{t, {}};
    for (int r = 0; r < 3; ++r)
      for (int k = 0; k < 3; ++k) rows.m[3 * r + k] = BeF(base + car + 12560 + 16 * r + 4 * k);
    std::lock_guard lock(g_body_vehicles_mutex);
    if (g_body_vehicles.size() > 64) g_body_vehicles.clear();  // bodies long gone
    BodyVehicle& bv = g_body_vehicles[body];
    if (bv.vehicle != car) bv = {car, 0, 0, {}, {}};
    bv.frame = bv.rows_frame = g_frame_no;
    if (!bv.rows.empty() && t < bv.rows.back().time) bv.rows.clear();  // the time started over
    if (!bv.rows.empty() && t == bv.rows.back().time) bv.rows.pop_back();  // paused: the same time
    bv.rows.push_back(rows);
    while (bv.rows.size() > 2 && bv.rows[1].time < t - 0.2) bv.rows.erase(bv.rows.begin());
  }
  __imp__sub_82484C60(ctx, base);
}
// sub_82475A30 (r4 the character's vehicle body, r5 the vehicle's pose, the vehicle + 736): copies
// the pose's +144 and +176 (the vehicle's +880 and +912) to the body's +112 and +124 as they are
// (read 20/11/2026); a bike's by sub_824817D8, a car's by sub_82484C60 before it turns the velocity.
REX_EXTERN(__imp__sub_82475A30);
REX_HOOK_RAW(sub_82475A30) {
  const uint32_t body = ctx.r4.u32, pose = ctx.r5.u32;
  if (REXCVAR_GET(pgr4_character_accel_window) && REXCVAR_GET(pgr4_character_accel_steps) &&
      body >= 0x40000000 && pose >= 0x40000000 + 736) {
    std::lock_guard lock(g_body_vehicles_mutex);
    if (g_body_vehicles.size() > 64) g_body_vehicles.clear();  // bodies long gone
    BodyVehicle& bv = g_body_vehicles[body];
    if (bv.vehicle != pose - 736) bv = {pose - 736, 0, 0, {}, {}};
    bv.frame = g_frame_no;
  }
  __imp__sub_82475A30(ctx, base);
}
// A step's physics (both loops): the physics' velocities of each vehicle a character's body was
// written from.
REX_EXTERN(__imp__sub_823058C8);
REX_HOOK_RAW(sub_823058C8) {
  __imp__sub_823058C8(ctx, base);
  if (!REXCVAR_GET(pgr4_character_accel_window) || !REXCVAR_GET(pgr4_character_accel_steps)) return;
  const int32_t n = StepCount(base);
  std::lock_guard lock(g_body_vehicles_mutex);
  for (auto& [body, bv] : g_body_vehicles) {
    if (g_frame_no > bv.frame + 30) continue;  // no longer written
    VehiclePhysics p;
    {
      std::lock_guard plock(g_vehicle_physics_mutex);
      const auto it = g_vehicle_physics.find(bv.vehicle);
      if (it != g_vehicle_physics.end()) p = it->second;
    }
    if (p.physics < 0x40000000 || !p.v_at || !p.w_at) continue;
    VehicleStep sample{n, {}, {}};
    for (int k = 0; k < 3; ++k) {
      sample.v[k] = BeF(base + p.physics + p.v_at + 4 * k);
      sample.w[k] = BeF(base + p.physics + p.w_at + 4 * k);
    }
    if (!bv.steps.empty() && bv.steps.back().n > n) bv.steps.clear();  // the count started over
    if (!bv.steps.empty() && bv.steps.back().n == n) bv.steps.pop_back();  // the same step again
    bv.steps.push_back(sample);
    while (bv.steps.front().n < n - 12) bv.steps.erase(bv.steps.begin());
  }
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
// Local change (15/11/2026, Tazz; redone 17/11/2026, perf-cpu; on testing 18/11/2026): the kudos
// counter's tick (HUD_Kudos_Up, index 5) at the console's pace, one per console frame at most. The
// tick is asked for by sub_825815A0 at each kudos award (sub_82582FB8, from sub_82583AE0,
// sub_82583958): the kudos shown, each floored, summed and compared with the sum at its last call
// (+0xD8D8), the tick when they differ by 1 or more. A drift awards twice a frame: two requests in
// the same frame (measured at a 30 FPS limit, 0 ms apart, every frame), which the console, at 30
// FPS, played at the same moment, its frame's, heard as one. Here they were heard as two (Tazz,
// 17/11/2026: too fast even at 30 FPS), and a higher frame rate awards in more frames. So, as on
// the console, the tick is played on the console's frames (1/30 s of game time,
// g_console_credit): one asked for during a console frame, however many times, plays once at the
// start of the next (sub_824C6FD8), never between: the ticks 33, 67, 100 ms... apart, as on the
// console, at the game frame nearest (at a frame rate that is not a multiple of 30, one game frame
// off at most). Tried before: each request adding 30 x the frame's length, a tick when that
// reached 1 (15/11/2026: in pairs, 17 then 50 ms apart, at 60 FPS); sub_825815A0 itself run once
// per 1/30 s (17/11/2026: wrong, it runs per award); the first request of each console frame
// played at once (17/11/2026: 6 to 21 ms gaps when the requests come unevenly above 60 FPS,
// measured at 80 FPS and simulated).
REXCVAR_DEFINE_BOOL(pgr4_kudos_tick_30fps_pace, true, "PGR4",
                    "The kudos counter's tick played at the console's 30 FPS pace at any frame rate");
REX_EXTERN(__imp__sub_826A03A8);
REX_HOOK_RAW(sub_826A03A8) {
  if (ctx.r4.u32 == 5 && REXCVAR_GET(pgr4_kudos_tick_30fps_pace) && !g_kudos_tick_playing) {
    g_kudos_tick_object.store(ctx.r3.u32);  // played at the next console frame's start
    return;
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

// Local change (15/11/2026, Tazz): manual gear presses kept until a physics step takes them, at any
// frame rate. A press lasts one frame in the controls (an up and a down byte, set by sub_822DC9E0
// on the frame the button goes down), but the gearbox reads them only in the 60 Hz physics step.
// At the console's 30 FPS every frame had a step; above 60 FPS many frames have none, and a quick
// tap was often gone before the next step (shifts missed). A press is put back in the next frames'
// controls until a gearbox step has seen it, for 50 ms at most: one made while the physics is
// stopped (a pause) is dropped. A press put back is cleared once taken, so that a second step in
// the same frame does not see it again.
// Cars: controls sub_822DC420(controls r3, pad r4 in 0..3): bytes +16 (up) and +17 (down); gearbox
//   sub_822DE628(gearbox r3): +36 the car, car +808 its controls, +29 set by the frame's first step
//   (the later ones ignore the press), +30 the gear.
// Bikes: controls sub_822F9D70(bike r3, its pad at +3904, -1 for an AI rider): bike +3230 (up) and
//   +3231 (down); gearbox sub_822FB680(gearbox r3): +0 the bike, the gear at +12.
REXCVAR_DEFINE_BOOL(pgr4_gear_press_latch, true, "PGR4",
                    "Manual gear presses kept until the physics takes them (no shift missed above 60 FPS)");
REXCVAR_DEFINE_BOOL(pgr4_gear_press_probe, false, "PGR4",
                    "Development: the gear presses put back and taken, in the log");
namespace {
struct HeldGearPress {
  uint32_t at = 0;  // the guest address of the up byte; the down byte follows it
  uint8_t up = 0, down = 0;
  bool put_back = false;  // the bytes in memory now are ours, not the game's
  std::chrono::steady_clock::time_point since;
};
std::mutex g_gear_mutex;
std::array<HeldGearPress, 8> g_gear_held;  // 4 pads, as a car or a bike

HeldGearPress* FindGearPress(uint32_t at, bool make) {
  HeldGearPress* free_slot = nullptr;
  for (HeldGearPress& h : g_gear_held) {
    if (h.at == at) return &h;
    if (!free_slot && !h.up && !h.down) free_slot = &h;
  }
  if (make && free_slot) free_slot->at = at;
  return make ? free_slot : nullptr;
}

// After a controls update: a new press is remembered; with none, a held one is put back.
void LatchGearPress(uint8_t* base, uint32_t at) {
  if (!REXCVAR_GET(pgr4_gear_press_latch)) return;
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard lock(g_gear_mutex);
  if (base[at] || base[at + 1]) {
    if (HeldGearPress* h = FindGearPress(at, true)) *h = {at, base[at], base[at + 1], false, now};
    return;
  }
  HeldGearPress* h = FindGearPress(at, false);
  if (!h || (!h->up && !h->down)) return;
  if (now - h->since > std::chrono::milliseconds(50)) {
    *h = {};
    return;
  }
  base[at] = h->up;
  base[at + 1] = h->down;
  if (REXCVAR_GET(pgr4_gear_press_probe) && !h->put_back)
    REXLOG_INFO("PGR4 gear: press {:08X} put back ({})", at, h->up ? "up" : "down");
  h->put_back = true;
}

// After a gearbox step that saw a press.
void GearPressTaken(uint8_t* base, uint32_t at, const char* what, int gear_before, int gear_after) {
  std::lock_guard lock(g_gear_mutex);
  HeldGearPress* h = FindGearPress(at, false);
  if (REXCVAR_GET(pgr4_gear_press_probe))
    REXLOG_INFO("PGR4 gear: {} press {:08X} taken{}, gear {} -> {}", what, at,
                h && h->put_back ? " (put back)" : "", gear_before, gear_after);
  if (!h) return;
  if (h->put_back) base[at] = base[at + 1] = 0;
  *h = {};
}
}  // namespace

REX_EXTERN(__imp__sub_822DC420);
REX_HOOK_RAW(sub_822DC420) {
  const uint32_t controls = ctx.r3.u32, pad = ctx.r4.u32 & 0xFF;
  __imp__sub_822DC420(ctx, base);
  if (pad < 4 && controls) LatchGearPress(base, controls + 16);
}

REX_EXTERN(__imp__sub_822DE628);
REX_HOOK_RAW(sub_822DE628) {
  const uint32_t gearbox = ctx.r3.u32;
  const uint32_t car = Be32(base, gearbox + 36);
  const uint32_t controls = car ? Be32(base, car + 808) : 0;
  const bool pressed = controls && (base[controls + 16] || base[controls + 17]);
  const int gear = int8_t(base[gearbox + 30]);
  __imp__sub_822DE628(ctx, base);
  if (pressed) GearPressTaken(base, controls + 16, "car", gear, int8_t(base[gearbox + 30]));
}

REX_EXTERN(__imp__sub_822F9D70);
REX_HOOK_RAW(sub_822F9D70) {
  const uint32_t bike = ctx.r3.u32;
  const int32_t pad = bike ? int32_t(Be32(base, bike + 3904)) : -1;
  __imp__sub_822F9D70(ctx, base);
  if (pad >= 0 && pad < 4) LatchGearPress(base, bike + 3230);
}

REX_EXTERN(__imp__sub_822FB680);
REX_HOOK_RAW(sub_822FB680) {
  const uint32_t gearbox = ctx.r3.u32;
  const uint32_t bike = Be32(base, gearbox);
  const bool pressed = bike && (base[bike + 3230] || base[bike + 3231]);
  const int gear = int32_t(Be32(base, gearbox + 12));
  __imp__sub_822FB680(ctx, base);
  if (pressed) GearPressTaken(base, bike + 3230, "bike", gear, int32_t(Be32(base, gearbox + 12)));
}

// Development (18/11/2026, the frame-rate audit: work/audit_complet/cadence_fonctions.tsv): the
// game's functions that read a 1/30, 1/60, 30 or 60 constant, outside the physics step, counted
// per game frame and per physics step (pgr4_cadence_probe, off by default). Every 600 game frames
// the log gives each one called: its calls per frame and per step. Generated; to be removed with
// the audit.
REXCVAR_DEFINE_BOOL(pgr4_cadence_probe, false, "PGR4",
                    "Development: count the frame-rate audit's functions' calls per frame and step");
namespace {
constexpr uint32_t kCadenceFns[] = {
    0x822C2CD8, 0x82405B98, 0x82406020, 0x822C2BB8, 0x822C2EB0, 0x822E1210,
    0x822E63C0, 0x822F7538, 0x8246E8A8, 0x8248B780, 0x824B2BC0, 0x824B4938,
    0x82632310, 0x8268BF40, 0x826E3C78, 0x82842AD0, 0x82844320, 0x82848720,
    0x828B5CD8, 0x828B6038, 0x828B8510, 0x828BAFB8, 0x828C9068, 0x828C91A8,
    0x822DD238, 0x822DE7F0, 0x822DED00, 0x822E0250, 0x822E0E68, 0x822E16F0,
    0x822E1CB8, 0x822E45B0, 0x822E52C0, 0x822E57B0, 0x822E5850, 0x822E6A90,
    0x822EDCC8, 0x822EDEA0, 0x822EEE30, 0x822EEF58, 0x822EF888, 0x822F79D8,
    0x823014D0, 0x82301730, 0x82302B68, 0x823057D0, 0x82307140, 0x8230F5F0,
    0x82311F38, 0x8233F090, 0x8233F4B8, 0x8233F5E0, 0x823404B8, 0x82341060,
    0x82369300, 0x82373A48, 0x823CF0D0, 0x823D9730, 0x823DE570, 0x823DE828,
    0x823DFA48, 0x823F3BF0, 0x823F6B08, 0x82406200, 0x82406860, 0x824069F8,
    0x8240A7C8, 0x824376F0, 0x8243C1F8, 0x82448598, 0x8244CE60, 0x8244E2C8,
    0x8245E7E0, 0x8246B610, 0x8246CB48, 0x8246FA48, 0x824841D0, 0x82485478,
    0x8248E700, 0x824945B8, 0x824B6438, 0x824B86F8, 0x824DC5E8, 0x824FD038,
    0x82538EC0, 0x82596278, 0x825BE5D0, 0x825D5220, 0x825DF530, 0x825EE9B8,
    0x82600D68, 0x82613EA8, 0x8261BDA8, 0x826289D8, 0x82631308, 0x82639A28,
    0x826442C0, 0x8267AB20, 0x8267B1D8, 0x8267BAF8, 0x8267BD78, 0x8267BFA8,
    0x8267C728, 0x8267CD18, 0x8267CFE8, 0x8267D200, 0x8267D410, 0x8267D740,
    0x8267DAA8, 0x8267DC90, 0x82699450, 0x827D2400, 0x827D2CD8, 0x827D5C80,
    0x827DC090, 0x827DCBA0, 0x82841958, 0x82843608, 0x82843DA8, 0x82874AE0,
    0x8289FDB8, 0x828B6320, 0x828B6470, 0x828B6FA8, 0x828BBD08, 0x828BBE28,
    0x828BBF48, 0x828BC080, 0x828BCE30, 0x828CA280, 0x828CB0A0, 0x828CC340,
    0x82915FF0, 0x82920180, 0x82920400, 0x829206D8, 0x82920A88, 0x82921110,
    0x82921320, 0x82921560, 0x82921808, 0x82921B30, 0x82921E60, 0x82922260,
    0x829227F8, 0x82922FA8, 0x82923240, 0x82923538, 0x82923900, 0x8293BA80,
    0x82940490, 0x829428F8, 0x8294D698, 0x8295E4A8, 0x8295EAD0, 0x8297C4A8,
    0x8297C5B0, 0x8297D640, 0x8297D748, 0x8297DF90, 0x8297ECF8, 0x8297EE00,
    0x82980980, 0x82983318, 0x829840C8, 0x82988340, 0x82992CA8, 0x82994E80,
    0x82995290, 0x82996358, 0x82996838, 0x82998098, 0x8299E1B0, 0x8299E2B8,
    0x8299E3C0, 0x8299E4C8, 0x8299EEC0, 0x829A35D0, 0x829AF7B8, 0x829BFBD8,
    0x829C00C0, 0x829D0B98, 0x829D30B8, 0x829E0E50, 0x829E56C0,
};
std::atomic<uint32_t> g_cadence_calls[std::size(kCadenceFns)];
std::atomic<bool> g_cadence_on{false};
}  // namespace
// Called once per game frame (sub_824C6FD8's hook): the probe's switch, its sums and its log.
void Pgr4CadenceFrame(const uint8_t* base) {
  static uint32_t frames = 0, last_n = 0;
  static uint64_t steps = 0;
  static double clock_first = 0.0;
  static uint32_t n_first = 0;
  const bool on = REXCVAR_GET(pgr4_cadence_probe);
  const uint8_t* np = base + 0x82A17644;
  const uint32_t n = uint32_t(np[0]) << 24 | uint32_t(np[1]) << 16 | uint32_t(np[2]) << 8 | np[3];
  if (!on) {
    if (g_cadence_on.exchange(false))
      for (auto& c : g_cadence_calls) c.store(0, std::memory_order_relaxed);
    frames = 0;
    steps = 0;
    last_n = n;
    return;
  }
  g_cadence_on.store(true, std::memory_order_relaxed);
  // The game's clock (seconds, standing still in the pause), as a second count of the steps: its
  // time over the step (0x82001170).
  const float clock = BeF(base + 0x82A8131C + 24), h = BeF(base + 0x82001170);
  if (!frames) {
    clock_first = clock;
    n_first = n;
  }
  if (frames) steps += uint32_t(n - last_n);
  last_n = n;
  if (++frames < 600) return;
  REXLOG_INFO("PGR4 cadence: {} frames, {} physics steps ({:.2f} a frame); counter {} -> {}, game "
              "clock {:.3f} -> {:.3f} s ({:.2f} steps a frame by the clock)",
              frames - 1, steps, double(steps) / double(frames - 1), n_first, n, clock_first,
              double(clock), h > 0 ? (double(clock) - clock_first) / h / double(frames - 1) : 0.0);
  for (size_t i = 0; i < std::size(kCadenceFns); ++i) {
    const uint32_t c = g_cadence_calls[i].exchange(0, std::memory_order_relaxed);
    if (c)
      REXLOG_INFO("PGR4 cadence: sub_{:08X} {} calls, {:.3f} a frame, {:.3f} a step",
                  kCadenceFns[i], c, double(c) / double(frames - 1),
                  steps ? double(c) / double(steps) : 0.0);
  }
  frames = 0;
  steps = 0;
}
#define PGR4_CADENCE(i, name)                                                     REX_EXTERN(__imp__##name);                                                      REX_HOOK_RAW(name) {                                                              if (g_cadence_on.load(std::memory_order_relaxed))                                 g_cadence_calls[i].fetch_add(1, std::memory_order_relaxed);                   __imp__##name(ctx, base);                                                     }
PGR4_CADENCE(0, sub_822C2CD8)
PGR4_CADENCE(1, sub_82405B98)
PGR4_CADENCE(2, sub_82406020)
PGR4_CADENCE(3, sub_822C2BB8)
PGR4_CADENCE(4, sub_822C2EB0)
PGR4_CADENCE(5, sub_822E1210)
PGR4_CADENCE(6, sub_822E63C0)
PGR4_CADENCE(7, sub_822F7538)
PGR4_CADENCE(8, sub_8246E8A8)
PGR4_CADENCE(9, sub_8248B780)
PGR4_CADENCE(10, sub_824B2BC0)
PGR4_CADENCE(11, sub_824B4938)
PGR4_CADENCE(12, sub_82632310)
PGR4_CADENCE(13, sub_8268BF40)
PGR4_CADENCE(14, sub_826E3C78)
PGR4_CADENCE(15, sub_82842AD0)
PGR4_CADENCE(16, sub_82844320)
PGR4_CADENCE(17, sub_82848720)
PGR4_CADENCE(18, sub_828B5CD8)
PGR4_CADENCE(19, sub_828B6038)
PGR4_CADENCE(20, sub_828B8510)
PGR4_CADENCE(21, sub_828BAFB8)
PGR4_CADENCE(22, sub_828C9068)
PGR4_CADENCE(23, sub_828C91A8)
PGR4_CADENCE(24, sub_822DD238)
PGR4_CADENCE(25, sub_822DE7F0)
PGR4_CADENCE(26, sub_822DED00)
PGR4_CADENCE(27, sub_822E0250)
PGR4_CADENCE(28, sub_822E0E68)
PGR4_CADENCE(29, sub_822E16F0)
PGR4_CADENCE(30, sub_822E1CB8)
PGR4_CADENCE(31, sub_822E45B0)
PGR4_CADENCE(32, sub_822E52C0)
PGR4_CADENCE(33, sub_822E57B0)
PGR4_CADENCE(34, sub_822E5850)
PGR4_CADENCE(35, sub_822E6A90)
PGR4_CADENCE(36, sub_822EDCC8)
PGR4_CADENCE(37, sub_822EDEA0)
PGR4_CADENCE(38, sub_822EEE30)
PGR4_CADENCE(39, sub_822EEF58)
PGR4_CADENCE(40, sub_822EF888)
PGR4_CADENCE(41, sub_822F79D8)
PGR4_CADENCE(42, sub_823014D0)
PGR4_CADENCE(43, sub_82301730)
PGR4_CADENCE(44, sub_82302B68)
PGR4_CADENCE(45, sub_823057D0)
PGR4_CADENCE(46, sub_82307140)
PGR4_CADENCE(47, sub_8230F5F0)
PGR4_CADENCE(48, sub_82311F38)
PGR4_CADENCE(49, sub_8233F090)
PGR4_CADENCE(50, sub_8233F4B8)
PGR4_CADENCE(51, sub_8233F5E0)
// 52, sub_823404B8: counted in its own hook (the exhaust flames' reflections, below).
PGR4_CADENCE(53, sub_82341060)
PGR4_CADENCE(54, sub_82369300)
PGR4_CADENCE(55, sub_82373A48)
// 56, sub_823CF0D0: counted in its own hook (the lightning, below).
PGR4_CADENCE(57, sub_823D9730)
PGR4_CADENCE(58, sub_823DE570)
PGR4_CADENCE(59, sub_823DE828)
PGR4_CADENCE(60, sub_823DFA48)
PGR4_CADENCE(61, sub_823F3BF0)
PGR4_CADENCE(62, sub_823F6B08)
PGR4_CADENCE(63, sub_82406200)
PGR4_CADENCE(64, sub_82406860)
PGR4_CADENCE(65, sub_824069F8)
PGR4_CADENCE(66, sub_8240A7C8)
PGR4_CADENCE(67, sub_824376F0)
// 68, sub_8243C1F8: counted in its own hook (the blurred wheels, below).
PGR4_CADENCE(69, sub_82448598)
PGR4_CADENCE(70, sub_8244CE60)
PGR4_CADENCE(71, sub_8244E2C8)
PGR4_CADENCE(72, sub_8245E7E0)
PGR4_CADENCE(73, sub_8246B610)
PGR4_CADENCE(74, sub_8246CB48)
PGR4_CADENCE(75, sub_8246FA48)
PGR4_CADENCE(76, sub_824841D0)
PGR4_CADENCE(77, sub_82485478)
PGR4_CADENCE(78, sub_8248E700)
PGR4_CADENCE(79, sub_824945B8)
// 80, sub_824B6438: counted by its hook (pgr4_character_ragdoll_steps), Pgr4CadenceCount.
void Pgr4CadenceCount(int i) {
  if (g_cadence_on.load(std::memory_order_relaxed))
    g_cadence_calls[i].fetch_add(1, std::memory_order_relaxed);
}
PGR4_CADENCE(81, sub_824B86F8)
PGR4_CADENCE(82, sub_824DC5E8)
PGR4_CADENCE(83, sub_824FD038)
PGR4_CADENCE(84, sub_82538EC0)
PGR4_CADENCE(85, sub_82596278)
PGR4_CADENCE(86, sub_825BE5D0)
PGR4_CADENCE(87, sub_825D5220)
PGR4_CADENCE(88, sub_825DF530)
PGR4_CADENCE(89, sub_825EE9B8)
PGR4_CADENCE(90, sub_82600D68)
PGR4_CADENCE(91, sub_82613EA8)
PGR4_CADENCE(92, sub_8261BDA8)
PGR4_CADENCE(93, sub_826289D8)
PGR4_CADENCE(94, sub_82631308)
PGR4_CADENCE(95, sub_82639A28)
PGR4_CADENCE(96, sub_826442C0)
PGR4_CADENCE(97, sub_8267AB20)
PGR4_CADENCE(98, sub_8267B1D8)
PGR4_CADENCE(99, sub_8267BAF8)
PGR4_CADENCE(100, sub_8267BD78)
PGR4_CADENCE(101, sub_8267BFA8)
PGR4_CADENCE(102, sub_8267C728)
PGR4_CADENCE(103, sub_8267CD18)
PGR4_CADENCE(104, sub_8267CFE8)
PGR4_CADENCE(105, sub_8267D200)
PGR4_CADENCE(106, sub_8267D410)
PGR4_CADENCE(107, sub_8267D740)
PGR4_CADENCE(108, sub_8267DAA8)
PGR4_CADENCE(109, sub_8267DC90)
PGR4_CADENCE(110, sub_82699450)
// 111, sub_827D2400: counted in its own hook (the photo mode's camera, below).
PGR4_CADENCE(112, sub_827D2CD8)
PGR4_CADENCE(113, sub_827D5C80)
PGR4_CADENCE(114, sub_827DC090)
PGR4_CADENCE(115, sub_827DCBA0)
PGR4_CADENCE(116, sub_82841958)
PGR4_CADENCE(117, sub_82843608)
PGR4_CADENCE(118, sub_82843DA8)
PGR4_CADENCE(119, sub_82874AE0)
PGR4_CADENCE(120, sub_8289FDB8)
PGR4_CADENCE(121, sub_828B6320)
PGR4_CADENCE(122, sub_828B6470)
PGR4_CADENCE(123, sub_828B6FA8)
PGR4_CADENCE(124, sub_828BBD08)
PGR4_CADENCE(125, sub_828BBE28)
PGR4_CADENCE(126, sub_828BBF48)
PGR4_CADENCE(127, sub_828BC080)
PGR4_CADENCE(128, sub_828BCE30)
PGR4_CADENCE(129, sub_828CA280)
PGR4_CADENCE(130, sub_828CB0A0)
PGR4_CADENCE(131, sub_828CC340)
PGR4_CADENCE(132, sub_82915FF0)
PGR4_CADENCE(133, sub_82920180)
PGR4_CADENCE(134, sub_82920400)
PGR4_CADENCE(135, sub_829206D8)
PGR4_CADENCE(136, sub_82920A88)
PGR4_CADENCE(137, sub_82921110)
PGR4_CADENCE(138, sub_82921320)
PGR4_CADENCE(139, sub_82921560)
PGR4_CADENCE(140, sub_82921808)
PGR4_CADENCE(141, sub_82921B30)
PGR4_CADENCE(142, sub_82921E60)
PGR4_CADENCE(143, sub_82922260)
PGR4_CADENCE(144, sub_829227F8)
PGR4_CADENCE(145, sub_82922FA8)
PGR4_CADENCE(146, sub_82923240)
PGR4_CADENCE(147, sub_82923538)
PGR4_CADENCE(148, sub_82923900)
PGR4_CADENCE(149, sub_8293BA80)
PGR4_CADENCE(150, sub_82940490)
PGR4_CADENCE(151, sub_829428F8)
PGR4_CADENCE(152, sub_8294D698)
PGR4_CADENCE(153, sub_8295E4A8)
PGR4_CADENCE(154, sub_8295EAD0)
PGR4_CADENCE(155, sub_8297C4A8)
PGR4_CADENCE(156, sub_8297C5B0)
PGR4_CADENCE(157, sub_8297D640)
PGR4_CADENCE(158, sub_8297D748)
PGR4_CADENCE(159, sub_8297DF90)
PGR4_CADENCE(160, sub_8297ECF8)
PGR4_CADENCE(161, sub_8297EE00)
PGR4_CADENCE(162, sub_82980980)
PGR4_CADENCE(163, sub_82983318)
PGR4_CADENCE(164, sub_829840C8)
PGR4_CADENCE(165, sub_82988340)
PGR4_CADENCE(166, sub_82992CA8)
PGR4_CADENCE(167, sub_82994E80)
PGR4_CADENCE(168, sub_82995290)
PGR4_CADENCE(169, sub_82996358)
PGR4_CADENCE(170, sub_82996838)
PGR4_CADENCE(171, sub_82998098)
PGR4_CADENCE(172, sub_8299E1B0)
PGR4_CADENCE(173, sub_8299E2B8)
PGR4_CADENCE(174, sub_8299E3C0)
PGR4_CADENCE(175, sub_8299E4C8)
PGR4_CADENCE(176, sub_8299EEC0)
PGR4_CADENCE(177, sub_829A35D0)
PGR4_CADENCE(178, sub_829AF7B8)
PGR4_CADENCE(179, sub_829BFBD8)
PGR4_CADENCE(180, sub_829C00C0)
PGR4_CADENCE(181, sub_829D0B98)
PGR4_CADENCE(182, sub_829D30B8)
PGR4_CADENCE(183, sub_829E0E50)
PGR4_CADENCE(184, sub_829E56C0)
#undef PGR4_CADENCE

// --- Blurred wheels (local change, 19/11/2026). A fast wheel is drawn as slices (sub_8243C1F8, r3
// the wheel's blur: slice count +4, base angle +20, slices from +92, 80 bytes each; f2 the wheel's
// spin). Vehicle.Render.Wheels.BlurMethod (0x82AAC3F8, 0 by default) 0: the base angle turns by
// sign(spin) x WheelAngularSpeed (0x82AAC328, 1 rad) x f1 at each call, then each slice is set from
// it by an offset also times f1 (the blur's width). Method 1: each slice turns by its own rate
// (+100) x f1. The caller (sub_82441040, 0x82441620-0x82441654) passes f1 = 1.0, 0 in a pause
// (pause byte x the frame's time 0x82A81330 not 0): one turn a frame, 30 rad/s at the console's 30
// FPS but 120 at 120 FPS. pgr4_wheel_blur_by_time turns them by f1 x 30 x the frame's time
// instead: method 0 puts the base angle where the game's own turn of f1 then lands it on the turn
// of 30 x dt, and the offsets keep the game's f1 (the blur's width, not a rate); method 1 is given
// f1 x 30 x dt. Read at each call; false: the game's turn per frame, for comparison.
REXCVAR_DEFINE_BOOL(pgr4_wheel_blur_by_time, true, "PGR4",
                    "Blurred wheels: the slices turn by the frame's time (as at the console's 30 "
                    "FPS) instead of a fixed turn each frame");
REXCVAR_DEFINE_BOOL(pgr4_wheel_blur_probe, false, "PGR4",
                    "Development: the blurred wheels' base angle, its turn per second of real time, "
                    "once a second");
namespace {
struct WheelBlurProbe {
  double turned = 0.0;  // radians, unwrapped, since the last line
  uint32_t calls = 0;
};
std::unordered_map<uint32_t, WheelBlurProbe> g_wheel_blur_probe;
std::chrono::steady_clock::time_point g_wheel_blur_probe_since{};
}  // namespace
REX_EXTERN(__imp__sub_8243C1F8);
REX_HOOK_RAW(sub_8243C1F8) {
  if (g_cadence_on.load(std::memory_order_relaxed))
    g_cadence_calls[68].fetch_add(1, std::memory_order_relaxed);
  const uint32_t blur = ctx.r3.u32;
  const uint32_t method = Be32(base, 0x82AAC3F8);
  const float f1 = float(ctx.f1.f64);
  const float dt = BeF(base + 0x82A81330);
  const float turn = BeF(base + 0x82002B2C);  // the game's wrap of the angles (2 pi)
  const bool by_time = REXCVAR_GET(pgr4_wheel_blur_by_time) && f1 != 0.0f && turn > 0.0f;
  const float scale = float(30.0 * double(dt));
  const float before = BeF(base + blur + 20);
  if (by_time && method == 0) {
    // The game's step, as it computes it (0x8243C240-0x8243C254): its 1.0 or -1.0 by the spin's
    // sign, times WheelAngularSpeed.
    const float sign = ctx.f2.f64 >= 0.0 ? BeF(base + 0x82258A24) : BeF(base + 0x82000CB0);
    const float step = float(double(sign) * double(BeF(base + 0x82AAC328)));
    float target = std::fmod(float(double(before) + double(step) * f1 * scale), turn);
    if (target < 0.0f) target += turn;
    PutBeF(base + blur + 20, float(double(target) - double(step) * f1));
  } else if (by_time && method == 1) {
    ctx.f1.f64 = double(float(f1 * scale));
  }
  __imp__sub_8243C1F8(ctx, base);
  if (by_time && method == 1) {
    // Below 30 FPS a slice can turn by more than the game's single wrap takes back.
    const float count = BeF(base + blur + 4);
    for (uint32_t i = 0; i < 64 && float(i) < count; ++i) {
      uint8_t* a = base + blur + 92 + 80 * i;
      const float v = BeF(a);
      if (v > turn || v < 0.0f) {
        const float w = std::fmod(v, turn);
        PutBeF(a, w < 0.0f ? w + turn : w);
      }
    }
  }
  if (REXCVAR_GET(pgr4_wheel_blur_probe)) {
    const auto now = std::chrono::steady_clock::now();
    if (g_wheel_blur_probe_since == std::chrono::steady_clock::time_point{})
      g_wheel_blur_probe_since = now;
    float d = BeF(base + blur + 20) - before;
    if (turn > 0.0f) {
      if (d > 0.5f * turn) d -= turn;
      if (d < -0.5f * turn) d += turn;
    }
    auto& p = g_wheel_blur_probe[blur];
    p.turned += d;
    ++p.calls;
    const double secs = std::chrono::duration<double>(now - g_wheel_blur_probe_since).count();
    if (secs >= 1.0) {
      REXLOG_INFO("PGR4 wheel blur probe: {:.3f} s, method {}, by time {}, f1 {:.3f}, dt {:.5f}, "
                  "WheelAngularSpeed {:.3f}, signs {:.1f} {:.1f}, wrap {:.5f}, {} wheels",
                  secs, method, REXCVAR_GET(pgr4_wheel_blur_by_time), f1, dt,
                  BeF(base + 0x82AAC328), BeF(base + 0x82258A24), BeF(base + 0x82000CB0), turn,
                  g_wheel_blur_probe.size());
      uint32_t shown = 0;
      for (const auto& [w, q] : g_wheel_blur_probe)
        if (shown++ < 4)
          REXLOG_INFO("PGR4 wheel blur probe: {:08X} {} calls ({:.1f} a second), base angle "
                      "turned {:.2f} rad/s, slices {:.0f}",
                      w, q.calls, q.calls / secs, q.turned / secs, BeF(base + w + 4));
      g_wheel_blur_probe.clear();
      g_wheel_blur_probe_since = now;
    }
  } else if (!g_wheel_blur_probe.empty()) {
    g_wheel_blur_probe.clear();
    g_wheel_blur_probe_since = {};
  }
}

// --- Lightning (local change, 19/11/2026). sub_823CF0D0 (r3 the weather's lightning, once a frame):
// a flash (+284 set) picks a record and its length in frames, +280 = int(the record's +12 x 30, at
// 0x8218BCC8); its timer +288 is set to 0, then grows by the frame's time (0x82A81330) at each call.
// Each call of a flash (while +288 is below the wait +292, out of the pause) reads the light curve at
// step +296 / +280, lights the sky and the scene by it, sets LightningIntensity and triggers
// LightningActionIn (sub_826A1BE0, called from 0x823CF294), then +296 += 1; the flash ends after +280
// steps. So a flash lasted +280 frames: its length at 30 FPS, a quarter of it at 120.
// pgr4_lightning_by_time gives each call the step of the flash's own time, int(+288 x 30), at most
// +280 - 1: one step a frame at 30 FPS as at the console, the same step for 4 frames at 120 (the
// flicker keeps its 30 Hz), and the sound's call only when the step is a new one (one a 1/30 s, as at
// the console). When the game does not play the flash in the call (pause, the wait over), the step is
// put back. Read at each call; false: the game's step a frame, for comparison.
REXCVAR_DEFINE_BOOL(pgr4_lightning_by_time, true, "PGR4",
                    "Lightning: each flash lasts its time (its steps at 30 a second, as at the "
                    "console's 30 FPS) instead of one step a frame");
REXCVAR_DEFINE_BOOL(pgr4_lightning_probe, false, "PGR4",
                    "Development: a line per lightning flash (its steps, real length, calls, sound "
                    "triggers)");
namespace {
bool g_lightning_hold_sound = false;  // sub_823CF0D0's call of sub_826A1BE0 skipped (same step)
uint32_t g_lightning_sounds = 0;      // sub_823CF0D0's calls of sub_826A1BE0 passed on
struct LightningProbe {
  bool on = false;
  std::chrono::steady_clock::time_point start{};
  uint32_t calls = 0, sounds_at_start = 0;
  int32_t steps = 0;
};
LightningProbe g_lightning_probe;
void PutBe32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}
}  // namespace
REX_EXTERN(__imp__sub_826A1BE0);
REX_HOOK_RAW(sub_826A1BE0) {
  if (uint32_t(ctx.lr) == 0x823CF294) {
    if (g_lightning_hold_sound) return;
    ++g_lightning_sounds;
  }
  __imp__sub_826A1BE0(ctx, base);
}
REX_EXTERN(__imp__sub_823CF0D0);
REX_HOOK_RAW(sub_823CF0D0) {
  if (g_cadence_on.load(std::memory_order_relaxed))
    g_cadence_calls[56].fetch_add(1, std::memory_order_relaxed);
  const uint32_t w = ctx.r3.u32;
  const bool was_on = base[w + 284] != 0;
  const int32_t step_before = int32_t(Be32(base, w + 296));
  int32_t step = step_before;
  const uint32_t sounds0 = g_lightning_sounds;
  g_lightning_hold_sound = false;
  if (was_on && REXCVAR_GET(pgr4_lightning_by_time)) {
    const int32_t count = int32_t(Be32(base, w + 280));
    const double t = double(BeF(base + w + 288)) * double(BeF(base + 0x8218BCC8));
    // Never back before the step last drawn (step_before - 1): the switch turned on mid-flash.
    step = std::max(int32_t(std::floor(t + 1e-3)), step_before - 1);
    step = std::clamp(step, 0, std::max(count - 1, 0));
    g_lightning_hold_sound = step < step_before;
    PutBe32(base + w + 296, uint32_t(step));
  }
  __imp__sub_823CF0D0(ctx, base);
  g_lightning_hold_sound = false;
  const bool is_on = base[w + 284] != 0;
  // Not played in this call (the pause, the wait over): the game's step back.
  if (was_on && is_on && step != step_before && int32_t(Be32(base, w + 296)) == step)
    PutBe32(base + w + 296, uint32_t(step_before));
  if (!REXCVAR_GET(pgr4_lightning_probe)) {
    g_lightning_probe.on = false;
    return;
  }
  auto& p = g_lightning_probe;
  if (!was_on && is_on) {
    p.on = true;
    p.start = std::chrono::steady_clock::now();
    p.calls = 0;
    p.sounds_at_start = sounds0;
    p.steps = int32_t(Be32(base, w + 280));
  }
  if (p.on) ++p.calls;
  if (p.on && was_on && !is_on) {
    p.on = false;
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - p.start).count();
    REXLOG_INFO("PGR4 lightning probe: by time {}, {} steps, {:.3f} s from the first to the last "
                "step (console {:.3f} s), {} calls, {} sound triggers, timer {:.3f}, dt {:.5f}",
                REXCVAR_GET(pgr4_lightning_by_time), p.steps, secs, (p.steps - 1) / 30.0, p.calls,
                g_lightning_sounds - p.sounds_at_start, BeF(base + w + 288),
                BeF(base + 0x82A81330));
  }
}

// --- Exhaust flames' reflections on the ground (local change, 19/11/2026). sub_823404B8 draws them:
// for each car (sub_82434BA0: the list at 0x82AAAFE0, its count at 0x82AAB008) and its 4 flames
// (intensity at car +10868, then every 32 bytes), a reflection at the intensity when it is above 0,
// then, out of the pause (0x82A135D8 +56 / +61), the intensity -= GfxEffects.ExhaustFlame.Reflections.
// FadeSpeed (0x82A9707C, 0.2) each frame; at 0 or below it is set to 0. A reflection from 1 faded in 5
// frames: 0.17 s at 30 FPS, 0.04 s at 120. pgr4_exhaust_reflection_fade_by_time: after the call, a
// flame the game has just lowered (its intensity now exactly the one before less FadeSpeed) is set to
// the one before less FadeSpeed x 30 x the frame's time (0x82A81330). The frame's drawing, the clamp
// at 0 and the pause are the game's. Read at each call; false: the game's fade a frame, for comparison.
REXCVAR_DEFINE_BOOL(pgr4_exhaust_reflection_fade_by_time, true, "PGR4",
                    "Exhaust flames' reflections fade by the frame's time (as at the console's 30 "
                    "FPS) instead of a fixed step each frame");
REXCVAR_DEFINE_BOOL(pgr4_exhaust_reflection_probe, false, "PGR4",
                    "Development: a line per exhaust flame reflection faded out (start, real time, "
                    "frames, the console's time)");
namespace {
constexpr uint32_t kExhaustCars = 10;  // the list's room: 0x82AAAFE0 to its count at +40
struct ExhaustFade {
  std::chrono::steady_clock::time_point start{};
  float from = 0.0f, last = 0.0f;
  uint32_t frames = 0;
};
std::unordered_map<uint64_t, ExhaustFade> g_exhaust_fade;  // by car << 2 | flame
}  // namespace
REX_EXTERN(__imp__sub_823404B8);
REX_HOOK_RAW(sub_823404B8) {
  if (g_cadence_on.load(std::memory_order_relaxed))
    g_cadence_calls[52].fetch_add(1, std::memory_order_relaxed);
  const bool by_time = REXCVAR_GET(pgr4_exhaust_reflection_fade_by_time);
  const bool probe = REXCVAR_GET(pgr4_exhaust_reflection_probe);
  if (!by_time && !probe) {
    g_exhaust_fade.clear();
    __imp__sub_823404B8(ctx, base);
    return;
  }
  const uint32_t count = std::min(Be32(base, 0x82AAB008), kExhaustCars);
  uint32_t cars[kExhaustCars] = {};
  float before[kExhaustCars][4] = {};
  for (uint32_t i = 0; i < count; ++i) {
    cars[i] = Be32(base, 0x82AAAFE0 + 4 * i);
    if (cars[i])
      for (uint32_t k = 0; k < 4; ++k) before[i][k] = BeF(base + cars[i] + 10868 + 32 * k);
  }
  __imp__sub_823404B8(ctx, base);
  const float fade = BeF(base + 0x82A9707C);
  const float dt = BeF(base + 0x82A81330);
  const auto now = std::chrono::steady_clock::now();
  for (uint32_t i = 0; i < count; ++i) {
    if (!cars[i]) continue;
    for (uint32_t k = 0; k < 4; ++k) {
      uint8_t* p = base + cars[i] + 10868 + 32 * k;
      const float b = before[i][k];
      float a = BeF(p);
      if (!(b > 0.0f) || a != float(double(b) - double(fade))) continue;  // not lowered by the game
      if (by_time) {
        a = float(double(b) - double(fade) * 30.0 * double(dt));
        PutBeF(p, a);
      }
      if (!probe) continue;
      const uint64_t key = uint64_t(cars[i]) << 2 | k;
      auto it = g_exhaust_fade.find(key);
      if (it == g_exhaust_fade.end() || b > it->second.last + 1e-6f)  // a new flame lit it again
        it = g_exhaust_fade.insert_or_assign(key, ExhaustFade{now, b, b, 0}).first;
      ExhaustFade& f = it->second;
      ++f.frames;
      f.last = a;
      if (a <= 0.0f) {
        REXLOG_INFO("PGR4 exhaust reflection probe: by time {}, car {} flame {}: from {:.3f}, out in "
                    "{} frames, {:.3f} s from the first to the last fade (console {} frames, {:.3f} "
                    "s), dt {:.5f}",
                    by_time, i, k, f.from, f.frames,
                    std::chrono::duration<double>(now - f.start).count(),
                    int(std::ceil(f.from / std::max(fade, 1e-3f) - 1e-4f)),
                    (std::ceil(f.from / std::max(fade, 1e-3f) - 1e-4f) - 1.0) / 30.0, dt);
        g_exhaust_fade.erase(it);
      }
    }
  }
  if (!probe) g_exhaust_fade.clear();
}

// --- Audio at the console's rate (local changes, 19/11/2026; work/audit_audio/code/
// cadence_audio_jeu.md items 1, 2 and 4).
// The music's fades (sub_8269D850, r3 the music, every frame): out (state +2152 = 2), the volume
// +2156 -= 0.06 (0x82A6397C) a call, sent to the music (SoundSystem +156), to 0 (state 1); in (state
// 3), += 0.06 a call up to the target +2100 (0.6 x (the profile's volume / 10)^2), the track started
// on the first step from 0 (sub_8269D498). The delayed stop (sub_8269D050, every frame): while
// +18487, +18488 -= 0.1 (0x82A63978) a call, at 0 the track stops (sub_8269CFE0). Both steps are
// read only there. 10 calls each: 0.33 s at 30 FPS, 0.08 s at 120. pgr4_music_fade_by_time: during
// the call, the step is its value x 30 x the frame's time (0x82A81330), then put back; the game's
// step when the frame's time is 0 (the fade in would start the track again at each call). Read at
// each call.
REXCVAR_DEFINE_BOOL(pgr4_music_fade_by_time, true, "PGR4",
                    "Music: the fades in and out and the delayed stop last their time (as at the "
                    "console's 30 FPS) instead of a fixed step each frame");
// The crowd after a rider's fall (sub_82697B30): it reacts only when the collisions' counter +1312
// is at 320, then sets it to 0; the counter is +1 a call of sub_82698628 (every frame) up to 320:
// 10.7 s at 30 FPS, 2.7 s at 120. pgr4_audio_crowd_delay_by_time: the counter moves on console
// frames only (g_console_frame_start), the call's +1 taken back on the others. Read at each call.
REXCVAR_DEFINE_BOOL(pgr4_audio_crowd_delay_by_time, true, "PGR4",
                    "Crowd: the delay between two reactions to a rider's fall counted in console "
                    "frames (10.7 s) instead of game frames");
// The collisions: each physics step's impact keeps the strongest since the last sending: the
// scenery's (sub_82697550: the collisions +1248..+1295, pending +1296), each car's (sub_82694278:
// the car's sound +2208 + 48 i, pending at +44) and bike's (sub_8268DCE8: +2144 + 48 i); each frame
// sends what is pending and clears it (sub_82698628; sub_82696278 -> sub_82695700 from 0x82697268;
// sub_8268FCD0 -> sub_8268F3A0 from 0x82690BAC). At 30 FPS a frame's two steps merged; above 60,
// one event a step, weaker. pgr4_audio_collisions_by_console_frame: on frames that start no
// console frame, nothing is sent and the kept impact (and its pending flag) is put back after the
// call; the strongest of the console frame goes on the next. Read at each call.
REXCVAR_DEFINE_BOOL(pgr4_audio_collisions_by_console_frame, true, "PGR4",
                    "Collision sounds: the strongest impact of each console frame (1/30 s) sent, "
                    "as at 30 FPS, instead of one event a game frame");
// Local change (20/11/2026, the frame-rate audit): the console frame above counted by the frames'
// times held 1, 2 or 3 physics steps at 72 or 144 frames a second (its 1/30 s fell across the
// frames), so a steady scrape changed strength from one sending to the next. The impacts are kept
// at the physics steps (read 20/11/2026): sub_82694278 (a car's sound r3, the key r4, f1 the
// strength) and sub_8268DCE8 (a bike's) write the slot of that key (+2208 / +2144 + 48 i, the
// count at 0x82A84480; strength +32, flag +44) when f1 is stronger; sub_82697550 the scenery's
// (+1248..+1295 of the collisions r3, strength +1280, flag +1296, always set). Each frame's
// sending (sub_82696278, sub_8268FCD0, sub_82698628) sends the flagged slots and resets them all
// (flag 0, strength 0x82258A0C's). pgr4_audio_collisions_by_steps: each kept impact is tagged with
// its two physics steps (the console's frame: steps 2k + 1 and 2k + 2 by the step count n); a
// sending sends the slots whose two steps are done and keeps the others for the next; a slot that
// holds two steps done when the next two's first impact comes (two steps in a frame, from 30 to 60
// frames a second) is set aside and reset first, and sent at the next sending. The strongest of
// each 1/30 s of physics, the same at any frame rate. Replaces
// pgr4_audio_collisions_by_console_frame when on. Read at each call.
REXCVAR_DEFINE_BOOL(pgr4_audio_collisions_by_steps, true, "PGR4",
                    "Collision sounds: the strongest impact of each two physics steps (1/30 s), "
                    "the same at any frame rate");
REXCVAR_DEFINE_BOOL(pgr4_audio_rate_probe, false, "PGR4",
                    "Development: the music's fades, the crowd's delay and the collision events "
                    "per second, in the log");
namespace {
struct AudioFadeRun {
  bool on = false;
  uint32_t state = 0, calls = 0;
  float from = 0.0f;
  std::chrono::steady_clock::time_point start{};
};
AudioFadeRun g_music_fade, g_music_stop;
struct HeldSlot {
  uint32_t address;
  uint8_t bytes[48];
};
std::vector<HeldSlot> g_held_collisions;  // the cars' and bikes' kept impacts, put back after
struct AudioRateCounts {
  uint32_t car = 0, bike = 0, world = 0, held = 0;
  float strongest = 0.0f;
  std::chrono::steady_clock::time_point since{};
};
AudioRateCounts g_audio_rate;
std::chrono::steady_clock::time_point g_crowd_counter_zero{};
double SecondsSince(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}
void AudioRateLog(const uint8_t* base) {
  auto& r = g_audio_rate;
  const auto now = std::chrono::steady_clock::now();
  if (r.since == std::chrono::steady_clock::time_point{}) r.since = now;
  const double secs = std::chrono::duration<double>(now - r.since).count();
  if (secs < 1.0) return;
  if (r.car || r.bike || r.world || r.held)
    REXLOG_INFO("PGR4 audio rate probe: {:.2f} s, collision events: cars {}, bikes {}, scenery {} "
                "({:.1f} a second), held {}, strongest {:.3f}; by console frame {}, dt {:.5f}",
                secs, r.car, r.bike, r.world, (r.car + r.bike + r.world) / secs, r.held,
                r.strongest, REXCVAR_GET(pgr4_audio_collisions_by_console_frame),
                BeF(base + 0x82A81330));
  r = AudioRateCounts{};
  r.since = now;
}
// A music fade step (at `at`) scaled for the call; `kept` is the game's, put back after.
// With pgr4_game_frame_time (20/11/2026), the real frame time instead of the game's: the music
// fades in real time like the console's frames, the pause and the menus included, where the game's
// frame time is 0 and the game's fixed step a call came back (10 calls, 0.07 s at 144 FPS).
void MusicStep(uint8_t* base, uint32_t at, bool active, float& kept, bool& scaled) {
  const float dt =
      REXCVAR_GET(pgr4_game_frame_time) ? Pgr4FrameStepSeconds() : BeF(base + 0x82A81330);
  kept = BeF(base + at);
  scaled = active && REXCVAR_GET(pgr4_music_fade_by_time) && dt > 0.0f;
  if (scaled) PutBeF(base + at, float(double(kept) * 30.0 * double(dt)));
}
void MusicRun(AudioFadeRun& run, bool active_before, bool active_after, uint32_t state,
              float from, float to, const char* what) {
  if (!REXCVAR_GET(pgr4_audio_rate_probe)) {
    run.on = false;
    return;
  }
  if (active_before && (!run.on || run.state != state)) {
    run.on = true;
    run.state = state;
    run.calls = 0;
    run.from = from;
    run.start = std::chrono::steady_clock::now();
  }
  if (run.on) ++run.calls;
  if (run.on && !active_after) {
    run.on = false;
    REXLOG_INFO("PGR4 audio rate probe: {} (state {}) from {:.3f} to {:.3f}: {} calls, {:.3f} s "
                "(console {:.3f} s), by time {}",
                what, state, run.from, to, run.calls, SecondsSince(run.start), run.calls / 30.0,
                REXCVAR_GET(pgr4_music_fade_by_time));
  }
}
// The cars' (0x82697268) and bikes' (0x82690BAC) sending of a kept impact: on a frame that starts
// no console frame, the slot (in the sound `obj`, the 48 bytes of key `key` from `first`) is kept
// to be put back after the update, and nothing is sent.
bool HoldCollision(uint8_t* base, uint32_t obj, uint32_t key, uint32_t first) {
  if (REXCVAR_GET(pgr4_audio_collisions_by_steps) ||
      !REXCVAR_GET(pgr4_audio_collisions_by_console_frame) ||
      g_console_frame_start.load(std::memory_order_relaxed))
    return false;
  const uint32_t count = std::min(Be32(base, 0x82A84480), 64u);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t s = obj + first + 48 * i;
    if (Be32(base, s) != key || !base[s + 44]) continue;
    HeldSlot h{s, {}};
    std::memcpy(h.bytes, base + s, 48);
    g_held_collisions.push_back(h);
    ++g_audio_rate.held;
    return true;
  }
  return false;
}
void PutBackCollisions(uint8_t* base) {
  for (const HeldSlot& h : g_held_collisions) std::memcpy(base + h.address, h.bytes, 48);
  g_held_collisions.clear();
}
// pgr4_audio_collisions_by_steps: each slot's kept impact and its two steps, and one set aside.
struct KeptWindow {
  int32_t window = 0;
  bool set_aside = false;
  uint8_t bytes[49];
};
std::unordered_map<uint32_t, KeptWindow> g_kept_windows;  // by slot address
struct Partial {
  uint32_t address, size;
  uint8_t bytes[49];
};
std::vector<Partial> g_partial_windows;  // kept back over a sending, put back after
bool CollisionsBySteps() { return REXCVAR_GET(pgr4_audio_collisions_by_steps); }
int32_t StepNow(const uint8_t* base) { return int32_t(Be32(base, 0x82A17644)); }
int32_t ImpactWindow(const uint8_t* base) { return (StepNow(base) - 1) >> 1; }
bool WindowDone(const uint8_t* base, int32_t window) { return (StepNow(base) >> 1) > window; }
// Before an impact is kept in `slot` (`size` bytes, flag at `flag`, strength at `strength`): a
// slot that still holds two steps done (not sent yet) is set aside (the stronger of two such) and
// reset as a sending resets it.
void BeforeKeep(uint8_t* base, uint32_t slot, uint32_t size, uint32_t flag, uint32_t strength) {
  if (g_kept_windows.size() > 512) g_kept_windows.clear();
  KeptWindow& k = g_kept_windows[slot];
  if (!base[flag] || k.window >= ImpactWindow(base)) return;
  if (!k.set_aside || BeF(base + strength) > BeF(k.bytes + (strength - slot))) {
    std::memcpy(k.bytes, base + slot, size);
    k.set_aside = true;
  }
  base[flag] = 0;
  std::memcpy(base + strength, base + 0x82258A0C, 4);
}
void AfterKeep(const uint8_t* base, uint32_t slot, uint32_t flag, bool flagged_before) {
  KeptWindow& k = g_kept_windows[slot];
  const int32_t window = ImpactWindow(base);
  if (base[flag] && (!flagged_before || k.window < window)) k.window = window;
}
// Before a sending: a slot set aside goes out now (what it holds kept back for later), a slot whose
// two steps are not done is kept back.
void BeforeSending(uint8_t* base, uint32_t slot, uint32_t size, uint32_t flag) {
  const auto it = g_kept_windows.find(slot);
  const bool flagged = base[flag] != 0;
  const bool done = it == g_kept_windows.end() || WindowDone(base, it->second.window);
  if (it != g_kept_windows.end() && it->second.set_aside) {
    if (flagged) {
      Partial p{slot, size, {}};
      std::memcpy(p.bytes, base + slot, size);
      g_partial_windows.push_back(p);
    }
    std::memcpy(base + slot, it->second.bytes, size);
    it->second.set_aside = false;
    ++g_audio_rate.held;
    return;
  }
  if (flagged && !done) {
    Partial p{slot, size, {}};
    std::memcpy(p.bytes, base + slot, size);
    g_partial_windows.push_back(p);
    base[flag] = 0;
    ++g_audio_rate.held;
  }
}
void AfterSending(uint8_t* base) {
  for (const Partial& p : g_partial_windows) std::memcpy(base + p.address, p.bytes, p.size);
  g_partial_windows.clear();
}
// The slots of a car's or bike's sound `obj` from `first`.
template <typename F>
void ForSlots(const uint8_t* base, uint32_t obj, uint32_t first, F&& f) {
  const uint32_t count = std::min(Be32(base, 0x82A84480), 64u);
  for (uint32_t i = 0; i < count; ++i) f(obj + first + 48 * i);
}
}  // namespace
REX_EXTERN(__imp__sub_8269D850);
REX_HOOK_RAW(sub_8269D850) {
  const uint32_t m = ctx.r3.u32;
  const uint32_t state = Be32(base, m + 2152);
  const float from = BeF(base + m + 2156);
  const bool fading = state == 2 || state == 3;
  float kept = 0.0f;
  bool scaled = false;
  MusicStep(base, 0x82A6397C, fading, kept, scaled);
  __imp__sub_8269D850(ctx, base);
  if (scaled) PutBeF(base + 0x82A6397C, kept);
  MusicRun(g_music_fade, fading, Be32(base, m + 2152) == state, state, from,
           BeF(base + m + 2156), "music fade");
}
REX_EXTERN(__imp__sub_8269D050);
REX_HOOK_RAW(sub_8269D050) {
  const uint32_t m = ctx.r3.u32;
  const bool stopping = base[m + 18487] != 0;
  const float from = BeF(base + m + 18488);
  float kept = 0.0f;
  bool scaled = false;
  MusicStep(base, 0x82A63978, stopping, kept, scaled);
  __imp__sub_8269D050(ctx, base);
  if (scaled) PutBeF(base + 0x82A63978, kept);
  MusicRun(g_music_stop, stopping, base[m + 18487] != 0, 1, from, BeF(base + m + 18488),
           "music delayed stop");
}
REX_EXTERN(__imp__sub_82698628);
REX_HOOK_RAW(sub_82698628) {
  const uint32_t c = ctx.r3.u32;
  const bool console_frame = g_console_frame_start.load(std::memory_order_relaxed);
  const bool by_steps = CollisionsBySteps();
  const bool hold_world =
      !by_steps && REXCVAR_GET(pgr4_audio_collisions_by_console_frame) && !console_frame;
  const bool hold_crowd = REXCVAR_GET(pgr4_audio_crowd_delay_by_time) && !console_frame;
  const bool active = base[c + 1236] != 0;
  const bool pending = base[c + 1296] != 0;
  const uint32_t counter = Be32(base, c + 1312);
  uint8_t kept[49];
  const bool held = hold_world && pending;
  if (held) {
    std::memcpy(kept, base + c + 1248, 49);  // +1248..+1295 and the pending flag +1296
    base[c + 1296] = 0;
    ++g_audio_rate.held;
  } else if (active && pending) {
    ++g_audio_rate.world;
    g_audio_rate.strongest = std::max(g_audio_rate.strongest, BeF(base + c + 1280));
  }
  if (by_steps) BeforeSending(base, c + 1248, 49, c + 1296);
  __imp__sub_82698628(ctx, base);
  if (by_steps) AfterSending(base);
  if (held) std::memcpy(base + c + 1248, kept, 49);
  if (hold_crowd && Be32(base, c + 1312) == counter + 1) PutBe32(base + c + 1312, counter);
  if (!REXCVAR_GET(pgr4_audio_rate_probe)) {
    g_crowd_counter_zero = {};
    return;
  }
  const uint32_t now_counter = Be32(base, c + 1312);
  if (now_counter <= 1 && counter != now_counter)
    g_crowd_counter_zero = std::chrono::steady_clock::now();
  else if (counter < 320 && now_counter == 320 &&
           g_crowd_counter_zero != std::chrono::steady_clock::time_point{})
    REXLOG_INFO("PGR4 audio rate probe: crowd fall delay at 320 after {:.2f} s (console 10.67 "
                "s), by time {}",
                SecondsSince(g_crowd_counter_zero), REXCVAR_GET(pgr4_audio_crowd_delay_by_time));
  AudioRateLog(base);
}
REX_EXTERN(__imp__sub_82695700);
REX_HOOK_RAW(sub_82695700) {
  if (uint32_t(ctx.lr) == 0x82697268) {
    if (HoldCollision(base, ctx.r3.u32, ctx.r4.u32, 2208)) return;
    ++g_audio_rate.car;
    g_audio_rate.strongest = std::max(g_audio_rate.strongest, float(ctx.f1.f64));
  }
  __imp__sub_82695700(ctx, base);
}
REX_EXTERN(__imp__sub_82696278);
REX_HOOK_RAW(sub_82696278) {
  g_held_collisions.clear();
  const bool by_steps = CollisionsBySteps();
  if (by_steps)
    ForSlots(base, ctx.r3.u32, 2208, [&](uint32_t s) { BeforeSending(base, s, 48, s + 44); });
  __imp__sub_82696278(ctx, base);
  if (by_steps) AfterSending(base);
  PutBackCollisions(base);
}
REX_EXTERN(__imp__sub_8268F3A0);
REX_HOOK_RAW(sub_8268F3A0) {
  if (uint32_t(ctx.lr) == 0x82690BAC) {
    if (HoldCollision(base, ctx.r3.u32, ctx.r4.u32, 2144)) return;
    ++g_audio_rate.bike;
    g_audio_rate.strongest = std::max(g_audio_rate.strongest, float(ctx.f1.f64));
  }
  __imp__sub_8268F3A0(ctx, base);
}
REX_EXTERN(__imp__sub_8268FCD0);
REX_HOOK_RAW(sub_8268FCD0) {
  g_held_collisions.clear();
  const bool by_steps = CollisionsBySteps();
  if (by_steps)
    ForSlots(base, ctx.r3.u32, 2144, [&](uint32_t s) { BeforeSending(base, s, 48, s + 44); });
  __imp__sub_8268FCD0(ctx, base);
  if (by_steps) AfterSending(base);
  PutBackCollisions(base);
}
// The keeping of an impact (see pgr4_audio_collisions_by_steps above).
REX_EXTERN(__imp__sub_82694278);
REX_HOOK_RAW(sub_82694278) {
  if (!CollisionsBySteps()) {
    __imp__sub_82694278(ctx, base);
    return;
  }
  const uint32_t obj = ctx.r3.u32, key = ctx.r4.u32;
  std::vector<std::pair<uint32_t, bool>> slots;
  ForSlots(base, obj, 2208, [&](uint32_t s) {
    if (Be32(base, s) != key) return;
    BeforeKeep(base, s, 48, s + 44, s + 32);
    slots.push_back({s, base[s + 44] != 0});
  });
  __imp__sub_82694278(ctx, base);
  for (const auto& [s, flagged] : slots) AfterKeep(base, s, s + 44, flagged);
}
REX_EXTERN(__imp__sub_8268DCE8);
REX_HOOK_RAW(sub_8268DCE8) {
  if (!CollisionsBySteps()) {
    __imp__sub_8268DCE8(ctx, base);
    return;
  }
  const uint32_t obj = ctx.r3.u32, key = ctx.r4.u32;
  std::vector<std::pair<uint32_t, bool>> slots;
  ForSlots(base, obj, 2144, [&](uint32_t s) {
    if (Be32(base, s) != key) return;
    BeforeKeep(base, s, 48, s + 44, s + 32);
    slots.push_back({s, base[s + 44] != 0});
  });
  __imp__sub_8268DCE8(ctx, base);
  for (const auto& [s, flagged] : slots) AfterKeep(base, s, s + 44, flagged);
}
REX_EXTERN(__imp__sub_82697550);
REX_HOOK_RAW(sub_82697550) {
  if (!CollisionsBySteps()) {
    __imp__sub_82697550(ctx, base);
    return;
  }
  const uint32_t c = ctx.r3.u32;
  BeforeKeep(base, c + 1248, 49, c + 1296, c + 1280);
  const bool flagged = base[c + 1296] != 0;
  __imp__sub_82697550(ctx, base);
  AfterKeep(base, c + 1248, c + 1296, flagged);
}

// --- The photo mode's camera (mode 513, sub_827D2400), updated at each frame of the photo mode
// only: the game frame it last ran in, for pgr4-native's motion blur rule (renderer.cpp
// ApplyMotionBlurRule: the photo mode keeps the game's motion blur) and the photo mode's focus
// (draw.cpp).
namespace {
std::atomic<uint64_t> g_photo_camera_frame{0};
}  // namespace
bool Pgr4PhotoModeActive() {
  const uint64_t f = g_photo_camera_frame.load(std::memory_order_relaxed);
  return f != 0 && g_frame_no - f <= 2;
}
REX_EXTERN(__imp__sub_827D2400);
REX_HOOK_RAW(sub_827D2400) {
  if (g_cadence_on.load(std::memory_order_relaxed))
    g_cadence_calls[111].fetch_add(1, std::memory_order_relaxed);
  g_photo_camera_frame.store(std::max<uint64_t>(g_frame_no, 1), std::memory_order_relaxed);
  __imp__sub_827D2400(ctx, base);
}

// --- The photo mode's focus (local change, 19/11/2026, Tazz: « le mode focus du mode photo est
// inversé », « le focus ne marche qu'une fois après il fait plus rien »). sub_824B8CE0 (called by
// the focus, sub_825E0108: InvDepth = 1 / its distance) locks a depth resolve (0x82A946A0), reads
// its centre texel (width / 2, height / 2) as the console's D24FS8 (the 20e4 float in the top 24
// bits, 1 the nearest), and turns it into a distance: v = (far - near) x (depth - 1) + far, the
// distance far x near / v (far when v is at most 0x82002C28), near and far the camera's
// (*(*0x82C98E48 + 0xC0) + 0x30C, + 0x310); 0 when the lock fails. pgr4-native never reads depth
// resolves back to guest memory: the texel kept an old value, the focus stayed where it first went.
// After the call, its result is computed the same way from the scene's depth at the centre read
// back from the GPU (Pgr4PhotoFocusDepth, pgr4-native draw.cpp: a few frames old at most, the photo
// mode's view still). pgr4_native_photo_focus_depth false: the game's result.
bool Pgr4PhotoFocusDepth(float* depth);  // pgr4-native draw.cpp
REXCVAR_DECLARE(bool, pgr4_native_photo_focus_depth);
REX_EXTERN(__imp__sub_824B8CE0);
REX_HOOK_RAW(sub_824B8CE0) {
  __imp__sub_824B8CE0(ctx, base);
  float depth = 0.0f;
  if (!REXCVAR_GET(pgr4_native_photo_focus_depth) || !Pgr4PhotoFocusDepth(&depth)) return;
  const uint32_t holder = Be32(base, 0x82C98E48);
  const uint32_t camera = holder ? Be32(base, holder + 0xC0) : 0;
  if (!camera) return;
  const float near_plane = BeF(base + camera + 0x30C), far_plane = BeF(base + camera + 0x310);
  const float one = BeF(base + 0x82258A24), least = BeF(base + 0x82002C28);
  const float v = float(double(float(double(far_plane) - double(near_plane))) *
                            double(float(double(depth) - double(one))) +
                        double(far_plane));
  const double before = ctx.f1.f64;
  ctx.f1.f64 = least < v ? double(float(float(double(far_plane) * double(near_plane)) / v))
                         : double(far_plane);
  static auto last_log = std::chrono::steady_clock::now();
  if (std::chrono::steady_clock::now() - last_log > std::chrono::seconds(1)) {
    last_log = std::chrono::steady_clock::now();
    REXLOG_INFO("PGR4 photo focus: depth {:.6f} (near {:.3f}, far {:.1f}): {:.2f} m, the game's "
                "{:.2f} m", depth, near_plane, far_plane, ctx.f1.f64, before);
  }
}
