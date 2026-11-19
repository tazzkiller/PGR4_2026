// pgr4-native PGR4 native renderer: D3D12 device, frame command lists, swapchain and the debug
// window.
#include "d3d12_device.h"

#include "clock.h"
#include "gpu_timer.h"
#include "streamline.h"

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>

#include <simde/x86/sse.h>

#include <algorithm>
#include <string>
#include <cstdio>
#include <condition_variable>
#include <cwchar>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <utility>

// Local change (pgr4 project), development: what the GPU was doing when it hung (DEVICE_HUNG, 06/11/2026: the
// log named nothing). Device Removed Extended Data: the commands in progress and the memory at fault, into
// the log and dred.txt beside the executable.
REXCVAR_DEFINE_BOOL(pgr4_native_dred, false, "PGR4 native",
                    "Development: report what the GPU was doing if the device is removed (a little slower)");

// Local change (pgr4 project): the wait below, with DLSS frame generation on. Switchable live: it costs far
// more than first measured (06/11/2026, x3 DLAA: 80 frames a second fell to about 60).
// On by default (Tazz, 07/11/2026): off, it was worse on a friend's PC. Without it the wrong sky can come back
// while the GPU is saturated.
REXCVAR_DEFINE_BOOL(pgr4_native_gpu_serialize_fg, true, "PGR4 native",
                    "Wait for the GPU at the end of every frame while DLSS frame generation is on "
                    "(against a wrong sky on the cars' reflections when the GPU is saturated)");
// Local addition (18/11/2026, Tazz): the draws' D3D12 calls replayed on a thread of their own
// (command_recorder.h). On by default; off: the draw path calls the frame's list itself, as before.
// Read at each frame's start.
REXCVAR_DEFINE_BOOL(pgr4_native_record_list, true, "PGR4 native",
                    "Record the draws' D3D12 calls and make them on the frame's list from a thread "
                    "of their own (the render thread no longer waits on the driver)");
// Local change (18/11/2026, Tazz: the render thread's load): the frame's submissions within it
// (--pgr4_native_submit_draws) made by the recorder's replay thread (CommandRecorder::SubmitList):
// the render thread submits the texture upload lists itself at once, ahead of the frame as they may
// always run, and records the frame list's submission. Off: Submit drains and submits, as before.
// Read at each frame's start.
REXCVAR_DEFINE_BOOL(pgr4_native_record_submit, true, "PGR4 native",
                    "Command recording: the submissions within a frame made by the replay thread");
// Development (18/11/2026): the recorder's bytes between two publishes and its replay thread's spin
// before it sleeps (0: the defaults, 4096 bytes and 10 us). Read at each frame's start.
REXCVAR_DEFINE_INT32(pgr4_native_record_publish_bytes, 0, "PGR4 native",
                     "Development: command recording, bytes recorded between two publishes (0: 4096)");
REXCVAR_DEFINE_INT32(pgr4_native_record_worker_spin_us, 0, "PGR4 native",
                     "Development: command recording, the replay thread's spin before it sleeps in "
                     "us (0: 10)");
REXCVAR_DEFINE_BOOL(pgr4_native_gpu_serialize, false, "PGR4 native",
                    "Debug: wait for the GPU at the end of every frame, so no frame is in flight "
                    "while the next one is recorded (slow)");
REXCVAR_DEFINE_BOOL(
    pgr4_native_hdr, true, "PGR4 native",
    "HDR10 output when the display is in HDR mode: the highlights the game renders above "
    "SDR white, which its front buffer clips, reach the screen");
REXCVAR_DEFINE_INT32(pgr4_native_hdr_peak_nits, 0, "PGR4 native",
                     "HDR output: the peak luminance in nits the image is fitted to; 0: the "
                     "display's, or 1000 when it reports none");
REXCVAR_DECLARE(int32_t, d3d12_queue_priority);
// Local change (pgr4 project), debug: PIX's GPU capturer loaded before the device, so that
// pixtool or PIX can attach to the running game and take a GPU capture.
REXCVAR_DEFINE_BOOL(pgr4_native_d3d12_debug, false, "PGR4 native",
                    "Debug: D3D12's debug layer, its errors and warnings in the log (slower)");
REXCVAR_DEFINE_BOOL(pgr4_native_pix, false, "PGR4 native",
                    "Debug: load PIX's WinPixGpuCapturer.dll (the newest under "
                    "%ProgramFiles%\\Microsoft PIX) before the device, for GPU captures");

namespace pgr4::native {

namespace {
constexpr UINT_PTR kTitleTimer = 1;
const wchar_t kClassName[] = L"pgr4_native_debug";
std::atomic<uint32_t> g_gpu_vendor{0x10DE};
}  // namespace

uint32_t GpuVendorId() { return g_gpu_vendor; }

bool Failed(HRESULT hr, const char* what) {
  if (SUCCEEDED(hr)) return false;
  REXLOG_ERROR("PGR4 native: {} failed: HRESULT {:08X}", what, uint32_t(hr));
  return true;
}

bool CreateBuffer(ID3D12Device* device, D3D12_HEAP_TYPE type, uint64_t size,
                  D3D12_RESOURCE_STATES state, const char* what, ComPtr<ID3D12Resource>* out) {
  const D3D12_HEAP_PROPERTIES heap = {type};
  const D3D12_RESOURCE_DESC desc = BufferDesc(size);
  return !Failed(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                                 IID_PPV_ARGS(out->ReleaseAndGetAddressOf())),
                 what);
}

pD3DCompile D3DCompiler() {
  static const pD3DCompile compile = [] {
    const HMODULE dll = LoadLibraryW(L"d3dcompiler_47.dll");
    const auto fn =
        dll ? reinterpret_cast<pD3DCompile>(GetProcAddress(dll, "D3DCompile")) : nullptr;
    if (!fn) REXLOG_ERROR("PGR4 native: d3dcompiler_47.dll / D3DCompile not found");
    return fn;
  }();
  return compile;
}

namespace {
void ReportRemoved(ID3D12Device* device) {
  std::string out = fmt::format("device removed, reason {:08X}\n", uint32_t(device->GetDeviceRemovedReason()));
  ComPtr<ID3D12DeviceRemovedExtendedData> dred;
  if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dred)))) {
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT crumbs = {};
    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&crumbs))) {
      for (const D3D12_AUTO_BREADCRUMB_NODE* node = crumbs.pHeadAutoBreadcrumbNode; node; node = node->pNext) {
        const uint32_t done = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
        if (done >= node->BreadcrumbCount) continue;  // finished: not what hung
        out += fmt::format("list {} queue {}: {} of {} operations done; operations around it:",
                           node->pCommandListDebugNameA ? node->pCommandListDebugNameA : "?",
                           node->pCommandQueueDebugNameA ? node->pCommandQueueDebugNameA : "?", done,
                           node->BreadcrumbCount);
        const uint32_t from = done > 8 ? done - 8 : 0;
        for (uint32_t i = from; i < node->BreadcrumbCount && i < done + 8; ++i)
          out += i == done ? fmt::format(" [{}]", int(node->pCommandHistory[i]))
                         : fmt::format(" {}", int(node->pCommandHistory[i]));
        out += "\n";
      }
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT fault = {};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&fault))) {
      out += fmt::format("page fault at {:016X}\n", fault.PageFaultVA);
      const auto list = [&out](const char* what, const D3D12_DRED_ALLOCATION_NODE* node) {
        for (int count = 0; node && count < 16; node = node->pNext, ++count)
          out += fmt::format("{}: {} (type {})\n", what, node->ObjectNameA ? node->ObjectNameA : "?",
                             int(node->AllocationType));
      };
      list("existing", fault.pHeadExistingAllocationNode);
      list("recently freed", fault.pHeadRecentFreedAllocationNode);
    }
  }
  REXLOG_ERROR("PGR4 native DRED: {}", out);
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  if (std::FILE* f = _wfopen((std::filesystem::path(exe).parent_path() / "dred.txt").c_str(), L"a")) {
    std::fputs(out.c_str(), f);
    std::fclose(f);
  }
}
}  // namespace

D3D12Device::~D3D12Device() {
  WaitIdle();
  // Local change: Streamline goes before the swapchain, queue and device it hooked.
  streamline::Shutdown();
  if (HWND h = hwnd_.load()) PostMessageW(h, WM_CLOSE, 1, 0);
  if (window_thread_.joinable()) window_thread_.join();
  if (spacing_timer_) CloseHandle(spacing_timer_);
}

LRESULT CALLBACK D3D12Device::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  auto* self = reinterpret_cast<D3D12Device*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_ERASEBKGND:
      return 1;  // the swapchain covers the client area
    case WM_TIMER:
      if (self && wp == kTitleTimer) {
        wchar_t title[96];
        std::swprintf(title, 96, L"PGR4 native (debug) - frame %llu",
                      (unsigned long long)self->title_frame_.load(std::memory_order_relaxed));
        SetWindowTextW(hwnd, title);
      }
      return 0;
    case WM_CLOSE:
      // The window lives as long as the renderer: the user's close button is ignored, the
      // destructor's WM_CLOSE (wp == 1) destroys it.
      if (wp == 1) DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(hwnd, msg, wp, lp);
  }
}

bool D3D12Device::CreateWindowThread(const wchar_t* title) {
  std::mutex m;
  std::condition_variable cv;
  bool done = false;
  window_thread_ = std::thread([&, title] {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW (no UNICODE here)
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    RECT r = {0, 0, LONG(width_), LONG(height_)};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    // The debug window is owned by a hidden window, so it never becomes the process's "main
    // window" (.NET MainWindowHandle: unowned and visible), which input scripts focus and send
    // keys to. It never activates
    // either: a window manager that focuses the newest window (gamescope) would otherwise hand
    // it the game's keyboard input (Wine activates a window on WM_TAKE_FOCUS unless it is
    // WS_EX_NOACTIVATE).
    HWND owner =
        CreateWindowExW(0, kClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_NOACTIVATE, kClassName, title,
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                                r.bottom - r.top, owner, nullptr, inst, nullptr);
    if (hwnd) {
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
      ShowWindow(hwnd, SW_SHOWNOACTIVATE);
      SetTimer(hwnd, kTitleTimer, 500, nullptr);
    }
    {
      // Notified under the lock: once it sees done, the waiter returns and destroys cv.
      std::lock_guard lock(m);
      hwnd_.store(hwnd);
      done = true;
      cv.notify_one();
    }
    if (!hwnd) return;
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    hwnd_.store(nullptr);
  });
  std::unique_lock lock(m);
  cv.wait(lock, [&] { return done; });
  if (!hwnd_.load()) {
    REXLOG_ERROR("PGR4 native: CreateWindowExW failed ({})", GetLastError());
    return false;
  }
  return true;
}

bool D3D12Device::Initialize(uint32_t width, uint32_t height, const wchar_t* title,
                             HWND game_window) {
  width_ = width;
  height_ = height;
  game_window_ = game_window;
  // Local change (pgr4 project), debug: D3D12's debug layer (pgr4_native_d3d12_debug), its
  // messages logged (InfoQueue callback, after the device): wrong resource states, barriers.
  if (REXCVAR_GET(pgr4_native_d3d12_debug)) {
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
      debug->EnableDebugLayer();
      REXLOG_INFO("PGR4 native: D3D12 debug layer on");
    } else {
      REXLOG_ERROR("PGR4 native: D3D12 debug layer unavailable (Graphics Tools not installed?)");
    }
  }
  // Local change (pgr4 project), debug: PIX's capturer first (pgr4_native_pix).
  if (REXCVAR_GET(pgr4_native_pix) && !GetModuleHandleW(L"WinPixGpuCapturer.dll")) {
    wchar_t pf[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH)) {
      std::filesystem::path newest;
      std::error_code ec;
      for (const auto& e :
           std::filesystem::directory_iterator(std::filesystem::path(pf) / L"Microsoft PIX", ec))
        if (std::filesystem::exists(e.path() / L"WinPixGpuCapturer.dll") && e.path() > newest)
          newest = e.path();
      if (!newest.empty() && LoadLibraryW((newest / L"WinPixGpuCapturer.dll").c_str()))
        REXLOG_INFO("PGR4 native: PIX GPU capturer loaded from {}", newest.string());
      else
        REXLOG_ERROR("PGR4 native: PIX GPU capturer not found or not loaded");
    }
  }
  // Local change (pgr4 project): Streamline before any of the device's DXGI/D3D12 objects.
  // Always tried (31/10/2026): loaded only with --pgr4_native_dlss, a start with FSR or MSAA
  // chosen left DLSS out of the Graphics page until it was set by hand. A GPU without DLSS drops
  // it at SetDevice.
  streamline::Init();
  if (Failed(CreateDXGIFactory2(0, IID_PPV_ARGS(factory_.ReleaseAndGetAddressOf())),
             "CreateDXGIFactory2"))
    return false;
  ComPtr<IDXGIAdapter1> adapter;
  if (Failed(factory_->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                  IID_PPV_ARGS(adapter.GetAddressOf())),
             "EnumAdapterByGpuPreference"))
    return false;
  if (REXCVAR_GET(pgr4_native_dred)) {
    ComPtr<ID3D12DeviceRemovedExtendedDataSettings> settings;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&settings)))) {
      settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
      settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
      REXLOG_INFO("PGR4 native: DRED on");
    }
  }
  if (Failed(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                               IID_PPV_ARGS(device_.ReleaseAndGetAddressOf())),
             "D3D12CreateDevice"))
    return false;
  // A removed device signals every fence to the maximum: the report, whichever thread is stopped by it.
  if (REXCVAR_GET(pgr4_native_dred)) {
    ComPtr<ID3D12Fence> removed;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event && SUCCEEDED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&removed))) &&
        SUCCEEDED(removed->SetEventOnCompletion(UINT64_MAX, event))) {
      std::thread([device = device_, removed, event] {
        SetThreadDescription(GetCurrentThread(), L"PGR4 native DRED");
        WaitForSingleObject(event, INFINITE);
        if (FAILED(device->GetDeviceRemovedReason())) ReportRemoved(device.Get());
      }).detach();
    }
  }
  // Local change (pgr4 project), debug: the debug layer's messages into the log, the first 400
  // of each ID (pgr4_native_d3d12_debug).
  if (REXCVAR_GET(pgr4_native_d3d12_debug)) {
    ComPtr<ID3D12InfoQueue1> info;
    if (SUCCEEDED(device_.As(&info))) {
      DWORD cookie = 0;
      info->RegisterMessageCallback(
          [](D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id,
             LPCSTR description, void*) {
            if (severity > D3D12_MESSAGE_SEVERITY_WARNING) return;
            static std::mutex m;
            static std::unordered_map<int, int> seen;
            std::lock_guard<std::mutex> lock(m);
            if (++seen[int(id)] > 400) return;
            REXLOG_WARN("PGR4 native D3D12 debug [{} #{}]: {}", int(id), seen[int(id)],
                        description);
          },
          D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie);
      REXLOG_INFO("PGR4 native: D3D12 debug messages logged");
    } else {
      REXLOG_WARN("PGR4 native: no ID3D12InfoQueue1 (debug messages not logged)");
    }
  }
  // Streamline's present queue and swapchain come from proxies of the device and the factory;
  // every other call stays on the native interfaces (manual hooking).
  ID3D12Device* queue_device = device_.Get();
  if (streamline::Active() && streamline::SetDevice(device_.Get())) {
    void* d = device_.Get();
    void* f = factory_.Get();
    if (streamline::Upgrade(&d) && streamline::Upgrade(&f)) {
      queue_device = static_cast<ID3D12Device*>(d);
      sl_factory_ = static_cast<IDXGIFactory6*>(f);
    } else {
      streamline::Shutdown();
    }
  }
  DXGI_ADAPTER_DESC1 ad = {};
  adapter->GetDesc1(&ad);
  char name[128] = {};
  WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
  g_gpu_vendor = ad.VendorId;

  // The Xenos plugin's priority (d3d12_provider.cpp): its queue still submits at every guest
  // swap, and vkd3d-proton submits a queue's lists one at a time, each after the last finished,
  // while a queue of higher priority is active (staggered submissions, command.c).
  constexpr D3D12_COMMAND_QUEUE_PRIORITY kPriorities[] = {
      D3D12_COMMAND_QUEUE_PRIORITY_NORMAL, D3D12_COMMAND_QUEUE_PRIORITY_HIGH,
      D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME};
  D3D12_COMMAND_QUEUE_DESC qd = {};
  qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  qd.Priority = kPriorities[std::clamp(REXCVAR_GET(d3d12_queue_priority), 0, 2)];
  HRESULT hr = queue_device->CreateCommandQueue(&qd, IID_PPV_ARGS(queue_.GetAddressOf()));
  // Realtime needs a privilege; the plugin falls back to high without it.
  if (FAILED(hr) && qd.Priority == D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME) {
    qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    hr = queue_device->CreateCommandQueue(&qd, IID_PPV_ARGS(queue_.GetAddressOf()));
  }
  if (Failed(hr, "CreateCommandQueue")) return false;
  queue_->SetName(L"pgr4 native queue");
  if (Failed(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence_.GetAddressOf())),
             "CreateFence") ||
      Failed(
          device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(idle_fence_.GetAddressOf())),
          "CreateFence(idle)"))
    return false;
  fence_event_ = CreateAutoResetEvent();
  idle_event_ = CreateAutoResetEvent();
  if (!fence_event_ || !idle_event_) {
    REXLOG_ERROR("PGR4 native: CreateEvent failed: error {}", GetLastError());
    return false;
  }
  for (auto& a : allocators_)
    if (Failed(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               IID_PPV_ARGS(a.GetAddressOf())),
               "CreateCommandAllocator"))
      return false;
  if (Failed(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators_[0].Get(),
                                        nullptr, IID_PPV_ARGS(list_.GetAddressOf())),
             "CreateCommandList"))
    return false;
  list_->SetName(L"pgr4 native frame");
  if (!CreateBusyTimer()) return false;
  StampBusy();
  UpdateRecording();  // local change

  BOOL tearing = FALSE;
  factory_->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing));
  tearing_ = tearing != FALSE;
  swapchain_flags_ = (tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0) |
                     (latency_waitable_ ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0);
  D3D12_DESCRIPTOR_HEAP_DESC hd = {};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  hd.NumDescriptors = kBackBuffers;
  if (Failed(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(rtv_heap_.GetAddressOf())),
             "CreateDescriptorHeap(RTV)"))
    return false;
  rtv_stride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  if (!game_window_ && !(CreateWindowThread(title) && CreateSwapchain(hwnd_.load()))) return false;
  REXLOG_INFO("PGR4 native: D3D12 on {} (feature level 11_0+), presenting into the {}, tearing {}",
              name, game_window_ ? "game window" : "debug window", tearing_ ? "on" : "off");
  return true;
}

bool D3D12Device::AttachSwapchain() {
  RECT r = {};
  GetClientRect(game_window_, &r);
  width_ = uint32_t((std::max)(LONG(64), r.right - r.left));
  height_ = uint32_t((std::max)(LONG(64), r.bottom - r.top));
  if (!CreateSwapchain(game_window_)) return false;
  REXLOG_INFO("PGR4 native: swapchain {}x{} in the game window", width_, height_);
  return true;
}

void D3D12Device::DetachSwapchain() {
  WaitIdle();
  for (auto& b : back_buffers_) b.Reset();
  swapchain_.Reset();
  REXLOG_INFO("PGR4 native: swapchain released");
}

// Local change (pgr4 project): is the display the window is on in HDR mode (its output's color
// space, as SetUpColorSpace reads it)? Unknown: no.
static bool DisplayInHdrMode(IDXGIFactory6* factory, HWND hwnd) {
  const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
  ComPtr<IDXGIAdapter1> adapter;
  for (UINT a = 0; SUCCEEDED(factory->EnumAdapters1(a, adapter.ReleaseAndGetAddressOf())); ++a) {
    ComPtr<IDXGIOutput> output;
    for (UINT o = 0; SUCCEEDED(adapter->EnumOutputs(o, output.ReleaseAndGetAddressOf())); ++o) {
      ComPtr<IDXGIOutput6> output6;
      DXGI_OUTPUT_DESC1 od = {};
      if (SUCCEEDED(output.As(&output6)) && SUCCEEDED(output6->GetDesc1(&od)) &&
          od.Monitor == monitor)
        return od.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    }
  }
  return false;
}

bool D3D12Device::CreateSwapchain(HWND hwnd) {
  DXGI_SWAP_CHAIN_DESC1 sd = {};
  sd.Width = width_;
  sd.Height = height_;
  // Local change (pgr4 project): the format it will keep from its creation. A swapchain created in
  // HDR10's format and resized to SDR's on an SDR display: no resize when none is needed.
  sd.Format = REXCVAR_GET(pgr4_native_hdr) && DisplayInHdrMode(factory_.Get(), hwnd)
                  ? kHdrBackBufferFormat
                  : kBackBufferFormat;
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = kBackBuffers;
  sd.Scaling = DXGI_SCALING_STRETCH;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  sd.Flags = swapchain_flags_;
  ComPtr<IDXGISwapChain1> sc1;
  IDXGIFactory6* const swapchain_factory = sl_factory_ ? sl_factory_ : factory_.Get();
  if (Failed(swapchain_factory->CreateSwapChainForHwnd(queue_.Get(), hwnd, &sd, nullptr, nullptr,
                                              sc1.GetAddressOf()),
             "CreateSwapChainForHwnd"))
    return false;
  // Fullscreen is the window's own (SDL's in the game window).
  factory_->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
  if (Failed(sc1.As(&swapchain_), "IDXGISwapChain3") || !SetUpColorSpace()) return false;
  return GetBackBuffers();
}

bool D3D12Device::SetUpColorSpace() {
  constexpr DXGI_COLOR_SPACE_TYPE kHdr10 = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
  const bool wanted = REXCVAR_GET(pgr4_native_hdr);
  ComPtr<IDXGIOutput> output;
  ComPtr<IDXGIOutput6> output6;
  DXGI_OUTPUT_DESC1 od = {};
  const bool described = SUCCEEDED(swapchain_->GetContainingOutput(&output)) &&
                         SUCCEEDED(output.As(&output6)) && SUCCEEDED(output6->GetDesc1(&od));
  // HDR10 only while the display is in HDR mode, which its output's color space tells (as in
  // Microsoft's D3D12HDR sample). Windows offers HDR10 to any 10-bit swapchain and shows it on
  // an SDR display clipped at SDR white. DXVK reports HDR10 there with DXVK_HDR=1, and its
  // swapchain offers it only for a surface that takes it, as gamescope's in HDR mode.
  UINT support = 0;
  hdr_ = wanted && described && od.ColorSpace == kHdr10 &&
         SUCCEEDED(swapchain_->CheckColorSpaceSupport(kHdr10, &support)) &&
         (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) &&
         !Failed(swapchain_->SetColorSpace1(kHdr10), "SetColorSpace1(HDR10)");
  if (!hdr_) {
    REXLOG_INFO("PGR4 native: SDR output{}", wanted ? " (the display is not in HDR mode)" : "");
    DXGI_SWAP_CHAIN_DESC1 current = {};
    if (FAILED(swapchain_->GetDesc1(&current)) || current.Format == kBackBufferFormat) return true;
    return !wanted || !Failed(swapchain_->ResizeBuffers(kBackBuffers, width_, height_,
                                                        kBackBufferFormat, swapchain_flags_),
                              "ResizeBuffers(SDR)");
  }
  const float display_peak = od.MaxLuminance;
  const int32_t peak = REXCVAR_GET(pgr4_native_hdr_peak_nits);
  hdr_peak_nits_ = peak > 0 ? float(peak) : display_peak > 0.0f ? display_peak : 1000.0f;
  // The image is fitted to the peak already, so the display has nothing left to tone map.
  // Primaries: the game's, BT.709 with a D65 white, in units of 0.00002.
  DXGI_HDR_METADATA_HDR10 md = {};
  md.RedPrimary[0] = 32000, md.RedPrimary[1] = 16500;
  md.GreenPrimary[0] = 15000, md.GreenPrimary[1] = 30000;
  md.BluePrimary[0] = 7500, md.BluePrimary[1] = 3000;
  md.WhitePoint[0] = 15635, md.WhitePoint[1] = 16450;
  md.MaxMasteringLuminance = UINT(hdr_peak_nits_);
  md.MaxContentLightLevel = UINT16(std::min(hdr_peak_nits_, 65535.0f));
  ComPtr<IDXGISwapChain4> sc4;
  if (SUCCEEDED(swapchain_.As(&sc4)))
    Failed(sc4->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, sizeof(md), &md), "SetHDRMetaData");
  REXLOG_INFO("PGR4 native: HDR10 output, peak {:.0f} nits (the display reports {:.0f})",
              hdr_peak_nits_, display_peak);
  return true;
}

HANDLE D3D12Device::TakeFrameLatencyWaitable(uint32_t max_latency) {
  if (!latency_waitable_) return nullptr;
  // The count must hold every release the waiter has not taken yet (DXGI bounds it by this).
  if (Failed(swapchain_->SetMaximumFrameLatency(max_latency), "SetMaximumFrameLatency"))
    return nullptr;
  return swapchain_->GetFrameLatencyWaitableObject();
}

bool D3D12Device::GetBackBuffers() {
  for (uint32_t i = 0; i < kBackBuffers; ++i) {
    if (Failed(swapchain_->GetBuffer(i, IID_PPV_ARGS(back_buffers_[i].ReleaseAndGetAddressOf())),
               "GetBuffer"))
      return false;
    D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += size_t(i) * rtv_stride_;
    device_->CreateRenderTargetView(back_buffers_[i].Get(), nullptr, h);
  }
  back_index_ = swapchain_->GetCurrentBackBufferIndex();
  return true;
}

// The game window's client size changed (window resize, fullscreen toggle): the swapchain
// buffers follow; a minimized window (0x0) is skipped.
bool D3D12Device::FollowWindowSize() {
  if (!game_window_ || !swapchain_) return true;
  RECT r = {};
  if (!GetClientRect(game_window_, &r)) return true;
  const uint32_t w = uint32_t(r.right - r.left), h = uint32_t(r.bottom - r.top);
  if (w < 64 || h < 64 || (w == width_ && h == height_)) return true;
  WaitIdle();
  for (auto& b : back_buffers_) b.Reset();
  if (Failed(swapchain_->ResizeBuffers(kBackBuffers, w, h,
                                       hdr_ ? kHdrBackBufferFormat : kBackBufferFormat,
                                       swapchain_flags_),
             "ResizeBuffers"))
    return false;
  REXLOG_INFO("PGR4 native: game window client {}x{} -> {}x{}", width_, height_, w, h);
  width_ = w;
  height_ = h;
  return GetBackBuffers();
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Device::back_buffer_rtv() const {
  if (frame_image_) return frame_image_rtv_;
  D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
  h.ptr += size_t(back_index_) * rtv_stride_;
  return h;
}

void D3D12Device::WaitFence(uint64_t value) {
  if (fence_->GetCompletedValue() >= value) return;
  fence_->SetEventOnCompletion(value, fence_event_.get());
  WaitForSingleObject(fence_event_.get(), INFINITE);
}

void D3D12Device::WaitIdle() {
  // A fence of its own: the frame fence's next value belongs to the frame still being recorded,
  // whose resources would count as done before it is submitted.
  if (!queue_ || !idle_fence_ || !idle_event_) return;
  // Local change: what the draw path recorded is in the list, so nothing names a resource the
  // caller frees after this.
  if (recorder_) recorder_->Drain();
  const uint64_t v = ++idle_fence_value_;
  if (FAILED(queue_->Signal(idle_fence_.Get(), v)) || idle_fence_->GetCompletedValue() >= v) return;
  idle_fence_->SetEventOnCompletion(v, idle_event_.get());
  WaitForSingleObject(idle_event_.get(), INFINITE);
}

bool D3D12Device::EnableGpuTimer() {
  if (recorder_) recorder_->Drain();  // local change: its first stamp after the recorded draws
  auto timer = std::make_unique<GpuTimer>();
  if (!timer->Initialize(device_.Get(), queue_.Get(), kFramesInFlight)) return false;
  timer->BeginFrame(list_.Get(), slot_);
  gpu_timer_ = std::move(timer);
  return true;
}

bool D3D12Device::CreateBusyTimer() {
  UINT64 frequency = 0;
  if (Failed(queue_->GetTimestampFrequency(&frequency), "GetTimestampFrequency") || !frequency)
    return false;
  busy_ns_per_tick_ = 1e9 / double(frequency);
  D3D12_QUERY_HEAP_DESC qd = {};
  qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
  qd.Count = kFramesInFlight * kBusySpans * 2;
  return !Failed(device_->CreateQueryHeap(&qd, IID_PPV_ARGS(busy_heap_.GetAddressOf())),
                 "CreateQueryHeap(busy)") &&
         CreateBuffer(device_.Get(), D3D12_HEAP_TYPE_READBACK, uint64_t(qd.Count) * 8,
                      D3D12_RESOURCE_STATE_COPY_DEST, "busy timestamp readback", &busy_readback_);
}

void D3D12Device::StampBusy() {
  list_->EndQuery(busy_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                  slot_ * kBusySpans * 2 + busy_stamps_[slot_]++);
}

void D3D12Device::ReadBusy() {
  const uint32_t n = std::exchange(busy_stamps_[slot_], 0);
  if (n < 2) return;
  const size_t first = size_t(slot_) * kBusySpans * 2;
  const D3D12_RANGE range = {first * 8, (first + n) * 8};
  void* data = nullptr;
  if (FAILED(busy_readback_->Map(0, &range, &data))) return;
  const uint64_t* ts = static_cast<const uint64_t*>(data) + first;
  uint64_t ticks = 0;
  for (uint32_t i = 0; i + 1 < n; i += 2) ticks += ts[i + 1] > ts[i] ? ts[i + 1] - ts[i] : 0;
  const D3D12_RANGE none = {};
  busy_readback_->Unmap(0, &none);
  gpu_busy_ns_ = uint64_t(double(ticks) * busy_ns_per_tick_);
}

bool D3D12Device::OpenList() {
  const uint64_t t0 = NowNs();
  WaitFence(slot_fence_[slot_]);
  end_frame_times_.fence_ns = NowNs() - t0;
  ReadBusy();
  if (Failed(allocators_[slot_]->Reset(), "CommandAllocator::Reset") ||
      Failed(list_->Reset(allocators_[slot_].Get(), nullptr), "CommandList::Reset"))
    return false;
  StampBusy();
  if (gpu_timer_) gpu_timer_->BeginFrame(list_.Get(), slot_);
  UpdateRecording();  // local change
  return true;
}

void D3D12Device::UpdateRecording() {
  const bool want = REXCVAR_GET(pgr4_native_record_list);
  if (want && !recorder_) {
    auto recorder = std::make_unique<CommandRecorder>();
    if (recorder->Start(list_.Get())) recorder_ = std::move(recorder);
  }
  const bool on = want && recorder_;
  if (recorder_)
    recorder_->SetTuning(uint32_t(std::max(REXCVAR_GET(pgr4_native_record_publish_bytes), 0)),
                         uint32_t(std::max(REXCVAR_GET(pgr4_native_record_worker_spin_us), 0)) * 1000u);
  submit_on_replay_ = on && REXCVAR_GET(pgr4_native_record_submit);
  if (on != recording_)
    REXLOG_INFO("PGR4 native: command recording {}",
                on ? "on (the draws' D3D12 calls made from the replay thread)" : "off");
  recording_ = on;
}

bool D3D12Device::CloseAndExecute(std::span<ID3D12CommandList* const> first) {
  if (Failed(list_->Close(), "CommandList::Close")) return false;
  submit_lists_.assign(first.begin(), first.end());
  submit_lists_.push_back(list_.Get());
  // The constant uploads are streaming stores, which are weakly ordered: make them visible
  // before the GPU can read them.
  simde_mm_sfence();
  queue_->ExecuteCommandLists(UINT(submit_lists_.size()), submit_lists_.data());
  return true;
}

bool D3D12Device::Submit(std::span<ID3D12CommandList* const> first) {
  // Local change (pgr4_native_record_submit): by the replay thread, after what was recorded before.
  if (submit_on_replay_ && recording_ && !gpu_timer_) {
    // The constant uploads are streaming stores, weakly ordered: visible before the GPU reads them.
    simde_mm_sfence();
    // The texture upload lists now, alone: they run ahead of the frame (TextureCache::
    // CloseUploadLists), and their threads may reopen them before the replay would reach them.
    if (!first.empty()) queue_->ExecuteCommandLists(UINT(first.size()), first.data());
    const bool timed = busy_stamps_[slot_] + 3 <= kBusySpans * 2;
    if (timed)
      recorder_->EndQuery(busy_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                          slot_ * kBusySpans * 2 + busy_stamps_[slot_]++);
    recorder_->SubmitList(queue_.Get(), allocators_[slot_].Get());
    if (timed)
      recorder_->EndQuery(busy_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                          slot_ * kBusySpans * 2 + busy_stamps_[slot_]++);
    return !recorder_->failed();
  }
  if (recorder_) recorder_->Drain();  // local change: the draws recorded so far go too
  if (gpu_timer_) gpu_timer_->Pass(list_.Get(), GpuTimer::KeySubmit());
  // Room for this submission's end, the next one's start and the frame's end.
  const bool timed = busy_stamps_[slot_] + 3 <= kBusySpans * 2;
  if (timed) StampBusy();
  // A list may be reset while its submission runs; the allocator only once the frame's fence
  // completed (OpenList).
  if (!CloseAndExecute(first) ||
      Failed(list_->Reset(allocators_[slot_].Get(), nullptr), "CommandList::Reset"))
    return false;
  if (timed) StampBusy();
  return true;
}

void D3D12Device::CopyToBackBuffer(ID3D12Resource* image) {
  D3D12_RESOURCE_BARRIER bs[2] = {
      TransitionBarrier(image, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE),
      TransitionBarrier(back_buffers_[back_index_].Get(), D3D12_RESOURCE_STATE_PRESENT,
                        D3D12_RESOURCE_STATE_COPY_DEST)};
  list_->ResourceBarrier(2, bs);
  list_->CopyResource(back_buffers_[back_index_].Get(), image);
  for (D3D12_RESOURCE_BARRIER& b : bs) std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
  list_->ResourceBarrier(2, bs);
}

bool D3D12Device::Present() {
  // Local change: Reflex/PCL present markers around it (DLSS-G presents its frames from here).
  streamline::Marker(sl::PCLMarker::ePresentStart);
  const HRESULT hr = vsync_ ? swapchain_->Present(present_interval_, 0)
                            : swapchain_->Present(0, tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0);
  streamline::Marker(sl::PCLMarker::ePresentEnd);
  if (!Failed(hr, "Present")) return true;
  if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
    Failed(device_->GetDeviceRemovedReason(), "device removed, reason");
  return false;
}

bool D3D12Device::EndFrame(bool present, std::span<ID3D12CommandList* const> first) {
  if (recorder_) {
    recorder_->Drain();  // local change: the frame's recorded draws (and submissions)
    if (recorder_->failed()) {
      REXLOG_ERROR("PGR4 native: a submission by the command recorder's replay thread failed");
      return false;
    }
  }
  const uint64_t t0 = NowNs();
  streamline::Marker(sl::PCLMarker::eRenderSubmitStart);  // local change
  const bool shown = swapchain_ && present;
  ID3D12Resource* const image = std::exchange(frame_image_, nullptr);
  ID3D12Resource* const generated = std::exchange(generated_frame_, nullptr);
  // The generated frame goes first; the frame's image then into the back buffer after it.
  if (shown && image) CopyToBackBuffer(generated ? generated : image);
  if (gpu_timer_) gpu_timer_->EndFrame(list_.Get());
  StampBusy();
  list_->ResolveQueryData(busy_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot_ * kBusySpans * 2,
                          busy_stamps_[slot_], busy_readback_.Get(),
                          uint64_t(slot_) * kBusySpans * 2 * 8);
  if (!CloseAndExecute(first)) return false;
  streamline::Marker(sl::PCLMarker::eRenderSubmitEnd);  // local change
  const uint64_t t1 = NowNs();
  bool ok = !shown || Present();
  if (ok && shown && generated) {
    const uint64_t generated_ns = NowNs();
    back_index_ = swapchain_->GetCurrentBackBufferIndex();
    ok = !Failed(list_->Reset(allocators_[slot_].Get(), nullptr), "CommandList::Reset");
    if (ok) CopyToBackBuffer(image);
    ok = ok && CloseAndExecute({});
    // The frame's image half the limit's interval after the generated one (set_generated_spacing).
    // The render thread waits there, so the next frame begins only after it: with the whole half interval,
    // a frame took its recording plus that half, over the limit's interval (120 fell to 90 with 13 ms of
    // recording). What the interval leaves after the time since the last frame's image, at most the half.
    uint64_t spacing = generated_spacing_ns_;
    if (spacing && last_image_present_ns_) {
      const uint64_t since = generated_ns - last_image_present_ns_;
      spacing = since < 2 * spacing ? std::min(spacing, 2 * spacing - since) : 0;
    }
    if (ok && spacing) {
      if (!spacing_timer_)
        spacing_timer_ = CreateWaitableTimerExW(nullptr, nullptr,
                                                CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                TIMER_ALL_ACCESS);
      const uint64_t now = NowNs();
      if (spacing_timer_ && generated_ns + spacing > now) {
        LARGE_INTEGER due;
        due.QuadPart = -int64_t((generated_ns + spacing - now + 99) / 100);
        if (SetWaitableTimer(spacing_timer_, &due, 0, nullptr, nullptr, FALSE))
          WaitForSingleObject(spacing_timer_, INFINITE);
      }
    }
    ok = ok && Present();
    last_image_present_ns_ = NowNs();
  }
  end_frame_times_.submit_ns = t1 - t0;
  end_frame_times_.present_ns = NowNs() - t1;
  if (!ok) return false;
  end_frame_times_.presented = shown;
  end_frame_times_.frame_presents = shown ? 1 + (generated != nullptr) : 0;
  end_frame_times_.presents += end_frame_times_.frame_presents;
  uint64_t v = next_fence_++;
  if (Failed(queue_->Signal(fence_.Get(), v), "CommandQueue::Signal")) return false;
  slot_fence_[slot_] = v;
  // Local change (pgr4 project): with DLSS frame generation on, each frame completes before the next
  // one's recording. With the GPU saturated, their overlap gave the cars' cube a wrong top face (a sky
  // reflected on the rear glass, on and off with the load, 04/11/2026; which data, unknown). About 2-3 %
  // of the frame rate in heavy scenes.
  if (REXCVAR_GET(pgr4_native_gpu_serialize) ||
      (REXCVAR_GET(pgr4_native_gpu_serialize_fg) && streamline::FrameGenerationOn()))
    WaitFence(v);
  slot_ = (slot_ + 1) % kFramesInFlight;
  if (swapchain_) back_index_ = swapchain_->GetCurrentBackBufferIndex();
  if (!FollowWindowSize()) return false;
  return OpenList();
}

}  // namespace pgr4::native
