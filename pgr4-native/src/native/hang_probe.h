// Local addition (22/11/2026, Tazz: the game froze 42 s at a race's reloading, nothing in the log):
// the freeze probe (pgr4_native_freeze_probe, draw.cpp). Each wait or long step of the renderer's
// threads names itself in its thread's slot (HangMark: what, a detail, since when); a watchdog
// thread sees the frames stop (5 s without one) and logs every thread's slot, then again every 5 s
// while it lasts, and its end. Development and release: a line only while frozen.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

#include "clock.h"  // local change (24/11/2026): NowNs

namespace pgr4::native {

enum class HangWhat : uint32_t {
  kNone,
  kTranslateWait,    // ShaderCache::Translate: another thread translates this shader
  kTranslating,      // ShaderCache::Translate: translating (detail: the shader)
  kTranslatorLock,   // ShaderCache::Translate: waiting for the cache's own translator
  kLoadWait,         // ShaderCache::Load: another thread analyzes this ucode
  kAnalyzing,        // ShaderCache::Load: analyzing (detail: the ucode)
  kFogEndWait,       // FindFogEnding: another thread scans this shader
  kFogEndScan,       // FindFogEnding: scanning
  kPipelineWait,     // WaitForPipelines (detail: pipelines pending)
  kPrewarmRoom,      // the prewarm's thread: the queue full
  kRaceLoadWait,     // a race's loading waits for the prewarm
  kPsoCreate,        // a pipeline worker: PipelineLibrary::Create (detail: 1 a prewarm job)
  kHlslCompile,      // ShaderReplacer: compiling our HLSL (detail: the shader)
  kGuestWaitFrames,  // the guest thread: waiting for the render thread's frames
  kGuestReadback,    // the guest thread: waiting for a read-back resolve
  kRenderWaitWork,   // the render thread: waiting for the guest's commands
  kGpuFence,         // D3D12Device::WaitFence (detail: the value)
  kGpuIdle,          // D3D12Device::WaitIdle
  kDrawing,          // DrawRenderer::Impl::Draw
  kVolume,           // Volumetrics::Apply (detail: 100 + its step early, 200 + at the resolve)
  kEncodeWaitWork,   // local change (24/11/2026): B, the encode thread, waiting for A's packets
  kEncodeWaitIdle,   // local change (24/11/2026): A waiting for B (detail: draw.cpp EncodeWait)
  kCount
};

inline const char* HangWhatName(uint32_t w) {
  static const char* const kNames[] = {
      "nothing marked",     "waiting for another's translation", "translating",
      "waiting for the cache's translator", "waiting for another's analysis", "analyzing ucode",
      "waiting for another's fog ending", "scanning a fog ending", "waiting for pipelines",
      "waiting for room in the prewarm queue", "a race's loading waiting for the prewarm",
      "creating a pipeline", "compiling our HLSL", "guest waiting for frames",
      "guest waiting for a read-back", "waiting for the guest's commands", "waiting for a GPU fence",
      "waiting for the GPU to idle", "drawing", "the volume's passes, step",
      "the encode thread waiting for packets", "waiting for the encode thread"};
  return w < sizeof(kNames) / sizeof(kNames[0]) ? kNames[w] : "?";
}

struct HangSlot {
  std::atomic<uint32_t> used{0};
  std::atomic<uint32_t> what{0};
  std::atomic<uint64_t> detail{0};
  std::atomic<int64_t> since_us{0};
  char name[48] = {};  // the thread's description, at its first mark
};
inline constexpr uint32_t kHangSlots = 96;
inline HangSlot g_hang_slots[kHangSlots];
inline std::atomic<uint32_t> g_hang_slot_count{0};
inline std::atomic<uint64_t> g_hang_frames{0};  // DrawRenderer::EndFrame
inline std::atomic<uint64_t> g_hang_draws{0};   // DrawRenderer::Impl::Draw

// Local change (24/11/2026, the CPU audit's T1, --pgr4_native_freeze_probe_light, set at each
// frame by draw.cpp's RefreshFrameFlags): the marks' time from the time-stamp counter (clock.h
// NowNs, a few ns; QueryPerformanceCounter is some 20-30 ns, about 150 ns under Wine) and the steps
// (HangStep) their detail alone, the time being the mark's. The same origin as steady_clock (both
// QueryPerformanceCounter's), so a slot written before a switch keeps a right "for N s".
inline std::atomic<bool> g_hang_light{true};
inline int64_t HangNowUs() {
  if (g_hang_light.load(std::memory_order_relaxed)) return int64_t(NowNs() / 1000);
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
// This thread's slot (named at its first use: draw.cpp), nullptr once all are taken.
HangSlot* HangRegisterThread();
inline HangSlot* HangThisThread() {
  thread_local HangSlot* slot = HangRegisterThread();
  return slot;
}

// The detail of this thread's current mark (a step within it), its time from now.
inline void HangStep(uint64_t detail);
// Names what this thread does until the end of the scope (the previous mark back after it).
class HangMark {
 public:
  explicit HangMark(HangWhat what, uint64_t detail = 0) : slot_(HangThisThread()) {
    if (!slot_) return;
    prev_what_ = slot_->what.load(std::memory_order_relaxed);
    prev_detail_ = slot_->detail.load(std::memory_order_relaxed);
    prev_since_ = slot_->since_us.load(std::memory_order_relaxed);
    slot_->detail.store(detail, std::memory_order_relaxed);
    slot_->since_us.store(HangNowUs(), std::memory_order_relaxed);
    slot_->what.store(uint32_t(what), std::memory_order_release);
  }
  ~HangMark() {
    if (!slot_) return;
    slot_->detail.store(prev_detail_, std::memory_order_relaxed);
    slot_->since_us.store(prev_since_, std::memory_order_relaxed);
    slot_->what.store(prev_what_, std::memory_order_release);
  }
  HangMark(const HangMark&) = delete;
  HangMark& operator=(const HangMark&) = delete;

 private:
  HangSlot* slot_;
  uint32_t prev_what_ = 0;
  uint64_t prev_detail_ = 0;
  int64_t prev_since_ = 0;
};

inline void HangStep(uint64_t detail) {
  if (HangSlot* s = HangThisThread()) {
    s->detail.store(detail, std::memory_order_relaxed);
    if (!g_hang_light.load(std::memory_order_relaxed))
      s->since_us.store(HangNowUs(), std::memory_order_relaxed);
  }
}

}  // namespace pgr4::native
