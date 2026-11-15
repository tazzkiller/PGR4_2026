// Local change (pgr4 project): an in-game overlay to test the renderer's settings live (Dear
// ImGui: the SDK's core, its DX12 backend), drawn into the back buffer at each Swap. bind_overlay
// (F2) shows or hides it; while shown it takes the window's mouse.
//
// All live: the upscaler on or off, DLSS or FSR, DLSS's mode (the upscaler made again) and
// preset, DLSS Frame Generation and its motion vector factors, the scene's MSAA, and the
// resolution scale and anisotropy (the renderer's pipeline made again: NativeRenderer::
// RebuildPipeline). "Save" writes them into pgr4.toml beside the executable (its other lines and
// comments kept).
//
// Threading: Draw on the thread that records the frames; the window's messages on its own thread
// reach it through a queue.
#pragma once

#include <d3d12.h>
#include <windows.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct ImGuiContext;

namespace pgr4::native {

class D3D12Device;

// The renderer's settings into pgr4.toml (overlay.cpp); returns what was written where.
std::string SaveRendererSettings(bool upscaler_on);

class Overlay {
 public:
  // What the renderer shows.
  struct Info {
    bool upscaler_on = false;
    bool dlss = false;           // the upscaler is DLSS (else FSR)
    bool dlss_available = false; // Streamline is up
    bool fg_available = false;   // DLSS-G loaded
    uint32_t render_width = 0, render_height = 0;
    uint32_t output_width = 0, output_height = 0;
    uint32_t backbuffer_width = 0, backbuffer_height = 0;
    uint32_t resolution_scale = 1;
  };
  // What the renderer does after Draw.
  struct Actions {
    bool switch_upscaler = false;    // on <-> off (as bind_upscaler)
    bool recreate_upscaler = false;  // its settings changed: made again
    bool rebuild_pipeline = false;   // resolution scale or anisotropy changed
    bool apply_msaa = false;         // the scene's MSAA changed
  };

  explicit Overlay(D3D12Device& device) : device_(device) {}
  ~Overlay();
  Overlay(const Overlay&) = delete;
  Overlay& operator=(const Overlay&) = delete;

  void Toggle() { toggle_requested_ = true; }
  bool visible() const { return visible_; }
  // Whether the last Draw drew the overlay.
  bool drew() const { return drew_; }

  // Each Swap with a shown frame: counts it, and while visible draws into `target` (PRESENT in and
  // out, `rtv` its view) with `list`.
  Actions Draw(ID3D12GraphicsCommandList* list, ID3D12Resource* target,
               D3D12_CPU_DESCRIPTOR_HANDLE rtv, DXGI_FORMAT format, const Info& info);

 private:
  struct Event {
    UINT msg;
    WPARAM wp;
    LPARAM lp;
  };
  bool Initialize(DXGI_FORMAT format);
  void Shutdown();
  void Hook(HWND hwnd);
  void Unhook();
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  void FeedInput();
  Actions Build(const Info& info);
  bool Save();

  D3D12Device& device_;
  ImGuiContext* context_ = nullptr;
  bool initialized_ = false, failed_ = false;
  DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srv_heap_;
  uint32_t srv_stride_ = 0;
  std::vector<bool> srv_used_;
  bool visible_ = false, drew_ = false;
  std::atomic<bool> toggle_requested_{false};
  // The window and its procedure before the hook.
  HWND hwnd_ = nullptr;
  WNDPROC previous_proc_ = nullptr;
  // Messages from the window's thread.
  std::mutex events_mutex_;
  std::vector<Event> events_;
  std::atomic<bool> capturing_{false};
  // Frame rate: shown frames over the last half second.
  uint64_t rate_start_ = 0;
  uint32_t rate_frames_ = 0;
  float fps_ = 0.0f, frame_ms_ = 0.0f;
  // Restart-only settings as chosen, and the last save's result.
  int resolution_scale_ = 0, msaa_ = -1, anisotropy_ = -1;
  int upscaler_width_ = 0, upscaler_height_ = 0;
  std::string save_status_;
};

}  // namespace pgr4::native
