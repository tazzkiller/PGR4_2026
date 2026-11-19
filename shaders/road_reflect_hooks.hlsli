// The wet roads' reflections of the scene (local change, pgr4 project): the hook of the road
// shaders that read the lights' halo (tf13), generated from their ucode by tools/ucode2hlsl.py and
// patched by tools/road_reflect_patch.py. The game's wet road reflects only its lights: their
// sprites blurred at 320x180 (the halo), read mirrored on the screen only where the road is wet
// (a bool), weighed by (puddles x c34.x + c35.z) x c35.w and the view angle, the puddles a mask
// that changes at each grain of the asphalt. The scene's reflection weighed by that mask was faint
// dots that swarmed as the camera moved, so it is laid over the road's final color instead:
// where the game read its halo (XeRoadReflect marks it), a ray from the road along its reflection
// (scene_ray.hlsli) over the previous frame's copy of the scene (scene_copy.h, taken at the
// scene's end: the road is drawn before most of what it reflects), projected by that frame's
// clip transform, weighed by the rain (c34.x, the game's wetness), wetter and drier patches a few
// metres across fixed to the ground, and the clear water film's Fresnel (Schlick, kRoadFresnelMin head on);
// nothing beyond kRoadFar (the sky, the fog: the game's road already lights them), the reflection
// stretched up and down over what it hit only. Reloaded when written; the PGR4_TUNE_* ones tuned
// from the overlay (shader_replace.cpp).
//   i.r0: the road's normal (the VS's o0), i.r5: the pixel towards the eye (the VS's c24 minus the
//   world position), pgr4_camera: the eye (the VS's c24).

#include "scene_ray.hlsli"

#ifndef PGR4_TUNE_kRoadReflect
#define PGR4_TUNE_kRoadReflect 0.5
#endif
static const float kRoadReflect = PGR4_TUNE_kRoadReflect; // the reflection's strength (0: the game's road)
static const float kRoadFar = 150.0;     // metres: nothing beyond is reflected, fading out from half of it
static const float kRoadRise = 0.15;     // metres above the road the ray must be for a hit ...
static const float kRoadRiseGrowth = 0.01;  // ... plus this per metre along it (the coarser depth far away)
#ifndef PGR4_TUNE_kRoadPuddles
#define PGR4_TUNE_kRoadPuddles 0.35
#endif
static const float kRoadPuddles = PGR4_TUNE_kRoadPuddles;  // the wetness's variation over the road (0: even)
static const float kRoadPuddleSize = 4.0;  // metres across a patch
#ifndef PGR4_TUNE_kRoadBlur
#define PGR4_TUNE_kRoadBlur 3.0
#endif
static const float kRoadBlur = PGR4_TUNE_kRoadBlur;     // the copy's level the reflection is read from (0: sharp, 3: the most blurred)
// Rays per pixel, spread as a rough road spreads its reflection (1, 2 or 4): one, stretched by
// kRoadStreak instead (more rays, a sign far away was reflected once per ray, copies stacked).
static const int kRoadRays = 1;
static const float kRoadSpread = 0.03;  // their spread across (radians)
// ... and along the view (radians): a wet asphalt's film is rough, its reflections of lights
// stretched towards the viewer (the long streaks of a wet road at night), by its microfacets
// rather than by smearing the screen.
static const float kRoadSpreadAlong = 0.12;
static const int kRoadStreak = 0;        // the reflection stretched over this many of its blurred level's texels up and down
#ifndef PGR4_TUNE_kRoadFresnelMin
#define PGR4_TUNE_kRoadFresnelMin 0.08
#endif
static const float kRoadFresnelMin = PGR4_TUNE_kRoadFresnelMin;  // the reflection seen from above (water's own: 0.02; higher looks wetter)
static const float kRoadWetScale = 1.0;  // the game's wetness (c34.x) times this, at most 1
static const float kRoadSoften = 0.3;    // the bright reflections (signs, neons) softened: c / (1 + this x its luminance)
static const float kRoadLift = 0.05;     // metres above the road the ray starts
// The reflection broken by the asphalt's grain as the game's own is: weighed by the game's puddles'
// mask (xe_road_mask: 1 where wettest), at least this much where it is 0.
#ifndef PGR4_TUNE_kRoadGrain
#define PGR4_TUNE_kRoadGrain 0.35
#endif
static const float kRoadGrain = PGR4_TUNE_kRoadGrain;
static const float kRoadDetail = 0.3;  // the less blurred level mixed in (thin bright lines kept, faint)
static const float kRoadOccluded = 0.0;  // what hides the reflected (a barrier before a sign's foot) reflected instead, this much
// Metres behind a vehicle a ray still hits it (scene_ray.hlsli xe_ray_vehicle; 0: as any surface,
// kReflectThickness 1.5 m): local change, 19/11/2026.
static const float kRoadVehicleThickness = 0.4;
// The ray's steps: each this much longer, this many (0.4 m x 1.1^64: about 175 m, past kRoadFar);
// longer steps jumped over a sign's plate at some distances, cutting its reflection by moments.
static const float kRoadStepGrowth = 1.1;
static const int kRoadSteps = 64;
static const float kRoadBump = 0.02;      // the relief's slope bending the reflection (0: a flat mirror)
static const float kRoadBumpSize = 0.6;    // metres across its bumps
// The rain's strength (0: light rain, 1: heavy rain or a storm) around the settings above (at
// 0.5), the gains of each side (kRainLighter below the middle, kRainHeavier above), at the ends:
// the reflection's strength x (1 -+ x / 2), its Fresnel head on x (1 -+ y / 2), the copy's level
// read +- z / 2 (heavier rain's film smoother), the grain -+ w / 2 (heavier: less broken); the
// wetter and drier patches x (1 +- kRainPuddles / 2).
// The game's rain setting the weather fixes (its VS's c210.x, pgr4_time.z): light rain ...
static const float kRainLight = 0.05;
static const float kRainHeavy = 0.025;  // ... and heavy rain or a storm (the same rain)
static const float4 kRainLighter = float4(0.6, 1.0, 0.8, 0.3);
static const float4 kRainHeavier = float4(1.0, 1.4, 0.9, 0.4);
static const float kRainPuddles = 1.0;
#ifndef PGR4_TUNE_kRoadSheen
#define PGR4_TUNE_kRoadSheen 0.0
#endif
static const float kRoadSheen = PGR4_TUNE_kRoadSheen;  // a soft even sheen where the rays find nothing: the top of the screen's average colour (0: none)
// The six above are heavy rain's and a storm's (the game gives both the same rain); light rain's
// own (kRainLight), each tuned from the overlay apart.
#ifndef PGR4_TUNE_kRoadReflectLight
#define PGR4_TUNE_kRoadReflectLight 0.65
#endif
static const float kRoadReflectLight = PGR4_TUNE_kRoadReflectLight;
#ifndef PGR4_TUNE_kRoadBlurLight
#define PGR4_TUNE_kRoadBlurLight 2.5
#endif
static const float kRoadBlurLight = PGR4_TUNE_kRoadBlurLight;
#ifndef PGR4_TUNE_kRoadFresnelMinLight
#define PGR4_TUNE_kRoadFresnelMinLight 0.02
#endif
static const float kRoadFresnelMinLight = PGR4_TUNE_kRoadFresnelMinLight;
#ifndef PGR4_TUNE_kRoadGrainLight
#define PGR4_TUNE_kRoadGrainLight 0.35
#endif
static const float kRoadGrainLight = PGR4_TUNE_kRoadGrainLight;
#ifndef PGR4_TUNE_kRoadPuddlesLight
#define PGR4_TUNE_kRoadPuddlesLight 0.35
#endif
static const float kRoadPuddlesLight = PGR4_TUNE_kRoadPuddlesLight;
#ifndef PGR4_TUNE_kRoadSheenLight
#define PGR4_TUNE_kRoadSheenLight 0.0
#endif
static const float kRoadSheenLight = PGR4_TUNE_kRoadSheenLight;
static const float kRoadSky = 0.0;  // the sky reflected where the rays find nothing (0: none)
static const int kRoadDebug = 0;  // 1: the weight in grey (red: no scene copy), 2: the game's puddles, 3: the sky found (red), 4: the game's wetness c34.x, c35.z, c35.w (red, green, blue), 5: the same as gauges, 6: the game's rain settings as gauges (ticks every 0.01), 7: the weight in bands, 8: the Fresnel in bands

static bool xe_road_wet = false;

// Smooth value noise in [0, 1] at the world's (x, z): the road's wetter and drier patches.
float XeRoadNoise(float2 p) {
  const float2 i = floor(p), f = frac(p);
  const float2 u = f * f * (3.0 - 2.0 * f);
  const float4 h = frac(sin(float4(dot(i, float2(127.1, 311.7)),
                                   dot(i + float2(1, 0), float2(127.1, 311.7)),
                                   dot(i + float2(0, 1), float2(127.1, 311.7)),
                                   dot(i + float2(1, 1), float2(127.1, 311.7)))) * 43758.5453);
  return lerp(lerp(h.x, h.y, u.x), lerp(h.z, h.w, u.x), u.y);
}

// The game's puddles' mask where its road is wet (its wetness over c34.x): 1 in its puddles.
static float xe_road_mask = 0.0;

// The game's halo fetch, only where its road is wet: as it is, and marked, with the game's
// wetness computed just before it (tools/road_reflect_patch.py finds it).
float4 XeRoadReflect(float4 halo, XeInput i, float wetness) {
  xe_road_wet = true;
#ifdef c34
  xe_road_mask = saturate(wetness / max(c34.x, 1e-4));
#endif
  return halo;
}

float4 XeRoadFinish(float4 color, XeInput i) {
  if (!xe_road_wet || max(kRoadReflect, kRoadReflectLight) <= 0.0) return color;
  if (pgr4_scene.z == 0u) return kRoadDebug ? float4(1.0, 0.0, 0.0, color.a) : color;
  float wet = 1.0;
#ifdef c34
  wet = saturate(c34.x * kRoadWetScale);
#endif
  const float3 to_eye = i.r5.xyz;
  const float3 world = pgr4_camera.xyz - to_eye;
  const float2 patch = world.xz / kRoadPuddleSize;
  // The rain's strength from the weather the game chose (draw.cpp: the setting it gives its rain,
  // pgr4_time.z; not its c209.x, pgr4_time.y, which it fades as the car speeds up), kRainLight 0,
  // kRainHeavy 1; the settings above at its middle: lighter rain a duller, more broken film,
  // heavier a smoother, brighter one (more water: the asphalt's pores filled).
  // No rain drawn: no reflection. In clear weather the game still marks its roads wet at its
  // wetness 1 (its halo of lights, by night), but the asphalt is dry.
  if (pgr4_time.z <= 0.0 && kRoadDebug == 0) return color;
  const float rain = pgr4_time.z > 0.0
                         ? saturate((pgr4_time.z - kRainLight) / (kRainHeavy - kRainLight))
                         : 0.5;
  const float more = rain - 0.5;
  const float4 gain = more > 0.0 ? kRainHeavier : kRainLighter;
  // The weather's own settings: heavy rain or a storm, or light rain.
  const bool heavy = more > 0.0;
  const float road_reflect = heavy ? kRoadReflect : kRoadReflectLight;
  const float road_blur = heavy ? kRoadBlur : kRoadBlurLight;
  const float road_fresnel = heavy ? kRoadFresnelMin : kRoadFresnelMinLight;
  const float road_grain = heavy ? kRoadGrain : kRoadGrainLight;
  const float road_puddles = heavy ? kRoadPuddles : kRoadPuddlesLight;
  const float road_sheen = heavy ? kRoadSheen : kRoadSheenLight;
  const float puddles = saturate(road_puddles * (1.0 - kRainPuddles * more));
  wet *= 1.0 - puddles * (0.65 * XeRoadNoise(patch) + 0.35 * XeRoadNoise(patch * 2.7 + 5.3));
  const float3 n = normalize(i.r0.xyz);
  const float3 v = normalize(to_eye);
  const float m = 1.0 - saturate(dot(n, v));
  const float fresnel_min = road_fresnel * (1.0 + gain.y * more);
  const float fresnel = fresnel_min + (1.0 - fresnel_min) * m * m * m * m * m;
  const float grain = saturate(road_grain + gain.w * more);
  const float weight = wet * fresnel * road_reflect * (1.0 + gain.x * more) *
                       lerp(grain, 1.0, xe_road_mask);
  const float blur = clamp(road_blur - gain.z * more, 0.0, 3.0);
  if (kRoadDebug == 2) return float4(xe_road_mask.xxx, color.a);
  if (kRoadDebug == 6) {
    // The game's rain settings as gauges (white from the left up to them, the screen's width:
    // 0.2): c209.x above the screen's bottom eighth, c210.x in it.
    const float2 at = i.pos.xy / (2.0 * pgr4_view[0].zw);
    const float value = (at.y < 0.875 ? pgr4_time.y : pgr4_time.z) / 0.2;
    return float4((at.x < value ? 1.0 : 0.0).xxx + (frac(at.x * 20.0) < 0.01 ? 0.5 : 0.0),
                  color.a);
  }
#if defined(c34) && defined(c35)
  if (kRoadDebug == 4) return float4(c34.x, c35.z, c35.w, color.a);
  if (kRoadDebug == 5) {
    // Gauges, whatever the post-processing: white from the left up to the value (the screen's
    // width: 1), c34.x above the screen's bottom eighth, c35.w / 8 in it.
    const float2 at = i.pos.xy / (2.0 * pgr4_view[0].zw);
    const float value = at.y < 0.875 ? c34.x : c35.w / 8.0;
    const float tick = frac(at.x * 10.0) < 0.01 ? 0.5 : 0.0;
    return float4((at.x < value ? 1.0 : 0.0).xxx + tick, color.a);
  }
#endif
  if (kRoadDebug == 3) {
    float sf;
    XeSkyColor(reflect(-normalize(to_eye), normalize(i.r0.xyz)), 0.0, sf);
    return float4(sf, 0.0, 0.0, color.a);
  }
  if (kRoadDebug == 7 || kRoadDebug == 8) {
    // Bands whatever the exposure: black below 0.02, then blue, green, yellow, red from 0.02,
    // 0.05, 0.1, 0.2; 7: the weight, 8: its Fresnel alone.
    const float v = kRoadDebug == 7 ? weight : fresnel;
    const float3 band = v < 0.02 ? float3(0, 0, 0)
                      : v < 0.05 ? float3(0, 0, 1)
                      : v < 0.1  ? float3(0, 1, 0)
                      : v < 0.2  ? float3(1, 1, 0) : float3(1, 0, 0);
    return float4(band, color.a);
  }
  if (kRoadDebug) return float4(weight.xxx, color.a);
  if (weight <= 0.001) return color;
  // Rays spread around the reflection, in a fixed pattern (the same each frame), averaged: the
  // reflections' edges are gradients instead of cuts, which blinked as the camera moved.
  xe_ray_detail = kRoadDetail;
  xe_ray_rise_growth = kRoadRiseGrowth;
  xe_ray_occluded = kRoadOccluded;
  xe_ray_vehicle = kRoadVehicleThickness;
  xe_ray_growth = kRoadStepGrowth;
  xe_ray_steps = kRoadSteps;
  // The asphalt's relief under its water film, fixed to the ground: the reflection bent by it
  // (wavy, in the surface rather than laid over it). Its gradient from a smooth noise kRoadBumpSize
  // metres across (and a finer one, faded out with the distance where it would turn to noise).
  const float dist = length(to_eye);
  const float2 b = world.xz / kRoadBumpSize;
  const float e = 0.05;
  const float fine = 1.0 - smoothstep(8.0, 25.0, dist);
  const float h0 = XeRoadNoise(b) + 0.5 * fine * XeRoadNoise(b * 3.1 + 7.7);
  const float hx = XeRoadNoise(b + float2(e, 0)) + 0.5 * fine * XeRoadNoise((b + float2(e, 0)) * 3.1 + 7.7);
  const float hz = XeRoadNoise(b + float2(0, e)) + 0.5 * fine * XeRoadNoise((b + float2(0, e)) * 3.1 + 7.7);
  const float3 nb = normalize(n + kRoadBump * float3(h0 - hx, 0.0, h0 - hz) / e);
  const float3 r = reflect(-v, nb);
  const float3 tx = normalize(cross(r, abs(r.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0)));
  const float3 ty = cross(r, tx);
  const float2 pattern[4] = {float2(0.0, -0.15), float2(0.6, 0.45), float2(-0.6, -0.75),
                             float2(0.2, 1.0)};
  float3 scene = 0.0;
  float fade = 0.0;
  [unroll] for (int k = 0; k < 4; ++k) {
    if (k >= kRoadRays) break;
    const float3 dir =
        normalize(r + tx * (pattern[k].x * kRoadSpread) + ty * (pattern[k].y * kRoadSpreadAlong));
    float f;
    const float3 c = XeTraceScene(world + n * kRoadLift, dir, n, kRoadStreak, kRoadFar, kRoadRise,
                                  blur, f);
    scene += c * f;
    fade += f;
  }
  scene /= max(fade, 1e-4);
  fade /= float(min(kRoadRays, 4));
  // Where the rays found nothing: the sky along the reflection (kRoadSky of it), if on screen.
  if (kRoadSky > 0.0 && fade < 0.999) {
    float sky_fade;
    const float3 sky = XeSkyColor(r, blur, sky_fade);
    const float rest = (1.0 - fade) * sky_fade * kRoadSky;
    scene = (scene * fade + sky * rest) / max(fade + rest, 1e-4);
    fade += rest;
  }
  // Where the rays found nothing: an even sheen, the average colour of the screen's top (the sky or
  // the façades: what a wet road mirrors there), the same everywhere (no band where the sky ends).
  if (road_sheen > 0.0 && fade < 0.999 && pgr4_scene.z != 0u) {
    Texture2DArray<float4> colors = xe_textures_2d[pgr4_scene.x];
    const float2 size = float2(pgr4_scene.zw);
    float3 tint = 0.0;
    [unroll] for (int k = 0; k < 8; ++k)
      tint += XeLevelColor(colors, float2((k + 0.5) / 8.0 * size.x, size.y * 0.04), 3);
    tint /= 8.0;
#ifdef c12
    tint /= max(c12.y, 1e-4);
#endif
    const float rest = (1.0 - fade) * road_sheen;
    scene = (scene * fade + tint * rest) / max(fade + rest, 1e-4);
    fade += rest;
  }
#ifdef c12
  scene *= c12.y;  // back to the output's exposure, as the copy holds it
#endif
  scene /= 1.0 + kRoadSoften * dot(scene, float3(0.2126, 0.7152, 0.0722));
  // A copy's texel not finite (an overflowing light, a projection behind the eye) made the pixel
  // NaN, black, spread into flashing black blots by the bloom and the blurs after it: the road as
  // the game drew it there.
  const float amount = saturate(weight * fade);
  if (!all(isfinite(scene)) || !isfinite(amount)) return color;
  color.rgb = lerp(color.rgb, max(scene, 0.0), amount);
  return color;
}

#define XeFinish(color) XeRoadFinish(color, i)
