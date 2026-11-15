// pgr4-native PGR4 native renderer: effects over the game's 3D scene (scene_effects.h).
#include "scene_effects.h"

#include "d3d12_device.h"
#include "upscaler.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

REXCVAR_DEFINE_BOOL(pgr4_native_gtao, false, "PGR4 native",
                    "Ground-truth ambient occlusion (GTAO) over the 3D scene (scene_effects.h)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_gtao_radius, 1.0, "PGR4 native",
                      "GTAO: the radius around each point it looks for occluders in, in the "
                      "game's units (meters)")
    .range(0.1, 5.0);
REXCVAR_DEFINE_DOUBLE(pgr4_native_gtao_intensity, 1.0, "PGR4 native",
                      "GTAO: the strength of the occlusion (0: none; the visibility's power)")
    .range(0.0, 4.0);
REXCVAR_DEFINE_DOUBLE(pgr4_native_gtao_distance, 300.0, "PGR4 native",
                      "GTAO: the distance beyond which nothing is occluded (meters), fading over "
                      "its last fifth")
    .range(10.0, 7000.0);
REXCVAR_DEFINE_BOOL(pgr4_native_gtao_full, false, "PGR4 native",
                    "GTAO at the scene's full size instead of half (steadier on thin edges, about "
                    "four times the cost)");
REXCVAR_DEFINE_BOOL(pgr4_native_gtao_debug, false, "PGR4 native",
                    "GTAO debug: the scene's color replaced by the occlusion alone");
bool Pgr4GtaoScreen();  // src/pgr4_menu.cpp: a race's, the garage's, a replay's or photo screen

namespace pgr4::native {

// Local change (pgr4 project): the overlay's "the game without our changes" (renderer.cpp).
bool OriginalLook();
namespace {

// Descriptors per frame slot: the depth pass's depth, (unused), half-size depth; the occlusion
// pass's half-size depth, (unused), raw; the blur's half-size depth, raw, blurred; the apply
// draw's blurred, half-size depth and depth.
constexpr uint32_t kViewsPerSlot = 12;

struct AoConstants {
  uint32_t size[2];  // the scene's host size
  float near_plane, far_plane;
  float proj[2];  // view-space x and y over depth at the edges: tan(fov_y / 2) times aspect, and it
  float radius, intensity;
  float max_distance;
  uint32_t frame;
  uint32_t half_size[2];  // the occlusion's
  uint32_t ratio;         // the scene's pixels per occlusion texel on each axis: 2, or 1 (full)
  uint32_t pad[3];
};

constexpr char kAoHlsl[] = R"(
cbuffer C : register(b0) {
  uint2 size; float near_plane; float far_plane;
  float2 proj; float radius; float intensity;
  float max_distance; uint frame; uint2 half_size;
  uint ratio; uint3 pad;
};
static const float kPi = 3.14159265;
// Inverted depth: 1 at the near plane, 0 at the far.
float ViewZ(float d) {
  return near_plane * far_plane / ((far_plane - near_plane) * d + near_plane);
}
// View space: x right, y up, z the depth forward; p in the scene's host pixels.
float3 ViewPos(float2 p, float z) {
  float2 uv = (p + 0.5) / float2(size);
  return float3(float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0) * proj * z, z);
}

#ifdef DOWN
// The depth at half size, read once (the scene's depth is multisampled and four times larger):
// texel q the depth of pixel 2q, sample 0, as the other passes take it.
#ifdef MS
Texture2DMS<float> depth_tex : register(t0);
float RawDepth(int2 p) { return depth_tex.Load(p, 0); }
#else
Texture2D<float> depth_tex : register(t0);
float RawDepth(int2 p) { return depth_tex.Load(int3(p, 0)); }
#endif
RWTexture2D<float> half_depth : register(u0);
[numthreads(8, 8, 1)]
void main(uint2 p : SV_DispatchThreadID) {
  if (any(p >= half_size)) return;
  half_depth[p] = RawDepth(min(int2(p) * int(ratio), int2(size) - 1));
}
#else
Texture2D<float> half_depth : register(t0);
Texture2D<float> src : register(t1);
RWTexture2D<unorm float> dst : register(u0);
// q in half-size texels.
float ZAt(int2 q) {
  return ViewZ(half_depth.Load(int3(clamp(q, int2(0, 0), int2(half_size) - 1), 0)));
}
float3 PosAt(int2 q) { return ViewPos(float2(q) * float(ratio), ZAt(q)); }

#ifdef BLUR
// Depth-aware 5x5 average of the raw occlusion.
[numthreads(8, 8, 1)]
void main(uint2 p : SV_DispatchThreadID) {
  if (any(p >= half_size)) return;
  float zc = ZAt(int2(p));
  float sum = 0.0, weight = 0.0;
  for (int y = -2; y <= 2; ++y)
    for (int x = -2; x <= 2; ++x) {
      int2 q = clamp(int2(p) + int2(x, y), int2(0, 0), int2(half_size) - 1);
      float w = exp(-abs(ZAt(q) - zc) / (0.02 * zc + 0.01));
      sum += src.Load(int3(q, 0)) * w;
      weight += w;
    }
  dst[p] = sum / max(weight, 1e-4);
}
#else
// GTAO (Jimenez et al., "Practical Real-Time Strategies for Accurate Indirect Occlusion", 2016,
// laid out as XeGTAO does): per slice (a screen direction), the highest horizon on each side
// within the radius, and the visibility between them over the normal projected into the slice.
static const int kSlices = 2;
static const int kSteps = 4;
float FastAcos(float x) {
  float r = (-0.156583 * abs(x) + kPi * 0.5) * sqrt(1.0 - abs(x));
  return x >= 0.0 ? r : kPi - r;
}
float Noise(float2 p) {  // interleaved gradient noise
  return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}
[numthreads(8, 8, 1)]
void main(uint2 p : SV_DispatchThreadID) {
  if (any(p >= half_size)) return;
  int2 ip = int2(p);
  float3 c = PosAt(ip);
  float fade = saturate((max_distance - c.z) / (0.2 * max_distance));
  if (fade <= 0.0 || intensity <= 0.0) { dst[p] = 1.0; return; }
  // The surface's normal from the nearer neighbour on each axis (no bleeding across edges).
  float3 l = PosAt(ip - int2(1, 0)), r = PosAt(ip + int2(1, 0));
  float3 u = PosAt(ip - int2(0, 1)), d = PosAt(ip + int2(0, 1));
  float3 dx = abs(r.z - c.z) < abs(c.z - l.z) ? r - c : c - l;
  float3 dy = abs(d.z - c.z) < abs(c.z - u.z) ? d - c : c - u;
  float3 n = normalize(cross(dx, dy));
  if (dot(n, c) > 0.0) n = -n;  // toward the camera
  float3 v = normalize(-c);
  // The radius in half-size texels at this depth.
  float pixels = radius / (c.z * proj.y) * (float(half_size.y) * 0.5);
  if (pixels < 1.0) { dst[p] = 1.0; return; }
  pixels = min(pixels, float(half_size.y) * 0.1);
  // Per pixel: the slices' rotation and the steps' jitter, a fixed pattern the blur averages (one
  // changing every frame flickers through the upscaler).
  float2 seed = float2(p);
  float rotation = Noise(seed), jitter = Noise(seed + float2(17.0, 31.0));
  float falloff_range = 0.615 * radius;
  float falloff_mul = -1.0 / falloff_range;
  float falloff_add = (radius - falloff_range) / falloff_range + 1.0;
  float visibility = 0.0;
  [unroll] for (int slice = 0; slice < kSlices; ++slice) {
    float phi = (float(slice) + rotation) * kPi / float(kSlices);
    float2 omega = float2(cos(phi), -sin(phi));     // on screen (y down)
    float3 dir = float3(cos(phi), sin(phi), 0.0);  // the same in view space (y up)
    float3 ortho = dir - dot(dir, v) * v;
    float3 axis = normalize(cross(ortho, v));
    float3 pn = n - axis * dot(n, axis);
    float pn_length = length(pn);
    float cos_n = saturate(dot(pn, v) / max(pn_length, 1e-4));
    float nn = sign(dot(ortho, pn)) * FastAcos(cos_n);
    float low0 = cos(nn + kPi * 0.5), low1 = cos(nn - kPi * 0.5);
    float h0c = low0, h1c = low1;
    [unroll] for (int step = 0; step < kSteps; ++step) {
      float t = (float(step) + jitter) / float(kSteps);
      float2 offset = round(omega * max(t * t * pixels, float(step) + 1.0));
      float3 d0 = PosAt(ip + int2(offset)) - c;
      float3 d1 = PosAt(ip - int2(offset)) - c;
      float l0 = length(d0), l1 = length(d1);
      float s0 = lerp(low0, dot(d0, v) / max(l0, 1e-4), saturate(l0 * falloff_mul + falloff_add));
      float s1 = lerp(low1, dot(d1, v) / max(l1, 1e-4), saturate(l1 * falloff_mul + falloff_add));
      h0c = max(h0c, s0);
      h1c = max(h1c, s1);
    }
    float h0 = -FastAcos(h1c), h1 = FastAcos(h0c);
    h0 = nn + clamp(h0 - nn, -kPi * 0.5, kPi * 0.5);
    h1 = nn + clamp(h1 - nn, -kPi * 0.5, kPi * 0.5);
    float sin_n = sin(nn);
    float arc0 = (cos_n + 2.0 * h0 * sin_n - cos(2.0 * h0 - nn)) * 0.25;
    float arc1 = (cos_n + 2.0 * h1 * sin_n - cos(2.0 * h1 - nn)) * 0.25;
    visibility += pn_length * (arc0 + arc1);
  }
  visibility = saturate(visibility / float(kSlices));
  // Light bouncing off the occluders (Jimenez et al.'s multi-bounce fit, for a mid albedo): a
  // corner is never black. Then at most half the light taken (the game's own baked occlusion
  // and its sun shadows are there already).
  const float albedo = 0.45;
  float ka = 2.0404 * albedo - 0.3324, kb = -4.7951 * albedo + 0.6417;
  float kc = 2.7552 * albedo + 0.6903;
  visibility = max(visibility, ((visibility * ka + kb) * visibility + kc) * visibility);
  visibility = max(pow(saturate(visibility), intensity), 0.5);
  dst[p] = lerp(1.0, visibility, fade);
}
#endif
#endif
)";

// The occlusion (half size) brought up to the scene's size: of the four half-size texels around
// the pixel, bilinear weights times how close each texel's depth is to the pixel's (relative,
// in the inverted depth, which is about 1 / view depth), so no occlusion crosses an edge between
// near and far.
constexpr char kApplyHlsl[] = R"(
Texture2D<float> ao : register(t0);
Texture2D<float> half_depth : register(t1);
#ifdef MS
Texture2DMS<float> depth_tex : register(t2);
float RawDepth(int2 p) { return depth_tex.Load(p, 0); }
#else
Texture2D<float> depth_tex : register(t2);
float RawDepth(int2 p) { return depth_tex.Load(int3(p, 0)); }
#endif
void vs(uint id : SV_VertexID, out float4 pos : SV_Position) {
  float2 uv = float2((id << 1) & 2, id & 2);
  pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
float4 ps(float4 pos : SV_Position) : SV_Target {
  uint w, h;
  ao.GetDimensions(w, h);
  int2 p = int2(pos.xy);
  float dp = RawDepth(p);
  // The occlusion texels around the pixel (texel q covers `ratio` pixels from ratio * q on each
  // axis, its depth taken at ratio * q as the occlusion pass did).
  uint fw, fh, fs;
#ifdef MS
  depth_tex.GetDimensions(fw, fh, fs);
#else
  depth_tex.GetDimensions(fw, fh);
#endif
  float ratio = round(float(fw) / float(w));
  float2 t = (pos.xy - 0.5 * ratio) / ratio;
  int2 q0 = int2(floor(t));
  float2 f = t - float2(q0);
  float sum = 0.0, weight = 0.0;
  [unroll] for (int k = 0; k < 4; ++k) {
    int2 o = int2(k & 1, k >> 1);
    int2 q = clamp(q0 + o, int2(0, 0), int2(w, h) - 1);
    float b = (o.x ? f.x : 1.0 - f.x) * (o.y ? f.y : 1.0 - f.y);
    float dq = half_depth.Load(int3(q, 0));
    float z = b * exp(-abs(dq - dp) / (0.03 * max(dp, 1e-6))) + 1e-5;
    sum += ao.Load(int3(q, 0)) * z;
    weight += z;
  }
  float a = sum / weight;
  return float4(a, a, a, 1.0);
}
)";

}  // namespace

// Local change: only in a race, the garage, a replay and the photo mode (src/pgr4_menu.cpp); the
// menus' scenes drew its shadows where nothing stands.
bool SceneEffects::Enabled() {
  return REXCVAR_GET(pgr4_native_gtao) && ::Pgr4GtaoScreen() && !OriginalLook();
}

bool SceneEffects::Initialize(ID3D12Device* device) {
  device_ = device;
  const pD3DCompile compile = D3DCompiler();
  if (!compile) return false;

  // Compute: t0-t1 and u0, then the constants.
  D3D12_DESCRIPTOR_RANGE ranges[2] = {
      {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0},
      {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 2},
  };
  D3D12_ROOT_PARAMETER params[2] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable = {2, ranges};
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants = {0, 0, sizeof(AoConstants) / 4};
  const D3D12_ROOT_SIGNATURE_DESC cs = {2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ComPtr<ID3DBlob> blob, error;
  if (Failed(D3D12SerializeRootSignature(&cs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error),
             "scene effects root signature serialize") ||
      Failed(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                         IID_PPV_ARGS(&compute_signature_)),
             "scene effects root signature"))
    return false;
  // The apply draw: t0 the occlusion, t1 the half-size depth, t2 the depth; pixel shader.
  D3D12_DESCRIPTOR_RANGE apply_range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0};
  D3D12_ROOT_PARAMETER apply_param = {};
  apply_param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  apply_param.DescriptorTable = {1, &apply_range};
  apply_param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  const D3D12_ROOT_SIGNATURE_DESC as = {1, &apply_param, 0, nullptr,
                                        D3D12_ROOT_SIGNATURE_FLAG_NONE};
  blob.Reset();
  error.Reset();
  if (Failed(D3D12SerializeRootSignature(&as, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error),
             "scene effects apply root signature serialize") ||
      Failed(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                         IID_PPV_ARGS(&apply_signature_)),
             "scene effects apply root signature"))
    return false;

  // pass: nullptr (the occlusion), "BLUR" or "DOWN" (the half-size depth).
  auto compute = [&](const char* name, bool ms, const char* pass,
                     ComPtr<ID3D12PipelineState>& out) {
    D3D_SHADER_MACRO macros[3] = {};
    int n = 0;
    if (ms) macros[n++] = {"MS", "1"};
    if (pass) macros[n++] = {pass, "1"};
    ComPtr<ID3DBlob> code, err;
    if (FAILED(compile(kAoHlsl, sizeof(kAoHlsl) - 1, name, macros, nullptr, "main", "cs_5_0",
                       D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err))) {
      REXLOG_ERROR("PGR4 native GTAO: {}: {}", name,
                   err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
      return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC d = {};
    d.pRootSignature = compute_signature_.Get();
    d.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    return !Failed(device->CreateComputePipelineState(&d, IID_PPV_ARGS(&out)), name);
  };
  if (!compute("pgr4_gtao_depth.hlsl", false, "DOWN", down_pso_) ||
      !compute("pgr4_gtao_depth_ms.hlsl", true, "DOWN", down_ms_pso_) ||
      !compute("pgr4_gtao.hlsl", false, nullptr, ao_pso_) ||
      !compute("pgr4_gtao_blur.hlsl", false, "BLUR", blur_pso_))
    return false;
  ComPtr<ID3DBlob> err;
  const D3D_SHADER_MACRO ms_macros[] = {{"MS", "1"}, {nullptr, nullptr}};
  if (FAILED(compile(kApplyHlsl, sizeof(kApplyHlsl) - 1, "pgr4_gtao_apply.hlsl", nullptr, nullptr,
                     "vs", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &apply_vs_, &err)) ||
      FAILED(compile(kApplyHlsl, sizeof(kApplyHlsl) - 1, "pgr4_gtao_apply.hlsl", nullptr, nullptr,
                     "ps", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &apply_ps_, &err)) ||
      FAILED(compile(kApplyHlsl, sizeof(kApplyHlsl) - 1, "pgr4_gtao_apply_ms.hlsl", ms_macros,
                     nullptr, "ps", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &apply_ps_ms_,
                     &err))) {
    REXLOG_ERROR("PGR4 native GTAO: apply: {}",
                 err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
    return false;
  }

  D3D12_DESCRIPTOR_HEAP_DESC hd = {};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  hd.NumDescriptors = kViewsPerSlot * D3D12Device::kFramesInFlight;
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (Failed(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_)), "scene effects heap"))
    return false;
  stride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  ready_ = true;
  return true;
}

bool SceneEffects::EnsureTargets(uint32_t width, uint32_t height) {
  if (raw_ && width_ == width && height_ == height) return true;
  raw_.Reset();
  ao_.Reset();
  half_depth_.Reset();
  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC d = {};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width = width;
  d.Height = height;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.Format = DXGI_FORMAT_R8_UNORM;
  d.SampleDesc.Count = 1;
  d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  for (ComPtr<ID3D12Resource>* r :
       {std::addressof(raw_), std::addressof(ao_), std::addressof(half_depth_)}) {
    d.Format = r == std::addressof(half_depth_) ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R8_UNORM;
    if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                IID_PPV_ARGS(r->GetAddressOf())),
               "GTAO target")) {
      raw_.Reset();
      ao_.Reset();
      half_depth_.Reset();
      return false;
    }
  }
  raw_state_ = ao_state_ = half_depth_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  width_ = width;
  height_ = height;
  return true;
}

ID3D12PipelineState* SceneEffects::ApplyPipeline(DXGI_FORMAT format, uint32_t samples) {
  const bool debug = REXCVAR_GET(pgr4_native_gtao_debug);
  // The debug view's key: no blending (the occlusion alone).
  const auto key = std::make_pair(format, samples | (debug ? 0x100u : 0u));
  if (auto it = apply_psos_.find(key); it != apply_psos_.end()) return it->second.Get();
  D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
  d.pRootSignature = apply_signature_.Get();
  d.VS = {apply_vs_->GetBufferPointer(), apply_vs_->GetBufferSize()};
  ID3DBlob* ps = samples > 1 ? apply_ps_ms_.Get() : apply_ps_.Get();
  d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
  D3D12_RENDER_TARGET_BLEND_DESC& b = d.BlendState.RenderTarget[0];
  b.BlendEnable = !debug;
  // color *= occlusion; alpha (the motion blur's mask) kept.
  b.SrcBlend = D3D12_BLEND_ZERO;
  b.DestBlend = D3D12_BLEND_SRC_COLOR;
  b.BlendOp = D3D12_BLEND_OP_ADD;
  b.SrcBlendAlpha = D3D12_BLEND_ZERO;
  b.DestBlendAlpha = D3D12_BLEND_ONE;
  b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
  b.LogicOp = D3D12_LOGIC_OP_NOOP;
  b.RenderTargetWriteMask = debug ? D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN |
                                        D3D12_COLOR_WRITE_ENABLE_BLUE
                                  : D3D12_COLOR_WRITE_ENABLE_ALL;
  d.SampleMask = UINT_MAX;
  d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  d.RasterizerState.DepthClipEnable = TRUE;
  d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  d.NumRenderTargets = 1;
  d.RTVFormats[0] = format;
  d.SampleDesc.Count = samples;
  ComPtr<ID3D12PipelineState> pso;
  if (Failed(device_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)), "GTAO apply")) {
    apply_psos_[key] = nullptr;
    return nullptr;
  }
  apply_psos_[key] = pso;
  return pso.Get();
}

void SceneEffects::Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* r,
                              D3D12_RESOURCE_STATES& state, D3D12_RESOURCE_STATES after) {
  if (state == after) return;
  const D3D12_RESOURCE_BARRIER b = TransitionBarrier(r, state, after);
  list->ResourceBarrier(1, &b);
  state = after;
}

void SceneEffects::Apply(ID3D12GraphicsCommandList* list, RenderTargets& targets,
                         const SurfaceRegs& regs, uint64_t frame) {
  if (!ready_ || !Enabled() || applied_frame_ == frame) return;
  applied_frame_ = frame;
  RenderTarget* color = targets.GetColor(regs, 0, Upscaler::SceneHeight());
  RenderTarget* depth = targets.GetDepth(regs, Upscaler::SceneHeight());
  if (!color || !depth || color->samples != depth->samples || !(color->scale == depth->scale))
    return;
  // The draws' host extent of the scene.
  const uint32_t w = color->scale.Extent(color->width, 0), h = color->scale.Extent(color->height, 1);
  // The occlusion's size: half the scene's, or all of it (pgr4_native_gtao_full).
  const uint32_t ratio = REXCVAR_GET(pgr4_native_gtao_full) ? 1 : 2;
  const uint32_t hw = (w + ratio - 1) / ratio, hh = (h + ratio - 1) / ratio;
  if (!w || !h || !EnsureTargets(hw, hh)) return;
  const D3D12_RECT all = {0, 0, LONG(color->width), LONG(color->height)};
  targets.PrepareRead(list, color, all);
  targets.PrepareRead(list, depth, all);
  const bool ms = depth->samples > 1;

  const uint32_t first = uint32_t(frame % D3D12Device::kFramesInFlight) * kViewsPerSlot;
  auto cpu = [&](uint32_t i) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = heap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += size_t(first + i) * stride_;
    return h;
  };
  auto gpu = [&](uint32_t i) {
    D3D12_GPU_DESCRIPTOR_HANDLE h = heap_->GetGPUDescriptorHandleForHeapStart();
    h.ptr += uint64_t(first + i) * stride_;
    return h;
  };
  D3D12_SHADER_RESOURCE_VIEW_DESC depth_srv = {};
  depth_srv.Format = depth->srv_format;
  depth_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  if (ms) {
    depth_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
  } else {
    depth_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    depth_srv.Texture2D.MipLevels = 1;
  }
  D3D12_SHADER_RESOURCE_VIEW_DESC r8_srv = {};
  r8_srv.Format = DXGI_FORMAT_R8_UNORM;
  r8_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  r8_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  r8_srv.Texture2D.MipLevels = 1;
  D3D12_UNORDERED_ACCESS_VIEW_DESC r8_uav = {};
  r8_uav.Format = DXGI_FORMAT_R8_UNORM;
  r8_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  D3D12_SHADER_RESOURCE_VIEW_DESC z_srv = r8_srv;
  z_srv.Format = DXGI_FORMAT_R32_FLOAT;
  D3D12_UNORDERED_ACCESS_VIEW_DESC z_uav = r8_uav;
  z_uav.Format = DXGI_FORMAT_R32_FLOAT;
  device_->CreateShaderResourceView(depth->resource.Get(), &depth_srv, cpu(0));
  device_->CreateShaderResourceView(nullptr, &r8_srv, cpu(1));
  device_->CreateUnorderedAccessView(half_depth_.Get(), nullptr, &z_uav, cpu(2));
  device_->CreateShaderResourceView(half_depth_.Get(), &z_srv, cpu(3));
  device_->CreateShaderResourceView(nullptr, &r8_srv, cpu(4));
  device_->CreateUnorderedAccessView(raw_.Get(), nullptr, &r8_uav, cpu(5));
  device_->CreateShaderResourceView(half_depth_.Get(), &z_srv, cpu(6));
  device_->CreateShaderResourceView(raw_.Get(), &r8_srv, cpu(7));
  device_->CreateUnorderedAccessView(ao_.Get(), nullptr, &r8_uav, cpu(8));
  device_->CreateShaderResourceView(ao_.Get(), &r8_srv, cpu(9));
  device_->CreateShaderResourceView(half_depth_.Get(), &z_srv, cpu(10));
  device_->CreateShaderResourceView(depth->resource.Get(), &depth_srv, cpu(11));

  const FfxCamera cam = camera_ ? *camera_ : FfxCamera{};
  AoConstants c = {};
  c.size[0] = w;
  c.size[1] = h;
  c.near_plane = cam.near_plane;
  c.far_plane = cam.far_plane;
  const float t = std::tan(cam.fov_y * 0.5f);
  c.proj[0] = t * float(w) / float(h);
  c.proj[1] = t;
  c.radius = float(REXCVAR_GET(pgr4_native_gtao_radius));
  c.intensity = float(REXCVAR_GET(pgr4_native_gtao_intensity));
  c.max_distance = float(REXCVAR_GET(pgr4_native_gtao_distance));
  c.frame = uint32_t(frame);
  c.half_size[0] = hw;
  c.half_size[1] = hh;
  c.ratio = ratio;

  Transition(list, depth->resource.Get(), depth->state,
             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Transition(list, half_depth_.Get(), half_depth_state_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  Transition(list, raw_.Get(), raw_state_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  Transition(list, ao_.Get(), ao_state_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(compute_signature_.Get());
  list->SetComputeRoot32BitConstants(1, sizeof(c) / 4, &c, 0);
  const UINT gx = (hw + 7) / 8, gy = (hh + 7) / 8;
  list->SetPipelineState(ms ? down_ms_pso_.Get() : down_pso_.Get());
  list->SetComputeRootDescriptorTable(0, gpu(0));
  list->Dispatch(gx, gy, 1);
  Transition(list, half_depth_.Get(), half_depth_state_,
             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  list->SetPipelineState(ao_pso_.Get());
  list->SetComputeRootDescriptorTable(0, gpu(3));
  list->Dispatch(gx, gy, 1);
  Transition(list, raw_.Get(), raw_state_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  list->SetPipelineState(blur_pso_.Get());
  list->SetComputeRootDescriptorTable(0, gpu(6));
  list->Dispatch(gx, gy, 1);
  Transition(list, ao_.Get(), ao_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  Transition(list, depth->resource.Get(), depth->state,
             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

  // color *= occlusion, a full-screen triangle into the scene's target.
  ID3D12PipelineState* pso = ApplyPipeline(color->view_format, color->samples);
  if (!pso) return;
  Transition(list, color->resource.Get(), color->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
  list->OMSetRenderTargets(1, &color->view, FALSE, nullptr);
  const D3D12_VIEWPORT vp = {0.0f, 0.0f, float(w), float(h), 0.0f, 1.0f};
  const D3D12_RECT scissor = {0, 0, LONG(w), LONG(h)};
  list->RSSetViewports(1, &vp);
  list->RSSetScissorRects(1, &scissor);
  list->SetGraphicsRootSignature(apply_signature_.Get());
  list->SetPipelineState(pso);
  list->SetGraphicsRootDescriptorTable(0, gpu(9));
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  list->DrawInstanced(3, 1, 0, 0);
}

}  // namespace pgr4::native
