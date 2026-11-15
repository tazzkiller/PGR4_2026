// Local addition (pgr4 project, not in pgr4-native): NVIDIA Streamline for DLSS Super Resolution
// (and later DLSS Frame Generation with Reflex), in manual hooking mode: only the native renderer's
// own device, present queue and swapchain go through Streamline; the Xenos plugin's device is
// never passed to it.
//
// sl.interposer.dll is loaded at run time from beside the executable after its NVIDIA signature is
// verified, so nothing links against it and the D3D12/DXGI imports stay the system's. The
// feature helpers of the SDK headers (slDLSSSetOptions...) reach their functions through
// slGetFeatureFunction, which streamline.cpp defines on top of the loaded DLL.
//
// Order (Streamline's ProgrammingGuideManualHooking): Init before the device, SetDevice right
// after D3D12CreateDevice, the present queue created through UpgradeDevice's device and the
// swapchain through UpgradeFactory's factory; Shutdown before the swapchain and device go.
//
// Threading: Init/Shutdown on the thread that creates the device; the rest on the recording thread.
#pragma once

#include <sl.h>
#include <sl_pcl.h>

#include <cstdint>

struct ID3D12Device;

namespace pgr4::native::streamline {

// Whether --pgr4_native_dlss asks for DLSS (Super Resolution in place of FSR).
bool DlssRequested();
// Whether --pgr4_native_dlss_fg asks for DLSS Frame Generation (with Reflex), and whether it is
// available after SetDevice.
bool FrameGenerationRequested();
bool FrameGenerationActive();
// Local change (pgr4 project): DLSS-G's mode is on (FrameGenerationActive: it is only available).
bool FrameGenerationOn();
// Local change (pgr4 project): whether this frame's generation restarts (DLSS-G's reset) after a
// scene drawn and never presented.
bool SkipGeneration();
// Local change (pgr4 project): the frames DLSS-G may generate per drawn frame (1 without Multi
// Frame Generation), and those --pgr4_native_dlss_fg_frames asks for within it.
uint32_t MaxGeneratedFrames();
uint32_t GeneratedFrames();

// Once per frame at its start (NativeRenderer::BeginFrameModules): the frame's token, Reflex's
// sleep and the simulation markers, and DLSS-G's mode from whether the frame before had the
// scene's inputs (MarkScene).
void BeginFrame(uint64_t frame);
// The token of the frame BeginFrame began (nullptr before the first).
sl::FrameToken* Token();
// The frame's DLSS evaluation tagged the scene's depth and motion: DLSS-G can generate from it.
void MarkScene();
// PCL markers around the frame's submission and present (D3D12Device::EndFrame).
void Marker(sl::PCLMarker marker);
// The game's image within the back buffer (the Presenter's aspect-fit rectangle), which the
// scene's depth and motion cover: DLSS-G maps them onto it instead of the whole back buffer.
// `hudless`: the frame's HUD-less image (hudless.h, the back buffers' size, PRESENT), or null.
void TagBackbuffer(int32_t left, int32_t top, int32_t width, int32_t height,
                   ID3D12Resource* hudless = nullptr);

// Loads and initializes Streamline with DLSS; false (logged) when the DLL, its signature or slInit
// fails, and everything below then stays off. Idempotent.
bool Init();
bool Active();
// The renderer's device, right after its creation; false (logged) when DLSS is not supported on
// its adapter (Active() then turns false).
bool SetDevice(ID3D12Device* device);
// Replaces *iface (an ID3D12Device*, IDXGIFactory* or IDXGISwapChain*) by Streamline's proxy of
// it; false (logged) and *iface unchanged on failure.
bool Upgrade(void** iface);
void Shutdown();

// The core functions, valid while Active().
struct Api {
  PFun_slGetNewFrameToken* getNewFrameToken = nullptr;
  PFun_slSetConstants* setConstants = nullptr;
  PFun_slSetTagForFrame* setTagForFrame = nullptr;
  PFun_slEvaluateFeature* evaluateFeature = nullptr;
  PFun_slIsFeatureSupported* isFeatureSupported = nullptr;
  PFun_slAllocateResources* allocateResources = nullptr;
  PFun_slFreeResources* freeResources = nullptr;
};
const Api& api();

}  // namespace pgr4::native::streamline
