// A ray over the scene's copy (local change, pgr4 project), for the reflections of the water
// (water_hooks.hlsli) and of the wet roads (road_reflect_hooks.hlsli). The copy (scene_copy.h: its
// color and depth at half size, pgr4_scene) is projected onto by pgr4_clip (world to clip), the
// translator's NDC scale and offset and the viewport (pgr4_view). From `origin` along `dir`, steps
// growing to about 1 km; a hit where the ray passes behind the copy's depth between two steps
// (not far behind it: else an object in front hides what it reflects), refined by bisection. The
// copy's color is the game's output (times its exposure c12.y), divided back. Reloaded when
// written: tuned in game.
static const float kReflectStart = 0.4;    // metres before the first step
static const float kReflectGrowth = 1.3;   // each step this much longer (to about 1.3 km)
static const int kReflectSteps = 32;       // (44 steps of 1.2: the same reach, 0.3 ms more)
static const float kReflectThickness = 1.5;  // metres behind a surface still a hit, plus 4 % of its distance
static const float kReflectEdge = 0.06;    // the screen's border the reflection fades out over
static const float kReflectRise = 0.3;     // metres above its surface the ray must be for a hit
static const int kReflectDebug = 0;  // 1: red where a ray hits, blue elsewhere (fade 1)
// With `blur`: how much of the level above (sharper) is mixed in (set by the caller before
// tracing: the wet roads).
static float xe_ray_detail = 0.0;
// The height above its surface a hit needs grows by this per metre along the ray (set by the
// caller: the wet roads, whose copy's depth is coarser far away).
static float xe_ray_rise_growth = 0.0;
// Where the ray passes far behind what the screen shows (what it would reflect is hidden there:
// the foot of a sign behind a barrier), that in front taken instead, weighed by this (0: nothing;
// set by the caller: the wet roads).
static float xe_ray_occluded = 0.0;
// The steps' growth and count instead of kReflectGrowth and kReflectSteps when above 0 (set by the
// caller: the wet roads, shorter steps not jumping over a sign's thin plate).
static float xe_ray_growth = 0.0;
static int xe_ray_steps = 0;

// The copy's texel (x, y), the host's inverted depth and the clip's w of the world point p.
float4 XeProject(float3 p) {
  const float4 q = float4(p, 1.0);
  const float4 c = float4(dot(pgr4_clip[0], q), dot(pgr4_clip[1], q), dot(pgr4_clip[2], q),
                          dot(pgr4_clip[3], q));
  const float3 ndc = c.xyz / c.w * pgr4_view[1].xyz + pgr4_view[2].xyz;
  const float2 texel = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * pgr4_view[0].zw +
                       pgr4_view[0].xy;
  return float4(texel, ndc.z, c.w);
}

// The color copy at `texel` (its first level's texels) in its level `level` (each half the one
// above: scene_copy.h), bilinear.
float3 XeLevelColor(Texture2DArray<float4> colors, float2 texel, int level) {
  uint w, h, layers, levels;
  colors.GetDimensions(level, w, h, layers, levels);
  const float2 c = (texel + 0.5) / exp2(float(level)) - 0.5;
  const float2 b = floor(c), f = c - b;
  const int2 last = int2(w, h) - 1;
  const float3 s00 = colors.Load(int4(clamp(int2(b), 0, last), 0, level)).rgb;
  const float3 s10 = colors.Load(int4(clamp(int2(b) + int2(1, 0), 0, last), 0, level)).rgb;
  const float3 s01 = colors.Load(int4(clamp(int2(b) + int2(0, 1), 0, last), 0, level)).rgb;
  const float3 s11 = colors.Load(int4(clamp(int2(b) + int2(1, 1), 0, last), 0, level)).rgb;
  return lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
}

// The sky the direction `dir` meets, from the copy: the point at infinity along it projected, where
// nothing was drawn with a depth there (the sky's shaders write none: draw.cpp, IsSkyShader), read
// at the copy's level `blur`; `fade`: 0 off the screen or where something stands there.
float3 XeSkyColor(float3 dir, float blur, out float fade) {
  fade = 0.0;
  if (pgr4_scene.z == 0u) return 0.0;
  Texture2DArray<float4> colors = xe_textures_2d[pgr4_scene.x];
  Texture2DArray<float4> depths = xe_textures_2d[pgr4_scene.y];
  const float2 size = float2(pgr4_scene.zw);
  const float4 p = XeProject(pgr4_camera.xyz + normalize(dir) * 1.0e5);
  if (p.w < 0.1 || any(p.xy < 0.0) || any(p.xy >= size)) return 0.0;
  if (depths.Load(int4(p.xy, 0, 0)).x > 1e-6) return 0.0;
  const int l0 = int(floor(blur));
  float3 color = lerp(XeLevelColor(colors, p.xy, l0), XeLevelColor(colors, p.xy, min(l0 + 1, 3)),
                      frac(blur));
#ifdef c12
  color /= max(c12.y, 1e-4);
#endif
  const float2 uv = p.xy / size;
  const float2 edge = min(uv, 1.0 - uv) / kReflectEdge;
  fade = saturate(min(edge.x, edge.y));
  return color;
}

// The scene's color the ray from `origin` along `dir` reaches; `fade`: 0 where it finds nothing,
// up to 1 (less near the screen's edges). `normal`: the surface it leaves, whose own depth near the
// ray (a wet road seen at a grazing angle, from the previous frame, at half size) is no hit: below
// kReflectRise over it the ray goes on. `streak`: the color is also averaged over that many texels
// up and down (a wet road's reflections, stretched across it). `far`: what lies farther than that
// (metres from the eye: the sky, the fog) is no hit, and what lies farther than half of it fades out.
// `rise`: metres above its surface the ray must be for a hit (kReflectRise when 0). `blur`: above 0,
// the color is read from the copy's smaller levels there (fractional: between two), blurred as a
// wet road's reflections are, and stable as the camera moves.
float3 XeTraceScene(float3 origin, float3 dir, float3 normal, int streak, float far, float rise,
                    float blur, out float fade) {
  fade = 0.0;
  if (pgr4_scene.z == 0u) return 0.0;
  Texture2DArray<float4> colors = xe_textures_2d[pgr4_scene.x];
  Texture2DArray<float4> depths = xe_textures_2d[pgr4_scene.y];
  const float2 size = float2(pgr4_scene.zw);
  // The host's inverted depth z = A + B / w: the clip's z row is a times its w row plus b, then
  // the NDC scale and offset.
  const float4 zr = pgr4_clip[2], wr = pgr4_clip[3];
  const int axis = abs(wr.x) > abs(wr.y) ? (abs(wr.x) > abs(wr.z) ? 0 : 2)
                                        : (abs(wr.y) > abs(wr.z) ? 1 : 2);
  const float a = zr[axis] / wr[axis], b = zr.w - a * wr.w;
  const float A = a * pgr4_view[1].z + pgr4_view[2].z, B = b * pgr4_view[1].z;
  const float3 d = normalize(dir);
  float t = kReflectStart, before = 0.0;
  bool hit = false;
  float depth_fade = 1.0;  // less where the ray passed far behind what it hit
  [loop] for (int k = 0; k < (xe_ray_steps > 0 ? xe_ray_steps : kReflectSteps); ++k) {
    const float4 p = XeProject(origin + d * t);
    if (p.w < 0.1 || any(p.xy < 0.0) || any(p.xy >= size)) break;
    const float scene = depths.Load(int4(p.xy, 0, 0)).x;
    const float min_rise = (rise > 0.0 ? rise : kReflectRise) + xe_ray_rise_growth * t;
    if (p.z < scene && t * dot(d, normal) > min_rise) {
      // Behind the scene's surface there: a hit if not far behind it (the last step's length).
      const float scene_w = B / max(scene - A, 1e-7);
      const float thickness = max(kReflectThickness + 0.04 * p.w, (t - before) * 1.5);
      hit = scene_w < far && (p.w - scene_w < thickness || xe_ray_occluded > 0.0);
      depth_fade = max(1.0 - smoothstep(0.5, 1.0, (p.w - scene_w) / thickness), xe_ray_occluded);
      break;
    }
    before = t;
    t *= xe_ray_growth > 0.0 ? xe_ray_growth : kReflectGrowth;
  }
  if (kReflectDebug) {
    fade = 1.0;
    return hit ? float3(20, 0, 0) : float3(0, 0, 20);
  }
  if (!hit) return 0.0;
  // The hit between the last step in front and this one.
  float lo = before, hi = t;
  [unroll] for (int n = 0; n < 6; ++n) {
    const float mid = 0.5 * (lo + hi);
    const float4 p = XeProject(origin + d * mid);
    const float scene = depths.Load(int4(clamp(p.xy, 0.0, size - 1.0), 0, 0)).x;
    if (p.z < scene) hi = mid; else lo = mid;
  }
  const float4 p = XeProject(origin + d * hi);
  const float2 texel = clamp(p.xy, 0.0, size - 1.0);
  // A wet surface is no perfect mirror: the 3x3 texels around the hit, and the streak.
  float3 color = 0.0;
  [unroll] for (int y = -1; y <= 1; ++y)
  [unroll] for (int x = -1; x <= 1; ++x)
    color += colors.Load(int4(clamp(texel + float2(x, y), 0.0, size - 1.0), 0, 0)).rgb;
  color /= 9.0;
  if (blur > 0.0 && pgr4_camera.w > 0.0) {
    // The wet roads: the running average of the copy's level 2 (scene_copy.h: the rain's drops,
    // new in each frame, averaged out), bilinear.
    Texture2DArray<float4> average = xe_textures_2d[uint(pgr4_camera.w)];
    uint w, h, layers;
    average.GetDimensions(w, h, layers);
    const float2 c = (texel + 0.5) / 4.0 - 0.5;
    const float2 b = floor(c), f = c - b;
    const int2 last = int2(w, h) - 1;
    const float3 s00 = average.Load(int4(clamp(int2(b), 0, last), 0, 0)).rgb;
    const float3 s10 = average.Load(int4(clamp(int2(b) + int2(1, 0), 0, last), 0, 0)).rgb;
    const float3 s01 = average.Load(int4(clamp(int2(b) + int2(0, 1), 0, last), 0, 0)).rgb;
    const float3 s11 = average.Load(int4(clamp(int2(b) + int2(1, 1), 0, last), 0, 0)).rgb;
    color = lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
  } else if (blur > 0.0) {
    const int l0 = int(floor(blur));
    color = lerp(XeLevelColor(colors, texel, l0), XeLevelColor(colors, texel, min(l0 + 1, 3)),
                 frac(blur));
    if (xe_ray_detail > 0.0) {
      // Some of the level above, sharper: thin bright lines (a sign's white border) not all
      // averaged away.
      const int l1 = max(l0 - 1, 0);
      color = lerp(color, lerp(XeLevelColor(colors, texel, l1), XeLevelColor(colors, texel, l0),
                               frac(blur)), xe_ray_detail);
    }
    if (streak > 0) {
      // Stretched up and down, its samples a level's texel apart (one streak, not copies),
      // weighed less further (a soft end rather than a block), only those of what the ray hit
      // (their distance near its).
      const float step = float(1 << l0);
      const float hit_w = B / max(depths.Load(int4(texel, 0, 0)).x - A, 1e-7);
      float3 sum = color;
      float count = 1.0;
      [loop] for (int k = -streak; k <= streak; ++k) {
        if (k == 0) continue;
        const float2 at = clamp(texel + float2(0, step * k), 0.0, size - 1.0);
        const float w = B / max(depths.Load(int4(at, 0, 0)).x - A, 1e-7);
        if (abs(w - hit_w) < 1.0 + 0.1 * hit_w) {
          const float g = float(k) / float(streak);
          const float weight = exp(-3.0 * g * g);
          sum += weight * lerp(XeLevelColor(colors, at, l0),
                               XeLevelColor(colors, at, min(l0 + 1, 3)), frac(blur));
          count += weight;
        }
      }
      color = sum / count;
    }
  } else if (streak > 0) {
    // Only the texels of what the ray hit (their distance near its): the surface under it, read
    // into the streak, drew the road again in its reflection.
    const float hit_w = B / max(depths.Load(int4(texel, 0, 0)).x - A, 1e-7);
    float3 sum = color;
    float count = 1.0;
    [loop] for (int k = -streak; k <= streak; ++k) {
      if (k == 0) continue;
      const int4 at = int4(clamp(texel + float2(0, 2 * k), 0.0, size - 1.0), 0, 0);
      const float w = B / max(depths.Load(at).x - A, 1e-7);
      if (abs(w - hit_w) < 1.0 + 0.1 * hit_w) {
        sum += colors.Load(at).rgb;
        count += 1.0;
      }
    }
    color = sum / count;
  }
#ifdef c12
  color /= max(c12.y, 1e-4);
#endif
  const float2 uv = texel / size;
  const float2 edge = min(uv, 1.0 - uv) / kReflectEdge;
  fade = saturate(min(edge.x, edge.y)) * depth_fade;
  if (far < 1e29) {
    const float hit_w = B / max(depths.Load(int4(texel, 0, 0)).x - A, 1e-7);
    fade *= saturate(2.0 - 2.0 * hit_w / far);
  }
  return color;
}
