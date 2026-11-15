// pgr4-native PGR4 native renderer: FidelityFX Super Resolution of the scene. See upscaler.h.
#include "upscaler.h"

#include "clock.h"
#include "frame_dump.h"
#include "frame_generation.h"
#include "log.h"
#include "streamline.h"

#include <ffx_upscale.h>
#include <dx12/ffx_api_dx12.h>
#include <sl_dlss.h>
#include <sl_matrix_helpers.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xfile.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

REXCVAR_DEFINE_BOOL(pgr4_native_upscaler, false, "PGR4 native",
                    "Render the 3D scene without MSAA and antialias it with AMD FidelityFX Super "
                    "Resolution (amd_fidelityfx_loader_dx12.dll and "
                    "amd_fidelityfx_upscaler_dx12.dll beside the executable, or what OptiScaler "
                    "puts behind them) from the start; bind_upscaler (F7) switches it");
REXCVAR_DEFINE_DOUBLE(pgr4_native_upscaler_lod_offset, 2.0, "PGR4 native",
                       "Mip levels added to the recommended texture LOD bias of the upscaler's "
                       "scene (-1 with DLAA): 0.5 or 1 softens fine grain the upscaler would "
                       "boil (local change, pgr4 project)");
REXCVAR_DEFINE_INT32(pgr4_native_upscaler_width, 1280, "PGR4 native",
                     "Width the upscaler's scene renders at, upscaled to 1280 (320-1280; 1280 "
                     "with the height at 720: antialiasing only)");
REXCVAR_DEFINE_INT32(pgr4_native_upscaler_height, 720, "PGR4 native",
                     "Height the upscaler's scene renders at, upscaled to 720 (180-720)");
REXCVAR_DEFINE_INT32(pgr4_native_upscaler_target_fps, 0, "PGR4 native",
                     "Vary the upscaler's render size each frame, from the width and height "
                     "down to --pgr4_native_upscaler_min_height, so that the GPU keeps up with "
                     "this frame rate (0: a fixed size)");
REXCVAR_DEFINE_INT32(pgr4_native_upscaler_min_height, 360, "PGR4 native",
                     "With --pgr4_native_upscaler_target_fps: the least height the scene renders "
                     "at (180-720)");
// Local change (pgr4 project): the reactive mask of the scene's transparents (Upscaler::
// SnapshotOpaque).
REXCVAR_DEFINE_BOOL(pgr4_native_reactive_mask, true, "PGR4 native",
                    "DLSS and FSR: a mask of the scene's transparents (smoke, sparks, glass), "
                    "from the scene's color before and after them, so they leave no trails");
// Local change (31/10/2026): DLSS takes the mask again (as its bias towards the current color),
// on its own switch: on 30/10 it ghosted more with it; under rain the exterior mirrors left
// trails without it. The strength and the largest value are the sliders below (the overlay).
REXCVAR_DEFINE_BOOL(pgr4_native_reactive_dlss, true, "PGR4 native",
                    "DLSS takes the reactive mask too (as its bias towards the current color)");
REXCVAR_DEFINE_BOOL(pgr4_native_reactive_show, false, "PGR4 native",
                    "The reactive mask in red over the upscaled scene (a diagnostic)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_reactive_scale, 2.0, "PGR4 native",
                      "The reactive mask: its value per unit of the change the transparents "
                      "made to a pixel (tone mapped)");
// Local change (31/10/2026): one strength per upscaler (pgr4_native_sharpen before), MSAA none.
REXCVAR_DEFINE_DOUBLE(pgr4_native_sharpen_dlss, 0.15, "PGR4 native",
                      "Contrast adaptive sharpening (AMD's CAS) of the scene upscaled by DLSS, "
                      "before the game's post-processing and HUD: 0 off, 1 the strongest")
    .range(0.0, 1.0);
REXCVAR_DEFINE_DOUBLE(pgr4_native_sharpen_fsr, 0.15, "PGR4 native",
                      "The same after FSR")
    .range(0.0, 1.0);
REXCVAR_DEFINE_DOUBLE(pgr4_native_reactive_max, 0.8, "PGR4 native",
                      "The reactive mask: its largest value (1: the history of a pixel the "
                      "transparents changed most is dropped)");
// Local change (pgr4 project): DLSS Super Resolution's quality mode and preset.
REXCVAR_DEFINE_STRING(pgr4_native_dlss_mode, "auto", "PGR4 native",
                      "The upscaler's quality mode, DLSS's or FSR's: auto (the render size from "
                      "--pgr4_native_upscaler_width/height), dlaa (native, antialiasing only), "
                      "quality, balanced, performance or ultra_performance (the render size from "
                      "the output's)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
// Local change (31/10/2026): E by default. The transformer models (J, K, and NVIDIA's default,
// K in DLAA) smeared the road along its motion in races; E, the CNN model, keeps it sharp.
REXCVAR_DEFINE_STRING(pgr4_native_dlss_preset, "E", "PGR4 native",
                      "DLSS Super Resolution's preset: E (the project's choice, CNN, any mode), "
                      "default (NVIDIA's for the mode) or a letter: "
                      "J, K (transformer, DLAA/Quality/Balanced default), L (Ultra Performance "
                      "default), M (Performance default)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
// Local change (pgr4 project): DLSS's jitter offset is the draws' jitter times this (1 or -1):
// Streamline's docs give no sign convention.
REXCVAR_DEFINE_INT32(pgr4_native_dlss_jitter_sign, 1, "PGR4 native",
                     "DLSS: sign of the jitter offset given to DLSS against the draws' (1 or -1)");
REXCVAR_DEFINE_INT32(pgr4_native_motion_dump, 0, "PGR4 native",
                     "Local diagnostic: every this many frames (0: off, up to 8 dumps), write the "
                     "motion vectors DLSS gets and the camera's motion of every pixel to "
                     "motion_dump_<frame>.bin beside the executable");
REXCVAR_DEFINE_INT32(pgr4_native_dlss_mvec_sign_x, 1, "PGR4 native",
                     "DLSS: sign of the motion vectors' x given to DLSS (1 or -1)");
REXCVAR_DEFINE_INT32(pgr4_native_dlss_mvec_sign_y, 1, "PGR4 native",
                     "DLSS: sign of the motion vectors' y given to DLSS (1 or -1)");
// DLSS-G reads the motion vectors DLSS SR takes (previous - current, UV) the other way round:
// with them as they are, its generated frames double the scenery toward the screen's centre
// (diagnosed 2026-10-28: the vectors, matrices, depth, cadence and timing all checked out).
REXCVAR_DEFINE_INT32(pgr4_native_dlss_fg_mvec_x, -1, "PGR4 native",
                     "DLSS-G: factor (1, -1 or 0) of the motion vectors' x frame generation gets, "
                     "set after DLSS Super Resolution has run with its own");
REXCVAR_DEFINE_INT32(pgr4_native_dlss_fg_mvec_y, -1, "PGR4 native",
                     "DLSS-G: factor (1, -1 or 0) of the motion vectors' y frame generation gets, "
                     "set after DLSS Super Resolution has run with its own");

namespace pgr4::native {

namespace {

constexpr DXGI_FORMAT kOutputFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr D3D12_RESOURCE_STATES kInputState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

void Message(uint32_t type, const wchar_t* message) {
  std::string s;
  const int n = WideCharToMultiByte(CP_UTF8, 0, message, -1, nullptr, 0, nullptr, nullptr);
  if (n > 1) {
    s.resize(size_t(n));
    WideCharToMultiByte(CP_UTF8, 0, message, -1, s.data(), n, nullptr, nullptr);
    s.resize(size_t(n - 1));
  }
  if (type == FFX_API_MESSAGE_TYPE_ERROR)
    PGR4_LOG_FIRST(16, ERROR, "PGR4 native upscaler: {}", s);
  else
    PGR4_LOG_FIRST(16, WARN, "PGR4 native upscaler: {}", s);
}

// The compute passes around FSR share a root signature: the source's SRV and the destination's UAV
// in a table, then PassConstants.
constexpr char kPassBindings[] = R"(
Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);
cbuffer Constants : register(b0) { float2 scale; uint2 size; };
)";
struct PassConstants {
  float scale[2];  // source pixels per destination pixel
  uint32_t size[2];
};
// Per frame in flight: the views of the input pass, of the alpha pass, then (local change) of the
// motion fill, of the motion dump's camera-motion pass, of the reactive mask's two passes and of
// the sharpening (slot 7), and of the frame generation's reduction (slots 8 and 9).
constexpr uint32_t kViewsPerFrame = 20;

// Local change (pgr4 project): the motion target's value where no draw wrote motion (FillMotion).
constexpr float kNoMotion = 30000.0f;

// The camera's motion into the scene's pixels without motion: from the pixel's depth (reversed),
// through clip_to_prev_clip (row vectors), the UV offset to where it was in the previous frame.
constexpr char kFillMotionHlsl[] = R"(
Texture2D<float> depth : register(t0);
RWTexture2D<float2> motion : register(u0);
cbuffer Constants : register(b0) { float4x4 clip_to_prev_clip; uint2 size; };
[numthreads(8, 8, 1)]
void main(uint2 p : SV_DispatchThreadID) {
  if (any(p >= size)) return;
#ifndef FILL_ALL
  if (motion[p].x < 20000.0) return;  // a draw wrote it
#endif
  float2 uv = (float2(p) + 0.5) / float2(size);
  float4 clip = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth.Load(int3(p, 0)), 1.0);
  float4 prev = mul(clip, clip_to_prev_clip);
  float2 prev_uv = float2(prev.x / prev.w * 0.5 + 0.5, 0.5 - prev.y / prev.w * 0.5);
  motion[p] = prev.w > 0.0 ? prev_uv - uv : float2(0.0, 0.0);
}
)";
// DLSS's depth (local change): the scene's depth as R32_FLOAT, FillMotion's bindings.
constexpr char kDepthCopyHlsl[] = R"(
Texture2D<float> depth : register(t0);
RWTexture2D<float> dst : register(u0);
cbuffer Constants : register(b0) { float4x4 unused; uint2 size; };
[numthreads(8, 8, 1)]
void main(uint2 p : SV_DispatchThreadID) {
  if (any(p >= size)) return;
  dst[p] = depth.Load(int3(p, 0));
}
)";
// FSR's frame generation's depth and motion at a size within the screen's (local change): each
// pixel the nearest (the largest reversed depth) of its block of the render's, and its motion
// (UV units: unchanged).
constexpr char kFgReduceHlsl[] = R"(
Texture2D<float> depth : register(t0);
Texture2D<float2> motion : register(t1);
RWTexture2D<float> dst_depth : register(u0);
RWTexture2D<float2> dst_motion : register(u1);
cbuffer Constants : register(b0) { uint2 src_size; uint2 dst_size; };
[numthreads(8, 8, 1)]
void main(uint2 p : SV_DispatchThreadID) {
  if (any(p >= dst_size)) return;
  uint2 lo = p * src_size / dst_size;
  uint2 hi = min(max(lo + 1, ((p + 1) * src_size + dst_size - 1) / dst_size), src_size);
  float best = -1.0;
  uint2 at = lo;
  for (uint y = lo.y; y < hi.y; ++y)
    for (uint x = lo.x; x < hi.x; ++x) {
      float z = depth.Load(int3(x, y, 0));
      if (z > best) { best = z; at = uint2(x, y); }
    }
  dst_depth[p] = best;
  dst_motion[p] = motion.Load(int3(at, 0));
}
)";
struct FgReduceConstants {
  uint32_t src_size[2];
  uint32_t dst_size[2];
};
struct FillConstants {
  float clip_to_prev_clip[16];  // HLSL float4x4 in a cbuffer is column-major: given transposed
  uint32_t size[2];
};

// The scene's color for FSR in the range of the 7e3 EDRAM format, as the resolves read it: RGB in
// [0, 31.875], alpha in [0, 1], NaN 0 (resolve.cpp's Quantize7e3, Xenia's UnclampedFloat32To7e3).
// The host target is R16G16B16A16_FLOAT and keeps what the shaders write beyond that, such as
// the start lights' emissive colors above 31.875.
constexpr char kInputHlsl[] = R"(
[numthreads(8, 8, 1)]
void main(uint2 p : SV_DispatchThreadID) {
  if (any(p >= size)) return;
  float4 c = src.Load(int3(p, 0));
  dst[p] = float4(clamp(c.rgb, 0.0, 31.875), saturate(c.a));
}
)";

// The scene's alpha into the upscaled color, which FSR leaves without it (the motion blur pass,
// pixel shader 27A3898925DDCAF7, masks the player's car by it): nearest to each output pixel.
constexpr char kAlphaHlsl[] = R"(
[numthreads(8, 8, 1)]
void main(uint2 p : SV_DispatchThreadID) {
  if (any(p >= size)) return;
  float4 c = dst[p];
  c.a = src.Load(int3(uint2((float2(p) + 0.5) * scale), 0)).a;
  dst[p] = c;
}
)";

// Contrast adaptive sharpening (local change; AMD FidelityFX CAS, MIT, without scaling): src a copy
// of the upscaled scene, dst the scene. Per pixel, the 3x3 neighbourhood's range sets how much the
// cross around it is subtracted: little where the contrast is already high (no halos), more on
// soft detail. scale.x the strength (0..1). In a tone-mapped space (x / (1 + x)), the scene being
// HDR, then back.
constexpr char kSharpenHlsl[] = R"(
float3 T(float3 c) { c = max(c, 0.0); return c / (1.0 + max(c.r, max(c.g, c.b))); }
float3 L(int2 p) { return T(src.Load(int3(clamp(p, int2(0, 0), int2(size) - 1), 0)).rgb); }
[numthreads(8, 8, 1)]
void main(uint2 p : SV_DispatchThreadID) {
  if (any(p >= size)) return;
  int2 q = int2(p);
  float4 center = src.Load(int3(q, 0));
  float3 a = L(q + int2(-1, -1)), b = L(q + int2(0, -1)), c = L(q + int2(1, -1));
  float3 d = L(q + int2(-1, 0)), e = T(center.rgb), f = L(q + int2(1, 0));
  float3 g = L(q + int2(-1, 1)), h = L(q + int2(0, 1)), i = L(q + int2(1, 1));
  float3 mn = min(min(min(d, e), min(f, b)), h);
  mn += min(mn, min(min(a, c), min(g, i)));
  float3 mx = max(max(max(d, e), max(f, b)), h);
  mx += max(mx, max(max(a, c), max(g, i)));
  float3 amp = sqrt(saturate(min(mn, 2.0 - mx) / max(mx, 1e-5)));
  float3 w = amp * (-1.0 / lerp(8.0, 5.0, saturate(scale.x)));
  float3 o = saturate((b * w + d * w + f * w + h * w + e) / (1.0 + 4.0 * w));
  // Back to the scene's range: x = t / (1 - peak(t)) for the tone map above.
  float peak = max(o.r, max(o.g, o.b));
  dst[p] = float4(o / max(1.0 - peak, 1.0 / 32.0), center.a);
}
)";

// The reactive mask (local change): src the finished scene (the input pass's), dst the scene
// before its transparents, which takes the mask. The change in a tone-mapped space (x / (1 + x)),
// as it shows on screen, times scale.x, at most scale.y.
constexpr char kReactiveHlsl[] = R"(
float Peak(float3 c) { return max(c.r, max(c.g, c.b)); }
[numthreads(8, 8, 1)]
void main(uint2 p : SV_DispatchThreadID) {
  if (any(p >= size)) return;
  float3 c = src.Load(int3(p, 0)).rgb;
  float3 o = dst[p].rgb;
  o = clamp(isnan(o) ? 0.0 : o, 0.0, 31.875);
  float m = Peak(abs(c / (1.0 + Peak(c)) - o / (1.0 + Peak(o))));
  m = min(saturate(m * scale.x), scale.y);
  dst[p] = float4(m, m, m, m);
}
)";

// The reactive mask shown (pgr4_native_reactive_show): red over the upscaled scene, nearest to
// each output pixel.
constexpr char kReactiveShowHlsl[] = R"(
[numthreads(8, 8, 1)]
void main(uint2 p : SV_DispatchThreadID) {
  if (any(p >= size)) return;
  float m = src.Load(int3(uint2((float2(p) + 0.5) * scale), 0)).r;
  float4 c = dst[p];
  dst[p] = float4(lerp(c.rgb, float3(4.0, 0.0, 0.0), m), c.a);
}
)";

// The render size of one axis from its setting, at least a quarter of the scene's and at most
// its output (the scene times the draw resolution scale: local change, pgr4 project).
uint32_t RenderExtent(int32_t setting, uint32_t scene) {
  return uint32_t(std::clamp<int32_t>(setting, int32_t(scene / 4),
                                      int32_t(scene * DrawResolutionScale())));
}

// DLSS Super Resolution's modes (local change): the render size's share of the output's per axis.
struct DlssModeInfo {
  const char* name;
  sl::DLSSMode mode;
  float ratio;
};
constexpr DlssModeInfo kDlssModes[] = {
    {"dlaa", sl::DLSSMode::eDLAA, 1.0f},
    {"quality", sl::DLSSMode::eMaxQuality, 1.0f / 1.5f},
    {"balanced", sl::DLSSMode::eBalanced, 1.0f / 1.7241f},
    {"performance", sl::DLSSMode::eMaxPerformance, 0.5f},
    {"ultra_performance", sl::DLSSMode::eUltraPerformance, 1.0f / 3.0f},
};
std::string Lower(std::string v) {
  for (char& ch : v) ch = char(std::tolower(static_cast<unsigned char>(ch)));
  return v;
}
// --pgr4_native_dlss_mode's, nullptr for auto.
const DlssModeInfo* DlssModeSetting() {
  const std::string v = Lower(REXCVAR_GET(pgr4_native_dlss_mode));
  for (const DlssModeInfo& m : kDlssModes)
    if (v == m.name) return &m;
  if (v != "auto" && !v.empty())
    PGR4_WARN_ONCE("PGR4 native DLSS: unknown mode '{}'; auto", v);
  return nullptr;
}
// --pgr4_native_dlss_preset's.
sl::DLSSPreset DlssPresetSetting() {
  const std::string v = Lower(REXCVAR_GET(pgr4_native_dlss_preset));
  if (v.size() == 1 && v[0] >= 'e' && v[0] <= 'o') return sl::DLSSPreset(v[0] - 'a' + 1);
  if (v != "default" && !v.empty())
    PGR4_WARN_ONCE("PGR4 native DLSS: preset '{}' unavailable (E to O); NVIDIA's default", v);
  return sl::DLSSPreset::eDefault;
}

// Element `index` (from 1) of the Halton sequence of `base`, in [0, 1) (DLSS's jitter).
float Halton(uint32_t index, uint32_t base) {
  float f = 1.0f, r = 0.0f;
  for (; index; index /= base) {
    f /= float(base);
    r += f * float(index % base);
  }
  return r;
}

}  // namespace

bool Upscaler::Requested() { return REXCVAR_GET(pgr4_native_upscaler); }

Upscaler::~Upscaler() {
  if (context_) api_->destroy(&context_, nullptr);
}

bool Upscaler::Initialize(ID3D12Device* device) {
  device_ = device;
  max_width_ = RenderExtent(REXCVAR_GET(pgr4_native_upscaler_width), kSceneWidth);
  max_height_ = RenderExtent(REXCVAR_GET(pgr4_native_upscaler_height), SceneHeight());
  // The quality mode (local change), DLSS's or FSR's: the render size from the output's.
  if (const DlssModeInfo* m = DlssModeSetting()) {
    max_width_ = RenderExtent(int32_t(std::lround(float(OutputWidth()) * m->ratio)), kSceneWidth);
    max_height_ =
        RenderExtent(int32_t(std::lround(float(OutputHeight()) * m->ratio)), SceneHeight());
  }
  if (const int32_t fps = REXCVAR_GET(pgr4_native_upscaler_target_fps); fps > 0) {
    const uint32_t min_height =
        RenderExtent(REXCVAR_GET(pgr4_native_upscaler_min_height), SceneHeight());
    const uint32_t min_level =
        (min_height * DynamicResolution::kSteps + max_height_ - 1) / max_height_;
    dynamic_.emplace(min_level, uint64_t(1'000'000'000 / fps));
  }
  // DLSS through Streamline (local change): no FidelityFX context.
  if (streamline::DlssRequested() && streamline::Active()) {
    dlss_ = true;
    REXLOG_INFO("PGR4 native upscaler: NVIDIA DLSS from {}x{}{} to {}x{}", max_width_, max_height_,
                dynamic_ ? " or less" : "", OutputWidth(), OutputHeight());
    return SetLevel(DynamicResolution::kSteps) && CreateTargets() && CreatePasses();
  }
  if (streamline::DlssRequested())
    REXLOG_WARN("PGR4 native upscaler: DLSS unavailable (see the Streamline lines); using FSR");
  api_ = LoadFfxApi();
  if (!api_ || !CreateContext(false)) return false;
  ffxQueryGetProviderVersion provider = {};
  provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
  if (api_->query(&context_, &provider.header) == FFX_API_RETURN_OK && provider.versionName)
    REXLOG_INFO("PGR4 native upscaler: {} from {}x{}{} to {}x{}", provider.versionName, max_width_,
                max_height_, dynamic_ ? " or less" : "", OutputWidth(), OutputHeight());
  return SetLevel(DynamicResolution::kSteps) && CreateTargets() && CreatePasses();
}

bool Upscaler::CreatePasses() {
  const D3D12_DESCRIPTOR_RANGE ranges[2] = {
      {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
      {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1},
  };
  D3D12_ROOT_PARAMETER params[2] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable = {2, ranges};
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants = {0, 0, sizeof(PassConstants) / 4};
  const D3D12_ROOT_SIGNATURE_DESC rs = {2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ComPtr<ID3DBlob> blob, err;
  const D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                         kViewsPerFrame * D3D12Device::kFramesInFlight,
                                         D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  if (Failed(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err),
             "upscaler pass root signature serialize") ||
      Failed(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                          IID_PPV_ARGS(&pass_signature_)),
             "upscaler pass root signature") ||
      Failed(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&pass_heap_)), "upscaler pass heap"))
    return false;
  pass_stride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

  const pD3DCompile compile = D3DCompiler();
  auto create = [&](const char* body, const char* name, ComPtr<ID3D12PipelineState>* pso) {
    const std::string hlsl = std::string(kPassBindings) + body;
    ComPtr<ID3DBlob> code, error;
    if (!compile || FAILED(compile(hlsl.data(), hlsl.size(), name, nullptr, nullptr, "main",
                                   "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error))) {
      REXLOG_ERROR("PGR4 native upscaler: {}: {}", name,
                   error ? static_cast<const char*>(error->GetBufferPointer()) : "?");
      return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = pass_signature_.Get();
    pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    return !Failed(device_->CreateComputePipelineState(&pd, IID_PPV_ARGS(pso->GetAddressOf())),
                   name);
  };
  if (!create(kInputHlsl, "pgr4_upscaler_input.hlsl", &input_pso_)) return false;
  D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = {kOutputFormat};
  if (FAILED(device_->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs))) ||
      !(fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD)) {
    REXLOG_WARN(
        "PGR4 native upscaler: no typed UAV loads of the output; the motion blur loses "
        "its mask");
    return true;
  }
  if (!create(kAlphaHlsl, "pgr4_upscaler_alpha.hlsl", &alpha_pso_)) return false;
  if (!create(kSharpenHlsl, "pgr4_upscaler_sharpen.hlsl", &sharpen_pso_)) sharpen_pso_.Reset();
  if (!create(kReactiveHlsl, "pgr4_upscaler_reactive.hlsl", &reactive_pso_)) reactive_pso_.Reset();
  if (!create(kReactiveShowHlsl, "pgr4_upscaler_reactive_show.hlsl", &reactive_show_pso_))
    reactive_show_pso_.Reset();

  // The motion fill (local change): its own root signature, the matrix being larger than
  // PassConstants. Without typed UAV loads of the motion format the motion clears to 0 as before.
  D3D12_FEATURE_DATA_FORMAT_SUPPORT ms = {kMotionFormat};
  if (FAILED(device_->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &ms, sizeof(ms))) ||
      !(ms.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD)) {
    REXLOG_WARN("PGR4 native upscaler: no typed UAV loads of the motion; no camera motion fill");
    return true;
  }
  D3D12_ROOT_PARAMETER fp[2] = {};
  fp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  fp[0].DescriptorTable = {2, ranges};
  fp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  fp[1].Constants = {0, 0, sizeof(FillConstants) / 4};
  const D3D12_ROOT_SIGNATURE_DESC frs = {2, fp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ComPtr<ID3DBlob> fblob, ferr;
  if (Failed(D3D12SerializeRootSignature(&frs, D3D_ROOT_SIGNATURE_VERSION_1, &fblob, &ferr),
             "upscaler motion fill root signature serialize") ||
      Failed(device_->CreateRootSignature(0, fblob->GetBufferPointer(), fblob->GetBufferSize(),
                                          IID_PPV_ARGS(&fill_signature_)),
             "upscaler motion fill root signature"))
    return true;
  ComPtr<ID3DBlob> code, error;
  if (!compile || FAILED(compile(kFillMotionHlsl, sizeof(kFillMotionHlsl) - 1,
                                 "pgr4_upscaler_fill_motion.hlsl", nullptr, nullptr, "main",
                                 "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error))) {
    REXLOG_ERROR("PGR4 native upscaler: motion fill: {}",
                 error ? static_cast<const char*>(error->GetBufferPointer()) : "?");
    return true;
  }
  D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
  pd.pRootSignature = fill_signature_.Get();
  pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
  if (Failed(device_->CreateComputePipelineState(&pd, IID_PPV_ARGS(&fill_pso_)),
             "upscaler motion fill"))
    fill_pso_.Reset();
  {
    ComPtr<ID3DBlob> copy_code, copy_error;
    if (SUCCEEDED(compile(kDepthCopyHlsl, sizeof(kDepthCopyHlsl) - 1, "pgr4_upscaler_depth.hlsl",
                          nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                          &copy_code, &copy_error))) {
      D3D12_COMPUTE_PIPELINE_STATE_DESC cd = pd;
      cd.CS = {copy_code->GetBufferPointer(), copy_code->GetBufferSize()};
      if (Failed(device_->CreateComputePipelineState(&cd, IID_PPV_ARGS(&depth_copy_pso_)),
                 "upscaler depth copy"))
        depth_copy_pso_.Reset();
    }
  }
  {
    // The frame generation's reduction (local change): two views read, two written.
    const D3D12_DESCRIPTOR_RANGE rr[2] = {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 2},
    };
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable = {2, rr};
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[1].Constants = {0, 0, sizeof(FgReduceConstants) / 4};
    const D3D12_ROOT_SIGNATURE_DESC rrs = {2, rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> rblob, rerr, rcode, rerror;
    if (SUCCEEDED(D3D12SerializeRootSignature(&rrs, D3D_ROOT_SIGNATURE_VERSION_1, &rblob, &rerr)) &&
        !Failed(device_->CreateRootSignature(0, rblob->GetBufferPointer(), rblob->GetBufferSize(),
                                             IID_PPV_ARGS(&fg_reduce_signature_)),
                "upscaler frame generation reduction root signature") &&
        SUCCEEDED(compile(kFgReduceHlsl, sizeof(kFgReduceHlsl) - 1, "pgr4_upscaler_fg_reduce.hlsl",
                          nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                          &rcode, &rerror))) {
      D3D12_COMPUTE_PIPELINE_STATE_DESC rd = {};
      rd.pRootSignature = fg_reduce_signature_.Get();
      rd.CS = {rcode->GetBufferPointer(), rcode->GetBufferSize()};
      if (Failed(device_->CreateComputePipelineState(&rd, IID_PPV_ARGS(&fg_reduce_pso_)),
                 "upscaler frame generation reduction"))
        fg_reduce_pso_.Reset();
    } else {
      REXLOG_ERROR("PGR4 native upscaler: frame generation reduction: {}",
                   rerror ? static_cast<const char*>(rerror->GetBufferPointer()) : "?");
    }
  }
  if (fill_pso_ && REXCVAR_GET(pgr4_native_motion_dump) > 0) {
    // The diagnostic's camera motion of every pixel (FILL_ALL).
    const D3D_SHADER_MACRO all[] = {{"FILL_ALL", "1"}, {nullptr, nullptr}};
    ComPtr<ID3DBlob> all_code, all_error;
    if (SUCCEEDED(compile(kFillMotionHlsl, sizeof(kFillMotionHlsl) - 1,
                          "pgr4_upscaler_camera_motion.hlsl", all, nullptr, "main", "cs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &all_code, &all_error))) {
      pd.CS = {all_code->GetBufferPointer(), all_code->GetBufferSize()};
      if (Failed(device_->CreateComputePipelineState(&pd, IID_PPV_ARGS(&camera_motion_pso_)),
                 "upscaler camera motion"))
        camera_motion_pso_.Reset();
    }
  }
  return true;
}

void Upscaler::UpdateCamera() {
  auto to_sl = [](const float* m) {
    sl::float4x4 t;
    for (int i = 0; i < 4; ++i) t[i] = {m[4 * i], m[4 * i + 1], m[4 * i + 2], m[4 * i + 3]};
    return t;
  };
  auto from_sl = [](const sl::float4x4& t, float* m) {
    for (int i = 0; i < 4; ++i) {
      m[4 * i] = t[i].x;
      m[4 * i + 1] = t[i].y;
      m[4 * i + 2] = t[i].z;
      m[4 * i + 3] = t[i].w;
    }
  };
  // PGR4's perspective with reversed depth (CameraFromClipRows): z_ndc = 1 at near, 0 at far.
  const float aspect = float(kSceneWidth) / float(SceneHeight());
  const float n = camera_->near_plane, f = camera_->far_plane;
  const float h = 1.0f / std::tan(0.5f * camera_->fov_y), w = h / aspect;
  const float v2c[16] = {w, 0, 0, 0, 0, h, 0, 0, 0, 0, n / (n - f), 1, 0, 0, f * n / (f - n), 0};
  std::copy(v2c, v2c + 16, view_to_clip_);
  sl::float4x4 clip_to_view;
  sl::matrixFullInvert(clip_to_view, to_sl(view_to_clip_));
  from_sl(clip_to_view, clip_to_view_);
  static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  std::copy(kIdentity, kIdentity + 16, clip_to_prev_clip_);
  std::copy(kIdentity, kIdentity + 16, prev_clip_to_clip_);
  if (view_projection_frame_ != frame_) {  // no transform this frame: a still camera
    static int missing_logged = 0;  // local diagnostic
    if (has_previous_view_projection_ && missing_logged < 200) {
      ++missing_logged;
      REXLOG_WARN("PGR4 native camera diag: frame {} without the scene's camera", frame_);
    }
    has_previous_view_projection_ = false;
    return;
  }
  // The game's constants take column vectors (clip = rows . world), Streamline's matrices row
  // vectors (v' = v * M): transposed. View = WorldToClip * ClipToView; the camera's axes and
  // position are the rows of the view's inverse.
  auto transposed = [](const float* rows) {
    sl::float4x4 t;
    for (int i = 0; i < 4; ++i) t[i] = {rows[i], rows[4 + i], rows[8 + i], rows[12 + i]};
    return t;
  };
  const sl::float4x4 world_to_clip = transposed(view_projection_);
  sl::float4x4 world_to_view, view_to_world;
  sl::matrixMul(world_to_view, world_to_clip, clip_to_view);
  sl::matrixFullInvert(view_to_world, world_to_view);
  float* const axes[3] = {camera_right_, camera_up_, camera_fwd_};
  for (int r = 0; r < 3; ++r) {
    sl::float3 v(view_to_world[r].x, view_to_world[r].y, view_to_world[r].z);
    sl::vectorNormalize(v);
    axes[r][0] = v.x;
    axes[r][1] = v.y;
    axes[r][2] = v.z;
  }
  camera_pos_[0] = view_to_world[3].x;
  camera_pos_[1] = view_to_world[3].y;
  camera_pos_[2] = view_to_world[3].z;
  if (has_previous_view_projection_) {
    sl::float4x4 clip_to_world, to_prev, from_prev;
    sl::matrixFullInvert(clip_to_world, world_to_clip);
    sl::matrixMul(to_prev, clip_to_world, transposed(previous_view_projection_));
    sl::matrixFullInvert(from_prev, to_prev);
    from_sl(to_prev, clip_to_prev_clip_);
    // Local change: a camera jump, points 10 m ahead moving over a quarter of the screen in a
    // frame (in screen units: a threshold in render pixels, 150, fired at 4K render several times
    // a second in fast driving, which reset DLSS and skipped DLSS-G's frames, 120 to 60 FPS).
    // Fast driving moves them about 5 % of the screen, the replays' and race starts' cuts 50 %.
    static int jump_logged = 0;
    float most = 0.0f;
    for (float y = -0.8f; y <= 0.81f; y += 0.8f)
      for (float x = -0.8f; x <= 0.81f; x += 0.8f) {
        const float z = camera_->near_plane / 10.0f;
        float o[4];
        for (int j = 0; j < 4; ++j)
          o[j] = x * clip_to_prev_clip_[j] + y * clip_to_prev_clip_[4 + j] +
                 z * clip_to_prev_clip_[8 + j] + clip_to_prev_clip_[12 + j];
        if (o[3] <= 0.0f) {
          most = 1e9f;
          continue;
        }
        most = std::max(most, 0.5f * std::hypot(o[0] / o[3] - x, o[1] / o[3] - y));
      }
    // A camera cut (no turn moves a point 10 m ahead that far in a frame): the history restarts,
    // for DLSS and for frame generation (Constants::reset).
    constexpr float kCutShare = 0.25f;
    if (most > kCutShare) reset_ = true;
    if (most > kCutShare && jump_logged < 200) {
      ++jump_logged;
      REXLOG_WARN("PGR4 native camera diag: frame {} camera cut ({:.0f} % of the screen)", frame_,
                  std::min(most, 99.99f) * 100.0f);
    }
    from_sl(from_prev, prev_clip_to_clip_);
  }
  std::copy(view_projection_, view_projection_ + 16, previous_view_projection_);
  has_previous_view_projection_ = true;
}

void Upscaler::FillMotion(ID3D12GraphicsCommandList* list, RenderTarget* depth,
                          ID3D12Resource* target, ID3D12PipelineState* pso, uint32_t slot,
                          DXGI_FORMAT target_format) {
  // Views in the frame's slot `slot` (RunPass has 0 and 1).
  const uint32_t first =
      uint32_t(frame_ % D3D12Device::kFramesInFlight) * kViewsPerFrame + 2 * slot;
  D3D12_CPU_DESCRIPTOR_HANDLE cpu = pass_heap_->GetCPUDescriptorHandleForHeapStart();
  cpu.ptr += size_t(first) * pass_stride_;
  D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
  sd.Format = depth->srv_format;
  sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  sd.Texture2D.MipLevels = 1;
  device_->CreateShaderResourceView(depth->resource.Get(), &sd, cpu);
  cpu.ptr += pass_stride_;
  D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
  ud.Format = target_format;
  ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  device_->CreateUnorderedAccessView(target, nullptr, &ud, cpu);
  D3D12_GPU_DESCRIPTOR_HANDLE gpu = pass_heap_->GetGPUDescriptorHandleForHeapStart();
  gpu.ptr += uint64_t(first) * pass_stride_;

  FillConstants c = {};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) c.clip_to_prev_clip[4 * j + i] = clip_to_prev_clip_[4 * i + j];
  c.size[0] = render_width_;
  c.size[1] = render_height_;
  ID3D12DescriptorHeap* heaps[] = {pass_heap_.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(fill_signature_.Get());
  list->SetPipelineState(pso);
  list->SetComputeRootDescriptorTable(0, gpu);
  list->SetComputeRoot32BitConstants(1, sizeof(c) / 4, &c, 0);
  list->Dispatch((render_width_ + 7) / 8, (render_height_ + 7) / 8, 1);
}

void Upscaler::DumpMotion(ID3D12GraphicsCommandList* list, RenderTarget* depth) {
  const int32_t every = REXCVAR_GET(pgr4_native_motion_dump);
  if (every <= 0 || !camera_motion_pso_) return;
  // A dump from a few frames ago: its copies have run.
  if (dump_pending_ != UINT64_MAX && frame_ >= dump_pending_ + 2 * D3D12Device::kFramesInFlight) {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path(exe);
    path = path.substr(0, path.find_last_of(L"\\/") + 1) + L"motion_dump_" +
           std::to_wstring(dump_pending_) + L".bin";
    if (std::FILE* f = _wfopen(path.c_str(), L"wb")) {
      const uint32_t header[3] = {dump_width_, dump_height_, dump_layout_.Footprint.RowPitch};
      std::fwrite(header, sizeof(header), 1, f);
      std::fwrite(dump_matrix_, sizeof(dump_matrix_), 1, f);
      const size_t bytes = size_t(dump_layout_.Footprint.RowPitch) * dump_height_;
      for (auto& rb : dump_readback_) {
        void* p = nullptr;
        const D3D12_RANGE range = {0, bytes};
        if (SUCCEEDED(rb->Map(0, &range, &p))) {
          std::fwrite(p, bytes, 1, f);
          const D3D12_RANGE none = {0, 0};
          rb->Unmap(0, &none);
        }
      }
      std::fclose(f);
      REXLOG_INFO("PGR4 native: motion dump of frame {} written", dump_pending_);
    }
    dump_pending_ = UINT64_MAX;
  }
  if (dump_pending_ != UINT64_MAX || dumps_ >= 8 || frame_ % uint64_t(every) != 0) return;

  if (!dump_camera_) {
    D3D12_RESOURCE_DESC d = motion_->GetDesc();
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, dump_camera_state_,
                                                nullptr, IID_PPV_ARGS(&dump_camera_)),
               "motion dump camera target"))
      return;
    UINT rows = 0;
    UINT64 row_bytes = 0, total = 0;
    device_->GetCopyableFootprints(&d, 0, 1, 0, &dump_layout_, &rows, &row_bytes, &total);
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC b = {};
    b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    b.Width = total;
    b.Height = b.DepthOrArraySize = b.MipLevels = 1;
    b.SampleDesc.Count = 1;
    b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for (auto& rb : dump_readback_)
      if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &b,
                                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                  IID_PPV_ARGS(&rb)),
                 "motion dump readback"))
        return;
  }
  // The camera's motion of every pixel, then both targets to the readbacks.
  if (dump_camera_state_ != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
    const D3D12_RESOURCE_BARRIER b = TransitionBarrier(dump_camera_.Get(), dump_camera_state_,
                                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->ResourceBarrier(1, &b);
  }
  FillMotion(list, depth, dump_camera_.Get(), camera_motion_pso_.Get(), 3);
  D3D12_RESOURCE_BARRIER bs[2] = {
      TransitionBarrier(dump_camera_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_COPY_SOURCE),
      TransitionBarrier(motion_.Get(), motion_state_, D3D12_RESOURCE_STATE_COPY_SOURCE)};
  list->ResourceBarrier(2, bs);
  dump_camera_state_ = motion_state_ = D3D12_RESOURCE_STATE_COPY_SOURCE;
  ID3D12Resource* const sources[2] = {motion_.Get(), dump_camera_.Get()};
  for (int i = 0; i < 2; ++i) {
    D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
    src.pResource = sources[i];
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = dump_readback_[i].Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = dump_layout_;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
  std::copy(clip_to_prev_clip_, clip_to_prev_clip_ + 16, dump_matrix_);
  dump_width_ = render_width_;
  dump_height_ = render_height_;
  dump_pending_ = frame_;
  ++dumps_;
}

bool Upscaler::ReduceForFrameGeneration(ID3D12GraphicsCommandList* list, RenderTarget* depth,
                                        uint32_t width, uint32_t height) {
  if (!fg_reduce_pso_) return false;
  std::erase_if(fg_retired_, [&](const auto& r) {
    return frame_ >= r.first + 2 * D3D12Device::kFramesInFlight;
  });
  if (!fg_depth_ || fg_size_[0] != width || fg_size_[1] != height) {
    if (fg_depth_) fg_retired_.push_back({frame_, fg_depth_});
    if (fg_motion_) fg_retired_.push_back({frame_, fg_motion_});
    fg_depth_.Reset();
    fg_motion_.Reset();
    const D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = width;
    rd.Height = height;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R32_FLOAT;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                IID_PPV_ARGS(&fg_depth_)),
               "frame generation depth"))
      return false;
    rd.Format = kMotionFormat;
    if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                IID_PPV_ARGS(&fg_motion_)),
               "frame generation motion")) {
      fg_depth_.Reset();
      return false;
    }
    fg_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    fg_size_[0] = width;
    fg_size_[1] = height;
    REXLOG_INFO("PGR4 native frame generation: depth and motion reduced from {}x{} to {}x{}",
                render_width_, render_height_, width, height);
  }
  if (fg_state_ != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
    const D3D12_RESOURCE_BARRIER bs[2] = {
        TransitionBarrier(fg_depth_.Get(), fg_state_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        TransitionBarrier(fg_motion_.Get(), fg_state_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
    list->ResourceBarrier(2, bs);
  }
  // Views in the frame's slots 8 and 9: the depth's and motion's, then the reduced ones'.
  const uint32_t first = uint32_t(frame_ % D3D12Device::kFramesInFlight) * kViewsPerFrame + 16;
  D3D12_CPU_DESCRIPTOR_HANDLE cpu = pass_heap_->GetCPUDescriptorHandleForHeapStart();
  cpu.ptr += size_t(first) * pass_stride_;
  D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
  sd.Format = depth->srv_format;
  sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  sd.Texture2D.MipLevels = 1;
  device_->CreateShaderResourceView(depth->resource.Get(), &sd, cpu);
  cpu.ptr += pass_stride_;
  sd.Format = kMotionFormat;
  device_->CreateShaderResourceView(motion_.Get(), &sd, cpu);
  cpu.ptr += pass_stride_;
  D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
  ud.Format = DXGI_FORMAT_R32_FLOAT;
  ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  device_->CreateUnorderedAccessView(fg_depth_.Get(), nullptr, &ud, cpu);
  cpu.ptr += pass_stride_;
  ud.Format = kMotionFormat;
  device_->CreateUnorderedAccessView(fg_motion_.Get(), nullptr, &ud, cpu);
  D3D12_GPU_DESCRIPTOR_HANDLE gpu = pass_heap_->GetGPUDescriptorHandleForHeapStart();
  gpu.ptr += uint64_t(first) * pass_stride_;
  const FgReduceConstants c = {{render_width_, render_height_}, {width, height}};
  ID3D12DescriptorHeap* heaps[] = {pass_heap_.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(fg_reduce_signature_.Get());
  list->SetPipelineState(fg_reduce_pso_.Get());
  list->SetComputeRootDescriptorTable(0, gpu);
  list->SetComputeRoot32BitConstants(1, sizeof(c) / 4, &c, 0);
  list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
  const D3D12_RESOURCE_BARRIER bs[2] = {
      TransitionBarrier(fg_depth_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kInputState),
      TransitionBarrier(fg_motion_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kInputState)};
  list->ResourceBarrier(2, bs);
  fg_state_ = kInputState;
  return true;
}

void Upscaler::RunPass(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso,
                       ID3D12Resource* src, ID3D12Resource* dst, uint32_t slot, float scale_x,
                       float scale_y, uint32_t width, uint32_t height) {
  // In the frame's own views: those of a frame kFramesInFlight back, whose fence the device
  // waited for, are free.
  const uint32_t first =
      uint32_t(frame_ % D3D12Device::kFramesInFlight) * kViewsPerFrame + 2 * slot;
  D3D12_CPU_DESCRIPTOR_HANDLE cpu = pass_heap_->GetCPUDescriptorHandleForHeapStart();
  cpu.ptr += size_t(first) * pass_stride_;
  D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
  sd.Format = kOutputFormat;
  sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  sd.Texture2D.MipLevels = 1;
  device_->CreateShaderResourceView(src, &sd, cpu);
  cpu.ptr += pass_stride_;
  D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
  ud.Format = kOutputFormat;
  ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  device_->CreateUnorderedAccessView(dst, nullptr, &ud, cpu);
  D3D12_GPU_DESCRIPTOR_HANDLE gpu = pass_heap_->GetGPUDescriptorHandleForHeapStart();
  gpu.ptr += uint64_t(first) * pass_stride_;

  const PassConstants c = {{scale_x, scale_y}, {width, height}};
  ID3D12DescriptorHeap* heaps[] = {pass_heap_.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(pass_signature_.Get());
  list->SetPipelineState(pso);
  list->SetComputeRootDescriptorTable(0, gpu);
  list->SetComputeRoot32BitConstants(1, sizeof(c) / 4, &c, 0);
  list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
}

uint32_t Upscaler::mip_bias_quarters() const {
  const int32_t q = int32_t(mip_bias_quarters_) -
                    int32_t(std::lround(4.0 * REXCVAR_GET(pgr4_native_upscaler_lod_offset)));
  return uint32_t(std::clamp(q, 0, 15));
}

bool Upscaler::SetLevel(uint32_t level) {
  constexpr uint32_t n = DynamicResolution::kSteps;
  render_width_ = std::max(1u, (max_width_ * level + n / 2) / n);
  render_height_ = std::max(1u, (max_height_ * level + n / 2) / n);
  // AMD's recommended texture LOD bias for the render size (FSR docs, "Mipmap biasing"),
  // in the sampler key's quarter steps.
  const float ratio = std::min(float(render_width_) / float(OutputWidth()),
                               float(render_height_) / float(OutputHeight()));
  mip_bias_quarters_ = uint32_t(std::lround(-4.0f * (std::log2(ratio) - 1.0f)));
  if (dlss_) {
    // NVIDIA's recommendation: 8 phases per output pixel per render pixel (DLSS guide).
    phase_count_ = std::max(8, int32_t(std::ceil(8.0f / (ratio * ratio))));
    return true;
  }
  ffxQueryDescUpscaleGetJitterPhaseCount phases = {};
  phases.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
  phases.renderWidth = render_width_;
  phases.displayWidth = OutputWidth();
  int32_t count = 0;
  phases.pOutPhaseCount = &count;
  if (api_->query(&context_, &phases.header) != FFX_API_RETURN_OK || count <= 0) {
    PGR4_WARN_ONCE("PGR4 native upscaler: no jitter phase count for {}x{}", render_width_,
                   render_height_);
    return false;
  }
  phase_count_ = count;
  return true;
}

bool Upscaler::CreateContext(bool debug) {
  if (context_) {
    api_->destroy(&context_, nullptr);
    context_ = nullptr;
  }
  ffxCreateBackendDX12Desc backend = {};
  backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
  backend.device = device_;
  ffxCreateContextDescUpscaleVersion version = {};
  version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
  version.header.pNext = &backend.header;
  version.version = FFX_UPSCALER_VERSION;
  ffxCreateContextDescUpscale desc = {};
  desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
  desc.header.pNext = &version.header;
  // The scene is linear light in a float target, before the game's exposure and tone mapping,
  // and its depth is reversed (cleared to 0, tested GEQUAL).
  desc.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE |
               FFX_UPSCALE_ENABLE_DEPTH_INVERTED |
               (dynamic_ ? FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION : 0u) |
               (debug ? FFX_UPSCALE_ENABLE_DEBUG_VISUALIZATION : 0u);
  desc.maxRenderSize = {max_width_, max_height_};
  desc.maxUpscaleSize = {OutputWidth(), OutputHeight()};
  desc.fpMessage = Message;
  if (const ffxReturnCode_t rc = api_->create(&context_, &desc.header, nullptr);
      rc != FFX_API_RETURN_OK) {
    REXLOG_ERROR("PGR4 native upscaler: ffxCreateContext failed ({}){}", rc,
                 debug ? " with debug visualization" : "");
    context_ = nullptr;
    return false;
  }
  debug_context_ = debug;
  reset_ = true;
  return true;
}

bool Upscaler::CreateTargets() {
  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC d = {};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width = OutputWidth();
  d.Height = OutputHeight();
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.Format = kOutputFormat;
  d.SampleDesc.Count = 1;
  d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                              IID_PPV_ARGS(&output_.resource)),
             "upscaler output"))
    return false;
  output_.resource_format = output_.view_format = output_.srv_format = kOutputFormat;
  output_.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  // Guest pixels; the resource is the draw resolution scale times them, as the other targets
  // (local change, pgr4 project): the resolves read it at that scale.
  output_.width = kSceneWidth;
  output_.height = SceneHeight();
  output_.scale = output_.size_scale = UniformScale(DrawResolutionScale());

  d.Width = max_width_;
  d.Height = max_height_;
  if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, input_state_, nullptr,
                                              IID_PPV_ARGS(&input_)),
             "upscaler input"))
    return false;

  d.Format = kMotionFormat;
  // Unordered access for the camera motion fill (local change).
  d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  D3D12_CLEAR_VALUE zero = {};
  zero.Format = kMotionFormat;
  if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, motion_state_, &zero,
                                              IID_PPV_ARGS(&motion_)),
             "upscaler motion vectors"))
    return false;
  D3D12_DESCRIPTOR_HEAP_DESC hd = {};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  hd.NumDescriptors = 1;
  if (Failed(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&motion_rtv_)), "upscaler RTV heap"))
    return false;
  device_->CreateRenderTargetView(motion_.Get(), nullptr,
                                  motion_rtv_->GetCPUDescriptorHandleForHeapStart());
  return true;
}

bool Upscaler::BeginFrame(uint64_t frame, uint64_t gpu_busy_ns) {
  frame_ = frame;
  bool resized = false;
  if (dynamic_) {
    const uint32_t level = dynamic_->level();
    if (dynamic_->Update(gpu_busy_ns) != level) {
      SetLevel(dynamic_->level());  // without a phase count, the jitter keeps the old size's
      resized = true;
    }
  }
  const uint64_t now = NowNs();
  frame_ms_ = last_ns_ ? float(double(now - last_ns_) * 1e-6) : 0.0f;
  last_ns_ = now;
  if (dlss_) {
    // Halton (2, 3) in [-0.5, 0.5) render pixels, as FSR's.
    const uint32_t i = uint32_t(frame % uint64_t(phase_count_)) + 1;
    jitter_[0] = Halton(i, 2) - 0.5f;
    jitter_[1] = Halton(i, 3) - 0.5f;
    return resized;
  }
  ffxQueryDescUpscaleGetJitterOffset q = {};
  q.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET;
  q.index = int32_t(frame % uint64_t(phase_count_));
  q.phaseCount = phase_count_;
  q.pOutX = &jitter_[0];
  q.pOutY = &jitter_[1];
  if (api_->query(&context_, &q.header) != FFX_API_RETURN_OK) jitter_[0] = jitter_[1] = 0.0f;
  return resized;
}

bool Upscaler::SetDebugView(bool on) {
  if (on && dlss_) return false;  // FSR's view only
  if (on && !debug_context_ && !CreateContext(true)) return false;
  debug_view_ = on;
  return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE Upscaler::MotionTarget(ID3D12GraphicsCommandList* list) {
  const D3D12_CPU_DESCRIPTOR_HANDLE rtv = motion_rtv_->GetCPUDescriptorHandleForHeapStart();
  if (motion_state_ != D3D12_RESOURCE_STATE_RENDER_TARGET) {
    const D3D12_RESOURCE_BARRIER b =
        TransitionBarrier(motion_.Get(), motion_state_, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->ResourceBarrier(1, &b);
    motion_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
  }
  if (motion_frame_ != frame_) {
    motion_frame_ = frame_;
    // No motion yet; with the fill (local change) a marker FillMotion replaces by camera motion.
    const float none = fill_pso_ ? kNoMotion : 0.0f;
    const float clear[4] = {none, none, 0.0f, 0.0f};
    list->ClearRenderTargetView(rtv, clear, 0, nullptr);
  }
  return rtv;
}

RenderTarget* Upscaler::Upscale(ID3D12GraphicsCommandList* list, RenderTargets& targets,
                                const SurfaceRegs& regs) {
  if (upscaled_frame_ == frame_) return upscaled_ ? &output_ : nullptr;
  upscaled_frame_ = frame_;
  upscaled_ = false;

  RenderTarget* color = targets.GetColor(regs, 0, SceneHeight());
  RenderTarget* depth = targets.GetDepth(regs, SceneHeight());
  if (!color || !depth || color->samples != 1 || depth->samples != 1 || color->scale != scale() ||
      depth->scale != scale() || color->srv_format != kOutputFormat) {
    PGR4_WARN_ONCE("PGR4 native upscaler: unexpected scene targets (surface {:08X} color {:08X})",
                   regs.surface_info, regs.color_info[0]);
    return nullptr;
  }
  const D3D12_RECT all = {0, 0, LONG(kSceneWidth), LONG(SceneHeight())};
  targets.PrepareRead(list, color, all);
  targets.PrepareRead(list, depth, all);

  D3D12_RESOURCE_BARRIER bs[5];
  uint32_t nb = 0;
  auto to = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES* state, D3D12_RESOURCE_STATES after) {
    if (*state == after) return;
    bs[nb++] = TransitionBarrier(r, *state, after);
    *state = after;
  };
  MotionTarget(list);  // cleared if no draw wrote motion this frame
  UpdateCamera();      // local change: the frame's camera, for the fill and DLSS
  to(color->resource.Get(), &color->state, kInputState);
  to(depth->resource.Get(), &depth->state, kInputState);
  if (fill_pso_) {
    // The camera's motion where no draw wrote any (local change), before the inputs are read.
    to(motion_.Get(), &motion_state_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (nb) list->ResourceBarrier(nb, bs);
    nb = 0;
    FillMotion(list, depth, motion_.Get(), fill_pso_.Get(), 2);
    DumpMotion(list, depth);
    if (dump_pending_ == frame_) {  // just dumped: the scene's color, then the next back buffers
      frame_dump::Capture(device_, list, color->resource.Get(), kInputState,
                          L"motion_dump_" + std::to_wstring(frame_) + L"_scene");
      frame_dump::armed_id = frame_;
      frame_dump::armed_swaps = 3;
    }
  }
  to(motion_.Get(), &motion_state_, kInputState);
  to(input_.Get(), &input_state_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  to(output_.resource.Get(), &output_.state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (nb) list->ResourceBarrier(nb, bs);
  RunPass(list, input_pso_.Get(), color->resource.Get(), input_.Get(), 0, 1.0f, 1.0f, render_width_,
          render_height_);
  nb = 0;
  to(input_.Get(), &input_state_, kInputState);
  list->ResourceBarrier(nb, bs);

  // The reactive mask (local change), from the copy before this frame's transparents.
  const bool reactive = reactive_pso_ && opaque_ && opaque_frame_ == frame_ &&
                        REXCVAR_GET(pgr4_native_reactive_mask) &&
                        (!dlss_ || REXCVAR_GET(pgr4_native_reactive_dlss));
  if (reactive) {
    if (opaque_state_ != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
      const D3D12_RESOURCE_BARRIER b = TransitionBarrier(opaque_.Get(), opaque_state_,
                                                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      list->ResourceBarrier(1, &b);
    }
    RunPass(list, reactive_pso_.Get(), input_.Get(), opaque_.Get(), 5,
            float(REXCVAR_GET(pgr4_native_reactive_scale)),
            float(REXCVAR_GET(pgr4_native_reactive_max)), render_width_, render_height_);
    const D3D12_RESOURCE_BARRIER b =
        TransitionBarrier(opaque_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kInputState);
    list->ResourceBarrier(1, &b);
    opaque_state_ = kInputState;
    PGR4_LOG_FIRST(1, INFO, "PGR4 native upscaler: reactive mask of the transparents at frame {}",
                   frame_);
  }
  reactive_ = reactive;

  if (dlss_) {
    if (!EvaluateDlss(list, depth)) return nullptr;
  } else {
  ffxDispatchDescUpscale d = {};
  d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
  d.commandList = list;
  d.color = ffxApiGetResourceDX12(input_.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
  d.depth = ffxApiGetResourceDX12(depth->resource.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
  d.motionVectors = ffxApiGetResourceDX12(motion_.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
  if (reactive)
    d.reactive = ffxApiGetResourceDX12(opaque_.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
  d.output = ffxApiGetResourceDX12(output_.resource.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
  d.jitterOffset = {jitter_[0], jitter_[1]};
  d.motionVectorScale = {float(render_width_), float(render_height_)};
  d.renderSize = {render_width_, render_height_};
  d.upscaleSize = {OutputWidth(), OutputHeight()};
  d.enableSharpening = false;
  if (debug_view_) d.flags = FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW;
  d.frameTimeDelta = frame_ms_;
  d.preExposure = 1.0f;
  d.reset = reset_;
  d.cameraNear = camera_->near_plane;
  d.cameraFar = camera_->far_plane;
  d.cameraFovAngleVertical = camera_->fov_y;
  d.viewSpaceToMetersFactor = 1.0f;
  if (const ffxReturnCode_t rc = api_->dispatch(&context_, &d.header); rc != FFX_API_RETURN_OK) {
    PGR4_WARN_ONCE("PGR4 native upscaler: ffxDispatch failed ({})", rc);
    return nullptr;
  }
  // Local change (pgr4 project): no frame generation while the game plays a video (Bink, its
  // frames are not the scene's); the frame is presented alone.
  if (frame_generation_ && rex::system::XFile::OpenMovies() == 0) {
    // Local change (pgr4 project): a render size above the screen's (a draw resolution scale
    // past it) reduced to one within it first, which the frame generation is made for.
    const uint32_t dw = frame_generation_->display_width(),
                   dh = frame_generation_->display_height();
    const double fit = dw && dh ? std::min(double(dw) / render_width_, double(dh) / render_height_)
                                : 1.0;
    const uint32_t fw = std::max(1u, uint32_t(render_width_ * fit)),
                   fh = std::max(1u, uint32_t(render_height_ * fit));
    if (fit < 1.0 && ReduceForFrameGeneration(list, depth, fw, fh)) {
      const FfxApiResource fd =
          ffxApiGetResourceDX12(fg_depth_.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
      const FfxApiResource fm =
          ffxApiGetResourceDX12(fg_motion_.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
      frame_generation_->Prepare(
          frame_, list, fd, fm, {fw, fh},
          {d.jitterOffset.x * float(fw) / float(render_width_),
           d.jitterOffset.y * float(fh) / float(render_height_)},
          {float(fw), float(fh)}, *camera_, frame_ms_, reset_);
    } else {
      frame_generation_->Prepare(frame_, list, d.depth, d.motionVectors, d.renderSize,
                                 d.jitterOffset, d.motionVectorScale, *camera_, frame_ms_, reset_);
    }
  }
  }  // FSR
  Sharpen(list);
  if (reactive && reactive_show_pso_ && REXCVAR_GET(pgr4_native_reactive_show)) {
    D3D12_RESOURCE_BARRIER b = {};  // after the upscaler's writes to the output
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = output_.resource.Get();
    list->ResourceBarrier(1, &b);
    RunPass(list, reactive_show_pso_.Get(), opaque_.Get(), output_.resource.Get(), 6,
            float(render_width_) / float(OutputWidth()),
            float(render_height_) / float(OutputHeight()), OutputWidth(), OutputHeight());
  }
  if (alpha_pso_) {
    D3D12_RESOURCE_BARRIER b = {};  // after FSR's writes to the output
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = output_.resource.Get();
    list->ResourceBarrier(1, &b);
    RunPass(list, alpha_pso_.Get(), color->resource.Get(), output_.resource.Get(), 1,
            float(render_width_) / float(OutputWidth()),
            float(render_height_) / float(OutputHeight()), OutputWidth(), OutputHeight());
  }
  reset_ = false;
  upscaled_ = true;
  output_.guest_format = color->guest_format;
  return &output_;
}

void Upscaler::Sharpen(ID3D12GraphicsCommandList* list) {
  const float strength =
      float(dlss_ ? REXCVAR_GET(pgr4_native_sharpen_dlss) : REXCVAR_GET(pgr4_native_sharpen_fsr));
  if (!sharpen_pso_ || strength <= 0.0f) return;
  const D3D12_RESOURCE_DESC od = output_.resource->GetDesc();
  if (sharpen_src_) {
    const D3D12_RESOURCE_DESC sd = sharpen_src_->GetDesc();
    if (sd.Width != od.Width || sd.Height != od.Height) sharpen_src_.Reset();
  }
  if (!sharpen_src_) {
    D3D12_RESOURCE_DESC desc = od;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    sharpen_src_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
    if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
                                                sharpen_src_state_, nullptr,
                                                IID_PPV_ARGS(&sharpen_src_)),
               "upscaler sharpening copy")) {
      sharpen_pso_.Reset();
      return;
    }
  }
  // The upscaled scene (in unordered access after the upscaler) copied, then read around each
  // pixel while the sharpened one is written back.
  D3D12_RESOURCE_BARRIER bs[2];
  uint32_t nb = 0;
  bs[nb++] = TransitionBarrier(output_.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                               D3D12_RESOURCE_STATE_COPY_SOURCE);
  if (sharpen_src_state_ != D3D12_RESOURCE_STATE_COPY_DEST)
    bs[nb++] = TransitionBarrier(sharpen_src_.Get(), sharpen_src_state_,
                                 D3D12_RESOURCE_STATE_COPY_DEST);
  list->ResourceBarrier(nb, bs);
  list->CopyResource(sharpen_src_.Get(), output_.resource.Get());
  bs[0] = TransitionBarrier(output_.resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  bs[1] = TransitionBarrier(sharpen_src_.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  list->ResourceBarrier(2, bs);
  sharpen_src_state_ = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  RunPass(list, sharpen_pso_.Get(), sharpen_src_.Get(), output_.resource.Get(), 7, strength, 0.0f,
          OutputWidth(), OutputHeight());
}

bool Upscaler::wants_opaque() const {
  // DLSS on its own switch: it took the mask as its bias towards the current color and ghosted
  // more with it (tested in race, 30/10/2026), but rain left trails on the mirrors without it.
  return reactive_pso_ && REXCVAR_GET(pgr4_native_reactive_mask) &&
         (!dlss_ || REXCVAR_GET(pgr4_native_reactive_dlss));
}

void Upscaler::SnapshotOpaque(ID3D12GraphicsCommandList* list, RenderTarget* color) {
  if (!reactive_pso_ || color->samples != 1 || color->scale != scale()) return;
  const D3D12_RESOURCE_DESC cd = color->resource->GetDesc();
  if (opaque_) {
    const D3D12_RESOURCE_DESC od = opaque_->GetDesc();
    if (od.Width != cd.Width || od.Height != cd.Height) opaque_.Reset();
  }
  if (!opaque_) {
    D3D12_RESOURCE_DESC d = cd;
    d.Format = kOutputFormat;
    d.MipLevels = 1;
    d.DepthOrArraySize = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    opaque_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
    if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, opaque_state_,
                                                nullptr, IID_PPV_ARGS(&opaque_)),
               "upscaler opaque color")) {
      reactive_pso_.Reset();
      return;
    }
    REXLOG_INFO("PGR4 native upscaler: reactive mask target {}x{}", d.Width, d.Height);
  }
  const D3D12_RESOURCE_STATES before = color->state;
  D3D12_RESOURCE_BARRIER bs[2];
  uint32_t nb = 0;
  if (before != D3D12_RESOURCE_STATE_COPY_SOURCE)
    bs[nb++] = TransitionBarrier(color->resource.Get(), before, D3D12_RESOURCE_STATE_COPY_SOURCE);
  if (opaque_state_ != D3D12_RESOURCE_STATE_COPY_DEST)
    bs[nb++] = TransitionBarrier(opaque_.Get(), opaque_state_, D3D12_RESOURCE_STATE_COPY_DEST);
  if (nb) list->ResourceBarrier(nb, bs);
  opaque_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
  D3D12_TEXTURE_COPY_LOCATION dst = {};
  dst.pResource = opaque_.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION src = {};
  src.pResource = color->resource.Get();
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  const D3D12_BOX box = {0, 0, 0, std::min<UINT>(render_width_, UINT(cd.Width)),
                         std::min<UINT>(render_height_, cd.Height), 1};
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
  if (before != D3D12_RESOURCE_STATE_COPY_SOURCE) {
    const D3D12_RESOURCE_BARRIER b =
        TransitionBarrier(color->resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, before);
    list->ResourceBarrier(1, &b);
  }
  opaque_frame_ = frame_;
}

// Local change (pgr4 project): DLSS Super Resolution through Streamline (streamline.h).
bool Upscaler::EvaluateDlss(ID3D12GraphicsCommandList* list, RenderTarget* depth) {
  const streamline::Api& sl_api = streamline::api();
  // DLSS Super Resolution's viewport; DLSS-G's is 0 (streamline.h), with its own constants (the
  // motion vectors' factors) and tags: Streamline takes one set of constants per viewport and frame.
  const sl::ViewportHandle viewport(1), fg_viewport(0);
  // The frame's token (streamline::BeginFrame), shared with Reflex, DLSS-G and the markers.
  sl::FrameToken* token = streamline::Token();
  if (!token) {
    PGR4_WARN_ONCE("PGR4 native DLSS: no frame token");
    return false;
  }

  // The options when the render or output size changed: the mode from their ratio.
  // (And the preset's, which the overlay changes live: overlay.h.)
  const uint64_t options_key = (uint64_t(render_width_) | uint64_t(render_height_) << 16 |
                                uint64_t(OutputWidth()) << 32 | uint64_t(OutputHeight()) << 48) ^
                               std::hash<std::string>{}(REXCVAR_GET(pgr4_native_dlss_preset));
  if (options_key != dlss_options_) {
    const float ratio = std::max(float(OutputWidth()) / float(render_width_),
                                 float(OutputHeight()) / float(render_height_));
    sl::DLSSOptions o;
    const DlssModeInfo* const setting = DlssModeSetting();
    o.mode = setting ? setting->mode
             : ratio <= 1.01f   ? sl::DLSSMode::eDLAA
             : ratio <= 1.55f ? sl::DLSSMode::eMaxQuality
             : ratio <= 1.75f ? sl::DLSSMode::eBalanced
             : ratio <= 2.05f ? sl::DLSSMode::eMaxPerformance
                              : sl::DLSSMode::eUltraPerformance;
    o.outputWidth = OutputWidth();
    o.outputHeight = OutputHeight();
    o.colorBuffersHDR = sl::Boolean::eTrue;  // linear light before the game's tone mapping
    const sl::DLSSPreset preset = DlssPresetSetting();
    o.dlaaPreset = o.qualityPreset = o.balancedPreset = o.performancePreset =
        o.ultraPerformancePreset = o.ultraQualityPreset = preset;
    if (const sl::Result r = slDLSSSetOptions(viewport, o); r != sl::Result::eOk) {
      PGR4_WARN_ONCE("PGR4 native DLSS: slDLSSSetOptions failed ({})", int(r));
      return false;
    }
    sl::DLSSOptimalSettings s;
    if (slDLSSGetOptimalSettings(o, s) == sl::Result::eOk)
      REXLOG_INFO("PGR4 native DLSS: {}x{} to {}x{}, mode {}, preset {} (render {}x{} to {}x{} "
                  "allowed)",
                  render_width_, render_height_, o.outputWidth, o.outputHeight, int(o.mode),
                  preset == sl::DLSSPreset::eDefault ? '-' : char('A' + int(preset) - 1),
                  s.renderWidthMin, s.renderHeightMin, s.renderWidthMax, s.renderHeightMax);
    dlss_options_ = options_key;
  }

  // The camera (UpdateCamera, at the start of Upscale).
  auto matrix = [](const float* m) {
    sl::float4x4 t;
    for (int i = 0; i < 4; ++i) t[i] = {m[4 * i], m[4 * i + 1], m[4 * i + 2], m[4 * i + 3]};
    return t;
  };
  const float aspect = float(kSceneWidth) / float(SceneHeight());
  const float n = camera_->near_plane, f = camera_->far_plane;
  sl::Constants c;
  c.cameraViewToClip = matrix(view_to_clip_);
  c.clipToCameraView = matrix(clip_to_view_);
  c.clipToPrevClip = matrix(clip_to_prev_clip_);
  c.prevClipToClip = matrix(prev_clip_to_clip_);
  const float sign = REXCVAR_GET(pgr4_native_dlss_jitter_sign) < 0 ? -1.0f : 1.0f;
  c.jitterOffset = {sign * jitter_[0], sign * jitter_[1]};
  // The motion vectors are in UV units (FSR's motionVectorScale is the render size); their signs
  // against Streamline's convention are settings while it is being matched.
  c.mvecScale = {REXCVAR_GET(pgr4_native_dlss_mvec_sign_x) < 0 ? -1.0f : 1.0f,
                 REXCVAR_GET(pgr4_native_dlss_mvec_sign_y) < 0 ? -1.0f : 1.0f};
  c.cameraPinholeOffset = {0.0f, 0.0f};
  c.cameraPos = {camera_pos_[0], camera_pos_[1], camera_pos_[2]};
  c.cameraUp = {camera_up_[0], camera_up_[1], camera_up_[2]};
  c.cameraRight = {camera_right_[0], camera_right_[1], camera_right_[2]};
  c.cameraFwd = {camera_fwd_[0], camera_fwd_[1], camera_fwd_[2]};
  c.cameraNear = n;
  c.cameraFar = f;
  c.cameraFOV = camera_->fov_y;
  c.cameraAspectRatio = aspect;
  c.depthInverted = sl::Boolean::eTrue;
  c.cameraMotionIncluded = sl::Boolean::eTrue;
  c.motionVectors3D = sl::Boolean::eFalse;
  c.reset = reset_ ? sl::Boolean::eTrue : sl::Boolean::eFalse;
  c.orthographicProjection = sl::Boolean::eFalse;
  c.motionVectorsDilated = sl::Boolean::eFalse;
  c.motionVectorsJittered = sl::Boolean::eFalse;
  if (const sl::Result r = sl_api.setConstants(c, *token, viewport); r != sl::Result::eOk) {
    PGR4_WARN_ONCE("PGR4 native DLSS: slSetConstants failed ({})", int(r));
    return false;
  }

  // The inputs at the render size in their largest-size resources; the output whole.
  const sl::Extent render = {0, 0, render_width_, render_height_};
  sl::Resource color_res(sl::ResourceType::eTex2d, input_.Get(), uint32_t(kInputState));
  // The depth as R32_FLOAT (depth_copy_pso_): Streamline's copy and DLSS-G read the typeless
  // depth-stencil format of the scene's target as something else.
  ID3D12Resource* depth_resource = depth->resource.Get();
  if (depth_copy_pso_ && fill_signature_) {
    if (!dlss_depth_) {
      D3D12_RESOURCE_DESC d = motion_->GetDesc();
      d.Format = DXGI_FORMAT_R32_FLOAT;
      d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
      D3D12_HEAP_PROPERTIES hp = {};
      hp.Type = D3D12_HEAP_TYPE_DEFAULT;
      dlss_depth_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
      if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, dlss_depth_state_,
                                                  nullptr, IID_PPV_ARGS(&dlss_depth_)),
                 "DLSS depth"))
        depth_copy_pso_.Reset();
    }
    if (dlss_depth_) {
      if (dlss_depth_state_ != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        const D3D12_RESOURCE_BARRIER b = TransitionBarrier(dlss_depth_.Get(), dlss_depth_state_,
                                                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &b);
      }
      FillMotion(list, depth, dlss_depth_.Get(), depth_copy_pso_.Get(), 4,
                 DXGI_FORMAT_R32_FLOAT);
      const D3D12_RESOURCE_BARRIER b = TransitionBarrier(
          dlss_depth_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kInputState);
      list->ResourceBarrier(1, &b);
      dlss_depth_state_ = kInputState;
      depth_resource = dlss_depth_.Get();
    }
  }
  sl::Resource depth_res(sl::ResourceType::eTex2d, depth_resource, uint32_t(kInputState));
  sl::Resource motion_res(sl::ResourceType::eTex2d, motion_.Get(), uint32_t(kInputState));
  sl::Resource output_res(sl::ResourceType::eTex2d, output_.resource.Get(),
                          uint32_t(D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
  // The reactive mask (local change) as DLSS's bias towards the current color; a null resource
  // without one this frame (untagged).
  sl::Resource bias_res(sl::ResourceType::eTex2d, reactive_ ? opaque_.Get() : nullptr,
                        uint32_t(kInputState));
  const sl::ResourceTag tags[] = {
      {&color_res, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eOnlyValidNow, &render},
      {&depth_res, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow, &render},
      {&motion_res, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, &render},
      {&output_res, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eOnlyValidNow},
      {&bias_res, sl::kBufferTypeBiasCurrentColorHint, sl::ResourceLifecycle::eOnlyValidNow,
       &render},
  };
  if (const sl::Result r =
          sl_api.setTagForFrame(*token, viewport, tags, uint32_t(std::size(tags)), list);
      r != sl::Result::eOk) {
    PGR4_WARN_ONCE("PGR4 native DLSS: slSetTagForFrame failed ({})", int(r));
    return false;
  }
  const sl::BaseStructure* inputs[] = {&viewport};
  if (const sl::Result r = sl_api.evaluateFeature(sl::kFeatureDLSS, *token, inputs,
                                                   uint32_t(std::size(inputs)), list);
      r != sl::Result::eOk) {
    PGR4_WARN_ONCE("PGR4 native DLSS: slEvaluateFeature failed ({})", int(r));
    return false;
  }
  // DLSS-G (viewport 0, read at the present): the same camera, its motion vector factors, and
  // the depth and motion.
  if (streamline::FrameGenerationActive()) {
    const int32_t fx = REXCVAR_GET(pgr4_native_dlss_fg_mvec_x),
                  fy = REXCVAR_GET(pgr4_native_dlss_fg_mvec_y);
    c.mvecScale = {c.mvecScale.x * float(fx), c.mvecScale.y * float(fy)};
    if (streamline::SkipGeneration()) c.reset = sl::Boolean::eTrue;
    if (const sl::Result r = sl_api.setConstants(c, *token, fg_viewport); r != sl::Result::eOk)
      PGR4_WARN_ONCE("PGR4 native DLSS-G: slSetConstants failed ({})", int(r));
    const sl::ResourceTag fg_tags[] = {tags[1], tags[2]};
    if (const sl::Result r = sl_api.setTagForFrame(*token, fg_viewport, fg_tags,
                                                   uint32_t(std::size(fg_tags)), list);
        r != sl::Result::eOk)
      PGR4_WARN_ONCE("PGR4 native DLSS-G: slSetTagForFrame failed ({})", int(r));
  }
  // Streamline does not restore the list's compute state (manual hooking): the alpha pass and the
  // resolves that follow set their own, and the draws bind theirs again after a resolve.
  // DLSS-G generates from this frame's depth and motion (copied: eOnlyValidNow).
  streamline::MarkScene();
  return true;
}

}  // namespace pgr4::native
