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
  // Whether any effect is on: the renderer then looks for the frame's camera.
  static bool Enabled();
  // At a resolve out of the scene surface: the first of the frame applies the effects to its
  // color target (`frame` the frame's number). Sets its own pipelines, root signatures,
  // descriptor heap and render target on the list.
  void Apply(ID3D12GraphicsCommandList* list, RenderTargets& targets, const SurfaceRegs& regs,
             uint64_t frame);

 private:
  template <typename T>
  using ComPtr = Microsoft::WRL::ComPtr<T>;

  bool EnsureTargets(uint32_t width, uint32_t height);
  ID3D12PipelineState* ApplyPipeline(DXGI_FORMAT format, uint32_t samples);
  void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES& state,
                  D3D12_RESOURCE_STATES after);

  ID3D12Device* device_ = nullptr;
  const FfxCamera* camera_ = nullptr;
  bool ready_ = false;
  uint64_t applied_frame_ = UINT64_MAX;
  ComPtr<ID3D12RootSignature> compute_signature_, apply_signature_;
  ComPtr<ID3D12PipelineState> down_pso_, down_ms_pso_, ao_pso_, blur_pso_;
  std::map<std::pair<DXGI_FORMAT, uint32_t>, ComPtr<ID3D12PipelineState>> apply_psos_;
  ComPtr<ID3DBlob> apply_vs_, apply_ps_, apply_ps_ms_;  // the pixel shader without, with MSAA
  ComPtr<ID3D12DescriptorHeap> heap_;
  uint32_t stride_ = 0;
  // Half the size of the scene's host target: the raw and blurred occlusion (R8_UNORM), and the
  // scene's depth (R32_FLOAT, as it is: inverted).
  ComPtr<ID3D12Resource> raw_, ao_, half_depth_;
  D3D12_RESOURCE_STATES raw_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        ao_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        half_depth_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  uint32_t width_ = 0, height_ = 0;  // the half size
};

}  // namespace pgr4::native
