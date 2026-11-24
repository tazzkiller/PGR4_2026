// The cars' shadows on the ground in the volumetric fog (local change, 24/11/2026; made with
// work/make_car_shadow_fog.py, which writes each psCarShadow* replacement including this file).
// The game multiplies what is under the car by 1 - d x T (its blend dst x src; d the shadow's
// strength, (1 - s) x c161.w per channel, T its own fog's transmittance): the fog's light already
// laid there darkened with the ground. The pixel there being ground x To + L (L the volume's light
// the road laid, To its transmittance), the shadow should darken the ground alone:
//   ground x (1 - d) x To + L = pixel x (1 - d) + d x L
// so with the volume given to the draw (draw.cpp CarShadowFogOn, which then sets its blend to
// src + dst x src.a, the screen's alpha kept) the shader returns (d x L, 1 - d), d grey (the
// shadow's channels averaged). L as the road's PgrFog (shader_replace.cpp) lays it: the volume's
// light at the pixel and depth, times its scale against the game's T, in the scene's units (the
// fog shaders' c12.y included, pgr4_native_volumetric_fog_c12 on).
#ifndef PGR4_CAR_SHADOW_FOG_HOOKS
#define PGR4_CAR_SHADOW_FOG_HOOKS
#include "fog_volume_hooks.hlsli"

// The volume given to this draw (draw.cpp: only with pgr4_native_car_shadow_fog and its view
// modes, its blend set).
bool PgrShadowOurs() {
#ifdef XE_HAS_POS
  return pgr4_fog_volume[3].w != 0.0;
#else
  return false;
#endif
}
// The volume's light and transmittance in column c (clamped), slice k (-1: the eye's, no light).
float4 PgrVolumeAt4(int2 c, int k) {
  if (k < 0) return float4(0.0, 0.0, 0.0, 1.0);
  int2 columns = int2(pgr4_fog_volume[0].zw);
  c = clamp(c, int2(0, 0), columns - 1);
  return xe_textures_3d[PGR4_FOG_VOLUME_INDEX].Load(int4(c, min(k, kPgrSlices - 1), 0));
}
// The fog's light at the shadow's point as the road laid it (shader_replace.cpp PgrFog): the volume
// at the pixel and view depth (as PgrVolumeT), scaled against the game's T there; d its distance
// from the eye, h its height (the game's interpolator), t the game's T.
float3 PgrShadowLight(float t, float d, float h, float3 eye, float4 pos) {
  float4 v0 = pgr4_fog_volume[0];
  float4 v1 = pgr4_fog_volume[1];
  float2 uv = pos.xy / v1.zw;
  float3 ray = float3(float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0) * v1.xy, 1.0);
  float z = min(d / length(ray), v0.x);
  float growth = v0.y;
  float e1 = kPgrVolumeNear * exp(growth);
  float sl = z < e1 ? z / e1 - 1.0 : min(log(z / kPgrVolumeNear) / growth, float(kPgrSlices)) - 1.0;
  int k0 = int(floor(sl));
  float fk = sl - float(k0);
  float2 cc = pos.xy / 16.0 - 0.5;
  int2 c0 = int2(floor(cc));
  float2 fc = cc - float2(c0);
  float4 v = 0.0;
  [unroll] for (int n = 0; n < 4; ++n) {
    int2 o = int2(n & 1, n >> 1);
    float w = (o.x ? fc.x : 1.0 - fc.x) * (o.y ? fc.y : 1.0 - fc.y);
    v += w * lerp(PgrVolumeAt4(c0 + o, k0), PgrVolumeAt4(c0 + o, k0 + 1), fk);
  }
  float tg = saturate(t);
  float ratio = min(v.a / max(tg, 1e-6), tg * 2.0 + 1.0);
  float to = tg * ratio;
  float scale = to < v.a ? (1.0 - to) / max(1.0 - v.a, 1e-4) : 1.0;
  float3 light = v.rgb * scale;
  return all(isfinite(light)) ? max(light, 0.0) : 0.0;
}
// The way out with the volume: minus_d the game's (s - 1) x c161.w per channel, t its T.
float4 PgrShadowOut(float3 minus_d, float t, float d, float h, float3 eye, float4 pos) {
  float dg = saturate(dot(-minus_d, float3(1.0, 1.0, 1.0) / 3.0));
  return float4(dg * PgrShadowLight(t, d, h, eye, pos), 1.0 - dg);
}
#endif
