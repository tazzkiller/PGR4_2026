// The highlights' compression (local change, pgr4 project): the hook of the game's final
// composition (pixel shader 6B06B8CDADD9CCBF: the scene times its exposure, c35.y c154.x over the
// mean luminance, plus the bloom, the grain and the vignette), written to a target that clips at
// 1. White surfaces in the sun (kerbs, painted lines, the tyre walls' stripes) went far above it:
// flat white, their edges stair-stepped and flickering. Above a knee, the colour's largest
// component is eased toward 1 instead (its hue kept: the three scaled alike), with the slope
// unchanged at the knee; below it nothing changes. Reloaded when written; kHighlights tuned from
// the overlay (PGR4_TUNE_kHighlights, 0: the game's).

#ifndef PGR4_TUNE_kHighlights
#define PGR4_TUNE_kHighlights 0.0
#endif
static const float kHighlights = PGR4_TUNE_kHighlights;  // 0 the game's clip, 1 a knee at 0.4

float4 XeHighlights(float4 color) {
  const float knee = 1.0 - 0.6 * saturate(kHighlights);
  const float m = max(color.r, max(color.g, color.b));
  if (kHighlights <= 0.0 || m <= knee) return color;
  const float span = 1.0 - knee;
  const float eased = knee + span * (1.0 - exp(-(m - knee) / span));
  return float4(color.rgb * (eased / m), color.a);
}
#define XeFinish(color) XeHighlights(color)

// The bloom (tf3, the 320x180 glow the game adds to the scene) times kBloom (1: the game's).
#ifndef PGR4_TUNE_kBloom
#define PGR4_TUNE_kBloom 1.0
#endif
static const float kBloom = PGR4_TUNE_kBloom;
#undef TEX2D
#define TEX2D(f, uv) (XE_TEXTURE_TF##f.Sample(XE_SAMPLER_TF##f, float3((uv), 0.0)) * ((f) == 3 ? kBloom : 1.0))
