// Local change (28/11/2026): the HUD-less image made unlike the frame's image under the volumetric
// light (light_mask_pass.h).
#include "light_mask_pass.h"

#include "log.h"

#include <d3dcompiler.h>
#include <rex/logging.h>

#include <utility>

namespace pgr4::native {

namespace {

constexpr char kLightMaskHlsl[] = R"(
Texture2D<float4> image_f : register(t0);   // the frame's image
Texture2D<float2> mask : register(t1);      // the light the volumetric pass added (luminance):
                                            // x the lamps', y the sun-lit air's
Texture2D<float> flares : register(t2);     // the light flares' red, added (the front buffer's)
Texture2D<float4> hud : register(t3);       // the HUD's coverage, its alpha (the front buffer's)
Texture2D<float> mirrors : register(t4);    // the wing mirrors still visible (the scene's pixels)
RWTexture2D<float4> hudless : register(u0); // the HUD-less image given to the frame generation
cbuffer Constants : register(b0) {
  int4 rect;          // the image in the back buffer: left, top, width, height
  float2 to_mask;     // mask pixels per back buffer pixel
  int2 mask_max;      // the mask's last pixel
  float threshold;    // the light at which the pixel is the frame's own
  uint has_mask;
  uint has_flares;
  float flare_threshold;
  float2 to_flares;   // flare layer pixels per back buffer pixel
  int2 flare_origin;  // its pixel of the image's corner
  int2 flare_max;     // its last pixel
  uint use_sun;       // the sun-lit air's too (pgr4_native_fg_light_mask_sun)
  uint has_hud;
  float hud_threshold;
  uint pad;
  int2 hud_max;       // the HUD layer's last pixel
  float2 to_mirrors;  // mirror mask pixels per back buffer pixel
  int2 mirrors_max;   // its last pixel
  uint has_mirrors;
  uint3 pad2;
};
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= uint(rect.z) || id.y >= uint(rect.w)) return;
  bool lit = false;
  if (has_mask != 0) {
    const int2 m = clamp(int2((float2(id.xy) + 0.5) * to_mask), int2(0, 0), mask_max);
    const float2 l = mask.Load(int3(m, 0));
    lit = l.x > threshold || (use_sun != 0 && l.y > threshold);
  }
  if (!lit && has_flares != 0) {
    const int2 m = clamp(flare_origin + int2((float2(id.xy) + 0.5) * to_flares), int2(0, 0), flare_max);
    lit = flares.Load(int3(m, 0)) > flare_threshold;
  }
  if (!lit && has_hud != 0) {
    const int2 m = clamp(flare_origin + int2((float2(id.xy) + 0.5) * to_flares), int2(0, 0), hud_max);
    lit = hud.Load(int3(m, 0)).a > hud_threshold;
  }
  if (!lit && has_mirrors != 0) {
    const int2 m = clamp(int2((float2(id.xy) + 0.5) * to_mirrors), int2(0, 0), mirrors_max);
    lit = mirrors.Load(int3(m, 0)) > 0.5;
  }
  if (!lit) return;
  const int2 p = int2(id.xy) + rect.xy;
  const float3 f = image_f.Load(int3(p, 0)).rgb;
  hudless[p] = float4(frac(f + 0.5), 1.0);
}
)";

struct Constants {
  int32_t rect[4];
  float to_mask[2];
  int32_t mask_max[2];
  float threshold;
  uint32_t has_mask, has_flares;
  float flare_threshold;
  float to_flares[2];
  int32_t flare_origin[2];
  int32_t flare_max[2];
  uint32_t use_sun, has_hud;
  float hud_threshold;
  uint32_t pad;
  int32_t hud_max[2];
  float to_mirrors[2];
  int32_t mirrors_max[2];
  uint32_t has_mirrors, pad2[3];
};
static_assert(sizeof(Constants) == 128);

// Local change (28/11/2026, pgr4_native_fg_mirror_mask): a wing mirror's pixel still visible at
// the end of the scene: the scene's final depth is the mirror's own (the same draw, the same
// format), within a relative 1e-4 (a car in front of it is far from that).
constexpr char kMirrorCompareHlsl[] = R"(
Texture2D<float> scene_depth : register(t0);
Texture2D<float> mirror_depth : register(t1);
RWTexture2D<float> mirror_mask : register(u0);
cbuffer Constants : register(b0) {
  uint2 size;
  float far_depth;
  uint pad;
};
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= size.x || id.y >= size.y) return;
  const float m = mirror_depth.Load(int3(id.xy, 0));
  float visible = 0.0;
  if (m != far_depth) {
    const float s = scene_depth.Load(int3(id.xy, 0));
    visible = abs(s - m) <= 1e-4 * max(abs(m), 1e-6) ? 1.0 : 0.0;
  }
  mirror_mask[id.xy] = visible;
}
)";

struct CompareConstants {
  uint32_t size[2];
  float far_depth;
  uint32_t pad;
};

}  // namespace

bool LightMaskPass::Initialize() {
  if (pso_) return true;
  ID3D12Device* device = device_.device();
  const D3D12_DESCRIPTOR_RANGE ranges[2] = {
      {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5, 0, 0, 0},
      {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 5},
  };
  D3D12_ROOT_PARAMETER params[2] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable = {2, ranges};
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants = {0, 0, sizeof(Constants) / 4};
  const D3D12_ROOT_SIGNATURE_DESC rs = {2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ComPtr<ID3DBlob> blob, err;
  const D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                         kViews * D3D12Device::kFramesInFlight,
                                         D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  if (Failed(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err),
             "light mask root signature serialize") ||
      Failed(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                         IID_PPV_ARGS(&signature_)),
             "light mask root signature") ||
      Failed(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_)), "light mask heap"))
    return false;
  stride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  const pD3DCompile compile = D3DCompiler();
  ComPtr<ID3DBlob> code, error;
  if (!compile || FAILED(compile(kLightMaskHlsl, sizeof(kLightMaskHlsl) - 1, "pgr4_light_mask.hlsl",
                                 nullptr, nullptr, "main", "cs_5_0",
                                 D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error))) {
    REXLOG_ERROR("PGR4 native light mask: {}",
                 error ? static_cast<const char*>(error->GetBufferPointer()) : "?");
    return false;
  }
  D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
  pd.pRootSignature = signature_.Get();
  pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
  if (Failed(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso_)), "light mask")) {
    pso_.Reset();
    return false;
  }
  // MirrorCompare's (without it, no mirror mask: the rest as before).
  {
    const D3D12_DESCRIPTOR_RANGE cr[2] = {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 2},
    };
    D3D12_ROOT_PARAMETER cp[2] = {};
    cp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    cp[0].DescriptorTable = {2, cr};
    cp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    cp[1].Constants = {0, 0, sizeof(CompareConstants) / 4};
    const D3D12_ROOT_SIGNATURE_DESC crs = {2, cp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    const D3D12_DESCRIPTOR_HEAP_DESC chd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                            kCompareViews * D3D12Device::kFramesInFlight,
                                            D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    ComPtr<ID3DBlob> cblob, cerr, ccode, cerror;
    if (Failed(D3D12SerializeRootSignature(&crs, D3D_ROOT_SIGNATURE_VERSION_1, &cblob, &cerr),
               "mirror compare root signature serialize") ||
        Failed(device->CreateRootSignature(0, cblob->GetBufferPointer(), cblob->GetBufferSize(),
                                           IID_PPV_ARGS(&compare_signature_)),
               "mirror compare root signature") ||
        Failed(device->CreateDescriptorHeap(&chd, IID_PPV_ARGS(&compare_heap_)),
               "mirror compare heap")) {
      compare_signature_.Reset();
      return true;
    }
    if (FAILED(compile(kMirrorCompareHlsl, sizeof(kMirrorCompareHlsl) - 1,
                       "pgr4_mirror_compare.hlsl", nullptr, nullptr, "main", "cs_5_0",
                       D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ccode, &cerror))) {
      REXLOG_ERROR("PGR4 native mirror compare: {}",
                   cerror ? static_cast<const char*>(cerror->GetBufferPointer()) : "?");
      return true;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC cpd = {};
    cpd.pRootSignature = compare_signature_.Get();
    cpd.CS = {ccode->GetBufferPointer(), ccode->GetBufferSize()};
    if (Failed(device->CreateComputePipelineState(&cpd, IID_PPV_ARGS(&compare_pso_)),
               "mirror compare"))
      compare_pso_.Reset();
  }
  return true;
}

void LightMaskPass::MirrorCompare(ID3D12GraphicsCommandList* list, uint32_t slot,
                                  ID3D12Resource* scene_depth, DXGI_FORMAT scene_format,
                                  ID3D12Resource* mirror_depth, DXGI_FORMAT mirror_format,
                                  bool reversed, uint32_t width, uint32_t height,
                                  uint64_t frame) {
  if (!compare_pso_ || !scene_depth || !mirror_depth || !width || !height) return;
  ID3D12Device* device = device_.device();
  if (!mirror_mask_ || mirror_mask_w_ != width || mirror_mask_h_ != height) {
    mirror_mask_old_[1] = std::move(mirror_mask_old_[0]);
    mirror_mask_old_[0] = std::move(mirror_mask_);
    const D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC rd = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1,
                              DXGI_FORMAT_R8_UNORM, {1, 0}};
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd,
                                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                               nullptr, IID_PPV_ARGS(&mirror_mask_)))) {
      mirror_mask_.Reset();
      return;
    }
    mirror_mask_w_ = width;
    mirror_mask_h_ = height;
  }
  const uint32_t first = (slot % D3D12Device::kFramesInFlight) * kCompareViews;
  const uint32_t stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  D3D12_CPU_DESCRIPTOR_HANDLE cpu = compare_heap_->GetCPUDescriptorHandleForHeapStart();
  cpu.ptr += size_t(first) * stride;
  ID3D12Resource* const views[2] = {scene_depth, mirror_depth};
  const DXGI_FORMAT formats[2] = {scene_format, mirror_format};
  for (uint32_t i = 0; i < 2; ++i) {
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = formats[i];
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE c = cpu;
    c.ptr += size_t(i) * stride;
    device->CreateShaderResourceView(views[i], &sd, c);
  }
  D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
  ud.Format = DXGI_FORMAT_R8_UNORM;
  ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  D3D12_CPU_DESCRIPTOR_HANDLE uc = cpu;
  uc.ptr += size_t(2) * stride;
  device->CreateUnorderedAccessView(mirror_mask_.Get(), nullptr, &ud, uc);
  D3D12_RESOURCE_BARRIER b =
      TransitionBarrier(mirror_mask_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  list->ResourceBarrier(1, &b);
  CompareConstants c = {{width, height}, reversed ? 0.0f : 1.0f, 0};
  ID3D12DescriptorHeap* heaps[] = {compare_heap_.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(compare_signature_.Get());
  D3D12_GPU_DESCRIPTOR_HANDLE gpu = compare_heap_->GetGPUDescriptorHandleForHeapStart();
  gpu.ptr += uint64_t(first) * stride;
  list->SetComputeRootDescriptorTable(0, gpu);
  list->SetComputeRoot32BitConstants(1, sizeof(c) / 4, &c, 0);
  list->SetPipelineState(compare_pso_.Get());
  list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
  std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
  list->ResourceBarrier(1, &b);
  mirror_mask_frame_ = frame;
}

void LightMaskPass::Apply(ID3D12GraphicsCommandList* list, uint32_t slot, ID3D12Resource* image,
                          ID3D12Resource* hudless, ID3D12Resource* mask, uint32_t mask_w,
                          uint32_t mask_h, const int32_t rect[4], float threshold,
                          ID3D12Resource* flares, const float to_flares[2],
                          const int32_t flare_origin[2], float flare_threshold, bool use_sun,
                          ID3D12Resource* hud, float hud_threshold, ID3D12Resource* mirrors,
                          uint32_t mirrors_w, uint32_t mirrors_h) {
  if (!pso_ || !image || !hudless || (!mask && !flares && !hud && !mirrors) || rect[2] <= 0 ||
      rect[3] <= 0)
    return;
  ID3D12Device* device = device_.device();
  const uint32_t first = (slot % D3D12Device::kFramesInFlight) * kViews;
  D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap_->GetCPUDescriptorHandleForHeapStart();
  cpu.ptr += size_t(first) * stride_;
  // A missing mask's view: another one's (not read: has_mask / has_flares / has_hud).
  ID3D12Resource* const any = mask ? mask : flares ? flares : hud ? hud : mirrors;
  ID3D12Resource* const views[5] = {image, mask ? mask : any, flares ? flares : any,
                                    hud ? hud : any, mirrors ? mirrors : any};
  for (uint32_t i = 0; i < 5; ++i) {
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = views[i]->GetDesc().Format;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE c = cpu;
    c.ptr += size_t(i) * stride_;
    device->CreateShaderResourceView(views[i], &sd, c);
  }
  D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
  ud.Format = hudless->GetDesc().Format;
  ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  D3D12_CPU_DESCRIPTOR_HANDLE uc = cpu;
  uc.ptr += size_t(5) * stride_;
  device->CreateUnorderedAccessView(hudless, nullptr, &ud, uc);

  D3D12_RESOURCE_BARRIER bs[2] = {
      TransitionBarrier(image, D3D12_RESOURCE_STATE_PRESENT,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
      TransitionBarrier(hudless, D3D12_RESOURCE_STATE_PRESENT,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
  list->ResourceBarrier(2, bs);
  Constants c = {};
  for (int i = 0; i < 4; ++i) c.rect[i] = rect[i];
  c.to_mask[0] = float(mask_w) / float(rect[2]);
  c.to_mask[1] = float(mask_h) / float(rect[3]);
  c.mask_max[0] = int32_t(mask_w) - 1;
  c.mask_max[1] = int32_t(mask_h) - 1;
  c.threshold = threshold;
  c.has_mask = mask ? 1u : 0u;
  c.has_flares = flares ? 1u : 0u;
  c.use_sun = use_sun ? 1u : 0u;
  c.flare_threshold = flare_threshold;
  c.has_hud = hud ? 1u : 0u;
  c.hud_threshold = hud_threshold;
  c.has_mirrors = mirrors ? 1u : 0u;
  if (mirrors) {
    c.to_mirrors[0] = float(mirrors_w) / float(rect[2]);
    c.to_mirrors[1] = float(mirrors_h) / float(rect[3]);
    c.mirrors_max[0] = int32_t(mirrors_w) - 1;
    c.mirrors_max[1] = int32_t(mirrors_h) - 1;
  }
  if (hud) {
    const D3D12_RESOURCE_DESC hd = hud->GetDesc();
    c.hud_max[0] = int32_t(hd.Width) - 1;
    c.hud_max[1] = int32_t(hd.Height) - 1;
  }
  if (flares || hud) {
    const D3D12_RESOURCE_DESC fd = (flares ? flares : hud)->GetDesc();
    c.to_flares[0] = to_flares[0];
    c.to_flares[1] = to_flares[1];
    c.flare_origin[0] = flare_origin[0];
    c.flare_origin[1] = flare_origin[1];
    c.flare_max[0] = flares ? int32_t(fd.Width) - 1 : 0;
    c.flare_max[1] = flares ? int32_t(fd.Height) - 1 : 0;
  }
  ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(signature_.Get());
  D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap_->GetGPUDescriptorHandleForHeapStart();
  gpu.ptr += uint64_t(first) * stride_;
  list->SetComputeRootDescriptorTable(0, gpu);
  list->SetComputeRoot32BitConstants(1, sizeof(c) / 4, &c, 0);
  list->SetPipelineState(pso_.Get());
  list->Dispatch((uint32_t(rect[2]) + 7) / 8, (uint32_t(rect[3]) + 7) / 8, 1);
  for (D3D12_RESOURCE_BARRIER& b : bs)
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
  list->ResourceBarrier(2, bs);
}

}  // namespace pgr4::native
