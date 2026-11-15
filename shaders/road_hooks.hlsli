// The road's relief (local change, pgr4 project): the hook of the road shaders
// (psSceneRoadSurfaceGotham4 and its variants, generated from their ucode by
// tools/ucode2hlsl.py and patched by tools/road_patch.py). The game lights the road with the
// interpolated normal of its polygons only (flat); XeRoadBump tilts that normal by the normal map
// of the gravel texture (tf0): PBRify's 1x-PBRify_NormalV3 of the HD texture, the second layer of
// its HD version (<hash>_n.dds, textures.cpp LoadHd). Precomputed with its mip levels, it does not
// shimmer as the earlier relief from the texture's own slope did.
// The tangent frame comes from the screen derivatives of the position and of the uv (Schüler's
// cotangent frame): the road has no tangents. Reloaded when written: tuned in game.

#ifndef PGR4_TUNE_kRoadNormal
#define PGR4_TUNE_kRoadNormal 0.6
#endif
static const float kRoadNormal = PGR4_TUNE_kRoadNormal;  // the relief (0: flat, 1: the map's)

// n: the game's normal (r0), uv: the gravel's coordinates, pos: the eye-to-pixel vector (r5).
float3 XeRoadBump(float3 n, float2 uv, float3 pos) {
  uint w, h, layers;
  XE_TEXTURE_TF0.GetDimensions(w, h, layers);
  if (layers < 2) return n;  // no normal map (an original texture, or no <hash>_n.dds)
  const float len = length(n);
  if (len < 1e-4) return n;
  const float3 N = n / len;
  // The map: DirectX convention (green towards -v), 8 bits per channel.
  float3 m = XE_TEXTURE_TF0.Sample(XE_SAMPLER_TF0, float3(uv, 1.0)).xyz * 2.0 - 1.0;
  m.y = -m.y;
  m.xy *= kRoadNormal;
  // The cotangent frame of the pixel.
  const float3 dp1 = ddx(pos), dp2 = ddy(pos);
  const float2 duv1 = ddx(uv), duv2 = ddy(uv);
  const float3 dp2perp = cross(dp2, N), dp1perp = cross(N, dp1);
  const float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
  const float3 B = dp2perp * duv1.y + dp1perp * duv2.y;
  const float scale = max(dot(T, T), dot(B, B));
  if (scale < 1e-20) return n;
  const float inv = rsqrt(scale);
  const float3 tilted = normalize(T * inv * m.x + B * inv * m.y + N * max(m.z, 0.05));
  return tilted * len;
}
