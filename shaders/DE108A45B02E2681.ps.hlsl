// Cars' exterior mirrors (pixel shader DE108A45B02E2681, local change, pgr4 project): the game's
// reflection of the car's surroundings cube map, supersampled. A mirror covers a few pixels and
// its reflection sweeps across the cube quickly: one sample per pixel of the cube at the draw
// resolution scale (a single level, TextureCache::CopyScaledCube) aliases into stairs.
// From the game's shader: r1.xyz the normal (r1.w the distance), r0.xyz the view, tf3 the cube,
// c10 the fog color (w its density), c12.y the exposure.

static const float kFootprint = 3.0;  // the samples' spread, in pixels

float4 Shade(XeInput i) {
  float3 n = normalize(i.r1.xyz);
  float3 v = normalize(i.r0.xyz);
  // The game's: n reflected about v, looked up in the opposite direction.
  float3 d = -(n - 2.0 * dot(n, v) * v);
  // 4x4 samples over the pixel's footprint of the direction.
  float3 dx = ddx(d) * kFootprint, dy = ddy(d) * kFootprint;
  float3 sum = 0;
  [unroll] for (int y = 0; y < 4; ++y)
  [unroll] for (int x = 0; x < 4; ++x) {
    float2 o = (float2(x, y) + 0.5) / 4.0 - 0.5;
    sum += TEXCUBE_LEVEL(3, d + o.x * dx + o.y * dy, 0.0).rgb;
  }
  float3 env = sum / 16.0;
  // The game's distance fog, then its exposure.
  float fog = exp(-i.r1.w * c10.w);
  return float4(lerp(c10.rgb, env, fog) * c12.y, 1.0);
}
