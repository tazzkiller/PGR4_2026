// The cars' reflections of their surroundings (local change, pgr4 project): the hook of every
// car shader that reads the surroundings' cube map (tf3), generated from its ucode by
// tools/ucode2hlsl.py: the body's paint (car_paint_hooks.hlsli), the glass and the chrome.
//   #define XeEnv(tf, dir) ...      the surroundings' cube map fetch (dir: the reflection)
// The rain's drops running on the glass (PGR4_DROPS_GLIDE: the shaders of the windscreens and windows,
// whose reading of tf7 goes through XeDrops7): drops_run.hlsli. r2 is the vector to the eye: the
// shaders' reflection, 2(n.v)n - v, says which.
#ifdef PGR4_DROPS_GLIDE
#include "drops_run.hlsli"
float4 XeStacked7(float3 uvw);
static float2 gXeSnowUv = float2(0.0, 0.0);  // where the snow is read (snow.hlsli)
static float gXeSnowKeep = 1.0;
#include "snow.hlsli"
// The night windscreens (PGR4_WIPER_NIGHT): the game's night shaders have no sweep, but it leaves its data
// set: draw.cpp gives it here (pgr4_wiper*, measured 06/11/2026), so the fan is the same as by day.
#ifdef PGR4_WIPER_NIGHT
#define PGR4_WIPER
#ifndef c214
#define c214 pgr4_wiper_blade
#endif
#endif
#ifdef PGR4_WIPER
// The sweep's mask at this pixel: .z where the blade passes, .x the angle, 0 to 1.
float4 XeWiperSweep(XeInput i) {
#ifdef PGR4_WIPER_NIGHT
  if (pgr4_wiper_mask.x == 0u) return float4(0.0, 0.0, 0.0, 0.0);
  const float4 at = float4(-i.r2.xyz, 1.0);
  const float w = dot(pgr4_wiper[3], at);
  if (abs(w) < 1e-6) return float4(0.0, 0.0, 0.0, 0.0);
  const float2 uv = float2(dot(pgr4_wiper[0], at), dot(pgr4_wiper[1], at)) / w;
  if (any(uv < 0.0) || any(uv > 1.0)) return float4(0.0, 0.0, 0.0, 0.0);
  return xe_textures_2d[pgr4_wiper_mask.x].SampleLevel(XE_SAMPLER_TF7, float3(uv, 0.0), 0.0);
#else
  return TEX2D(6, i.r3.xy / i.r3.w);
#endif
}
#endif
float4 XeDrops7(XeInput i, float3 uvw) {
  if (XeSnowing) {
    gXeSnowUv = uvw.xy * 0.8;  // the glass's own coordinates, about 0.67 a metre
    {
      float3 frost_n = normalize(i.r1.xyz);
      if (dot(frost_n, i.r2.xyz) < 0.0) frost_n = -frost_n;
      gXeFrostMask = XeFrostPatch(frost_n);
    }
    if (XeHeavySnow) return float4(0.06, 0.06, 0.06, 0.06);
    // The rain's texture under the flakes: as in the rain, none running, none in the wiper's fan.
#ifdef XE_HAS_POS
    const float snow_contrast = 1.5 + 7.0 * smoothstep(1.5, 6.0, i.pos.w);
#else
    const float snow_contrast = 1.0;
#endif
    const float4 snow_still = saturate(0.06 + (XeStacked7(uvw) - 0.06) * snow_contrast);
#ifdef PGR4_WIPER
    const float snow_fan = XeOurCar ? smoothstep(0.1, 0.4, XeWiperSweep(i).z) : 0.0;
    return snow_still * (1.0 - snow_fan);
#else
    return snow_still;
#endif
  }
  // Stronger from afar, where a drop is a few pixels bending a reflection and the texture's reduced
  // levels bring little: nothing changes up close (under 2.5 m).
#ifdef XE_HAS_POS
  const float far = smoothstep(2.5, 8.0, i.pos.w);
#else
  const float far = 0.0;
#endif
#ifdef PGR4_WIPER
  // The wiper as for the snow (XeGlassFinish): its fan holds none of the texture's drops, only the running
  // ones, and the blade takes for good those it passes: new ones land elsewhere (cycle measured once: 1.4 s).
  // The game's shader then weighs what we return by its own sweep, which pulses over the whole fan: divided out.
  const float4 sweep = XeWiperSweep(i);
  const float fan = XeOurCar ? smoothstep(0.1, 0.4, sweep.z) : 0.0;
  if (fan > 0.5) {
    const float at = sweep.x * 0.5, w = c214.w;
    gXeDropsWiped = (w < at ? w + at : (w < 1.0 - at ? w - at : w - (1.0 - at))) * 1.4;
  }
#endif
  const float value = saturate(XeDropsRunner(i, uvw.xy, i.r2.xyz, 0.0) * (1.0 + 1.4 * far));
  // The texture's drops as visible from afar as up close: its reduced levels average the drops away
  // (their contrast falls by half and more), so it is given back with the level read, nothing
  // close up. Around its mean, so the glass as a whole keeps its brightness.
  // By the distance, not the level read: a glass seen at an angle reads small levels up close.
#ifdef XE_HAS_POS
  const float contrast = 1.5 + 5.0 * smoothstep(1.5, 6.0, i.pos.w);
#else
  const float contrast = 1.0;
#endif
  const float4 still = saturate(0.06 + (XeStacked7(uvw) - 0.06) * contrast);
#ifdef XE_HAS_POS
  gXeDropDim = saturate((max(still.x, value) - 0.06) * 2.0) * smoothstep(2.0, 5.0, i.pos.w);
#endif
#ifdef PGR4_WIPER
  // On the other cars, the game's own sweep. Its night shaders have none to divide out.
#ifdef PGR4_WIPER_NIGHT
  const float game = 1.0;
#else
  const float game = XeOurCar ? lerp(1.0, sweep.x * (1.0 - c214.z), sweep.z) : 1.0;
#endif
  return saturate(max(still * (1.0 - fan), float4(value, value, value, value)) / max(game, 0.05));
#else
  return max(still, float4(value, value, value, value));
#endif
}
#endif

#ifdef XE_HAS_CUBE3

// The sun's highlight on the clear coat, from the scene's sun (pgr4_sun_direction and
// pgr4_sun_color: the game's own, which the engine reads from the scenery's shaders), where the
// game's shader lights with the sun (XeEnvImproved).
// Tuned hot from the overlay (PGR4_TUNE_*, shader_replace.cpp); the defaults otherwise.
#ifndef PGR4_TUNE_kSunGlint
#define PGR4_TUNE_kSunGlint 0.0
#define PGR4_TUNE_kSunSharpness 600.0
#endif
static const float kSunGlint = PGR4_TUNE_kSunGlint;          // its strength
static const float kSunSharpness = PGR4_TUNE_kSunSharpness;  // its size: larger is smaller
// The glass's reflection (the shaders reading the drops on tf7: windscreens and windows): its
// strength, and the share of it left where it reflects the sky (directions rising above
// kGlassSkyFrom): a rainy day's grey sky veiled the middle of the windscreen; three quarters of it kept (Tazz). 1, 1: the game's.
#ifndef PGR4_TUNE_kGlassReflect
#define PGR4_TUNE_kGlassReflect 1.0
#define PGR4_TUNE_kGlassSky 1.0
#define PGR4_TUNE_kGlassSkyFrom 0.0
#endif
static const float kGlassReflect = PGR4_TUNE_kGlassReflect;
static const float kGlassSky = PGR4_TUNE_kGlassSky;
static const float kGlassSkyFrom = PGR4_TUNE_kGlassSkyFrom;

float XeLuminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// How much of the sun the car sees: the surroundings' cube map towards the sun against its
// average over the six faces (a tunnel, a building or a bridge between: dark there).
float XeSunVisible(float3 sun) {
  float average = 0.0;
  [unroll] for (int k = 0; k < 6; ++k) {
    float3 d = float3(k == 0 ? 1.0 : k == 1 ? -1.0 : 0.0, k == 2 ? 1.0 : k == 3 ? -1.0 : 0.0,
                      k == 4 ? 1.0 : k == 5 ? -1.0 : 0.0);
    average += XeLuminance(TEXCUBE_LEVEL(3, d, 0.0).rgb);
  }
  average /= 6.0;
  float toward = XeLuminance(TEXCUBE_LEVEL(3, sun, 0.0).rgb);
  return smoothstep(0.6, 1.4, toward / max(average, 1e-4));
}

// The reflection of the surroundings: anti-aliased (four samples over the pixel's footprint of
// the reflected direction instead of one: no stairs or speckles on curved panels, the same
// brightness), plus the sun's sharp highlight (a normalized Phong lobe around the sun's
// direction). The game's shader weighs the result as its own reflection (clear coat, view angle).
float4 XeEnvImproved(float3 dir) {
#ifdef PGR4_DROPS_RUN
  // A running drop shows the reflection from another angle, on the sides too: no white added.
  dir = dir + float3(0.0, gXeDropsRunning * 1.5 * (1.0 - 0.6 * gXeDropDim), 0.0);
#endif
  float3 dx = ddx(dir) * 0.35, dy = ddy(dir) * 0.35;
  float4 env = 0.25 * (TEXCUBE(3, dir + dx + dy) + TEXCUBE(3, dir + dx - dy) +
                       TEXCUBE(3, dir - dx + dy) + TEXCUBE(3, dir - dx - dy));
#if defined(PGR4_DROPS_RUN) || defined(PGR4_DROPS_GLIDE)
#ifdef PGR4_DROPS_RUN
  env.rgb *= 1.0 - kDropDimPaint * gXeDropDim;  // the paint: its drops stayed too white at kDropDim (Tazz)
#else
  env.rgb *= 1.0 - kDropDim * gXeDropDim;
#endif
#endif
  // Only where the game's own shader lights with the sun (it reads c6, the sun's direction: the
  // prelude defines c6 only then). Its night variants (psCar*Night*) do not, nor its mirrors and
  // blurred wheels: the scenery's night shaders still give a sun, which shone on the cars at night.
#ifdef c6
  if (pgr4_sun_color.w > 0.0) {
    float3 sun = normalize(pgr4_sun_direction.xyz);
    float lobe = pow(saturate(dot(normalize(dir), sun)), kSunSharpness) *
                 (kSunSharpness + 2.0) / 6.2832;
    env.rgb += pgr4_sun_color.rgb * lobe * kSunGlint * XeSunVisible(sun);
  }
#endif
#ifdef XE_HAS_STACKED7
  const float sky = smoothstep(kGlassSkyFrom, kGlassSkyFrom + 0.3, normalize(dir).y);
  env.rgb *= kGlassReflect * lerp(1.0, kGlassSky, sky);
#endif
#if defined(PGR4_DROPS_RUN) || defined(PGR4_DROPS_GLIDE)
  env.rgb *= 1.0 - gXeFrostMask;
#endif
  return env;
}
// Only tf3 (the surroundings): a shader's other cube maps (tf: a literal) are read as the game does.
#define XeEnv(tf, dir) XeEnvTf##tf(dir)
#define XeEnvTf3(dir) XeEnvImproved(dir)
#define XeEnvTf0(dir) TEXCUBE(0, dir)
#define XeEnvTf1(dir) TEXCUBE(1, dir)
#define XeEnvTf2(dir) TEXCUBE(2, dir)
#define XeEnvTf4(dir) TEXCUBE(4, dir)
#define XeEnvTf5(dir) TEXCUBE(5, dir)
#define XeEnvTf6(dir) TEXCUBE(6, dir)
#define XeEnvTf7(dir) TEXCUBE(7, dir)
#define XeEnvTf8(dir) TEXCUBE(8, dir)
#define XeEnvTf9(dir) TEXCUBE(9, dir)
#define XeEnvTf10(dir) TEXCUBE(10, dir)
#define XeEnvTf11(dir) TEXCUBE(11, dir)
#define XeEnvTf12(dir) TEXCUBE(12, dir)
#define XeEnvTf13(dir) TEXCUBE(13, dir)
#define XeEnvTf14(dir) TEXCUBE(14, dir)
#define XeEnvTf15(dir) TEXCUBE(15, dir)
#define XeEnvTf16(dir) TEXCUBE(16, dir)
#define XeEnvTf17(dir) TEXCUBE(17, dir)
#define XeEnvTf18(dir) TEXCUBE(18, dir)
#define XeEnvTf19(dir) TEXCUBE(19, dir)
#define XeEnvTf20(dir) TEXCUBE(20, dir)
#define XeEnvTf21(dir) TEXCUBE(21, dir)
#define XeEnvTf22(dir) TEXCUBE(22, dir)
#define XeEnvTf23(dir) TEXCUBE(23, dir)
#define XeEnvTf24(dir) TEXCUBE(24, dir)
#define XeEnvTf25(dir) TEXCUBE(25, dir)
#define XeEnvTf26(dir) TEXCUBE(26, dir)
#define XeEnvTf27(dir) TEXCUBE(27, dir)
#define XeEnvTf28(dir) TEXCUBE(28, dir)
#define XeEnvTf29(dir) TEXCUBE(29, dir)
#define XeEnvTf30(dir) TEXCUBE(30, dir)
#define XeEnvTf31(dir) TEXCUBE(31, dir)
#endif

#ifdef PGR4_DROPS_GLIDE
// The snow on the glass, seen from outside only (the views from the car have none, as for the drops).
float4 XeGlassFinish(float4 color, XeInput i) {
  if (!XeSnowing || !XeOurCar) return color;  // the car the camera is on alone: it costs too much for all
  float3 n = normalize(i.r1.xyz);
  if (dot(n, i.r2.xyz) < 0.0) n = -n;
#ifdef XE_HAS_POS
  const float outside = smoothstep(kDropsNear, kDropsNear + 0.4, i.pos.w);
#else
  const float outside = 1.0;
#endif
#ifdef PGR4_WIPER
  // The wiper's fan (slot 6, the sweep's mask): .z where it passes, .x the angle, 0 to 1. c214.z the blade's
  // angle (checked: it lies where .x equals it), c214.w its place in a cycle, 0 to 1, out then back. No lying
  // snow there; flakes land, and the blade takes those older than its last passage. Cycle measured once: 1.4 s.
  const float4 sweep = XeWiperSweep(i);
  const float fan = XeOurCar ? smoothstep(0.1, 0.4, sweep.z) : 0.0;
  gXeSnowLying = 1.0 - fan;
  if (fan > 0.5) {
    const float at = sweep.x * 0.5, w = c214.w;
    const float since = w < at ? w + at : (w < 1.0 - at ? w - at : w - (1.0 - at));
    gXeFlakeSince = since * 1.4;
    gXeFlakeEvery = 6.0;
    gXeFlakeSmaller = 2.5;
    gXeFlakeGain = 0.45;
  }
  return XeSnowFinishGlass(color, n, outside);
#else
  return XeSnowFinishGlass(color, n, outside);
#endif
}
#define XeFinish(color) XeGlassFinish(color, i)
#endif
