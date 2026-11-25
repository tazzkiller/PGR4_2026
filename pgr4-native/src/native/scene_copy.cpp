// pgr4-native PGR4 native renderer: the scene before its water (scene_copy.h).
#include "scene_copy.h"

#include "d3d12_device.h"
#include "volumetrics.h"  // local change (25/11/2026): Pgr4ViewKey, the split screen's views

#include <rex/cvar.h>
#include <rex/logging.h>

#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>

// Local change (19/11/2026, Tazz: « regarde autour des motos y'a comme un flou bizarre »): the wet
// roads' running average (kAccumLevel, no reprojection) kept what moved on screen for a few frames:
// a smeared ghost of the bikes around them, mirrored by the road. Before each mix, the average is
// clamped to the range (min, max) of the current level's 3x3 texels around it: where a bike went
// away, its old colours fall out of the range and are dropped; a rain drop of this frame is in its
// texels' range with the road behind it, so the average still smooths the drops out. Read at each
// frame.
REXCVAR_DEFINE_BOOL(pgr4_native_road_average_clamp, true, "PGR4 native",
                    "Wet roads: the reflections' running average clamped to the current image "
                    "(no ghost of the moving vehicles)");

namespace pgr4::native {

namespace {

// Per frame in flight: t0 color, t1 depth, u0 color copy, u1 depth copy; then for each smaller
// level of the color copy, its source level and itself (t0, u0, two unused).
constexpr uint32_t kViewsPerSlot = 4 + 4 * (SceneCopy::kColorLevels - 1) + 4;  // + the average's

struct CopyConstants {
  uint32_t size[2];       // the scene's host pixels
  uint32_t half_size[2];  // the copies'
};

constexpr char kCopyHlsl[] = R"(
cbuffer constants : register(b0) { uint2 size; uint2 half_size; };
#ifdef MS
Texture2DMS<float4> color_tex : register(t0);
Texture2DMS<float> depth_tex : register(t1);
float4 Color(int2 p) { return color_tex.Load(p, 0); }
float Depth(int2 p) { return depth_tex.Load(p, 0); }
#else
Texture2D<float4> color_tex : register(t0);
Texture2D<float> depth_tex : register(t1);
float4 Color(int2 p) { return color_tex.Load(int3(p, 0)); }
float Depth(int2 p) { return depth_tex.Load(int3(p, 0)); }
#endif
RWTexture2D<float4> color_copy : register(u0);
RWTexture2D<float> depth_copy : register(u1);
[numthreads(8, 8, 1)]
void main(uint2 p : SV_DispatchThreadID) {
  if (any(p >= half_size)) return;
  const int2 q = int2(p) * 2, last = int2(size) - 1;
  const int2 a = min(q, last), b = min(q + int2(1, 0), last);
  const int2 c = min(q + int2(0, 1), last), d = min(q + int2(1, 1), last);
  color_copy[p] = (Color(a) + Color(b) + Color(c) + Color(d)) * 0.25;
  // Inverted depth: the nearest of the four is the largest.
  depth_copy[p] = max(max(Depth(a), Depth(b)), max(Depth(c), Depth(d)));
}
)";

// The running average of a level (set_accumulate): weight asfloat(size.x), half_size its size;
// size.y not 0: the average clamped to the current level's 3x3 texels first
// (pgr4_native_road_average_clamp).
constexpr char kAccumHlsl[] = R"(
cbuffer constants : register(b0) { uint2 size; uint2 half_size; };
Texture2D<float4> level : register(t0);
RWTexture2D<float4> average : register(u0);
[numthreads(8, 8, 1)]
void main(uint2 p : SV_DispatchThreadID) {
  if (any(p >= half_size)) return;
  const float4 now = level.Load(int3(p, 0));
  float4 before = average[p];
  if (size.y != 0u) {
    const int2 last = int2(half_size) - 1;
    float4 lo = now, hi = now;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x) {
      const float4 c = level.Load(int3(clamp(int2(p) + int2(x, y), 0, last), 0));
      lo = min(lo, c);
      hi = max(hi, c);
    }
    before = clamp(before, lo, hi);
  }
  average[p] = lerp(before, now, asfloat(size.x));
}
)";

// A smaller level of the color copy: the average of 2x2 texels of the level above.
constexpr char kLevelHlsl[] = R"(
cbuffer constants : register(b0) { uint2 size; uint2 half_size; };
Texture2D<float4> above : register(t0);
RWTexture2D<float4> level : register(u0);
[numthreads(8, 8, 1)]
void main(uint2 p : SV_DispatchThreadID) {
  if (any(p >= half_size)) return;
  const int2 q = int2(p) * 2, last = int2(size) - 1;
  level[p] = (above.Load(int3(min(q, last), 0)) + above.Load(int3(min(q + int2(1, 0), last), 0)) +
              above.Load(int3(min(q + int2(0, 1), last), 0)) +
              above.Load(int3(min(q + int2(1, 1), last), 0))) * 0.25;
}
)";

}  // namespace

bool SceneCopy::Initialize(ID3D12Device* device) {
  device_ = device;
  const pD3DCompile compile = D3DCompiler();
  if (!compile) return false;
  D3D12_DESCRIPTOR_RANGE ranges[2] = {
      {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0},
      {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 2},
  };
  D3D12_ROOT_PARAMETER params[2] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable = {2, ranges};
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants = {0, 0, sizeof(CopyConstants) / 4};
  const D3D12_ROOT_SIGNATURE_DESC rs = {2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ComPtr<ID3DBlob> blob, error;
  if (Failed(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error),
             "scene copy root signature serialize") ||
      Failed(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                         IID_PPV_ARGS(&signature_)),
             "scene copy root signature"))
    return false;
  auto pipeline = [&](const char* name, bool ms, ComPtr<ID3D12PipelineState>& out) {
    const D3D_SHADER_MACRO macros[] = {{ms ? "MS" : "SINGLE", "1"}, {nullptr, nullptr}};
    ComPtr<ID3DBlob> code, err;
    if (FAILED(compile(kCopyHlsl, sizeof(kCopyHlsl) - 1, name, macros, nullptr, "main", "cs_5_0",
                       D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err))) {
      REXLOG_ERROR("PGR4 native scene copy: {}: {}", name,
                   err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
      return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC d = {};
    d.pRootSignature = signature_.Get();
    d.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    return !Failed(device->CreateComputePipelineState(&d, IID_PPV_ARGS(&out)), name);
  };
  if (!pipeline("pgr4_scene_copy.hlsl", false, pso_) ||
      !pipeline("pgr4_scene_copy_ms.hlsl", true, pso_ms_))
    return false;
  {
    ComPtr<ID3DBlob> code, err;
    if (FAILED(compile(kLevelHlsl, sizeof(kLevelHlsl) - 1, "pgr4_scene_copy_level.hlsl", nullptr,
                       nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err))) {
      REXLOG_ERROR("PGR4 native scene copy: levels: {}",
                   err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
      return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC d = {};
    d.pRootSignature = signature_.Get();
    d.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    if (Failed(device->CreateComputePipelineState(&d, IID_PPV_ARGS(&level_pso_)),
               "scene copy levels"))
      return false;
    code.Reset();
    err.Reset();
    if (FAILED(compile(kAccumHlsl, sizeof(kAccumHlsl) - 1, "pgr4_scene_copy_accum.hlsl", nullptr,
                       nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err))) {
      REXLOG_ERROR("PGR4 native scene copy: average: {}",
                   err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
      return false;
    }
    d.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    if (Failed(device->CreateComputePipelineState(&d, IID_PPV_ARGS(&accum_pso_)),
               "scene copy average"))
      return false;
  }
  D3D12_DESCRIPTOR_HEAP_DESC hd = {};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  hd.NumDescriptors = kViewsPerSlot * D3D12Device::kFramesInFlight;
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (Failed(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_)), "scene copy heap"))
    return false;
  stride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  return true;
}

bool SceneCopy::taken(uint64_t frame) const { return taken_frame_ == Pgr4ViewKey(frame); }

bool SceneCopy::EnsureTargets(uint32_t width, uint32_t height, uint64_t frame) {
  // The frames in flight are done with the copies they read.
  std::erase_if(retired_, [&](const auto& r) {
    return r.first + D3D12Device::kFramesInFlight + 1 <= frame;
  });
  if (color_ && width <= allocated_width_ && height <= allocated_height_) {
    width_ = width;
    height_ = height;
    return true;
  }
  if (color_) retired_.emplace_back(frame, std::move(color_));
  if (depth_) retired_.emplace_back(frame, std::move(depth_));
  if (accum_) retired_.emplace_back(frame, std::move(accum_));
  width = std::max(width, allocated_width_);
  height = std::max(height, allocated_height_);
  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC d = {};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width = width;
  d.Height = height;
  d.DepthOrArraySize = 1;
  d.MipLevels = kColorLevels;
  d.SampleDesc.Count = 1;
  d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                              IID_PPV_ARGS(&color_)),
             "scene copy color"))
    return false;
  d.Format = DXGI_FORMAT_R32_FLOAT;
  d.MipLevels = 1;
  if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                              IID_PPV_ARGS(&depth_)),
             "scene copy depth")) {
    color_.Reset();
    return false;
  }
  if (accumulate_) {
    d.Width = std::max<uint64_t>(1, width >> kAccumLevel);
    d.Height = std::max(1u, height >> kAccumLevel);
    d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                IID_PPV_ARGS(&accum_)),
               "scene copy average")) {
      color_.Reset();
      depth_.Reset();
      return false;
    }
    accum_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    accum_fresh_ = true;
  }
  color_state_ = depth_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  allocated_width_ = width_ = width;
  allocated_height_ = height_ = height;
  ++generation_;
  REXLOG_INFO("PGR4 native scene copy: {}x{} for the water's reflections", width, height);
  return true;
}

bool SceneCopy::Take(ID3D12GraphicsCommandList* list, RenderTargets& targets,
                     const SurfaceRegs& regs, uint32_t height, uint64_t frame) {
  // Local change (25/11/2026, issue 29, split screen): once a view of the frame (Pgr4ViewKey: the
  // frame alone with one view), player 2's reflections from its own scene, not player 1's; over a
  // view's rows (g_pgr4_split_rows, 360: the rays leaving the view's bottom found nothing there,
  // not the target's rows no view draws).
  const uint64_t key = Pgr4ViewKey(frame);
  if (!signature_ || taken_frame_ == key) return false;
  // Local change (26/11/2026, split screen only): player 2's view's descriptors on a heap of its
  // own, made at its first copy; one view (the solo) on the heap of before.
  ID3D12DescriptorHeap* heap = heap_.Get();
  if (g_pgr4_scene_view == 1) {
    if (!heap_view1_) {
      D3D12_DESCRIPTOR_HEAP_DESC hd = {};
      hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
      hd.NumDescriptors = kViewsPerSlot * D3D12Device::kFramesInFlight;
      hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
      if (Failed(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_view1_)), "scene copy heap"))
        return false;
    }
    heap = heap_view1_.Get();
  }
  if (g_pgr4_split_rows) height = std::min(height, g_pgr4_split_rows);
  RenderTarget* color = targets.GetColor(regs, 0, height);
  RenderTarget* depth = targets.GetDepth(regs, height);
  if (!color || !depth || color->samples != depth->samples || !(color->scale == depth->scale))
    return false;
  // Local change (pgr4 project): only the scene's rows (`height`), not the rest of its target's
  // EDRAM range (1024 rows for the scene's 720: 42 % of the copy, and of its levels, unseen).
  const uint32_t w = color->scale.Extent(color->width, 0),
                 h = color->scale.Extent(std::min(color->height, height), 1);
  const uint32_t hw = (w + 1) / 2, hh = (h + 1) / 2;
  if (!w || !h || !EnsureTargets(hw, hh, frame)) return false;
  taken_frame_ = key;
  const D3D12_RECT all = {0, 0, LONG(color->width), LONG(color->height)};
  targets.PrepareRead(list, color, all);
  targets.PrepareRead(list, depth, all);
  const bool ms = color->samples > 1;

  const uint32_t first = Pgr4ViewSlot(frame, D3D12Device::kFramesInFlight) * kViewsPerSlot;
  D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap->GetCPUDescriptorHandleForHeapStart();
  cpu.ptr += size_t(first) * stride_;
  D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap->GetGPUDescriptorHandleForHeapStart();
  gpu.ptr += uint64_t(first) * stride_;
  auto at = [&](uint32_t i) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = cpu;
    h.ptr += size_t(i) * stride_;
    return h;
  };
  D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  if (ms) {
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
  } else {
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
  }
  srv.Format = color->srv_format;
  device_->CreateShaderResourceView(color->resource.Get(), &srv, at(0));
  srv.Format = depth->srv_format;
  device_->CreateShaderResourceView(depth->resource.Get(), &srv, at(1));
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
  uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  device_->CreateUnorderedAccessView(color_.Get(), nullptr, &uav, at(2));
  uav.Format = DXGI_FORMAT_R32_FLOAT;
  device_->CreateUnorderedAccessView(depth_.Get(), nullptr, &uav, at(3));

  RenderTargets::Transition(list, color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  RenderTargets::Transition(list, depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  D3D12_RESOURCE_BARRIER bs[2];
  uint32_t nb = 0;
  if (color_state_ != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    bs[nb++] = TransitionBarrier(color_.Get(), color_state_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (depth_state_ != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    bs[nb++] = TransitionBarrier(depth_.Get(), depth_state_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (nb) list->ResourceBarrier(nb, bs);
  const CopyConstants c = {{w, h}, {hw, hh}};
  ID3D12DescriptorHeap* heaps[] = {heap};
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(signature_.Get());
  list->SetPipelineState(ms ? pso_ms_.Get() : pso_.Get());
  list->SetComputeRootDescriptorTable(0, gpu);
  list->SetComputeRoot32BitConstants(1, sizeof(c) / 4, &c, 0);
  list->Dispatch((hw + 7) / 8, (hh + 7) / 8, 1);
  // The color copy's smaller levels, each from the one above (read once written).
  auto level_barrier = [&](uint32_t level, D3D12_RESOURCE_STATES before,
                           D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b = TransitionBarrier(color_.Get(), before, after);
    b.Transition.Subresource = level;
    return b;
  };
  list->SetPipelineState(level_pso_.Get());
  uint32_t lw = hw, lh = hh;
  for (uint32_t level = 1; level < kColorLevels; ++level) {
    const uint32_t nw = std::max(1u, lw / 2), nh = std::max(1u, lh / 2);
    const uint32_t base = 4 + 4 * (level - 1);
    D3D12_SHADER_RESOURCE_VIEW_DESC above = {};
    above.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    above.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    above.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    above.Texture2D.MostDetailedMip = level - 1;
    above.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(color_.Get(), &above, at(base));
    device_->CreateShaderResourceView(color_.Get(), &above, at(base + 1));  // unused t1
    D3D12_UNORDERED_ACCESS_VIEW_DESC out = {};
    out.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    out.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    out.Texture2D.MipSlice = level;
    device_->CreateUnorderedAccessView(color_.Get(), nullptr, &out, at(base + 2));
    device_->CreateUnorderedAccessView(color_.Get(), nullptr, &out, at(base + 3));  // unused u1
    const D3D12_RESOURCE_BARRIER b =
        level_barrier(level - 1, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    list->ResourceBarrier(1, &b);
    D3D12_GPU_DESCRIPTOR_HANDLE g = gpu;
    g.ptr += uint64_t(base) * stride_;
    list->SetComputeRootDescriptorTable(0, g);
    const CopyConstants lc = {{lw, lh}, {nw, nh}};
    list->SetComputeRoot32BitConstants(1, sizeof(lc) / 4, &lc, 0);
    list->Dispatch((nw + 7) / 8, (nh + 7) / 8, 1);
    lw = nw;
    lh = nh;
  }
  // The running average of kAccumLevel (its level read as written above: NON_PIXEL_SHADER_RESOURCE,
  // but for the last level, still an unordered access view).
  if (accumulate_ && accum_) {
    static_assert(kAccumLevel + 1 < kColorLevels);
    const uint32_t base = 4 + 4 * (kColorLevels - 1);
    D3D12_SHADER_RESOURCE_VIEW_DESC lv = {};
    lv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    lv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    lv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    lv.Texture2D.MostDetailedMip = kAccumLevel;
    lv.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(color_.Get(), &lv, at(base));
    device_->CreateShaderResourceView(color_.Get(), &lv, at(base + 1));  // unused t1
    D3D12_UNORDERED_ACCESS_VIEW_DESC av = {};
    av.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    av.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device_->CreateUnorderedAccessView(accum_.Get(), nullptr, &av, at(base + 2));
    device_->CreateUnorderedAccessView(accum_.Get(), nullptr, &av, at(base + 3));  // unused u1
    if (accum_state_ != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
      const D3D12_RESOURCE_BARRIER b = TransitionBarrier(
          accum_.Get(), accum_state_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      list->ResourceBarrier(1, &b);
    }
    const uint32_t aw = std::max(1u, hw >> kAccumLevel), ah = std::max(1u, hh >> kAccumLevel);
    const float weight = accum_fresh_ ? 1.0f : kAccumWeight;
    uint32_t weight_bits;
    std::memcpy(&weight_bits, &weight, 4);
    const CopyConstants ac = {{weight_bits, REXCVAR_GET(pgr4_native_road_average_clamp) ? 1u : 0u},
                              {aw, ah}};
    list->SetPipelineState(accum_pso_.Get());
    D3D12_GPU_DESCRIPTOR_HANDLE g = gpu;
    g.ptr += uint64_t(base) * stride_;
    list->SetComputeRootDescriptorTable(0, g);
    list->SetComputeRoot32BitConstants(1, sizeof(ac) / 4, &ac, 0);
    list->Dispatch((aw + 7) / 8, (ah + 7) / 8, 1);
    const D3D12_RESOURCE_BARRIER b = TransitionBarrier(
        accum_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    list->ResourceBarrier(1, &b);
    accum_state_ = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    accum_fresh_ = false;
  }
  // Read by the water's and the roads' pixel shaders from here on.
  const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  D3D12_RESOURCE_BARRIER rb[kColorLevels + 1];
  uint32_t n = 0;
  for (uint32_t level = 0; level + 1 < kColorLevels; ++level)
    rb[n++] = level_barrier(level, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, read);
  rb[n++] = level_barrier(kColorLevels - 1, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, read);
  rb[n++] = TransitionBarrier(depth_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, read);
  list->ResourceBarrier(n, rb);
  color_state_ = depth_state_ = read;
  return true;
}

void SceneCopy::CopyDepthTexel(ID3D12GraphicsCommandList* list, ID3D12Resource* buffer,
                               uint32_t x, uint32_t y) {
  if (!depth_ || !buffer || x >= width_ || y >= height_) return;
  D3D12_RESOURCE_BARRIER b =
      TransitionBarrier(depth_.Get(), depth_state_, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->ResourceBarrier(1, &b);
  D3D12_TEXTURE_COPY_LOCATION dst = {};
  dst.pResource = buffer;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint.Offset = 0;
  dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R32_FLOAT, 1, 1, 1,
                                   D3D12_TEXTURE_DATA_PITCH_ALIGNMENT};
  D3D12_TEXTURE_COPY_LOCATION src = {};
  src.pResource = depth_.Get();
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src.SubresourceIndex = 0;
  const D3D12_BOX box = {x, y, 0, x + 1, y + 1, 1};
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
  b = TransitionBarrier(depth_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, depth_state_);
  list->ResourceBarrier(1, &b);
}

}  // namespace pgr4::native
