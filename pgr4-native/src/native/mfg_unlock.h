// pgr4-native PGR4 native renderer, local addition (pgr4 project): DLSS Multi Frame Generation on
// RTX 40 (Ada) GPUs, ported from MFG Unlocker (dashdogy/RTX40MFG-Unlock, MIT, mfg_unlock/LICENSE;
// commit in mfg_unlock/UPSTREAM_COMMIT), D3D12 only.
//
// Ada's DLSS-G provider generates one frame between two, at their midpoint. Three in-memory
// patches of the loaded NVIDIA modules (the files on disk are left as they are) let it generate
// up to the Streamline wrapper's compiled maximum, as MFG Unlocker does on Ada:
//   - sl.dlss_g.dll: its plug-in startup caches min(the provider's DLSSG.MultiFrameCountMax, its
//     compiled maximum, 1, 3 or 5) as the frames it may generate; the cmovb that takes the
//     provider's 1 becomes nops (MFG Unlocker's PatchUniqueWrapperMaximumPattern);
//   - nvngx_dlssg.dll: its frame count/index validator's branch that rejects counts above 1
//     (mfg_unlock/ngx_mfg_gate.h);
//   - nvngx_dlssg.dll: its EstimateIntermMvecsScatter CUDA kernel, rewritten from the 0.5
//     midpoint to each generated frame's own position (mfg_unlock/midpoint_fix.cpp; the driver
//     compiles the patched PTX at the first frame generation).
// Each one matches an exact, unique pattern (and the provider an exact version and payload hash)
// and is skipped otherwise; frame generation then stays at one frame.
//
// Threading: the renderer's thread, from streamline::SetDevice.
#pragma once

#include <cstdint>

struct ID3D12Device;

namespace pgr4::native::mfg_unlock {

// Before slSetD3DDevice (sl.dlss_g's startup reads the maximum there): on an Ada adapter, patches
// sl.dlss_g.dll (loaded by slInit) and nvngx_dlssg.dll beside the executable (loaded here, the
// file NGX loads). Returns whether all three patches are in place.
bool Apply(ID3D12Device* device);
// The frames DLSS-G may generate per real frame: the wrapper's compiled maximum once Apply
// succeeded, else 1.
uint32_t MaxGeneratedFrames();

}  // namespace pgr4::native::mfg_unlock
