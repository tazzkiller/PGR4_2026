// Xenos shader translation for the native renderer: ReXGlue's DXBC translator (BSD-3, from
// Xenia) compiled into the app with the host-render-target path (no ROV, no EDRAM buffer).
// Output is DXBC SM 5.1, which D3D12 takes directly; no HLSL step.
//
// What the translated shaders expect from the draw code (DxbcShaderTranslator's ABI):
// - cbuffers in space 0 (DxbcShaderTranslator::CbufferRegister): b0 system constants
//   (DxbcShaderTranslator::SystemConstants), b1 float constants (only the constants the shader
//   reads, packed in ascending order: Shader::constant_register_map().float_bitmap), b2 bool and
//   loop constants, b3 the 96 fetch constants (raw guest dwords), b4 bindless descriptor indices;
//   with motion vectors (Modification::vertex.motion_vectors) b5 and b6, the previous frame's
//   b1 and b3 of the vertex shader;
// - vertex data is not fed through the input assembler: the vertex shader reads guest memory
//   from the "shared memory" ByteAddressBuffer (SRV t0 space 0) at the vfetch constant's address;
// - textures and samplers: bindless (spaces 1-3, indices from b4), listed by
//   DxbcShader::Get{Texture,Sampler}BindingsAfterTranslation();
// - one translation per (ucode, modification); the modification comes from a few registers per
//   draw (SelectModifications below), the same values the Xenos plugin uses.
#pragma once

#include <bit>
#include <cstdint>

#include <d3d12.h>
#include <wrl/client.h>

#include <rex/graphics/pipeline/shader/dxbc.h>
#include <rex/graphics/pipeline/shader/dxbc_translator.h>

namespace pgr4::native {

// Local change (pgr4 project): the system constants buffer the draws upload: the translator's,
// then the scene's sun and a clock for the replacement shaders (shader_replace.h).
struct SystemConstantsWithSun {
  rex::graphics::DxbcShaderTranslator::SystemConstants sys;
  float sun_direction[4];  // normalized, towards the sun
  float sun_color[4];      // w: 1 once known
  // x: seconds of the renderer's clock, per frame, wrapped every hour (the water's waves); y, z:
  // the settings the game gives its rain (its VS's c209.x and c210.x), 0 without rain (the wet
  // roads)
  float time[4];
  // The water's draws only (zero otherwise): their clip transform's rows (world to clip), and the
  // scene's copy (scene_copy.h): its color's and depth's bindless indices, its width and height.
  float clip[16];
  uint32_t scene[4];
  // ... and how their clip maps onto the copy: the viewport (x, y, width, height in its texels),
  // the translator's NDC scale and offset (xyz).
  float view[12];
  // The wet roads' draws only: the eye (their VS's c24), w the bindless index of the copy's
  // color running average (0: none).
  float camera[4];
  // The night windscreens' draws only (zero otherwise): the game leaves the wiper's data set, but its
  // night shaders do not read it. The mask's projection from the eye (rows; the VS's c68-c71, its w
  // moved to the eye), the blade (the PS's c214), x the bindless index of the mask (fetch 6).
  float wiper[16];
  float wiper_blade[4];
  uint32_t wiper_mask[4];
  // The last time the game drew no falling particles (a bridge, a tunnel), in seconds ago: x since it began,
  // y since it ended (0 while it lasts). A flake that landed then is not drawn (snow.hlsli). z: 1 in heavy snow, w: 1 in light rain.
  float shelter[4];
  // The cars' grime in slot 4 (car_paint_hooks.hlsli): x its amount forced (below 0: the game's c220.x),
  // y 1 under frost and snow (else the game's coordinates), z its size off the frost, w the frost's fine grain.
  float dirt[4];
};
static_assert(sizeof(rex::graphics::DxbcShaderTranslator::SystemConstants) % 16 == 0);

using rex::graphics::DxbcShader;
using rex::graphics::DxbcShaderTranslator;

// Raw guest register values that select the translation of a draw's shaders.
struct ShaderDrawRegs {
  uint32_t sq_program_cntl = 0;
  uint32_t sq_context_misc = 0;
  uint32_t sq_interpolator_cntl = 0;
  uint32_t pa_cl_clip_cntl = 0;
  uint32_t vgt_draw_initiator = 0;  // only prim_type is read
  uint32_t vgt_output_path_cntl = 0;
  uint32_t pa_su_sc_mode_cntl = 0;
  uint32_t rb_modecontrol = 0;
  uint32_t rb_surface_info = 0;
  uint32_t rb_colorcontrol = 0;
  uint32_t rb_color_mask = 0;
  uint32_t rb_color_info[4] = {};  // RB_COLOR_INFO, RB_COLOR1_INFO, RB_COLOR2_INFO, RB_COLOR3_INFO
};

struct ShaderModifications {
  // false: the plugin drops the draw (EDRAM copy mode, i.e. the D3D's own resolve rectangles, or
  // nothing can rasterize and the vertex shader does not memexport); nothing else is valid then.
  bool draw = false;
  bool pixel_shader_used = false;  // false: draw without a pixel shader (depth only)
  uint64_t vertex = 0;
  uint64_t pixel = 0;
};

// Mirrors D3D12CommandProcessor::IssueDraw + PipelineCache::GetCurrent*ShaderModification for
// the host-render-target path. Both shaders must have their ucode analyzed (ShaderCache::Load
// does that); ps may be null.
ShaderModifications SelectModifications(const DxbcShaderTranslator& translator,
                                        const DxbcShader& vs, const DxbcShader* ps,
                                        const ShaderDrawRegs& regs);

// The pixel shader modification specialized for the draw's texture fetch constants: sets
// Modification::pixel.textures_unsigned when every fetch constant in `textures` (bit i: fetch
// constant i) has no exponent adjustment and components that fetch as unsigned: sign_x..sign_w
// all unsigned, or unsigned and signed on a format in `signed_is_unsigned_formats`
// (SignedViewIsUnsignedFormats: the signed view is the unsigned one, and kSigned applies no
// conversion of its own). `fetch` is the first of the 32 six-dword texture fetch constants.
// Signs and exponent adjustment change per draw, so this stays outside SelectModifications and
// its memo.
inline uint64_t SpecializePixelTextures(uint64_t pixel, const uint32_t* fetch, uint32_t textures,
                                        uint64_t signed_is_unsigned_formats) {
  if (!textures) return pixel;
  for (; textures; textures &= textures - 1) {
    const uint32_t* f = fetch + std::countr_zero(textures) * 6;
    const uint32_t signs = f[0] & 0x3FCu;  // 2 bits per component: 0 unsigned, 1 signed
    if (f[3] & 0x7E000u) return pixel;
    if (signs && ((signs & 0x2A8u) || !((signed_is_unsigned_formats >> (f[1] & 63)) & 1)))
      return pixel;
  }
  DxbcShaderTranslator::Modification m(pixel);
  m.pixel.textures_unsigned = 1;
  return m.value;
}
// The draw's modifications with motion vectors (Modification::vertex/pixel.motion_vectors), for
// a draw with a pixel shader.
inline ShaderModifications WithMotionVectors(ShaderModifications mods) {
  DxbcShaderTranslator::Modification vm(mods.vertex), pm(mods.pixel);
  vm.vertex.motion_vectors = 1;
  pm.pixel.motion_vectors = 1;
  mods.vertex = vm.value;
  mods.pixel = pm.value;
  return mods;
}

// Root parameter indices of the bindless root signature (the plugin's order, then the motion
// vector pass's).
enum BindlessRootParameter : uint32_t {
  kBindlessRoot_FetchConstants,
  kBindlessRoot_FloatConstantsVertex,
  kBindlessRoot_FloatConstantsPixel,
  kBindlessRoot_DescriptorIndicesPixel,
  kBindlessRoot_DescriptorIndicesVertex,
  kBindlessRoot_SystemConstants,
  kBindlessRoot_BoolLoopConstants,
  kBindlessRoot_SharedMemory,  // table: SRV t0 (shared memory), UAV u0 (shared memory)
  kBindlessRoot_SamplerHeap,   // unbounded sampler table from heap start
  kBindlessRoot_ViewHeap,      // unbounded 2D-array / 3D / cube SRVs (spaces 1-3) from table start
  kBindlessRoot_PreviousFloatConstantsVertex,
  kBindlessRoot_PreviousFetchConstantsVertex,
  kBindlessRoot_Count,
};

// The one root signature every bindless translation uses. *blob_hash: XXH3 of its serialized
// blob, which names in the pipeline library depend on (PipelineLibrary::Open).
Microsoft::WRL::ComPtr<ID3D12RootSignature> CreateBindlessRootSignature(
    ID3D12Device* device, uint64_t* blob_hash = nullptr);

}  // namespace pgr4::native
