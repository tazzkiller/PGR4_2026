// pgr4-native PGR4 native renderer, local addition (pgr4 project): the scene as it is before its
// water, for the water's reflections of the scenery (shaders/water_hooks.hlsli: rays marched over
// the screen, the sky's cube map where they find nothing).
//
// At the frame's first water draw (DrawRenderer: the water's pixel shaders), the scene surface's
// color and depth are copied at half its size: each texel the average color of 2x2 pixels (sample
// 0 of each, any MSAA) and the nearest of their depths (inverted: the largest). The water's
// replacement shaders read both through two fixed entries of the draws' bindless table
// (draw.cpp: kSceneColor, kSceneDepth) and the size in the system constants (pgr4_scene).
//
// Threading: the thread that records the frames (DrawRenderer).
#pragma once

#include "render_targets.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <utility>
#include <vector>

namespace pgr4::native {

class SceneCopy {
 public:
  // The color copy's levels: half the scene's size, then each half the one above (the wet roads'
  // blurred reflections read a smaller one).
  static constexpr uint32_t kColorLevels = 4;
  // With set_accumulate: the color level the wet roads read, kept as a running average over the
  // frames (accum()): the rain's drops, new in each frame, became noise in the reflections even
  // with the camera still. Each frame mixed in by kAccumWeight (no reprojection: the blurred
  // reflection follows the camera a few frames late).
  static constexpr uint32_t kAccumLevel = 2;
  static constexpr float kAccumWeight = 0.3f;
  void set_accumulate(bool on) { accumulate_ = on; }
  ID3D12Resource* accum() const { return accum_.Get(); }

  bool Initialize(ID3D12Device* device);
  // Copies the scene surface `regs` (its color 0 and its depth) once a frame; returns true when it
  // recorded the copy (with its own root signature, pipeline and descriptor heap: the caller binds
  // its draw state again).
  bool Take(ID3D12GraphicsCommandList* list, RenderTargets& targets, const SurfaceRegs& regs,
            uint32_t height, uint64_t frame);
  // Whether this frame's copy exists (Take recorded it).
  // Local change (25/11/2026): the copy of the draws' view (volumetrics.h Pgr4ViewKey).
  bool taken(uint64_t frame) const;
  ID3D12Resource* color() const { return color_.Get(); }
  ID3D12Resource* depth() const { return depth_.Get(); }
  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  // Bumped when the copies are made again (their views must be too).
  uint32_t generation() const { return generation_; }
  // Local change (19/11/2026): the depth copy's texel (x, y), R32_FLOAT, into the first 4 bytes of
  // `buffer` (a readback buffer of 256 bytes at least), after this frame's Take: the photo mode's
  // focus (draw.cpp OnSceneEnd).
  void CopyDepthTexel(ID3D12GraphicsCommandList* list, ID3D12Resource* buffer, uint32_t x,
                      uint32_t y);

 private:
  template <typename T>
  using ComPtr = Microsoft::WRL::ComPtr<T>;

  bool EnsureTargets(uint32_t width, uint32_t height, uint64_t frame);

  ID3D12Device* device_ = nullptr;
  ComPtr<ID3D12RootSignature> signature_;
  ComPtr<ID3D12PipelineState> pso_, pso_ms_, level_pso_, accum_pso_;
  ComPtr<ID3D12DescriptorHeap> heap_;
  uint32_t stride_ = 0;
  ComPtr<ID3D12Resource> color_, depth_;  // R16G16B16A16_FLOAT, R32_FLOAT, half size
  ComPtr<ID3D12Resource> accum_;          // kAccumLevel's size, R16G16B16A16_FLOAT
  D3D12_RESOURCE_STATES accum_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  bool accumulate_ = false, accum_fresh_ = true;
  // Their allocated size: they only grow (a smaller scene uses their top left), and the ones they
  // replace are kept until the frames that may read them are done (with the frame they left).
  uint32_t allocated_width_ = 0, allocated_height_ = 0;
  std::vector<std::pair<uint64_t, ComPtr<ID3D12Resource>>> retired_;
  D3D12_RESOURCE_STATES color_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        depth_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  uint32_t width_ = 0, height_ = 0;
  uint32_t generation_ = 0;
  uint64_t taken_frame_ = UINT64_MAX;  // Pgr4ViewKey (25/11/2026)
};

}  // namespace pgr4::native
