// pgr4-native PGR4 native renderer: host of the native D3D12 path.
//
// The guest D3D hooks in pgr4_d3d_trace.cpp forward every call to the guest D3D (without
// --pgr4_native_primary the Xenos plugin keeps rendering, as the oracle) and, with
// --pgr4_native, then call the NativeRenderer entry points below with the guest device and the
// call's arguments. Only the outermost
// draw/clear/resolve/Swap call is passed on (the D3D's internal clear, copy and Swap quads are
// not game draws); a top-level BeginVertices reaches OnDraw at the entry of the next hooked
// call, once the game has written its vertices. The renderer owns its own D3D12 queue, window
// and swapchain, separate from ReXGlue's presenter (d3d12_device.h).
//
// Threading: the guest D3D device is used by one guest thread at a time; all On* entry points
// are called on that thread, after the original guest function returned, and are serialized
// by the renderer. Modules may assume single-threaded access from the On* calls.
//
// With --pgr4_native_render_thread (default on; false keeps everything on the guest thread as
// above) the On* calls only capture, on the guest thread, what each call reads from guest
// memory: the register changes since the last draw (which the tracker records straight into the
// batch, GuestStateTracker::RecordInto), the DrawState, DrawRenderer::Prepare's shader lookup and
// vertex/index copies, and the decoded Clear/Resolve/Swap arguments. They queue it in batches to
// the render thread
// ("PGR4 native render"), which replays it into the modules with its own copy of the register
// file. From then on the command list, the D3D12 objects and every module's state belong to
// that thread (DrawRenderer's vertex upload pool and shader lookup stay on the guest thread).
// The guest thread blocks at Swap while the render thread is more than one frame behind, and
// waits for the render thread to finish the Swaps around a capture (present.h).
#pragma once

#include "hook_args.h"
#include "fidelityfx.h"

#include <rex/cvar.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

struct ID3D12Device;
struct ID3D12CommandList;
struct ID3D12CommandQueue;
struct ID3D12GraphicsCommandList;

REXCVAR_DECLARE(bool, pgr4_native_render_thread);  // record on a render thread (see above)

namespace pgr4::native {

class D3D12Device;
class DrawRenderer;
class FrameGeneration;
class FgProbe;  // development (28/11/2026)
class HudlessCapture;
class LightMaskPass;  // local change (28/11/2026)
class Overlay;
class FramePacer;
class GpuTimer;
class OcclusionQueries;
class Presenter;
class RenderTargets;
struct RenderTarget;  // local change (28/11/2026)
class Resolver;
class TextureCache;
class Upscaler;
class SceneEffects;
class Volumetrics;
struct ClearCall;
struct DrawPrep;
struct DrawState;
struct ResolveParams;
struct SwapCall;

// Per-frame counters of the native path (also the frame CSV's columns).
struct FrameStats {
  uint64_t draws = 0, clears = 0, resolves = 0;
  uint64_t state_fail = 0;  // draws ReadDrawState rejected
  // CPU time inside ExecuteDraw, ExecuteClear and ExecuteResolve, summed in ticks and converted
  // at the Swap: the render thread's time (the guest thread's without it).
  uint64_t draw_ns = 0, clear_ns = 0, resolve_ns = 0;
  uint64_t swap_ns = 0;  // ExecuteSwap: record, submit, present
  // Guest thread: time in the On* calls (render thread mode: capturing and queueing) and the
  // part of it spent waiting for the render thread.
  uint64_t hook_ns = 0, hook_wait_ns = 0;
  uint64_t hook_max_ns = 0;  // render thread mode: the longest single On* call, without waits
  uint64_t reg_writes = 0;   // render thread mode: register values replayed
  // Parts of swap_ns: waiting for the texture untile workers, then D3D12Device::EndFrameTimes.
  uint64_t swap_untile_ns = 0, swap_submit_ns = 0, swap_present_ns = 0, swap_fence_ns = 0;
  uint64_t frames_held = 0;  // Swaps not presented (DrawRenderer::FrameIncomplete)
  uint64_t submits = 0;      // lists submitted before the Swap's (--pgr4_native_submit_draws)
  // The guest's pad reads since the Swap before (rex_xam_take_frame_pad, taken at the Swap).
  uint64_t pad = 0;
  // Pacing (NowNs clock): when the guest thread began the frame (its previous Swap returned) and
  // reached its Swap, how long FramePacer::WaitToBegin held it before it began, and the frame's
  // present number (D3D12Device::EndFrameTimes::presents; 0: not presented).
  uint64_t guest_begin_ns = 0, guest_swap_ns = 0, pace_wait_ns = 0, present = 0;
  // Local change (18/11/2026): the command recorder (D3D12Device::TakeRecorderStats), 1 when on.
  uint64_t rec_on = 0, rec_wait_ns = 0, rec_drains = 0, rec_replay_ns = 0, rec_bytes = 0,
           rec_forwarded = 0;
};

class NativeRenderer {
 public:
  // The renderer, created on first use when --pgr4_native is on; nullptr when it is off, when
  // D3D12 initialization failed (logged once) and from a failed frame on (Fail).
  static NativeRenderer* Get();
  // The renderer while it draws the game: Get(), except after the renderer switch handed the
  // game to the Xenos plugin (primary mode, bind_renderer_switch) or a frame failed (in primary
  // mode from the guest thread's next Swap, which hands the game to the plugin as the switch
  // does). The hooks use this one, a single load while the renderer draws; while it is null the
  // guest D3D runs unchanged and nothing is tracked.
  static NativeRenderer* Live() {
    if (NativeRenderer* r = live_.load(std::memory_order_acquire)) [[likely]]
      return r;
    Get();  // creates the renderer on first use
    return live_.load(std::memory_order_acquire);
  }

  // True while the renderer presents the game's frames and paces them on the display
  // (frame_pacer.h): the guest D3D then presents without waiting for a vblank
  // (Pgr4PresentationInterval in pgr4_d3d_trace.cpp). Any thread.
  static bool PacesFrames() { return paces_frames_.load(std::memory_order_relaxed); }

  // Hook entry points. `base` is the guest memory base the hook received (guest virtual
  // address a is at base + a, plus 0x1000 for a >= 0xE0000000 like the generated code).
  void OnDraw(const uint8_t* base, const HookArgs& args);
  void OnClear(const uint8_t* base, const HookArgs& args);
  void OnResolve(const uint8_t* base, const HookArgs& args);
  void OnSwap(uint8_t* base, const HookArgs& args);
  // Occlusion queries (occlusion.h), primary mode: after QueryIssue returned, and at the entry
  // of GetData.
  void OnQueryIssue(const uint8_t* base, const HookArgs& args);
  // Local change: the game's transparent world starts (1) or ends (2) (pgr4_d3d_trace.cpp).
  void OnWorldMarker(uint32_t id);
  void OnQueryGetData(uint8_t* base, const HookArgs& args);
  // A Swap while the Xenos plugin draws (Live() is null): the renderer switch takes the game
  // back.
  void OnSwapSuspended();

  // For modules.
  D3D12Device& device() { return *device_; }
  ID3D12Device* d3d12() const;
  ID3D12CommandQueue* command_queue() const;
  // The frame's direct command list: open from the previous Swap until OnSwap closes and
  // submits it. Record into it from the On* calls only (the render thread, in that mode).
  ID3D12GraphicsCommandList* command_list() const;
  static void LoadShaderFamilies();  // local change, development (VsFamily)
  // Local change (18/11/2026): the draw path's list (D3D12Device::recording_list).
  ID3D12GraphicsCommandList* recording_list() const;
  // Development (18/11/2026, --pgr4_native_profile): 1 for a car's draw, 2 for an animated
  // character's, else 0 (renderer.cpp: shaders\shader_names.tsv).
  static int VsFamily(const DrawPrep* prep);
  // Swaps recorded so far (= index of the frame being recorded).
  uint64_t frame_index() const { return frame_index_; }
  // Fence value OnSwap will signal for the frame being recorded; a resource last used in this
  // frame may be freed once device().completed_fence() >= this.
  uint64_t frame_fence() const;
  // Modules owned by the renderer.
  TextureCache& textures() { return *textures_; }
  RenderTargets& render_targets() { return *rt_; }
  Resolver& resolver() { return *resolver_; }
  // The game's occlusion queries, counted by the draws; nullptr when they are not.
  OcclusionQueries* occlusion() { return occlusion_.get(); }
  // The scene's upscaler while it replaces the scene's MSAA (--pgr4_native_upscaler, switched
  // with bind_upscaler); nullptr otherwise.
  Upscaler* upscaler() { return upscaler_on_ ? upscaler_.get() : nullptr; }
  // Local change (21/11/2026): the volumetric fog (its volume shared with the draws after it).
  Volumetrics* volumetrics() { return volumetrics_.get(); }
  // Local change (pgr4 project): settings changed outside the renderer (the game's Graphics page,
  // pgr4_menu.cpp): `fn` sets the cvars and calls the Request* below, on the thread that records
  // the frames at the next frame's start (the cvars it reads there are not written under it).
  void QueueSettings(std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(settings_mutex_);
    queued_settings_.push_back(std::move(fn));
  }
  bool upscaler_on() const { return upscaler_on_; }
  void RequestUpscaler(bool on) {
    if (on != upscaler_on_) upscaler_switch_requested_.store(true, std::memory_order_relaxed);
  }
  void RequestUpscalerRecreate() {
    upscaler_recreate_requested_.store(true, std::memory_order_relaxed);
  }
  void RequestRebuild() { rebuild_requested_.store(true, std::memory_order_relaxed); }
  void RequestMsaa() { msaa_requested_.store(true, std::memory_order_relaxed); }
  // FidelityFX's frame generation follows --pgr4_native_frame_generation from the next frame;
  // possible with the frame pacer only.
  void RequestFsrFrameGeneration() { fsr_fg_requested_.store(true, std::memory_order_relaxed); }
  bool CanFsrFrameGeneration() const { return paced_; }

  ~NativeRenderer();

 private:
  NativeRenderer() = default;
  bool Initialize();
  // At the end of each frame's ExecuteSwap: its row of the frame CSV, its share of the stats
  // log (every --pgr4_d3d_stats_frames frames), then the per-frame counters start over.
  void WriteCsvRow(const FrameStats& s, uint64_t frame_ns);
  // The frame CSV's columns, their names (header) or this frame's values: those of every frame,
  // with --pgr4_native_profile the stage times and texture counters, with
  // --pgr4_native_luma_probe the probe's.
  void AppendCsvColumns(const FrameStats& s, uint64_t frame_ns, bool header, std::string* line);
  void AppendCsvProfileColumns(bool header, std::string* line);
  void AppendCsvLumaColumns(bool header, std::string* line);
  void AddToStatsLog(const FrameStats& s, uint64_t frame_ns);
  void ResetFrameCounters();
  // Starts a frame's module state (RenderTargets/Resolver BeginFrame) at its first On* call.
  void BeginFrame() {
    if (!frame_begun_) BeginFrameModules();
  }
  void BeginFrameModules();
  // The recording half of the On* calls (on the guest thread, or the render thread).
  void ExecuteDraw(const DrawState& state, const DrawPrep* prep);
  // The frame's camera (camera_): from the clip transform of its first depth-writing scene
  // draws, while camera_scan_ counts how many more of them to look at.
  void FindCamera(const DrawState& state);
  // Local diagnostic (--pgr4_native_camera_diag): whether c4-c7 of the scene's depth draws is one
  // matrix per frame (view-projection) or one per object.
  void DiagCamera(const DrawState& state);
  void ExecuteClear(const ClearCall& clear);
  void ExecuteResolve(const ResolveParams& p);
  // Local change (pgr4 project): resolves whose texels the game's CPU reads (the team logo and
  // liveries it composes on the GPU and compresses itself, --pgr4_native_resolve_readback):
  // after ExecuteResolve, the frame so far is submitted, the GPU waited for, and the texels
  // written into guest memory (Resolver::WriteReadBack). In render thread mode the guest thread
  // waits for it at the resolve.
  static bool WantsReadBack(const ResolveParams& p);
  void ReadBackResolve(const ResolveParams& p);
  void ExecuteSwap(const SwapCall& swap);
  struct QueryCall {
    uint32_t query;
    bool end;
  };
  void ExecuteQuery(const QueryCall& q);
  // After a draw: submits the frame so far once --pgr4_native_submit_draws draws were recorded
  // since the last submission.
  void SubmitEarly();
  // Before each submission, once the untiles finished: the texture upload lists to submit first
  // (TextureCache::CloseUploadLists), into upload_lists_. False on failure (logged).
  bool CloseUploadLists();
  // A failed submission or present: logs and turns the native path off for the rest of the run.
  // In primary mode it also releases the game window to ReXGlue's presenter (on the thread that
  // records, which owns the swapchain), and the guest thread hands the game to the Xenos plugin
  // at its next Swap (HandOver).
  void Fail();
  // Primary mode, at a Swap before anything is recorded into the back buffer: who presents into
  // the game window. The renderer while it draws the game and no XAM dialog is up; ReXGlue's
  // presenter otherwise (the dialogs are its ImGui overlay, and after the renderer switch it
  // shows the Xenos plugin's frames). A window has one swapchain at a time: the presenter's
  // surface is disconnected before ours is created, and ours released before it is connected
  // again. Returns false when the swapchain cannot be created (logged).
  bool OwnGameWindow(bool own);
  // OwnGameWindow(false) from any state, including a half-done OwnGameWindow(true).
  void ReleaseGameWindow();
  // Render thread mode.
  struct Queue;
  void QueueDraw(const uint8_t* base, const HookArgs& args);
  void QueueClear(const uint8_t* base, const HookArgs& args);
  void QueueResolve(const uint8_t* base, const HookArgs& args);
  void QueueSwap(uint8_t* base, const HookArgs& args);
  void RenderThreadMain();
  // Renderer switch. The key bind_renderer_switch (F9; a Steam Input binding can send it from a
  // back grip) requests it on the UI thread; the guest thread takes the request at its next
  // Swap. A key rather than a pad combination: the game reads every button of the pad.
  // HandOver: the Xenos plugin draws from the next frame on (also after a failed frame, Fail);
  // TakeBack: native again.
  bool SwitchRequested() {
    return switch_requested_.load(std::memory_order_relaxed) &&
           switch_requested_.exchange(false, std::memory_order_relaxed);
  }
  void HandOver(uint8_t* base, uint32_t device);
  void TakeBack();
  // Upscaler switch. bind_upscaler (F7) switches the scene between MSAA and the upscaler and
  // bind_upscaler_debug (F8) its debug view, requested on the UI thread and taken at the start
  // of the next frame. The upscaler is created at its first use and kept after.
  void SwitchUpscaler(bool on);
  // Local change: the pipeline (render targets to Presenter), and the overlay's live changes.
  bool CreatePipeline();
  bool RebuildPipeline();
  void ApplyMsaa();
  void ApplyFsrFrameGeneration();
  // Local change (pgr4 project): the frame's begin held back to the frame rate limit
  // (pgr4_native_present_interval: the present's sync interval, or with frame generation a pace),
  // and to 60 FPS on frames without a 3D scene. Returns the time slept.
  uint64_t LimitFrameRate();
  void SplitScreenUpscaler();
  std::atomic<bool> upscaler_switch_requested_{false}, upscaler_debug_requested_{false};
  // Local change (pgr4 project): bind_screenshot (F1), handled at the next frame's begin.
  std::atomic<bool> screenshot_requested_{false};
  uint32_t clean_shots_handled_ = 0;  // local change: the photo mode's screenshots taken
  void TakeScreenshot();
  bool upscaler_bound_ = false;  // primary mode
  // Local change: the in-game settings overlay (overlay.h, bind_overlay) and its request to make
  // the upscaler again with new settings.
  bool overlay_bound_ = false;
  std::atomic<bool> upscaler_recreate_requested_{false};
  std::atomic<bool> rebuild_requested_{false}, msaa_requested_{false}, fsr_fg_requested_{false};
  bool paced_ = false;  // Initialize's: frames paced on the display
  std::atomic<bool> last_frame_scene_{false};  // LimitFrameRate
  // SplitScreenUpscaler's (render thread).
  bool scene_drawn_since_resolve_ = false, upscaler_split_off_ = false;
  uint32_t scene_passes_ = 0, single_pass_frames_ = 0;
  // Local change (25/11/2026): the last frame had two scene passes (its views: volumetrics.h
  // g_pgr4_scene_view), and the rows each pass's depth resolve covered.
  bool split_last_frame_ = false;
  uint32_t pass_rows_[2] = {};
  // Development (25/11/2026): the side by side test's resolves (pgr4_split_side_by_side).
  std::string side_resolves_;
  uint32_t side_depth_resolves_ = 0, side_logs_ = 0;
  uint32_t side_missed_ = 0, side_without_two_ = 0, split_without_two_ = 0;  // development (26/11/2026): the live surface's probe
  // Local diagnostic: the frame's scene resolves (pass, source, rect, destination), logged the
  // first frames SplitScreenUpscaler sees two passes in.
  std::string scene_resolves_;
  uint32_t split_logs_ = 0;
  std::mutex settings_mutex_;
  std::vector<std::function<void()>> queued_settings_;  // QueueSettings
  bool upscaler_on_ = false;
  // The game's motion blur as the menu and the game want it (guest thread, at the Swap).
  void ApplyMotionBlurRule(uint8_t* base);
  uint32_t scene_msaa_ = 2;  // --pgr4_native_msaa, as far as the GPU has it

  static inline std::atomic<bool> paces_frames_{false};       // pacer_->pacing(), for any thread
  static inline std::atomic<NativeRenderer*> live_{nullptr};  // Live()
  std::atomic<bool> switch_requested_{false};
  bool switch_bound_ = false;  // primary mode with --pgr4_native_renderer_switch

  std::unique_ptr<Queue> queue_;  // render thread mode only

  std::unique_ptr<D3D12Device> device_;
  bool primary_ = false;       // --pgr4_native_primary: presenting into the game window
  bool window_taken_ = false;  // OwnGameWindow disconnected ReXGlue's presenter from the window
  std::unique_ptr<RenderTargets> rt_;
  std::unique_ptr<Resolver> resolver_;
  std::unique_ptr<TextureCache> textures_;
  std::unique_ptr<DrawRenderer> draw_;
  std::unique_ptr<Presenter> present_;
  std::unique_ptr<OcclusionQueries> occlusion_;  // primary mode with --pgr4_native_occlusion
  std::unique_ptr<Upscaler> upscaler_;
  // Local change (22/11/2026, pgr4_native_upscaler_keep_other): the other upscaler (DLSS or FSR)
  // kept once made, with the settings it was made with (UpscalerSettingsKey), for the next switch.
  std::unique_ptr<Upscaler> parked_upscaler_;
  std::string parked_key_;
  // Local change: the effects over the scene (scene_effects.h).
  std::unique_ptr<SceneEffects> scene_effects_;
  std::unique_ptr<Volumetrics> volumetrics_;  // local change (20/11/2026)
  // Local change: DLSS-G's HUD-less image (hudless.h), with Streamline's frame generation.
  std::unique_ptr<HudlessCapture> hudless_capture_;
  std::unique_ptr<Overlay> overlay_;  // local change
  std::unique_ptr<FrameGeneration> frame_generation_;  // --pgr4_native_frame_generation, paced
  std::unique_ptr<FgProbe> fg_probe_;  // development (28/11/2026): fg_probe.h, made on first use
  // Local change (28/11/2026, pgr4_native_fg_light_mask): light_mask_pass.h, made on first use;
  // the frames it ran on, by way (FSR, DLSS-G), for the probe (development).
  std::unique_ptr<LightMaskPass> light_mask_;
  bool light_mask_failed_ = false;
  uint64_t light_mask_fsr_ = 0, light_mask_dlssg_ = 0, light_mask_none_ = 0, light_mask_flares_ = 0;
  // Local change (28/11/2026, pgr4_native_fg_hud_mask): the HUD layer's generation (counted up at
  // each opaque composite into the front buffer), the frame's composites and the last frame's.
  uint64_t hud_generation_ = 0, hud_frame_ = UINT64_MAX, light_mask_hud_ = 0;
  // Local change (28/11/2026, pgr4_native_fg_mirror_mask): the wing mirrors' compare, at the
  // upscaler's scene depth (Upscaler::set_scene_depth_hook).
  static void SceneDepthHook(void* ctx, ID3D12GraphicsCommandList* list, const RenderTarget& depth,
                             uint32_t width, uint32_t height);
  bool EnsureLightMask();
  uint64_t light_mask_mirrors_ = 0;
  uint32_t hud_composites_ = 0, hud_composites_last_ = 0;
  // The scene's camera for the FidelityFX effects, found each frame by FindCamera (the defaults
  // until one matches); the upscaler reads it through the pointer set_camera gave it.
  FfxCamera camera_;
  // Local change (20/11/2026): its clip transform's four rows (world to clip), for the volumetric
  // light (volumetrics.h).
  float camera_clip_[16] = {};
  uint32_t camera_scan_ = 0;  // this frame's scene depth draws still to scan for it
  // Development (--pgr4_native_profile, 18/11/2026): the draws by render target
  // (RB_SURFACE_INFO): their count, the cars' among them (VsFamily) and the render thread's time,
  // logged every 300 frames (LogPasses).
  struct PassCount {
    uint64_t draws = 0, car = 0, ticks = 0;
    std::unordered_map<uint64_t, uint32_t> vs, ps;  // draws by shader (ucode hash)
  };
  std::unordered_map<uint32_t, PassCount> passes_;
  uint32_t passes_frames_ = 0;
  void LogPasses();
  // Local change (18/11/2026): the CSV has the profiler's columns (--pgr4_native_profile at the
  // start); --pgr4_native_profile is then read again at each Swap (a profile with and without it
  // in one session), the columns written either way (zeros while it is off).
  bool csv_profile_ = false;
  bool camera_logged_ = false;
  // Primary mode with --pgr4_native_frame_latency > 0, or observing only with --pgr4_native_csv.
  std::unique_ptr<FramePacer> pacer_;
  GpuTimer* gpu_timer_ = nullptr;  // device_'s, with --pgr4_native_gpu_timing
  bool frame_begun_ = false;
  uint32_t screenshot_poll_ = 0;  // local diagnostic: frames since the last screenshot.txt check
  uint64_t frame_index_ = 0;
  uint64_t frame_start_ns_ = 0;
  FrameStats stats_;
  // Ticks inside ExecuteDraw, ExecuteClear and ExecuteResolve, into stats_ at the Swap.
  uint64_t draw_ticks_ = 0, clear_ticks_ = 0, resolve_ticks_ = 0;
  uint64_t draws_submitted_ = 0;                  // stats_.draws at the frame's last SubmitEarly
  std::vector<ID3D12CommandList*> upload_lists_;  // CloseUploadLists'
  struct Csv;
  std::unique_ptr<Csv> csv_;
};

}  // namespace pgr4::native
