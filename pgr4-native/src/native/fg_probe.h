// Development probe (28/11/2026, Tazz: with FSR's frame generation or DLSS-G, the GPU at its limit
// after several races and replays, some frames show the road dry and grainy and no rain at all,
// for single frames or seconds at a time). Never in the release: only with the capture dir
// (g_pgr4_dev) and pgr4_native_fg_probe (renderer.cpp), read at each frame.
//
// Each shown frame: two bands of the frame's own image (the road's, 72-76 % of the scene's height,
// and the scene's middle, 40-44 %, both 30-70 % of its width) copied to a readback buffer, with the
// same bands of FSR's generated frame and of the HUD-less copy when the frame has them; their mean
// luma and spread once the GPU has passed them. With the frame's record: the game's rain and road
// draws (draw.cpp, Impl::Draw) and those drawn after the HUD-less copy, the draw the copy was taken
// before, the GPU's busy time, the video memory (local and non-local), the HD textures', the
// textures' and the process's memory.
//
// Faults found alone, each logged with the frame's record and the frame before's:
//   GEN OFF       FSR's generated frame outside its two drawn frames (its road band's luma or
//                 spread);
//   REAL JUMP     a drawn frame's road band away from both its neighbours, which agree;
//   HUDLESS OFF   the HUD-less copy's road band away from the frame's own;
//   COPY LATE     rain or road drawn after the HUD-less copy (the copy without them);
//   STALE IMAGE   the frame's bands the same bytes as the frame before's for 30 frames, the scene
//                 drawn (the capture reads an image nothing draws into).
// A fault arms whole-image dumps of the next two frames (frame_dump.h: fgprobe_<frame>_<what>.bin
// beside the executable), pgr4_native_fg_probe_dumps times at most.
//
// Threading: the thread that records the frames (NativeRenderer::ExecuteSwap).
#pragma once

#include <windows.h>
#include <psapi.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <rex/logging.h>

#include "d3d12_device.h"
#include "frame_dump.h"

namespace pgr4::native {

// The frame's game draws (draw.cpp fills it, main thread; reset when `frame` changes).
struct FgProbeDraws {
  uint64_t frame = UINT64_MAX;
  uint32_t draws = 0;                    // draws reaching the pipeline's choice
  uint32_t rain = 0, rain_vertices = 0;  // the rain's (kRainShader, any vertex shader)
  uint32_t rain_after = 0;               // ... drawn after the HUD-less copy
  uint32_t road = 0, road_after = 0;     // the roads' (Traits::road), and after the copy
  uint32_t copy_draw = UINT32_MAX;       // the draws before the copy (renderer.cpp)
};

// Memory at the frame (bytes; 0 unknown).
struct FgProbeMemory {
  uint64_t local = 0, local_budget = 0, nonlocal = 0, nonlocal_budget = 0;
  uint64_t hd = 0, textures = 0, private_bytes = 0;
};

// The process's private memory (bytes; 0 unknown).
inline uint64_t FgProbePrivateBytes() {
  PROCESS_MEMORY_COUNTERS_EX pmc = {};
  pmc.cb = sizeof(pmc);
  return K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                                 sizeof(pmc))
             ? uint64_t(pmc.PrivateUsage)
             : 0;
}

class FgProbe {
 public:
  // The memory, read every 30 frames by renderer.cpp (TextureCache::MemoryDev).
  FgProbeMemory memory;
  uint32_t memory_poll = 0;

  struct Frame {
    uint64_t frame = 0;
    bool generated = false, hudless = false, dlss_fg = false;
    FgProbeDraws draws;
    FgProbeMemory memory;
    double gpu_busy_ms = 0.0;
  };

  // After the frame's image is final (and FSR's generated frame recorded): copies the bands of
  // `image` (the frame's), `generated` and `hudless` (nullptr without) into `list`. All three in
  // PRESENT, left in it. `rect` the scene's part of the image.
  void Sample(ID3D12Device* device, ID3D12GraphicsCommandList* list, uint64_t fence,
              const Frame& info, ID3D12Resource* image, ID3D12Resource* generated,
              ID3D12Resource* hudless, uint32_t left, uint32_t top, uint32_t width, uint32_t height,
              int32_t max_dumps) {
    if (!image || width < 64 || height < 64) return;
    const D3D12_RESOURCE_DESC d = image->GetDesc();
    if (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM && d.Format != DXGI_FORMAT_R10G10B10A2_UNORM) return;
    // The armed dumps: whole images of this frame.
    if (dumps_armed_ > 0 && dumps_done_ < max_dumps) {
      --dumps_armed_;
      ++dumps_done_;
      const std::wstring base = L"fgprobe_" + std::to_wstring(info.frame);
      frame_dump::Capture(device, list, image, D3D12_RESOURCE_STATE_PRESENT, base + L"_image");
      if (generated)
        frame_dump::Capture(device, list, generated, D3D12_RESOURCE_STATE_PRESENT, base + L"_generated");
      if (hudless)
        frame_dump::Capture(device, list, hudless, D3D12_RESOURCE_STATE_PRESENT, base + L"_hudless");
      REXLOG_INFO("PGR4 fg probe: frame {} dumped ({}_image.bin{}{})", info.frame,
                  std::string(base.begin(), base.end()), generated ? ", _generated" : "",
                  hudless ? ", _hudless" : "");
    }
    const uint32_t band_w = std::min<uint32_t>(width * 2 / 5, 1024);
    const uint32_t x0 = left + width * 3 / 10;
    const uint32_t ys[kBands] = {top + height * 72 / 100, top + height * 40 / 100};
    const uint32_t row_pitch = (band_w * 4 + 255) & ~255u;
    const uint64_t band_bytes = (uint64_t(row_pitch) * kBandRows + 511) & ~511ull;
    Slot* slot = nullptr;
    for (Slot& s : slots_)
      if (!s.pending) {
        slot = &s;
        break;
      }
    if (!slot) {
      ++skipped_;
      return;
    }
    const uint64_t size = band_bytes * kBands * 3;
    if (!slot->readback || slot->size < size) {
      slot->readback.Reset();
      if (!CreateBuffer(device, D3D12_HEAP_TYPE_READBACK, size, D3D12_RESOURCE_STATE_COPY_DEST,
                        "fg probe readback", &slot->readback))
        return;
      slot->size = size;
    }
    ID3D12Resource* sources[3] = {image, generated, hudless};
    for (uint32_t k = 0; k < 3; ++k) {
      slot->has[k] = false;
      ID3D12Resource* src = sources[k];
      if (!src) continue;
      const D3D12_RESOURCE_DESC sd = src->GetDesc();
      if (sd.Format != d.Format || sd.Width < x0 + band_w || sd.Height < ys[0] + kBandRows) continue;
      D3D12_RESOURCE_BARRIER b = TransitionBarrier(src, D3D12_RESOURCE_STATE_PRESENT,
                                                   D3D12_RESOURCE_STATE_COPY_SOURCE);
      list->ResourceBarrier(1, &b);
      for (uint32_t band = 0; band < kBands; ++band) {
        D3D12_TEXTURE_COPY_LOCATION s = {}, t = {};
        s.pResource = src;
        s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        t.pResource = slot->readback.Get();
        t.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        t.PlacedFootprint.Offset = band_bytes * (k * kBands + band);
        t.PlacedFootprint.Footprint = {d.Format, band_w, kBandRows, 1, row_pitch};
        const D3D12_BOX box = {x0, ys[band], 0, x0 + band_w, ys[band] + kBandRows, 1};
        list->CopyTextureRegion(&t, 0, 0, 0, &s, &box);
      }
      std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
      list->ResourceBarrier(1, &b);
      slot->has[k] = true;
    }
    slot->pending = true;
    slot->fence = fence;
    slot->info = info;
    slot->format = d.Format;
    slot->band_w = band_w;
    slot->row_pitch = row_pitch;
    slot->band_bytes = band_bytes;
  }

  // Each frame: reads the bands the GPU has passed (in frame order), looks for faults, and every
  // 2 s logs the period's summary.
  void Poll(uint64_t completed_fence) {
    for (;;) {
      Slot* next = nullptr;
      for (Slot& s : slots_)
        if (s.pending && s.fence <= completed_fence && (!next || s.info.frame < next->info.frame))
          next = &s;
      if (!next) break;
      Read(*next);
      next->pending = false;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - last_summary_ >= std::chrono::seconds(2)) {
      last_summary_ = now;
      if (period_.frames) Summary();
      period_ = Period{};
    }
  }

 private:
  static constexpr uint32_t kBands = 2, kBandRows = 16, kSlots = 8;
  struct Stats {
    float mean = 0.0f, spread = 0.0f;
  };
  struct Slot {
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    uint64_t size = 0, fence = 0, band_bytes = 0;
    bool pending = false, has[3] = {};
    Frame info;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t band_w = 0, row_pitch = 0;
  };
  // A frame read back: its stats (image, generated, HUD-less; band 0 the road's), its record.
  struct Read_ {
    bool valid = false, has[3] = {};
    Stats s[3][kBands];
    uint64_t hash[kBands] = {};  // the image's bands' bytes (STALE IMAGE)
    Frame info;
  };
  struct Period {
    uint32_t frames = 0, generated = 0, hudless = 0, faults = 0, copy_late = 0, stale = 0;
    float road_lo[3] = {9, 9, 9}, road_hi[3] = {-1, -1, -1};
    uint32_t rain_frames = 0, no_rain_frames = 0;
  };

  static Stats BandStats(const uint8_t* p, uint32_t w, uint32_t pitch, DXGI_FORMAT f) {
    double sum = 0.0, sum2 = 0.0;
    for (uint32_t y = 0; y < kBandRows; ++y) {
      const uint32_t* row = reinterpret_cast<const uint32_t*>(p + size_t(y) * pitch);
      for (uint32_t x = 0; x < w; ++x) {
        const uint32_t v = row[x];
        double r, g, b;
        if (f == DXGI_FORMAT_R10G10B10A2_UNORM) {
          r = (v & 1023) / 1023.0;
          g = ((v >> 10) & 1023) / 1023.0;
          b = ((v >> 20) & 1023) / 1023.0;
        } else {
          r = (v & 255) / 255.0;
          g = ((v >> 8) & 255) / 255.0;
          b = ((v >> 16) & 255) / 255.0;
        }
        const double l = 0.2126 * r + 0.7152 * g + 0.0722 * b;
        sum += l;
        sum2 += l * l;
      }
    }
    const double n = double(w) * kBandRows;
    const double mean = sum / n;
    return {float(mean), float(std::sqrt(std::max(sum2 / n - mean * mean, 0.0)))};
  }
  static uint64_t Hash(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i += 8) {
      uint64_t v = 0;
      std::memcpy(&v, p + i, std::min<size_t>(8, n - i));
      h = (h ^ v) * 1099511628211ull;
    }
    return h;
  }

  void Read(Slot& slot) {
    void* data = nullptr;
    const D3D12_RANGE range = {0, size_t(slot.band_bytes * kBands * 3)};
    if (FAILED(slot.readback->Map(0, &range, &data))) return;
    Read_ cur;
    cur.valid = true;
    cur.info = slot.info;
    const uint8_t* base = static_cast<const uint8_t*>(data);
    for (uint32_t k = 0; k < 3; ++k) {
      cur.has[k] = slot.has[k];
      if (!slot.has[k]) continue;
      for (uint32_t band = 0; band < kBands; ++band) {
        const uint8_t* p = base + slot.band_bytes * (k * kBands + band);
        cur.s[k][band] = BandStats(p, slot.band_w, slot.row_pitch, slot.format);
        if (k == 0) cur.hash[band] = Hash(p, size_t(slot.row_pitch) * kBandRows);
      }
    }
    const D3D12_RANGE none = {0, 0};
    slot.readback->Unmap(0, &none);
    Detect(cur);
    prev2_ = prev_;
    prev_ = cur;
  }

  void Detect(const Read_& cur) {
    Period& p = period_;
    ++p.frames;
    p.generated += cur.has[1];
    p.hudless += cur.has[2];
    for (uint32_t k = 0; k < 3; ++k)
      if (cur.has[k]) {
        p.road_lo[k] = std::min(p.road_lo[k], cur.s[k][0].mean);
        p.road_hi[k] = std::max(p.road_hi[k], cur.s[k][0].mean);
      }
    const FgProbeDraws& dr = cur.info.draws;
    const bool draws_known = dr.frame == cur.info.frame;
    if (draws_known) (dr.rain ? p.rain_frames : p.no_rain_frames)++;
    std::string faults;
    // GEN OFF: the generated frame (between the frame before and this one) outside them.
    if (cur.has[1] && prev_.valid && prev_.has[0] && cur.has[0]) {
      const Stats& a = prev_.s[0][0];
      const Stats& b = cur.s[0][0];
      const Stats& g = cur.s[1][0];
      const float slack = 0.06f + 0.5f * std::fabs(b.mean - a.mean);
      const float sslack = 0.05f + 0.5f * std::fabs(b.spread - a.spread);
      if (g.mean < std::min(a.mean, b.mean) - slack || g.mean > std::max(a.mean, b.mean) + slack ||
          g.spread < std::min(a.spread, b.spread) - sslack ||
          g.spread > std::max(a.spread, b.spread) + sslack)
        faults += " GEN OFF";
    }
    // REAL JUMP: the frame before away from both its neighbours, which agree.
    if (prev2_.valid && prev_.valid && prev2_.has[0] && prev_.has[0] && cur.has[0]) {
      const float a = prev2_.s[0][0].mean, m = prev_.s[0][0].mean, b = cur.s[0][0].mean;
      if (std::fabs(a - b) < 0.04f && std::fabs(m - a) > 0.12f && std::fabs(m - b) > 0.12f)
        faults += " REAL JUMP (the frame before)";
    }
    // HUDLESS OFF: the copy's road band away from the frame's own.
    if (cur.has[2] && cur.has[0] && std::fabs(cur.s[2][0].mean - cur.s[0][0].mean) > 0.04f)
      faults += " HUDLESS OFF";
    // COPY LATE: rain or road drawn after the HUD-less copy.
    if (draws_known && dr.copy_draw != UINT32_MAX && (dr.rain_after || dr.road_after)) {
      ++p.copy_late;
      faults += " COPY LATE";
    }
    // STALE IMAGE: the same bytes for 30 frames with the scene drawn.
    if (cur.has[0] && prev_.valid && prev_.has[0] && cur.hash[0] == prev_.hash[0] &&
        cur.hash[1] == prev_.hash[1] && draws_known && dr.draws > 1000) {
      if (++stale_run_ == 30) {
        ++p.stale;
        faults += " STALE IMAGE (30 frames)";
      }
    } else {
      stale_run_ = 0;
    }
    if (faults.empty()) return;
    ++p.faults;
    if (++logged_ <= kMaxLogged || logged_ % 100 == 0) {
      REXLOG_INFO("PGR4 fg probe FAULT{}: {}; the frame before: {}{}", faults, Text(cur),
                  prev_.valid ? Text(prev_) : std::string("(none)"),
                  logged_ == kMaxLogged ? " (further faults: every 100th logged)" : "");
    }
    if (!dumps_armed_) dumps_armed_ = 2;
  }

  static std::string StatsText(const Read_& r, uint32_t k) {
    if (!r.has[k]) return "-";
    return fmt::format("road {:.3f}/{:.3f} middle {:.3f}/{:.3f}", r.s[k][0].mean, r.s[k][0].spread,
                       r.s[k][1].mean, r.s[k][1].spread);
  }
  static std::string Text(const Read_& r) {
    const Frame& f = r.info;
    const FgProbeDraws& d = f.draws;
    const FgProbeMemory& m = f.memory;
    const std::string draws =
        d.frame == f.frame
            ? fmt::format("{} draws, HUD-less copy before draw {}, rain {} ({} vertices, {} after the "
                          "copy), road {} ({} after the copy)",
                          d.draws, d.copy_draw == UINT32_MAX ? std::string("-") : std::to_string(d.copy_draw),
                          d.rain, d.rain_vertices, d.rain_after, d.road, d.road_after)
            : std::string("draws not recorded");
    return fmt::format(
        "frame {} ({}{}{}): image {} | generated {} | HUD-less {}; {}; GPU busy {:.1f} ms; video "
        "memory {} of {} MB, non-local {} of {} MB, HD textures {} MB, textures {} MB, process {} MB",
        f.frame, f.generated ? "FSR generated" : "drawn only", f.dlss_fg ? ", DLSS-G" : "",
        f.hudless ? ", HUD-less given" : "", StatsText(r, 0), StatsText(r, 1), StatsText(r, 2), draws,
        f.gpu_busy_ms, m.local >> 20, m.local_budget >> 20, m.nonlocal >> 20, m.nonlocal_budget >> 20,
        m.hd >> 20, m.textures >> 20, m.private_bytes >> 20);
  }

  void Summary() {
    const Period& p = period_;
    auto range = [&](uint32_t k) {
      return p.road_hi[k] < 0 ? std::string("-")
                              : fmt::format("{:.3f}..{:.3f}", p.road_lo[k], p.road_hi[k]);
    };
    const FgProbeMemory& m = prev_.info.memory;
    REXLOG_INFO(
        "PGR4 fg probe over 2 s: {} frames ({} generated, {} with the HUD-less copy, {} skipped), "
        "{} faults ({} copy late, {} stale); rain drawn in {} frames, not in {}; road band image {}, "
        "generated {}, HUD-less {}; video memory {} of {} MB, non-local {} of {} MB, HD textures {} "
        "MB, textures {} MB, process {} MB; GPU busy {:.1f} ms",
        p.frames, p.generated, p.hudless, skipped_, p.faults, p.copy_late, p.stale, p.rain_frames,
        p.no_rain_frames, range(0), range(1), range(2), m.local >> 20, m.local_budget >> 20,
        m.nonlocal >> 20, m.nonlocal_budget >> 20, m.hd >> 20, m.textures >> 20,
        m.private_bytes >> 20, prev_.info.gpu_busy_ms);
    skipped_ = 0;
  }

  static constexpr uint32_t kMaxLogged = 200;
  Slot slots_[kSlots];
  Read_ prev_, prev2_;
  Period period_;
  std::chrono::steady_clock::time_point last_summary_ = std::chrono::steady_clock::now();
  uint32_t skipped_ = 0, stale_run_ = 0, logged_ = 0;
  int32_t dumps_armed_ = 0, dumps_done_ = 0;
};

}  // namespace pgr4::native
