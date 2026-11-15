// Light flares (pixel shader AB2BD72DF7FE8E7A, psLightFlare, local change, pgr4 project): the
// game's flare sprite, its texture times the vertex color (the flare's color and its fade by the
// occlusion query) times c12.y, added to the scene. The full size flares (c12.y 6) blow out into a
// white blob when a lamp faces the camera; they are dimmed by kFlare. The 320x180 halo pass
// (c12.y 1, blurred into the glow on the road) is left as it is.
// From the game's shader: r0.xy the texture coordinates, r1 the vertex color.

static const float kFlare = 0.75;  // the full size flares' brightness, 1 = the game's

float4 Shade(XeInput i) {
  float3 c = TEX2D(0, i.r0.xy).rgb * i.r1.rgb * c12.y;
  if (c12.y > 1.0) c *= kFlare;
  return float4(c, 1.0);
}
