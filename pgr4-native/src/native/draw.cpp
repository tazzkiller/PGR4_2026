// pgr4-native PGR4 native renderer: guest draws -> host D3D12 draws. See draw.h.
//
// Ports (BSD-3, Xenia / ReXGlue): d3d12::PipelineCache::GetCurrentStateDescription and
// CreateD3D12Pipeline (pipeline state), D3D12CommandProcessor::IssueDraw,
// UpdateFixedFunctionState, UpdateSystemConstantValues and UpdateBindings (host render target
// path, bindless), draw_util::GetHostViewportInfo / GetScissor / GetNormalizedDepthControl /
// GetNormalizedColorMask / IsRasterizationPotentiallyDone / GetPreferredFacePolygonOffset,
// D3D12TextureCache::GetSamplerParameters / WriteSampler, and the DMA index path of
// PrimitiveProcessor::Process. Copyright Ben Vanik and Xenia developers; adapted by Tom Clay
// for ReXGlue; adapted here for the native renderer.
#include "draw.h"

#include "geometry_shader.h"
#include "log.h"
#include "motion_history.h"
#include "occlusion.h"
#include "pipeline_store.h"
#include "profile.h"
#include "render_targets.h"
#include "renderer.h"
#include "road_reflect_list.h"
#include "scene_copy.h"
#include "resolve.h"
#include "shader_cache.h"
#include "shader_replace.h"
#include "shader_specialize.h"
#include "shader_translate.h"
#include "textures.h"
#include "upload_pool.h"
#include "upscaler.h"

#include <map>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <dxgi1_4.h>
#include <xxhash.h>
#include <simde/x86/sse2.h>

#include <algorithm>
#include <optional>
#include <array>
#include <chrono>
#include <filesystem>
#include <charconv>
#include <fstream>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <bit>
#include <cfloat>
#include <cstddef>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>


REXCVAR_DEFINE_BOOL(pgr4_native_motion_current_fetch, true, "PGR4 native",
                    "Motion vectors: the previous position from this frame's vertex streams with "
                    "the previous frame's constants (local change; instanced props' streams are "
                    "rewritten every frame)");
REXCVAR_DEFINE_BOOL(pgr4_native_async_pipelines, true, "PGR4 native",
                    "Compile new pipeline states on worker threads, the frame's draws that need "
                    "them skipped, instead of compiling each at its draw");
REXCVAR_DEFINE_INT32(pgr4_native_pipeline_threads, 0, "PGR4 native",
                     "Pipeline compile threads with --pgr4_native_async_pipelines (0: a quarter "
                     "of the hardware threads, 1 to 4)");
REXCVAR_DEFINE_BOOL(pgr4_native_pipeline_wait, true, "PGR4 native",
                    "With --pgr4_native_async_pipelines, a frame whose draws need pipelines still "
                    "compiling is not shown and waits for them at its end, up to 0.5 s (false: "
                    "show it without those draws)");
// Local change (pgr4 project): the micro-freezes mid-race (a sharp turn's tyre smoke, a
// collision's sparks and debris: one to a few new pipelines) held whole frames up to 0.5 s.
REXCVAR_DEFINE_INT32(pgr4_native_pipeline_wait_min, 8, "PGR4 native",
                     "With --pgr4_native_pipeline_wait, a frame is held for its compiling "
                     "pipelines only when it needs at least this many (a scene's first frames); "
                     "with fewer it is shown without those draws for the frames they take "
                     "(1: hold for any, as pgr4-native)");
// Local change (pgr4 project): the launcher's precompile step (PGR4Launcher, before the game):
// every recorded pipeline (this scale's and the other scales' records) compiled into this
// scale's library by all the workers, the progress in <cache>\<scale>\precompile_progress.txt
// ("done total"), then the process exits.
REXCVAR_DEFINE_BOOL(pgr4_native_precompile, false, "PGR4 native",
                    "Compile every recorded pipeline into the pipeline library, then exit (the "
                    "launcher's precompile step)");
REXCVAR_DEFINE_BOOL(pgr4_native_generic_pipelines, true, "PGR4 native",
                    "With --pgr4_native_async_pipelines, a draw whose bool or texture-sign "
                    "specialization is still compiling uses the pipeline of its unspecialized "
                    "shaders when that one is ready (false: it waits for the specialization)");
REXCVAR_DEFINE_BOOL(pgr4_native_pipeline_cache, true, "PGR4 native",
                    "Keep the shaders and pipelines the native renderer creates under "
                    "<cache_root>\\pgr4-native and compile them at start-up in later runs "
                    "(prewarm, plus a D3D12 pipeline library)");
REXCVAR_DEFINE_STRING(pgr4_native_watch_trace, "", "PGR4 native",
                      "Binary log of vertex write-watch events (arms and faults, one 64-bit "
                      "record each, see WatchTrace) for offline analysis of the watch policy");
REXCVAR_DEFINE_BOOL(pgr4_native_dynamic_depth_bias, true, "PGR4 native",
                    "Set the polygon offset with RSSetDepthBias instead of baking it into the "
                    "pipeline state, where the driver supports it (the game animates it, which "
                    "made hundreds of pipelines per shader pair)");
REXCVAR_DEFINE_INT32(pgr4_native_debug_checker_frames, 0, "PGR4 native",
                     "Debug: log the start line checker draw's pixel constants for this many frames");
float g_pgr4_camera_eye[3] = {};  // Local change (pgr4 project): the eye, for src/pgr4_autopilot.cpp
// The game's clock stands still (the pause, the photo mode; src/pgr4_hooks.cpp): our shaders' clocks too.
bool g_pgr4_game_paused = false;
float g_pgr4_car_speed = 0.0f;    // the speed of the car the camera is on, m/s (src/pgr4_hooks.cpp)
float g_pgr4_car_side[3] = {};    // in a bend, the way the running drops lean (src/pgr4_hooks.cpp)
float g_pgr4_car_yaw_rate = 0.0f;
// Local change (pgr4 project): the race's weather, as the game's own record says it (src/pgr4_autopilot.cpp,
// read when the race loads): -1 unknown, 0 clear, 1 overcast, 2 fog, 3 rain, 4 snow or light snow, 5 icy (its
// record's kind is snow's, but nothing falls), 6 heavy snow. Whatever is drawn: the game draws no falling
// particles under a bridge.
int g_pgr4_weather_kind = -1;
const uint8_t* g_pgr4_guest_base = nullptr;  // development: the rain's log, EndFrame
bool g_pgr4_light_rain = false;  // its record's name has "Light Rain": half the running drops
// On a race's screens (src/pgr4_menu.cpp): the garages and the menus keep the last race's weather otherwise.
bool g_pgr4_race_screen = false;
// The car the camera is on (src/pgr4_hooks.cpp: the record nearest the eye, as for the drops' speed): its
// position; far away when there is none. The running drops and the wiper are for it alone (they cost too
// much for all). Read by the game's thread a frame or more before the draws: the margin grows with the speed.
float g_pgr4_car_pos[3] = {1e6f, 1e6f, 1e6f};
// The vertex shaders of the cars' paint and glass: their c0-c2 the object to world (rows, affine), c24
// the eye.
// Local change (pgr4 project): the game's night windscreens (psCarGlassWindscreen[Damaged]Night1). They
// read no wiper data: the draw gives it to their replacements (SystemConstantsWithSun::wiper).
bool IsNightWindscreen(uint64_t ps_hash) {
  return ps_hash == 0x9148064A4FDF2824ull || ps_hash == 0x2D1425235CED5909ull ||
         ps_hash == 0xEF66DABF1C928809ull || ps_hash == 0x3807F73C0312F49Cull ||
         ps_hash == 0xE91726D41059E3BEull;
}
bool IsCarPaintVs(uint64_t vs_hash) {
  return vs_hash == 0x56CCC4693B764A9Cull || vs_hash == 0x1832B31EEB903BC6ull ||
         vs_hash == 0x68E7BA5FE3C94481ull || vs_hash == 0xB02F907294528522ull ||
         vs_hash == 0xE4C8B3E7DE6D6EDFull || vs_hash == 0x29D7127DDE6DBF2Eull ||
         vs_hash == 0x7687697BE0E3F9BFull || vs_hash == 0xB0BA87C47C293AF6ull ||
         vs_hash == 0x24978B524815FA1Aull || vs_hash == 0xF18B35C5A03EE147ull ||
         vs_hash == 0xF67DAD8C0620E06Bull;
}
// The rotation of the frame whose axes x and z are given, y made so it is a proper one, as a quaternion
// (x, y, z, its w >= 0).
void Pgr4FrameQuat(const float x[3], const float z[3], float out[3]) {
  float m[3][3];
  for (int k = 0; k < 3; ++k) {
    m[0][k] = x[k];
    m[2][k] = z[k];
  }
  m[1][0] = z[1] * x[2] - z[2] * x[1];
  m[1][1] = z[2] * x[0] - z[0] * x[2];
  m[1][2] = z[0] * x[1] - z[1] * x[0];
  float q[4];
  const float trace = m[0][0] + m[1][1] + m[2][2];
  if (trace > 0.0f) {
    const float s = 2.0f * std::sqrt(1.0f + trace);
    q[3] = 0.25f * s;
    q[0] = (m[2][1] - m[1][2]) / s;
    q[1] = (m[0][2] - m[2][0]) / s;
    q[2] = (m[1][0] - m[0][1]) / s;
  } else {
    const int a = m[0][0] >= m[1][1] && m[0][0] >= m[2][2] ? 0 : m[1][1] >= m[2][2] ? 1 : 2;
    const int b = (a + 1) % 3, c = (a + 2) % 3;
    const float s = 2.0f * std::sqrt(std::max(1.0f + m[a][a] - m[b][b] - m[c][c], 1e-6f));
    q[a] = 0.25f * s;
    q[b] = (m[b][a] + m[a][b]) / s;
    q[c] = (m[c][a] + m[a][c]) / s;
    q[3] = (m[c][b] - m[b][c]) / s;
  }
  const float way = q[3] < 0.0f ? -1.0f : 1.0f;
  for (int k = 0; k < 3; ++k) out[k] = q[k] * way;
}
float g_pgr4_car_back[3] = {};    // the wind on the running drops (src/pgr4_hooks.cpp)
REXCVAR_DEFINE_BOOL(pgr4_native_drops_glide, true, "PGR4 native",
                    "The rain's drops glide on the cars' glass with the speed, in the outside views");
REXCVAR_DEFINE_DOUBLE(pgr4_native_drops_glide_rate, 0.002, "PGR4 native",
                      "How much faster the running drops' clock goes, per m/s of the car's speed over glide_from");
REXCVAR_DEFINE_DOUBLE(pgr4_native_drops_glide_from, 3.0, "PGR4 native",
                      "The speed, m/s, under which the drops stay in place");
REXCVAR_DEFINE_DOUBLE(pgr4_native_drops_glide_idle, 0.014, "PGR4 native",
                      "How fast the drops run down when the car stands still: textures a second");
REXCVAR_DEFINE_BOOL(pgr4_native_drops_glide_log, false, "PGR4 native",
                    "Logs the camera's position and speed once a second");
REXCVAR_DEFINE_DOUBLE(pgr4_native_coplanar_bias, 0.0, "PGR4 native",
                      "Test: the scene's draws testing depth without writing it, without a polygon "
                      "offset, pulled toward the camera by this many depth units (0: none)");
REXCVAR_DEFINE_BOOL(pgr4_native_alpha_to_coverage, false, "PGR4 native",
                    "The scene's alpha-tested draws with alpha to coverage under MSAA (their "
                    "edges smoothed by the samples)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_occlusion_depth_bias_scale, 1.0, "PGR4 native",
                      "Debug (local change, pgr4 project): the absolute polygon offset of the "
                      "light flares' test sprites (draws inside an occlusion query that do not "
                      "write depth), times this, with dynamic depth bias");
REXCVAR_DEFINE_BOOL(pgr4_native_bool_specialize, true, "PGR4 native",
                    "Translate shaders whose control flow tests bool constants for each draw's "
                    "values, without the branches (shader_specialize.h)");
REXCVAR_DEFINE_BOOL(pgr4_native_watch_hot, false, "PGR4 native",
                    "Pages the game rewrites again and again are hashed at each request instead of watched "
                    "(false: watched like the others, every write a fault, as the Xenos plugin does; a trial "
                    "for the bikes' riders, which drew another frame's pose now and then; off by default since "
                    "14/11/2026: a lap measured the same either way, 13.3 against 13.7 ms a frame; live)");
REXCVAR_DEFINE_INT32(pgr4_native_hot_ranges_exact_kb, 0, "PGR4 native",
                     "A vertex or index range of at most this many KB with a page rewritten again and again is "
                     "copied whole at each draw (local change: the bikes' riders drew another frame's pose now and "
                     "then; off: even at 256 KB, the menus and races' many animated ranges cost 40 ms a frame; 0: off)");
REXCVAR_DEFINE_BOOL(pgr4_native_coherency_check, false, "PGR4 native",
                    "Debug: keep a CPU copy of what shared memory holds and compare the vertex and "
                    "index ranges draws read with guest memory, per request and again at the Swap "
                    "(stale pages: a write the page watch missed; late pages: data rewritten after "
                    "a draw of the frame read it), and check copy segments against every read. "
                    "Slow (512 MB of host memory, a memcmp of every range read)");
REXCVAR_DEFINE_INT32(pgr4_native_draw_log_from, -1, "PGR4 native",
                     "Debug: from this native frame on, log the draws of each frame to "
                     "<capture dir>/draws-f<frame>.txt, as bit 16 of <capture dir>/draw-debug.txt "
                     "does from when it is set (-1: off)");
REXCVAR_DEFINE_INT32(pgr4_native_draw_log_frames, 1, "PGR4 native",
                     "Frames one draw log trigger (--pgr4_native_draw_log_from or draw-debug.txt "
                     "bit 16) covers");
REXCVAR_DEFINE_BOOL(pgr4_native_fit_glass_c45, true, "PGR4 native",
                    "Development (21:9): the menus' translucent panels fitted by their c45 (true) "
                    "or by the viewport as the rest of the interface (false)");
REXCVAR_DEFINE_BOOL(pgr4_native_fit_ui_viewport, true, "PGR4 native",
                    "Development (21:9): the interface fitted by the viewport (true) or by its c45 "
                    "(false, the shaders reading the HUD projection only)");
REXCVAR_DEFINE_BOOL(pgr4_native_fit_hud, true, "PGR4 native",
                    "With a render height other than 720, fit the HUD's and the menus' projection "
                    "(c45) to the frame (local change, pgr4 project; false: the game's)");
REXCVAR_DEFINE_BOOL(pgr4_native_texture_catalog, false, "PGR4 native",
                    "Write every texture the draws bind, with the shader and the slot that read it, "
                    "to <capture dir>/texture_catalog.tsv (local change, pgr4 project)");
REXCVAR_DEFINE_INT32(pgr4_native_anisotropy, 1, "PGR4 native",
                     "Anisotropic filtering of the game's linearly filtered textures with mips, "
                     "at least this many samples per pixel (2, 4, 8 or 16; 1: the game's own)");
REXCVAR_DECLARE(std::string, pgr4_native_capture_dir);
REXCVAR_DECLARE(bool, pgr4_native_decal_light_off);  // shader_replace.cpp
// Local change (pgr4 project): the cars' grime, at will (the overlay's Tests tab).
REXCVAR_DEFINE_DOUBLE(pgr4_native_dirt_force, -1.0, "PGR4 native",
                      "The cars' grime forced to this amount (below 0: the game's, c220.x)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_dirt_fine, 0.7, "PGR4 native",
                      "The frost's fine grain over it (0: none)");
// Local change (pgr4 project): see the alpha to mask in UpdateSystemConstants.
// Local change (pgr4 project): the freezes at a change of view: the cars' shaders no longer hold the frame.
REXCVAR_DEFINE_BOOL(pgr4_native_car_no_wait, true, "PGR4 native",
                    "A car's replaced shader still compiling: drawn with the game's shader, the frame not held");
REXCVAR_DEFINE_BOOL(pgr4_native_paint_prepare, true, "PGR4 native",
                    "A car's paint first seen: its other variants compiled quietly in the background");
REXCVAR_DEFINE_DOUBLE(pgr4_native_alpha_cut, 0.375, "PGR4 native",
                      "One sample per pixel: the threshold of the game's alpha to mask, the same on all pixels (0: its grid)");
REXCVAR_DEFINE_INT32(pgr4_native_view_limit, 0, "PGR4 native",
                     "Test: the view heap counted as full at this many views (0: its real size)");
REXCVAR_DEFINE_DOUBLE(pgr4_native_msaa_texture_soften, 0.5, "PGR4 native",
                      "The scene's textures softened without an upscaler (MSAA), in mip levels "
                      "(0: as sharp as the game's; 0.25 steps up to 3.75): their shimmer in motion");

namespace pgr4::native {

// Local change (pgr4 project): the overlay's "the game without our changes" (renderer.cpp).
bool OriginalLook();
// The scene's textures softened without an upscaler (MSAA): its quarter levels of LOD bias. Its
// sharp textures (and the HD ones) shimmered as the camera moved at the screen's own resolution:
// MSAA smooths the edges only, and the render scale's supersampling was gone at 2 on 1440p.
uint32_t MsaaSoftenQuarters() {
  return uint32_t(std::clamp(int(std::lround(4.0 * REXCVAR_GET(pgr4_native_msaa_texture_soften))), 0, 15));
}
// The enhanced reflections (shader_replace.cpp): the water's and wet roads' own, below.
bool EnhancedReflections();

namespace xenos = rex::graphics::xenos;
namespace reg = rex::graphics::reg;
using rex::graphics::RegisterFile;
using rex::graphics::Shader;
using rex::graphics::draw_util::Scissor;
using rex::graphics::draw_util::ViewportInfo;
namespace texture_util = rex::graphics::texture_util;

namespace {

constexpr uint32_t kSharedMemorySize = 512u << 20;  // guest physical address space
// Shared memory is a reserved buffer whose 4 MB chunks get memory when a range in them is first
// requested (the Xenos plugin's shared memory does the same): a committed 512 MB buffer took half
// of a Steam Deck's 1 GB of VRAM, of which a race touches a fraction.
constexpr uint32_t kSharedChunkShift = 22;
constexpr uint32_t kSharedChunkCount = kSharedMemorySize >> kSharedChunkShift;
constexpr uint32_t kPageShift = 12;
constexpr uint32_t kPageCount = kSharedMemorySize >> kPageShift;
// The memory unwatches at most the 4 MB-aligned block around a write fault
// (PhysicalHeap::TriggerCallbacks), so the invalidation callback looks no further.
constexpr uint32_t kUnwatchWindow = 4u << 20;
// The Xenos plugin's invalidation callbacks let the memory unwatch at most the 256 KB-aligned
// block around a fault: the pages of one 64-bit word of the valid bits.
constexpr uint32_t kPluginUnwatchBlock = 256u << 10;
static_assert(kPluginUnwatchBlock == 64u << kPageShift);
// Local change (pgr4 project): the most D3D12 allows every card (binding tiers 1 and 2), for all video
// memories: the textures kept grow with it (TextureCache's limits). 65 536 ran out on a 16 GB card, whose
// limits let more textures stay than that, and the textures after them were black (06/11/2026). Smaller
// ones if a driver refuses it (Init).
constexpr uint32_t kViewHeapSizes[] = {1000000, 262144, 65536};
constexpr uint32_t kSamplerHeapSize = 2048;
// View heap: 0 shared memory SRV, 1 shared memory UAV, then the bindless table (descriptor
// indices in b4 count from here): 0 null 2D array, 1 null 3D, 2 null cube, 3-4 the scene's copy
// for the water, 5-7 the copy at the scene's end for the wet roads (color, depth, the color's
// running average; local change, pgr4 project: scene_copy.h), 8 unused, then textures.
constexpr uint32_t kViewTableStart = 2;
constexpr uint32_t kNull2DArray = 0, kNull3D = 1, kNullCube = 2, kSceneColor = 3, kSceneDepth = 4,
                   kHistoryColor = 5, kHistoryDepth = 6, kHistoryAccum = 7, kFirstTexture = 9;
// Local change (pgr4 project): the sky's pixel shaders (psSky, psSkyFog), drawn without depth.
// Local change (pgr4 project): the rain's pixel shader, and the frames without a wet road drawn
// after which the race is left and its rain forgotten (Impl::rain).
constexpr uint64_t kRainShader = 0x60C8BBC443921024ull;
// ... with the drops' vertex shader (its other draw, the splashes near the camera, has other
// constants).
constexpr uint64_t kRainDropsVs = 0x531F25354F7275B9ull;
constexpr uint64_t kRainSlowVs = 0x16610A1EF7C5BE27ull;
// Local change (pgr4 project): the rain a race's script brings (Macau, "Light Rain Dry Bright", its record
// says clear): one draw of 40 000 vertices, now and then (06/11/2026).
constexpr uint64_t kRainScriptVs = 0x0462CAD6511F1FA6ull;
constexpr uint32_t kRainSlowVertices = 1900;
constexpr uint32_t kRainGoneFrames = 120;
bool IsSkyShader(uint64_t ps_hash) {
  switch (ps_hash) {
    case 0x25E956077772CC47ull:
    case 0xAA1B910E0D596BC3ull:
    case 0x529E0E9E179149DBull:
    case 0xA0120EC7D19963FDull:
    case 0xA7B4D6C2F3180089ull:
      return true;
    default:
      return false;
  }
}
// Local change (pgr4 project): the road shaders whose replacements reflect the scene on wet roads
// (road_reflect_list.h, shaders/road_reflect_hooks.hlsli).
bool IsRoadReflectShader(uint64_t ps_hash) {
  return std::binary_search(std::begin(kRoadReflectShaders), std::end(kRoadReflectShaders),
                            ps_hash);
}
// Local change (pgr4 project): the water plane (vsWaterPlane's 4 vertices, float3 positions first)
// is the camera's view over the water, a trapezium the game makes each frame: its near edge some
// 20 m wide just before the camera, its far one kilometres away (St Petersburg). Its sides end
// just inside the screen: a column of sky at the screen's left edge beside the rivers, 1 to 3
// guest pixels, as wide as the camera makes it, in both renderers (under a TV's overscan at the
// console's 720p). Its upload (RequestVertices sets the range, thread_local, that RequestRange's
// copy patches) gets each edge across the view widened along itself, around its middle: the near
// one (the shortest) kWaterPlaneNear times, the far one kWaterPlaneFar, and the near edge moved
// back by half its length (made larger around its centre, kilometres away, the near edge went a
// kilometre behind the camera and the water lost its waves and reflections). The land above the
// water hides what is added.
constexpr uint64_t kWaterPlaneVs = 0x4B0A0DDD4761505Bull;
constexpr float kWaterPlaneNear = 3.0f, kWaterPlaneFar = 1.2f, kWaterPlaneBack = 0.5f;
thread_local uint32_t water_plane_start = 0, water_plane_size = 0;
void WidenWaterPlane(uint8_t* data, uint32_t size) {
  // Its VS fetches 6 dwords a vertex (float3 position, then the normal at dword 5); the fetch's
  // range may hold more than the 4 vertices.
  constexpr uint32_t stride = 24;
  if (size < 4 * stride) return;
  auto load = [&](uint32_t v, uint32_t c) {
    uint32_t bits;
    std::memcpy(&bits, data + v * stride + 4 * c, 4);
    bits = _byteswap_ulong(bits);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
  };
  auto store = [&](uint32_t v, uint32_t c, float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    bits = _byteswap_ulong(bits);
    std::memcpy(data + v * stride + 4 * c, &bits, 4);
  };
  float x[4], z[4];
  for (uint32_t v = 0; v < 4; ++v) {
    x[v] = load(v, 0);
    z[v] = load(v, 2);
    if (!std::isfinite(x[v]) || !std::isfinite(z[v])) return;
  }
  // The near edge: the shortest of the quad's four; the far one opposite it.
  uint32_t near_edge = 0;
  float shortest = INFINITY;
  for (uint32_t e = 0; e < 4; ++e) {
    const uint32_t a = e, b = (e + 1) % 4;
    const float length = std::hypot(x[b] - x[a], z[b] - z[a]);
    if (length < shortest) {
      shortest = length;
      near_edge = e;
    }
  }
  auto widen = [&](uint32_t e, float factor) {
    const uint32_t a = e, b = (e + 1) % 4;
    const float mx = 0.5f * (x[a] + x[b]), mz = 0.5f * (z[a] + z[b]);
    for (const uint32_t v : {a, b}) {
      store(v, 0, mx + (x[v] - mx) * factor);
      store(v, 2, mz + (z[v] - mz) * factor);
    }
  };
  // The near edge also moved back, away from the far one, by kWaterPlaneBack of its length: it
  // ended just above the screen's bottom too when the camera looks up.
  const uint32_t n0 = near_edge, n1 = (near_edge + 1) % 4;
  const uint32_t f0 = (near_edge + 2) % 4, f1 = (near_edge + 3) % 4;
  float bx = 0.5f * (x[n0] + x[n1]) - 0.5f * (x[f0] + x[f1]);
  float bz = 0.5f * (z[n0] + z[n1]) - 0.5f * (z[f0] + z[f1]);
  const float back = std::hypot(bx, bz);
  if (back > 0.0f) {
    bx *= kWaterPlaneBack * shortest / back;
    bz *= kWaterPlaneBack * shortest / back;
  }
  widen(near_edge, kWaterPlaneNear);
  widen(f0, kWaterPlaneFar);
  for (const uint32_t v : {n0, n1}) {
    store(v, 0, load(v, 0) + bx);
    store(v, 2, load(v, 2) + bz);
  }
}
// Local change (pgr4 project): the water's pixel shaders (vsWater's and vsWaterPlane's), whose
// replacements reflect the scene (shaders/water_hooks.hlsli, scene_copy.h).
bool IsWaterShader(uint64_t ps_hash) {
  switch (ps_hash) {
    case 0xAF63CE3241D1A82Aull:
    case 0xB5C3FF723AD0C7D5ull:
    case 0x6F723874C2C63F7Full:
    case 0xB81D3A80F2183223ull:
    case 0xF3F558B3A7687D31ull:
    case 0xA105067698E02255ull:
    case 0x441604EE7597C1D1ull:  // the garage's harbour (its own reflection map, tf3)
      return true;
    default:
      return false;
  }
}
// The null view standing in for a view of dimension `dim`.
constexpr uint32_t NullView(D3D12_SRV_DIMENSION dim) {
  return dim == D3D12_SRV_DIMENSION_TEXTURECUBE ? kNullCube
         : dim == D3D12_SRV_DIMENSION_TEXTURE3D ? kNull3D
                                                : kNull2DArray;
}
// ... and for a texture binding of fetch dimension `dim`.
constexpr uint32_t NullView(xenos::FetchOpDimension dim) {
  return dim == xenos::FetchOpDimension::kCube          ? kNullCube
         : dim == xenos::FetchOpDimension::k3DOrStacked ? kNull3D
                                                        : kNull2DArray;
}
constexpr D3D12_RESOURCE_STATES kSharedReadState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                                   D3D12_RESOURCE_STATE_INDEX_BUFFER;

using enum rex::graphics::Register;  // XE_GPU_REG_* (clang-cl, C++20)

// reg::RB_COLOR_INFO / RB_BLENDCONTROL::rt_register_indices live in the plugin DLL (not exported).
constexpr rex::graphics::Register kColorInfoRegs[4] = {
    XE_GPU_REG_RB_COLOR_INFO, XE_GPU_REG_RB_COLOR1_INFO, XE_GPU_REG_RB_COLOR2_INFO,
    XE_GPU_REG_RB_COLOR3_INFO};
constexpr rex::graphics::Register kBlendControlRegs[4] = {
    XE_GPU_REG_RB_BLENDCONTROL0, XE_GPU_REG_RB_BLENDCONTROL1, XE_GPU_REG_RB_BLENDCONTROL2,
    XE_GPU_REG_RB_BLENDCONTROL3};

// ---- draw_util ports (host render targets, no resolution scale) ----

bool IsRasterizationPotentiallyDone(const RegisterFile& regs, bool primitive_polygonal) {
  xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode != xenos::EdramMode::kColorDepth && edram_mode != xenos::EdramMode::kDepthOnly) {
    return false;
  }
  if (regs.Get<reg::SQ_PROGRAM_CNTL>().vs_export_mode ==
          xenos::VertexShaderExportMode::kMultipass ||
      !regs.Get<reg::RB_SURFACE_INFO>().surface_pitch) {
    return false;
  }
  if (primitive_polygonal) {
    auto sc = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
    if (sc.cull_front && sc.cull_back) return false;
  }
  return true;
}

reg::RB_DEPTHCONTROL GetNormalizedDepthControl(const RegisterFile& regs) {
  xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode != xenos::EdramMode::kColorDepth && edram_mode != xenos::EdramMode::kDepthOnly) {
    reg::RB_DEPTHCONTROL disabled;
    disabled.value = 0;
    return disabled;
  }
  reg::RB_DEPTHCONTROL depthcontrol = regs.Get<reg::RB_DEPTHCONTROL>();
  if (depthcontrol.z_enable && !depthcontrol.z_write_enable &&
      depthcontrol.zfunc == xenos::CompareFunction::kAlways) {
    depthcontrol.z_enable = 0;
  }
  return depthcontrol;
}

void GetPreferredFacePolygonOffset(const RegisterFile& regs, bool primitive_polygonal,
                                   float& scale_out, float& offset_out) {
  float scale = 0.0f, offset = 0.0f;
  auto sc = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  if (primitive_polygonal) {
    if (sc.poly_offset_front_enable && !sc.cull_front) {
      scale = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE);
      offset = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET);
    }
    if (sc.poly_offset_back_enable && !sc.cull_back && !scale && !offset) {
      scale = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE);
      offset = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET);
    }
  } else if (sc.poly_offset_para_enable) {
    scale = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE);
    offset = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET);
  }
  scale_out = scale;
  offset_out = offset;
}

uint32_t GetNormalizedColorMask(const RegisterFile& regs, uint32_t ps_writes_color_targets) {
  if (regs.Get<reg::RB_MODECONTROL>().edram_mode != xenos::EdramMode::kColorDepth) return 0;
  uint32_t normalized = 0;
  uint32_t rb_color_mask = regs[XE_GPU_REG_RB_COLOR_MASK];
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    if (!(ps_writes_color_targets & (1u << i))) continue;
    uint32_t format_mask = (1u << xenos::GetColorRenderTargetFormatComponentCount(
                                regs.Get<reg::RB_COLOR_INFO>(kColorInfoRegs[i]).color_format)) -
                           1;
    uint32_t rt_mask = (rb_color_mask >> (4 * i)) & format_mask;
    if (!rt_mask) continue;
    rt_mask |= 0b1111 & ~format_mask;
    normalized |= rt_mask << (4 * i);
  }
  return normalized;
}

// GetHostViewportInfo with origin_bottom_left = true, x/y max = D3D12_VIEWPORT_BOUNDS_MAX, no
// reverse Z, no float24 conversion, resolution scale 1 and half_pixel_offset on (the plugin's
// settings), and full_float24_in_0_to_1 = false: the host depth buffers here are not shared
// with EDRAM ownership transfers, so depth stays in 0...1 (the Clear values RenderTargets uses,
// and the shader-written depth: the system flags never carry kSysFlag_DepthFloat24).
void HostViewportInfo(const RegisterFile& regs, bool pixel_shader_writes_depth, ViewportInfo& out) {
  auto clip = regs.Get<reg::PA_CL_CLIP_CNTL>();
  auto vte = regs.Get<reg::PA_CL_VTE_CNTL>();
  auto sc = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  auto vtx = regs.Get<reg::PA_SU_VTX_CNTL>();
  float scale_xy[] = {
      vte.vport_x_scale_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XSCALE) : 1.0f,
      vte.vport_y_scale_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE) : 1.0f,
  };
  float scale_z = vte.vport_z_scale_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZSCALE) : 1.0f;
  float offset_base_xy[] = {
      vte.vport_x_offset_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XOFFSET) : 0.0f,
      vte.vport_y_offset_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET) : 0.0f,
  };
  float offset_z = vte.vport_z_offset_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZOFFSET) : 0.0f;
  float offset_add_xy[2] = {};
  if (sc.vtx_window_offset_enable) {
    auto wo = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
    offset_add_xy[0] += float(wo.window_x_offset);
    offset_add_xy[1] += float(wo.window_y_offset);
  }
  if (vtx.pix_center == xenos::PixelCenter::kD3DZero) {
    offset_add_xy[0] += 0.5f;
    offset_add_xy[1] += 0.5f;
  }
  const uint32_t xy_max[] = {D3D12_VIEWPORT_BOUNDS_MAX, D3D12_VIEWPORT_BOUNDS_MAX};
  float z_min, z_max, ndc_scale[3], ndc_offset[3];
  if (clip.clip_disable) {
    for (uint32_t i = 0; i < 2; ++i) {
      out.xy_offset[i] = 0;
      uint32_t extent = std::min(xenos::kTexture2DCubeMaxWidthHeight, xy_max[i]);
      out.xy_extent[i] = extent;
      float extent_f = float(extent);
      float px_to_ndc = 2.0f / extent_f;
      ndc_scale[i] = scale_xy[i] * px_to_ndc;
      ndc_offset[i] = (offset_base_xy[i] - extent_f * 0.5f + offset_add_xy[i]) * px_to_ndc;
    }
    z_min = 0.0f;
    z_max = 1.0f;
    ndc_scale[2] = scale_z;
    ndc_offset[2] = offset_z;
  } else {
    for (uint32_t i = 0; i < 2; ++i) {
      float offset_axis = offset_base_xy[i] + offset_add_xy[i];
      float scale_axis = scale_xy[i];
      float scale_abs = std::abs(scale_axis);
      float max_f = float(xy_max[i]);
      uint32_t a0 = uint32_t(std::clamp(offset_axis - scale_abs, 0.0f, max_f));
      uint32_t a1 = uint32_t(std::clamp(offset_axis + scale_abs, 0.0f, max_f));
      uint32_t extent = a1 - a0;
      out.xy_offset[i] = a0;
      out.xy_extent[i] = extent;
      if (extent) {
        float extent_f = float(extent);
        ndc_scale[i] = scale_axis * 2.0f / extent_f;
        ndc_offset[i] = (offset_axis - (float(a0) + extent_f * 0.5f)) * 2.0f / extent_f;
      } else {
        ndc_scale[i] = 1.0f;
        ndc_offset[i] = 0.0f;
      }
    }
    float host_offset_z, host_scale_z;
    if (clip.dx_clip_space_def) {
      host_offset_z = offset_z;
      host_scale_z = scale_z;
      ndc_scale[2] = 1.0f;
      ndc_offset[2] = 0.0f;
    } else {
      host_offset_z = offset_z - scale_z;
      host_scale_z = scale_z * 2.0f;
      ndc_scale[2] = 0.5f;
      ndc_offset[2] = 0.5f;
    }
    if (pixel_shader_writes_depth) {
      z_min = 0.0f;
      z_max = 1.0f;
    } else {
      z_min = std::clamp(host_offset_z, 0.0f, 1.0f);
      z_max = std::clamp(host_offset_z + host_scale_z, 0.0f, 1.0f);
      if (z_min > z_max) {
        std::swap(z_min, z_max);
        ndc_scale[2] = -ndc_scale[2];
        ndc_offset[2] = 1.0f - ndc_offset[2];
      }
    }
  }
  out.z_min = z_min;
  out.z_max = z_max;
  ndc_scale[1] = -ndc_scale[1];  // origin_bottom_left
  ndc_offset[1] = -ndc_offset[1];
  for (uint32_t i = 0; i < 3; ++i) {
    out.ndc_scale[i] = ndc_scale[i];
    out.ndc_offset[i] = ndc_offset[i];
  }
}

void HostScissor(const RegisterFile& regs, Scissor& out) {
  auto tl = regs.Get<reg::PA_SC_WINDOW_SCISSOR_TL>();
  auto br = regs.Get<reg::PA_SC_WINDOW_SCISSOR_BR>();
  int32_t tl_x = int32_t(tl.tl_x), tl_y = int32_t(tl.tl_y);
  int32_t br_x = int32_t(br.br_x), br_y = int32_t(br.br_y);
  if (!tl.window_offset_disable) {
    auto wo = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
    tl_x += wo.window_x_offset;
    tl_y += wo.window_y_offset;
    br_x += wo.window_x_offset;
    br_y += wo.window_y_offset;
  }
  auto stl = regs.Get<reg::PA_SC_SCREEN_SCISSOR_TL>();
  auto sbr = regs.Get<reg::PA_SC_SCREEN_SCISSOR_BR>();
  tl_x = std::max(tl_x, int32_t(stl.tl_x));
  tl_y = std::max(tl_y, int32_t(stl.tl_y));
  br_x = std::min(br_x, int32_t(sbr.br_x));
  br_y = std::min(br_y, int32_t(sbr.br_y));
  uint32_t pitch = regs.Get<reg::RB_SURFACE_INFO>().surface_pitch;
  tl_x = std::min(tl_x, int32_t(pitch));
  br_x = std::min(br_x, int32_t(pitch));
  tl_x = std::max(tl_x, 0);
  tl_y = std::max(tl_y, 0);
  br_x = std::max(br_x, tl_x);
  br_y = std::max(br_y, tl_y);
  out.offset[0] = uint32_t(tl_x);
  out.offset[1] = uint32_t(tl_y);
  out.extent[0] = uint32_t(br_x - tl_x);
  out.extent[1] = uint32_t(br_y - tl_y);
}

// ---- pipeline description ----

struct PipelineKey {
  const void* vs;  // translation objects (stable for the cache's lifetime)
  const void* ps;
  uint32_t gs_key;
  uint8_t topology_type;  // D3D12_PRIMITIVE_TOPOLOGY_TYPE
  uint8_t strip_cut;      // 0 none, 1 0xFFFF, 2 0xFFFFFFFF
  uint8_t cull;           // D3D12_CULL_MODE
  uint8_t wireframe;
  uint8_t front_ccw;
  uint8_t depth_clip;
  uint8_t samples;
  uint8_t disable_raster;
  int32_t depth_bias;
  float slope_scaled_depth_bias;
  uint8_t depth_enable, depth_write, depth_func, stencil_enable;
  uint8_t stencil_read_mask, stencil_write_mask;
  uint8_t front[4], back[4];  // fail, depth fail, pass, func (D3D12 values)
  uint8_t num_rts, pad[5];
  DXGI_FORMAT dsv;
  DXGI_FORMAT rtv[4];
  struct Rt {
    uint8_t blend, src, dst, op, src_a, dst_a, op_a, mask;
  } rt[4];
  uint8_t pad_end[4];
};
// No implicit padding: keys are hashed and stored as bytes, and a copy need not carry padding.
static_assert(offsetof(PipelineKey, depth_bias) == 28 && offsetof(PipelineKey, dsv) == 56 &&
              offsetof(PipelineKey, rt) == 76 && sizeof(PipelineKey) == 112);
// Bump when PipelineKey's layout or the meaning of a field changes: keys are stored on disk
// (PipelineStore) and an old file is then discarded.
// 2: depth bias zero with dynamic depth bias; 3: pixel shaders specialized for unsigned textures
constexpr uint32_t kPipelineKeyVersion = 3;

enum class BlendFactor : uint8_t {
  kZero,
  kOne,
  kSrcColor,
  kInvSrcColor,
  kSrcAlpha,
  kInvSrcAlpha,
  kDestColor,
  kInvDestColor,
  kDestAlpha,
  kInvDestAlpha,
  kBlendFactor,
  kInvBlendFactor,
  kSrcAlphaSat,
};
constexpr BlendFactor kBlendFactorMap[32] = {
    BlendFactor::kZero,        BlendFactor::kOne,
    BlendFactor::kZero,        BlendFactor::kZero,
    BlendFactor::kSrcColor,    BlendFactor::kInvSrcColor,
    BlendFactor::kSrcAlpha,    BlendFactor::kInvSrcAlpha,
    BlendFactor::kDestColor,   BlendFactor::kInvDestColor,
    BlendFactor::kDestAlpha,   BlendFactor::kInvDestAlpha,
    BlendFactor::kBlendFactor, BlendFactor::kInvBlendFactor,
    BlendFactor::kBlendFactor, BlendFactor::kInvBlendFactor,
    BlendFactor::kSrcAlphaSat,
};
constexpr BlendFactor kBlendFactorAlphaMap[32] = {
    BlendFactor::kZero,        BlendFactor::kOne,
    BlendFactor::kZero,        BlendFactor::kZero,
    BlendFactor::kSrcAlpha,    BlendFactor::kInvSrcAlpha,
    BlendFactor::kSrcAlpha,    BlendFactor::kInvSrcAlpha,
    BlendFactor::kDestAlpha,   BlendFactor::kInvDestAlpha,
    BlendFactor::kDestAlpha,   BlendFactor::kInvDestAlpha,
    BlendFactor::kBlendFactor, BlendFactor::kInvBlendFactor,
    BlendFactor::kBlendFactor, BlendFactor::kInvBlendFactor,
    BlendFactor::kSrcAlphaSat,
};
constexpr D3D12_BLEND kD3DBlend[] = {
    D3D12_BLEND_ZERO,          D3D12_BLEND_ONE,
    D3D12_BLEND_SRC_COLOR,     D3D12_BLEND_INV_SRC_COLOR,
    D3D12_BLEND_SRC_ALPHA,     D3D12_BLEND_INV_SRC_ALPHA,
    D3D12_BLEND_DEST_COLOR,    D3D12_BLEND_INV_DEST_COLOR,
    D3D12_BLEND_DEST_ALPHA,    D3D12_BLEND_INV_DEST_ALPHA,
    D3D12_BLEND_BLEND_FACTOR,  D3D12_BLEND_INV_BLEND_FACTOR,
    D3D12_BLEND_SRC_ALPHA_SAT,
};
constexpr D3D12_BLEND_OP kD3DBlendOp[8] = {
    D3D12_BLEND_OP_ADD,          D3D12_BLEND_OP_SUBTRACT, D3D12_BLEND_OP_MIN, D3D12_BLEND_OP_MAX,
    D3D12_BLEND_OP_REV_SUBTRACT, D3D12_BLEND_OP_ADD,      D3D12_BLEND_OP_ADD, D3D12_BLEND_OP_ADD};

// ---- samplers ----

// Every sampler field the guest's fetch constant and shader binding select, packed losslessly.
struct SamplerKey {
  uint32_t clamp_x : 3 = 0, clamp_y : 3 = 0, clamp_z : 3 = 0;  // xenos::ClampMode
  uint32_t border : 2 = 0;                                     // xenos::BorderColor
  uint32_t aniso : 3 = 0;                                      // xenos::AnisoFilter
  uint32_t mag_linear : 1 = 0, min_linear : 1 = 0, mip_linear : 1 = 0, mip_base_map : 1 = 0;
  uint32_t mip_min_level : 4 = 0;  // below xenos::kTextureMaxMips
  // Negative LOD bias in quarter levels, of linearly filtered textures with mips only (the
  // upscaler's scene, Upscaler::mip_bias_quarters).
  uint32_t lod_bias : 4 = 0;
  // Local change (pgr4 project): positive LOD bias in quarter levels, the same textures (the scene
  // without an upscaler: pgr4_native_msaa_texture_soften).
  uint32_t lod_soften : 4 = 0;
  uint32_t reserved : 2 = 0;
  uint32_t value() const { return std::bit_cast<uint32_t>(*this); }
};
static_assert(sizeof(SamplerKey) == sizeof(uint32_t));

xenos::ClampMode NormalizeClampMode(xenos::ClampMode m) {
  if (m == xenos::ClampMode::kClampToHalfway) return xenos::ClampMode::kClampToEdge;
  if (m == xenos::ClampMode::kMirrorClampToHalfway || m == xenos::ClampMode::kMirrorClampToBorder)
    return xenos::ClampMode::kMirrorClampToEdge;
  return m;
}

// `min_aniso`: --pgr4_native_anisotropy (DrawRenderer::Impl::min_aniso); `lod_bias`: the
// draw's SamplerKey::lod_bias, and its lod_soften in bits 4-7.
SamplerKey GetSamplerKey(const RegisterFile& regs, const DxbcShader::SamplerBinding& binding,
                         xenos::AnisoFilter min_aniso, uint32_t lod_bias) {
  xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(binding.fetch_constant);
  SamplerKey k{};
  xenos::ClampMode cx, cy, cz;
  texture_util::GetClampModesForDimension(fetch, cx, cy, cz);
  cx = NormalizeClampMode(cx);
  cy = NormalizeClampMode(cy);
  cz = NormalizeClampMode(cz);
  k.clamp_x = uint32_t(cx);
  k.clamp_y = uint32_t(cy);
  k.clamp_z = uint32_t(cz);
  k.border = (xenos::ClampModeUsesBorder(cx) || xenos::ClampModeUsesBorder(cy) ||
              xenos::ClampModeUsesBorder(cz))
                 ? uint32_t(fetch.border_color)
                 : uint32_t(xenos::BorderColor::k_ABGR_Black);
  uint32_t mip_min_level, mip_max_level;
  texture_util::GetSubresourcesFromFetchConstant(fetch, nullptr, nullptr, nullptr, nullptr, nullptr,
                                                 &mip_min_level, &mip_max_level);
  k.mip_min_level = mip_min_level;
  auto mag = binding.mag_filter == xenos::TextureFilter::kUseFetchConst ? fetch.mag_filter
                                                                        : binding.mag_filter;
  auto min = binding.min_filter == xenos::TextureFilter::kUseFetchConst ? fetch.min_filter
                                                                        : binding.min_filter;
  auto mip = binding.mip_filter == xenos::TextureFilter::kUseFetchConst ? fetch.mip_filter
                                                                        : binding.mip_filter;
  auto aniso = binding.aniso_filter == xenos::AnisoFilter::kUseFetchConst ? fetch.aniso_filter
                                                                          : binding.aniso_filter;
  aniso = std::min(aniso, xenos::AnisoFilter::kMax_16_1);
  // Linearly filtered textures with mips get at least `min_aniso`; render targets, lookup
  // tables and other point-sampled or single-level textures keep the game's filtering.
  if (mag == xenos::TextureFilter::kLinear && min == xenos::TextureFilter::kLinear &&
      mip != xenos::TextureFilter::kBaseMap && mip_max_level > mip_min_level) {
    aniso = std::max(aniso, min_aniso);
    k.lod_bias = lod_bias & 15;
    k.lod_soften = (lod_bias >> 4) & 15;
  }
  k.aniso = uint32_t(aniso);
  if (aniso != xenos::AnisoFilter::kDisabled) {
    k.mag_linear = k.min_linear = k.mip_linear = 1;
  } else {
    k.mag_linear = mag == xenos::TextureFilter::kLinear;
    k.min_linear = min == xenos::TextureFilter::kLinear;
    k.mip_linear = mip == xenos::TextureFilter::kLinear;
  }
  k.mip_base_map = mip == xenos::TextureFilter::kBaseMap;
  return k;
}

D3D12_SAMPLER_DESC SamplerDesc(const SamplerKey& k) {
  D3D12_SAMPLER_DESC d = {};
  if (xenos::AnisoFilter(k.aniso) != xenos::AnisoFilter::kDisabled) {
    d.Filter = D3D12_FILTER_ANISOTROPIC;
    d.MaxAnisotropy = 1u << (uint32_t(k.aniso) - 1);
  } else {
    d.Filter =
        D3D12_ENCODE_BASIC_FILTER(k.min_linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
                                  k.mag_linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
                                  k.mip_linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
                                  D3D12_FILTER_REDUCTION_TYPE_STANDARD);
    d.MaxAnisotropy = 1;
  }
  static const D3D12_TEXTURE_ADDRESS_MODE kAddress[] = {
      D3D12_TEXTURE_ADDRESS_MODE_WRAP,   D3D12_TEXTURE_ADDRESS_MODE_MIRROR,
      D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE,
      D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE,
      D3D12_TEXTURE_ADDRESS_MODE_BORDER, D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE,
  };
  d.AddressU = kAddress[k.clamp_x];
  d.AddressV = kAddress[k.clamp_y];
  d.AddressW = kAddress[k.clamp_z];
  d.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  switch (xenos::BorderColor(k.border)) {
    case xenos::BorderColor::k_ABGR_White:
      d.BorderColor[0] = d.BorderColor[1] = d.BorderColor[2] = d.BorderColor[3] = 1.0f;
      break;
    case xenos::BorderColor::k_ACBYCR_Black:
      d.BorderColor[0] = 0.5f;
      d.BorderColor[2] = 0.5f;
      break;
    case xenos::BorderColor::k_ACBCRY_Black:
      d.BorderColor[1] = 0.5f;
      d.BorderColor[2] = 0.5f;
      break;
    default:
      break;
  }
  d.MipLODBias = (float(k.lod_soften) - float(k.lod_bias)) * 0.25f;
  d.MinLOD = float(k.mip_min_level);
  if (k.mip_base_map) {
    d.MaxLOD = d.MinLOD;
    if (xenos::AnisoFilter(k.aniso) == xenos::AnisoFilter::kDisabled) d.MaxLOD += 0.25f;
  } else {
    d.MaxLOD = FLT_MAX;
  }
  return d;
}

// Whether `size` bytes at `a` and `b` are equal: 16 bytes at a time, four per round, no
// ordering (cheaper than memcmp).
inline bool Equal(const void* a, const void* b, uint32_t size) {
  auto* x = static_cast<const uint8_t*>(a);
  auto* y = static_cast<const uint8_t*>(b);
  auto load = [](const uint8_t* p) {
    return simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(p));
  };
  auto zero = [](simde__m128i v) {
    return simde_mm_movemask_epi8(simde_mm_cmpeq_epi8(v, simde_mm_setzero_si128())) == 0xFFFF;
  };
  uint32_t i = 0;
  for (; i + 64 <= size; i += 64) {
    const simde__m128i d = simde_mm_or_si128(
        simde_mm_or_si128(simde_mm_xor_si128(load(x + i), load(y + i)),
                          simde_mm_xor_si128(load(x + i + 16), load(y + i + 16))),
        simde_mm_or_si128(simde_mm_xor_si128(load(x + i + 32), load(y + i + 32)),
                          simde_mm_xor_si128(load(x + i + 48), load(y + i + 48))));
    if (!zero(d)) return false;
  }
  for (; i + 16 <= size; i += 16)
    if (!zero(simde_mm_xor_si128(load(x + i), load(y + i)))) return false;
  return i == size || std::memcmp(x + i, y + i, size - i) == 0;
}

// A constant buffer that is re-uploaded only when its bytes change within a frame. `last`
// holds the bytes of the current upload (the largest CBV is 256 float4 = 4 KB): new data is
// compared with it, and an upload streams it into upload memory that the CPU never reads back.
// gpu == 0: nothing current uploaded.
struct CachedCbv {
  static constexpr uint32_t kMaxSize = 256 * 16;
  alignas(16) uint8_t last[kMaxSize];
  uint32_t size = 0;
  D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
  D3D12_GPU_VIRTUAL_ADDRESS bound = 0;  // list state (ResetListState)
  // Forgets the upload, whose memory may be recycled: the next commit uploads `last` again.
  void Reset() {
    size = 0;
    gpu = 0;
  }
};

// Float constants packed by a shader's constant_register_map (UpdateBindings) straight into
// `cb.last`, compared with what was there on the way (one register = 16 bytes = one SSE
// load/store, no calls); 16 zero bytes for no shader or no constants (a CBV needs some data).
void PackFloats(CachedCbv& cb, const DxbcShader* shader, const uint32_t* regs, uint32_t reg_base) {
  const Shader::ConstantRegisterMap* map = shader ? &shader->constant_register_map() : nullptr;
  const uint32_t n = map ? map->float_count : 0;  // the bitmap's population count
  if (!n) {
    static constexpr uint8_t kZero[16] = {};
    if (cb.size != 16 || !Equal(cb.last, kZero, 16)) {
      std::memset(cb.last, 0, 16);
      cb.size = 16;
      cb.gpu = 0;
    }
    return;
  }
  const uint32_t size = n * 16;
  const bool resized = cb.size != size;
  cb.size = size;
  simde__m128i* out = reinterpret_cast<simde__m128i*>(cb.last);
  simde__m128i diff = simde_mm_setzero_si128();
  for (uint32_t i = 0; i < 4; ++i) {
    uint64_t bits = map->float_bitmap[i];
    const uint32_t* base = regs + reg_base + (i << 8);
    while (bits) {
      const uint32_t b = uint32_t(std::countr_zero(bits));
      bits &= bits - 1;
      const simde__m128i v =
          simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(base + (b << 2)));
      diff = simde_mm_or_si128(diff, simde_mm_xor_si128(v, simde_mm_load_si128(out)));
      simde_mm_store_si128(out++, v);
    }
  }
  if (resized ||
      simde_mm_movemask_epi8(simde_mm_cmpeq_epi8(diff, simde_mm_setzero_si128())) != 0xFFFF)
    cb.gpu = 0;
}

// Local change (pgr4 project): a menu's full-screen background in a frame wider than 16:9: a draw
// of the menus' vertex shaders (44-byte vertices at fetch 95, the position first, in the game's
// 640x480 space whose 16:9 sides are -106.67 and 746.67) whose every vertex is on a side or
// beyond, so it spans the 16:9 width. Not the video (texture 0 the Bink frame's resolve), which
// keeps its shape. FitHudOrtho then leaves its x as the game's: the background covers the frame.
bool FullWidthBackground(const RegisterFile& regs, const DxbcShader* vs, const uint8_t* membase,
                         uint32_t video_texture) {
  if (!vs || !membase || Upscaler::SceneHeight() == 720) return false;
  const uint64_t hash = vs->ucode_data_hash();
  if (hash != 0x19EE9CB8A3A22FD7ull && hash != 0x902579CC4AB6A791ull &&
      hash != 0xA1A5C0F8B117DC24ull)
    return false;
  const xenos::xe_gpu_vertex_fetch_t vf = regs.GetVertexFetch(95);
  const uint32_t address = vf.address << 2, size = vf.size << 2;
  if (size < 44 * 3 || size > 44 * 64 || size_t(address) + size > 0x20000000) return false;
  if (video_texture && (regs.GetTextureFetch(0).base_address << 12) == video_texture) return false;
  for (uint32_t v = 0; v * 44 + 4 <= size; ++v) {
    const float x = std::bit_cast<float>(
        __builtin_bswap32(*reinterpret_cast<const uint32_t*>(membase + address + v * 44)));
    if (x > -106.0f && x < 746.0f) return false;
  }
  return true;
}

// Local change (pgr4 project): the HUD's and the menus' orthographic projection, c45 of their
// vertex shaders: the game's 640x480 space into the central 960x720 of its 720-tall frame (x
// scale 3/1280, offset -0.75; y scale -3/720, offset 1), rewritten in the constants a draw uploads
// (after PackFloats). A taller frame (16:10) keeps the central 960x720, centered, for every HUD
// draw. A shorter one (21:9) fits its interface by the viewport (Draw, ui_fit) but for the
// translucent panels, whose x this shrinks to the frame's height, centered.
// Whether a draw's vertex shader reads the game's HUD and menus' projection in c45 (x scale
// 3/1280, offset -0.75; y scale -3/720, offset 1), as the game computes it: within a few units in
// the last place. `c` points at c45's four floats.
bool IsHudOrtho(const float* c) {
  const auto close = [](float a, float b) { return std::abs(a - b) <= std::abs(b) * 1e-5f; };
  return close(c[0], 3.0f / 1280.0f) && close(c[1], -3.0f / 720.0f) && close(c[2], -0.75f) &&
         close(c[3], 1.0f);
}

// Whether a draw is one of the HUD's or the menus': its vertex shader reads c45, and c45 holds the
// game's HUD projection. The first of a frame's on its final image starts the interface's phase
// (DrawRenderer::Impl::ui_phase).
bool ReadsHudOrtho(const RegisterFile& regs, const DxbcShader* vs) {
  if (!vs) return false;
  const Shader::ConstantRegisterMap& map = vs->constant_register_map();
  if (!map.float_count || !((map.float_bitmap[0] >> 45) & 1)) return false;
  float c[4];
  for (uint32_t k = 0; k < 4; ++k)
    c[k] = regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + 45 * 4 + k);
  return IsHudOrtho(c);
}

// The menus' translucent panels (the wet glass: vs 902579CC4AB6A791 and A1A5C0F8B117DC24) read the
// image behind them at the screen position their shader computes from oPos: fitted by their c45
// (FitHudOrtho) in a full-frame viewport, so that position stays where they are drawn, where the
// rest of the interface is fitted by its viewport.
// Which they are: the menus' vertex shaders that read c46 (the panel's texture mapping) and c255
// (the clip-to-texture helper, 1 and 0.5) besides c45: 902579CC4AB6A791, A1A5C0F8B117DC24, the
// glass 91A5322322B3B77D, which samples a copy of the screen at that position (in a fitted viewport
// it showed a wider ghost of the previous frame's panel). Not the race's 87B13831BF0A358D, whose
// c255 is a branch's flag and which has no c46.
bool IsScreenSampledHud(const DxbcShader* vs) {
  if (!vs) return false;
  const Shader::ConstantRegisterMap& map = vs->constant_register_map();
  return ((map.float_bitmap[0] >> 45) & 1) && ((map.float_bitmap[0] >> 46) & 1) &&
         ((map.float_bitmap[3] >> 63) & 1);
}

void FitHudOrtho(CachedCbv& cb, const DxbcShader* vs, bool full_width) {
  static const uint32_t h = Upscaler::SceneHeight();
  if (h == 720 || !vs || !REXCVAR_GET(pgr4_native_fit_hud)) return;
  if (full_width) return;  // the game's: the 16:9 screen over the whole frame
  const Shader::ConstantRegisterMap& map = vs->constant_register_map();
  const uint64_t bits = map.float_bitmap[0];
  if (!map.float_count || !((bits >> 45) & 1)) return;
  float* c = reinterpret_cast<float*>(cb.last + 16 * std::popcount(bits & ((1ull << 45) - 1)));
  if (h > 720) {
    if (!IsHudOrtho(c)) return;
    c[1] = -3.0f / float(h);
    c[3] = 720.0f / float(h);
  } else {
    // What the central 16:9 viewport does to the other draws: x scaled about the center by
    // height / 720, whatever the game's c45 (a popup's opening animation has its own); y as it is.
    const float s = float(h) / 720.0f;
    c[0] *= s;
    c[2] *= s;
  }
  cb.gpu = 0;  // uploaded again: what PackFloats compares with is the game's value next draw
  static bool logged = false;
  if (!logged) {
    logged = true;
    REXLOG_INFO("PGR4 native HUD: c45 fitted to the 1280x{} frame ({:016X})", h,
                vs->ucode_data_hash());
  }
}

}  // namespace

struct DrawRenderer::Impl {
  NativeRenderer& r;
  DrawRenderer::Stats& stats;
  ID3D12Device* device = nullptr;
  rex::memory::Memory* memory = nullptr;
  const uint8_t* membase = nullptr;
  ShaderCache shaders;
  BoolSpecializer vs_bools{shaders}, ps_bools{shaders};
  bool bool_specialize = REXCVAR_GET(pgr4_native_bool_specialize);
  ComPtr<ID3D12RootSignature> root_signature;
  uint64_t root_signature_hash = 0;  // names in the pipeline library depend on it

  // Shared memory: guest physical memory mirror and its page validity (cleared by CPU writes).
  ComPtr<ID3D12Resource> shared;
  // Its state in the list being recorded. Buffers are created in the common state and decay to
  // it when ExecuteCommandLists completes, so every new list starts from there (SyncListFrame,
  // and InvalidateListState after a submission).
  D3D12_RESOURCE_STATES shared_state = D3D12_RESOURCE_STATE_COMMON;
  D3D12_GPU_VIRTUAL_ADDRESS shared_gpu = 0;
  // Reserved shared buffer: which chunks have memory (set under shared_map_mutex, read without
  // it), and the tile heaps behind them. False: a committed buffer.
  bool shared_tiled = false;
  std::atomic<uint64_t> shared_mapped[kSharedChunkCount / 64] = {};
  std::mutex shared_map_mutex;
  std::vector<ComPtr<ID3D12Heap>> shared_heaps;
  uint32_t shared_mapped_chunks = 0;
  std::unique_ptr<std::atomic<uint64_t>[]> valid;
  void* invalidation_handle = nullptr;
  // Without the render thread: the copies of the draw being recorded. With it, Prepare
  // (guest thread) writes into the batch's vector instead: this one is the render thread's.
  std::vector<VertexCopy> copies;
  // Fence (or Prepare's tag) RequestRange tags vertex upload memory with.
  uint64_t upload_fence = 0;
  // Render thread mode: vertex_upload belongs to the guest thread (Prepare/EndFramePrepare).
  bool threaded = false;
  uint64_t prep_upload_bytes = 0, prep_uploads = 0;
  PrepareDiag prep_diag;  // the guest thread's, until EndFramePrepare hands it over
  // Copy segments (DrawRenderer::Prepare): what the current segment's draws read. Watched
  // ranges are copied in whole pages, so their reads are kept one bit per page, valid in the
  // 64-page words whose epoch is the segment's. Unwatched ranges (inline vertices, packed
  // back to back in the guest's command buffer and often several to a page) are copied to the
  // byte, so their reads are kept as byte intervals.
  struct Read {
    uint32_t begin, end;
    bool exact;
  };
  // This many exact intervals end the segment: the overlap test scans them all.
  static constexpr size_t kSegmentExactLimit = 64;
  uint32_t segment_epoch = 1;
  std::unique_ptr<uint32_t[]> segment_word_epoch;
  std::unique_ptr<uint64_t[]> segment_pages;
  std::vector<std::pair<uint32_t, uint32_t>> segment_exact;
  // The draw being prepared: its ranges, and whether one of its copies overwrites memory the
  // segment reads.
  std::vector<Read> draw_reads;
  bool preparing = false, segment_conflict = false;
  static uint64_t PageMask(uint32_t w, uint32_t first, uint32_t last) {
    uint64_t m = ~0ull;
    if (w == first >> 6) m &= ~0ull << (first & 63);
    if (w == last >> 6) m &= ~0ull >> (63 - (last & 63));
    return m;
  }
  void NewSegment() {
    ++segment_epoch;
    segment_exact.clear();
    if (coherency) coherency->segment_reads.clear();
  }
  bool SegmentReads(uint32_t begin, uint32_t end) const {
    for (const auto& [b, e] : segment_exact)
      if (b < end && begin < e) return true;
    const uint32_t first = begin >> kPageShift, last = (end - 1) >> kPageShift;
    for (uint32_t w = first >> 6; w <= last >> 6; ++w)
      if (segment_word_epoch[w] == segment_epoch && (segment_pages[w] & PageMask(w, first, last)))
        return true;
    return false;
  }
  void SegmentAdd(const Read& read) {
    if (read.exact) {
      // Successive inline blocks are separated by the draw packets and constant writes around
      // them: the merged interval covers those too, which no draw copies.
      constexpr uint32_t kMergeGap = 16 << 10;
      if (!segment_exact.empty() && segment_exact.back().second + kMergeGap >= read.begin &&
          read.end + kMergeGap >= segment_exact.back().first) {
        auto& last = segment_exact.back();
        last = {std::min(last.first, read.begin), std::max(last.second, read.end)};
      } else {
        segment_exact.emplace_back(read.begin, read.end);
      }
      return;
    }
    const uint32_t first = read.begin >> kPageShift, last = (read.end - 1) >> kPageShift;
    for (uint32_t w = first >> 6; w <= last >> 6; ++w) {
      if (segment_word_epoch[w] != segment_epoch) {
        segment_word_epoch[w] = segment_epoch;
        segment_pages[w] = 0;
      }
      segment_pages[w] |= PageMask(w, first, last);
    }
  }

  UploadPool vertex_upload{16u << 20};
  // Write-back pages, written with streaming stores (CommitCbv).
  UploadPool constant_upload{4u << 20, /*cpu_cached=*/true};

  // Descriptors.
  ComPtr<ID3D12DescriptorHeap> view_heap, sampler_heap;
  uint32_t view_stride = 0, sampler_stride = 0;
  D3D12_CPU_DESCRIPTOR_HANDLE view_cpu{}, sampler_cpu{};
  D3D12_GPU_DESCRIPTOR_HANDLE view_gpu{}, sampler_gpu{};
  uint32_t views_allocated = kViewTableStart + kFirstTexture;  // heap indices handed out so far
  uint32_t view_heap_size = 0;
  // A texture given the null view because the heap was full: forgotten at the frame's end (EndFrame).
  bool view_heap_overflow = false;
  uint32_t views_logged = 0;  // the journal's last step (EndFrame)
  uint32_t ViewLimit() const {
    const int32_t limit = REXCVAR_GET(pgr4_native_view_limit);
    return limit > 0 ? std::min(uint32_t(limit), view_heap_size) : view_heap_size;
  }
  std::vector<uint32_t> free_views;  // heap indices of evicted textures' views
  // Development: this frame's draws of the game's rain shader, by vertex shader (the log, EndFrame).
  struct RainDraw {
    uint32_t draws = 0, vertices = 0;
  };
  std::map<uint64_t, RainDraw> rain_draws;
  double rain_script_seen = -100.0;  // when the game last drew kRainScriptVs
  float rain_script[2] = {};  // development: its c209.x and c210.x
  uint32_t samplers_allocated = 0;
  // Each texture resource's views: (XXH3 of the SRV desc, table index). The views of a texture
  // the cache evicts go back to free_views (ReleaseViews).
  std::unordered_map<ID3D12Resource*, std::vector<std::pair<uint64_t, uint32_t>>> srv_views;
  std::unordered_map<uint32_t, uint32_t> sampler_indices;  // SamplerKey::value()
  // Direct-mapped memo in front of srv_views: (resource, component mapping, signed, fetch
  // dimension) -> descriptor index; a hit skips TextureDescriptor's desc build, XXH3 and map
  // lookup. Every other field of the SRV desc (format, view dimension, mips, array size) is fixed
  // per resource, so the key determines the index while the resource lives; ReleaseViews drops
  // the entries of evicted ones, whose addresses new resources may reuse.
  struct SrvMemo {
    ID3D12Resource* resource = nullptr;
    uint32_t mapping = 0;
    uint8_t is_signed = 0, dimension = 0xFF;
    uint32_t index = 0;
  };
  static constexpr uint32_t kSrvMemoBits = 12;
  std::unique_ptr<SrvMemo[]> srv_memo = std::make_unique<SrvMemo[]>(size_t(1) << kSrvMemoBits);
  // Direct-mapped memo in front of GetSamplerKey + sampler_indices: (6 fetch dwords, binding
  // mag/min/mip/aniso) -> sampler index; the sampler key is a function of exactly those.
  struct SamplerMemo {
    uint32_t fetch[6] = {};
    uint8_t filters[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    uint32_t lod_bias = 0;
    uint32_t index = 0;
  };
  static constexpr uint32_t kSamplerMemoBits = 10;
  std::unique_ptr<SamplerMemo[]> sampler_memo =
      std::make_unique<SamplerMemo[]>(size_t(1) << kSamplerMemoBits);
  xenos::AnisoFilter min_aniso = xenos::AnisoFilter::kDisabled;  // --pgr4_native_anisotropy
  // Fetch constants a shader reads (texture and sampler bindings), its descriptor table size and
  // its last BindMemo entry (checked before hashing), direct-mapped by shader.
  struct BindMemo;
  struct ShaderSlots {
    const DxbcShader* shader = nullptr;
    uint32_t mask = 0;   // fetch constants read
    uint32_t count = 0;  // bits in mask
    uint32_t n = 0;      // descriptor table entries (max bindless index + 1, at most 64)
    BindMemo* last = nullptr;
    // Takes the slot for `shader`. Cold and out of line: its loops vectorize with 256-bit
    // loads, and inlined they would make Draw run VZEROUPPER before its calls (build.sh).
    [[gnu::cold, gnu::noinline]] void Assign(const DxbcShader& shader);
  };
  ShaderSlots shader_slots[512];
  // Whole-shader binding memo, 2-way set-associative by (shader, the fetch constants it reads):
  // the descriptor table the shader's textures and samplers resolved to and the texture signs it
  // set. Valid while every texture it bound is unchanged (TextureCache::Unchanged: no guest
  // write, no resolve into it); `epoch` is the TextureCache::change_epoch at which that was last
  // known, so while the epoch stays the same the per-texture checks are skipped. A hit skips
  // TextureCache::Get and the descriptor memos for the whole shader. Keying on the fetch
  // constants keeps one entry per texture set a shader is drawn with (one entry per shader missed
  // ~900 times per frame on the race). Shaders with a missing texture are not memoized.
  struct BindMemo {
    static constexpr uint32_t kMaxSlots = 16, kMaxBound = 32, kMaxIndices = 64;
    // Fields a hit reads, packed so a hit reads one contiguous run (entry[] only when the epoch
    // moved, or at its first hit in a frame).
    const DxbcShader* shader = nullptr;
    uint32_t lod_bias = 0;  // the samplers' (TextureBindContext::lod_bias)
    uint64_t epoch = 0;
    uint64_t used = 0;  // bind_clock at the last fill or set lookup (the set's replacement order)
    uint64_t touched = 0;  // the frame fence its textures were last marked used with
    uint32_t bound = 0;    // textures bound, in order: their cache entries and signs[] writes
    uint32_t textures = 0;
    // The shader's fetch constants in ShaderSlots::mask order (count x 6 dwords), then its
    // descriptor table (n), then one signs[] write per bound texture (slot | value << 8).
    uint32_t data[kMaxSlots * 6 + kMaxIndices + kMaxBound];
    TextureHandle entry[kMaxBound];
  };
  static constexpr uint32_t kBindMemoSetBits = 11;
  std::unique_ptr<BindMemo[]> bind_memo =
      std::make_unique<BindMemo[]>(size_t(2) << kBindMemoSetBits);
  uint64_t bind_clock = 0;

  // Pipelines by PipelineKey hash. With --pgr4_native_async_pipelines new ones compile on
  // pso_threads and the draws that need them are skipped until they are ready
  // (stats.pso_pending). With pipeline_wait such a frame is not shown (FrameIncomplete): the
  // previous frame stays on screen while this one waits at its end for every pipeline its
  // draws skipped (WaitForPipelines, at most kPipelineWaitBudget per frame), compiled meanwhile
  // on all but two hardware threads, so no frame is shown with draws missing. A scene's first
  // frame needs many at once (180 at race start); a frame mid-race seldom needs more than one.
  // Waiting at each draw instead compiled one pipeline at a time, as the next draw requested
  // its own only once the previous one's was ready. Entries are never removed.
  // With --pgr4_native_pipeline_cache every new pipeline is recorded in `store`; the next run
  // translates and creates the recently used ones at start-up (Prewarm, a slice per frame) and
  // the workers load them from `library` instead of compiling them again. A record no recent
  // session used leaves the store: it stays the size of what the player plays.
  enum : uint8_t { kCompiling, kReady, kFailed };
  struct Pipeline {
    ComPtr<ID3D12PipelineState> pso;
    std::atomic<uint8_t> state{kCompiling};
    // The store record the pipeline was prewarmed from: the first draw that uses it touches the
    // record (it survives another kKeepSessions), then this is kNoEntry again (draw thread only).
    uint32_t store_entry = PipelineStore::kNoEntry;
    bool prewarm = false;  // its job was queued in prewarm_jobs (draw thread only)
    // A specialization whose draws use their unspecialized shaders' pipeline until it is ready
    // (draw thread only).
    bool behind_generic = false;
    uint64_t pending_frame = ~0ull;  // last frame it was added to frame_pending (draw thread)
    // Local change (pgr4 project): a surface (depth written) drawn with a replaced pixel shader
    // (the road, the cars: the slowest to compile, made again when the render scale or the MSAA
    // changes in game), whose draws skipped left holes flashing black; the others' still only
    // past --pgr4_native_pipeline_wait_min (draw thread only).
    bool surface = false;
    // Local change (pgr4 project): a replaced pixel shader's pipeline whose HLSL compiles in the
    // background: its description kept, made once the code is there (SelectPipeline). Made with
    // the game's unspecialized shader instead, it drew wrong (black or white flashes).
    uint64_t ps_ucode_hash = 0;  // development: named in the long frames' log
    bool awaiting_hlsl = false;
    const DxbcShader::Translation* await_ps = nullptr;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC await_desc = {};
  };
  std::unordered_map<uint64_t, std::unique_ptr<Pipeline>> pipelines;
  // Local change (pgr4 project): by a replaced pixel shader's pipeline key (its hash untagged),
  // its last pipeline ready, drawn with while one of a newer generation compiles (SelectPipeline).
  std::unordered_map<uint64_t, Pipeline*> replaced_ready;
  struct PipelineJob {
    Pipeline* target;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;  // bytecode and root signature outlive the job
    bool logged = false;                      // a draw needed it new: its compile time is logged
  };
  std::vector<std::thread> pso_threads;
  std::mutex pso_mutex;
  std::condition_variable pso_cv;        // background workers: a job was queued
  std::condition_variable pso_wait_cv;   // wait workers: a frame began waiting
  std::condition_variable pso_ready_cv;  // a job finished (notified under pso_mutex)
  std::chrono::steady_clock::time_point last_frame_end = std::chrono::steady_clock::now();  // development
  bool pso_waiting = false;              // WaitForPipelines is waiting (under pso_mutex)
  std::deque<PipelineJob> pso_jobs;      // a draw is waiting for these
  std::deque<PipelineJob> prewarm_jobs;  // taken only when pso_jobs is empty
  bool pso_stopping = false;
  static constexpr std::chrono::milliseconds kPipelineWaitBudget{500};
  bool pipeline_wait = false;
  bool generic_pipelines = false;        // --pgr4_native_generic_pipelines (with worker threads)
  std::vector<Pipeline*> frame_pending;  // pipelines of the frame's skipped draws, once each
  // Local change: --pgr4_native_pipeline_wait with at least --pgr4_native_pipeline_wait_min of
  // them (a scene's start), not a frame mid-race needing one or two.
  size_t pipeline_wait_min = 1;
  // Local change: or for any replaced surface's (Pipeline::surface).
  bool HoldsFrame() const {
    return pipeline_wait &&
           (frame_pending.size() >= pipeline_wait_min ||
            std::any_of(frame_pending.begin(), frame_pending.end(),
                        [](const Pipeline* p) { return p->surface; }));
  }
  PipelineStore store;
  ShaderReplacer replacer;  // local change: pixel shaders replaced by HLSL (shader_replace.h)
  // What made the pipelines created while running new (logged, kNewPipelineLog lines): a
  // shader pair or a render state no pipeline had before, bool or texture-sign variants.
  // Counted only while those lines are logged: nothing reads them afterwards.
  std::unordered_map<uint64_t, uint32_t> pipeline_states, pipeline_pairs;  // digest -> pipelines
  uint32_t new_pipelines_logged = 0;
  static constexpr uint32_t kNewPipelineLog = 64;
  static uint64_t StateDigest(const PipelineKey& key) {
    PipelineKey k = key;
    k.vs = k.ps = nullptr;
    return XXH3_64bits(&k, sizeof(k));
  }
  static uint64_t PairDigest(const PipelineKey& key) {
    const void* pair[2] = {key.vs, key.ps};
    return XXH3_64bits(pair, sizeof(pair));
  }
  std::once_flag library_open;
  PipelineLibrary library;  // pipeline workers only
  // D3D12 OPTIONS16 dynamic depth bias: pipelines are created with
  // D3D12_PIPELINE_STATE_FLAG_DYNAMIC_DEPTH_BIAS and keyed with a zero bias, and draws set it
  // with RSSetDepthBias.
  bool dynamic_depth_bias = false;
  ComPtr<ID3D12GraphicsCommandList9> list9;
  ID3D12GraphicsCommandList* list9_of = nullptr;
  std::filesystem::path library_path;
  size_t prewarm_next = 0;  // into store.loaded_pipelines()
  uint32_t prewarm_failed = 0, prewarm_frames = 0;
  double prewarm_ms = 0;
  std::unordered_map<uint32_t, std::vector<uint32_t>> geometry_shaders;

  // Shader lookup memo, direct-mapped by ucode address and size (plus the immediate load
  // sequence and the vertex shader variant's binding): a hit skips ShaderCache::Load's XXH3
  // over the whole ucode. An entry also keeps the first and last dwords of the ucode it was
  // made from, so a different shader loaded at the same address with the same size misses.
  // A race binds a few hundred shaders a frame.
  struct ShaderMemo {
    static constexpr uint32_t kBits = 8, kEntries = 1u << kBits, kSample = 16;
    static uint32_t Slot(const ShaderRef& ref) {
      return ((ref.ucode ^ (ref.dwords << 16)) * 0x9E3779B1u) >> (32 - kBits);  // Fibonacci
    }
    uint32_t ucode = 0, dwords = 0, immediate_seq = 0;
    uint64_t binding = 0;
    uint32_t sample[kSample] = {};
    // Local change (pgr4 project): the whole ucode's hash. The sample (8 dwords at each end) took
    // two programs the game puts at one address in turn, same size, ends alike, for one: a part
    // of a car or a bike drawn with another program's transform, until the entry went.
    uint64_t content = 0;
    DxbcShader* shader = nullptr;
  };
  ShaderMemo vs_memo[ShaderMemo::kEntries], ps_memo[ShaderMemo::kEntries];
  ShaderMemo* vs_last = nullptr;  // the entry the previous draw used
  ShaderMemo* ps_last = nullptr;
  // SelectModifications is a function of the two shaders and these registers: the last call.
  struct ModsMemo {
    const DxbcShader* vs = nullptr;
    const DxbcShader* ps = nullptr;
    ShaderDrawRegs sdr;
    ShaderModifications mods;
    uint32_t ps_textures = 0;  // fetch constants the pixel shader reads (bit per constant)
  } mods_memo;
  const uint64_t signed_is_unsigned_formats = SignedViewIsUnsignedFormats();
  // Translation per shader, direct-mapped by shader: the last modification it was translated
  // with (valid translations only; they live as long as the shader cache).
  struct TranslationMemo {
    const DxbcShader* shader = nullptr;
    uint64_t modification = 0;
    DxbcShader::Translation* translation = nullptr;
  };
  TranslationMemo translation_memo[256];
  // Ready pipelines by PipelineKey hash, direct-mapped, in front of GetPipeline's map (ready
  // pipelines are never replaced or freed).
  struct PsoMemo {
    uint64_t hash = 0;
    ID3D12PipelineState* pso = nullptr;
  };
  PsoMemo pso_memo[1024];

  // Draw path counters, logged every 300 frames with --pgr4_native_profile.
  struct Diag {
    uint64_t frames = 0, draws = 0, mods_hits = 0, translation_hits = 0, pso_hits = 0, flushes = 0;
    PrepareDiag prep;  // with the render thread, added at each Swap (AddPrepareCounters)
    uint64_t cbv_uploads[16] = {}, cbv_bytes[16] = {};  // by root parameter
    // Textures bound, by the fetch constant state the shaders read at run time: swizzled signs
    // all unsigned, gamma RGB + unsigned A, all gamma, all signed, other; then exponent bias
    // set, LOD bias set (any signs).
    uint64_t fetch_classes[7] = {};
  } diag;
  // Where Prepare's counters go: with the render thread, which owns diag, the guest thread's own.
  PrepareDiag& PrepDiag() { return threaded ? prep_diag : diag.prep; }

  // List state.
  uint64_t list_frame = UINT64_MAX;
  bool list_dirty = true;
  ID3D12PipelineState* bound_pso = nullptr;
  D3D_PRIMITIVE_TOPOLOGY bound_topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
  D3D12_VIEWPORT bound_viewport{};
  D3D12_RECT bound_scissor{};
  float bound_blend[4] = {};
  uint32_t bound_stencil_ref = UINT32_MAX;
  float bound_depth_bias = NAN, bound_slope_bias = NAN;
  D3D12_CPU_DESCRIPTOR_HANDLE bound_rtv[DxbcShaderTranslator::kMotionVectorRenderTarget + 1] = {};
  D3D12_CPU_DESCRIPTOR_HANDLE bound_dsv{};
  uint32_t bound_rt_count = UINT32_MAX;
  D3D12_INDEX_BUFFER_VIEW bound_ibv{};
  CachedCbv cb_system, cb_float_vs, cb_float_ps, cb_bool_loop, cb_fetch, cb_desc_vs, cb_desc_ps;
  // The motion vector pass's previous constants (b5, b6) as bound in the list.
  D3D12_GPU_VIRTUAL_ADDRESS bound_previous_float = 0, bound_previous_fetch = 0;
  MotionHistory motion_history;
  std::array<CachedCbv*, 7> Cbvs() {
    return {&cb_system, &cb_float_vs, &cb_float_ps, &cb_bool_loop,
            &cb_fetch,  &cb_desc_vs,  &cb_desc_ps};
  }

  // Oracle toggles from <capture dir>\draw-debug.txt (with --pgr4_native_capture_dir)
  // ("<mask> [max_draws] [skip_from] [skip_to] [shader hash]..."), read every 15 frames.
  // max_draws > 0 issues only the first N draws of a frame; draws whose vertex or pixel shader
  // ucode hash is listed are skipped (selected with kDebugOnlyListed). Hashes stay stable across
  // frames where draw indices do not.
  enum DebugBit : uint32_t {
    kDebugNoDepth = 1,  // depth and stencil off
    kDebugNoCull = 2,
    kDebugNoBlend = 4,
    kDebugNoAlphaTest = 8,
    kDebugLogDraws = 16,  // log the next frames' draws
    kDebugUploadEveryDraw = 32,
    kDebugNoDepthBias = 64,
    kDebugOnlyListed = 128,  // issue only the draws the hashes select
    kDebugSnapshots = 256,   // snap_pending (below)
  };
  uint32_t debug_mask = 0, debug_max_draws = 0, frame_draws = 0, debug_poll = 0;
  // Debug: the cube's sky's geometry in the shared buffer, read back (Draw, EndFrame).
  struct GeometryProbe {
    uint64_t fence = 0, frame = 0;
  };
  GeometryProbe geometry_probe[8];
  ComPtr<ID3D12Resource> geometry_buffer;
  uint64_t geometry_probed = ~0ull;
  // Local change (pgr4 project): the scene's sun (Draw), uploaded after the system constants.
  struct Sun {
    float direction[4] = {0.0f, 1.0f, 0.0f, 0.0f};
    float color[4] = {};  // w: 1 once known
  } sun;
  // Local change (pgr4 project): the replacement shaders' clock (SystemConstantsWithSun::time),
  // set once a frame so that the system constants stay the same across its draws.
  std::chrono::steady_clock::time_point clock_last = std::chrono::steady_clock::now();
  double clock_total = 0.0;  // seconds of the game's time running
  // Local change (pgr4 project): the game's falling particles (kRainShader) this frame, and the last time
  // it drew none for longer than kShelterSeconds: under a bridge, in a tunnel (SystemConstantsWithSun::shelter).
  bool particles_drawn = false, sheltered = false;
  double particles_seen = 0.0, shelter_began = 0.0, shelter_ended = 0.0;
  static constexpr double kShelterSeconds = 0.2;
  float clock_s = 0.0f;
  // Local change (pgr4 project): the weather's rain (SystemConstantsWithSun::time.yz), for the wet
  // roads (road_reflect_hooks.hlsli): the settings the game gives its rain's shader (kRainShader)
  // from the weather it chose at the race's load (its VS's c209.x: 0.075 in light rain, 0.1 in heavy
  // rain or a storm, but faded as the car speeds up; c210.x: 0.05, 0.025, fixed through the race),
  // kept through the race (the game draws no rain under a bridge or a shelter), zero once no wet
  // road has been drawn for kRainGoneFrames frames (a menu, a loading: the race left).
  float rain[2] = {};
  uint32_t frames_without_road = 0;
  bool road_drawn = false;
  // Local change (pgr4 project): the scene before its water (scene_copy.h), the frames counted
  // for it, and the copy's views made for (its generation).
  SceneCopy scene_copy;
  bool scene_copy_ready = false;
  uint64_t frame_count = 0;
  uint32_t scene_views_generation = 0;
  // ... and the wet roads': the copy at the scene's end (the roads, drawn before it in the frame,
  // read the previous frame's), with the clip transform and viewport mapping of its frame (`view`
  // as SystemConstantsWithSun::view), from that frame's roads, and its color's running average
  // (SceneCopy::set_accumulate). One: the average needs every frame's.
  struct History {
    SceneCopy copy;
    uint32_t views_generation = 0;
    float clip[16] = {};
    float view[12] = {};
    bool valid = false;
  };
  History history[1];
  float road_clip[16] = {}, road_view[12] = {};  // this frame's, from its last road draw
  void UpdateDropsGlide() {
    const auto now = std::chrono::steady_clock::now();
    const double dt = std::min(
        std::chrono::duration<double>(now - drops_scroll_time).count(), 0.25);
    drops_scroll_time = now;
    if (drops_eye_fresh) {
      std::memcpy(g_pgr4_camera_eye, drops_eye, sizeof(drops_eye));
      std::memcpy(drops_prev_eye, drops_eye, sizeof(drops_eye));
      drops_eye_fresh = false;
    }
    // The car's own speed, smoothed: the drops' clock and shape follow it (a third of a second).
    drops_speed += float((g_pgr4_car_speed - drops_speed) * std::min(1.0, dt * 3.0));
    for (int k = 0; k < 3; ++k)
      drops_side[k] += float((g_pgr4_car_side[k] - drops_side[k]) * std::min(1.0, dt * 4.0));
    for (int k = 0; k < 3; ++k)
      drops_back[k] += float((g_pgr4_car_back[k] - drops_back[k]) * std::min(1.0, dt * 2.0));
    const double moving = std::max<double>(drops_speed - REXCVAR_GET(pgr4_native_drops_glide_from), 0.0);
    // Still running down a little when the car stands still.
    const double step = g_pgr4_game_paused ? 0.0
                                           : (REXCVAR_GET(pgr4_native_drops_glide_rate) * moving +
                                              REXCVAR_GET(pgr4_native_drops_glide_idle)) * dt;
    // The texture repeats at 1: the scrolls stay precise however long the race. The second layer
    // goes at another speed, so the drops are not one picture sliding.
    drops_scroll[0] = float(std::fmod(drops_scroll[0] + step, 1.0));
    drops_scroll[1] = float(std::fmod(drops_scroll[1] + step * 1.37, 1.0));
    if (REXCVAR_GET(pgr4_native_drops_glide_log) && ++drops_log_frames >= 60) {
      drops_log_frames = 0;
      REXLOG_INFO("PGR4 native drops: eye {:.1f} {:.1f} {:.1f}, {:.1f} m/s, scroll {:.3f}, yaw {:.3f}, side {:.2f} {:.2f}",
                  drops_prev_eye[0], drops_prev_eye[1], drops_prev_eye[2], drops_speed,
                  drops_scroll[0], g_pgr4_car_yaw_rate, drops_side[0], drops_side[2]);
    }
  }
  bool road_seen = false;
  // Local change (pgr4 project): the videos. A Bink frame drawn (bink_drawn) is resolved into the
  // texture the menus show (video_texture, its guest address), which FitHudOrtho keeps 16:9.
  bool bink_drawn = false;
  uint32_t video_texture = 0;
  // Local change (pgr4 project): the interface's part of the frame (21:9): from the first HUD or
  // menu draw on the final image to the next scene draw (split screen) or the frame's end. Its
  // draws on the final image are made in the central 16:9 (Draw, ui_fit).
  bool ui_phase = false;
  // The race draws scene effects in the HUD's projection before its tone map (the sun's glare: its
  // flares and veils, 246A313B2DFCE07F, 2EE17BB253937C29), which the tone map's full-frame
  // rectangle list then composes with the scene: they span the whole frame like the scene. A frame
  // whose previous one had such a pass after a HUD draw (ui_wait_rect) starts its interface only
  // after that pass (ui_armed). ui_hud_seen: a HUD draw on the final image since the last scene draw.
  bool ui_hud_seen = false, ui_armed = false, ui_rect_after_hud = false, ui_wait_rect = false;
  // Local change (pgr4 project): the rain's running drops (drops_run.hlsli): their clock, summed from
  // the car's speed so a change of speed never makes them jump; the road draws' eye (their VS's c24)
  // for the code that needs the camera.
  float drops_eye[3] = {}, drops_prev_eye[3] = {};
  bool drops_eye_fresh = false, drops_has_prev = false;
  std::chrono::steady_clock::time_point drops_prev_time, drops_scroll_time;
  float drops_speed = 0.0f;
  float drops_side[3] = {};
  float drops_back[3] = {};
  float drops_scroll[2] = {};
  uint32_t drops_log_frames = 0;
  // The reactive mask's copy of the scene (Upscaler::SnapshotOpaque) is taken this frame and no
  // draw wrote depth since.
  bool opaque_taken = false;
  std::atomic<bool> upload_every_draw{false};  // kDebugUploadEveryDraw, for RequestRange's thread
  uint32_t debug_skip_from = 0, debug_skip_to = 0;
  std::string debug_path;  // empty without --pgr4_native_capture_dir: not polled
  std::array<uint64_t, 8> debug_hashes{};
  uint32_t debug_hash_count = 0;
  uint32_t draw_log_left = 0;  // frames still to log
  std::FILE* draw_log = nullptr;
  // Local change (pgr4 project): (shader ucode hash, fetch slot, TextureHash) of every binding
  // while pgr4_native_texture_catalog is on, written to texture_catalog.tsv (WriteCatalog).
  struct CatalogEntry {
    uint64_t shader, content;
    uint32_t slot, width, height, format;
    char hd = '0';  // TextureBinding::hd (the frame's list only; not part of its identity)
    // The frame's list only: the texture's mips, and the levels its fetch constant allows.
    uint32_t mips = 0, fetch_min = 0, fetch_max = 0;
    bool operator==(const CatalogEntry& o) const {
      return shader == o.shader && content == o.content && slot == o.slot;
    }
  };
  struct CatalogHash {
    size_t operator()(const CatalogEntry& e) const {
      return size_t((e.shader ^ e.content * 0x9E3779B97F4A7C15ull) + e.slot);
    }
  };
  std::unordered_set<CatalogEntry, CatalogHash> catalog;
  size_t catalog_written = 0;
  uint32_t catalog_frames = 0;
  std::string catalog_path;
  void WriteCatalog();
  // RecordFrameTextures: the path asked for (any thread, under frame_textures_mutex), then, from
  // the EndFrame that takes it to the next, the frame's bindings (render thread).
  std::mutex frame_textures_mutex;
  std::filesystem::path frame_textures_asked, frame_textures_path;
  bool frame_textures_on = false;
  std::unordered_set<CatalogEntry, CatalogHash> frame_textures;
  // 256: snapshots instead of skips. After the first draw of the next frame that uses each
  // listed hash (or, with none listed, after draw skip_from), the draw's bound single-sampled
  // targets are read back to <capture dir>\snap-f<frame>-d<draw>-{depth,c<i>}.bin, each with a
  // .txt holding "width height row_bytes dxgi_format".
  struct Snap {
    std::string stem;
    ComPtr<ID3D12Resource> readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    uint32_t rows = 0;
    uint64_t row_bytes = 0;
  };
  std::vector<Snap> snaps;
  uint64_t snap_fence = 0;
  uint32_t snap_pending = 0;  // bit i: listed hash i (bit 31: draw skip_from) still to snapshot
  void SnapshotTargets(ID3D12GraphicsCommandList* list, const BoundTargets& targets,
                       uint32_t draw_index);
  void Snapshot(ID3D12GraphicsCommandList* list, RenderTarget* rt, const std::string& stem);
  void WriteSnapshots(uint64_t completed_fence);

  Impl(NativeRenderer& renderer, DrawRenderer::Stats& s) : r(renderer), stats(s) {}
  ~Impl() {
    if (!pso_threads.empty()) {
      {
        std::lock_guard<std::mutex> lock(pso_mutex);
        pso_stopping = true;
      }
      pso_cv.notify_all();
      pso_wait_cv.notify_all();
      for (std::thread& t : pso_threads) t.join();
      library.Save();
    }
    if (arm_thread.joinable()) {
      {
        std::lock_guard<std::mutex> lock(arm_mutex);
        arm_stopping = true;
      }
      arm_cv.notify_one();
      arm_thread.join();
    }
    if (invalidation_handle && memory)
      memory->UnregisterPhysicalMemoryInvalidationCallback(invalidation_handle);
    r.textures().SetEvictCallback(nullptr);
    if (trace.file) {
      trace.Flush();
      std::fclose(trace.file);
    }
  }

  bool Initialize();
  void Draw(const DrawState& s, const DrawPrep* prep);
  // Draw's parts. Each has one call site, in Draw.
  //
  // At the first use of a frame's command list: forgets the previous list's state.
  void SyncListFrame();
  // The translation of `shader` with `modification` (translation_memo first), null on failure.
  DxbcShader::Translation* Translated(DxbcShader& shader, uint64_t modification);
  PipelineKey BuildPipelineKey(const RegisterFile& regs, xenos::PrimitiveType prim,
                               const ShaderModifications& mods, const DxbcShader::Translation* vst,
                               const DxbcShader::Translation* pst, uint8_t strip_cut,
                               bool rasterization, bool polygonal, reg::PA_SU_SC_MODE_CNTL sc,
                               reg::RB_DEPTHCONTROL depth_control, uint32_t color_mask,
                               const BoundTargets& targets) const;
  // The draw's line in draw_log (open).
  void LogDraw(const DrawState& s, const RegisterFile& regs, uint32_t draw_index, uint32_t count,
               const DxbcShader& vs, const DxbcShader* ps, const ShaderModifications& mods,
               const PipelineKey& key, const ViewportInfo& vp, const Scissor& scissor);
  // The ready pipeline for `key` (hashed here), or null when the draw is skipped (compiling or
  // failed, counted). Drawing with the pipeline of the unspecialized shaders sets `vs` and `ps`
  // to those.
  ID3D12PipelineState* SelectPipeline(const PipelineKey& key, const DxbcShader::Translation& vst,
                                      const DxbcShader::Translation* pst, DxbcShader*& vs,
                                      DxbcShader*& ps, DxbcShader* vs_generic,
                                      DxbcShader* ps_generic, uint64_t pixel_mod,
                                      uint64_t generic_pixel_mod, const ShaderDrawRegs& sdr,
                                      xenos::PrimitiveType prim);
  // What BindTextures reads besides the shader, and where it writes the texture signs.
  struct TextureBindContext {
    const DrawState& s;
    const RegisterFile& regs;
    ID3D12GraphicsCommandList* list;
    TextureCache& texture_cache;
    uint64_t frame_fence;
    uint64_t captured;  // TextureCache::MarkCapture stamp
    uint8_t* signs;     // [32], by fetch constant: the bound textures' swizzled signs
    uint32_t lod_bias;  // SamplerKey::lod_bias of the draw's samplers
    // Bit per fetch constant bound to a resolved image at a draw resolution scale above 1
    // (SystemConstants::textures_resolution_scaled). Local change (pgr4 project).
    uint32_t* resolution_scaled;
  };
  // The shader's descriptor table into `indices`; returns its entry count.
  uint32_t BindTextures(const TextureBindContext& ctx, const DxbcShader& shader, uint32_t* indices);
  void FillSystemConstants(const DrawState& s, const RegisterFile& regs, xenos::PrimitiveType prim,
                           bool polygonal, xenos::Endian index_endian, const ViewportInfo& vp,
                           uint32_t used_textures, const uint8_t* signs,
                           const BoundTargets& targets, DxbcShaderTranslator::SystemConstants& sys);
  DxbcShader* LoadShader(ShaderMemo* table, ShaderMemo*& last, const ShaderRef& ref,
                         xenos::ShaderType type);
  // Copies go to the end of `out`.
  bool RequestVertices(const DrawState& s, const DxbcShader& vs, std::vector<VertexCopy>& out);
  float FetchedMaxDepth(const DrawState& s, const RegisterFile& regs, const DxbcShader& vs,
                        uint32_t count);
  void Prepare(const DrawState& s, uint64_t tag, DrawPrep* prep, std::vector<VertexCopy>* out,
               std::vector<AuditRange>* audit);
  // A triangle fan as a host triangle list (v1, v2, v0), (v2, v3, v0)... like the plugin's
  // primitive processor; guest indices are copied raw (the VS still swaps them). The guest
  // indices are in physical memory (RequestVertices checked them); `count` >= 3. Out of line:
  // the vectorized interleaved stores are 256 bits wide, and inlined they would make Prepare
  // and Draw run VZEROUPPER before their calls (build.sh).
  [[gnu::noinline]] void WriteFanIndices(const DrawState& s, uint32_t count, bool wide,
                                         uint8_t* out) const;
  struct FanIndices {
    D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
    uint32_t count = 0;  // host indices, 0: none (out of upload memory)
    bool wide = false;   // 32-bit
  };
  // The host indices of the triangle fan `s` (s.count >= 3) in upload memory tagged `tag`.
  FanIndices UploadFanIndices(const DrawState& s, uint64_t tag);
  void ResetListState();
  // Whether [start, start + length) lies in guest physical memory (logged once if not).
  bool InSharedMemory(uint32_t start, uint32_t length);
  bool RequestRange(uint32_t start, uint32_t length, bool watch, std::vector<VertexCopy>& out);
  // Sets (or clears) the bits of pages [first, end) in a page bitmap (valid, pending). acq_rel
  // orders what each thread wrote before it ahead of the other's reads after it: RequestRange's
  // copy_hash stores before a valid bit it clears, the watch thread's valid bits before the
  // pending bits it clears.
  static void SetPageBits(std::atomic<uint64_t>* words, uint32_t first, uint32_t end, bool set) {
    for (uint32_t q = first; q < end;) {
      const uint32_t word_end = std::min(end, (q | 63) + 1);
      const uint32_t n = word_end - q;
      const uint64_t mask = (n == 64 ? ~0ull : ((1ull << n) - 1)) << (q & 63);
      if (set)
        words[q >> 6].fetch_or(mask, std::memory_order_acq_rel);
      else
        words[q >> 6].fetch_and(~mask, std::memory_order_acq_rel);
      q = word_end;
    }
  }
  // Gives the shared buffer memory under [start, start + length) if it is reserved. Any thread;
  // the tile mappings are queued ahead of the copies the caller records afterwards.
  bool MapShared(uint32_t start, uint32_t length);
  void FlushCopies(ID3D12GraphicsCommandList* list);
  void TransitionShared(ID3D12GraphicsCommandList* list, D3D12_RESOURCE_STATES to);
  // Uploads cb.size bytes of cb.last unless cb.gpu is current, and binds it at `root_index`.
  bool CommitCbv(CachedCbv& cb, uint32_t root_index, ID3D12GraphicsCommandList* list);
  // A motion vector draw's previous float and vertex fetch constants (b5, b6), after its own
  // are committed: the matching draw's of the previous frame (MotionHistory), or its own.
  void BindPreviousConstants(const DrawState& s, const DxbcShader& vs_generic, const DxbcShader* vs,
                             ID3D12GraphicsCommandList* list);
  bool UploadCbv(CachedCbv& cb, const void* data, uint32_t size, uint32_t root_index,
                 ID3D12GraphicsCommandList* list) {
    if (cb.size != size || !Equal(cb.last, data, size)) {
      std::memcpy(cb.last, data, size);
      cb.size = size;
      cb.gpu = 0;
    }
    return CommitCbv(cb, root_index, list);
  }
  uint32_t TextureDescriptor(const TextureBinding& b, const DxbcShader::TextureBinding& tb);
  // TextureCache::EvictCallback: frees the views of evicted textures.
  void ReleaseViews(const std::vector<ID3D12Resource*>& resources);
  uint32_t SamplerDescriptor(const SamplerKey& k);
  // The pipeline for `key` (`hash`: its XXH3), added and recorded in the store if new.
  // variant (for the log): 1 the vertex, 2 the pixel shader is a bool specialization, 4 the
  // pixel shader's texture signs are specialized. `background`: a new one compiles behind the
  // pipelines draws wait for (prewarm_jobs).
  Pipeline* GetPipeline(uint64_t hash, const PipelineKey& key, const DxbcShader::Translation& vs,
                        const DxbcShader::Translation* ps, uint32_t variant,
                        bool background = false);
  // `key`'s pipeline with the depth and stencil tests off and nothing written, for the coverage
  // of a draw inside an occlusion query (occlusion.h). Compiles in the background; nullptr until
  // it is ready.
  ID3D12PipelineState* CoveragePipeline(const PipelineKey& key, const DxbcShader::Translation& vs,
                                        const DxbcShader::Translation* ps);
  // Moves a pipeline's prewarm job, if no worker has taken it yet, to the draw queue: a draw
  // waits for it now.
  void PromotePipeline(Pipeline* p);
  void ResumeAwaiting(Pipeline* p);  // local change: Pipeline::awaiting_hlsl
  // Local change (pgr4 project): the paints seen, and their variants made in the background.
  bool IsCarShader(uint64_t hash);
  void PreparePaints();
  std::unordered_map<uint64_t, bool> car_shaders;
  std::unordered_set<uint64_t> paints_seen;
  // Development (capture dir set): the cars' pixel shaders drawn with no HLSL of ours, logged once each with
  // their name (shaders\shader_names.tsv), so their car is known and their ucode dumped (14/11/2026).
  std::unordered_set<uint64_t> car_unreplaced_seen;
  std::unordered_map<uint64_t, std::string> shader_names;
  bool shader_names_read = false;
  std::deque<DxbcShader*> paints_to_prepare;
  std::vector<uint32_t> paint_records;
  bool paint_records_known = false;
  size_t paint_record_next = 0;
  std::vector<Pipeline*> prepared_awaiting;
  // At the end of a frame: waits until none of the pipelines its draws skipped is compiling,
  // within kPipelineWaitBudget.
  void WaitForPipelines();
  // Adds the pipeline under `hash` and creates it (queued on pso_threads when they run; a
  // prewarm job waits behind every draw's job).
  // Local change: what a replaced pixel shader's pipelines' hash is XORed with (the replacer's
  // generation, plus one: never 0, which is the game's shader's), 0 for the others.
  uint64_t ReplacedTag(const DxbcShader* ps) const {
    return ps && replacer.Has(ps->ucode_data_hash())
               ? (replacer.generation() + 1) * 0x9E3779B97F4A7C15ull
               : 0;
  }
  Pipeline* CreatePipeline(uint64_t hash, const PipelineKey& key, const DxbcShader::Translation& vs,
                           const DxbcShader::Translation* ps, bool prewarm,
                           uint32_t store_entry = PipelineStore::kNoEntry);
  // `saves`: this worker saves the library when the workers have been idle for a while.
  // `on_wait`: this worker compiles only while WaitForPipelines waits.
  void PipelineWorker(bool saves, bool on_wait);
  // Per frame: translates and queues stored pipelines from earlier runs for up to ~2 ms.
  void Prewarm();
  [[noreturn]] void PrecompileAndExit();

  // A watched page may be unwatched once its valid bit is clear: the next request copies it
  // again and re-arms the watch. Unless the range must be exact, the pages around it that hold
  // no valid copy are offered for unwatching too (InvalidRunAround). Valid neighbours stay
  // watched: unwatching them too (one fault per kPluginUnwatchBlock instead of per 4 KB page)
  // tripled the bytes re-copied per frame.
  static std::pair<uint32_t, uint32_t> InvalidationThunk(void* context, uint32_t start,
                                                         uint32_t length, bool exact) {
    auto* self = static_cast<Impl*>(context);
    self->watch_faults.fetch_add(1, std::memory_order_relaxed);
    if (start >= kSharedMemorySize || !length) return {start, length};
    const uint32_t end = std::min(kSharedMemorySize, start + length);
    const uint32_t first = start >> kPageShift, last = (end - 1) >> kPageShift;
    const uint32_t frame = self->watch_frame.load(std::memory_order_relaxed);
    for (uint32_t p = first; p <= last; ++p) {
      const uint64_t bit = 1ull << (p & 63);
      std::atomic<uint64_t>& valid_word = self->valid[p >> 6];
      const bool was_valid = valid_word.fetch_and(~bit, std::memory_order_relaxed) & bit;
      if (self->trace.file)
        self->trace.Add(was_valid ? WatchTrace::kFaultValid : WatchTrace::kFaultOther, p, 1, 0);
      if (!was_valid) continue;
      std::atomic<uint8_t>& quick = self->quick_rewrites[p];
      uint8_t n = quick.load(std::memory_order_relaxed);
      n = frame - self->armed_frame[p].load(std::memory_order_relaxed) > kHotRewriteFrames
              ? 0
              : std::min<uint8_t>(n + 1, 2);
      quick.store(n, std::memory_order_relaxed);
      if (n == 2 && REXCVAR_GET(pgr4_native_watch_hot)) self->hot[p >> 6].fetch_or(bit, std::memory_order_relaxed);
      // The page after one written since the previous frame: a buffer being filled in order.
      // Its valid pages ahead, to the end of the kPluginUnwatchBlock (the valid word), are
      // invalidated with it, so the fill faults about once per block instead of once per page. In
      // the race this cut the faults on valid pages 4x, and under 1 % of the pages invalidated
      // ahead went unwritten.
      const uint32_t previous = p ? self->write_frame[p - 1].load(std::memory_order_relaxed) : 0;
      self->write_frame[p].store(frame + 1, std::memory_order_relaxed);
      const uint32_t shift = (p & 63) + 1;
      if (!previous || frame + 1 - previous > 1 || shift == 64) continue;
      const uint32_t ahead =
          uint32_t(std::countr_one(valid_word.load(std::memory_order_relaxed) >> shift));
      if (!ahead) continue;
      valid_word.fetch_and(~(((1ull << ahead) - 1) << shift), std::memory_order_relaxed);
      // Counted as written, so a fault on the next block's first page continues the sequence.
      for (uint32_t q = p + 1; q <= p + ahead; ++q)
        self->write_frame[q].store(frame + 1, std::memory_order_relaxed);
      if (self->trace.file) self->trace.Add(WatchTrace::kAhead, p + 1, ahead, 0);
    }
    return exact ? std::pair{start, length} : self->InvalidRunAround(first, last);
  }
  // Pages [first, last] widened to the nearest valid page on each side within the
  // kUnwatchWindow block, as a byte range. Pages without a valid copy lose nothing by being
  // unwatched (the next request copies and re-arms them). The memory unwatches only the
  // intersection of every callback's range, so with exact ranges a texture or buffer being
  // filled faulted once per 4 KB page although no consumer still watched its other pages (the
  // race's write faults came in runs of about 10 adjacent pages). A page made valid meanwhile is
  // re-armed after the unwatch: both hold the global lock.
  std::pair<uint32_t, uint32_t> InvalidRunAround(uint32_t first, uint32_t last) const {
    constexpr uint32_t kWindowPages = kUnwatchWindow >> kPageShift;
    const uint32_t window_first = first & ~(kWindowPages - 1);
    const uint32_t window_last = last | (kWindowPages - 1);
    uint32_t w = first >> 6;
    uint64_t bits = valid[w].load(std::memory_order_relaxed) & ((1ull << (first & 63)) - 1);
    while (!bits && w > window_first >> 6) bits = valid[--w].load(std::memory_order_relaxed);
    const uint32_t lo = bits ? (w << 6) + 64 - uint32_t(std::countl_zero(bits)) : window_first;
    w = last >> 6;
    bits = valid[w].load(std::memory_order_relaxed) & ~((2ull << (last & 63)) - 1);
    while (!bits && w < window_last >> 6) bits = valid[++w].load(std::memory_order_relaxed);
    const uint32_t end = bits ? (w << 6) + uint32_t(std::countr_zero(bits)) : window_last + 1;
    return {lo << kPageShift, (end - lo) << kPageShift};
  }
  // Dynamic vertex data. A valid page written again within kHotRewriteFrames of being armed,
  // twice in a row (once is more often a buffer being reused), becomes hot: data the game
  // rewrites every few frames, where each write fault and re-arm (a protection change, and a
  // TLB shootdown across the process's threads: tens of µs each under Wine) costs far more than
  // hashing the page. Hot pages are not watched: each request hashes them (about 0.1 µs) and
  // copies those whose content changed since the last copy. A hot page hashed kHotMaxHashes
  // times without a change (a large buffer that many draws read whole) is watched again, which
  // then costs less. Frames are counted by watch_frame on the thread that calls RequestRange.
  static constexpr uint32_t kHotRewriteFrames = 16;
  static constexpr uint16_t kHotMaxHashes = 64;
  std::unique_ptr<std::atomic<uint64_t>[]> hot;            // one bit per page
  std::unique_ptr<std::atomic<uint32_t>[]> armed_frame;    // watch_frame of the last arm
  std::unique_ptr<std::atomic<uint8_t>[]> quick_rewrites;  // quick rewrites in a row (to 2)
  // PageHash of the page taken just before its last whole-page copy, 0: none or not copied.
  std::unique_ptr<std::atomic<uint64_t>[]> copy_hash;
  std::unique_ptr<uint16_t[]> hot_hashes;  // hashes since the content last changed
  uint64_t PageHash(uint32_t page) const {
    return XXH3_64bits(membase + (size_t(page) << kPageShift), 1u << kPageShift) | 1;
  }
  // watch_frame + 1 of the last write, 0: none.
  std::unique_ptr<std::atomic<uint32_t>[]> write_frame;
  std::atomic<uint32_t> watch_frame{0};

  // Watches are armed on a thread of their own (WatchThread): a protection change takes about
  // 20 µs under Wine (mprotect and its TLB shootdown), and geometry streamed in arms hundreds in
  // a frame (td-175: 100-420 in about 25 frames of the race, 1.6-6.5 ms of the game thread).
  // A run RequestRange copies is hashed just before the copy and stays pending until then,
  // hashed by every request like a hot page. The watch thread marks it valid, arms it, and keeps
  // each page valid that still hashes as copied: a write before the arm shows in the hash, and
  // one after it faults and clears the valid bit, which the watch thread does not set again.
  // Runs queued back to back are armed with one protection change.
  std::unique_ptr<std::atomic<uint64_t>[]> pending;  // one bit per page
  struct ArmRun {
    uint32_t first, end;  // pages
    uint32_t request;     // WatchTrace's request number
  };
  std::mutex arm_mutex;
  std::condition_variable arm_cv;
  std::vector<ArmRun> arm_queue;
  bool arm_idle = false, arm_stopping = false;
  std::thread arm_thread;
  void QueueArm(uint32_t first, uint32_t end, uint32_t request);
  void WatchThread();

  // Pages hashed (hot, pending, before a copy) and copied as they changed since EndFrame.
  std::atomic<uint64_t> hash_pages{0}, hash_copies{0};
  // Write-watch faults (InvalidationThunk calls) since the last EndFrame; any guest thread.
  std::atomic<uint64_t> watch_faults{0};
  // The watch thread's protection changes, their pages, the pages found written since their
  // copy, and its time, since EndFrame.
  std::atomic<uint64_t> watch_arms{0}, watch_arm_pages{0}, watch_arm_changed{0}, watch_arm_ns{0};

  // --pgr4_native_watch_trace. Record: type (2 bits) << 62 | frame (22) << 40 | page (17) << 23
  // | pages (9, saturated) << 14 | request (14): arms of one RequestRange call share the request
  // number. Faults come from any guest thread, hence the mutex.
  struct WatchTrace {
    enum Type : uint64_t { kArm, kFaultValid, kFaultOther, kAhead };
    std::mutex mutex;
    std::vector<uint64_t> records;
    std::FILE* file = nullptr;
    uint32_t frame = 0;
    std::atomic<uint32_t> request{0};
    void Add(Type type, uint32_t page, uint32_t pages, uint32_t req) {
      std::lock_guard<std::mutex> lock(mutex);
      records.push_back(uint64_t(type) << 62 | uint64_t(frame & 0x3FFFFF) << 40 |
                        uint64_t(page & 0x1FFFF) << 23 | uint64_t(std::min(pages, 511u)) << 14 |
                        (req & 0x3FFF));
    }
    void Flush() {
      std::lock_guard<std::mutex> lock(mutex);
      if (file && !records.empty()) std::fwrite(records.data(), 8, records.size(), file);
      records.clear();
    }
  } trace;

  // --pgr4_native_coherency_check; RequestRange's thread only.
  struct Coherency {
    // What shared memory holds once the copies recorded so far are done, in Prepare order.
    std::unique_ptr<uint8_t[]> shadow =
        std::make_unique_for_overwrite<uint8_t[]>(kSharedMemorySize);
    // watch_frame + 1 when a page last matched guest memory, per request and at the Swap.
    std::unique_ptr<uint32_t[]> checked = std::make_unique<uint32_t[]>(kPageCount);
    std::unique_ptr<uint32_t[]> swap_checked = std::make_unique<uint32_t[]>(kPageCount);
    struct Range {
      uint32_t start, length;
      uint8_t slot;
    };
    std::vector<Range> ranges;  // watched ranges read since the last Swap
    // Every byte range the current copy segment's draws read (SegmentReads is conservative).
    std::vector<std::pair<uint32_t, uint32_t>> segment_reads;
    uint8_t slot = 0xFF;           // vertex fetch constant being requested, 0xFF: indices
    uint64_t stale = 0, late = 0;  // pages, since the last Swap
    std::vector<std::pair<uint32_t, uint32_t>> frame_copies;  // (start, size) since the Swap
  };
  std::unique_ptr<Coherency> coherency;
  void CheckRequest(uint32_t start, uint32_t length);
  void CheckSegmentCopy(uint32_t from, uint32_t size);
  void CheckLateWrites();
  std::shared_ptr<CoherencyJob> MakeCoherencyJob();
  static constexpr uint32_t kRegisterBlocks[6] = {0,      0x4000, 0x4400,
                                                  0x4800, 0x4900, kGuestRegisterCount};
  static void HashRegisterBlocks(const uint32_t* regs, uint64_t out[5]) {
    for (uint32_t i = 0; i < 5; ++i)
      out[i] = XXH3_64bits(regs + kRegisterBlocks[i],
                           (kRegisterBlocks[i + 1] - kRegisterBlocks[i]) * 4) |
               1;
  }
  // The GPU half (render thread): jobs whose read back is recorded, waiting for their fence.
  static constexpr uint32_t kCoherencyReadback = 8u << 20;
  std::shared_ptr<CoherencyJob> coherency_next;
  std::deque<std::shared_ptr<CoherencyJob>> coherency_inflight;
  std::vector<ComPtr<ID3D12Resource>> coherency_readbacks;  // free, kCoherencyReadback each
  void CheckCoherencyJobs(uint64_t completed_fence);

  // --pgr4_native_mark_audit. Guest thread: the frames still to audit, the ranges taken this
  // frame (once each: key AuditKey), and the frame's counts.
  static constexpr uint32_t kAuditReadback = 64u << 20;  // bytes read back per audited frame
  uint32_t audit_left = 0;
  std::unordered_set<uint64_t> audit_seen;
  uint64_t audit_draws = 0, audit_ranges = 0, audit_bytes = 0, audit_over = 0, audit_shaders = 0;
  static uint64_t AuditKey(const AuditRange& a) { return XXH3_64bits(&a, sizeof(a)); }
  void AuditShader(const ShaderRef& ref, const DxbcShader* shader, const char* type);
  // Render thread: the frame's read back, recorded after each audited draw's copies, and the
  // frames waiting for their fence.
  struct AuditJob {
    struct Entry {
      AuditRange range;
      uint32_t offset;  // in the read back
      uint32_t draw;    // frame_draws when it was recorded (the draw log's index of the draw)
    };
    uint64_t frame = 0, fence = 0;
    ComPtr<ID3D12Resource> readback;
    std::vector<Entry> entries;
    uint32_t used = 0, over = 0;
  };
  std::unique_ptr<AuditJob> audit_job;
  std::deque<std::unique_ptr<AuditJob>> audit_inflight;
  std::vector<ComPtr<ID3D12Resource>> audit_readbacks;  // free, kAuditReadback each
  void AuditDraw(ID3D12GraphicsCommandList* list, const DrawPrep& prep);
  void CheckAuditJobs(uint64_t completed_fence);
};

// --pgr4_native_coherency_check: one frame's copied shared memory ranges, the bytes they should
// hold once the frame's copies are done (the shadow's at the frame's Swap), and their read back.
struct CoherencyJob {
  uint32_t frame = 0;
  std::vector<std::pair<uint32_t, uint32_t>> runs;  // (start, size), sorted and disjoint
  std::vector<uint8_t> expected;                    // the runs' bytes back to back
  uint64_t fence = 0;
  ComPtr<ID3D12Resource> readback;
};

bool DrawRenderer::Impl::Initialize() {
  device = r.d3d12();
  memory = rex::system::kernel_memory();
  if (!memory) {
    REXLOG_ERROR("PGR4 native draw: no guest memory");
    return false;
  }
  membase = memory->physical_membase();
  vertex_upload.SetDevice(device);
  threaded = REXCVAR_GET(pgr4_native_render_thread);
  // kMax_N_1 is log2(N) + 1.
  if (const int32_t samples = std::clamp(REXCVAR_GET(pgr4_native_anisotropy), 1, 16); samples > 1)
    min_aniso = xenos::AnisoFilter(std::bit_width(uint32_t(samples)));
  if (const std::string dir = REXCVAR_GET(pgr4_native_capture_dir); !dir.empty())
    debug_path = (std::filesystem::path(dir) / "draw-debug.txt").string();
  if (const std::string dir = REXCVAR_GET(pgr4_native_capture_dir); !dir.empty())
    catalog_path = (std::filesystem::path(dir) / "texture_catalog.tsv").string();
  constant_upload.SetDevice(device);
  // Race start keeps 3 vertex and 10 constant blocks in flight; creating one there takes up to
  // 16 ms.
  vertex_upload.Reserve(4);
  constant_upload.Reserve(12);
  root_signature = CreateBindlessRootSignature(device, &root_signature_hash);
  if (!root_signature) return false;

  const D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT};
  const D3D12_RESOURCE_DESC rd =
      BufferDesc(kSharedMemorySize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  // Tier 2: reads of chunks without memory return zero, as in a committed buffer never written.
  D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
  shared_tiled = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options,
                                                       sizeof(options))) &&
                 options.TiledResourcesTier >= D3D12_TILED_RESOURCES_TIER_2 &&
                 SUCCEEDED(device->CreateReservedResource(&rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                          IID_PPV_ARGS(&shared)));
  if (!shared_tiled) {
    if (Failed(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                               D3D12_RESOURCE_STATE_COMMON, nullptr,
                                               IID_PPV_ARGS(&shared)),
               "shared memory buffer (512 MB)"))
      return false;
  }
  shared_gpu = shared->GetGPUVirtualAddress();
  // Zeroed: make_unique value-initializes, and std::atomic's default constructor value-initializes
  // its value (C++20).
  valid = std::make_unique<std::atomic<uint64_t>[]>(kPageCount / 64);
  hot = std::make_unique<std::atomic<uint64_t>[]>(kPageCount / 64);
  armed_frame = std::make_unique<std::atomic<uint32_t>[]>(kPageCount);
  write_frame = std::make_unique<std::atomic<uint32_t>[]>(kPageCount);
  quick_rewrites = std::make_unique<std::atomic<uint8_t>[]>(kPageCount);
  copy_hash = std::make_unique<std::atomic<uint64_t>[]>(kPageCount);
  hot_hashes = std::make_unique<uint16_t[]>(kPageCount);
  pending = std::make_unique<std::atomic<uint64_t>[]>(kPageCount / 64);
  segment_word_epoch = std::make_unique<uint32_t[]>(kPageCount / 64);
  segment_pages = std::make_unique<uint64_t[]>(kPageCount / 64);
  if (const std::string& path = REXCVAR_GET(pgr4_native_watch_trace); !path.empty())
    trace.file = std::fopen(path.c_str(), "wb");
  if (REXCVAR_GET(pgr4_native_coherency_check)) {
    coherency = std::make_unique<Coherency>();
    REXLOG_INFO("PGR4 native draw: coherency check on");
  }
  invalidation_handle = memory->RegisterPhysicalMemoryInvalidationCallback(InvalidationThunk, this);
  arm_thread = std::thread([this] { WatchThread(); });

  D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 0,
                                   D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  for (const uint32_t size : kViewHeapSizes) {
    hd.NumDescriptors = view_heap_size = size;
    if (SUCCEEDED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&view_heap)))) break;
  }
  if (!view_heap) {
    REXLOG_ERROR("PGR4 native draw: no view heap");
    return false;
  }
  hd = {D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerHeapSize,
        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  if (Failed(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&sampler_heap)), "draw sampler heap"))
    return false;
  view_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  sampler_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  view_cpu = view_heap->GetCPUDescriptorHandleForHeapStart();
  view_gpu = view_heap->GetGPUDescriptorHandleForHeapStart();
  sampler_cpu = sampler_heap->GetCPUDescriptorHandleForHeapStart();
  sampler_gpu = sampler_heap->GetGPUDescriptorHandleForHeapStart();

  auto view_at = [&](uint32_t i) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = view_cpu;
    h.ptr += size_t(i) * view_stride;
    return h;
  };
  D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
  sd.Format = DXGI_FORMAT_R32_TYPELESS;
  sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  sd.Buffer.NumElements = kSharedMemorySize / 4;
  sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
  device->CreateShaderResourceView(shared.Get(), &sd, view_at(0));
  D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
  ud.Format = DXGI_FORMAT_R32_TYPELESS;
  ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  ud.Buffer.NumElements = kSharedMemorySize / 4;
  ud.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
  device->CreateUnorderedAccessView(shared.Get(), nullptr, &ud, view_at(1));
  // Null textures for unbound or mismatching bindings.
  D3D12_SHADER_RESOURCE_VIEW_DESC nd = {};
  nd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
  nd.Texture2DArray.MipLevels = 1;
  nd.Texture2DArray.ArraySize = 1;
  device->CreateShaderResourceView(nullptr, &nd, view_at(kViewTableStart + kNull2DArray));
  nd = {};
  nd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
  nd.Texture3D.MipLevels = 1;
  device->CreateShaderResourceView(nullptr, &nd, view_at(kViewTableStart + kNull3D));
  nd = {};
  nd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
  nd.TextureCube.MipLevels = 1;
  device->CreateShaderResourceView(nullptr, &nd, view_at(kViewTableStart + kNullCube));
  // No running average: the road's swarming it hid was its textures read at their sharpest
  // (ucode2hlsl's register LOD), and it trailed the camera.
  history[0].copy.set_accumulate(false);
  scene_copy_ready = scene_copy.Initialize(device) && history[0].copy.Initialize(device);
  r.textures().SetEvictCallback(
      [this](const std::vector<ID3D12Resource*>& resources) { ReleaseViews(resources); });
  REXLOG_INFO(
      "PGR4 native draw: up (shared memory 512 MB {}, bindless heaps {} views / {} samplers)",
      shared_tiled ? "reserved" : "committed", view_heap_size, kSamplerHeapSize);
  D3D12_FEATURE_DATA_D3D12_OPTIONS16 options16 = {};
  dynamic_depth_bias = REXCVAR_GET(pgr4_native_dynamic_depth_bias) &&
                       SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS16,
                                                             &options16, sizeof(options16))) &&
                       options16.DynamicDepthBiasSupported;
  REXLOG_INFO("PGR4 native draw: dynamic depth bias {}", dynamic_depth_bias ? "on" : "off");
  if (REXCVAR_GET(pgr4_native_pipeline_cache)) {
    // The runtime's cache root (userdata\cache unless --cache_root): co_run keeps it.
    rex::Runtime* runtime =
        rex::system::kernel_state() ? rex::system::kernel_state()->emulator() : nullptr;
    std::filesystem::path dir = runtime ? runtime->cache_root() : std::filesystem::path();
    if (dir.empty()) {
      REXLOG_WARN("PGR4 native pipelines: no cache root, no pipeline cache");
    } else {
      // Shaders translate differently at another draw resolution scale, and the pipeline
      // library is keyed by guest shader hashes: one cache per scale (local change).
      dir /= DrawResolutionScale() > 1 ? "pgr4-native-scale" + std::to_string(DrawResolutionScale())
                                       : std::string("pgr4-native");
      // Local change: the base list shipped beside the executable (pipelines_base.bin, every
      // city's pipelines), imported into this scale's store.
      wchar_t exe[MAX_PATH] = {};
      GetModuleFileNameW(nullptr, exe, MAX_PATH);
      std::filesystem::path base_list = std::filesystem::path(exe).parent_path() / "pipelines_base.bin";
      std::error_code base_ec;
      if (!std::filesystem::exists(base_list, base_ec)) base_list.clear();
      store.Open(dir, sizeof(PipelineKey), kPipelineKeyVersion, base_list);
      library_path = dir / "pipelines.d3d12lib";
    }
  }
  replacer.Initialize();
  if (REXCVAR_GET(pgr4_native_async_pipelines)) {
    pipeline_wait = REXCVAR_GET(pgr4_native_pipeline_wait);
    pipeline_wait_min = size_t(std::max(1, REXCVAR_GET(pgr4_native_pipeline_wait_min)));
    generic_pipelines = REXCVAR_GET(pgr4_native_generic_pipelines);
    int threads = REXCVAR_GET(pgr4_native_pipeline_threads);
    if (threads <= 0) threads = int(std::thread::hardware_concurrency() / 4);
    threads = std::clamp(threads, 1, 4);
    // While a frame waits for its pipelines, the game threads wait as well.
    const int total = std::max(threads, std::min(int(std::thread::hardware_concurrency()) - 2, 8));
    for (int i = 0; i < total; ++i) {
      pso_threads.emplace_back([this, i, threads] {
        // Each thread names itself: Wine gives the Linux thread a name only once that thread has
        // started (set_native_thread_name), so a name its creator sets at once can be lost.
        wchar_t name[32];
        std::swprintf(name, 32, L"PGR4 native pipelines %d", i);
        SetThreadDescription(GetCurrentThread(), name);
        PipelineWorker(i == 0, i >= threads);
      });
    }
  }
  if (REXCVAR_GET(pgr4_native_precompile) && !library_path.empty() && !pso_threads.empty())
    PrecompileAndExit();
  Prewarm();  // the first slice before the first frame, which needs some of them
  return true;
}

void DrawRenderer::Impl::PipelineWorker(bool saves, bool on_wait) {
  std::call_once(library_open, [this] { library.Open(device, library_path, root_signature_hash); });
  std::unique_lock<std::mutex> lock(pso_mutex);
  // Local change (pgr4 project): prewarm on one worker only (the saving one), below normal
  // priority. With every worker on it, the driver's pipeline creations ran through the first
  // minutes of a race and made the render thread's D3D12 calls about five times slower, one
  // frame in four (10 ms of recording instead of 6). The draws' own pipelines keep every worker.
  auto has_work = [this, saves] {
    return pso_stopping || !pso_jobs.empty() || (saves && !prewarm_jobs.empty());
  };
  int priority = THREAD_PRIORITY_NORMAL;
  for (;;) {
    if (on_wait) {
      pso_wait_cv.wait(lock, [this] { return pso_stopping || (pso_waiting && !pso_jobs.empty()); });
    } else if (saves && library.dirty()) {
      // Save once no new pipeline came for 3 s (the burst is over).
      if (!pso_cv.wait_for(lock, std::chrono::seconds(3), has_work)) {
        lock.unlock();
        library.Save();
        lock.lock();
        continue;
      }
    } else {
      pso_cv.wait(lock, has_work);
    }
    if (pso_stopping) return;  // pending pipelines never get used
    const bool prewarm_job = pso_jobs.empty();
    std::deque<PipelineJob>& queue = prewarm_job ? prewarm_jobs : pso_jobs;
    if (queue.empty()) continue;
    const PipelineJob job = queue.front();
    queue.pop_front();
    lock.unlock();
    if (const int want = prewarm_job ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_NORMAL;
        want != priority) {
      SetThreadPriority(GetCurrentThread(), want);
      priority = want;
    }
    ComPtr<ID3D12PipelineState> pso;
    bool loaded;
    const auto t0 = std::chrono::steady_clock::now();
    const HRESULT hr = library.Create(job.desc, &pso, &loaded);
    if (job.logged)
      PGR4_LOG_FIRST(
          kNewPipelineLog, INFO, "PGR4 native pipelines: compiled in {:.1f} ms{}",
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(),
          loaded ? " (from the library)" : "");
    if (SUCCEEDED(hr)) {
      job.target->pso = std::move(pso);
      job.target->state.store(kReady, std::memory_order_release);
    } else {
      PGR4_WARN_ONCE("PGR4 native draw: CreateGraphicsPipelineState failed {:08X}", uint32_t(hr));
      job.target->state.store(kFailed, std::memory_order_release);
    }
    lock.lock();
    pso_ready_cv.notify_all();
  }
}

void DrawRenderer::Impl::ResetListState() {
  list_dirty = true;
  bound_pso = nullptr;
  bound_topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
  bound_viewport = {};
  bound_scissor = {};
  std::memset(bound_blend, 0xFF, sizeof(bound_blend));  // NaN: never equal
  bound_stencil_ref = UINT32_MAX;
  bound_depth_bias = bound_slope_bias = NAN;
  for (auto& h : bound_rtv) h.ptr = 0;
  bound_dsv.ptr = 0;
  bound_rt_count = UINT32_MAX;
  bound_ibv = {};
  for (CachedCbv* cb : Cbvs()) cb->bound = 0;
  bound_previous_float = bound_previous_fetch = 0;
}

void DrawRenderer::Impl::TransitionShared(ID3D12GraphicsCommandList* list,
                                          D3D12_RESOURCE_STATES to) {
  if (shared_state == to) return;
  const D3D12_RESOURCE_BARRIER b = TransitionBarrier(shared.Get(), shared_state, to);
  list->ResourceBarrier(1, &b);
  shared_state = to;
}

bool DrawRenderer::Impl::InSharedMemory(uint32_t start, uint32_t length) {
  if (start < kSharedMemorySize && kSharedMemorySize - start >= length) return true;
  PGR4_WARN_ONCE("PGR4 native draw: range {:08X}+{:X} outside physical memory", start, length);
  return false;
}

bool DrawRenderer::Impl::RequestRange(uint32_t start, uint32_t length, bool watch,
                                      std::vector<VertexCopy>& out) {
  if (!length) return true;
  if (!InSharedMemory(start, length)) return false;
  const bool exact = !watch || upload_every_draw.load(std::memory_order_relaxed);
  if (preparing) draw_reads.emplace_back(start, start + length, exact);
  const uint64_t fence = upload_fence;
  PrepareDiag& pd = PrepDiag();
  auto copy = [&](uint32_t from, uint32_t size) {
    if (preparing && !segment_conflict) segment_conflict = SegmentReads(from, from + size);
    if (coherency && preparing) CheckSegmentCopy(from, size);
    if (!MapShared(from, size)) return false;
    UploadPool::Alloc a;
    if (!vertex_upload.Allocate(size, 16, fence, &a)) return false;
    if (coherency) {
      // The upload is made from the shadow, so the two hold the same bytes.
      std::memcpy(coherency->shadow.get() + from, membase + from, size);
      std::memcpy(a.cpu, coherency->shadow.get() + from, size);
      coherency->frame_copies.emplace_back(from, size);
    } else {
      std::memcpy(a.cpu, membase + from, size);
    }
    // Local change (pgr4 project): the water plane's vertices, larger (WidenWaterPlane).
    if (water_plane_size && EnhancedReflections() && from <= water_plane_start &&
        water_plane_start + water_plane_size <= from + size)
      WidenWaterPlane(a.cpu + (water_plane_start - from), water_plane_size);
    out.emplace_back(a.resource, a.offset, from, size);
    if (threaded) {
      prep_upload_bytes += size;
      ++prep_uploads;
    } else {
      stats.upload_bytes += size;
      ++stats.uploads;
    }
    return true;
  };
  if (exact) {
    ++pd.unwatched;
    return copy(start, length);
  }
  ++pd.ranges;
  const uint32_t first = start >> kPageShift, last = (start + length - 1) >> kPageShift;
  // Local change (pgr4 project, 14/11/2026): without hot pages (pgr4_native_watch_hot false, live), a hot page met
  // here leaves that state invalid: copied below and watched again like any other.
  if (!REXCVAR_GET(pgr4_native_watch_hot)) {
    for (uint32_t w = first >> 6; w <= (last >> 6); ++w) {
      uint64_t bits = hot[w].load(std::memory_order_relaxed);
      if (w == first >> 6) bits &= ~0ull << (first & 63);
      if (w == last >> 6) bits &= ~0ull >> (63 - (last & 63));
      if (!bits || (pending[w].load(std::memory_order_relaxed) & bits)) continue;
      hot[w].fetch_and(~bits, std::memory_order_relaxed);
      valid[w].fetch_and(~bits, std::memory_order_relaxed);
    }
  }
  // Local change (pgr4 project, 14/11/2026): a range with a hot page (rewritten again and again: vertices the CPU
  // animates, the bikes' riders) is copied whole, as kDebugUploadEveryDraw does: hashed page by page, the rider drew
  // now and then another frame's pose, a page caught as the game rewrote it (gone with every draw uploaded, and with
  // the coherency check's slower copies).
  // Small ranges only (a character's vertices): its hot pages are then never hashed again and stay hot, so a large
  // range copied whole every draw cost 40 ms a frame in a race (14/11/2026).
  if (const int32_t max_kb = REXCVAR_GET(pgr4_native_hot_ranges_exact_kb);
      max_kb > 0 && length <= uint32_t(max_kb) * 1024u) {
    bool any_hot = false;
    for (uint32_t w = first >> 6; w <= (last >> 6) && !any_hot; ++w) {
      uint64_t bits = hot[w].load(std::memory_order_relaxed);
      if (w == first >> 6) bits &= ~0ull << (first & 63);
      if (w == last >> 6) bits &= ~0ull >> (63 - (last & 63));
      any_hot = bits != 0;
    }
    if (any_hot) {
      ++pd.unwatched;
      return copy(start, length);
    }
  }
  pd.range_pages += last - first + 1;
  const uint32_t trace_request =
      trace.file ? trace.request.fetch_add(1, std::memory_order_relaxed) : 0;
  const uint32_t frame = watch_frame.load(std::memory_order_relaxed);
  // Hashed pages whose content changed, copied in runs of adjacent pages.
  uint32_t changed_first = 0, changed_end = 0;
  auto copy_changed = [&] {
    if (changed_first == changed_end ||
        copy(changed_first << kPageShift, (changed_end - changed_first) << kPageShift))
      return true;
    // Hashed as changed but not copied: the next request copies them. The watch thread may
    // have found a pending one unchanged meanwhile, hence the valid bits.
    for (uint32_t q = changed_first; q < changed_end; ++q)
      copy_hash[q].store(0, std::memory_order_relaxed);
    SetPageBits(valid.get(), changed_first, changed_end, false);
    return false;
  };
  uint64_t hashed = 0, changed = 0;
  uint32_t p = first;
  while (p <= last) {
    // Skip valid pages that are neither hot nor pending a 64-page word at a time (whole vertex
    // buffers are requested per draw).
    const uint32_t w = p >> 6;
    // Pending before valid: a pending bit seen clear makes the valid bit the watch thread left
    // visible.
    const uint64_t pending_bits = pending[w].load(std::memory_order_acquire);
    const uint64_t hot_bits = hot[w].load(std::memory_order_relaxed);
    uint64_t work =
        (~valid[w].load(std::memory_order_relaxed) | hot_bits | pending_bits) & (~0ull << (p & 63));
    if (w == last >> 6) work &= ~0ull >> (63 - (last & 63));
    if (!work) {
      p = (w + 1) << 6;
      continue;
    }
    p = (w << 6) + uint32_t(std::countr_zero(work));
    const uint64_t bit = 1ull << (p & 63);
    if ((hot_bits | pending_bits) & bit) {
      ++hashed;
      const uint64_t hash = PageHash(p);
      if (hash != copy_hash[p].load(std::memory_order_relaxed)) {
        copy_hash[p].store(hash, std::memory_order_relaxed);
        hot_hashes[p] = 0;
        ++changed;
        if (changed_end != p) {
          if (!copy_changed()) return false;
          changed_first = p;
        }
        changed_end = p + 1;
        ++p;
        continue;
      }
      if ((pending_bits & bit) || ++hot_hashes[p] < kHotMaxHashes) {
        ++p;
        continue;
      }
      // Hot, but read far more often than written: watched again. Its copy is current.
      hot[w].fetch_and(~bit, std::memory_order_relaxed);
      quick_rewrites[p].store(0, std::memory_order_relaxed);
      armed_frame[p].store(frame, std::memory_order_relaxed);
      pending[w].fetch_or(bit, std::memory_order_relaxed);
      QueueArm(p, p + 1, trace_request);
      ++p;
      continue;
    }
    uint32_t run_end = p + 1;
    while (run_end <= last) {
      const uint32_t rw = run_end >> 6;
      if ((valid[rw].load(std::memory_order_relaxed) | hot[rw].load(std::memory_order_relaxed) |
           pending[rw].load(std::memory_order_relaxed)) &
          (1ull << (run_end & 63)))
        break;
      ++run_end;
    }
    // Hashed before the copy, so a write between the two shows as a change.
    for (uint32_t q = p; q < run_end; ++q)
      copy_hash[q].store(PageHash(q), std::memory_order_relaxed);
    hashed += run_end - p;
    ++pd.range_runs;
    // Not copied: neither valid nor pending, so the next request copies them.
    if (!copy(p << kPageShift, (run_end - p) << kPageShift)) return false;
    for (uint32_t q = p; q < run_end; ++q) armed_frame[q].store(frame, std::memory_order_relaxed);
    SetPageBits(pending.get(), p, run_end, true);
    QueueArm(p, run_end, trace_request);
    p = run_end;
  }
  if (hashed) {
    hash_pages.fetch_add(hashed, std::memory_order_relaxed);
    hash_copies.fetch_add(changed, std::memory_order_relaxed);
  }
  if (!copy_changed()) return false;
  if (coherency) CheckRequest(start, length);
  return true;
}

void DrawRenderer::Impl::QueueArm(uint32_t first, uint32_t end, uint32_t request) {
  bool wake;
  {
    std::lock_guard<std::mutex> lock(arm_mutex);
    arm_queue.push_back({first, end, request});
    wake = std::exchange(arm_idle, false);
  }
  if (wake) arm_cv.notify_one();
}

void DrawRenderer::Impl::WatchThread() {
  SetThreadDescription(GetCurrentThread(), L"PGR4 native watches");
  std::vector<ArmRun> runs;
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(arm_mutex);
      while (arm_queue.empty() && !arm_stopping) {
        arm_idle = true;
        arm_cv.wait(lock);
      }
      if (arm_queue.empty()) return;
      runs.swap(arm_queue);
    }
    const uint64_t t0 = NowNs();
    std::sort(runs.begin(), runs.end(),
              [](const ArmRun& a, const ArmRun& b) { return a.first < b.first; });
    uint64_t arms = 0, pages = 0, changed = 0;
    for (size_t i = 0; i < runs.size();) {
      const uint32_t first = runs[i].first;
      uint32_t end = runs[i].end;
      size_t j = i + 1;
      while (j < runs.size() && runs[j].first == end) end = runs[j++].end;
      // Valid before the arm: a write after it faults and clears the bit, which stays clear.
      SetPageBits(valid.get(), first, end, true);
      memory->EnablePhysicalMemoryAccessCallbacks(first << kPageShift, (end - first) << kPageShift,
                                                  true, false);
      for (uint32_t q = first; q < end; ++q) {
        if (PageHash(q) == copy_hash[q].load(std::memory_order_relaxed)) continue;
        // Written between its copy and the arm: the next request copies it again.
        SetPageBits(valid.get(), q, q + 1, false);
        ++changed;
      }
      SetPageBits(pending.get(), first, end, false);
      if (trace.file) trace.Add(WatchTrace::kArm, first, end - first, runs[i].request);
      ++arms;
      pages += end - first;
      i = j;
    }
    runs.clear();
    watch_arms.fetch_add(arms, std::memory_order_relaxed);
    watch_arm_pages.fetch_add(pages, std::memory_order_relaxed);
    watch_arm_changed.fetch_add(changed, std::memory_order_relaxed);
    watch_arm_ns.fetch_add(NowNs() - t0, std::memory_order_relaxed);
  }
}

void DrawRenderer::Impl::CheckRequest(uint32_t start, uint32_t length) {
  Coherency& c = *coherency;
  const uint32_t stamp = watch_frame.load(std::memory_order_relaxed) + 1;
  if (c.ranges.empty() || c.ranges.back().start != start || c.ranges.back().length != length)
    c.ranges.push_back({start, length, c.slot});
  const uint32_t end = start + length;
  for (uint32_t p = start >> kPageShift; p <= (end - 1) >> kPageShift; ++p) {
    if (c.checked[p] == stamp) continue;
    const uint32_t lo = std::max(start, p << kPageShift), hi = std::min(end, (p + 1) << kPageShift);
    if (!std::memcmp(membase + lo, c.shadow.get() + lo, hi - lo)) {
      if (hi - lo == 1u << kPageShift) c.checked[p] = stamp;
      continue;
    }
    const uint64_t bit = 1ull << (p & 63);
    const bool is_valid = valid[p >> 6].load(std::memory_order_relaxed) & bit;
    const bool hashed = (hot[p >> 6].load(std::memory_order_relaxed) |
                         pending[p >> 6].load(std::memory_order_relaxed)) &
                        bit;
    // A write after this request copied or trusted the page: it faulted (valid cleared) or, on a
    // hot or pending page, changed the hash since. Either way the next request copies it.
    const bool written_since =
        hashed ? PageHash(p) != copy_hash[p].load(std::memory_order_relaxed) : !is_valid;
    if (written_since) continue;
    ++c.stale;
    uint32_t differ = 0, first = UINT32_MAX;
    for (uint32_t i = lo; i < hi; ++i)
      if (membase[i] != c.shadow[i]) {
        ++differ;
        first = std::min(first, i);
      }
    PGR4_LOG_FIRST(200, WARN,
                   "PGR4 native coherency: frame {} stale page {:05X} (request {:08X}+{:X}, slot "
                   "{}): {} of {} bytes differ from {:08X}, {} armed at frame {}, last write fault "
                   "frame {}",
                   stamp - 1, p, start, length, c.slot, differ, hi - lo, first,
                   hashed ? "hashed" : "valid", armed_frame[p].load(std::memory_order_relaxed),
                   int64_t(write_frame[p].load(std::memory_order_relaxed)) - 1);
  }
}

void DrawRenderer::Impl::CheckSegmentCopy(uint32_t from, uint32_t size) {
  Coherency& c = *coherency;
  const uint32_t end = from + size;
  for (const auto& [b, e] : c.segment_reads) {
    if (!(b < end && from < e)) continue;
    if (segment_conflict) return;
    PGR4_LOG_FIRST(
        200, WARN,
        "PGR4 native coherency: frame {} copy {:08X}+{:X} overwrites {:08X}-{:08X}, read "
        "earlier in its copy segment, without a new segment",
        watch_frame.load(std::memory_order_relaxed), from, size, b, e);
    return;
  }
}

std::shared_ptr<CoherencyJob> DrawRenderer::Impl::MakeCoherencyJob() {
  Coherency& c = *coherency;
  if (c.frame_copies.empty()) return nullptr;
  auto job = std::make_shared<CoherencyJob>();
  job->frame = watch_frame.load(std::memory_order_relaxed);
  std::sort(c.frame_copies.begin(), c.frame_copies.end());
  uint32_t total = 0;
  for (const auto& [start, size] : c.frame_copies) {
    uint32_t end = start + size;
    if (!job->runs.empty()) {
      auto& last = job->runs.back();
      const uint32_t last_end = last.first + last.second;
      if (start <= last_end) {
        if (end > last_end) {
          const uint32_t grow = std::min(end - last_end, kCoherencyReadback - total);
          last.second += grow;
          total += grow;
        }
        continue;
      }
    }
    if (total >= kCoherencyReadback) break;
    const uint32_t take = std::min(size, kCoherencyReadback - total);
    job->runs.push_back({start, take});
    total += take;
  }
  c.frame_copies.clear();
  job->expected.resize(total);
  uint8_t* out = job->expected.data();
  for (const auto& [start, size] : job->runs) {
    std::memcpy(out, c.shadow.get() + start, size);
    out += size;
  }
  return job;
}

void DrawRenderer::Impl::CheckCoherencyJobs(uint64_t completed_fence) {
  while (!coherency_inflight.empty() && coherency_inflight.front()->fence <= completed_fence) {
    std::shared_ptr<CoherencyJob> job = std::move(coherency_inflight.front());
    coherency_inflight.pop_front();
    void* data = nullptr;
    const D3D12_RANGE range = {0, job->expected.size()};
    if (SUCCEEDED(job->readback->Map(0, &range, &data))) {
      const uint8_t* got = static_cast<const uint8_t*>(data);
      const uint8_t* want = job->expected.data();
      for (const auto& [start, size] : job->runs) {
        for (uint32_t off = 0; off < size;) {
          const uint32_t addr = start + off;
          const uint32_t n =
              std::min(size - off, ((addr >> kPageShift) + 1) * (1u << kPageShift) - addr);
          if (std::memcmp(got + off, want + off, n)) {
            ++stats.gpu_mismatch_pages;
            uint32_t differ = 0, first = UINT32_MAX;
            for (uint32_t i = 0; i < n; ++i)
              if (got[off + i] != want[off + i]) {
                ++differ;
                first = std::min(first, addr + i);
              }
            PGR4_LOG_FIRST(200, WARN,
                           "PGR4 native coherency: frame {} shared memory page {:05X} (copied run "
                           "{:08X}+{:X}) differs from its copies after the frame: {} of {} bytes "
                           "from {:08X}",
                           job->frame, addr >> kPageShift, start, size, differ, n, first);
          }
          off += n;
        }
        got += size;
        want += size;
      }
      const D3D12_RANGE none = {0, 0};
      job->readback->Unmap(0, &none);
    }
    coherency_readbacks.push_back(std::move(job->readback));
  }
}

void DrawRenderer::Impl::AuditShader(const ShaderRef& ref, const DxbcShader* shader,
                                     const char* type) {
  if (!shader) return;
  const uint32_t* ucode =
      ref.immediate ? ref.immediate : reinterpret_cast<const uint32_t*>(membase + ref.ucode);
  const uint64_t hash = XXH3_64bits(ucode, size_t(ref.dwords) * sizeof(uint32_t));
  if (hash == shader->ucode_data_hash()) return;
  ++audit_shaders;
  PGR4_LOG_FIRST(100, WARN,
                 "PGR4 native audit: frame {} {} shader {:08X} ({} dwords{}) hashes {:016X} in "
                 "guest memory, the shader memo gave {:016X}",
                 watch_frame.load(std::memory_order_relaxed), type, ref.ucode, ref.dwords,
                 ref.immediate ? ", immediate" : "", hash, shader->ucode_data_hash());
}

void DrawRenderer::Impl::AuditDraw(ID3D12GraphicsCommandList* list, const DrawPrep& prep) {
  if (!audit_job) {
    audit_job = std::make_unique<AuditJob>();
    audit_job->frame = r.frame_index();
    if (!audit_readbacks.empty()) {
      audit_job->readback = std::move(audit_readbacks.back());
      audit_readbacks.pop_back();
    } else {
      CreateBuffer(device, D3D12_HEAP_TYPE_READBACK, kAuditReadback, D3D12_RESOURCE_STATE_COPY_DEST,
                   "audit readback", &audit_job->readback);
    }
  }
  AuditJob& job = *audit_job;
  if (!job.readback) return;
  TransitionShared(list, D3D12_RESOURCE_STATE_COPY_SOURCE);
  for (uint32_t i = 0; i < prep.audit_count; ++i) {
    const AuditRange& a = prep.audit[i];
    if (job.used + a.size > kAuditReadback) {
      ++job.over;
      continue;
    }
    list->CopyBufferRegion(job.readback.Get(), job.used, shared.Get(), a.start, a.size);
    job.entries.push_back({a, job.used, frame_draws});
    job.used += a.size;
  }
}

void DrawRenderer::Impl::CheckAuditJobs(uint64_t completed_fence) {
  while (!audit_inflight.empty() && audit_inflight.front()->fence <= completed_fence) {
    std::unique_ptr<AuditJob> job = std::move(audit_inflight.front());
    audit_inflight.pop_front();
    void* data = nullptr;
    const D3D12_RANGE range = {0, job->used};
    if (!job->readback || FAILED(job->readback->Map(0, &range, &data))) {
      REXLOG_WARN("PGR4 native audit: frame {}: no read back", job->frame);
      continue;
    }
    const uint8_t* got = static_cast<const uint8_t*>(data);
    uint64_t differ = 0, newer = 0;
    for (const AuditJob::Entry& e : job->entries) {
      const uint8_t* bytes = got + e.offset;
      if (XXH3_64bits(bytes, e.range.size) == e.range.hash) continue;
      ++differ;
      // Shared memory may hold what the game wrote after the draw was queued (a copy recorded
      // too early) or something older (a write the page watch or the hashes missed).
      const bool now = !std::memcmp(bytes, membase + e.range.start, e.range.size);
      newer += now;
      uint32_t pages = 0, first = UINT32_MAX;
      for (uint32_t off = 0; off < e.range.size;) {
        const uint32_t addr = e.range.start + off;
        const uint32_t n =
            std::min(e.range.size - off, (((addr >> kPageShift) + 1) << kPageShift) - addr);
        if (std::memcmp(bytes + off, membase + addr, n)) {
          ++pages;
          first = std::min(first, addr >> kPageShift);
        }
        off += n;
      }
      const uint32_t p = first == UINT32_MAX ? e.range.start >> kPageShift : first;
      const uint64_t bit = 1ull << (p & 63);
      PGR4_LOG_FIRST(
          200, WARN,
          "PGR4 native audit: frame {} draw {}: shared memory {:08X}+{:X} differs from guest "
          "memory when the draw was queued; {}: {} pages differ from it, first {:05X} (valid {}, "
          "hot {}, pending {}, armed at frame {}, written at frame {})",
          job->frame, e.draw, e.range.start, e.range.size,
          now ? "it holds guest memory as it is now" : "and from guest memory now", pages, p,
          (valid[p >> 6].load(std::memory_order_relaxed) & bit) != 0,
          (hot[p >> 6].load(std::memory_order_relaxed) & bit) != 0,
          (pending[p >> 6].load(std::memory_order_relaxed) & bit) != 0,
          armed_frame[p].load(std::memory_order_relaxed),
          int64_t(write_frame[p].load(std::memory_order_relaxed)) - 1);
    }
    const D3D12_RANGE none = {0, 0};
    job->readback->Unmap(0, &none);
    REXLOG_INFO(
        "PGR4 native audit: frame {}: {} ranges ({:.1f} MB) read back{}, {} differ from guest "
        "memory when their draw was queued ({} hold it as it is now)",
        job->frame, job->entries.size(), double(job->used) / (1 << 20),
        job->over ? fmt::format(", {} not (read back full)", job->over) : std::string(), differ,
        newer);
    audit_readbacks.push_back(std::move(job->readback));
  }
}

void DrawRenderer::Impl::CheckLateWrites() {
  Coherency& c = *coherency;
  const uint32_t stamp = watch_frame.load(std::memory_order_relaxed) + 1;
  uint64_t late = 0, stale = 0;
  uint32_t late_ranges = 0, slot_pages[256] = {};
  const Coherency::Range* first_late = nullptr;
  for (const Coherency::Range& r : c.ranges) {
    const uint32_t end = r.start + r.length;
    bool any = false;
    for (uint32_t p = r.start >> kPageShift; p <= (end - 1) >> kPageShift; ++p) {
      if (c.swap_checked[p] == stamp) continue;
      c.swap_checked[p] = stamp;
      const uint32_t lo = std::max(r.start, p << kPageShift);
      const uint32_t hi = std::min(end, (p + 1) << kPageShift);
      if (!std::memcmp(membase + lo, c.shadow.get() + lo, hi - lo)) continue;
      const uint64_t bit = 1ull << (p & 63);
      if ((valid[p >> 6].load(std::memory_order_relaxed) & bit) &&
          !((hot[p >> 6].load(std::memory_order_relaxed) |
             pending[p >> 6].load(std::memory_order_relaxed)) &
            bit)) {
        // Valid and watched, yet guest memory differs: a write the watch missed.
        ++stale;
        PGR4_LOG_FIRST(200, WARN,
                       "PGR4 native coherency: frame {} stale page {:05X} at the Swap (range "
                       "{:08X}+{:X}, slot {}), armed at frame {}",
                       stamp - 1, p, r.start, r.length, r.slot,
                       armed_frame[p].load(std::memory_order_relaxed));
        continue;
      }
      ++late;
      ++slot_pages[r.slot];
      any = true;
    }
    if (any) {
      ++late_ranges;
      if (!first_late) first_late = &r;
    }
  }
  c.late += late;
  c.stale += stale;
  if (late) {
    auto slots = [&] {
      std::string list;
      for (uint32_t s = 0; s < 256; ++s)
        if (slot_pages[s]) list += fmt::format(" {}:{}", s == 0xFF ? -1 : int(s), slot_pages[s]);
      return list;
    };
    PGR4_LOG_FIRST(400, INFO,
                   "PGR4 native coherency: frame {} {} pages of {} ranges rewritten after a draw "
                   "read them (first {:08X}+{:X}); pages per slot{}",
                   stamp - 1, late, late_ranges, first_late->start, first_late->length, slots());
  }
  c.ranges.clear();
}

bool DrawRenderer::Impl::MapShared(uint32_t start, uint32_t length) {
  if (!shared_tiled) return true;
  const uint32_t first = start >> kSharedChunkShift;
  const uint32_t last = (start + length - 1) >> kSharedChunkShift;
  auto mapped = [&](uint32_t c) {
    return (shared_mapped[c >> 6].load(std::memory_order_acquire) >> (c & 63)) & 1;
  };
  bool all = true;
  for (uint32_t c = first; c <= last && all; ++c) all = mapped(c);
  if (all) return true;

  std::lock_guard<std::mutex> lock(shared_map_mutex);
  for (uint32_t c = first; c <= last; ++c) {
    if (mapped(c)) continue;
    uint32_t end = c + 1;
    while (end <= last && !mapped(end)) ++end;
    const uint32_t chunks = end - c;
    D3D12_HEAP_DESC hd = {};
    hd.SizeInBytes = uint64_t(chunks) << kSharedChunkShift;
    hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    ComPtr<ID3D12Heap> heap;
    if (Failed(device->CreateHeap(&hd, IID_PPV_ARGS(&heap)), "shared memory tile heap"))
      return false;
    D3D12_TILED_RESOURCE_COORDINATE coord = {};
    coord.X = UINT(uint64_t(c) << kSharedChunkShift) / D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
    D3D12_TILE_REGION_SIZE size = {};
    size.NumTiles = UINT(hd.SizeInBytes / D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES);
    const D3D12_TILE_RANGE_FLAGS flags = D3D12_TILE_RANGE_FLAG_NONE;
    const UINT heap_offset = 0;
    r.command_queue()->UpdateTileMappings(shared.Get(), 1, &coord, &size, heap.Get(), 1, &flags,
                                          &heap_offset, &size.NumTiles,
                                          D3D12_TILE_MAPPING_FLAG_NONE);
    shared_heaps.push_back(std::move(heap));
    for (uint32_t m = c; m < end; ++m)
      shared_mapped[m >> 6].fetch_or(1ull << (m & 63), std::memory_order_release);
    shared_mapped_chunks += chunks;
    // At most 128 lines (4 MB chunks of 512 MB); the frame places a mapping next to a glitch.
    REXLOG_INFO("PGR4 native draw: shared memory {:08X}+{} MB mapped at frame {}, {} MB in all",
                c << kSharedChunkShift, chunks << (kSharedChunkShift - 20),
                watch_frame.load(std::memory_order_relaxed),
                shared_mapped_chunks << (kSharedChunkShift - 20));
    c = end - 1;
  }
  return true;
}

void DrawRenderer::Impl::FlushCopies(ID3D12GraphicsCommandList* list) {
  if (copies.empty()) {
    TransitionShared(list, kSharedReadState);
    return;
  }
  ++diag.flushes;
  ++stats.copy_batches;
  TransitionShared(list, D3D12_RESOURCE_STATE_COPY_DEST);
  for (const VertexCopy& c : copies)
    list->CopyBufferRegion(shared.Get(), c.dst, c.src, c.src_offset, c.size);
  copies.clear();
  TransitionShared(list, kSharedReadState);
}

bool DrawRenderer::Impl::CommitCbv(CachedCbv& cb, uint32_t root_index,
                                   ID3D12GraphicsCommandList* list) {
  if (!cb.gpu) {
    // Whole float4s (a CBV is read in them; `last` has room). Streaming stores: each upload
    // takes fresh lines that only the GPU reads, so they bypass the caches instead of being
    // read from memory for ownership and then pushing the game's code and data out.
    // D3D12Device::CloseAndExecute fences them before the GPU can read them.
    const uint32_t vectors = std::max<uint32_t>((cb.size + 15) / 16, 1);
    UploadPool::Alloc a;
    if (!constant_upload.Allocate(vectors * 16, 256, r.frame_fence(), &a)) return false;
    auto* dst = reinterpret_cast<simde__m128i*>(a.cpu);
    const auto* src = reinterpret_cast<const simde__m128i*>(cb.last);
    for (uint32_t i = 0; i < vectors; ++i)
      simde_mm_stream_si128(dst + i, simde_mm_load_si128(src + i));
    cb.gpu = a.gpu;
    ++diag.cbv_uploads[root_index & 15];
    diag.cbv_bytes[root_index & 15] += cb.size;
  }
  if (cb.bound != cb.gpu) {
    list->SetGraphicsRootConstantBufferView(root_index, cb.gpu);
    cb.bound = cb.gpu;
  }
  return true;
}

void DrawRenderer::Impl::BindPreviousConstants(const DrawState& s, const DxbcShader& vs_generic,
                                               const DxbcShader* vs,
                                               ID3D12GraphicsCommandList* list) {
  const uint64_t identity = vs_generic.ucode_data_hash() ^
                            (uint64_t(s.index_address) << 32 | s.count) ^
                            (uint64_t(s.primitive) << 58);
  const uint64_t key = motion_history.Key(identity);
  // Local change (pgr4 project): matched first by the draw's placement too (c0-c3, the object's
  // transform in PGR4's scene shaders). The game culls its instanced props (signs, barriers) by
  // group, so as the camera turns a group's order among draws of the same mesh shifts and the
  // order alone gave it another group's previous transform: wrong motion, jumps in DLSS. A
  // static group keeps its placement, so it finds its own; a moving object (cars) falls back
  // to the order.
  const uint64_t placed_key = motion_history.Key(
      identity ^ 0x9E3779B97F4A7C15ull * (1 | XXH3_64bits(s.regs + XE_GPU_REG_SHADER_CONSTANT_000_X,
                                                          16 * sizeof(uint32_t))));
  const MotionHistory::Constants now = {vs, cb_float_vs.gpu, cb_fetch.gpu};
  const MotionHistory::Constants* previous = motion_history.Previous(placed_key);
  const bool by_place = previous && previous->vs == vs;
  if (!by_place) previous = motion_history.Previous(key);
  const bool by_order = !by_place && previous && previous->vs == vs;
  if (!by_place && !by_order) previous = &now;
  // Development (capture dir set): how many draws found their previous constants by their placement, by their
  // order, or none (no motion of their own then, not even the camera's: ghosts in generated frames if it lasts),
  // every 300 frames (14/11/2026).
  if (!REXCVAR_GET(pgr4_native_capture_dir).empty()) {
    static uint64_t place = 0, order = 0, none = 0, first_frame = 0;
    (by_place ? place : by_order ? order : none)++;
    const uint64_t frame = r.frame_index();
    if (!first_frame) first_frame = frame;
    if (frame - first_frame >= 300) {
      const double total = double(std::max<uint64_t>(place + order + none, 1));
      REXLOG_INFO("PGR4 motion: {:.0f} draws a frame with motion; previous found by placement {:.1f} %, by order "
                  "{:.1f} %, none {:.1f} % ({:.0f} a frame)",
                  total / double(frame - first_frame), 100.0 * place / total, 100.0 * order / total,
                  100.0 * none / total, double(none) / double(frame - first_frame));
      place = order = none = 0;
      first_frame = frame;
    }
  }
  if (bound_previous_float != previous->float_constants) {
    list->SetGraphicsRootConstantBufferView(kBindlessRoot_PreviousFloatConstantsVertex,
                                            previous->float_constants);
    bound_previous_float = previous->float_constants;
  }
  // Local change (pgr4 project): with --pgr4_native_motion_current_fetch the previous position
  // reads this frame's vertex streams. PGR4 rewrites instanced props' transforms (lamp posts,
  // signs, barriers) into per-frame streams, so last frame's fetch addresses can hold another
  // instance's by now: their motion pointed at other props (ghosts in generated frames). Static
  // props keep their transforms, so this frame's give them the camera's exact motion.
  const D3D12_GPU_VIRTUAL_ADDRESS previous_fetch =
      REXCVAR_GET(pgr4_native_motion_current_fetch) ? now.fetch_constants
                                                    : previous->fetch_constants;
  if (bound_previous_fetch != previous_fetch) {
    list->SetGraphicsRootConstantBufferView(kBindlessRoot_PreviousFetchConstantsVertex,
                                            previous_fetch);
    bound_previous_fetch = previous_fetch;
  }
  motion_history.Record(key, now);
  motion_history.Record(placed_key, now);
}

uint32_t DrawRenderer::Impl::TextureDescriptor(const TextureBinding& b,
                                               const DxbcShader::TextureBinding& tb) {
  ++stats.srv_lookups;
  D3D12_SHADER_RESOURCE_VIEW_DESC desc = b.srv;
  // The translator's space for the binding: 2D array (1D/2D), cube, or 3D-or-stacked (either).
  const D3D12_SRV_DIMENSION dim = desc.ViewDimension;
  bool fits;
  switch (tb.dimension) {
    case xenos::FetchOpDimension::kCube:
      fits = dim == D3D12_SRV_DIMENSION_TEXTURECUBE;
      break;
    case xenos::FetchOpDimension::k3DOrStacked:
      fits = dim == D3D12_SRV_DIMENSION_TEXTURE3D || dim == D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
      break;
    default:
      fits = dim == D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
      break;
  }
  if (!fits) return NullView(tb.dimension);
  if (tb.is_signed) {
    if (b.srv_signed_format == DXGI_FORMAT_UNKNOWN) return NullView(dim);
    desc.Format = b.srv_signed_format;
  }
  const uint64_t key = XXH3_64bits(&desc, sizeof(desc));
  auto views = srv_views.find(b.resource);
  if (views != srv_views.end())
    for (const auto& [view_key, index] : views->second)
      if (view_key == key) return index;
  uint32_t heap_index;
  if (!free_views.empty()) {
    heap_index = free_views.back();
    free_views.pop_back();
  } else if (views_allocated < ViewLimit()) {
    heap_index = views_allocated++;
  } else {
    PGR4_WARN_ONCE("PGR4 native draw: view heap full ({} descriptors)", ViewLimit());
    view_heap_overflow = true;
    return NullView(dim);
  }
  D3D12_CPU_DESCRIPTOR_HANDLE h = view_cpu;
  h.ptr += size_t(heap_index) * view_stride;
  device->CreateShaderResourceView(b.resource, &desc, h);
  const uint32_t index = heap_index - kViewTableStart;
  if (views == srv_views.end()) views = srv_views.try_emplace(b.resource).first;
  views->second.emplace_back(key, index);
  ++stats.srv_created;
  return index;
}

void DrawRenderer::Impl::ReleaseViews(const std::vector<ID3D12Resource*>& resources) {
  for (ID3D12Resource* resource : resources) {
    auto it = srv_views.find(resource);
    if (it == srv_views.end()) continue;
    for (const auto& view : it->second) free_views.push_back(view.second + kViewTableStart);
    srv_views.erase(it);
  }
  std::vector<ID3D12Resource*> sorted(resources);
  std::sort(sorted.begin(), sorted.end());
  for (size_t i = 0; i < (size_t(1) << kSrvMemoBits); ++i) {
    SrvMemo& m = srv_memo[i];
    if (m.resource && std::binary_search(sorted.begin(), sorted.end(), m.resource)) m = SrvMemo{};
  }
}

uint32_t DrawRenderer::Impl::SamplerDescriptor(const SamplerKey& k) {
  const uint32_t key = k.value();
  auto it = sampler_indices.find(key);
  if (it != sampler_indices.end()) return it->second;
  if (samplers_allocated >= kSamplerHeapSize) {
    PGR4_WARN_ONCE("PGR4 native draw: sampler heap full");
    return 0;
  }
  const uint32_t index = samplers_allocated++;
  D3D12_CPU_DESCRIPTOR_HANDLE h = sampler_cpu;
  h.ptr += size_t(index) * sampler_stride;
  D3D12_SAMPLER_DESC d = SamplerDesc(k);
  device->CreateSampler(&d, h);
  sampler_indices.emplace(key, index);
  ++stats.samplers_created;
  return index;
}

DrawRenderer::Impl::Pipeline* DrawRenderer::Impl::GetPipeline(uint64_t hash, const PipelineKey& key,
                                                              const DxbcShader::Translation& vs,
                                                              const DxbcShader::Translation* ps,
                                                              uint32_t variant, bool background) {
  auto it = pipelines.find(hash);
  Pipeline* p;
  if (it != pipelines.end()) {
    p = it->second.get();
    if (p->store_entry != PipelineStore::kNoEntry) {
      store.TouchEntry(p->store_entry);
      p->store_entry = PipelineStore::kNoEntry;
    }
  } else {
    if (new_pipelines_logged < kNewPipelineLog) {
      ++new_pipelines_logged;
      const auto st = pipeline_states.find(StateDigest(key));
      const auto pr = pipeline_pairs.find(PairDigest(key));
      REXLOG_INFO(
          "PGR4 native pipelines: new at frame {}: vs {:016X}{} ps {:016X}{}{}, state {} "
          "({} pipelines), shader pair {} ({} pipelines)",
          r.frame_index(), vs.shader().ucode_data_hash(), variant & 1 ? " (bools)" : "",
          ps ? ps->shader().ucode_data_hash() : 0, variant & 2 ? " (bools)" : "",
          variant & 4 ? " (signs)" : "", st == pipeline_states.end() ? "new" : "known",
          st == pipeline_states.end() ? 0 : st->second,
          pr == pipeline_pairs.end() ? "new" : "known",
          pr == pipeline_pairs.end() ? 0 : pr->second);
    }
    p = CreatePipeline(hash, key, vs, ps, background);
    ++stats.pso_created;
    if (store.is_open()) {
      PipelineKey stored = key;
      stored.vs = stored.ps = nullptr;
      const Shader& vss = vs.shader();
      store.RecordShader(uint32_t(vss.type()), vss.ucode_data_hash(), vss.ucode_dwords(),
                         vss.ucode_dword_count());
      if (ps) {
        const Shader& pss = ps->shader();
        store.RecordShader(uint32_t(pss.type()), pss.ucode_data_hash(), pss.ucode_dwords(),
                           pss.ucode_dword_count());
      }
      store.RecordPipeline(vss.ucode_data_hash(), vs.modification(),
                           ps ? ps->shader().ucode_data_hash() : 0, ps ? ps->modification() : 0,
                           &stored);
      // One of the store's older records: mark it used so it stays (fresh records carry this
      // session already).
      store.TouchRecord(vss.ucode_data_hash(), vs.modification(),
                        ps ? ps->shader().ucode_data_hash() : 0, ps ? ps->modification() : 0,
                        &stored);
    }
  }
  return p;
}

// Local change (pgr4 project): a pipeline waiting for its replacement's HLSL (CreatePipeline) made
// once the code is compiled (or failed: then with the game's shader, as before).
void DrawRenderer::Impl::ResumeAwaiting(Pipeline* p) {
  const std::vector<uint8_t>* code = replacer.Get(*p->await_ps);
  if (!code && replacer.Pending(*p->await_ps)) return;
  if (!code) code = &p->await_ps->translated_binary();
  p->await_desc.PS.pShaderBytecode = code->data();
  p->await_desc.PS.BytecodeLength = code->size();
  p->awaiting_hlsl = false;
  if (!pso_threads.empty()) {
    {
      std::lock_guard<std::mutex> lock(pso_mutex);
      pso_jobs.push_back({p, p->await_desc, true});
    }
    pso_cv.notify_one();
  } else if (SUCCEEDED(device->CreateGraphicsPipelineState(&p->await_desc, IID_PPV_ARGS(&p->pso)))) {
    p->state.store(kReady, std::memory_order_relaxed);
  } else {
    p->state.store(kFailed, std::memory_order_relaxed);
  }
}

void DrawRenderer::Impl::PromotePipeline(Pipeline* p) {
  p->prewarm = false;
  std::lock_guard<std::mutex> lock(pso_mutex);
  auto it = std::find_if(prewarm_jobs.begin(), prewarm_jobs.end(),
                         [p](const PipelineJob& j) { return j.target == p; });
  if (it == prewarm_jobs.end()) return;  // being compiled
  pso_jobs.push_back(*it);
  prewarm_jobs.erase(it);
  pso_cv.notify_one();  // local change: the other workers do not watch prewarm_jobs
}

void DrawRenderer::Impl::WaitForPipelines() {
  // Development: what a long frame was made of (freezes at a change of view, 07/11/2026).
  const auto frame_end = std::chrono::steady_clock::now();
  const double frame_ms = std::chrono::duration<double, std::milli>(frame_end - last_frame_end).count();
  const size_t pending_now = frame_pending.size();
  std::string pending_list;
  if (!REXCVAR_GET(pgr4_native_capture_dir).empty()) {
    for (size_t k = 0; k < frame_pending.size() && k < 8; ++k) {
      const Pipeline* q = frame_pending[k];
      pending_list += fmt::format(" {:016X}{}{}", q->ps_ucode_hash, q->awaiting_hlsl ? ":hlsl" : ":pso", q->surface ? ":surface" : "");
    }
  }
  if (HoldsFrame()) {
    const auto t0 = std::chrono::steady_clock::now();
    {
      std::unique_lock<std::mutex> lock(pso_mutex);
      pso_waiting = true;
      pso_wait_cv.notify_all();
      pso_ready_cv.wait_for(lock, kPipelineWaitBudget, [this] {
        return std::none_of(frame_pending.begin(), frame_pending.end(), [](const Pipeline* p) {
          return !p->awaiting_hlsl && p->state.load(std::memory_order_acquire) == kCompiling;
        });
      });
      pso_waiting = false;
    }
    // Local change (pgr4 project): replaced pipelines waiting for their HLSL (no condition
    // variable tells its compile's end): made as soon as it is there, within the same budget.
    while (std::any_of(frame_pending.begin(), frame_pending.end(),
                       [](const Pipeline* p) { return p->awaiting_hlsl; }) &&
           std::chrono::steady_clock::now() - t0 < kPipelineWaitBudget) {
      for (Pipeline* p : frame_pending)
        if (p->awaiting_hlsl) ResumeAwaiting(p);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    {
      std::unique_lock<std::mutex> lock(pso_mutex);
      const auto left = kPipelineWaitBudget - (std::chrono::steady_clock::now() - t0);
      if (left > std::chrono::steady_clock::duration::zero())
        pso_ready_cv.wait_for(lock, left, [this] {
          return std::none_of(frame_pending.begin(), frame_pending.end(), [](const Pipeline* p) {
            return p->state.load(std::memory_order_acquire) == kCompiling;
          });
        });
    }
    stats.pso_wait_us += uint64_t(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0)
            .count());
  }
  if (!REXCVAR_GET(pgr4_native_capture_dir).empty()) {
    const auto done = std::chrono::steady_clock::now();
    const double wait_ms = std::chrono::duration<double, std::milli>(done - frame_end).count();
    if (frame_ms + wait_ms > 60.0)
      REXLOG_INFO("PGR4 long frame: {:.0f} ms before, {:.0f} ms held, {} pipelines pending{}", frame_ms, wait_ms, pending_now, pending_list);
    last_frame_end = done;
  }
  frame_pending.clear();
}

DrawRenderer::Impl::Pipeline* DrawRenderer::Impl::CreatePipeline(
    uint64_t hash, const PipelineKey& key, const DxbcShader::Translation& vs,
    const DxbcShader::Translation* ps, bool prewarm, uint32_t store_entry) {
  D3D12_GRAPHICS_PIPELINE_STATE_DESC d;
  std::memset(&d, 0, sizeof(d));  // padding too: PipelineLibrary hashes the bytes
  bool awaiting = false;  // local change: our HLSL compiling (Pipeline::awaiting_hlsl)
  d.pRootSignature = root_signature.Get();
  if (dynamic_depth_bias) d.Flags = D3D12_PIPELINE_STATE_FLAG_DYNAMIC_DEPTH_BIAS;
  d.IBStripCutValue = key.strip_cut == 1   ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF
                      : key.strip_cut == 2 ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF
                                           : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
  d.VS.pShaderBytecode = vs.translated_binary().data();
  d.VS.BytecodeLength = vs.translated_binary().size();
  if (ps && !key.disable_raster) {
    // Local change (pgr4 project): an HLSL replacement of ours when there is one, only for a
    // pipeline keyed as replaced (SelectPipeline's hash: the key's, tagged with the replacer's
    // generation). Under the key's own hash, the game's shader: a prewarmed pipeline made with
    // ours under it was found by "the game without our changes", which kept our car paint and
    // water.
    const bool replaced = hash != XXH3_64bits(&key, sizeof(key));
    const std::vector<uint8_t>* code = replaced ? replacer.Get(*ps) : nullptr;
    awaiting = replaced && !code && replacer.Pending(*ps);
    if (!code) code = &ps->translated_binary();
    d.PS.pShaderBytecode = code->data();
    d.PS.BytecodeLength = code->size();
  }
  if (key.gs_key) {
    auto g = geometry_shaders.find(key.gs_key);
    if (g == geometry_shaders.end()) {
      GeometryShaderKey gk;
      gk.key = key.gs_key;
      g = geometry_shaders.emplace(key.gs_key, CreateGeometryShader(gk)).first;
    }
    d.GS.pShaderBytecode = g->second.data();
    d.GS.BytecodeLength = g->second.size() * sizeof(uint32_t);
  }
  d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE(key.topology_type);
  d.RasterizerState.FillMode = key.wireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
  d.RasterizerState.CullMode = D3D12_CULL_MODE(key.cull);
  d.RasterizerState.FrontCounterClockwise = key.front_ccw;
  d.RasterizerState.DepthBias = key.depth_bias;
  d.RasterizerState.SlopeScaledDepthBias = key.slope_scaled_depth_bias;
  d.RasterizerState.DepthClipEnable = key.depth_clip;
  d.SampleMask = UINT_MAX;
  d.SampleDesc.Count = key.samples;
  if (!key.disable_raster) {
    d.DepthStencilState.DepthEnable = key.depth_enable;
    d.DepthStencilState.DepthWriteMask =
        key.depth_write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC(key.depth_func);
    if (!key.depth_enable) d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    d.DepthStencilState.StencilEnable = key.stencil_enable;
    d.DepthStencilState.StencilReadMask = key.stencil_read_mask;
    d.DepthStencilState.StencilWriteMask = key.stencil_write_mask;
    auto face = [](const uint8_t f[4]) {
      D3D12_DEPTH_STENCILOP_DESC o;
      o.StencilFailOp = f[0] ? D3D12_STENCIL_OP(f[0]) : D3D12_STENCIL_OP_KEEP;
      o.StencilDepthFailOp = f[1] ? D3D12_STENCIL_OP(f[1]) : D3D12_STENCIL_OP_KEEP;
      o.StencilPassOp = f[2] ? D3D12_STENCIL_OP(f[2]) : D3D12_STENCIL_OP_KEEP;
      o.StencilFunc = f[3] ? D3D12_COMPARISON_FUNC(f[3]) : D3D12_COMPARISON_FUNC_ALWAYS;
      return o;
    };
    d.DepthStencilState.FrontFace = face(key.front);
    d.DepthStencilState.BackFace = face(key.back);
    d.DSVFormat = key.dsv;
  }
  d.BlendState.IndependentBlendEnable = TRUE;
  d.NumRenderTargets = key.num_rts;
  for (uint32_t i = 0; i < 4; ++i) {
    d.RTVFormats[i] = key.rtv[i];
    const PipelineKey::Rt& rt = key.rt[i];
    D3D12_RENDER_TARGET_BLEND_DESC& b = d.BlendState.RenderTarget[i];
    b.BlendEnable = rt.blend;
    b.SrcBlend = kD3DBlend[rt.src];
    b.DestBlend = kD3DBlend[rt.dst];
    b.BlendOp = kD3DBlendOp[rt.op & 7];
    b.SrcBlendAlpha = kD3DBlend[rt.src_a];
    b.DestBlendAlpha = kD3DBlend[rt.dst_a];
    b.BlendOpAlpha = kD3DBlendOp[rt.op_a & 7];
    b.LogicOp = D3D12_LOGIC_OP_NOOP;
    b.RenderTargetWriteMask = rt.mask;
  }
  if (ps && DxbcShaderTranslator::Modification(ps->modification()).pixel.motion_vectors) {
    constexpr uint32_t kMotion = DxbcShaderTranslator::kMotionVectorRenderTarget;
    d.NumRenderTargets = kMotion + 1;
    d.RTVFormats[kMotion] = Upscaler::kMotionFormat;
    D3D12_RENDER_TARGET_BLEND_DESC& b = d.BlendState.RenderTarget[kMotion];
    b.SrcBlend = b.SrcBlendAlpha = D3D12_BLEND_ONE;
    b.DestBlend = b.DestBlendAlpha = D3D12_BLEND_ZERO;
    b.BlendOp = b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    b.LogicOp = D3D12_LOGIC_OP_NOOP;
    b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN;
  }
  Pipeline* p = pipelines.emplace(hash, std::make_unique<Pipeline>()).first->second.get();
  p->ps_ucode_hash = ps ? ps->shader().ucode_data_hash() : 0;
  p->store_entry = store_entry;
  if (awaiting) {
    p->awaiting_hlsl = true;
    p->await_ps = ps;
    p->await_desc = d;
    p->prewarm = prewarm;
    return p;
  }
  if (new_pipelines_logged < kNewPipelineLog) {
    ++pipeline_states[StateDigest(key)];
    ++pipeline_pairs[PairDigest(key)];
  }
  if (!pso_threads.empty()) {
    p->prewarm = prewarm;
    {
      std::lock_guard<std::mutex> lock(pso_mutex);
      (prewarm ? prewarm_jobs : pso_jobs).push_back({p, d, !prewarm});
    }
    // Local change: a prewarm job is the saving worker's alone (PipelineWorker).
    if (prewarm)
      pso_cv.notify_all();
    else
      pso_cv.notify_one();
    return p;
  }
  HRESULT hr = device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&p->pso));
  if (FAILED(hr)) {
    PGR4_WARN_ONCE("PGR4 native draw: CreateGraphicsPipelineState failed {:08X}", uint32_t(hr));
    p->state.store(kFailed, std::memory_order_relaxed);
    return p;
  }
  p->state.store(kReady, std::memory_order_relaxed);
  return p;
}

// The GPU and its driver's version, as PGR4Launcher writes them (FindGpu).
std::string DriverName(ID3D12Device* device) {
  Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
      FAILED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))))
    return {};
  DXGI_ADAPTER_DESC1 desc;
  adapter->GetDesc1(&desc);
  LARGE_INTEGER umd = {};
  if (FAILED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) return {};
  const std::wstring name = std::wstring(desc.Description) + L" " +
                            std::to_wstring(HIWORD(umd.HighPart)) + L"." +
                            std::to_wstring(LOWORD(umd.HighPart)) + L"." +
                            std::to_wstring(HIWORD(umd.LowPart)) + L"." +
                            std::to_wstring(LOWORD(umd.LowPart));
  const int n = WideCharToMultiByte(CP_UTF8, 0, name.data(), int(name.size()), nullptr, 0,
                                    nullptr, nullptr);
  std::string out(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, name.data(), int(name.size()), out.data(), n, nullptr, nullptr);
  return out;
}

void DrawRenderer::Impl::PrecompileAndExit() {
  const std::vector<PipelineStore::Pipeline>& list = store.loaded_pipelines();
  auto load = [&](uint64_t hash, xenos::ShaderType type) -> DxbcShader* {
    const PipelineStore::Shader* s = store.loaded_shader(hash);
    if (!s || s->type != uint32_t(type) || s->ucode.empty()) return nullptr;
    return shaders.Load(type, s->ucode.data(), uint32_t(s->ucode.size()));
  };
  const std::filesystem::path progress = library_path.parent_path() / "precompile_progress.txt";
  auto report = [&](size_t done, size_t total) {
    if (std::FILE* f = _wfopen(progress.c_str(), L"w")) {
      std::fprintf(f, "%zu %zu\n", done, total);
      std::fclose(f);
    }
  };
  // Local change (pgr4 project): ENHANCED REFLECTIONS changes a pipeline only when its pixel shader is
  // replaced in one state and not in the other: those made twice (below), and counted so.
  std::vector<bool> twice(list.size());
  size_t total = list.size();
  replacer.ForceReflections(false);
  for (size_t index = 0; index < list.size(); ++index)
    twice[index] = list[index].ps_hash && replacer.Has(list[index].ps_hash);
  replacer.ForceReflections(true);
  for (size_t index = 0; index < list.size(); ++index) {
    twice[index] = list[index].ps_hash && twice[index] != replacer.Has(list[index].ps_hash);
    if (twice[index]) ++total;
  }
  report(0, total);
  // Every record queued as a draw's own pipeline (all the workers, normal priority), the motion
  // vector ones too (the upscaler may be turned on in the game). made: (record, its pipeline).
  std::vector<std::pair<size_t, Pipeline*>> made;
  size_t failed = 0;
  // Local change (pgr4 project): with ENHANCED REFLECTIONS off, then on: else the other state's
  // shaders compile in the game, which holds its frames for them (a freeze at each new screen).
  for (int reflections = 0; reflections < 2; ++reflections) {
  replacer.ForceReflections(reflections != 0);
  for (size_t index = 0; index < list.size(); ++index) {
    if (reflections && !twice[index]) continue;
    // While they are queued, they compile too: the bar moves from the start.
    if (index % 256 == 0) {
      size_t ready = failed;
      for (const auto& [i, p] : made)
        if (p->state.load(std::memory_order_acquire) != kCompiling) ++ready;
      report(ready, total);
    }
    const PipelineStore::Pipeline& sp = list[index];
    DxbcShader* vs = load(sp.vs_hash, xenos::ShaderType::kVertex);
    DxbcShader* ps = sp.ps_hash ? load(sp.ps_hash, xenos::ShaderType::kPixel) : nullptr;
    DxbcShader::Translation* vst = vs ? shaders.Translate(*vs, sp.vs_mod) : nullptr;
    DxbcShader::Translation* pst = ps ? shaders.Translate(*ps, sp.ps_mod) : nullptr;
    if (!vst || (sp.ps_hash && !pst) || sp.key.size() != sizeof(PipelineKey)) {
      ++failed;
      continue;
    }
    PipelineKey key;
    std::memcpy(&key, sp.key.data(), sizeof(key));
    key.vs = vst;
    key.ps = pst;
    // Tagged as SelectPipeline and Prewarm tag it: ours when the pixel shader is replaced.
    const uint64_t hash = XXH3_64bits(&key, sizeof(key)) ^ ReplacedTag(ps);
    auto it = pipelines.find(hash);
    made.emplace_back(index, it != pipelines.end()
                                 ? it->second.get()
                                 : CreatePipeline(hash, key, *vst, pst, false, uint32_t(index)));
  }
  }
  // Local change (pgr4 project): a car's paint in every variant the paints were seen in (signs, motion
  // vectors): only those of the cars already played were listed, and the others' HLSL, compiled in the
  // game, held its frames for 3 s (06/11/2026). Their HLSL only: its pipeline then takes little.
  std::vector<const DxbcShader::Translation*> paint_extra;
  {
    std::unordered_set<uint64_t> paints, mods;
    for (const PipelineStore::Pipeline& sp : list)
      if (sp.ps_hash && replacer.Has(sp.ps_hash) && replacer.IsCarPaint(sp.ps_hash)) {
        paints.insert(sp.ps_hash);
        mods.insert(sp.ps_mod);
      }
    for (const uint64_t hash : paints) {
      DxbcShader* ps = load(hash, xenos::ShaderType::kPixel);
      if (!ps) continue;
      for (const uint64_t mod : mods) {
        DxbcShader::Translation* pst = shaders.Translate(*ps, mod);
        if (!pst) continue;
        replacer.Get(*pst);
        if (replacer.Pending(*pst)) paint_extra.push_back(pst);
      }
    }
    total += paint_extra.size();
    REXLOG_INFO("PGR4 native pipelines: {} paints in {} variants, {} to compile", paints.size(), mods.size(),
                paint_extra.size());
  }
  const auto t0 = std::chrono::steady_clock::now();
  for (;;) {
    size_t done = failed;
    for (const DxbcShader::Translation* extra : paint_extra)
      if (!replacer.Pending(*extra)) ++done;
    for (const auto& [index, p] : made) {
      // Local change: those waiting for our HLSL made once it is compiled.
      if (p->awaiting_hlsl) ResumeAwaiting(p);
      if (p->state.load(std::memory_order_acquire) != kCompiling) ++done;
    }
    report(done, total);
    if (done == total) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  library.Save();
  // The compiled records for the launcher's check at each start (PGR4Launcher NeedsPrecompile):
  // the GPU and its driver's version (a new driver drops the library), then a record's FNV-1a of
  // its hashes, modifications and key per line, as in pipelines.bin.
  if (std::FILE* f = _wfopen((library_path.parent_path() / "compiled.txt").c_str(), L"w")) {
    std::fprintf(f, "%s\n", DriverName(device).c_str());
    for (const auto& [index, p] : made) {
      if (p->state.load(std::memory_order_acquire) != kReady) continue;
      const PipelineStore::Pipeline& sp = list[index];
      uint64_t h = 0xCBF29CE484222325ull;
      auto mix = [&h](const void* data, size_t size) {
        for (size_t i = 0; i < size; ++i) {
          h ^= static_cast<const uint8_t*>(data)[i];
          h *= 0x100000001B3ull;
        }
      };
      mix(&sp.vs_hash, 8);
      mix(&sp.vs_mod, 8);
      mix(&sp.ps_hash, 8);
      mix(&sp.ps_mod, 8);
      mix(sp.key.data(), sp.key.size());
      std::fprintf(f, "%016llX\n", static_cast<unsigned long long>(h));
    }
    std::fclose(f);
  }
  // The base list shipped beside the executable grows with what was played (the store holds the
  // base's records, the other scales' and this one's); from the build folder, the project's too,
  // which the next build and export carry.
  {
    std::error_code ec;
    const std::filesystem::path records = library_path.parent_path() / "pipelines.bin";
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const std::filesystem::path exe_dir = std::filesystem::path(exe).parent_path();
    std::filesystem::copy_file(records, exe_dir / "pipelines_base.bin",
                               std::filesystem::copy_options::overwrite_existing, ec);
#if defined(PGR4_SOURCE_TOML) && defined(PGR4_BUILD_DIR)
    if (std::filesystem::equivalent(exe_dir, PGR4_BUILD_DIR, ec))
      std::filesystem::copy_file(records,
                                 std::filesystem::path(PGR4_SOURCE_TOML).parent_path() /
                                     "pipelines_base.bin",
                                 std::filesystem::copy_options::overwrite_existing, ec);
#endif
  }
  report(total, total);
  REXLOG_INFO("PGR4 native pipelines: precompiled {} pipelines ({} failed) in {:.1f} s", made.size(),
              failed,
              std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
  std::_Exit(0);
}

bool DrawRenderer::Impl::IsCarShader(uint64_t hash) {
  const auto it = car_shaders.find(hash);
  if (it != car_shaders.end()) return it->second;
  const bool car = replacer.IsCarShader(hash);
  car_shaders.emplace(hash, car);
  return car;
}

// A paint first seen: the states and variants the other paints were recorded in, quietly (at the prewarm's
// priority): most of what a change of view would have asked for at once. Once a pipeline is in the library, a
// later run gets it at once.
void DrawRenderer::Impl::PreparePaints() {
  for (size_t k = 0; k < prepared_awaiting.size();) {
    Pipeline* const a = prepared_awaiting[k];
    if (a->awaiting_hlsl) ResumeAwaiting(a);
    if (a->awaiting_hlsl) {
      ++k;
    } else {
      prepared_awaiting[k] = prepared_awaiting.back();
      prepared_awaiting.pop_back();
    }
  }
  if (paints_to_prepare.empty() || !REXCVAR_GET(pgr4_native_paint_prepare)) return;
  const std::vector<PipelineStore::Pipeline>& list = store.loaded_pipelines();
  if (!paint_records_known) {
    paint_records_known = true;
    for (size_t i = 0; i < list.size(); ++i)
      if (list[i].ps_hash && replacer.Has(list[i].ps_hash) && replacer.IsCarPaint(list[i].ps_hash))
        paint_records.push_back(uint32_t(i));
    REXLOG_INFO("PGR4 native pipelines: {} paint records to prepare the paints from", paint_records.size());
  }
  auto load = [&](uint64_t hash) -> DxbcShader* {
    const PipelineStore::Shader* s = store.loaded_shader(hash);
    if (!s || s->type != uint32_t(xenos::ShaderType::kVertex) || s->ucode.empty()) return nullptr;
    return shaders.Load(xenos::ShaderType::kVertex, s->ucode.data(), uint32_t(s->ucode.size()));
  };
  const auto t0 = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(1)) {
    if (paint_record_next >= paint_records.size()) {
      paint_record_next = 0;
      paints_to_prepare.pop_front();
      if (paints_to_prepare.empty()) return;
    }
    const PipelineStore::Pipeline& sp = list[paint_records[paint_record_next++]];
    if (sp.key.size() != sizeof(PipelineKey)) continue;
    if (!r.upscaler() && DxbcShaderTranslator::Modification(sp.vs_mod).vertex.motion_vectors) continue;
    DxbcShader* const ps = paints_to_prepare.front();
    DxbcShader* const vs = load(sp.vs_hash);
    DxbcShader::Translation* const vst = vs ? shaders.Translate(*vs, sp.vs_mod) : nullptr;
    DxbcShader::Translation* const pst = shaders.Translate(*ps, sp.ps_mod);
    if (!vst || !pst) continue;
    PipelineKey key;
    std::memcpy(&key, sp.key.data(), sizeof(key));
    key.vs = vst;
    key.ps = pst;
    const uint64_t hash = XXH3_64bits(&key, sizeof(key)) ^ ReplacedTag(ps);
    if (pipelines.count(hash)) continue;
    Pipeline* const made = CreatePipeline(hash, key, *vst, pst, true, PipelineStore::kNoEntry);
    if (made->awaiting_hlsl) prepared_awaiting.push_back(made);
  }
}

void DrawRenderer::Impl::Prewarm() {
  const std::vector<PipelineStore::Pipeline>& list = store.loaded_pipelines();
  // The store is ordered most recently used first; only its hot prefix is prewarmed (the rest
  // compiles on demand if it comes up), so a start pays for what the player plays, not for
  // everything they ever played.
  const size_t hot = std::min(store.hot_count(), list.size());
  if (prewarm_next >= hot) return;
  const auto t0 = std::chrono::steady_clock::now();
  auto load = [&](uint64_t hash, xenos::ShaderType type) -> DxbcShader* {
    const PipelineStore::Shader* s = store.loaded_shader(hash);
    if (!s || s->type != uint32_t(type) || s->ucode.empty()) return nullptr;
    return shaders.Load(type, s->ucode.data(), uint32_t(s->ucode.size()));
  };
  do {
    const size_t index = prewarm_next++;
    const PipelineStore::Pipeline& sp = list[index];
    // An upscaler session's motion vector pipelines: unused without the upscaler.
    if (!r.upscaler() && DxbcShaderTranslator::Modification(sp.vs_mod).vertex.motion_vectors)
      continue;
    DxbcShader* vs = load(sp.vs_hash, xenos::ShaderType::kVertex);
    DxbcShader* ps = sp.ps_hash ? load(sp.ps_hash, xenos::ShaderType::kPixel) : nullptr;
    DxbcShader::Translation* vst = vs ? shaders.Translate(*vs, sp.vs_mod) : nullptr;
    DxbcShader::Translation* pst = ps ? shaders.Translate(*ps, sp.ps_mod) : nullptr;
    if (!vst || (sp.ps_hash && !pst) || sp.key.size() != sizeof(PipelineKey)) {
      ++prewarm_failed;
      continue;
    }
    PipelineKey key;
    std::memcpy(&key, sp.key.data(), sizeof(key));
    key.vs = vst;
    key.ps = pst;
    // Keyed as SelectPipeline keys it: tagged when the pixel shader is replaced (ours made).
    const uint64_t hash = XXH3_64bits(&key, sizeof(key)) ^ ReplacedTag(ps);
    if (!pipelines.count(hash)) CreatePipeline(hash, key, *vst, pst, true, uint32_t(index));
  } while (prewarm_next < hot &&
           std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(2));
  prewarm_ms +=
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  ++prewarm_frames;
  if (prewarm_next == hot)
    REXLOG_INFO(
        "PGR4 native pipelines: prewarm queued {} of {} recorded pipelines ({} failed) in "
        "{:.1f} ms over {} frames",
        hot - prewarm_failed, list.size(), prewarm_failed, prewarm_ms, prewarm_frames);
}

namespace {

// Fan vertex i is guest index i of `guest`, or i itself without one.
template <typename T>
void FanAsList(const uint8_t* guest, uint32_t count, T* out) {
  auto at = [guest](uint32_t i) {
    if (!guest) return T(i);
    T v;
    std::memcpy(&v, guest + size_t(i) * sizeof(T), sizeof(T));
    return v;
  };
  const T first = at(0);
  for (uint32_t t = 0; t + 2 < count; ++t, out += 3) {
    out[0] = at(t + 1);
    out[1] = at(t + 2);
    out[2] = first;
  }
}

}  // namespace

void DrawRenderer::Impl::WriteFanIndices(const DrawState& s, uint32_t count, bool wide,
                                         uint8_t* out) const {
  const uint8_t* guest = s.indexed ? membase + s.index_address : nullptr;
  if (wide)
    FanAsList(guest, count, reinterpret_cast<uint32_t*>(out));
  else
    FanAsList(guest, count, reinterpret_cast<uint16_t*>(out));
}

DrawRenderer::Impl::FanIndices DrawRenderer::Impl::UploadFanIndices(const DrawState& s,
                                                                    uint64_t tag) {
  const uint32_t host_count = (s.count - 2) * 3;
  const bool wide = s.indexed ? s.index_32bit : s.count > 0xFFFF;
  UploadPool::Alloc a;
  if (!vertex_upload.Allocate(uint64_t(host_count) * (wide ? 4 : 2), 16, tag, &a)) return {};
  WriteFanIndices(s, s.count, wide, a.cpu);
  return {a.gpu, host_count, wide};
}

DxbcShader* DrawRenderer::Impl::LoadShader(ShaderMemo* table, ShaderMemo*& last,
                                           const ShaderRef& ref, xenos::ShaderType type) {
  if (!ref.ucode || !ref.dwords) return nullptr;
  // Declaration-patched vertex shaders (IM_LOAD_IMMEDIATE) come from the tracker's copy.
  const uint32_t* ucode =
      ref.immediate ? ref.immediate : reinterpret_cast<const uint32_t*>(membase + ref.ucode);
  // Local change: the whole program checked (XXH3, about a kilobyte per draw), the previous
  // draw's shader again included.
  const uint64_t content = XXH3_64bits(ucode, size_t(ref.dwords) * sizeof(uint32_t));
  // The previous draw's shader again (most draws), as before the table.
  if (last && last->shader && last->ucode == ref.ucode && last->dwords == ref.dwords &&
      last->immediate_seq == ref.immediate_seq && last->binding == ref.binding &&
      last->content == content)
    return last->shader;
  uint32_t sample[ShaderMemo::kSample];
  for (uint32_t i = 0; i < ShaderMemo::kSample / 2; ++i) {
    sample[i] = ucode[std::min(i, ref.dwords - 1)];
    sample[ShaderMemo::kSample / 2 + i] = ucode[ref.dwords - 1 - std::min(i, ref.dwords - 1)];
  }
  ShaderMemo& memo = table[ShaderMemo::Slot(ref)];
  last = &memo;
  if (memo.shader && memo.ucode == ref.ucode && memo.dwords == ref.dwords &&
      memo.immediate_seq == ref.immediate_seq &&
      std::memcmp(memo.sample, sample, sizeof(sample)) == 0 && memo.content != content) {
    // What the sample alone missed: another program with the same ends.
    PGR4_LOG_FIRST(16, WARN,
                   "PGR4 native draw: {} shader {:08X} ({} dwords) changed with the same ends at "
                   "frame {}: loaded again",
                   type == xenos::ShaderType::kVertex ? "vertex" : "pixel", ref.ucode, ref.dwords,
                   r.frame_index());
  }
  if (memo.shader && memo.ucode == ref.ucode && memo.dwords == ref.dwords &&
      memo.immediate_seq == ref.immediate_seq && memo.content == content &&
      std::memcmp(memo.sample, sample, sizeof(sample)) == 0) {
    if (memo.binding == ref.binding) return memo.shader;
    PGR4_LOG_FIRST(
        8, INFO, "PGR4 native draw: vertex shader {:08X} ({} dwords) rebound in place at frame {}",
        ref.ucode, ref.dwords, r.frame_index());
  }
  memo.ucode = ref.ucode;
  memo.dwords = ref.dwords;
  memo.immediate_seq = ref.immediate_seq;
  memo.binding = ref.binding;
  memo.content = content;
  std::memcpy(memo.sample, sample, sizeof(sample));
  PrepareDiag& pd = PrepDiag();
  ++pd.shader_loads;
  if (ref.immediate) ++pd.shader_loads_immediate;
  memo.shader = shaders.Load(type, ucode, ref.dwords);
  return memo.shader;
}

// Local change (pgr4 project): the largest z / w of a non-indexed draw's vertices, read as the
// float4 at the start of each vertex of the VS's first fetch (positions already projected), or 0
// when the draw does not have that shape.
float DrawRenderer::Impl::FetchedMaxDepth(const DrawState& s, const RegisterFile& regs,
                                          const DxbcShader& vs, uint32_t count) {
  if (s.indexed || !count || count > 64) return 0.0f;
  const Shader::ConstantRegisterMap& vmap = vs.constant_register_map();
  uint32_t index = 0;
  while (index < 96 && !(vmap.vertex_fetch_bitmap[index / 32] >> (index % 32) & 1)) ++index;
  if (index == 96) return 0.0f;
  const xenos::xe_gpu_vertex_fetch_t f = regs.GetVertexFetch(index);
  if (f.type != xenos::FetchConstantType::kVertex || f.endian != xenos::Endian::k8in32)
    return 0.0f;
  const uint32_t start = f.address << 2, stride = (f.size << 2) / count;
  if (stride < 16 || stride % 4 || !InSharedMemory(start, stride * count)) return 0.0f;
  float z_max = 0.0f;
  for (uint32_t i = 0; i < count; ++i) {
    float v[4];
    for (uint32_t c = 0; c < 4; ++c) {
      uint32_t bits;
      std::memcpy(&bits, membase + start + i * stride + 4 * c, 4);
      bits = _byteswap_ulong(bits);
      std::memcpy(&v[c], &bits, 4);
    }
    if (!(v[3] > 0.0f)) return 0.0f;
    z_max = std::max(z_max, v[2] / v[3]);
  }
  return z_max;
}

// Vertex ranges the VS fetches and the index range into `out`. Fan indices are only checked:
// WriteFanIndices reads them from guest memory.
bool DrawRenderer::Impl::RequestVertices(const DrawState& s, const DxbcShader& vs,
                                         std::vector<VertexCopy>& out) {
  const RegisterFile& regs = *reinterpret_cast<const RegisterFile*>(s.regs);
  const Shader::ConstantRegisterMap& vmap = vs.constant_register_map();
  for (uint32_t w = 0; w < 3; ++w) {
    uint32_t bits = vmap.vertex_fetch_bitmap[w];
    while (bits) {
      uint32_t b = uint32_t(std::countr_zero(bits));
      bits &= bits - 1;
      const uint32_t index = w * 32 + b;
      xenos::xe_gpu_vertex_fetch_t f = regs.GetVertexFetch(index);
      if (f.type != xenos::FetchConstantType::kVertex) {
        PGR4_WARN_ONCE("PGR4 native draw: vertex fetch {} has type {} ({:08X} {:08X})", index,
                       uint32_t(f.type), f.dword_0, f.dword_1);
        if (f.type != xenos::FetchConstantType::kInvalidVertex) return false;
      }
      const uint32_t start = f.address << 2, size = f.size << 2;
      const bool inline_data = s.inline_vertices && start == s.inline_vertices;
      if (coherency) coherency->slot = uint8_t(index);
      // The water plane: copied at each draw (not left in place by the watch) and made larger.
      const bool plane = vs.ucode_data_hash() == kWaterPlaneVs && !s.indexed && s.count == 4;
      if (plane) {
        water_plane_start = start;
        water_plane_size = size;
      }
      const bool requested = RequestRange(start, size, !inline_data && !plane, out);
      water_plane_size = 0;
      if (!requested) return false;
    }
  }
  if (coherency) coherency->slot = 0xFF;
  if (!s.indexed) return true;
  const uint32_t index_bytes = s.count * (s.index_32bit ? 4u : 2u);
  if (xenos::PrimitiveType(s.primitive) == xenos::PrimitiveType::kTriangleFan)
    return InSharedMemory(s.index_address, index_bytes);
  return RequestRange(s.index_address, index_bytes, true, out);
}

void DrawRenderer::Impl::Prepare(const DrawState& s, uint64_t tag, DrawPrep* prep,
                                 std::vector<VertexCopy>* out, std::vector<AuditRange>* audit) {
  prep->captured = r.textures().MarkCapture();
  if (!s.vs.ucode || !s.vs.dwords) return;
  prep->vs = LoadShader(vs_memo, vs_last, s.vs, xenos::ShaderType::kVertex);
  prep->ps = LoadShader(ps_memo, ps_last, s.ps, xenos::ShaderType::kPixel);
  profile::Mark(profile::kCapShaders);
  if (audit_left) {
    ++audit_draws;
    AuditShader(s.vs, prep->vs, "vertex");
    AuditShader(s.ps, prep->ps, "pixel");
  }
  if (!prep->vs) return;
  if (coherency || audit_left) HashRegisterBlocks(s.regs, prep->regs_hash);
  upload_fence = tag;
  const bool indexed_ok = !s.indexed || s.index_address;
  // Straight into the batch: copies made before a failed range still go out (their pages
  // are marked valid already).
  preparing = true;
  segment_conflict = false;
  draw_reads.clear();
  prep->upload_ok = indexed_ok && RequestVertices(s, *prep->vs, *out);
  preparing = false;
  if (segment_conflict || segment_exact.size() >= kSegmentExactLimit) {
    NewSegment();
    prep->copy_segment = true;
  }
  for (const Read& read : draw_reads) SegmentAdd(read);
  if (audit_left) {
    for (const Read& read : draw_reads) {
      const AuditRange a = {read.begin, read.end - read.begin,
                            XXH3_64bits(membase + read.begin, read.end - read.begin)};
      if (!audit_seen.insert(AuditKey(a)).second) continue;
      if (audit_bytes + a.size > kAuditReadback) {
        ++audit_over;
        continue;
      }
      audit->push_back(a);
      ++audit_ranges;
      audit_bytes += a.size;
    }
  }
  if (coherency)
    for (const Read& read : draw_reads) coherency->segment_reads.push_back({read.begin, read.end});
  // Triangle fans: host indices from the guest's now (Draw reads them from here).
  if (prep->upload_ok && xenos::PrimitiveType(s.primitive) == xenos::PrimitiveType::kTriangleFan &&
      s.count >= 3) {
    const FanIndices fan = UploadFanIndices(s, tag);
    prep->fan_indices = fan.gpu;
    prep->fan_count = fan.count;
    prep->fan_wide = fan.wide;
  }
  profile::Mark(profile::kCapVertices);
}

void DrawRenderer::Impl::ShaderSlots::Assign(const DxbcShader& s) {
  // The translator numbers bindless descriptors from 1 (it counts the new binding before
  // assigning its index), so the table needs max index + 1 entries, not texture + sampler
  // count: the last binding (often a fetch's signed SRV) was left out of the upload.
  uint32_t entries = 1;
  mask = 0;
  for (const auto& tb : s.GetTextureBindingsAfterTranslation()) {
    entries = std::max(entries, tb.bindless_descriptor_index + 1);
    mask |= 1u << (tb.fetch_constant & 31);
  }
  for (const auto& sb : s.GetSamplerBindingsAfterTranslation()) {
    entries = std::max(entries, sb.bindless_descriptor_index + 1);
    mask |= 1u << (sb.fetch_constant & 31);
  }
  shader = &s;
  count = uint32_t(std::popcount(mask));
  n = std::min(entries, 64u);
  last = nullptr;
}

void DrawRenderer::Impl::SyncListFrame() {
  if (list_frame == r.frame_index()) return;
  // A new frame's list: nothing is bound, and shared memory decayed to the common state when the
  // previous frame's lists completed. Every CBV is uploaded again: the upload memory holding older
  // frames' constants may have been recycled.
  list_frame = r.frame_index();
  opaque_taken = false;
  ResetListState();
  shared_state = D3D12_RESOURCE_STATE_COMMON;
  for (CachedCbv* cb : Cbvs()) cb->Reset();
  // The motion vector pass reads the previous frame's constants where that frame uploaded them.
  constant_upload.set_keep_previous_frame(r.upscaler() != nullptr);
  motion_history.BeginFrame(list_frame);
}

DxbcShader::Translation* DrawRenderer::Impl::Translated(DxbcShader& shader, uint64_t modification) {
  TranslationMemo& m = translation_memo[(reinterpret_cast<uintptr_t>(&shader) >> 6) & 255];
  if (m.shader == &shader && m.modification == modification) {
    ++diag.translation_hits;
    return m.translation;
  }
  DxbcShader::Translation* t = shaders.Translate(shader, modification);
  if (t) m = {&shader, modification, t};
  return t;
}

// GetCurrentStateDescription, host render targets.
PipelineKey DrawRenderer::Impl::BuildPipelineKey(
    const RegisterFile& regs, xenos::PrimitiveType prim, const ShaderModifications& mods,
    const DxbcShader::Translation* vst, const DxbcShader::Translation* pst, uint8_t strip_cut,
    bool rasterization, bool polygonal, reg::PA_SU_SC_MODE_CNTL sc,
    reg::RB_DEPTHCONTROL depth_control, uint32_t color_mask, const BoundTargets& targets) const {
  PipelineKey key;
  std::memset(&key, 0, sizeof(key));
  key.vs = vst;
  key.ps = pst;
  key.gs_key = GeometryShaderKeyFor(prim, mods.vertex, pst ? mods.pixel : 0);
  key.topology_type = uint8_t(TopologyTypeForPrimitive(prim));
  key.strip_cut = strip_cut;
  key.samples = 1;
  if (!rasterization) {
    key.disable_raster = 1;
    key.cull = uint8_t(D3D12_CULL_MODE_NONE);
    key.depth_clip = 1;
  } else {
    bool cull_front = false, cull_back = false;
    key.cull = uint8_t(D3D12_CULL_MODE_NONE);
    if (polygonal) {
      key.front_ccw = sc.face == 0;
      cull_front = sc.cull_front != 0;
      cull_back = sc.cull_back != 0;
      if (cull_front)
        key.cull = uint8_t(D3D12_CULL_MODE_FRONT);
      else if (cull_back)
        key.cull = uint8_t(D3D12_CULL_MODE_BACK);
      if (!cull_front && sc.polymode_front_ptype != xenos::PolygonType::kTriangles)
        key.wireframe = 1;
      if (!cull_back && sc.polymode_back_ptype != xenos::PolygonType::kTriangles) key.wireframe = 1;
      if (sc.poly_mode != xenos::PolygonModeEnable::kDualMode) key.wireframe = 0;
    }
    float poly_offset, poly_offset_scale;
    GetPreferredFacePolygonOffset(regs, polygonal, poly_offset_scale, poly_offset);
    key.depth_bias = rex::graphics::draw_util::GetD3D10IntegerPolygonOffset(
        regs.Get<reg::RB_DEPTH_INFO>().depth_format, poly_offset);
    // The slope is per host pixel: scaled like Canary's for its draw resolution scale, by the
    // larger factor (more bias rather than Z fighting).
    key.slope_scaled_depth_bias = poly_offset_scale * xenos::kPolygonOffsetScaleSubpixelUnit *
                                  std::max(targets.scale.factor(0), targets.scale.factor(1));
    key.depth_clip = !regs.Get<reg::PA_CL_CLIP_CNTL>().clip_disable;
    if (targets.depth) {
      uint8_t func = uint8_t(xenos::CompareFunction::kAlways);
      if (depth_control.z_enable) {
        func = uint8_t(depth_control.zfunc);
        key.depth_write = depth_control.z_write_enable;
      }
      key.depth_func = uint8_t(D3D12_COMPARISON_FUNC_NEVER + func);
      key.depth_enable = func != uint8_t(xenos::CompareFunction::kAlways) || key.depth_write;
      if (depth_control.stencil_enable) {
        key.stencil_enable = 1;
        const bool bf = polygonal && depth_control.backface_enable;
        auto ref = regs.Get<reg::RB_STENCILREFMASK>(
            bf && cull_front ? XE_GPU_REG_RB_STENCILREFMASK_BF : XE_GPU_REG_RB_STENCILREFMASK);
        key.stencil_read_mask = uint8_t(ref.stencilmask);
        key.stencil_write_mask = uint8_t(ref.stencilwritemask);
        key.front[0] = uint8_t(D3D12_STENCIL_OP_KEEP + uint32_t(depth_control.stencilfail));
        key.front[1] = uint8_t(D3D12_STENCIL_OP_KEEP + uint32_t(depth_control.stencilzfail));
        key.front[2] = uint8_t(D3D12_STENCIL_OP_KEEP + uint32_t(depth_control.stencilzpass));
        key.front[3] = uint8_t(D3D12_COMPARISON_FUNC_NEVER + uint32_t(depth_control.stencilfunc));
        if (bf) {
          key.back[0] = uint8_t(D3D12_STENCIL_OP_KEEP + uint32_t(depth_control.stencilfail_bf));
          key.back[1] = uint8_t(D3D12_STENCIL_OP_KEEP + uint32_t(depth_control.stencilzfail_bf));
          key.back[2] = uint8_t(D3D12_STENCIL_OP_KEEP + uint32_t(depth_control.stencilzpass_bf));
          key.back[3] =
              uint8_t(D3D12_COMPARISON_FUNC_NEVER + uint32_t(depth_control.stencilfunc_bf));
        } else {
          std::memcpy(key.back, key.front, 4);
        }
      }
      if (key.depth_enable || key.stencil_enable) key.dsv = targets.depth->view_format;
    }
    for (uint32_t i = 0; i < 4; ++i) {
      if (!targets.color[i]) continue;
      key.num_rts = uint8_t(i + 1);
      key.rtv[i] = targets.color[i]->view_format;
      PipelineKey::Rt& rt = key.rt[i];
      rt.mask = uint8_t((color_mask >> (i * 4)) & 0xF);
      BlendFactor src = BlendFactor::kOne, dst = BlendFactor::kZero, src_a = BlendFactor::kOne,
                  dst_a = BlendFactor::kZero;
      uint32_t op = 0, op_a = 0;
      if (rt.mask) {
        auto bc = regs.Get<reg::RB_BLENDCONTROL>(kBlendControlRegs[i]);
        src = kBlendFactorMap[uint32_t(bc.color_srcblend) & 31];
        dst = kBlendFactorMap[uint32_t(bc.color_destblend) & 31];
        op = uint32_t(bc.color_comb_fcn);
        src_a = kBlendFactorAlphaMap[uint32_t(bc.alpha_srcblend) & 31];
        dst_a = kBlendFactorAlphaMap[uint32_t(bc.alpha_destblend) & 31];
        op_a = uint32_t(bc.alpha_comb_fcn);
      }
      if (src != BlendFactor::kOne || dst != BlendFactor::kZero || op != 0 ||
          src_a != BlendFactor::kOne || dst_a != BlendFactor::kZero || op_a != 0) {
        rt.blend = 1;
        rt.src = uint8_t(src);
        rt.dst = uint8_t(dst);
        rt.op = uint8_t(op);
        rt.src_a = uint8_t(src_a);
        rt.dst_a = uint8_t(dst_a);
        rt.op_a = uint8_t(op_a);
      } else {
        rt.src = uint8_t(BlendFactor::kOne);
        rt.src_a = uint8_t(BlendFactor::kOne);
      }
    }
    if (key.num_rts || key.dsv != DXGI_FORMAT_UNKNOWN) key.samples = uint8_t(targets.samples);
  }
  if (debug_mask & kDebugNoDepth) {
    key.depth_enable = key.depth_write = key.stencil_enable = 0;
    key.dsv = DXGI_FORMAT_UNKNOWN;
    if (!key.num_rts) key.samples = 1;
  }
  if (debug_mask & kDebugNoCull) key.cull = uint8_t(D3D12_CULL_MODE_NONE);
  if (debug_mask & kDebugNoBlend)
    for (auto& rt : key.rt) rt.blend = 0;
  if (debug_mask & kDebugNoDepthBias) {
    key.depth_bias = 0;
    key.slope_scaled_depth_bias = 0;
  }
  return key;
}

void DrawRenderer::Impl::LogDraw(const DrawState& s, const RegisterFile& regs, uint32_t draw_index,
                                 uint32_t count, const DxbcShader& vs, const DxbcShader* ps,
                                 const ShaderModifications& mods, const PipelineKey& key,
                                 const ViewportInfo& vp, const Scissor& scissor) {
  std::fprintf(
      draw_log,
      "%u fn=%08X prim=%u n=%u idx=%u vs=%016llX:%llX ps=%016llX:%llX rt=%u:%d,%d dsv=%d "
      "z=%u/%u/%u st=%u cull=%u blend=%u:%u/%u/%u,%u/%u/%u mask=%X bc=%08X cc=%08X "
      "f=%.2f,%.2f,%.2f,%.2f samples=%u vp=%u,%u,%u,%u z=%.3f-%.3f sc=%u,%u,%u,%u "
      "tex=%zu ci=%08X si=%08X di=%08X su=%08X dc=%08X po=%g/%g,%g/%g bias=%d/%g\n",
      draw_index, uint32_t(s.fn), s.primitive, count, uint32_t(s.indexed),
      (unsigned long long)vs.ucode_data_hash(), (unsigned long long)mods.vertex,
      ps ? (unsigned long long)ps->ucode_data_hash() : 0ull, (unsigned long long)mods.pixel,
      key.num_rts, int(key.rtv[0]), int(key.rtv[1]), int(key.dsv), key.depth_enable,
      key.depth_write, key.depth_func, key.stencil_enable, key.cull, key.rt[0].blend, key.rt[0].src,
      key.rt[0].dst, key.rt[0].op, key.rt[0].src_a, key.rt[0].dst_a, key.rt[0].op_a, key.rt[0].mask,
      regs[XE_GPU_REG_RB_BLENDCONTROL0], regs[XE_GPU_REG_RB_COLORCONTROL],
      regs.Get<float>(XE_GPU_REG_RB_BLEND_RED), regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
      regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE), regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA),
      key.samples, vp.xy_offset[0], vp.xy_offset[1], vp.xy_extent[0], vp.xy_extent[1], vp.z_min,
      vp.z_max, scissor.offset[0], scissor.offset[1], scissor.extent[0], scissor.extent[1],
      ps ? ps->GetTextureBindingsAfterTranslation().size() : size_t(0),
      regs[XE_GPU_REG_RB_COLOR_INFO], regs[XE_GPU_REG_RB_SURFACE_INFO],
      regs[XE_GPU_REG_RB_DEPTH_INFO], regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL],
      regs[XE_GPU_REG_RB_DEPTHCONTROL], regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE),
      regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET),
      regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE),
      regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET), key.depth_bias,
      key.slope_scaled_depth_bias);
  // Local change (pgr4 project): the guest call chain and the caller's frame
  // (pgr4_native_draw_log_stack).
  if (s.sp) {
    std::fprintf(draw_log, "  stack sp=%08X ret=", s.sp);
    for (uint32_t r : s.ret)
      if (r) std::fprintf(draw_log, "%08X ", r);
    std::fprintf(draw_log, "frame=");
    for (uint32_t w : s.frame) std::fprintf(draw_log, "%08X ", w);
    std::fprintf(draw_log, "\n");
  }
  // Where the draw's data comes from and the float constants each shader reads (hashed, then
  // in full).
  std::fprintf(draw_log, "  ia=%08X", s.indexed ? s.index_address : 0u);
  const Shader::ConstantRegisterMap& vmap = vs.constant_register_map();
  for (uint32_t w = 0; w < 3; ++w)
    for (uint32_t bits = vmap.vertex_fetch_bitmap[w]; bits; bits &= bits - 1) {
      const uint32_t index = w * 32 + uint32_t(std::countr_zero(bits));
      const xenos::xe_gpu_vertex_fetch_t vf = regs.GetVertexFetch(index);
      std::fprintf(draw_log, " vf%u=%08X+%X", index, vf.address << 2, vf.size << 2);
      // Local change (pgr4 project), development: the menus' quads (vs 19EE9CB8A3A22FD7, 44-byte
      // vertices, the position first) as the game placed them, in its 640x480 space.
      // And the videos' Bink frame (vs 161091CC82B3A2E9, 20-byte vertices), in pixels.
      // And the translucent panels (902579CC, A1A5C0F8), 44-byte vertices too.
      const uint64_t vs_hash = vs.ucode_data_hash();
      const uint32_t stride = vs_hash == 0x19EE9CB8A3A22FD7ull || vs_hash == 0x902579CC4AB6A791ull ||
                                      vs_hash == 0xA1A5C0F8B117DC24ull
                                  ? 44
                              : vs_hash == 0x161091CC82B3A2E9ull ? 20
                                                                 : 0;
      if (stride && index == 95 && membase && (vf.size << 2) <= 44 * 64) {
        std::fprintf(draw_log, " pos");
        for (uint32_t v = 0; v * stride + 16 <= (vf.size << 2) && v < 8; ++v) {
          const uint8_t* p = membase + (vf.address << 2) + v * stride;
          float f[4];
          for (uint32_t k = 0; k < 4; ++k)
            f[k] = std::bit_cast<float>(__builtin_bswap32(*reinterpret_cast<const uint32_t*>(p + 4 * k)));
          std::fprintf(draw_log, " (%g,%g,%g,%g)", f[0], f[1], f[2], f[3]);
        }
      }
    }
  auto float_hash = [&](const Shader* shader, uint32_t base) {
    uint64_t h = 0;
    if (!shader) return h;
    const Shader::ConstantRegisterMap& map = shader->constant_register_map();
    for (uint32_t w = 0; w < 4; ++w)
      for (uint64_t bits = map.float_bitmap[w]; bits; bits &= bits - 1)
        h = XXH3_64bits_withSeed(&regs[base + (w * 64 + uint32_t(std::countr_zero(bits))) * 4], 16,
                                 h);
    return h;
  };
  std::fprintf(draw_log, " cvs=%016llX cps=%016llX\n",
               (unsigned long long)float_hash(&vs, XE_GPU_REG_SHADER_CONSTANT_000_X),
               (unsigned long long)float_hash(ps, XE_GPU_REG_SHADER_CONSTANT_256_X));
  auto log_floats = [&](const char* name, const Shader& shader, uint32_t base) {
    std::fprintf(draw_log, "  %s", name);
    const Shader::ConstantRegisterMap& map = shader.constant_register_map();
    for (uint32_t w = 0; w < 4; ++w)
      for (uint64_t bits = map.float_bitmap[w]; bits; bits &= bits - 1) {
        const uint32_t c = w * 64 + uint32_t(std::countr_zero(bits));
        const uint32_t r = base + c * 4;
        std::fprintf(draw_log, " %u=%g,%g,%g,%g", c, regs.Get<float>(r), regs.Get<float>(r + 1),
                     regs.Get<float>(r + 2), regs.Get<float>(r + 3));
      }
    std::fputc('\n', draw_log);
  };
  log_floats("vc", vs, XE_GPU_REG_SHADER_CONSTANT_000_X);
  if (ps) log_floats("pc", *ps, XE_GPU_REG_SHADER_CONSTANT_256_X);
}

ID3D12PipelineState* DrawRenderer::Impl::CoveragePipeline(const PipelineKey& key,
                                                          const DxbcShader::Translation& vs,
                                                          const DxbcShader::Translation* ps) {
  PipelineKey c = key;
  c.depth_enable = 1;
  c.depth_write = 0;
  c.depth_func = D3D12_COMPARISON_FUNC_ALWAYS;
  c.stencil_enable = 0;
  c.stencil_read_mask = c.stencil_write_mask = 0;
  std::memset(c.front, 0, sizeof(c.front));
  std::memset(c.back, 0, sizeof(c.back));
  for (PipelineKey::Rt& rt : c.rt) rt = PipelineKey::Rt{};
  const uint64_t hash = XXH3_64bits(&c, sizeof(c));
  Pipeline* const p = GetPipeline(hash, c, vs, ps, 0, /*background=*/true);
  return p->state.load(std::memory_order_acquire) == kReady ? p->pso.Get() : nullptr;
}

ID3D12PipelineState* DrawRenderer::Impl::SelectPipeline(
    const PipelineKey& key, const DxbcShader::Translation& vst, const DxbcShader::Translation* pst,
    DxbcShader*& vs, DxbcShader*& ps, DxbcShader* vs_generic, DxbcShader* ps_generic,
    uint64_t pixel_mod, uint64_t generic_pixel_mod, const ShaderDrawRegs& sdr,
    xenos::PrimitiveType prim) {
  uint64_t key_hash = XXH3_64bits(&key, sizeof(key));
  // Local change: a replaced pixel shader's pipelines are made again at each reload of its file
  // (ShaderReplacer::generation).
  const uint64_t replaced = ReplacedTag(ps);
  key_hash ^= replaced;
  if (replaced && ps && paints_seen.insert(ps->ucode_data_hash()).second && replacer.IsCarPaint(ps->ucode_data_hash()))
    paints_to_prepare.push_back(ps);
  // Each car shader's first draw, ours or the game's, again every 2 s (the vehicles menu's car, named by the
  // VehiclePvsData file the game asks for as it shows it: tools/car_shaders.py), and any pixel shader with no name
  // yet (shader_names.tsv to make again).
  if (ps && !REXCVAR_GET(pgr4_native_capture_dir).empty()) {
    static auto since = std::chrono::steady_clock::now();
    if (const auto now = std::chrono::steady_clock::now(); now - since > std::chrono::seconds(2)) {
      since = now;
      car_unreplaced_seen.clear();
    }
  }
  if (ps && !REXCVAR_GET(pgr4_native_capture_dir).empty() &&
      car_unreplaced_seen.insert(ps->ucode_data_hash()).second) {
    if (!shader_names_read) {
      shader_names_read = true;
      wchar_t exe[MAX_PATH] = {};
      GetModuleFileNameW(nullptr, exe, MAX_PATH);
      std::ifstream in(std::filesystem::path(exe).parent_path() / "shaders" / "shader_names.tsv");
      std::string line;
      while (std::getline(in, line)) {
        uint64_t h = 0;
        if (line.size() > 20 && line.compare(16, 4, "\tps\t") == 0 &&
            std::from_chars(line.data(), line.data() + 16, h, 16).ec == std::errc()) {
          const size_t end = line.find('\t', 20);
          shader_names.emplace(h, line.substr(20, end == std::string::npos ? std::string::npos : end - 20));
        }
      }
    }
    const auto it = shader_names.find(ps->ucode_data_hash());
    static std::unordered_set<uint64_t> unnamed_logged;
    if (it == shader_names.end() && !unnamed_logged.insert(ps->ucode_data_hash()).second) {
    } else if (it == shader_names.end())
      REXLOG_INFO("PGR4 shader first drawn: {:016X} (no name yet) {}", ps->ucode_data_hash(), replaced ? "ours" : "game's");
    else if (it->second.starts_with("psCar") && replaced)
      REXLOG_INFO("PGR4 car shader first drawn: {:016X} {} ours", ps->ucode_data_hash(), it->second);
    else if (it->second.starts_with("psCar"))
      REXLOG_WARN("PGR4 car shader without HLSL: {:016X} {} (ucode in the shader dump dir)", ps->ucode_data_hash(),
                  it->second);
  }
  PsoMemo& pso_slot = pso_memo[key_hash & 1023];
  if (pso_slot.pso && pso_slot.hash == key_hash) {
    ++diag.pso_hits;
    return pso_slot.pso;
  }
  const uint32_t variant = (vs != vs_generic ? 1u : 0u) | (ps && ps != ps_generic ? 2u : 0u) |
                           (pst && pixel_mod != generic_pixel_mod ? 4u : 0u);
  const auto found = pipelines.find(key_hash);
  Pipeline* p = found != pipelines.end() ? found->second.get() : nullptr;
  // A new specialization (bool constants, texture signs) of shaders and state whose
  // unspecialized pipeline is ready compiles in the background while its draws use that
  // pipeline: the same image for a little more GPU time. A race keeps bringing new
  // combinations of known shaders for minutes, and each would otherwise hold a frame for its
  // compile. The first draw of the shaders and state waits for its specialization, which has
  // less code and compiles faster, and queues the unspecialized pipeline behind it.
  Pipeline* use = nullptr;
  DxbcShader* generic_ps = nullptr;
  if (variant && generic_pipelines &&
      (!p || (p->behind_generic && p->state.load(std::memory_order_acquire) == kCompiling))) {
    ShaderModifications gm =
        SelectModifications(shaders.translator(), *vs_generic, ps_generic, sdr);
    if (DxbcShaderTranslator::Modification(pixel_mod).pixel.motion_vectors && gm.pixel_shader_used)
      gm = WithMotionVectors(gm);
    DxbcShader* gps = gm.pixel_shader_used ? ps_generic : nullptr;
    // The draw's setup so far read the specialized pixel shader's depth output.
    if (gm.draw && !gps == !ps && (!gps || gps->writes_depth() == ps->writes_depth())) {
      DxbcShader::Translation* gvst = Translated(*vs_generic, gm.vertex);
      DxbcShader::Translation* gpst = gps ? Translated(*gps, gm.pixel) : nullptr;
      if (gvst && (!gps || gpst)) {
        PipelineKey gkey = key;
        gkey.vs = gvst;
        gkey.ps = gpst;
        gkey.gs_key = GeometryShaderKeyFor(prim, gm.vertex, gpst ? gm.pixel : 0);
        const uint64_t ghash = XXH3_64bits(&gkey, sizeof(gkey)) ^
                               (gps == ps ? replaced : 0);
        const auto gf = pipelines.find(ghash);
        if (gf == pipelines.end()) {
          GetPipeline(ghash, gkey, *gvst, gpst, 0, /*background=*/true);
        } else if (gf->second->state.load(std::memory_order_acquire) == kReady) {
          use = gf->second.get();
          generic_ps = gps;
          if (!p) {
            p = GetPipeline(key_hash, key, vst, pst, variant, /*background=*/true);
            p->behind_generic = true;
          }
        }
      }
    }
  }
  if (!use) use = p ? p : GetPipeline(key_hash, key, vst, pst, variant);
  // The store's record of a prewarmed pipeline ages with its first use (the pso memo above and
  // the pipelines map's hits reach no GetPipeline), so it stays while the player plays it.
  if (use->store_entry != PipelineStore::kNoEntry) {
    store.TouchEntry(use->store_entry);
    use->store_entry = PipelineStore::kNoEntry;
  }
  if (use->awaiting_hlsl) ResumeAwaiting(use);
  const uint8_t state = use->state.load(std::memory_order_acquire);
  if (state == kCompiling) {
    if (use->prewarm && !use->awaiting_hlsl) PromotePipeline(use);
    if (use->pending_frame != r.frame_index() && !use->awaiting_hlsl) {
      use->pending_frame = r.frame_index();
      use->surface = replaced && key.depth_enable && key.depth_write;
      frame_pending.push_back(use);
    }
  }
  // Local change (pgr4 project): a replaced pixel shader's pipeline compiling (its file changed,
  // its HLSL just compiled, a new specialization): drawn meanwhile with its last pipeline ready,
  // or the game's shader's, the same shaders' bindings. Skipped, its draws left holes (black
  // flashes on the menu's scene for 5 to 10 s after a hooks file changed).
  if (replaced && use == p) {
    const uint64_t base = key_hash ^ replaced;
    if (state == kReady) {
      replaced_ready[base] = use;
    } else if (state == kCompiling) {
      // Not the game's shader's pipeline (the key's own hash): for a replaced shader the key
      // holds its unspecialized translation, which draws wrong (white flashes).
      Pipeline* fallback = nullptr;
      if (const auto r = replaced_ready.find(base); r != replaced_ready.end() &&
          r->second->state.load(std::memory_order_acquire) == kReady)
        fallback = r->second;
      if (fallback) {
        // Not a draw missing: the frame needs no wait for it (WaitForPipelines).
        if (!frame_pending.empty() && frame_pending.back() == use) frame_pending.pop_back();
        ++stats.pso_pending;
        return fallback->pso.Get();
      }
      // A car's shader: never the frame held for it, whose compile takes seconds (freezes at a change of
      // view, 07/11/2026). The game's own shader meanwhile, which takes milliseconds: the frame waits for
      // that one at most. Ours takes over as soon as it is ready.
      if (ps && REXCVAR_GET(pgr4_native_car_no_wait) && IsCarShader(ps->ucode_data_hash())) {
        if (!frame_pending.empty() && frame_pending.back() == use) frame_pending.pop_back();
        Pipeline* const game = GetPipeline(base, key, vst, pst, variant);
        ++stats.pso_pending;
        if (game->state.load(std::memory_order_acquire) == kReady) {
          if (!REXCVAR_GET(pgr4_native_capture_dir).empty())
            PGR4_LOG_FIRST(64, INFO, "PGR4 car shader {:016X}: the game's meanwhile", ps->ucode_data_hash());
          return game->pso.Get();
        }
        if (game->state.load(std::memory_order_acquire) == kCompiling && game->pending_frame != r.frame_index()) {
          game->pending_frame = r.frame_index();
          game->surface = true;
          frame_pending.push_back(game);
        }
        return nullptr;
      }
      // None ready yet (a first run, its HLSL compiling: the wet road's in a race): the frame is
      // held (not shown, the last one stays) until it is, as a surface's. Drawn with the game's
      // shader meanwhile, it flashed white.
      use->surface = true;
      if (use->pending_frame != r.frame_index()) {
        use->pending_frame = r.frame_index();
        frame_pending.push_back(use);
      }
    }
  }
  if (state != kReady) {
    if (state == kCompiling) {
      ++stats.pso_pending;
    } else {
      ++stats.failed;
    }
    return nullptr;
  }
  ID3D12PipelineState* const pso = use->pso.Get();
  if (use == p) {
    pso_slot = {key_hash, pso};
  } else {
    // Bindings and constants follow the shaders the pipeline was built from.
    vs = vs_generic;
    ps = generic_ps;
    ++stats.pso_generic;
  }
  return pso;
}

uint32_t DrawRenderer::Impl::BindTextures(const TextureBindContext& ctx, const DxbcShader& shader,
                                          uint32_t* indices) {
  const DrawState& s = ctx.s;
  const RegisterFile& regs = ctx.regs;
  TextureCache& texture_cache = ctx.texture_cache;
  const uint64_t frame_fence = ctx.frame_fence;
  uint8_t* const signs = ctx.signs;
  const auto& tex = shader.GetTextureBindingsAfterTranslation();
  const auto& smp = shader.GetSamplerBindingsAfterTranslation();
  ShaderSlots& ss = shader_slots[(reinterpret_cast<uintptr_t>(&shader) >> 6) & 511];
  if (ss.shader != &shader) ss.Assign(shader);
  const uint32_t n = ss.n;
  // The memo entry for (shader, the fetch constants it reads); null if not memoizable.
  BindMemo* bm = nullptr;
  const uint64_t epoch = texture_cache.change_epoch();
  // While the draw log records, every binding goes through Get so each one is logged.
  const bool catalog_on = REXCVAR_GET(pgr4_native_texture_catalog) || frame_textures_on;
  if (!draw_log && !catalog_on && ss.count <= BindMemo::kMaxSlots &&
      tex.size() <= BindMemo::kMaxBound) {
    auto same_fetch = [&](const BindMemo& e) {
      if (e.shader != &shader || e.lod_bias != ctx.lod_bias) return false;
      const uint32_t* stored = e.data;
      for (uint32_t m = ss.mask; m; m &= m - 1, stored += 6)
        if (std::memcmp(stored, s.fetch(uint32_t(std::countr_zero(m))), 24) != 0) return false;
      return true;
    };
    profile::Counter miss_kind = profile::kBindMissEmpty;
    bool found = false;
    if (ss.last && same_fetch(*ss.last)) {
      bm = ss.last;
      found = true;
      profile::Count(profile::kBindMru);
    } else {
      uint64_t h =
          (uint64_t(reinterpret_cast<uintptr_t>(&shader)) ^ ctx.lod_bias) * 0x9E3779B97F4A7C15ull;
      for (uint32_t m = ss.mask; m; m &= m - 1) {
        const uint32_t* f = s.fetch(uint32_t(std::countr_zero(m)));
        uint64_t a, b, c;
        std::memcpy(&a, f, 8);
        std::memcpy(&b, f + 2, 8);
        std::memcpy(&c, f + 4, 8);
        h = (h ^ a ^ std::rotl(b, 21) ^ std::rotl(c, 42)) * 0x9E3779B97F4A7C15ull;
      }
      BindMemo* set = &bind_memo[size_t(h >> (64 - kBindMemoSetBits)) << 1];
      for (uint32_t way = 0; way < 2 && !found; ++way) {
        if (same_fetch(set[way])) {
          bm = &set[way];
          found = true;
        } else if (set[way].shader == &shader) {
          miss_kind = profile::kBindMissFetch;
        }
      }
      // Not found: fill the least recently used way.
      if (!found) bm = set[0].used <= set[1].used ? &set[0] : &set[1];
      bm->used = ++bind_clock;
      ss.last = bm;
    }
    if (found) {
      bool same = true;
      if (bm->epoch != epoch) {
        for (uint32_t i = 0; i < bm->bound && same; ++i)
          same = texture_cache.Unchanged(bm->entry[i]);
        if (same) bm->epoch = epoch;
      }
      if (same) {
        if (bm->touched != frame_fence) {
          for (uint32_t i = 0; i < bm->bound; ++i) texture_cache.Touch(bm->entry[i], frame_fence);
          bm->touched = frame_fence;
        }
        const uint32_t* stored = bm->data + ss.count * 6;
        std::memcpy(indices, stored, n * sizeof(uint32_t));
        stored += n;
        for (uint32_t i = 0; i < bm->bound; ++i) {
          signs[stored[i] & 0xFF] = uint8_t(stored[i] >> 8);
          if (stored[i] >> 16 & 1) *ctx.resolution_scaled |= 1u << (stored[i] & 31);
        }
        stats.textures += bm->textures;
        ++stats.bind_memo_hits;
        profile::Count(profile::kBindHit);
        return n;
      }
      miss_kind = profile::kBindMissTexture;
    }
    profile::Count(miss_kind);
  }
  profile::Scope miss_scope(profile::kTexMiss);
  ++stats.bind_memo_misses;
  std::fill(indices, indices + n, 0u);
  uint32_t bound = 0, textures = 0, missing = 0, stale = 0;
  for (const auto& tb : tex) {
    ++textures;
    TextureBinding b;
    uint32_t index;
    const uint32_t* fetch = s.fetch(tb.fetch_constant);
    bool got;
    {
      profile::Scope get_scope(profile::kTexGet);
      got = texture_cache.Get(fetch, ctx.list, frame_fence, &b, ctx.captured) && b.resource;
    }
    if (catalog_on && got && b.content) {
      CatalogEntry e = {shader.ucode_data_hash(), b.content, tb.fetch_constant,
                        b.key.width, b.key.height, uint32_t(b.key.format), b.hd};
      if (frame_textures_on) {
        xenos::xe_gpu_texture_fetch_t tf;
        std::memcpy(&tf, fetch, sizeof(tf));
        texture_util::GetSubresourcesFromFetchConstant(tf, nullptr, nullptr, nullptr, nullptr,
                                                       nullptr, &e.fetch_min, &e.fetch_max);
        e.mips = b.key.mip_levels();
      }
      if (frame_textures_on) frame_textures.insert(e);
      if (REXCVAR_GET(pgr4_native_texture_catalog)) catalog.insert(e);
    }
    if (draw_log)
      std::fprintf(draw_log, "  tf%u %08X fmt=%u %ux%ux%u dim=%u tiled=%u mips=%u%s%s\n",
                   tb.fetch_constant, b.key.base_page << 12, uint32_t(b.key.format), b.key.width,
                   b.key.height, b.key.depth_or_array_size, uint32_t(b.key.dimension),
                   uint32_t(b.key.tiled), b.key.mip_levels(), got ? "" : " missing",
                   got && b.aliased ? " alias" : "");
    if (got) {
      profile::Scope srv_scope(profile::kTexSrv);
      stale += b.stale;
      const uint32_t mapping = b.srv.Shader4ComponentMapping;
      const uint64_t sh =
          (uint64_t(reinterpret_cast<uintptr_t>(b.resource)) ^ (uint64_t(mapping) << 32) ^
           (uint64_t(tb.is_signed) << 3) ^ (uint64_t(tb.dimension) << 4)) *
          0x9E3779B97F4A7C15ull;
      SrvMemo& m = srv_memo[sh >> (64 - kSrvMemoBits)];
      if (m.resource == b.resource && m.mapping == mapping &&
          m.is_signed == uint8_t(tb.is_signed) && m.dimension == uint8_t(tb.dimension)) {
        index = m.index;
        profile::Count(profile::kSrvMemoHit);
      } else {
        index = TextureDescriptor(b, tb);
        m.resource = b.resource;
        m.mapping = mapping;
        m.is_signed = uint8_t(tb.is_signed);
        m.dimension = uint8_t(tb.dimension);
        m.index = index;
      }
      signs[tb.fetch_constant] = b.swizzled_signs;
      const bool scaled = b.aliased && DrawResolutionScale() > 1;
      if (scaled) *ctx.resolution_scaled |= 1u << (tb.fetch_constant & 31);
      if (bm) {
        bm->entry[bound] = b.entry;
        bm->data[ss.count * 6 + n + bound] = uint32_t(uint8_t(tb.fetch_constant)) |
                                             uint32_t(b.swizzled_signs) << 8 |
                                             uint32_t(scaled) << 16;
      }
      ++bound;
    } else {
      ++missing;
      index = NullView(tb.dimension);
    }
    if (tb.bindless_descriptor_index < 64) indices[tb.bindless_descriptor_index] = index;
  }
  for (const auto& sb : smp) {
    const uint32_t* fetch = s.fetch(sb.fetch_constant);
    const uint8_t filters[4] = {uint8_t(sb.mag_filter), uint8_t(sb.min_filter),
                                uint8_t(sb.mip_filter), uint8_t(sb.aniso_filter)};
    uint32_t sh = 0;
    for (int i = 0; i < 6; ++i) sh = (sh ^ fetch[i]) * 0x9E3779B1u;
    uint32_t fw;
    std::memcpy(&fw, filters, 4);
    sh = (sh ^ fw) * 0x9E3779B1u;
    sh = (sh ^ ctx.lod_bias) * 0x9E3779B1u;
    SamplerMemo& m = sampler_memo[sh >> (32 - kSamplerMemoBits)];
    uint32_t index;
    if (std::memcmp(m.filters, filters, 4) == 0 && m.lod_bias == ctx.lod_bias &&
        std::memcmp(m.fetch, fetch, sizeof(m.fetch)) == 0) {
      index = m.index;
    } else {
      index = SamplerDescriptor(GetSamplerKey(regs, sb, min_aniso, ctx.lod_bias));
      std::memcpy(m.filters, filters, 4);
      std::memcpy(m.fetch, fetch, sizeof(m.fetch));
      m.lod_bias = ctx.lod_bias;
      m.index = index;
    }
    if (sb.bindless_descriptor_index < 64) indices[sb.bindless_descriptor_index] = index;
  }
  stats.textures += textures;
  stats.textures_missing += missing;
  stats.textures_stale += stale;
  if (bm && !missing && !stale) {
    bm->shader = &shader;
    bm->lod_bias = ctx.lod_bias;
    bm->epoch = epoch;
    bm->touched = frame_fence;  // Get marked them
    uint32_t* stored = bm->data;
    for (uint32_t m = ss.mask; m; m &= m - 1, stored += 6)
      std::memcpy(stored, s.fetch(uint32_t(std::countr_zero(m))), 24);
    std::memcpy(stored, indices, n * sizeof(uint32_t));
    bm->bound = bound;
    bm->textures = textures;
  } else {
    if (bm) bm->shader = nullptr;
    profile::Count(profile::kBindNoMemo);
  }
  return n;
}

// UpdateSystemConstantValues, host render targets, no ROV.
void DrawRenderer::Impl::FillSystemConstants(const DrawState& s, const RegisterFile& regs,
                                             xenos::PrimitiveType prim, bool polygonal,
                                             xenos::Endian index_endian, const ViewportInfo& vp,
                                             uint32_t used_textures, const uint8_t* signs,
                                             const BoundTargets& targets,
                                             DxbcShaderTranslator::SystemConstants& sys) {
  std::memset(&sys, 0, sizeof(sys));
  auto vte = regs.Get<reg::PA_CL_VTE_CNTL>();
  auto clip = regs.Get<reg::PA_CL_CLIP_CNTL>();
  auto colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  auto surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  uint32_t flags = 0;
  if (vte.vtx_xy_fmt) flags |= DxbcShaderTranslator::kSysFlag_XYDividedByW;
  if (vte.vtx_z_fmt) flags |= DxbcShaderTranslator::kSysFlag_ZDividedByW;
  if (vte.vtx_w0_fmt) flags |= DxbcShaderTranslator::kSysFlag_WNotReciprocal;
  if (polygonal) flags |= DxbcShaderTranslator::kSysFlag_PrimitivePolygonal;
  if (rex::graphics::draw_util::IsPrimitiveLine(false, prim))
    flags |= DxbcShaderTranslator::kSysFlag_PrimitiveLine;
  // No kSysFlag_DepthFloat24: without float24 conversion in the shader, its only effect is to
  // scale oDepth into the plugin's host 0...0.5 float24 range, while viewports, clears and
  // resolves here keep D24FS8 in 0...1 (HostViewportInfo). Setting it halves the depth that
  // depth-restoring passes write.
  const xenos::CompareFunction alpha_func =
      colorcontrol.alpha_test_enable && !(debug_mask & kDebugNoAlphaTest)
          ? colorcontrol.alpha_func
          : xenos::CompareFunction::kAlways;
  flags |= uint32_t(alpha_func) << DxbcShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;
  // Gamma render targets are unorm16 on the host (the plugin's setting): no conversion flag.
  sys.flags = flags;
  sys.tessellation_factor_range_min = regs.Get<float>(XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL) + 1.0f;
  sys.tessellation_factor_range_max = regs.Get<float>(XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL) + 1.0f;
  sys.vertex_index_endian = index_endian;
  sys.vertex_index_offset = regs.Get<reg::VGT_INDX_OFFSET>().indx_offset;
  sys.vertex_index_min = regs.Get<reg::VGT_MIN_VTX_INDX>().min_indx;
  sys.vertex_index_max = regs.Get<reg::VGT_MAX_VTX_INDX>().max_indx;
  if (!clip.clip_disable) {
    float* ucp = sys.user_clip_planes[0];
    uint32_t remaining = clip.ucp_ena;
    while (remaining) {
      uint32_t i = uint32_t(std::countr_zero(remaining));
      remaining &= remaining - 1;
      std::memcpy(ucp, &regs[XE_GPU_REG_PA_CL_UCP_0_X + i * 4], 4 * sizeof(float));
      ucp += 4;
    }
  }
  for (uint32_t i = 0; i < 3; ++i) {
    sys.ndc_scale[i] = vp.ndc_scale[i];
    sys.ndc_offset[i] = vp.ndc_offset[i];
  }
  // The upscaler's jitter in render pixels, a translation in host clip space as FSR's docs
  // apply it to the projection (NDC y up, pixels down).
  if (const Upscaler* upscaler = r.upscaler();
      upscaler && Upscaler::IsScene(surface_info.value) &&
      Upscaler::IsSceneWidth(int32_t(vp.xy_offset[0]),
                             int32_t(vp.xy_offset[0] + vp.xy_extent[0]))) {
    const float w = float(std::max(vp.xy_extent[0], 1u)) * targets.scale.factor(0);
    const float h = float(std::max(vp.xy_extent[1], 1u)) * targets.scale.factor(1);
    sys.ndc_offset[0] += 2.0f * upscaler->jitter_x() / w;
    sys.ndc_offset[1] -= 2.0f * upscaler->jitter_y() / h;
  }
  if (prim == xenos::PrimitiveType::kPointList) {
    auto minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
    auto size = regs.Get<reg::PA_SU_POINT_SIZE>();
    sys.point_vertex_diameter_min = float(minmax.min_size) * (2.0f / 16.0f);
    sys.point_vertex_diameter_max = float(minmax.max_size) * (2.0f / 16.0f);
    sys.point_constant_diameter[0] = float(size.width) * (2.0f / 16.0f);
    sys.point_constant_diameter[1] = float(size.height) * (2.0f / 16.0f);
    sys.point_screen_diameter_to_ndc_radius[0] = 1.0f / float(std::max(vp.xy_extent[0], 1u));
    sys.point_screen_diameter_to_ndc_radius[1] = 1.0f / float(std::max(vp.xy_extent[1], 1u));
  }
  uint32_t remaining = used_textures;
  while (remaining) {
    uint32_t i = uint32_t(std::countr_zero(remaining));
    remaining &= remaining - 1;
    sys.texture_swizzled_signs[i >> 2] |= uint32_t(signs[i]) << ((i & 3) * 8);
    if (profile::On()) {
      const uint32_t* f = s.fetch(i);
      const uint8_t sg = signs[i];
      ++diag.fetch_classes[sg == 0 ? 0 : sg == 0x3F ? 1 : sg == 0xFF ? 2 : sg == 0x55 ? 3 : 4];
      if ((f[3] >> 13) & 0x3F) ++diag.fetch_classes[5];
      if ((f[4] >> 12) & 0x3FF) ++diag.fetch_classes[6];
    }
  }
  // The host targets' (the scene's own with the upscaler or --pgr4_native_msaa). 8x takes 4x's:
  // the translator's alpha to coverage gives samples 4:7 the coverage of 0:3 (patches/0051).
  sys.sample_count_log2[0] = targets.samples >= 4 ? 1 : 0;
  sys.sample_count_log2[1] = targets.samples >= 2 ? 1 : 0;
  sys.alpha_test_reference = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
  sys.alpha_to_mask = colorcontrol.alpha_to_mask_enable ? (colorcontrol.value >> 24) | (1 << 8) : 0;
  // Local change (pgr4 project): the scene's alpha-tested draws (the painted lines, the barriers'
  // panels, foliage: cut out by their alpha) given alpha to coverage too with host MSAA, as the
  // game's own alpha-to-mask draws: their edges were all or nothing per pixel whatever the
  // samples, and crawled as the camera moved (most at low speed).
  if (!sys.alpha_to_mask && colorcontrol.alpha_test_enable && targets.samples > 1 &&
      REXCVAR_GET(pgr4_native_alpha_to_coverage) && Upscaler::IsScene(surface_info.value))
    sys.alpha_to_mask = (colorcontrol.value >> 24) | (1 << 8);
  // Local change (pgr4 project): the game's alpha to mask (the trees) varies its threshold on a 2x2 grid of
  // pixels, for its samples to smooth over. On one sample nothing smooths it: a checker on the thin branches, the
  // more visible the lower the resolution scale. One threshold instead, as an alpha test: 0.5 thinned the trees
  // close up and 0.25 thickened them (Tazz, 06/11/2026), which the register's steps cannot go between.
  // Never inside an occlusion query: the lamps' test sprites count their samples through the game's grid, and the
  // single threshold left them none (headlights unlit and without their flare at night, 14/11/2026).
  if (const float cut = float(REXCVAR_GET(pgr4_native_alpha_cut));
      cut > 0.0f && colorcontrol.alpha_to_mask_enable && !colorcontrol.alpha_test_enable && targets.samples == 1 &&
      Upscaler::IsScene(surface_info.value) && !(r.occlusion() && r.occlusion()->counting())) {
    sys.alpha_to_mask = 0;
    sys.flags = (sys.flags & ~(7u << DxbcShaderTranslator::kSysFlag_AlphaPassIfLess_Shift)) |
                (uint32_t(xenos::CompareFunction::kGreaterEqual) << DxbcShaderTranslator::kSysFlag_AlphaPassIfLess_Shift);
    sys.alpha_test_reference = cut;
  }
  for (uint32_t i = 0; i < 4; ++i) {
    auto ci = regs.Get<reg::RB_COLOR_INFO>(kColorInfoRegs[i]);
    int32_t bias = ci.color_exp_bias;
    // snorm16 render targets use the full -32...32 range (the plugin's default).
    if (ci.color_format == xenos::ColorRenderTargetFormat::k_16_16 ||
        ci.color_format == xenos::ColorRenderTargetFormat::k_16_16_16_16)
      bias -= 5;
    uint32_t bits = uint32_t(0x3F800000 + (bias << 23));
    std::memcpy(&sys.color_exp_bias[i], &bits, 4);
  }
}

void DrawRenderer::Impl::Draw(const DrawState& s, const DrawPrep* prep) {
  ID3D12GraphicsCommandList* list = r.command_list();
  SyncListFrame();
  if (prep && prep->regs_hash[0]) {
    uint64_t now[5];
    HashRegisterBlocks(s.regs, now);
    if (std::memcmp(now, prep->regs_hash, sizeof(now))) {
      ++stats.reg_mismatches;
      PGR4_LOG_FIRST(100, WARN,
                     "PGR4 native coherency: frame {} draw {} registers differ from Prepare's in "
                     "blocks{}{}{}{}{}",
                     r.frame_index(), frame_draws, now[0] != prep->regs_hash[0] ? " state" : "",
                     now[1] != prep->regs_hash[1] ? " vs-float" : "",
                     now[2] != prep->regs_hash[2] ? " ps-float" : "",
                     now[3] != prep->regs_hash[3] ? " fetch" : "",
                     now[4] != prep->regs_hash[4] ? " bool-loop" : "");
    }
  }
  if (prep) {
    // The guest thread copied the ranges (and marked their pages valid): they go into shared
    // memory whether or not the draw is issued.
    if (prep->copy_count) {
      ++stats.copy_batches;
      TransitionShared(list, D3D12_RESOURCE_STATE_COPY_DEST);
      for (uint32_t i = 0; i < prep->copy_count; ++i) {
        const VertexCopy& c = prep->copies[i];
        list->CopyBufferRegion(shared.Get(), c.dst, c.src, c.src_offset, c.size);
      }
    }
    if (prep->audit_count) AuditDraw(list, *prep);
    if (!prep->upload_ok) {
      ++stats.failed;
      return;
    }
  }
  const RegisterFile& regs = *reinterpret_cast<const RegisterFile*>(s.regs);
  const xenos::PrimitiveType prim = xenos::PrimitiveType(s.primitive);

  // Shaders.
  if (!s.vs.ucode || !s.vs.dwords) {
    ++stats.skipped;
    return;
  }
  DxbcShader* vs = prep ? prep->vs : LoadShader(vs_memo, vs_last, s.vs, xenos::ShaderType::kVertex);
  DxbcShader* ps = prep ? prep->ps : LoadShader(ps_memo, ps_last, s.ps, xenos::ShaderType::kPixel);
  if (!vs) {
    ++stats.failed;
    return;
  }
  DxbcShader* const vs_generic = vs;
  DxbcShader* const ps_generic = ps;
  if (bool_specialize) {
    const uint32_t* bools = &regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031];
    vs = vs_bools.Get(vs, bools);
    if (!ps || !replacer.Has(ps->ucode_data_hash())) ps = ps_bools.Get(ps, bools);
  }

  // SelectModifications reads only prim_type; the index count would defeat mods_memo.
  reg::VGT_DRAW_INITIATOR initiator;
  initiator.value = 0;
  initiator.prim_type = prim;

  ShaderDrawRegs sdr;
  sdr.sq_program_cntl = regs[XE_GPU_REG_SQ_PROGRAM_CNTL];
  sdr.sq_context_misc = regs[XE_GPU_REG_SQ_CONTEXT_MISC];
  sdr.sq_interpolator_cntl = regs[XE_GPU_REG_SQ_INTERPOLATOR_CNTL];
  sdr.pa_cl_clip_cntl = regs[XE_GPU_REG_PA_CL_CLIP_CNTL];
  sdr.vgt_draw_initiator = initiator.value;
  sdr.vgt_output_path_cntl = regs[XE_GPU_REG_VGT_OUTPUT_PATH_CNTL];
  sdr.pa_su_sc_mode_cntl = regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL];
  sdr.rb_modecontrol = regs[XE_GPU_REG_RB_MODECONTROL];
  sdr.rb_surface_info = regs[XE_GPU_REG_RB_SURFACE_INFO];
  sdr.rb_colorcontrol = regs[XE_GPU_REG_RB_COLORCONTROL];
  sdr.rb_color_mask = regs[XE_GPU_REG_RB_COLOR_MASK];
  for (uint32_t i = 0; i < 4; ++i) sdr.rb_color_info[i] = regs[kColorInfoRegs[i]];
  if (mods_memo.vs == vs && mods_memo.ps == ps &&
      std::memcmp(&mods_memo.sdr, &sdr, sizeof(sdr)) == 0) {
    ++diag.mods_hits;
  } else {
    mods_memo.vs = vs;
    mods_memo.ps = ps;
    mods_memo.sdr = sdr;
    mods_memo.mods = SelectModifications(shaders.translator(), *vs, ps, sdr);
    mods_memo.ps_textures = 0;
    if (mods_memo.mods.pixel_shader_used)
      for (const auto& tb : ps->texture_bindings())
        mods_memo.ps_textures |= 1u << tb.fetch_constant;
  }
  ShaderModifications mods = mods_memo.mods;
  const reg::RB_DEPTHCONTROL depth_control = GetNormalizedDepthControl(regs);
  // The upscaler's scene: the draws that write depth write their motion too.
  const Upscaler* const scene_upscaler =
      Upscaler::IsScene(sdr.rb_surface_info) ? r.upscaler() : nullptr;
  bool motion = mods.pixel_shader_used && depth_control.z_enable &&
                depth_control.z_write_enable && scene_upscaler;
  // Local change (pgr4 project): only the scene's own draws, across its whole width, as the
  // jitter (FillSystemConstants): the rear-view mirror renders into a corner of the same surface
  // with its own camera, and its motion there would be the main view's in the upscaler's and
  // frame generation's inputs (ghosts of what the mirror shows, flickering in generated frames).
  if (motion) {
    ViewportInfo motion_vp;
    HostViewportInfo(regs, ps && ps->writes_depth(), motion_vp);
    motion = Upscaler::IsSceneWidth(int32_t(motion_vp.xy_offset[0]),
                                    int32_t(motion_vp.xy_offset[0] + motion_vp.xy_extent[0]));
  }
  if (motion) mods = WithMotionVectors(mods);
  const uint64_t generic_pixel_mod = mods.pixel;
  mods.pixel = SpecializePixelTextures(mods.pixel, s.fetch(0), mods_memo.ps_textures,
                                       signed_is_unsigned_formats);

  if (!mods.draw) {
    ++stats.skipped;
    return;
  }
  if (!mods.pixel_shader_used) ps = nullptr;
  if (vs->memexport_eM_written() || (ps && ps->memexport_eM_written())) {
    PGR4_WARN_ONCE("PGR4 native draw: memexport unsupported (draw skipped)");
    ++stats.unsupported;
    return;
  }
  const bool tess = xenos::IsMajorModeExplicit(initiator.major_mode, initiator.prim_type) &&
                    regs.Get<reg::VGT_OUTPUT_PATH_CNTL>().path_select ==
                        xenos::VGTOutputPath::kTessellationEnable;
  if (tess) {
    PGR4_WARN_ONCE("PGR4 native draw: tessellation unsupported");
    ++stats.unsupported;
    return;
  }
  profile::Mark(profile::kShaders);
  const bool polygonal = rex::graphics::draw_util::IsPrimitivePolygonal(false, prim);
  const bool rasterization = IsRasterizationPotentiallyDone(regs, polygonal);

  // Host primitive (the plugin's primitive processor on D3D12: points, rectangles and quads go
  // through a geometry shader, triangle fans through converted indices).
  D3D_PRIMITIVE_TOPOLOGY topology;
  switch (prim) {
    case xenos::PrimitiveType::kPointList:
      topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
      break;
    case xenos::PrimitiveType::kLineList:
      topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
      break;
    case xenos::PrimitiveType::kLineStrip:
      topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
      break;
    case xenos::PrimitiveType::kTriangleList:
    case xenos::PrimitiveType::kRectangleList:
      topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      break;
    case xenos::PrimitiveType::kTriangleStrip:
      topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
      break;
    case xenos::PrimitiveType::kQuadList:
      topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ;
      break;
    case xenos::PrimitiveType::kTriangleFan:
      // As a triangle list with host indices (v1, v2, v0), (v2, v3, v0)... like the plugin's
      // primitive processor; guest indices are copied raw (the VS still swaps them).
      topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      break;
    default:
      PGR4_WARN_ONCE("PGR4 native draw: primitive type {} unsupported", s.primitive);
      ++stats.unsupported;
      return;
  }
  uint32_t count = s.count;
  if (!count) {
    ++stats.skipped;
    return;
  }

  // Index buffer (kGuestDMA) and primitive reset.
  xenos::Endian index_endian = xenos::Endian::kNone;
  uint8_t strip_cut = 0;
  if (s.indexed) {
    index_endian = xenos::Endian(s.index_endian);
    if (!s.index_32bit && index_endian != xenos::Endian::kNone &&
        index_endian != xenos::Endian::k8in16)
      index_endian =
          index_endian == xenos::Endian::k8in32 ? xenos::Endian::k8in16 : xenos::Endian::kNone;
    if (regs.Get<reg::PA_SU_SC_MODE_CNTL>().multi_prim_ib_ena &&
        (prim == xenos::PrimitiveType::kLineStrip ||
         prim == xenos::PrimitiveType::kTriangleStrip)) {
      uint32_t reset =
          xenos::GpuSwap(regs.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx, index_endian);
      if (!s.index_32bit) {
        if (reset <= 0xFFFF) {
          if (reset != 0xFFFF) {
            PGR4_WARN_ONCE("PGR4 native draw: primitive reset index {:X} unsupported", reset);
            ++stats.unsupported;
            return;
          }
          strip_cut = 1;
        }
      } else {
        // The host compares all 32 bits; the guest only the low 24. Fine when the guest uses
        // 0xFFFFFFFF (or the index never appears).
        strip_cut = 2;
      }
    }
    if (!s.index_address) {
      ++stats.failed;
      return;
    }
  }

  // Render targets.
  const uint32_t color_mask = ps ? GetNormalizedColorMask(regs, ps->writes_color_targets()) : 0;
  const bool depth_used = rasterization && (depth_control.z_enable || depth_control.stencil_enable);
  uint32_t rt_mask = 0;
  for (uint32_t i = 0; i < 4; ++i)
    if ((color_mask >> (4 * i)) & 0xF) rt_mask |= 1u << i;
  if (!rt_mask && !depth_used) {
    ++stats.skipped;  // nothing to write or test, so an occlusion query does not count it
    return;
  }
  Scissor scissor;
  HostScissor(regs, scissor);
  // Local change (pgr4 project): a frame that is not 16:9 (21:9, 32:9, 16:10) shows the HUD and
  // the menus as a 16:9 screen did, in its central 16:9 (columns of a wider frame, rows of a taller
  // one), the scene and the menus' full-screen backgrounds over the whole frame. In the interface's
  // phase (ui_phase), every draw on the final image (pitch 1280, not the scene's surface) is made
  // there: its viewport and scissor scaled about the frame's center, x by height / 720 (wider),
  // y by 720 / height (taller), whatever its shader (panels, text, the menus' 3D track outlines, the
  // videos), and the game's elements waiting just off a 16:9 screen stay out of sight. The
  // translucent panels are fitted by their c45 instead (IsScreenSampledHud).
  const bool hud_full_width = FullWidthBackground(regs, vs, membase, video_texture);
  uint32_t ui_fit = 0;  // 0: as the game drew it; 1: by the viewport; 2: by c45 (FitHudOrtho)
  if (Upscaler::SceneHeight() != 720 && REXCVAR_GET(pgr4_native_fit_hud)) {
    const uint32_t si = regs[XE_GPU_REG_RB_SURFACE_INFO];
    if (Upscaler::IsScene(si)) {
      ui_phase = ui_hud_seen = ui_armed = false;
    } else if ((si & 0x3FFF) == 1280) {
      // A full-frame pass after a HUD draw: the tone map, the interface not begun yet.
      if (prim == xenos::PrimitiveType::kRectangleList && ui_hud_seen && scissor.offset[0] == 0 &&
          scissor.extent[0] >= 1280) {
        ui_armed = ui_rect_after_hud = true;
        ui_phase = false;
      }
      if (!ui_phase && ReadsHudOrtho(regs, vs)) {
        ui_hud_seen = true;
        if (!ui_wait_rect || ui_armed) ui_phase = true;
      }
      // A rectangle list is a full-screen pass (the race's tone map composes the scene after its
      // HUD has begun on another part of EDRAM): never fitted. The interface draws quads,
      // triangles and strips.
      // A draw in the world (its vertex shader's c4-c7 a perspective camera: the race's sprites
      // over the scene) stays over the whole frame, as the scene does. The menus' track outlines
      // read an orthographic c4-c7, which CameraFromClipRows does not take.
      bool in_world = false;
      if (vs) {
        const Shader::ConstantRegisterMap& map = vs->constant_register_map();
        if (((map.float_bitmap[0] >> 4) & 0xF) == 0xF) {
          float rows[16];
          for (uint32_t k = 0; k < 16; ++k)
            rows[k] = regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + 4 * 4 + k);
          FfxCamera camera;
          in_world = CameraFromClipRows(rows, camera);
        }
      }
      if (ui_phase && !hud_full_width && !in_world &&
          prim != xenos::PrimitiveType::kRectangleList) {
        const bool c45 = IsScreenSampledHud(vs) ? REXCVAR_GET(pgr4_native_fit_glass_c45)
                                                : !REXCVAR_GET(pgr4_native_fit_ui_viewport);
        ui_fit = c45 ? 2 : 1;
      }
    }
  }
  // The central 16:9: x scaled about 640 (a wider frame), y about the frame's middle (a taller).
  const float ui_h = float(Upscaler::SceneHeight());
  const float ui_kx = std::min(1.0f, ui_h / 720.0f), ui_ky = std::min(1.0f, 720.0f / ui_h);
  const auto ui_fit_span = [](uint32_t& offset, uint32_t& extent, float center, float k,
                              uint32_t min_extent) {
    const auto at = [center, k](uint32_t v) {
      return uint32_t(std::max(0L, std::lround(center + (float(v) - center) * k)));
    };
    const uint32_t a = at(offset), b = at(offset + extent);
    offset = a;
    extent = b > a ? b - a : min_extent;
  };
  if (ui_fit) {
    ui_fit_span(scissor.offset[0], scissor.extent[0], 640.0f, ui_kx, 0);
    ui_fit_span(scissor.offset[1], scissor.extent[1], ui_h * 0.5f, ui_ky, 0);
  }
  // Local change (pgr4 project): the videos' Bink frame (its YUV-to-RGB pixel shader) is drawn
  // 1280x720 whole for the resolve that copies it into the texture the menus show; with a frame
  // shorter than 720 (pgr4_hooks.cpp) its scissor came at the render height, the picture's bottom
  // rows missing from the texture.
  if (ps && ps->ucode_data_hash() == 0x0EB90A9B6A606BCCull) {
    bink_drawn = true;  // its resolve gives the video's texture (LogResolve)
    if (scissor.offset[1] == 0 && Upscaler::SceneHeight() < 720 &&
        scissor.extent[1] == Upscaler::SceneHeight())
      scissor.extent[1] = 720;
  }
  ViewportInfo vp;
  HostViewportInfo(regs, ps && ps->writes_depth(), vp);
  // Local change (pgr4 project): the interface's draws in the central 16:9 (ui_fit above); a
  // viewport in pixels (clipping off) is left as it is.
  if (ui_fit == 1 && !regs.Get<reg::PA_CL_CLIP_CNTL>().clip_disable) {
    ui_fit_span(vp.xy_offset[0], vp.xy_extent[0], 640.0f, ui_kx, 1);
    ui_fit_span(vp.xy_offset[1], vp.xy_extent[1], ui_h * 0.5f, ui_ky, 1);
  }
  const uint32_t height = std::clamp(scissor.offset[1] + scissor.extent[1], 1u, 2048u);
  profile::Mark(profile::kSetup);
  // Local change (pgr4 project): the scene before the frame's first water draw, for its
  // reflections (scene_copy.h), and its views in the bindless table once made.
  bool water_draw = false;
  // Only on the scene's surface: the game also draws water into smaller targets (its
  // reflections' maps), whose copy the scene's water would read.
  if (ps && scene_copy_ready && EnhancedReflections() && Upscaler::IsScene(sdr.rb_surface_info) &&
      IsWaterShader(ps->ucode_data_hash())) {
    if (scene_copy.Take(list, r.render_targets(), SurfaceRegsFromRegisters(s.regs), height,
                        frame_count))
      ResetListState();
    if (scene_copy.taken(frame_count)) {
      if (scene_views_generation != scene_copy.generation()) {
        scene_views_generation = scene_copy.generation();
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        sd.Texture2DArray.MipLevels = 1;
        sd.Texture2DArray.ArraySize = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE h = view_cpu;
        h.ptr += size_t(kViewTableStart + kSceneColor) * view_stride;
        sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        sd.Texture2DArray.MipLevels = SceneCopy::kColorLevels;
        device->CreateShaderResourceView(scene_copy.color(), &sd, h);
        h = view_cpu;
        h.ptr += size_t(kViewTableStart + kSceneDepth) * view_stride;
        sd.Format = DXGI_FORMAT_R32_FLOAT;
        sd.Texture2DArray.MipLevels = 1;
        device->CreateShaderResourceView(scene_copy.depth(), &sd, h);
      }
      water_draw = true;
    }
  }
  // ... and the wet roads: the previous frame's copy at the scene's end (OnSceneEnd), with its
  // views in the bindless table once made.
  const History* road_history = nullptr;
  uint32_t road_slot = 0;
  if (ps && scene_copy_ready && EnhancedReflections() && Upscaler::IsScene(sdr.rb_surface_info) &&
      IsRoadReflectShader(ps->ucode_data_hash())) {
    road_seen = true;
    History& h = history[0];
    if (h.valid && h.copy.color()) {
      road_slot = 0;
      if (h.views_generation != h.copy.generation()) {
        h.views_generation = h.copy.generation();
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        sd.Texture2DArray.MipLevels = 1;
        sd.Texture2DArray.ArraySize = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE c = view_cpu;
        c.ptr += size_t(kViewTableStart + kHistoryColor + road_slot) * view_stride;
        sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        sd.Texture2DArray.MipLevels = SceneCopy::kColorLevels;
        device->CreateShaderResourceView(h.copy.color(), &sd, c);
        c = view_cpu;
        c.ptr += size_t(kViewTableStart + kHistoryDepth + road_slot) * view_stride;
        sd.Format = DXGI_FORMAT_R32_FLOAT;
        sd.Texture2DArray.MipLevels = 1;
        device->CreateShaderResourceView(h.copy.depth(), &sd, c);
        c = view_cpu;
        c.ptr += size_t(kViewTableStart + kHistoryAccum) * view_stride;
        sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        device->CreateShaderResourceView(h.copy.accum(), &sd, c);
      }
      road_history = &h;
    }
  }
  BoundTargets targets;
  if (!r.render_targets().Bind(list, SurfaceRegsFromRegisters(s.regs), rt_mask, depth_used, height,
                               &targets)) {
    PGR4_WARN_ONCE("PGR4 native draw: RenderTargets::Bind failed (surface {:08X})",
                   regs[XE_GPU_REG_RB_SURFACE_INFO]);
    ++stats.failed;
    return;
  }
  profile::Mark(profile::kRtBind);

  // Translations.
  DxbcShader::Translation* vst = Translated(*vs, mods.vertex);
  DxbcShader::Translation* pst = ps ? Translated(*ps, mods.pixel) : nullptr;
  if (!vst || (ps && !pst)) {
    PGR4_WARN_ONCE("PGR4 native draw: shader translation failed");
    ++stats.failed;
    return;
  }
  profile::Mark(profile::kTranslate);

  const auto sc = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  PipelineKey key = BuildPipelineKey(regs, prim, mods, vst, pst, strip_cut, rasterization,
                                     polygonal, sc, depth_control, color_mask, targets);
  // Local change (pgr4 project): the sky (psSky, psSkyFog) without its depth: a dome at a finite
  // distance, it stood in the depth like any object, and the wet roads' reflections took it for
  // one (grey sheets over the road). Drawn behind everything, its depth hides nothing.
  if (ps_generic && IsSkyShader(ps_generic->ucode_data_hash()) && !OriginalLook())
    key.depth_write = 0;
  if (ps_generic && ps_generic->ucode_data_hash() == kRainShader && vs_generic) [[unlikely]] {
    RainDraw& d = rain_draws[vs_generic->ucode_data_hash()];
    ++d.draws;
    d.vertices += count;
  }
  // Local change (pgr4 project): at low speed the game draws its rain with another vertex shader: one draw,
  // some 2 100 vertices in the open (a stopped car at the start of a race), fewer under a bridge (1 680
  // stopped under one, 40 to 1 040 passing). Measured in a storm on 06/11/2026: to check in other rains.
  if (ps_generic && ps_generic->ucode_data_hash() == kRainShader && vs_generic &&
      vs_generic->ucode_data_hash() == kRainSlowVs && count >= kRainSlowVertices)
    particles_drawn = true;
  if (ps_generic && ps_generic->ucode_data_hash() == kRainShader && vs_generic &&
      vs_generic->ucode_data_hash() == kRainScriptVs) {
    rain_script[0] = regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + 209 * 4);
    rain_script[1] = regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + 210 * 4);
    if (rain_script[1] > 0.0f) {
      rain[0] = rain_script[0];
      rain[1] = rain_script[1];
    }
    rain_script_seen = clock_total;
    particles_drawn = true;
  }
  if (ps_generic && ps_generic->ucode_data_hash() == kRainShader && vs_generic &&
      vs_generic->ucode_data_hash() == kRainDropsVs) {
    rain[0] = regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + 209 * 4);
    rain[1] = regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + 210 * 4);
    particles_drawn = true;
  }
  if (ps_generic && IsRoadReflectShader(ps_generic->ucode_data_hash())) road_drawn = true;
  // Local change (pgr4 project): the scene's color before its transparents, for the upscaler's
  // reactive mask (Upscaler::SnapshotOpaque): at the first blended draw without depth writes
  // after a draw that wrote depth.
  if (scene_upscaler && targets.color[0] && key.rt[0].mask) {
    Upscaler* const upscaler = r.upscaler();
    if (depth_control.z_enable && depth_control.z_write_enable) {
      if (opaque_taken) upscaler->DropOpaque();
      opaque_taken = false;
    } else if (key.rt[0].blend && !opaque_taken && upscaler->wants_opaque()) {
      upscaler->SnapshotOpaque(list, targets.color[0]);
      opaque_taken = true;
    }
  }
  const uint32_t draw_index = frame_draws++;
  bool debug_skip = (debug_max_draws && draw_index >= debug_max_draws) ||
                    (draw_index >= debug_skip_from && draw_index < debug_skip_to);
  uint32_t snap_now = 0;
  if (debug_hash_count) {
    const uint64_t vs_hash = vs->ucode_data_hash(), ps_hash = ps ? ps->ucode_data_hash() : 0;
    uint32_t listed = 0;
    for (uint32_t i = 0; i < debug_hash_count; ++i)
      if (debug_hashes[i] == vs_hash || (ps && debug_hashes[i] == ps_hash)) listed |= 1u << i;
    if (debug_mask & kDebugSnapshots)
      snap_now = listed & snap_pending;
    else
      debug_skip |= bool(listed) != bool(debug_mask & kDebugOnlyListed);
  } else if ((snap_pending >> 31) && draw_index == debug_skip_from) {
    snap_now = 1u << 31;
  }
  if (debug_skip) {
    ++stats.skipped;
    return;
  }
  if (draw_log) LogDraw(s, regs, draw_index, count, *vs, ps, mods, key, vp, scissor);
  // Debug: the start line's checker (psSceneDiffuse 1466A57FDA1A2B11, a 4-vertex strip): its
  // pixel constants c0-c31 and fetch 3/14 each frame, while pgr4_native_debug_checker_frames > 0.
  if (ps && ps->ucode_data_hash() == 0x1466A57FDA1A2B11ull && count == 4) [[unlikely]] {
    if (const int32_t left = REXCVAR_GET(pgr4_native_debug_checker_frames); left > 0) {
      REXCVAR_SET(pgr4_native_debug_checker_frames, left - 1);
      std::string m;
      for (uint32_t c = 0; c < 32; ++c) {
        float v[4];
        for (uint32_t k = 0; k < 4; ++k)
          v[k] = regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_256_X + c * 4 + k);
        if (v[0] != 0.0f || v[1] != 0.0f || v[2] != 0.0f || v[3] != 0.0f)
          m += fmt::format(" {}={:.5g},{:.5g},{:.5g},{:.5g}", c, v[0], v[1], v[2], v[3]);
      }
      for (uint32_t c : {48u, 49u, 50u, 59u}) {
        m += fmt::format(" vc{}=", c);
        for (uint32_t k = 0; k < 4; ++k)
          m += fmt::format("{:.7g},", regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + c * 4 + k));
      }
      {
        TextureBinding tb;
        if (r.textures().Get(s.fetch(14), list, r.frame_fence(), &tb,
                             prep ? prep->captured : TextureCache::kCapturedNow))
          m += fmt::format(" tf14content={:016X} hd={} stale={} aliased={} uploads={} res={}", tb.content, tb.hd, tb.stale, tb.aliased, r.textures().stats().uploads,
                           reinterpret_cast<uintptr_t>(tb.resource));
      }
      REXLOG_INFO("PGR4 native checker: frame {} draw {} tf3 {:08X} tf14 {:08X} pc{}",
                  r.frame_index(), draw_index, s.fetch(3)[1], s.fetch(14)[1], m);
    }
  }
  // Local change (pgr4 project), debug: every draw on the cars' cube's surface while the cube is
  // logged (its pixel shader, count, primitive), to find a draw only some frames make.
  if (regs[XE_GPU_REG_RB_SURFACE_INFO] == 0x04020118u && CubeLogActive()) [[unlikely]]
    REXLOG_INFO("PGR4 native cube surface draw: frame {} draw {} ps {:016X} vs {:016X} n={} prim={}",
                r.frame_index(), draw_index, ps ? ps->ucode_data_hash() : 0,
                vs->ucode_data_hash(), count, uint32_t(s.primitive));
  // Local change (pgr4 project), debug: the sky drawn into the cars' surroundings cube (psSky on
  // its surface), while the cube is logged (RequestCubeLog): what may differ between its frames.
  if (ps && ps->ucode_data_hash() == 0x25E956077772CC47ull &&
      regs[XE_GPU_REG_RB_SURFACE_INFO] == 0x04020118u && CubeLogActive()) [[unlikely]] {
    const uint32_t* b = &regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031];
    const uint32_t* f = s.fetch(0);
    // Its view-projection (c28-c31: the face's camera) and c48, as the draw log shows them.
    std::string m;
    for (uint32_t c = 28; c <= 31; ++c)
      for (uint32_t k = 0; k < 4; ++k)
        m += fmt::format(" {:.6g}", regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + c * 4 + k));
    // ... and the pixel constants it reads (c12, c97 in the draw log), every one hashed.
    for (uint32_t c : {12u, 97u})
      for (uint32_t k = 0; k < 4; ++k)
        m += fmt::format(" p{}={:.6g}", c,
                         regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_256_X + c * 4 + k));
    m += fmt::format(" pall={:016X}",
                     XXH3_64bits(&regs[XE_GPU_REG_SHADER_CONSTANT_256_X], 256 * 16));
    // The guest memory the draw reads as it is now: its vertices (fetch 95) and indices, hashed
    // (the game rewriting them while the frame's copy is taken would show as a change).
    const xenos::xe_gpu_vertex_fetch_t vf = regs.GetVertexFetch(95);
    const uint32_t vfa = vf.address << 2, vfs = vf.size << 2;
    m += fmt::format(" vf95={:08X}+{:X} vh={:016X}", vfa, vfs,
                     vfs && size_t(vfa) + vfs <= 0x20000000 ? XXH3_64bits(membase + vfa, vfs) : 0);
    if (s.indexed)
      m += fmt::format(" ih={:016X}", XXH3_64bits(membase + s.index_address, size_t(count) * 4));
    REXLOG_INFO("PGR4 native cube sky: frame {} draw {} n={} mods {:X}/{:X} bools {:08X} fetch0 "
                "{:08X} {:08X} c28-31{} c48 {:.6g} {:.6g} {:.6g} {:.6g}",
                r.frame_index(), draw_index, count, mods.vertex, mods.pixel, b[0], f[0], f[1], m,
                regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + 48 * 4),
                regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + 48 * 4 + 1),
                regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + 48 * 4 + 2),
                regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + 48 * 4 + 3));
  }
  // Local change: the pixel shader before its bool specialization, the hash a replacement file
  // (shader_replace.h) takes to cover every specialization.
  if (draw_log && ps && ps_generic && ps_generic != ps)
    std::fprintf(draw_log, "  generic ps=%016llX\n",
                 (unsigned long long)ps_generic->ucode_data_hash());
  // With dynamic depth bias the polygon offset is not part of the pipeline.
  float depth_bias = float(key.depth_bias);
  const float slope_bias = key.slope_scaled_depth_bias;
  // Local change (pgr4 project): the light flares' test sprites (inside an occlusion query, depth
  // not written) sit inside the lamp's housing and rely on their polygon offset to clear it. With
  // float24 depth, Canary's GetD3D10IntegerPolygonOffset gives the offset its [0.5, 1) size in
  // depth units, but the hardware's is absolute: at a car's depth (reversed Z, about 0.01) it is
  // 64 times larger, and without it the headlights' tests failed nearly every frame (lamps unlit
  // on the car, flares blinking or gone, in both renderers). The game projects these sprites on
  // the CPU (their VS passes the fetched position through), so the absolute offset is set from
  // their nearest vertex: DepthBias * 2^(exponent - 23) on the host's float32 depth.
  if (depth_bias != 0.0f && !key.depth_write && r.occlusion() && r.occlusion()->counting() &&
      regs.Get<reg::RB_DEPTH_INFO>().depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
    const float z = FetchedMaxDepth(s, regs, *vs, count);
    if (z > 0.0f && z <= 1.0f)
      depth_bias = std::ldexp(depth_bias, -1 - std::ilogb(z)) *
                   float(REXCVAR_GET(pgr4_native_occlusion_depth_bias_scale));
  }
  // Local change (pgr4 project), test: the scene's draws that test depth without writing it and
  // have no polygon offset (the painted lines lying in the road's plane?) pulled toward the camera
  // by pgr4_native_coplanar_bias depth units, against their Z fighting with what is under them.
  else if (depth_bias == 0.0f && dynamic_depth_bias && depth_control.z_enable &&
           !depth_control.z_write_enable && Upscaler::IsScene(sdr.rb_surface_info)) {
    if (const double bias = REXCVAR_GET(pgr4_native_coplanar_bias); bias > 0.0) {
      const auto func = depth_control.zfunc;
      const bool reversed = func == xenos::CompareFunction::kGreater ||
                            func == xenos::CompareFunction::kGreaterEqual;
      depth_bias = float(reversed ? bias : -bias);
      static std::vector<uint64_t> logged;
      const uint64_t h = ps ? ps->ucode_data_hash() : 0;
      if (logged.size() < 48 && std::find(logged.begin(), logged.end(), h) == logged.end()) {
        logged.push_back(h);
        REXLOG_INFO("PGR4 native coplanar bias: ps {:016X} zfunc {} blend {}", h, uint32_t(func),
                    uint32_t(key.rt[0].blend));
      }
    }
  }
  if (dynamic_depth_bias) {
    key.depth_bias = 0;
    key.slope_scaled_depth_bias = 0;
  }
  const DxbcShader* const vs_selected = vs;
  const DxbcShader* const ps_selected = ps;
  ID3D12PipelineState* const pso = SelectPipeline(key, *vst, pst, vs, ps, vs_generic, ps_generic,
                                                  mods.pixel, generic_pixel_mod, sdr, prim);
  if (draw_log)
    std::fprintf(draw_log, "  pso=%s\n",
                 !pso                ? "pending"
                 : vs != vs_selected ? "generic"
                                     : "ready");
  if (!pso) return;
  profile::Mark(profile::kPipeline);

  // Textures and samplers (bindless descriptor indices, b4).
  const uint32_t used_textures = vs->GetUsedTextureMaskAfterTranslation() |
                                 (ps ? ps->GetUsedTextureMaskAfterTranslation() : 0);
  uint32_t desc_vs[64], desc_ps[64];
  uint8_t signs[32] = {};
  uint32_t resolution_scaled = 0;
  const TextureBindContext bind = {s,
                                   regs,
                                   list,
                                   r.textures(),
                                   r.frame_fence(),
                                   prep ? prep->captured : TextureCache::kCapturedNow,
                                   signs,
                                   scene_upscaler ? scene_upscaler->mip_bias_quarters()
                                                  : Upscaler::IsScene(sdr.rb_surface_info)
                                                      ? MsaaSoftenQuarters() << 4
                                                      : 0,
                                   &resolution_scaled};
  const uint64_t downsample_passes = r.resolver().compute_passes();
  const uint32_t n_desc_vs = BindTextures(bind, *vs, desc_vs);
  const uint32_t n_desc_ps = ps ? BindTextures(bind, *ps, desc_ps) : 1;
  if (!ps) desc_ps[0] = 0;
  // Local change (pgr4 project): a night windscreen's wiper mask, which its shader does not bind.
  uint32_t night_wiper_mask = 0;
  const bool night_windscreen = ps && IsNightWindscreen(ps->ucode_data_hash());
  if (night_windscreen) {
    TextureBinding mask;
    if (r.textures().Get(s.fetch(6), list, r.frame_fence(), &mask,
                         prep ? prep->captured : TextureCache::kCapturedNow) &&
        mask.resource) {
      DxbcShader::TextureBinding binding{};
      binding.fetch_constant = 6;
      binding.dimension = xenos::FetchOpDimension::k2D;
      night_wiper_mask = TextureDescriptor(mask, binding);
    }
  }
  // Local change (pgr4 project), debug: what the sky of the cars' cube's faces really draws with
  // (pipeline, shaders, descriptors) while the cube is logged, beside the cube probe (resolve.cpp).
  if (ps_selected && ps_selected->ucode_data_hash() == 0x25E956077772CC47ull &&
      regs[XE_GPU_REG_RB_SURFACE_INFO] == 0x04020118u && CubeLogActive()) [[unlikely]] {
    std::string d;
    for (uint32_t i = 0; i < std::min(n_desc_ps, 8u); ++i) d += fmt::format(" {}", desc_ps[i]);
    // Its viewport's conversion to the host's NDC (the zenith of the sky's dome falls on the far
    // plane: a change of z there draws it or not) and the clip registers.
    d += fmt::format(" ndc {:.9g} {:.9g} {:.9g} / {:.9g} {:.9g} {:.9g} z {:.9g}-{:.9g} clip {:08X} "
                     "vte {:08X} depth_clip {}",
                     vp.ndc_scale[0], vp.ndc_scale[1], vp.ndc_scale[2], vp.ndc_offset[0],
                     vp.ndc_offset[1], vp.ndc_offset[2], vp.z_min, vp.z_max,
                     regs[XE_GPU_REG_PA_CL_CLIP_CNTL], regs[XE_GPU_REG_PA_CL_VTE_CNTL],
                     key.depth_clip);
    REXLOG_INFO("PGR4 native cube sky draw: frame {} pso {} vs {} ps {} upscaler {} scaled {:X} "
                "desc{}",
                r.frame_index(), static_cast<const void*>(pso),
                vs != vs_selected ? "generic" : "own", ps != ps_selected ? "generic" : "own",
                scene_upscaler ? 1 : 0, resolution_scaled, d);
    // ... and its indices and vertices as the GPU's copy of guest memory holds them (12922000 to
    // the end of its vertices), read back once a frame and hashed (WriteGeometryProbes).
    if (geometry_probed != r.frame_index()) {
      geometry_probed = r.frame_index();
      constexpr uint64_t kFrom = 0x12922000, kSize = 0x2000 + 0x11940;
      const uint32_t slot = uint32_t(r.frame_index() % 8);
      if (!geometry_probe[slot].fence) {
        if (!geometry_buffer)
          CreateBuffer(device, D3D12_HEAP_TYPE_READBACK, 8 * kSize, D3D12_RESOURCE_STATE_COPY_DEST,
                       "geometry probe", &geometry_buffer);
        if (geometry_buffer) {
          const D3D12_RESOURCE_STATES before = shared_state;
          TransitionShared(list, D3D12_RESOURCE_STATE_COPY_SOURCE);
          list->CopyBufferRegion(geometry_buffer.Get(), slot * kSize, shared.Get(), kFrom, kSize);
          TransitionShared(list, before);
          geometry_probe[slot] = {r.frame_fence(), r.frame_index()};
        }
      }
    }
  }
  // A guest-size downsample of a scaled resolve (Resolver::GuestSize, local change) set its own
  // descriptor heap and compute pipeline on the list: bind the draw's root state again.
  if (r.resolver().compute_passes() != downsample_passes) ResetListState();
  profile::Mark(profile::kTextures);

  // Local change (pgr4 project): the sun of the scene's lighting (PS c6 its direction, c7 its
  // color), read from the scene's draws whose pixel shader reads both, for the replacement
  // shaders (shader_replace.h: pgr4_sun_direction, pgr4_sun_color after the system constants),
  // whose game shader has no sun (the cars' paint).
  if (ps && Upscaler::IsScene(sdr.rb_surface_info)) {
    const Shader::ConstantRegisterMap& pmap = ps->constant_register_map();
    if (!pmap.float_dynamic_addressing && (pmap.float_bitmap[0] & (uint64_t(3) << 6)) ==
                                              (uint64_t(3) << 6)) {
      float dir[3], color[3];
      for (int k = 0; k < 3; ++k) {
        dir[k] = regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_256_X + 6 * 4 + k);
        color[k] = regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_256_X + 7 * 4 + k);
      }
      const float length = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
      const float brightness = color[0] + color[1] + color[2];
      if (length > 0.95f && length < 1.05f && brightness > 0.01f && brightness < 30.0f) {
        for (int k = 0; k < 3; ++k) {
          sun.direction[k] = dir[k] / length;
          sun.color[k] = color[k];
        }
        sun.color[3] = 1.0f;  // known
      }
    }
  }
  SystemConstantsWithSun sys_sun;
  DxbcShaderTranslator::SystemConstants& sys = sys_sun.sys;
  FillSystemConstants(s, regs, prim, polygonal, index_endian, vp, used_textures, signs, targets,
                      sys);
  std::memcpy(sys_sun.sun_direction, sun.direction, sizeof(sun.direction));
  std::memcpy(sys_sun.sun_color, sun.color, sizeof(sun.color));
  sys_sun.time[0] = clock_s;
  // By the race's weather (g_pgr4_weather_kind). Only rain has the game's rain settings: no wet road's
  // reflection otherwise (road_reflect_hooks.hlsli stops at 0), should one be drawn or an older value be left.
  const bool weather_known = g_pgr4_race_screen && g_pgr4_weather_kind >= 0;
  const bool heavy_snow = weather_known && g_pgr4_weather_kind == 6;
  const bool snow_falling = heavy_snow || (weather_known && g_pgr4_weather_kind == 4);
  // A race's script may bring rain to a record that says clear, overcast or fog: the game's drawing it tells.
  const bool script_rain = weather_known && g_pgr4_weather_kind <= 2 && clock_total - rain_script_seen < 10.0;
  const bool no_rain = weather_known && g_pgr4_weather_kind != 3 && !script_rain;
  sys_sun.time[1] = no_rain ? 0.0f : rain[0];
  sys_sun.time[2] = no_rain ? 0.0f : rain[1];
  // Local change (pgr4 project): 1 for the scene's decals (depth tested, not written, blended over
  // what is under them: the painted lines, the start line's checker) with
  // pgr4_native_decal_light_off: their replacement's cue to leave out the baked area light
  // (tf14) that vibrated on the white lines as the camera moved.
  sys_sun.time[3] = REXCVAR_GET(pgr4_native_decal_light_off) && depth_control.z_enable &&
                            !depth_control.z_write_enable && key.rt[0].blend &&
                            Upscaler::IsScene(sdr.rb_surface_info)
                        ? 1.0f
                        : 0.0f;
  // The water's: its clip transform (its VS's c4-c7, applied to the world position) and the
  // scene's copy; zero for the other draws, whose system constants then stay the same.
  std::memset(sys_sun.clip, 0, sizeof(sys_sun.clip));
  std::memset(sys_sun.scene, 0, sizeof(sys_sun.scene));
  std::memset(sys_sun.view, 0, sizeof(sys_sun.view));
  if (water_draw) {
    for (int k = 0; k < 16; ++k)
      sys_sun.clip[k] = regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + 4 * 4 + k);
    sys_sun.scene[0] = kSceneColor;
    sys_sun.scene[1] = kSceneDepth;
    sys_sun.scene[2] = scene_copy.width();
    sys_sun.scene[3] = scene_copy.height();
    // Clip to the copy's texels: the translator's NDC scale and offset, then the host viewport
    // (the scene's part of its target) at the copy's half size.
    const float hx = targets.scale.factor(0) * 0.5f, hy = targets.scale.factor(1) * 0.5f;
    const float view[12] = {float(vp.xy_offset[0]) * hx, float(vp.xy_offset[1]) * hy,
                            float(vp.xy_extent[0]) * hx, float(vp.xy_extent[1]) * hy,
                            vp.ndc_scale[0], vp.ndc_scale[1], vp.ndc_scale[2], 0.0f,
                            vp.ndc_offset[0], vp.ndc_offset[1], vp.ndc_offset[2], 0.0f};
    std::memcpy(sys_sun.view, view, sizeof(view));
  }
  std::memset(sys_sun.camera, 0, sizeof(sys_sun.camera));
  if (ps && Upscaler::IsScene(sdr.rb_surface_info) && IsRoadReflectShader(ps->ucode_data_hash())) {
    if (!drops_eye_fresh) {
      for (uint32_t k = 0; k < 3; ++k)
        drops_eye[k] = regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + 24 * 4 + k);
      drops_eye_fresh = true;
    }
  }
  sys_sun.shelter[0] = float(clock_total - shelter_began);
  sys_sun.shelter[1] = sheltered ? 0.0f : float(clock_total - shelter_ended);
  sys_sun.shelter[2] = heavy_snow ? 1.0f : 0.0f;  // drops_run.hlsli: XeHeavySnow
  sys_sun.shelter[3] = weather_known && g_pgr4_light_rain ? 1.0f : 0.0f;  // drops_run.hlsli
  // Local change (pgr4 project): the frost's own settings under frost and snow; the grime of other weathers is
  // told from it here (car_paint_hooks.hlsli XeFrostRead).
  const bool frost_weather = weather_known && g_pgr4_weather_kind >= 4;
  sys_sun.dirt[0] = float(REXCVAR_GET(pgr4_native_dirt_force));
  sys_sun.dirt[1] = frost_weather ? 1.0f : 0.0f;
  sys_sun.dirt[2] = 0.0f;
  sys_sun.dirt[3] = float(REXCVAR_GET(pgr4_native_dirt_fine));
  std::memset(sys_sun.wiper, 0, sizeof(sys_sun.wiper));
  std::memset(sys_sun.wiper_blade, 0, sizeof(sys_sun.wiper_blade));
  std::memset(sys_sun.wiper_mask, 0, sizeof(sys_sun.wiper_mask));
  if (night_windscreen && night_wiper_mask) {
    // The windscreen's world position in the day's VS times c68-c71; here from the eye (its c24), as
    // the pixel shader has the vector to the eye, and its floats keep their precision.
    auto vc = [&](uint32_t c, uint32_t k) {
      return regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + c * 4 + k);
    };
    for (uint32_t row = 0; row < 4; ++row) {
      for (uint32_t k = 0; k < 3; ++k) sys_sun.wiper[row * 4 + k] = vc(68 + row, k);
      sys_sun.wiper[row * 4 + 3] = vc(68 + row, 3) + vc(68 + row, 0) * vc(24, 0) +
                                   vc(68 + row, 1) * vc(24, 1) + vc(68 + row, 2) * vc(24, 2);
    }
    for (uint32_t k = 0; k < 4; ++k)
      sys_sun.wiper_blade[k] = regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_256_X + 214 * 4 + k);
    sys_sun.wiper_mask[0] = night_wiper_mask;
  }
  // The other draws' pgr4_camera: the drops' two scrolls and how fast the car goes, 0 to 1 (the
  // road's own, below, is read by the road's shaders only).
  sys_sun.camera[0] = drops_scroll[0];
  sys_sun.camera[1] = drops_scroll[1];
  sys_sun.camera[2] = float(std::min<double>(
      std::max<double>(drops_speed - REXCVAR_GET(pgr4_native_drops_glide_from), 0.0) / 60.0, 1.0));
  bool our_car = false;
  // The running drops' lean in a bend, where the other draws have nothing (pgr4_view[0]).
  if (!water_draw) {
    std::memcpy(sys_sun.view, drops_side, sizeof(drops_side));
    std::memcpy(sys_sun.view + 4, drops_back, sizeof(drops_back));  // pgr4_view[1]
    // The frame of the piece of car drawn (drops_run.hlsli), from what its vertex shader itself reads:
    // its centre from the eye, and its rotation in the rows' .w. Far away for the other draws.
    if (vs_generic && IsCarPaintVs(vs_generic->ucode_data_hash())) {
      auto vc = [&](uint32_t c, uint32_t k) {
        return regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + c * 4 + k);
      };
      float x[3], z[3], q[3];
      for (uint32_t k = 0; k < 3; ++k) {
        sys_sun.view[8 + k] = vc(k, 3) - vc(24, k);
        x[k] = vc(k, 0);
        z[k] = vc(k, 2);
      }
      const float lx = std::max(std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]), 1e-6f);
      const float lz = std::max(std::sqrt(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]), 1e-6f);
      for (uint32_t k = 0; k < 3; ++k) {
        x[k] /= lx;
        z[k] /= lz;
      }
      Pgr4FrameQuat(x, z, q);
      for (uint32_t k = 0; k < 3; ++k) sys_sun.view[3 + 4 * k] = q[k];
      const float dx = vc(0, 3) - g_pgr4_car_pos[0], dy = vc(1, 3) - g_pgr4_car_pos[1],
                  dz = vc(2, 3) - g_pgr4_car_pos[2];
      const float margin = 3.0f + 0.06f * g_pgr4_car_speed;
      our_car = dx * dx + dy * dy + dz * dz < margin * margin;
    } else {
      for (uint32_t k = 0; k < 3; ++k) sys_sun.view[8 + k] = 1e6f;
    }
  }
  // For the cars' shaders (pgr4_camera.w: the road's and the water's have another use): 1 it snows, 2 the
  // car the camera is on.
  sys_sun.camera[3] =
      (snow_falling && !water_draw &&
               !(ps && IsRoadReflectShader(ps->ucode_data_hash()))
           ? 1.0f
           : 0.0f) +
      (our_car ? 2.0f : 0.0f);
  if (road_seen && ps && Upscaler::IsScene(sdr.rb_surface_info) &&
      IsRoadReflectShader(ps->ucode_data_hash())) {
    // This frame's world to clip for its copy at the scene's end: the road VS's object to clip
    // (c4-c7) times the inverse of its object to world (c0-c2, affine), and the viewport mapping.
    auto vc = [&](uint32_t c, uint32_t k) {
      return regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + c * 4 + k);
    };
    const float a[3][3] = {{vc(0, 0), vc(0, 1), vc(0, 2)},
                           {vc(1, 0), vc(1, 1), vc(1, 2)},
                           {vc(2, 0), vc(2, 1), vc(2, 2)}};
    const float t[3] = {vc(0, 3), vc(1, 3), vc(2, 3)};
    float inv[3][3] = {{a[1][1] * a[2][2] - a[1][2] * a[2][1], a[0][2] * a[2][1] - a[0][1] * a[2][2],
                        a[0][1] * a[1][2] - a[0][2] * a[1][1]},
                       {a[1][2] * a[2][0] - a[1][0] * a[2][2], a[0][0] * a[2][2] - a[0][2] * a[2][0],
                        a[0][2] * a[1][0] - a[0][0] * a[1][2]},
                       {a[1][0] * a[2][1] - a[1][1] * a[2][0], a[0][1] * a[2][0] - a[0][0] * a[2][1],
                        a[0][0] * a[1][1] - a[0][1] * a[1][0]}};
    const float det = a[0][0] * inv[0][0] + a[0][1] * inv[1][0] + a[0][2] * inv[2][0];
    if (std::abs(det) > 1e-12f) {
      for (auto& row : inv)
        for (float& v : row) v /= det;
      for (uint32_t r = 0; r < 4; ++r) {
        float* out = road_clip + 4 * r;
        for (uint32_t j = 0; j < 3; ++j)
          out[j] = vc(4 + r, 0) * inv[0][j] + vc(4 + r, 1) * inv[1][j] + vc(4 + r, 2) * inv[2][j];
        out[3] = vc(4 + r, 3) - (out[0] * t[0] + out[1] * t[1] + out[2] * t[2]);
      }
      const float hx = targets.scale.factor(0) * 0.5f, hy = targets.scale.factor(1) * 0.5f;
      const float view[12] = {float(vp.xy_offset[0]) * hx, float(vp.xy_offset[1]) * hy,
                              float(vp.xy_extent[0]) * hx, float(vp.xy_extent[1]) * hy,
                              vp.ndc_scale[0], vp.ndc_scale[1], vp.ndc_scale[2], 0.0f,
                              vp.ndc_offset[0], vp.ndc_offset[1], vp.ndc_offset[2], 0.0f};
      std::memcpy(road_view, view, sizeof(view));
    }
    // The previous frame's copy, its transform and mapping, and the eye.
    if (road_history) {
      std::memcpy(sys_sun.clip, road_history->clip, sizeof(sys_sun.clip));
      std::memcpy(sys_sun.view, road_history->view, sizeof(sys_sun.view));
      sys_sun.scene[0] = kHistoryColor + road_slot;
      sys_sun.scene[1] = kHistoryDepth + road_slot;
      sys_sun.scene[2] = road_history->copy.width();
      sys_sun.scene[3] = road_history->copy.height();
      for (uint32_t k = 0; k < 3; ++k) sys_sun.camera[k] = vc(24, k);
      sys_sun.camera[3] = road_history->copy.accum() ? float(kHistoryAccum) : 0.0f;
    }
  }
  // Unnormalized fetches, texture sizes and offsets of these textures in guest texels
  // (DxbcShaderTranslator built with the draw resolution scale, shader_cache.cpp).
  sys.textures_resolution_scaled = resolution_scaled;
  profile::Mark(profile::kSysConst);

  // Float constants packed by the shaders' maps (UpdateBindings), compared and staged in place.
  PackFloats(cb_float_vs, vs, s.regs, XE_GPU_REG_SHADER_CONSTANT_000_X);
  // Local change (pgr4 project): the render height's HUD; a menu's full-screen background keeps
  // the game's width, stretched to the whole frame.
  if (ui_fit == 2) FitHudOrtho(cb_float_vs, vs, hud_full_width);
  PackFloats(cb_float_ps, ps, s.regs, XE_GPU_REG_SHADER_CONSTANT_256_X);
  profile::Mark(profile::kPack);

  // Vertex data and indices into shared memory (made by Prepare with `prep`).
  const uint32_t index_bytes = count * (s.index_32bit ? 4u : 2u);
  const bool fan = prim == xenos::PrimitiveType::kTriangleFan;
  if (!prep) {
    upload_fence = r.frame_fence();
    if (!RequestVertices(s, *vs, copies)) {
      // The copies made before the failed range go out: their pages are marked valid already.
      FlushCopies(list);
      ++stats.failed;
      return;
    }
  }
  FlushCopies(list);
  profile::Mark(profile::kVertices);

  // Fixed-function state, in the targets' host pixels. The pixel position PsParamGen gives
  // stays in host pixels: the scene's shader that reads it in the race (3F533EA17A8E6CFC, draw
  // logs of td-265) uses only its facing sign.
  const float sx = targets.scale.factor(0), sy = targets.scale.factor(1);
  D3D12_VIEWPORT viewport = {float(vp.xy_offset[0]) * sx,
                             float(vp.xy_offset[1]) * sy,
                             float(vp.xy_extent[0]) * sx,
                             float(vp.xy_extent[1]) * sy,
                             vp.z_min,
                             vp.z_max};
  const D3D12_RECT scissor_rect = targets.scale.Rect(
      {LONG(scissor.offset[0]), LONG(scissor.offset[1]),
       LONG(scissor.offset[0] + scissor.extent[0]), LONG(scissor.offset[1] + scissor.extent[1])});

  // Root state.
  if (list_dirty) {
    ID3D12DescriptorHeap* heaps[] = {view_heap.Get(), sampler_heap.Get()};
    list->SetDescriptorHeaps(2, heaps);
    list->SetGraphicsRootSignature(root_signature.Get());
    list->SetGraphicsRootDescriptorTable(kBindlessRoot_SharedMemory, view_gpu);
    list->SetGraphicsRootDescriptorTable(kBindlessRoot_SamplerHeap, sampler_gpu);
    D3D12_GPU_DESCRIPTOR_HANDLE table = view_gpu;
    table.ptr += uint64_t(kViewTableStart) * view_stride;
    list->SetGraphicsRootDescriptorTable(kBindlessRoot_ViewHeap, table);
    list_dirty = false;
  }
  static_assert(sizeof(sys_sun) <= CachedCbv::kMaxSize);
  bool ok =
      UploadCbv(cb_system, &sys_sun, sizeof(sys_sun), kBindlessRoot_SystemConstants, list) &&
      CommitCbv(cb_float_vs, kBindlessRoot_FloatConstantsVertex, list) &&
      CommitCbv(cb_float_ps, kBindlessRoot_FloatConstantsPixel, list) &&
      UploadCbv(cb_bool_loop, &regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031], (8 + 32) * 4,
                kBindlessRoot_BoolLoopConstants, list) &&
      UploadCbv(cb_fetch, &regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], 32 * 6 * 4,
                kBindlessRoot_FetchConstants, list) &&
      UploadCbv(cb_desc_vs, desc_vs, n_desc_vs * 4, kBindlessRoot_DescriptorIndicesVertex, list) &&
      UploadCbv(cb_desc_ps, desc_ps, n_desc_ps * 4, kBindlessRoot_DescriptorIndicesPixel, list);
  if (!ok) {
    ++stats.failed;
    return;
  }
  if (motion) BindPreviousConstants(s, *vs_generic, vs, list);
  profile::Mark(profile::kCbv);
  if (bound_pso != pso) {
    profile::Scope rec_pso(profile::kRecPso);  // local change
    list->SetPipelineState(pso);
    bound_pso = pso;
    // SetPipelineState loads the pipeline's depth bias, dynamic or not (DirectX-Specs
    // VulkanOn12, "initial dynamic depth bias state"). These pipelines are keyed with zero bias.
    if (dynamic_depth_bias) bound_depth_bias = bound_slope_bias = 0.0f;
  }
  if (bound_topology != topology) {
    list->IASetPrimitiveTopology(topology);
    bound_topology = topology;
  }
  if (std::memcmp(&bound_viewport, &viewport, sizeof(viewport))) {
    list->RSSetViewports(1, &viewport);
    bound_viewport = viewport;
  }
  if (std::memcmp(&bound_scissor, &scissor_rect, sizeof(scissor_rect))) {
    list->RSSetScissorRects(1, &scissor_rect);
    bound_scissor = scissor_rect;
  }
  float blend[4] = {
      regs.Get<float>(XE_GPU_REG_RB_BLEND_RED), regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
      regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE), regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA)};
  if (std::memcmp(bound_blend, blend, sizeof(blend))) {
    list->OMSetBlendFactor(blend);
    std::memcpy(bound_blend, blend, sizeof(blend));
  }
  {
    const bool bf = polygonal && depth_control.backface_enable && sc.cull_front && !sc.cull_back;
    uint32_t ref = regs.Get<reg::RB_STENCILREFMASK>(bf ? XE_GPU_REG_RB_STENCILREFMASK_BF
                                                       : XE_GPU_REG_RB_STENCILREFMASK)
                       .stencilref;
    if (ref != bound_stencil_ref) {
      list->OMSetStencilRef(ref);
      bound_stencil_ref = ref;
    }
  }
  if (dynamic_depth_bias && (depth_bias != bound_depth_bias || slope_bias != bound_slope_bias)) {
    if (list9_of != list) {
      list9.Reset();
      list->QueryInterface(IID_PPV_ARGS(&list9));
      list9_of = list;
    }
    if (list9) list9->RSSetDepthBias(depth_bias, 0.0f, slope_bias);
    bound_depth_bias = depth_bias;
    bound_slope_bias = slope_bias;
  }
  // Render targets: only the ones the pipeline declares (the guest's, null ones up to the motion
  // vectors with them), the DSV only when it is used.
  std::optional<profile::Scope> rec_rtv(std::in_place, profile::kRecRtv);  // local change
  D3D12_CPU_DESCRIPTOR_HANDLE rtv[DxbcShaderTranslator::kMotionVectorRenderTarget + 1] = {};
  uint32_t rt_count = key.num_rts;
  if (motion) {
    rt_count = DxbcShaderTranslator::kMotionVectorRenderTarget + 1;
    for (uint32_t i = 0; i < DxbcShaderTranslator::kMotionVectorRenderTarget; ++i)
      rtv[i] = targets.rtv[i];
    rtv[DxbcShaderTranslator::kMotionVectorRenderTarget] = r.upscaler()->MotionTarget(list);
  } else {
    for (uint32_t i = 0; i < key.num_rts; ++i) rtv[i] = targets.rtv[i];
  }
  D3D12_CPU_DESCRIPTOR_HANDLE dsv = {};
  if (key.dsv != DXGI_FORMAT_UNKNOWN) dsv = targets.dsv;
  if (bound_rt_count != rt_count || std::memcmp(bound_rtv, rtv, sizeof(rtv)) ||
      bound_dsv.ptr != dsv.ptr) {
    list->OMSetRenderTargets(rt_count, rt_count ? rtv : nullptr, FALSE, dsv.ptr ? &dsv : nullptr);
    std::memcpy(bound_rtv, rtv, sizeof(rtv));
    bound_dsv = dsv;
    bound_rt_count = rt_count;
  }
  rec_rtv.reset();

  uint32_t draw_count = count, first_index = 0;
  bool draw_indexed = true;
  if (fan) {
    if (count < 3) {
      ++stats.skipped;
      return;
    }
    const FanIndices f = prep ? FanIndices{prep->fan_indices, prep->fan_count, prep->fan_wide}
                              : UploadFanIndices(s, r.frame_fence());
    if (!f.count) {
      ++stats.failed;
      return;
    }
    D3D12_INDEX_BUFFER_VIEW ibv;
    ibv.BufferLocation = f.gpu;
    ibv.SizeInBytes = f.count * (f.wide ? 4 : 2);
    ibv.Format = f.wide ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    list->IASetIndexBuffer(&ibv);
    bound_ibv = ibv;
    draw_count = f.count;
  } else if (s.indexed) {
    // The whole shared memory buffer is the index buffer and the draw starts at the guest
    // address, so consecutive draws only rebind it when the index format changes.
    const uint32_t index_size = s.index_32bit ? 4u : 2u;
    const bool aligned = (s.index_address & (index_size - 1)) == 0;
    D3D12_INDEX_BUFFER_VIEW ibv;
    ibv.BufferLocation = aligned ? shared_gpu : shared_gpu + s.index_address;
    ibv.SizeInBytes = aligned ? kSharedMemorySize : index_bytes;
    ibv.Format = s.index_32bit ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    if (std::memcmp(&bound_ibv, &ibv, sizeof(ibv))) {
      list->IASetIndexBuffer(&ibv);
      bound_ibv = ibv;
    }
    first_index = aligned ? s.index_address / index_size : 0;
  } else {
    draw_indexed = false;
  }
  auto issue = [&] {
    if (draw_indexed)
      list->DrawIndexedInstanced(draw_count, 1, first_index, 0, 0);
    else
      list->DrawInstanced(draw_count, 1, 0, 0);
  };
  profile::Scope rec_draw(profile::kRecDraw);  // local change
  OcclusionQueries* const occlusion = r.occlusion();
  const uint32_t pair =
      occlusion && occlusion->counting() ? occlusion->Acquire() : OcclusionQueries::kNoPair;
  if (pair == OcclusionQueries::kNoPair) {
    issue();
  } else {
    occlusion->BeginPass(list, pair);
    issue();
    occlusion->EndPass(list, pair);
    // Without a depth target nothing is tested, and every sample passes. A generic pipeline's
    // shaders differ from the key's, which the bindings were made for, and a coverage pipeline
    // still compiling leaves the pair uncounted rather than reported fully visible.
    ID3D12PipelineState* cover = nullptr;
    auto coverage = OcclusionQueries::Coverage::kUnknown;
    if (key.dsv == DXGI_FORMAT_UNKNOWN) {
      coverage = OcclusionQueries::Coverage::kPasses;
    } else if (vs == vs_selected && ps == ps_selected) {
      cover = CoveragePipeline(key, *vst, pst);
      if (cover) coverage = OcclusionQueries::Coverage::kCounted;
    }
    occlusion->BeginCoverage(list, pair);
    if (cover) {
      list->SetPipelineState(cover);
      bound_pso = cover;
      if (dynamic_depth_bias) bound_depth_bias = bound_slope_bias = 0.0f;
      issue();
    }
    const RenderTarget* const t = targets.depth ? targets.depth : targets.color[0];
    const float guest_per_host =
        t ? float(t->edram_samples) /
                (float(t->samples) * targets.scale.factor(0) * targets.scale.factor(1))
          : 1.0f;
    occlusion->EndCoverage(list, pair, coverage, guest_per_host);
    if (draw_log) std::fprintf(draw_log, "  occ pair=%u cov=%u\n", pair, uint32_t(coverage));
  }
  ++stats.draws;
  ++diag.draws;
  if (snap_now) {
    snap_pending &= ~snap_now;
    SnapshotTargets(list, targets, draw_index);
  }
  profile::Mark(profile::kRecord);
}

void DrawRenderer::Impl::SnapshotTargets(ID3D12GraphicsCommandList* list,
                                         const BoundTargets& targets, uint32_t draw_index) {
  const std::string stem =
      (std::filesystem::path(REXCVAR_GET(pgr4_native_capture_dir)) / "snap-f").string() +
      std::to_string(r.frame_index()) + "-d" + std::to_string(draw_index);
  for (uint32_t i = 0; i < targets.color_count; ++i)
    Snapshot(list, targets.color[i], stem + "-c" + std::to_string(i));
  Snapshot(list, targets.depth, stem + "-depth");
  snap_fence = r.frame_fence();
  REXLOG_INFO("PGR4 native draw: snapshot of draw {} ({} targets) to {}-*", draw_index,
              snaps.size(), stem);
  ResetListState();  // the barriers moved the targets out of their bound states
}

void DrawRenderer::Impl::Snapshot(ID3D12GraphicsCommandList* list, RenderTarget* rt,
                                  const std::string& stem) {
  if (!rt || rt->samples != 1) return;
  const D3D12_RESOURCE_DESC desc = rt->resource->GetDesc();
  Snap s;
  s.stem = stem;
  uint64_t total = 0;
  // Subresource 0: the color target, or a depth target's depth plane.
  device->GetCopyableFootprints(&desc, 0, 1, 0, &s.layout, &s.rows, &s.row_bytes, &total);
  if (!CreateBuffer(device, D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST,
                    "snapshot readback", &s.readback))
    return;
  const D3D12_RESOURCE_STATES state = rt->state;
  RenderTargets::Transition(list, rt, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
  src.pResource = rt->resource.Get();
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src.SubresourceIndex = 0;
  dst.pResource = s.readback.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint = s.layout;
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  RenderTargets::Transition(list, rt, state);
  snaps.push_back(std::move(s));
}

void DrawRenderer::Impl::WriteSnapshots(uint64_t completed_fence) {
  if (snaps.empty() || completed_fence < snap_fence) return;
  for (Snap& s : snaps) {
    void* data = nullptr;
    const D3D12_RANGE range = {0, size_t(s.layout.Footprint.RowPitch) * s.rows};
    if (FAILED(s.readback->Map(0, &range, &data))) continue;
    if (std::FILE* f = std::fopen((s.stem + ".bin").c_str(), "wb")) {
      for (uint32_t y = 0; y < s.rows; ++y)
        std::fwrite(static_cast<const uint8_t*>(data) + size_t(y) * s.layout.Footprint.RowPitch, 1,
                    size_t(s.row_bytes), f);
      std::fclose(f);
    }
    const D3D12_RANGE none = {0, 0};
    s.readback->Unmap(0, &none);
    if (std::FILE* f = std::fopen((s.stem + ".txt").c_str(), "w")) {
      std::fprintf(f, "%u %u %llu %u\n", s.layout.Footprint.Width, s.rows,
                   (unsigned long long)s.row_bytes, uint32_t(s.layout.Footprint.Format));
      std::fclose(f);
    }
  }
  REXLOG_INFO("PGR4 native draw: wrote {} snapshot targets", snaps.size());
  snaps.clear();
}

DrawRenderer::DrawRenderer(NativeRenderer& renderer)
    : renderer_(renderer), impl_(std::make_unique<Impl>(renderer, stats_)) {}
DrawRenderer::~DrawRenderer() = default;

bool DrawRenderer::Initialize() { return impl_->Initialize(); }

bool DrawRenderer::OnSceneEnd(ID3D12GraphicsCommandList* list, RenderTargets& targets,
                              const SurfaceRegs& regs) {
  Impl& m = *impl_;
  if (!m.road_seen || !m.scene_copy_ready) return false;
  Impl::History& h = m.history[0];
  if (!h.copy.Take(list, targets, regs, Upscaler::SceneHeight(), m.frame_count)) return false;
  std::memcpy(h.clip, m.road_clip, sizeof(h.clip));
  std::memcpy(h.view, m.road_view, sizeof(h.view));
  h.valid = true;
  m.ResetListState();
  return true;
}

void DrawRenderer::OnDraw(const DrawState& state, const DrawPrep* prep) {
  impl_->Draw(state, prep);
}

void DrawRenderer::Prepare(const DrawState& state, uint64_t upload_tag, DrawPrep* prep,
                           std::vector<VertexCopy>* copies, std::vector<AuditRange>* audit) {
  impl_->Prepare(state, upload_tag, prep, copies, audit);
}

void DrawRenderer::StartAudit(uint32_t frames) { impl_->audit_left = frames; }

void DrawRenderer::LogFrames(uint32_t frames) {
  impl_->draw_log_left = std::max(impl_->draw_log_left, frames);
}

void DrawRenderer::BeginCopySegment() { impl_->NewSegment(); }

DrawRenderer::PrepareCounters DrawRenderer::EndFramePrepare(uint64_t done) {
  Impl& m = *impl_;
  PrepareCounters c;
  if (m.coherency) {
    m.CheckLateWrites();
    c.stale_pages = std::exchange(m.coherency->stale, 0);
    c.late_pages = std::exchange(m.coherency->late, 0);
    c.job = m.MakeCoherencyJob();
  }
  if (m.audit_left) {
    --m.audit_left;
    REXLOG_INFO(
        "PGR4 native audit: frame {} queued: {} draws, {} ranges ({:.1f} MB) to read back{}, {} "
        "shaders differ from the guest's ucode",
        m.watch_frame.load(std::memory_order_relaxed), m.audit_draws, m.audit_ranges,
        double(m.audit_bytes) / (1 << 20),
        m.audit_over ? fmt::format(", {} not (read back full)", m.audit_over) : std::string(),
        m.audit_shaders);
    m.audit_seen.clear();
    m.audit_draws = m.audit_ranges = m.audit_bytes = m.audit_over = m.audit_shaders = 0;
  }
  m.watch_frame.fetch_add(1, std::memory_order_relaxed);
  m.vertex_upload.EndFrame(done);
  c.upload_bytes = std::exchange(m.prep_upload_bytes, 0);
  c.uploads = std::exchange(m.prep_uploads, 0);
  c.diag = std::exchange(m.prep_diag, {});
  return c;
}

void DrawRenderer::AddPrepareCounters(const PrepareCounters& c) {
  stats_.upload_bytes += c.upload_bytes;
  stats_.uploads += c.uploads;
  stats_.stale_pages += c.stale_pages;
  stats_.late_pages += c.late_pages;
  PrepareDiag& d = impl_->diag.prep;
  d.shader_loads += c.diag.shader_loads;
  d.shader_loads_immediate += c.diag.shader_loads_immediate;
  d.ranges += c.diag.ranges;
  d.range_pages += c.diag.range_pages;
  d.range_runs += c.diag.range_runs;
  d.unwatched += c.diag.unwatched;
  if (c.job) impl_->coherency_next = c.job;
}

void DrawRenderer::BeforeSubmit() {
  Impl& m = *impl_;
  if (m.audit_job) {
    m.audit_job->fence = m.r.frame_fence();
    m.audit_inflight.push_back(std::move(m.audit_job));
  }
  if (!m.coherency_next) return;
  std::shared_ptr<CoherencyJob> job = std::move(m.coherency_next);
  if (!m.coherency_readbacks.empty()) {
    job->readback = std::move(m.coherency_readbacks.back());
    m.coherency_readbacks.pop_back();
  } else {
    if (!CreateBuffer(m.device, D3D12_HEAP_TYPE_READBACK, Impl::kCoherencyReadback,
                      D3D12_RESOURCE_STATE_COPY_DEST, "coherency readback", &job->readback))
      return;
  }
  ID3D12GraphicsCommandList* list = m.r.command_list();
  m.SyncListFrame();  // a frame without draws
  m.TransitionShared(list, D3D12_RESOURCE_STATE_COPY_SOURCE);
  uint64_t offset = 0;
  for (const auto& [start, size] : job->runs) {
    list->CopyBufferRegion(job->readback.Get(), offset, m.shared.Get(), start, size);
    offset += size;
  }
  job->fence = m.r.frame_fence();
  m.coherency_inflight.push_back(std::move(job));
}

void DrawRenderer::InvalidateListState(ListBreak why) {
  impl_->ResetListState();
  // Recording goes on in a new list, and the submitted one left shared memory in the common
  // state once it completed.
  const bool submit = why == ListBreak::kSubmit;
  if (submit) impl_->shared_state = D3D12_RESOURCE_STATE_COMMON;
  if (impl_->draw_log)
    std::fprintf(impl_->draw_log, "%s (after draw %u)\n", submit ? "submit" : "resolve",
                 impl_->frame_draws);
}

std::pair<uint64_t, uint64_t> DrawRenderer::ShaderHashes(const DrawPrep& prep) {
  return {prep.vs ? prep.vs->ucode_data_hash() : 0, prep.ps ? prep.ps->ucode_data_hash() : 0};
}

std::string DrawRenderer::ShaderDisassembly(uint64_t hash) const {
  const DxbcShader* shader = impl_->shaders.Find(hash);
  return shader ? shader->ucode_disassembly() : std::string();
}

void DrawRenderer::LogClear(const ClearCall& c) {
  std::FILE* f = impl_->draw_log;
  if (!f) return;
  std::fprintf(
      f,
      "clear (after draw %u) flags=%X colors=%X depth_bound=%d z=%g stencil=%u height=%u si=%08X "
      "ci0=%08X di=%08X rects",
      impl_->frame_draws, c.p.flags, c.bound_colors, int(c.depth_bound), c.p.depth,
      uint32_t(c.p.stencil), c.height, c.regs.surface_info, c.regs.color_info[0],
      c.regs.depth_info);
  for (const D3D12_RECT& r : c.p.rects)
    std::fprintf(f, " %ld,%ld-%ld,%ld", r.left, r.top, r.right, r.bottom);
  std::fprintf(f, " edram");
  for (const D3D12_RECT& r : c.p.edram_rects)
    std::fprintf(f, " %ld,%ld-%ld,%ld", r.left, r.top, r.right, r.bottom);
  std::fprintf(f, "\n");
}

void DrawRenderer::LogResolve(const ResolveParams& p) {
  // Local change (pgr4 project): the first resolve after a Bink frame is the video's texture.
  if (impl_->bink_drawn) {
    impl_->bink_drawn = false;
    impl_->video_texture = p.dest_base;
  }
  std::FILE* f = impl_->draw_log;
  if (!f) return;
  std::fprintf(f,
               "resolve params (after draw %u) src=%u si=%08X ci0=%08X di=%08X rect=%d,%d-%d,%d "
               "dest=%08X fmt=%u %ux%u at %d,%d clear=%X z=%g stencil=%u edram=%d,%d\n",
               impl_->frame_draws, p.source, p.regs.surface_info, p.regs.color_info[0],
               p.regs.depth_info, p.x0, p.y0, p.x1, p.y1, p.dest_base, p.dest_format, p.dest_width,
               p.dest_height, p.dest_x, p.dest_y, p.clear_flags, p.clear_depth,
               uint32_t(p.clear_stencil), p.edram_x, p.edram_y);
}

bool DrawRenderer::FrameIncomplete() const {
  const Impl& m = *impl_;
  return m.HoldsFrame();
}

void DrawRenderer::Impl::WriteCatalog() {
  if (catalog.size() == catalog_written || catalog_path.empty()) return;
  std::FILE* f = std::fopen(catalog_path.c_str(), "w");
  if (!f) return;
  std::fprintf(f, "shader\tslot\ttexture\twidth\theight\tformat\n");
  for (const CatalogEntry& e : catalog)
    std::fprintf(f, "%016llX\t%u\t%016llX\t%u\t%u\t%u\n", (unsigned long long)e.shader,
                 e.slot, (unsigned long long)e.content, e.width, e.height, e.format);
  std::fclose(f);
  catalog_written = catalog.size();
}

void DrawRenderer::RecordFrameTextures(const std::filesystem::path& path) {
  std::lock_guard<std::mutex> lock(impl_->frame_textures_mutex);
  impl_->frame_textures_asked = path;
}


void DrawRenderer::EndFrame(uint64_t completed_fence) {
  // Local change (pgr4 project): the texture catalog, rewritten every 300 frames while it grows.
  if (!impl_->catalog.empty() && ++impl_->catalog_frames >= 300) {
    impl_->catalog_frames = 0;
    impl_->WriteCatalog();
  }
  // Local change (pgr4 project): a frame's textures (RecordFrameTextures): the frame recorded
  // written, or the recording of the next one started.
  if (Impl& m = *impl_; m.frame_textures_on) {
    m.frame_textures_on = false;
    if (std::FILE* f = _wfopen(m.frame_textures_path.c_str(), L"w")) {
      std::fprintf(f, "shader\tslot\ttexture\twidth\theight\tformat\thd\tmips\tfetch_mips\n");
      for (const Impl::CatalogEntry& e : m.frame_textures)
        std::fprintf(f, "%016llX\t%u\t%016llX\t%u\t%u\t%u\t%c\t%u\t%u-%u\n",
                     (unsigned long long)e.shader, e.slot, (unsigned long long)e.content, e.width,
                     e.height, e.format, e.hd, e.mips, e.fetch_min, e.fetch_max);
      std::fclose(f);
    }
    REXLOG_INFO("PGR4 native screenshot: {} textures ({})", m.frame_textures.size(),
                m.frame_textures_path.string());
    m.frame_textures.clear();
  } else {
    std::lock_guard<std::mutex> lock(m.frame_textures_mutex);
    if (!m.frame_textures_asked.empty()) {
      m.frame_textures_path = std::move(m.frame_textures_asked);
      m.frame_textures_asked.clear();
      m.frame_textures_on = true;
    }
  }
  impl_->WaitForPipelines();  // after the frame's submission: the GPU runs it meanwhile
  stats_.views =
      impl_->views_allocated - (kViewTableStart + kFirstTexture) - impl_->free_views.size();
  // Local change (pgr4 project): the unused textures go once 3/4 of the view heap is taken, whatever the
  // video memory (TextureCache::SetViewsTight). The null views given when it was full are not kept.
  impl_->r.textures().SetViewsTight(stats_.views > impl_->ViewLimit() / 4 * 3);
  if (stats_.views >= impl_->views_logged + 10000) {
    impl_->views_logged = stats_.views / 10000 * 10000;
    REXLOG_INFO("PGR4 native draw: {} texture views (at most {})", stats_.views, impl_->ViewLimit());
  }
  if (impl_->view_heap_overflow) [[unlikely]] {
    impl_->view_heap_overflow = false;
    for (size_t i = 0; i < (size_t(1) << Impl::kSrvMemoBits); ++i) impl_->srv_memo[i] = Impl::SrvMemo{};
    for (size_t i = 0; i < (size_t(2) << Impl::kBindMemoSetBits); ++i) impl_->bind_memo[i].shader = nullptr;
  }
  stats_.watch_faults = impl_->watch_faults.exchange(0, std::memory_order_relaxed);
  stats_.watch_arms = impl_->watch_arms.exchange(0, std::memory_order_relaxed);
  stats_.watch_arm_pages = impl_->watch_arm_pages.exchange(0, std::memory_order_relaxed);
  stats_.watch_arm_changed = impl_->watch_arm_changed.exchange(0, std::memory_order_relaxed);
  stats_.watch_arm_ns = impl_->watch_arm_ns.exchange(0, std::memory_order_relaxed);
  stats_.hash_pages = impl_->hash_pages.exchange(0, std::memory_order_relaxed);
  stats_.hash_copies = impl_->hash_copies.exchange(0, std::memory_order_relaxed);
  if (!impl_->threaded) impl_->watch_frame.fetch_add(1, std::memory_order_relaxed);
  if (impl_->trace.file) {
    {
      std::lock_guard<std::mutex> lock(impl_->trace.mutex);
      ++impl_->trace.frame;
    }
    if ((impl_->trace.frame & 255) == 0) impl_->trace.Flush();
  }
  if (!impl_->threaded) impl_->vertex_upload.EndFrame(completed_fence);
  impl_->constant_upload.EndFrame(completed_fence);
  if (!impl_->coherency_inflight.empty()) impl_->CheckCoherencyJobs(completed_fence);
  if (!impl_->audit_inflight.empty()) impl_->CheckAuditJobs(completed_fence);
  impl_->Prewarm();
  impl_->PreparePaints();
  impl_->store.Flush();
  impl_->frame_draws = 0;
  ++impl_->frame_count;
  impl_->road_seen = false;
  impl_->ui_phase = false;
  impl_->ui_wait_rect = impl_->ui_rect_after_hud;
  impl_->ui_hud_seen = impl_->ui_armed = impl_->ui_rect_after_hud = false;
  impl_->frames_without_road = impl_->road_drawn ? 0 : impl_->frames_without_road + 1;
  impl_->rain_draws.clear();
  if (impl_->frames_without_road > kRainGoneFrames) impl_->rain[0] = impl_->rain[1] = 0.0f;
  impl_->road_drawn = false;
  {
    const auto clock_now = std::chrono::steady_clock::now();
    if (!g_pgr4_game_paused)
      impl_->clock_total += std::chrono::duration<double>(clock_now - impl_->clock_last).count();
    impl_->clock_last = clock_now;
    impl_->clock_s = float(std::fmod(impl_->clock_total, 3600.0));
    if (impl_->particles_drawn) {
      if (impl_->sheltered) impl_->shelter_ended = impl_->clock_total;
      impl_->sheltered = false;
      impl_->particles_seen = impl_->clock_total;
    } else if (!impl_->sheltered &&
               impl_->clock_total - impl_->particles_seen > Impl::kShelterSeconds &&
               impl_->clock_total - impl_->rain_script_seen > 5.0) {  // it comes and goes
      impl_->sheltered = true;
      impl_->shelter_began = impl_->particles_seen;
    }
    impl_->particles_drawn = false;
  }
  impl_->UpdateDropsGlide();
  impl_->replacer.Poll();
  impl_->WriteSnapshots(completed_fence);
  // Debug: the cube's sky's geometry read back (Draw), hashed.
  for (uint32_t s = 0; s < 8; ++s) {
    auto& p = impl_->geometry_probe[s];
    if (!p.fence || p.fence > completed_fence) continue;
    constexpr uint64_t kSize = 0x2000 + 0x11940;
    const D3D12_RANGE range = {size_t(s) * kSize, size_t(s + 1) * kSize};
    uint8_t* data = nullptr;
    if (SUCCEEDED(impl_->geometry_buffer->Map(0, &range, reinterpret_cast<void**>(&data)))) {
      REXLOG_INFO("PGR4 native geometry probe: frame {} indices {:016X} vertices {:016X}", p.frame,
                  XXH3_64bits(data + range.Begin, 0x2000),
                  XXH3_64bits(data + range.Begin + 0x2000, 0x11940));
      const D3D12_RANGE none = {0, 0};
      impl_->geometry_buffer->Unmap(0, &none);
    }
    p.fence = 0;
  }
  if (profile::On() && ++impl_->diag.frames >= 300) {
    Impl::Diag& d = impl_->diag;
    const double n = double(std::max<uint64_t>(d.draws, 1));
    auto per = [&](uint64_t v) { return double(v) / n; };
    REXLOG_INFO(
        "PGR4 native draw diag: {} draws/frame; per draw: shader loads {:.3f} ({:.3f} immediate), "
        "mods hits {:.3f}, translation hits {:.3f}, pso hits {:.3f}, ranges {:.3f} (pages "
        "{:.1f}, copied runs {:.3f}), unwatched copies {:.3f}, copy flushes {:.3f}",
        d.draws / d.frames, per(d.prep.shader_loads), per(d.prep.shader_loads_immediate),
        per(d.mods_hits), per(d.translation_hits), per(d.pso_hits), per(d.prep.ranges),
        per(d.prep.range_pages), per(d.prep.range_runs), per(d.prep.unwatched), per(d.flushes));
    std::string cbv;
    for (uint32_t i = 0; i < 16; ++i) {
      if (!d.cbv_uploads[i]) continue;
      char part[64];
      std::snprintf(part, sizeof(part), " r%u=%.3f/%.0fB", i, per(d.cbv_uploads[i]),
                    double(d.cbv_bytes[i]) / double(d.cbv_uploads[i]));
      cbv += part;
    }
    REXLOG_INFO("PGR4 native draw diag: cbv uploads per draw / bytes per upload:{}", cbv);
    const uint64_t* fc = d.fetch_classes;
    REXLOG_INFO(
        "PGR4 native draw diag: textures per draw by signs: unsigned {:.3f}, gamma rgb "
        "{:.3f}, gamma {:.3f}, signed {:.3f}, other {:.3f}; exp bias {:.3f}, lod bias {:.3f}",
        per(fc[0]), per(fc[1]), per(fc[2]), per(fc[3]), per(fc[4]), per(fc[5]), per(fc[6]));
    d = Impl::Diag{};
  }
  if (!impl_->debug_path.empty() && ++impl_->debug_poll >= 15) {
    impl_->debug_poll = 0;
    // Local change (pgr4 project), development: <capture dir>\\set.txt, a "name=value" per line,
    // any setting changed while the game runs as from the overlay; the file removed once read.
    {
      const std::filesystem::path set_path =
          std::filesystem::path(impl_->debug_path).parent_path() / "set.txt";
      std::ifstream in(set_path);
      if (in) {
        std::string line;
        while (std::getline(in, line)) {
          while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
          const size_t eq = line.find('=');
          if (eq == std::string::npos) continue;
          const bool ok = rex::cvar::SetFlagByName(line.substr(0, eq), line.substr(eq + 1));
          REXLOG_INFO("PGR4 native set.txt: {} {}", line, ok ? "applied" : "refused");
        }
        in.close();
        std::error_code ec;
        std::filesystem::remove(set_path, ec);
      }
    }
    uint32_t mask = 0, max_draws = 0, skip_from = 0, skip_to = 0, hash_count = 0;
    std::array<uint64_t, 8> hashes{};
    if (std::FILE* f = std::fopen(impl_->debug_path.c_str(), "r")) {
      if (std::fscanf(f, "%u %u %u %u", &mask, &max_draws, &skip_from, &skip_to) < 1) mask = 0;
      unsigned long long h;
      while (hash_count < hashes.size() && std::fscanf(f, "%llx", &h) == 1)
        hashes[hash_count++] = h;
      std::fclose(f);
    }
    if (mask != impl_->debug_mask || max_draws != impl_->debug_max_draws ||
        skip_from != impl_->debug_skip_from || skip_to != impl_->debug_skip_to ||
        hash_count != impl_->debug_hash_count || hashes != impl_->debug_hashes)
      REXLOG_INFO("PGR4 native draw: debug mask {} max draws {} skip [{}, {}), {} shader hashes",
                  mask, max_draws, skip_from, skip_to, hash_count);
    impl_->debug_hashes = hashes;
    impl_->debug_hash_count = hash_count;
    // Set, not held: the next frames' draws are logged once (to <capture dir>\draws-f<frame>.txt)
    // and the snapshots taken once.
    const auto set = [&](uint32_t bit) { return (mask & bit) && !(impl_->debug_mask & bit); };
    if (set(Impl::kDebugLogDraws))
      impl_->draw_log_left = uint32_t(std::max(REXCVAR_GET(pgr4_native_draw_log_frames), 1));
    if (set(Impl::kDebugSnapshots))
      impl_->snap_pending = hash_count ? (1u << hash_count) - 1 : 1u << 31;
    impl_->debug_mask = mask;
    impl_->upload_every_draw.store(mask & Impl::kDebugUploadEveryDraw, std::memory_order_relaxed);
    impl_->debug_max_draws = max_draws;
    impl_->debug_skip_from = skip_from;
    impl_->debug_skip_to = skip_to;
  }
  if (impl_->draw_log) {
    std::fclose(impl_->draw_log);
    impl_->draw_log = nullptr;
    renderer_.render_targets().SetLog(nullptr);
  }
  if (const int32_t from = REXCVAR_GET(pgr4_native_draw_log_from);
      from >= 0 && renderer_.frame_index() == uint64_t(from))
    impl_->draw_log_left = uint32_t(std::max(REXCVAR_GET(pgr4_native_draw_log_frames), 1));
  if (impl_->draw_log_left) {
    --impl_->draw_log_left;
    const std::filesystem::path dir(REXCVAR_GET(pgr4_native_capture_dir));
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path path =
        dir / ("draws-f" + std::to_string(renderer_.frame_index()) + ".txt");
    impl_->draw_log = std::fopen(path.string().c_str(), "w");
    renderer_.render_targets().SetLog(impl_->draw_log);
  }
  // The next frame's list finds the shared buffer in the common state, to which buffers decay
  // when ExecuteCommandLists completes: SyncListFrame resets shared_state at its first use.
}

}  // namespace pgr4::native
