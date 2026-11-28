// Local change (pgr4 project): the HUD-less image for DLSS Frame Generation (streamline.h), as
// frame_generation.h makes it for FidelityFX's: the front buffer's resolve (the last resolve
// before the previous Swap) copied early (Resolver::ResolveCopy), at the frame's first blended draw
// into its surface after an opaque one (the final composite, then the HUD), and drawn by the
// Presenter into an image of the back buffers' size and format as it draws the front buffer
// (SwapCopy). DLSS-G takes the HUD from where the back buffer and it differ.
//
// Threading: the thread that records the frames (NativeRenderer::Execute*).
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <optional>

#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>

#include "d3d12_device.h"
#include "log.h"
#include "resolve.h"

namespace pgr4::native {

class HudlessCapture {
 public:
  HudlessCapture(D3D12Device& device, Resolver& resolver) : device_(device), resolver_(resolver) {}

  void OnResolve(const ResolveParams& p) { last_resolve_ = p; }
  // `front_base`: the Swap's front buffer (its guest physical address).
  void OnSwap(uint32_t front_base) {
    if (last_resolve_.source < 4 && last_resolve_.dest_base == front_base)
      front_ = last_resolve_;
    else
      front_.reset();
  }
  // Before each draw of frame `frame`: when the draw is the HUD's first, records the copy into
  // `list` and returns true (the list's compute state changed).
  // Local change (28/11/2026, `last_composite`: pgr4_native_hudless_last_composite): the copy taken
  // again after each later opaque composite of the frame, so that the one after the last (the
  // tone map, 6B06B8CD, before the HUD) is given, not the one after the scene's first composite
  // (27A38989: no car shadows, flares, rain nor tone map; draw log 26835 of 19/11/2026). The same
  // image each time (Resolver's own destination), written over in the list's order.
  bool BeforeDraw(ID3D12GraphicsCommandList* list, uint64_t frame, const uint32_t* regs,
                  bool last_composite) {
    if (!front_ || (copy_frame_ == frame && !last_composite)) [[likely]]
      return false;
    using enum rex::graphics::Register;
    const SurfaceRegs& f = front_->regs;
    if (regs[XE_GPU_REG_RB_SURFACE_INFO] != f.surface_info ||
        ColorStorage(regs[XE_GPU_REG_RB_COLOR_INFO]) != ColorStorage(f.color_info[front_->source]) ||
        !(regs[XE_GPU_REG_RB_COLOR_MASK] & 0x7))
      return false;
    // RB_BLENDCONTROL0's color part: source ONE, ADD, destination ZERO replaces what was there.
    if ((regs[XE_GPU_REG_RB_BLENDCONTROL0] & 0x1FFF) == 0x0001) {
      composited_frame_ = frame;
      copied_since_composite_ = false;
      return false;
    }
    if (composited_frame_ != frame) return false;
    if (copy_frame_ == frame && copied_since_composite_) return false;
    copy_frame_ = frame;
    copied_since_composite_ = true;
    copy_ = resolver_.ResolveCopy(list, *front_);
    return true;
  }
  // Frame `frame`'s copy, nullptr without.
  const ResolvedImage* source(uint64_t frame) const {
    return copy_frame_ == frame ? copy_ : nullptr;
  }
  // Local change (28/11/2026, pgr4_native_fg_light_mask): a draw into the front buffer's surface,
  // and the front buffer's resolve (nullptr without).
  bool FrontDraw(const uint32_t* regs) const {
    using enum rex::graphics::Register;
    if (!front_) return false;
    const SurfaceRegs& f = front_->regs;
    return regs[XE_GPU_REG_RB_SURFACE_INFO] == f.surface_info &&
           ColorStorage(regs[XE_GPU_REG_RB_COLOR_INFO]) == ColorStorage(f.color_info[front_->source]);
  }
  const ResolveParams* front() const { return front_ ? &*front_ : nullptr; }

  // The image (the back buffers' size and format, PRESENT outside the Presenter's draw), made
  // again when they change; false (logged) on failure.
  bool Ensure() {
    const uint32_t w = device_.width(), h = device_.height();
    const bool hdr = device_.hdr();
    if (image_ && w == width_ && h == height_ && hdr == hdr_) return true;
    if (image_) device_.WaitIdle();
    image_.Reset();
    ID3D12Device* device = device_.device();
    if (!rtv_heap_) {
      D3D12_DESCRIPTOR_HEAP_DESC hd = {};
      hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
      hd.NumDescriptors = 1;
      if (Failed(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap_)),
                 "CreateDescriptorHeap(DLSS-G HUD-less RTV)"))
        return false;
    }
    const D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC rd = {D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                              0,
                              w,
                              h,
                              1,
                              1,
                              hdr ? D3D12Device::kHdrBackBufferFormat
                                  : D3D12Device::kBackBufferFormat,
                              {1, 0}};
    // Local change (28/11/2026, pgr4_native_fg_light_mask): also written by LightMaskPass, from
    // the back buffer's copy (shown_).
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (Failed(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd,
                                               D3D12_RESOURCE_STATE_PRESENT, nullptr,
                                               IID_PPV_ARGS(&image_)),
               "CreateCommittedResource(DLSS-G HUD-less image)"))
      return false;
    device->CreateRenderTargetView(image_.Get(), nullptr, rtv());
    shown_.Reset();
    D3D12_RESOURCE_DESC sd = rd;
    sd.Flags = D3D12_RESOURCE_FLAG_NONE;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &sd,
                                               D3D12_RESOURCE_STATE_PRESENT, nullptr,
                                               IID_PPV_ARGS(&shown_))))
      shown_.Reset();
    width_ = w;
    height_ = h;
    hdr_ = hdr;
    return true;
  }
  ID3D12Resource* image() const { return image_.Get(); }
  ID3D12Resource* shown() const { return shown_.Get(); }  // the back buffer's copy (local change)
  D3D12_CPU_DESCRIPTOR_HANDLE rtv() const { return rtv_heap_->GetCPUDescriptorHandleForHeapStart(); }

 private:
  // A color target's EDRAM data: its base and storage format (frame_generation.cpp's).
  static uint32_t ColorStorage(uint32_t color_info) {
    namespace xenos = rex::graphics::xenos;
    const auto format = xenos::ColorRenderTargetFormat((color_info >> 16) & 0xF);
    return (color_info & 0xFFF) | uint32_t(xenos::GetStorageColorFormat(format)) << 16;
  }

  D3D12Device& device_;
  Resolver& resolver_;
  Microsoft::WRL::ComPtr<ID3D12Resource> image_;
  Microsoft::WRL::ComPtr<ID3D12Resource> shown_;  // local change (pgr4_native_fg_light_mask)
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv_heap_;
  uint32_t width_ = 0, height_ = 0;
  bool hdr_ = false;
  ResolveParams last_resolve_;
  std::optional<ResolveParams> front_;
  uint64_t composited_frame_ = UINT64_MAX;
  uint64_t copy_frame_ = UINT64_MAX;
  bool copied_since_composite_ = false;  // the copy taken after the frame's latest composite
  const ResolvedImage* copy_ = nullptr;
};

}  // namespace pgr4::native
