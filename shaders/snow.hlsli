// Snow on the car's paint (local change, pgr4 project): what the paint shows instead of the rain's
// drops when it snows (XeSnowing, drops_run.hlsli). A matte layer laid on what faces up, its edge
// uneven, and flakes that land one by one and melt away. gXeSnowUv: coordinates on the body
// (car_paint_hooks.hlsli), 0.53 a metre. No depth: it is painted on the surface.
#ifndef SNOW_HLSLI
#define SNOW_HLSLI
// The snow's style: 1 flecks (chosen), 2 a fine powder, 3 soft patches, 4 a veil and few flecks.
#define PGR4_SNOW_STYLE 1
// The lying snow: in heavy snow only (XeHeavySnow, drops_run.hlsli).
#define PGR4_SNOW_LAYER (XeHeavySnow ? 1.0 : 0.0)
// The frost on the car: 1 a light crystal veil, 2 ferns, 3 thick patches of ferns, 4 light white patches,
// 5 ice and snow: a glassy sheet of ice, with snow lying only in the middle of its patches.
#define PGR4_FROST 0
// The size of its patches (above 1, smaller): 3.5 a few tens of centimetres.
#if PGR4_FROST == 5
static const float kFrostPatch = 22.0;
#else
static const float kFrostPatch = 3.5;
#endif
#if PGR4_FROST == 5
static const float kFrostVeil = 0.55, kFrostFerns = 0.4, kFrostScale = 1000.0, kFrostFrom = 0.47;
#elif PGR4_FROST == 4
static const float kFrostVeil = 0.42, kFrostFerns = 0.1, kFrostScale = 220.0, kFrostFrom = 0.38;
#elif PGR4_FROST == 1
static const float kFrostVeil = 0.3, kFrostFerns = 0.25, kFrostScale = 90.0, kFrostFrom = 0.25;
#elif PGR4_FROST == 3
static const float kFrostVeil = 0.32, kFrostFerns = 0.65, kFrostScale = 45.0, kFrostFrom = 0.42;
#else
static const float kFrostVeil = 0.14, kFrostFerns = 0.6, kFrostScale = 60.0, kFrostFrom = 0.3;
#endif
#if PGR4_SNOW_STYLE == 2
static const float kSnowGrainScale = 620.0, kSnowSoft = 0.14, kSnowMore = 1.3, kSnowVeil = 0.1, kSnowPatches = 0.0;
#elif PGR4_SNOW_STYLE == 3
static const float kSnowGrainScale = 280.0, kSnowSoft = 0.1, kSnowMore = 0.5, kSnowVeil = 0.0, kSnowPatches = 1.0;
#elif PGR4_SNOW_STYLE == 4
static const float kSnowGrainScale = 280.0, kSnowSoft = 0.07, kSnowMore = 0.4, kSnowVeil = 0.3, kSnowPatches = 0.0;
#else
static const float kSnowGrainScale = 280.0, kSnowSoft = 0.07, kSnowMore = 0.7, kSnowVeil = 0.0, kSnowPatches = 0.0;
#endif
static const float kSnowCover = 0.3;      // how much of what faces up the layer covers, 0 to 1
static const float kSnowFlakeCells = 48.0;  // cells for a flake, a coordinate's unit: about 4 cm
// No sine: over a car's coordinates, it left lines and bands (Hoskins' hash).
float XeSnowHash(float2 p) {
  float3 q = frac(float3(p.x, p.y, p.x) * 0.1031);
  q += dot(q, q.yzx + 33.33);
  return frac((q.x + q.y) * q.z);
}
float XeSnowNoise(float2 p) {
  const float2 c = floor(p), f = frac(p);
  const float2 u = f * f * (3.0 - 2.0 * f);
  return lerp(lerp(XeSnowHash(c), XeSnowHash(c + float2(1.0, 0.0)), u.x),
              lerp(XeSnowHash(c + float2(0.0, 1.0)), XeSnowHash(c + float2(1.0, 1.0)), u.x), u.y);
}
float XeSnowLayers(float2 p) {
  return 0.5 * XeSnowNoise(p) + 0.27 * XeSnowNoise(p * 2.13 + 17.0) + 0.15 * XeSnowNoise(p * 4.37 + 41.0) +
         0.08 * XeSnowNoise(p * 9.1 + 73.0);
}
// The powder's grain: turned at each scale and bent a little, so no grid shows.
float XeSnowGrain(float2 p) {
  p += 0.9 * float2(XeSnowNoise(p * 0.31 + 5.0), XeSnowNoise(p * 0.31 + 23.0));
  const float2 a = float2(0.8 * p.x - 0.6 * p.y, 0.6 * p.x + 0.8 * p.y);
  const float2 b = float2(0.28 * p.x + 0.96 * p.y, -0.96 * p.x + 0.28 * p.y) * 2.1 + 11.0;
  const float2 c = float2(-0.71 * p.x + 0.71 * p.y, -0.71 * p.x - 0.71 * p.y) * 4.3 + 29.0;
  return 0.5 * XeSnowNoise(a) + 0.3 * XeSnowNoise(b) + 0.2 * XeSnowNoise(c);
}
// In the wiper's fan (car_reflect_hooks.hlsli): no lying snow, flakes more often, and none older than
// the blade's last passage.
static float gXeSnowLying = 1.0;
static float gXeFlakeEvery = 0.0;     // seconds between a cell's flakes; 0: the usual
static float gXeFlakeSince = 1e6;     // seconds since the blade passed here
static float gXeFlakeGain = 0.22;     // how much of the grain a flake brings out
static float gXeFlakeSmaller = 1.0;   // above 1, smaller flakes
// One grid of flakes, each at its own place and time. Not drawn: how much more of the snow's own grain
// shows there (XeSnowAmount), so a flake is a fleck like the others. The grid is turned (angle) and its
// rows slid, each flake placed anywhere in its cell, so no line follows the car's coordinates.
float XeSnowFlakes(float2 uv, float cells, float offset) {
  const float angle = 0.65 + offset * 0.031;
  const float ca = cos(angle), sa = sin(angle);
  const float2 grid = float2(ca * uv.x - sa * uv.y, sa * uv.x + ca * uv.y) * cells * gXeFlakeSmaller + offset;
  const float pixel = max(length(ddx(grid)), length(ddy(grid)));
  const float slide = XeSnowHash(float2(floor(grid.y), offset + 3.0));
  const float2 shifted = float2(grid.x + slide, grid.y);
  const float2 place = floor(shifted) + offset;
  const float seed = XeSnowHash(place);
  // In seconds, whatever the car's speed: in each cell's cycle (16 to 24 s), a flake comes at a moment
  // of its own, each time another, and stays 1 to 3 s, each time another.
  const float period = gXeFlakeEvery > 0.0 ? gXeFlakeEvery * (1.0 + 0.5 * XeSnowHash(place + 11.0))
                                           : 16.0 + 8.0 * XeSnowHash(place + 11.0);
  const float cycle_time = pgr4_time.x / period + seed * 53.0;
  const float2 key = place + floor(cycle_time) * float2(61.0, 97.0);
  // Half the flakes, but in heavy snow (Tazz, 06/11/2026).
  if (!XeHeavySnow && XeSnowHash(key + 8.0) > 0.5) return 0.0;
  const float stays = 1.0 + 2.0 * XeSnowHash(key + 2.0);
  const float t = (frac(cycle_time) * period - XeSnowHash(key + 3.0) * (period - stays)) / stays;
  if (t < 0.0 || t >= 1.0 || t * stays > gXeFlakeSince) return 0.0;
  // None lands while the game's own flakes stop (a bridge, a tunnel): those it had keep melting.
  if (t * stays >= pgr4_shelter.y && t * stays < pgr4_shelter.x) return 0.0;
  const float h = XeSnowHash(key + 4.0);
  const float radius_base = 0.14 + 0.2 * h * h;
  const float2 centre = radius_base + (1.0 - 2.0 * radius_base) * float2(XeSnowHash(key + 6.0), XeSnowHash(key + 7.0));
  const float2 from = frac(shifted) - centre;
  const float turn = atan2(from.y, from.x);
  const float uneven = 1.0 + 0.15 * sin(turn * 2.0 + seed * 40.0) + 0.1 * sin(turn * 3.0 + seed * 91.0);
  const float leaving = smoothstep(0.0, 1.0, saturate((1.0 - t) / 0.3));
  const float radius = radius_base * uneven;
  const float edge = max(pixel, radius * 0.6);
  const float body = 1.0 - smoothstep(radius - edge, radius + edge, length(from));
  return body * saturate(t / 0.05) * leaving;
}
// The frost, 0 to 1: patches with soft edges, a thin veil in them and crystals branching over it. Mostly
// on what faces up. Never a grid, and from afar its crystals' mean, so it does not shimmer.
float XeFrostRidge(float2 p) {
  const float r = 1.0 - abs(2.0 * XeSnowNoise(p) - 1.0);
  return r * r * r;
}
float XeFrostAmount(float3 n) {
#if PGR4_FROST == 0
  return 0.0;
#endif
  const float2 uv = gXeSnowUv;
  const float holds = max(smoothstep(0.45, 0.9, n.y), 0.3 * smoothstep(-0.25, 0.1, n.y));
  const float patches = smoothstep(kFrostFrom, kFrostFrom + 0.2, XeSnowLayers(uv * kFrostPatch + 7.0));
  float2 q = uv * kFrostScale;
  const float footprint = max(length(ddx(q)), length(ddy(q)));
  q += 1.3 * float2(XeSnowNoise(q * 0.37 + 3.0), XeSnowNoise(q * 0.37 + 19.0));
#if PGR4_FROST == 5
  // A grain of ice with no shape: no lines to follow, from noise at three scales.
  const float crystals = 0.5 * XeSnowNoise(q) + 0.3 * XeSnowNoise(q * 2.3 + 9.0) + 0.2 * XeSnowNoise(q * 5.1 + 21.0);
  const float ferns = lerp(smoothstep(0.42, 0.62, crystals), 0.5, smoothstep(0.4, 1.5, footprint));
#else
  const float crystals = 0.55 * XeFrostRidge(q) + 0.3 * XeFrostRidge(q * 2.3 + 9.0) + 0.15 * XeFrostRidge(q * 5.1 + 21.0);
  const float ferns = lerp(smoothstep(0.3, 0.75, crystals), 0.3, smoothstep(0.4, 1.5, footprint));
#endif
  return saturate(holds * patches * gXeSnowKeep * (kFrostVeil + kFrostFerns * ferns));
}
// The frost's patches alone, 0 to 1: the reflection stops there, and only there.
float XeFrostPatch(float3 n) {
#if PGR4_FROST == 0
  return 0.0;
#endif
  const float holds = max(smoothstep(0.45, 0.9, n.y), 0.3 * smoothstep(-0.25, 0.1, n.y));
  return holds * gXeSnowKeep * smoothstep(kFrostFrom, kFrostFrom + 0.2, XeSnowLayers(gXeSnowUv * kFrostPatch + 7.0));
}
// The sheet of ice around the snow, 0 to 1, on the paint (XeFinishImproved).
float XeIceAmount(float3 n) {
#if PGR4_FROST == 5
  const float holds = max(smoothstep(0.45, 0.9, n.y), 0.25 * smoothstep(-0.25, 0.1, n.y));
  return holds * gXeSnowKeep * smoothstep(0.3, 0.45, XeSnowLayers(gXeSnowUv * kFrostPatch + 7.0));
#else
  return 0.0;
#endif
}
// How much snow here, 0 to 1 (n: the surface's normal, in the world).
float XeSnowAmount(float3 n) {
  const float up = saturate(n.y);
  const float faces_up = smoothstep(0.55, 0.95, up);
  // A fine powder, thicker here and thinner there: a grain of a centimetre or so, its density varying
  // slowly. From afar its grain grows, never under a few pixels, so it stays snow and not a pale veil.
  const float2 grain_at = gXeSnowUv * kSnowGrainScale;
  const float grain_pixel = max(max(length(ddx(grain_at)), length(ddy(grain_at))), 1e-5);
  const float grain_level = clamp(log2(grain_pixel / 0.5), 0.0, 5.0);
  const float grain_step = floor(grain_level);
  // Where the coordinates are stretched, less of it, not a dimmer one: a fleck is white or is not there.
  // The sides hold a little of what the wind throws at them; what faces down, nothing.
  const float holds = max(faces_up, 0.3 * smoothstep(-0.25, 0.1, n.y));
  const float density = holds * kSnowCover * (0.45 + 1.1 * XeSnowLayers(gXeSnowUv * 5.0)) * gXeSnowKeep * kSnowMore * PGR4_SNOW_LAYER * gXeSnowLying;
  const float grain = lerp(XeSnowGrain(grain_at / pow(2.0, grain_step)), XeSnowGrain(grain_at / pow(2.0, grain_step + 1.0)),
                           grain_level - grain_step);
  // The grain's values gather around 0.5: the density moved to their range.
  const float flakes = max(XeSnowFlakes(gXeSnowUv, kSnowFlakeCells, 0.0), XeSnowFlakes(gXeSnowUv, kSnowFlakeCells * 0.7, 37.0)) *
                       max(max(holds, 0.05), gXeFlakeEvery > 0.0 ? 1.0 : 0.0) * smoothstep(0.4, 0.7, gXeSnowKeep);
  const float cut = 0.76 - 0.5 * saturate(density) + 0.06 * (1.0 - faces_up) - gXeFlakeGain * flakes;  // the sides: much sparser
  const float powder = smoothstep(cut - kSnowSoft, cut + kSnowSoft, grain);
  const float soft_layers = XeSnowLayers(gXeSnowUv * 16.0 + 3.0);
  const float patches = kSnowPatches * holds * gXeSnowKeep * smoothstep(0.44, 0.6, soft_layers + 0.25 * (grain - 0.5));
  const float veil = kSnowVeil * holds * gXeSnowKeep * (0.6 + 0.8 * soft_layers);
  const float layer = max(max(powder * smoothstep(0.0, 0.04, max(density, flakes)), patches * 0.9), veil);
  return layer;
}
// Matte, so no reflection passes over it: lit by the air's colour (the game's fog, c10), the same
// whatever the surroundings mirrored in the surface, brighter on what faces up.
float3 XeSnowColor(float3 n) {
#if defined(c10) && defined(c12)
  return max(c10.rgb * c12.y * (0.95 + 0.1 * saturate(n.y)), 0.03) * float3(0.97, 0.99, 1.0);
#else
  return float3(0.6, 0.62, 0.65);
#endif
}
// The frost: white, the air's tint gone, a little brighter than the air, following its light.
float3 XeFrostColor(float3 n) {
  const float3 air = XeSnowColor(n);
  const float top = max(max(air.r, air.g), air.b);
  return float3(top, top, top) * 1.5;
}
// On the paint: it hides it.
float4 XeSnowFinish(float4 color, XeInput i, float3 n) {
  color.rgb = lerp(color.rgb, XeFrostColor(n), XeFrostAmount(n));
  color.rgb = lerp(color.rgb, XeSnowColor(n), XeSnowAmount(n));
  return color;
}
// On the glass, drawn blended by its alpha: the snow over it, as opaque as it is.
float4 XeSnowFinishGlass(float4 color, float3 n, float outside) {
  const float flake = XeSnowAmount(n), frost = XeFrostAmount(n);
  const float a = max(flake, frost) * outside;
  const float out_a = a + color.a * (1.0 - a);
  const float3 lying = lerp(XeFrostColor(n), XeSnowColor(n), flake);
  color.rgb = (lying * a + color.rgb * color.a * (1.0 - a)) / max(out_a, 1e-4);
  color.a = out_a;
  return color;
}
#endif
