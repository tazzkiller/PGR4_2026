// pgr4-native PGR4 native renderer, local addition (pgr4 project, 20-21/11/2026): volumetric fog
// over the game's 3D scene, an option (--pgr4_native_volumetric, the overlay); off, the game as it
// was.
//
// Two parts, both from each pixel's ray through the air before its surface (the sky left as the
// game made it):
//   - The game's own fog laid again in volume (--pgr4_native_volumetric_fog). Its scene shaders fog
//     each pixel as fog + (surface - fog) x T, T = exp2(-c254.y x tau) (c254.y 0.5), tau its distance fog
//     (c10.w per metre) plus, with bool 158, its height fog (below the ceiling c11.x, c11.y times
//     min(1, depth below x c11.z) at the lower end, over the way below the ceiling), c10.rgb the
//     fog's color (21/11/2026, read in 00D7B7A2AA58BCF3 and measured: Heavy Fog is that height
//     fog, ceiling 130 m above a road at about 100 m). From the same constants (draw.cpp copies the
//     last scene draw's), the game's T at each pixel is known exactly: its fog is taken out of the
//     scene's color and laid again with the same thickness and color, but in banks that drift with
//     the wind (3D noise over the world, --pgr4_native_volumetric_fog_variation and _size), lit by
//     the sun where it gets through (--pgr4_native_volumetric_fog_sun). Variation 0 and no sun: the
//     game's own exactly. Where the game's fog hides the surface, so does ours (what is behind is
//     not in the image).
//   - The sun's glow in the air (--pgr4_native_volumetric_density, _distance, _strength): air lit
//     by the sun up to a distance, with the shadows of what hides it, added.
// Passes, at the scene's first resolve of the frame, after the ambient occlusion and the scene's
// copy our reflections read, before the game's bloom and exposure:
//   1. MARCH (compute, half size): along each ray, the sun-lit air's light (24 steps, each with a
//      few steps towards the sun projected on the screen and found behind the scene's depth or not:
//      the shadows), and over the game's fog (16 steps) the mean of the banks' thickening and the
//      share of it the sun reaches, both weighed by the fog met.
//   2. BLUR (compute, half size, across then down): depth-aware; the steps' per-pixel noise (fixed:
//      nothing accumulates it) smoothed out.
//   3. APPLY (a full-screen draw into the scene's target, blended color x a + rgb): the game's T
//      worked out at the pixel, ours from it and the banks, the fog laid again and the light added.
// Each frame on its own, the banks drifting on the game's clock (real time, still in the pause):
// the same at any frame rate.
//
// Threading: the thread that records the frames (the resolver's).
#pragma once

#include "fidelityfx.h"
#include "render_targets.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdint>
#include <map>
#include <array>
#include <utility>
#include <vector>

namespace pgr4::native {

// The game's transparent world starts (1) or ends (2), in the draws' order (renderer.cpp's queue,
// draw.cpp's probe "PGR4 volumetric cut").
void WorldMarker(uint32_t id);

// Local change (25/11/2026, issue 29: player 1's shadows on player 2's half in split screen): the
// scene's view the draws and resolves are on, 0 for player 1's (or the only one), 1 for player 2's
// (renderer.cpp: the scene passes before it in the frame, with pgr4_native_split_screen_views;
// always 0 with it off). The render thread's (the draws' A and the resolves). The scene effects
// made once a frame (the volumetric fog, the ambient occlusion) are made once a view: their
// "this frame" is this key, their descriptors and uploads player 2's own.
// Local change (26/11/2026, Tazz: the split screen's changes only ever in split screen): with one
// view (the solo) the key is the frame and the slot the frame's in flight, as before; player 2's
// key has its top bit set, and its descriptors and uploads are on heaps and buffers of its own,
// made at its first use (Volumetrics::ViewState, SceneCopy and SceneEffects heap_view1_).
constexpr uint32_t kPgr4MaxViews = 2;
extern uint32_t g_pgr4_scene_view;
inline uint64_t Pgr4ViewKey(uint64_t frame) {
  return frame | (uint64_t(g_pgr4_scene_view) << 63);
}
inline uint32_t Pgr4ViewSlot(uint64_t frame, uint32_t frames_in_flight) {
  return uint32_t(frame % frames_in_flight);
}
// ... and the rows each view's scene is drawn on (its depth resolve's, 360 of the scene's 720)
// while the last frame had two scene passes, with pgr4_native_split_screen_view_rows; 0 otherwise
// (the scene's whole height, as before).
extern uint32_t g_pgr4_split_rows;

class Volumetrics {
 public:
  bool Initialize(ID3D12Device* device);
  // The frame's camera (its planes and field of view) and its clip transform, the four row
  // vectors (world to clip) FindCamera found it from; read at Apply.
  void set_camera(const FfxCamera* camera, const float* clip_rows) {
    camera_ = camera;
    clip_rows_ = clip_rows;
  }
  // The option on, on a screen with the 3D scene, not the game as it was.
  static bool Enabled();
  // At the scene's first resolve of the frame (`frame` its number). Sets its own pipelines,
  // root signatures, descriptor heap and render target on the list: returns true when it did,
  // so the draws bind their state again.
  // `timer` (--pgr4_native_gpu_timing): its passes timed as the scene effects' steps 5 (the
  // computes: sun-lit air, ground map, volume) and 6 (laid on the scene), 21/11/2026.
  // Local change (21/11/2026, the fog to be laid by each shader: pgr4_native_volumetric_volume_early):
  // `early` true, only the volume (the lamps, the smoothing, MEDIA and VOLUME, shared), at the
  // frame's first main-view draw with the game's fog (draw.cpp), before the scene's draws read it;
  // the depth not touched (bound for writing then). The pass at the resolve then lays it as before,
  // the volume not made again. Returns true when it set its own state on the list.
  bool Apply(ID3D12GraphicsCommandList* list, RenderTargets& targets, const SurfaceRegs& regs,
             uint64_t frame, class GpuTimer* timer = nullptr, bool early = false);
  // Whether the volume was already made this frame (early): the draws look before calling.
  // Local change (25/11/2026): for the draws' view (Pgr4ViewKey).
  bool VolumeMade(uint64_t frame) const { return volume_frame_ == Pgr4ViewKey(frame); }
  // Local change (21/11/2026): the volume as this frame made it, for the replacement shaders of the
  // game's transparent draws after it (draw.cpp, shaders/fog_volume_hooks.hlsli: the spectators'
  // flags), which take their fog's transmittance from it.
  struct Share {
    uint64_t frame = UINT64_MAX;    // the frame and view it was made in (Pgr4ViewKey)
    ID3D12Resource* volume = nullptr;
    uint32_t generation = 0;        // counted up each time the volume is made again
    float values[16] = {};          // pgr4_fog_volume (fog_volume_hooks.hlsli)
    // Local change (21/11/2026, the fog laid by each shader): the columns' ours / the game's at the
    // range (R32_FLOAT, columns x rows; bindless kFogRange), and SystemConstants::fog_shader.
    ID3D12Resource* range = nullptr;
    float shader[16] = {};
  };
  // Whether this frame's fog is laid by the shaders (pgr4_native_volumetric_in_shader, mode 2): the
  // pass at the resolve then lays only the lamps, the sun-lit air and the sky (draw.cpp sets it).
  static bool InShader();
  const Share& share() const { return share_; }
  // Local change (28/11/2026, pgr4_native_fg_light_mask): the light the pass at the resolve added
  // (the lamps and the sun-lit air), its luminance per pixel of the scene (R16G16_FLOAT: x the
  // lamps', y the sun-lit air's; the scene's
  // visible size `w` x `h`), for the frame generation: frame `frame`'s, moved to
  // NON_PIXEL_SHADER_RESOURCE on `list`; nullptr when this frame laid none (view 0 only).
  ID3D12Resource* LightMaskToRead(ID3D12GraphicsCommandList* list, uint64_t frame, uint32_t* w,
                                  uint32_t* h);

 private:
  template <typename T>
  using ComPtr = Microsoft::WRL::ComPtr<T>;

  bool EnsureTargets(uint32_t width, uint32_t height);
  // The volume's noise (once) and its cells for that many columns.
  bool EnsureVolume(uint32_t columns_x, uint32_t columns_y);
  ID3D12PipelineState* ApplyPipeline(DXGI_FORMAT format, uint32_t samples, bool debug);
  // The grid probe after the apply (pgr4_native_volumetric_probe_grid): the points of two frames
  // ago read and compared, this frame's copied.
  void GridProbe(ID3D12GraphicsCommandList* list, uint64_t frame, const float eye[3]);
  // Two frames' volume inputs (volume_inputs_), those that changed, for the grid probe's log.
  std::string VolumeInputsChanged(uint64_t a_frame, uint64_t b_frame) const;
  void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES& state,
                  D3D12_RESOURCE_STATES after);

  ID3D12Device* device_ = nullptr;
  const FfxCamera* camera_ = nullptr;
  const float* clip_rows_ = nullptr;
  bool ready_ = false;
  uint64_t applied_frame_ = UINT64_MAX;
  // The frame whose volume is made (early or at the resolve), and the early pass's probe.
  uint64_t volume_frame_ = UINT64_MAX;
  // The columns' ours / the game's transmittance at the range (VOLUME's u1), for the shaders'
  // fog beyond the range.
  ComPtr<ID3D12Resource> range_;
  D3D12_RESOURCE_STATES range_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  uint64_t early_tried_frame_ = UINT64_MAX;  // the early pass tried once a frame, made or not
  // The ground map's corner when it was made (at the resolve, from that frame's depth): the early
  // volume of the next frame reads it there, not at its own eye's corner (a cell off when the eye
  // crossed a cell's edge).
  float ground_made_origin_[2] = {};
  bool ground_made_ = false;
  struct EarlyProbe {
    uint64_t early = 0, late = 0, twice = 0, frames = 0;
    float eye_gap_max = 0.0f;   // the eye early against the eye at the resolve (m)
    uint64_t ground_shifted = 0;  // early volumes read the map at another corner than their own
    float early_eye[3] = {};
    uint64_t early_frame = UINT64_MAX;
  } early_probe_;
  // The banks' clock: seconds of the game's clock running.
  double fog_time_ = 0.0;
  std::chrono::steady_clock::time_point clock_last_;
  bool clock_started_ = false;
  ComPtr<ID3D12RootSignature> compute_signature_, apply_signature_;
  ComPtr<ID3D12PipelineState> march_pso_, march_ms_pso_, blur_pso_, noise_pso_, volume_pso_,
      media_pso_;
  ComPtr<ID3D12PipelineState> ground_clear_pso_, ground_scatter_pso_, ground_scatter_ms_pso_,
      ground_fill_pso_;
  // The grid probe's colour reading (pgr4_native_volumetric_probe_grid): plain and multisampled.
  ComPtr<ID3D12PipelineState> grid_pso_, grid_ms_pso_;
  ComPtr<ID3DBlob> apply_vs_;
  ComPtr<ID3DBlob> apply_froxel_ps_[4];  // with the volume: [multisampled x 2 + debug]
  std::map<std::pair<DXGI_FORMAT, uint32_t>, ComPtr<ID3D12PipelineState>> apply_psos_;
  ComPtr<ID3D12DescriptorHeap> heap_;
  uint32_t stride_ = 0;
  // Half the scene's size (R16G16B16A16_FLOAT), each blurred through the second and back: the
  // sun-lit air's light and its view depth; the banks' mean and the sun's share of the fog.
  ComPtr<ID3D12Resource> light_[2], fog_[2];
  D3D12_RESOURCE_STATES light_state_[2] = {D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS},
                        fog_state_[2] = {D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
  uint32_t width_ = 0, height_ = 0;
  // The volume (local change, 21/11/2026): the banks' noise (128^3, R16_FLOAT, made once) and the
  // cells (a column per 16 x 16 pixels, 64 slices, R16G16B16A16_FLOAT: light, transmittance).
  ComPtr<ID3D12Resource> noise_, volume_;
  D3D12_RESOURCE_STATES noise_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        volume_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  bool noise_ready_ = false;
  // The cells' fog (local change, 21/11/2026, pgr4_native_volumetric_temporal): this frame's and
  // the previous (R16G16B16A16_FLOAT: the banks' thickening, the mist per metre, the shade), the
  // two swapped each frame; the previous frame's camera, the blend and the jitter for MEDIA
  // (lights' headers 4-8), the clock and the jitter's count.
  ComPtr<ID3D12Resource> media_[2];
  D3D12_RESOURCE_STATES media_state_[2] = {D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
  uint32_t media_now_ = 0;
  bool media_valid_ = false;
  float temporal_header_[20] = {};
  float temporal_prev_[16] = {};  // eye 3, east 3, up 3, north 3, tan 2, range, set
  double temporal_last_s_ = 0.0;
  uint32_t temporal_count_ = 0, temporal_resets_ = 0;
  // Resources replaced (a size change), kept until the frames in flight are past them.
  void Retire(ComPtr<ID3D12Resource>& resource);
  std::vector<std::pair<uint64_t, ComPtr<ID3D12Resource>>> retired_;
  uint64_t apply_frame_ = 0;
  // The apply probe (development, pgr4_native_volumetric_probe_uv): a pixel's values written by
  // the apply (UAV u1), copied out every 2 s.
  ComPtr<ID3D12Resource> probe_, probe_readback_;
  bool probe_copied_ = false, probe_on_ = false;
  // The grid probe (pgr4_native_volumetric_probe_grid, 21/11/2026): 16 x 9 points of the screen
  // each frame, the scene's colour before the apply and the apply's values there, copied to a ring
  // of readbacks read two frames later (done by then: kFramesInFlight 2), the previous frame's
  // points kept to find the flashes. The frame and eye each slot was written for.
  ComPtr<ID3D12Resource> grid_readback_[3];
  uint64_t grid_written_[3] = {UINT64_MAX, UINT64_MAX, UINT64_MAX};
  float grid_eye_[3][3] = {};
  bool grid_on_ = false;
  // Local change (28/11/2026, pgr4_native_fg_light_mask): LightMaskToRead's image.
  ComPtr<ID3D12Resource> light_mask_, light_mask_old_;
  D3D12_RESOURCE_STATES light_mask_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  uint32_t light_mask_w_ = 0, light_mask_h_ = 0;
  uint64_t light_mask_frame_ = UINT64_MAX;
  struct GridPoint {
    float colour[4], depth[4], fog[4], add[4], volume[4];
  };
  GridPoint grid_prev_[144] = {};
  uint64_t grid_prev_frame_ = UINT64_MAX;
  float grid_prev_eye_[3] = {};
  // ... and each frame's volume inputs (at the early pass, by frame % 8): the fog-laying draws
  // before it, its constants (volumetrics.cpp Constants, 63 dwords), the flares taken, the lamps
  // sent, the cells' fog slot, the ground map made; a flash's two frames compared on them.
  struct VolumeInputs {
    uint64_t frame = UINT64_MAX, before = 0;
    uint32_t constants[63] = {};
    uint32_t flares = 0, lamps = 0, media_now = 0;
    bool ground_made = false;
  };
  VolumeInputs volume_inputs_[8];
  // ... and the visibility against the game's by distance band (0-20, 20-50, 50-100, 100-300, beyond).
  struct Visibility {
    double n[5] = {}, ours[5] = {}, game[5] = {};
  };
  Visibility vis_;
  uint32_t lamps_sent_ = 0;
  float probe_pixel_[2] = {};
  // The ground map (local change, 21/11/2026): the lowest height seen in each cell (R32_UINT,
  // orderable), then filled (R32_FLOAT), 128 x 128 cells of 2 m around the eye.
  ComPtr<ID3D12Resource> ground_[2];
  D3D12_RESOURCE_STATES ground_state_[2] = {D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
  ComPtr<ID3D12Resource> ground_readback_;  // development: the filled map, read back every 2 s
  // The lights (local change, 21/11/2026): an upload buffer, a slot a frame in flight, each a
  // header and two float4 a light (fog_volume: VOLUME's t0), mapped.
  ComPtr<ID3D12Resource> lights_;
  uint8_t* lights_mapped_ = nullptr;
  std::vector<std::array<float, 11>> flares_;  // the flares taken (draw.cpp): c51, the raw position, colour
  std::vector<std::array<float, 6>> flare_centres_;  // ... decoded, colour
  bool ground_copied_ = false;
  uint32_t volume_columns_[2] = {};
  Share share_;
  // Local change (25/11/2026, split screen): the state each view keeps from one frame to the next
  // (its cells' fog history and its camera then, its ground map), the members above being the
  // view's Apply is on (active_view_), the others' kept here (SelectView swaps them). The cells'
  // fog's size (media_columns_) is the view's own: made again when the volume's changed.
  uint32_t media_columns_[2] = {};
  struct ViewState {
    // (26/11/2026) The view's descriptor heap and lights' upload buffer: player 2's made at its
    // first volume (EnsureVolume), player 1's (the solo's) those of before.
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12Resource> lights;
    uint8_t* lights_mapped = nullptr;
    ComPtr<ID3D12Resource> media[2];
    D3D12_RESOURCE_STATES media_state[2] = {D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    uint32_t media_now = 0;
    bool media_valid = false;
    uint32_t media_columns[2] = {};
    float temporal_header[20] = {};
    float temporal_prev[16] = {};
    double temporal_last_s = 0.0;
    ComPtr<ID3D12Resource> ground[2];
    D3D12_RESOURCE_STATES ground_state[2] = {D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    float ground_made_origin[2] = {};
    bool ground_made = false;
  };
  ViewState views_[kPgr4MaxViews];
  uint32_t active_view_ = 0;
  void SwapViewState(ViewState& s);
  void SelectView(uint32_t view);
  // The probe (development): each view's eye at its volume, the frame it was made in.
  float view_eye_[kPgr4MaxViews][3] = {};
  uint64_t view_eye_frame_[kPgr4MaxViews] = {UINT64_MAX, UINT64_MAX};
};

}  // namespace pgr4::native
