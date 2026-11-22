// pgr4-native PGR4 native renderer, local addition (pgr4 project, 20/11/2026): volumetric fog
// (volumetrics.h).
#include "volumetrics.h"

#include "clock.h"  // development: Ticks, the race probe's timing (Apply)
#include "hang_probe.h"  // local change: the freeze probe

#include "d3d12_device.h"
#include "gpu_timer.h"
#include "upscaler.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <d3dcompiler.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>

// Local change (22/11/2026, Tazz): the volumetric effects chosen in the launcher (VOLUMETRIC EFFECTS),
// off by default: off, nothing of them made or prepared (the precompile, the prewarm, the volume, the
// draws' fog variants) and the GRAPHICS menu's row hidden. Read at the start (the volume) and live.
REXCVAR_DEFINE_BOOL(pgr4_native_volumetric_effects, false, "PGR4 native",
                    "The volumetric effects (the launcher's choice): off, their shaders neither made "
                    "nor prepared and the game's GRAPHICS menu without them");
REXCVAR_DEFINE_BOOL(pgr4_native_volumetric, false, "PGR4 native",
                    "Volumetric fog over the 3D scene: the game's fog redone in volume (drifting "
                    "banks, the sun's shafts) and the sun's glow in the air (an option; live)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_volumetric_strength, 1.0, "PGR4 native",
                      "Volumetric fog: the strength of the sun's glow in the air (0: none)")
    .range(0.0, 4.0);
// Off by default (Tazz, 21/11/2026: 0.004 veiled Macau's clear day milky, its far street 105 -> 122,
// with no shafts to show on a sunny street; the fog alone is the game's there).
REXCVAR_DEFINE_DOUBLE(pgr4_native_volumetric_density, 0.0, "PGR4 native",
                      "Volumetric fog: the sun-lit air's density near the ground, per metre, the same "
                      "in every weather (0: none)")
    .range(0.0, 0.05);
REXCVAR_DEFINE_DOUBLE(pgr4_native_volumetric_distance, 150.0, "PGR4 native",
                      "Volumetric fog: the metres of sun-lit air, thinning out over the second half "
                      "(Tazz, 21/11/2026: 400 m made the far end of a street a white wall into the sun)")
    .range(20.0, 400.0);
REXCVAR_DEFINE_BOOL(pgr4_native_volumetric_fog, true, "PGR4 native",
                    "Volumetric fog: the game's own fog taken out of the scene and laid again in "
                    "volume, its thickness and color kept (false: the game's as it is; live)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_volumetric_fog_variation, 0.45, "PGR4 native",
                      "Volumetric fog: how much its banks thicken and thin (0: even, the game's own "
                      "exactly)")
    .range(0.0, 1.0);
REXCVAR_DEFINE_DOUBLE(pgr4_native_volumetric_fog_size, 6.0, "PGR4 native",
                      "Volumetric fog: the banks' size, metres")
    .range(5.0, 200.0);
// Off by default (Tazz, 21/11/2026: "more logical without", the fog keeps the game's color).
REXCVAR_DEFINE_DOUBLE(pgr4_native_volumetric_fog_sun, 0.0, "PGR4 native",
                      "Volumetric fog: the sun's light in it, where it gets through (0: the game's "
                      "even color alone)")
    .range(0.0, 4.0);
// The rest of its tuning, live too (Tazz, 21/11/2026: "everything tunable live").
REXCVAR_DEFINE_DOUBLE(pgr4_native_volumetric_phase, 0.6, "PGR4 native",
                      "Volumetric fog: how much the air scatters the sun forward, towards the eye "
                      "looking at it (Henyey-Greenstein's g: 0 even, 0.9 a tight glow)")
    .range(0.0, 0.95);
REXCVAR_DEFINE_DOUBLE(pgr4_native_volumetric_height_scale, 60.0, "PGR4 native",
                      "Volumetric fog: the metres over which the sun-lit air thins by e going up")
    .range(5.0, 500.0);
REXCVAR_DEFINE_DOUBLE(pgr4_native_volumetric_eye_height, 2.0, "PGR4 native",
                      "Volumetric fog: the eye's height above the road taken for the sun-lit air, metres")
    .range(0.0, 20.0);
REXCVAR_DEFINE_DOUBLE(pgr4_native_volumetric_thickness, 30.0, "PGR4 native",
                      "Volumetric fog: the metres behind a surface still taken as inside it for the "
                      "sun's shadow (a building's depth)")
    .range(1.0, 200.0);
REXCVAR_DEFINE_DOUBLE(pgr4_native_volumetric_wind, 1.5, "PGR4 native",
                      "Volumetric fog: the banks' drift, metres a second")
    .range(0.0, 20.0);
REXCVAR_DEFINE_DOUBLE(pgr4_native_volumetric_wind_angle, 20.0, "PGR4 native",
                      "Volumetric fog: the drift's direction, degrees from the world's x axis")
    .range(0.0, 360.0);
REXCVAR_DEFINE_INT32(pgr4_native_volumetric_steps, 24, "PGR4 native",
                     "Volumetric fog: the steps along each ray for the sun-lit air")
    .range(4, 96);
REXCVAR_DEFINE_INT32(pgr4_native_volumetric_shadow_steps, 5, "PGR4 native",
                     "Volumetric fog: the steps towards the sun for each step's shadow (0: none)")
    .range(0, 12);
REXCVAR_DEFINE_DOUBLE(pgr4_native_volumetric_fog_phase, 0.2, "PGR4 native",
                      "Volumetric fog: how much the fog scatters the sun forward (Henyey-Greenstein's "
                      "g; thick fog spreads it every way, 21/11/2026: 0.6 burnt the view into the sun "
                      "white)")
    .range(0.0, 0.95);
// The ground mist (Tazz, 21/11/2026: the fog laid again looked the same as the game's): patches of
// mist on the road, near the eye, their top rising and falling, drifting with the wind. 0.5 by
// default (Tazz, 21/11/2026; it was 2).
REXCVAR_DEFINE_DOUBLE(pgr4_native_volumetric_mist, 1.0, "PGR4 native",
                      "Volumetric fog: the ground mist's thickness, times the game's fog at the road "
                      "(thick in fog and rain, next to none in clear weather; 0: none)")
    .range(0.0, 10.0);
REXCVAR_DEFINE_DOUBLE(pgr4_native_volumetric_mist_height, 3.0, "PGR4 native",
                      "Volumetric fog: the ground mist's height above the road, metres (its top "
                      "rising and falling by half of it)")
    .range(0.5, 30.0);
REXCVAR_DEFINE_DOUBLE(pgr4_native_volumetric_mist_distance, 50.0, "PGR4 native",
                      "Volumetric fog: the metres of road the ground mist covers, thinning out over "
                      "the second half")
    .range(10.0, 400.0);
// Local change (21/11/2026, Tazz: "flat, not a next-gen volumetric fog"): the fog in a volume of
// cells following the view, its banks contrasted and lit from above (the per-pixel march's mean
// along each ray, which averaged the banks away, removed on 21/11/2026).
REXCVAR_DEFINE_DOUBLE(pgr4_native_volumetric_range, 300.0, "PGR4 native",
                      "Volumetric fog: the volume's depth, metres (the game's fog beyond)")
    .range(50.0, 1000.0);
// Local change (21/11/2026, Heavy Fog: "the fog lights up now and then while driving"): the eye
// from the frame's camera (its clip transform's center), not the last fog draw's c5: some of the
// scene's fog draws are made from another camera (c5.y 94.8 against 105.2 within 2 s, the road's
// reflection's most likely), and the height fog worked out from the wrong eye lit or darkened ours.
REXCVAR_DEFINE_BOOL(pgr4_native_volumetric_eye_from_camera, true, "PGR4 native",
                    "Volumetric fog: the eye from the frame's camera; off: the last fog draw's c5");
// Local change (21/11/2026, Las Vegas' Heavy Fog: the banks darkened the fog, its far street 56
// against the game's 115): the fog shaders scale their fogged color by c12.y (all 820 dumped), so
// the fog's color in the scene's target is c10.rgb x c12.y; laid again as c10.rgb alone, the banks
// put a fog 3 times too dark where they changed it. On by default; off: c10.rgb alone.
// Local change (21/11/2026, the fog to be laid by each shader as the game lays its own): the volume
// made at the frame's first main-view draw with the game's fog (draw.cpp), before the scene's draws
// that will read it, instead of at the scene's resolve; laid at the resolve as before. Off: made at
// the resolve, as it was.
REXCVAR_DEFINE_BOOL(pgr4_native_volumetric_volume_early, true, "PGR4 native",
                    "Volumetric fog: the volume made before the scene's draws (at its first draw "
                    "with the game's fog), not at the scene's resolve; off: at the resolve");
REXCVAR_DEFINE_BOOL(pgr4_native_volumetric_fog_c12, true, "PGR4 native",
                    "Volumetric fog: the fog's color times the fog shaders' c12.y, as in the scene; "
                    "off: c10.rgb alone");
// Local change (21/11/2026, Tazz: the spectators' flags drawn after the fog, by
// 287EC49DD0AD9E71 with a fog of its own (c32-c35), kept the game's fog, not the banks around
// them): the volume shared with the replacement shaders of those draws (fog_volume_hooks.hlsli).
// On by default; off: their own fog as the game has it.
REXCVAR_DEFINE_BOOL(pgr4_native_volumetric_transparents, true, "PGR4 native",
                    "Volumetric fog: the transparent draws made after it (the spectators' flags) "
                    "take their fog's thickness from its volume; off: their own fog");
// Local change (21/11/2026, the Nurburgring: the banks followed the car, the camera FindCamera keeps
// there centred on the world's origin and tilted): the camera from the fog draws themselves
// (draw.cpp g_pgr4_fog_clip). On by default; off: FindCamera's.
REXCVAR_DEFINE_BOOL(pgr4_native_volumetric_fog_camera, true, "PGR4 native",
                    "Volumetric fog: the camera from the fog draws (centred on their eye, not "
                    "mirrored); off: the scene camera the upscaler uses");
// Local change (21/11/2026, Tazz: the ground mist moved at the replays' cuts (laid 2 m under the eye)
// and, from the filmed car, on fixed shots (the cars passing at other heights)): the mist laid over
// the ground seen, a map of the lowest height seen in each 2 m cell around the eye, made from the
// scene's depth each frame. On by default; off: the plane under the eye.
REXCVAR_DEFINE_BOOL(pgr4_native_volumetric_ground, true, "PGR4 native",
                    "Volumetric fog: the ground mist laid over the ground seen (a height map from the "
                    "scene's depth); off: a plane under the eye");
// Local change (21/11/2026, Tazz: "lights in the fog"): the game's lamps and lights (its light
// flares' centres, draw.cpp) scatter their light in the volume. On by default; off: none.
REXCVAR_DEFINE_BOOL(pgr4_native_volumetric_lights, true, "PGR4 native",
                    "Volumetric fog: the lamps light the fog around them (the game's light flares)");
// Tazz's (21/11/2026, 20 h 57: 15 -> 10; cone 50 -> 65, around 0.15 -> 0.1).
REXCVAR_DEFINE_DOUBLE(pgr4_native_volumetric_light_strength, 10.0, "PGR4 native",
                      "Volumetric fog: how bright the lamps' light in the fog is")
    .range(0.0, 100.0);
REXCVAR_DEFINE_DOUBLE(pgr4_native_volumetric_light_range, 150.0, "PGR4 native",
                      "Volumetric fog: the lamps taken within these metres of the eye (and, with "
                      "pgr4_native_volumetric_light_reach 0, how far each lights the fog)")
    .range(5.0, 300.0);
// 21/11/2026 (Tazz, Tokyo in fog: a row of lamps seen down its length made a white wall, the
// others a halo): the range both chose the lamps and was how far each lit the fog, so taking the
// far lamps (150 m) made each light 150 m of fog, and a row along the view piled them up. Each
// lamp's light now ends at its own reach, (1 - (d / reach)^4)^2 (smooth, nothing beyond); 0: the
// old linear fade over the range, for comparison.
REXCVAR_DEFINE_DOUBLE(pgr4_native_volumetric_light_reach, 10.0, "PGR4 native",
                      "Volumetric fog: the metres each lamp lights the fog to (0: the range's)")
    .range(0.0, 150.0);
// 21/11/2026 (Tazz: the light must pass over the bulb's texture, it stays black): a ray ending on
// the lamp's own head gathered only the glow before it, its neighbours passing by the lamp the glow
// behind it too, so the head stood dark in its halo. Where the surface is within a lamp's core (its
// flare's place), the glow is taken to the lamp's far side.
REXCVAR_DEFINE_BOOL(pgr4_native_volumetric_light_over_bulb, true, "PGR4 native",
                    "Volumetric fog: a lamp's glow over its own head (not cut by it)");
// 21/11/2026 (Tazz: "the light in the fog still blinks as I drive"). A lamp leaving the nearest
// ones was cut at once, one taken again came back at full light: each lamp's share now fades
// towards taken or not over half a second (off: the way before).
// 21/11/2026 (Tazz: "why has the glow the lamp's colour and not the light itself?"): every lamp
// had the one colour of pgr4_native_volumetric_light_red/green/blue; each now its flare's (the mean
// of the flare's texture, its brightest channel 1), the setting's colour a tint over it (off: the
// setting's colour alone, as before).
REXCVAR_DEFINE_BOOL(pgr4_native_volumetric_light_flare_colour, true, "PGR4 native",
                    "Volumetric fog: each lamp's light the colour of its flare (its texture's)");
// 21/11/2026 (Tazz: "I really want the light directional, towards the road: rays of light through
// the fog"): a street lamp lights downwards. Its light a cone around straight down, this half angle
// (0: all around, as before), its edge softened over light_cone_soft degrees inside it, and outside
// it light_cone_around of the light (the head's own glow).
REXCVAR_DEFINE_DOUBLE(pgr4_native_volumetric_light_cone, 65.0, "PGR4 native",
                      "Volumetric fog: the lamps' cone of light downwards, its half angle in degrees "
                      "(0: all around)")
    .range(0.0, 89.0);
// 21/11/2026 (Tazz: "the cone should start from the top of the flare, there it looks cut"): its
// tip at the lamp, the cone had no width just under the lamp's panel (a metre across). Its tip now
// this far above the lamp, so that it leaves the lamp already as wide as the panel (0: at the lamp).
REXCVAR_DEFINE_DOUBLE(pgr4_native_volumetric_light_cone_top, 0.17, "PGR4 native",
                      "Volumetric fog: the metres the lamps' cone's tip stands above the lamp")
    .range(0.0, 3.0);
REXCVAR_DEFINE_DOUBLE(pgr4_native_volumetric_light_cone_soft, 15.0, "PGR4 native",
                      "Volumetric fog: the degrees the cone's edge is softened over")
    .range(0.0, 60.0);
REXCVAR_DEFINE_DOUBLE(pgr4_native_volumetric_light_cone_around, 0.1, "PGR4 native",
                      "Volumetric fog: the share of the light outside the cone")
    .range(0.0, 1.0);
// 21/11/2026 (Tazz: "look on the internet how to make a nice volumetric with the light", then "do
// it"): the lamps' light was laid in the volume's cells (16 x 16 pixels, 64 slices): its cone's edge
// blurred over a cell, it slid between the cells' rays and slices as the eye moved. As the papers
// on lights in fog do (Sun et al. 2005; a cone: Geiger et al. 2023), each pixel's own ray now:
// each lamp's light integrated along it to the pixel's depth (the same 1 / (d2 + core2), its cone,
// its fade at the reach and its phase taken at eight parts of equal light), times the fog's
// extinction at the lamp (the game's, no banks) and the transmittance from the eye to it (the
// volume's). A lamp farther from the ray than its reach is skipped (needs a reach; with none the
// volume's cells, as before). Off: the cells, as before.
REXCVAR_DEFINE_BOOL(pgr4_native_volumetric_light_per_pixel, true, "PGR4 native",
                    "Volumetric fog: the lamps' light worked out on each pixel's ray (not the "
                    "volume's cells)");
// 21/11/2026 (Tazz, after the papers: "do both"): the fog's own texture in the lamps' beams: the
// per pixel light at its eight points thickened as a finer drift of the banks' noise there (its
// size in metres), times this much (0: none, the beams even).
REXCVAR_DEFINE_DOUBLE(pgr4_native_volumetric_light_texture, 0.3, "PGR4 native",
                      "Volumetric fog: the fog's texture in the lamps' beams (0: none)")
    .range(0.0, 1.0);
REXCVAR_DEFINE_DOUBLE(pgr4_native_volumetric_light_texture_size, 1.0, "PGR4 native",
                      "Volumetric fog: the size in metres of the texture in the lamps' beams")
    .range(0.5, 20.0);
// ... and the volume smoothed over time (Wronski 2014, Hillaire 2015): its fog (the banks'
// thickening, the ground mist, the shade) taken each frame at another place in each cell (a
// Halton sequence across the column and along the slice) and blended with the previous frames'
// at the same place in the world (the fog does not move with the cars: no trails), over this
// many seconds of real time (the same at any frame rate). 0: none, each frame at the cells'
// middles alone, as before. The game's own fog stays exact at the slices' edges.
REXCVAR_DEFINE_DOUBLE(pgr4_native_volumetric_temporal, 0.15, "PGR4 native",
                      "Volumetric fog: the seconds the volume's fog is smoothed over (0: none)")
    .range(0.0, 2.0);
// 21/11/2026 (Tazz: "add that", after the papers on light scattered many times, Hillaire,
// Premoze): around each lamp a wide soft halo besides its beam, the light that came back after
// many bounces in the fog: all around (no cone), e^(-d / size) of the distance to the lamp,
// integrated exactly along each pixel's ray (to its surface), times the fog at the lamp as the
// beam; this share of the lamp's light (0: none).
REXCVAR_DEFINE_DOUBLE(pgr4_native_volumetric_light_halo, 0.09, "PGR4 native",
                      "Volumetric fog: the wide soft halo around the lamps (0: none)")
    .range(0.0, 2.0);
REXCVAR_DEFINE_DOUBLE(pgr4_native_volumetric_light_halo_size, 4.0, "PGR4 native",
                      "Volumetric fog: the lamps' halo's size in metres")
    .range(0.5, 20.0);
// ... and the ground mist in wisps rather than sheets: a second, finer noise (its size in metres,
// flatter than wide, drifting faster than the banks) thickening and thinning the mist, this much
// (0: none, the mist as the banks alone).
REXCVAR_DEFINE_DOUBLE(pgr4_native_volumetric_mist_detail, 0.9, "PGR4 native",
                      "Volumetric fog: the ground mist's wisps (0: none)")
    .range(0.0, 1.0);
REXCVAR_DEFINE_DOUBLE(pgr4_native_volumetric_mist_detail_size, 2.0, "PGR4 native",
                      "Volumetric fog: the size in metres of the ground mist's wisps")
    .range(0.5, 20.0);
// Development (21/11/2026, Tazz: the trees behind showed through the parapet, only with the
// volumetric): the apply's values on one pixel of the screen (u v, 0..1 across and down; empty:
// none), every 2 s in the log: the depth read (each sample's with MSAA), its distance, the game's
// fog and ours there, the ratio kept and the light added. Live.
REXCVAR_DEFINE_STRING(pgr4_native_volumetric_probe_uv, "", "PGR4 native",
                      "Development: the volumetric apply's values on this pixel (\"u v\", 0..1)");
// Development (21/11/2026, Tazz, Shanghai's Heavy Fog with the fog laid by the shaders: regions
// lit then fogged again, safety barriers darker): 16 x 9 points of the screen every frame, the
// scene's colour as the shaders left it (before the apply) and the apply's values there (depth,
// the game's T, ours, the ratio, the light added, the volume's), read back two frames later;
// a point whose colour jumps between two frames on the same surface flagged, a frame with four or
// more such points logged with its values and the fog-laying draws before the volume in both.
// Development only (22/11/2026, Tazz: never in the release): it ran in every race with the volumetric
// fog, the release's too (its dispatch, copy and readback each frame); now only with the capture dir
// set (g_pgr4_dev, draw.cpp).
REXCVAR_DEFINE_BOOL(pgr4_native_volumetric_probe_grid, true, "PGR4 native",
                    "Development: the volumetric fog's 16 x 9 points probe, every frame, its flashes");
REXCVAR_DEFINE_BOOL(pgr4_native_volumetric_light_smooth, true, "PGR4 native",
                    "Volumetric fog: the lamps taken and dropped fade in and out (not cut)");
REXCVAR_DEFINE_INT32(pgr4_native_volumetric_light_count, 64, "PGR4 native",
                     "Volumetric fog: the nearest lamps taken (the others none)")
    .range(0, 64);
// 0.1 by default (Tazz, 21/11/2026: no glow towards the eye; it was 0.5).
REXCVAR_DEFINE_DOUBLE(pgr4_native_volumetric_light_phase, 0.0, "PGR4 native",
                      "Volumetric fog: the lamps' light scattered forward (Henyey-Greenstein's g)")
    .range(0.0, 0.95);
REXCVAR_DEFINE_DOUBLE(pgr4_native_volumetric_light_core, 0.8, "PGR4 native",
                      "Volumetric fog: the metres around a lamp its light is softened over (smaller: "
                      "a brighter, tighter glow at the bulb)")
    .range(0.2, 10.0);
REXCVAR_DEFINE_DOUBLE(pgr4_native_volumetric_light_red, 1.0, "PGR4 native",
                      "Volumetric fog: the lamps' colour, red")
    .range(0.0, 2.0);
REXCVAR_DEFINE_DOUBLE(pgr4_native_volumetric_light_green, 1.0, "PGR4 native",
                      "Volumetric fog: the lamps' colour, green")
    .range(0.0, 2.0);
REXCVAR_DEFINE_DOUBLE(pgr4_native_volumetric_light_blue, 1.0, "PGR4 native",
                      "Volumetric fog: the lamps' colour, blue")
    .range(0.0, 2.0);
// Local change (21/11/2026): the volume's rays over the scene's visible height (see Apply). On by
// default; off: over the whole target (the error, to compare).
REXCVAR_DEFINE_BOOL(pgr4_native_volumetric_visible_height, true, "PGR4 native",
                    "Volumetric fog: its rays over the scene's visible height (off: the whole "
                    "target, 1.42 times too tall)");
REXCVAR_DEFINE_BOOL(pgr4_native_volumetric_debug, false, "PGR4 native",
                    "Volumetric fog debug: the scene's color replaced by the light added alone");

bool Pgr4GtaoScreen();  // src/pgr4_menu.cpp: a race's, the garage's, a replay's or photo screen
// draw.cpp: the race's weather read at its load, no screen loaded since (the load and the intro's
// camera, before the race's first screen, 3-2-1-Go).
extern bool g_pgr4_race_loaded;
extern uint64_t g_pgr4_vol_ticks[8];  // draw.cpp: the race probe's CPU times (development)
extern bool g_pgr4_vol_timing;
extern bool g_pgr4_dev;  // draw.cpp: development (the capture dir set), never in the release
// Local change (22/11/2026, Tazz: "il doit se charger en premier pour éviter un effet moche pendant
// l'intro", as the water): the volume and the shaders' fog variants from the race's load, as the
// water's weather (pgr4_native_water_weather_at_load), not from its first screen (Screens\Ingame\
// 321Go.lua, 09:48:24 in the morning's log, with the intro drawn already): the intro was drawn with
// the game's fog, then the volumetric fog came at 3-2-1 with its fog variants made at once (a
// 370 ms frame, then some 36 ms ones).
REXCVAR_DEFINE_BOOL(pgr4_native_volumetric_at_load, true, "PGR4 native",
                    "Volumetric fog from the race's load (its intro included), not from its first "
                    "screen (3-2-1-Go)");
extern std::atomic<float> g_pgr4_sun_dir[3];    // draw.cpp: the sun's direction (PS c6)
extern std::atomic<float> g_pgr4_sun_color[3];  // draw.cpp: its color (PS c7)
extern std::atomic<int64_t> g_pgr4_sun_seen_ns;  // draw.cpp: when a scenery draw last read it
extern std::atomic<float> g_pgr4_fog_c10[4], g_pgr4_fog_c11[4], g_pgr4_fog_eye[3];  // draw.cpp
extern std::atomic<float> g_pgr4_fog_exp2;
extern std::atomic<float> g_pgr4_fog_c12y;  // draw.cpp: the fog shaders' scale after the fog
extern float g_pgr4_fog_clip[16];        // draw.cpp: the fog draws' camera (world to clip)
void Pgr4TakeFlareLights(std::vector<std::array<float, 11>>& out);  // draw.cpp: the flares, raw
extern uint64_t g_pgr4_fog_order_frame[8], g_pgr4_fog_order_before[8],
    g_pgr4_fog_order_after[8];  // draw.cpp: the fog-laying draws before / after the volume
extern uint64_t g_pgr4_fog_clip_frame;   // ... the frame it was seen in
extern uint64_t g_pgr4_fog_camera_stats[3];  // ... draws taken, not world to clip, mirrored
// Development (21/11/2026): the passes drawn into the scene so far, for draw.cpp's count of what the
// game draws after it.
std::atomic<uint64_t> g_pgr4_volumetrics_applies{0};
extern std::atomic<bool> g_pgr4_fog_height_on;
extern int g_pgr4_weather_kind;  // draw.cpp: the race's weather, -1 unknown, 0 clear .. 6
extern bool g_pgr4_race_screen;  // draw.cpp
extern bool g_pgr4_game_paused;  // draw.cpp: the game's clock stands still

// Local change (21/11/2026): draw.cpp's switches of the fog laid by each shader (InShader).
REXCVAR_DECLARE(bool, pgr4_native_volumetric_in_shader);
REXCVAR_DECLARE(int32_t, pgr4_native_volumetric_in_shader_mode);

namespace pgr4::native {

bool OriginalLook();  // renderer.cpp: the overlay's "the game without our changes"

namespace {

// The steps along each pixel's ray for the sun-lit air and towards the sun.
constexpr int kSteps = 24;
constexpr int kShadowSteps = 5;
// Descriptors per frame slot, five per compute pass (t0-t2, u0-u1): the march's depth, -, -,
// light, fog; the blur across's light, fog, -, light 2, fog 2; the blur down's light 2, fog 2, -,
// light, fog; then the apply draw's light, fog, depth.
// Then the noise's (made once) five, the volume's five and the apply with the volume's three.
// ... then the grid probe's colour reading (58-62): the scene's colour, -, -, -, the probe's buffer.
constexpr uint32_t kViewsPerSlot = 63;  // and the ground map's three passes' fifteen
// The grid probe (pgr4_native_volumetric_probe_grid): its points in the probe's buffer from
// kGridAt, kGridStride float4 each (kGridAt, kGridStride in the HLSL too).
constexpr uint32_t kGridX = 16, kGridY = 9, kGridAt = 8, kGridStride = 5;
constexpr uint32_t kGridBytes = kGridX * kGridY * kGridStride * 16;
constexpr uint32_t kProbeElements = kGridAt + kGridX * kGridY * kGridStride;
// The ground map's size and cell (kGroundSize, kGroundCell in the HLSL too).
constexpr uint32_t kGroundSize = 128;
constexpr float kGroundCell = 2.0f;
// The lights' buffer: a header and two float4 a light, up to kMaxLights, a slot a frame in flight.
constexpr uint32_t kMaxLights = 96;  // the count at most 64, the others fading out
// Nine headers (kLampsAt in the HLSL), then two per light: 0 count, range, strength, g; 1 reach,
// over the bulb, filter, nearest; 2 the cone; 3 per pixel, cone's tip, beams' texture, its size;
// 4-8 the previous frame's eye and axes, the blend and the jitter (the MEDIA pass); 9 the lamps'
// halo (its share, its size), the ground mist's wisps (their share, their size).
// 10: the apply probe's pixel (x, y in the scene's pixels, on).
constexpr uint32_t kLightHeaders = 11;
constexpr uint32_t kLightElements = kLightHeaders + 2 * kMaxLights;
// The volume's slices (kSlices in the HLSL too) and the noise's size.
constexpr uint32_t kSlices = 64;
constexpr uint32_t kNoiseSize = 128;

struct Constants {
  uint32_t size[2];       // the scene's host size
  uint32_t half_size[2];  // the march's size
  float near_plane, far_plane;
  float tan_half[2];      // the view's half extent at 1 m: x, y
  float sun_view[3];      // towards the sun, view space (x right, y up, z forward)
  float sigma;            // the sun-lit air's density at the ground, per metre
  float up_view[3];       // the world's x, y, z axes in view space
  float camera_height;    // the eye above the road, metres (an estimate: the chase view's)
  float sun_color[3];
  float strength;
  float max_distance;     // metres of sun-lit air, thinning out over the second half
  float height_scale;     // metres over which the sun-lit air thins by e
  float thickness;        // metres behind a surface still taken as inside it (the shadow test)
  float g;                // Henyey-Greenstein's asymmetry
  float east_view[3];
  uint32_t axis;          // the blur's: 0 across, 1 down
  float north_view[3];
  float time;             // seconds of the game's clock running (the banks' drift)
  float eye[3];           // the eye in the world (the scene shaders' c5)
  float fog_exp2;         // their exponent's scale (c254.y: 0.5 measured, T = exp2(-0.5 tau))
  float fog_color[3];     // c10.rgb
  float fog_distance;     // c10.w, per metre
  float fog_ceiling;      // c11.x
  float fog_density;      // c11.y
  float fog_falloff;      // c11.z
  uint32_t fog_height_on; // bool 158
  float variation;        // the banks' thickening and thinning, 0..1
  float noise_scale;      // 1 / the banks' size
  float fog_sun;          // the sun in the fog
  uint32_t replace;       // the game's fog taken out and laid again
  float wind[2];          // the banks' drift over the world's x and z, metres a second
  uint32_t steps;         // the steps along each ray for the sun-lit air
  uint32_t unused_fog_steps;  // (the per-pixel march's fog steps: removed 21/11/2026)
  uint32_t shadow_steps;  // ... towards the sun for each step's shadow
  float fog_g;            // the fog's Henyey-Greenstein asymmetry
  float mist_density;     // the ground mist's at its thickest, per metre
  float mist_height;      // its height above the road, metres
  float mist_distance;    // the metres it covers
  float vol_range;        // the volume's depth, metres
  float unused_relief;    // (the fog darker under thicker fog above: removed 21/11/2026)
  float unused_sky;       // (the banks before the sky: removed 21/11/2026)
  float ground_origin[2];  // the ground map's corner, world x and z (local change, 21/11/2026)
  uint32_t ground_on;      // the mist laid over the ground map (else a plane under the eye)
};
static_assert(sizeof(Constants) == 252);

constexpr char kConstantsHlsl[] = R"(
cbuffer C : register(b0) {
  uint2 size; uint2 half_size;
  float near_plane; float far_plane; float2 tan_half;
  float3 sun_view; float sigma;
  float3 up_view; float camera_height;
  float3 sun_color; float strength;
  float max_distance; float height_scale; float thickness; float g;
  float3 east_view; uint axis;
  float3 north_view; float time;
  float3 eye; float fog_exp2;
  float3 fog_color; float fog_distance;
  float fog_ceiling; float fog_density; float fog_falloff; uint fog_height_on;
  float variation; float noise_scale; float fog_sun; uint replace;
  float2 wind; uint steps; uint unused_fog_steps;
  uint shadow_steps; float fog_g; float mist_density; float mist_height;
  float mist_distance; float vol_range; float unused_relief; float unused_sky;
  float2 ground_origin; uint ground_on;
};
SamplerState linear_clamp : register(s0);
#ifdef MS
Texture2DMS<float> depth_tex : register(DEPTH_REGISTER);
float RawDepth(int2 p) { return depth_tex.Load(p, 0); }
#else
Texture2D<float> depth_tex : register(DEPTH_REGISTER);
float RawDepth(int2 p) { return depth_tex.Load(int3(p, 0)); }
#endif
// The inverted depth's view depth, metres.
float ViewDepth(float d) { return near_plane * far_plane / ((far_plane - near_plane) * d + near_plane); }
// The game's fog between the eye and a point (its scene shaders' own computation, line by line): the
// distance fog (c10.w per metre) and, with bool 158, the height fog: below the ceiling c11.x, c11.y
// times min(1, depth below it x c11.z) taken at the lower of the two ends, over the part of the way
// below the ceiling. Returns tau: T = exp2(-c254.y x tau) (c254.y measured 0.5 on 21/11/2026; the
// shader's distance r2.w is positive, its exponent -tau times c254.y).
float GameFogDepth(float distance, float point_height) {
  float tau = fog_distance * distance;
  if (fog_height_on != 0) {
    float p = point_height - fog_ceiling, e = eye.y - fog_ceiling;
    float lo = min(p, e), hi = max(p, e);
    float f = saturate(-lo * fog_falloff);
    float below = p < 0.0 ? distance : (lo < 0.0 ? distance * (-lo) / max(hi - lo, 1e-4) : 0.0);
    tau += fog_density * below * f;
  }
  return tau;
}
float HeightOf(float3 q) { return eye.y + dot(q, up_view); }
// The volume's slices (kSlices on the C++ side too) from the eye, their edges' depth growing from
// kVolumeNear; its columns 16 x 16 pixels.
static const uint kSlices = 64;
static const float kVolumeNear = 0.5;
// The lamps' buffer: ten headers, then two float4 a lamp (kLightHeaders on the C++ side).
static const uint kLampsAt = 11;
// The grid probe's points (kGridX x kGridY) in the probe's buffer from kGridAt, kGridStride float4
// each: the scene's colour (GRID), then the apply's four (the C++ side's constants too).
static const uint kGridX = 16, kGridY = 9, kGridAt = 8, kGridStride = 5;
int2 GridPixel(uint2 g) { return int2((float2(g) + 0.5) * float2(size) / float2(kGridX, kGridY)); }
uint2 VolumeColumns() { return (size + 15) / 16; }
// The ground map (GROUND_*): kGroundSize x kGroundSize cells of kGroundCell metres around the eye
// (kGroundSize, kGroundCell on the C++ side too).
static const uint kGroundSize = 128;
static const float kGroundCell = 2.0;
static const float kGroundUnknown = 1e9;
// The game's fog between the eye and depth z along a view ray (not normalised: z its view depth), in
// natural units: its scenes' T = exp2(-scale x tau) = exp(-this) (scale 1 / ln 2 read, 21/11/2026).
float GameTauNatural(float3 ray, float z) {
  return GameFogDepth(z * length(ray), HeightOf(ray * z)) * fog_exp2 * 0.69314718;
}
)";

constexpr char kComputeHlsl[] = R"(
#ifdef MARCH
RWTexture2D<float4> light_dst : register(u0);
RWTexture2D<float4> fog_dst : register(u1);
float SceneDepthAt(float2 uv) {
  int2 p = clamp(int2(uv * float2(size)), int2(0, 0), int2(size) - 1);
  return ViewDepth(RawDepth(p));
}
// Whether the sun is hidden from view-space point q: a few steps towards it (1.5 m, then each 2.2
// times further, up to about 70 m), each projected on the screen and found behind the scene's
// surface there. Off the screen or behind the eye: unknown, lit.
float SunVisible(float3 q) {
  float len = 1.5;
  [loop] for (uint i = 0; i < shadow_steps; ++i) {
    float3 s = q + sun_view * len;
    len *= 2.2;
    if (s.z <= near_plane) return 1.0;
    float2 ndc = s.xy / (s.z * tan_half);
    if (any(abs(ndc) >= 1.0)) return 1.0;
    float behind = s.z - SceneDepthAt(float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5));
    if (behind > 0.3 + 0.01 * s.z && behind < thickness + 0.1 * s.z) return 0.0;
  }
  return 1.0;
}
// Interleaved gradient noise (Jimenez 2014): each pixel's own offset of the steps, the same every
// frame (nothing accumulates it: the same at any frame rate), blurred away after.
float Noise(float2 p) { return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715)))); }
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= half_size)) return;
  int2 p = min(int2(id.xy) * 2, int2(size) - 1);
  float2 uv = (float2(p) + 0.5) / float2(size);
  float raw = RawDepth(p);
  float view_depth = ViewDepth(raw);
  float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
  float3 ray = float3(ndc * tan_half, 1.0);
  float ray_length = length(ray);
  float3 dir = ray / ray_length;
  float noise = Noise(float2(id.xy));
  float sky = raw <= 1e-6;
  float surface = sky ? 1e9 : view_depth * ray_length;
  // The sky (nothing drawn) keeps the game's own, its air already in it. fog_dst: nothing (the
  // per-pixel march's mist and banks, read by its own apply, removed on 21/11/2026).
  if (sky) {
    light_dst[id.xy] = float4(0.0, 0.0, 0.0, far_plane);
    fog_dst[id.xy] = float4(1.0, 1.0, 1.0, 0.0);
    return;
  }
  bool sun_on = dot(sun_color, 1.0) > 0.01;
  // 1. The sun-lit air: up to the overlay's distance, the steps denser near the eye.
  float air_distance = min(surface, max_distance);
  float lit = 0.0, transmittance = 1.0, previous = 0.0;
  if (sigma > 0.0 && sun_on) {
    [loop] for (uint k = 0; k < steps; ++k) {
      float f = (float(k) + noise) / float(steps);
      float t = air_distance * f * f;
      float dt = t - previous;
      previous = t;
      float3 q = dir * t;
      float height = max(dot(q, up_view) + camera_height, 0.0);
      float density = sigma * exp(-height / height_scale) *
                      saturate((max_distance - t) / (0.5 * max_distance));
      lit += transmittance * density * dt * SunVisible(q);
      transmittance *= exp(-density * dt);
    }
  }
  float c = dot(dir, sun_view);
  // Henyey-Greenstein (per steradian) in the game's units of light: its sun's color lights a matte
  // surface as albedo x color x cos (pi in it), so the air's radiance is a quarter of HG x 4 pi
  // times density x length x color (Tazz, 21/11/2026: four times this was far too much light).
  float phase = 0.25 * (1.0 - g * g) * pow(max(1.0 + g * g - 2.0 * g * c, 1e-4), -1.5);
  float3 light = sun_color * phase * lit;
  if (!all(isfinite(light))) light = 0.0;
  light_dst[id.xy] = float4(max(light, 0.0), min(surface / ray_length, far_plane));
  fog_dst[id.xy] = float4(1.0, 1.0, 1.0, 0.0);
}
#endif
#ifdef BLUR
Texture2D<float4> light_src : register(t0);
Texture2D<float4> fog_src : register(t1);
RWTexture2D<float4> light_dst : register(u0);
RWTexture2D<float4> fog_dst : register(u1);
// Across or down, 9 taps weighed by distance and by how close their depth is to the pixel's (the
// air before a car not spread onto the air behind it).
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= half_size)) return;
  float4 c = light_src[id.xy];
  float3 sum = c.rgb;
  float4 fog = fog_src[id.xy];
  float weights = 1.0;
  int2 step = axis == 0 ? int2(1, 0) : int2(0, 1);
  [unroll] for (int i = 1; i <= 4; ++i) {
    float gauss = exp(-float(i * i) / 8.0);
    [unroll] for (int s = -1; s <= 1; s += 2) {
      int2 q = clamp(int2(id.xy) + step * (i * s), int2(0, 0), int2(half_size) - 1);
      float4 v = light_src[q];
      float w = gauss * exp(-abs(v.a - c.a) / (0.05 * c.a + 0.5));
      sum += v.rgb * w;
      fog += fog_src[q] * w;
      weights += w;
    }
  }
  light_dst[id.xy] = float4(sum / weights, c.a);
  fog_dst[id.xy] = fog / weights;
}
#endif
#ifdef GROUND_CLEAR
// The ground map emptied (all ones: no height seen).
RWTexture2D<uint> ground_dst : register(u0);
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) { ground_dst[id.xy] = 0xFFFFFFFFu; }
#endif
#ifdef GROUND_SCATTER
// The ground map: each pixel of the scene (every other one across and down) put in its cell of
// the grid around the eye, the lowest height kept (InterlockedMin over the heights made orderable
// as uints): what is seen lowest in a cell is its ground, under the walls, the cars and the trees.
RWTexture2D<uint> ground_dst : register(u0);
uint OrderedHeight(float y) {
  uint u = asuint(y);
  return (u & 0x80000000u) != 0u ? ~u : (u | 0x80000000u);
}
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
  int2 p = int2(id.xy) * 2;
  if (any(p >= int2(size))) return;
  float raw = RawDepth(p);
  if (raw <= 1e-6) return;  // the sky
  float2 uv = (float2(p) + 0.5) / float2(size);
  float3 ray = float3(float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0) * tan_half, 1.0);
  float3 q = ray * ViewDepth(raw);
  float3 world = eye + float3(dot(q, east_view), dot(q, up_view), dot(q, north_view));
  int2 cell = int2(floor((world.xz - ground_origin) / kGroundCell));
  if (any(cell < 0) || any(cell >= int(kGroundSize))) return;
  InterlockedMin(ground_dst[cell], OrderedHeight(world.y));
}
#endif
#ifdef GROUND_FILL
// The cells seen as heights, the others the nearest seen within 4 cells' (kGroundUnknown beyond).
Texture2D<uint> ground_src : register(t1);
RWTexture2D<float> ground_dst : register(u0);
float HeightOf(uint u) { return asfloat((u & 0x80000000u) != 0u ? (u & 0x7FFFFFFFu) : ~u); }
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
  int2 c = int2(id.xy);
  uint v = ground_src[c];
  if (v != 0xFFFFFFFFu) {
    ground_dst[c] = HeightOf(v);
    return;
  }
  float best = 1e9, sum = 0.0, count = 0.0;
  [loop] for (int dy = -4; dy <= 4; ++dy) {
    [loop] for (int dx = -4; dx <= 4; ++dx) {
      int2 q = c + int2(dx, dy);
      if (any(q < 0) || any(q >= int(kGroundSize))) continue;
      uint u = ground_src[q];
      if (u == 0xFFFFFFFFu) continue;
      float d = float(dx * dx + dy * dy);
      if (d < best - 0.5) { best = d; sum = 0.0; count = 0.0; }
      if (d < best + 0.5) { sum += HeightOf(u); count += 1.0; }
    }
  }
  ground_dst[c] = count > 0.0 ? sum / count : kGroundUnknown;
}
#endif
#ifdef NOISE
// The banks' noise, made once: 128^3 texels of value noise over four octaves (8, 16, 32 and 64
// cells across, each wrapping: the texture tiles), stretched to spread over 0..1.
RWTexture3D<float> noise_dst : register(u0);
uint PcgHash(uint v) {
  uint s = v * 747796405u + 2891336453u;
  uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
  return (w >> 22u) ^ w;
}
float Lattice(int3 c, int period) {
  uint3 u = uint3((c % period + period) % period);
  return float(PcgHash(u.x + PcgHash(u.y + PcgHash(u.z + uint(period) * 7919u)))) *
         (1.0 / 4294967295.0);
}
float TiledNoise(float3 p, int period) {
  int3 i = int3(floor(p));
  float3 f = frac(p);
  f = f * f * (3.0 - 2.0 * f);
  return lerp(lerp(lerp(Lattice(i, period), Lattice(i + int3(1, 0, 0), period), f.x),
                   lerp(Lattice(i + int3(0, 1, 0), period), Lattice(i + int3(1, 1, 0), period), f.x),
                   f.y),
              lerp(lerp(Lattice(i + int3(0, 0, 1), period), Lattice(i + int3(1, 0, 1), period), f.x),
                   lerp(Lattice(i + int3(0, 1, 1), period), Lattice(i + int3(1, 1, 1), period), f.x),
                   f.y),
              f.z);
}
[numthreads(4, 4, 4)] void main(uint3 id : SV_DispatchThreadID) {
  if (any(id >= 128u)) return;
  float3 p = (float3(id) + 0.5) / 128.0;
  float n = 0.0, weights = 0.0, w = 0.5;
  [unroll] for (int o = 0; o < 4; ++o) {
    int period = 8 << o;
    n += w * TiledNoise(p * float(period), period);
    weights += w;
    w *= 0.5;
  }
  noise_dst[id] = saturate(0.5 + (n / weights - 0.5) * 2.5);
}
#endif
#if defined(VOLUME) || defined(MEDIA)
// The fog in volume: one thread per column of 16 x 16 pixels, kSlices slices from the eye to the
// range (their depth growing as it goes), each its own fog: the game's between its edges (exactly
// the game's T at each edge, the banks aside) times the banks' thickening there, plus the ground
// mist; lit from above (darker under thicker fog), its light and transmittance from the eye
// written at each slice. Nothing carried from frame to frame: the same at any frame rate.
#ifdef MEDIA
Texture3D<float> noise_tex : register(t1);
Texture2D<float> ground_tex : register(t2);
SamplerState linear_wrap : register(s1);
#else
// The cells' fog (MEDIA): the banks' thickening, the ground mist per metre, the shade.
Texture3D<float4> media_tex : register(t1);
#endif
// The lights (the game's flares, the nearest): t0, a header (count, range, strength, g) then two
// float4 a light (position, colour).
StructuredBuffer<float4> lights : register(t0);
// The lamps' light scattered towards the eye along a stretch of a ray, the eye at e, the ray's
// direction w (world, unit), from ta to tb metres, the fog there sigma per metre: each lamp's
// colour over its distance squared (softened over its core: 1 / (d2 + core2), core 2 m by default) integrated exactly
// along the stretch (21/11/2026, Tazz: the lamps flickered while driving: measured at one point a
// slice, 1.5 to 3 m long, against a light a few metres across, the slices slid over it), thinned by
// the fog from the lamp to the stretch's middle, fading out at the range, times Henyey-Greenstein
// (the light's way from the lamp against the way to the eye). Per metre of the stretch.
float3 LampLight(float3 e, float3 w, float ta, float tb, float sigma) {
  float4 h = lights[0];
  float reach = lights[1].x;  // 0: the old fade over the range (h.y)
  uint count = uint(h.x);
  float3 sum = 0.0;
  float tm = 0.5 * (ta + tb);
  // The cone (lights[2]): cos of its outer and inner half angles, the share outside, on.
  float4 cone_h = lights[2];
  [loop] for (uint i = 0; i < count; ++i) {
    float3 l = lights[kLampsAt + 2 * i].xyz - e;
    float t0 = dot(l, w);
    // w: the core's size squared; at least lights[1].z columns' width at the lamp's distance
    // (0 since 21/11/2026: finer, it twinkled between the columns' rays).
    float foot = lights[1].z * length(l) * 32.0 * tan_half.x / float(size.x);
    float k = sqrt(max(dot(l, l) - t0 * t0, 0.0) + lights[kLampsAt + 2 * i].w + foot * foot);
    float along = (atan((tb - t0) / k) - atan((ta - t0) / k)) / k;
    float3 m = w * tm - l;  // from the lamp to the stretch's middle
    float d = length(m);
    float c = dot(m / max(d, 1e-4), -w);
    float g = h.w;
    float phase = 0.25 * (1.0 - g * g) * pow(max(1.0 + g * g - 2.0 * g * c, 1e-4), -1.5);
    // The fade and the thinning at the stretch's point nearest the lamp (lights[1].w,
    // always since 21/11/2026), else at its middle.
    if (cone_h.w > 0.5) {
      // With the cone: the stretch cut in four parts of equal light (equal steps of the angle
      // atan((t - t0) / k), the integral's own variable), the cone, the fade and the thinning at
      // each part's middle: they follow the light, not the slices (no slice sliding over it).
      float ua = atan((ta - t0) / k), ub = atan((tb - t0) / k);
      float share = 0.0;
      [unroll] for (int s = 0; s < 4; ++s) {
        float ts = t0 + k * tan(lerp(ua, ub, (float(s) + 0.5) * 0.25));
        float3 v = w * ts - l;  // from the lamp to that point
        float dv = length(v);
        float3 from_tip = v - float3(0.0, lights[3].y, 0.0);  // from the cone's tip
        float down = -from_tip.y / max(length(from_tip), 1e-4);
        float in_cone = lerp(cone_h.z, 1.0, smoothstep(cone_h.x, cone_h.y, down));
        float xs = saturate(dv / max(reach, 1e-3));
        float ws = saturate(1.0 - xs * xs * xs * xs);
        float fs = reach > 0.0 ? ws * ws : saturate(1.0 - dv / h.y);
        share += 0.25 * in_cone * fs * exp(-sigma * dv);
      }
      sum += lights[kLampsAt + 1 + 2 * i].rgb * phase * share * along;
    } else {
      float dn = lights[1].w > 0.5 ? length(w * clamp(t0, ta, tb) - l) : d;
      float x = saturate(dn / max(reach, 1e-3));
      float window = saturate(1.0 - x * x * x * x);
      float fade = reach > 0.0 ? window * window : saturate(1.0 - dn / h.y);
      sum += lights[kLampsAt + 1 + 2 * i].rgb * phase * exp(-sigma * dn) * fade * along;
    }
  }
  return sum * h.z / max(tb - ta, 1e-3);
}

#ifdef MEDIA
RWTexture3D<float4> media_dst : register(u0);
RWTexture3D<float4> media_prev : register(u1);  // the previous frame's
#else
RWTexture3D<float4> volume_dst : register(u0);
// Local change (21/11/2026, the fog laid by each shader): the column's ours / the game's
// transmittance at the range, for the shaders' fog beyond it (draw.cpp kFogRange).
RWTexture2D<float> range_dst : register(u1);
#endif
float3 VolumeWorld(float3 q) {
  return eye + float3(dot(q, east_view), dot(q, up_view), dot(q, north_view));
}
#ifdef MEDIA
// The ground's height under world (x, z): the ground map's four cells around, those seen; the plane
// under the eye where none was (or the map off).
float GroundAt(float2 xz, float plane) {
  if (ground_on == 0) return plane;
  float2 c = (xz - ground_origin) / kGroundCell - 0.5;
  int2 c0 = int2(floor(c));
  float2 f = c - float2(c0);
  float sum = 0.0, weights = 0.0;
  [unroll] for (int k = 0; k < 4; ++k) {
    int2 o = int2(k & 1, k >> 1);
    int2 q = c0 + o;
    if (any(q < 0) || any(q >= int(kGroundSize))) continue;
    float h = ground_tex.Load(int3(q, 0));
    if (h >= 0.5 * kGroundUnknown) continue;
    float w = (o.x ? f.x : 1.0 - f.x) * (o.y ? f.y : 1.0 - f.y) + 1e-4;
    sum += h * w;
    weights += w;
  }
  return weights > 0.0 ? sum / weights : plane;
}
// The banks there, 0..1: the tile (8 bank sizes across) and its detail 3.7 times finer, drifting
// with the wind, taller than wide.
float BankNoise(float3 world) {
  float3 p = (world + float3(wind.x, 0.0, wind.y) * time) * (noise_scale * 0.125) *
             float3(1.0, 2.0, 1.0);
  float base = noise_tex.SampleLevel(linear_wrap, p, 0);
  float detail = noise_tex.SampleLevel(linear_wrap, p * 3.7 + float3(0.31, 0.57, 0.11), 0);
  return saturate(0.5 + (0.7 * base + 0.3 * detail - 0.5) * 1.3);
}
// The fog's thickening there: 1 on average, from 0 (holes) to 1 + 2 x variation.
float Thickening(float n) { return max(1.0 + 2.0 * variation * (2.0 * n - 1.0), 0.0); }
// The cells' fog (pgr4_native_volumetric_temporal): one thread a cell, its fog taken at the frame's
// jittered place in it (lights[7].w, lights[8].w across the column, lights[6].w along the slice;
// all 0.5 without smoothing: the middles, as before), then blended with the previous frame's at the
// same place in the world (lights[4] its eye, valid in w; lights[5-7] its axes, the blend in
// lights[5].w; lights[8] its view's half extents and range), read trilinear from the previous
// cells. Off the previous volume: this frame's alone.
float4 MediaPrev(float3 c) {
  uint3 dims = uint3(VolumeColumns(), kSlices);
  float3 b = floor(c);
  float3 f = c - b;
  float4 sum = 0.0;
  [unroll] for (int k = 0; k < 8; ++k) {
    int3 o = int3(k & 1, (k >> 1) & 1, k >> 2);
    int3 q = clamp(int3(b) + o, int3(0, 0, 0), int3(dims) - 1);
    float w = (o.x ? f.x : 1.0 - f.x) * (o.y ? f.y : 1.0 - f.y) * (o.z ? f.z : 1.0 - f.z);
    sum += media_prev[uint3(q)] * w;
  }
  return sum;
}
[numthreads(4, 4, 4)] void main(uint3 id : SV_DispatchThreadID) {
  uint2 columns = VolumeColumns();
  if (any(id.xy >= columns) || id.z >= kSlices) return;
  float growth = log(vol_range / kVolumeNear) / float(kSlices);
  uint k = id.z;
  float z0 = k == 0 ? 0.0 : kVolumeNear * exp(growth * float(k));
  float z1 = kVolumeNear * exp(growth * float(k + 1));
  float2 jitter = float2(lights[7].w, lights[8].w);
  float2 uv = (float2(id.xy) * 16.0 + 16.0 * jitter) / float2(size);
  float3 ray = float3(float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0) * tan_half, 1.0);
  float ray_length = length(ray);
  float zs = lerp(z0, z1, lights[6].w);
  float3 world = VolumeWorld(ray * zs);
  float n = BankNoise(world);
  float mist_rate = 0.0;
  if (mist_density > 0.0) {
    float t = zs * ray_length;
    float top = mist_height * (0.5 + n);
    float ground = GroundAt(world.xz, eye.y - camera_height);
    mist_rate = mist_density * saturate((n - 0.35) * 2.5) * saturate(1.0 - (world.y - ground) / top) *
                saturate((mist_distance - t) / (0.5 * mist_distance));
    // Its wisps (lights[9].z their share, lights[9].w their size): two octaves of the noise,
    // three times flatter than wide, drifting 1.5 times as fast as the banks.
    if (mist_rate > 0.0 && lights[9].z > 0.0) {
      float3 p = (world + float3(wind.x, 0.0, wind.y) * (1.5 * time)) *
                 (0.125 / max(lights[9].w, 0.1)) * float3(1.0, 3.0, 1.0);
      float m = 0.6 * noise_tex.SampleLevel(linear_wrap, p, 0) +
                0.4 * noise_tex.SampleLevel(linear_wrap, p * 2.3 + float3(0.21, 0.67, 0.43), 0);
      mist_rate *= max(1.0 + 2.0 * lights[9].z * (2.0 * saturate(0.5 + (m - 0.5) * 1.8) - 1.0), 0.0);
    }
  }
  // Lit from above: darker under thicker fog (the banks 3 and 8 m higher), lighter under holes.
  float shade = 1.0;  // even (the relief under thicker fog above: removed 21/11/2026, Tazz)
  float4 now = float4(Thickening(n), mist_rate, shade, 1.0);
  float blend = lights[5].w;
  if (lights[4].w > 0.5 && blend < 1.0) {
    // The cell's middle in the previous frame's volume.
    float2 uvc = (float2(id.xy) * 16.0 + 8.0) / float2(size);
    float3 rayc = float3(float2(uvc.x * 2.0 - 1.0, 1.0 - uvc.y * 2.0) * tan_half, 1.0);
    float3 d = VolumeWorld(rayc * (0.5 * (z0 + z1))) - lights[4].xyz;
    float3 q = lights[5].xyz * d.x + lights[6].xyz * d.y + lights[7].xyz * d.z;
    if (q.z > kVolumeNear) {
      float2 ndc = q.xy / (q.z * lights[8].xy);
      float2 puv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
      float pgrowth = log(lights[8].z / kVolumeNear) / float(kSlices);
      float3 c = float3(puv * float2(size) / 16.0 - 0.5, log(q.z / kVolumeNear) / pgrowth - 0.5);
      if (all(c >= -0.5) && all(c.xy <= float2(columns) - 0.5) && c.z <= float(kSlices) - 0.5)
        now = lerp(MediaPrev(c), now, blend);
    }
  }
  media_dst[id] = now;
}
#endif
#ifndef MEDIA
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
  uint2 columns = VolumeColumns();
  if (any(id.xy >= columns)) return;
  float2 uv = (float2(id.xy) * 16.0 + 8.0) / float2(size);
  float3 ray = float3(float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0) * tan_half, 1.0);
  float ray_length = length(ray);
  float c = dot(ray / ray_length, sun_view);
  float phase = 0.25 * (1.0 - fog_g * fog_g) *
                pow(max(1.0 + fog_g * fog_g - 2.0 * fog_g * c, 1e-4), -1.5);
  float3 sun_light = sun_color * phase * fog_sun;
  float road = eye.y - camera_height;
  float growth = log(vol_range / kVolumeNear) / float(kSlices);
  float3 scattered = 0.0;
  float transmittance = 1.0, game_tau = 0.0, z0 = 0.0;
  [loop] for (uint k = 0; k < kSlices; ++k) {
    float z1 = kVolumeNear * exp(growth * float(k + 1));
    float zc = 0.5 * (z0 + z1);
    float len = (z1 - z0) * ray_length;
    // The cell's fog (MEDIA): the banks' thickening, the ground mist per metre, the shade.
    float4 media = media_tex.Load(int4(id.xy, k, 0));
    float tau = 0.0;
    if (replace != 0) {
      float tau1 = GameTauNatural(ray, z1);
      tau = max(tau1 - game_tau, 0.0) * media.x;
      game_tau = max(game_tau, tau1);
    }
    tau += media.y * len;
    float shade = media.z;
    float a = 1.0 - exp(-tau);
    float3 w_dir = VolumeWorld(ray / ray_length) - eye;
    // Per pixel (the apply): none here. A branch, not ?: (HLSL works out both sides of ?:, the
    // lamps cost 0.63 ms here for nothing, measured 21/11/2026).
    float3 lamps = 0.0;
    [branch] if (lights[3].x < 0.5)
      lamps = LampLight(eye, w_dir, z0 * ray_length, z1 * ray_length, tau / max(len, 1e-3));
    scattered += transmittance * a * (fog_color * shade + sun_light + lamps);
    transmittance *= 1.0 - a;
    volume_dst[uint3(id.xy, k)] = float4(scattered, transmittance);
    z0 = z1;
  }
  range_dst[id.xy] = replace != 0 ? transmittance / max(exp(-game_tau), 1e-6) : 1.0;
}
#endif
#endif
#ifdef GRID
// The grid probe: the scene's colour at its points, as the shaders left it (before the apply).
#ifdef MS
Texture2DMS<float4> grid_colour : register(t0);
#else
Texture2D<float4> grid_colour : register(t0);
#endif
RWStructuredBuffer<float4> grid_probe : register(u1);
[numthreads(16, 9, 1)] void main(uint3 id : SV_GroupThreadID) {
  int2 p = GridPixel(id.xy);
#ifdef MS
  float4 colour = grid_colour.Load(p, 0);
#else
  float4 colour = grid_colour.Load(int3(p, 0));
#endif
  grid_probe[kGridAt + (id.y * kGridX + id.x) * kGridStride] = colour;
}
#endif
)";

constexpr char kApplyHlsl[] = R"(
Texture2D<float4> light_tex : register(t0);
Texture3D<float4> volume_tex : register(t1);
StructuredBuffer<float4> lights : register(t3);  // the VOLUME pass's lamps (LampLight)
Texture3D<float> noise_tex : register(t4);        // the banks' noise (the beams' texture)
RWStructuredBuffer<float4> probe : register(u1);  // the probe's pixel's values (lights[10])
SamplerState linear_wrap : register(s1);
// The fog's texture in the lamps' beams (lights[3].z its share, lights[3].w its size in metres):
// the banks' noise, two octaves, drifting with the wind, 1 on average.
float BeamTexture(float3 world) {
  float3 p = (world + float3(wind.x, 0.0, wind.y) * time) * (0.125 / max(lights[3].w, 0.1)) *
             float3(1.0, 1.5, 1.0);
  float n = 0.6 * noise_tex.SampleLevel(linear_wrap, p, 0) +
            0.4 * noise_tex.SampleLevel(linear_wrap, p * 2.7 + float3(0.37, 0.11, 0.73), 0);
  return max(1.0 + 2.0 * lights[3].z * (2.0 * saturate(0.5 + (n - 0.5) * 1.6) - 1.0), 0.0);
}
void vs(uint id : SV_VertexID, out float4 pos : SV_Position) {
  float2 uv = float2((id << 1) & 2, id & 2);
  pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
// The volume (VOLUME): its light and its transmittance from the eye to the pixel's depth (the slices'
// edges spaced as the depth grows: edge j at kVolumeNear x e^(growth j), slice k up to edge k + 1),
// the game's fog beyond its range as the game has it.
float4 VolumeAt(float2 pixel, float z) {
  float growth = log(vol_range / kVolumeNear) / float(kSlices);
  float2 uv = pixel / (16.0 * float2(VolumeColumns()));
  float e1 = kVolumeNear * exp(growth);
  if (z < e1)
    return lerp(float4(0.0, 0.0, 0.0, 1.0),
                volume_tex.SampleLevel(linear_clamp, float3(uv, 0.5 / float(kSlices)), 0), z / e1);
  float j = min(log(z / kVolumeNear) / growth, float(kSlices));
  return volume_tex.SampleLevel(linear_clamp, float3(uv, (j - 0.5) / float(kSlices)), 0);
}
// color = color x a + rgb. The game's fog in the scene's color was fog + (surface - fog) x Tg; ours,
// light L and transmittance To from the volume: color x To / Tg + L - fog x (1 - Tg) x To / Tg. The
// ratio at most 1 + 2 Tg (where the game's fog hides the surface it cannot come back: a white ghost
// of the far buildings, 21/11/2026), at most 1 on the sky (its own air already in it: thickened
// where the banks are, never cleared), L then scaled to the To kept.
float4 ps(float4 pos : SV_Position) : SV_Target {
  float raw = RawDepth(int2(pos.xy));
  bool sky = raw <= 1e-6;
  float2 uv = pos.xy / float2(size);
  float3 ray = float3(float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0) * tan_half, 1.0);
  float depth = sky ? 0.0 : ViewDepth(raw);  // the sky: no banks before it (Tazz, 21/11/2026)
  float4 v = VolumeAt(pos.xy, min(depth, vol_range));
  float3 scattered = v.rgb;
  // A lamp's glow over its own head (pgr4_native_volumetric_light_over_bulb): the surface within a
  // lamp's core (its flare's place, the core's half fully, out to the core smoothly), the light
  // gathered to the lamp's far side (its distance along the ray plus the core) instead of the
  // surface: the head no longer cuts the glow behind it. The transmittance stays the surface's.
  if (!sky && lights[1].y > 0.5 && lights[3].x < 0.5) {
    float rl = length(ray);
    float3 wd = float3(dot(ray, east_view), dot(ray, up_view), dot(ray, north_view)) / rl;
    float t_hit = depth * rl;
    float3 p = eye + wd * t_hit;
    uint count = uint(lights[0].x);
    float best = 0.0, far_t = 0.0;
    [loop] for (uint i = 0; i < count; ++i) {
      float4 l = lights[kLampsAt + 2 * i];
      float core = sqrt(l.w);
      float wgt = 1.0 - smoothstep(0.5 * core, core, distance(p, l.xyz));
      if (wgt > best) {
        best = wgt;
        far_t = dot(l.xyz - eye, wd) + core;
      }
    }
    if (best > 0.0 && far_t > t_hit) {
      float4 behind = VolumeAt(pos.xy, min(far_t / rl, vol_range));
      scattered = lerp(scattered, max(behind.rgb, scattered), best);
    }
  }
  float transmittance = v.a;
  float tg = 1.0;
  if (replace != 0) {
    float tau = GameTauNatural(ray, depth);
    tg = exp(-tau);
    if (depth > vol_range) {
      float rest = exp(-max(tau - GameTauNatural(ray, vol_range), 0.0));
      scattered += transmittance * fog_color * (1.0 - rest);
      transmittance *= rest;
    }
  }
  float ratio = min(transmittance / max(tg, 1e-6), sky ? 1.0 : 1.0 + 2.0 * tg);
  float to = tg * ratio;
  if (to < transmittance) scattered *= (1.0 - to) / max(1.0 - transmittance, 1e-4);
  float3 add = scattered - fog_color * (1.0 - tg) * ratio;
  float keep = ratio;
  // Local change (21/11/2026): replace 2, the fog laid by each shader at the game's fog ending
  // (pgr4_native_volumetric_in_shader): nothing of it here.
  if (replace == 2) {
    add = 0.0;
    keep = 1.0;
  }
  // The lamps on this pixel's ray (pgr4_native_volumetric_light_per_pixel, lights[3].x): each lamp
  // within its reach of the ray, from where the ray enters its reach to where it leaves it or meets
  // the surface (over its own head, pgr4_native_volumetric_light_over_bulb: to its far side), its
  // light 1 / (d2 + core2) integrated exactly along, the cone, the fade at the reach and the phase
  // at the middles of eight parts of equal light (equal steps of atan((t - t0) / k)); times the
  // game's fog extinction at the lamp (no banks: they made it come and go) and the volume's
  // transmittance from the eye to it, the lamps' strength (with c12.y) and colour.
  if (lights[3].x > 0.5) {
    float rl = length(ray);
    float3 wd = float3(dot(ray, east_view), dot(ray, up_view), dot(ray, north_view)) / rl;
    float t_hit = (sky ? vol_range : depth) * rl;
    float3 p_hit = eye + wd * t_hit;
    float4 lh = lights[0];
    float reach = lights[1].x;
    float4 cone_h = lights[2];
    float lg = lh.w;
    uint count = uint(lh.x);
    float3 lamp_light = 0.0;
    [loop] for (uint i = 0; i < count; ++i) {
      float4 lp = lights[kLampsAt + 2 * i];
      float3 l = lp.xyz - eye;
      float t0 = dot(l, wd);
      float h2 = max(dot(l, l) - t0 * t0, 0.0);
      // The halo (lights[9].x its share, lights[9].y its size): out to four sizes from the ray.
      float halo_size = lights[9].y;
      bool halo = lights[9].x > 0.0 && h2 < 16.0 * halo_size * halo_size;
      if (h2 >= reach * reach && !halo) continue;
      float t_end = t_hit;
      if (!sky && lights[1].y > 0.5) {
        float core = sqrt(lp.w);
        float on_head = 1.0 - smoothstep(0.5 * core, core, distance(p_hit, lp.xyz));
        t_end = lerp(t_hit, max(t_hit, t0 + max(reach, halo ? 4.0 * halo_size : 0.0)), on_head);
      }
      [branch] if (halo) {
        // e^(-d / size), d the distance to the lamp, along the ray from the eye to t_end: the
        // part off the ray e^(-h / size), along it the integral of e^(-|s| / size) exactly,
        // over size squared (per metre squared, as the beam's 1 / d2); the fog at the lamp.
        float hz = clamp(t0, 0.0, t_end) / rl;
        float halo_sigma = replace != 0
                               ? max(GameTauNatural(ray, hz + 1.0) - GameTauNatural(ray, max(hz - 1.0, 0.0)),
                                     0.0) / ((hz + 1.0 - max(hz - 1.0, 0.0)) * rl)
                               : 0.0;
        float sa = -t0, sb = t_end - t0;
        float fa = sign(sa) * halo_size * (1.0 - exp(-abs(sa) / halo_size));
        float fb = sign(sb) * halo_size * (1.0 - exp(-abs(sb) / halo_size));
        float along_halo = exp(-sqrt(h2) / halo_size) * (fb - fa) / (halo_size * halo_size);
        lamp_light += lights[kLampsAt + 1 + 2 * i].rgb *
                      (lights[9].x * 0.25 * along_halo * halo_sigma *
                       VolumeAt(pos.xy, min(hz, vol_range)).a);
      }
      if (h2 >= reach * reach) continue;
      float half_chord = sqrt(reach * reach - h2);
      float ta = max(t0 - half_chord, 0.0), tb = min(t0 + half_chord, t_end);
      if (tb <= ta) continue;
      float k = sqrt(h2 + lp.w);
      float ua = atan((ta - t0) / k), ub = atan((tb - t0) / k);
      // The fog's extinction at the lamp (the game's, per metre) and the way to it.
      float tl = clamp(t0, ta, tb);
      float zl = tl / rl;
      float lamp_sigma = replace != 0
                             ? max(GameTauNatural(ray, zl + 1.0) - GameTauNatural(ray, max(zl - 1.0, 0.0)),
                                   0.0) / ((zl + 1.0 - max(zl - 1.0, 0.0)) * rl)
                             : 0.0;
      float to_lamp = VolumeAt(pos.xy, min(zl, vol_range)).a;
      float share = 0.0;
      [unroll] for (int s = 0; s < 8; ++s) {
        float ts = t0 + k * tan(lerp(ua, ub, (float(s) + 0.5) * 0.125));
        float3 v = wd * ts - l;  // from the lamp to that point
        float dv = length(v);
        float3 vn = v / max(dv, 1e-4);
        float3 from_tip = v - float3(0.0, lights[3].y, 0.0);  // from the cone's tip
        float down = -from_tip.y / max(length(from_tip), 1e-4);
        float in_cone = cone_h.w > 0.5 ? lerp(cone_h.z, 1.0, smoothstep(cone_h.x, cone_h.y, down))
                                       : 1.0;
        float x = saturate(dv / reach);
        float window = saturate(1.0 - x * x * x * x);
        float c = dot(vn, -wd);
        float phase = 0.25 * (1.0 - lg * lg) * pow(max(1.0 + lg * lg - 2.0 * lg * c, 1e-4), -1.5);
        float beam_tex = 1.0;
        [branch] if (lights[3].z > 0.0) beam_tex = BeamTexture(eye + wd * ts);
        share += 0.125 * in_cone * window * window * phase * beam_tex * exp(-lamp_sigma * dv);
      }
      lamp_light += lights[kLampsAt + 1 + 2 * i].rgb * share * ((ub - ua) / k) * lamp_sigma * to_lamp;
    }
    add += lamp_light * lh.z;
  }
  // The sun-lit air (MARCH), on what is drawn only.
  if (sigma > 0.0 && !sky) {
    float2 t = pos.xy * 0.5 - 0.5;
    int2 q0 = int2(floor(t));
    float2 f = t - float2(q0);
    float3 light = 0.0;
    float weights = 0.0;
    [unroll] for (int k = 0; k < 4; ++k) {
      int2 o = int2(k & 1, k >> 1);
      int2 q = clamp(q0 + o, int2(0, 0), int2(half_size) - 1);
      float4 l = light_tex.Load(int3(q, 0));
      float w = (o.x ? f.x : 1.0 - f.x) * (o.y ? f.y : 1.0 - f.y);
      w *= exp(-abs(l.a - min(depth, far_plane)) / (0.05 * depth + 0.5)) + 1e-3;
      light += l.rgb * w;
      weights += w;
    }
    add = add + strength * light / max(weights, 1e-6) * keep;
  }
  // The probe (pgr4_native_volumetric_probe_uv): this pixel's values.
  if ((uint(lights[10].z) & 1u) != 0u && all(int2(pos.xy) == int2(lights[10].xy))) {
    float samples = 1.0;
    float4 each = float4(raw, raw, raw, raw);
#ifdef MS
    uint dw, dh, ds;
    depth_tex.GetDimensions(dw, dh, ds);
    samples = float(ds);
    [unroll] for (uint k = 0; k < 4; ++k)
      if (k < ds) each[k] = depth_tex.Load(int2(pos.xy), k);
#endif
    probe[0] = float4(raw, depth, sky ? 1.0 : 0.0, samples);
    probe[1] = float4(tg, transmittance, ratio, keep);
    probe[2] = float4(add, v.a);
    probe[3] = each;
    probe[4] = float4(v.rgb, length(ray) * depth);
  }
  // The grid probe (pgr4_native_volumetric_probe_grid, lights[10].z bit 1): at its points, the
  // depth, the game's T and ours, the ratio, keep, the light added (with replace 2: the lamps and
  // the sun-lit air only), the volume's transmittance, its light as laid (scaled) and To.
  if ((uint(lights[10].z) & 2u) != 0u) {
    uint2 g = min(uint2(pos.xy * float2(kGridX, kGridY) / float2(size)), uint2(kGridX - 1, kGridY - 1));
    if (all(int2(pos.xy) == GridPixel(g))) {
      uint at = kGridAt + (g.y * kGridX + g.x) * kGridStride;
      probe[at + 1] = float4(raw, depth, sky ? 1.0 : 0.0, float(replace));
      probe[at + 2] = float4(tg, transmittance, ratio, keep);
      probe[at + 3] = float4(add, v.a);
      probe[at + 4] = float4(scattered, to);
    }
  }
  if (!all(isfinite(add)) || !isfinite(keep)) return float4(0.0, 0.0, 0.0, 1.0);
#ifdef DEBUG
  return float4(max(add, 0.0), 0.0);
#else
  return float4(add, keep);
#endif
}
)";

// The camera's center: the world point whose clip x, y and w are all 0 (rows 0, 1 and 3 of the clip
// transform, each (a, b, c, d): a x + b y + c z + d = 0), by Cramer's rule. False if none.
bool CameraCenter(const float* m, float out[3]) {
  const float* r[3] = {m, m + 4, m + 12};
  double a[3][3], b[3];
  for (int i = 0; i < 3; ++i) {
    for (int k = 0; k < 3; ++k) a[i][k] = r[i][k];
    b[i] = -double(r[i][3]);
  }
  const auto det = [](const double x[3][3]) {
    return x[0][0] * (x[1][1] * x[2][2] - x[1][2] * x[2][1]) -
           x[0][1] * (x[1][0] * x[2][2] - x[1][2] * x[2][0]) +
           x[0][2] * (x[1][0] * x[2][1] - x[1][1] * x[2][0]);
  };
  const double d = det(a);
  if (!(std::abs(d) > 1e-12)) return false;
  for (int k = 0; k < 3; ++k) {
    double t[3][3];
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j) t[i][j] = j == k ? b[i] : a[i][j];
    out[k] = float(det(t) / d);
    if (!std::isfinite(out[k])) return false;
  }
  return true;
}

// The direction (x, y, z, 0) through the clip transform's rows, then into view space: the clip's x
// and y are the view's over the half extents, its w the view's depth. Normalised; false if none.
bool ViewDirection(const float* m, const float d[3], const float tan_half[2], float out[3]) {
  float clip[4];
  for (int r = 0; r < 4; ++r) clip[r] = m[4 * r] * d[0] + m[4 * r + 1] * d[1] + m[4 * r + 2] * d[2];
  const float v[3] = {clip[0] * tan_half[0], clip[1] * tan_half[1], clip[3]};
  const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (!(l > 1e-6f)) return false;
  for (int k = 0; k < 3; ++k) out[k] = v[k] / l;
  return true;
}

}  // namespace

// Local change (21/11/2026): the fog laid by each shader this frame (draw.cpp's switch and mode 2).
bool Volumetrics::InShader() {
  return REXCVAR_GET(pgr4_native_volumetric_in_shader) &&
         REXCVAR_GET(pgr4_native_volumetric_in_shader_mode) == 2;
}

bool Volumetrics::Enabled() {
  return REXCVAR_GET(pgr4_native_volumetric_effects) && REXCVAR_GET(pgr4_native_volumetric) &&
         (::Pgr4GtaoScreen() || (REXCVAR_GET(pgr4_native_volumetric_at_load) && g_pgr4_race_loaded)) &&
         !OriginalLook();
}

bool Volumetrics::Initialize(ID3D12Device* device) {
  device_ = device;
  const pD3DCompile compile = D3DCompiler();
  if (!compile) return false;
  D3D12_STATIC_SAMPLER_DESC sampler = {};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  // Compute: t0-t2 and u0-u1, then the constants; the apply draw: t0-t2, then the constants.
  {
    D3D12_DESCRIPTOR_RANGE ranges[2] = {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 3},
    };
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable = {2, ranges};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants = {0, 0, sizeof(Constants) / 4};
    // s0 clamped, s1 wrapping (the volume's noise, which tiles).
    D3D12_STATIC_SAMPLER_DESC samplers[2] = {sampler, sampler};
    samplers[1].ShaderRegister = 1;
    samplers[1].AddressU = samplers[1].AddressV = samplers[1].AddressW =
        D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    const D3D12_ROOT_SIGNATURE_DESC rs = {2, params, 2, samplers, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, error;
    if (Failed(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error),
               "volumetrics root signature serialize") ||
        Failed(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                           IID_PPV_ARGS(&compute_signature_)),
               "volumetrics root signature"))
      return false;
  }
  {
    // t0-t4: t3 the lamps, t4 the banks' noise (the apply with the volume; null views for the other).
    // ... then u1 the probe's buffer (pgr4_native_volumetric_probe_uv).
    D3D12_DESCRIPTOR_RANGE apply_ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 6, 0, 0, 0},
                                              {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1, 0, 6}};
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable = {2, apply_ranges};
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants = {0, 0, sizeof(Constants) / 4};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC apply_samplers[2] = {sampler, sampler};
    apply_samplers[1].ShaderRegister = 1;  // s1 wrapping (the beams' texture)
    apply_samplers[1].AddressU = apply_samplers[1].AddressV = apply_samplers[1].AddressW =
        D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    const D3D12_ROOT_SIGNATURE_DESC rs = {2, params, 2, apply_samplers, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, error;
    if (Failed(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error),
               "volumetrics apply root signature serialize") ||
        Failed(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                           IID_PPV_ARGS(&apply_signature_)),
               "volumetrics apply root signature"))
      return false;
  }
  const std::string steps = std::to_string(kSteps), shadow_steps = std::to_string(kShadowSteps);
  const std::string compute_text = std::string(kConstantsHlsl) + kComputeHlsl;
  const std::string apply_text = std::string(kConstantsHlsl) + kApplyHlsl;
  auto cs = [&](const char* name, const char* pass, bool ms, ComPtr<ID3D12PipelineState>& out) {
    const D3D_SHADER_MACRO macros[] = {{pass, "1"},
                                       {"STEPS", steps.c_str()},
                                       {"SHADOW_STEPS", shadow_steps.c_str()},
                                       {"DEPTH_REGISTER", !std::strcmp(pass, "BLUR") ||
                                                                  !std::strcmp(pass, "GRID")
                                                              ? "t2"
                                                          : !std::strcmp(pass, "VOLUME") ? "t9"
                                                          : !std::strcmp(pass, "MEDIA")  ? "t9"
                                                                                         : "t0"},
                                       {ms ? "MS" : "NOT_MS", "1"},
                                       {nullptr, nullptr}};
    ComPtr<ID3DBlob> code, err;
    if (FAILED(compile(compute_text.data(), compute_text.size(), name, macros, nullptr, "main",
                       "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err))) {
      REXLOG_ERROR("PGR4 native volumetrics: {}: {}", name,
                   err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
      return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC d = {};
    d.pRootSignature = compute_signature_.Get();
    d.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    return !Failed(device->CreateComputePipelineState(&d, IID_PPV_ARGS(&out)), name);
  };
  // The blur reads no depth: its texture is declared, unused, at t2.
  if (!cs("pgr4_volumetric_march.hlsl", "MARCH", false, march_pso_) ||
      !cs("pgr4_volumetric_march_ms.hlsl", "MARCH", true, march_ms_pso_) ||
      !cs("pgr4_volumetric_blur.hlsl", "BLUR", false, blur_pso_) ||
      !cs("pgr4_volumetric_noise.hlsl", "NOISE", false, noise_pso_) ||
      !cs("pgr4_volumetric_volume.hlsl", "VOLUME", false, volume_pso_) ||
      !cs("pgr4_volumetric_media.hlsl", "MEDIA", false, media_pso_) ||
      !cs("pgr4_volumetric_ground_clear.hlsl", "GROUND_CLEAR", false, ground_clear_pso_) ||
      !cs("pgr4_volumetric_ground_scatter.hlsl", "GROUND_SCATTER", false, ground_scatter_pso_) ||
      !cs("pgr4_volumetric_ground_scatter_ms.hlsl", "GROUND_SCATTER", true, ground_scatter_ms_pso_) ||
      !cs("pgr4_volumetric_ground_fill.hlsl", "GROUND_FILL", false, ground_fill_pso_) ||
      !cs("pgr4_volumetric_grid.hlsl", "GRID", false, grid_pso_) ||
      !cs("pgr4_volumetric_grid_ms.hlsl", "GRID", true, grid_ms_pso_))
    return false;
  ComPtr<ID3DBlob> err;
  const auto apply = [&](const char* name, const char* entry, const char* profile, bool ms,
                         bool debug, ComPtr<ID3DBlob>& out) {
    const D3D_SHADER_MACRO macros[] = {{"DEPTH_REGISTER", "t2"},
                                       {ms ? "MS" : "NOT_MS", "1"},
                                       {debug ? "DEBUG" : "NOT_DEBUG", "1"},
                                       {nullptr, nullptr}};
    if (SUCCEEDED(compile(apply_text.data(), apply_text.size(), name, macros, nullptr, entry,
                          profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out, &err)))
      return true;
    REXLOG_ERROR("PGR4 native volumetrics: {}: {}", name,
                 err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
    return false;
  };
  // The apply: the volume's only (the per-pixel march's own apply removed on 21/11/2026).
  if (!apply("pgr4_volumetric_apply.hlsl", "vs", "vs_5_0", false, false, apply_vs_)) return false;
  for (int i = 0; i < 4; ++i)
    if (!apply("pgr4_volumetric_apply_volume.hlsl", "ps", "ps_5_0", (i & 2) != 0, (i & 1) != 0,
               apply_froxel_ps_[i]))
      return false;
  D3D12_DESCRIPTOR_HEAP_DESC hd = {};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  hd.NumDescriptors = kViewsPerSlot * D3D12Device::kFramesInFlight;
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (Failed(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_)), "volumetrics heap"))
    return false;
  stride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  ready_ = true;
  REXLOG_INFO("PGR4 native volumetrics: ready");
  return true;
}

// A resource some frame in flight may still use: kept until the GPU is surely past it, then freed.
void Volumetrics::Retire(ComPtr<ID3D12Resource>& resource) {
  if (resource) retired_.emplace_back(apply_frame_, resource);
  resource.Reset();
}

bool Volumetrics::EnsureTargets(uint32_t width, uint32_t height) {
  if (light_[0] && width_ == width && height_ == height) return true;
  // The old ones kept until the frames in flight that use them are done (21/11/2026, the review:
  // freed at once, a size change left the GPU reading freed memory).
  for (int i = 0; i < 2; ++i) {
    Retire(light_[i]);
    Retire(fog_[i]);
  }
  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC d = {};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width = width;
  d.Height = height;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  d.SampleDesc.Count = 1;
  d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  for (ComPtr<ID3D12Resource>* r : {std::addressof(light_[0]), std::addressof(light_[1]),
                                    std::addressof(fog_[0]), std::addressof(fog_[1])}) {
    if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                IID_PPV_ARGS(r->GetAddressOf())),
               "volumetrics target")) {
      for (int i = 0; i < 2; ++i) {
        light_[i].Reset();
        fog_[i].Reset();
      }
      return false;
    }
  }
  for (int i = 0; i < 2; ++i)
    light_state_[i] = fog_state_[i] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  width_ = width;
  height_ = height;
  REXLOG_INFO("PGR4 native volumetrics: targets {}x{}", width, height);
  return true;
}

bool Volumetrics::EnsureVolume(uint32_t columns_x, uint32_t columns_y) {
  if (!noise_pso_ || !volume_pso_) return false;
  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC d = {};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
  d.MipLevels = 1;
  d.SampleDesc.Count = 1;
  d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  if (!noise_) {
    d.Width = d.Height = kNoiseSize;
    d.DepthOrArraySize = kNoiseSize;
    d.Format = DXGI_FORMAT_R16_FLOAT;
    if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                IID_PPV_ARGS(&noise_)),
               "volumetrics noise"))
      return false;
    noise_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    noise_ready_ = false;
  }
  if (!lights_) {
    D3D12_HEAP_PROPERTIES up = {};
    up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC b = {};
    b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    b.Width = uint64_t(kLightElements) * 16 * D3D12Device::kFramesInFlight;
    b.Height = b.DepthOrArraySize = b.MipLevels = 1;
    b.SampleDesc.Count = 1;
    b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (Failed(device_->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &b,
                                                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                IID_PPV_ARGS(&lights_)),
               "volumetrics lights") ||
        FAILED(lights_->Map(0, nullptr, reinterpret_cast<void**>(&lights_mapped_)))) {
      lights_.Reset();
      lights_mapped_ = nullptr;
      return false;
    }
    std::memset(lights_mapped_, 0, size_t(b.Width));
  }
  // The apply probe's buffer (UAV) and its readback (pgr4_native_volumetric_probe_uv).
  if (!probe_) {
    D3D12_RESOURCE_DESC pb = {};
    pb.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    pb.Width = uint64_t(kProbeElements) * 16;
    pb.Height = pb.DepthOrArraySize = pb.MipLevels = 1;
    pb.SampleDesc.Count = 1;
    pb.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    pb.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES dh = {};
    dh.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_PROPERTIES rh = {};
    rh.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rb = pb;
    rb.Flags = D3D12_RESOURCE_FLAG_NONE;
    rb.Width = 128;
    D3D12_RESOURCE_DESC gb = rb;
    gb.Width = kGridBytes;
    if (Failed(device_->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &pb,
                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                IID_PPV_ARGS(&probe_)),
               "volumetrics probe") ||
        Failed(device_->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &rb,
                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&probe_readback_)),
               "volumetrics probe readback")) {
      probe_.Reset();
      probe_readback_.Reset();
    }
    for (int i = 0; i < 3 && probe_; ++i)
      if (Failed(device_->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &gb,
                                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                  IID_PPV_ARGS(&grid_readback_[i])),
                 "volumetrics grid probe readback"))
        grid_readback_[i].Reset();
  }
  if (!ground_[0]) {
    D3D12_RESOURCE_DESC g = {};
    g.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    g.Width = g.Height = kGroundSize;
    g.DepthOrArraySize = 1;
    g.MipLevels = 1;
    g.SampleDesc.Count = 1;
    g.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    for (int i = 0; i < 2; ++i) {
      g.Format = i == 0 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R32_FLOAT;
      if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &g,
                                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                  IID_PPV_ARGS(&ground_[i])),
                 "volumetrics ground map")) {
        ground_[0].Reset();
        ground_[1].Reset();
        return false;
      }
      ground_state_[i] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
  }
  if (volume_ && volume_columns_[0] == columns_x && volume_columns_[1] == columns_y) return true;
  Retire(range_);
  Retire(volume_);
  Retire(media_[0]);
  Retire(media_[1]);
  media_valid_ = false;
  d.Width = columns_x;
  d.Height = columns_y;
  d.DepthOrArraySize = kSlices;
  d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                              IID_PPV_ARGS(&volume_)),
             "volumetrics volume"))
    return false;
  volume_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  {
    D3D12_RESOURCE_DESC r = d;
    r.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    r.DepthOrArraySize = 1;
    r.Format = DXGI_FORMAT_R32_FLOAT;
    if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &r,
                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                IID_PPV_ARGS(&range_)),
               "volumetrics range ratio")) {
      volume_.Reset();
      return false;
    }
    range_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  }
  // The cells' fog, this frame's and the previous (MEDIA), the same size.
  for (int i = 0; i < 2; ++i) {
    if (Failed(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                IID_PPV_ARGS(&media_[i])),
               "volumetrics cells' fog")) {
      volume_.Reset();
      return false;
    }
    media_state_[i] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  }
  ++share_.generation;
  volume_columns_[0] = columns_x;
  volume_columns_[1] = columns_y;
  REXLOG_INFO("PGR4 native volumetrics: volume {}x{}x{}", columns_x, columns_y, kSlices);
  return true;
}

ID3D12PipelineState* Volumetrics::ApplyPipeline(DXGI_FORMAT format, uint32_t samples, bool debug) {
  const auto key = std::make_pair(format, samples | (debug ? 0x100u : 0u));
  if (auto it = apply_psos_.find(key); it != apply_psos_.end()) return it->second.Get();
  D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
  d.pRootSignature = apply_signature_.Get();
  d.VS = {apply_vs_->GetBufferPointer(), apply_vs_->GetBufferSize()};
  ID3DBlob* ps = apply_froxel_ps_[(samples > 1 ? 2 : 0) + (debug ? 1 : 0)].Get();
  d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
  D3D12_RENDER_TARGET_BLEND_DESC& b = d.BlendState.RenderTarget[0];
  // color = color x src.a + src.rgb (the debug view: color = src.rgb); alpha (the motion blur's
  // mask) kept.
  b.BlendEnable = TRUE;
  b.SrcBlend = D3D12_BLEND_ONE;
  b.DestBlend = debug ? D3D12_BLEND_ZERO : D3D12_BLEND_SRC_ALPHA;
  b.BlendOp = D3D12_BLEND_OP_ADD;
  b.SrcBlendAlpha = D3D12_BLEND_ZERO;
  b.DestBlendAlpha = D3D12_BLEND_ONE;
  b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
  b.LogicOp = D3D12_LOGIC_OP_NOOP;
  b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  d.SampleMask = UINT_MAX;
  d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  d.RasterizerState.DepthClipEnable = TRUE;
  d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  d.NumRenderTargets = 1;
  d.RTVFormats[0] = format;
  d.SampleDesc.Count = samples;
  ComPtr<ID3D12PipelineState> pso;
  if (Failed(device_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)), "volumetrics apply")) {
    apply_psos_[key] = nullptr;
    return nullptr;
  }
  apply_psos_[key] = pso;
  return pso.Get();
}

void Volumetrics::Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* r,
                             D3D12_RESOURCE_STATES& state, D3D12_RESOURCE_STATES after) {
  if (state == after) return;
  const D3D12_RESOURCE_BARRIER b = TransitionBarrier(r, state, after);
  list->ResourceBarrier(1, &b);
  state = after;
}

bool Volumetrics::Apply(ID3D12GraphicsCommandList* list, RenderTargets& targets,
                        const SurfaceRegs& regs, uint64_t frame, GpuTimer* timer, bool early) {
  // Development (22/11/2026): its CPU time, the race probe's (draw.cpp g_pgr4_vol_ticks, slots 1-2).
  const uint64_t vol_t0 = g_pgr4_vol_timing ? Ticks() : 0;
  struct VolTime {
    uint64_t t0;
    int slot;
    ~VolTime() {
      if (t0) g_pgr4_vol_ticks[slot] += Ticks() - t0;
    }
  } const vol_time{vol_t0, early ? 1 : 2};
  if (!ready_ || !Enabled() || !clip_rows_) return false;
  if (early ? (early_tried_frame_ == frame || volume_frame_ == frame ||
               !REXCVAR_GET(pgr4_native_volumetric_volume_early))
            : applied_frame_ == frame)
    return false;
  if (early) early_tried_frame_ = frame;
  // Local change (22/11/2026): the freeze probe, the step reached (HangStep).
  HangMark hang_mark(HangWhat::kVolume, early ? 100 : 200);
  // The volume already made this frame by the early pass: not made again here.
  const bool volume_done = !early && volume_frame_ == frame;
  if (!early) {
    applied_frame_ = frame;
    apply_frame_ = frame;
    std::erase_if(retired_, [&](const auto& x) {
      return x.first + D3D12Device::kFramesInFlight + 1 <= frame;
    });
  }
  if (timer) timer->Pass(list, GpuTimer::KeyEffects(early ? 7 : 5));
  // The banks' clock: real time while the game's runs (still in the pause); once a frame, at the
  // resolve (the early volume takes the clock as the previous frame's resolve left it).
  if (!early) {
    const auto now = std::chrono::steady_clock::now();
    if (clock_started_ && !g_pgr4_game_paused)
      fog_time_ += std::min(std::chrono::duration<double>(now - clock_last_).count(), 0.25);
    clock_last_ = now;
    clock_started_ = true;
  }
  float sun[3], sun_color[3];
  for (int k = 0; k < 3; ++k) {
    sun[k] = g_pgr4_sun_dir[k].load(std::memory_order_relaxed);
    sun_color[k] = std::max(0.0f, g_pgr4_sun_color[k].load(std::memory_order_relaxed));
  }
  RenderTarget* color = targets.GetColor(regs, 0, Upscaler::SceneHeight());
  RenderTarget* depth = targets.GetDepth(regs, Upscaler::SceneHeight());
  if (!color || !depth || color->samples != depth->samples || !(color->scale == depth->scale))
    return false;
  // The scene's visible height (21/11/2026, the lamps' glow below the lamps: the target is the
  // EDRAM's, 1024 guest rows, of which the scene draws the top SceneHeight (720); the rays mapped
  // over all of it stood 1024 / 720 = 1.42 times too low on the screen, the banks, the ground map and
  // the lamps with them). The switch off: the whole target, as it was.
  const uint32_t target_rows = REXCVAR_GET(pgr4_native_volumetric_visible_height)
                                   ? std::min(color->height, Upscaler::SceneHeight())
                                   : color->height;
  const uint32_t w = color->scale.Extent(color->width, 0), h = color->scale.Extent(target_rows, 1);
  const uint32_t hw = (w + 1) / 2, hh = (h + 1) / 2;
  const FfxCamera cam = camera_ ? *camera_ : FfxCamera{};
  Constants c = {};
  c.size[0] = w;
  c.size[1] = h;
  c.half_size[0] = hw;
  c.half_size[1] = hh;
  c.near_plane = cam.near_plane;
  c.far_plane = cam.far_plane;
  const float t = std::tan(cam.fov_y * 0.5f);
  c.tan_half[0] = t * float(w) / float(h);
  c.tan_half[1] = t;
  // The frame's clip transform: the fog draws' own when one was seen this frame (draw.cpp), else
  // the scene camera's (FindCamera).
  const bool fog_camera =
      REXCVAR_GET(pgr4_native_volumetric_fog_camera) && g_pgr4_fog_clip_frame == frame;
  float fog_rows[16];
  const float* rows = clip_rows_;
  if (fog_camera) {
    std::memcpy(fog_rows, g_pgr4_fog_clip, sizeof(fog_rows));
    rows = fog_rows;
  }
  // The view's half extents at 1 m from the same transform (21/11/2026, the replays' cinema cameras
  // zoom: the scene camera's field of view lagged or was another's, the volume's rays went beside
  // the pixels): world to clip is the projection's scale times a rotation, so each of the x and y
  // rows' length is the w row's over the half extent.
  const float scene_tan[2] = {c.tan_half[0], c.tan_half[1]};
  if (fog_camera) {
    const auto len3 = [](const float* r) { return std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]); };
    const float lx = len3(rows), ly = len3(rows + 4), lw = len3(rows + 12);
    if (lx > 1e-6f && ly > 1e-6f && lw > 1e-6f && std::isfinite(lw / lx) && std::isfinite(lw / ly)) {
      c.tan_half[0] = lw / lx;
      c.tan_half[1] = lw / ly;
    }
  }
  // The sun and the world's axes in view space, through the frame's clip transform.
  static constexpr float kEast[3] = {1.0f, 0.0f, 0.0f}, kUp[3] = {0.0f, 1.0f, 0.0f},
                         kNorth[3] = {0.0f, 0.0f, 1.0f};
  // The scenery's sun, only while its draws give one (none at night: its lighting is in its maps,
  // and the cars' key light is not a sun; 21/11/2026, Las Vegas whitened by it).
  const int64_t sun_seen = g_pgr4_sun_seen_ns.load(std::memory_order_relaxed);
  const bool sun_recent =
      sun_seen != 0 &&
      std::chrono::steady_clock::now().time_since_epoch().count() - sun_seen < 500'000'000;
  const bool sun_known = sun_recent && sun_color[0] + sun_color[1] + sun_color[2] >= 0.01f &&
                         ViewDirection(rows, sun, c.tan_half, c.sun_view);
  const bool axes_known = ViewDirection(rows, kUp, c.tan_half, c.up_view) &&
                          ViewDirection(rows, kEast, c.tan_half, c.east_view) &&
                          ViewDirection(rows, kNorth, c.tan_half, c.north_view);
  if (sun_known)
    for (int k = 0; k < 3; ++k) c.sun_color[k] = sun_color[k];
  // The sun-lit air, the same in every weather: the fog's own thickness is the game's, laid again
  // (21/11/2026: three times as thick in fog, it counted the fog's light twice over).
  const int weather = g_pgr4_race_screen ? g_pgr4_weather_kind : -1;
  c.sigma = sun_known ? float(REXCVAR_GET(pgr4_native_volumetric_density)) : 0.0f;
  c.fog_g = float(REXCVAR_GET(pgr4_native_volumetric_fog_phase));
  // The ground mist: the game's fog at the road (its distance fog, its height fog there) times the
  // overlay's thickness; none without the game's fog known.
  c.mist_height = float(REXCVAR_GET(pgr4_native_volumetric_mist_height));
  c.mist_distance = float(REXCVAR_GET(pgr4_native_volumetric_mist_distance));
  c.camera_height = float(REXCVAR_GET(pgr4_native_volumetric_eye_height));
  c.strength = float(REXCVAR_GET(pgr4_native_volumetric_strength));
  c.max_distance = float(REXCVAR_GET(pgr4_native_volumetric_distance));
  c.height_scale = float(REXCVAR_GET(pgr4_native_volumetric_height_scale));
  c.thickness = float(REXCVAR_GET(pgr4_native_volumetric_thickness));
  c.g = float(REXCVAR_GET(pgr4_native_volumetric_phase));
  {
    const double speed = REXCVAR_GET(pgr4_native_volumetric_wind);
    const double angle = REXCVAR_GET(pgr4_native_volumetric_wind_angle) * 3.14159265358979 / 180.0;
    c.wind[0] = float(speed * std::cos(angle));
    c.wind[1] = float(speed * std::sin(angle));
  }
  c.steps = uint32_t(std::clamp(REXCVAR_GET(pgr4_native_volumetric_steps), 4, 96));
  c.shadow_steps = uint32_t(std::clamp(REXCVAR_GET(pgr4_native_volumetric_shadow_steps), 0, 12));
  c.time = float(std::fmod(fog_time_, 100000.0));
  // The game's fog, as its last scene draw had it (draw.cpp).
  for (int k = 0; k < 3; ++k) {
    c.eye[k] = g_pgr4_fog_eye[k].load(std::memory_order_relaxed);
    c.fog_color[k] = g_pgr4_fog_c10[k].load(std::memory_order_relaxed);
  }
  // The eye: the frame's camera's (the switch on), the last fog draw's c5 kept to compare.
  float c5_eye[3] = {c.eye[0], c.eye[1], c.eye[2]}, camera_eye[3] = {};
  const bool camera_eye_known = CameraCenter(rows, camera_eye);
  // The scene camera's centre and up, to compare (the probe below).
  float scene_centre[3] = {}, scene_up[3] = {};
  const bool scene_centre_known = CameraCenter(clip_rows_, scene_centre);
  ViewDirection(clip_rows_, kUp, c.tan_half, scene_up);
  float eye_gap = -1.0f;
  if (camera_eye_known) {
    eye_gap = std::max({std::abs(camera_eye[0] - c5_eye[0]), std::abs(camera_eye[1] - c5_eye[1]),
                        std::abs(camera_eye[2] - c5_eye[2])});
    if (REXCVAR_GET(pgr4_native_volumetric_eye_from_camera))
      for (int k = 0; k < 3; ++k) c.eye[k] = camera_eye[k];
  }
  // The road: pgr4_native_volumetric_eye_height under the eye (camera_height). The filmed car's
  // height (pgr4_native_volumetric_road_from_car) was a rejected track, removed on 21/11/2026.
  const bool road_from_car = false;
  const float car_distance = -1.0f;
  // The fog's color as the scene has it: c10.rgb times the shaders' scale after the fog (c12.y).
  const float fog_c12 = g_pgr4_fog_c12y.load(std::memory_order_relaxed);
  if (REXCVAR_GET(pgr4_native_volumetric_fog_c12) && fog_c12 > 0.0f && std::isfinite(fog_c12))
    for (int k = 0; k < 3; ++k) c.fog_color[k] *= fog_c12;
  c.fog_distance = g_pgr4_fog_c10[3].load(std::memory_order_relaxed);
  c.fog_ceiling = g_pgr4_fog_c11[0].load(std::memory_order_relaxed);
  c.fog_density = g_pgr4_fog_c11[1].load(std::memory_order_relaxed);
  c.fog_falloff = g_pgr4_fog_c11[2].load(std::memory_order_relaxed);
  c.fog_height_on = g_pgr4_fog_height_on.load(std::memory_order_relaxed);
  c.fog_exp2 = g_pgr4_fog_exp2.load(std::memory_order_relaxed);
  c.variation = float(REXCVAR_GET(pgr4_native_volumetric_fog_variation));
  c.noise_scale = 1.0f / float(REXCVAR_GET(pgr4_native_volumetric_fog_size));
  c.fog_sun = float(REXCVAR_GET(pgr4_native_volumetric_fog_sun));
  c.vol_range = float(REXCVAR_GET(pgr4_native_volumetric_range));
  // The ground map's corner: the cell the eye is in, half the map back (snapped to the cells: still
  // in the world while the eye moves).
  c.ground_origin[0] = (std::floor(c.eye[0] / kGroundCell) - float(kGroundSize / 2)) * kGroundCell;
  c.ground_origin[1] = (std::floor(c.eye[2] / kGroundCell) - float(kGroundSize / 2)) * kGroundCell;
  c.ground_on = REXCVAR_GET(pgr4_native_volumetric_ground) ? 1u : 0u;
  // Only with the game's fog known (its exponent's scale read from a scene draw: positive).
  const bool fog_known = c.fog_exp2 > 0.0f && (c.fog_distance > 0.0f || c.fog_height_on);
  c.replace = REXCVAR_GET(pgr4_native_volumetric_fog) && fog_known;
  if (fog_known) {
    const float road = c.eye[1] - c.camera_height;
    float at_road = c.fog_distance;
    if (c.fog_height_on)
      at_road += c.fog_density * std::clamp((c.fog_ceiling - road) * c.fog_falloff, 0.0f, 1.0f);
    // In the mist's units (exp(-tau)): the game's fog keeps exp2(-scale x tau) of the surface, so ln 2 x
    // its scale (1 / ln 2 in every fog shader read, 21/11/2026: the game's fog is exp(-tau) too).
    c.mist_density = float(REXCVAR_GET(pgr4_native_volumetric_mist)) * at_road * c.fog_exp2 *
                     0.69314718f;
  }
  const bool debug = REXCVAR_GET(pgr4_native_volumetric_debug);
  // Development (21/11/2026, the far fog flickering while driving): the inputs frame to frame over
  // 2 s, their flips counted, so a flicker shows itself in the log ("FLICKER").
  struct InputsSeen {
    uint64_t frames = 0, replace_flips = 0, sun_flips = 0, eye_jumps = 0, scale_changes = 0;
    // The c5 copy against the camera's eye: frames off by more than 0.5 m, the most, the camera's
    // eye unknown.
    uint64_t c5_off = 0, camera_unknown = 0;
    uint64_t fog_camera_frames = 0;  // frames on the fog draws' camera
    // Its vertical half extent against the scene camera's: the largest gap (relative), the last of
    // each.
    float tan_gap = 0.0f, tan_fog = 0.0f, tan_scene = 0.0f;
    // The road's height: frames from the car, the eye's height above the road (range), the car's
    // distance (last).
    uint64_t road_car_frames = 0;
    float above_lo = 0.0f, above_hi = 0.0f, car_distance = -1.0f;
    // The fog shaders' c12.y copied: its changes from frame to frame, its range.
    uint64_t c12_changes = 0;
    float c12_lo = 0.0f, c12_hi = 0.0f;
    float gap_max = 0.0f, c5_lo = 0.0f, c5_hi = 0.0f, camera_lo = 0.0f, camera_hi = 0.0f;
    float lo[4], hi[4];  // the exponent's scale, the distance fog, the eye's height, the mist
  };
  static InputsSeen in;
  static bool in_last_replace = false, in_last_sun = false, in_any = false;
  static float in_last_eye = 0.0f, in_last_scale = 0.0f, in_last_c12 = 0.0f;
  if (!early) {
    const float v[4] = {c.fog_exp2, c.fog_distance, c.eye[1], c.mist_density};
    if (in_any) {
      in.replace_flips += bool(c.replace) != in_last_replace;
      in.sun_flips += sun_known != in_last_sun;
      in.eye_jumps += std::abs(c.eye[1] - in_last_eye) > 0.5f;
      in.scale_changes += c.fog_exp2 != in_last_scale;
    }
    for (int i = 0; i < 4; ++i) {
      in.lo[i] = in.frames ? std::min(in.lo[i], v[i]) : v[i];
      in.hi[i] = in.frames ? std::max(in.hi[i], v[i]) : v[i];
    }
    in.c12_changes += in_any && fog_c12 != in_last_c12;
    in.c12_lo = in.frames ? std::min(in.c12_lo, fog_c12) : fog_c12;
    in.c12_hi = in.frames ? std::max(in.c12_hi, fog_c12) : fog_c12;
    in_last_c12 = fog_c12;
    in.fog_camera_frames += fog_camera;
    in.tan_gap = std::max(in.tan_gap, std::abs(c.tan_half[1] - scene_tan[1]) /
                                          std::max(scene_tan[1], 1e-6f));
    in.tan_fog = c.tan_half[1];
    in.tan_scene = scene_tan[1];
    in.road_car_frames += road_from_car;
    in.above_lo = in.frames ? std::min(in.above_lo, c.camera_height) : c.camera_height;
    in.above_hi = in.frames ? std::max(in.above_hi, c.camera_height) : c.camera_height;
    in.car_distance = car_distance;
    in.c5_off += eye_gap > 0.5f;
    in.camera_unknown += !camera_eye_known;
    in.gap_max = std::max(in.gap_max, eye_gap);
    in.c5_lo = in.frames ? std::min(in.c5_lo, c5_eye[1]) : c5_eye[1];
    in.c5_hi = in.frames ? std::max(in.c5_hi, c5_eye[1]) : c5_eye[1];
    in.camera_lo = in.frames ? std::min(in.camera_lo, camera_eye[1]) : camera_eye[1];
    in.camera_hi = in.frames ? std::max(in.camera_hi, camera_eye[1]) : camera_eye[1];
    ++in.frames;
    in_any = true;
    in_last_replace = c.replace;
    in_last_sun = sun_known;
    in_last_eye = c.eye[1];
    in_last_scale = c.fog_exp2;
  }
  // The values it works from, every 2 s.
  if (!early) {
    static auto last = std::chrono::steady_clock::now() - std::chrono::seconds(3);
    // Development only (g_pgr4_dev, 22/11/2026: never in the release).
    if (const auto now = std::chrono::steady_clock::now(); g_pgr4_dev && now - last >= std::chrono::seconds(2)) {
      last = now;
      REXLOG_INFO("PGR4 native volumetrics: sun {:.3f} {:.3f} {:.3f} color {:.3f} {:.3f} {:.3f}, "
                  "in view {:.3f} {:.3f} {:.3f}; up {:.3f} {:.3f} {:.3f}; sun-lit air {:.5f} per m "
                  "(weather {}); game's fog {} (color {:.3f} {:.3f} {:.3f}, {:.6f} per m, height "
                  "{} ceiling {:.1f} density {:.4f} falloff {:.4f}, exp2 {:.4f}, eye {:.1f} {:.1f} "
                  "{:.1f}), banks {:.2f} of {:.0f} m, mist {:.5f} per m ({:.1f} m high, {:.0f} m), time "
                  "{:.1f} s; scene {}x{} x{}{}",
                  sun[0], sun[1], sun[2], sun_color[0], sun_color[1], sun_color[2], c.sun_view[0],
                  c.sun_view[1], c.sun_view[2], c.up_view[0], c.up_view[1], c.up_view[2], c.sigma,
                  weather, c.replace ? "laid again" : "the game's", c.fog_color[0], c.fog_color[1],
                  c.fog_color[2], c.fog_distance, c.fog_height_on ? "on" : "off", c.fog_ceiling,
                  c.fog_density, c.fog_falloff, c.fog_exp2, c.eye[0], c.eye[1], c.eye[2],
                  c.variation, 1.0f / c.noise_scale, c.mist_density, c.mist_height,
                  c.mist_distance, c.time, w, h, color->samples,
                  !axes_known                ? ": nothing drawn, no camera known"
                  : !c.replace && !(c.sigma > 0.0f) ? ": nothing drawn, no sun and no fog"
                                             : "");
      const bool flicker = in.replace_flips || in.sun_flips || in.eye_jumps || in.scale_changes ||
                           in.c12_changes;
      REXLOG_INFO("PGR4 native volumetrics c12: the copy changed in {} of {} frames ({:.4f}..{:.4f}){}",
                  in.c12_changes, in.frames, in.c12_lo, in.c12_hi,
                  in.c12_changes ? " FLICKER" : "");
      REXLOG_INFO("PGR4 native volumetrics camera: the fog draws' in {} of {} frames (draws taken {}, "
                  "not world to clip {}, mirrored {}); up in view {:.3f} {:.3f} {:.3f}; the scene "
                  "camera's centre {:.1f} {:.1f} {:.1f}{}, up {:.3f} {:.3f} {:.3f}",
                  in.fog_camera_frames, in.frames, g_pgr4_fog_camera_stats[0],
                  g_pgr4_fog_camera_stats[1], g_pgr4_fog_camera_stats[2], c.up_view[0],
                  c.up_view[1], c.up_view[2], scene_centre[0], scene_centre[1], scene_centre[2],
                  scene_centre_known ? "" : " (unknown)", scene_up[0], scene_up[1], scene_up[2]);
      g_pgr4_fog_camera_stats[0] = g_pgr4_fog_camera_stats[1] = g_pgr4_fog_camera_stats[2] = 0;
      REXLOG_INFO("PGR4 native volumetrics field of view: the volume's {:.2f} deg, the scene camera's "
                  "{:.2f} deg (at the last), apart by up to {:.1f} % over 2 s{}",
                  2.0f * std::atan(in.tan_fog) * 57.2958f, 2.0f * std::atan(in.tan_scene) * 57.2958f,
                  in.tan_gap * 100.0f, in.tan_gap > 0.01f ? " APART" : "");
      REXLOG_INFO("PGR4 native volumetrics road: from the car in {} of {} frames (the car {:.1f} m "
                  "away at the last); the eye {:.2f}..{:.2f} m above the road",
                  in.road_car_frames, in.frames, in.car_distance, in.above_lo, in.above_hi);
      REXLOG_INFO("PGR4 native volumetrics eye: c5 off the camera's by over 0.5 m in {} of {} frames "
                  "(at most {:.2f} m; c5.y {:.2f}..{:.2f}, the camera's {:.2f}..{:.2f}; camera unknown in "
                  "{}); taken from {}",
                  in.c5_off, in.frames, in.gap_max, in.c5_lo, in.c5_hi, in.camera_lo, in.camera_hi,
                  in.camera_unknown,
                  REXCVAR_GET(pgr4_native_volumetric_eye_from_camera) ? "the camera" : "c5 (test)");
      REXLOG_INFO("PGR4 native volumetrics inputs: {} frames, fog laid again flipped {}, sun flipped "
                  "{}, eye height jumped {}, exponent's scale changed {}; scale {:.4f}..{:.4f}, "
                  "distance fog {:.6f}..{:.6f}, eye height {:.2f}..{:.2f}, mist {:.5f}..{:.5f}{}",
                  in.frames, in.replace_flips, in.sun_flips, in.eye_jumps, in.scale_changes,
                  in.lo[0], in.hi[0], in.lo[1], in.hi[1], in.lo[2], in.hi[2], in.lo[3], in.hi[3],
                  flicker ? " FLICKER" : "");
      in = {};
      REXLOG_INFO("PGR4 native volumetrics volume: {} ({}x{}x{} cells over {:.0f} m, "
                  "banks variation {:.2f} of {:.0f} m); the fog's color "
                  "{:.3f} {:.3f} {:.3f} (c12.y {:.4f}, {}); smoothed over {:.2f} s: blend {:.3f}, "
                  "history {}, {} restarts, this frame's place in the cells {:.2f} {:.2f} {:.2f}",
                  "on",
                  (w + 15) / 16, (h + 15) / 16, kSlices, c.vol_range, c.variation,
                  1.0f / c.noise_scale, c.fog_color[0], c.fog_color[1],
                  c.fog_color[2], fog_c12,
                  REXCVAR_GET(pgr4_native_volumetric_fog_c12) ? "applied" : "not applied (test)",
                  REXCVAR_GET(pgr4_native_volumetric_temporal), temporal_header_[7],
                  temporal_header_[3] > 0.5f ? "used" : "none", temporal_resets_,
                  temporal_header_[15], temporal_header_[19], temporal_header_[11]);
    }
  }
  if (!axes_known || (!c.replace && !(c.sigma > 0.0f) && !(c.mist_density > 0.0f)) || !w || !h ||
      !EnsureTargets(hw, hh))
    return false;

  // The volume (columns of 16 x 16 pixels, kSlices deep). Not made (its memory refused): no
  // volumetric fog this frame, the shaders keep the game's fog (volume_frame_ not this frame's).
  const uint32_t columns_x = (w + 15) / 16, columns_y = (h + 15) / 16;  HangStep((early ? 100 : 200) + 1);
  if (!EnsureVolume(columns_x, columns_y)) return false;
  const D3D12_RECT all = {0, 0, LONG(color->width), LONG(color->height)};
  // The cells' fog: last frame's becomes the previous (pgr4_native_volumetric_temporal); here,
  // past the early returns, so that a frame without the pass does not swap them (21/11/2026, the
  // review: in clear weather it returned after the swap every frame, the history then stale).
  if (!volume_done) media_now_ ^= 1u;
  if (!early) targets.PrepareRead(list, depth, all);
  ID3D12Resource* depth_resource = depth->resource.Get();
  D3D12_RESOURCE_STATES* depth_state = &depth->state;
  const bool ms = depth->samples > 1;
  const uint32_t first = uint32_t(frame % D3D12Device::kFramesInFlight) * kViewsPerSlot;
  auto cpu = [&](uint32_t i) {
    D3D12_CPU_DESCRIPTOR_HANDLE hnd = heap_->GetCPUDescriptorHandleForHeapStart();
    hnd.ptr += size_t(first + i) * stride_;
    return hnd;
  };
  auto gpu = [&](uint32_t i) {
    D3D12_GPU_DESCRIPTOR_HANDLE hnd = heap_->GetGPUDescriptorHandleForHeapStart();
    hnd.ptr += uint64_t(first + i) * stride_;
    return hnd;
  };
  D3D12_SHADER_RESOURCE_VIEW_DESC depth_srv = {};
  depth_srv.Format = depth->srv_format;
  depth_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  HangStep((early ? 100 : 200) + 2);
  if (ms) {
    depth_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
  } else {
    depth_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    depth_srv.Texture2D.MipLevels = 1;
  }
  D3D12_SHADER_RESOURCE_VIEW_DESC half_srv = {};
  half_srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  half_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  half_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  half_srv.Texture2D.MipLevels = 1;
  D3D12_UNORDERED_ACCESS_VIEW_DESC half_uav = {};
  half_uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  half_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  const auto srv = [&](ID3D12Resource* r, const D3D12_SHADER_RESOURCE_VIEW_DESC& d, uint32_t i) {
    device_->CreateShaderResourceView(r, &d, cpu(i));
  };
  const auto uav = [&](ID3D12Resource* r, uint32_t i) {
    device_->CreateUnorderedAccessView(r, nullptr, &half_uav, cpu(i));
  };
  // The march (0-4): depth, -, -, light 0, fog 0. The blur across (5-9): light 0, fog 0, -,
  // light 1, fog 1. The blur down (10-14): light 1, fog 1, -, light 0, fog 0. The apply (15-17):
  // light 0, fog 0, depth.
  srv(depth_resource, depth_srv, 0);
  srv(nullptr, half_srv, 1);
  srv(nullptr, half_srv, 2);
  uav(light_[0].Get(), 3);
  uav(fog_[0].Get(), 4);
  srv(light_[0].Get(), half_srv, 5);
  srv(fog_[0].Get(), half_srv, 6);
  srv(nullptr, half_srv, 7);
  uav(light_[1].Get(), 8);
  uav(fog_[1].Get(), 9);
  srv(light_[1].Get(), half_srv, 10);
  srv(fog_[1].Get(), half_srv, 11);
  srv(nullptr, half_srv, 12);
  uav(light_[0].Get(), 13);
  uav(fog_[0].Get(), 14);
  srv(light_[0].Get(), half_srv, 15);
  srv(fog_[0].Get(), half_srv, 16);
  srv(depth_resource, depth_srv, 17);
  srv(nullptr, half_srv, 18);  // the apply's t3 (none without the volume; the noise's t0 with it)
  srv(nullptr, half_srv, 19);  // ... and t4
  srv(nullptr, half_srv, 20);  // ... and t5
  // The noise (18-22): -, -, -, noise, -. The volume (23-27): depth, noise, -, volume, -. The apply
  // with the volume (28-30): light 0, volume, depth.
  {
    D3D12_UNORDERED_ACCESS_VIEW_DESC noise_uav = {};
    noise_uav.Format = DXGI_FORMAT_R16_FLOAT;
    noise_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
    noise_uav.Texture3D.WSize = kNoiseSize;
    D3D12_SHADER_RESOURCE_VIEW_DESC noise_srv = {};
    noise_srv.Format = DXGI_FORMAT_R16_FLOAT;
    noise_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    noise_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    noise_srv.Texture3D.MipLevels = 1;
    D3D12_UNORDERED_ACCESS_VIEW_DESC volume_uav = noise_uav;
    volume_uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    volume_uav.Texture3D.WSize = kSlices;
    D3D12_SHADER_RESOURCE_VIEW_DESC volume_srv = noise_srv;
    volume_srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srv(nullptr, half_srv, 18);
    srv(nullptr, half_srv, 19);
    srv(nullptr, half_srv, 20);
    device_->CreateUnorderedAccessView(noise_.Get(), nullptr, &noise_uav, cpu(21));
    uav(nullptr, 22);
    // The lights at t0 (the volume reads no depth), this frame slot's part of the upload buffer.
    {
      D3D12_SHADER_RESOURCE_VIEW_DESC ls = {};
      ls.Format = DXGI_FORMAT_UNKNOWN;
      ls.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
      ls.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      ls.Buffer.FirstElement = uint64_t(frame % D3D12Device::kFramesInFlight) * kLightElements;
      ls.Buffer.NumElements = kLightElements;
      ls.Buffer.StructureByteStride = 16;
      device_->CreateShaderResourceView(lights_.Get(), &ls, cpu(23));
    }
    // The cells' fog of this frame (MEDIA) at t1.
    srv(media_[media_now_].Get(), volume_srv, 24);
    // The volume reads the filled ground map at t2 (below).
    {
      D3D12_SHADER_RESOURCE_VIEW_DESC gs = {};
      gs.Format = DXGI_FORMAT_R32_FLOAT;
      gs.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      gs.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      gs.Texture2D.MipLevels = 1;
      device_->CreateShaderResourceView(ground_[1].Get(), &gs, cpu(25));
    }
    device_->CreateUnorderedAccessView(volume_.Get(), nullptr, &volume_uav, cpu(26));
    {
      D3D12_UNORDERED_ACCESS_VIEW_DESC ru = {};
      ru.Format = DXGI_FORMAT_R32_FLOAT;
      ru.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
      device_->CreateUnorderedAccessView(range_.Get(), nullptr, &ru, cpu(27));
    }
    srv(light_[0].Get(), half_srv, 28);
    srv(volume_.Get(), volume_srv, 29);
    srv(depth_resource, depth_srv, 30);
    // The apply with the volume reads the lamps too (46-49): light 0, volume, depth, lamps.
    srv(light_[0].Get(), half_srv, 46);
    srv(volume_.Get(), volume_srv, 47);
    srv(depth_resource, depth_srv, 48);
    {
      D3D12_SHADER_RESOURCE_VIEW_DESC ls = {};
      ls.Format = DXGI_FORMAT_UNKNOWN;
      ls.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
      ls.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      ls.Buffer.FirstElement = uint64_t(frame % D3D12Device::kFramesInFlight) * kLightElements;
      ls.Buffer.NumElements = kLightElements;
      ls.Buffer.StructureByteStride = 16;
      device_->CreateShaderResourceView(lights_.Get(), &ls, cpu(49));
    }
    srv(noise_.Get(), noise_srv, 50);  // the beams' texture
    // 51 (t5): unread since the fade mark was removed (21/11/2026): a null view, the table as it was.
    {
      D3D12_SHADER_RESOURCE_VIEW_DESC ss = {};
      ss.Format = DXGI_FORMAT_R32G32_UINT;
      ss.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      if (ms) {
        ss.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
      } else {
        ss.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        ss.Texture2D.MipLevels = 1;
      }
      device_->CreateShaderResourceView(nullptr, &ss, cpu(51));
    }
    // The apply probe's buffer at 52 (u1).
    if (probe_) {
      D3D12_UNORDERED_ACCESS_VIEW_DESC pu = {};
      pu.Format = DXGI_FORMAT_UNKNOWN;
      pu.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
      pu.Buffer.NumElements = kProbeElements;
      pu.Buffer.StructureByteStride = 16;
      device_->CreateUnorderedAccessView(probe_.Get(), nullptr, &pu, cpu(52));
      // The grid probe's colour reading (58-62): the scene's colour, -, -, -, the probe's buffer.
      D3D12_SHADER_RESOURCE_VIEW_DESC colour_srv = {};
      colour_srv.Format = color->view_format;
      colour_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      if (color->samples > 1) {
        colour_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
      } else {
        colour_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        colour_srv.Texture2D.MipLevels = 1;
      }
      device_->CreateShaderResourceView(color->resource.Get(), &colour_srv, cpu(58));
      srv(nullptr, half_srv, 59);
      srv(nullptr, half_srv, 60);
      uav(nullptr, 61);
      device_->CreateUnorderedAccessView(probe_.Get(), nullptr, &pu, cpu(62));
    }
    // MEDIA (51-55): lamps (their headers), noise, filled ground map, this frame's cells' fog, the
    // previous frame's.
    {
      D3D12_SHADER_RESOURCE_VIEW_DESC ls = {};
      ls.Format = DXGI_FORMAT_UNKNOWN;
      ls.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
      ls.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      ls.Buffer.FirstElement = uint64_t(frame % D3D12Device::kFramesInFlight) * kLightElements;
      ls.Buffer.NumElements = kLightElements;
      ls.Buffer.StructureByteStride = 16;
      device_->CreateShaderResourceView(lights_.Get(), &ls, cpu(53));
      srv(noise_.Get(), noise_srv, 54);
      D3D12_SHADER_RESOURCE_VIEW_DESC gs = {};
      gs.Format = DXGI_FORMAT_R32_FLOAT;
      gs.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      gs.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      gs.Texture2D.MipLevels = 1;
      device_->CreateShaderResourceView(ground_[1].Get(), &gs, cpu(55));
      device_->CreateUnorderedAccessView(media_[media_now_].Get(), nullptr, &volume_uav, cpu(56));
      device_->CreateUnorderedAccessView(media_[1 - media_now_].Get(), nullptr, &volume_uav, cpu(57));
    }
    // The ground map: clear (31-35): -, -, -, map, -. Scatter (36-40): depth, -, -, map, -. Fill
    // (41-45): -, map (uint), -, filled map, -.
    D3D12_UNORDERED_ACCESS_VIEW_DESC gu = {};
    gu.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    gu.Format = DXGI_FORMAT_R32_UINT;
    D3D12_SHADER_RESOURCE_VIEW_DESC gs = {};
    gs.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    gs.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    gs.Texture2D.MipLevels = 1;
    gs.Format = DXGI_FORMAT_R32_UINT;
    for (uint32_t i : {31u, 32u, 33u, 37u, 38u, 41u, 43u}) srv(nullptr, half_srv, i);
    srv(depth_resource, depth_srv, 36);
    device_->CreateUnorderedAccessView(ground_[0].Get(), nullptr, &gu, cpu(34));
    device_->CreateUnorderedAccessView(ground_[0].Get(), nullptr, &gu, cpu(39));
    device_->CreateShaderResourceView(ground_[0].Get(), &gs, cpu(42));
    gu.Format = DXGI_FORMAT_R32_FLOAT;
    device_->CreateUnorderedAccessView(ground_[1].Get(), nullptr, &gu, cpu(44));
    for (uint32_t i : {35u, 40u, 45u}) uav(nullptr, i);
  }

  constexpr D3D12_RESOURCE_STATES kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  constexpr D3D12_RESOURCE_STATES kWrite = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  const auto state = [&](int i, D3D12_RESOURCE_STATES s) {
    Transition(list, light_[i].Get(), light_state_[i], s);
    Transition(list, fog_[i].Get(), fog_state_[i], s);
  };
  if (!early) Transition(list, depth_resource, *depth_state, kRead);
  ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(compute_signature_.Get());
  list->SetComputeRoot32BitConstants(1, sizeof(c) / 4, &c, 0);
  const UINT gx = (hw + 7) / 8, gy = (hh + 7) / 8;
  // The march only for the sun-lit air (its light at t0 of the apply, unread without).
  const bool march = !early && c.sigma > 0.0f;
  HangStep((early ? 100 : 200) + 3);
  if (march) {
  state(0, kWrite);
  state(1, kWrite);
  list->SetPipelineState(ms ? march_ms_pso_.Get() : march_pso_.Get());
  list->SetComputeRootDescriptorTable(0, gpu(0));
  list->Dispatch(gx, gy, 1);
  state(0, kRead);
  list->SetPipelineState(blur_pso_.Get());
  list->SetComputeRootDescriptorTable(0, gpu(5));
  list->Dispatch(gx, gy, 1);
  state(1, kRead);
  state(0, kWrite);
  c.axis = 1;
  list->SetComputeRoot32BitConstants(1, sizeof(c) / 4, &c, 0);
  list->SetComputeRootDescriptorTable(0, gpu(10));
  list->Dispatch(gx, gy, 1);
  state(0, kRead);
  } else if (!early) {
    state(0, kRead);
  }
  {
    HangStep((early ? 100 : 200) + 4);
    if (!noise_ready_) {
      Transition(list, noise_.Get(), noise_state_, kWrite);
      list->SetPipelineState(noise_pso_.Get());
      list->SetComputeRootDescriptorTable(0, gpu(18));
      list->Dispatch(kNoiseSize / 4, kNoiseSize / 4, kNoiseSize / 4);
      noise_ready_ = true;
    }
    // Read by MEDIA and by the apply's pixel shader (the beams' texture, t4).
    Transition(list, noise_.Get(), noise_state_, kRead);
    // The ground map, from this frame's depth: emptied, the scene's pixels put in, the gaps filled.
    // At the resolve only (it reads the depth); its corner kept for the next early volume.
    HangStep((early ? 100 : 200) + 5);
    if (c.ground_on && !early) {
      ground_made_origin_[0] = c.ground_origin[0];
      ground_made_origin_[1] = c.ground_origin[1];
      ground_made_ = true;
      Transition(list, ground_[0].Get(), ground_state_[0], kWrite);
      Transition(list, ground_[1].Get(), ground_state_[1], kWrite);
      list->SetPipelineState(ground_clear_pso_.Get());
      list->SetComputeRootDescriptorTable(0, gpu(31));
      list->Dispatch(kGroundSize / 8, kGroundSize / 8, 1);
      D3D12_RESOURCE_BARRIER ub = {};
      ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
      ub.UAV.pResource = ground_[0].Get();
      list->ResourceBarrier(1, &ub);
      list->SetPipelineState(ms ? ground_scatter_ms_pso_.Get() : ground_scatter_pso_.Get());
      list->SetComputeRootDescriptorTable(0, gpu(36));
      list->Dispatch((w / 2 + 7) / 8, (h / 2 + 7) / 8, 1);
      Transition(list, ground_[0].Get(), ground_state_[0], kRead);
      list->SetPipelineState(ground_fill_pso_.Get());
      list->SetComputeRootDescriptorTable(0, gpu(41));
      list->Dispatch(kGroundSize / 8, kGroundSize / 8, 1);
      // Development: the filled map read back every 2 s (the copy made 2 s before, long done): the
      // cells seen or filled, the heights, the ground under the eye and the eye above it.
      static auto ground_last = std::chrono::steady_clock::now();
      if (const auto now = std::chrono::steady_clock::now();
          g_pgr4_dev && now - ground_last >= std::chrono::seconds(2)) {  // never in the release
        ground_last = now;
        if (!ground_readback_) {
          D3D12_HEAP_PROPERTIES rh = {};
          rh.Type = D3D12_HEAP_TYPE_READBACK;
          D3D12_RESOURCE_DESC rd = {};
          rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
          rd.Width = kGroundSize * kGroundSize * 4;
          rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
          rd.SampleDesc.Count = 1;
          rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
          if (Failed(device_->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &rd,
                                                      D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                      IID_PPV_ARGS(&ground_readback_)),
                     "volumetrics ground readback"))
            ground_readback_.Reset();
        } else if (ground_copied_) {
          void* mapped = nullptr;
          const D3D12_RANGE all_range = {0, size_t(kGroundSize) * kGroundSize * 4};
          if (SUCCEEDED(ground_readback_->Map(0, &all_range, &mapped))) {
            const float* v = static_cast<const float*>(mapped);
            uint32_t known = 0;
            float lo = 1e30f, hi = -1e30f;
            for (uint32_t i = 0; i < kGroundSize * kGroundSize; ++i)
              if (v[i] < 5e8f) {
                ++known;
                lo = std::min(lo, v[i]);
                hi = std::max(hi, v[i]);
              }
            const float under = v[(kGroundSize / 2) * kGroundSize + kGroundSize / 2];
            const D3D12_RANGE none = {0, 0};
            ground_readback_->Unmap(0, &none);
            REXLOG_INFO("PGR4 native volumetrics ground: {} of {} cells known, heights {:.1f}..{:.1f} m; "
                        "under the eye {}, the eye {:.2f} m (the plane under it: {:.2f} m)",
                        known, kGroundSize * kGroundSize, known ? lo : 0.0f, known ? hi : 0.0f,
                        under < 5e8f ? fmt::format("{:.2f} m", under) : std::string("unknown"),
                        c.eye[1], c.eye[1] - c.camera_height);
          }
        }
        if (ground_readback_) {
          Transition(list, ground_[1].Get(), ground_state_[1], D3D12_RESOURCE_STATE_COPY_SOURCE);
          D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
          dst.pResource = ground_readback_.Get();
          dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
          dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R32_FLOAT, kGroundSize, kGroundSize, 1,
                                           kGroundSize * 4};
          src.pResource = ground_[1].Get();
          src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
          list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
          ground_copied_ = true;
        }
      }
    }
    Transition(list, ground_[1].Get(), ground_state_[1], kRead);
    // The early volume reads the map made at the last resolve, at the corner it was made at.
    if (early && ground_made_) {
      if (ground_made_origin_[0] != c.ground_origin[0] || ground_made_origin_[1] != c.ground_origin[1])
        ++early_probe_.ground_shifted;
      c.ground_origin[0] = ground_made_origin_[0];
      c.ground_origin[1] = ground_made_origin_[1];
      list->SetComputeRoot32BitConstants(1, sizeof(c) / 4, &c, 0);
    }
    // The lights, the smoothing, MEDIA and VOLUME: once a frame (not again at the resolve after the
    // early pass).
    HangStep((early ? 100 : 200) + 6);
    if (!volume_done) {
    // The lights: the flares drawn since the last pass (the previous frame's: they are drawn after
    // it), the nearest within the range, brightest first by nothing (all the same lamp colour,
    // times the scene's scale after the fog, c12.y: the fog's own light is in those units).
    {
      // The volume smoothed over time (pgr4_native_volumetric_temporal): the previous frame's
      // camera (its eye, its view's axes, half extents and range), the blend over the seconds set
      // (1 - e^(-dt / s), real time: the same at any frame rate) and this frame's place in each
      // cell (Halton 2, 3, 5). No history (blend 1, the cells' middles) when off, at the start,
      // after a cut (the eye 30 m away) or when the cells were made again.
      {
        const float seconds = float(REXCVAR_GET(pgr4_native_volumetric_temporal));
        const double now_t = std::chrono::duration<double>(
                                 std::chrono::steady_clock::now().time_since_epoch()).count();
        const float dt_t = temporal_last_s_ > 0.0
                               ? float(std::clamp(now_t - temporal_last_s_, 0.0, 0.25))
                               : 0.0f;
        temporal_last_s_ = now_t;
        const float* pv = temporal_prev_;
        const float jx = c.eye[0] - pv[0], jy = c.eye[1] - pv[1], jz = c.eye[2] - pv[2];
        const bool valid = seconds > 0.0f && media_valid_ && pv[15] > 0.5f && dt_t > 0.0f &&
                           jx * jx + jy * jy + jz * jz < 900.0f && pv[14] == c.vol_range;
        if (seconds > 0.0f && !valid) ++temporal_resets_;
        const float blend = valid ? 1.0f - std::exp(-dt_t / seconds) : 1.0f;
        const auto halton = [](uint32_t i, uint32_t b) {
          float f = 1.0f, r = 0.0f;
          for (; i > 0; i /= b) {
            f /= float(b);
            r += f * float(i % b);
          }
          return r;
        };
        float hx = 0.5f, hy = 0.5f, hz = 0.5f;
        if (seconds > 0.0f) {
          ++temporal_count_;
          const uint32_t i = 1 + temporal_count_ % 1024;
          hx = halton(i, 2);
          hy = halton(i, 3);
          hz = halton(i, 5);
        }
        float* t = temporal_header_;
        for (int k = 0; k < 3; ++k) {
          t[k] = pv[k];           // eye
          t[4 + k] = pv[3 + k];   // east
          t[8 + k] = pv[6 + k];   // up
          t[12 + k] = pv[9 + k];  // north
        }
        t[3] = valid ? 1.0f : 0.0f;
        t[7] = blend;
        t[11] = hz;
        t[15] = hx;
        t[16] = pv[12];
        t[17] = pv[13];
        t[18] = pv[14];
        t[19] = hy;
        float* n = temporal_prev_;
        for (int k = 0; k < 3; ++k) {
          n[k] = c.eye[k];
          n[3 + k] = c.east_view[k];
          n[6 + k] = c.up_view[k];
          n[9 + k] = c.north_view[k];
        }
        n[12] = c.tan_half[0];
        n[13] = c.tan_half[1];
        n[14] = c.vol_range;
        n[15] = 1.0f;
      }
      Pgr4TakeFlareLights(flares_);
      // The centre: c51.xyz plus the fetched position times c51.w, the colour (draw.cpp, checked
      // against the lamps on a screenshot, 21/11/2026).
      flare_centres_.clear();
      for (const auto& f : flares_)
        flare_centres_.push_back({f[0] + f[4] * f[3], f[1] + f[5] * f[3], f[2] + f[6] * f[3], f[8],
                                  f[9], f[10]});
      const float range = float(REXCVAR_GET(pgr4_native_volumetric_light_range));
      const uint32_t want = REXCVAR_GET(pgr4_native_volumetric_lights)
                                ? uint32_t(std::clamp(REXCVAR_GET(pgr4_native_volumetric_light_count), 0, 64))
                                : 0u;
      // The lamps seen, kept (21/11/2026, Tazz: "the lights go on and off as I drive": the game
      // draws a flare only while its lamp is in view and not hidden, so the lamps came and went
      // with the view): each flare's lamp (within 0.75 m of a known one: the same) remembered while
      // within twice the range, a new one faded in over half a second of real time.
      // Its share of the light (pgr4_native_volumetric_light_smooth, 21/11/2026, Tazz: "the light in
      // the fog still blinks as I drive"): the nearest lamps taken, at most the count, but one
      // leaving them was cut at once and one taken again, seen long before, came back at full
      // light; now each lamp's weight moves towards 1 taken, 0 not, over half a second of real time
      // (any frame rate), a lamp taken ranked as at 0.9 of its distance (no swapping back and forth
      // of two lamps at the same distance). The buffer holds those fading out beyond the count.
      struct Lamp {
        float p[3];
        double first;
        float colour[3] = {1.0f, 1.0f, 1.0f};  // its flare's, its brightest channel 1
        float weight = 0.0f;      // its share now (smooth)
        float old_weight = 0.0f;  // the share the way before (the probe's comparison)
        bool taken = false;       // among the nearest last frame
        float ratio = -1.0f;      // the probe: its glow at its place through the columns, to the exact
        float ratio_min = 1e9f, ratio_max = -1e9f;  // ... over the probe's 2 s
        // The probe: its light along the ray through it summed over the slices, the fade at each
        // slice's middle (before) and at its point nearest the lamp (now), over the 2 s; and the
        // largest change from one frame to the next, against the change of its distance alone.
        float glow_before = -1.0f, glow_now = -1.0f;
        float before_min = 1e9f, before_max = -1e9f, now_min = 1e9f, now_max = -1e9f;
        float before_step = 0.0f, now_step = 0.0f;
      };
      static std::vector<Lamp> lamps;
      static double lamps_last_s = 0.0;
      const double now_s = std::chrono::duration<double>(
                               std::chrono::steady_clock::now().time_since_epoch()).count();
      const float dt = lamps_last_s > 0.0 ? float(std::clamp(now_s - lamps_last_s, 0.0, 0.25)) : 0.0f;
      lamps_last_s = now_s;
      const bool smooth = REXCVAR_GET(pgr4_native_volumetric_light_smooth);
      for (const auto& f : flare_centres_) {
        bool known = false;
        for (const Lamp& l : lamps) {
          const float ex = l.p[0] - f[0], ey = l.p[1] - f[1], ez = l.p[2] - f[2];
          if (ex * ex + ey * ey + ez * ez < 0.5625f) { known = true; break; }
        }
        if (!known && lamps.size() < 4096) {
          Lamp lamp;
          lamp.p[0] = f[0];
          lamp.p[1] = f[1];
          lamp.p[2] = f[2];
          lamp.first = now_s;
          const float top = std::max({f[3], f[4], f[5]});
          for (int k = 0; k < 3; ++k) lamp.colour[k] = top > 1e-4f ? f[3 + k] / top : 1.0f;
          lamps.push_back(lamp);
        }
      }
      std::erase_if(lamps, [&](const Lamp& l) {
        const float dx = l.p[0] - c.eye[0], dy = l.p[1] - c.eye[1], dz = l.p[2] - c.eye[2];
        return dx * dx + dy * dy + dz * dz > 4.0f * range * range;
      });
      std::vector<std::pair<float, uint32_t>> nearby;
      for (uint32_t i = 0; i < lamps.size(); ++i) {
        const float dx = lamps[i].p[0] - c.eye[0], dy = lamps[i].p[1] - c.eye[1],
                    dz = lamps[i].p[2] - c.eye[2];
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < range * range) nearby.emplace_back(smooth && lamps[i].taken ? 0.81f * d2 : d2, i);
      }
      std::sort(nearby.begin(), nearby.end());
      std::vector<uint8_t> taken(lamps.size(), 0);
      for (size_t k = 0; k < nearby.size() && k < want; ++k) taken[nearby[k].second] = 1;
      // The probe (every frame, logged every 2 s): a lamp in view and within the range whose share
      // jumped (more than 0.2 in a frame and more than the fade's own step), the way before and
      // now, and each such lamp's glow at its place as the columns sample it (the four column
      // centres' rays around its place, bilinear, against the ray through it).
      struct LampProbe {
        uint64_t frames = 0, taken_in = 0, taken_out = 0, jumps_before = 0, jumps_now = 0;
        float jump_before_nearest = 1e9f, jump_now_nearest = 1e9f, step_max = 0.0f;
        uint32_t sent_max = 0;
      };
      static LampProbe lp;
      ++lp.frames;
      const float fade_step = dt / 0.5f;
      const float core = float(REXCVAR_GET(pgr4_native_volumetric_light_core));
      const float reach = float(REXCVAR_GET(pgr4_native_volumetric_light_reach));
      // The volume's own lamps (LampLight, per pixel off): no widening over the columns, the fade
      // and thinning at each slice's point nearest the lamp (their settings' values, the settings
      // removed on 21/11/2026).
      const float filter = 0.0f;
      const bool nearest = true;
      // One column's angle (16 pixels of the scene's width over its view), as the VOLUME pass.
      const float column_angle = 32.0f * c.tan_half[0] / float(std::max(c.size[0], 1u));
      std::vector<std::pair<float, uint32_t>> send;  // (rank: the taken first, then by share), lamp
      for (uint32_t i = 0; i < lamps.size(); ++i) {
        Lamp& l = lamps[i];
        const float old_w = taken[i] ? float(std::clamp((now_s - l.first) / 0.5, 0.0, 1.0)) : 0.0f;
        const float new_w = taken[i] ? std::min(l.weight + fade_step, 1.0f)
                                     : std::max(l.weight - fade_step, 0.0f);
        // In view: its clip place inside the frame (a little margin).
        const float pc[4] = {l.p[0], l.p[1], l.p[2], 1.0f};
        float clip[4];
        for (int r = 0; r < 4; ++r)
          clip[r] = rows[4 * r] * pc[0] + rows[4 * r + 1] * pc[1] + rows[4 * r + 2] * pc[2] + rows[4 * r + 3];
        const bool in_view = clip[3] > 0.5f && std::abs(clip[0]) < 1.05f * clip[3] &&
                             std::abs(clip[1]) < 1.05f * clip[3];
        const float dx = l.p[0] - c.eye[0], dy = l.p[1] - c.eye[1], dz = l.p[2] - c.eye[2];
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        lp.taken_in += taken[i] && !l.taken;
        lp.taken_out += !taken[i] && l.taken;
        if (in_view && dist < range) {
          const float jump_before = std::abs(old_w - l.old_weight), jump_now = std::abs(new_w - l.weight);
          if (jump_before > 0.2f && jump_before > 2.0f * fade_step) {
            ++lp.jumps_before;
            lp.jump_before_nearest = std::min(lp.jump_before_nearest, dist);
          }
          if (jump_now > 0.2f && jump_now > 2.0f * fade_step) {
            ++lp.jumps_now;
            lp.jump_now_nearest = std::min(lp.jump_now_nearest, dist);
          }
          lp.step_max = std::max(lp.step_max, smooth ? jump_now : jump_before);
          // Its glow through the columns: view space place (x, y over w times the half extents,
          // w the depth), the four column centres around its pixel.
          const float ndc[2] = {clip[0] / clip[3], clip[1] / clip[3]};
          const float vx = ndc[0] * c.tan_half[0] * clip[3], vy = ndc[1] * c.tan_half[1] * clip[3],
                      vz = clip[3];
          const float foot = filter * dist * column_angle;
          const float k2 = core * core + foot * foot;
          const auto glow = [&](float h) {
            if (reach > 0.0f && h >= reach) return 0.0f;
            float window = 1.0f;
            if (reach > 0.0f) {
              const float x = h / reach;
              window = std::max(1.0f - x * x * x * x, 0.0f);
              window *= window;
            }
            return window / std::sqrt(h * h + k2);
          };
          const float px = (ndc[0] * 0.5f + 0.5f) * float(c.size[0]) / 16.0f - 0.5f;
          const float py = (0.5f - ndc[1] * 0.5f) * float(c.size[1]) / 16.0f - 0.5f;
          const float fx = std::floor(px), fy = std::floor(py);
          float sampled = 0.0f;
          for (int q = 0; q < 4; ++q) {
            const float cx = fx + float(q & 1), cy = fy + float(q >> 1);
            const float wq = ((q & 1) ? px - fx : 1.0f - (px - fx)) * ((q >> 1) ? py - fy : 1.0f - (py - fy));
            const float rx = ((cx + 0.5f) * 16.0f / float(c.size[0]) * 2.0f - 1.0f) * c.tan_half[0];
            const float ry = (1.0f - (cy + 0.5f) * 16.0f / float(c.size[1]) * 2.0f) * c.tan_half[1];
            const float rl = std::sqrt(rx * rx + ry * ry + 1.0f);
            const float t = (vx * rx + vy * ry + vz) / rl;
            const float h2 = std::max(vx * vx + vy * vy + vz * vz - t * t, 0.0f);
            sampled += wq * glow(std::sqrt(h2));
          }
          const float exact = glow(0.0f);
          // Its light down the ray through it, slice by slice as the VOLUME pass (the fog's
          // thinning and density left out: the same for both ways).
          {
            const float rl = std::sqrt(ndc[0] * c.tan_half[0] * ndc[0] * c.tan_half[0] +
                                       ndc[1] * c.tan_half[1] * ndc[1] * c.tan_half[1] + 1.0f);
            const float t0 = vz * rl;
            const float kk = std::sqrt(k2);
            // The cone (as the header's lights[2]) and the ray's world up (the ray through the lamp).
            const float cone_outer = float(REXCVAR_GET(pgr4_native_volumetric_light_cone)) * 0.0174533f;
            const float cone_cos_outer = std::cos(cone_outer);
            const float cone_cos_inner = std::max(
                std::cos(std::max(cone_outer - float(REXCVAR_GET(pgr4_native_volumetric_light_cone_soft)) *
                                                   0.0174533f, 0.0f)),
                cone_cos_outer + 1e-4f);
            const float cone_around = float(REXCVAR_GET(pgr4_native_volumetric_light_cone_around));
            const float ray_up = dy / std::max(dist, 1e-4f);
            const float growth = std::log(c.vol_range / 0.5f) / 64.0f;
            const auto fade_at = [&](float dd) {
              if (reach > 0.0f) {
                const float x = std::min(dd / reach, 1.0f);
                const float wnd = std::max(1.0f - x * x * x * x, 0.0f);
                return wnd * wnd;
              }
              return std::clamp(1.0f - dd / range, 0.0f, 1.0f);
            };
            float before = 0.0f, now_sum = 0.0f, z0 = 0.0f;
            for (int s = 0; s < 64; ++s) {
              const float z1 = 0.5f * std::exp(growth * float(s + 1));
              const float ta = z0 * rl, tb = z1 * rl;
              const float along = (std::atan((tb - t0) / kk) - std::atan((ta - t0) / kk)) / kk;
              before += along * fade_at(std::abs(0.5f * (ta + tb) - t0));
              if (cone_outer > 0.0f) {
                // As the VOLUME pass with the cone: four parts of equal light, the cone and the
                // fade at each one's middle (the ray through the lamp: up or down it).
                const float ua = std::atan((ta - t0) / kk), ub = std::atan((tb - t0) / kk);
                float part = 0.0f;
                for (int q = 0; q < 4; ++q) {
                  const float ts = t0 + kk * std::tan(ua + (ub - ua) * (float(q) + 0.5f) * 0.25f);
                  const float down = (ts >= t0 ? -1.0f : 1.0f) * ray_up;
                  const float x = std::clamp((down - cone_cos_outer) / (cone_cos_inner - cone_cos_outer), 0.0f, 1.0f);
                  part += 0.25f * (cone_around + (1.0f - cone_around) * x * x * (3.0f - 2.0f * x)) *
                          fade_at(std::abs(ts - t0));
                }
                now_sum += along * part;
              } else {
                now_sum += along * fade_at(std::abs(std::clamp(t0, ta, tb) - t0));
              }
              z0 = z1;
            }
            if (l.glow_before > 0.0f)
              l.before_step = std::max(l.before_step, std::abs(before - l.glow_before) /
                                                          std::max(l.glow_before, 1e-6f));
            if (l.glow_now > 0.0f)
              l.now_step = std::max(l.now_step, std::abs(now_sum - l.glow_now) /
                                                    std::max(l.glow_now, 1e-6f));
            l.glow_before = before;
            l.glow_now = now_sum;
            l.before_min = std::min(l.before_min, before);
            l.before_max = std::max(l.before_max, before);
            l.now_min = std::min(l.now_min, now_sum);
            l.now_max = std::max(l.now_max, now_sum);
          }
          l.ratio = exact > 0.0f ? sampled / exact : -1.0f;
          if (l.ratio >= 0.0f) {
            l.ratio_min = std::min(l.ratio_min, l.ratio);
            l.ratio_max = std::max(l.ratio_max, l.ratio);
          }
        }
        l.old_weight = old_w;
        l.weight = new_w;
        l.taken = taken[i] != 0;
        const float share = smooth ? new_w : old_w;
        if (share > 0.0f) send.emplace_back(taken[i] ? 2.0f - share : 1.0f - share, i);
      }
      std::sort(send.begin(), send.end());
      if (send.size() > kMaxLights) send.resize(kMaxLights);
      lp.sent_max = std::max<uint32_t>(lp.sent_max, uint32_t(send.size()));
      lamps_sent_ = uint32_t(send.size());
      const float scale = fog_c12 > 0.0f && std::isfinite(fog_c12) ? fog_c12 : 1.0f;
      float* out = reinterpret_cast<float*>(lights_mapped_ + size_t(frame % D3D12Device::kFramesInFlight) *
                                                                  kLightElements * 16);
      out[0] = float(send.size());
      out[1] = range;
      out[2] = float(REXCVAR_GET(pgr4_native_volumetric_light_strength)) * scale;
      out[3] = float(REXCVAR_GET(pgr4_native_volumetric_light_phase));
      const float colour[3] = {float(REXCVAR_GET(pgr4_native_volumetric_light_red)),
                               float(REXCVAR_GET(pgr4_native_volumetric_light_green)),
                               float(REXCVAR_GET(pgr4_native_volumetric_light_blue))};
      out[4] = reach;
      out[5] = REXCVAR_GET(pgr4_native_volumetric_light_over_bulb) ? 1.0f : 0.0f;
      out[6] = filter;
      out[7] = nearest ? 1.0f : 0.0f;
      // The cone: cos of the outer and inner half angles (down from the lamp), the share outside.
      {
        const float outer = float(REXCVAR_GET(pgr4_native_volumetric_light_cone)) * 0.0174533f;
        const float inner = std::max(outer - float(REXCVAR_GET(pgr4_native_volumetric_light_cone_soft)) *
                                                 0.0174533f,
                                     0.0f);
        out[8] = std::cos(outer);
        out[9] = std::max(std::cos(inner), out[8] + 1e-4f);
        out[10] = float(REXCVAR_GET(pgr4_native_volumetric_light_cone_around));
        out[11] = outer > 0.0f ? 1.0f : 0.0f;
      }
      // Per pixel (the apply), with a reach only.
      out[12] = REXCVAR_GET(pgr4_native_volumetric_light_per_pixel) && reach > 0.0f ? 1.0f : 0.0f;
      out[13] = float(REXCVAR_GET(pgr4_native_volumetric_light_cone_top));
      out[14] = float(REXCVAR_GET(pgr4_native_volumetric_light_texture));
      out[15] = float(REXCVAR_GET(pgr4_native_volumetric_light_texture_size));
      // 4-8: the previous frame's camera, the blend, the jitter (MEDIA).
      std::memcpy(out + 16, temporal_header_, sizeof(temporal_header_));
      // 9: the lamps' halo, the ground mist's wisps.
      out[36] = float(REXCVAR_GET(pgr4_native_volumetric_light_halo));
      out[37] = float(REXCVAR_GET(pgr4_native_volumetric_light_halo_size));
      out[38] = float(REXCVAR_GET(pgr4_native_volumetric_mist_detail));
      out[39] = float(REXCVAR_GET(pgr4_native_volumetric_mist_detail_size));
      // 10: the apply probe's pixel.
      {
        float pu = 0.0f, pv = 0.0f;
        const bool on = std::sscanf(REXCVAR_GET(pgr4_native_volumetric_probe_uv).c_str(), "%f %f", &pu,
                                    &pv) == 2 && probe_;
        out[40] = std::floor(std::clamp(pu, 0.0f, 1.0f) * float(w - 1) + 0.5f);
        out[41] = std::floor(std::clamp(pv, 0.0f, 1.0f) * float(h - 1) + 0.5f);
        grid_on_ = REXCVAR_GET(pgr4_native_volumetric_probe_grid) && probe_ && g_pgr4_dev;
        out[42] = (on ? 1.0f : 0.0f) + (grid_on_ ? 2.0f : 0.0f);
        out[43] = 0.0f;  // (the fading scenery's mark: removed 21/11/2026)
        probe_pixel_[0] = out[40];
        probe_pixel_[1] = out[41];
        probe_on_ = on;
      }
      for (uint32_t j = 0; j < send.size(); ++j) {
        const Lamp& lamp = lamps[send[j].second];
        const float share = smooth ? lamp.weight : lamp.old_weight;
        float* l = out + 4 * kLightHeaders + 8 * j;
        l[0] = lamp.p[0];
        l[1] = lamp.p[1];
        l[2] = lamp.p[2];
        l[3] = core * core;
        const bool own = REXCVAR_GET(pgr4_native_volumetric_light_flare_colour);
        l[4] = colour[0] * (own ? lamp.colour[0] : 1.0f) * share;
        l[5] = colour[1] * (own ? lamp.colour[1] : 1.0f) * share;
        l[6] = colour[2] * (own ? lamp.colour[2] : 1.0f) * share;
        l[7] = 0.0f;
      }
      // Development: the flares taken and the lights kept, the nearest, every 2 s.
      static auto lights_last = std::chrono::steady_clock::now();
      if (const auto now = std::chrono::steady_clock::now();
          g_pgr4_dev && now - lights_last >= std::chrono::seconds(2)) {  // never in the release
        lights_last = now;
        // The camera (world to clip rows) and the flares' raw centres, to place them on a screenshot.
        {
          std::string rows_text;
          for (int k = 0; k < 16; ++k) rows_text += fmt::format(" {:.6g}", rows[k]);
          std::string raw_text;
          for (uint32_t i = 0; i < std::min<size_t>(flares_.size(), 60); ++i)
            raw_text += fmt::format(" ({:.1f} {:.1f} {:.1f} {:.6f}: {:.1f} {:.1f} {:.1f} {:.1f})", flares_[i][0], flares_[i][1], flares_[i][2], flares_[i][3], flares_[i][4], flares_[i][5], flares_[i][6], flares_[i][7]);
          REXLOG_INFO("PGR4 native volumetrics lights camera: rows{}; tan {:.5f} {:.5f}; eye {:.2f} {:.2f} "
                      "{:.2f}; flares{}",
                      rows_text, c.tan_half[0], c.tan_half[1], c.eye[0], c.eye[1], c.eye[2], raw_text);
        }
        {
          std::map<std::array<int, 3>, uint32_t> colours;
          for (const auto& [rank, i] : send)
            ++colours[{int(lamps[i].colour[0] * 100.0f + 0.5f), int(lamps[i].colour[1] * 100.0f + 0.5f),
                       int(lamps[i].colour[2] * 100.0f + 0.5f)}];
          std::string text;
          for (const auto& [rgb, n] : colours)
            text += fmt::format(" {} at {:.2f} {:.2f} {:.2f};", n, rgb[0] / 100.0f, rgb[1] / 100.0f,
                                rgb[2] / 100.0f);
          REXLOG_INFO("PGR4 native volumetrics lights colours (their flares', {}):{}",
                      REXCVAR_GET(pgr4_native_volumetric_light_flare_colour) ? "on" : "off", text);
        }
        REXLOG_INFO("PGR4 native volumetrics lights: {} flares taken, {} within {:.0f} m, {} lamps sent "
                    "(strength {:.1f} x c12.y {:.2f}, reach {:.0f} m, core {:.1f} m, over the bulb {}, "
                    "cone {:.0f} deg soft {:.0f} outside {:.2f} tip {:.2f} m up, per pixel {}); "
                    "the nearest {}",
                    flare_centres_.size(), nearby.size(), range, send.size(), out[2] / scale, scale,
                    out[4], core, out[5] > 0.5f ? "on" : "off",
                    REXCVAR_GET(pgr4_native_volumetric_light_cone),
                    REXCVAR_GET(pgr4_native_volumetric_light_cone_soft),
                    REXCVAR_GET(pgr4_native_volumetric_light_cone_around), out[13],
                    out[12] > 0.5f ? "on" : "off",
                    nearby.empty() ? std::string("none")
                                   : fmt::format("{:.1f} m away", std::sqrt(nearby.front().first)));
        // The flicker: the shares' jumps, the way before and now, and the columns' sampling of each
        // lamp in view (its glow at its place over the exact one, 1 if sampled right; the spread
        // over 2 s for the lamp it moved most: the twinkle as it slides between column centres).
        float spread = 0.0f, spread_lo = 0.0f, spread_hi = 0.0f, spread_dist = 0.0f;
        uint32_t twinkling = 0, sampled_lamps = 0;
        for (Lamp& l : lamps) {
          if (l.ratio_max < 0.0f) continue;
          ++sampled_lamps;
          const float s = l.ratio_max - l.ratio_min;
          if (s > 0.25f) ++twinkling;
          if (s > spread) {
            spread = s;
            spread_lo = l.ratio_min;
            spread_hi = l.ratio_max;
            const float dx = l.p[0] - c.eye[0], dy = l.p[1] - c.eye[1], dz = l.p[2] - c.eye[2];
            spread_dist = std::sqrt(dx * dx + dy * dy + dz * dz);
          }
          l.ratio_min = 1e9f;
          l.ratio_max = -1e9f;
        }
        // The pumping: the lamp in view whose light (down the ray through it) went up and down
        // most over the 2 s, (max - min) / max, the slices' middle way and the nearest point way,
        // and the largest change in one frame.
        float pump_before = 0.0f, pump_now = 0.0f, step_before = 0.0f, step_now = 0.0f;
        float pump_dist = 0.0f;
        uint32_t pumping_before = 0, pumping_now = 0;
        for (Lamp& l : lamps) {
          if (l.before_max > 0.0f) {
            const float pb = (l.before_max - l.before_min) / l.before_max;
            const float pn = l.now_max > 0.0f ? (l.now_max - l.now_min) / l.now_max : 0.0f;
            pumping_before += pb > 0.2f;
            pumping_now += pn > 0.2f;
            if (pb > pump_before) {
              pump_before = pb;
              const float dx = l.p[0] - c.eye[0], dy = l.p[1] - c.eye[1], dz = l.p[2] - c.eye[2];
              pump_dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            }
            pump_now = std::max(pump_now, pn);
            step_before = std::max(step_before, l.before_step);
            step_now = std::max(step_now, l.now_step);
          }
          l.before_min = l.now_min = 1e9f;
          l.before_max = l.now_max = -1e9f;
          l.before_step = l.now_step = 0.0f;
          l.glow_before = l.glow_now = -1.0f;
        }
        REXLOG_INFO("PGR4 native volumetrics lights pumping: nearest {}; lamps in view going up and "
                    "down over 20 % in 2 s: the slices' middle way {} (worst {:.0f} % at {:.0f} m, "
                    "a frame up to {:.0f} %), the nearest point way {} (worst {:.0f} %, a frame up to "
                    "{:.0f} %){}",
                    nearest ? "on" : "off", pumping_before, 100.0f * pump_before, pump_dist,
                    100.0f * step_before, pumping_now, 100.0f * pump_now, 100.0f * step_now,
                    (nearest ? pumping_now : pumping_before) > 0 ? " PUMPING" : "");
        const bool fault = lp.jumps_now > 0 || twinkling > 0;
        REXLOG_INFO("PGR4 native volumetrics lights flicker: {} frames, smooth {}, filter {:.2f} "
                    "column(s) ({:.3f} m a column at 100 m); taken in {} / out {}, sent at most {}; "
                    "jumps in view: the way before {} (nearest {}), now {} (nearest {}), largest step "
                    "{:.3f}; columns: {} lamps in view, {} twinkling (spread over 0.25), the worst "
                    "{:.2f} to {:.2f} at {:.0f} m{}",
                    lp.frames, smooth ? "on" : "off", filter, 100.0f * column_angle, lp.taken_in,
                    lp.taken_out, lp.sent_max, lp.jumps_before,
                    lp.jumps_before ? fmt::format("{:.0f} m", lp.jump_before_nearest) : std::string("-"),
                    lp.jumps_now,
                    lp.jumps_now ? fmt::format("{:.0f} m", lp.jump_now_nearest) : std::string("-"),
                    lp.step_max, sampled_lamps, twinkling, spread_lo, spread_hi, spread_dist,
                    fault ? " FLICKER" : "");
        lp = LampProbe{};
      }
    }
    HangStep((early ? 100 : 200) + 7);
    // The cells' fog (MEDIA): this frame's written, the previous frame's read (both UAVs), then
    // this frame's read by VOLUME.
    Transition(list, media_[media_now_].Get(), media_state_[media_now_], kWrite);
    Transition(list, media_[1 - media_now_].Get(), media_state_[1 - media_now_], kWrite);
    list->SetPipelineState(media_pso_.Get());
    list->SetComputeRootDescriptorTable(0, gpu(53));
    list->Dispatch((columns_x + 3) / 4, (columns_y + 3) / 4, kSlices / 4);
    Transition(list, media_[media_now_].Get(), media_state_[media_now_], kRead);
    media_valid_ = true;
    Transition(list, volume_.Get(), volume_state_, kWrite);
    Transition(list, range_.Get(), range_state_, kWrite);
    list->SetPipelineState(volume_pso_.Get());
    list->SetComputeRootDescriptorTable(0, gpu(23));
    list->Dispatch((columns_x + 7) / 8, (columns_y + 7) / 8, 1);
    Transition(list, volume_.Get(), volume_state_, kRead);
    Transition(list, range_.Get(), range_state_, kRead);
    HangStep((early ? 100 : 200) + 8);
    // Shared with the transparent draws after it (fog_volume_hooks.hlsli's pgr4_fog_volume).
    const float growth = std::log(c.vol_range / 0.5f) / float(kSlices);
    const float values[16] = {c.vol_range, growth, float(columns_x), float(columns_y),
                              c.tan_half[0], c.tan_half[1], float(w), float(h),
                              c.fog_distance, c.fog_ceiling, c.fog_density, c.fog_falloff,
                              c.fog_height_on ? 1.0f : 0.0f, c.fog_exp2 * 0.69314718f, c.eye[1],
                              c.replace && REXCVAR_GET(pgr4_native_volumetric_transparents) ? 1.0f
                                                                                            : 0.0f};
    std::memcpy(share_.values, values, sizeof(values));
    share_.volume = volume_.Get();
    share_.range = range_.Get();
    // SystemConstants::fog_shader (dxbc_translator.h): the volume's range, ln 2 / growth, columns;
    // near x far, far - near, near, the first edge; the fog's color in the volume's units
    // (c10.rgb x c12.y as applied), 1 / c12.y; c10.rgb, the volume made.
    {
      float* s = share_.shader;
      const float inv_c12 =
          REXCVAR_GET(pgr4_native_volumetric_fog_c12) && fog_c12 > 0.0f && std::isfinite(fog_c12)
              ? 1.0f / fog_c12
              : 1.0f;
      s[0] = c.vol_range;
      s[1] = 0.69314718f / growth;
      s[2] = float(columns_x);
      s[3] = float(columns_y);
      s[4] = c.near_plane * c.far_plane;
      s[5] = c.far_plane - c.near_plane;
      s[6] = c.near_plane;
      s[7] = 0.5f * std::exp(growth);
      s[8] = c.fog_color[0];
      s[9] = c.fog_color[1];
      s[10] = c.fog_color[2];
      s[11] = inv_c12;
      for (int k = 0; k < 3; ++k) s[12 + k] = g_pgr4_fog_c10[k].load(std::memory_order_relaxed);
      s[15] = c.replace ? 1.0f : 0.0f;
    }
    share_.frame = frame;
    volume_frame_ = frame;
    }  // !volume_done
  }
  // The early pass: the volume only. Its probe: early or at the resolve, the eye then and at the
  // resolve, the ground map's corner (every 2 s, at the resolve).
  HangStep((early ? 100 : 200) + 9);
  if (early) {
    ++early_probe_.early;
    // The grid probe's record of this frame's volume inputs.
    {
      static_assert(sizeof(Constants) == sizeof(VolumeInputs::constants));
      VolumeInputs& vi = volume_inputs_[frame % 8];
      vi.frame = frame;
      vi.before = g_pgr4_fog_order_frame[frame % 8] == frame ? g_pgr4_fog_order_before[frame % 8] : 0;
      std::memcpy(vi.constants, &c, sizeof(c));
      vi.flares = uint32_t(flares_.size());
      vi.lamps = lamps_sent_;
      vi.media_now = media_now_;
      vi.ground_made = ground_made_;
    }
    for (int k = 0; k < 3; ++k) early_probe_.early_eye[k] = c.eye[k];
    early_probe_.early_frame = frame;
    return true;  // its descriptor heap, root signature and pipelines set on the list
  }
  {
    EarlyProbe& ep = early_probe_;
    ++ep.frames;
    if (volume_done && ep.early_frame == frame) {
      float gap = 0.0f;
      for (int k = 0; k < 3; ++k) gap = std::max(gap, std::abs(c.eye[k] - ep.early_eye[k]));
      ep.eye_gap_max = std::max(ep.eye_gap_max, gap);
    } else {
      ++ep.late;
    }
    static auto early_last = std::chrono::steady_clock::now();
    if (const auto now = std::chrono::steady_clock::now();
        g_pgr4_dev && now - early_last >= std::chrono::seconds(2)) {  // never in the release
      early_last = now;
      REXLOG_INFO("PGR4 volumetric volume early ({}): {} frames, made early in {}, at the resolve in {} "
                  "(no fog draw before it); the eye early against at the resolve apart by up to {:.3f} m; "
                  "the ground map read at the last resolve's corner, another than the eye's in {}",
                  REXCVAR_GET(pgr4_native_volumetric_volume_early) ? "on" : "off", ep.frames, ep.early,
                  ep.late, ep.eye_gap_max, ep.ground_shifted);
      ep = EarlyProbe{};
    }
  }

  // The fog laid again and the light added, a full-screen triangle into the scene's target.
  if (timer) timer->Pass(list, GpuTimer::KeyEffects(6));
  ID3D12PipelineState* pso = ApplyPipeline(color->view_format, color->samples, debug);
  HangStep((early ? 100 : 200) + 10);
  if (!pso) return true;
  targets.PrepareRead(list, color, all);
  // The grid probe: the scene's colour at its points before the apply (as the shaders left it).
  ID3D12PipelineState* grid_pso = color->samples > 1 ? grid_ms_pso_.Get() : grid_pso_.Get();
  const bool grid = grid_on_ && probe_ && grid_pso && grid_readback_[0] &&
                    grid_readback_[1] && grid_readback_[2];
  HangStep((early ? 100 : 200) + 11);
  if (grid) {
    Transition(list, color->resource.Get(), color->state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    list->SetComputeRootSignature(compute_signature_.Get());
    list->SetPipelineState(grid_pso);
    list->SetComputeRootDescriptorTable(0, gpu(58));
    list->SetComputeRoot32BitConstants(1, sizeof(c) / 4, &c, 0);
    list->Dispatch(1, 1, 1);
  }
  Transition(list, color->resource.Get(), color->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
  list->OMSetRenderTargets(1, &color->view, FALSE, nullptr);
  const D3D12_VIEWPORT vp = {0.0f, 0.0f, float(w), float(h), 0.0f, 1.0f};
  const D3D12_RECT scissor = {0, 0, LONG(w), LONG(h)};
  list->RSSetViewports(1, &vp);
  list->RSSetScissorRects(1, &scissor);
  list->SetGraphicsRootSignature(apply_signature_.Get());
  list->SetPipelineState(pso);
  list->SetGraphicsRootDescriptorTable(0, gpu(46));  // 46-50
  // The fog laid by the shaders this frame (InShader, the volume made): replace 2, the pass lays
  // only the lamps, the sun-lit air and the sky.
  if (c.replace && InShader() && volume_frame_ == frame) c.replace = 2;
  list->SetGraphicsRoot32BitConstants(1, sizeof(c) / 4, &c, 0);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  list->DrawInstanced(3, 1, 0, 0);
  g_pgr4_volumetrics_applies.fetch_add(1, std::memory_order_relaxed);  HangStep((early ? 100 : 200) + 12);
  if (grid) GridProbe(list, frame, c.eye);
  HangStep((early ? 100 : 200) + 13);
  // The apply probe (pgr4_native_volumetric_probe_uv): every 2 s, the values read back from the
  // copy made 2 s before (long done), then a new copy.
  if (probe_ && probe_readback_ && probe_on_) {
    static auto probe_last = std::chrono::steady_clock::now();
    if (const auto now = std::chrono::steady_clock::now(); now - probe_last >= std::chrono::seconds(2)) {
      probe_last = now;
      if (probe_copied_) {
        void* mapped = nullptr;
        const D3D12_RANGE all_range = {0, 128};
        if (SUCCEEDED(probe_readback_->Map(0, &all_range, &mapped))) {
          const float* p = static_cast<const float*>(mapped);
          REXLOG_INFO("PGR4 native volumetrics probe pixel {:.0f} {:.0f} ({}): depth raw {:.7f} (samples "
                      "{:.0f}: {:.7f} {:.7f} {:.7f} {:.7f}), {:.2f} m along the ray, view depth {:.2f}{}; "
                      "the game's T {:.4f}, ours {:.4f} (the volume's {:.4f}), ratio kept {:.4f}, keep "
                      "{:.4f}; added {:.4f} {:.4f} {:.4f}, the volume's light {:.4f} {:.4f} {:.4f}; "
                      "stencil {:.0f}{}",
                      probe_pixel_[0], probe_pixel_[1], REXCVAR_GET(pgr4_native_volumetric_probe_uv),
                      p[0], p[3], p[12], p[13], p[14], p[15], p[19], p[1], p[2] > 0.5f ? " (sky)" : "",
                      p[4], p[5], p[11], p[6], p[7], p[8], p[9], p[10], p[16], p[17], p[18], p[20],
                      p[21] > 0.5f ? " (marked: the game's fog kept)" : "");
          const D3D12_RANGE none = {0, 0};
          probe_readback_->Unmap(0, &none);
        }
      }
      D3D12_RESOURCE_BARRIER b = {};
      b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      b.Transition.pResource = probe_.Get();
      b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
      b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
      list->ResourceBarrier(1, &b);
      list->CopyBufferRegion(probe_readback_.Get(), 0, probe_.Get(), 0, 128);
      std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
      list->ResourceBarrier(1, &b);
      probe_copied_ = true;
    }
  }
  return true;
}

// Development (21/11/2026): two frames' volume inputs (the grid probe's flashes): the draws before
// the volume, the flares, the lamps, the cells' fog slot and the ground map always; the constants
// that changed (over 1e-5, or 0.1 % of the larger), by name.
std::string Volumetrics::VolumeInputsChanged(uint64_t a_frame, uint64_t b_frame) const {
  const VolumeInputs& a = volume_inputs_[a_frame % 8];
  const VolumeInputs& b = volume_inputs_[b_frame % 8];
  if (a.frame != a_frame || b.frame != b_frame) return " (not recorded)";
  std::string out = fmt::format(": draws before it {} -> {}, flares {} -> {}, lamps {} -> {}, cells' slot "
                                "{} -> {}, ground map {} -> {}; changed:",
                                a.before, b.before, a.flares, b.flares, a.lamps, b.lamps, a.media_now,
                                b.media_now, a.ground_made, b.ground_made);
  Constants ca, cb;
  std::memcpy(&ca, a.constants, sizeof(ca));
  std::memcpy(&cb, b.constants, sizeof(cb));
  struct Field {
    const char* name;
    size_t at;
    int count;
    bool integer;
  };
  static const Field kFields[] = {
      {"size", offsetof(Constants, size), 2, true},
      {"near", offsetof(Constants, near_plane), 1, false},
      {"far", offsetof(Constants, far_plane), 1, false},
      {"tan_half", offsetof(Constants, tan_half), 2, false},
      {"sun_view", offsetof(Constants, sun_view), 3, false},
      {"sigma", offsetof(Constants, sigma), 1, false},
      {"up_view", offsetof(Constants, up_view), 3, false},
      {"camera_height", offsetof(Constants, camera_height), 1, false},
      {"sun_color", offsetof(Constants, sun_color), 3, false},
      {"strength", offsetof(Constants, strength), 1, false},
      {"east_view", offsetof(Constants, east_view), 3, false},
      {"north_view", offsetof(Constants, north_view), 3, false},
      {"time", offsetof(Constants, time), 1, false},
      {"eye", offsetof(Constants, eye), 3, false},
      {"fog_exp2", offsetof(Constants, fog_exp2), 1, false},
      {"fog_color", offsetof(Constants, fog_color), 3, false},
      {"fog_distance", offsetof(Constants, fog_distance), 1, false},
      {"fog_ceiling", offsetof(Constants, fog_ceiling), 1, false},
      {"fog_density", offsetof(Constants, fog_density), 1, false},
      {"fog_falloff", offsetof(Constants, fog_falloff), 1, false},
      {"fog_height_on", offsetof(Constants, fog_height_on), 1, true},
      {"variation", offsetof(Constants, variation), 1, false},
      {"noise_scale", offsetof(Constants, noise_scale), 1, false},
      {"fog_sun", offsetof(Constants, fog_sun), 1, false},
      {"replace", offsetof(Constants, replace), 1, true},
      {"wind", offsetof(Constants, wind), 2, false},
      {"fog_g", offsetof(Constants, fog_g), 1, false},
      {"mist_density", offsetof(Constants, mist_density), 1, false},
      {"mist_height", offsetof(Constants, mist_height), 1, false},
      {"mist_distance", offsetof(Constants, mist_distance), 1, false},
      {"vol_range", offsetof(Constants, vol_range), 1, false},
      {"ground_origin", offsetof(Constants, ground_origin), 2, false},
      {"ground_on", offsetof(Constants, ground_on), 1, true},
  };
  const auto* pa = reinterpret_cast<const uint8_t*>(&ca);
  const auto* pb = reinterpret_cast<const uint8_t*>(&cb);
  bool any = false;
  for (const Field& f : kFields) {
    bool differs = false;
    std::string va, vb;
    for (int k = 0; k < f.count; ++k) {
      if (f.integer) {
        uint32_t x, y;
        std::memcpy(&x, pa + f.at + 4 * k, 4);
        std::memcpy(&y, pb + f.at + 4 * k, 4);
        differs |= x != y;
        va += fmt::format(" {}", x);
        vb += fmt::format(" {}", y);
      } else {
        float x, y;
        std::memcpy(&x, pa + f.at + 4 * k, 4);
        std::memcpy(&y, pb + f.at + 4 * k, 4);
        differs |= !(std::abs(x - y) <= std::max(1e-5f, 1e-3f * std::max(std::abs(x), std::abs(y))));
        va += fmt::format(" {:.5g}", x);
        vb += fmt::format(" {:.5g}", y);
      }
    }
    if (differs) {
      out += fmt::format(" {}{} ->{};", f.name, va, vb);
      any = true;
    }
  }
  if (!any) out += " none;";
  return out;
}

// Development (21/11/2026): the grid probe (pgr4_native_volumetric_probe_grid), after the apply
// each frame. The slot copied two frames ago is read (the GPU done with it: kFramesInFlight 2, as
// the lamps' ring), its points against those of the frame before it: on the same surface (depth
// within 2 % and 5 cm, not the sky), a colour whose luminance moves by over 0.03 and 35 % from one
// frame to the next is a flash; a frame with four or more is logged with each point's values in
// both frames and the fog-laying draws before and after the volume in both (draw.cpp). Then this
// frame's points copied. Every 2 s the counts, and the screen's centre point now.
void Volumetrics::GridProbe(ID3D12GraphicsCommandList* list, uint64_t frame, const float eye[3]) {
  static_assert(sizeof(GridPoint) == kGridStride * 16, "the HLSL's layout");
  constexpr uint32_t kPoints = kGridX * kGridY;
  const uint32_t read = uint32_t((frame + 1) % 3), write = uint32_t(frame % 3);
  struct Stats {
    uint64_t frames = 0, compared = 0, flashes = 0, darker = 0, brighter = 0, flash_frames = 0,
             order_changed = 0, still_frames = 0, colour_jumps = 0;
    int logged_still = 0, logged_moving = 0;
    std::string events;
  };
  static Stats st;
  static auto last = std::chrono::steady_clock::now();
  const auto lum = [](const float* c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; };
  const auto order = [](uint64_t f, bool after) -> std::string {
    const uint32_t s = uint32_t(f % 8);
    if (g_pgr4_fog_order_frame[s] != f) return "?";
    return std::to_string(after ? g_pgr4_fog_order_after[s] : g_pgr4_fog_order_before[s]);
  };
  if (grid_written_[read] != UINT64_MAX && grid_written_[read] + 2 <= frame) {
    void* mapped = nullptr;
    const D3D12_RANGE range = {0, kGridBytes};
    if (SUCCEEDED(grid_readback_[read]->Map(0, &range, &mapped))) {
      static GridPoint now[kPoints];
      std::memcpy(now, mapped, sizeof(now));
      const D3D12_RANGE none = {0, 0};
      grid_readback_[read]->Unmap(0, &none);
      const uint64_t f = grid_written_[read];
      const float* e = grid_eye_[read];
      if (grid_prev_frame_ != UINT64_MAX && grid_prev_frame_ + 1 == f) {
        ++st.frames;
        float moved = 0.0f;
        for (int k = 0; k < 3; ++k) moved += (e[k] - grid_prev_eye_[k]) * (e[k] - grid_prev_eye_[k]);
        moved = std::sqrt(moved);
        uint32_t n = 0;
        std::string points;
        for (uint32_t i = 0; i < kPoints; ++i) {
          const GridPoint& a = grid_prev_[i];
          const GridPoint& b = now[i];
          if (a.depth[2] > 0.5f || b.depth[2] > 0.5f) continue;
          const float da = a.depth[1], db = b.depth[1];
          if (!(std::abs(da - db) <= 0.02f * std::max(da, db) + 0.05f)) continue;
          ++st.compared;
          // The colour's jumps, counted only: the scene's colour before the upscaler moves with its
          // sub-pixel jitter (21/11/2026: 1.24, 1.75, 1.22 on one point, the eye still, the fog the
          // same), so they are not the fog's.
          const float la = lum(a.colour), lb = lum(b.colour), d = std::abs(lb - la);
          if (d > 0.03f && d > 0.35f * std::max(std::min(la, lb), 0.02f)) {
            ++st.colour_jumps;
            ++(lb < la ? st.darker : st.brighter);
          }
          // The fog's: on the same surface (the game's T within 0.01), our transmittance moving by
          // over 0.03 or the volume's light by over 0.03 and 15 %.
          if (!(std::abs(a.fog[0] - b.fog[0]) <= 0.01f)) continue;
          const float va = lum(a.volume), vb = lum(b.volume), dv = std::abs(vb - va);
          if (!(std::abs(a.fog[1] - b.fog[1]) > 0.03f || (dv > 0.03f && dv > 0.15f * std::max(va, vb))))
            continue;
          ++n;
          if (points.size() < 2400) {
            points += fmt::format(" | point {},{} at {:.1f} m: colour {:.3f} {:.3f} {:.3f} -> {:.3f} {:.3f} {:.3f}",
                                  i % kGridX, i / kGridX, db, a.colour[0], a.colour[1], a.colour[2],
                                  b.colour[0], b.colour[1], b.colour[2]);
            points += fmt::format("; the game's T {:.3f} -> {:.3f}, ours {:.3f} -> {:.3f}, ratio {:.3f} -> "
                                  "{:.3f}, To {:.3f} -> {:.3f}",
                                  a.fog[0], b.fog[0], a.fog[1], b.fog[1], a.fog[2], b.fog[2],
                                  a.volume[3], b.volume[3]);
            points += fmt::format(", the volume's light {:.3f} {:.3f} {:.3f} -> {:.3f} {:.3f} {:.3f}, the "
                                  "apply's {:.3f} {:.3f} {:.3f} -> {:.3f} {:.3f} {:.3f}, replace {:.0f} -> {:.0f}",
                                  a.volume[0], a.volume[1], a.volume[2], b.volume[0], b.volume[1],
                                  b.volume[2], a.add[0], a.add[1], a.add[2], b.add[0], b.add[1],
                                  b.add[2], a.depth[3], b.depth[3]);
          }
        }
        st.flashes += n;
        if (n >= 3) {
          ++st.flash_frames;
          const bool still = moved < 0.05f;
          st.still_frames += still;
          const std::string b0 = order(f - 1, false), b1 = order(f, false);
          st.order_changed += b0 != b1;
          // Logged: three with the eye still (a moving view's points change surface), one moving.
          if (still ? st.logged_still < 3 : st.logged_moving < 1) {
            ++(still ? st.logged_still : st.logged_moving);
            st.events += fmt::format(" || FOG JUMP frame {}{}: {} points, the eye moved {:.2f} m, fog draws "
                                     "before the volume {} -> {}, after it {} -> {}; the volume's inputs{}{}",
                                     f, still ? " (STILL)" : "", n, moved, b0, b1, order(f - 1, true),
                                     order(f, true), VolumeInputsChanged(f - 1, f), points);
          }
        }
      }
      // The visibility against the game's (21/11/2026, Tazz: a look as clear as the original with
      // the banks and wisps): by distance band, the mean of our transmittance (To, as the shaders
      // lay it) and of the game's T over the points not on the sky, and of their opacities.
      {
        static constexpr float kBands[] = {20.0f, 50.0f, 100.0f, 300.0f, 1e9f};
        for (uint32_t i = 0; i < kPoints; ++i) {
          const GridPoint& g = now[i];
          if (g.depth[2] > 0.5f || !(g.fog[0] > 0.0f) || !std::isfinite(g.volume[3])) continue;
          const float dist = g.depth[1];
          // Local change (22/11/2026): bounded (a distance at or over 1e9, or infinite, went past
          // the five bands and wrote past vis_), a distance not finite left out.
          if (!std::isfinite(dist)) continue;
          int b = 0;
          while (b < 4 && dist >= kBands[b]) ++b;
          vis_.n[b] += 1.0;
          vis_.ours[b] += g.volume[3];
          vis_.game[b] += g.fog[0];
        }
      }
      std::memcpy(grid_prev_, now, sizeof(now));
      grid_prev_frame_ = f;
      for (int k = 0; k < 3; ++k) grid_prev_eye_[k] = e[k];
    }
  }
  // This frame's points to the readback.
  D3D12_RESOURCE_BARRIER b = {};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = probe_.Get();
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  list->ResourceBarrier(1, &b);
  list->CopyBufferRegion(grid_readback_[write].Get(), 0, probe_.Get(), uint64_t(kGridAt) * 16, kGridBytes);
  std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
  list->ResourceBarrier(1, &b);
  grid_written_[write] = frame;
  for (int k = 0; k < 3; ++k) grid_eye_[write][k] = eye[k];
  if (const auto t = std::chrono::steady_clock::now(); t - last >= std::chrono::seconds(2)) {
    last = t;
    const GridPoint& m = grid_prev_[4 * kGridX + 8];
    REXLOG_INFO("PGR4 volumetric grid probe: {} frames compared, {} points on the same surface, the "
                "colour jumping in {} ({} darker, {} brighter: the upscaler's jitter too), the fog in {}; "
                "{} frames with 3 or more FOG JUMPS{} ({} with the eye still, "
                "{} with the fog draws before the volume changed); the centre now: colour {:.3f} {:.3f} {:.3f} at "
                "{:.1f} m{}, the game's T {:.3f}, ours {:.3f}, ratio {:.3f}, To {:.3f}, the volume's "
                "light {:.3f} {:.3f} {:.3f}, the apply's {:.3f} {:.3f} {:.3f}, replace {:.0f}{}",
                st.frames, st.compared, st.colour_jumps, st.darker, st.brighter, st.flashes,
                st.flash_frames, st.flash_frames ? " FOG JUMPS" : "", st.still_frames, st.order_changed, m.colour[0], m.colour[1],
                m.colour[2], m.depth[1], m.depth[2] > 0.5f ? " (sky)" : "", m.fog[0], m.fog[1],
                m.fog[2], m.volume[3], m.volume[0], m.volume[1], m.volume[2], m.add[0], m.add[1],
                m.add[2], m.depth[3], st.events);
    st = Stats{};
    {
      std::string bands;
      static const char* const kNames[] = {"0-20 m", "20-50 m", "50-100 m", "100-300 m", "300 m+"};
      for (int b = 0; b < 5; ++b) {
        if (vis_.n[b] < 1.0) continue;
        const double o = vis_.ours[b] / vis_.n[b], gm = vis_.game[b] / vis_.n[b];
        bands += fmt::format(" | {}: ours {:.3f}, the game's {:.3f}, ratio {:.2f}, opacity ratio {:.2f} "
                             "({:.0f} points)",
                             kNames[b], o, gm, gm > 1e-4 ? o / gm : 0.0,
                             1.0 - gm > 1e-4 ? (1.0 - o) / (1.0 - gm) : 0.0, vis_.n[b]);
      }
      REXLOG_INFO("PGR4 volumetric visibility (the transmittance to the surface, the grid's points, "
                  "2 s){}", bands.empty() ? std::string(" none") : bands);
      vis_ = Visibility{};
    }
  }
}

}  // namespace pgr4::native
