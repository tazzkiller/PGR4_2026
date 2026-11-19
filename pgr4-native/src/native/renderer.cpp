// pgr4-native PGR4 native renderer: NativeRenderer host.
#include "renderer.h"

#include "async_file.h"
#include "clock.h"
#include "d3d12_device.h"
#include "d3d_state.h"
#include "draw.h"
#include "frame_dump.h"
#include "frame_generation.h"
#include "tunables.h"
#include "hudless.h"
#include "overlay.h"
#include "frame_pacer.h"
#include "gpu_timer.h"
#include "guest_memory.h"
#include "occlusion.h"
#include "present.h"
#include "profile.h"
#include "render_targets.h"
#include "resolve.h"
#include "scene_effects.h"
#include "textures.h"
#include "upscaler.h"
#include "streamline.h"

#include <rex/graphics/registers.h>
#include <rex/kernel/xam/module.h>
#include <rex/logging.h>
#include <fmt/format.h>
#include <rex/system/xfile.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

// patches/0025 (xam_input.cpp): what user 0's XamInputGetState calls returned
// since the last call, merged (bit layout there).
extern "C" uint64_t rex_xam_take_frame_pad();

namespace reg = rex::graphics::reg;

REXCVAR_DEFINE_BOOL(pgr4_native, false, "PGR4 native",
                    "Run the native D3D12 renderer beside the Xenos plugin (debug window)");
REXCVAR_DEFINE_BOOL(pgr4_native_primary, false, "PGR4 native",
                    "The native renderer is the game's output: it presents into the game window "
                    "and the Xenos plugin skips all draws, resolves and swaps (gpu_skip_draws, "
                    "patches/0031); implies --pgr4_native");
REXCVAR_DEFINE_INT32(pgr4_native_msaa, 2, "PGR4 native",
                     "Samples of the 3D scene's MSAA without the upscaler: 1, 2 (the game's), 4 "
                     "or 8 (fewer if the GPU lacks them)");
// Local change (pgr4 project): Xenia's draw resolution scale for the native renderer.
REXCVAR_DEFINE_INT32(pgr4_native_resolution_scale, 1, "PGR4 native",
                     "Draw resolution scale: render targets and resolves at this many host pixels "
                     "per guest pixel on each axis (1-4; 2 renders 1280x720 at 2560x1440); the "
                     "upscaler then outputs at that size")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
// Local change (pgr4 project): resolves the game's CPU reads back (ReadBackResolve).
REXCVAR_DEFINE_BOOL(pgr4_native_resolve_readback, true, "PGR4 native",
                    "Write k_5_6_5 / k_6_5_5 resolves back into guest memory (the GPU waited "
                    "for at the resolve): the team logo on the suit and the liveries, which the "
                    "game composes on the GPU and compresses on the CPU");
REXCVAR_DEFINE_INT32(pgr4_render_height, 0, "PGR4",
                     "The render height the patch gives every hooked 720 (720 disables it, 0: from the "
                     "screen's shape, 21:9 about 540)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(pgr4_native_csv, "", "PGR4 native",
                      "Per-frame CSV of the native path: counts and CPU time per hook class");
REXCVAR_DEFINE_INT32(pgr4_native_width, 1280, "PGR4 native", "Debug window width");
REXCVAR_DEFINE_INT32(pgr4_native_height, 720, "PGR4 native", "Debug window height");
// Local change (18/11/2026, Tazz: "a prerequisite of DLSS"): the game's motion blur off while DLSS
// runs (ApplyMotionBlurRule), whatever the menu's FLOU DE MOUVEMENT (pgr4_disable_motion_blur,
// which applies at the game's start); without DLSS, the menu's choice. Read at each frame.
REXCVAR_DEFINE_BOOL(pgr4_native_dlss_no_motion_blur, true, "PGR4 native",
                    "The game's motion blur off while DLSS runs (its prerequisite)");
// Local change (19/11/2026, Tazz: « le flou qui doit se désactiver c'est celui dans les options
// seulement »): the rule above held the byte at 0 in the photo mode too, so its view lost its
// motion blur under DLSS. The photo mode (its camera, sub_827D2400, ran in the last frames) gets the
// menu's choice back, as without DLSS. Read at each frame.
REXCVAR_DEFINE_BOOL(pgr4_native_dlss_blur_in_photo_mode, true, "PGR4 native",
                    "Under DLSS, the photo mode keeps the game's motion blur (the menu's choice)");
// The same for the pause (19/11/2026, Tazz: « active le je vais te dire »): the game's clock still
// for 6 frames (g_pgr4_game_paused, src/pgr4_hooks.cpp: the pause and the photo mode). Read at each
// frame.
REXCVAR_DEFINE_BOOL(pgr4_native_dlss_blur_in_pause, true, "PGR4 native",
                    "Under DLSS, the pause keeps the game's motion blur (the menu's choice)");
REXCVAR_DECLARE(bool, pgr4_disable_motion_blur);  // src/pgr4_hooks.cpp
REXCVAR_DEFINE_BOOL(pgr4_native_profile, false, "PGR4 native",
                    "Per-stage CPU time of the draw path (RDTSC), appended to the native CSV as "
                    "st_<stage>_us columns and to the stats log line");
REXCVAR_DEFINE_BOOL(pgr4_native_gpu_timing, false, "PGR4 native",
                    "GPU timestamps around every pass of the native frame (draws per render "
                    "target set, clears, resolves, present); the costliest passes are logged "
                    "every --pgr4_d3d_stats_frames frames");
REXCVAR_DEFINE_BOOL(pgr4_native_gpu_timing_shaders, false, "PGR4 native",
                    "With --pgr4_native_gpu_timing: time draws per vertex/pixel shader pair "
                    "instead of per render target set, and log the ucode of the costliest "
                    "shaders once (render thread mode)");
REXCVAR_DEFINE_BOOL(pgr4_native_texture_capture_order, true, "PGR4 native",
                    "Bind textures as they were when the game queued the draw: a texture the game "
                    "rewrites before the render thread reaches the draw keeps its older data for "
                    "it (TextureCache::MarkCapture)");
REXCVAR_DEFINE_BOOL(pgr4_native_render_thread, true, "PGR4 native",
                    "Record the native frames on their own thread (\"PGR4 native render\"): the "
                    "game thread only copies what each D3D call reads from guest memory and "
                    "queues it, at most one frame ahead of the render thread");
REXCVAR_DEFINE_INT32(pgr4_native_submit_draws, 256, "PGR4 native",
                     "Submit the frame's commands every this many draws, so the GPU starts on them "
                     "while the rest of the frame is recorded (0: one submission per frame, at the "
                     "Swap)");
REXCVAR_DEFINE_BOOL(pgr4_native_occlusion, true, "PGR4 native",
                    "With --pgr4_native_primary: count the samples of the game's occlusion "
                    "queries (the light flares) on the host, so lights behind walls and cars stay "
                    "hidden; off, every query reports its whole draw visible")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(pgr4_native_renderer_switch, true, "PGR4 native",
                    "With --pgr4_native_primary, the key bind_renderer_switch (F9) switches "
                    "between the native renderer and the Xenos plugin at the next Swap");
REXCVAR_DEFINE_INT32(pgr4_native_frame_latency, 3, "PGR4 native",
                     "With --pgr4_native_primary: pace the game's frames on the display, with at "
                     "most this many frames begun but not yet shown (1-3, frame_pacer.h; the game "
                     "thread, render thread and GPU each hold one at 90 fps); 0 leaves "
                     "the pacing to the game's vblank wait (with --pgr4_native_csv the shown "
                     "presents are still logged)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
// Local change: off by default (31/10/2026). Its delay fed on itself: the GPU, idle while it
// waited, lowered its clocks, each frame took longer, and the delay grew (40-50 ms a frame in
// the menus, the GPU at its lowest clocks).
REXCVAR_DEFINE_BOOL(pgr4_native_low_latency, false, "PGR4 native",
                    "With the frames paced on the display: begin each frame as late as the "
                    "time the frames before it waited for the display allows (frame_pacer.h), "
                    "so a frame limit lowers the latency instead of raising it")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(pgr4_native_screenshot_textures, false, "PGR4 native",
                    "Development: a screenshot (bind_screenshot, F1) also lists the textures of "
                    "the frame's draws beside its PNG (.tsv: shader, slot, texture, size, format)");
REXCVAR_DEFINE_INT32(pgr4_native_latency_margin_us, 4000, "PGR4 native",
                     "With --pgr4_native_low_latency: how long before the display shows a frame "
                     "the GPU should have finished it, in microseconds")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(
    pgr4_native_mark_audit, 0, "PGR4 native",
    "Debug, render thread mode: a mark (F12 or <capture dir>/capture.request) also "
    "logs the draws of the next this many frames to <capture dir>/draws-f<frame>.txt "
    "and audits them: each shader against the guest's ucode, the render thread's "
    "registers against the game thread's, and the shared memory each draw reads "
    "against guest memory as it was when the draw was queued (0: off)");
REXCVAR_DECLARE(std::string, pgr4_native_capture_dir);
// The last screen the game loaded was a race's (games/pgr4/src/pgr4_menu.cpp).
bool Pgr4InGameScreen();
// The photo mode's camera ran in the last frames (src/pgr4_hooks.cpp).
bool Pgr4PhotoModeActive();
// The game's clock stands still (the pause, the photo mode; draw.cpp, set by src/pgr4_hooks.cpp).
extern bool g_pgr4_game_paused;
REXCVAR_DECLARE(int32_t, pgr4_d3d_stats_frames);
REXCVAR_DECLARE(bool, pgr4_native_luma_probe);
REXCVAR_DECLARE(bool, pgr4_native_dlss);
REXCVAR_DECLARE(bool, pgr4_native_dlss_fg);
// Local change (pgr4 project), debug: DLSS-G without its HUD-less image (hudless.h), to test what
// it changes (the cars' reflection cube's flicker with DLSS-G at a race's loading).
REXCVAR_DEFINE_BOOL(pgr4_native_dlss_fg_hudless, true, "PGR4 native",
                    "Debug: give DLSS-G the HUD-less image (hudless.h)");
REXCVAR_DECLARE(bool, pgr4_native_frame_generation);
// Local change (18/11/2026): LimitFrameRate by the upscaler running (else by the settings, as before).
REXCVAR_DEFINE_BOOL(pgr4_native_pace_by_upscaler, true, "PGR4 native",
                    "Frame generation's pacing by the upscaler running (DLSS or FSR), not by "
                    "the settings");
REXCVAR_DEFINE_INT32(pgr4_native_test_fps_cap, 0, "PGR4 native",
                     "Development: frames per second at most, by a sleep (0: none)");
// Local change (pgr4 project): the game's PC DISPLAY page's frame rate.
REXCVAR_DEFINE_INT32(pgr4_native_present_interval, 1, "PGR4 native",
                     "With vsync: the display's refreshes per frame shown (1-4: its rate over that); "
                     "with frame generation, presented each refresh and the game paced to match");
REXCVAR_DEFINE_INT32(pgr4_native_fps_limit, 0, "PGR4 native",
                     "Frames per second at most, by a sleep (0: none; PC DISPLAY's fixed limits, "
                     "with pgr4_native_present_interval 1)");
REXCVAR_DEFINE_BOOL(pgr4_native_camera_diag, false, "PGR4 native",
                    "Log the distinct c4-c7 matrices of the scene's depth draws every 120 frames "
                    "(local diagnostic for DLSS-G's camera)");

// Local change (pgr4 project): the photo mode's screenshots (src/pgr4_menu.cpp): asked from the
// game's thread, taken at the next frame's begin (NativeRenderer's), done once its frame is copied.
namespace {
std::atomic<uint32_t> g_clean_shots_asked{0};
std::atomic<uint32_t> g_clean_shot_target{0};
}  // namespace
void Pgr4AskScreenshot() { g_clean_shots_asked.fetch_add(1, std::memory_order_release); }
bool Pgr4ScreenshotTaken() {
  return pgr4::native::Presenter::ScreenshotsCopied() >=
         g_clean_shot_target.load(std::memory_order_acquire);
}

REXCVAR_DEFINE_BOOL(pgr4_native_original_look, false, "PGR4 native",
                    "The game as it was, without this project's changes (the overlay's check "
                    "box): no replacement shaders, HD textures, scaled car reflections, GTAO, sky "
                    "or water plane changes; the other settings kept as they are");

namespace pgr4::native {

bool OriginalLook() { return REXCVAR_GET(pgr4_native_original_look); }

namespace {
// 0 until first read: --pgr4_native_resolution_scale's, then the overlay's (RebuildPipeline).
std::atomic<uint32_t> g_draw_resolution_scale{0};
}  // namespace
uint32_t DrawResolutionScale() {
  uint32_t scale = g_draw_resolution_scale.load(std::memory_order_relaxed);
  if (!scale) {
    scale = uint32_t(std::clamp(REXCVAR_GET(pgr4_native_resolution_scale), 1, 4));
    g_draw_resolution_scale.store(scale, std::memory_order_relaxed);
  }
  return scale;
}
void SetDrawResolutionScale(uint32_t scale) {
  g_draw_resolution_scale.store(std::clamp(scale, 1u, 4u), std::memory_order_relaxed);
}

struct NativeRenderer::Csv {
  std::unique_ptr<AsyncFile> file;
  // Window for the log summary.
  uint64_t frames = 0, draws = 0, draw_ns = 0, swap_ns = 0, frame_ns = 0, hook_ns = 0,
           hook_wait_ns = 0;
  // Profile: the window's stage ticks.
  uint64_t stage_ticks[profile::kStageCount] = {}, track_calls = 0;
  TextureCache::Stats tex_prev;                  // texture counters at the previous CSV row
  uint64_t evicted_prev = 0, evict_ns_prev = 0;  // the same for every row (tex_prev: profile)
  uint64_t occ_unknown_prev = 0;                 // OcclusionQueries::unknown_pairs() likewise
  bool luma = false;                             // --pgr4_native_luma_probe columns
  std::string line;                              // the row being written
};

namespace {
// Set once the device or swapchain failed; the native path stops.
std::atomic<bool> g_failed{false};

// The game window (the SDK's display window), and ReXGlue's presenter, which presents into it
// unless the renderer does (OwnGameWindow).
rex::ui::Window* GameWindow() {
  rex::Runtime* runtime = rex::Runtime::instance();
  return runtime ? runtime->display_window() : nullptr;
}
rex::ui::Presenter* SdkPresenter() {
  rex::Runtime* runtime = rex::Runtime::instance();
  rex::system::IGraphicsSystem* graphics = runtime ? runtime->graphics_system() : nullptr;
  return graphics ? graphics->presenter() : nullptr;
}

// `path`, or the first of <stem>.1<ext>, <stem>.2<ext>... that does not exist: a title launched
// from this one gets this process's arguments (games/common/title_launch.cpp), and PGR4 started
// again after Geometry Wars must not overwrite the first run's files.
std::string UnusedPath(const std::string& path) {
  std::filesystem::path p(path);
  std::error_code ec;
  for (int i = 1; std::filesystem::exists(p, ec); ++i) {
    p = path;
    p.replace_filename(p.stem().string() + "." + std::to_string(i) + p.extension().string());
  }
  return p.string();
}

bool FlagOn(const char* name) {
  const std::string v = rex::cvar::GetFlagByName(name);
  return v == "true" || v == "1";
}

// One column of the frame CSV: its name in the header and its value in this frame's row, so
// that the header and the rows come from one list.
struct CsvCell {
  enum Format : uint8_t { kUnsigned, kSigned, kHex4, kFixed1, kFixed3, kFixed4 };
  const char* name;
  Format format;
  uint64_t u;  // kUnsigned, kSigned (two's complement), kHex4
  double d;    // kFixed*: digits after the point
};
CsvCell Uint(const char* name, uint64_t v) { return {name, CsvCell::kUnsigned, v, 0.0}; }
CsvCell Int(const char* name, int64_t v) { return {name, CsvCell::kSigned, uint64_t(v), 0.0}; }
CsvCell Hex4(const char* name, uint64_t v) { return {name, CsvCell::kHex4, v, 0.0}; }
CsvCell Fix1(const char* name, double v) { return {name, CsvCell::kFixed1, 0, v}; }
CsvCell Fix3(const char* name, double v) { return {name, CsvCell::kFixed3, 0, v}; }
CsvCell Fix4(const char* name, double v) { return {name, CsvCell::kFixed4, 0, v}; }

// The cell's name (header) or value, after a comma unless it is the line's first.
void AppendCsvCell(const CsvCell& c, bool header, bool comma, std::string* line) {
  if (comma) line->push_back(',');
  if (header) {
    line->append(c.name);
    return;
  }
  char text[64];
  int n = 0;
  switch (c.format) {
    case CsvCell::kUnsigned:
      n = std::snprintf(text, sizeof(text), "%llu", (unsigned long long)c.u);
      break;
    case CsvCell::kSigned:
      n = std::snprintf(text, sizeof(text), "%lld", (long long)int64_t(c.u));
      break;
    case CsvCell::kHex4:
      n = std::snprintf(text, sizeof(text), "%04llX", (unsigned long long)c.u);
      break;
    case CsvCell::kFixed1:
      n = std::snprintf(text, sizeof(text), "%.1f", c.d);
      break;
    case CsvCell::kFixed3:
      n = std::snprintf(text, sizeof(text), "%.3f", c.d);
      break;
    case CsvCell::kFixed4:
      n = std::snprintf(text, sizeof(text), "%.4f", c.d);
      break;
  }
  line->append(text, size_t(std::clamp(n, 0, int(sizeof(text)) - 1)));
}

void AppendCsvCells(std::span<const CsvCell> cells, bool header, bool leading_comma,
                    std::string* line) {
  for (size_t i = 0; i < cells.size(); ++i)
    AppendCsvCell(cells[i], header, i || leading_comma, line);
}

// The window's presenter belongs to the UI thread.
void ConnectSdkPresenter(bool connect) {
  rex::ui::Window* window = GameWindow();
  rex::ui::Presenter* presenter = SdkPresenter();
  if (window && presenter)
    window->app_context().CallInUIThreadSynchronous(
        [&] { window->SetPresenter(connect ? presenter : nullptr); });
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Render thread mode (renderer.h).

struct NativeRenderer::Queue {
  // Commands per batch before the guest thread hands it over (and at every Swap). Each
  // hand-over can wake the render thread (a syscall), so batches are large.
  static constexpr size_t kBatchCommands = 256;

  enum Kind : uint8_t { kDraw, kClear, kResolve, kSwap, kQuery };
  struct Cmd {
    Kind kind;
    uint32_t index;    // into the batch's vector of that kind
    uint32_t reg_end;  // Batch::regs up to here are applied before the command
  };
  struct DrawCmd {
    DrawState state;  // regs and immediate ucode pointers are filled in on the render thread
    DrawPrep prep;
    uint32_t copy_begin, copy_end;    // Batch::copies
    uint32_t audit_begin, audit_end;  // Batch::audits
  };
  struct SwapCmd {
    SwapCall call;
    // Guest-thread numbers of the frame, added to the render thread's before its CSV row.
    uint64_t hook_ns, hook_wait_ns, hook_max_ns, state_fail, track_calls, pad;
    uint64_t guest_begin_ns, guest_swap_ns, pace_wait_ns;
    DrawRenderer::PrepareCounters prep;
    uint64_t ticks[profile::kStageCount];
    uint32_t log_frames;  // a marked Swap with --pgr4_native_mark_audit: the frames to log
  };
  struct Batch {
    std::vector<Cmd> cmds;
    RegLog regs;
    std::vector<VertexCopy> copies;
    std::vector<uint32_t> segments;  // indices into copies where a copy segment starts
    std::vector<AuditRange> audits;
    std::vector<DrawCmd> draws;
    std::vector<ClearCall> clears;
    std::vector<ResolveParams> resolves;
    std::vector<SwapCmd> swaps;
    std::vector<QueryCall> queries;
    void Clear() {
      cmds.clear();
      regs.clear();
      copies.clear();
      segments.clear();
      audits.clear();
      draws.clear();
      clears.clear();
      resolves.clear();
      swaps.clear();
      queries.clear();
    }
  };

  std::mutex mutex;
  std::condition_variable work_cv, done_cv;
  std::deque<std::unique_ptr<Batch>> pending;  // guest -> render thread
  std::vector<std::unique_ptr<Batch>> spare;   // render -> guest thread
  uint64_t frames_done = 0;                    // Swaps the render thread finished (mutex)
  // Local change: resolves read back (ResolveParams::readback), queued (guest thread) and
  // written into guest memory (mutex, done_cv).
  uint64_t readbacks_queued = 0, readbacks_done = 0;
  // Frames whose fence the GPU reached (render thread writes; DrawRenderer::Prepare's tags).
  std::atomic<uint64_t> gpu_frames_done{0};
  std::thread thread;

  // Guest thread. The tracker records the register changes straight into the current batch.
  std::unique_ptr<Batch> current = std::make_unique<Batch>();
  Queue() { GuestState().RecordInto(&current->regs); }
  uint64_t frame = 0;  // Swaps queued
  bool capture_armed = false;
  // Time in the On* calls this frame: the Swap's in ns, the others' in ticks, summed per call
  // and converted at the Swap.
  uint64_t hook_ns = 0, hook_wait_ns = 0, hook_max_ns = 0, state_fail = 0;
  uint64_t hook_ticks = 0, hook_max_ticks = 0;
  uint64_t begin_ns = 0, pace_wait_ns = 0;  // the frame being captured (FrameStats)
  void AddHook(uint64_t ticks) {
    hook_ticks += ticks;
    hook_max_ticks = std::max(hook_max_ticks, ticks);
  }

  // Render thread.
  uint32_t regs[kGuestRegisterCount] = {};
  std::deque<std::pair<uint64_t, uint64_t>> frame_fences;  // (frames submitted, their fence)

  void Publish() {
    std::unique_ptr<Batch> next;
    {
      std::lock_guard<std::mutex> lock(mutex);
      pending.push_back(std::move(current));
      if (!spare.empty()) {
        next = std::move(spare.back());
        spare.pop_back();
      }
    }
    work_cv.notify_one();
    current = next ? std::move(next) : std::make_unique<Batch>();
    GuestState().RecordInto(&current->regs);
  }
  void Push(Kind kind, uint32_t index) {
    current->cmds.emplace_back(kind, index, uint32_t(current->regs.size()));
    if (kind != kSwap && current->cmds.size() >= kBatchCommands) Publish();
  }
  // Blocks the guest thread until the render thread finished `frames` Swaps; returns the ns.
  uint64_t WaitFrames(uint64_t frames) {
    std::unique_lock<std::mutex> lock(mutex);
    if (frames_done >= frames) return 0;
    const uint64_t t0 = NowNs();
    done_cv.wait(lock, [&] { return frames_done >= frames; });
    return NowNs() - t0;
  }
};

NativeRenderer* NativeRenderer::Get() {
  if (!(REXCVAR_GET(pgr4_native) || REXCVAR_GET(pgr4_native_primary)) || g_failed) return nullptr;
  // Never destroyed: the process exits with the game, and the guest thread may still be in a
  // hook during static destruction.
  static NativeRenderer* r = [] {
    auto* n = new NativeRenderer();
    if (!n->Initialize()) {
      REXLOG_ERROR("PGR4 native: initialization failed; native path off for this run");
      g_failed = true;
      delete n;
      return static_cast<NativeRenderer*>(nullptr);
    }
    live_.store(n, std::memory_order_release);
    return n;
  }();
  return g_failed ? nullptr : r;
}

// Local change (pgr4 project): bind_screenshot (F1). The next frame shown, as the game drew it
// (the front buffer at the render resolution: the draw resolution scale's, before the
// presentation's scaling to the window), to screenshots\PGR4_<date>_<time>.png beside the
// executable; with pgr4_native_screenshot_textures (development), the textures of the next
// frame's draws beside it (.tsv), to find a texture seen in the picture.
REXCVAR_DEFINE_INT32(pgr4_native_screenshot_burst, 0, "PGR4 native",
                     "Development: F1 takes this many frames in a row (0 or 1: one)");
void NativeRenderer::TakeScreenshot() {
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  const std::filesystem::path dir = std::filesystem::path(exe).parent_path() / "screenshots";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  localtime_s(&tm, &t);
  char name[64];
  std::snprintf(name, sizeof(name), "PGR4_%04d-%02d-%02d_%02d-%02d-%02d-%03d", tm.tm_year + 1900,
                tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                int(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch())
                        .count() %
                    1000));
  present_->RequestScreenshot(dir / (std::string(name) + ".png"));
  if (REXCVAR_GET(pgr4_native_screenshot_textures))
    draw_->RecordFrameTextures(dir / (std::string(name) + ".tsv"));
}

NativeRenderer::~NativeRenderer() {
  if (overlay_bound_) {
    rex::ui::UnregisterBind("bind_overlay");
    rex::ui::UnregisterBind("bind_original_look");
  }
  overlay_.reset();
  if (switch_bound_) rex::ui::UnregisterBind("bind_renderer_switch");
  if (upscaler_bound_) {
    rex::ui::UnregisterBind("bind_upscaler");
    rex::ui::UnregisterBind("bind_upscaler_debug");
    rex::ui::UnregisterBind("bind_screenshot");
  }
}

bool NativeRenderer::Initialize() {
  device_ = std::make_unique<D3D12Device>();
  uint32_t w = uint32_t(std::max(64, REXCVAR_GET(pgr4_native_width)));
  uint32_t h = uint32_t(std::max(64, REXCVAR_GET(pgr4_native_height)));
  const bool primary = primary_ = REXCVAR_GET(pgr4_native_primary);
  const bool paced = primary && REXCVAR_GET(pgr4_native_frame_latency) > 0;
  // The pacer, or with the CSV only its log of the presents shown.
  const bool pacer = paced || (primary && !REXCVAR_GET(pgr4_native_csv).empty());
  HWND game_window = nullptr;
  if (primary) {
    // The first D3D call comes after the SDK opened its window; wait briefly in case not.
    for (int i = 0; i < 100 && !game_window; ++i) {
      if (rex::ui::Window* window = GameWindow())
        game_window = static_cast<HWND>(window->GetNativeWindowHandle());
      if (!game_window) Sleep(50);
    }
    if (!game_window) {
      REXLOG_ERROR("PGR4 native: --pgr4_native_primary: no game window found");
      return false;
    }
    // The pacer needs FIFO: vkd3d-proton releases the frame latency waitable on the display's
    // present feedback only for capped present modes, and on the blit for the others. Without
    // the pacer, the same switch as the SDK presenter's.
    device_->set_vsync(paced || FlagOn("d3d12_present_vsync"));
    device_->set_frame_latency_waitable(pacer);
  }
  if (!device_->Initialize(w, h, primary ? L"PGR4 native" : L"PGR4 native (debug)", game_window))
    return false;
  if (REXCVAR_GET(pgr4_native_gpu_timing)) {
    if (device_->EnableGpuTimer()) {
      gpu_timer_ = device_->gpu_timer();
      gpu_timer_->set_per_shader(REXCVAR_GET(pgr4_native_gpu_timing_shaders));
    } else
      REXLOG_ERROR("PGR4 native: GPU timing unavailable");
  }
  // Local change: the in-game settings overlay (overlay.h).
  overlay_ = std::make_unique<Overlay>(*device_);
  if (primary) {
    rex::ui::RegisterBind("bind_overlay", "F2",
                          "PGR4 native: show or hide the settings overlay (DLSS, FSR, frame "
                          "generation, resolution)",
                          [this] {
                            if (overlay_) overlay_->Toggle();
                          });
    // Local change (pgr4 project): in a release (no settings overlay, its key empty in the toml), F2
    // switches between the game as it was and our version, live and not saved. The developer's F2
    // stays the overlay. Also on a toml made before this key existed.
    rex::ui::RegisterBind("bind_original_look", "",
                          "PGR4 native: switch between the game as it was and the enhanced version",
                          [] {
                            const bool original = !REXCVAR_GET(pgr4_native_original_look);
                            REXCVAR_SET(pgr4_native_original_look, original);
                            REXLOG_INFO("PGR4 native: {}", original ? "the game as it was" : "enhanced");
                          });
    if (rex::cvar::GetFlagByName("bind_overlay").empty() &&
        rex::cvar::GetFlagByName("bind_original_look").empty())
      rex::cvar::SetFlagByName("bind_original_look", "F2");
    overlay_bound_ = true;
  }
  paced_ = paced;
  if (!CreatePipeline()) return false;
  // Beside the Xenos plugin (debug window) the plugin's fake counts are the game's.
  if (primary && REXCVAR_GET(pgr4_native_occlusion)) {
    occlusion_ = std::make_unique<OcclusionQueries>();
    if (!occlusion_->Initialize(device_->device()) ||
        !rex::cvar::SetFlagByName("occlusion_query_external", "true")) {
      REXLOG_ERROR(
          "PGR4 native: occlusion queries unavailable (patches/0049 missing?); every query "
          "reports its draw visible");
      occlusion_.reset();
    }
  }
  // Only once native is up: from here the Xenos plugin does no draw, copy or swap work, and its
  // command processor only runs fences, write-backs and interrupts.
  if (primary && !rex::cvar::SetFlagByName("gpu_skip_draws", "true"))
    REXLOG_ERROR(
        "PGR4 native: could not set gpu_skip_draws (patches/0031 missing?); "
        "Xenos keeps drawing");
  // A handheld has no keyboard for the replay name prompt; the title's default name is what
  // confirming the dialog unchanged returns.
  if (primary && !rex::cvar::SetFlagByName("xam_keyboard_accept_default", "true"))
    REXLOG_ERROR(
        "PGR4 native: could not set xam_keyboard_accept_default "
        "(patches/0024 missing?)");
  if (primary && REXCVAR_GET(pgr4_native_renderer_switch)) {
    rex::ui::RegisterBind("bind_renderer_switch", "F9",
                          "PGR4 native: switch between the native renderer and the Xenos plugin",
                          [this] { switch_requested_.store(true, std::memory_order_relaxed); });
    switch_bound_ = true;
  }
  if (primary) {
    // Local change (pgr4 project): F7 and F8 are development keys, without a default (a release
    // has neither); the developer's pgr4.toml sets them (bind_upscaler = "F7",
    // bind_upscaler_debug = "F8").
    rex::ui::RegisterBind(
        "bind_upscaler", "",
        "PGR4 native: switch the 3D scene between MSAA and FidelityFX Super Resolution",
        [this] { upscaler_switch_requested_.store(true, std::memory_order_relaxed); });
    rex::ui::RegisterBind(
        "bind_upscaler_debug", "",
        "PGR4 native: show FidelityFX Super Resolution's debug view (its inputs and history)",
        [this] { upscaler_debug_requested_.store(true, std::memory_order_relaxed); });
    rex::ui::RegisterBind(
        "bind_screenshot", "F1",
        "PGR4: screenshot at the render resolution (screenshots\\ beside the executable)",
        [this] { screenshot_requested_.store(true, std::memory_order_relaxed); });
    upscaler_bound_ = true;
  }
  csv_ = std::make_unique<Csv>();
  profile::enabled.store(REXCVAR_GET(pgr4_native_profile), std::memory_order_relaxed);
  if (profile::On()) LoadShaderFamilies();  // local change
  const std::string path = REXCVAR_GET(pgr4_native_csv).empty()
                               ? std::string()
                               : UnusedPath(REXCVAR_GET(pgr4_native_csv));
  if (!path.empty()) {
    csv_->file = AsyncFile::Create(path);
    if (csv_->file) {
      csv_->luma = REXCVAR_GET(pgr4_native_luma_probe);
      std::string header;
      AppendCsvColumns(FrameStats{}, 0, true, &header);
      csv_profile_ = profile::On();  // local change
      if (csv_profile_) AppendCsvProfileColumns(true, &header);
      if (csv_->luma) AppendCsvLumaColumns(true, &header);
      header += '\n';
      csv_->file->Append(header);
    } else {
      REXLOG_ERROR("PGR4 native: cannot open {}", path);
    }
  }
  if (pacer) {
    std::unique_ptr<AsyncFile> log;
    if (!path.empty()) {
      const std::string log_path =
          path.substr(0, path.size() - (path.ends_with(".csv") ? 4 : 0)) + "-presents.csv";
      if (!(log = AsyncFile::Create(log_path)))
        REXLOG_ERROR("PGR4 native: cannot open {}", log_path);
    }
    pacer_ = std::make_unique<FramePacer>(FramePacer::Options{
        .frame_latency = uint32_t(std::max(0, REXCVAR_GET(pgr4_native_frame_latency))),
        .low_latency = REXCVAR_GET(pgr4_native_low_latency),
        .latency_margin_ns =
            uint64_t(std::max(0, REXCVAR_GET(pgr4_native_latency_margin_us))) * 1000,
        .log = std::move(log)});
    if (pacer_->frame_latency() > 0)
      REXLOG_INFO("PGR4 native: frames paced on the display, at most {} begun but not shown",
                  pacer_->frame_latency());
  }
  frame_start_ns_ = NowNs();
  if (REXCVAR_GET(pgr4_native_render_thread)) {
    queue_ = std::make_unique<Queue>();
    queue_->thread = std::thread([this] { RenderThreadMain(); });
    REXLOG_INFO("PGR4 native: recording on the render thread");
  }
  return true;
}

// The pipeline from the render targets to the Presenter (Initialize's; local change: also
// RebuildPipeline's, for a new draw resolution scale or anisotropy). False (logged) on failure.
bool NativeRenderer::CreatePipeline() {
  rt_ = std::make_unique<RenderTargets>();
  if (!rt_->Initialize(device_->device())) return false;
  if (const int32_t msaa = REXCVAR_GET(pgr4_native_msaa);
      msaa == 1 || msaa == 2 || msaa == 4 || msaa == 8) {
    scene_msaa_ = rt_->SupportedSamples(uint32_t(msaa));
    if (scene_msaa_ != uint32_t(msaa))
      REXLOG_WARN("PGR4 native: {}x MSAA unsupported; the scene takes {}x", msaa, scene_msaa_);
  } else {
    REXLOG_ERROR("PGR4 native: --pgr4_native_msaa={} is not 1, 2, 4 or 8", msaa);
  }
  resolver_ = std::make_unique<Resolver>();
  if (!resolver_->Initialize(device_->device(), rt_.get(), device_->command_queue())) return false;
  resolver_->set_gpu_timer(gpu_timer_);
  scene_effects_ = std::make_unique<SceneEffects>();
  if (scene_effects_->Initialize(device_->device())) {
    scene_effects_->set_camera(&camera_);
    resolver_->set_scene_effects(scene_effects_.get());
  } else {
    REXLOG_ERROR("PGR4 native: scene effects (GTAO) unavailable");
    scene_effects_.reset();
  }
  if (FrameGeneration::Requested() && !paced_)
    REXLOG_ERROR("PGR4 native: frame generation needs the frame pacer (--pgr4_native_primary)");
  ApplyFsrFrameGeneration();
  // Local change: DLSS-G's HUD-less image, also beside FidelityFX's frame generation (the game's
  // Graphics page switches between them with the upscaler, pgr4_menu.cpp).
  if (streamline::FrameGenerationRequested() && REXCVAR_GET(pgr4_native_dlss_fg_hudless))
    hudless_capture_ = std::make_unique<HudlessCapture>(*device_, *resolver_);
  SwitchUpscaler(Upscaler::Requested());
  textures_ = std::make_unique<TextureCache>(device_->device(), rex::system::kernel_memory());
  textures_->SetResolvedLookup([this](uint32_t a) { return resolver_->Find(a); });
  textures_->SetGuestSize(
      [this](ID3D12GraphicsCommandList* list, const ResolvedImage& image, ID3D12Resource* source) {
        return resolver_->GuestSize(list, image, source);
      });
  textures_->SetCaptureOrder(REXCVAR_GET(pgr4_native_texture_capture_order));
  resolver_->SetChangedCallback([this](uint32_t a) { textures_->OnResolved(a); });
  draw_ = std::make_unique<DrawRenderer>(*this);
  if (!draw_->Initialize()) return false;
  resolver_->set_scene_end([this](ID3D12GraphicsCommandList* list, RenderTargets& targets,
                                  const SurfaceRegs& regs) {
    return draw_->OnSceneEnd(list, targets, regs);
  });
  present_ = std::make_unique<Presenter>(*this);
  if (!present_->Initialize()) return false;
  return true;
}

// Local change (pgr4 project): the pipeline made again with the settings the overlay changed
// (overlay.h): draw resolution scale and anisotropy. The device, swapchain, Streamline and pacer
// stay; the textures and resolved images start over (each frame's are made again by its draws).
bool NativeRenderer::RebuildPipeline() {
  // Between frames (a Swap's end, the render thread idle): no recorded draw holds the old one.
  const uint64_t t0 = NowNs();
  upscaler_recreate_requested_.store(false, std::memory_order_relaxed);
  device_->WaitIdle();
  const bool on = upscaler_on_;
  upscaler_.reset();
  upscaler_on_ = false;
  hudless_capture_.reset();
  frame_generation_.reset();
  present_.reset();
  draw_.reset();
  textures_.reset();
  resolver_.reset();
  rt_.reset();
  SetDrawResolutionScale(uint32_t(REXCVAR_GET(pgr4_native_resolution_scale)));
  if (!CreatePipeline()) {
    REXLOG_ERROR("PGR4 native: the pipeline could not be made again");
    return false;
  }
  SwitchUpscaler(on);
  REXLOG_INFO("PGR4 native: pipeline made again at resolution scale {} in {} ms",
              DrawResolutionScale(), (NowNs() - t0) / 1000000);
  return true;
}

// Local change (pgr4 project): FidelityFX's frame generation made or dropped to follow
// --pgr4_native_frame_generation (the game's Graphics page: RequestFsrFrameGeneration), with
// the frame pacer only. Between frames.
void NativeRenderer::ApplyFsrFrameGeneration() {
  const bool want = FrameGeneration::Requested() && paced_;
  if (want && !frame_generation_) {
    frame_generation_ = std::make_unique<FrameGeneration>(*device_, *resolver_);
    if (!frame_generation_->Initialize()) {
      REXLOG_ERROR("PGR4 native: frame generation unavailable");
      frame_generation_.reset();
    }
  } else if (!want && frame_generation_) {
    device_->WaitIdle();
    frame_generation_.reset();
  }
  if (upscaler_) upscaler_->set_frame_generation(frame_generation_.get());
}

// Local change (pgr4 project): --pgr4_native_msaa changed (overlay.h): the scene's sample count
// from the next frame on.
void NativeRenderer::ApplyMsaa() {
  if (const int32_t msaa = REXCVAR_GET(pgr4_native_msaa);
      msaa == 1 || msaa == 2 || msaa == 4 || msaa == 8)
    scene_msaa_ = rt_->SupportedSamples(uint32_t(msaa));
  if (!upscaler_on_) SwitchUpscaler(false);
}

ID3D12Device* NativeRenderer::d3d12() const { return device_->device(); }
ID3D12CommandQueue* NativeRenderer::command_queue() const { return device_->command_queue(); }
ID3D12GraphicsCommandList* NativeRenderer::command_list() const { return device_->command_list(); }

namespace {
// Development (--pgr4_native_profile, 18/11/2026): the vertex shaders of the cars (names vsCar*)
// and of the animated characters (vsSkinnedModel*) from shaders\shader_names.tsv beside the
// executable (hash, stage, name...), for the CSV's draw_car / cap_car / draws_car columns (and
// _skin). Read-only once loaded: both threads look them up.
std::unordered_set<uint64_t> g_car_vs, g_skin_vs;
std::unordered_map<uint64_t, std::string> g_shader_names;  // every shader's (LogPasses)
}  // namespace

void NativeRenderer::LoadShaderFamilies() {
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  const std::filesystem::path path =
      std::filesystem::path(exe).parent_path() / "shaders" / "shader_names.tsv";
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    const size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1);
    if (b == std::string::npos) continue;
    if (line.compare(a + 1, b - a - 1, "vs") != 0) {
      // The pixel shaders' names too (LogPasses).
      g_shader_names[std::strtoull(line.substr(0, a).c_str(), nullptr, 16)] =
          line.substr(b + 1, line.find('\t', b + 1) - b - 1);
      continue;
    }
    const std::string name = line.substr(b + 1, line.find('\t', b + 1) - b - 1);
    const uint64_t hash = std::strtoull(line.substr(0, a).c_str(), nullptr, 16);
    g_shader_names[hash] = name;
    if (name.rfind("vsCar", 0) == 0) g_car_vs.insert(hash);
    else if (name.rfind("vsSkinnedModel", 0) == 0) g_skin_vs.insert(hash);
  }
  REXLOG_INFO("PGR4 native profile: {} car and {} character vertex shaders from {}",
              g_car_vs.size(), g_skin_vs.size(), path.string());
}

int NativeRenderer::VsFamily(const DrawPrep* prep) {
  if (!prep || !prep->vs) return 0;
  const uint64_t hash = DrawRenderer::ShaderHashes(*prep).first;
  return g_car_vs.count(hash) ? 1 : g_skin_vs.count(hash) ? 2 : 0;
}
ID3D12GraphicsCommandList* NativeRenderer::recording_list() const {
  return device_->recording_list();
}
uint64_t NativeRenderer::frame_fence() const { return device_->frame_fence(); }

void NativeRenderer::SwitchUpscaler(bool on) {
  if (on && !upscaler_) {
    upscaler_ = std::make_unique<Upscaler>();
    if (!upscaler_->Initialize(device_->device())) {
      REXLOG_ERROR("PGR4 native: upscaler unavailable; the scene keeps its MSAA");
      upscaler_.reset();
      return;
    }
    upscaler_->set_frame_generation(frame_generation_.get());
    upscaler_->set_camera(&camera_);
  }
  upscaler_on_ = on;
  // The scene's targets are keyed by their sample count: the next draws make the other ones,
  // and the unused ones age out (RenderTargets::BeginFrame).
  if (on) {
    rt_->OverrideScene(Upscaler::kSceneSurfaceMask, Upscaler::kSceneSurface, 1,
                       upscaler_->max_scale());
    rt_->SetSceneScale(upscaler_->scale());
    upscaler_->Reset();
  } else if (scene_msaa_ != 2) {
    rt_->OverrideScene(Upscaler::kSceneSurfaceMask, Upscaler::kSceneSurface, scene_msaa_,
                       UniformScale(DrawResolutionScale()));
  } else {
    rt_->OverrideScene(0, 0, 1, {});
  }
  resolver_->set_upscaler(upscaler());
  if (on)
    REXLOG_INFO("PGR4 native: the scene upscaled by FidelityFX");
  else
    REXLOG_INFO("PGR4 native: the scene with {}x MSAA", scene_msaa_);
}

void NativeRenderer::BeginFrameModules() {
  // Local change: the frame's Streamline token, Reflex sleep and DLSS-G mode (streamline.h).
  streamline::BeginFrame(frame_index_);
  {
    // Local change: QueueSettings' (the game's Graphics page), before the requests they make.
    std::vector<std::function<void()>> settings;
    {
      std::lock_guard<std::mutex> lock(settings_mutex_);
      settings.swap(queued_settings_);
    }
    for (auto& fn : settings) fn();
  }
  if (upscaler_switch_requested_.exchange(false, std::memory_order_relaxed))
    SwitchUpscaler(!upscaler_on_);
  // Local change (pgr4 project), debug: F1 takes pgr4_native_screenshot_burst frames in a row (each
  // once the previous one is copied), to see what changes from one frame to the next.
  // The burst's frames are raw files (present.cpp), screenshotsurst_<n>.raw.
  static int burst_left = 0, burst_index = 0;
  static uint32_t burst_after = 0;
  const auto burst_shot = [&] {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    present_->RequestScreenshot(std::filesystem::path(exe).parent_path() / "screenshots" /
                                ("burst_" + std::to_string(burst_index++) + ".raw"));
    burst_after = Presenter::ScreenshotsCopied() + 1;
  };
  if (screenshot_requested_.exchange(false, std::memory_order_relaxed)) {
    if (REXCVAR_GET(pgr4_native_screenshot_burst) > 1) {
      burst_index = 0;
      burst_left = int(REXCVAR_GET(pgr4_native_screenshot_burst)) - 1;
      burst_shot();
    } else {
      TakeScreenshot();
    }
  } else if (burst_left > 0 && Presenter::ScreenshotsCopied() >= burst_after) {
    --burst_left;
    burst_shot();
  }
  // Local change: the photo mode's screenshots without its interface (src/pgr4_menu.cpp, Start
  // in PhotoMode\PhotoMode.lua): asked once its interface is hidden, taken like F1's.
  if (const uint32_t asked = g_clean_shots_asked.load(std::memory_order_acquire);
      asked != clean_shots_handled_) {
    clean_shots_handled_ = asked;
    g_clean_shot_target.store(Presenter::ScreenshotsCopied() + 1, std::memory_order_release);
    TakeScreenshot();
  }
  // Local change: the overlay's live changes (overlay.h): the scene's MSAA, the upscaler made
  // again for its new settings (the whole pipeline: at a Swap, RebuildPipeline).
  if (msaa_requested_.exchange(false, std::memory_order_relaxed)) ApplyMsaa();
  if (fsr_fg_requested_.exchange(false, std::memory_order_relaxed)) ApplyFsrFrameGeneration();
  if (upscaler_recreate_requested_.exchange(false, std::memory_order_relaxed) && upscaler_) {
    const bool on = upscaler_on_;
    device_->WaitIdle();
    if (on) SwitchUpscaler(false);
    resolver_->set_upscaler(nullptr);
    upscaler_.reset();
    if (on) SwitchUpscaler(true);
  }
  if (upscaler_debug_requested_.exchange(false, std::memory_order_relaxed) && upscaler()) {
    // The context made again: its old resources may be in the lists still running.
    if (!upscaler_->debug_context()) device_->WaitIdle();
    if (!upscaler_->SetDebugView(!upscaler_->debug_view())) {
      REXLOG_ERROR("PGR4 native: upscaler unavailable; the scene keeps its MSAA");
      upscaler_.reset();
      SwitchUpscaler(false);
    }
  }
  const uint64_t fence = device_->frame_fence(), done = device_->completed_fence();
  rt_->BeginFrame(frame_index_, fence, done);
  resolver_->set_linear_output(device_->hdr());
  resolver_->BeginFrame(frame_index_, fence, done);
  if (Upscaler* u = upscaler(); u && u->BeginFrame(frame_index_, device_->gpu_busy_ns()))
    rt_->SetSceneScale(u->scale());
  frame_begun_ = true;
}

void NativeRenderer::RenderThreadMain() {
  SetThreadDescription(GetCurrentThread(), L"PGR4 native render");
  Queue& q = *queue_;
  for (;;) {
    std::unique_ptr<Queue::Batch> b;
    {
      std::unique_lock<std::mutex> lock(q.mutex);
      q.work_cv.wait(lock, [&] { return !q.pending.empty(); });
      b = std::move(q.pending.front());
      q.pending.pop_front();
    }
    size_t reg = 0;
    uint32_t copies_issued = 0;
    size_t segment = 0;
    for (const Queue::Cmd& c : b->cmds) {
      stats_.reg_writes += c.reg_end - reg;
      for (; reg < c.reg_end; ++reg) q.regs[b->regs[reg].reg] = b->regs[reg].value;
      // A read-back resolve: the guest thread waits for it, also after a failure.
      if (c.kind == Queue::kResolve && b->resolves[c.index].readback) {
        if (!g_failed) {
          ExecuteResolve(b->resolves[c.index]);
          ReadBackResolve(b->resolves[c.index]);
        }
        {
          std::lock_guard<std::mutex> lock(q.mutex);
          ++q.readbacks_done;
        }
        q.done_cv.notify_all();
        continue;
      }
      if (g_failed && c.kind != Queue::kSwap) continue;
      switch (c.kind) {
        case Queue::kDraw: {
          Queue::DrawCmd& d = b->draws[c.index];
          d.state.regs = q.regs;
          // A draw whose copies are not recorded yet is the first of its copy segment to
          // need any: it records the whole segment's (DrawRenderer::Prepare).
          uint32_t to = copies_issued;
          if (d.copy_end > copies_issued) {
            while (segment < b->segments.size() && b->segments[segment] <= copies_issued) ++segment;
            to = segment < b->segments.size() ? b->segments[segment] : uint32_t(b->copies.size());
            to = std::max(to, d.copy_end);
          }
          d.prep.copies = b->copies.data() + copies_issued;
          d.prep.copy_count = to - copies_issued;
          d.prep.audit = b->audits.data() + d.audit_begin;
          d.prep.audit_count = d.audit_end - d.audit_begin;
          copies_issued = to;
          ExecuteDraw(d.state, &d.prep);
          break;
        }
        case Queue::kClear:
          ExecuteClear(b->clears[c.index]);
          break;
        case Queue::kResolve:
          ExecuteResolve(b->resolves[c.index]);
          break;
        case Queue::kQuery:
          ExecuteQuery(b->queries[c.index]);
          break;
        case Queue::kSwap: {
          const Queue::SwapCmd& s = b->swaps[c.index];
          if (!g_failed) {
            stats_.hook_ns = s.hook_ns;
            stats_.hook_wait_ns = s.hook_wait_ns;
            stats_.hook_max_ns = s.hook_max_ns;
            stats_.pad = s.pad;
            stats_.guest_begin_ns = s.guest_begin_ns;
            stats_.guest_swap_ns = s.guest_swap_ns;
            stats_.pace_wait_ns = s.pace_wait_ns;
            stats_.state_fail += s.state_fail;
            draw_->AddPrepareCounters(s.prep);
            if (s.log_frames) draw_->LogFrames(s.log_frames);
            if (profile::On()) {
              for (uint32_t i = 0; i < profile::kStageCount; ++i) profile::g.ticks[i] += s.ticks[i];
              profile::g.track_calls += s.track_calls;
            }
            ExecuteSwap(s.call);
          }
          {
            std::lock_guard<std::mutex> lock(q.mutex);
            ++q.frames_done;
          }
          q.done_cv.notify_all();
          break;
        }
      }
    }
    b->Clear();
    std::lock_guard<std::mutex> lock(q.mutex);
    q.spare.push_back(std::move(b));
  }
}

void NativeRenderer::QueueDraw(const uint8_t* base, const HookArgs& args) {
  const uint64_t t0 = Ticks();
  profile::Begin();
  Queue& q = *queue_;
  Queue::Batch& b = *q.current;
  Queue::DrawCmd& d = b.draws.emplace_back();
  // The render thread wrote the next slot last (batches come back through `spare`): its cache
  // lines are fetched for writing while this draw is captured.
  for (size_t offset = 0; offset < sizeof(Queue::DrawCmd); offset += 64)
    __builtin_prefetch(reinterpret_cast<const char*>(&d + 1) + offset, 1);
  const bool ok = GuestState().ReadDrawState(base, args, &d.state);
  profile::Mark(profile::kReadState);
  if (!ok) {
    b.draws.pop_back();
    ++q.state_fail;
  } else {
    // The render thread records a batch's copies while it runs the batch: each batch starts
    // a copy segment.
    if (b.draws.size() == 1) draw_->BeginCopySegment();
    d.copy_begin = uint32_t(b.copies.size());
    d.audit_begin = uint32_t(b.audits.size());
    // Upload memory of frame F is tagged F + 1: free once F's fence completed.
    draw_->Prepare(d.state, q.frame + 1, &d.prep, &b.copies, &b.audits);
    d.copy_end = uint32_t(b.copies.size());
    d.audit_end = uint32_t(b.audits.size());
    if (d.prep.copy_segment) b.segments.push_back(d.copy_begin);
    d.state.regs = nullptr;
    d.state.vs.immediate = d.state.ps.immediate = nullptr;  // Prepare loaded the shaders
    q.Push(Queue::kDraw, uint32_t(b.draws.size() - 1));
    profile::Mark(profile::kCapRecord);
    // Development: the cars' and characters' share of the guest thread (profile.h kCapCar).
    if (profile::On()) [[unlikely]] {
      if (const int family = VsFamily(&d.prep))
        profile::g.ticks[family == 1 ? profile::kCapCar : profile::kCapSkin] += Ticks() - t0;
    }
  }
  q.AddHook(Ticks() - t0);
}

void NativeRenderer::QueueClear(const uint8_t* base, const HookArgs& args) {
  const uint64_t t0 = Ticks();
  profile::Scope scope(profile::kCapRecord);
  Queue& q = *queue_;
  Queue::Batch& b = *q.current;
  ClearCall& c = b.clears.emplace_back();
  if (RenderTargets::DecodeClear(base, args, &c))
    q.Push(Queue::kClear, uint32_t(b.clears.size() - 1));
  else
    b.clears.pop_back();
  q.AddHook(Ticks() - t0);
}

void NativeRenderer::QueueResolve(const uint8_t* base, const HookArgs& args) {
  const uint64_t t0 = Ticks();
  profile::Scope scope(profile::kCapRecord);
  Queue& q = *queue_;
  Queue::Batch& b = *q.current;
  ResolveParams& p = b.resolves.emplace_back();
  if (DecodeResolve(base, args, &p)) {
    p.readback = WantsReadBack(p);
    const bool readback = p.readback;
    q.Push(Queue::kResolve, uint32_t(b.resolves.size() - 1));
    // The game reads it back from memory: the render thread runs it now, and the guest
    // continues once its texels are there.
    if (readback) {
      const uint64_t ticket = ++q.readbacks_queued;
      q.Publish();
      std::unique_lock<std::mutex> lock(q.mutex);
      q.done_cv.wait(lock, [&] { return q.readbacks_done >= ticket; });
    }
  } else {
    b.resolves.pop_back();  // logged once per reason by DecodeResolve
  }
  q.AddHook(Ticks() - t0);
}

// Small color resolves into k_5_6_5, k_6_5_5 or k_8_8_8_8 (the team logo, liveries), the first
// two times into each address: those the game composes once and then reads on the CPU. Resolves
// it repeats every frame (reflections) stop being read back after their second.
bool NativeRenderer::WantsReadBack(const ResolveParams& p) {
  using rex::graphics::xenos::TextureFormat;
  if (!REXCVAR_GET(pgr4_native_resolve_readback) || p.source == 4 || p.dest_width > 512 ||
      p.dest_height > 512)
    return false;
  if (p.dest_format != uint32_t(TextureFormat::k_5_6_5) &&
      p.dest_format != uint32_t(TextureFormat::k_6_5_5) &&
      p.dest_format != uint32_t(TextureFormat::k_8_8_8_8))
    return false;
  static std::unordered_map<uint32_t, uint32_t> counts;  // guest thread
  return ++counts[p.dest_base] <= 2;
}

void NativeRenderer::ReadBackResolve(const ResolveParams& p) {
  if (g_failed || !resolver_->RecordReadBack(device_->command_list(), p)) return;
  // The texture copies recorded so far read upload memory the untile workers fill.
  while (!textures_->UploadsDone()) std::this_thread::yield();
  if (!CloseUploadLists() || !device_->Submit(upload_lists_)) return Fail();
  draw_->InvalidateListState(DrawRenderer::ListBreak::kSubmit);
  draws_submitted_ = stats_.draws;
  ++stats_.submits;
  device_->WaitIdle();
  resolver_->WriteReadBack(p);
}

void NativeRenderer::QueueSwap(uint8_t* base, const HookArgs& args) {
  // The render thread failed a frame (Fail): what was queued since is dropped.
  if (g_failed) [[unlikely]]
    return HandOver(base, args.device);
  const uint64_t t0 = NowNs();
  Queue& q = *queue_;
  Queue::Batch& b = *q.current;
  Queue::SwapCmd& s = b.swaps.emplace_back();
  s.guest_begin_ns = q.begin_ns;
  s.guest_swap_ns = t0;
  s.pace_wait_ns = q.pace_wait_ns;
  Presenter::ReadSwap(base, args, &s.call);
  // Captures with the Xenos copy: from the Swap that arms one (HoldFullReadback) to the one
  // captured, the next shown (DrawRenderer::FrameIncomplete holds frames back), every Swap runs
  // before the guest continues, as without the render thread.
  bool sync = q.capture_armed, requested = false;
  if (sync) {
    s.call.capture_request = 0;
  } else {
    requested = present_->CaptureRequested(q.frame);
    s.call.capture_request = requested ? 1 : 0;
    sync = requested && present_->capture_holds_guest();
  }
  s.prep = draw_->EndFramePrepare(q.gpu_frames_done.load(std::memory_order_acquire));
  s.log_frames = requested ? uint32_t(std::max(0, REXCVAR_GET(pgr4_native_mark_audit))) : 0;
  if (s.log_frames) draw_->StartAudit(s.log_frames);
  // At most one frame ahead: frame F is queued once the render thread finished F - 1.
  const uint64_t wait = q.WaitFrames(q.frame);
  q.hook_wait_ns += wait;
  s.state_fail = q.state_fail;
  s.hook_wait_ns = q.hook_wait_ns;
  const uint64_t t1 = NowNs();
  s.hook_ns = q.hook_ns + TicksToNs(q.hook_ticks) + (t1 - t0);
  s.hook_max_ns = std::max({q.hook_max_ns, TicksToNs(q.hook_max_ticks), t1 - t0 - wait});
  s.track_calls = profile::g.track_calls;
  s.pad = rex_xam_take_frame_pad();
  const bool hand_over = s.call.hand_over = SwitchRequested();
  std::memcpy(s.ticks, profile::g.ticks, sizeof(s.ticks));
  for (uint64_t& t : profile::g.ticks) t = 0;
  profile::g.track_calls = 0;
  q.state_fail = q.hook_ns = q.hook_wait_ns = q.hook_ticks = q.hook_max_ticks = 0;
  q.Push(Queue::kSwap, uint32_t(b.swaps.size() - 1));
  q.Publish();
  ++q.frame;
  // Handing over and a capture's wait count towards the next frame.
  const uint64_t capture_wait = sync ? q.WaitFrames(q.frame) : 0;
  // The render thread has run this Swap: the capture is taken, or waits for a shown frame.
  if (sync) q.capture_armed = present_->capture_holding_guest();
  const uint64_t pace_wait = pacer_ ? pacer_->WaitToBegin() + LimitFrameRate() : 0;
  const uint64_t t2 = NowNs();
  // The pacer's wait is the gap between two frames, not time in the hook.
  const uint64_t tail = t2 - t1 - pace_wait;
  q.hook_wait_ns = capture_wait;
  q.hook_ns = tail;
  q.hook_max_ns = tail - capture_wait;
  q.begin_ns = t2;
  q.pace_wait_ns = pace_wait;
  // Local change: the overlay's pipeline rebuild (overlay.h) once the render thread ran every
  // queued frame: it then waits for the next batch, and DrawRenderer::Prepare (this thread's)
  // starts over with the new pipeline.
  if (rebuild_requested_.exchange(false, std::memory_order_relaxed)) {
    q.WaitFrames(q.frame);
    if (!RebuildPipeline()) g_failed = true;
  }
  if (hand_over) HandOver(base, args.device);
}

// ---------------------------------------------------------------------------------------------
// Hook entry points.

void NativeRenderer::OnDraw(const uint8_t* base, const HookArgs& args) {
  if (queue_) return QueueDraw(base, args);
  if (g_failed) [[unlikely]]
    return;
  profile::Begin();
  DrawState state;
  const bool ok = GuestState().ReadDrawState(base, args, &state);
  profile::Mark(profile::kReadState);
  if (ok)
    ExecuteDraw(state, nullptr);
  else
    ++stats_.state_fail;
}

void NativeRenderer::OnClear(const uint8_t* base, const HookArgs& args) {
  if (queue_) return QueueClear(base, args);
  if (g_failed) [[unlikely]]
    return;
  if (ClearCall c; RenderTargets::DecodeClear(base, args, &c)) ExecuteClear(c);
}

void NativeRenderer::OnResolve(const uint8_t* base, const HookArgs& args) {
  if (queue_) return QueueResolve(base, args);
  if (g_failed) [[unlikely]]
    return;
  if (ResolveParams p; DecodeResolve(base, args, &p)) {
    p.readback = WantsReadBack(p);
    ExecuteResolve(p);
    if (p.readback) ReadBackResolve(p);
  }
}

// The game's own switch, Render.PostProcess.MotionBlur.Enable (a byte at 0x82C98CA4, its engine
// variable: work/rea/variables_moteur.tsv), which its post-processing (sub_8285A848) reads at each
// frame before its blur pass, and which the photo mode's shutter (sub_824B86F8) saves, sets to 0
// for its sub-frames and puts back. While DLSS runs, out of the photo mode: 0 at each frame. When
// DLSS stops or the photo mode starts: the menu's choice, once, and nothing after (the photo mode's
// shutter keeps its own saving and putting back). Guest thread, at the Swap, before the next frame's
// post-processing.
void NativeRenderer::ApplyMotionBlurRule(uint8_t* base) {
  constexpr uint32_t kMotionBlurEnable = 0x82C98CA4;
  uint8_t& enable = base[kMotionBlurEnable];
  const bool photo = (REXCVAR_GET(pgr4_native_dlss_blur_in_photo_mode) && Pgr4PhotoModeActive()) ||
                     (REXCVAR_GET(pgr4_native_dlss_blur_in_pause) && g_pgr4_game_paused);
  if (dlss_running() && REXCVAR_GET(pgr4_native_dlss_no_motion_blur) && !photo) {
    if (!blur_held_off_) REXLOG_INFO("PGR4 native: the game's motion blur off while DLSS runs");
    enable = 0;
    blur_held_off_ = true;
  } else if (blur_held_off_) {
    enable = REXCVAR_GET(pgr4_disable_motion_blur) ? 0 : 1;
    blur_held_off_ = false;
    REXLOG_INFO("PGR4 native: the game's motion blur back to the menu's choice ({}){}",
                enable ? "on" : "off", photo ? ", the photo mode or the pause" : "");
  }
}

void NativeRenderer::OnSwap(uint8_t* base, const HookArgs& args) {
  ApplyMotionBlurRule(base);  // local change
  if (queue_) return QueueSwap(base, args);
  if (g_failed) [[unlikely]]
    return HandOver(base, args.device);
  stats_.guest_swap_ns = NowNs();
  SwapCall swap;
  Presenter::ReadSwap(base, args, &swap);
  stats_.pad = rex_xam_take_frame_pad();
  swap.hand_over = SwitchRequested();
  ExecuteSwap(swap);
  // stats_ is the next frame's from here.
  stats_.pace_wait_ns = pacer_ ? pacer_->WaitToBegin() + LimitFrameRate() : 0;
  stats_.guest_begin_ns = NowNs();
  // Local change: the overlay's pipeline rebuild (overlay.h), between frames.
  if (rebuild_requested_.exchange(false, std::memory_order_relaxed) && !RebuildPipeline())
    g_failed = true;
  if (swap.hand_over || g_failed) HandOver(base, args.device);
}

void NativeRenderer::OnQueryIssue(const uint8_t* base, const HookArgs& args) {
  // D3DQUERYTYPE_OCCLUSION at query+4, and D3DISSUE_END / D3DISSUE_BEGIN; QueryIssue takes BEGIN
  // when both are set.
  constexpr uint32_t kTypeOcclusion = 9, kIssueEnd = 1, kIssueBegin = 2;
  if (!occlusion_ || g_failed) return;
  const uint32_t query = args.r[0], flags = args.r[1];
  if (!query || Load32(base, query + 4) != kTypeOcclusion || !(flags & (kIssueBegin | kIssueEnd)))
    return;
  const QueryCall c = {query, !(flags & kIssueBegin)};
  if (!queue_) return ExecuteQuery(c);
  Queue::Batch& b = *queue_->current;
  b.queries.push_back(c);
  queue_->Push(Queue::kQuery, uint32_t(b.queries.size() - 1));
}

void NativeRenderer::OnQueryGetData(uint8_t* base, const HookArgs& args) {
  if (occlusion_) occlusion_->WriteResult(base, args.r[0], frame_index_);
}

void NativeRenderer::OnSwapSuspended() {
  if (SwitchRequested()) TakeBack();
}

// ---------------------------------------------------------------------------------------------
// Renderer switch (primary mode)

void NativeRenderer::HandOver(uint8_t* base, uint32_t device) {
  // Beside the Xenos plugin (debug window) it never stopped drawing; Fail cleared live_ already.
  if (!primary_) return;
  // The replaced draws (guest_d3d.h) cleared dirty bits without writing the state to the
  // stream, and the plugin skipped the shader loads: it has neither. The next draw writes all
  // of it.
  MarkAllDirty(base, device);
  rex::cvar::SetFlagByName("gpu_skip_draws", "false");
  if (occlusion_) rex::cvar::SetFlagByName("occlusion_query_external", "false");
  // The plugin presents on its own: the game waits for its vblanks again.
  paces_frames_.store(false, std::memory_order_relaxed);
  live_.store(nullptr, std::memory_order_relaxed);
  if (g_failed)
    REXLOG_INFO("PGR4 native: the Xenos plugin draws for the rest of the run");
  else
    REXLOG_INFO("PGR4 native: renderer switch: the Xenos plugin draws");
}

void NativeRenderer::TakeBack() {
  // The tracker saw none of the calls the plugin drew. The next native frame's ExecuteSwap
  // takes the game window back (OwnGameWindow), and the pacing with it.
  GuestState().Resync();
  rex::cvar::SetFlagByName("gpu_skip_draws", "true");
  if (occlusion_) rex::cvar::SetFlagByName("occlusion_query_external", "true");
  live_.store(this, std::memory_order_release);
  REXLOG_INFO("PGR4 native: renderer switch: native draws");
}

// ---------------------------------------------------------------------------------------------
// Recording (render thread, or the guest thread without it).

void NativeRenderer::ExecuteDraw(const DrawState& state, const DrawPrep* prep) {
  const uint64_t t0 = Ticks();
  profile::Begin();
  BeginFrame();
  ++stats_.draws;
  // Local change: the scene's passes (split screen, LimitFrameRate's neighbour below).
  if (Upscaler::IsScene(state.regs[rex::graphics::XE_GPU_REG_RB_SURFACE_INFO]))
    scene_drawn_since_resolve_ = true;
  if (gpu_timer_) {
    uint64_t key;
    if (gpu_timer_->per_shader() && prep && prep->vs) {
      const auto [vs, ps] = DrawRenderer::ShaderHashes(*prep);
      key = gpu_timer_->KeyDrawShaders(state.regs, vs, ps);
    } else {
      key = GpuTimer::KeyDraw(state.regs);
    }
    gpu_timer_->Pass(device_->recording_list(), key);
  }
  if (camera_scan_ && (upscaler_ || frame_generation_ || SceneEffects::Enabled()))
    FindCamera(state);
  if (REXCVAR_GET(pgr4_native_camera_diag)) DiagCamera(state);
  if (frame_generation_ &&
      frame_generation_->BeforeDraw(device_->recording_list(), frame_index_, state.regs))
    draw_->InvalidateListState();
  if (hudless_capture_ && streamline::FrameGenerationActive() &&
      hudless_capture_->BeforeDraw(device_->recording_list(), frame_index_, state.regs))
    draw_->InvalidateListState();
  draw_->OnDraw(state, prep);
  SubmitEarly();
  const uint64_t dt = Ticks() - t0;
  draw_ticks_ += dt;
  // Development: the cars' and characters' share (profile.h kDrawCar).
  if (profile::On()) [[unlikely]] {
    const int family = VsFamily(prep);
    if (family) {
      profile::g.ticks[family == 1 ? profile::kDrawCar : profile::kDrawSkin] += dt;
      profile::Count(family == 1 ? profile::kDrawsCar : profile::kDrawsSkin);
    }
    PassCount& p = passes_[state.regs[rex::graphics::XE_GPU_REG_RB_SURFACE_INFO]];
    ++p.draws;
    p.car += family == 1;
    p.ticks += dt;
    if (prep) {
      const auto [vs_hash, ps_hash] = DrawRenderer::ShaderHashes(*prep);
      ++p.vs[vs_hash];
      ++p.ps[ps_hash];
    }
  }
}

void NativeRenderer::LogPasses() {
  if (!profile::On() || ++passes_frames_ < 300) return;
  std::vector<std::pair<uint32_t, PassCount>> v(passes_.begin(), passes_.end());
  std::sort(v.begin(), v.end(),
            [](const auto& a, const auto& b) { return a.second.ticks > b.second.ticks; });
  std::string s;
  const double n = double(passes_frames_);
  for (size_t i = 0; i < v.size() && i < 10; ++i) {
    const uint32_t info = v[i].first;
    s += fmt::format(" | {:08X} pitch {} x{}: {:.0f} draws ({:.0f} car) {:.0f} us", info,
                     info & 0x3FFF, 1u << ((info >> 16) & 3), double(v[i].second.draws) / n,
                     double(v[i].second.car) / n, TicksToNs(v[i].second.ticks) / 1000.0 / n);
  }
  REXLOG_INFO("PGR4 native passes (a frame, by render thread time):{}", s);
  // The four heaviest passes' most drawn shaders, by name (shaders\shader_names.tsv).
  auto top = [&](const std::unordered_map<uint64_t, uint32_t>& m) {
    std::vector<std::pair<uint64_t, uint32_t>> w(m.begin(), m.end());
    std::sort(w.begin(), w.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::string out;
    for (size_t i = 0; i < w.size() && i < 8; ++i) {
      const auto it = g_shader_names.find(w[i].first);
      out += fmt::format(" {} {:.0f},",
                         it != g_shader_names.end() ? it->second : fmt::format("{:016X}", w[i].first),
                         double(w[i].second) / n);
    }
    return out;
  };
  for (size_t i = 0; i < v.size() && i < 4; ++i)
    REXLOG_INFO("PGR4 native pass {:08X}: vs{} | ps{}", v[i].first, top(v[i].second.vs),
                top(v[i].second.ps));
  passes_.clear();
  passes_frames_ = 0;
}

void NativeRenderer::DiagCamera(const DrawState& state) {
  using enum rex::graphics::Register;
  const uint32_t* regs = state.regs;
  if (frame_index_ % 120 != 0 || !Upscaler::IsScene(regs[XE_GPU_REG_RB_SURFACE_INFO])) return;
  const reg::RB_DEPTHCONTROL depth{.value = regs[XE_GPU_REG_RB_DEPTHCONTROL]};
  if (!depth.z_enable || !depth.z_write_enable) return;
  static uint64_t frame = UINT64_MAX;
  static std::vector<std::array<float, 16>> seen;
  static std::vector<uint32_t> counts;
  static uint32_t draws = 0;
  if (frame != frame_index_) {
    frame = frame_index_;
    seen.clear();
    counts.clear();
    draws = 0;
  }
  const uint32_t* c = regs + XE_GPU_REG_SHADER_CONSTANT_000_X + 4 * 4;  // c4
  std::array<float, 16> m;
  for (uint32_t i = 0; i < 16; ++i) m[i] = std::bit_cast<float>(c[i]);
  ++draws;
  size_t k = 0;
  for (; k < seen.size(); ++k) {
    float d = 0.0f;
    for (uint32_t i = 0; i < 16; ++i) d = std::max(d, std::abs(seen[k][i] - m[i]));
    if (d < 1e-4f) break;
  }
  if (k == seen.size() && seen.size() < 8) {
    seen.push_back(m);
    counts.push_back(0);
    REXLOG_INFO(
        "PGR4 camera diag: frame {} draw {} new c4-c7 #{}: x({:.4f} {:.4f} {:.4f} {:.3f}) "
        "y({:.4f} {:.4f} {:.4f} {:.3f}) w({:.4f} {:.4f} {:.4f} {:.3f})",
        frame_index_, draws, k, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[12], m[13],
        m[14], m[15]);
  }
  if (k < counts.size()) ++counts[k];
}

void NativeRenderer::FindCamera(const DrawState& state) {
  using enum rex::graphics::Register;
  const uint32_t* regs = state.regs;
  if (!Upscaler::IsScene(regs[XE_GPU_REG_RB_SURFACE_INFO])) return;
  const reg::RB_DEPTHCONTROL depth{.value = regs[XE_GPU_REG_RB_DEPTHCONTROL]};
  if (!depth.z_enable || !depth.z_write_enable) return;
  --camera_scan_;
  // A perspective clip transform: four aligned constant registers whose z row is a multiple of
  // their w row (fidelityfx.cpp). The scene's vertex shaders keep it in c4-c7 (td-323).
  const uint32_t* c = regs + XE_GPU_REG_SHADER_CONSTANT_000_X;
  float m[16];
  for (uint32_t v = 0; v + 4 <= 256; v += 4) {
    for (uint32_t i = 0; i < 16; ++i) m[i] = std::bit_cast<float>(c[4 * v + i]);
    if (!CameraFromClipRows(m, camera_)) continue;
    camera_scan_ = 0;
    // Local change: the whole transform for DLSS's camera matrices (upscaler.h).
    if (Upscaler* u = upscaler()) u->SetViewProjection(m, frame_index_);
    if (!camera_logged_) {
      camera_logged_ = true;
      REXLOG_INFO(
          "PGR4 native: the scene's camera, from c{} of its vertex shaders: near {}, "
          "far {}, fov {:.1f} deg",
          v, camera_.near_plane, camera_.far_plane, camera_.fov_y * 57.2958f);
    }
    return;
  }
}

void NativeRenderer::ExecuteClear(const ClearCall& c) {
  const uint64_t t0 = Ticks();
  BeginFrame();
  ++stats_.clears;
  if (gpu_timer_) gpu_timer_->Pass(device_->command_list(), GpuTimer::KeyClear(c));
  draw_->LogClear(c);
  rt_->Clear(device_->command_list(), c.regs, c.bound_colors, c.depth_bound, c.height, c.p);
  clear_ticks_ += Ticks() - t0;
}

void NativeRenderer::ExecuteResolve(const ResolveParams& p) {
  const uint64_t t0 = Ticks();
  BeginFrame();
  ++stats_.resolves;
  // Local change: a resolve of the scene's whole depth after scene draws starts a pass (its color
  // tiles follow without draws between them; the mirror resolves a corner). Only depth: the
  // garage first draws and resolves the scene's color alone, then the car's view with its depth
  // into the same image, one view (draw logs of 30/10/2026); split screen resolves a depth per
  // player's view.
  if (Upscaler::IsScene(p.regs.surface_info) && Upscaler::IsSceneWidth(p.x0, p.x1) &&
      p.source == 4 && std::exchange(scene_drawn_since_resolve_, false))
    ++scene_passes_;
  if (Upscaler::IsScene(p.regs.surface_info) && scene_resolves_.size() < 2000)
    scene_resolves_ += fmt::format(" [pass {} src {} {},{}-{},{} -> {:08X} {}x{} fmt {}]",
                                   scene_passes_, p.source, p.x0, p.y0, p.x1, p.y1, p.dest_base,
                                   p.dest_width, p.dest_height, p.dest_format);
  if (gpu_timer_) gpu_timer_->Pass(device_->command_list(), GpuTimer::KeyResolve(p));
  draw_->LogResolve(p);
  if (frame_generation_) frame_generation_->OnResolve(p);
  if (hudless_capture_) hudless_capture_->OnResolve(p);
  // Sets its own compute root signature, pipeline and descriptor heap on the list.
  resolver_->Resolve(device_->command_list(), p);
  draw_->InvalidateListState();
  resolve_ticks_ += Ticks() - t0;
}

void NativeRenderer::ExecuteQuery(const QueryCall& q) {
  if (q.end)
    occlusion_->End(q.query, frame_index_, device_->frame_fence());
  else
    occlusion_->Begin(q.query);
}

// With one submission per frame the GPU idles from the end of a frame until the render thread
// has recorded all of the next one. Every --pgr4_native_submit_draws draws, the frame so far is
// submitted and recording goes on in a fresh list, where nothing bound carries over. The texture
// copies recorded so far read upload memory the untile workers fill: while untiles are still
// running, the next draw or resolve tries again.
void NativeRenderer::SubmitEarly() {
  const int32_t every = REXCVAR_GET(pgr4_native_submit_draws);
  if (every <= 0 || stats_.draws - draws_submitted_ < uint64_t(every) || !textures_->UploadsDone())
    return;
  profile::Scope scope(profile::kSubmit);
  if (!CloseUploadLists() || !device_->Submit(upload_lists_)) return Fail();
  draw_->InvalidateListState(DrawRenderer::ListBreak::kSubmit);
  draws_submitted_ = stats_.draws;
  ++stats_.submits;
}

bool NativeRenderer::CloseUploadLists() {
  profile::Scope scope(profile::kUploadList);
  upload_lists_.clear();
  return textures_->CloseUploadLists(&upload_lists_);
}

void NativeRenderer::Fail() {
  g_failed = true;
  paces_frames_.store(false, std::memory_order_relaxed);
  if (pacer_) pacer_->Stop();
  REXLOG_ERROR("PGR4 native: frame {} failed; native path off for the rest of the run",
               frame_index_);
  if (!primary_) {
    live_.store(nullptr, std::memory_order_relaxed);
    return;
  }
  // The Xenos plugin skips its draws until the guest thread's next Swap hands the game back
  // (HandOver: it needs the guest device, between two of the guest's D3D calls); live_ stays
  // set so that the hooks reach that Swap.
  ReleaseGameWindow();
}

void NativeRenderer::ReleaseGameWindow() {
  D3D12Device& d = *device_;
  paces_frames_.store(false, std::memory_order_relaxed);
  if (d.has_swapchain()) {
    d.DetachSwapchain();
    if (pacer_) pacer_->Detach();
  }
  if (window_taken_) {
    ConnectSdkPresenter(true);
    window_taken_ = false;
    REXLOG_INFO("PGR4 native: the game window shows ReXGlue's presenter");
  }
}

bool NativeRenderer::OwnGameWindow(bool own) {
  D3D12Device& d = *device_;
  if (own == d.has_swapchain()) return true;
  if (!own) {
    ReleaseGameWindow();
    return true;
  }
  ConnectSdkPresenter(false);
  window_taken_ = true;
  if (!d.AttachSwapchain()) return false;
  if (pacer_) {
    HANDLE waitable =
        d.TakeFrameLatencyWaitable(FramePacer::kMaxFrameLatency * (frame_generation_ ? 2 : 1));
    if (!waitable) return false;
    // Local change: through Streamline's swapchain the waitable is not one signal a present.
    pacer_->set_shown_on_fence(streamline::Active());
    pacer_->Attach(waitable, d.fence());
    paces_frames_.store(pacer_->pacing(), std::memory_order_relaxed);
  }
  return true;
}

// Local change (pgr4 project): frames without the 3D scene (videos, loading and title screens)
// at most 60 a second with the frame rate unlocked: the intro videos, drawn at 160 FPS from
// frames decoded at 30, flashed black. The frame before's draw count tells (the render thread's
// ExecuteSwap sets it): the scene draws thousands, those screens a few hundred at most.
// Local change (pgr4 project): the upscaler upscales one scene a frame (Upscaler::Upscale, at
// its first resolve), so in split screen, whose frames draw and resolve the scene surface twice,
// both halves showed player 1's view. Off while a frame has two scene passes, on again after 120
// frames of one (bind_upscaler's switch, taken at the next frame's start).
void NativeRenderer::SplitScreenUpscaler() {
  const bool split = scene_passes_ >= 2;
  if (split && split_logs_ < 3) {
    ++split_logs_;
    REXLOG_INFO("PGR4 native: {} scene passes, resolves:{}", scene_passes_, scene_resolves_);
  }
  scene_resolves_.clear();
  scene_passes_ = 0;
  scene_drawn_since_resolve_ = false;
  if (split) {
    single_pass_frames_ = 0;
    if (upscaler_on_ && !upscaler_split_off_) {
      upscaler_split_off_ = true;
      upscaler_switch_requested_.store(true, std::memory_order_relaxed);
      REXLOG_INFO("PGR4 native: split screen, the upscaler off until it ends");
    }
  } else if (upscaler_split_off_ && ++single_pass_frames_ >= 120) {
    upscaler_split_off_ = false;
    if (!upscaler_on_) upscaler_switch_requested_.store(true, std::memory_order_relaxed);
    REXLOG_INFO("PGR4 native: split screen over, the upscaler on again");
  }
}

namespace {
// The refresh rate of the monitor showing `window` (its current mode), looked up once a second.
double DisplayHz(HWND window) {
  static double hz = 60;
  static uint64_t checked_ns = 0;
  const uint64_t now = NowNs();
  if (checked_ns && now - checked_ns < 1'000'000'000) return hz;
  checked_ns = now;
  MONITORINFOEXW info = {};
  info.cbSize = sizeof(info);
  DEVMODEW mode = {};
  mode.dmSize = sizeof(mode);
  if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY), &info) &&
      EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) &&
      mode.dmDisplayFrequency >= 24) {
    hz = mode.dmDisplayFrequency;
  }
  return hz;
}
}  // namespace

uint64_t NativeRenderer::LimitFrameRate() {
  if (!pacer_ || pacer_->frame_latency() == 0) return 0;
  const bool scene = last_frame_scene_.load(std::memory_order_relaxed);
  uint64_t interval = scene ? 0 : 16'666'667;
  // The frame rate: N refreshes per frame. Presented with that sync interval (exact on any
  // screen); with frame generation, whose frames are presented each refresh, the game is paced
  // to one frame per (G+1)N refreshes instead, G the frames generated per drawn frame (DLSS-G's
  // Multi Frame Generation; 1 for FSR's).
  const uint32_t refreshes = uint32_t(std::clamp(REXCVAR_GET(pgr4_native_present_interval), 1, 4));
  // Not while the game plays a video: it has no frame generation then (upscaler.cpp, streamline.cpp).
  // Local change (18/11/2026, Tazz: each upscaler on its own): by the upscaler running, not by the
  // settings (with DLSS asked but unavailable FSR runs): DLSS-G with DLSS only, FSR's frame
  // generation with FSR only.
  const bool by_running = REXCVAR_GET(pgr4_native_pace_by_upscaler);
  const bool dlss_running = by_running ? upscaler_on_ && upscaler_ && upscaler_->dlss()
                                       : upscaler_on_ && REXCVAR_GET(pgr4_native_dlss);
  const bool fsr_running = by_running
                               ? upscaler_on_ && upscaler_ && !upscaler_->dlss()
                               : upscaler_on_ && !REXCVAR_GET(pgr4_native_dlss);
  const bool generating =
      rex::system::XFile::OpenMovies() == 0 &&
      (dlss_running ? REXCVAR_GET(pgr4_native_dlss_fg) && streamline::FrameGenerationActive()
                    : fsr_running && REXCVAR_GET(pgr4_native_frame_generation) &&
                          CanFsrFrameGeneration());
  device_->set_present_interval(generating ? 1 : refreshes);
  // Local change (pgr4 project): the display page's VSYNC row, read at each frame.
  device_->set_vsync(FlagOn("d3d12_present_vsync"));
  const double shown =
      !generating ? 1.0
                  : 1.0 + (dlss_running ? streamline::GeneratedFrames() : 1u);
  // At the screen's own rate (one refresh), DLSS-G keeps to the refresh rate by itself; FSR's
  // frame generation went past it (its presents are not held to the vsync), so its game frames
  // are paced too.
  if (generating && (refreshes > 1 || !dlss_running)) {
    interval =
        std::max(interval, uint64_t(shown * 1e9 * refreshes / DisplayHz(device_->window())));
  }
  // A fixed limit (PC DISPLAY's, frames per second): the frames shown, the game's times the
  // frames shown for each with frame generation.
  // With vsync and FSR's frame generation, a limit that does not divide the refresh gives less than itself:
  // its two presents per frame each wait for a refresh (120 at 165 Hz: 2 refreshes each, about 82 frames a
  // second). The display page's VSYNC row is there for it.
  const int32_t limit = REXCVAR_GET(pgr4_native_fps_limit);
  if (limit > 0) interval = std::max(interval, uint64_t(shown * 1e9 / limit));
  // Local change (pgr4 project): while the game plays a video (a Bink file open), its frames at
  // the videos' own rate: every Movies\*.bik is 29.97 (30000/1001) frames per second, and the game
  // steps the video once per frame (it ran at the 2D screens' 60).
  if (rex::system::XFile::OpenMovies() > 0)
    interval = std::max(interval, uint64_t(1e9 * 1001.0 / 30000.0));
  // FSR's generated frame and the drawn one, half the limit's interval apart (DLSS-G paces its
  // own); without a limit the game keeps to half the refresh rate and vsync spaces them.
  device_->set_generated_spacing(generating && limit > 0 && !dlss_running
                                     ? interval / 2
                                     : 0);
  // Development: a plain frame rate cap (frames per second, 0: none), for tests.
  if (const int32_t cap = REXCVAR_GET(pgr4_native_test_fps_cap); cap > 0)
    interval = std::max(interval, uint64_t(1e9 / cap));
  return pacer_->LimitBegin(interval);
}

void NativeRenderer::ExecuteSwap(const SwapCall& swap) {
  const uint64_t t0 = NowNs();
  // Local change: for the guest thread's ApplyMotionBlurRule.
  dlss_running_.store(upscaler_on_ && upscaler_ && upscaler_->dlss(), std::memory_order_relaxed);
  TunablesApply();  // local change (development): the game's settings forced (tunables.h)
  // Local change (pgr4 project): a frame with a 3D scene (a pass of the scene's surface, counted
  // until SplitScreenUpscaler below), or a race's screen (the game's InGame\ scripts), is not
  // held to the 2D screens' 60 FPS (LimitFrameRate). By draw count alone (over 1000), close views
  // of a car (about 900 draws: the garage, some race views) were, and fell from 158 to 60.
  last_frame_scene_.store(stats_.draws > 1000 || scene_passes_ > 0 || Pgr4InGameScreen(),
                          std::memory_order_relaxed);
  // The HD textures' upload per frame is larger on screens without a 3D scene (textures.h).
  TextureCache::SetNoScene(stats_.draws <= 1000 && scene_passes_ == 0);
  SplitScreenUpscaler();
  BeginFrame();
  D3D12Device& d = *device_;
  if (gpu_timer_) gpu_timer_->Pass(d.command_list(), GpuTimer::KeyPresent());
  // A scene's first frame without the draws whose pipelines were compiling: the previous frame
  // stays on screen while the next one waits for them.
  const bool incomplete = draw_->FrameIncomplete();
  if (incomplete) ++stats_.frames_held;
  if (primary_ && !OwnGameWindow(!swap.hand_over && !rex::kernel::xam::xeXamIsUIActive()))
    return Fail();
  const bool shown = !incomplete && d.has_swapchain();
  FrameGeneration* const fg = frame_generation_.get();
  const bool generate = shown && fg && fg->prepared(frame_index_);
  // FindCamera's budget in the next frame's draws (also for the scene's effects: local change).
  camera_scan_ = (upscaler_ || fg || SceneEffects::Enabled()) ? 8 : 0;
  SwapCopy hudless;
  if (generate) {
    d.SetFrameImage(fg->image(), fg->image_rtv());
    if (const ResolvedImage* source = fg->hudless_source(frame_index_))
      hudless = {source, fg->hudless(), fg->hudless_rtv()};
  } else if (shown && hudless_capture_ && streamline::FrameGenerationActive()) {
    if (const ResolvedImage* source = hudless_capture_->source(frame_index_);
        source && hudless_capture_->Ensure())
      hudless = {source, hudless_capture_->image(), hudless_capture_->rtv()};
  }
  // The resolved front buffer into the back buffer (PRESENT in and out).
  present_->OnSwap(swap, shown, hudless.image ? &hudless : nullptr);
  // Local change: DLSS-G maps the scene's depth and motion onto the image's part of the back
  // buffer (streamline.h).
  if (shown && streamline::FrameGenerationActive()) {
    const ImageRect& r = present_->image_rect();
    streamline::TagBackbuffer(r.left, r.top, r.width, r.height,
                              hudless.drawn && !generate ? hudless.target : nullptr);
  }
  // Local diagnostic (frame_dump.h): the back buffers after a motion dump.
  if (shown && frame_dump::armed_swaps > 0) {
    const int k = 3 - frame_dump::armed_swaps--;
    frame_dump::Capture(d.device(), d.command_list(), d.back_buffer(), D3D12_RESOURCE_STATE_PRESENT,
                        L"motion_dump_" + std::to_wstring(frame_dump::armed_id) + L"_bb" +
                            std::to_wstring(k));
  }
  // Local diagnostic: <capture dir>\screenshot.txt asks for the next shown back buffer, written
  // beside the executable as screenshot_<frame>.bin (frame_dump.h); the file is removed.
  if (shown && !REXCVAR_GET(pgr4_native_capture_dir).empty() && ++screenshot_poll_ >= 15) {
    screenshot_poll_ = 0;
    std::error_code ec;
    const std::filesystem::path ask =
        std::filesystem::path(REXCVAR_GET(pgr4_native_capture_dir)) / "screenshot.txt";
    if (std::filesystem::remove(ask, ec))
      frame_dump::Capture(d.device(), d.command_list(), d.back_buffer(),
                          D3D12_RESOURCE_STATE_PRESENT,
                          L"screenshot_" + std::to_wstring(frame_index_));
    // Local diagnostic: <capture dir>\cvars.txt, lines "name value", sets those cvars live
    // (rex::cvar::SetFlagByName) and is removed, so that renders can be compared from outside.
    const std::filesystem::path cvars =
        std::filesystem::path(REXCVAR_GET(pgr4_native_capture_dir)) / "cvars.txt";
    if (std::filesystem::exists(cvars, ec)) {
      std::ifstream in(cvars);
      std::string name, value;
      while (in >> name >> value)
        REXLOG_INFO("PGR4 native: cvar {} = {}{}", name, value,
                    rex::cvar::SetFlagByName(name, value) ? "" : " (failed)");
      in.close();
      std::filesystem::remove(cvars, ec);
    }
  }
  frame_dump::Poll();
  // Local change: the settings overlay over the frame (overlay.h).
  if (overlay_ && shown) {
    Overlay::Info info;
    info.upscaler_on = upscaler_on_ && upscaler_;
    info.dlss = info.upscaler_on && upscaler_->dlss();
    info.dlss_available = streamline::Active();
    info.fg_available = streamline::FrameGenerationActive();
    info.render_width = upscaler_ ? upscaler_->render_width() : 0;
    info.render_height = upscaler_ ? upscaler_->render_height() : 0;
    info.output_width = Upscaler::OutputWidth();
    info.output_height = Upscaler::OutputHeight();
    info.backbuffer_width = d.width();
    info.backbuffer_height = d.height();
    info.resolution_scale = DrawResolutionScale();
    ID3D12Resource* const target = d.back_buffer();
    const Overlay::Actions a =
        overlay_->Draw(d.command_list(), target, d.back_buffer_rtv(), target->GetDesc().Format, info);
    if (a.switch_upscaler) upscaler_switch_requested_.store(true, std::memory_order_relaxed);
    if (a.recreate_upscaler) upscaler_recreate_requested_.store(true, std::memory_order_relaxed);
    if (a.rebuild_pipeline) rebuild_requested_.store(true, std::memory_order_relaxed);
    if (a.apply_msaa) msaa_requested_.store(true, std::memory_order_relaxed);
    if (overlay_->drew()) draw_->InvalidateListState();
  }
  if (fg) fg->OnSwap(present_->front_buffer_base());
  if (hudless_capture_) hudless_capture_->OnSwap(present_->front_buffer_base());
  if (generate) {
    const ImageRect& r = present_->image_rect();
    d.SetGeneratedFrame(fg->Generate(d.command_list(), frame_index_,
                                     {r.left, r.top, r.width, r.height}, hudless.image));
  }
  // The frame's texture copies read upload memory the untile workers fill.
  const uint64_t u0 = NowNs();
  textures_->FinishUploads();
  stats_.swap_untile_ns = NowNs() - u0;
  draw_->BeforeSubmit();
  const uint64_t fence = d.frame_fence();
  if (occlusion_) occlusion_->Resolve(d.command_list(), fence);
  if (!CloseUploadLists() || !d.EndFrame(shown, upload_lists_)) return Fail();
  const D3D12Device::EndFrameTimes& et = d.end_frame_times();
  stats_.swap_submit_ns = et.submit_ns;
  stats_.swap_present_ns = et.present_ns;
  stats_.swap_fence_ns = et.fence_ns;
  stats_.present = et.presented ? et.presents : 0;
  if (pacer_ && et.presented)
    pacer_->Presented(fence, et.frame_presents);
  else if (pacer_)
    pacer_->RetireUnshown();
  ++frame_index_;
  frame_begun_ = false;
  d.SetTitleFrame(frame_index_);
  const uint64_t completed = d.completed_fence();
  if (queue_) {
    // Frees the guest thread's vertex upload memory of finished frames (Prepare's tags).
    Queue& q = *queue_;
    q.frame_fences.emplace_back(frame_index_, fence);
    while (!q.frame_fences.empty() && q.frame_fences.front().second <= completed) {
      q.gpu_frames_done.store(q.frame_fences.front().first, std::memory_order_release);
      q.frame_fences.pop_front();
    }
  }
  textures_->Retire(completed);
  if (occlusion_) occlusion_->Retire(completed);
  draw_->EndFrame(completed);
  resolver_->AfterSwap();
  present_->AfterEndFrame();
  const uint64_t t1 = NowNs();
  stats_.swap_ns = t1 - t0;
  stats_.draw_ns = TicksToNs(draw_ticks_);
  stats_.clear_ns = TicksToNs(clear_ticks_);
  stats_.resolve_ns = TicksToNs(resolve_ticks_);
  draw_ticks_ = clear_ticks_ = resolve_ticks_ = 0;
  if (!queue_)
    stats_.hook_ns = stats_.draw_ns + stats_.clear_ns + stats_.resolve_ns + stats_.swap_ns;
  {
    // Local change: the command recorder's frame (command_recorder.h).
    const CommandRecorder::Stats rec = d.TakeRecorderStats();
    stats_.rec_on = d.recording() ? 1 : 0;
    stats_.rec_wait_ns = rec.wait_ns;
    stats_.rec_drains = rec.drains;
    stats_.rec_replay_ns = rec.replay_ns;
    stats_.rec_bytes = rec.bytes;
    stats_.rec_forwarded = rec.forwarded;
  }
  WriteCsvRow(stats_, t1 - frame_start_ns_);
  LogPasses();  // local change, development
  AddToStatsLog(stats_, t1 - frame_start_ns_);
  ResetFrameCounters();
  frame_start_ns_ = t1;
  stats_ = FrameStats{};
  draws_submitted_ = 0;
}

void NativeRenderer::AppendCsvColumns(const FrameStats& s, uint64_t frame_ns, bool header,
                                      std::string* line) {
  const DrawRenderer::Stats& ds = draw_->stats();
  const TextureCache::Stats& ts = textures_->stats();
  const CsvCell cells[] = {
      Uint("frame", frame_index_ - 1),
      Uint("draws", s.draws),
      Uint("clears", s.clears),
      Uint("resolves", s.resolves),
      Uint("state_fail", s.state_fail),
      Fix1("draw_us", s.draw_ns / 1000.0),
      Fix1("clear_us", s.clear_ns / 1000.0),
      Fix1("resolve_us", s.resolve_ns / 1000.0),
      Fix1("swap_us", s.swap_ns / 1000.0),
      Fix4("us_per_draw", s.draws ? double(s.draw_ns) / 1000.0 / double(s.draws) : 0.0),
      Fix3("frame_ms", frame_ns / 1e6),
      Uint("host_draws", ds.draws),
      Uint("skipped", ds.skipped),
      Uint("unsupported", ds.unsupported),
      Uint("failed", ds.failed),
      Uint("pso_created", ds.pso_created),
      Fix1("upload_kb", ds.upload_bytes / 1024.0),
      Uint("textures", ds.textures),
      Uint("textures_missing", ds.textures_missing),
      Uint("front_buffer", present_->front_buffer_found() ? 1 : 0),
      Fix1("hook_us", s.hook_ns / 1000.0),
      Fix1("hook_wait_us", s.hook_wait_ns / 1000.0),
      Uint("reg_writes", s.reg_writes),
      Fix1("swap_untile_us", s.swap_untile_ns / 1000.0),
      Fix1("swap_submit_us", s.swap_submit_ns / 1000.0),
      Fix1("swap_present_us", s.swap_present_ns / 1000.0),
      Fix1("swap_fence_us", s.swap_fence_ns / 1000.0),
      Fix1("hook_max_us", s.hook_max_ns / 1000.0),
      Uint("watch_faults", ds.watch_faults),
      Fix3("gpu_prev_ms", gpu_timer_ ? gpu_timer_->last_frame_ms() : 0.0),
      Uint("copy_batches", ds.copy_batches),
      Uint("watch_arms", ds.watch_arms),
      Uint("watch_arm_pages", ds.watch_arm_pages),
      Uint("watch_arm_changed", ds.watch_arm_changed),
      Fix1("watch_arm_us", ds.watch_arm_ns / 1000.0),
      Fix3("t_ms", NowNs() / 1e6),
      Uint("hash_pages", ds.hash_pages),
      Uint("hash_copies", ds.hash_copies),
      Uint("pso_pending", ds.pso_pending),
      Uint("pso_wait_us", ds.pso_wait_us),
      Uint("held", s.frames_held),
      Uint("tex_stale", ds.textures_stale),
      Uint("coh_stale", ds.stale_pages),
      Uint("coh_late", ds.late_pages),
      Uint("coh_gpu", ds.gpu_mismatch_pages),
      Uint("coh_regs", ds.reg_mismatches),
      Fix1("tex_mb", textures_->resident_bytes() / 1048576.0),
      Uint("tex_count", textures_->texture_count()),
      Uint("tex_evicted", ts.evicted - csv_->evicted_prev),
      Fix1("tex_evict_us", (ts.evict_ns - csv_->evict_ns_prev) / 1000.0),
      Uint("views", ds.views),
      Uint("pso_generic", ds.pso_generic),
      Uint("occ_unknown", occlusion_ ? occlusion_->unknown_pairs() - csv_->occ_unknown_prev : 0),
      // rex_xam_take_frame_pad's layout.
      Hex4("pad_buttons", s.pad & 0xFFFF),
      Uint("pad_lt", uint8_t(s.pad >> 16)),
      Uint("pad_rt", uint8_t(s.pad >> 24)),
      Int("pad_lx", int8_t(s.pad >> 32)),
      Int("pad_ly", int8_t(s.pad >> 40)),
      Int("pad_rx", int8_t(s.pad >> 48)),
      Int("pad_ry", int8_t(s.pad >> 56)),
      Uint("submits", s.submits),
      Fix3("guest_begin_ms", s.guest_begin_ns / 1e6),
      Fix3("guest_swap_ms", s.guest_swap_ns / 1e6),
      Fix1("pace_wait_us", s.pace_wait_ns / 1000.0),
      Uint("present", s.present),
      Fix3("gpu_busy_ms", device_->gpu_busy_ns() / 1e6),
      Uint("render_height", upscaler() ? upscaler()->render_height() : 0),
      // Local change (18/11/2026): the command recorder.
      Uint("rec_on", s.rec_on),
      Fix1("rec_wait_us", s.rec_wait_ns / 1000.0),
      Uint("rec_drains", s.rec_drains),
      Fix1("rec_replay_us", s.rec_replay_ns / 1000.0),
      Fix1("rec_kb", s.rec_bytes / 1024.0),
      Uint("rec_forwarded", s.rec_forwarded),
  };
  AppendCsvCells(cells, header, false, line);
}

void NativeRenderer::AppendCsvProfileColumns(bool header, std::string* line) {
  for (uint32_t i = 0; i < profile::kStageCount; ++i) {
    if (header)
      *line += std::string(",st_") + profile::kStageNames[i] + "_us";
    else
      AppendCsvCell(Fix1("", double(TicksToNs(profile::g.ticks[i])) / 1000.0), false, true, line);
  }
  const DrawRenderer::Stats& ds = draw_->stats();
  const TextureCache::Stats& t = textures_->stats();
  const TextureCache::Stats& p = csv_->tex_prev;
  const CsvCell cells[] = {
      Uint("track_calls", profile::g.track_calls),
      Uint("tex_memo_miss", t.memo_misses - p.memo_misses),
      Uint("tex_created", t.created - p.created),
      Fix1("tex_create_us", (t.create_ns - p.create_ns) / 1000.0),
      Uint("tex_uploads", t.uploads - p.uploads),
      Uint("tex_deferred", t.deferred_uploads - p.deferred_uploads),
      Fix1("tex_upload_kb", (t.upload_bytes - p.upload_bytes) / 1024.0),
      Fix1("tex_upload_us", (t.upload_ns - p.upload_ns) / 1000.0),
      Fix1("tex_untile_us", (t.untile_ns - p.untile_ns) / 1000.0),
      Fix1("tex_record_us", (t.upload_record_ns - p.upload_record_ns) / 1000.0),
      Uint("tex_resolved_copies", t.resolved_copies - p.resolved_copies),
      Fix1("tex_copy_resolved_us", (t.copy_resolved_ns - p.copy_resolved_ns) / 1000.0),
      Uint("tex_resolved_aliases", t.resolved_aliases - p.resolved_aliases),
      Uint("srv_lookups", ds.srv_lookups),
      Uint("srv_created", ds.srv_created),
      Uint("samplers_created", ds.samplers_created),
      Uint("bind_hits", ds.bind_memo_hits),
      Uint("bind_misses", ds.bind_memo_misses),
  };
  AppendCsvCells(cells, header, true, line);
  for (uint32_t i = 0; i < profile::kCounterCount; ++i)
    AppendCsvCell(Uint(profile::kCounterNames[i], profile::g.counters[i]), header, true, line);
}

void NativeRenderer::AppendCsvLumaColumns(bool header, std::string* line) {
  const LumaProbe& l = present_->luma_probe();
  AppendCsvCell(Uint("luma_frame", l.frame), header, true, line);
  static constexpr const char* kRowNames[LumaProbe::kRows] = {"top", "mid", "low"};
  for (uint32_t row = 0; row < LumaProbe::kRows; ++row) {
    for (uint32_t s = 0; s < LumaProbe::kSegments; ++s) {
      if (header)
        *line += ",luma_" + std::string(kRowNames[row]) + "_" + std::to_string(s);
      else
        AppendCsvCell(Fix1("", l.luma[row][s]), false, true, line);
    }
  }
}

void NativeRenderer::WriteCsvRow(const FrameStats& s, uint64_t frame_ns) {
  Csv& c = *csv_;
  if (!c.file) return;
  c.line.clear();
  AppendCsvColumns(s, frame_ns, false, &c.line);
  c.evicted_prev = textures_->stats().evicted;
  c.evict_ns_prev = textures_->stats().evict_ns;
  c.occ_unknown_prev = occlusion_ ? occlusion_->unknown_pairs() : 0;
  if (csv_profile_) {  // local change: the header's columns, profiler on or off
    AppendCsvProfileColumns(false, &c.line);
    c.tex_prev = textures_->stats();
  }
  if (c.luma) AppendCsvLumaColumns(false, &c.line);
  c.line += '\n';
  c.file->Append(c.line);
}

void NativeRenderer::AddToStatsLog(const FrameStats& s, uint64_t frame_ns) {
  Csv& w = *csv_;
  ++w.frames;
  w.draws += s.draws;
  w.draw_ns += s.draw_ns;
  w.swap_ns += s.swap_ns;
  w.frame_ns += frame_ns;
  w.hook_ns += s.hook_ns;
  w.hook_wait_ns += s.hook_wait_ns;
  if (profile::On()) {
    for (uint32_t i = 0; i < profile::kStageCount; ++i) w.stage_ticks[i] += profile::g.ticks[i];
    w.track_calls += profile::g.track_calls;
  }
  const int32_t every = REXCVAR_GET(pgr4_d3d_stats_frames);
  if (every <= 0 || w.frames < uint64_t(every)) return;
  const double n = double(w.frames);
  REXLOG_INFO(
      "PGR4 native: frames {} | per frame: draws {:.1f}, draw CPU {:.1f} us "
      "({:.4f} us/draw), swap {:.1f} us, frame {:.2f} ms, game thread {:.1f} us "
      "(waiting {:.1f} us)",
      frame_index_, w.draws / n, w.draw_ns / 1000.0 / n,
      w.draws ? w.draw_ns / 1000.0 / double(w.draws) : 0.0, w.swap_ns / 1000.0 / n,
      w.frame_ns / 1e6 / n, w.hook_ns / 1000.0 / n, w.hook_wait_ns / 1000.0 / n);
  if (profile::On() && w.draws) {
    // ns per draw for each stage (the hook-side ones are per draw too, for comparison).
    std::string line;
    char item[64];
    for (uint32_t i = 0; i < profile::kStageCount; ++i) {
      std::snprintf(item, sizeof(item), " %s %.0f", profile::kStageNames[i],
                    double(TicksToNs(w.stage_ticks[i])) / double(w.draws));
      line += item;
    }
    REXLOG_INFO("PGR4 native profile: ns/draw{} | track calls/frame {:.0f}", line,
                double(w.track_calls) / n);
    for (uint64_t& t : w.stage_ticks) t = 0;
    w.track_calls = 0;
  }
  if (gpu_timer_) {
    gpu_timer_->Log();
    for (uint64_t hash : gpu_timer_->TakeNewTopShaders())
      REXLOG_INFO("PGR4 native GPU: shader {:016X} ucode:\n{}", hash,
                  draw_->ShaderDisassembly(hash));
  }
  w.frames = w.draws = w.draw_ns = w.swap_ns = w.frame_ns = w.hook_ns = w.hook_wait_ns = 0;
}

void NativeRenderer::ResetFrameCounters() {
  draw_->ResetStats();
  // Local change: the profiler switched live (csv_profile_), between two frames.
  if (csv_profile_)
    profile::enabled.store(REXCVAR_GET(pgr4_native_profile), std::memory_order_relaxed);
  if (csv_profile_) {
    for (uint64_t& t : profile::g.ticks) t = 0;
    profile::g.track_calls = 0;
    for (uint64_t& c : profile::g.counters) c = 0;
  }
}
}  // namespace pgr4::native
