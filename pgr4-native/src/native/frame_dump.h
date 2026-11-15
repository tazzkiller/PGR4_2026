// Local diagnostic (pgr4 project): GPU textures read back to files beside the executable, written
// once the GPU has passed their copies. The upscaler arms it with its motion dump
// (--pgr4_native_motion_dump): the scene's color then, and the back buffer of the next swaps, to see
// which scene each presented image shows.
//
// Threading: the thread that records the frames.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdio>
#include <string>
#include <vector>

#include "d3d12_device.h"

namespace pgr4::native::frame_dump {

struct Pending {
  Microsoft::WRL::ComPtr<ID3D12Resource> readback;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout = {};
  uint32_t height = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  std::wstring name;
  uint64_t tick = 0;
};

inline std::vector<Pending> pending;
inline uint64_t tick = 0;      // Poll's calls
inline uint64_t armed_id = 0;  // the dump the next back buffers belong to
inline int armed_swaps = 0;    // back buffers still to capture for it

// Records into `list` the copy of `res` (subresource 0, in `state`, left in it) to `name`.bin.
inline void Capture(ID3D12Device* device, ID3D12GraphicsCommandList* list, ID3D12Resource* res,
                    D3D12_RESOURCE_STATES state, std::wstring name) {
  const D3D12_RESOURCE_DESC d = res->GetDesc();
  Pending p;
  UINT rows = 0;
  UINT64 row_bytes = 0, total = 0;
  device->GetCopyableFootprints(&d, 0, 1, 0, &p.layout, &rows, &row_bytes, &total);
  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC b = {};
  b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  b.Width = total;
  b.Height = b.DepthOrArraySize = b.MipLevels = 1;
  b.SampleDesc.Count = 1;
  b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &b,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&p.readback))))
    return;
  D3D12_RESOURCE_BARRIER t = {};
  t.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  t.Transition.pResource = res;
  t.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  t.Transition.StateBefore = state;
  t.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) list->ResourceBarrier(1, &t);
  D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
  src.pResource = res;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dst.pResource = p.readback.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint = p.layout;
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  std::swap(t.Transition.StateBefore, t.Transition.StateAfter);
  if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) list->ResourceBarrier(1, &t);
  p.height = d.Height;
  p.format = d.Format;
  p.name = std::move(name);
  p.tick = tick;
  pending.push_back(std::move(p));
}

// Once a frame: writes the captures the GPU has passed (header: width, height, row pitch, DXGI
// format; then the rows).
inline void Poll() {
  ++tick;
  for (auto it = pending.begin(); it != pending.end();) {
    if (tick < it->tick + 2 * D3D12Device::kFramesInFlight + 2) {
      ++it;
      continue;
    }
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path(exe);
    path = path.substr(0, path.find_last_of(L"\\/") + 1) + it->name + L".bin";
    if (std::FILE* f = _wfopen(path.c_str(), L"wb")) {
      const uint32_t header[4] = {it->layout.Footprint.Width, it->height,
                                  it->layout.Footprint.RowPitch, uint32_t(it->format)};
      std::fwrite(header, sizeof(header), 1, f);
      const size_t bytes = size_t(it->layout.Footprint.RowPitch) * it->height;
      void* p = nullptr;
      const D3D12_RANGE range = {0, bytes};
      if (SUCCEEDED(it->readback->Map(0, &range, &p))) {
        std::fwrite(p, bytes, 1, f);
        const D3D12_RANGE none = {0, 0};
        it->readback->Unmap(0, &none);
      }
      std::fclose(f);
    }
    it = pending.erase(it);
  }
}

}  // namespace pgr4::native::frame_dump
