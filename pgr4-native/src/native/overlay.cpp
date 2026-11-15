// Local change (pgr4 project): the in-game settings overlay (overlay.h).
#include "overlay.h"

#include "streamline.h"
#include "tunables.h"

#include "d3d12_device.h"
#include "log.h"

#include <imgui.h>
#include <imgui_impl_dx12.h>

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

REXCVAR_DECLARE(bool, pgr4_native_dlss);
REXCVAR_DECLARE(bool, pgr4_freeze_car);  // src/pgr4_hooks.cpp
REXCVAR_DECLARE(std::string, pgr4_native_dlss_mode);
REXCVAR_DECLARE(std::string, pgr4_native_dlss_preset);
REXCVAR_DECLARE(bool, pgr4_native_dlss_fg);
REXCVAR_DECLARE(int32_t, pgr4_native_dlss_fg_frames);
REXCVAR_DECLARE(bool, pgr4_native_frame_generation);
REXCVAR_DECLARE(int32_t, pgr4_native_dlss_fg_mvec_x);
REXCVAR_DECLARE(bool, pgr4_native_watch_hot);  // draw.cpp
REXCVAR_DECLARE(int32_t, pgr4_native_dlss_fg_mvec_y);
REXCVAR_DECLARE(int32_t, pgr4_native_upscaler_width);
REXCVAR_DECLARE(int32_t, pgr4_native_upscaler_height);
REXCVAR_DECLARE(bool, pgr4_native_reactive_mask);
REXCVAR_DECLARE(bool, pgr4_native_scaled_cube);
REXCVAR_DECLARE(bool, pgr4_native_shader_replace);
REXCVAR_DECLARE(bool, pgr4_native_gtao);
REXCVAR_DECLARE(bool, pgr4_native_original_look);
REXCVAR_DECLARE(double, pgr4_native_gtao_radius);
REXCVAR_DECLARE(double, pgr4_native_gtao_intensity);
REXCVAR_DECLARE(double, pgr4_native_gtao_distance);
REXCVAR_DECLARE(bool, pgr4_native_gtao_debug);
REXCVAR_DECLARE(bool, pgr4_native_gtao_full);
REXCVAR_DECLARE(double, pgr4_native_sharpen_dlss);
REXCVAR_DECLARE(double, pgr4_native_sharpen_fsr);
REXCVAR_DECLARE(double, pgr4_native_reactive_scale);
REXCVAR_DECLARE(double, pgr4_native_reactive_max);
REXCVAR_DECLARE(bool, pgr4_native_reactive_dlss);
REXCVAR_DECLARE(bool, pgr4_native_reactive_show);
REXCVAR_DECLARE(int32_t, pgr4_native_msaa);
REXCVAR_DECLARE(int32_t, pgr4_native_anisotropy);
REXCVAR_DECLARE(int32_t, pgr4_native_resolution_scale);
REXCVAR_DECLARE(double, pgr4_native_tune_road_relief);
REXCVAR_DECLARE(double, pgr4_native_tune_sun_glint);
REXCVAR_DECLARE(double, pgr4_native_tune_sun_sharpness);
REXCVAR_DECLARE(double, pgr4_native_tune_rim);
REXCVAR_DECLARE(double, pgr4_native_tune_glass_reflect);
REXCVAR_DECLARE(double, pgr4_native_tune_glass_sky);
REXCVAR_DECLARE(double, pgr4_native_tune_glass_sky_from);
REXCVAR_DECLARE(double, pgr4_native_tune_drops_paint_scale);
REXCVAR_DECLARE(double, pgr4_native_tune_drops_glide_angle);
REXCVAR_DECLARE(double, pgr4_native_tune_drops_stretch);
REXCVAR_DECLARE(double, pgr4_native_tune_drops_near);
REXCVAR_DECLARE(double, pgr4_native_tune_drops_paint_strength);
REXCVAR_DECLARE(double, pgr4_native_drops_glide_rate);
REXCVAR_DECLARE(double, pgr4_native_drops_glide_idle);
REXCVAR_DECLARE(double, pgr4_native_tune_frost_paint_scale);
REXCVAR_DECLARE(double, pgr4_native_dirt_force);
REXCVAR_DECLARE(double, pgr4_native_dirt_fine);
REXCVAR_DECLARE(double, pgr4_native_tune_scratch_paint_scale);
REXCVAR_DECLARE(double, pgr4_native_tune_wave_strength);
REXCVAR_DECLARE(double, pgr4_native_tune_wave_steepness);
REXCVAR_DECLARE(double, pgr4_native_tune_wave_noise);
REXCVAR_DECLARE(double, pgr4_native_tune_wind_angle);
REXCVAR_DECLARE(double, pgr4_native_tune_wave_speed);
REXCVAR_DECLARE(double, pgr4_native_tune_wave_size);
REXCVAR_DECLARE(double, pgr4_native_tune_harbour_clarity);
REXCVAR_DECLARE(double, pgr4_native_tune_harbour_distort);
REXCVAR_DECLARE(double, pgr4_native_tune_harbour_waves);
REXCVAR_DECLARE(double, pgr4_native_tune_highlights);
REXCVAR_DECLARE(double, pgr4_native_tune_bloom);
REXCVAR_DECLARE(double, pgr4_native_tune_water_reflect);
REXCVAR_DECLARE(double, pgr4_native_tune_road_reflect);
REXCVAR_DECLARE(double, pgr4_native_tune_road_blur);
REXCVAR_DECLARE(double, pgr4_native_tune_road_fresnel);
REXCVAR_DECLARE(double, pgr4_native_tune_road_grain);
REXCVAR_DECLARE(double, pgr4_native_tune_road_puddles);
REXCVAR_DECLARE(double, pgr4_native_tune_road_sheen);
REXCVAR_DECLARE(double, pgr4_native_tune_road_reflect_light);
REXCVAR_DECLARE(double, pgr4_native_tune_road_blur_light);
REXCVAR_DECLARE(double, pgr4_native_tune_road_fresnel_light);
REXCVAR_DECLARE(double, pgr4_native_tune_road_grain_light);
REXCVAR_DECLARE(double, pgr4_native_tune_road_puddles_light);
REXCVAR_DECLARE(double, pgr4_native_tune_road_sheen_light);
REXCVAR_DECLARE(double, pgr4_native_upscaler_lod_offset);
REXCVAR_DECLARE(double, pgr4_native_msaa_texture_soften);  // draw.cpp
REXCVAR_DECLARE(double, pgr4_camera_shake_scale);  // src/pgr4_hooks.cpp
REXCVAR_DECLARE(double, pgr4_crowd_3d_radius);     // src/pgr4_hooks.cpp
REXCVAR_DECLARE(bool, pgr4_car_shadow_fix);         // src/pgr4_hooks.cpp
REXCVAR_DECLARE(bool, pgr4_native_alpha_to_coverage);  // draw.cpp
REXCVAR_DECLARE(double, pgr4_native_coplanar_bias);     // draw.cpp

REXCVAR_DECLARE(bool, pgr4_native_enhanced_reflections);  // shader_replace.cpp
REXCVAR_DECLARE(int32_t, pgr4_native_details);  // tunables.cpp
REXCVAR_DECLARE(bool, pgr4_native_decal_light_off);       // shader_replace.cpp

namespace pgr4::native {
// Defined in textures.cpp, inside this namespace.
REXCVAR_DECLARE(bool, pgr4_native_texture_hd);
REXCVAR_DECLARE(bool, pgr4_native_texture_hd_reload);

namespace {

Overlay* g_overlay = nullptr;  // the hooked window's

uint64_t NowTicks() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return uint64_t(t.QuadPart);
}
double TickSeconds() {
  static const double s = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return 1.0 / double(f.QuadPart);
  }();
  return s;
}

// A combo over `values` (shown as `labels`) for the string `value`; true when changed.
bool StringCombo(const char* label, std::string& value, const char* const* values,
                 const char* const* labels, int count) {
  int current = 0;
  for (int i = 0; i < count; ++i)
    if (value == values[i]) current = i;
  if (!ImGui::Combo(label, &current, labels, count)) return false;
  value = values[current];
  return true;
}
// The same over integers.
bool IntCombo(const char* label, int32_t& value, const int* values, const char* const* labels,
              int count) {
  int current = 0;
  for (int i = 0; i < count; ++i)
    if (value == values[i]) current = i;
  if (!ImGui::Combo(label, &current, labels, count)) return false;
  value = values[current];
  return true;
}

}  // namespace

// The renderer's settings (the overlay's and the game's Graphics page's, pgr4_menu.cpp) into
// pgr4.toml beside the executable and the project's copy, other lines and comments kept.
// Returns what was written where.
std::string SaveRendererSettings(bool upscaler_on) {
  const auto quoted = [](const std::string& v) { return "\"" + v + "\""; };
  const std::pair<const char*, std::string> values[] = {
      {"pgr4_native_upscaler", upscaler_on ? "true" : "false"},
      {"pgr4_native_dlss", REXCVAR_GET(pgr4_native_dlss) ? "true" : "false"},
      {"pgr4_native_dlss_mode", quoted(REXCVAR_GET(pgr4_native_dlss_mode))},
      {"pgr4_native_dlss_preset", quoted(REXCVAR_GET(pgr4_native_dlss_preset))},
      {"pgr4_native_dlss_fg", REXCVAR_GET(pgr4_native_dlss_fg) ? "true" : "false"},
      {"pgr4_native_dlss_fg_frames", rex::cvar::GetFlagByName("pgr4_native_dlss_fg_frames")},
      {"pgr4_native_frame_generation", REXCVAR_GET(pgr4_native_frame_generation) ? "true" : "false"},
      {"pgr4_native_dlss_fg_mvec_x", std::to_string(REXCVAR_GET(pgr4_native_dlss_fg_mvec_x))},
      {"pgr4_native_dlss_fg_mvec_y", std::to_string(REXCVAR_GET(pgr4_native_dlss_fg_mvec_y))},
      {"pgr4_native_upscaler_width", std::to_string(REXCVAR_GET(pgr4_native_upscaler_width))},
      {"pgr4_native_upscaler_height", std::to_string(REXCVAR_GET(pgr4_native_upscaler_height))},
      {"pgr4_native_resolution_scale", std::to_string(REXCVAR_GET(pgr4_native_resolution_scale))},
      // The SDK's (d3d12_presenter.cpp), the DISPLAY page's VSYNC row: set there by its name, it
      // was applied but never saved, back to the file's value at the next start.
      {"d3d12_present_vsync",
       [] {
         const std::string v = rex::cvar::GetFlagByName("d3d12_present_vsync");
         return std::string(v == "true" || v == "1" ? "true" : "false");
       }()},
      {"pgr4_native_msaa", std::to_string(REXCVAR_GET(pgr4_native_msaa))},
      {"pgr4_native_anisotropy", std::to_string(REXCVAR_GET(pgr4_native_anisotropy))},
      {"pgr4_native_gtao", REXCVAR_GET(pgr4_native_gtao) ? "true" : "false"},
      {"pgr4_native_gtao_radius", rex::cvar::GetFlagByName("pgr4_native_gtao_radius")},
      {"pgr4_native_gtao_intensity", rex::cvar::GetFlagByName("pgr4_native_gtao_intensity")},
      {"pgr4_native_gtao_distance", rex::cvar::GetFlagByName("pgr4_native_gtao_distance")},
      {"pgr4_native_gtao_full", REXCVAR_GET(pgr4_native_gtao_full) ? "true" : "false"},
      {"pgr4_native_sharpen_dlss", rex::cvar::GetFlagByName("pgr4_native_sharpen_dlss")},
      {"pgr4_native_sharpen_fsr", rex::cvar::GetFlagByName("pgr4_native_sharpen_fsr")},
      {"pgr4_native_reactive_mask", REXCVAR_GET(pgr4_native_reactive_mask) ? "true" : "false"},
      {"pgr4_native_reactive_dlss", REXCVAR_GET(pgr4_native_reactive_dlss) ? "true" : "false"},
      {"pgr4_native_reactive_scale", rex::cvar::GetFlagByName("pgr4_native_reactive_scale")},
      {"pgr4_native_reactive_max", rex::cvar::GetFlagByName("pgr4_native_reactive_max")},
      {"pgr4_crowd_3d_radius", rex::cvar::GetFlagByName("pgr4_crowd_3d_radius")},
      {"pgr4_car_shadow_fix", REXCVAR_GET(pgr4_car_shadow_fix) ? "true" : "false"},
      {"pgr4_native_alpha_to_coverage",
       REXCVAR_GET(pgr4_native_alpha_to_coverage) ? "true" : "false"},
      {"pgr4_native_msaa_texture_soften",
       rex::cvar::GetFlagByName("pgr4_native_msaa_texture_soften")},
      {"pgr4_native_upscaler_lod_offset",
       rex::cvar::GetFlagByName("pgr4_native_upscaler_lod_offset")},
      {"pgr4_native_texture_hd", REXCVAR_GET(pgr4_native_texture_hd) ? "true" : "false"},
      {"pgr4_native_decal_light_off",
       REXCVAR_GET(pgr4_native_decal_light_off) ? "true" : "false"},
      {"pgr4_native_details", std::to_string(REXCVAR_GET(pgr4_native_details))},
      {"pgr4_native_enhanced_reflections",
       REXCVAR_GET(pgr4_native_enhanced_reflections) ? "true" : "false"},
      {"pgr4_native_tune_road_relief", rex::cvar::GetFlagByName("pgr4_native_tune_road_relief")},
      {"pgr4_native_tune_sun_glint", rex::cvar::GetFlagByName("pgr4_native_tune_sun_glint")},
      {"pgr4_native_tune_sun_sharpness",
       rex::cvar::GetFlagByName("pgr4_native_tune_sun_sharpness")},
      {"pgr4_native_tune_rim", rex::cvar::GetFlagByName("pgr4_native_tune_rim")},
      {"pgr4_native_tune_glass_reflect",
       rex::cvar::GetFlagByName("pgr4_native_tune_glass_reflect")},
      {"pgr4_native_tune_glass_sky", rex::cvar::GetFlagByName("pgr4_native_tune_glass_sky")},
      {"pgr4_native_tune_glass_sky_from",
       rex::cvar::GetFlagByName("pgr4_native_tune_glass_sky_from")},
      {"pgr4_native_tune_drops_paint_scale",
       rex::cvar::GetFlagByName("pgr4_native_tune_drops_paint_scale")},
      {"pgr4_native_tune_drops_glide_angle",
       rex::cvar::GetFlagByName("pgr4_native_tune_drops_glide_angle")},
      {"pgr4_native_tune_drops_stretch",
       rex::cvar::GetFlagByName("pgr4_native_tune_drops_stretch")},
      {"pgr4_native_tune_drops_near", rex::cvar::GetFlagByName("pgr4_native_tune_drops_near")},
      {"pgr4_native_tune_drops_paint_strength",
       rex::cvar::GetFlagByName("pgr4_native_tune_drops_paint_strength")},
      {"pgr4_native_drops_glide_rate", rex::cvar::GetFlagByName("pgr4_native_drops_glide_rate")},
      {"pgr4_native_drops_glide_idle", rex::cvar::GetFlagByName("pgr4_native_drops_glide_idle")},
      {"pgr4_native_tune_frost_paint_scale",
       rex::cvar::GetFlagByName("pgr4_native_tune_frost_paint_scale")},
      {"pgr4_native_tune_scratch_paint_scale",
       rex::cvar::GetFlagByName("pgr4_native_tune_scratch_paint_scale")},
      {"pgr4_native_tune_wave_strength", rex::cvar::GetFlagByName("pgr4_native_tune_wave_strength")},
      {"pgr4_native_tune_wave_steepness",
       rex::cvar::GetFlagByName("pgr4_native_tune_wave_steepness")},
      {"pgr4_native_tune_wave_noise", rex::cvar::GetFlagByName("pgr4_native_tune_wave_noise")},
      {"pgr4_native_tune_wind_angle", rex::cvar::GetFlagByName("pgr4_native_tune_wind_angle")},
      {"pgr4_native_tune_wave_speed", rex::cvar::GetFlagByName("pgr4_native_tune_wave_speed")},
      {"pgr4_native_tune_wave_size", rex::cvar::GetFlagByName("pgr4_native_tune_wave_size")},
      {"pgr4_native_tune_harbour_clarity",
       rex::cvar::GetFlagByName("pgr4_native_tune_harbour_clarity")},
      {"pgr4_native_tune_harbour_distort",
       rex::cvar::GetFlagByName("pgr4_native_tune_harbour_distort")},
      {"pgr4_native_tune_harbour_waves",
       rex::cvar::GetFlagByName("pgr4_native_tune_harbour_waves")},
      {"pgr4_native_tune_highlights", rex::cvar::GetFlagByName("pgr4_native_tune_highlights")},
      {"pgr4_native_tune_water_reflect", rex::cvar::GetFlagByName("pgr4_native_tune_water_reflect")},
      {"pgr4_native_tune_road_reflect", rex::cvar::GetFlagByName("pgr4_native_tune_road_reflect")},
      {"pgr4_native_tune_road_blur", rex::cvar::GetFlagByName("pgr4_native_tune_road_blur")},
      {"pgr4_native_tune_road_fresnel", rex::cvar::GetFlagByName("pgr4_native_tune_road_fresnel")},
      {"pgr4_native_tune_road_grain", rex::cvar::GetFlagByName("pgr4_native_tune_road_grain")},
      {"pgr4_native_tune_road_puddles", rex::cvar::GetFlagByName("pgr4_native_tune_road_puddles")},
      {"pgr4_native_tune_road_sheen", rex::cvar::GetFlagByName("pgr4_native_tune_road_sheen")},
      {"pgr4_native_tune_road_reflect_light",
       rex::cvar::GetFlagByName("pgr4_native_tune_road_reflect_light")},
      {"pgr4_native_tune_road_blur_light",
       rex::cvar::GetFlagByName("pgr4_native_tune_road_blur_light")},
      {"pgr4_native_tune_road_fresnel_light",
       rex::cvar::GetFlagByName("pgr4_native_tune_road_fresnel_light")},
      {"pgr4_native_tune_road_grain_light",
       rex::cvar::GetFlagByName("pgr4_native_tune_road_grain_light")},
      {"pgr4_native_tune_road_puddles_light",
       rex::cvar::GetFlagByName("pgr4_native_tune_road_puddles_light")},
      {"pgr4_native_tune_road_sheen_light",
       rex::cvar::GetFlagByName("pgr4_native_tune_road_sheen_light")},
      // The window mode (the game's PC DISPLAY page), the SDK's own cvars.
      {"fullscreen", rex::cvar::GetFlagByName("fullscreen")},
      {"fullscreen_exclusive", rex::cvar::GetFlagByName("fullscreen_exclusive")},
      {"fullscreen_resolution", quoted(rex::cvar::GetFlagByName("fullscreen_resolution"))},
      // PC DISPLAY's motion blur and frame rate limit.
      {"pgr4_disable_motion_blur", rex::cvar::GetFlagByName("pgr4_disable_motion_blur")},
      {"pgr4_native_present_interval", rex::cvar::GetFlagByName("pgr4_native_present_interval")},
      {"pgr4_native_brightness", rex::cvar::GetFlagByName("pgr4_native_brightness")},
      {"pgr4_native_fps_limit", rex::cvar::GetFlagByName("pgr4_native_fps_limit")},
  };
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::vector<std::filesystem::path> paths = {std::filesystem::path(exe).parent_path() /
                                              "pgr4.toml"};
#ifdef PGR4_SOURCE_TOML
  // The project's copy too: the build copies it beside the executable.
  // Local change (31/10/2026): only from the build folder. A copy elsewhere (the desktop
  // release) wrote its own folders into the project's, which the next build copied back.
  if (std::error_code ec; std::filesystem::exists(PGR4_SOURCE_TOML) &&
      std::filesystem::equivalent(paths.front().parent_path(), PGR4_BUILD_DIR, ec))
    paths.push_back(PGR4_SOURCE_TOML);
#endif
  std::string status;
  for (const std::filesystem::path& path : paths) {
    std::vector<std::string> file;
    {
      std::ifstream in(path);
      for (std::string line; std::getline(in, line);) file.push_back(line);
    }
    for (const auto& [key, value] : values) {
      const std::string k = key;
      bool found = false;
      for (std::string& line : file) {
        const size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line.compare(start, k.size(), k) != 0) continue;
        const size_t eq = line.find_first_not_of(" \t", start + k.size());
        if (eq == std::string::npos || line[eq] != '=') continue;
        line = k + " = " + value;
        found = true;
      }
      if (!found) file.push_back(k + " = " + value);
    }
    std::ofstream out(path, std::ios::trunc);
    for (const std::string& line : file) out << line << '\n';
    status += std::string(status.empty() ? "" : "\n") +
                    (out ? "Enregistre : " : "Echec de l'ecriture de ") + path.string();
  }
  return status;
}

Overlay::~Overlay() { Shutdown(); }

bool Overlay::Initialize(DXGI_FORMAT format) {
  ImGuiContext* const previous = ImGui::GetCurrentContext();
  context_ = ImGui::CreateContext();
  ImGui::SetCurrentContext(context_);
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  io.MouseDrawCursor = true;  // the game window may hide the system's
  ImGui::StyleColorsDark();
  ImGuiStyle& style = ImGui::GetStyle();
  const float scale = std::max(1.0f, float(device_.height()) / 900.0f);
  style.ScaleAllSizes(scale);
  style.FontScaleMain = scale;
  style.WindowRounding = 6.0f;
  style.Colors[ImGuiCol_WindowBg].w = 0.92f;

  ID3D12Device* device = device_.device();
  D3D12_DESCRIPTOR_HEAP_DESC hd = {};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  hd.NumDescriptors = 64;
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  bool ok = !Failed(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srv_heap_)),
                    "CreateDescriptorHeap(overlay)");
  if (ok) {
    srv_stride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    srv_used_.assign(hd.NumDescriptors, false);
    ImGui_ImplDX12_InitInfo ii;
    ii.Device = device;
    ii.CommandQueue = device_.command_queue();
    ii.NumFramesInFlight = int(D3D12Device::kFramesInFlight) + 1;
    ii.RTVFormat = format;
    ii.DSVFormat = DXGI_FORMAT_UNKNOWN;
    ii.UserData = this;
    ii.SrvDescriptorHeap = srv_heap_.Get();
    ii.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* cpu,
                                 D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
      auto* self = static_cast<Overlay*>(info->UserData);
      const auto it = std::find(self->srv_used_.begin(), self->srv_used_.end(), false);
      const size_t i = it == self->srv_used_.end() ? 0 : size_t(it - self->srv_used_.begin());
      self->srv_used_[i] = true;
      *cpu = self->srv_heap_->GetCPUDescriptorHandleForHeapStart();
      *gpu = self->srv_heap_->GetGPUDescriptorHandleForHeapStart();
      cpu->ptr += i * self->srv_stride_;
      gpu->ptr += i * self->srv_stride_;
    };
    ii.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                                D3D12_GPU_DESCRIPTOR_HANDLE) {
      auto* self = static_cast<Overlay*>(info->UserData);
      const size_t i = size_t(cpu.ptr - self->srv_heap_->GetCPUDescriptorHandleForHeapStart().ptr) /
                       self->srv_stride_;
      if (i < self->srv_used_.size()) self->srv_used_[i] = false;
    };
    ok = ImGui_ImplDX12_Init(&ii);
    if (!ok) REXLOG_ERROR("PGR4 native overlay: ImGui's DX12 backend failed");
  }
  ImGui::SetCurrentContext(previous);
  if (!ok) {
    ImGui::DestroyContext(context_);
    context_ = nullptr;
    return false;
  }
  format_ = format;
  initialized_ = true;
  Hook(device_.window());
  REXLOG_INFO("PGR4 native overlay: ready (ImGui {})", IMGUI_VERSION);
  return true;
}

void Overlay::Shutdown() {
  Unhook();
  if (!context_) return;
  device_.WaitIdle();
  ImGuiContext* const previous = ImGui::GetCurrentContext();
  ImGui::SetCurrentContext(context_);
  if (initialized_) ImGui_ImplDX12_Shutdown();
  ImGui::DestroyContext(context_);
  ImGui::SetCurrentContext(previous == context_ ? nullptr : previous);
  context_ = nullptr;
  initialized_ = false;
  srv_heap_.Reset();
}

void Overlay::Hook(HWND hwnd) {
  if (!hwnd || hwnd_) return;
  g_overlay = this;
  hwnd_ = hwnd;
  previous_proc_ = reinterpret_cast<WNDPROC>(
      SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Overlay::WndProc)));
  if (!previous_proc_) {
    REXLOG_WARN("PGR4 native overlay: the window's messages are out of reach; no mouse");
    hwnd_ = nullptr;
    g_overlay = nullptr;
  }
}

void Overlay::Unhook() {
  if (!hwnd_) return;
  // Only if nothing hooked the window after it.
  if (GetWindowLongPtrW(hwnd_, GWLP_WNDPROC) == reinterpret_cast<LONG_PTR>(&Overlay::WndProc))
    SetWindowLongPtrW(hwnd_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(previous_proc_));
  hwnd_ = nullptr;
  g_overlay = nullptr;
}

LRESULT CALLBACK Overlay::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  Overlay* const self = g_overlay;
  if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
  if (self->capturing_.load(std::memory_order_relaxed)) {
    switch (msg) {
      case WM_MOUSEMOVE:
      case WM_LBUTTONDOWN:
      case WM_LBUTTONUP:
      case WM_LBUTTONDBLCLK:
      case WM_RBUTTONDOWN:
      case WM_RBUTTONUP:
      case WM_RBUTTONDBLCLK:
      case WM_MBUTTONDOWN:
      case WM_MBUTTONUP:
      case WM_MOUSEWHEEL: {
        std::lock_guard lock(self->events_mutex_);
        self->events_.push_back({msg, wp, lp});
        return 0;
      }
      case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
          SetCursor(nullptr);  // ImGui draws its own
          return TRUE;
        }
        break;
      default:
        break;
    }
  }
  return CallWindowProcW(self->previous_proc_, hwnd, msg, wp, lp);
}

void Overlay::FeedInput() {
  std::vector<Event> events;
  {
    std::lock_guard lock(events_mutex_);
    events.swap(events_);
  }
  ImGuiIO& io = ImGui::GetIO();
  // Client pixels to back buffer pixels.
  RECT client = {};
  float sx = 1.0f, sy = 1.0f;
  if (hwnd_ && GetClientRect(hwnd_, &client) && client.right > 0 && client.bottom > 0) {
    sx = io.DisplaySize.x / float(client.right);
    sy = io.DisplaySize.y / float(client.bottom);
  }
  for (const Event& e : events) {
    switch (e.msg) {
      case WM_MOUSEMOVE:
        io.AddMousePosEvent(float(short(LOWORD(e.lp))) * sx, float(short(HIWORD(e.lp))) * sy);
        break;
      case WM_LBUTTONDOWN:
      case WM_LBUTTONDBLCLK:
        io.AddMouseButtonEvent(0, true);
        break;
      case WM_LBUTTONUP:
        io.AddMouseButtonEvent(0, false);
        break;
      case WM_RBUTTONDOWN:
      case WM_RBUTTONDBLCLK:
        io.AddMouseButtonEvent(1, true);
        break;
      case WM_RBUTTONUP:
        io.AddMouseButtonEvent(1, false);
        break;
      case WM_MBUTTONDOWN:
        io.AddMouseButtonEvent(2, true);
        break;
      case WM_MBUTTONUP:
        io.AddMouseButtonEvent(2, false);
        break;
      case WM_MOUSEWHEEL:
        io.AddMouseWheelEvent(0.0f, float(GET_WHEEL_DELTA_WPARAM(e.wp)) / float(WHEEL_DELTA));
        break;
      default:
        break;
    }
  }
}

Overlay::Actions Overlay::Draw(ID3D12GraphicsCommandList* list, ID3D12Resource* target,
                               D3D12_CPU_DESCRIPTOR_HANDLE rtv, DXGI_FORMAT format,
                               const Info& info) {
  // The frame rate, over half a second.
  const uint64_t now = NowTicks();
  if (!rate_start_) rate_start_ = now;
  ++rate_frames_;
  const double elapsed = double(now - rate_start_) * TickSeconds();
  if (elapsed >= 0.5) {
    fps_ = float(rate_frames_ / elapsed);
    frame_ms_ = float(elapsed * 1000.0 / rate_frames_);
    rate_start_ = now;
    rate_frames_ = 0;
  }

  if (toggle_requested_.exchange(false)) visible_ = !visible_;
  drew_ = false;
  if (visible_ && !initialized_ && !failed_ && !Initialize(format)) failed_ = true;
  if (initialized_ && format != format_) {  // the swapchain went HDR or back
    Shutdown();
    if (!Initialize(format)) failed_ = true;
  }
  capturing_.store(visible_ && initialized_, std::memory_order_relaxed);
  if (!visible_ || !initialized_) return {};
  drew_ = true;

  ImGuiContext* const previous = ImGui::GetCurrentContext();
  ImGui::SetCurrentContext(context_);
  ImGuiIO& io = ImGui::GetIO();
  const D3D12_RESOURCE_DESC desc = target->GetDesc();
  io.DisplaySize = ImVec2(float(desc.Width), float(desc.Height));
  io.DeltaTime = frame_ms_ > 0.0f ? frame_ms_ / 1000.0f : 1.0f / 60.0f;
  FeedInput();
  ImGui_ImplDX12_NewFrame();
  ImGui::NewFrame();
  const Actions actions = Build(info);
  ImGui::Render();

  D3D12_RESOURCE_BARRIER b = {};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = target;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
  b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
  list->ResourceBarrier(1, &b);
  list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get()};
  list->SetDescriptorHeaps(1, heaps);
  ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), list);
  std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
  list->ResourceBarrier(1, &b);
  ImGui::SetCurrentContext(previous);
  return actions;
}

Overlay::Actions Overlay::Build(const Info& info) {
  Actions actions;
  if (resolution_scale_ == 0) {  // the restart-only settings as launched
    resolution_scale_ = int(info.resolution_scale);
    msaa_ = REXCVAR_GET(pgr4_native_msaa);
    anisotropy_ = REXCVAR_GET(pgr4_native_anisotropy);
  }
  ImGui::SetNextWindowPos(ImVec2(24.0f, 120.0f), ImGuiCond_FirstUseEver);
  // Local change (pgr4 project): a fixed size to begin with, the screen's height at most (it had
  // grown past it), its contents scrolled; resizable, kept within the screen.
  const ImVec2 screen = ImGui::GetIO().DisplaySize;
  ImGui::SetNextWindowSize(ImVec2(560.0f, std::min(720.0f, screen.y - 160.0f)),
                           ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(ImVec2(360.0f, 200.0f),
                                      ImVec2(screen.x - 24.0f, screen.y - 24.0f));
  ImGui::Begin("PGR4 - reglages en direct (F2)", nullptr);
  // Local change (pgr4 project): two tabs, the renderer's settings and the tests (textures and
  // shaders tuned hot).
  const bool tabs = ImGui::BeginTabBar("pgr4_tabs");
  if (tabs && ImGui::BeginTabItem("Rendu")) {

  // Local change (pgr4 project): the game as it was, without this project's changes, to compare
  // (pgr4_native_original_look: each change checks it, the other settings kept as they are).
  {
    bool original = REXCVAR_GET(pgr4_native_original_look);
    if (ImGui::Checkbox("Jeu d'origine (sans nos modifs)", &original))
      REXCVAR_SET(pgr4_native_original_look, original);
    ImGui::TextDisabled("Coupe nos shaders, textures HD, reflets HD, GTAO, ciel et eau");
  }

  ImGui::SeparatorText("Performances");
  ImGui::Text("Jeu : %.1f images/s (%.2f ms)", fps_, frame_ms_);
  const bool fg_on = info.fg_available && REXCVAR_GET(pgr4_native_dlss_fg) && info.upscaler_on &&
                     info.dlss;
  if (fg_on)
    ImGui::Text("Affichees : ~%.0f images/s avec DLSS-G x%u", fps_ * float(streamline::GeneratedFrames() + 1),
                streamline::GeneratedFrames() + 1);
  if (info.upscaler_on)
    ImGui::Text("Rendu %ux%u -> sortie %ux%u", info.render_width, info.render_height,
                info.output_width, info.output_height);
  else
    ImGui::Text("Rendu %ux%u (MSAA, sans upscaler)", info.output_width, info.output_height);
  ImGui::Text("Ecran %ux%u, resolution scale %u", info.backbuffer_width, info.backbuffer_height,
              info.resolution_scale);

  // Local change (pgr4 project): the game's shaders or ours (shaders\*.ps.hlsl,
  // shader_replace.h), with the cars' surroundings cube at the game's size or at the resolution
  // scale (TextureCache::CopyScaledCube), to compare them live.
  {
    static const char* const kLooks[] = {"Original", "Original + reflets HD", "Nos shaders",
                                         "Nos shaders + reflets HD"};
    int look = (REXCVAR_GET(pgr4_native_shader_replace) ? 2 : 0) +
               (REXCVAR_GET(pgr4_native_scaled_cube) ? 1 : 0);
    if (ImGui::Combo("Rendu des voitures", &look, kLooks, 4)) {
      REXCVAR_SET(pgr4_native_shader_replace, look >= 2);
      REXCVAR_SET(pgr4_native_scaled_cube, (look & 1) != 0);
    }
  }

  // Local change (pgr4 project): the ambient occlusion over the scene (scene_effects.h).
  ImGui::SeparatorText("Occlusion ambiante (GTAO)");
  {
    bool gtao = REXCVAR_GET(pgr4_native_gtao);
    if (ImGui::Checkbox("GTAO", &gtao)) REXCVAR_SET(pgr4_native_gtao, gtao);
    if (gtao) {
      float radius = float(REXCVAR_GET(pgr4_native_gtao_radius));
      if (ImGui::SliderFloat("Rayon (m)", &radius, 0.1f, 5.0f, "%.2f"))
        REXCVAR_SET(pgr4_native_gtao_radius, double(radius));
      float intensity = float(REXCVAR_GET(pgr4_native_gtao_intensity));
      if (ImGui::SliderFloat("Intensite", &intensity, 0.0f, 4.0f, "%.2f"))
        REXCVAR_SET(pgr4_native_gtao_intensity, double(intensity));
      float distance = float(REXCVAR_GET(pgr4_native_gtao_distance));
      if (ImGui::SliderFloat("Distance (m)", &distance, 10.0f, 2000.0f, "%.0f"))
        REXCVAR_SET(pgr4_native_gtao_distance, double(distance));
      bool full = REXCVAR_GET(pgr4_native_gtao_full);
      if (ImGui::Checkbox("Pleine resolution (plus stable, plus cher)", &full))
        REXCVAR_SET(pgr4_native_gtao_full, full);
      bool debug = REXCVAR_GET(pgr4_native_gtao_debug);
      if (ImGui::Checkbox("Voir l'occlusion seule", &debug)) REXCVAR_SET(pgr4_native_gtao_debug, debug);
    }
  }

  ImGui::SeparatorText("Upscaler");
  {
    // Local change (pgr4 project): the sharpening after DLSS / FSR (Upscaler::Sharpen).
    // One strength per upscaler; none with MSAA.
    float dlss_sharpen = float(REXCVAR_GET(pgr4_native_sharpen_dlss));
    if (ImGui::SliderFloat("Nettete DLSS (CAS)", &dlss_sharpen, 0.0f, 1.0f, "%.2f"))
      REXCVAR_SET(pgr4_native_sharpen_dlss, double(dlss_sharpen));
    float fsr_sharpen = float(REXCVAR_GET(pgr4_native_sharpen_fsr));
    if (ImGui::SliderFloat("Nettete FSR (CAS)", &fsr_sharpen, 0.0f, 1.0f, "%.2f"))
      REXCVAR_SET(pgr4_native_sharpen_fsr, double(fsr_sharpen));
  }
  bool upscaler = info.upscaler_on;
  if (ImGui::Checkbox("Upscaler actif (F7)", &upscaler)) actions.switch_upscaler = true;
  if (info.dlss_available) {
    int backend = REXCVAR_GET(pgr4_native_dlss) ? 0 : 1;
    const int before = backend;
    ImGui::RadioButton("DLSS", &backend, 0);
    ImGui::SameLine();
    ImGui::RadioButton("FSR", &backend, 1);
    if (backend != before) {
      REXCVAR_SET(pgr4_native_dlss, backend == 0);
      actions.recreate_upscaler = true;
    }
  } else {
    ImGui::TextDisabled("DLSS indisponible (Streamline non charge) : FSR");
  }
  if (info.upscaler_on) ImGui::Text("En cours : %s", info.dlss ? "DLSS" : "FSR");
  // Local change (pgr4 project): the reactive mask of the transparents (Upscaler::SnapshotOpaque).
  // The mask keeps the transparents (rain, smoke, glass) out of the upscaler's history.
  bool reactive = REXCVAR_GET(pgr4_native_reactive_mask);
  if (ImGui::Checkbox("Masque des transparents (pluie, fumee, vitres)", &reactive))
    REXCVAR_SET(pgr4_native_reactive_mask, reactive);
  if (reactive) {
    bool dlss_mask = REXCVAR_GET(pgr4_native_reactive_dlss);
    if (ImGui::Checkbox("Aussi avec DLSS", &dlss_mask))
      REXCVAR_SET(pgr4_native_reactive_dlss, dlss_mask);
    float scale = float(REXCVAR_GET(pgr4_native_reactive_scale));
    if (ImGui::SliderFloat("Force du masque", &scale, 0.25f, 8.0f, "%.2f"))
      REXCVAR_SET(pgr4_native_reactive_scale, double(scale));
    float most = float(REXCVAR_GET(pgr4_native_reactive_max));
    if (ImGui::SliderFloat("Masque maximum", &most, 0.05f, 1.0f, "%.2f"))
      REXCVAR_SET(pgr4_native_reactive_max, double(most));
    bool show = REXCVAR_GET(pgr4_native_reactive_show);
    if (ImGui::Checkbox("Voir le masque (rouge)", &show))
      REXCVAR_SET(pgr4_native_reactive_show, show);
    // The mask's settings back to their defaults (upscaler.cpp's).
    if (ImGui::Button("Masque : valeurs par defaut")) {
      REXCVAR_SET(pgr4_native_reactive_dlss, true);
      REXCVAR_SET(pgr4_native_reactive_scale, 2.0);
      REXCVAR_SET(pgr4_native_reactive_max, 0.8);
      REXCVAR_SET(pgr4_native_reactive_show, false);
    }
  }
  const bool dlss_chosen = info.dlss_available && REXCVAR_GET(pgr4_native_dlss);
  std::string mode = REXCVAR_GET(pgr4_native_dlss_mode);
  if (dlss_chosen) {
    static const char* const kModes[] = {"auto",     "dlaa",        "quality",
                                         "balanced", "performance", "ultra_performance"};
    static const char* const kModeLabels[] = {
        "Auto (taille de rendu ci-dessous)", "DLAA (100 %)",     "Qualite (67 %)",
        "Equilibre (58 %)",                  "Performance (50 %)", "Ultra performance (33 %)"};
    if (StringCombo("Mode DLSS", mode, kModes, kModeLabels, 6)) {
      REXCVAR_SET(pgr4_native_dlss_mode, mode);
      actions.recreate_upscaler = true;
    }
    std::string preset = REXCVAR_GET(pgr4_native_dlss_preset);
    static const char* const kPresets[] = {"E", "default", "J", "K", "L", "M"};
    static const char* const kPresetLabels[] = {"E (CNN, route nette)", "Defaut NVIDIA (K en DLAA)",
                                                "J", "K (transformer)", "L (ultra perf.)",
                                                "M (performance)"};
    if (StringCombo("Preset DLSS", preset, kPresets, kPresetLabels, 6))
      REXCVAR_SET(pgr4_native_dlss_preset, preset);
  }
  if (!dlss_chosen || mode == "auto" || mode.empty()) {
    if (!upscaler_width_) {
      upscaler_width_ = REXCVAR_GET(pgr4_native_upscaler_width);
      upscaler_height_ = REXCVAR_GET(pgr4_native_upscaler_height);
    }
    const int max_w = int(info.output_width), max_h = int(info.output_height);
    ImGui::SliderInt("Largeur de rendu", &upscaler_width_, 320, std::max(320, max_w));
    const bool w_done = ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SliderInt("Hauteur de rendu", &upscaler_height_, 180, std::max(180, max_h));
    if (w_done || ImGui::IsItemDeactivatedAfterEdit()) {
      REXCVAR_SET(pgr4_native_upscaler_width, upscaler_width_);
      REXCVAR_SET(pgr4_native_upscaler_height, upscaler_height_);
      actions.recreate_upscaler = true;
    }
  }

  ImGui::SeparatorText("DLSS Frame Generation");
  if (!info.fg_available) {
    ImGui::TextDisabled("Indisponible : pgr4_native_dlss_fg = true au lancement");
  } else {
    bool fg = REXCVAR_GET(pgr4_native_dlss_fg);
    if (ImGui::Checkbox("DLSS-G actif", &fg)) REXCVAR_SET(pgr4_native_dlss_fg, fg);
    // Local change: Multi Frame Generation's multiplier (an RTX 50's, or an RTX 40 unlocked,
    // mfg_unlock.h).
    if (const uint32_t max = std::min(5u, streamline::MaxGeneratedFrames()); max > 1) {
      static const int kFrames[] = {1, 2, 3, 4, 5};
      static const char* const kFrameLabels[] = {"x2", "x3", "x4", "x5", "x6"};
      int32_t frames = int32_t(streamline::GeneratedFrames());
      if (IntCombo("Multiplicateur", frames, kFrames, kFrameLabels, int(max)))
        REXCVAR_SET(pgr4_native_dlss_fg_frames, frames);
    }
    if (!dlss_chosen) ImGui::TextDisabled("(seulement avec DLSS comme upscaler)");
    // No 0: saved in the toml, it left frame generation with no motion at all (heavy ghosting; seen in another
    // project, 14/11/2026).
    static const int kFactors[] = {1, -1};
    static const char* const kFactorLabels[] = {"1", "-1 (PGR4)"};
    int32_t fx = REXCVAR_GET(pgr4_native_dlss_fg_mvec_x),
            fy = REXCVAR_GET(pgr4_native_dlss_fg_mvec_y);
    if (IntCombo("Vecteurs X", fx, kFactors, kFactorLabels, 2))
      REXCVAR_SET(pgr4_native_dlss_fg_mvec_x, fx);
    if (IntCombo("Vecteurs Y", fy, kFactors, kFactorLabels, 2))
      REXCVAR_SET(pgr4_native_dlss_fg_mvec_y, fy);
  }

  ImGui::SeparatorText("Rendu");
  // Trial (14/11/2026): the bikes' riders drew another frame's pose now and then; unchecked, the pages the game
  // rewrites again and again are watched like the others, every write a fault, as the Xenos plugin does.
  {
    bool xenos = !REXCVAR_GET(pgr4_native_watch_hot);
    if (ImGui::Checkbox("Methode Xenos pour les pages rapides (pilotes des motos)", &xenos))
      REXCVAR_SET(pgr4_native_watch_hot, !xenos);
  }
  // The draw resolution scale by the definition it renders the game's 720p at.
  static const int kScales[] = {1, 2, 3, 4};
  static const char* const kScaleLabels[] = {"720p", "1440p", "2160p (4K)", "2880p (5K)"};
  if (IntCombo("Definition", resolution_scale_, kScales, kScaleLabels, 4) &&
      resolution_scale_ != int(info.resolution_scale)) {
    REXCVAR_SET(pgr4_native_resolution_scale, resolution_scale_);
    actions.rebuild_pipeline = true;
  }
  static const int kMsaa[] = {1, 2, 4, 8};
  static const char* const kMsaaLabels[] = {"1x", "2x (jeu)", "4x", "8x"};
  if (IntCombo("MSAA (sans upscaler)", msaa_, kMsaa, kMsaaLabels, 4)) {
    REXCVAR_SET(pgr4_native_msaa, msaa_);
    actions.apply_msaa = true;
  }
  static const int kAniso[] = {1, 2, 4, 8, 16};
  static const char* const kAnisoLabels[] = {"Jeu", "2x", "4x", "8x", "16x"};
  if (IntCombo("Anisotropie", anisotropy_, kAniso, kAnisoLabels, 5)) {
    REXCVAR_SET(pgr4_native_anisotropy, anisotropy_);
    actions.rebuild_pipeline = true;
  }
  ImGui::TextDisabled("Scale et anisotropie : pipeline refait (pause d'un instant)");

    ImGui::EndTabItem();
  }
  if (tabs && ImGui::BeginTabItem("Tests")) {
    ImGui::SeparatorText("Textures HD");
    bool hd = REXCVAR_GET(pgr4_native_texture_hd);
    if (ImGui::Checkbox("Textures HD", &hd)) REXCVAR_SET(pgr4_native_texture_hd, hd);
    bool decal_off = REXCVAR_GET(pgr4_native_decal_light_off);
    if (ImGui::Checkbox("Sans lumiere sur les marquages au sol (moins de vibration)", &decal_off))
      REXCVAR_SET(pgr4_native_decal_light_off, decal_off);
    bool enhanced = REXCVAR_GET(pgr4_native_enhanced_reflections);
    if (ImGui::Checkbox("Reflets ameliores (voitures, eau, route mouillee)", &enhanced))
      REXCVAR_SET(pgr4_native_enhanced_reflections, enhanced);
    if (ImGui::Button("Recharger les textures HD"))
      REXCVAR_SET(pgr4_native_texture_hd_reload, true);
    ImGui::TextDisabled("Relit le dossier textures_hd : fichiers nouveaux ou modifies, sans relancer.");
    float coplanar = float(REXCVAR_GET(pgr4_native_coplanar_bias));
    if (ImGui::SliderFloat("Decalage des surfaces au sol (test)", &coplanar, 0.0f, 4096.0f, "%.0f",
                           ImGuiSliderFlags_Logarithmic))
      REXCVAR_SET(pgr4_native_coplanar_bias, double(coplanar));
    bool a2c = REXCVAR_GET(pgr4_native_alpha_to_coverage);
    if (ImGui::Checkbox("Bords lisses des decoupes (MSAA)", &a2c))
      REXCVAR_SET(pgr4_native_alpha_to_coverage, a2c);
    float crowd3d = float(REXCVAR_GET(pgr4_crowd_3d_radius));
    if (ImGui::SliderFloat("Rayon des spectateurs en 3D (2,5 : jeu ; au prochain circuit)", &crowd3d, 1.0f, 10.0f, "%.1f"))
      REXCVAR_SET(pgr4_crowd_3d_radius, double(crowd3d));
    bool car_shadow = REXCVAR_GET(pgr4_car_shadow_fix);
    if (ImGui::Checkbox("Ombre projetee des voitures (decoche : jeu)", &car_shadow))
      REXCVAR_SET(pgr4_car_shadow_fix, car_shadow);
    float shake = float(REXCVAR_GET(pgr4_camera_shake_scale));
    if (ImGui::SliderFloat("Secousse de la camera (1 : jeu)", &shake, 0.0f, 2.0f, "%.2f"))
      REXCVAR_SET(pgr4_camera_shake_scale, double(shake));
    float soften = float(REXCVAR_GET(pgr4_native_msaa_texture_soften));
    if (ImGui::SliderFloat("Douceur des textures (MSAA)", &soften, 0.0f, 2.0f, "%.2f"))
      REXCVAR_SET(pgr4_native_msaa_texture_soften, double(soften));
    float lod = float(REXCVAR_GET(pgr4_native_upscaler_lod_offset));
    if (ImGui::SliderFloat("LOD des textures DLSS/FSR (- net, + doux, 0 : recommande)", &lod,
                           -1.0f, 2.0f, "%.2f"))
      REXCVAR_SET(pgr4_native_upscaler_lod_offset, double(lod));

    ImGui::SeparatorText("SHADERS (appliques au relachement du curseur)");
    // A tuning applied once its slider is let go (each change recompiles the shaders concerned:
    // dragged, it queued a compilation at every step): the value being dragged held in ImGui's
    // state meanwhile.
    auto tuning = [](const char* label, double& value, float lo, float hi, const char* format) {
      ImGuiStorage* state = ImGui::GetStateStorage();
      const ImGuiID held = ImGui::GetID(label) + 1;
      float v = state->GetBool(held + 1) ? state->GetFloat(held) : float(value);
      ImGui::SliderFloat(label, &v, lo, hi, format);
      const bool active = ImGui::IsItemActive();
      state->SetBool(held + 1, active);
      state->SetFloat(held, v);
      if (ImGui::IsItemDeactivatedAfterEdit()) value = double(v);
    };
    ImGui::SeparatorText("Image : hautes lumieres");
    tuning("Blancs adoucis (0 : jeu d'origine)", FLAGS_pgr4_native_tune_highlights_storage_(), 0.0f,
           1.0f, "%.2f");
    tuning("Halo lumineux / bloom (1 : jeu d'origine)", FLAGS_pgr4_native_tune_bloom_storage_(), 0.0f,
           2.0f, "%.2f");
    ImGui::SeparatorText("Route : relief");
    tuning("Relief de la route", FLAGS_pgr4_native_tune_road_relief_storage_(), 0.0f, 2.0f, "%.2f");
    ImGui::SeparatorText("Route mouillee : pluie forte / orage");
    tuning("Force du reflet##forte", FLAGS_pgr4_native_tune_road_reflect_storage_(), 0.0f, 1.5f,
           "%.2f");
    tuning("Flou du reflet (0 net, 3 flou)##forte", FLAGS_pgr4_native_tune_road_blur_storage_(),
           0.0f, 3.0f, "%.2f");
    tuning("Reflet vu de haut##forte", FLAGS_pgr4_native_tune_road_fresnel_storage_(), 0.0f,
           0.2f, "%.3f");
    tuning("Continuite du film d'eau##forte", FLAGS_pgr4_native_tune_road_grain_storage_(),
           0.0f, 1.0f, "%.2f");
    tuning("Flaques (zones plus ou moins mouillees)##forte",
           FLAGS_pgr4_native_tune_road_puddles_storage_(), 0.0f, 1.0f, "%.2f");
    tuning("Brillance##forte", FLAGS_pgr4_native_tune_road_sheen_storage_(), 0.0f, 1.0f,
           "%.2f");
    ImGui::SeparatorText("Route mouillee : pluie fine");
    tuning("Force du reflet##fine", FLAGS_pgr4_native_tune_road_reflect_light_storage_(), 0.0f, 1.5f,
           "%.2f");
    tuning("Flou du reflet (0 net, 3 flou)##fine", FLAGS_pgr4_native_tune_road_blur_light_storage_(),
           0.0f, 3.0f, "%.2f");
    tuning("Reflet vu de haut##fine", FLAGS_pgr4_native_tune_road_fresnel_light_storage_(), 0.0f,
           0.2f, "%.3f");
    tuning("Continuite du film d'eau##fine", FLAGS_pgr4_native_tune_road_grain_light_storage_(),
           0.0f, 1.0f, "%.2f");
    tuning("Flaques (zones plus ou moins mouillees)##fine",
           FLAGS_pgr4_native_tune_road_puddles_light_storage_(), 0.0f, 1.0f, "%.2f");
    tuning("Brillance##fine", FLAGS_pgr4_native_tune_road_sheen_light_storage_(), 0.0f, 1.0f,
           "%.2f");
    {
      bool frozen = REXCVAR_GET(pgr4_freeze_car);
      if (ImGui::Checkbox("Geler la position de la voiture", &frozen))
        REXCVAR_SET(pgr4_freeze_car, frozen);
    }
    ImGui::SeparatorText("Voitures : peinture");
    tuning("Reflet du soleil", FLAGS_pgr4_native_tune_sun_glint_storage_(), 0.0f, 1.0f, "%.2f");
    tuning("Taille du reflet (grand = petit)", FLAGS_pgr4_native_tune_sun_sharpness_storage_(),
           50.0f, 3000.0f, "%.0f");
    tuning("Reflet sur les bords", FLAGS_pgr4_native_tune_rim_storage_(), 0.0f, 1.0f, "%.2f");
    ImGui::SeparatorText("Voitures : vitres");
    tuning("Reflet des vitres", FLAGS_pgr4_native_tune_glass_reflect_storage_(), 0.0f, 1.5f,
           "%.2f");
    tuning("Ciel reflete sur les vitres", FLAGS_pgr4_native_tune_glass_sky_storage_(), 0.0f, 1.0f,
           "%.2f");
    tuning("Hauteur ou commence le ciel", FLAGS_pgr4_native_tune_glass_sky_from_storage_(), -0.5f,
           0.8f, "%.2f");
    ImGui::SeparatorText("Voitures : gouttes de pluie");
    tuning("Gouttes plus petites (carrosserie)",
           FLAGS_pgr4_native_tune_drops_paint_scale_storage_(), 0.5f, 4.0f, "%.2f");
    tuning("Vitres : vitesse des gouttes", FLAGS_pgr4_native_drops_glide_rate_storage_(), 0.0f,
           0.04f, "%.4f");
    tuning("Vitres : ecoulement a l'arret", FLAGS_pgr4_native_drops_glide_idle_storage_(), 0.0f,
           0.2f, "%.3f");
    tuning("Vitres : direction des gouttes", FLAGS_pgr4_native_tune_drops_glide_angle_storage_(),
           -180.0f, 180.0f, "%.0f");
    tuning("Vitres : etirement des gouttes", FLAGS_pgr4_native_tune_drops_stretch_storage_(), 0.0f,
           3.0f, "%.2f");
    tuning("Carrosserie : force des gouttes qui coulent",
           FLAGS_pgr4_native_tune_drops_paint_strength_storage_(), 0.0f, 6.0f, "%.2f");
    tuning("Vitres : distance sans glissement", FLAGS_pgr4_native_tune_drops_near_storage_(), 0.0f,
           6.0f, "%.1f");
    ImGui::SeparatorText("Voitures : salete (a chaud)");
    float dirt_force = float(REXCVAR_GET(pgr4_native_dirt_force));
    if (ImGui::SliderFloat("Quantite (-1 : celle du jeu)", &dirt_force, -1.0f, 4.0f, "%.2f"))
      REXCVAR_SET(pgr4_native_dirt_force, double(dirt_force));
    float dirt_fine = float(REXCVAR_GET(pgr4_native_dirt_fine));
    if (ImGui::SliderFloat("Grain fin du givre", &dirt_fine, 0.0f, 2.0f, "%.2f"))
      REXCVAR_SET(pgr4_native_dirt_fine, double(dirt_fine));
    ImGui::SeparatorText("Voitures : givre (temps glacial)");
    tuning("Givre plus petit (carrosserie)", FLAGS_pgr4_native_tune_frost_paint_scale_storage_(),
           0.5f, 4.0f, "%.2f");
    ImGui::SeparatorText("Voitures : rayures (degats)");
    tuning("Rayures plus petites (carrosserie)",
           FLAGS_pgr4_native_tune_scratch_paint_scale_storage_(), 0.5f, 4.0f, "%.2f");
    ImGui::SeparatorText("Eau : vagues et reflets");
    tuning("Force des vagues (0 : jeu d'origine)", FLAGS_pgr4_native_tune_wave_strength_storage_(),
           0.0f, 1.5f, "%.2f");
    tuning("Vagues plus creusees", FLAGS_pgr4_native_tune_wave_steepness_storage_(), 0.02f,
           0.4f, "%.3f");
    tuning("Part de clapot irregulier", FLAGS_pgr4_native_tune_wave_noise_storage_(), 0.0f, 1.0f,
           "%.2f");
    tuning("Taille des vagues (1 : 0,4 a 7 m)", FLAGS_pgr4_native_tune_wave_size_storage_(), 0.1f,
           2.0f, "%.2f");
    tuning("Vitesse des vagues", FLAGS_pgr4_native_tune_wave_speed_storage_(), 0.0f, 3.0f,
           "%.2f");
    tuning("Direction du vent (radians)", FLAGS_pgr4_native_tune_wind_angle_storage_(), -3.14f,
           3.14f, "%.2f");
    tuning("Reflet du decor (0 : ciel seul)", FLAGS_pgr4_native_tune_water_reflect_storage_(),
           0.0f, 1.0f, "%.2f");
    ImGui::SeparatorText("Eau du port (garage)");
    tuning("Transparence (0 : couleur de l'eau, 1 : jeu)",
           FLAGS_pgr4_native_tune_harbour_clarity_storage_(), 0.0f, 1.0f, "%.2f");
    tuning("Deformation par les vagues (1 : jeu)",
           FLAGS_pgr4_native_tune_harbour_distort_storage_(), 0.0f, 1.0f, "%.2f");
    tuning("Force des vagues du port", FLAGS_pgr4_native_tune_harbour_waves_storage_(), 0.0f,
           2.0f, "%.2f");
    ImGui::TextDisabled("Applique au relachement du curseur : recompile les shaders concernes (une seconde environ).");
    ImGui::EndTabItem();
  }
  // Local change (pgr4 project), development: the game's settings for the draw distance (tunables.h).
  if (tabs && TunablesEnabled() && ImGui::BeginTabItem("Draw distance")) {
    TunablesTab();
    ImGui::EndTabItem();
  }
  if (tabs) ImGui::EndTabBar();
  ImGui::Separator();
  if (ImGui::Button("Enregistrer dans pgr4.toml")) {
    save_status_ = SaveRendererSettings(info.upscaler_on);
    REXLOG_INFO("PGR4 native overlay: {}", save_status_);
  }
  if (!save_status_.empty()) ImGui::TextWrapped("%s", save_status_.c_str());
  ImGui::TextDisabled("F2 : masquer");
  ImGui::End();
  return actions;
}

}  // namespace pgr4::native
