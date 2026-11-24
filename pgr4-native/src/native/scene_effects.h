// pgr4-native PGR4 native renderer, local addition (pgr4 project): effects over the game's 3D
// scene, applied to its surface once all of it is drawn, before its first resolve of the frame
// (EndTiling's depth resolve, Resolver::Resolve), so before the game's own post-processing
// (motion blur, bloom), the upscaler and the HUD.
//
// Ground-truth ambient occlusion (GTAO, Jimenez et al. 2016; --pgr4_native_gtao): from the
// scene's depth (inverted, the frame's camera from NativeRenderer::FindCamera), copied once at
// half the scene's size, at that size: each pixel's view-space position and normal, the horizons on both sides along two
// screen directions, and the cosine-weighted visibility between them integrated in closed form;
// blurred (depth aware), then multiplied into the scene's color (a full-screen draw into its
// target, any MSAA, the occlusion brought up to the full size by depth-aware weights).
// Or, local addition (16/11/2026), Intel's XeGTAO in place of that occlusion under an upscaler
// (--pgr4_native_ao_method, automatic by default): see the members below.
//
// Threading: the thread that records the frames (NativeRenderer::Execute*).
#pragma once

#include "fidelityfx.h"
#include "render_targets.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <map>
#include <utility>

namespace pgr4::native {

class SceneEffects {
 public:
  bool Initialize(ID3D12Device* device);
  // The frame's camera (NativeRenderer::camera_), read at Apply.
  void set_camera(const FfxCamera* camera) { camera_ = camera; }
  // Local change (21/11/2026, the volumetric fog laid by the shaders, mode 2): this frame's volume
  // (Volumetrics::Share, resolve.cpp), on when the shaders laid it: the occlusion then darkens the
  // part of each pixel that fog leaves of its surface, not the game's (pgr4_native_gtao_fog_volume).
  struct FogVolume {
    bool on = false;
    ID3D12Resource* volume = nullptr;  // Share::volume (R16G16B16A16_FLOAT, columns x rows x 64)
    float range = 0.0f, ln2_growth = 0.0f, first_edge = 0.0f;  // Share::shader 0, 1, 7
    float columns[2] = {};                                      // Share::shader 2, 3
    float fog[3] = {};  // the fog's colour as the scene has it (c10 x c12.y, Share::shader 8-10)
  };
  void set_fog_volume(const FogVolume& f) { fog_volume_ = f; }
  // Whether any effect is on: the renderer then looks for the frame's camera.
  static bool Enabled();

  // Local addition (16/11/2026): XeGTAO's tuning, the general one (its cvars) or the race's
  // weather's: --pgr4_native_xegtao_<weather>, "key=value ..." over the general (keys: the
  // fields' names), empty for none. Tazz's tuning in fog by default.
  struct XeTuning {
    float radius, intensity, radius_multiplier, falloff, sample_power, thin_compensation,
        final_power, mip_offset;
  };
  static constexpr int kXeWeathers = 7;  // g_pgr4_weather_kind's 0 (clear) to 6 (heavy snow)
  // The race's weather (draw.cpp's g_pgr4_weather_kind), -1 out of a race or unknown.
  static int XeWeather();
  static const char* XeWeatherName(int weather);  // the cvar's suffix: clear, fog, ...
  static XeTuning XeGeneralTuning();
  static void XeSetGeneral(const XeTuning& t);
  static XeTuning XeTuningFor(int weather);  // the general one with the weather's overrides
  static bool XeHasOwn(int weather);
  static void XeSetOwn(int weather, const XeTuning* t);  // nullptr: none (the general)
  // At a resolve out of the scene surface: the first of the frame applies the effects to its
  // color target (`frame` the frame's number). Sets its own pipelines, root signatures,
  // descriptor heap and render target on the list.
  // `timer`: --pgr4_native_gpu_timing, each step its own span.
  // `defer`: a multisampled scene that no upscaler reads (the resolver's copies are its only
  // readers): the occlusion is brought up to the scene's size into an image of its own instead,
  // which the scene's color resolves of the frame multiply in (DeferredOcclusion), as they average
  // the samples anyway; the full-screen draw would read and write every sample of the target.
  void Apply(ID3D12GraphicsCommandList* list, RenderTargets& targets, const SurfaceRegs& regs,
             uint64_t frame, class GpuTimer* timer = nullptr, bool defer = false);
  // This frame's occlusion at the scene's host size (R8_UNORM, readable by compute shaders) when
  // Apply deferred it, else null.
  ID3D12Resource* DeferredOcclusion(uint64_t frame) const {
    return deferred_frame_ == frame ? full_.Get() : nullptr;
  }

 private:
  template <typename T>
  using ComPtr = Microsoft::WRL::ComPtr<T>;

  bool EnsureTargets(uint32_t width, uint32_t height);
  ID3D12PipelineState* ApplyPipeline(DXGI_FORMAT format, uint32_t samples);
  void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES& state,
                  D3D12_RESOURCE_STATES after);

  ID3D12Device* device_ = nullptr;
  const FfxCamera* camera_ = nullptr;
  FogVolume fog_volume_;
  bool ready_ = false;
  uint64_t applied_frame_ = UINT64_MAX;
  ComPtr<ID3D12RootSignature> compute_signature_, apply_signature_;
  ComPtr<ID3D12PipelineState> down_pso_, down_ms_pso_, ao_pso_, ao_ms_pso_, blur_pso_;
  bool EnsureFull(uint32_t width, uint32_t height);
  std::map<std::pair<DXGI_FORMAT, uint32_t>, ComPtr<ID3D12PipelineState>> apply_psos_;
  ComPtr<ID3DBlob> apply_vs_, apply_ps_, apply_ps_ms_;  // the pixel shader without, with MSAA
  ComPtr<ID3DBlob> apply_ps_debug_, apply_ps_ms_debug_;  // the debug view's (writes everywhere)
  ComPtr<ID3D12DescriptorHeap> heap_;
  uint32_t stride_ = 0;
  // Half the size of the scene's host target: the raw and blurred occlusion (R8_UNORM), and the
  // scene's depth (R32_FLOAT, as it is: inverted).
  ComPtr<ID3D12Resource> raw_, ao_, half_depth_;
  D3D12_RESOURCE_STATES raw_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        ao_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        half_depth_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  uint32_t width_ = 0, height_ = 0;  // the half size
  // The deferred occlusion at the scene's host size, and the frame it is for.
  ComPtr<ID3D12Resource> full_;
  D3D12_RESOURCE_STATES full_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  uint32_t full_width_ = 0, full_height_ = 0;
  uint64_t deferred_frame_ = UINT64_MAX;
  ComPtr<ID3D12PipelineState> upsample_ms_pso_;

  // Local addition (16/11/2026): Intel's XeGTAO, the ambient occlusion under an upscaler
  // (--pgr4_native_ao_method; thirdparty/XeGTAO, MIT). From the same view depth (the DOWN pass, at
  // the scene's full size), its depth MIP chain (5 levels, XeGTAO_PrefilterDepths16x16's), its
  // main pass (XeGTAO_MainPass, normals from the depth, Hilbert/R2 noise changing every frame
  // under an upscaler, which accumulates it) and its edge-aware denoise, then ours (the game's
  // fog and the distance, at most half the light taken) into raw_, which the apply draw and the
  // deferred path read as they read our GTAO's.
  bool EnsureXeTargets(uint32_t width, uint32_t height);
  bool xe_ready_ = false;
  ComPtr<ID3D12RootSignature> xe_signature_;
  ComPtr<ID3D12PipelineState> xe_prefilter_pso_, xe_denoise_pso_, xe_denoise_last_pso_,
      xe_finish_pso_, xe_finish_ms_pso_;
  ComPtr<ID3D12PipelineState> xe_main_pso_[4];  // per quality level (--pgr4_native_xegtao_quality)
  ComPtr<ID3D12Resource> xe_depth_;  // view depth (R32_FLOAT) and its 4 MIPs
  ComPtr<ID3D12Resource> xe_ao_[2];  // the working and final occlusion (R8_UINT)
  ComPtr<ID3D12Resource> xe_edges_;  // the denoise's edges (R8_UNORM)
  D3D12_RESOURCE_STATES xe_depth_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        xe_ao_state_[2] = {D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS},
                        xe_edges_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  uint32_t xe_width_ = 0, xe_height_ = 0;
};

}  // namespace pgr4::native
