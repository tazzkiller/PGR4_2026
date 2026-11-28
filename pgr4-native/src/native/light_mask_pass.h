// Local change (28/11/2026, pgr4_native_fg_light_mask; Tazz: the v1.65 frame generation kept, only
// the volumetric lights' flashes to fix): the HUD-less image the frame generation gets, made unlike
// the frame's image where the volumetric pass added light (Volumetrics::LightMaskToRead: the lamps,
// and the sun-lit air with pgr4_native_fg_light_mask_sun) and where the game drew its light flares
// (draw.h FlareLayerToRead: the street lamps' and the vehicles' headlights).
//
// With v1.65's HUD-less copy (taken before the tone map), FidelityFX and DLSS-G take the frame's
// own image wherever the copy and the image differ, almost everywhere, and the generated frames are
// clean. Where the volumetric light lies on the road they partly trusted the copy, brighter there,
// and the generated frames showed it (Tazz's video of 14:05: single frames 12 to 17 % brighter on the
// road, the same orange). Under the mask the copy is set to the image shifted by half its range
// (frac(F + 0.5)): far from it, so the generation takes the frame's image there too.
//
// Threading: the thread that records the frames.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>

#include "d3d12_device.h"

namespace pgr4::native {

class LightMaskPass {
 public:
  explicit LightMaskPass(D3D12Device& device) : device_(device) {}
  bool Initialize();
  // Records the pass into `list`: `hudless` (UAV-capable, PRESENT in and out) changed under the
  // mask from `image` (PRESENT in and out); the mask (the scene's visible size) in
  // NON_PIXEL_SHADER_RESOURCE, mapped onto `rect` (left, top, width, height) of the back buffer.
  // `slot` a frame in flight's.
  // Local change (28/11/2026): `flares` (or null), the light flares' layer (draw.h FlareLayerToRead,
  // the front buffer's target pixels): `to_flares` its pixels per back buffer pixel in `rect`,
  // `flare_origin` its pixel of the image's corner; `mask` may be null with it.
  void Apply(ID3D12GraphicsCommandList* list, uint32_t slot, ID3D12Resource* image,
             ID3D12Resource* hudless, ID3D12Resource* mask, uint32_t mask_w, uint32_t mask_h,
             const int32_t rect[4], float threshold, ID3D12Resource* flares,
             const float to_flares[2], const int32_t flare_origin[2], float flare_threshold,
             bool use_sun, ID3D12Resource* hud = nullptr, float hud_threshold = 0.0f,
             ID3D12Resource* mirrors = nullptr, uint32_t mirrors_w = 0, uint32_t mirrors_h = 0);
  // Local change (28/11/2026, pgr4_native_fg_mirror_mask): `mirrors` (or null), the wing mirrors
  // still visible at the end of the scene (MirrorMask, mirrors_w x mirrors_h, the scene's render
  // pixels mapped onto `rect` as the mask).
  // MirrorCompare, at the upscaler's scene depth (Upscaler::set_scene_depth_hook): the mirror
  // layer (draw.h MirrorLayerToRead) against the scene's final depth (both NON_PIXEL_SHADER_RESOURCE,
  // the same pixels) over `width` x `height`; frame `frame`'s MirrorMask then (R8, 1 where the
  // mirror's depth is the scene's). MirrorMask: frame `frame`'s, NON_PIXEL_SHADER_RESOURCE, or null.
  void MirrorCompare(ID3D12GraphicsCommandList* list, uint32_t slot, ID3D12Resource* scene_depth,
                     DXGI_FORMAT scene_format, ID3D12Resource* mirror_depth,
                     DXGI_FORMAT mirror_format, bool reversed, uint32_t width, uint32_t height,
                     uint64_t frame);
  ID3D12Resource* MirrorMask(uint64_t frame, uint32_t* w, uint32_t* h) const {
    if (mirror_mask_frame_ != frame || !mirror_mask_) return nullptr;
    *w = mirror_mask_w_;
    *h = mirror_mask_h_;
    return mirror_mask_.Get();
  }
  // Local change (28/11/2026, pgr4_native_fg_hud_mask): `hud` (or null), the HUD's coverage
  // (draw.h HudLayerToRead, its alpha; the flare layer's pixels: to_flares, flare_origin).

 private:
  D3D12Device& device_;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> signature_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
  uint32_t stride_ = 0;
  static constexpr uint32_t kViews = 6;  // image, mask, flares, HUD, mirrors, the HUD-less image
  // MirrorCompare's.
  Microsoft::WRL::ComPtr<ID3D12RootSignature> compare_signature_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> compare_pso_;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> compare_heap_;
  Microsoft::WRL::ComPtr<ID3D12Resource> mirror_mask_, mirror_mask_old_[2];
  uint32_t mirror_mask_w_ = 0, mirror_mask_h_ = 0;
  uint64_t mirror_mask_frame_ = UINT64_MAX;
  static constexpr uint32_t kCompareViews = 3;  // the scene's depth, the mirrors', the mask
};

}  // namespace pgr4::native
