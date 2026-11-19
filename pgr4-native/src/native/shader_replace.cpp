// pgr4-native PGR4 native renderer (local change, pgr4 project): the game's pixel shaders
// replaced by HLSL of ours. See shader_replace.h.
#include "shader_replace.h"

#include "d3d12_device.h"
#include "log.h"

#include <d3dcompiler.h>
#include <xxhash.h>

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

REXCVAR_DEFINE_BOOL(pgr4_native_shader_replace, true, "PGR4 native",
                    "Replace the game's pixel shaders that have an HLSL file in "
                    "pgr4_native_shader_replace_dir");
// Local change (18/11/2026, Tazz): the water's tuning for the race's weather from the race's load, not
// from its first screen (3-2-1-Go, with the intro's camera already drawn), and a weather's change
// compiled at once (the wait is for a slider being dragged): the water no longer changes in the intro.
REXCVAR_DEFINE_BOOL(pgr4_native_water_weather_at_load, true, "PGR4 native",
                    "The water's tuning for the race's weather from the race's load, compiled at once");
// Local change (18/11/2026, Tazz: "je change des valeurs et des fois il ne se passe rien"): a
// shader still compiling when a tuning changed was skipped, its code kept the values read when its
// compile started (Compile), until the next change.
// Local change (18/11/2026, Tazz): the water's tuning given to its draws each frame instead of
// compiled in (a slider at once, no compile that comes late or is lost), and its waves' clock run
// here: their speed or size changed without the waves jumping (water_hooks.hlsli PGR4_WATER_LIVE).
REXCVAR_DEFINE_BOOL(pgr4_native_water_live, true, "PGR4 native",
                    "The water's tuning given to its shaders each frame, its waves' clock run by the "
                    "renderer (a slider applied at once, the speed without a jump)");
REXCVAR_DEFINE_BOOL(pgr4_native_tune_requeue_pending, true, "PGR4 native",
                    "A shader compiling when a tuning changes compiled again once done (else it "
                    "keeps the values from before the change)");
REXCVAR_DEFINE_BOOL(pgr4_native_replace_alpha_test, true, "PGR4 native",
                    "Our replaced shaders do the game's alpha test (their cut-out parts not "
                    "written), as the translator does");
REXCVAR_DEFINE_BOOL(pgr4_native_decal_light_off, false, "PGR4 native",
                    "The painted lines' and start line's baked area light left out (it vibrated "
                    "on them in motion): the scenery shader's decals only");
REXCVAR_DECLARE(bool, pgr4_native_precompile);  // draw.cpp
REXCVAR_DEFINE_BOOL(pgr4_native_enhanced_reflections, true, "PGR4 native",
                    "Our shaders of the cars, the water and the wet roads, and the water's and "
                    "wet roads' scene reflections (the game menu's ENHANCED REFLECTIONS)");
// Local change (pgr4 project): the hooks' tunings, as #defines of the replacements' header
// (PGR4_TUNE_<name>); the shaders are compiled again when one changes.
REXCVAR_DEFINE_DOUBLE(pgr4_native_tune_road_relief, 0.6, "PGR4 native",
                      "The road's relief from its normal maps (road_hooks.hlsli kRoadNormal)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_tune_sun_glint, 0.0, "PGR4 native",
                      "The sun's highlight on the cars (car_reflect_hooks.hlsli kSunGlint)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_tune_sun_sharpness, 600.0, "PGR4 native",
                      "The size of that highlight, larger is smaller (kSunSharpness)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_tune_rim, 0.0, "PGR4 native",
                      "The paint's extra reflection at grazing angles (car_paint_hooks.hlsli "
                      "kRimReflection)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_tune_glass_reflect, 1.0, "PGR4 native",
                      "The cars' glass's reflection (car_reflect_hooks.hlsli kGlassReflect)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_tune_glass_sky, 1.0, "PGR4 native",
                      "The share of the glass's reflection left where it reflects the sky "
                      "(kGlassSky)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_tune_glass_sky_from, 0.0, "PGR4 native",
                      "From which height a reflected direction counts as the sky, -1 to 1 "
                      "(kGlassSkyFrom)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_tune_drops_paint_scale, 2.5, "PGR4 native",
                      "The rain's drops on the cars' paint made smaller by this, to the glass's size "
                      "(car_paint_hooks.hlsli kDropsPaintScale)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_tune_drops_glide_angle, 0.0, "PGR4 native",
                      "The direction the drops glide on the glass, degrees in the texture "
                      "(car_reflect_hooks.hlsli kDropsGlideAngle)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_tune_drops_stretch, 0.1, "PGR4 native",
                      "The drops drawn out along it at full speed (kDropsStretch)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_tune_drops_near, 0.1, "PGR4 native",
                      "Glass closer to the camera than this, metres, keeps its drops still: the "
                      "views from the car (kDropsNear)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_tune_drops_paint_strength, 1.8, "PGR4 native",
                      "The running drops' strength on the paint (kDropsPaintStrength)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_tune_frost_paint_scale, 4.0, "PGR4 native",
                      "The weather's grime on the cars' paint (frost when icy) made smaller by this "
                      "(car_paint_hooks.hlsli kFrostPaintScale)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_tune_scratch_paint_scale, 3.0, "PGR4 native",
                      "The damage's scratches on the cars' paint made smaller by this "
                      "(car_paint_hooks.hlsli kScratchPaintScale)");
// The wet roads' in heavy rain or a storm (the game gives both the same rain), then in light rain
// (*_light).
REXCVAR_DEFINE_DOUBLE(pgr4_native_tune_road_reflect, 0.5, "PGR4 native",
                      "The wet roads' reflection's strength (road_reflect_hooks.hlsli kRoadReflect)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_tune_road_blur, 3.0, "PGR4 native",
                      "The wet roads' reflection's blur, 0 sharp to 3 (kRoadBlur)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_tune_road_fresnel, 0.08, "PGR4 native",
                      "The wet roads' reflection seen from above, water's own 0.02 "
                      "(kRoadFresnelMin)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_tune_road_grain, 0.35, "PGR4 native",
                      "How even the wet roads' water film is over the asphalt's grain "
                      "(kRoadGrain)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_tune_road_puddles, 0.35, "PGR4 native",
                      "The wet roads' wetter and drier patches (kRoadPuddles)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_tune_road_sheen, 0.0, "PGR4 native",
                      "The wet roads' even sheen where the reflection finds nothing (kRoadSheen)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_tune_road_reflect_light, 0.65, "PGR4 native",
                      "The same in light rain (kRoadReflectLight)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_tune_road_blur_light, 2.5, "PGR4 native",
                      "The same in light rain (kRoadBlurLight)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_tune_road_fresnel_light, 0.02, "PGR4 native",
                      "The same in light rain (kRoadFresnelMinLight)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_tune_road_grain_light, 0.35, "PGR4 native",
                      "The same in light rain (kRoadGrainLight)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_tune_road_puddles_light, 0.35, "PGR4 native",
                      "The same in light rain (kRoadPuddlesLight)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_tune_road_sheen_light, 0.0, "PGR4 native",
                      "The same in light rain (kRoadSheenLight)");
// The water's (water_hooks.hlsli).
REXCVAR_DEFINE_DOUBLE(pgr4_native_tune_wave_strength, 0.35, "PGR4 native",
                      "The water's fine waves' strength, 0 the game's (water_hooks.hlsli kWaveStrength)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_tune_wave_steepness, 0.281, "PGR4 native",
                      "The water's waves' slope: larger is choppier (kWaveSteepness)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_tune_wave_noise, 0.75, "PGR4 native",
                      "The water's irregular patches against its regular waves (kNoiseShare)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_tune_wind_angle, 0.0, "PGR4 native",
                      "The wind's direction over the water, radians (kWindAngle)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_tune_wave_speed, 0.25, "PGR4 native",
                      "The water's waves' speed (kWaveSpeed)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_tune_highlights, 0.0, "PGR4 native",
                      "The final image's highlights eased below white, 0 the game's clip "
                      "(highlights_hooks.hlsli kHighlights)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_tune_bloom, 1.0, "PGR4 native",
                      "The final image's bloom times this, 1 the game's (highlights_hooks.hlsli kBloom)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_tune_wave_size, 0.1, "PGR4 native",
                      "The water's waves' and patches' size, 1 the 0.4 to 7 m waves (kWaveSize)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_tune_water_reflect, 1.0, "PGR4 native",
                      "The scenery's reflection on the water, 0 the game's sky alone (kWaterReflect)");
// Local change (17/11/2026): the sun's reflection on the fine waves (water_hooks.hlsli kSunWaves).
REXCVAR_DEFINE_DOUBLE(pgr4_native_tune_water_sun_waves, 1.0, "PGR4 native",
                      "The sun's reflection on the water's fine waves, 0 on the game's smooth water "
                      "(its soft spot; the rest keeps the waves), 1 on the waves (kSunWaves)")
    .range(0.0, 1.0);
// Local change (17/11/2026, Tazz: the water and its reflections hidden in heavy fog): the game's
// fog over the water times this (water_hooks.hlsli kWaterFog).
REXCVAR_DEFINE_DOUBLE(pgr4_native_tune_water_haze, 1.0, "PGR4 native",
                      "The game's fog over the water times this, 1 the game's (kWaterFog)")
    .range(0.0, 1.0);
// Local change (17/11/2026, Tazz): the water's tuning per weather, over the above (WaterTuningFor).
// In heavy fog the game still lights its water with the sun (c6, c7), a soft spot on its smooth
// water, which the fine waves broke into glitter all over it; fog comes with calm air.
REXCVAR_DEFINE_STRING(pgr4_native_tune_water_clear, "", "PGR4 native",
                      "The water in clear weather: \"key=value ...\" over its general tuning (keys "
                      "wave_strength, wave_steepness, wave_noise, wave_size, wave_speed, "
                      "wind_angle, water_reflect, sun_waves, haze); empty: the general one");
REXCVAR_DEFINE_STRING(pgr4_native_tune_water_overcast, "", "PGR4 native",
                      "The water when overcast (as --pgr4_native_tune_water_clear)");
// Fog: Light Fog, Heavy Fog and Foggy F1 alike (the game's weather kind 2, src/pgr4_autopilot.cpp).
REXCVAR_DEFINE_STRING(pgr4_native_tune_water_fog, "sun_waves=0 haze=0.4", "PGR4 native",
                      "The water in fog (as --pgr4_native_tune_water_clear; by default Tazz's "
                      "tuning of 17/11/2026: the sun on the game's smooth water, no glitter, and "
                      "0.4 of the game's fog over the water, which shows it and its reflections)");
// Rain: every rain of the game's weather kind 3 (Light Rain, Heavy Rain, Storm, their blends, Wet).
REXCVAR_DEFINE_STRING(pgr4_native_tune_water_rain,
                      "wave_strength=1.5 wave_steepness=0.193 wave_size=0.75 wave_speed=1.51 "
                      "wind_angle=-1.68 sun_waves=0 haze=0.4",
                      "PGR4 native",
                      "The water in rain (as --pgr4_native_tune_water_clear; by default Tazz's "
                      "tuning of 17/11/2026, in a storm at St Petersburg)");
// Local change (17/11/2026, Tazz): light rain apart (a weather name with "Light Rain"), the rain's
// tuning while empty.
REXCVAR_DEFINE_STRING(pgr4_native_tune_water_light_rain, "", "PGR4 native",
                      "The water in light rain (as --pgr4_native_tune_water_clear; empty: the "
                      "rain's, --pgr4_native_tune_water_rain)");
REXCVAR_DEFINE_STRING(pgr4_native_tune_water_snow, "", "PGR4 native",
                      "The water in snow (as --pgr4_native_tune_water_clear)");
REXCVAR_DEFINE_STRING(pgr4_native_tune_water_icy, "", "PGR4 native",
                      "The water on ice (as --pgr4_native_tune_water_clear)");
REXCVAR_DEFINE_STRING(pgr4_native_tune_water_heavy_snow, "", "PGR4 native",
                      "The water in heavy snow (as --pgr4_native_tune_water_clear)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_tune_harbour_clarity, 0.0, "PGR4 native",
                      "The garage harbour's water: the scene seen through it, 0 its own color "
                      "alone, 1 the game's (441604EE7597C1D1 kHarbourClarity)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_tune_harbour_distort, 0.3, "PGR4 native",
                      "The garage harbour's water: its reads' shift by the waves, 1 the game's "
                      "(kWaterDistort)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_tune_harbour_waves, 1.0, "PGR4 native",
                      "The garage harbour's water: its fine waves times this (kHarbourWaves)");
REXCVAR_DEFINE_STRING(pgr4_native_shader_replace_dir, "shaders", "PGR4 native",
                      "Folder of replacement pixel shaders (<ucode hash>.ps.hlsl), beside the "
                      "executable");

extern int g_pgr4_weather_kind;  // draw.cpp: the race's weather, -1 unknown, 0 clear .. 6
extern bool g_pgr4_race_screen;  // draw.cpp: a race's, the garage's, a replay's or photo screen
extern bool g_pgr4_light_rain;   // draw.cpp: the race's weather's name has "Light Rain"
extern bool g_pgr4_race_loaded;  // draw.cpp: the race's weather read, no screen loaded since

namespace pgr4::native {

namespace {
std::string WaterWeatherSetting(int weather) {
  switch (weather) {
    case 0: return REXCVAR_GET(pgr4_native_tune_water_clear);
    case 1: return REXCVAR_GET(pgr4_native_tune_water_overcast);
    case 2: return REXCVAR_GET(pgr4_native_tune_water_fog);
    case 3: return REXCVAR_GET(pgr4_native_tune_water_rain);
    case 4: return REXCVAR_GET(pgr4_native_tune_water_snow);
    case 5: return REXCVAR_GET(pgr4_native_tune_water_icy);
    case 6: return REXCVAR_GET(pgr4_native_tune_water_heavy_snow);
    case 7: return REXCVAR_GET(pgr4_native_tune_water_light_rain);
    default: return {};
  }
}
void WaterSetWeatherSetting(int weather, const std::string& v) {
  switch (weather) {
    case 0: REXCVAR_SET(pgr4_native_tune_water_clear, v); break;
    case 1: REXCVAR_SET(pgr4_native_tune_water_overcast, v); break;
    case 2: REXCVAR_SET(pgr4_native_tune_water_fog, v); break;
    case 3: REXCVAR_SET(pgr4_native_tune_water_rain, v); break;
    case 4: REXCVAR_SET(pgr4_native_tune_water_snow, v); break;
    case 5: REXCVAR_SET(pgr4_native_tune_water_icy, v); break;
    case 6: REXCVAR_SET(pgr4_native_tune_water_heavy_snow, v); break;
    case 7: REXCVAR_SET(pgr4_native_tune_water_light_rain, v); break;
    default: break;
  }
}
// The tuning's fields by their keys.
struct WaterKey {
  const char* name;
  float WaterTuning::*field;
};
constexpr WaterKey kWaterKeys[] = {
    {"wave_strength", &WaterTuning::wave_strength}, {"wave_steepness", &WaterTuning::wave_steepness},
    {"wave_noise", &WaterTuning::wave_noise},       {"wave_size", &WaterTuning::wave_size},
    {"wave_speed", &WaterTuning::wave_speed},       {"wind_angle", &WaterTuning::wind_angle},
    {"water_reflect", &WaterTuning::water_reflect}, {"sun_waves", &WaterTuning::sun_waves},
    {"haze", &WaterTuning::haze},
};
}  // namespace

int WaterWeather() {
  const bool race = g_pgr4_race_screen ||
                    (REXCVAR_GET(pgr4_native_water_weather_at_load) && g_pgr4_race_loaded);
  if (!race || g_pgr4_weather_kind < 0 || g_pgr4_weather_kind > 6) return -1;
  return g_pgr4_weather_kind == 3 && g_pgr4_light_rain ? 7 : g_pgr4_weather_kind;
}

WaterTuning WaterGeneralTuning() {
  return {float(REXCVAR_GET(pgr4_native_tune_wave_strength)),
          float(REXCVAR_GET(pgr4_native_tune_wave_steepness)),
          float(REXCVAR_GET(pgr4_native_tune_wave_noise)),
          float(REXCVAR_GET(pgr4_native_tune_wave_size)),
          float(REXCVAR_GET(pgr4_native_tune_wave_speed)),
          float(REXCVAR_GET(pgr4_native_tune_wind_angle)),
          float(REXCVAR_GET(pgr4_native_tune_water_reflect)),
          float(REXCVAR_GET(pgr4_native_tune_water_sun_waves)),
          float(REXCVAR_GET(pgr4_native_tune_water_haze))};
}

void WaterSetGeneral(const WaterTuning& t) {
  REXCVAR_SET(pgr4_native_tune_wave_strength, double(t.wave_strength));
  REXCVAR_SET(pgr4_native_tune_wave_steepness, double(t.wave_steepness));
  REXCVAR_SET(pgr4_native_tune_wave_noise, double(t.wave_noise));
  REXCVAR_SET(pgr4_native_tune_wave_size, double(t.wave_size));
  REXCVAR_SET(pgr4_native_tune_wave_speed, double(t.wave_speed));
  REXCVAR_SET(pgr4_native_tune_wind_angle, double(t.wind_angle));
  REXCVAR_SET(pgr4_native_tune_water_reflect, double(t.water_reflect));
  REXCVAR_SET(pgr4_native_tune_water_sun_waves, double(t.sun_waves));
  REXCVAR_SET(pgr4_native_tune_water_haze, double(t.haze));
}

WaterTuning WaterTuningFor(int weather) {
  WaterTuning t = WaterGeneralTuning();
  std::string s = WaterWeatherSetting(weather);
  if (weather == 7 && s.empty()) s = WaterWeatherSetting(3);  // light rain: the rain's
  // "key=value" pairs, separated by spaces, commas or semicolons; unknown keys are skipped.
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && (s[i] == ' ' || s[i] == ',' || s[i] == ';')) ++i;
    const size_t end = s.find_first_of(" ,;", i);
    const std::string token = s.substr(i, end == std::string::npos ? std::string::npos : end - i);
    i = end == std::string::npos ? s.size() : end;
    const size_t eq = token.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = token.substr(0, eq);
    for (const WaterKey& k : kWaterKeys)
      if (key == k.name) t.*k.field = std::strtof(token.c_str() + eq + 1, nullptr);
  }
  return t;
}

bool WaterHasOwn(int weather) { return !WaterWeatherSetting(weather).empty(); }

void WaterSetOwn(int weather, const WaterTuning* t) {
  if (!t) return WaterSetWeatherSetting(weather, {});
  // Only what differs from the general tuning.
  const WaterTuning g = WaterGeneralTuning();
  std::string s;
  for (const WaterKey& k : kWaterKeys) {
    if (std::abs(t->*k.field - g.*k.field) < 1e-4f) continue;
    char v[32];
    std::snprintf(v, sizeof(v), "%g", double(t->*k.field));
    s += (s.empty() ? "" : " ") + std::string(k.name) + "=" + v;
  }
  // All alike: still its own (the check box stays), its strength written.
  if (s.empty()) {
    char v[32];
    std::snprintf(v, sizeof(v), "%g", double(t->wave_strength));
    s = std::string("wave_strength=") + v;
  }
  WaterSetWeatherSetting(weather, s);
}

// Local change (pgr4 project): the overlay's "the game without our changes" (renderer.cpp).
bool OriginalLook();
// The enhanced reflections on (draw.cpp reads it too): the setting, unless the original look.
bool EnhancedReflections() {
  return REXCVAR_GET(pgr4_native_enhanced_reflections) && !OriginalLook();
}
namespace {

// The hooks' tunings as #defines (the pgr4_native_tune_* cvars).
std::string ShaderTuningDefines() {
  // The water's: the race's weather's (local change, 17/11/2026). Live (pgr4_native_water_live,
  // 18/11/2026): taken from the draw's constants (draw.cpp), none compiled in, so that no slider
  // nor weather recompiles the water.
  const bool live = REXCVAR_GET(pgr4_native_water_live);
  const WaterTuning water = live ? WaterTuning{} : WaterTuningFor(WaterWeather());
  char text[2560];
  std::snprintf(text, sizeof(text),
                "#define PGR4_TUNE_kRoadNormal %.6f\n#define PGR4_TUNE_kSunGlint %.6f\n"
                "#define PGR4_TUNE_kSunSharpness %.6f\n#define PGR4_TUNE_kRimReflection %.6f\n"
                "#define PGR4_TUNE_kGlassReflect %.6f\n#define PGR4_TUNE_kGlassSky %.6f\n"
                "#define PGR4_TUNE_kGlassSkyFrom %.6f\n#define PGR4_TUNE_kDropsPaintScale %.6f\n"
                "#define PGR4_TUNE_kDropsGlideAngle %.6f\n#define PGR4_TUNE_kDropsStretch %.6f\n"
                "#define PGR4_TUNE_kDropsNear %.6f\n#define PGR4_TUNE_kDropsPaintStrength %.6f\n"
                "#define PGR4_TUNE_kFrostPaintScale %.6f\n#define PGR4_TUNE_kScratchPaintScale %.6f\n"
                "#define PGR4_TUNE_kRoadReflect %.6f\n#define PGR4_TUNE_kRoadBlur %.6f\n"
                "#define PGR4_TUNE_kRoadFresnelMin %.6f\n#define PGR4_TUNE_kRoadGrain %.6f\n"
                "#define PGR4_TUNE_kRoadPuddles %.6f\n#define PGR4_TUNE_kRoadSheen %.6f\n"
                "#define PGR4_TUNE_kRoadReflectLight %.6f\n#define PGR4_TUNE_kRoadBlurLight %.6f\n"
                "#define PGR4_TUNE_kRoadFresnelMinLight %.6f\n"
                "#define PGR4_TUNE_kRoadGrainLight %.6f\n"
                "#define PGR4_TUNE_kRoadPuddlesLight %.6f\n#define PGR4_TUNE_kRoadSheenLight %.6f\n"
                "#define PGR4_TUNE_kWaveStrength %.6f\n#define PGR4_TUNE_kWaveSteepness %.6f\n"
                "#define PGR4_TUNE_kNoiseShare %.6f\n#define PGR4_TUNE_kWindAngle %.6f\n"
                "#define PGR4_TUNE_kWaveSpeed %.6f\n#define PGR4_TUNE_kWaterReflect %.6f\n"
                "#define PGR4_TUNE_kWaveSize %.6f\n#define PGR4_TUNE_kHighlights %.6f\n"
                "#define PGR4_TUNE_kBloom %.6f\n#define PGR4_TUNE_kHarbourClarity %.6f\n"
                "#define PGR4_TUNE_kWaterDistort %.6f\n#define PGR4_TUNE_kHarbourWaves %.6f\n"
                "#define PGR4_TUNE_kSunWaves %.6f\n#define PGR4_TUNE_kWaterFog %.6f\n",
                REXCVAR_GET(pgr4_native_tune_road_relief), REXCVAR_GET(pgr4_native_tune_sun_glint),
                REXCVAR_GET(pgr4_native_tune_sun_sharpness), REXCVAR_GET(pgr4_native_tune_rim),
                REXCVAR_GET(pgr4_native_tune_glass_reflect), REXCVAR_GET(pgr4_native_tune_glass_sky),
                REXCVAR_GET(pgr4_native_tune_glass_sky_from),
                REXCVAR_GET(pgr4_native_tune_drops_paint_scale),
                REXCVAR_GET(pgr4_native_tune_drops_glide_angle),
                REXCVAR_GET(pgr4_native_tune_drops_stretch),
                REXCVAR_GET(pgr4_native_tune_drops_near),
                REXCVAR_GET(pgr4_native_tune_drops_paint_strength),
                REXCVAR_GET(pgr4_native_tune_frost_paint_scale),
                REXCVAR_GET(pgr4_native_tune_scratch_paint_scale),
                REXCVAR_GET(pgr4_native_tune_road_reflect), REXCVAR_GET(pgr4_native_tune_road_blur),
                REXCVAR_GET(pgr4_native_tune_road_fresnel), REXCVAR_GET(pgr4_native_tune_road_grain),
                REXCVAR_GET(pgr4_native_tune_road_puddles), REXCVAR_GET(pgr4_native_tune_road_sheen),
                REXCVAR_GET(pgr4_native_tune_road_reflect_light),
                REXCVAR_GET(pgr4_native_tune_road_blur_light),
                REXCVAR_GET(pgr4_native_tune_road_fresnel_light),
                REXCVAR_GET(pgr4_native_tune_road_grain_light),
                REXCVAR_GET(pgr4_native_tune_road_puddles_light),
                REXCVAR_GET(pgr4_native_tune_road_sheen_light),
                double(water.wave_strength), double(water.wave_steepness),
                double(water.wave_noise), double(water.wind_angle),
                double(water.wave_speed), double(water.water_reflect),
                double(water.wave_size), REXCVAR_GET(pgr4_native_tune_highlights),
                REXCVAR_GET(pgr4_native_tune_bloom),
                REXCVAR_GET(pgr4_native_tune_harbour_clarity),
                REXCVAR_GET(pgr4_native_tune_harbour_distort),
                REXCVAR_GET(pgr4_native_tune_harbour_waves), double(water.sun_waves),
                double(water.haze));
  return live ? std::string(text) + "#define PGR4_WATER_LIVE 1\n" : std::string(text);
}

std::filesystem::path BesideExe(const std::string& dir) {
  std::filesystem::path p(dir);
  if (p.is_absolute()) return p;
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  return std::filesystem::path(exe).parent_path() / p;
}

// An element of a DXBC input or output signature.
struct SignatureElement {
  std::string name;
  uint32_t semantic_index, system_value, component_type, reg;
  uint8_t mask;
};

// The ISGN/OSGN (or ISG1/OSG1) chunk `fourcc` of a DXBC container.
std::vector<SignatureElement> ReadSignature(const std::vector<uint8_t>& dxbc, const char* fourcc,
                                            const char* fourcc1) {
  std::vector<SignatureElement> out;
  auto u32 = [&](size_t at) {
    uint32_t v = 0;
    if (at + 4 <= dxbc.size()) std::memcpy(&v, dxbc.data() + at, 4);
    return v;
  };
  if (dxbc.size() < 32 || std::memcmp(dxbc.data(), "DXBC", 4)) return out;
  const uint32_t chunks = u32(28);
  for (uint32_t c = 0; c < chunks; ++c) {
    const size_t chunk = u32(32 + 4 * c);
    if (chunk + 8 > dxbc.size()) break;
    const bool v1 = !std::memcmp(dxbc.data() + chunk, fourcc1, 4);
    if (!v1 && std::memcmp(dxbc.data() + chunk, fourcc, 4)) continue;
    const size_t data = chunk + 8;
    const uint32_t count = u32(data);
    const size_t stride = v1 ? 32 : 24;
    for (uint32_t i = 0; i < count; ++i) {
      const size_t e = data + 8 + i * stride + (v1 ? 4 : 0);
      SignatureElement s;
      const uint32_t name = u32(e);
      for (size_t k = data + name; k < dxbc.size() && dxbc[k]; ++k) s.name += char(dxbc[k]);
      s.semantic_index = u32(e + 4);
      s.system_value = u32(e + 8);
      s.component_type = u32(e + 12);
      s.reg = u32(e + 16);
      s.mask = e + 20 < dxbc.size() ? dxbc[e + 20] : 0;
      out.push_back(s);
    }
    break;
  }
  return out;
}

const char* ComponentType(uint32_t t, uint32_t mask_bits) {
  static const char* const f[] = {"float", "float", "float2", "float3", "float4"};
  static const char* const u[] = {"uint", "uint", "uint2", "uint3", "uint4"};
  static const char* const s[] = {"int", "int", "int2", "int3", "int4"};
  const uint32_t n = std::max(1u, mask_bits);
  return t == 1 ? u[n] : t == 2 ? s[n] : f[n];
}

// The prelude of a replacement for `translation`, then the main function (after the file).
bool MakePrelude(const DxbcShader::Translation& translation, std::string* prelude,
                 std::string* main) {
  const auto& shader = static_cast<const DxbcShader&>(translation.shader());
  const std::vector<uint8_t>& binary = translation.translated_binary();
  const std::vector<SignatureElement> in = ReadSignature(binary, "ISGN", "ISG1");
  const std::vector<SignatureElement> out = ReadSignature(binary, "OSGN", "OSG1");
  if (in.empty() || out.empty()) return false;
  const bool motion =
      DxbcShaderTranslator::Modification(translation.modification()).pixel.motion_vectors;

  std::ostringstream p;
  p << "// Generated for pixel shader " << std::hex << std::uppercase << shader.ucode_data_hash()
    << " modification " << translation.modification() << std::dec << "\n";
  p << ShaderTuningDefines();
  // DxbcShaderTranslator::SystemConstants up to color_exp_bias (byte 240).
  // ... and after them the scene's sun, the clock, and for the water its clip transform and the
  // scene's copy (SystemConstantsWithSun).
  p << "cbuffer xe_system_cbuffer : register(b0) {\n"
       "  uint4 xe_system_head[15];\n"
       "  float4 xe_color_exp_bias;\n"
       "  uint4 xe_system_tail["
    << sizeof(DxbcShaderTranslator::SystemConstants) / 16 - 16
    << "];\n"
       "  float4 pgr4_sun_direction;\n"
       "  float4 pgr4_sun_color;\n"
       "  float4 pgr4_time;\n"
       "  float4 pgr4_clip[4];\n"
       "  uint4 pgr4_scene;\n"
       "  float4 pgr4_view[3];\n"
       "  float4 pgr4_camera;\n"
       "  float4 pgr4_wiper[4];\n"
       "  float4 pgr4_wiper_blade;\n"
       "  uint4 pgr4_wiper_mask;\n"
       "  float4 pgr4_shelter;\n"
       "  float4 pgr4_dirt;\n";
  // Only then, so that the other shaders' text (and their cache) stays the same without it.
  if (REXCVAR_GET(pgr4_native_water_live)) p << "  float4 pgr4_water[6];\n";
  p << "};\n";
  const auto& map = shader.constant_register_map();
  const uint32_t floats = map.float_dynamic_addressing ? 256 : std::max(1u, map.float_count);
  p << "cbuffer xe_float_cbuffer : register(b1) { float4 xe_float_constants[" << floats
    << "]; };\n";
  for (uint32_t c = 0; c < 256; ++c) {
    const uint32_t packed = map.GetPackedFloatConstantIndex(c);
    if (packed != UINT32_MAX && packed < floats)
      p << "#define c" << c << " xe_float_constants[" << packed << "]\n";
  }
  // The bool constants (the 256 bits of SHADER_CONSTANT_BOOL; a pixel shader's from 128) and the
  // loop constants.
  p << "cbuffer xe_bool_loop_cbuffer : register(b2) {\n"
       "  uint4 xe_bool_constants[2];\n"
       "  uint4 xe_loop_constants[8];\n"
       "};\n"
       "#define XE_BOOL(n) ((xe_bool_constants[(n) >> 7][((n) >> 5) & 3] >> ((n) & 31)) & 1u)\n";
  p << "cbuffer xe_fetch_cbuffer : register(b3) { uint4 xe_fetch_constants[48]; };\n";
  uint32_t entries = 1;
  for (const auto& tb : shader.GetTextureBindingsAfterTranslation())
    entries = std::max(entries, tb.bindless_descriptor_index + 1);
  for (const auto& sb : shader.GetSamplerBindingsAfterTranslation())
    entries = std::max(entries, sb.bindless_descriptor_index + 1);
  p << "cbuffer xe_descriptor_indices_cbuffer : register(b4) { uint4 xe_descriptor_indices["
    << (entries + 3) / 4 << "]; };\n"
       "#define XE_DESCRIPTOR(i) xe_descriptor_indices[(i) >> 2][(i) & 3]\n"
       "SamplerState xe_samplers[] : register(s0, space0);\n"
       "Texture2DArray<float4> xe_textures_2d[] : register(t0, space1);\n"
       "Texture3D<float4> xe_textures_3d[] : register(t0, space2);\n"
       "TextureCube<float4> xe_textures_cube[] : register(t0, space3);\n";
  for (const auto& sb : shader.GetSamplerBindingsAfterTranslation()) {
    const uint32_t f = sb.fetch_constant;
    p << "#define XE_SAMPLER_TF" << f << " xe_samplers[XE_DESCRIPTOR("
      << sb.bindless_descriptor_index << ")]\n";
    // The 2D and the cube texture at the fetch constant, unsigned if both signs are bound.
    using rex::graphics::xenos::FetchOpDimension;
    for (const FetchOpDimension dim : {FetchOpDimension::k2D, FetchOpDimension::kCube}) {
      const DxbcShader::TextureBinding* tex = nullptr;
      for (const auto& tb : shader.GetTextureBindingsAfterTranslation())
        if (tb.fetch_constant == f && tb.dimension == dim &&
            (!tex || (tex->is_signed && !tb.is_signed)))
          tex = &tb;
      if (!tex) continue;
      const bool cube = dim == FetchOpDimension::kCube;
      p << "#define XE_HAS_" << (cube ? "CUBE" : "TF") << f << " 1\n"
        << "#define XE_" << (cube ? "CUBE" : "TEXTURE") << "_TF" << f << " xe_textures_"
        << (cube ? "cube" : "2d") << "[XE_DESCRIPTOR(" << tex->bindless_descriptor_index
        << ")]\n";
    }
    // A 3D-or-stacked binding: one descriptor, a 3D view for a 3D texture and a 2D array for a
    // stacked one (TextureCache::Get), read as XE_TEXTURE3D_TF<f> or XE_STACKED_TF<f>; which one
    // the fetch constant says (dimension, dword 5 bits 9-10: 2 for 3D), as the translator does.
    for (const auto& tb : shader.GetTextureBindingsAfterTranslation())
      if (tb.fetch_constant == f && tb.dimension == FetchOpDimension::k3DOrStacked &&
          !tb.is_signed) {
        p << "#define XE_HAS_STACKED" << f << " 1\n"
          << "#define XE_STACKED_TF" << f << " xe_textures_2d[XE_DESCRIPTOR("
          << tb.bindless_descriptor_index << ")]\n"
          << "#define XE_TEXTURE3D_TF" << f << " xe_textures_3d[XE_DESCRIPTOR("
          << tb.bindless_descriptor_index << ")]\n"
          << "#define XE_IS_3D_TF" << f << " (((xe_fetch_constants[" << (f * 6 + 5) / 4 << "]["
          << (f * 6 + 5) % 4 << "] >> 9) & 3u) == 2u)\n";
        break;
      }
  }
  p << "#define TEX2D(f, uv) XE_TEXTURE_TF##f.Sample(XE_SAMPLER_TF##f, float3((uv), 0.0))\n"
       "#define TEX2D_LEVEL(f, uv, level) "
       "XE_TEXTURE_TF##f.SampleLevel(XE_SAMPLER_TF##f, float3((uv), 0.0), (level))\n"
       "#define TEX2D_BIAS(f, uv, bias) "
       "XE_TEXTURE_TF##f.SampleBias(XE_SAMPLER_TF##f, float3((uv), 0.0), (bias))\n"
       "#define TEX2D_LAYER(f, uv, layer, level) "
       "XE_TEXTURE_TF##f.SampleLevel(XE_SAMPLER_TF##f, float3((uv), (layer)), (level))\n"
       "#define TEXCUBE(f, dir) XE_CUBE_TF##f.Sample(XE_SAMPLER_TF##f, (dir))\n"
       "#define TEXCUBE_LEVEL(f, dir, level) "
       "XE_CUBE_TF##f.SampleLevel(XE_SAMPLER_TF##f, (dir), (level))\n";

  // The input signature: the guest's interpolators r0..., then the motion vectors' positions.
  uint32_t texcoords = 0;
  for (const SignatureElement& e : in)
    if (e.name == "TEXCOORD") ++texcoords;
  const uint32_t guest = motion ? texcoords - std::min(texcoords, 2u) : texcoords;
  p << "struct XeInput {\n";
  for (const SignatureElement& e : in) {
    const char* type = ComponentType(e.component_type, uint32_t(std::popcount(e.mask)));
    if (e.name == "TEXCOORD") {
      if (e.semantic_index < guest)
        p << "  float4 r" << e.semantic_index << " : TEXCOORD" << e.semantic_index << ";\n";
      else
        p << "  float4 xe_motion" << e.semantic_index - guest << " : TEXCOORD"
          << e.semantic_index << ";\n";
    } else if (e.name == "SV_Position") {
      p << "  float4 pos : SV_Position;\n#define XE_HAS_POS 1\n";
    } else if (e.name == "SV_IsFrontFace") {
      p << "  bool front : SV_IsFrontFace;\n";
    } else {
      p << "  " << type << " xe_in_" << e.reg << " : " << e.name << e.semantic_index << ";\n";
    }
  }
  p << "};\n#define XE_MOTION_VECTORS " << (motion ? 1 : 0) << "\n";
  p << "#define XE_INTERPOLATORS " << guest << "\n";
  *prelude = p.str();

  std::ostringstream m;
  m << "\nstruct XeOutput {\n";
  bool color = false, coverage = false;
  for (const SignatureElement& e : out) {
    if (e.name == "SV_Coverage") {  // alpha to coverage: every sample
      m << "  uint xe_coverage : SV_Coverage;\n";
      coverage = true;
      continue;
    }
    if (e.name != "SV_Target") return false;  // depth: not supported
    if (e.semantic_index == 0)
      color = true;
    else if (!motion || e.semantic_index != DxbcShaderTranslator::kMotionVectorRenderTarget)
      return false;  // more color targets: not supported
    m << "  " << (e.semantic_index ? "float2" : "float4") << " o" << e.semantic_index
      << " : SV_Target" << e.semantic_index << ";\n";
  }
  if (!color) return false;
  // Local change (19/11/2026, Tazz: « regarde autour des motos y'a comme un flou bizarre »): the
  // game's alpha test, as the translator does it (dxbc_translator_om.cpp, CompletePixelShader: on
  // the color's alpha before the exponent bias, discarded when it fails; not equal true for NaN).
  // Without it, the replaced draws cut out by their alpha (the bikes' blurred wheels' quads, parts
  // of the riders, the lamps' covers) wrote their whole quads: their alpha 0 there, which the
  // game's motion blur takes for a vehicle (not blurred: a sharp zone around the bikes), and their
  // depth. Only in the variants the translator gives no early depth (depth_stencil_mode: alpha test
  // or alpha to mask possible, ShaderModification in shader_translate.cpp), which then lose the
  // forced early depth too; the other variants' text (and cache) stays the same. Read when the
  // shaders are made.
  using DepthStencilMode = DxbcShaderTranslator::Modification::DepthStencilMode;
  const bool early = DxbcShaderTranslator::Modification(translation.modification())
                         .pixel.depth_stencil_mode == DepthStencilMode::kEarlyHint;
  const bool alpha_test = !early && REXCVAR_GET(pgr4_native_replace_alpha_test);
  m << "};\n" << (coverage || alpha_test ? "" : "[earlydepthstencil]\n")
    << "XeOutput main(XeInput i) {\n"
       "  XeOutput o;\n"
       "  const float4 xe_color = Shade(i);\n";
  if (alpha_test) {
    constexpr uint32_t ref = offsetof(DxbcShaderTranslator::SystemConstants, alpha_test_reference);
    static_assert(ref < 240 && ref % 4 == 0);
    m << "  const uint xe_alpha_func = (xe_system_head[0].x >> "
      << uint32_t(DxbcShaderTranslator::kSysFlag_AlphaPassIfLess_Shift) << "u) & 7u;\n"
      << "  if (xe_alpha_func != 7u) {\n"
      << "    const float xe_alpha_ref = asfloat(xe_system_head[" << ref / 16 << "]."
      << "xyzw"[(ref % 16) / 4] << ");\n"
      << "    const bool xe_alpha_pass = xe_alpha_func == 5u\n"
         "        ? xe_color.a != xe_alpha_ref\n"
         "        : ((xe_alpha_func & 1u) != 0u && xe_color.a < xe_alpha_ref) ||\n"
         "          ((xe_alpha_func & 2u) != 0u && xe_color.a == xe_alpha_ref) ||\n"
         "          ((xe_alpha_func & 4u) != 0u && xe_color.a > xe_alpha_ref);\n"
         "    if (!xe_alpha_pass) discard;\n"
         "  }\n";
  }
  m << "  o.o0 = xe_color * xe_color_exp_bias.x;\n";
  if (coverage) m << "  o.xe_coverage = 0xFFFFFFFFu;\n";
  // Local change (16/11/2026): as the translator writes them (dxbc_translator_om.cpp): none for a
  // point behind the camera in the previous frame (w at most 0: an infinite or NaN motion, black
  // blocks in DLSS Frame Generation's frames), any other at most one screen per frame.
  if (motion)
    m << "  float2 xe_now = i.xe_motion0.xy / i.xe_motion0.w;\n"
         "  float2 xe_before = i.xe_motion1.xy / i.xe_motion1.w;\n"
         "  o.o"
      << DxbcShaderTranslator::kMotionVectorRenderTarget
      << " = i.xe_motion1.w >= 1.0 / 1024.0\n"
         "      ? clamp((xe_before - xe_now) * float2(0.5, -0.5), -1.0, 1.0) : float2(0.0, 0.0);\n";
  m << "  return o;\n}\n";
  *main = m.str();
  return true;
}

}  // namespace

std::unordered_map<uint64_t, std::filesystem::file_time_type> ShaderReplacer::List() const {
  std::unordered_map<uint64_t, std::filesystem::file_time_type> out;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(dir_, ec)) {
    const std::string name = entry.path().filename().string();
    uint64_t hash = 0;
    char rest[16] = {};
    if (name.size() == 16 + 8 &&
        std::sscanf(name.c_str(), "%16llX%15s", reinterpret_cast<unsigned long long*>(&hash),
                    rest) == 2 &&
        !_stricmp(rest, ".ps.hlsl"))
      out[hash] = entry.last_write_time(ec);
    // The files they include (*.hlsli): the latest write time of any, as kIncludes, so that a
    // change to one compiles every replacement again.
    else if (entry.path().extension() == ".hlsli") {
      const auto time = entry.last_write_time(ec);
      auto [it, added] = out.try_emplace(kIncludes, time);
      if (!added && time > it->second) it->second = time;
    }
  }
  return out;
}

bool ShaderReplacer::IsCarPaint(uint64_t hash) const {
  char name[32];
  std::snprintf(name, sizeof(name), "%016llX.ps.hlsl", static_cast<unsigned long long>(hash));
  char head[800] = {};
  if (std::FILE* f = _wfopen((dir_ / name).c_str(), L"rb")) {
    const size_t got = std::fread(head, 1, sizeof(head) - 1, f);
    std::fclose(f);
    head[got] = 0;
  }
  return std::strstr(head, "car_paint_hooks") != nullptr;
}

bool ShaderReplacer::IsCarShader(uint64_t hash) const {
  char name[32];
  std::snprintf(name, sizeof(name), "%016llX.ps.hlsl", static_cast<unsigned long long>(hash));
  char head[1600] = {};
  if (std::FILE* f = _wfopen((dir_ / name).c_str(), L"rb")) {
    const size_t got = std::fread(head, 1, sizeof(head) - 1, f);
    std::fclose(f);
    head[got] = 0;
  }
  return std::strstr(head, "car_paint_hooks") != nullptr || std::strstr(head, "car_reflect") != nullptr;
}

void ShaderReplacer::Initialize() {
  enabled_ = REXCVAR_GET(pgr4_native_shader_replace) && !OriginalLook();
  dir_ = BesideExe(REXCVAR_GET(pgr4_native_shader_replace_dir));
  auto files = List();
  std::lock_guard<std::mutex> lock(mutex_);
  files_ = std::move(files);
  reflections_ = EnhancedReflections();
  decal_light_off_ = REXCVAR_GET(pgr4_native_decal_light_off) && !OriginalLook();
  ClassifyFiles();
  REXLOG_INFO("PGR4 native shaders: {} replacement pixel shaders in {} (reloaded when written)",
              files_.size(), dir_.string());
}

std::string ShaderReplacer::TuningDefines() { return ShaderTuningDefines(); }

void ShaderReplacer::ClassifyFiles() {
  reflection_files_.clear();
  decal_files_.clear();
  for (const auto& [hash, time] : files_) {
    if (hash == kIncludes) continue;
    char name[32];
    std::snprintf(name, sizeof(name), "%016llX.ps.hlsl", (unsigned long long)hash);
    std::ifstream in(dir_ / name);
    const std::string text((std::istreambuf_iterator<char>(in)), {});
    // The cars' exterior mirrors' (DE108A45B02E2681, supersampled) are theirs too.
    if (hash == 0xDE108A45B02E2681ull || text.find("car_") != std::string::npos ||
        text.find("water_hooks.hlsli") != std::string::npos ||
        text.find("road_reflect_hooks.hlsli") != std::string::npos)
      reflection_files_.insert(hash);
    if (text.find("pgr4_decal_light") != std::string::npos) decal_files_.insert(hash);
  }
}

void ShaderReplacer::Poll() {
  // A tuning changed (once it held kTuningFrames: a slider being dragged is not compiled at each
  // step): the replaced shaders whose hooks read it compiled again in the background, each one
  // drawn with its current code until its new one is ready (not the game's: no flicker).
  // A weather's change (WaterWeather) is not a slider: compiled at once (pgr4_native_water_weather_at_load).
  const int weather = WaterWeather();
  if (std::string tuning = TuningDefines(); tuning == tuning_) {
    tuning_weather_ = weather;
  } else if (tuning_.empty()) {
    tuning_ = std::move(tuning);
    tuning_weather_ = weather;
  } else {
    const bool weather_changed =
        REXCVAR_GET(pgr4_native_water_weather_at_load) && weather != tuning_weather_;
    const bool fresh = tuning != tuning_wanted_;
    if (fresh) {
      tuning_wanted_ = std::move(tuning);
      tuning_frames_ = 0;
    }
    if (weather_changed || (!fresh && ++tuning_frames_ >= kTuningFrames)) {
      tuning_weather_ = weather;
      // Which hooks the change concerns, by the #defines that differ.
      const auto value = [](const std::string& s, const char* name) {
        const size_t at = s.find(name);
        return at == std::string::npos ? std::string() : s.substr(at, s.find('\n', at) - at);
      };
      const bool road = value(tuning_wanted_, "kRoadNormal ") != value(tuning_, "kRoadNormal ");
      bool cars = false;
      for (const char* name : {"kSunGlint ", "kSunSharpness ", "kRimReflection ", "kGlassReflect ",
                               "kGlassSky ", "kGlassSkyFrom ", "kDropsPaintScale ",
                               "kDropsGlideAngle ", "kDropsStretch ", "kDropsNear ", "kDropsPaintStrength ",
                               "kFrostPaintScale ", "kScratchPaintScale "})
        cars |= value(tuning_wanted_, name) != value(tuning_, name);
      bool wet = false;  // the wet roads' reflection (road_reflect_hooks.hlsli)
      for (const char* name : {"kRoadReflect ", "kRoadBlur ", "kRoadFresnelMin ", "kRoadGrain ",
                               "kRoadPuddles ", "kRoadSheen ", "kRoadReflectLight ",
                               "kRoadBlurLight ", "kRoadFresnelMinLight ", "kRoadGrainLight ",
                               "kRoadPuddlesLight ", "kRoadSheenLight "})
        wet |= value(tuning_wanted_, name) != value(tuning_, name);
      bool water = false;  // the water (water_hooks.hlsli)
      for (const char* name : {"kWaveStrength ", "kWaveSteepness ", "kNoiseShare ", "kWindAngle ",
                               "kWaveSpeed ", "kWaterReflect ", "kWaveSize ",
                               "kHarbourClarity ", "kWaterDistort ", "kHarbourWaves ",
                               "kSunWaves ", "kWaterFog ", "PGR4_WATER_LIVE "})
        water |= value(tuning_wanted_, name) != value(tuning_, name);
      // The final composition's highlights (highlights_hooks.hlsli).
      const bool highlights =
          value(tuning_wanted_, "kHighlights ") != value(tuning_, "kHighlights ") ||
          value(tuning_wanted_, "kBloom ") != value(tuning_, "kBloom ");
      std::lock_guard<std::mutex> lock(mutex_);
      tuning_ = tuning_wanted_;
      uint32_t queued = 0, stale = 0;
      // A shader compiling now may have read the tunings before this change (Compile reads them
      // when it starts): compiled again once done (pgr4_native_tune_requeue_pending).
      if (REXCVAR_GET(pgr4_native_tune_requeue_pending))
        for (const void* key : pending_) stale += stale_.insert(key).second;
      for (auto& [key, code] : compiled_) {
        if (!code || code->empty() || pending_.count(key)) continue;
        const auto* translation = static_cast<const DxbcShader::Translation*>(key);
        const uint64_t hash = translation->shader().ucode_data_hash();
        char name[32];
        std::snprintf(name, sizeof(name), "%016llX.ps.hlsl", (unsigned long long)hash);
        std::ifstream in(dir_ / name);
        const std::string text((std::istreambuf_iterator<char>(in)), {});
        const bool uses_road = text.find("road_hooks.hlsli") != std::string::npos;
        const bool uses_cars = text.find("car_") != std::string::npos;
        const bool uses_wet = text.find("road_reflect_hooks.hlsli") != std::string::npos;
        const bool uses_water = text.find("water_hooks.hlsli") != std::string::npos;
        const bool uses_highlights = text.find("highlights_hooks.hlsli") != std::string::npos;
        if (!((road && uses_road) || (cars && uses_cars) || (wet && uses_wet) ||
              (water && uses_water) || (highlights && uses_highlights)))
          continue;
        pending_.insert(key);
        jobs_.push_back({translation, hash, files_epoch_, true});
        ++queued;
      }
      if (queued) {
        if (workers_.empty())
          for (int k = 0; k < 2; ++k) workers_.emplace_back([this] { Work(); });
        jobs_cv_.notify_all();
      }
      REXLOG_INFO("PGR4 native shaders: tunings changed, {} shaders compiled again, {} compiling "
                  "again once done (water weather {}{})",
                  queued, stale, weather, weather_changed ? ", at once" : "");
      return;
    }
  }
  // pgr4_native_shader_replace switched (the overlay), or the enhanced reflections: every replaced
  // shader's pipelines again.
  if (const bool enabled = REXCVAR_GET(pgr4_native_shader_replace) && !OriginalLook(),
      reflections = EnhancedReflections(),
      decal_off = REXCVAR_GET(pgr4_native_decal_light_off) && !OriginalLook();
      enabled != enabled_ || reflections != reflections_ || decal_off != decal_light_off_) {
    std::lock_guard<std::mutex> lock(mutex_);
    enabled_ = enabled;
    reflections_ = reflections;
    decal_light_off_ = decal_off;
    for (auto& [translation, code] : compiled_) retired_.push_back(std::move(code));
    compiled_.clear();
    ++files_epoch_;
    generation_.fetch_add(1, std::memory_order_acq_rel);
    REXLOG_INFO("PGR4 native shaders: replacements {}, enhanced reflections {}",
                enabled ? "on" : "off", reflections ? "on" : "off");
    return;
  }
  if (!enabled_ || ++poll_ < kPollFrames) return;
  poll_ = 0;
  auto files = List();
  std::lock_guard<std::mutex> lock(mutex_);
  if (files == files_) return;
  for (const auto& [hash, time] : files) {
    auto it = files_.find(hash);
    if (it == files_.end() || it->second != time)
      REXLOG_INFO("PGR4 native shaders: {:016X}.ps.hlsl {}", hash,
                  it == files_.end() ? "added" : "changed: compiled again");
  }
  for (const auto& [hash, time] : files_)
    if (!files.count(hash)) REXLOG_INFO("PGR4 native shaders: {:016X}.ps.hlsl removed", hash);
  files_ = std::move(files);
  ClassifyFiles();
  for (auto& [translation, code] : compiled_) retired_.push_back(std::move(code));
  compiled_.clear();
  ++files_epoch_;
  generation_.fetch_add(1, std::memory_order_acq_rel);
}

const std::vector<uint8_t>* ShaderReplacer::Get(const DxbcShader::Translation& translation) {
  const uint64_t hash = translation.shader().ucode_data_hash();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_ || !files_.count(hash)) return nullptr;
    if (!reflections_ && reflection_files_.count(hash)) return nullptr;
    if (!decal_light_off_ && decal_files_.count(hash)) return nullptr;
    auto it = compiled_.find(&translation);
    if (it != compiled_.end()) return it->second->empty() ? nullptr : it->second.get();
    if (pending_.count(&translation)) return nullptr;
  }
  // From the cache at once; otherwise compiled in the background while the game's shader draws.
  std::vector<uint8_t> cached = Compile(translation, hash, /*cache_only=*/true);
  std::lock_guard<std::mutex> lock(mutex_);
  if (!cached.empty()) {
    auto& slot = compiled_[&translation];
    if (!slot) slot = std::make_unique<std::vector<uint8_t>>(std::move(cached));
    return slot->empty() ? nullptr : slot.get();
  }
  if (pending_.insert(&translation).second) {
    jobs_.push_back({&translation, hash, files_epoch_});
    if (workers_.empty())
      // Local change (pgr4 project): the launcher's precompile step has nothing to leave the CPU to.
      for (int k = 0, n = REXCVAR_GET(pgr4_native_precompile)
                              ? std::max(2, int(std::thread::hardware_concurrency()) - 2)
                              : std::clamp(int(std::thread::hardware_concurrency()) / 2, 2, 8);
           k < n; ++k)
        workers_.emplace_back([this] { Work(); });
    jobs_cv_.notify_one();
  }
  return nullptr;
}

void ShaderReplacer::Work() {
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      jobs_cv_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
      if (stopping_) return;
      job = jobs_.front();
      jobs_.pop_front();
    }
    auto code = std::make_unique<std::vector<uint8_t>>(Compile(*job.translation, job.hash));
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.erase(job.translation);
    const bool stale = stale_.erase(job.translation) > 0;  // a tuning changed while it compiled
    if (job.epoch != files_epoch_) continue;  // reloaded meanwhile: compiled again on demand
    auto& slot = compiled_[job.translation];
    if (!slot) {
      slot = std::move(code);
    } else if (job.replace && !code->empty()) {
      retired_.push_back(std::move(slot));
      slot = std::move(code);
    }
    // Local change (pgr4 project): only a tuning's compile (replacing code pipelines were made
    // with) makes the replaced pipelines again. A first compile is picked up by the pipelines
    // waiting for it (draw.cpp: Pipeline::awaiting_hlsl); counting up here at each one remade
    // every replaced shader's pipelines hundreds of times while a hooks file compiled.
    if (job.replace) generation_.fetch_add(1, std::memory_order_acq_rel);
    // Its code made with the tunings before the last change: compiled again with the current ones.
    if (stale && pending_.insert(job.translation).second) {
      jobs_.push_back({job.translation, job.hash, files_epoch_, true});
      jobs_cv_.notify_one();
      REXLOG_INFO("PGR4 native shaders: {:016X} compiled again (a tuning changed while it compiled)",
                  job.hash);
    }
  }
}

ShaderReplacer::~ShaderReplacer() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  jobs_cv_.notify_all();
  for (std::thread& t : workers_) t.join();
}

std::vector<uint8_t> ShaderReplacer::Compile(const DxbcShader::Translation& translation,
                                             uint64_t hash, bool cache_only) {
  std::vector<uint8_t> code;

  char file[32];
  std::snprintf(file, sizeof(file), "%016llX.ps.hlsl", static_cast<unsigned long long>(hash));
  const std::filesystem::path path = dir_ / file;
  std::ifstream f(path, std::ios::binary);
  std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  std::string prelude, main;
  if (body.empty() || !MakePrelude(translation, &prelude, &main)) {
    REXLOG_WARN("PGR4 native shaders: {:016X} modification {:016X}: {} (not replaced)", hash,
                translation.modification(),
                body.empty() ? "empty file" : "unsupported translation");
    return code;
  }
  // The file's full path names it, so that its #include "x.hlsli" are found beside it.
  const std::string where = path.generic_string();
  const std::string source =
      prelude + "#line 1 \"" + where + "\"\n" + body + "\n#line 1 \"main\"\n" + main;
  // The bytecode of this source and of the files it may include (*.hlsli) from an earlier run:
  // <dir>\cache\<key>.cso.
  uint64_t key = XXH3_64bits(source.data(), source.size());
  {
    std::error_code ec;
    std::vector<std::filesystem::path> includes;
    for (const auto& entry : std::filesystem::directory_iterator(dir_, ec))
      if (entry.path().extension() == ".hlsli") includes.push_back(entry.path());
    std::sort(includes.begin(), includes.end());
    for (const auto& include : includes) {
      std::ifstream in(include, std::ios::binary);
      const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      key = XXH3_64bits_withSeed(text.data(), text.size(), key);
    }
  }
  char cached_name[32];
  std::snprintf(cached_name, sizeof(cached_name), "%016llX.cso",
                static_cast<unsigned long long>(key));
  const std::filesystem::path cached = dir_ / "cache" / cached_name;
  {
    std::ifstream in(cached, std::ios::binary);
    if (in) {
      code.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
      if (!code.empty()) return code;
    }
  }
  if (cache_only) return code;
  const pD3DCompile compile = D3DCompiler();
  Microsoft::WRL::ComPtr<ID3DBlob> blob, errors;
  if (!compile || FAILED(compile(source.data(), source.size(), where.c_str(), nullptr,
                                 D3D_COMPILE_STANDARD_FILE_INCLUDE, "main",
                                 "ps_5_1",
                                 D3DCOMPILE_OPTIMIZATION_LEVEL3 |
                                     D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES,
                                 0, &blob, &errors))) {
    REXLOG_ERROR("PGR4 native shaders: {} failed to compile: {}", file,
                 errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no compiler");
    std::ofstream(dir_ / (std::string(file) + ".generated.txt")) << source;
    return code;
  }
  const auto* b = static_cast<const uint8_t*>(blob->GetBufferPointer());
  code.assign(b, b + blob->GetBufferSize());
  {
    std::error_code ec;
    std::filesystem::create_directories(cached.parent_path(), ec);
    std::ofstream out(cached, std::ios::binary);
    out.write(reinterpret_cast<const char*>(code.data()), std::streamsize(code.size()));
  }
  REXLOG_INFO("PGR4 native shaders: {} replaces modification {:016X}{}", file,
              translation.modification(),
              DxbcShaderTranslator::Modification(translation.modification()).pixel.motion_vectors
                  ? " (with motion vectors)"
                  : "");
  return code;
}

}  // namespace pgr4::native
