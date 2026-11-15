// The water's detail (local change, pgr4 project): the hook of the water's pixel shaders (vsWater's
// and vsWaterPlane's, generated from their ucode by tools/ucode2hlsl.py and patched by
// tools/water_patch.py). The game's water takes its slope from a small height map (tf6) read at
// the pixel and at four neighbours (its waves), then reflects the sky's cube map (tf7) and the sun
// (c6, c7: by day only) by it, weighed by a Fresnel table (tf2). Stretched over a whole river, the
// height map leaves the water smooth and still. Each read of tf6 here also gets the slope of finer
// waves that move: wind waves of 0.4 to 7 m (deep water's speed, sqrt(g wavelength / 2 pi)) and
// two layers of moving noise, written as a plane through the pixel, so that the game's own
// differences find exactly that slope, whatever its step: the reflections, the sun's glitter and
// the Fresnel weight all follow, in every variant, by day and by night as the game lights them.
// Waves smaller than four pixels fade out (no shimmer in the distance). Reloaded when written;
// the PGR4_TUNE_* ones tuned from the overlay.
//   i.r0.xy: the height map's coordinates (the world's x, z times the VS's c227.y), i.r2.xyz: the
//   world position; pgr4_time.x: seconds (the renderer's clock).

// Tuned from the overlay (PGR4_TUNE_*, shader_replace.cpp).
#ifndef PGR4_TUNE_kWaveStrength
#define PGR4_TUNE_kWaveStrength 0.35
#endif
#ifndef PGR4_TUNE_kWaveSteepness
#define PGR4_TUNE_kWaveSteepness 0.299
#endif
#ifndef PGR4_TUNE_kNoiseShare
#define PGR4_TUNE_kNoiseShare 1.0
#endif
#ifndef PGR4_TUNE_kWindAngle
#define PGR4_TUNE_kWindAngle -1.5
#endif
#ifndef PGR4_TUNE_kWaveSpeed
#define PGR4_TUNE_kWaveSpeed 0.3
#endif
#ifndef PGR4_TUNE_kWaterReflect
#define PGR4_TUNE_kWaterReflect 1.0
#endif
#ifndef PGR4_TUNE_kWaveSize
#define PGR4_TUNE_kWaveSize 0.1
#endif
static const float kWaveStrength = PGR4_TUNE_kWaveStrength;  // the detail's slope in the height map's units (0: the game's)
static const float kWaveSteepness = PGR4_TUNE_kWaveSteepness;  // the waves' slope: larger is choppier
static const float kNoiseShare = PGR4_TUNE_kNoiseShare;    // the moving noise's part, against the regular waves
static const float kWindAngle = PGR4_TUNE_kWindAngle;      // the wind's direction (radians, around the world's y)
static const float kWaveSpeed = PGR4_TUNE_kWaveSpeed;      // the waves' time runs this much faster
static const float kWaterReflect = PGR4_TUNE_kWaterReflect;  // the scenery's reflection (0: the game's sky alone)
static const float kWaveSize = PGR4_TUNE_kWaveSize;  // the waves' and patches' size (1: 0.4 to 7 m)

static float2 xe_wave_slope = 0.0;   // the detail's slope at the pixel (height map units per uv)
static float2 xe_wave_centre = 0.0;  // the pixel's height map coordinates

float2 XeHash2(float2 p) {
  p = float2(dot(p, float2(127.1, 311.7)), dot(p, float2(269.5, 183.3)));
  return frac(sin(p) * 43758.5453) * 2.0 - 1.0;
}

// Gradient noise's derivative at p (Inigo Quilez's analytic form): (d/dx, d/dy).
float2 XeNoiseGrad(float2 p) {
  float2 i = floor(p), f = frac(p);
  float2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
  float2 du = 30.0 * f * f * (f * (f - 2.0) + 1.0);
  float2 ga = XeHash2(i), gb = XeHash2(i + float2(1, 0));
  float2 gc = XeHash2(i + float2(0, 1)), gd = XeHash2(i + float2(1, 1));
  float va = dot(ga, f), vb = dot(gb, f - float2(1, 0));
  float vc = dot(gc, f - float2(0, 1)), vd = dot(gd, f - float2(1, 1));
  return ga + u.x * (gb - ga) + u.y * (gc - ga) + u.x * u.y * (ga - gb - gc + gd) +
         du * (u.yx * (va - vb - vc + vd) + float2(vb, vc) - va);
}

// The slope of the fine waves at the world's (x, z), in metres per metre; `footprint`: the pixel's
// size there (metres).
float2 XeWaveSlope(float2 w, float t, float footprint) {
  float2 slope = 0.0;
  // Eight wind waves, their directions spread around the wind's, their lengths 0.4 to 7 m.
  [unroll] for (int k = 0; k < 8; ++k) {
    float length = 7.0 * pow(0.66, k) * kWaveSize;
    float angle = kWindAngle + (frac(sin(k * 12.9898) * 43758.5453) - 0.5) * 1.6;
    float2 d = float2(cos(angle), sin(angle));
    float wavenumber = 6.2832 / length;
    float speed = sqrt(9.81 / wavenumber);
    float fade = saturate(length / (4.0 * footprint) - 1.0);
    float phase = wavenumber * (dot(d, w) - speed * t) + k * 1.7;
    // A slope of kWaveSteepness at the crests, so the same steepness at every size.
    slope += d * cos(phase) * kWaveSteepness * fade;
  }
  slope *= (1.0 - kNoiseShare) * 0.35;
  // Two layers of noise drifting with the wind: the waves' irregular patches.
  float2 wind = float2(cos(kWindAngle), sin(kWindAngle));
  [unroll] for (int n = 0; n < 2; ++n) {
    float scale = (n == 0 ? 0.55 : 1.9) / kWaveSize;  // cycles per metre
    float fade = saturate(1.0 / (scale * 4.0 * footprint) - 1.0);
    float2 p = w * scale + wind * t * (0.35 + 0.4 * n) * scale;
    slope += XeNoiseGrad(p) * scale * kWaveSteepness * kNoiseShare * 0.35 / scale * fade;
  }
  return slope;
}

void XeWaterBegin(XeInput i) {
  xe_wave_centre = i.r0.xy;
  float2 w = i.r2.xz;
  float footprint = max(length(fwidth(w)), 1e-3);
  // The height map's mapping from the world, whatever its offset, scale or axes (they differ
  // between the game's water meshes): metres per uv, from the two coordinates' screen derivatives.
  // A slope s in metres per metre is s * (metres per uv) in the height map's units.
  const float2x2 du = float2x2(ddx(i.r0.xy), ddy(i.r0.xy));  // rows: d(uv)/dx, d(uv)/dy
  const float2x2 dw = float2x2(ddx(w), ddy(w));
  const float det = determinant(du);
  const float size = dot(du[0], du[0]) + dot(du[1], du[1]);
  if (abs(det) > 1e-4 * size && size > 0.0) {
    // metres per uv: inverse(du) * dw (both row-vector maps from the screen).
    const float2x2 inv = float2x2(du[1].y, -du[0].y, -du[1].x, du[0].x) / det;
    const float2x2 m = mul(inv, dw);  // rows: d(w)/d(u), d(w)/d(v)
    xe_wave_slope = mul(m, XeWaveSlope(w, pgr4_time.x * kWaveSpeed, footprint)) * kWaveStrength;
  } else {
    xe_wave_slope = 0.0;
  }
}

// The game's height, plus the fine waves' plane through the pixel.
float4 XeWaterHeight(float4 height, float2 uv) {
  height.x += dot(xe_wave_slope, uv - xe_wave_centre);
  return height;
}

#include "scene_ray.hlsli"

// The reflections of the scenery: the sky's cube map (tf7) reflects only the sky; the scene's
// copy from before the water (scene_ray.hlsli), along the reflection the game computed (its cube
// map fetch's direction, in the world), projected by the water's own clip transform (its VS's
// c4-c7). The sky where the ray finds nothing.
float3 XeSceneReflection(float3 sky, float3 dir, float3 origin) {
  float fade;
  const float3 color = XeTraceScene(origin, dir, float3(0.0, 1.0, 0.0), 0, 1e30, 0.0, 0.0, fade);
  return lerp(sky, color, fade * kWaterReflect);
}

// The cube map fetch of the water (tf7) with the scenery's reflection; `i` is Shade's input.
#define XeEnv(tf, dir) XeWaterEnv##tf(dir)
#define XeWaterEnv7(dir) float4(XeSceneReflection(TEXCUBE(7, dir).rgb, dir, i.r2.xyz), TEXCUBE(7, dir).a)
