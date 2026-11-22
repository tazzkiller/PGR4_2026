// Light flares (pixel shader AB2BD72DF7FE8E7A, psLightFlare, local change, pgr4 project): the
// game's flare sprite, its texture times the vertex color (the flare's color and its fade by the
// occlusion query) times c12.y, added to the scene. The full size flares (c12.y 6) blow out into a
// white blob when a lamp faces the camera; they are dimmed by kFlare. The 320x180 halo pass
// (c12.y 1, blurred into the glow on the road) times kFlareHalo, 1 the game's. Both live
// (pgr4_native_tune_flare, pgr4_native_tune_flare_halo: the overlay's Volumetrique tab), with the
// volumetric fog on; without it, V1.45's (0.75 and the game's halo, ShaderTuningDefines).
// From the game's shader: r0.xy the texture coordinates, r1 the vertex color.

#ifndef PGR4_TUNE_kFlare
#define PGR4_TUNE_kFlare 0.75
#endif
#ifndef PGR4_TUNE_kFlareHalo
#define PGR4_TUNE_kFlareHalo 1.0
#endif
// The cars' flares (vsLightFlare and the headlights': draw.cpp sets pgr4_dirt.z to 1): their own
// (pgr4_native_tune_flare_car, pgr4_native_tune_flare_car_halo, with the volumetric fog).
#ifndef PGR4_TUNE_kFlareCar
#define PGR4_TUNE_kFlareCar 0.75
#endif
#ifndef PGR4_TUNE_kFlareCarHalo
#define PGR4_TUNE_kFlareCarHalo 1.0
#endif
static const float kFlare = PGR4_TUNE_kFlare;  // the full size flares' brightness, 1 = the game's
static const float kFlareHalo = PGR4_TUNE_kFlareHalo;  // the halo pass's, 1 = the game's

float4 Shade(XeInput i) {
  float3 c = TEX2D(0, i.r0.xy).rgb * i.r1.rgb * c12.y;
  const bool car = pgr4_dirt.z > 0.5;
  c *= c12.y > 1.0 ? (car ? PGR4_TUNE_kFlareCar : kFlare) : (car ? PGR4_TUNE_kFlareCarHalo : kFlareHalo);
  return float4(c, 1.0);
}
