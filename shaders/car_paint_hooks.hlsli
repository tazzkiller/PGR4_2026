// Car body paint (local change, pgr4 project): our changes to the game's body shader, which
// every variant (<hash>.ps.hlsl) carries as generated from its ucode by tools/ucode2hlsl.py.
// Hooks:
//   #define XeEnv(tf, dir) ...      the surroundings' cube map fetch (dir: the reflection)
//   #define XeFinish(color) ...     the color written
// The rain's drops (tf13: the 3D texture the glass reads too, tools/diffusion/rain_drops.py) at
// the glass's size: the paint reads them at its second UV set times c216.x, which spreads them
// about twice as large as the glass's projection (c218.x). Tuned hot (PGR4_TUNE_*).
#ifndef PGR4_TUNE_kDropsPaintScale
#define PGR4_TUNE_kDropsPaintScale 2.5
#endif
static const float kDropsPaintScale = PGR4_TUNE_kDropsPaintScale;
// The damage's scratches (slot 8, a layer per damage level: tools/diffusion/damage_hd.py) made
// smaller the same way (above 1, repeated more). Tuned hot.
#ifndef PGR4_TUNE_kScratchPaintScale
#define PGR4_TUNE_kScratchPaintScale 3.0
#endif
static const float kScratchPaintScale = PGR4_TUNE_kScratchPaintScale;
#define XeStackedUv(tf, uvw)                                                 \
  ((tf) == 13  ? float3((uvw).xy * kDropsPaintScale, (uvw).z)                \
   : (tf) == 8 ? float3((uvw).xy * kScratchPaintScale, (uvw).z) : (uvw))
// The grime's amount forced at will (the overlay: pgr4_dirt.x, below 0 the game's c220.x).
#ifdef c220
float4 XeC220Game() { return c220; }
#undef c220
float4 XeC220() {
  float4 c = XeC220Game();
  if (pgr4_dirt.x >= 0.0) c.x = pgr4_dirt.x;
  return c;
}
#define c220 XeC220()
#endif
// The weather's grime in slot 4 (frost when icy, road dust when dry: tools/diffusion/frost.py),
// at the paint's UV times c220.z: its size, tuned hot (above 1, smaller and repeated more).
#ifndef PGR4_TUNE_kFrostPaintScale
#define PGR4_TUNE_kFrostPaintScale 4.0
#endif
static const float kFrostPaintScale = PGR4_TUNE_kFrostPaintScale;
// The rain's drops running down the paint (PGR4_DROPS_RUN: the body's shaders, not the bonnet view's
// nor the ghost's, whose reading of tf13 goes through XeDrops13): drops_run.hlsli, at the size of
// the glass's drops: read by the game's shader as its own drops, which lean the reflection (no white,
// no lightening of the paint).
#ifdef PGR4_DROPS_RUN
#include "drops_run.hlsli"
#ifndef PGR4_TUNE_kDropsPaintStrength
#define PGR4_TUNE_kDropsPaintStrength 1.8
#endif
static const float kDropsPaintStrength = PGR4_TUNE_kDropsPaintStrength;
static float2 gXeSnowUv = float2(0.0, 0.0);  // where the snow is read (snow.hlsli)
static float gXeSnowKeep = 1.0;
static float gXeDropsRunning = 0.0;  // also bends the reflection (XeEnvImproved): the game's own lean
                                     // of the normal is for surfaces facing up, not the car's sides
// The frost (tf4, read after XeDrops13): where the paint's coordinates are stretched, as for the drops.
static float3 gXeFrostEye = float3(0.0, 0.0, 0.0);
static XeDropsCarFrame gXeFrostCar;
float4 XeStacked13(float3 uvw);
float4 XeDrops13(XeInput i, float3 uvw) {
  // Where the paint's coordinates are stretched (the joins), the drops are read at coordinates fixed on
  // the car (drops_run.hlsli): they stay, round. On another car, whose frame is not known, none there.
  const float stretched = smoothstep(1.4, 2.2, XeDropsStretchOf(uvw.xy, i.r4.xyz));
  const XeDropsCarFrame car = XeDropsCarFrameAt(i.r4.xyz, i.r1.xyz);
  gXeFrostEye = i.r4.xyz;
  gXeFrostCar = car;
  if (XeSnowing) {
    // The frame's coordinates: neither moving on the body nor stretched.
    gXeSnowUv = car.on_car > 0.5 ? car.best : uvw.xy;
    gXeSnowKeep = car.on_car > 0.5 ? 1.0 : 1.0 - stretched;
    gXeFrostMask = XeFrostPatch(normalize(i.r1.xyz));
    if (XeHeavySnow) return float4(0.06, 0.06, 0.06, 0.06);
  }
  // One reading, costly: at the car's coordinates where the paint's are stretched (it changes per triangle,
  // as the stretch is measured), else at the paint's; on a car without its frame, fading out there.
  // In the snow, the texture's drops alone: none running.
  float running = 0.0;
  if (!XeSnowing) {
    const bool on_frame = stretched > 0.5 && car.on_car > 0.5;
    running = saturate(XeDropsRunner(i, (on_frame ? car.best : uvw.xy) * kDropsPaintScale * 0.5, i.r4.xyz, 0.22) *
                       (on_frame || car.on_car > 0.5 ? 1.0 : 1.0 - stretched) * kDropsPaintStrength);
  }
  // The texture's drops as visible from afar as up close, as on the glass (car_reflect_hooks.hlsli).
  // By the distance, not the level read: a glass seen at an angle reads small levels up close.
#ifdef XE_HAS_POS
  // A little less from afar (Tazz, 06/11/2026: 7 and no fade of the running ones, too marked at a race's distance).
  const float contrast = 3.0 + 5.0 * smoothstep(1.5, 5.0, i.pos.w);
  running *= 1.0 - 0.25 * smoothstep(2.5, 6.0, i.pos.w);
#else
  const float contrast = 1.0;
#endif
  gXeDropsRunning = running;
  const float4 own = XeStacked13(uvw);
  const float4 fixed_on_car = XeStacked13(float3(car.faces[0], uvw.z)) * car.weights.x +
                              XeStacked13(float3(car.faces[1], uvw.z)) * car.weights.y +
                              XeStacked13(float3(car.faces[2], uvw.z)) * car.weights.z;
  const float4 drops = lerp(own, lerp(float4(0.06, 0.06, 0.06, 0.06), fixed_on_car, car.on_car), stretched);
  const float4 paint_drops = max(saturate(0.06 + (drops - 0.06) * contrast), float4(running, running, running, running));
#ifdef XE_HAS_POS
  gXeDropDim = saturate((paint_drops.x - 0.06) * 2.0) * smoothstep(2.0, 5.0, i.pos.w);
#endif
  return paint_drops;
}
#endif
#undef TEX2D
#if defined(PGR4_DROPS_RUN) && defined(XE_HAS_TF4)
// The frost: no streaks at the joins (06/11/2026). It is read at coordinates fixed on the car, three
// faces blended as for the drops. Its density: the frame's 0.53 a metre is for the paint's coordinates × c216.x
// (3), the frost's × c220.z (1); both measured the same on every car seen.
static const float kFrostFine = 6.0;
float4 XeFrostRead(float2 uv) {
  // Our grime (tools/diffusion/dirt.py) is made to repeat: it is read as the frost, which keeps it at the joins.
  // Without textures_hd.pak (or its HD version released) it is still ours: its 512 levels in textures\<hash>.bin
  // replace the game's, as the frost's (tools/diffusion/hd_to_bin512.py, 12/11/2026). The game's own, which does
  // not repeat (a checker on the car's frame), is only shown with the game as it was, without these shaders.
  const bool frost = pgr4_dirt.y > 0.5;
  const float4 own = XE_TEXTURE_TF4.Sample(XE_SAMPLER_TF4, float3(uv * (frost ? kFrostPaintScale : 1.0), 0.0));  // no frame known
  // Everywhere on the car, not only where its coordinates stretch: the stretch is measured per triangle, and the
  // switch between the two readings drew the triangles (06/11/2026).
  const XeDropsCarFrame car = gXeFrostCar;
  if (car.on_car <= 0.5) return own;
  const float k = kFrostPaintScale / 3.0;
  const float4 fixed_on_car =
      XE_TEXTURE_TF4.Sample(XE_SAMPLER_TF4, float3(car.faces[0] * k, 0.0)) * car.weights.x +
      XE_TEXTURE_TF4.Sample(XE_SAMPLER_TF4, float3(car.faces[1] * k, 0.0)) * car.weights.y +
      XE_TEXTURE_TF4.Sample(XE_SAMPLER_TF4, float3(car.faces[2] * k, 0.0)) * car.weights.z;
  if (!frost) {
    // Above an amount of 1, it rises more slowly: right at 1, too thick at 2 otherwise (Tazz, 07/11/2026).
#ifdef c220
    return fixed_on_car * lerp(1.0, 0.8125, saturate(c220.x - 1.0));
#else
    return fixed_on_car;
#endif
  }
  // A finer grain over it, the same frost for the amount: the texture's own detail 6 times smaller, around its
  // mean (its smallest level), for a picture less magnified (trial, Tazz 06/11/2026).
  const float4 mean = XE_TEXTURE_TF4.SampleLevel(XE_SAMPLER_TF4, float3(0.5, 0.5, 0.0), 12.0);
  const float fine = k * kFrostFine;
  const float4 detail =
      XE_TEXTURE_TF4.Sample(XE_SAMPLER_TF4, float3(car.faces[0] * fine, 0.0)) * car.weights.x +
      XE_TEXTURE_TF4.Sample(XE_SAMPLER_TF4, float3(car.faces[1] * fine, 0.0)) * car.weights.y +
      XE_TEXTURE_TF4.Sample(XE_SAMPLER_TF4, float3(car.faces[2] * fine, 0.0)) * car.weights.z;
  return saturate(fixed_on_car + (detail - mean) * pgr4_dirt.w * saturate(2.0 * fixed_on_car));
}
#define TEX2D(f, uv) ((f) == 4 ? XeFrostRead(uv) : XE_TEXTURE_TF##f.Sample(XE_SAMPLER_TF##f, float3((uv), 0.0)))
#else
#define TEX2D(f, uv) XE_TEXTURE_TF##f.Sample(XE_SAMPLER_TF##f, \
                                              float3((uv) * ((f) == 4 && pgr4_dirt.y > 0.5 ? kFrostPaintScale : 1.0), 0.0))
#endif

#ifdef XE_HAS_CUBE3

#include "car_reflect_hooks.hlsli"


// The color the variant writes, with one more layer (from every variant's inputs, as their ucode
// reads them: r1 the normal, r4 towards the camera):
#ifndef PGR4_TUNE_kRimReflection
#define PGR4_TUNE_kRimReflection 0.0
#endif
static const float kRimReflection = PGR4_TUNE_kRimReflection;  // the clear coat's extra reflection at grazing angles

#ifdef PGR4_DROPS_RUN
#include "snow.hlsli"
#endif
float4 XeFinishImproved(float4 color, XeInput i) {
  float3 n = normalize(i.r1.xyz);
  float3 v = normalize(i.r4.xyz);
  if (dot(n, v) < 0.0) n = -n;
  float nv = saturate(dot(n, v));
  float3 r = reflect(-v, n);
  // Fresnel (Schlick, the clear coat's 4 % head on): the game's own reflection weighs the view
  // angle with a coarse table; the edges of the body get the physically larger rest.
  float m = 1.0 - nv;
  float fresnel = 0.04 + 0.96 * m * m * m * m * m;
  float3 env = TEXCUBE_LEVEL(3, r, 0.0).rgb;
  color.rgb += env * c12.y * max(0.0, fresnel - 0.04) * kRimReflection;
#ifdef PGR4_DROPS_RUN
  if (XeSnowing && XeOurCar) {  // the car the camera is on alone: it costs too much for all
    // Ice: the paint under it cooler and darker, a clear mirror of the surroundings on top.
    const float ice = XeIceAmount(n);
    color.rgb = lerp(color.rgb, color.rgb * float3(0.82, 0.88, 0.94), ice * 0.6);
    color = XeSnowFinish(color, i, n);
  }
#endif
  return color;
}
#define XeFinish(color) XeFinishImproved(color, i)
#endif
