// pgr4-native PGR4 native renderer: effects over the game's 3D scene (scene_effects.h).
#include "scene_effects.h"

#include "d3d12_device.h"
#include "gpu_timer.h"
#include "upscaler.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <d3dcompiler.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

// Local addition (16/11/2026): Intel's XeGTAO (thirdparty/XeGTAO, MIT): its constants' layout.
#include PGR4_XEGTAO_H

REXCVAR_DEFINE_BOOL(pgr4_native_gtao, false, "PGR4 native",
                    "Ground-truth ambient occlusion (GTAO) over the 3D scene (scene_effects.h)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_gtao_radius, 5.0, "PGR4 native",
                      "GTAO: the radius around each point it looks for occluders in, in the "
                      "game's units (meters)")
    .range(0.1, 5.0);
REXCVAR_DEFINE_DOUBLE(pgr4_native_gtao_intensity, 0.15, "PGR4 native",
                      "GTAO: the strength of the occlusion (0: none; the visibility's power)")
    .range(0.0, 4.0);
REXCVAR_DEFINE_DOUBLE(pgr4_native_gtao_distance, 1500.0, "PGR4 native",
                      "GTAO: the distance beyond which nothing is occluded (meters), fading over "
                      "its last fifth")
    .range(10.0, 7000.0);
REXCVAR_DEFINE_BOOL(pgr4_native_gtao_full, true, "PGR4 native",
                    "GTAO at the scene's full size instead of half (steadier on thin edges, about "
                    "four times the cost)");
REXCVAR_DEFINE_BOOL(pgr4_native_gtao_debug, false, "PGR4 native",
                    "GTAO debug: the scene's color replaced by the occlusion alone");
// Local addition (16/11/2026): Intel's XeGTAO beside our GTAO, one menu choice (GTAO) for both:
// under an upscaler (DLSS, FSR) XeGTAO, its noise changing every frame and accumulated by the
// upscaler, steadier than ours there (Tazz in DLSS Quality); with MSAA, nothing accumulates a
// changing noise, ours. Its own radius, strength and tuning; the distance and the game's fog
// shared.
REXCVAR_DEFINE_INT32(pgr4_native_ao_method, 0, "PGR4 native",
                     "The ambient occlusion's method (with --pgr4_native_gtao): 0 automatic (our "
                     "GTAO with MSAA, Intel's XeGTAO under an upscaler), 1 always our GTAO, 2 "
                     "always XeGTAO")
    .range(0, 2);
REXCVAR_DEFINE_INT32(pgr4_native_xegtao_quality, 2, "PGR4 native",
                     "XeGTAO's quality: 0 low (1 slice, 2 steps), 1 medium (2, 2), 2 high (3, 3), "
                     "3 ultra (9, 3)")
    .range(0, 3);
REXCVAR_DEFINE_INT32(pgr4_native_xegtao_denoise, -1, "PGR4 native",
                     "XeGTAO's denoise passes (0 to 3; -1: 1 under an upscaler, which accumulates "
                     "its noise, else 2)")
    .range(-1, 3);
// Defaults: Tazz's tuning in a race (16/11/2026; intensity 0.3 on 18/11/2026): radius 5 m, intensity 0.3, falloff 0.619,
// samples' spread 1.5, thin occluder compensation 0.6; the rest as Intel's.
REXCVAR_DEFINE_DOUBLE(pgr4_native_xegtao_radius, 5.0, "PGR4 native",
                      "XeGTAO: the radius around each point it looks for occluders in (metres; "
                      "times XeGTAO's 1.457), its own: our GTAO's is for a far lighter occlusion")
    .range(0.1, 5.0);
// XeGTAO's own tuning (its GTAOImGuiSettings' "auto-tuned" ones), Intel's defaults and ranges.
REXCVAR_DEFINE_DOUBLE(pgr4_native_xegtao_radius_multiplier, 1.457, "PGR4 native",
                      "XeGTAO: multiplies the radius (Intel's auto-tune against ray-traced ground "
                      "truth; 0.3 to 3)")
    .range(0.3, 3.0);
REXCVAR_DEFINE_DOUBLE(pgr4_native_xegtao_falloff, 0.619, "PGR4 native",
                      "XeGTAO: the share of the radius over which distant samples fade out (0 to 1)")
    .range(0.0, 1.0);
REXCVAR_DEFINE_DOUBLE(pgr4_native_xegtao_sample_power, 1.5, "PGR4 native",
                      "XeGTAO: the samples' spread along a slice, 1 even, more toward the centre "
                      "(small crevices over big surfaces; 1 to 3)")
    .range(1.0, 3.0);
REXCVAR_DEFINE_DOUBLE(pgr4_native_xegtao_thin_compensation, 0.6, "PGR4 native",
                      "XeGTAO: less weight to samples further back, against the overdarkening by "
                      "thin objects the depth sees as solid (poles, fences, leaves; 0 to 0.7)")
    .range(0.0, 0.7);
REXCVAR_DEFINE_DOUBLE(pgr4_native_xegtao_final_power, 2.2, "PGR4 native",
                      "XeGTAO: its own power on the occlusion, before --pgr4_native_xegtao_intensity "
                      "(0.5 to 5)")
    .range(0.5, 5.0);
REXCVAR_DEFINE_DOUBLE(pgr4_native_xegtao_mip_offset, 3.3, "PGR4 native",
                      "XeGTAO: the depth MIP the far samples read (lower: finer, slower, steadier on "
                      "thin objects; 2 to 6)")
    .range(2.0, 6.0);
// Local change (17/11/2026, Tazz: too strong in the cockpit view): XeGTAO fades out near the eye.
// Its radius (5 m) is for the scenery; the cockpit, within about 1.5 m, saw it cover the whole
// screen and darken all of it, over the shading its textures already have. Nothing else is that
// near in a race's views (the car is 4 to 6 m away in the chase view); a close-up in the garage or
// the photo mode is. On by default since V1.4 (19/11/2026, Tazz).
REXCVAR_DEFINE_BOOL(pgr4_native_xegtao_near_fade, true, "PGR4 native",
                    "XeGTAO fades out near the eye (the cockpit view): none before "
                    "--pgr4_native_xegtao_near_start, all from --pgr4_native_xegtao_near_end");
REXCVAR_DEFINE_DOUBLE(pgr4_native_xegtao_near_start, 1.0, "PGR4 native",
                      "XeGTAO's near fade: the distance from the eye (metres) it starts from")
    .range(0.0, 10.0);
REXCVAR_DEFINE_DOUBLE(pgr4_native_xegtao_near_end, 3.0, "PGR4 native",
                      "XeGTAO's near fade: the distance from the eye (metres) it is whole from")
    .range(0.0, 20.0);
// Local addition (16/11/2026): per weather, over the above (SceneEffects::XeTuningFor).
REXCVAR_DEFINE_STRING(pgr4_native_xegtao_clear, "", "PGR4 native",
                      "XeGTAO in clear weather: \"key=value ...\" over its general settings (keys "
                      "radius, intensity, radius_multiplier, falloff, sample_power, "
                      "thin_compensation, final_power, mip_offset); empty: the general ones");
REXCVAR_DEFINE_STRING(pgr4_native_xegtao_overcast, "", "PGR4 native",
                      "XeGTAO when overcast (as --pgr4_native_xegtao_clear)");
REXCVAR_DEFINE_STRING(pgr4_native_xegtao_fog, "intensity=0.2 falloff=1", "PGR4 native",
                      "XeGTAO in fog (as --pgr4_native_xegtao_clear; Tazz's tuning by default)");
REXCVAR_DEFINE_STRING(pgr4_native_xegtao_rain, "", "PGR4 native",
                      "XeGTAO in rain (as --pgr4_native_xegtao_clear)");
REXCVAR_DEFINE_STRING(pgr4_native_xegtao_snow, "", "PGR4 native",
                      "XeGTAO in snow (as --pgr4_native_xegtao_clear)");
REXCVAR_DEFINE_STRING(pgr4_native_xegtao_icy, "", "PGR4 native",
                      "XeGTAO on ice (as --pgr4_native_xegtao_clear)");
REXCVAR_DEFINE_STRING(pgr4_native_xegtao_heavy_snow, "", "PGR4 native",
                      "XeGTAO in heavy snow (as --pgr4_native_xegtao_clear)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_xegtao_intensity, 0.3, "PGR4 native",
                      "XeGTAO: a power on its occlusion (1: as XeGTAO makes it; less, lighter)")
    .range(0.0, 4.0);
bool Pgr4GtaoScreen();  // src/pgr4_menu.cpp: a race's, the garage's, a replay's or photo screen
extern std::atomic<float> g_pgr4_fog_density;  // draw.cpp: the scene's fog, per metre
extern std::atomic<float> g_pgr4_fog_color[3];  // draw.cpp: its color (the shaders' c10.rgb)
extern int g_pgr4_weather_kind;    // draw.cpp: the race's weather, -1 unknown, 0 clear .. 6
extern bool g_pgr4_race_screen;    // draw.cpp: a race's, the garage's, a replay's or photo screen

namespace pgr4::native {

// Local change (pgr4 project): the overlay's "the game without our changes" (renderer.cpp).
bool OriginalLook();
namespace {

// Descriptors per frame slot, four per pass (t0, t1, t2, u0): the depth pass's depth, -, -,
// half-size depth; the occlusion's half-size depth, -, -, raw; the blur across's half-size depth,
// raw, -, ao; the blur down's half-size depth, ao, -, raw; the upsample's raw, half-size depth,
// depth, full (whose three sources are also the apply draw's t0-t2).
// Local addition (16/11/2026): then, from kXeFirstView, XeGTAO's passes: kXeViews each (t0-t2,
// u0-u4), the prefilter, the main pass, up to three denoise passes and the finish.
constexpr uint32_t kXeFirstView = 20;
constexpr uint32_t kXeViews = 8;
constexpr uint32_t kViewsPerSlot = kXeFirstView + 6 * kXeViews;

struct AoConstants {
  uint32_t size[2];  // the scene's host size
  float near_plane, far_plane;
  float proj[2];  // view-space x and y over depth at the edges: tan(fov_y / 2) times aspect, and it
  float radius, intensity;
  float max_distance;
  uint32_t frame;
  uint32_t half_size[2];  // the occlusion's
  uint32_t ratio;         // the scene's pixels per occlusion texel on each axis: 2, or 1 (full)
  float fog_density;      // the game's distance fog: exp(-distance x it) of the surface shows
  uint32_t axis;          // the blur's: 0 across, 1 down
  uint32_t pad;
  float fog_color[3];     // the fog's color, the rest of the pixel (local change, 16/11/2026)
  float pad2;
};

constexpr char kAoHlsl[] = R"(
cbuffer C : register(b0) {
  uint2 size; float near_plane; float far_plane;
  float2 proj; float radius; float intensity;
  float max_distance; uint frame; uint2 half_size;
  uint ratio; float fog_density; uint axis; uint pad;
  float3 fog_color; float pad2;
};
static const float kPi = 3.14159265;
// Local change (16/11/2026): the multiplier of a pixel's color `color` that darkens by `v` only
// the part of it that is its surface's. The game's shaders made it surface x f + fog x (1 - f)
// (f = exp(-distance x c10.w), fog = c10.rgb), so the surface's part is the color less the fog's;
// a darkening of the whole color darkened the fog itself, the further the more (with a strong
// occlusion, the fog seemed to clear from the distant facades). Of their luminances, so the
// apply draw and the deferred path multiply by it as before.
float SurfaceShare(float3 color, float3 fog, float f, float v) {
  const float3 kLuma = float3(0.2126, 0.7152, 0.0722);
  float3 surface = max(color - fog * (1.0 - f), 0.0);
  float lc = dot(max(color, 0.0), kLuma);
  if (lc <= 1e-5) return 1.0;
  return saturate(1.0 - dot(surface, kLuma) * (1.0 - v) / lc);
}
// Inverted depth: 1 at the near plane, 0 at the far.
float ViewZ(float d) {
  return near_plane * far_plane / ((far_plane - near_plane) * d + near_plane);
}
// View space: x right, y up, z the depth forward; p in the scene's host pixels.
float3 ViewPos(float2 p, float z) {
  float2 uv = (p + 0.5) / float2(size);
  return float3(float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0) * proj * z, z);
}

#ifdef UPSAMPLE
// The occlusion (half size) brought up to the scene's size, as the apply draw does (kApplyHlsl),
// for the scene's color resolves to multiply in (SceneEffects::DeferredOcclusion).
Texture2D<float> ao_tex : register(t0);
Texture2D<float> half_z : register(t1);  // view depth, metres
#ifdef MS
Texture2DMS<float> depth_tex : register(t2);
float RawDepth(int2 p) { return depth_tex.Load(p, 0); }
#else
Texture2D<float> depth_tex : register(t2);
float RawDepth(int2 p) { return depth_tex.Load(int3(p, 0)); }
#endif
RWTexture2D<unorm float> full : register(u0);
[numthreads(8, 8, 1)]
void main(uint2 p : SV_DispatchThreadID) {
  if (any(p >= size)) return;
  float r = float(ratio);
  float2 t = (float2(p) + 0.5 - 0.5 * r) / r;
  int2 q0 = int2(floor(t));
  float2 f = t - float2(q0);
  int2 q[4];
  float o4[4];
  float least = 1.0;
  [unroll] for (int k = 0; k < 4; ++k) {
    q[k] = clamp(q0 + int2(k & 1, k >> 1), int2(0, 0), int2(half_size) - 1);
    o4[k] = ao_tex.Load(int3(q[k], 0));
    least = min(least, o4[k]);
  }
  if (least >= 0.998) { full[p] = 1.0; return; }
  // Where the four texels nearly agree (no edge between near and far through them), their
  // bilinear mix: the multisampled depth (64 bytes a pixel at 8x) is read only at the edges.
  float most = max(max(o4[0], o4[1]), max(o4[2], o4[3]));
  if (most - least < 0.03) {
    full[p] = lerp(lerp(o4[0], o4[1], f.x), lerp(o4[2], o4[3], f.x), f.y);
    return;
  }
  float zp = ViewZ(RawDepth(int2(p)));
  float sum = 0.0, weight = 0.0;
  [unroll] for (int k = 0; k < 4; ++k) {
    int2 o = int2(k & 1, k >> 1);
    float b = (o.x ? f.x : 1.0 - f.x) * (o.y ? f.y : 1.0 - f.y);
    float zq = half_z.Load(int3(q[k], 0));
    float z = b * exp(-abs(zq - zp) / (0.03 * max(zp, 1e-3))) + 1e-5;
    sum += o4[k] * z;
    weight += z;
  }
  full[p] = sum / weight;
}
#elif defined(DOWN)
// The depth at half size, read once (the scene's depth is multisampled and four times larger):
// texel q the view depth (metres) of pixel 2q, sample 0, as the other passes take it, so they
// read it without converting the depth buffer's value at every tap.
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
  half_depth[p] = ViewZ(RawDepth(min(int2(p) * int(ratio), int2(size) - 1)));
}
#else
Texture2D<float> half_depth : register(t0);
Texture2D<float> src : register(t1);
RWTexture2D<unorm float> dst : register(u0);
#if !defined(BLUR)
// The scene's color (the occlusion pass's t2), sample 0 with MSAA.
#ifdef MS
Texture2DMS<float4> scene_color : register(t2);
float3 SceneColorAt(int2 p) { return scene_color.Load(p, 0).rgb; }
#else
Texture2D<float4> scene_color : register(t2);
float3 SceneColorAt(int2 p) { return scene_color.Load(int3(p, 0)).rgb; }
#endif
#endif
// q in half-size texels.
float ZAt(int2 q) {
  return half_depth.Load(int3(clamp(q, int2(0, 0), int2(half_size) - 1), 0));
}
float3 PosAt(int2 q) { return ViewPos(float2(q) * float(ratio), ZAt(q)); }

#ifdef BLUR
// Depth-aware 5x5 average of the raw occlusion, in two passes of five (across, then down).
[numthreads(8, 8, 1)]
void main(uint2 p : SV_DispatchThreadID) {
  if (any(p >= half_size)) return;
  float zc = ZAt(int2(p));
  int2 step = axis ? int2(0, 1) : int2(1, 0);
  float sum = 0.0, weight = 0.0;
  [unroll] for (int k = -2; k <= 2; ++k) {
    int2 q = clamp(int2(p) + step * k, int2(0, 0), int2(half_size) - 1);
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
  // The game's fog is already in the scene's color: the occlusion only darkens the part of the
  // surface the fog leaves, exp(-distance x density) (the shaders' c10.w), never the fog itself
  // (SurfaceShare, at the end).
  const float fog_f = exp(-length(c) * fog_density);
  if (fade <= 0.001 || fog_f <= 0.001 || intensity <= 0.0) { dst[p] = 1.0; return; }
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
  dst[p] = SurfaceShare(SceneColorAt(int2(p) * int(ratio)), fog_color, fog_f,
                        lerp(1.0, visibility, fade));
}
#endif
#endif
)";

// Local addition (16/11/2026): XeGTAO's header and shaders, embedded (CMakeLists.txt), served to
// the compiler by name (XeGTAO.hlsli includes XeGTAO.h).
constexpr char kXeGtaoH[] = {
#embed PGR4_XEGTAO_H
    , 0};
constexpr char kXeGtaoHlsli[] = {
#embed PGR4_XEGTAO_HLSLI
    , 0};
class XeGtaoInclude final : public ID3DInclude {
 public:
  HRESULT __stdcall Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID, LPCVOID* data,
                         UINT* bytes) noexcept override {
    if (std::strcmp(name, "XeGTAO.h") == 0) {
      *data = kXeGtaoH;
      *bytes = UINT(sizeof(kXeGtaoH) - 1);
      return S_OK;
    }
    if (std::strcmp(name, "XeGTAO.hlsli") == 0) {
      // Written for DXC; the game compiles with fxc (the system's d3dcompiler_47), which reads
      // "2.xx" as the float 2. and a name: an integer literal swizzled ("% 2.xx", "1.xxxx": four
      // in XeGTAO.hlsli) gets parentheses, "(2).xx". The file itself stays as Intel's.
      static const std::string patched = [] {
        const std::string text = kXeGtaoHlsli;
        std::string out;
        out.reserve(text.size() + 16);
        auto word = [](char c) { return std::isalnum(uint8_t(c)) || c == '_'; };
        for (size_t i = 0; i < text.size();) {
          size_t j = i;
          while (j < text.size() && std::isdigit(uint8_t(text[j]))) ++j;
          size_t k = j + 1;
          while (k < text.size() && text[k] == 'x') ++k;
          const bool literal = j > i && (i == 0 || (!word(text[i - 1]) && text[i - 1] != '.'));
          if (literal && j < text.size() && text[j] == '.' && k - (j + 1) >= 2 &&
              k - (j + 1) <= 4 && (k == text.size() || !word(text[k]))) {
            out += '(';
            out.append(text, i, j - i);
            out += ')';
            out.append(text, j, k - j);
            i = k;
          } else if (j > i) {
            out.append(text, i, j - i);
            i = j;
          } else {
            out += text[i++];
          }
        }
        return out;
      }();
      *data = patched.data();
      *bytes = UINT(patched.size());
      return S_OK;
    }
    return E_FAIL;
  }
  HRESULT __stdcall Close(LPCVOID) noexcept override { return S_OK; }
};

// The constants of XeGTAO's passes: XeGTAO's own, then ours.
struct XeConstants {
  XeGTAO::GTAOConstants gtao;
  float fog_density;   // the game's distance fog (AoConstants')
  float max_distance;  // --pgr4_native_gtao_distance
  float intensity;     // --pgr4_native_xegtao_intensity
  float near_start;    // the near fade (local change, 17/11/2026), 0 and 0 for none
  float fog_color[3];  // the fog's color (AoConstants')
  float near_end;
};
static_assert(sizeof(XeGTAO::GTAOConstants) == 24 * 4, "XeGTAO's constants: 6 registers");

// Our entry points around XeGTAO's (as its sample's vaGTAO.hlsl wires them), one per pass macro.
// The depth they start from is ours: the DOWN pass's view depth (metres) at the scene's full size,
// which reads any MSAA; the prefilter is XeGTAO_PrefilterDepths16x16 reading it instead of the
// NDC depth.
constexpr char kXeHlsl[] = R"(
#define XE_GTAO_FP32_DEPTHS 1
#define XE_GTAO_USE_HALF_FLOAT_PRECISION 0
// Its tuning from the constants (the overlay's sliders), not its compiled-in defaults.
#define XE_GTAO_USE_DEFAULT_CONSTANTS 0
#define XE_GTAO_GENERATE_NORMALS_INPLACE 1
#define VA_SATURATE saturate  // its sample framework's (vaShared.hlsl), which XeGTAO.hlsli uses
#include "XeGTAO.h"
cbuffer C : register(b0) {
  GTAOConstants g_GTAOConsts;
  float pgr4_fog_density; float pgr4_max_distance; float pgr4_intensity; float pgr4_near_start;
  float3 pgr4_fog_color; float pgr4_near_end;
};
#include "XeGTAO.hlsli"
SamplerState g_samplerPointClamp : register(s0);

#if defined(PREFILTER)
Texture2D<float> g_viewDepth : register(t0);
RWTexture2D<lpfloat> g_outDepth0 : register(u0);
RWTexture2D<lpfloat> g_outDepth1 : register(u1);
RWTexture2D<lpfloat> g_outDepth2 : register(u2);
RWTexture2D<lpfloat> g_outDepth3 : register(u3);
RWTexture2D<lpfloat> g_outDepth4 : register(u4);
lpfloat LoadZ(uint2 p) {
  return XeGTAO_ClampDepth(g_viewDepth.Load(int3(min(p, uint2(g_GTAOConsts.ViewportSize) - 1), 0)));
}
// XeGTAO_PrefilterDepths16x16, its MIP 0 loaded from the view depth.
[numthreads(8, 8, 1)]
void main(uint2 dispatchThreadID : SV_DispatchThreadID, uint2 groupThreadID : SV_GroupThreadID) {
  const uint2 baseCoord = dispatchThreadID;
  const uint2 pixCoord = baseCoord * 2;
  lpfloat depth0 = LoadZ(pixCoord + uint2(0, 0));
  lpfloat depth1 = LoadZ(pixCoord + uint2(1, 0));
  lpfloat depth2 = LoadZ(pixCoord + uint2(0, 1));
  lpfloat depth3 = LoadZ(pixCoord + uint2(1, 1));
  g_outDepth0[pixCoord + uint2(0, 0)] = depth0;
  g_outDepth0[pixCoord + uint2(1, 0)] = depth1;
  g_outDepth0[pixCoord + uint2(0, 1)] = depth2;
  g_outDepth0[pixCoord + uint2(1, 1)] = depth3;
  lpfloat dm1 = XeGTAO_DepthMIPFilter(depth0, depth1, depth2, depth3, g_GTAOConsts);
  g_outDepth1[baseCoord] = dm1;
  g_scratchDepths[groupThreadID.x][groupThreadID.y] = dm1;
  GroupMemoryBarrierWithGroupSync();
  [branch] if (all((groupThreadID.xy % 2u) == 0u)) {
    lpfloat dm2 = XeGTAO_DepthMIPFilter(
        g_scratchDepths[groupThreadID.x + 0][groupThreadID.y + 0],
        g_scratchDepths[groupThreadID.x + 1][groupThreadID.y + 0],
        g_scratchDepths[groupThreadID.x + 0][groupThreadID.y + 1],
        g_scratchDepths[groupThreadID.x + 1][groupThreadID.y + 1], g_GTAOConsts);
    g_outDepth2[baseCoord / 2] = dm2;
    g_scratchDepths[groupThreadID.x][groupThreadID.y] = dm2;
  }
  GroupMemoryBarrierWithGroupSync();
  [branch] if (all((groupThreadID.xy % 4u) == 0u)) {
    lpfloat dm3 = XeGTAO_DepthMIPFilter(
        g_scratchDepths[groupThreadID.x + 0][groupThreadID.y + 0],
        g_scratchDepths[groupThreadID.x + 2][groupThreadID.y + 0],
        g_scratchDepths[groupThreadID.x + 0][groupThreadID.y + 2],
        g_scratchDepths[groupThreadID.x + 2][groupThreadID.y + 2], g_GTAOConsts);
    g_outDepth3[baseCoord / 4] = dm3;
    g_scratchDepths[groupThreadID.x][groupThreadID.y] = dm3;
  }
  GroupMemoryBarrierWithGroupSync();
  [branch] if (all((groupThreadID.xy % 8u) == 0u)) {
    lpfloat dm4 = XeGTAO_DepthMIPFilter(
        g_scratchDepths[groupThreadID.x + 0][groupThreadID.y + 0],
        g_scratchDepths[groupThreadID.x + 4][groupThreadID.y + 0],
        g_scratchDepths[groupThreadID.x + 0][groupThreadID.y + 4],
        g_scratchDepths[groupThreadID.x + 4][groupThreadID.y + 4], g_GTAOConsts);
    g_outDepth4[baseCoord / 8] = dm4;
  }
}
#elif defined(MAIN)
Texture2D<lpfloat> g_srcWorkingDepth : register(t0);
RWTexture2D<uint> g_outWorkingAOTerm : register(u0);
RWTexture2D<unorm float> g_outWorkingEdges : register(u1);
// The sample's (vaGTAO.hlsl): the Hilbert curve's index over 64 x 64 pixels (XeGTAO.h's
// HilbertIndex) driving the R2 sequence, shifted each frame by the noise index (0 without an
// upscaler).
lpfloat2 SpatioTemporalNoise(uint2 pixCoord, uint temporalIndex) {
  uint index = HilbertIndex(pixCoord.x, pixCoord.y);
  index += 288u * (temporalIndex % 64u);
  return lpfloat2(frac(0.5 + index * float2(0.75487766624669276005, 0.5698402909980532659114)));
}
[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void main(uint2 pixCoord : SV_DispatchThreadID) {
  if (any(pixCoord >= uint2(g_GTAOConsts.ViewportSize))) return;
  XeGTAO_MainPass(pixCoord, SLICES, STEPS,
                  SpatioTemporalNoise(pixCoord, uint(g_GTAOConsts.NoiseIndex)), lpfloat3(0, 0, 1),
                  g_GTAOConsts, g_srcWorkingDepth, g_samplerPointClamp, g_outWorkingAOTerm,
                  g_outWorkingEdges);
}
#elif defined(DENOISE)
Texture2D<uint> g_srcWorkingAOTerm : register(t0);
Texture2D<lpfloat> g_srcWorkingEdges : register(t1);
RWTexture2D<uint> g_outFinalAOTerm : register(u0);
[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void main(uint2 dispatchThreadID : SV_DispatchThreadID) {
  const uint2 pixCoordBase = dispatchThreadID * uint2(2, 1);
  XeGTAO_Denoise(pixCoordBase, g_GTAOConsts, g_srcWorkingAOTerm, g_srcWorkingEdges,
                 g_samplerPointClamp, g_outFinalAOTerm, LAST != 0);
}
#elif defined(FINISH)
// Ours, as our GTAO's end: the distance's fade and the game's fog (already in the scene's color:
// the occlusion darkens only the surface's part of it, SurfaceShare), XeGTAO's occlusion to
// the power --pgr4_native_xegtao_intensity, at most half the light taken (the game's baked
// occlusion and sun shadows are there already), into the R8_UNORM target the apply draw reads.
Texture2D<uint> g_finalAOTerm : register(t0);
Texture2D<float> g_viewDepth : register(t1);
#ifdef MS
Texture2DMS<float4> g_sceneColor : register(t2);
float3 SceneColorAt(int2 p) { return g_sceneColor.Load(p, 0).rgb; }
#else
Texture2D<float4> g_sceneColor : register(t2);
float3 SceneColorAt(int2 p) { return g_sceneColor.Load(int3(p, 0)).rgb; }
#endif
RWTexture2D<unorm float> g_out : register(u0);
// As our GTAO's (kAoHlsl's SurfaceShare): the occlusion on the surface's part of the color only.
float SurfaceShare(float3 color, float3 fog, float f, float v) {
  const float3 kLuma = float3(0.2126, 0.7152, 0.0722);
  float3 surface = max(color - fog * (1.0 - f), 0.0);
  float lc = dot(max(color, 0.0), kLuma);
  if (lc <= 1e-5) return 1.0;
  return saturate(1.0 - dot(surface, kLuma) * (1.0 - v) / lc);
}
[numthreads(8, 8, 1)]
void main(uint2 p : SV_DispatchThreadID) {
  if (any(p >= uint2(g_GTAOConsts.ViewportSize))) return;
  float z = g_viewDepth.Load(int3(p, 0));
  float2 uv = (float2(p) + 0.5) * g_GTAOConsts.ViewportPixelSize;
  float3 pos = XeGTAO_ComputeViewspacePosition(uv, z, g_GTAOConsts);
  float fade = saturate((pgr4_max_distance - z) / (0.2 * pgr4_max_distance));
  // Local change (17/11/2026): the near fade, by the distance from the eye (the cockpit's sides
  // are near too): none before pgr4_near_start, all from pgr4_near_end; off when it is 0.
  if (pgr4_near_end > 0.0)
    fade *= saturate((length(pos) - pgr4_near_start) / (pgr4_near_end - pgr4_near_start));
  float fog_f = exp(-length(pos) * pgr4_fog_density);
  float v = float(g_finalAOTerm.Load(int3(p, 0))) / 255.0;
  v = max(pow(saturate(v), pgr4_intensity), 0.5);
  g_out[p] = SurfaceShare(SceneColorAt(int2(p)), pgr4_fog_color, fog_f, lerp(1.0, v, fade));
}
#endif
)";

// The occlusion (half size) brought up to the scene's size: of the four half-size texels around
// the pixel, bilinear weights times how close each texel's depth is to the pixel's (relative,
// in the inverted depth, which is about 1 / view depth), so no occlusion crosses an edge between
// near and far.
// Local change (16/11/2026): the scene's pixels per occlusion texel given (ratio), as the compute
// passes have it: worked out from the depth texture's width, it was that of the target's
// allocation, at the upscaler's largest scale. Below it (DLSS or FSR under native, or a dynamic
// size) the ratio came out wrong (2560 drawn in 3840: 2 for 1), the occlusion read from other
// texels than the pixel's: misplaced and flickering in the distance (Tazz, 16/11/2026).
constexpr char kApplyHlsl[] = R"(
cbuffer C : register(b0) { float near_plane; float far_plane; float ratio; float pad; };
Texture2D<float> ao : register(t0);
Texture2D<float> half_depth : register(t1);  // view depth, metres
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
  // The occlusion texels around the pixel (texel q covers `ratio` pixels from ratio * q on each
  // axis, its depth taken at ratio * q as the occlusion pass did).
  float2 t = (pos.xy - 0.5 * ratio) / ratio;
  int2 q0 = int2(floor(t));
  float2 f = t - float2(q0);
  int2 q[4];
  float o4[4];
  float least = 1.0;
  [unroll] for (int k = 0; k < 4; ++k) {
    q[k] = clamp(q0 + int2(k & 1, k >> 1), int2(0, 0), int2(w, h) - 1);
    o4[k] = ao.Load(int3(q[k], 0));
    least = min(least, o4[k]);
  }
#ifndef DEBUG
  // Nothing occluded around the pixel (most of the frame: the sky, open road, the cars' bodies):
  // color x 1, nothing written, so its multisampled samples are neither read nor written.
  if (least >= 0.998) discard;
#endif
  // Where the four texels nearly agree, their bilinear mix (the depth read only at the edges).
  float most = max(max(o4[0], o4[1]), max(o4[2], o4[3]));
  if (most - least < 0.03) {
    float b = lerp(lerp(o4[0], o4[1], f.x), lerp(o4[2], o4[3], f.x), f.y);
    return float4(b, b, b, 1.0);
  }
  float dp = RawDepth(p);
  float zp = near_plane * far_plane / ((far_plane - near_plane) * dp + near_plane);
  float sum = 0.0, weight = 0.0;
  [unroll] for (int k = 0; k < 4; ++k) {
    int2 o = int2(k & 1, k >> 1);
    float b = (o.x ? f.x : 1.0 - f.x) * (o.y ? f.y : 1.0 - f.y);
    float zq = half_depth.Load(int3(q[k], 0));
    float z = b * exp(-abs(zq - zp) / (0.03 * max(zp, 1e-3))) + 1e-5;
    sum += o4[k] * z;
    weight += z;
  }
  float a = sum / weight;
  return float4(a, a, a, 1.0);
}
)";

}  // namespace

// Local addition (16/11/2026): XeGTAO's tuning per weather.
namespace {
std::string XeWeatherSetting(int weather) {
  switch (weather) {
    case 0: return REXCVAR_GET(pgr4_native_xegtao_clear);
    case 1: return REXCVAR_GET(pgr4_native_xegtao_overcast);
    case 2: return REXCVAR_GET(pgr4_native_xegtao_fog);
    case 3: return REXCVAR_GET(pgr4_native_xegtao_rain);
    case 4: return REXCVAR_GET(pgr4_native_xegtao_snow);
    case 5: return REXCVAR_GET(pgr4_native_xegtao_icy);
    case 6: return REXCVAR_GET(pgr4_native_xegtao_heavy_snow);
    default: return {};
  }
}
void XeSetWeatherSetting(int weather, const std::string& v) {
  switch (weather) {
    case 0: REXCVAR_SET(pgr4_native_xegtao_clear, v); break;
    case 1: REXCVAR_SET(pgr4_native_xegtao_overcast, v); break;
    case 2: REXCVAR_SET(pgr4_native_xegtao_fog, v); break;
    case 3: REXCVAR_SET(pgr4_native_xegtao_rain, v); break;
    case 4: REXCVAR_SET(pgr4_native_xegtao_snow, v); break;
    case 5: REXCVAR_SET(pgr4_native_xegtao_icy, v); break;
    case 6: REXCVAR_SET(pgr4_native_xegtao_heavy_snow, v); break;
    default: break;
  }
}
// The tuning's fields by their keys.
struct XeKey {
  const char* name;
  float SceneEffects::XeTuning::*field;
};
constexpr XeKey kXeKeys[] = {
    {"radius", &SceneEffects::XeTuning::radius},
    {"intensity", &SceneEffects::XeTuning::intensity},
    {"radius_multiplier", &SceneEffects::XeTuning::radius_multiplier},
    {"falloff", &SceneEffects::XeTuning::falloff},
    {"sample_power", &SceneEffects::XeTuning::sample_power},
    {"thin_compensation", &SceneEffects::XeTuning::thin_compensation},
    {"final_power", &SceneEffects::XeTuning::final_power},
    {"mip_offset", &SceneEffects::XeTuning::mip_offset},
};
}  // namespace

int SceneEffects::XeWeather() {
  return g_pgr4_race_screen && g_pgr4_weather_kind >= 0 && g_pgr4_weather_kind < kXeWeathers
             ? g_pgr4_weather_kind
             : -1;
}

const char* SceneEffects::XeWeatherName(int weather) {
  static const char* const kNames[kXeWeathers] = {"clear", "overcast", "fog", "rain",
                                                  "snow", "icy", "heavy_snow"};
  return weather >= 0 && weather < kXeWeathers ? kNames[weather] : "general";
}

SceneEffects::XeTuning SceneEffects::XeGeneralTuning() {
  return {float(REXCVAR_GET(pgr4_native_xegtao_radius)),
          float(REXCVAR_GET(pgr4_native_xegtao_intensity)),
          float(REXCVAR_GET(pgr4_native_xegtao_radius_multiplier)),
          float(REXCVAR_GET(pgr4_native_xegtao_falloff)),
          float(REXCVAR_GET(pgr4_native_xegtao_sample_power)),
          float(REXCVAR_GET(pgr4_native_xegtao_thin_compensation)),
          float(REXCVAR_GET(pgr4_native_xegtao_final_power)),
          float(REXCVAR_GET(pgr4_native_xegtao_mip_offset))};
}

void SceneEffects::XeSetGeneral(const XeTuning& t) {
  REXCVAR_SET(pgr4_native_xegtao_radius, double(t.radius));
  REXCVAR_SET(pgr4_native_xegtao_intensity, double(t.intensity));
  REXCVAR_SET(pgr4_native_xegtao_radius_multiplier, double(t.radius_multiplier));
  REXCVAR_SET(pgr4_native_xegtao_falloff, double(t.falloff));
  REXCVAR_SET(pgr4_native_xegtao_sample_power, double(t.sample_power));
  REXCVAR_SET(pgr4_native_xegtao_thin_compensation, double(t.thin_compensation));
  REXCVAR_SET(pgr4_native_xegtao_final_power, double(t.final_power));
  REXCVAR_SET(pgr4_native_xegtao_mip_offset, double(t.mip_offset));
}

SceneEffects::XeTuning SceneEffects::XeTuningFor(int weather) {
  XeTuning t = XeGeneralTuning();
  const std::string s = XeWeatherSetting(weather);
  // "key=value" pairs, separated by spaces, commas or semicolons; unknown keys are skipped.
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && (s[i] == ' ' || s[i] == ',' || s[i] == ';')) ++i;
    const size_t end = s.find_first_of(" ,;", i);
    const std::string token = s.substr(i, end == std::string::npos ? std::string::npos : end - i);
    i = end == std::string::npos ? s.size() : end;
    const size_t eq = token.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = token.substr(0, eq);
    for (const XeKey& k : kXeKeys)
      if (key == k.name) t.*k.field = std::strtof(token.c_str() + eq + 1, nullptr);
  }
  return t;
}

bool SceneEffects::XeHasOwn(int weather) { return !XeWeatherSetting(weather).empty(); }

void SceneEffects::XeSetOwn(int weather, const XeTuning* t) {
  if (!t) return XeSetWeatherSetting(weather, {});
  // Only what differs from the general settings.
  const XeTuning g = XeGeneralTuning();
  std::string s;
  for (const XeKey& k : kXeKeys) {
    if (std::abs(t->*k.field - g.*k.field) < 1e-4f) continue;
    char v[32];
    std::snprintf(v, sizeof(v), "%g", double(t->*k.field));
    s += (s.empty() ? "" : " ") + std::string(k.name) + "=" + v;
  }
  // All alike: still its own (the check box stays), its radius written.
  if (s.empty()) {
    char v[32];
    std::snprintf(v, sizeof(v), "%g", double(t->radius));
    s = std::string("radius=") + v;
  }
  XeSetWeatherSetting(weather, s);
}

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
      {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0},
      {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 3},
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
  // The apply draw: t0 the occlusion, t1 the half-size depth, t2 the depth, then the near and far
  // planes; pixel shader.
  D3D12_DESCRIPTOR_RANGE apply_range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0};
  D3D12_ROOT_PARAMETER apply_params[2] = {};
  apply_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  apply_params[0].DescriptorTable = {1, &apply_range};
  apply_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  apply_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  apply_params[1].Constants = {0, 0, 4};
  apply_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  const D3D12_ROOT_SIGNATURE_DESC as = {2, apply_params, 0, nullptr,
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
      !compute("pgr4_gtao_ms.hlsl", true, nullptr, ao_ms_pso_) ||
      !compute("pgr4_gtao_blur.hlsl", false, "BLUR", blur_pso_) ||
      !compute("pgr4_gtao_upsample_ms.hlsl", true, "UPSAMPLE", upsample_ms_pso_))
    return false;
  ComPtr<ID3DBlob> err;
  const D3D_SHADER_MACRO ms_macros[] = {{"MS", "1"}, {nullptr, nullptr}};
  const D3D_SHADER_MACRO debug_macros[] = {{"DEBUG", "1"}, {nullptr, nullptr}};
  const D3D_SHADER_MACRO ms_debug_macros[] = {{"MS", "1"}, {"DEBUG", "1"}, {nullptr, nullptr}};
  const auto apply_ps = [&](const D3D_SHADER_MACRO* m, ComPtr<ID3DBlob>& out) {
    return compile(kApplyHlsl, sizeof(kApplyHlsl) - 1, "pgr4_gtao_apply.hlsl", m, nullptr, "ps",
                   "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out, &err);
  };
  if (FAILED(compile(kApplyHlsl, sizeof(kApplyHlsl) - 1, "pgr4_gtao_apply.hlsl", nullptr, nullptr,
                     "vs", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &apply_vs_, &err)) ||
      FAILED(apply_ps(nullptr, apply_ps_)) || FAILED(apply_ps(ms_macros, apply_ps_ms_)) ||
      FAILED(apply_ps(debug_macros, apply_ps_debug_)) ||
      FAILED(apply_ps(ms_debug_macros, apply_ps_ms_debug_))) {
    REXLOG_ERROR("PGR4 native GTAO: apply: {}",
                 err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
    return false;
  }

  // Local addition (16/11/2026): XeGTAO's passes: t0-t2 and u0-u4, then XeConstants, and the
  // point clamp sampler they all take (s0). Without them, the menu's XeGTAO is our GTAO.
  {
    D3D12_DESCRIPTOR_RANGE xe_ranges[2] = {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 5, 0, 0, 3},
    };
    D3D12_ROOT_PARAMETER xe_params[2] = {};
    xe_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    xe_params[0].DescriptorTable = {2, xe_ranges};
    xe_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    xe_params[1].Constants = {0, 0, sizeof(XeConstants) / 4};
    D3D12_STATIC_SAMPLER_DESC point = {};
    point.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    point.AddressU = point.AddressV = point.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    point.MaxLOD = D3D12_FLOAT32_MAX;
    point.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    const D3D12_ROOT_SIGNATURE_DESC xs = {2, xe_params, 1, &point, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    blob.Reset();
    error.Reset();
    XeGtaoInclude include;
    auto xe_compute = [&](const char* name, std::initializer_list<D3D_SHADER_MACRO> defines,
                          ComPtr<ID3D12PipelineState>& out) {
      std::vector<D3D_SHADER_MACRO> macros(defines);
      macros.push_back({nullptr, nullptr});
      ComPtr<ID3DBlob> code, err;
      if (FAILED(compile(kXeHlsl, sizeof(kXeHlsl) - 1, name, macros.data(), &include, "main",
                         "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err))) {
        REXLOG_ERROR("PGR4 native XeGTAO: {}: {}", name,
                     err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        return false;
      }
      D3D12_COMPUTE_PIPELINE_STATE_DESC d = {};
      d.pRootSignature = xe_signature_.Get();
      d.CS = {code->GetBufferPointer(), code->GetBufferSize()};
      return !Failed(device->CreateComputePipelineState(&d, IID_PPV_ARGS(&out)), name);
    };
    xe_ready_ =
        !Failed(D3D12SerializeRootSignature(&xs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error),
                "XeGTAO root signature serialize") &&
        !Failed(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                           IID_PPV_ARGS(&xe_signature_)),
                "XeGTAO root signature") &&
        xe_compute("xegtao_prefilter.hlsl", {{"PREFILTER", "1"}}, xe_prefilter_pso_) &&
        // One main pass per quality level, its slices and steps in it (XeGTAO unrolls the steps),
        // as its sample's CSGTAOLow to CSGTAOUltra.
        xe_compute("xegtao_main_low.hlsl", {{"MAIN", "1"}, {"SLICES", "1"}, {"STEPS", "2"}},
                   xe_main_pso_[0]) &&
        xe_compute("xegtao_main_medium.hlsl", {{"MAIN", "1"}, {"SLICES", "2"}, {"STEPS", "2"}},
                   xe_main_pso_[1]) &&
        xe_compute("xegtao_main_high.hlsl", {{"MAIN", "1"}, {"SLICES", "3"}, {"STEPS", "3"}},
                   xe_main_pso_[2]) &&
        xe_compute("xegtao_main_ultra.hlsl", {{"MAIN", "1"}, {"SLICES", "9"}, {"STEPS", "3"}},
                   xe_main_pso_[3]) &&
        xe_compute("xegtao_denoise.hlsl", {{"DENOISE", "1"}, {"LAST", "0"}}, xe_denoise_pso_) &&
        xe_compute("xegtao_denoise_last.hlsl", {{"DENOISE", "1"}, {"LAST", "1"}},
                   xe_denoise_last_pso_) &&
        xe_compute("xegtao_finish.hlsl", {{"FINISH", "1"}}, xe_finish_pso_) &&
        xe_compute("xegtao_finish_ms.hlsl", {{"FINISH", "1"}, {"MS", "1"}}, xe_finish_ms_pso_);
    if (!xe_ready_) REXLOG_WARN("PGR4 native XeGTAO: unavailable, the menu's XeGTAO is our GTAO");
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

bool SceneEffects::EnsureFull(uint32_t width, uint32_t height) {
  if (full_ && full_width_ == width && full_height_ == height) return true;
  full_.Reset();
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
  if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                              IID_PPV_ARGS(&full_)),
             "GTAO full-size target"))
    return false;
  full_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  full_width_ = width;
  full_height_ = height;
  return true;
}

// Local addition (16/11/2026): XeGTAO's targets at the scene's full size.
bool SceneEffects::EnsureXeTargets(uint32_t width, uint32_t height) {
  if (xe_depth_ && xe_width_ == width && xe_height_ == height) return true;
  xe_depth_.Reset();
  xe_ao_[0].Reset();
  xe_ao_[1].Reset();
  xe_edges_.Reset();
  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC d = {};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width = width;
  d.Height = height;
  d.DepthOrArraySize = 1;
  d.SampleDesc.Count = 1;
  d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  auto make = [&](ComPtr<ID3D12Resource>& r, DXGI_FORMAT format, uint16_t mips) {
    d.Format = format;
    d.MipLevels = mips;
    return !Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                    IID_PPV_ARGS(r.GetAddressOf())),
                   "XeGTAO target");
  };
  if (!make(xe_depth_, DXGI_FORMAT_R32_FLOAT, XE_GTAO_DEPTH_MIP_LEVELS) ||
      !make(xe_ao_[0], DXGI_FORMAT_R8_UINT, 1) || !make(xe_ao_[1], DXGI_FORMAT_R8_UINT, 1) ||
      !make(xe_edges_, DXGI_FORMAT_R8_UNORM, 1)) {
    xe_depth_.Reset();
    return false;
  }
  xe_depth_state_ = xe_ao_state_[0] = xe_ao_state_[1] = xe_edges_state_ =
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  xe_width_ = width;
  xe_height_ = height;
  REXLOG_INFO("PGR4 native XeGTAO: targets {}x{}", width, height);
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
  ID3DBlob* ps = debug ? (samples > 1 ? apply_ps_ms_debug_.Get() : apply_ps_debug_.Get())
                       : (samples > 1 ? apply_ps_ms_.Get() : apply_ps_.Get());
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
                         const SurfaceRegs& regs, uint64_t frame, GpuTimer* timer, bool defer) {
  const auto step = [&](uint32_t s) {
    if (timer) timer->Pass(list, GpuTimer::KeyEffects(s));
  };
  if (!ready_ || !Enabled() || applied_frame_ == frame) return;
  applied_frame_ = frame;
  RenderTarget* color = targets.GetColor(regs, 0, Upscaler::SceneHeight());
  RenderTarget* depth = targets.GetDepth(regs, Upscaler::SceneHeight());
  if (!color || !depth || color->samples != depth->samples || !(color->scale == depth->scale))
    return;
  // The draws' host extent of the scene.
  const uint32_t w = color->scale.Extent(color->width, 0), h = color->scale.Extent(color->height, 1);
  // The occlusion's size: half the scene's, or all of it (pgr4_native_gtao_full).
  // Local addition (16/11/2026): XeGTAO (the menu's third choice), at the scene's full size.
  // XeGTAO under an upscaler (defer: none), or as --pgr4_native_ao_method forces.
  const int32_t method = REXCVAR_GET(pgr4_native_ao_method);
  const bool xe = xe_ready_ && (method == 2 || (method == 0 && !defer));
  const uint32_t ratio = xe || REXCVAR_GET(pgr4_native_gtao_full) ? 1 : 2;
  const uint32_t hw = (w + ratio - 1) / ratio, hh = (h + ratio - 1) / ratio;
  if (!w || !h || !EnsureTargets(hw, hh) || (xe && !EnsureXeTargets(w, h))) return;
  const D3D12_RECT all = {0, 0, LONG(color->width), LONG(color->height)};
  step(0);
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
  // The deferred path: a multisampled scene the resolver's copies alone read (no upscaler), not
  // the debug view (which shows the occlusion in place of the color).
  const bool deferred = defer && color->samples > 1 && !REXCVAR_GET(pgr4_native_gtao_debug) &&
                        upsample_ms_pso_ && EnsureFull(w, h);
  // Pass g's views at 4g: t0, t1, t2, u0 (kViewsPerSlot).
  const auto views = [&](uint32_t g, ID3D12Resource* t0, const D3D12_SHADER_RESOURCE_VIEW_DESC& d0,
                         ID3D12Resource* t1, const D3D12_SHADER_RESOURCE_VIEW_DESC& d1,
                         ID3D12Resource* t2, const D3D12_SHADER_RESOURCE_VIEW_DESC& d2,
                         ID3D12Resource* u0, const D3D12_UNORDERED_ACCESS_VIEW_DESC& du) {
    device_->CreateShaderResourceView(t0, &d0, cpu(4 * g));
    device_->CreateShaderResourceView(t1, &d1, cpu(4 * g + 1));
    device_->CreateShaderResourceView(t2, &d2, cpu(4 * g + 2));
    device_->CreateUnorderedAccessView(u0, nullptr, &du, cpu(4 * g + 3));
  };
  views(0, depth->resource.Get(), depth_srv, nullptr, r8_srv, nullptr, r8_srv, half_depth_.Get(),
        z_uav);
  // Local change (16/11/2026): the scene's color too (t2: SurfaceShare).
  D3D12_SHADER_RESOURCE_VIEW_DESC color_srv = {};
  color_srv.Format = color->srv_format;
  color_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  if (color->samples > 1) {
    color_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
  } else {
    color_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    color_srv.Texture2D.MipLevels = 1;
  }
  views(1, half_depth_.Get(), z_srv, nullptr, r8_srv, color->resource.Get(), color_srv, raw_.Get(),
        r8_uav);
  views(2, half_depth_.Get(), z_srv, raw_.Get(), r8_srv, nullptr, r8_srv, ao_.Get(), r8_uav);
  views(3, half_depth_.Get(), z_srv, ao_.Get(), r8_srv, nullptr, r8_srv, raw_.Get(), r8_uav);
  views(4, raw_.Get(), r8_srv, half_depth_.Get(), z_srv, depth->resource.Get(), depth_srv,
        deferred ? full_.Get() : nullptr, r8_uav);

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
  c.fog_density = std::max(0.0f, g_pgr4_fog_density.load(std::memory_order_relaxed));
  for (int i = 0; i < 3; ++i)
    c.fog_color[i] = std::max(0.0f, g_pgr4_fog_color[i].load(std::memory_order_relaxed));

  Transition(list, depth->resource.Get(), depth->state,
             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  // Local change (16/11/2026): the color, read by the occlusion (SurfaceShare).
  Transition(list, color->resource.Get(), color->state,
             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Transition(list, half_depth_.Get(), half_depth_state_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  Transition(list, raw_.Get(), raw_state_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  Transition(list, ao_.Get(), ao_state_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(compute_signature_.Get());
  list->SetComputeRoot32BitConstants(1, sizeof(c) / 4, &c, 0);
  const UINT gx = (hw + 7) / 8, gy = (hh + 7) / 8;
  step(1);
  list->SetPipelineState(ms ? down_ms_pso_.Get() : down_pso_.Get());
  list->SetComputeRootDescriptorTable(0, gpu(0));
  list->Dispatch(gx, gy, 1);
  Transition(list, half_depth_.Get(), half_depth_state_,
             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  if (xe) {
    // XeGTAO's passes from the view depth just made (Apply's DOWN pass, at full size), into raw_.
    constexpr D3D12_RESOURCE_STATES kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    constexpr D3D12_RESOURCE_STATES kWrite = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_SHADER_RESOURCE_VIEW_DESC u8_srv = r8_srv;
    u8_srv.Format = DXGI_FORMAT_R8_UINT;
    D3D12_UNORDERED_ACCESS_VIEW_DESC u8_uav = r8_uav;
    u8_uav.Format = DXGI_FORMAT_R8_UINT;
    D3D12_SHADER_RESOURCE_VIEW_DESC mips_srv = z_srv;
    mips_srv.Texture2D.MipLevels = XE_GTAO_DEPTH_MIP_LEVELS;
    // Pass k's views at kXeFirstView + kXeViews * k: t0-t2, u0-u4 (nullptr: a null view).
    const auto xe_views = [&](uint32_t k, std::initializer_list<std::pair<ID3D12Resource*,
                                                                          D3D12_SHADER_RESOURCE_VIEW_DESC>> srvs,
                              std::initializer_list<std::pair<ID3D12Resource*,
                                                              D3D12_UNORDERED_ACCESS_VIEW_DESC>> uavs) {
      const uint32_t base = kXeFirstView + kXeViews * k;
      uint32_t i = 0;
      for (const auto& [r, d] : srvs) device_->CreateShaderResourceView(r, &d, cpu(base + i++));
      for (; i < 3; ++i) device_->CreateShaderResourceView(nullptr, &z_srv, cpu(base + i));
      for (const auto& [r, d] : uavs) device_->CreateUnorderedAccessView(r, nullptr, &d, cpu(base + i++));
      for (; i < kXeViews; ++i) device_->CreateUnorderedAccessView(nullptr, nullptr, &z_uav, cpu(base + i));
      return gpu(base);
    };
    const auto mip_uav = [&](uint32_t mip) {
      D3D12_UNORDERED_ACCESS_VIEW_DESC d = z_uav;
      d.Texture2D.MipSlice = mip;
      return d;
    };
    // The constants: XeGTAO's (as its GTAOUpdateConstants makes them, from our camera: inverted
    // depth, the view's y up as its NDCToView), then ours.
    const int32_t quality = std::clamp(REXCVAR_GET(pgr4_native_xegtao_quality), 0, 3);
    const bool temporal = !defer;  // an upscaler: it accumulates the noise changing every frame
    const int32_t denoise_setting = REXCVAR_GET(pgr4_native_xegtao_denoise);
    const int32_t denoise = denoise_setting < 0 ? (temporal ? 1 : 2) : std::min(denoise_setting, 3);
    XeConstants xc = {};
    XeGTAO::GTAOConstants& g = xc.gtao;
    g.ViewportSize = {int32_t(w), int32_t(h)};
    g.ViewportPixelSize = {1.0f / float(w), 1.0f / float(h)};
    const float n = cam.near_plane, f = cam.far_plane;
    g.DepthUnpackConsts = {-n * f / std::max(f - n, 1e-3f), -n / std::max(f - n, 1e-3f)};
    g.CameraTanHalfFOV = {c.proj[0], c.proj[1]};
    g.NDCToViewMul = {g.CameraTanHalfFOV.x * 2.0f, g.CameraTanHalfFOV.y * -2.0f};
    g.NDCToViewAdd = {g.CameraTanHalfFOV.x * -1.0f, g.CameraTanHalfFOV.y * 1.0f};
    g.NDCToViewMul_x_PixelSize = {g.NDCToViewMul.x * g.ViewportPixelSize.x,
                                  g.NDCToViewMul.y * g.ViewportPixelSize.y};
    // Its tuning: the race's weather's, or the general one (XeTuningFor).
    const XeTuning tune = XeTuningFor(XeWeather());
    g.EffectRadius = tune.radius;
    g.EffectFalloffRange = tune.falloff;
    g.RadiusMultiplier = tune.radius_multiplier;
    g.Padding0 = 0.0f;
    g.FinalValuePower = tune.final_power;
    g.DenoiseBlurBeta = denoise == 0 ? 1e4f : 1.2f;
    g.SampleDistributionPower = tune.sample_power;
    g.ThinOccluderCompensation = tune.thin_compensation;
    g.DepthMIPSamplingOffset = tune.mip_offset;
    g.NoiseIndex = temporal ? int32_t(frame % 64) : 0;
    xc.fog_density = c.fog_density;
    xc.max_distance = c.max_distance;
    xc.intensity = tune.intensity;
    std::memcpy(xc.fog_color, c.fog_color, sizeof(xc.fog_color));
    if (REXCVAR_GET(pgr4_native_xegtao_near_fade)) {
      xc.near_start = std::max(0.0f, float(REXCVAR_GET(pgr4_native_xegtao_near_start)));
      xc.near_end =
          std::max(xc.near_start + 0.01f, float(REXCVAR_GET(pgr4_native_xegtao_near_end)));
    } else {
      xc.near_start = xc.near_end = 0.0f;
    }

    list->SetComputeRootSignature(xe_signature_.Get());
    list->SetComputeRoot32BitConstants(1, sizeof(xc) / 4, &xc, 0);
    // The depth's MIP chain.
    step(2);
    Transition(list, xe_depth_.Get(), xe_depth_state_, kWrite);
    list->SetPipelineState(xe_prefilter_pso_.Get());
    list->SetComputeRootDescriptorTable(
        0, xe_views(0, {{half_depth_.Get(), z_srv}},
                    {{xe_depth_.Get(), mip_uav(0)}, {xe_depth_.Get(), mip_uav(1)},
                     {xe_depth_.Get(), mip_uav(2)}, {xe_depth_.Get(), mip_uav(3)},
                     {xe_depth_.Get(), mip_uav(4)}}));
    list->Dispatch((w + 15) / 16, (h + 15) / 16, 1);
    Transition(list, xe_depth_.Get(), xe_depth_state_, kRead);
    // The occlusion and the edges.
    Transition(list, xe_ao_[0].Get(), xe_ao_state_[0], kWrite);
    Transition(list, xe_edges_.Get(), xe_edges_state_, kWrite);
    list->SetPipelineState(xe_main_pso_[quality].Get());
    list->SetComputeRootDescriptorTable(
        0, xe_views(1, {{xe_depth_.Get(), mips_srv}},
                    {{xe_ao_[0].Get(), u8_uav}, {xe_edges_.Get(), r8_uav}}));
    list->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    Transition(list, xe_ao_[0].Get(), xe_ao_state_[0], kRead);
    Transition(list, xe_edges_.Get(), xe_edges_state_, kRead);
    // The denoise: at least one pass, the last scaling the term back (XeGTAO's sample does so).
    step(3);
    const uint32_t passes = uint32_t(std::max(denoise, 1));
    uint32_t src = 0;
    for (uint32_t i = 0; i < passes; ++i) {
      const uint32_t dst = src ^ 1u;
      Transition(list, xe_ao_[dst].Get(), xe_ao_state_[dst], kWrite);
      list->SetPipelineState(i + 1 == passes ? xe_denoise_last_pso_.Get() : xe_denoise_pso_.Get());
      list->SetComputeRootDescriptorTable(
          0, xe_views(2 + i, {{xe_ao_[src].Get(), u8_srv}, {xe_edges_.Get(), r8_srv}},
                      {{xe_ao_[dst].Get(), u8_uav}}));
      list->Dispatch((w + 15) / 16, (h + 7) / 8, 1);
      Transition(list, xe_ao_[dst].Get(), xe_ao_state_[dst], kRead);
      src = dst;
    }
    // Ours: fade and limits, into raw_.
    Transition(list, raw_.Get(), raw_state_, kWrite);
    list->SetPipelineState(ms ? xe_finish_ms_pso_.Get() : xe_finish_pso_.Get());
    list->SetComputeRootDescriptorTable(
        0, xe_views(2 + passes,
                    {{xe_ao_[src].Get(), u8_srv}, {half_depth_.Get(), z_srv},
                     {color->resource.Get(), color_srv}},
                    {{raw_.Get(), r8_uav}}));
    list->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    Transition(list, raw_.Get(), raw_state_,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    // The upsample (deferred path) uses the effects' own signature and constants again.
    list->SetComputeRootSignature(compute_signature_.Get());
    list->SetComputeRoot32BitConstants(1, sizeof(c) / 4, &c, 0);
  } else {
    step(2);
    list->SetPipelineState(ms ? ao_ms_pso_.Get() : ao_pso_.Get());
    list->SetComputeRootDescriptorTable(0, gpu(4));
    list->Dispatch(gx, gy, 1);
    Transition(list, raw_.Get(), raw_state_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    step(3);
    // The blur across (raw into ao), then down (ao back into raw, which the apply draw reads).
    list->SetPipelineState(blur_pso_.Get());
    list->SetComputeRootDescriptorTable(0, gpu(8));
    list->Dispatch(gx, gy, 1);
    Transition(list, ao_.Get(), ao_state_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(list, raw_.Get(), raw_state_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    c.axis = 1;
    list->SetComputeRoot32BitConstants(1, sizeof(c) / 4, &c, 0);
    list->SetComputeRootDescriptorTable(0, gpu(12));
    list->Dispatch(gx, gy, 1);
    Transition(list, raw_.Get(), raw_state_,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  }
  Transition(list, depth->resource.Get(), depth->state,
             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

  step(4);
  if (deferred) {
    // The occlusion at the scene's size, for the frame's color resolves (DeferredOcclusion).
    Transition(list, full_.Get(), full_state_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->SetPipelineState(upsample_ms_pso_.Get());
    list->SetComputeRootDescriptorTable(0, gpu(16));
    list->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    Transition(list, full_.Get(), full_state_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    deferred_frame_ = frame;
    return;
  }
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
  list->SetGraphicsRootDescriptorTable(0, gpu(16));
  const float planes[4] = {cam.near_plane, cam.far_plane, float(ratio), 0.0f};
  list->SetGraphicsRoot32BitConstants(1, 4, planes, 0);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  list->DrawInstanced(3, 1, 0, 0);
}

}  // namespace pgr4::native
