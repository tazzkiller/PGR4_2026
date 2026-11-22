// Local change (21/11/2026): the volumetric fog's volume (pgr4-native volumetrics.cpp) for the game's
// transparent draws made after it, which fog themselves (the spectators' flags, 287EC49DD0AD9E71,
// with a fog of their own: c32 its color, c33-c34 its thickness, c35.x the scale after it). Their
// transmittance T is multiplied by ours against the game's along the same way, as the volume's
// pass does for what is behind them, so that they sit in the same banks:
//   T' = T x min(To / Tg, 1 + 2 T)   (To ours, Tg the game's, both from the eye to them; where the
//                                     game's fog hides them, ours can only thicken it)
// pgr4_fog_volume (draw.cpp, Volumetrics::Share::values; zero while the volume is not made this
// frame or the switch is off: T kept):
//   [0] the volume's depth (m), its slices' growth, its columns (x, y)
//   [1] the view's half extent at 1 m (x, y), the scene's size (x, y)
//   [2] the game's fog: c10.w, the height fog's c11.x, c11.y, c11.z
//   [3] bool 158, its exponent's scale x ln 2, the eye's height, 1 once made
// The volume: bindless 3D texture 8, a column per 16 x 16 pixels, 64 slices; slice k holds the
// light and the transmittance from the eye to edge k + 1, edge j at 0.5 m x e^(growth j).
#ifndef PGR4_FOG_VOLUME_HOOKS
#define PGR4_FOG_VOLUME_HOOKS

#define PGR4_FOG_VOLUME_INDEX 8
// The pixel's SV_Position for PgrFogT (zero without one: T kept).
#ifdef XE_HAS_POS
#define PGR4_PIXEL_POS(i) ((i).pos)
#else
#define PGR4_PIXEL_POS(i) float4(0.0, 0.0, 0.0, 0.0)
#endif
static const float kPgrVolumeNear = 0.5;
static const int kPgrSlices = 64;

// The game's fog between the eye and a point (volumetrics.cpp GameFogDepth), in natural units.
float PgrGameTau(float distance, float point_height) {
  float4 g = pgr4_fog_volume[2];
  float4 h = pgr4_fog_volume[3];
  float tau = g.x * distance;
  if (h.x != 0.0) {
    float p = point_height - g.y, e = h.z - g.y;
    float lo = min(p, e), hi = max(p, e);
    float f = saturate(-lo * g.w);
    float below = p < 0.0 ? distance : (lo < 0.0 ? distance * (-lo) / max(hi - lo, 1e-4) : 0.0);
    tau += g.z * below * f;
  }
  return tau * h.y;
}

// The volume's transmittance in column c (clamped), slice k (-1: the eye's, 1).
float PgrVolumeAt(int2 c, int k) {
  if (k < 0) return 1.0;
  int2 columns = int2(pgr4_fog_volume[0].zw);
  c = clamp(c, int2(0, 0), columns - 1);
  return xe_textures_3d[PGR4_FOG_VOLUME_INDEX].Load(int4(c, min(k, kPgrSlices - 1), 0)).a;
}

// Its transmittance from the eye to view depth z at pixel p: across the columns (their centres at
// 16 c + 8) and along the slices (edge j's value is slice j - 1's), by hand from Load.
float PgrVolumeT(float2 p, float z) {
  float growth = pgr4_fog_volume[0].y;
  // The slice coordinate, -1 at the eye: before the first edge from the eye's 1 to edge 1's,
  // linearly in depth; then edge j = log(z / near) / growth at slice j - 1.
  float e1 = kPgrVolumeNear * exp(growth);
  float s = z < e1 ? z / e1 - 1.0
                   : min(log(z / kPgrVolumeNear) / growth, float(kPgrSlices)) - 1.0;
  int k0 = int(floor(s));
  float fk = s - float(k0);
  float2 cc = p / 16.0 - 0.5;
  int2 c0 = int2(floor(cc));
  float2 fc = cc - float2(c0);
  float t = 0.0;
  [unroll] for (int n = 0; n < 4; ++n) {
    int2 o = int2(n & 1, n >> 1);
    float w = (o.x ? fc.x : 1.0 - fc.x) * (o.y ? fc.y : 1.0 - fc.y);
    t += w * lerp(PgrVolumeAt(c0 + o, k0), PgrVolumeAt(c0 + o, k0 + 1), fk);
  }
  return t;
}

// The draw's transmittance t (its own fog's) at world point `world`, the eye `eye` (its c5), the
// pixel `pos` (SV_Position): ours in its place.
float PgrFogT(float t, float3 world, float3 eye, float4 pos) {
#ifdef XE_HAS_POS
  if (pgr4_fog_volume[3].w == 0.0) return t;
  float4 v0 = pgr4_fog_volume[0];
  float4 v1 = pgr4_fog_volume[1];
  float2 uv = pos.xy / v1.zw;
  float3 ray = float3(float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0) * v1.xy, 1.0);
  float rl = length(ray);
  float d = length(world - eye);
  float z = d / rl;  // its view depth
  float zr = min(z, v0.x);
  float to = PgrVolumeT(pos.xy, zr);
  float eye_y = pgr4_fog_volume[3].z;
  float height = eye_y + (world.y - eye.y) * zr / max(z, 1e-4);
  float tg = exp(-PgrGameTau(zr * rl, height));
  float ratio = min(to / max(tg, 1e-6), 1.0 + 2.0 * t);
  float r = saturate(t * ratio);
  return all(isfinite(float2(r, ratio))) ? r : t;
#else
  return t;
#endif
}

#endif
