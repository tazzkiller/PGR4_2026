// Local addition (pgr4 project): see streamline.h.
#include "streamline.h"

#include "mfg_unlock.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>

// The SDK's signature check: non-inline definitions, so in this one file only.
#include <Softpub.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <sl_security.h>

#include <sl_dlss_g.h>
#include <sl_reflex.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xfile.h>

#include <algorithm>
#include <filesystem>
#include <string>

REXCVAR_DEFINE_BOOL(pgr4_native_dlss, false, "PGR4 native",
                    "NVIDIA DLSS Super Resolution (through Streamline) in place of FSR for the "
                    "upscaler (--pgr4_native_upscaler); needs sl.interposer.dll, sl.common.dll, "
                    "sl.dlss.dll and nvngx_dlss.dll beside the executable and an RTX GPU")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(pgr4_native_dlss_fg, false, "PGR4 native",
                    "NVIDIA DLSS Frame Generation (with Reflex): one generated frame between each "
                    "two the game draws, while the scene renders through DLSS (--pgr4_native_dlss); "
                    "needs sl.dlss_g.dll, nvngx_dlssg.dll and sl.reflex.dll, an RTX 40 or newer "
                    "GPU and hardware-accelerated GPU scheduling")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
// Local change (pgr4 project): Multi Frame Generation, on RTX 40 through mfg_unlock.h.
REXCVAR_DEFINE_INT32(pgr4_native_dlss_fg_frames, 1, "PGR4 native",
                     "DLSS Frame Generation: frames generated between each two the game draws "
                     "(1: x2, up to 5: x6; above 1 needs an RTX 50, or an RTX 40 with "
                     "--pgr4_native_dlss_mfg_unlock)")
    .range(1, 5);
REXCVAR_DEFINE_INT32(pgr4_native_dlss_fg_present_lag, 0, "PGR4 native",
                     "DLSS-G: frames between the scene DLSS evaluates and the image the guest "
                     "presents (0 or 1): the present markers and the back buffer tag take the "
                     "token of that many frames before, so frame generation pairs each image "
                     "with its own depth, motion and camera");
REXCVAR_DEFINE_BOOL(pgr4_native_reflex_boost, false, "PGR4 native",
                    "NVIDIA Reflex with Boost (with DLSS Frame Generation): the GPU keeps its "
                    "clocks when the frames are light (loading screens, menus), so a race does "
                    "not start at low clocks")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
// Local diagnostic (17/11/2026, brought over from perf-cpu on 18/11): a folder of Streamline's
// development DLLs (the SDK's bin/x64/development) loaded instead of the signed production ones
// beside the executable: the development DLSS-G's debug view (Ctrl+Shift+Insert on and off,
// Ctrl+Shift+End the next view: depth, motion vectors, their alignment with the color, the dynamic
// objects; Ctrl+Shift+Home its statistics) and NGX's development DLSS. Not signed by NVIDIA: not
// checked, and the RTX 40 unlock (mfg_unlock.h, made for the production provider) not applied.
// Empty: the production DLLs. Never in a release.
REXCVAR_DEFINE_STRING(pgr4_native_streamline_dev_dir, "", "PGR4 native",
                      "Development: the folder of Streamline's development DLLs to load instead of "
                      "the production ones (DLSS-G's debug view)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
// With them, sl.imgui too. Off (17/11/2026: with it the GPU was removed at the 60th frame's
// Present, in the menus; without it, no crash): the development DLSS-G alone.
REXCVAR_DEFINE_BOOL(pgr4_native_streamline_dev_imgui, false, "PGR4 native",
                    "Development: with pgr4_native_streamline_dev_dir, load sl.imgui too (it "
                    "removed the GPU on 17/11/2026)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(pgr4_native_dlss_fg_debug_generated_only, false, "PGR4 native",
                    "DLSS-G debug: show only the generated frames (DLSSGFlags::"
                    "eShowOnlyInterpolatedFrame)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {

struct State {
  bool tried = false, active = false;
  bool fg = false;                     // DLSS-G and Reflex loaded and supported
  sl::FrameToken* token = nullptr;     // BeginFrame's
  sl::FrameToken* previous_token = nullptr;  // the frame before's (present_lag)
  bool scene = false, scene_before = false;  // MarkScene this frame, the frame before
  int fg_mode = -1;                    // the DLSS-G mode last set (sl::DLSSGMode), -1 none
  int fg_frames = 0;                   // the frames generated last set
  uint32_t fg_max = 1;                 // DLSS-G's maximum frames generated (SetDevice)
  // Cadence diagnostic: the frame's scenes (MarkScene), back buffer tags and presents.
  int scenes = 0, tags = 0, presents = 0, odd_frames = 0;
  // Local change (pgr4 project): frames left whose generation restarts (DLSS-G's reset: the frame
  // shown alone, nothing made from the one before) after a frame that drew the scene without
  // presenting it (the game loading: a car in the garage, the menu's scene).
  int fg_skip = 0;
  uint64_t frame = 0;
  HMODULE module = nullptr;
  PFun_slInit* init = nullptr;
  PFun_slShutdown* shutdown = nullptr;
  PFun_slSetD3DDevice* set_d3d_device = nullptr;
  PFun_slUpgradeInterface* upgrade_interface = nullptr;
  PFun_slGetFeatureFunction* get_feature_function = nullptr;
  pgr4::native::streamline::Api api;
};
State g;

void LogMessage(sl::LogType type, const char* message) {
  std::string m(message ? message : "");
  while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back();
  if (type == sl::LogType::eError)
    REXLOG_ERROR("Streamline: {}", m);
  else if (type == sl::LogType::eWarn)
    REXLOG_WARN("Streamline: {}", m);
  else
    REXLOG_INFO("Streamline: {}", m);
}

template <typename T>
bool Load(T*& out, const char* name) {
  out = reinterpret_cast<T*>(GetProcAddress(g.module, name));
  if (!out) REXLOG_ERROR("PGR4 native DLSS: sl.interposer.dll has no {}", name);
  return out != nullptr;
}

std::filesystem::path ExeDir() {
  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  return std::filesystem::path(path).parent_path();
}

}  // namespace

// The feature helpers of the SDK headers (SL_FEATURE_FUN_IMPORT_STATIC) call this.
extern "C" sl::Result slGetFeatureFunction(sl::Feature feature, const char* function_name,
                                           void*& function) {
  if (!g.get_feature_function) return sl::Result::eErrorNotInitialized;
  return g.get_feature_function(feature, function_name, function);
}

namespace pgr4::native::streamline {

bool DlssRequested() { return REXCVAR_GET(pgr4_native_dlss); }
bool Active() { return g.active; }
const Api& api() { return g.api; }

bool Init() {
  if (g.tried) return g.active;
  g.tried = true;
  const std::string dev_dir = REXCVAR_GET(pgr4_native_streamline_dev_dir);
  const std::filesystem::path dir = dev_dir.empty() ? ExeDir() : std::filesystem::path(dev_dir);
  const std::filesystem::path dll = dir / L"sl.interposer.dll";
  if (!std::filesystem::exists(dll)) {
    REXLOG_ERROR("PGR4 native DLSS: no {}", dll.string());
    return false;
  }
  if (!dev_dir.empty()) {
    REXLOG_WARN("PGR4 native DLSS: Streamline's development DLLs from {} (unsigned)",
                dir.string());
  } else if (!sl::security::verifyEmbeddedSignature(dll.c_str())) {
    REXLOG_ERROR("PGR4 native DLSS: {} is not signed by NVIDIA; not loaded", dll.string());
    return false;
  }
  g.module = LoadLibraryW(dll.c_str());
  if (!g.module) {
    REXLOG_ERROR("PGR4 native DLSS: LoadLibrary({}) failed: error {}", dll.string(),
                 GetLastError());
    return false;
  }
  if (!Load(g.init, "slInit") || !Load(g.shutdown, "slShutdown") ||
      !Load(g.set_d3d_device, "slSetD3DDevice") ||
      !Load(g.upgrade_interface, "slUpgradeInterface") ||
      !Load(g.get_feature_function, "slGetFeatureFunction") ||
      !Load(g.api.getNewFrameToken, "slGetNewFrameToken") ||
      !Load(g.api.setConstants, "slSetConstants") ||
      !Load(g.api.setTagForFrame, "slSetTagForFrame") ||
      !Load(g.api.evaluateFeature, "slEvaluateFeature") ||
      !Load(g.api.isFeatureSupported, "slIsFeatureSupported") ||
      !Load(g.api.allocateResources, "slAllocateResources") ||
      !Load(g.api.freeResources, "slFreeResources"))
    return false;

  // DLSS-G needs Reflex through Streamline; PCL loads on its own.
  static const sl::Feature features_sr[] = {sl::kFeatureDLSS};
  static const sl::Feature features_fg[] = {sl::kFeatureDLSS, sl::kFeatureDLSS_G,
                                            sl::kFeatureReflex};
  // The development DLLs with pgr4_native_streamline_dev_imgui: sl.imgui too.
  static const sl::Feature features_dev[] = {sl::kFeatureDLSS, sl::kFeatureDLSS_G,
                                             sl::kFeatureReflex, sl::kFeatureImGUI};
  // DLSS-G and Reflex always load (local change): --pgr4_native_dlss_fg only switches the
  // generation, live (the game's Graphics page), which needs them loaded from the start.
  const bool want_fg = true;
  const bool dev = !dev_dir.empty();
  const bool imgui = dev && REXCVAR_GET(pgr4_native_streamline_dev_imgui);
  const sl::Feature* features = imgui ? features_dev : want_fg ? features_fg : features_sr;
  sl::Preferences pref;
  pref.showConsole = false;
  pref.logLevel = sl::LogLevel::eDefault;
  pref.logMessageCallback = LogMessage;
  pref.featuresToLoad = features;
  pref.numFeaturesToLoad = imgui     ? uint32_t(std::size(features_dev))
                           : want_fg ? uint32_t(std::size(features_fg))
                                     : uint32_t(std::size(features_sr));
  // The plugins (and the NGX providers) from the development folder.
  static std::wstring plugin_dir;
  static const wchar_t* plugin_paths[1] = {};
  if (dev) {
    plugin_dir = dir.wstring();
    plugin_paths[0] = plugin_dir.c_str();
    pref.pathsToPlugins = plugin_paths;
    pref.numPathsToPlugins = 1;
  }
  // Manual hooking: only the interfaces Upgrade gets are Streamline's (the Xenos plugin's device
  // and the process's other DXGI objects stay untouched). No over-the-air updates: only the
  // signed DLLs shipped beside the executable run.
  pref.flags |= sl::PreferenceFlags::eUseManualHooking |
                sl::PreferenceFlags::eUseFrameBasedResourceTagging;
  pref.flags &= ~(sl::PreferenceFlags::eAllowOTA | sl::PreferenceFlags::eLoadDownloadedPlugins);
  pref.engine = sl::EngineType::eCustom;
  pref.engineVersion = "pgr4-native";
  pref.projectId = "6c1a3a0e-6f8f-4b3e-9a62-5f0f3b7d2c41";
  pref.renderAPI = sl::RenderAPI::eD3D12;
  if (const sl::Result r = g.init(pref, sl::kSDKVersion); r != sl::Result::eOk) {
    REXLOG_ERROR("PGR4 native DLSS: slInit failed ({})", int(r));
    return false;
  }
  g.active = true;
  REXLOG_INFO("PGR4 native DLSS: Streamline initialized");
  return true;
}

bool SetDevice(ID3D12Device* device) {
  if (!g.active) return false;
  // Before slSetD3DDevice, where sl.dlss_g's startup reads the provider's maximum. Not over the
  // development DLLs (pgr4_native_streamline_dev_dir): made for the production provider.
  if (REXCVAR_GET(pgr4_native_streamline_dev_dir).empty()) mfg_unlock::Apply(device);
  if (const sl::Result r = g.set_d3d_device(device); r != sl::Result::eOk) {
    REXLOG_ERROR("PGR4 native DLSS: slSetD3DDevice failed ({})", int(r));
    Shutdown();
    return false;
  }
  const LUID luid = device->GetAdapterLuid();
  sl::AdapterInfo adapter;
  adapter.deviceLUID = reinterpret_cast<uint8_t*>(const_cast<LUID*>(&luid));
  adapter.deviceLUIDSizeInBytes = sizeof(LUID);
  if (const sl::Result r = g.api.isFeatureSupported(sl::kFeatureDLSS, adapter);
      r != sl::Result::eOk) {
    REXLOG_ERROR("PGR4 native DLSS: DLSS is not supported on this adapter ({})", int(r));
    Shutdown();
    return false;
  }
  REXLOG_INFO("PGR4 native DLSS: DLSS supported on the renderer's adapter");
  {  // DLSS-G and Reflex: always loaded (Init)
    const sl::Result fg = g.api.isFeatureSupported(sl::kFeatureDLSS_G, adapter);
    const sl::Result reflex = g.api.isFeatureSupported(sl::kFeatureReflex, adapter);
    if (fg != sl::Result::eOk || reflex != sl::Result::eOk) {
      // eErrorOSDisabledHWS (Result 6...): hardware-accelerated GPU scheduling is off.
      REXLOG_ERROR("PGR4 native DLSS: frame generation unavailable (DLSS-G {}, Reflex {})",
                   int(fg), int(reflex));
    } else {
      // Reflex on, as DLSS-G requires; its sleep paces the frames from BeginFrame.
      // Boost (local change, off by default): meant to keep the GPU's clocks through the
      // loading screens; it did not change the race start (still 20 to 28 ms of GPU time for
      // its first frames) and came with violent drops of the frame rate.
      sl::ReflexOptions ro;
      ro.mode = REXCVAR_GET(pgr4_native_reflex_boost) ? sl::ReflexMode::eLowLatencyWithBoost
                                                      : sl::ReflexMode::eLowLatency;
      if (const sl::Result r = slReflexSetOptions(ro); r != sl::Result::eOk) {
        REXLOG_ERROR("PGR4 native DLSS: slReflexSetOptions failed ({}); no frame generation",
                     int(r));
      } else {
        g.fg = true;
        // The frames DLSS-G may generate: an RTX 50's, or the unlocked RTX 40's (mfg_unlock.h).
        sl::DLSSGState state;
        if (slDLSSGGetState(sl::ViewportHandle(0), state, nullptr) == sl::Result::eOk)
          g.fg_max = std::max(1u, state.numFramesToGenerateMax);
        g.fg_max = std::max(g.fg_max, mfg_unlock::MaxGeneratedFrames());
        REXLOG_INFO("PGR4 native DLSS: frame generation and Reflex supported, up to x{}{}",
                    g.fg_max + 1, mfg_unlock::MaxGeneratedFrames() > 1 ? " (RTX 40 unlocked)" : "");
      }
    }
  }
  return true;
}

// Loaded whenever DLSS is (Init): --pgr4_native_dlss_fg switches it on and off live.
bool FrameGenerationRequested() { return DlssRequested(); }
bool FrameGenerationActive() { return g.active && g.fg; }
bool FrameGenerationOn() { return g.active && g.fg && g.fg_mode == int(sl::DLSSGMode::eOn); }
bool SkipGeneration() { return g.fg_skip > 0; }
uint32_t MaxGeneratedFrames() { return g.active && g.fg ? g.fg_max : 1; }
uint32_t GeneratedFrames() {
  return std::clamp(uint32_t(REXCVAR_GET(pgr4_native_dlss_fg_frames)), 1u, MaxGeneratedFrames());
}

void BeginFrame(uint64_t frame) {
  if (!g.active) return;
  g.scene_before = g.scene;
  g.scene = false;
  if (g.fg && g.fg_mode == int(sl::DLSSGMode::eOn) &&
      (g.scenes != 1 || g.tags != 1 || g.presents != 1) && g.odd_frames < 300) {
    ++g.odd_frames;
    REXLOG_WARN("PGR4 native DLSS-G cadence: frame {} had {} scenes, {} back buffer tags, {} presents",
                g.frame, g.scenes, g.tags, g.presents);
  }
  // Local change (pgr4 project): a scene drawn and never presented (the game loading something)
  // left DLSS-G generating from a frame it never showed: black frames on the screen. The next two
  // frames' generation restarts instead (SkipGeneration), generation staying on.
  if (g.scenes > 0 && g.presents == 0) g.fg_skip = 2;
  else if (g.fg_skip > 0) --g.fg_skip;
  g.scenes = g.tags = g.presents = 0;
  g.frame = frame;
  const uint32_t index = uint32_t(frame);
  g.previous_token = g.token;
  if (g.api.getNewFrameToken(g.token, &index) != sl::Result::eOk) g.token = nullptr;
  if (!g.fg || !g.token) return;
  slReflexSleep(*g.token);
  // The game's simulation runs on its own threads; the frame's recording starts here.
  slPCLSetMarker(sl::PCLMarker::eSimulationStart, *g.token);
  slPCLSetMarker(sl::PCLMarker::eSimulationEnd, *g.token);
  // Generation only from frames with the scene's inputs (not the menus' or loading screens'):
  // the mode takes effect at the next present.
  // --pgr4_native_dlss_fg read each frame: the overlay (overlay.h) switches it live.
  // Local change: never while the game plays a video (a Bink file open, XFile::OpenMovies).
  const bool generate = g.scene_before && REXCVAR_GET(pgr4_native_dlss_fg) &&
                        rex::system::XFile::OpenMovies() == 0;
  const int mode = int(generate ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff);
  const int frames = int(GeneratedFrames());
  if (mode != g.fg_mode || (mode && frames != g.fg_frames)) {
    sl::DLSSGOptions o;
    o.mode = sl::DLSSGMode(mode);
    o.numFramesToGenerate = uint32_t(frames);
    o.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
    if (REXCVAR_GET(pgr4_native_dlss_fg_debug_generated_only))
      o.flags |= sl::DLSSGFlags::eShowOnlyInterpolatedFrame;
    if (const sl::Result r = slDLSSGSetOptions(sl::ViewportHandle(0), o); r != sl::Result::eOk) {
      REXLOG_ERROR("PGR4 native DLSS: slDLSSGSetOptions failed ({}); no frame generation", int(r));
      g.fg = false;
      return;
    }
    REXLOG_INFO("PGR4 native DLSS: frame generation {}", mode ? "x" + std::to_string(frames + 1) : "off");
    g.fg_mode = mode;
    g.fg_frames = frames;
  }
}

sl::FrameToken* Token() { return g.active ? g.token : nullptr; }
void MarkScene() {
  g.scene = true;
  ++g.scenes;
}

// The token the guest's present belongs to: present_lag frames before the scene's (BeginFrame).
static sl::FrameToken* PresentToken() {
  return REXCVAR_GET(pgr4_native_dlss_fg_present_lag) > 0 ? g.previous_token : g.token;
}

void Marker(sl::PCLMarker marker) {
  if (!g.active || !g.fg) return;
  const bool present =
      marker == sl::PCLMarker::ePresentStart || marker == sl::PCLMarker::ePresentEnd;
  sl::FrameToken* const token = present ? PresentToken() : g.token;
  if (marker == sl::PCLMarker::ePresentStart) ++g.presents;
  if (token) slPCLSetMarker(marker, *token);
}

void TagBackbuffer(int32_t left, int32_t top, int32_t width, int32_t height,
                   ID3D12Resource* hudless) {
  if (!g.active || !g.fg || !PresentToken() || width <= 0 || height <= 0) return;
  ++g.tags;
  // No resource: the extent alone (Streamline's back buffer is the proxy swapchain's).
  static sl::Extent extent;
  extent = {uint32_t(std::max(top, 0)), uint32_t(std::max(left, 0)), uint32_t(width),
            uint32_t(height)};
  static sl::Resource hudless_res;
  hudless_res = sl::Resource(sl::ResourceType::eTex2d, hudless,
                             uint32_t(D3D12_RESOURCE_STATE_PRESENT));
  // Without this frame's HUD-less image, a null one: DLSS-G forgets the last frame's.
  const sl::ResourceTag tags[2] = {
      {nullptr, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle::eValidUntilPresent, &extent},
      {hudless ? &hudless_res : nullptr, sl::kBufferTypeHUDLessColor,
       sl::ResourceLifecycle::eValidUntilPresent, &extent}};
  if (const sl::Result r = g.api.setTagForFrame(*PresentToken(), sl::ViewportHandle(0), tags, 2, nullptr);
      r != sl::Result::eOk) {
    static bool logged = false;
    if (!std::exchange(logged, true))
      REXLOG_WARN("PGR4 native DLSS: back buffer extent tag failed ({})", int(r));
  }
}

bool Upgrade(void** iface) {
  if (!g.active) return false;
  void* before = *iface;
  if (const sl::Result r = g.upgrade_interface(iface); r != sl::Result::eOk || !*iface) {
    REXLOG_ERROR("PGR4 native DLSS: slUpgradeInterface failed ({})", int(r));
    *iface = before;
    return false;
  }
  return true;
}

void Shutdown() {
  if (g.active && g.shutdown) g.shutdown();
  g.active = false;
}

}  // namespace pgr4::native::streamline
