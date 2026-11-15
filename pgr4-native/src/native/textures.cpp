// pgr4-native PGR4 native renderer: texture cache (see textures.h).
#include "textures.h"

#include "clock.h"
#include "profile.h"
#include "render_targets.h"  // DrawResolutionScale

#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#ifdef PGR4_TEXTURES_OFFLINE
// Offline build, outside the game: no ReXGlue runtime, no write tracking.
#include <fmt/format.h>
#include <cstdio>
#include <cstdlib>
#define TEXLOG(level, ...) std::fprintf(stderr, "%s: %s\n", level, fmt::format(__VA_ARGS__).c_str())
#define REXLOG_INFO(...) TEXLOG("info", __VA_ARGS__)
#define REXLOG_WARN(...) TEXLOG("warn", __VA_ARGS__)
#define REXLOG_ERROR(...) TEXLOG("error", __VA_ARGS__)
#include "log.h"
#else
#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

#include "log.h"

REXCVAR_DEFINE_STRING(pgr4_native_texture_trace, "", "PGR4 native",
                      "Log every TextureCache::Get of the texture at this guest physical address "
                      "(hex): dirty upload, resolved image seq, copy");
// Local change (pgr4 project): -1 by default, from the video memory budget Windows gives the
// process on the adapter (15 % and 30 % of it, at least 512 and 1024 MB, the fixed values these
// had): a fixed 1 GB evicted most of an area on any card, and with them their HD versions.
REXCVAR_DEFINE_INT32(pgr4_native_texture_memory_soft_mb, -1, "PGR4 native",
                     "Texture memory above which textures unused for "
                     "--pgr4_native_texture_memory_soft_lifetime_s are evicted (0: no limit; -1: "
                     "15 % of the video memory budget, at least 512)");
REXCVAR_DEFINE_INT32(pgr4_native_texture_memory_soft_lifetime_s, 30, "PGR4 native",
                     "See --pgr4_native_texture_memory_soft_mb");
REXCVAR_DEFINE_INT32(pgr4_native_texture_memory_hard_mb, -1, "PGR4 native",
                     "Texture memory above which the least recently used textures are evicted, "
                     "once no frame in flight uses them (0: no limit; -1: 30 % of the video "
                     "memory budget, at least 1024)");
#endif

#include <algorithm>
#include <xxhash.h>
#include <unordered_set>
#include <unordered_map>
#include <mutex>
#include <fstream>
#include <filesystem>
#include <charconv>
#include <cstring>
#include <string_view>
#include <sstream>

#include <windows.h>
#include <compressapi.h>  // local change: the HD pack's entries (Cabinet.dll's Decompressor)

REXCVAR_DECLARE(bool, pgr4_native_texture_catalog);
REXCVAR_DECLARE(std::string, pgr4_native_capture_dir);

REXCVAR_DEFINE_BOOL(pgr4_native_test_no_unaligned_bc, false, "PGR4 native",
                    "Development: as if the card had no unaligned BC textures (Windows 10): "
                    "those textures decoded on the CPU (at startup)");

REXCVAR_DEFINE_BOOL(pgr4_native_scaled_cube, true, "PGR4 native",
                    "Cube maps the game renders (the cars' surroundings) bound at the draw "
                    "resolution scale, not shrunk to the game's size (local change)");


REXCVAR_DECLARE(bool, pgr4_native_dred);

namespace pgr4::native {

// Local change (pgr4 project): the overlay's "the game without our changes" (renderer.cpp).
bool OriginalLook();
bool EnhancedReflections();  // shader_replace.cpp

namespace xenos = rex::graphics::xenos;
namespace texture_util = rex::graphics::texture_util;
using Microsoft::WRL::ComPtr;

struct TextureCache::Texture {
  TextureKey key;
  const HostFormat* host = nullptr;
  ComPtr<ID3D12Resource> resource;
  D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
  texture_util::TextureGuestLayout layout{};
  uint32_t base_bytes = 0, mips_bytes = 0;
  // Set by the invalidation callback (any thread), cleared before the guest memory is read.
  std::atomic<bool> dirty{true};
  // The last MarkCapture stamp when a write set `dirty` (stored before it, under watch_mutex_).
  std::atomic<uint64_t> write_mark{0};
  bool failed = false;                 // unsupported or creation failed; logged once
  // Local change (pgr4 project): the card refused its resource (full video memory): tried again, not left
  // black for good (RetryCreate).
  bool create_refused = false;
  uint64_t retry_at = 0;
  bool retry_small = false;
  D3D12_RESOURCE_DESC retry_desc = {};
  uint64_t stale_gets = 0;             // for the log
  std::vector<uint64_t> resolved_seq;  // per subresource; 0 = holds guest-memory data
  // A resolve may have changed an image at one of its subresource addresses (OnResolved), or
  // the texture was uploaded: the next Get runs CopyResolved.
  bool resolve_pending = true;
  // The resolved image bound in place of `resource` (CopyResolved), or null.
  ID3D12Resource* alias = nullptr;
  // Local change: the cube at the draw resolution scale (CopyScaledCube), bound as the alias.
  ComPtr<ID3D12Resource> scaled_cube;
  D3D12_RESOURCE_STATES scaled_cube_state = D3D12_RESOURCE_STATE_COPY_DEST;
  // Cubes of the other sizes (pgr4_native_cube_detail changed), kept with their states: a frame
  // in flight may read one, and the draws keep descriptors of them. Freed with the texture.
  std::vector<std::pair<ComPtr<ID3D12Resource>, D3D12_RESOURCE_STATES>> other_cubes;
  // Local change: the HD version of the texture's current data (ApplyHd), bound as the alias,
  // and the hash it is for; earlier ones (the memory now holds another texture) kept like
  // other_cubes.
  ComPtr<ID3D12Resource> hd;
  uint64_t hd_hash = 0;
  bool hd_current = false;  // `hd` is of the data the last upload brought
  uint64_t hd_pending = 0;
  uint32_t hd_layers = 1;  // 2 with the normal map as the second layer (LoadHd)
  uint64_t content_hash = 0;  // TextureHash of the data the last upload brought (ApplyHd)  // the hash whose file the HD reader thread is reading (TakeHd)
  std::vector<ComPtr<ID3D12Resource>> other_hd;
  uint32_t uploads = 0;  // Upload calls so far (the slow-upload log)
  // Local change (pgr4 project): a small uncompressed texture's base level as uploaded, checked
  // against guest memory once a frame (TextureCache::GuardMissed).
  uint64_t guard_hash = 0, guard_frame = ~0ull;
  uint32_t slot = 0;
  uint64_t placed = kCommitted, bytes = 0;  // CreateTextureResource's placement and size
  uint64_t used_fence = 0, used_ms = 0;     // the last frame that bound it (MarkUsed)
  Texture* lru_prev = nullptr;
  Texture* lru_next = nullptr;
};

namespace {

// Local change (pgr4 project): textures go first if the system must move video memory to system
// memory, before the render targets and the upscaler's images (at the normal priority).
void LowResidency(ID3D12Device* device, ID3D12Pageable* pageable) {
  Microsoft::WRL::ComPtr<ID3D12Device1> device1;
  if (!pageable || FAILED(device->QueryInterface(IID_PPV_ARGS(&device1)))) return;
  const D3D12_RESIDENCY_PRIORITY priority = D3D12_RESIDENCY_PRIORITY_LOW;
  device1->SetResidencyPriority(1, &pageable, &priority);
}

constexpr D3D12_RESOURCE_STATES kShaderRead =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

// Typeless family for CopyTextureRegion compatibility.
DXGI_FORMAT Family(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SINT:
      return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UINT:
      return DXGI_FORMAT_R10G10B10A2_TYPELESS;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SINT:
      return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32_SINT:
      return DXGI_FORMAT_R32_TYPELESS;
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_SNORM:
      return DXGI_FORMAT_R16G16_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_SNORM:
      return DXGI_FORMAT_R16_TYPELESS;
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_SNORM:
      return DXGI_FORMAT_R8_TYPELESS;
    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_SNORM:
      return DXGI_FORMAT_R8G8_TYPELESS;
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT:
      return DXGI_FORMAT_R32G32_TYPELESS;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
      return DXGI_FORMAT_R32G32B32A32_TYPELESS;
    default:
      return f;
  }
}

// Calls f(subresource index, guest physical address) for each subresource a resolve can target
// on its own: level 0 and the stored mips of each slice (not levels inside a packed mip tail).
template <typename F>
void ForEachResolveAddress(const TextureKey& key, const texture_util::TextureGuestLayout& layout,
                           F&& f) {
  for (uint32_t slice = 0; slice < key.array_size(); ++slice) {
    for (uint32_t level = 0; level <= key.mip_max_level; ++level) {
      uint32_t address;
      if (level == 0) {
        if (!key.base_page) continue;
        address = (key.base_page << 12) + layout.base.array_slice_stride_bytes * slice;
      } else {
        if (!key.mip_page) continue;
        uint32_t stored = std::min(level, layout.packed_level);
        if (level > stored) continue;  // inside a packed tail: no resolve targets it alone
        address = (key.mip_page << 12) + layout.mip_offsets_bytes[stored] +
                  layout.mips[stored].array_slice_stride_bytes * slice;
      }
      f(size_t(level) + size_t(slice) * key.mip_levels(), address);
    }
  }
}

}  // namespace

TextureCache::TextureCache(ID3D12Device* device, const uint8_t* physical_membase)
    : device_(device), membase_(physical_membase) {
  upload_lists_.resize(1);
  upload_.SetDevice(device_);
  upload_.Reserve(4);  // the race start keeps 3 in flight (HitchFix)
  StartSpareHeap();
  D3D12_FEATURE_DATA_D3D12_OPTIONS8 options8 = {};
  if (SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS8, &options8,
                                             sizeof(options8)))) {
    unaligned_bc_supported_ = options8.UnalignedBlockTexturesSupported;
  }
  if (REXCVAR_GET(pgr4_native_test_no_unaligned_bc)) unaligned_bc_supported_ = false;
  REXLOG_INFO("PGR4 native textures: cache up (unaligned BC textures {})",
              unaligned_bc_supported_ ? "supported" : "not supported");
#ifndef PGR4_TEXTURES_OFFLINE
  // Local change (pgr4 project): the device's adapter, for its video memory (PollVideoMemory).
  Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
  if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
    factory->EnumAdapterByLuid(device_->GetAdapterLuid(), IID_PPV_ARGS(&adapter_));
  PollVideoMemory();
#endif
}

#ifndef PGR4_TEXTURES_OFFLINE
TextureCache::TextureCache(ID3D12Device* device, rex::memory::Memory* memory)
    : TextureCache(device, memory->physical_membase()) {
  memory_ = memory;
  invalidation_handle_ =
      memory_->RegisterPhysicalMemoryInvalidationCallback(InvalidationThunk, this);
  const uint64_t video = video_budget_;
  auto limit = [video](int32_t mb, uint64_t percent, uint64_t floor_mb) {
    if (mb < 0) return std::max(floor_mb << 20, video / 100 * percent);
    return mb > 0 ? uint64_t(mb) << 20 : ~0ull;
  };
  budget_.soft_bytes = limit(REXCVAR_GET(pgr4_native_texture_memory_soft_mb), 15, 512);
  budget_.hard_bytes = limit(REXCVAR_GET(pgr4_native_texture_memory_hard_mb), 30, 1024);
  REXLOG_INFO("PGR4 native textures: video memory budget {} MB ({} MB used); textures soft {} "
              "MB, hard {} MB", video >> 20, video_usage_ >> 20, budget_.soft_bytes >> 20,
              budget_.hard_bytes >> 20);
  budget_.soft_lifetime_ms =
      uint64_t(std::max(REXCVAR_GET(pgr4_native_texture_memory_soft_lifetime_s), 0)) * 1000;
  std::string_view trace = REXCVAR_GET(pgr4_native_texture_trace);
  if (trace.starts_with("0x") || trace.starts_with("0X")) trace.remove_prefix(2);
  std::from_chars(trace.data(), trace.data() + trace.size(), trace_address_, 16);
  // A quarter of the hardware threads, 1-4: 4 on an i9-13900K, 2 on a Steam Deck.
  const uint32_t n = std::clamp(std::thread::hardware_concurrency() / 4, 1u, 4u);
  upload_lists_.resize(n);
  for (uint32_t i = 0; i < n; ++i) {
    workers_.emplace_back([this, uploads = &upload_lists_[i]] { WorkerMain(uploads); });
  }
}
#endif

TextureCache::~TextureCache() {
  {
    std::lock_guard<std::mutex> lock(jobs_mutex_);
    stopping_ = true;
  }
  jobs_cv_.notify_all();
  for (std::thread& w : workers_) w.join();  // after the queued jobs ran
  {
    std::lock_guard<std::mutex> lock(release_mutex_);
    release_stopping_ = true;
  }
  release_cv_.notify_all();
  if (release_thread_.joinable()) release_thread_.join();  // after the queue, while the device lives
#ifndef PGR4_TEXTURES_OFFLINE
  if (invalidation_handle_)
    memory_->UnregisterPhysicalMemoryInvalidationCallback(invalidation_handle_);
#endif
}

void TextureCache::ReleaseMain() {
  SetThreadDescription(GetCurrentThread(), L"PGR4 native release");
  for (;;) {
    std::vector<ComPtr<ID3D12Resource>> batch;
    {
      std::unique_lock<std::mutex> lock(release_mutex_);
      release_cv_.wait(lock, [this] { return release_stopping_ || !release_queue_.empty(); });
      if (release_queue_.empty()) return;
      batch.swap(release_queue_);
    }
    const uint64_t t0 = NowNs();
    const size_t count = batch.size();
    batch.clear();
    PGR4_LOG_FIRST(16, INFO, "PGR4 native textures: {} HD versions destroyed in {:.1f} ms (background)",
                   count, (NowNs() - t0) / 1e6);
  }
}

void TextureCache::WorkerMain(UploadList* uploads) {
  SetThreadDescription(GetCurrentThread(), L"PGR4 native untile");
  std::vector<uint8_t> scratch;
  std::unique_lock<std::mutex> lock(jobs_mutex_);
  for (;;) {
    jobs_cv_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
    if (jobs_.empty()) return;  // stopping, queue drained
    UntileJob job = std::move(jobs_.front());
    jobs_.pop_front();
    lock.unlock();
    ArmWatches(job.key, job.base_bytes, job.mips_bytes);
    worker_untile_ns_.fetch_add(RunUntile(job, &scratch), std::memory_order_relaxed);
    if (job.deferred)
      worker_record_ns_.fetch_add(RecordDeferredUpload(job, uploads), std::memory_order_relaxed);
    lock.lock();
    if (--jobs_busy_ == 0) idle_cv_.notify_all();
  }
}

void TextureCache::FinishUploads() {
  if (workers_.empty()) return;
  std::unique_lock<std::mutex> lock(jobs_mutex_);
  idle_cv_.wait(lock, [this] { return jobs_busy_ == 0; });
  stats_.untile_ns += worker_untile_ns_.exchange(0, std::memory_order_relaxed);
  stats_.upload_record_ns += worker_record_ns_.exchange(0, std::memory_order_relaxed);
}

bool TextureCache::UploadsDone() {
  if (workers_.empty()) return true;
  std::lock_guard<std::mutex> lock(jobs_mutex_);
  return jobs_busy_ == 0;
}

// Local addition (pgr4 project): texture dumps and replacements, by a hash of the guest texture
// (its base level's bytes, size and format). pgr4_native_texture_dump_dir: each texture of 64x64
// or more is written there once as <hash>_<width>x<height>_<DXGI format>_<levels>.bin, every
// subresource's host blocks one after another without row padding. pgr4_native_texture_replace_dir
// (beside the executable unless absolute): a <hash>.bin of that layout and size is uploaded in
// place of the game's texture.
REXCVAR_DEFINE_STRING(pgr4_native_texture_dump_dir, "", "PGR4 native",
                      "Folder the textures are dumped to, beside the executable (empty: none): "
                      "<hash>.dds an image editor opens, to change and put in textures_hd under "
                      "the same name, and textures.txt listing them");
REXCVAR_DEFINE_BOOL(pgr4_native_texture_dump_dds, true, "PGR4 native",
                    "The dump as DDS files (<hash>.dds, with textures.txt); false: the raw blocks "
                    "(<hash>_<w>x<h>_<DXGI format>_<levels>.bin), as the project's tools read them");
REXCVAR_DEFINE_STRING(pgr4_native_texture_replace_dir, "textures", "PGR4 native",
                      "Folder of replacement textures (<hash>.bin), beside the executable");
REXCVAR_DEFINE_STRING(pgr4_native_texture_hd_dir, "textures_hd", "PGR4 native",
                      "Folder of HD textures (<hash>.dds, any size, the game's block format and "
                      "its mips), beside the executable (local change)");
REXCVAR_DEFINE_STRING(pgr4_native_texture_hd_pack, "textures_hd.pak", "PGR4 native",
                      "The HD textures packed and compressed (tools/hd_pack.py), beside the "
                      "executable; the loose files of pgr4_native_texture_hd_dir come first "
                      "(local change)");
REXCVAR_DEFINE_STRING(pgr4_native_texture_hd_trace, "", "PGR4 native",
                      "Development: a TextureHash (hex); each upload of that texture is logged "
                      "with what its HD version is at");
REXCVAR_DEFINE_BOOL(pgr4_native_texture_hd_reload, false, "PGR4 native",
                    "Set: the HD textures' folder listed again and every HD texture read again "
                    "(new or changed files without restarting); set back to false at once");
REXCVAR_DEFINE_BOOL(pgr4_native_texture_watch, false, "PGR4 native",
                    "Development: write in the log an HD version shown on a content that is not its own");
REXCVAR_DEFINE_BOOL(pgr4_native_texture_hd, true, "PGR4 native",
                    "Bind the HD textures of pgr4_native_texture_hd_dir in place of the game's "
                    "(local change)");
// The HD textures shown: pgr4_native_texture_hd, unless the game is shown as it was.
static bool HdOn() { return REXCVAR_GET(pgr4_native_texture_hd) && !OriginalLook(); }
// ... and for this texture: the ones made for our cars' shaders (the drops, the scratches, the frost)
// only with them (ENHANCED REFLECTIONS): the game's shaders keep the game's.
static bool HdOnFor(uint64_t hd_hash) {
  if (!HdOn()) return false;
  if (EnhancedReflections()) return true;
  return hd_hash != 0x320A02826343FE4Bull && hd_hash != 0xA8F8B1ADE995A485ull &&
         hd_hash != 0xBBF69F56E74D0D9Full;
}

namespace {
struct TextureFiles {
  std::mutex mutex;
  bool listed = false;
  std::unordered_map<uint64_t, std::filesystem::path> replacements, playstation;
  // Local change: the guest ranges of the textures with a PlayStation version, made stale when
  // the controller's family changes (TextureCache::Get) so that the other version is loaded.
  std::vector<std::pair<uint32_t, uint32_t>> switchable;
  // Local change (pgr4 project): the guest ranges of the textures the folder replaces (not the
  // PlayStation ones), reloaded when the overlay's "the game without our changes" switches.
  std::vector<std::pair<uint32_t, uint32_t>> replaced;
  std::unordered_map<std::string, std::vector<uint8_t>> loaded;  // by file
  std::unordered_set<uint64_t> dumped;
};
TextureFiles& Files() {
  static TextureFiles files;
  return files;
}
std::filesystem::path BesideExe(const std::string& dir) {
  std::filesystem::path p(dir);
  if (p.is_absolute()) return p;
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  return std::filesystem::path(exe).parent_path() / p;
}
// Local change (pgr4 project): the HD textures, <hash>.dds by TextureHash, listed once, and read
// by threads of their own (kHdReaders; an x4 texture is tens of MB: read in the draw, it stalled
// the frame).
// A <hash>_n.dds beside one is its normal map (PBRify's), loaded as the texture's second layer.
// TextureCache::SetNoScene.
std::atomic<bool> g_no_scene{false};
struct HdData {
  std::vector<uint8_t> file, normal;  // normal empty without a <hash>_n.dds
};
// Where an HD file is: a loose .dds of the folder, or an entry of the pack
// (pgr4_native_texture_hd_pack, tools/hd_pack.py): "PGR4HDP1", the entry count (u32), 0 (u32),
// then per entry its hash (u64), offset (u64), compressed and file sizes (u32), kind (u32: 0 a
// texture, 1 its normal map, 2 areas.tsv) and Compression API algorithm (u32: 0 stored, else
// COMPRESS_ALGORITHM_*, in its buffer mode), little-endian, then the entries' data.
struct HdSource {
  std::filesystem::path path;  // empty: in the pack
  uint64_t offset = 0;
  uint32_t csize = 0, usize = 0, algo = 0;
};
struct HdFiles {
  std::mutex mutex;
  bool listed = false;
  std::filesystem::path pack;  // the pack's path, empty without one
  std::unordered_map<uint64_t, HdSource> files, normals;
  std::condition_variable wake;
  bool reader = false;
  // Asked for and not read yet, the latest ask last: read last first. Entering an area, every
  // texture of it with an HD file asks at its upload (a thousand files, over 1 GB in Macau);
  // read oldest first, the ones on screen, asked again at each draw, waited behind all of them.
  std::deque<uint64_t> queue;
  std::unordered_set<uint64_t> asked;  // queued or being read
  // Read files no texture took yet, oldest first, dropped past kHdReadyBytes (64 files at
  // first: most of an area's files read were dropped before their textures were drawn).
  std::unordered_map<uint64_t, std::shared_ptr<HdData>> ready;
  std::deque<uint64_t> ready_order;
  size_t ready_bytes = 0;
  // textures_hdreas.tsv (tools/hd_areas.py): the areas (.stream2) of each HD texture, and the
  // HD textures of each area, for TextureCache::PreloadArea.
  std::vector<std::string> area_names;
  std::vector<std::vector<uint64_t>> area_textures;
  std::unordered_map<uint64_t, std::vector<uint16_t>> texture_areas;
  // Local change: the groups whose name starts with '*' ("*Cars": the textures common to every
  // car, its rain, frost and damage): preloaded with each area and never freed, so that a car's
  // first crash is already in HD; they trigger no preload themselves.
  std::vector<uint64_t> always;
  std::unordered_set<uint64_t> always_set;
};
constexpr size_t kHdReadyBytes = size_t(512) << 20;
// An HD file's bytes into `out` (empty if it cannot be read): a loose file whole, a pack entry
// read and decompressed.
void HdRead(const std::filesystem::path& pack, const HdSource& s, std::vector<uint8_t>& out) {
  out.clear();
  if (!s.path.empty()) {
    std::ifstream in(s.path, std::ios::binary | std::ios::ate);
    if (!in) return;
    out.resize(size_t(in.tellg()));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(out.data()), std::streamsize(out.size()));
    return;
  }
  std::ifstream in(pack, std::ios::binary);
  if (!in) return;
  std::vector<uint8_t> packed(s.csize);
  in.seekg(std::streamoff(s.offset));
  if (!in.read(reinterpret_cast<char*>(packed.data()), std::streamsize(packed.size()))) return;
  if (!s.algo) {
    out = std::move(packed);
    return;
  }
  DECOMPRESSOR_HANDLE d = nullptr;
  if (!CreateDecompressor(s.algo, nullptr, &d)) return;
  out.resize(s.usize);
  SIZE_T got = 0;
  if (!Decompress(d, packed.data(), packed.size(), out.data(), out.size(), &got) ||
      got != s.usize) {
    PGR4_LOG_FIRST(8, WARN, "PGR4 native textures: pack entry at {} not decompressed", s.offset);
    out.clear();
  }
  CloseDecompressor(d);
}
constexpr int kHdReaders = 2;
void HdReader(HdFiles& hd) {
  for (;;) {
    uint64_t hash;
    HdSource path, normal;
    bool with_normal = false;
    std::filesystem::path pack;
    {
      std::unique_lock<std::mutex> lock(hd.mutex);
      hd.wake.wait(lock, [&] { return !hd.queue.empty(); });
      hash = hd.queue.back();
      hd.queue.pop_back();
      path = hd.files[hash];
      if (auto n = hd.normals.find(hash); n != hd.normals.end()) {
        normal = n->second;
        with_normal = true;
      }
      pack = hd.pack;
    }
    auto file = std::make_shared<HdData>();
    HdRead(pack, path, file->file);
    if (with_normal) HdRead(pack, normal, file->normal);
    const size_t bytes = file->file.size() + file->normal.size();
    std::lock_guard<std::mutex> lock(hd.mutex);
    hd.asked.erase(hash);
    if (auto old = hd.ready.find(hash); old != hd.ready.end()) {
      hd.ready_bytes -= old->second->file.size() + old->second->normal.size();
      hd.ready_order.erase(std::find(hd.ready_order.begin(), hd.ready_order.end(), hash));
    }
    hd.ready[hash] = std::move(file);
    hd.ready_order.push_back(hash);
    hd.ready_bytes += bytes;
    while (hd.ready_bytes > kHdReadyBytes && hd.ready_order.size() > 1) {
      auto drop = hd.ready.find(hd.ready_order.front());
      hd.ready_bytes -= drop->second->file.size() + drop->second->normal.size();
      hd.ready.erase(drop);
      hd.ready_order.pop_front();
    }
  }
}
// The file of `hash` if the reader has it (taken), else nullptr, and asked for: queued, or moved
// to the queue's end (read next) when it is already there.
std::shared_ptr<HdData> HdTake(HdFiles& hd, uint64_t hash, bool low = false) {
  std::lock_guard<std::mutex> lock(hd.mutex);
  auto it = hd.ready.find(hash);
  if (it != hd.ready.end()) {
    auto file = std::move(it->second);
    hd.ready_bytes -= file->file.size() + file->normal.size();
    hd.ready.erase(it);
    hd.ready_order.erase(std::find(hd.ready_order.begin(), hd.ready_order.end(), hash));
    return file;
  }
  if (hd.asked.insert(hash).second) {
    // Low (a preload, PumpPreload): read after everything the game draws asked for.
    if (low) hd.queue.push_front(hash);
    else hd.queue.push_back(hash);
    if (!hd.reader) {
      hd.reader = true;
      for (int i = 0; i < kHdReaders; ++i) std::thread(HdReader, std::ref(hd)).detach();
    }
    hd.wake.notify_one();
  } else if (!low && !hd.queue.empty() && hd.queue.back() != hash) {
    // Asked again (drawn again): read next. Not found: being read now.
    if (auto q = std::find(hd.queue.begin(), hd.queue.end(), hash); q != hd.queue.end()) {
      hd.queue.erase(q);
      hd.queue.push_back(hash);
    }
  }
  return nullptr;
}
// Asks for the file of `hash` at the lowest priority (a preload) unless it is asked or read.
void HdAsk(HdFiles& hd, uint64_t hash) {
  std::lock_guard<std::mutex> lock(hd.mutex);
  if (hd.ready.count(hash) || !hd.asked.insert(hash).second) return;
  hd.queue.push_front(hash);
  if (!hd.reader) {
    hd.reader = true;
    for (int i = 0; i < kHdReaders; ++i) std::thread(HdReader, std::ref(hd)).detach();
  }
  hd.wake.notify_one();
}
HdFiles& Hd();
// pgr4_native_texture_hd_reload: listed again at the next Hd(), nothing kept from the files read.
void HdForget() {
  HdFiles& hd = Hd();
  std::lock_guard<std::mutex> lock(hd.mutex);
  hd.listed = false;
  hd.pack.clear();
  hd.files.clear();
  hd.normals.clear();
  hd.ready.clear();
  hd.ready_order.clear();
  hd.ready_bytes = 0;
}
HdFiles& Hd() {
  static HdFiles hd;
  std::lock_guard<std::mutex> lock(hd.mutex);
  if (!hd.listed) {
    hd.listed = true;
    const std::filesystem::path dir = BesideExe(REXCVAR_GET(pgr4_native_texture_hd_dir));
    // The pack first (its index), then the folder's loose files over it (a file being made or
    // tried: the reload button reads them without a new pack).
    HdSource areas_entry;
    if (const std::string name = REXCVAR_GET(pgr4_native_texture_hd_pack); !name.empty()) {
      const std::filesystem::path pack = BesideExe(name);
      if (std::ifstream in(pack, std::ios::binary); in) {
        char magic[8] = {};
        uint32_t head[2] = {};
        in.read(magic, 8);
        in.read(reinterpret_cast<char*>(head), sizeof(head));
        if (in && std::memcmp(magic, "PGR4HDP1", 8) == 0) {
          struct Entry {
            uint64_t hash, offset;
            uint32_t csize, usize, kind, algo;
          };
          static_assert(sizeof(Entry) == 32);
          std::vector<Entry> entries(head[0]);
          in.read(reinterpret_cast<char*>(entries.data()),
                  std::streamsize(entries.size() * sizeof(Entry)));
          if (in) {
            hd.pack = pack;
            for (const Entry& e : entries) {
              const HdSource s{{}, e.offset, e.csize, e.usize, e.algo};
              if (e.kind == 0) hd.files[e.hash] = s;
              else if (e.kind == 1) hd.normals[e.hash] = s;
              else if (e.kind == 2) areas_entry = s;
            }
            REXLOG_INFO("PGR4 native textures: {} HD texture(s) in {}", hd.files.size(),
                        pack.string());
          }
        }
        if (hd.pack.empty())
          REXLOG_WARN("PGR4 native textures: {} is not an HD pack", pack.string());
      }
    }
    const size_t packed = hd.files.size();
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
      if (e.path().extension() != ".dds") continue;
      const std::string stem = e.path().stem().string();
      const bool normal = stem.size() == 18 && stem.ends_with("_n");
      uint64_t h = 0;
      if ((stem.size() == 16 || normal) &&
          std::from_chars(stem.data(), stem.data() + 16, h, 16).ec == std::errc())
        (normal ? hd.normals : hd.files)[h] = HdSource{e.path()};
    }
    if (hd.files.size() != packed)
      REXLOG_INFO("PGR4 native textures: {} HD texture(s) with {}", hd.files.size(),
                  dir.string());
    hd.area_names.clear();
    hd.area_textures.clear();
    hd.texture_areas.clear();
    hd.always.clear();
    hd.always_set.clear();
    // areas.tsv: the folder's, else the pack's.
    std::string areas_text;
    if (std::ifstream in(dir / "areas.tsv", std::ios::binary); in) {
      areas_text.assign(std::istreambuf_iterator<char>(in), {});
    } else if (areas_entry.csize) {
      std::vector<uint8_t> bytes;
      HdRead(hd.pack, areas_entry, bytes);
      areas_text.assign(bytes.begin(), bytes.end());
    }
    if (!areas_text.empty()) {
      std::istringstream in(areas_text);
      std::string line;
      std::unordered_map<std::string, uint16_t> index;
      while (std::getline(in, line)) {
        const size_t tab = line.find('	');
        uint64_t h = 0;
        if (tab != 16 || std::from_chars(line.data(), line.data() + 16, h, 16).ec != std::errc() ||
            !hd.files.count(h))
          continue;
        const std::string area = line.substr(17);
        if (!area.empty() && area[0] == '*') {
          if (hd.always_set.insert(h).second) hd.always.push_back(h);
          continue;
        }
        auto [it, added] = index.emplace(area, uint16_t(hd.area_names.size()));
        if (added) {
          hd.area_names.push_back(area);
          hd.area_textures.emplace_back();
        }
        hd.area_textures[it->second].push_back(h);
        hd.texture_areas[h].push_back(it->second);
      }
      REXLOG_INFO("PGR4 native textures: {} HD areas in {}, {} textures preloaded with each",
                  hd.area_names.size(), (dir / "areas.tsv").string(), hd.always.size());
    }
  }
  return hd;
}
uint64_t TextureHash(const TextureKey& key, const GuestTextureMemory& guest) {
  if (!guest.base || !guest.base_size) return 0;
  return XXH3_64bits(guest.base, guest.base_size) ^
         (uint64_t(key.width) << 48 | uint64_t(key.height) << 32 | uint64_t(key.format) << 24);
}
// The subresources' blocks, packed, from or to the host buffer.
uint64_t PackedSize(const std::vector<HostSubresource>& footprints) {
  uint64_t size = 0;
  for (const HostSubresource& f : footprints) size += uint64_t(f.row_bytes) * f.block_rows * f.depth;
  return size;
}
// Local change (pgr4 project, 14/11/2026): a dumped texture as a DDS file an image editor opens (and
// MakeHd reads back from textures_hd): the classic header for the formats every editor knows (DXT1,
// DXT3, DXT5, ATI1, ATI2, 32-bit RGBA), DX10's for the others; 2D textures and volumes only (the HD
// textures are those). False for a texture it does not cover.
bool DdsHeader(const TextureKey& key, DXGI_FORMAT format, std::string* out) {
  if (!key.is_3d() && key.array_size() > 1) return false;  // arrays and cubes
  if (key.dimension == rex::graphics::xenos::DataDimension::kCube) return false;
  uint32_t fourcc = 0, rgb_bits = 0, block = 16;
  bool compressed = true;
  switch (format) {
    case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB: case DXGI_FORMAT_BC1_TYPELESS:
      fourcc = 0x31545844; block = 8; break;  // DXT1
    case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB: case DXGI_FORMAT_BC2_TYPELESS:
      fourcc = 0x33545844; break;  // DXT3
    case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB: case DXGI_FORMAT_BC3_TYPELESS:
      fourcc = 0x35545844; break;  // DXT5
    case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_TYPELESS: fourcc = 0x31495441; block = 8; break;
    case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_TYPELESS: fourcc = 0x32495441; break;
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
      rgb_bits = 32; compressed = false; block = 4; break;
    default:
      return false;  // the HD loader takes the block formats and 8-bit RGBA only
  }
  const uint32_t depth = key.is_3d() ? key.depth() : 1, mips = key.mip_levels();
  uint32_t h[31] = {};
  h[0] = 124;                                                    // size
  h[1] = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | (depth > 1 ? 0x800000 : 0) |
         (compressed ? 0x80000 : 0x8);                           // caps, height, width, format, mips
  h[2] = key.height;
  h[3] = key.width;
  h[4] = compressed ? std::max(1u, (key.width + 3) / 4) * std::max(1u, (key.height + 3) / 4) * block
                    : key.width * 4;                             // linear size, or pitch
  h[5] = depth;
  h[6] = mips;
  h[18] = 32;                                                    // pixel format: size
  if (compressed) {
    h[19] = 0x4;                                                 // DDPF_FOURCC
    h[20] = fourcc;
  } else {
    h[19] = 0x40 | 0x1;                                          // DDPF_RGB | DDPF_ALPHAPIXELS
    h[21] = rgb_bits;
    h[22] = 0x000000FF;                                          // R
    h[23] = 0x0000FF00;                                          // G
    h[24] = 0x00FF0000;                                          // B
    h[25] = 0xFF000000;                                          // A
  }
  h[26] = 0x1000 | (mips > 1 ? 0x400000 | 0x8 : 0);              // DDSCAPS_TEXTURE, MIPMAP, COMPLEX
  h[27] = depth > 1 ? 0x200000 : 0;                              // DDSCAPS2_VOLUME
  out->assign("DDS ");
  out->append(reinterpret_cast<const char*>(h), sizeof(h));
  return true;
}
const char* DdsFormatName(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB: case DXGI_FORMAT_BC1_TYPELESS: return "DXT1";
    case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB: case DXGI_FORMAT_BC2_TYPELESS: return "DXT3";
    case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB: case DXGI_FORMAT_BC3_TYPELESS: return "DXT5";
    case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_TYPELESS: return "ATI1 (BC4)";
    case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_TYPELESS: return "ATI2 (BC5)";
    default: return "RGBA 8-bit";
  }
}

void ReplaceOrDump(const TextureKey& key, const GuestTextureMemory& guest,
                   const std::vector<HostSubresource>& footprints, uint8_t* dst,
                   DXGI_FORMAT format) {
  const std::string dump_dir = REXCVAR_GET(pgr4_native_texture_dump_dir);
  const std::string replace_dir = REXCVAR_GET(pgr4_native_texture_replace_dir);
  if (dump_dir.empty() && replace_dir.empty()) return;
  const uint64_t hash = TextureHash(key, guest);
  if (key.is_3d())
    PGR4_LOG_FIRST(16, INFO, "PGR4 native textures: 3D {:016X} {}x{}x{} base {} B at {:08X}", hash,
                   key.width, key.height, key.depth(), guest.base_size, key.base_page << 12);
  if (!hash) return;
  TextureFiles& files = Files();
  std::lock_guard<std::mutex> lock(files.mutex);
  if (!files.listed) {
    files.listed = true;
    std::error_code ec;
    // Local change (pgr4 project): playstation\ holds those for a PlayStation controller (the
    // button icons; rex::input::PlayStationLayout), in place of the folder's.
    auto list = [&ec](const std::filesystem::path& dir,
                      std::unordered_map<uint64_t, std::filesystem::path>& into) {
      for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.path().extension() != ".bin") continue;
        const std::string stem = e.path().stem().string();
        uint64_t h = 0;
        if (std::from_chars(stem.data(), stem.data() + std::min<size_t>(stem.size(), 16), h, 16).ec ==
            std::errc())
          into[h] = e.path();
      }
    };
    if (!replace_dir.empty()) {
      list(BesideExe(replace_dir), files.replacements);
      list(BesideExe(replace_dir) / "playstation", files.playstation);
    }
    if (!files.replacements.empty() || !files.playstation.empty())
      REXLOG_INFO("PGR4 native textures: {} replacement(s) in {}, {} for PlayStation",
                  files.replacements.size(), BesideExe(replace_dir).string(),
                  files.playstation.size());
  }
  const uint64_t packed = PackedSize(footprints);
  if (files.playstation.count(hash) && key.base_page) {
    const std::pair<uint32_t, uint32_t> range{key.base_page << 12, uint32_t(guest.base_size)};
    if (std::find(files.switchable.begin(), files.switchable.end(), range) ==
        files.switchable.end())
      files.switchable.push_back(range);
  }
  const std::filesystem::path* file = nullptr;
  if (rex::input::PlayStationLayout())
    if (auto it = files.playstation.find(hash); it != files.playstation.end()) file = &it->second;
  if (!file)
    if (auto it = files.replacements.find(hash); it != files.replacements.end()) {
      if (key.base_page) {
        const std::pair<uint32_t, uint32_t> range{key.base_page << 12, uint32_t(guest.base_size)};
        if (std::find(files.replaced.begin(), files.replaced.end(), range) == files.replaced.end())
          files.replaced.push_back(range);
      }
      // The game's own texture with "the game without our changes" (OriginalLook). Our drops'
      // texture only with our cars' shaders (ENHANCED REFLECTIONS).
      if (!OriginalLook() && (EnhancedReflections() || hash != 0x320A02826343FE4Bull))
        file = &it->second;
    }
  if (file) {
    auto [data, fresh] = files.loaded.try_emplace(file->string());
    if (fresh) {
      std::ifstream in(*file, std::ios::binary);
      data->second.assign(std::istreambuf_iterator<char>(in), {});
    }
    if (data->second.size() != packed) {
      PGR4_LOG_FIRST(8, WARN, "PGR4 native textures: {} is {} bytes, the texture takes {}",
                     file->string(), data->second.size(), packed);
      return;
    }
    const uint8_t* src = data->second.data();
    for (const HostSubresource& f : footprints)
      for (uint32_t z = 0; z < f.depth; ++z)
        for (uint32_t row = 0; row < f.block_rows; ++row) {
          std::memcpy(dst + f.offset + (uint64_t(z) * f.block_rows + row) * f.row_pitch, src,
                      f.row_bytes);
          src += f.row_bytes;
        }
    PGR4_LOG_FIRST(32, INFO, "PGR4 native textures: {:016X} {}x{} replaced", hash, key.width,
                   key.height);
    return;
  }
  if (dump_dir.empty() || key.width < 64 || key.height < 64 || !files.dumped.insert(hash).second)
    return;
  const std::filesystem::path dir = BesideExe(dump_dir);
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  std::string dds;
  const bool as_dds = REXCVAR_GET(pgr4_native_texture_dump_dds) && DdsHeader(key, format, &dds);
  if (REXCVAR_GET(pgr4_native_texture_dump_dds) && !as_dds) return;  // not one the HD loader takes
  if (as_dds) {
    std::ofstream list(dir / "textures.txt", std::ios::app);
    list << fmt::format("{:016X}.dds\t{}x{}{}\t{}\t{} levels\n", hash, key.width, key.height,
                        key.is_3d() ? fmt::format("x{}", key.depth()) : std::string(),
                        DdsFormatName(format), key.mip_levels());
  }
  std::ofstream out(dir / (as_dds ? fmt::format("{:016X}.dds", hash)
                                   : fmt::format("{:016X}_{}x{}_{}_{}.bin", hash, key.width, key.height,
                                                 uint32_t(format), key.mip_levels())),
                    std::ios::binary);
  if (as_dds) out.write(dds.data(), std::streamsize(dds.size()));
  for (const HostSubresource& f : footprints)
    for (uint32_t z = 0; z < f.depth; ++z)
      for (uint32_t row = 0; row < f.block_rows; ++row)
        out.write(reinterpret_cast<const char*>(dst + f.offset +
                                                (uint64_t(z) * f.block_rows + row) * f.row_pitch),
                  f.row_bytes);
}
}  // namespace

uint64_t TextureCache::RunUntile(const UntileJob& job, std::vector<uint8_t>* scratch) {
  const uint64_t t0 = NowNs();
  UntileTexture(job.key, job.layout, job.guest, job.footprints, job.dst, scratch, nullptr,
                job.host);
  ReplaceOrDump(job.key, job.guest, job.footprints, job.dst, job.copies.format);
  const uint64_t ns = NowNs() - t0;
  if (ns > 500000) {
    const TextureKey& key = job.key;
    PGR4_LOG_FIRST(
        40, INFO,
        "PGR4 native textures: slow upload #{} of {:08X} {}x{}x{} {} dim{} tiled{} endian{} "
        "mips{} packed{} conv{}: host {} B, guest base {} B mips {} B, untile {} us",
        job.upload_number, key.base_page << 12, key.width, key.height, key.depth_or_array_size,
        rex::graphics::FormatInfo::Get(key.format)->name, uint32_t(key.dimension),
        uint32_t(key.tiled), uint32_t(key.endianness), key.mip_levels(), uint32_t(key.packed_mips),
        uint32_t(job.conversion), job.size, job.base_bytes, job.mips_bytes, ns / 1000);
  }
  return ns;
}

bool TextureCache::ClampRange(uint32_t start, uint32_t length, uint32_t* end) {
  if (!length || start >= kMemorySize) return false;
  *end = start + std::min(length, kMemorySize - start);
  return true;
}

void TextureCache::ArmWatches(const TextureKey& key, uint32_t base_bytes, uint32_t mips_bytes) {
#ifndef PGR4_TEXTURES_OFFLINE
  if (!memory_) return;
  const uint32_t base = key.base_page << 12, mips = key.mip_page << 12;
  // Mips from the page after the base's last: one protection change for both.
  if (base_bytes && mips_bytes && mips == base + ((base_bytes + kPageMask) & ~kPageMask)) {
    memory_->EnablePhysicalMemoryAccessCallbacks(base, mips - base + mips_bytes, true, false);
    return;
  }
  if (base_bytes) memory_->EnablePhysicalMemoryAccessCallbacks(base, base_bytes, true, false);
  if (mips_bytes) memory_->EnablePhysicalMemoryAccessCallbacks(mips, mips_bytes, true, false);
#endif
}

std::pair<uint32_t, uint32_t> TextureCache::InvalidationThunk(void* context, uint32_t start,
                                                              uint32_t length, bool exact) {
  return static_cast<TextureCache*>(context)->Invalidate(start, length, exact);
}

std::pair<uint32_t, uint32_t> TextureCache::Invalidate(uint32_t start, uint32_t length,
                                                       bool exact) {
  uint32_t end;
  if (!ClampRange(start, length, &end)) return {start, length};
  uint32_t lo = start, hi = end;
  bool any = false;
  {
    std::lock_guard<std::mutex> lock(watch_mutex_);
    const uint64_t mark = capture_seq_.value.load(std::memory_order_acquire);
    for (uint32_t b = start >> kWatchBucketShift; b <= (end - 1) >> kWatchBucketShift; ++b) {
      for (const Watch& w : watch_buckets_[b]) {
        if (w.start < end && start < w.end) {
          Texture& t = *w.texture;
          // The first write since the data was read: draws captured until now predate it.
          if (!t.dirty.load(std::memory_order_acquire)) {
            t.write_mark.store(mark, std::memory_order_relaxed);
            t.dirty.store(true, std::memory_order_release);
          }
          any = true;
        }
      }
    }
    if (!exact) {
      // Out to the nearest clean texture on each side: the pages of stale textures need no
      // watch (their next upload re-arms all of them), so a texture being streamed in faults
      // once rather than once per 4 KB page. Any texture overlapping [start, end) is dirty now,
      // and a watch lies in every bucket it touches, so the scan stops at the first bucket
      // holding a clean one. A texture cleared meanwhile re-arms after the unwatch (both hold
      // the global lock).
      auto clean = [](const Watch& w) { return !w.texture->dirty.load(std::memory_order_acquire); };
      const uint32_t window_lo = start & ~(kUnwatchWindow - 1);
      const uint32_t window_hi = ((end - 1) | (kUnwatchWindow - 1)) + 1;
      lo = window_lo;
      for (uint32_t b = start >> kWatchBucketShift;; --b) {
        for (const Watch& w : watch_buckets_[b])
          if (w.end <= start && clean(w)) lo = std::max(lo, w.end);
        if (lo != window_lo || b == window_lo >> kWatchBucketShift) break;
      }
      hi = window_hi;
      for (uint32_t b = (end - 1) >> kWatchBucketShift;; ++b) {
        for (const Watch& w : watch_buckets_[b])
          if (w.start >= end && clean(w)) hi = std::min(hi, w.start);
        if (hi != window_hi || b == (window_hi - 1) >> kWatchBucketShift) break;
      }
      // The page holding a clean texture's last byte stays watched.
      lo = std::min(start, (lo + kPageMask) & ~kPageMask);
      hi = std::max(end, hi & ~kPageMask);
    }
  }
  if (any) change_epoch_.fetch_add(1, std::memory_order_acq_rel);
  return {lo, hi - lo};
}

void TextureCache::OnResolved(uint32_t guest_physical) {
  auto it = resolve_targets_.find(guest_physical);
  if (it == resolve_targets_.end()) return;
  for (Texture* t : it->second) t->resolve_pending = true;
  change_epoch_.fetch_add(1, std::memory_order_acq_rel);
}

// Local change (pgr4 project): the small uncompressed textures the game's CPU writes (the cars'
// paint palette, 3x512 k_8_8_8_8 read by the body shaders: a colour picked in the car choice
// stayed unseen, after a long session, until the renderer was switched and back). Their base
// level is hashed once a frame and compared with the upload's: a write the page watch missed
// makes them dirty (logged). Resolve destinations and HD-bound ones are left out.
bool TextureCache::Guarded(const Texture& t) {
  return !t.host->block_compressed && t.base_bytes && t.base_bytes <= (256u << 10) && !t.alias &&
         (t.resolved_seq.empty() || t.resolved_seq[0] == 0) && t.uploads;
}
bool TextureCache::GuardMissed(Texture& t) {
  if (t.guard_frame == retire_count_ || !Guarded(t) || t.dirty.load(std::memory_order_acquire))
    return false;
  t.guard_frame = retire_count_;
  const uint64_t h = XXH3_64bits(membase_ + (t.key.base_page << 12), t.base_bytes);
  if (h == t.guard_hash) return false;
  PGR4_LOG_FIRST(32, WARN,
                 "PGR4 native textures: {:08X} {}x{} fmt{} changed without its page watch "
                 "seeing it: uploaded again",
                 t.key.base_page << 12, t.key.width, t.key.height, uint32_t(t.key.format));
  t.dirty.store(true, std::memory_order_release);
  change_epoch_.fetch_add(1, std::memory_order_acq_rel);
  return true;
}

// Local change (pgr4 project): the game reuses a texture's memory for another (the same address, size
// and format, in another part of a circuit or the next circuit). A write the page watch misses left
// the old picture shown, in HD above all, which GuardMissed leaves out: a sign showing a poster until
// its next upload. Some textures' base level hashed each frame, in turn (4 MB: about 0.3 ms), and
// compared with the upload's; different: uploaded again at its next use.
void TextureCache::RecheckContents() {
  constexpr size_t kBytesPerFrame = size_t(4) << 20;
  constexpr int kVisitsPerFrame = 128;
  if (textures_.empty()) return;
  recheck_pos_ %= textures_.size();
  auto it = std::next(textures_.begin(), std::ptrdiff_t(recheck_pos_));
  size_t bytes = 0;
  for (int visits = 0; visits < kVisitsPerFrame && bytes < kBytesPerFrame; ++visits) {
    if (it == textures_.end()) {
      it = textures_.begin();
      recheck_pos_ = 0;
    }
    Texture& t = *it->second;
    ++it;
    ++recheck_pos_;
    if (!t.content_hash || !t.uploads || !t.base_bytes || t.resolve_pending ||
        t.dirty.load(std::memory_order_acquire) ||
        !(t.resolved_seq.empty() || t.resolved_seq[0] == 0))
      continue;
    GuestTextureMemory guest;
    guest.base = membase_ + (t.key.base_page << 12);
    guest.base_size = t.base_bytes;
    bytes += t.base_bytes;
    if (TextureHash(t.key, guest) == t.content_hash) continue;
    PGR4_LOG_FIRST(64, WARN,
                   "PGR4 native textures: recheck: {:08X} {}x{} fmt{}{} changed without its page "
                   "watch seeing it: uploaded again",
                   t.key.base_page << 12, t.key.width, t.key.height, uint32_t(t.key.format),
                   BoundToHd(t) ? " (HD shown)" : "");
    t.dirty.store(true, std::memory_order_release);
    change_epoch_.fetch_add(1, std::memory_order_acq_rel);
  }
}

bool TextureCache::Clean(const Texture& t) {
  return !t.dirty.load(std::memory_order_acquire) && !t.resolve_pending;
}

void TextureCache::SetAlias(Texture& t, ID3D12Resource* alias) {
  if (t.alias == alias) return;
  t.alias = alias;
  // Bindings of the texture memoized by other fetch constants (GetMemo, callers' memos through
  // Unchanged) still name the previous resource.
  ++slots_[t.slot].generation;
  change_epoch_.fetch_add(1, std::memory_order_acq_rel);
}

void TextureCache::ClearDirty(Texture& t) {
  // Clear the flag, re-arm the page watches, then read: a write after the watch is armed marks
  // the texture stale again, a write before it is already in the data the untile reads.
  t.dirty.store(false, std::memory_order_release);
  ArmWatches(t.key, t.base_bytes, t.mips_bytes);
}

void TextureCache::ResetResolved(Texture& t) {
  std::fill(t.resolved_seq.begin(), t.resolved_seq.end(), 0);
  t.resolve_pending = true;
  change_epoch_.fetch_add(1, std::memory_order_acq_rel);
}

bool TextureCache::Unchanged(TextureHandle entry) const {
  const Slot& s = slots_[entry.slot];
  return s.generation == entry.generation && Clean(*s.texture);
}

void TextureCache::Touch(TextureHandle entry, uint64_t frame_fence) {
  const Slot& s = slots_[entry.slot];
  if (s.generation == entry.generation) MarkUsed(*s.texture, frame_fence);
}

void TextureCache::MarkUsed(Texture& t, uint64_t frame_fence) {
  if (t.used_fence == frame_fence) return;
  t.used_fence = frame_fence;
  t.used_ms = now_ms_;
  if (lru_last_ == &t) return;
  if (t.lru_prev || lru_first_ == &t) {
    (t.lru_prev ? t.lru_prev->lru_next : lru_first_) = t.lru_next;
    t.lru_next->lru_prev = t.lru_prev;  // not the last, so it has a next
  }
  t.lru_prev = lru_last_;
  t.lru_next = nullptr;
  (lru_last_ ? lru_last_->lru_next : lru_first_) = &t;
  lru_last_ = &t;
}

void TextureCache::WatchRange(Texture& t, uint32_t start, uint32_t length) {
  uint32_t end;
  if (!ClampRange(start, length, &end)) return;
  std::lock_guard<std::mutex> lock(watch_mutex_);
  for (uint32_t b = start >> kWatchBucketShift; b <= (end - 1) >> kWatchBucketShift; ++b)
    watch_buckets_[b].push_back({start, end, &t});
}

TextureCache::Texture* TextureCache::NewTexture(const TextureKey& key) {
  uint32_t slot;
  if (!free_slots_.empty()) {
    slot = free_slots_.back();
    free_slots_.pop_back();
  } else {
    slot = uint32_t(slots_.size());
    slots_.emplace_back();
  }
  Slot& s = slots_[slot];
  s.texture = std::make_unique<Texture>();
  Texture* t = s.texture.get();
  t->key = key;
  t->slot = slot;
  textures_.emplace(key, t);
  return t;
}

TextureCache::Texture* TextureCache::Create(const TextureKey& key, const HostFormat& guest_host) {
  Texture& t = *NewTexture(key);
  t.layout = GetGuestLayout(key);
  GetGuestExtents(key, t.layout, &t.base_bytes, &t.mips_bytes);
  t.resolved_seq.assign(size_t(key.mip_levels()) * key.array_size(), 0);

  const rex::graphics::FormatInfo* info = rex::graphics::FormatInfo::Get(key.format);
  bool unaligned = (key.width % info->block_width) || (key.height % info->block_height);
  // Local change (15/11/2026): a card without unaligned BC textures (Windows 10) gets such a
  // texture (a 128x1 DXT1) decoded on the CPU into R8G8B8A8 (R8G8 for DXN). Before, it was not
  // drawn at all (a player's race HUD lost its minimap).
  const HostFormat* decompressed =
      guest_host.block_compressed && unaligned && !unaligned_bc_supported_
          ? GetDecompressedHostFormat(key.format)
          : nullptr;
  if (decompressed)
    PGR4_LOG_FIRST(8, INFO,
                   "PGR4 native textures: {}x{} {} decoded on the CPU (no unaligned BC support)",
                   key.width, key.height, info->name);
  const HostFormat& host = decompressed ? *decompressed : guest_host;
  t.host = &host;
  if (host.block_compressed && unaligned && !unaligned_bc_supported_) {
    PGR4_WARN_ONCE(
        "PGR4 native textures: {}x{} {} needs decompression (no unaligned BC support); "
        "not supported (logged once)",
        key.width, key.height, info->name);
    t.failed = true;
  } else {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension =
        key.is_3d() ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = key.width;
    desc.Height = key.height;
    desc.DepthOrArraySize = uint16_t(key.depth_or_array_size);
    desc.MipLevels = uint16_t(key.mip_levels());
    desc.Format = host.resource;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    // D3D12 allows 4 KB placement only for a texture whose most detailed slice takes at most
    // 64 KB (D3D12_RESOURCE_DESC, small resources); vkd3d-proton applies the same estimate.
    const uint32_t bw = host.block_compressed ? info->block_width : 1;
    const uint32_t bh = host.block_compressed ? info->block_height : 1;
    const bool small_placement =
        uint64_t(key.width) * key.height * info->bytes_per_block() / (bw * bh) <=
        D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    HRESULT hr = CreateTextureResource(desc, small_placement, &t.resource, &t.placed, &t.bytes);
    if (FAILED(hr)) {
      PGR4_LOG_FIRST(1, ERROR,
                     "PGR4 native textures: texture resource {}x{}x{} {} failed: {:08X} (logged "
                     "once)",
                     key.width, key.height, key.depth_or_array_size, info->name, uint32_t(hr));
      t.failed = true;
      t.create_refused = true;
      t.retry_small = small_placement;
      t.retry_desc = desc;
      t.retry_at = retire_count_ + 60;
      t.bytes = 0;
      refused_until_ = retire_count_ + 60;
    } else {
      Created(t);
    }
  }
  return &t;
}

void TextureCache::Created(Texture& t) {
  resident_bytes_ += t.bytes;
  t.state = D3D12_RESOURCE_STATE_COPY_DEST;
  WatchRange(t, t.key.base_page << 12, t.base_bytes);
  WatchRange(t, t.key.mip_page << 12, t.mips_bytes);
  ForEachResolveAddress(t.key, t.layout, [&](size_t, uint32_t address) {
    resolve_targets_[address].push_back(&t);
  });
}

// Local change (pgr4 project): a texture the card refused (Create), once a second or so: the textures
// unused go meanwhile (MemoryPressure).
void TextureCache::RetryCreate(Texture& t) {
  if (FAILED(CreateTextureResource(t.retry_desc, t.retry_small, &t.resource, &t.placed, &t.bytes))) {
    t.bytes = 0;
    t.retry_at = retire_count_ + 60;
    refused_until_ = retire_count_ + 60;
    return;
  }
  PGR4_LOG_FIRST(8, INFO, "PGR4 native textures: {}x{} created on a later try", t.key.width, t.key.height);
  t.failed = false;
  t.create_refused = false;
  Created(t);
}

void TextureCache::UnwatchRange(Texture& t, uint32_t start, uint32_t length) {
  uint32_t end;
  if (!ClampRange(start, length, &end)) return;
  std::lock_guard<std::mutex> lock(watch_mutex_);
  for (uint32_t b = start >> kWatchBucketShift; b <= (end - 1) >> kWatchBucketShift; ++b) {
    std::vector<Watch>& v = watch_buckets_[b];
    v.erase(std::remove_if(v.begin(), v.end(), [&](const Watch& w) { return w.texture == &t; }),
            v.end());
  }
}

void TextureCache::Destroy(Texture& t) {
  // The pages stay armed: a later write faults once, finds no watch and unwatches them.
  UnwatchRange(t, t.key.base_page << 12, t.base_bytes);
  UnwatchRange(t, t.key.mip_page << 12, t.mips_bytes);
  ForEachResolveAddress(t.key, t.layout, [&](size_t, uint32_t address) {
    auto it = resolve_targets_.find(address);
    if (it == resolve_targets_.end()) return;
    std::vector<Texture*>& v = it->second;
    v.erase(std::remove(v.begin(), v.end(), &t), v.end());
    if (v.empty()) resolve_targets_.erase(it);
  });
  (t.lru_prev ? t.lru_prev->lru_next : lru_first_) = t.lru_next;
  (t.lru_next ? t.lru_next->lru_prev : lru_last_) = t.lru_prev;
  textures_.erase(t.key);
  resident_bytes_ -= t.bytes;
  if (t.placed != kCommitted) FreePlaced(t.placed, t.bytes);
  Slot& s = slots_[t.slot];
  ++s.generation;
  free_slots_.push_back(t.slot);
  s.texture.reset();  // releases the resource
}

void TextureCache::EvictOverBudget(uint64_t completed_fence) {
  // Over 90 % of the video memory budget, whatever the card: the textures unused for the soft
  // lifetime go even under the soft limit, and then the HD versions only they held (TrimHdCache).
  const bool tight = VideoMemoryTight(90) || MemoryPressure();
  if (!tight && resident_bytes_ <= budget_.soft_bytes) return;
  const uint64_t t0 = NowNs();
  evict_batch_.clear();
  evict_resources_.clear();
  std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> evict_hd;
  uint64_t bytes = resident_bytes_;
  for (Texture* t = lru_first_; t && evict_batch_.size() < kMaxEvictionsPerFrame; t = t->lru_next) {
    // A frame in flight may read it, and every texture used after it.
    if (t->used_fence > completed_fence) break;
    if (bytes <= budget_.hard_bytes &&
        ((!tight && bytes <= budget_.soft_bytes) || now_ms_ - t->used_ms < budget_.soft_lifetime_ms))
      break;
    evict_batch_.push_back(t);
    evict_resources_.push_back(t->resource.Get());
    if (t->scaled_cube) evict_resources_.push_back(t->scaled_cube.Get());
    for (const auto& [cube, state] : t->other_cubes) evict_resources_.push_back(cube.Get());
    // Local change (pgr4 project): an HD version is shared (hd_cache_, other textures with its data): its
    // views go only with its last reference, below. Freed here, a new texture took their descriptors while
    // the live textures' memoized bindings still named them: another texture shown, until they were bound
    // again (the sign that shows a building, the car a facade; 06/11/2026).
    if (t->hd) evict_hd.push_back(t->hd);
    for (const auto& r : t->other_hd) evict_hd.push_back(r);
    bytes -= t->bytes;
  }
  if (evict_batch_.empty()) return;
  if (MemoryPressure())
    PGR4_LOG_FIRST(16, INFO, "PGR4 native textures: {}: {} unused textures evicted",
                   views_tight_ ? "view heap nearly full" : "a texture refused", evict_batch_.size());
  if (tight)
    PGR4_LOG_FIRST(16, INFO, "PGR4 native textures: video memory {} of {} MB: {} unused textures evicted",
                   video_usage_ >> 20, video_budget_ >> 20, evict_batch_.size());
  if (!stats_.evicted)
    REXLOG_INFO(
        "PGR4 native textures: {} MB in {} textures, over the budget (soft {} MB for {} s "
        "unused, hard {} MB): evicting",
        resident_bytes_ >> 20, textures_.size(), budget_.soft_bytes >> 20,
        budget_.soft_lifetime_ms / 1000, budget_.hard_bytes >> 20);
  if (evict_callback_) evict_callback_(evict_resources_);
  std::sort(evict_batch_.begin(), evict_batch_.end());
  for (size_t i = 0; i < (size_t(1) << kGetMemoBits); ++i) {
    GetMemo& m = get_memo_[i];
    if (m.texture && std::binary_search(evict_batch_.begin(), evict_batch_.end(), m.texture))
      m.texture = nullptr;
  }
  for (Texture* t : evict_batch_) Destroy(*t);
  // The HD versions nothing else holds now: their views too.
  std::sort(evict_hd.begin(), evict_hd.end(),
            [](const auto& a, const auto& b) { return a.Get() < b.Get(); });
  evict_hd.erase(std::unique(evict_hd.begin(), evict_hd.end(),
                             [](const auto& a, const auto& b) { return a.Get() == b.Get(); }),
                 evict_hd.end());
  evict_resources_.clear();
  for (const auto& r : evict_hd) {
    r->AddRef();
    if (r->Release() == 1) evict_resources_.push_back(r.Get());
  }
  if (evict_callback_ && !evict_resources_.empty()) evict_callback_(evict_resources_);
  if (heap_failed_) {
    heap_failed_ = false;
    if (!spare_heap_.valid()) StartSpareHeap();
  }
  stats_.evicted += evict_batch_.size();
  change_epoch_.fetch_add(1, std::memory_order_acq_rel);
  stats_.evict_ns += NowNs() - t0;
}

void TextureCache::AddFreeRange(uint64_t offset, uint64_t size) {
  free_ranges_.emplace(offset, size);
  free_by_size_.emplace(size, offset);
}

uint64_t TextureCache::AllocatePlaced(uint64_t size, uint64_t alignment) {
  for (auto it = free_by_size_.lower_bound({size, 0}); it != free_by_size_.end(); ++it) {
    const auto [range_size, start] = *it;
    const uint64_t at = (start + alignment - 1) & ~(alignment - 1);
    const uint64_t end = start + range_size;
    if (at + size > end) continue;
    free_by_size_.erase(it);
    free_ranges_.erase(start);
    if (at > start) AddFreeRange(start, at - start);
    if (at + size < end) AddFreeRange(at + size, end - (at + size));
    return at;
  }
  return kCommitted;
}

void TextureCache::FreePlaced(uint64_t offset, uint64_t size) {
  uint64_t end = offset + size;
  // Coalesce with the free neighbours in the same heap.
  auto next = free_ranges_.lower_bound(offset);
  if (next != free_ranges_.end() && next->first == end && end % kTextureHeapSize) {
    end += next->second;
    free_by_size_.erase({next->second, next->first});
    next = free_ranges_.erase(next);
  }
  if (next != free_ranges_.begin() && offset % kTextureHeapSize) {
    auto prev = std::prev(next);
    if (prev->first + prev->second == offset) {
      offset = prev->first;
      free_by_size_.erase({prev->second, prev->first});
      free_ranges_.erase(prev);
    }
  }
  AddFreeRange(offset, end - offset);
}

bool TextureCache::AddHeap() {
  if (!spare_heap_.valid()) StartSpareHeap();
  const uint64_t t0 = NowNs();
  ComPtr<ID3D12Heap> heap = spare_heap_.get();
  const uint64_t wait_ns = NowNs() - t0;
  if (wait_ns >= 1000000)
    REXLOG_INFO("PGR4 native textures: waited {:.1f} ms for texture heap {}", wait_ns / 1e6,
                heaps_.size() + 1);
  if (!heap) {
    heap_failed_ = true;
    PGR4_WARN_ONCE("PGR4 native textures: heap creation failed; committed textures");
    return false;
  }
  AddFreeRange(heaps_.size() * kTextureHeapSize, kTextureHeapSize);
  heaps_.push_back(std::move(heap));
  StartSpareHeap();
  return true;
}

HRESULT TextureCache::CreateTextureResource(D3D12_RESOURCE_DESC& desc, bool small_placement,
                                            ComPtr<ID3D12Resource>* out, uint64_t* placed,
                                            uint64_t* bytes) {
  // 4 KB placement when the driver allows it for this texture, else 64 KB. Outside the size
  // rule the 4 KB query only fails (and vkd3d-proton logs it twice): 343 textures of a race run.
  D3D12_RESOURCE_ALLOCATION_INFO info = {};
  if (small_placement) {
    desc.Alignment = D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT;
    info = device_->GetResourceAllocationInfo(0, 1, &desc);
  }
  if (info.Alignment != D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT) {
    desc.Alignment = 0;
    info = device_->GetResourceAllocationInfo(0, 1, &desc);
  }
  *placed = kCommitted;
  *bytes = info.SizeInBytes == UINT64_MAX ? 0 : info.SizeInBytes;
  if (info.SizeInBytes != UINT64_MAX && info.SizeInBytes <= kTextureHeapSize) {
    uint64_t offset = AllocatePlaced(info.SizeInBytes, info.Alignment);
    if (offset == kCommitted && !heap_failed_ && AddHeap())
      offset = AllocatePlaced(info.SizeInBytes, info.Alignment);
    if (offset != kCommitted) {
      if (SUCCEEDED(device_->CreatePlacedResource(heaps_[offset / kTextureHeapSize].Get(),
                                                  offset % kTextureHeapSize, &desc,
                                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                  IID_PPV_ARGS(out->ReleaseAndGetAddressOf())))) {
        *placed = offset;
        return S_OK;
      }
      FreePlaced(offset, info.SizeInBytes);
    }
  }
  desc.Alignment = 0;
  D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT};
  return device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
                                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(out->ReleaseAndGetAddressOf()));
}

void TextureCache::StartSpareHeap() {
  spare_heap_ = std::async(std::launch::async, [device = device_, n = heaps_.size() + 1] {
    const uint64_t t0 = NowNs();
    D3D12_HEAP_DESC hd = {};
    hd.SizeInBytes = kTextureHeapSize;
    hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    hd.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
    ComPtr<ID3D12Heap> heap;
    if (FAILED(device->CreateHeap(&hd, IID_PPV_ARGS(&heap)))) return ComPtr<ID3D12Heap>();
    LowResidency(device, heap.Get());
    REXLOG_INFO("PGR4 native textures: texture heap {} created in {:.1f} ms (background)", n,
                (NowNs() - t0) / 1e6);
    return heap;
  });
}

void TextureCache::Transition(Texture& t, ID3D12GraphicsCommandList* list,
                              D3D12_RESOURCE_STATES to) {
  if (t.state == to) return;
  const D3D12_RESOURCE_BARRIER b = TransitionBarrier(t.resource.Get(), t.state, to);
  list->ResourceBarrier(1, &b);
  t.state = to;
}

bool TextureCache::Upload(Texture& t, ID3D12GraphicsCommandList* list, uint64_t frame_fence,
                          bool defer) {
  const TextureKey& key = t.key;
  uint64_t size = GetHostFootprints(key, true, &footprints_, t.host);
  if (!size) return false;
  if (ResolvesCoverAll(t)) {
    // The CopyResolved that follows overwrites every subresource in full (it copies even over
    // fresh guest data), so the untile and its copies would be dead work: 1280x720 RGBA8
    // resolve destinations took 1.6-3.4 ms of untile each.
    ClearDirty(t);
    ResetResolved(t);
    profile::Count(profile::kUploadSkipped);
    return true;
  }
  UploadPool::Alloc a;
  if (!upload_.Allocate(size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, frame_fence, &a))
    return false;

  // ClearDirty's order, with the watches armed by the untile job just before it reads: their
  // protection changes (about 20 µs each under Wine) run off the render thread.
  t.dirty.store(false, std::memory_order_release);
  if (Guarded(t)) {
    t.guard_hash = XXH3_64bits(membase_ + (key.base_page << 12), t.base_bytes);
    t.guard_frame = retire_count_;
  }
  UntileJob job;
  UploadCopies& copies = job.copies;
  copies.texture = t.resource.Get();
  copies.source = a.resource;
  copies.source_offset = a.offset;
  copies.format = t.host->resource;
  if (t.host->block_compressed) {
    const rex::graphics::FormatInfo* info = rex::graphics::FormatInfo::Get(key.format);
    copies.block_width = info->block_width;
    copies.block_height = info->block_height;
  }
  copies.mip_levels = key.mip_levels();
  if (defer) {
    job.deferred = true;
    job.state = t.state;
    job.frame_fence = frame_fence;
    t.state = kShaderRead;
    ++stats_.deferred_uploads;
  } else {
    // The copies read the upload memory only once the list is submitted, after FinishUploads.
    Transition(t, list, D3D12_RESOURCE_STATE_COPY_DEST);
    RecordCopies(list, copies, footprints_);
  }
  job.key = key;
  job.layout = t.layout;
  if (key.base_page) {
    job.guest.base = membase_ + (key.base_page << 12);
    job.guest.base_size = t.base_bytes;
  }
  if (key.mip_page) {
    job.guest.mips = membase_ + (key.mip_page << 12);
    job.guest.mips_size = t.mips_bytes;
  }
  job.footprints = std::move(footprints_);
  job.dst = a.cpu;
  job.size = size;
  job.upload_number = ++t.uploads;
  job.base_bytes = t.base_bytes;
  job.mips_bytes = t.mips_bytes;
  job.conversion = t.host->conversion;
  job.host = t.host;
  if (workers_.empty()) {
    ArmWatches(job.key, job.base_bytes, job.mips_bytes);
    stats_.untile_ns += RunUntile(job, &untile_scratch_);
    if (job.deferred) stats_.upload_record_ns += RecordDeferredUpload(job, &upload_lists_[0]);
    footprints_ = std::move(job.footprints);
  } else {
    {
      std::lock_guard<std::mutex> lock(jobs_mutex_);
      jobs_.push_back(std::move(job));
      ++jobs_busy_;
    }
    jobs_cv_.notify_one();
  }

  SetAlias(t, nullptr);  // the resource holds the newer data now
  ResetResolved(t);
  ApplyHd(t, list, frame_fence);
  ++stats_.uploads;
  stats_.upload_bytes += size;
  return true;
}

ID3D12Resource* TextureCache::ResolvedSource(const Texture& t, const ResolvedImage& image) const {
  // Local change (pgr4 project): a much larger image at the same address is not this texture's: the game
  // reused the memory. The number plate drawn on the GPU (256x256 at 0FDCA000) went into the 8x6 grid a car's
  // vertex shaders read for its dents, and that car stayed crumpled from the race's start (06/11/2026).
  if (image.width > t.key.width * 2 || image.height > t.key.height * 2) {
    PGR4_LOG_FIRST(16, INFO, "PGR4 native textures: resolve {}x{} at {:08X} not taken by a {}x{} texture",
                   image.width, image.height, image.guest_base, t.key.width, t.key.height);
    return nullptr;
  }
  if (image.texture && Family(image.texture->GetDesc().Format) == Family(t.host->resource))
    return image.texture;
  if (image.depth_r32f && Family(t.host->resource) == DXGI_FORMAT_R32_TYPELESS)
    return image.depth_r32f;
  return nullptr;  // incompatible view of the destination: keep the memory copy
}

bool TextureCache::ResolvesCoverAll(const Texture& t) const {
  const TextureKey& key = t.key;
  if (!resolved_lookup_ || key.is_3d()) return false;
  size_t covered = 0;
  bool all = true;
  ForEachResolveAddress(key, t.layout, [&](size_t index, uint32_t address) {
    if (!all) return;
    const ResolvedImage* image = resolved_lookup_(address);
    ID3D12Resource* source = image ? ResolvedSource(t, *image) : nullptr;
    if (!source) {
      all = false;
      return;
    }
    const D3D12_RESOURCE_DESC sd = source->GetDesc();
    const uint32_t level = uint32_t(index % key.mip_levels());
    // In guest texels: the image is `scale` times larger (DrawResolutionScale).
    all = sd.Width / image->scale >= std::max(key.width >> level, 1u) &&
          sd.Height / image->scale >= std::max(key.height >> level, 1u);
    ++covered;
  });
  return all && covered == t.resolved_seq.size();
}


bool TextureCache::CopyScaledCube(Texture& t, ID3D12GraphicsCommandList* list) {
  const TextureKey& key = t.key;
  if (key.dimension != xenos::DataDimension::kCube || key.mip_levels() != 1 ||
      t.resolved_seq.size() != 6 || !REXCVAR_GET(pgr4_native_scaled_cube) || !EnhancedReflections())
    return false;
  // Every face a resolved image of one scale above 1, its whole size the face's times it.
  const ResolvedImage* images[6] = {};
  ID3D12Resource* sources[6] = {};
  uint32_t scale = 0;
  bool all = true;
  ForEachResolveAddress(key, t.layout, [&](size_t index, uint32_t address) {
    const ResolvedImage* image = resolved_lookup_(address);
    ID3D12Resource* source = image ? ResolvedSource(t, *image) : nullptr;
    if (!source || index >= 6 || image->scale <= 1 || (scale && image->scale != scale)) {
      all = false;
      return;
    }
    scale = image->scale;
    const D3D12_RESOURCE_DESC sd = source->GetDesc();
    if (sd.Width < uint64_t(key.width) * scale || sd.Height < key.height * scale) all = false;
    images[index] = image;
    sources[index] = source;
  });
  if (!all || !scale) return false;
  const uint32_t size = key.width * scale;
  if (t.scaled_cube && t.scaled_cube->GetDesc().Width != size) {
    // Another size: set aside (never released while the texture lives), and one of this size
    // taken back if there is one, all of its faces copied again.
    t.other_cubes.emplace_back(std::move(t.scaled_cube), t.scaled_cube_state);
    for (auto it = t.other_cubes.begin(); it != t.other_cubes.end(); ++it)
      if (it->first->GetDesc().Width == size) {
        t.scaled_cube = std::move(it->first);
        t.scaled_cube_state = it->second;
        t.other_cubes.erase(it);
        break;
      }
    std::fill(t.resolved_seq.begin(), t.resolved_seq.end(), 0);
  }
  if (!t.scaled_cube) {
    D3D12_RESOURCE_DESC desc = t.resource->GetDesc();
    desc.Width = size;
    desc.Height = size;
    desc.DepthOrArraySize = 6;
    desc.MipLevels = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    t.scaled_cube_state = D3D12_RESOURCE_STATE_COPY_DEST;
    if (FAILED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
                                                t.scaled_cube_state, nullptr,
                                                IID_PPV_ARGS(&t.scaled_cube)))) {
      PGR4_WARN_ONCE("PGR4 native textures: scaled cube {}x{} not created", size, size);
      t.scaled_cube.Reset();
      return false;
    }
    std::fill(t.resolved_seq.begin(), t.resolved_seq.end(), 0);
    PGR4_LOG_FIRST(8, INFO, "PGR4 native textures: cube at {:08X} bound at {}x{} (scale {})",
                   key.base_page << 12, size, size, scale);
  }
  if (t.scaled_cube_state != D3D12_RESOURCE_STATE_COPY_DEST) {
    const D3D12_RESOURCE_BARRIER b = TransitionBarrier(t.scaled_cube.Get(), t.scaled_cube_state,
                                                       D3D12_RESOURCE_STATE_COPY_DEST);
    list->ResourceBarrier(1, &b);
    t.scaled_cube_state = D3D12_RESOURCE_STATE_COPY_DEST;
  }
  // The faces that changed, whole (the images are in the Resolver's read state, with COPY_SOURCE).
  for (uint32_t f = 0; f < 6; ++f) {
    if (images[f]->seq == t.resolved_seq[f]) continue;
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = t.scaled_cube.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = f;
    src.pResource = sources[f];
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX box = {0, 0, 0, size, size, 1};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    t.resolved_seq[f] = images[f]->seq;
    ++stats_.resolved_copies;
  }
  const D3D12_RESOURCE_BARRIER b =
      TransitionBarrier(t.scaled_cube.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kShaderRead);
  list->ResourceBarrier(1, &b);
  t.scaled_cube_state = kShaderRead;
  SetAlias(t, t.scaled_cube.Get());
  return true;
}

// Local change (pgr4 project), debug: the cars' surroundings cube read back (textures.h).
namespace {
constexpr uint32_t kCubeDumpAddress = 0x104A8000;
std::atomic<int> g_cube_dump_frames{0};
std::atomic<int> g_cube_log_frames{0};
}  // namespace
void RequestCubeDump(int frames) { g_cube_dump_frames.store(frames, std::memory_order_relaxed); }
void RequestCubeLog(int frames) { g_cube_log_frames.store(frames, std::memory_order_relaxed); }
bool CubeLogActive() { return g_cube_log_frames.load(std::memory_order_relaxed) > 0; }

void TextureCache::DumpCube(Texture& t, ID3D12Resource* resource, ID3D12GraphicsCommandList* list,
                            uint64_t frame_fence) {
  CubeDump d;
  d.stem = (std::filesystem::path(REXCVAR_GET(pgr4_native_capture_dir)) / "cube-f").string() +
           std::to_string(retire_count_);
  const D3D12_RESOURCE_DESC desc = resource->GetDesc();
  UINT64 total = 0, offset = 0;
  for (uint32_t f = 0; f < 6; ++f) {
    UINT64 size = 0;
    device_->GetCopyableFootprints(&desc, f * desc.MipLevels, 1, offset, &d.layout[f], &d.rows[f],
                                   &d.row_bytes[f], &size);
    offset = (d.layout[f].Offset + size + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) &
             ~UINT64(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
    total = offset;
  }
  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC bd = {};
  bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bd.Width = total;
  bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
  bd.SampleDesc.Count = 1;
  bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(&d.readback))))
    return;
  // The scaled cube's state is its own (CopyScaledCube); the game's texture's goes through
  // Transition, with COPY_SOURCE added to its shader-read state.
  const bool scaled = resource == t.scaled_cube.Get();
  if (scaled) {
    const D3D12_RESOURCE_BARRIER b =
        TransitionBarrier(resource, t.scaled_cube_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, &b);
  } else if (resource == t.resource.Get()) {
    Transition(t, list, kShaderRead | D3D12_RESOURCE_STATE_COPY_SOURCE);
  } else {
    return;  // another alias (an HD version): not the cube the game draws
  }
  for (uint32_t f = 0; f < 6; ++f) {
    D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
    src.pResource = resource;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = f * desc.MipLevels;
    dst.pResource = d.readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = d.layout[f];
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
  if (scaled) {
    const D3D12_RESOURCE_BARRIER b =
        TransitionBarrier(resource, D3D12_RESOURCE_STATE_COPY_SOURCE, t.scaled_cube_state);
    list->ResourceBarrier(1, &b);
  }
  d.fence = frame_fence;
  cube_dumps_.push_back(std::move(d));
}

void TextureCache::ProbeSky(Texture& t, ID3D12GraphicsCommandList* list, uint64_t frame_fence) {
  constexpr uint64_t kSlot = 4 * 4096;  // 4 block rows of 512 DXT1 blocks
  const uint32_t slot = uint32_t(retire_count_ % 8);
  if (sky_probe_[slot].fence) return;
  if (!sky_buffer_) {
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = 8 * kSlot;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&sky_buffer_))))
      return;
  }
  Transition(t, list, kShaderRead | D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
  src.pResource = t.resource.Get();
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dst.pResource = sky_buffer_.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint.Offset = uint64_t(slot) * kSlot;
  dst.PlacedFootprint.Footprint = {t.resource->GetDesc().Format, 2048, 16, 1, 4096};
  const D3D12_BOX box = {0, 0, 0, 2048, 16, 1};
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
  sky_probe_[slot] = {frame_fence, retire_count_};
}

void TextureCache::WriteSkyProbes(uint64_t completed_fence) {
  for (uint32_t s = 0; s < 8; ++s) {
    auto& p = sky_probe_[s];
    if (!p.fence || p.fence > completed_fence) continue;
    const D3D12_RANGE range = {size_t(s) * 4 * 4096, size_t(s + 1) * 4 * 4096};
    uint8_t* data = nullptr;
    if (SUCCEEDED(sky_buffer_->Map(0, &range, reinterpret_cast<void**>(&data)))) {
      REXLOG_INFO("PGR4 native sky probe: frame {} top {:016X}", p.frame,
                  XXH3_64bits(data + range.Begin, 4 * 4096));
      const D3D12_RANGE none = {0, 0};
      sky_buffer_->Unmap(0, &none);
    }
    p.fence = 0;
  }
}

void TextureCache::WriteCubeDumps(uint64_t completed_fence) {
  for (auto it = cube_dumps_.begin(); it != cube_dumps_.end();) {
    if (completed_fence < it->fence) {
      ++it;
      continue;
    }
    uint8_t* data = nullptr;
    if (SUCCEEDED(it->readback->Map(0, nullptr, reinterpret_cast<void**>(&data)))) {
      for (uint32_t f = 0; f < 6; ++f) {
        const std::string stem = it->stem + "-" + std::to_string(f);
        if (std::FILE* out = std::fopen((stem + ".bin").c_str(), "wb")) {
          for (UINT y = 0; y < it->rows[f]; ++y)
            std::fwrite(data + it->layout[f].Offset + uint64_t(y) * it->layout[f].Footprint.RowPitch,
                        1, size_t(it->row_bytes[f]), out);
          std::fclose(out);
        }
        if (std::FILE* out = std::fopen((stem + ".txt").c_str(), "w")) {
          std::fprintf(out, "%u %u %llu %u\n", it->layout[f].Footprint.Width, it->rows[f],
                       (unsigned long long)it->row_bytes[f],
                       uint32_t(it->layout[f].Footprint.Format));
          std::fclose(out);
        }
      }
      const D3D12_RANGE none = {0, 0};
      it->readback->Unmap(0, &none);
      REXLOG_INFO("PGR4 native textures: cube dumped to {}-*", it->stem);
    }
    it = cube_dumps_.erase(it);
  }
}

// Local change (pgr4 project): the texture is bound to one of its HD versions (the current one, or
// one its data had before, in other_hd).
bool TextureCache::BoundToHd(const Texture& t) {
  if (!t.alias) return false;
  if (t.alias == t.hd.Get()) return true;
  for (const auto& r : t.other_hd)
    if (t.alias == r.Get()) return true;
  return false;
}

void TextureCache::ApplyHd(Texture& t, ID3D12GraphicsCommandList* list, uint64_t frame_fence) {
  const TextureKey& key = t.key;
  t.hd_current = false;
  // Unbound from its HD version until it is current again: an upload whose HD version was not
  // ready (or of new data, its old HD version moved to other_hd) left it bound to that one while
  // no longer hd_current, out of the HD switch's reach (HD shown with it off) and, for new data,
  // showing another texture's HD version.
  if (BoundToHd(t)) SetAlias(t, nullptr);
  const uint64_t pending = t.hd_pending;
  t.hd_pending = 0;
  // Local change: 3D textures too (the cars' rain drops), their HD file a volume DDS. And the
  // 8888 ones (the HUD's rev counters: one picture per car), their HD file R8G8B8A8 in the
  // game's texture's byte order (as the dumps have it).
  const DXGI_FORMAT resource_format = t.resource->GetDesc().Format;
  const bool rgba8 = resource_format == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
                     resource_format == DXGI_FORMAT_R8G8B8A8_UNORM;
  if (!key.base_page || key.dimension == xenos::DataDimension::kCube ||
      (!key.is_3d() && key.array_size() != 1) || !(t.host->block_compressed || rgba8) ||
      IsDecompressed(*t.host))
    return;
  HdFiles& hd = Hd();
  // Without an HD pack, still the 128 x 128 ones: the snow's flakes tell that it snows (draw.cpp).
  if (hd.files.empty() && !REXCVAR_GET(pgr4_native_texture_catalog) &&
      !(key.width == 128 && key.height == 128))
    return;
  GuestTextureMemory guest;
  guest.base = membase_ + (key.base_page << 12);
  guest.base_size = t.base_bytes;
  const uint64_t hash = TextureHash(key, guest);
  t.content_hash = hash;
  if (const std::string trace = REXCVAR_GET(pgr4_native_texture_hd_trace); !trace.empty() &&
      std::strtoull(trace.c_str(), nullptr, 16) == hash)
    REXLOG_INFO("PGR4 native textures: trace {:016X}: uploaded {}x{} at {:08X}, HD file {}, "
                "HD {}", hash, key.width, key.height, key.base_page << 12,
                hd.files.count(hash) ? "listed" : "not listed",
                t.hd_hash == hash ? (t.hd ? "loaded" : "pending") : "not taken yet");
  if (hd.files.empty()) return;
  auto it = hd.files.find(hash);
  if (it == hd.files.end()) return;
  // Local change (pgr4 project): the PlayStation version of a texture (the button icons) is not covered
  // by the HD version of the Xbox one: the pack has CB9C371CA8213F81, so the icons stayed Xbox in the
  // game whatever the controller (06/11/2026).
  if (rex::input::PlayStationLayout()) {
    TextureFiles& files = Files();
    std::lock_guard<std::mutex> lock(files.mutex);
    if (files.playstation.count(hash)) return;
  }
  // Local change: the first HD texture of an area (a race's or the garage's loading) has the
  // rest of the area's read and made ahead (PumpPreload).
  PreloadArea(hash);
  if (t.hd_hash != hash) {
    if (t.hd) t.other_hd.push_back(std::move(t.hd));
    t.hd_hash = hash;
    t.hd_pending = hash;
  } else if (pending == hash) {
    t.hd_pending = hash;
  }
  if (t.hd_pending) {
    TakeHd(t, list, frame_fence);
    return;
  }
  t.hd_current = t.hd != nullptr;
  if (t.hd_current && HdOnFor(t.hd_hash)) SetAlias(t, t.hd.Get());
}

void TextureCache::TakeHd(Texture& t, ID3D12GraphicsCommandList* list, uint64_t frame_fence) {
  // Local change: loaded already (by this texture before it was evicted, or another with the
  // same data): bound at once.
  if (auto c = hd_cache_.find(t.hd_pending);
      c != hd_cache_.end() && c->second.format == t.resource->GetDesc().Format &&
      c->second.resource->GetDesc().Dimension == t.resource->GetDesc().Dimension) {
    t.hd = c->second.resource;
    t.hd_layers = c->second.layers;
    c->second.used = retire_count_;
    t.hd_pending = 0;
    t.hd_current = true;
    if (HdOnFor(t.hd_hash)) SetAlias(t, t.hd.Get());
    return;
  }
  // At most kHdFrameBytes of HD textures uploaded per frame; the rest wait for the next frames.
  // Local change: 8 times more on screens without a 3D scene (a loading screen: nothing to keep
  // smooth); none near the video memory budget (the game's own textures kept).
  const uint64_t kHdFrameBytes =
      (g_no_scene.load(std::memory_order_relaxed) ? 384ull : 48ull) << 20;
  if (hd_frame_bytes_.load(std::memory_order_relaxed) >= kHdFrameBytes) return;
  if (VideoMemoryTight(95)) return;
  const auto file = HdTake(Hd(), t.hd_pending);
  if (!file) return;
  t.hd_pending = 0;
  hd_frame_bytes_.fetch_add(file->file.size() + file->normal.size(), std::memory_order_relaxed);
  if (!LoadHd(t, file->file, file->normal, list, frame_fence)) return;
  t.hd_current = true;
  if (const std::string trace = REXCVAR_GET(pgr4_native_texture_hd_trace);
      !trace.empty() && std::strtoull(trace.c_str(), nullptr, 16) == t.hd_hash)
    REXLOG_INFO("PGR4 native textures: trace {:016X}: HD loaded, {}", t.hd_hash,
                HdOnFor(t.hd_hash) ? "bound" : "HD off");
  if (HdOnFor(t.hd_hash)) SetAlias(t, t.hd.Get());
}

bool TextureCache::MakeHd(uint64_t hash, const std::vector<uint8_t>& file,
                          const std::vector<uint8_t>& normal, DXGI_FORMAT format,
                          ID3D12GraphicsCommandList* list, uint64_t frame_fence, HdCached* out) {
  auto u32 = [&](size_t o) {
    uint32_t v = 0;
    if (o + 4 <= file.size()) std::memcpy(&v, file.data() + o, 4);
    return v;
  };
  // DDS: "DDS ", then the header (height at 12, width at 16, mips at 28, the pixel format's
  // FourCC at 84; DX10's DXGI format at 128 after it).
  if (file.size() < 128 || std::memcmp(file.data(), "DDS ", 4)) {
    PGR4_LOG_FIRST(8, WARN, "PGR4 native textures: {:016X}.dds: {} bytes read, not a DDS", hash,
                   file.size());
    return false;
  }
  const uint32_t height = u32(12), width = u32(16), mips = std::max(1u, u32(28));
  // A volume (DDSD_DEPTH in the flags at 8, DDSCAPS2_VOLUME in the caps at 112): its depth at 24,
  // halved at each level as its width and height.
  const bool volume = (u32(8) & 0x800000u) && (u32(112) & 0x200000u) && u32(24) > 1;
  const uint32_t depth = volume ? u32(24) : 1;
  const uint32_t fourcc = u32(84);
  size_t data = 128;
  DXGI_FORMAT file_format = DXGI_FORMAT_UNKNOWN;
  if (fourcc == 0x31545844) file_format = DXGI_FORMAT_BC1_UNORM;  // DXT1
  else if (fourcc == 0x35545844) file_format = DXGI_FORMAT_BC3_UNORM;  // DXT5
  else if (fourcc == 0x32495441) file_format = DXGI_FORMAT_BC5_UNORM;  // ATI2
  // Local change (pgr4 project, 14/11/2026): the other classic headers image editors write (a texture
  // of the dump, pgr4_native_texture_dump_dds, changed and put in textures_hd).
  else if (fourcc == 0x33545844 || fourcc == 0x32545844) file_format = DXGI_FORMAT_BC2_UNORM;  // DXT3, DXT2
  else if (fourcc == 0x34545844) file_format = DXGI_FORMAT_BC3_UNORM;                          // DXT4
  else if (fourcc == 0x31495441 || fourcc == 0x55344342) file_format = DXGI_FORMAT_BC4_UNORM;  // ATI1, BC4U
  else if (fourcc == 0x55354342) file_format = DXGI_FORMAT_BC5_UNORM;                          // BC5U
  else if (!fourcc && (u32(80) & 0x40) && u32(88) == 32) {
    // Uncompressed 32-bit: RGBA as it is; BGRA (the common A8R8G8B8) read again with red and blue
    // swapped.
    if (u32(92) == 0x000000FF && u32(100) == 0x00FF0000) {
      file_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    } else if (u32(92) == 0x00FF0000 && u32(100) == 0x000000FF) {
      std::vector<uint8_t> rgba(file);
      for (size_t i = 128; i + 4 <= rgba.size(); i += 4) std::swap(rgba[i], rgba[i + 2]);
      const uint32_t masks[2] = {0x000000FF, 0x00FF0000};
      std::memcpy(rgba.data() + 92, &masks[0], 4);
      std::memcpy(rgba.data() + 100, &masks[1], 4);
      return MakeHd(hash, rgba, normal, format, list, frame_fence, out);
    }
  }
  else if (fourcc == 0x30315844) {  // DX10
    file_format = DXGI_FORMAT(u32(128));
    data = 148;
  }
  // Local change: 4 for R8G8B8A8 (a pixel; ApplyHd's 8888 textures).
  const auto block_of = [](DXGI_FORMAT f) {
    if (f == DXGI_FORMAT_R8G8B8A8_TYPELESS || f == DXGI_FORMAT_R8G8B8A8_UNORM) return 4u;
    return f == DXGI_FORMAT_BC1_TYPELESS || f == DXGI_FORMAT_BC1_UNORM ||
                   f == DXGI_FORMAT_BC1_UNORM_SRGB || f == DXGI_FORMAT_BC4_TYPELESS ||
                   f == DXGI_FORMAT_BC4_UNORM || f == DXGI_FORMAT_BC4_SNORM
               ? 8u
               : 16u;
  };
  // Bound in place of a texture (format given): its block size must be the texture's, and the
  // resource takes the texture's format (the views are made for it). Preloaded (UNKNOWN): the
  // file's, checked against the texture's when one takes it (TakeHd).
  if (format == DXGI_FORMAT_UNKNOWN) format = file_format;
  if (file_format == DXGI_FORMAT_UNKNOWN || block_of(file_format) != block_of(format) || !width ||
      !height) {
    PGR4_LOG_FIRST(8, WARN, "PGR4 native textures: {:016X}.dds does not match the game's format",
                   hash);
    return false;
  }
  // The normal map: the same header (size, levels, block format) and so the same layout.
  const bool with_normal = normal.size() == file.size() &&
                           std::memcmp(normal.data(), file.data(), data) == 0;
  const uint32_t layers = with_normal && !volume ? 2 : 1;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension =
      volume ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  // Alignment 0: the game's small textures are placed with the small resource alignment (4 KB,
  // CreateHost); copied from theirs for an HD version 16 times their size, GetCopyableFootprints
  // gave no levels (the file then looked short) and no texture of 64 KB or less had its HD one.
  desc.Alignment = 0;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = uint16_t(volume ? depth : layers);
  desc.MipLevels = uint16_t(mips);
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = D3D12_RESOURCE_FLAG_NONE;
  const uint32_t subresources = mips * layers;
  std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(subresources);
  std::vector<UINT> rows(subresources);
  std::vector<UINT64> row_bytes(subresources);
  UINT64 total = 0;
  device_->GetCopyableFootprints(&desc, 0, subresources, 0, layouts.data(), rows.data(),
                                 row_bytes.data(), &total);
  // The file's levels, packed, must be there.
  size_t need = data;
  for (uint32_t m = 0; m < mips; ++m)
    need += size_t(row_bytes[m]) * rows[m] * layouts[m].Footprint.Depth;
  if (file.size() < need) {
    PGR4_LOG_FIRST(8, WARN, "PGR4 native textures: {:016X}.dds is shorter than its {} levels",
                   hash, mips);
    return false;
  }
  UploadPool::Alloc a;
  if (!upload_.Allocate(total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, frame_fence, &a)) {
    PGR4_LOG_FIRST(8, WARN, "PGR4 native textures: {:016X}.dds: no upload memory ({} bytes)",
                   hash, total);
    return false;
  }
  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  Microsoft::WRL::ComPtr<ID3D12Resource> hd;
  if (const HRESULT hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
                                                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                          IID_PPV_ARGS(&hd));
      FAILED(hr)) {
    PGR4_LOG_FIRST(8, WARN, "PGR4 native textures: {:016X}.dds: {}x{} ({} levels) not created "
                   "({:08X})", hash, width, height, mips, uint32_t(hr));
    refused_until_ = retire_count_ + 60;
    return false;
  }
  LowResidency(device_, hd.Get());
  if (REXCVAR_GET(pgr4_native_dred)) {  // named in the report of a GPU that hung (d3d12_device.cpp)
    wchar_t name[32];
    std::swprintf(name, 32, L"HD %016llX", static_cast<unsigned long long>(hash));
    hd->SetName(name);
  }
  for (uint32_t layer = 0; layer < layers; ++layer) {
    const uint8_t* src = (layer ? normal : file).data() + data;
    for (uint32_t m = 0; m < mips; ++m) {
      const uint32_t i = m + layer * mips;
      // A volume's slices one after another, each its rows (DDS and the footprint alike).
      const UINT slices = rows[i] * layouts[i].Footprint.Depth;
      for (UINT r = 0; r < slices; ++r)
        std::memcpy(a.cpu + layouts[i].Offset + uint64_t(r) * layouts[i].Footprint.RowPitch,
                    src + uint64_t(r) * row_bytes[i], size_t(row_bytes[i]));
      src += size_t(row_bytes[i]) * slices;
      D3D12_TEXTURE_COPY_LOCATION dst = {}, from = {};
      dst.pResource = hd.Get();
      dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      dst.SubresourceIndex = i;
      from.pResource = a.resource;
      from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      from.PlacedFootprint = layouts[i];
      from.PlacedFootprint.Offset += a.offset;
      list->CopyTextureRegion(&dst, 0, 0, 0, &from, nullptr);
    }
  }
  if (!normal.empty() && !with_normal)
    PGR4_LOG_FIRST(8, WARN, "PGR4 native textures: {:016X}_n.dds does not match its texture",
                   hash);
  const D3D12_RESOURCE_BARRIER b =
      TransitionBarrier(hd.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kShaderRead);
  list->ResourceBarrier(1, &b);
  // Kept by its TextureHash (hd_cache_), for the textures with this data to come.
  auto& c = hd_cache_[hash];
  hd_cache_bytes_ += total - c.bytes;
  c = {hd, layers, total, retire_count_, format};
  if (out) *out = c;
  return true;
}

bool TextureCache::LoadHd(Texture& t, const std::vector<uint8_t>& file,
                          const std::vector<uint8_t>& normal, ID3D12GraphicsCommandList* list,
                          uint64_t frame_fence) {
  HdCached c;
  if (!MakeHd(t.hd_hash, file, normal, t.resource->GetDesc().Format, list, frame_fence, &c))
    return false;
  // Local change: a volume for a 3D texture, an image for the others (the views are the game's).
  if (c.resource->GetDesc().Dimension != t.resource->GetDesc().Dimension) {
    PGR4_LOG_FIRST(8, WARN, "PGR4 native textures: {:016X}.dds: not the texture's dimension",
                   t.hd_hash);
    return false;
  }
  t.hd = c.resource;
  t.hd_layers = c.layers;
  PGR4_LOG_FIRST(16, INFO, "PGR4 native textures: {:016X} {}x{} bound at {}x{} (HD)", t.hd_hash,
                 t.key.width, t.key.height, c.resource->GetDesc().Width,
                 c.resource->GetDesc().Height);
  return true;
}

bool TextureCache::CopyResolved(Texture& t, ID3D12GraphicsCommandList* list) {
  const TextureKey& key = t.key;
  if (CopyScaledCube(t, list)) return true;
  if (t.hd && t.alias == t.hd.Get()) return false;
  // A 2D texture with one subresource that a resolved image of its format family covers
  // exactly binds that image in place of its resource: no copy, and the draws read the texels
  // the resolve wrote. Most resolve destinations (full-screen buffers, shadow maps) are such
  // textures.
  if (t.resolved_seq.size() == 1 && !key.is_3d()) {
    const ResolvedImage* image = nullptr;
    ForEachResolveAddress(key, t.layout,
                          [&](size_t, uint32_t address) { image = resolved_lookup_(address); });
    ID3D12Resource* source = image ? ResolvedSource(t, *image) : nullptr;
    if (source) {
      const D3D12_RESOURCE_DESC sd = source->GetDesc();
      // The image is `scale` times the guest size (DrawResolutionScale); the draw's shaders get
      // the fetch constant in textures_resolution_scaled (draw.cpp, TextureBinding::aliased).
      // An image of another scale (the cars' cube surface: RenderTargets::OwnScale) is copied.
      if (image->scale == DrawResolutionScale() &&
          sd.Width == uint64_t(key.width) * image->scale &&
          sd.Height == key.height * image->scale) {
        SetAlias(t, source);
        t.resolved_seq[0] = image->seq;
        ++stats_.resolved_aliases;
        return true;
      }
    }
  }
  if (t.alias) {
    // Leaving the alias (the address now holds an image of another size or format): the
    // resource takes over the aliased image's texels, which resolved_seq describes, before the
    // copies below update it. Images outlive the cache's use of them (Resolver).
    Transition(t, list, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = t.resource.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = t.alias;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    // A scaled alias (DrawResolutionScale) is larger than the resource: copy what fits.
    const D3D12_RESOURCE_DESC ad = t.alias->GetDesc(), rd = t.resource->GetDesc();
    const D3D12_BOX fit = {0, 0, 0, UINT(std::min(ad.Width, rd.Width)),
                           std::min(ad.Height, rd.Height), 1};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, &fit);
    ++stats_.resolved_copies;
    SetAlias(t, nullptr);
  }
  bool any = false;
  ForEachResolveAddress(key, t.layout, [&](size_t index, uint32_t address) {
    const ResolvedImage* image = resolved_lookup_(address);
    if (!image) return;
    if (image->seq == t.resolved_seq[index]) {
      any = true;
      return;
    }
    ID3D12Resource* source = ResolvedSource(t, *image);
    if (!source) return;
    // A scaled image (DrawResolutionScale) does not fit the guest-size resource: copy its
    // guest-size downsample instead (Resolver::GuestSize).
    bool scaled = image->scale > 1;
    if (scaled && guest_size_) {
      if (ID3D12Resource* guest = guest_size_(list, *image, source)) {
        source = guest;
        scaled = false;
      }
    }
    D3D12_RESOURCE_DESC sd = source->GetDesc();
    D3D12_BOX box = {0, 0, 0, 0, 0, 1};
    const uint32_t level = uint32_t(index % key.mip_levels());
    box.right = std::min<uint32_t>(uint32_t(sd.Width), std::max(key.width >> level, 1u));
    box.bottom = std::min<uint32_t>(sd.Height, std::max(key.height >> level, 1u));
    // No guest-size downsample (B5G6R5, or no descriptors left this frame): a copy cannot
    // shrink the scaled image, so this takes its top-left guest-size corner. Logged.
    if (scaled)
      PGR4_LOG_FIRST(16, WARN,
                     "PGR4 native textures: scaled resolve {:08X} copied into a {}x{} level {} "
                     "texture (format {}): top-left corner only",
                     address, key.width, key.height, level, uint32_t(key.format));
    Transition(t, list, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = t.resource.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = UINT(index);
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = source;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    t.resolved_seq[index] = image->seq;
    ++stats_.resolved_copies;
    any = true;
  });
  return any;
}

bool TextureCache::Get(const uint32_t fetch_dwords[6], ID3D12GraphicsCommandList* list,
                       uint64_t frame_fence, TextureBinding* out, uint64_t captured) {
  ++stats_.gets;
  if (preload_frame_ != retire_count_) {
    preload_frame_ = retire_count_;
    PumpPreload(list, frame_fence);
  }
  // Local change (pgr4 project): the controller's family changed (rex::input::PlayStationLayout):
  // the textures with a PlayStation version (the button icons, the controls screen's controller)
  // made stale, so that their next use loads the other version.
  if (const bool playstation = rex::input::PlayStationLayout(); playstation != playstation_) {
    playstation_ = playstation;
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    {
      TextureFiles& files = Files();
      std::lock_guard<std::mutex> lock(files.mutex);
      ranges = files.switchable;
    }
    for (const auto& [start, length] : ranges) Invalidate(start, length, true);
    if (!ranges.empty())
      REXLOG_INFO("PGR4 native textures: {} controller, {} texture(s) reloaded",
                  playstation ? "PlayStation" : "Xbox", ranges.size());
  }
  // Local change (pgr4 project): "the game without our changes" switched: the replaced textures
  // (texture_replace_dir) made stale, so that their next use loads the game's or ours.
  static bool reflections_seen = EnhancedReflections();
  if (const bool original = OriginalLook(), reflections = EnhancedReflections();
      original != original_look_ || reflections != reflections_seen) {
    original_look_ = original;
    reflections_seen = reflections;
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    {
      TextureFiles& files = Files();
      std::lock_guard<std::mutex> lock(files.mutex);
      ranges = files.replaced;
    }
    for (const auto& [start, length] : ranges) Invalidate(start, length, true);
    if (!ranges.empty())
      REXLOG_INFO("PGR4 native textures: {} replaced texture(s) reloaded ({})", ranges.size(),
                  original ? "the game's" : "ours");
  }
  uint32_t hash = 0;
  for (int i = 0; i < 6; ++i) hash = (hash ^ fetch_dwords[i]) * 0x9E3779B1u;
  GetMemo& memo = get_memo_[hash >> (32 - kGetMemoBits)];
  const bool same_fetch =
      memo.texture && std::memcmp(memo.fetch, fetch_dwords, sizeof(memo.fetch)) == 0;
  // The generation also changes when the texture's alias does, which Clean does not see.
  if (same_fetch && !GuardMissed(*memo.texture) && Clean(*memo.texture) &&
      memo.binding.entry.generation == slots_[memo.texture->slot].generation &&
      !memo.texture->hd_pending &&
      !(memo.texture->hd_current && (memo.texture->alias == memo.texture->hd.Get()) !=
                                        HdOnFor(memo.texture->hd_hash))) {
    MarkUsed(*memo.texture, frame_fence);
    // Local change (pgr4 project), debug: the cube's sky (ProbeSky), served from here when clean.
    if ((memo.texture->key.base_page << 12) == 0x128A2000 && memo.texture->key.width == 2048 &&
        !memo.texture->alias && CubeLogActive() && sky_probed_at_ != retire_count_)
        [[unlikely]] {
      sky_probed_at_ = retire_count_;
      ProbeSky(*memo.texture, list, frame_fence);
    }
    if (!memo.texture->alias) Transition(*memo.texture, list, kShaderRead);
    *out = memo.binding;
    return true;
  }
  ++stats_.memo_misses;
  profile::Count(same_fetch ? profile::kGetMissChanged : profile::kGetMissFetch);
  TextureKey key;
  if (!TextureKeyFromFetch(fetch_dwords, &key)) return false;
  auto it = textures_.find(key);
  Texture* t;
  if (it != textures_.end()) {
    t = it->second;
  } else {
    const HostFormat* host = GetHostFormat(key.format);
    if (!host) {
      // Remembered so the key is not looked up again; the warning goes once per format.
      t = NewTexture(key);
      t->failed = true;
      const uint64_t bit = uint64_t(1) << (uint32_t(key.format) & 63);
      if (!(unsupported_logged_ & bit)) {
        unsupported_logged_ |= bit;
        REXLOG_WARN("PGR4 native textures: unsupported format {} (first {}x{} at {:08X})",
                    rex::graphics::FormatInfo::Get(key.format)->name, key.width, key.height,
                    key.base_page << 12);
      }
    } else {
      const uint64_t c0 = NowNs();
      t = Create(key, *host);
      stats_.create_ns += NowNs() - c0;
      ++stats_.created;
    }
  }
  if (t->create_refused && retire_count_ >= t->retry_at) [[unlikely]] RetryCreate(*t);
  if (t->failed) return false;
  const bool first_use = t->used_fence != frame_fence;
  MarkUsed(*t, frame_fence);
  GuardMissed(*t);  // local change: a write its page watch missed
  const bool was_dirty = t->dirty.load(std::memory_order_acquire);
  const bool stale =
      was_dirty && t->uploads && captured <= t->write_mark.load(std::memory_order_relaxed);
  // Logged at its 1st, 256th, 512th, 1024th... stale Get.
  if (stale &&
      (++t->stale_gets == 1 || (t->stale_gets >= 256 && !(t->stale_gets & (t->stale_gets - 1))))) {
    PGR4_LOG_FIRST(96, INFO,
                   "PGR4 native textures: {:08X} {}x{}x{} {} mips {} rewritten after a draw that "
                   "reads it was queued: {} times (upload {}, get {})",
                   key.base_page << 12, key.width, key.height, key.depth_or_array_size,
                   rex::graphics::FormatInfo::Get(key.format)->name, key.mip_levels(),
                   t->stale_gets, t->uploads, stats_.gets);
  }
  if (was_dirty && !(stale && capture_order_)) {
    const uint64_t u0 = NowNs();
    const bool uploaded = Upload(*t, list, frame_fence, first_use);
    stats_.upload_ns += NowNs() - u0;
    if (!uploaded) return false;
  }
#ifndef PGR4_TEXTURES_OFFLINE
  const uint64_t seq_before = t->resolved_seq.empty() ? 0 : t->resolved_seq[0];
#endif
  // CopyResolved only changes anything after an Upload or a resolve into one of the texture's
  // subresource addresses (OnResolved); both set resolve_pending.
  if (t->resolve_pending) {
    if (resolved_lookup_) {
      const uint64_t r0 = NowNs();
      CopyResolved(*t, list);
      stats_.copy_resolved_ns += NowNs() - r0;
    }
    t->resolve_pending = false;
  }
#ifndef PGR4_TEXTURES_OFFLINE
  if (trace_address_ && (key.base_page << 12) == trace_address_) [[unlikely]] {
    const ResolvedImage* image = resolved_lookup_ ? resolved_lookup_(trace_address_) : nullptr;
    REXLOG_INFO(
        "PGR4 native texture trace {:08X} {}x{} fmt{}: get #{} dirty_upload={} image_seq={} "
        "level0_seq {} -> {} image_fmt={} image_size={}x{}",
        trace_address_, key.width, key.height, uint32_t(key.format), stats_.gets, was_dirty,
        image ? image->seq : 0, seq_before, t->resolved_seq.empty() ? 0 : t->resolved_seq[0],
        image && image->texture ? uint32_t(image->texture->GetDesc().Format) : 0,
        image ? image->width : 0, image ? image->height : 0);
  }
#endif
  // Local change: an HD file the reader thread had not read at the upload, once it has.
  if (t->hd_pending) TakeHd(*t, list, frame_fence);
  // Local change: the HD version on or off as pgr4_native_texture_hd says, at once.
  if (t->hd_current) {
    const bool want = HdOnFor(t->hd_hash);
    if (want && !t->alias) SetAlias(*t, t->hd.Get());
    else if (!want && t->alias == t->hd.Get()) SetAlias(*t, nullptr);
  } else if (BoundToHd(*t)) {
    SetAlias(*t, nullptr);  // not hd_current: no HD version shows
  }
  // An alias is in the Resolver's read state, which includes kShaderRead.
  if (!t->alias) Transition(*t, list, kShaderRead);

  xenos::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, fetch_dwords, sizeof(fetch));
  out->resource = t->alias ? t->alias : t->resource.Get();
  out->content = t->content_hash;
  out->hd = t->alias != nullptr && t->alias == t->hd.Get() ? 'H'
            : t->hd_pending                                  ? 'P'
            : t->hd                                          ? 'O'
            : t->hd_hash && t->hd_hash == t->content_hash    ? 'F'
                                                             : '0';
  out->key = key;
  out->entry = {t->slot, slots_[t->slot].generation};
  // Local change (pgr4 project), development (pgr4_native_texture_watch): an HD version shown for a content
  // that is not its own. RecheckContents (below in the frame) covers the memory against the upload.
  if (REXCVAR_GET(pgr4_native_texture_watch) && out->hd == 'H' && t->hd_hash != t->content_hash) [[unlikely]] {
    PGR4_LOG_FIRST(200, WARN, "PGR4 texture watch: {:08X} {}x{} fmt {} HD {:016X} shown on content {:016X}",
                   key.base_page << 12, uint32_t(key.width), uint32_t(key.height), uint32_t(key.format),
                   t->hd_hash, t->content_hash);
  }
  // Local change (pgr4 project), debug: the sky the cube's faces draw (128A2000), its top block
  // rows read back while the cube is logged: whether its memory changes under it.
  if ((key.base_page << 12) == 0x128A2000 && key.width == 2048 && !t->alias &&
      CubeLogActive() && sky_probed_at_ != retire_count_) [[unlikely]] {
    sky_probed_at_ = retire_count_;
    ProbeSky(*t, list, frame_fence);
  }
  // Local change (pgr4 project), debug: the cars' surroundings cube's faces' resolved images at
  // each Get while asked (RequestCubeLog): their resolve's sequence, address, size, scale.
  if (key.dimension == xenos::DataDimension::kCube && (key.base_page << 12) == kCubeDumpAddress &&
      g_cube_log_frames.load(std::memory_order_relaxed) > 0 && resolved_lookup_) [[unlikely]] {
    if (cube_logged_at_ != retire_count_) {
      cube_logged_at_ = retire_count_;
      g_cube_log_frames.fetch_sub(1, std::memory_order_relaxed);
    }
    std::string line;
    ForEachResolveAddress(key, t->layout, [&](size_t index, uint32_t address) {
      const ResolvedImage* image = resolved_lookup_(address);
      line += fmt::format(" | {} {:08X}", index, address);
      if (image)
        line += fmt::format(" seq={} base={:08X} {}x{} s{} fmt{} tex={}", image->seq,
                            image->guest_base, image->width, image->height, image->scale,
                            uint32_t(image->format), static_cast<const void*>(image->texture));
      else
        line += " none";
    });
    REXLOG_INFO("PGR4 native cube log: frame {} res={} scaled={} alias={}{}", retire_count_,
                static_cast<const void*>(out->resource),
                static_cast<const void*>(t->scaled_cube.Get()),
                static_cast<const void*>(t->alias), line);
  }
  // Local change (pgr4 project), debug: the cars' surroundings cube, once a frame while asked.
  if (key.dimension == xenos::DataDimension::kCube && (key.base_page << 12) == kCubeDumpAddress &&
      cube_dumped_at_ != retire_count_ && g_cube_dump_frames.load(std::memory_order_relaxed) > 0)
      [[unlikely]] {
    cube_dumped_at_ = retire_count_;
    g_cube_dump_frames.fetch_sub(1, std::memory_order_relaxed);
    DumpCube(*t, out->resource, list, frame_fence);
  }
  // Local change: a scaled cube (CopyScaledCube) is read by direction, with nothing in texels for
  // the shaders to scale back (textures_resolution_scaled made the cars' mirrors read the wrong
  // place of it).
  out->aliased = t->alias != nullptr && t->alias != t->scaled_cube.Get() && t->alias != t->hd.Get();
  out->stale = stale;
  out->srv_signed_format = t->host->snorm;
  out->swizzled_signs = texture_util::SwizzleSigns(fetch);
  D3D12_SHADER_RESOURCE_VIEW_DESC& srv = out->srv;
  srv = {};
  srv.Format = t->host->unorm;
  srv.Shader4ComponentMapping = GuestToHostComponentMapping(fetch.swizzle, t->host->host_swizzle);
  switch (key.dimension) {
    case xenos::DataDimension::k3D:
      srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
      srv.Texture3D.MipLevels = key.mip_levels();
      // Local change: an HD volume with every level of its own (far more than the game's).
      if (t->hd && out->resource == t->hd.Get())
        srv.Texture3D.MipLevels = t->hd->GetDesc().MipLevels;
      break;
    case xenos::DataDimension::kCube:
      srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
      srv.TextureCube.MipLevels = key.mip_levels();
      break;
    default:
      srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
      srv.Texture2DArray.MipLevels = key.mip_levels();
      srv.Texture2DArray.ArraySize = key.array_size();
      // Local change: the HD version's normal map, its second layer (road_hooks.hlsli reads it).
      if (t->hd && out->resource == t->hd.Get() && t->hd_layers == 2)
        srv.Texture2DArray.ArraySize = 2;
      break;
  }
  std::memcpy(memo.fetch, fetch_dwords, sizeof(memo.fetch));
  memo.texture = t;
  memo.binding = *out;
  memo.binding.stale = false;  // a memo hit needs the texture clean
  return true;
}

void TextureCache::RecordCopies(ID3D12GraphicsCommandList* list, const UploadCopies& copies,
                                const std::vector<HostSubresource>& footprints) {
  for (const HostSubresource& s : footprints) {
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = copies.texture;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = s.level + s.slice * copies.mip_levels;
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = copies.source;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = copies.source_offset + s.offset;
    src.PlacedFootprint.Footprint.Format = copies.format;
    src.PlacedFootprint.Footprint.Width = s.block_columns * copies.block_width;
    src.PlacedFootprint.Footprint.Height = s.block_rows * copies.block_height;
    src.PlacedFootprint.Footprint.Depth = s.depth;
    src.PlacedFootprint.Footprint.RowPitch = s.row_pitch;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
}

uint64_t TextureCache::RecordDeferredUpload(const UntileJob& job, UploadList* uploads) {
  const uint64_t t0 = NowNs();
  ID3D12GraphicsCommandList* list = OpenUploadList(uploads, job.frame_fence);
  if (!list) {
    upload_list_failed_.store(true, std::memory_order_relaxed);
    return 0;
  }
  // One transition per call: vkd3d-proton folds transitions between COPY_DEST and shader read
  // into the next copy's barrier and the list's last one (its trivial copy barrier path).
  ID3D12Resource* r = job.copies.texture;
  if (job.state != D3D12_RESOURCE_STATE_COPY_DEST) {
    const D3D12_RESOURCE_BARRIER b =
        TransitionBarrier(r, job.state, D3D12_RESOURCE_STATE_COPY_DEST);
    list->ResourceBarrier(1, &b);
  }
  RecordCopies(list, job.copies, job.footprints);
  const D3D12_RESOURCE_BARRIER b =
      TransitionBarrier(r, D3D12_RESOURCE_STATE_COPY_DEST, kShaderRead);
  list->ResourceBarrier(1, &b);
  return NowNs() - t0;
}

ID3D12GraphicsCommandList* TextureCache::OpenUploadList(UploadList* uploads, uint64_t frame_fence) {
  UploadList& u = *uploads;
  if (u.open) return u.list.Get();
  if (u.allocator && u.allocator_fence != frame_fence)
    u.retired.emplace_back(u.allocator_fence, std::move(u.allocator));
  HRESULT hr = S_OK;
  if (!u.allocator) {
    if (!u.retired.empty() &&
        u.retired.front().first <= completed_fence_.load(std::memory_order_relaxed)) {
      u.allocator = std::move(u.retired.front().second);
      u.retired.pop_front();
      hr = u.allocator->Reset();
    } else {
      hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           IID_PPV_ARGS(u.allocator.GetAddressOf()));
    }
    if (FAILED(hr)) {
      REXLOG_ERROR("PGR4 native textures: upload list allocator failed: {:08X}", uint32_t(hr));
      u.allocator.Reset();
      return nullptr;
    }
    u.allocator_fence = frame_fence;
  }
  if (u.list) {
    hr = u.list->Reset(u.allocator.Get(), nullptr);
  } else {
    hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, u.allocator.Get(), nullptr,
                                    IID_PPV_ARGS(u.list.GetAddressOf()));
    if (SUCCEEDED(hr)) u.list->SetName(L"pgr4 native uploads");
  }
  if (FAILED(hr)) {
    REXLOG_ERROR("PGR4 native textures: upload list reset failed: {:08X}", uint32_t(hr));
    return nullptr;
  }
  u.open = true;
  return u.list.Get();
}

bool TextureCache::CloseUploadLists(std::vector<ID3D12CommandList*>* lists) {
  for (UploadList& u : upload_lists_) {
    if (!u.open) continue;
    u.open = false;
    const HRESULT hr = u.list->Close();
    if (FAILED(hr)) {
      REXLOG_ERROR("PGR4 native textures: upload list close failed: {:08X}", uint32_t(hr));
      return false;
    }
    lists->push_back(u.list.Get());
  }
  return !upload_list_failed_.load(std::memory_order_relaxed);
}

void TextureCache::PreloadArea(uint64_t hash) {
  if (!video_budget_) return;  // no video memory budget known: no preload
  HdFiles& hd = Hd();
  std::lock_guard<std::mutex> lock(hd.mutex);
  // Only a texture of one area tells which one loads: the ones shared by several (common to the
  // cities, or the menus') started all of them at the first menu.
  const auto it = hd.texture_areas.find(hash);
  if (it == hd.texture_areas.end() || it->second.size() != 1) return;
  for (const uint16_t area : it->second) {
    auto [at, first] = preloaded_areas_.emplace(area, retire_count_);
    // Once for an area, until another one comes: tried again while textures were missing, in a tight
    // video memory it freed what it had made and made it again, for the whole race (some 900 textures
    // every 20 s). What is still missing comes when a draw binds it (TakeHd).
    if (area == preload_last_area_) continue;
    preload_last_area_ = area;
    at->second = retire_count_;
    if (!preload_start_ns_) preload_start_retire_ = retire_count_;
    size_t n = 0;
    for (const uint64_t h : hd.area_textures[area])
      if (!hd_cache_.count(h)) {
        preload_list_.push_back(h);
        ++n;
      }
    // The '*' groups' (HdFiles::always) with it, those not in video memory yet.
    size_t common = 0;
    for (const uint64_t h : hd.always)
      if (!hd_cache_.count(h)) {
        preload_list_.push_back(h);
        ++common;
      }
    if (!n && !common) continue;
    if (common)
      REXLOG_INFO("PGR4 native textures: HD preload of the cars' common textures ({})", common);
    if (!preload_start_ns_) preload_start_ns_ = NowNs();
    preload_made_at_area_ = preload_made_;
    REXLOG_INFO("PGR4 native textures: HD preload of {} ({} textures, {} in video memory "
                "already)",
                hd.area_names[area], n, hd.area_textures[area].size() - n);
  }
}

void TextureCache::PumpPreload(ID3D12GraphicsCommandList* list, uint64_t frame_fence) {
  if (preload_list_.empty() || !HdOn()) return;
  HdFiles& hd = Hd();
  // The HD versions take half of the video memory budget at most, and leave room on the card:
  // over that, the ones no texture holds go, least recently used first (an area driven before);
  // with none to free, the preload waits.
  const uint64_t cap = video_budget_ / 2;
  if (hd_cache_bytes_ >= cap || VideoMemoryTight(85)) {
    FreeHd(hd_cache_bytes_ >= cap ? hd_cache_bytes_ - cap + (64ull << 20) : 64ull << 20,
           preload_start_retire_);
    if (hd_cache_bytes_ >= cap || VideoMemoryTight(85)) return;
  }
  // Half of the frame's HD upload: the textures on screen come first (TakeHd).
  const uint64_t frame_bytes = (g_no_scene.load(std::memory_order_relaxed) ? 384ull : 48ull) << 20;
  for (size_t i = 0; i < preload_asked_.size();) {
    const uint64_t hash = preload_asked_[i];
    if (hd_cache_.count(hash)) {  // a texture took it first
      preload_asked_[i] = preload_asked_.back();
      preload_asked_.pop_back();
      continue;
    }
    if (hd_frame_bytes_.load(std::memory_order_relaxed) >= frame_bytes / 2) return;
    const auto file = HdTake(hd, hash, true);
    if (!file) {
      ++i;
      continue;
    }
    hd_frame_bytes_.fetch_add(file->file.size() + file->normal.size(), std::memory_order_relaxed);
    if (MakeHd(hash, file->file, file->normal, DXGI_FORMAT_UNKNOWN, list, frame_fence, nullptr))
      ++preload_made_;
    preload_asked_[i] = preload_asked_.back();
    preload_asked_.pop_back();
  }
  // 8 files asked ahead, at the lowest priority: their reading keeps up with the making.
  while (preload_asked_.size() < 8 && preload_next_ < preload_list_.size()) {
    const uint64_t hash = preload_list_[preload_next_++];
    if (hd_cache_.count(hash)) continue;
    HdAsk(hd, hash);
    preload_asked_.push_back(hash);
  }
  if (preload_asked_.empty() && preload_next_ >= preload_list_.size()) {
    REXLOG_INFO("PGR4 native textures: HD preload done: {} made in {:.1f} s, {} HD versions in "
                "video memory ({} MB; budget {} MB, {} MB used)",
                preload_made_ - preload_made_at_area_, double(NowNs() - preload_start_ns_) / 1e9,
                hd_cache_.size(), hd_cache_bytes_ >> 20, video_budget_ >> 20,
                video_usage_ >> 20);
    preload_list_.clear();
    preload_next_ = 0;
    preload_start_ns_ = 0;
  }
}

void TextureCache::SetNoScene(bool no_scene) {
  g_no_scene.store(no_scene, std::memory_order_relaxed);
}

void TextureCache::PollVideoMemory() {
  if (!adapter_) return;
  DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
  if (FAILED(adapter_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) return;
  video_budget_ = info.Budget;
  video_usage_ = info.CurrentUsage;
}

uint64_t TextureCache::FreeHd(uint64_t bytes, uint64_t before) {
  // The HD versions no texture holds (the cache's reference the only one), unused for 3 Retires
  // or more (no frame in flight binds them), least recently used first, until `bytes` are freed.
  std::vector<std::pair<uint64_t, uint64_t>> free;  // (used, hash)
  HdFiles& hd = Hd();
  std::unique_lock<std::mutex> lock(hd.mutex);  // HdFiles::always
  for (const auto& [hash, c] : hd_cache_) {
    if (hd.always_set.count(hash)) continue;  // the '*' groups': kept
    c.resource->AddRef();
    if (c.resource->Release() == 1 && retire_count_ - c.used > 3 && c.used < before)
      free.push_back({c.used, hash});
  }
  lock.unlock();
  std::sort(free.begin(), free.end());
  uint64_t freed = 0;
  uint32_t n = 0;
  std::vector<ComPtr<ID3D12Resource>> released;
  for (const auto& [used, hash] : free) {
    if (freed >= bytes) break;
    auto it = hd_cache_.find(hash);
    freed += it->second.bytes;
    hd_cache_bytes_ -= it->second.bytes;
    // Local change (pgr4 project): its views go with it: a new resource at its address would find them.
    evict_resources_.clear();
    evict_resources_.push_back(it->second.resource.Get());
    if (evict_callback_) evict_callback_(evict_resources_);
    released.push_back(std::move(it->second.resource));
    hd_cache_.erase(it);
    ++n;
  }
  if (!released.empty()) {
    {
      std::lock_guard<std::mutex> release_lock(release_mutex_);
      for (auto& r : released) release_queue_.push_back(std::move(r));
    }
    release_cv_.notify_one();
    if (!release_thread_.joinable()) release_thread_ = std::thread([this] { ReleaseMain(); });
  }
  if (n)
    PGR4_LOG_FIRST(16, INFO,
                   "PGR4 native textures: {} unused HD versions freed ({} MB), {} kept ({} MB); "
                   "video memory {} of {} MB",
                   n, freed >> 20, hd_cache_.size(), hd_cache_bytes_ >> 20, video_usage_ >> 20,
                   video_budget_ >> 20);
  return freed;
}

void TextureCache::TrimHdCache() {
  // Over 90 % of the budget: down to 85 %.
  if (VideoMemoryTight(90))
    FreeHd(video_usage_ - video_budget_ / 100 * 85);
  else if (MemoryPressure())
    FreeHd(256ull << 20);  // each version kept holds its views
}

void TextureCache::Retire(uint64_t completed_fence) {
  completed_fence_.store(completed_fence, std::memory_order_relaxed);
  upload_.EndFrame(completed_fence);
  WriteCubeDumps(completed_fence);
  WriteSkyProbes(completed_fence);
  now_ms_ = NowNs() / 1000000;
  ++retire_count_;
  RecheckContents();
  if (++video_poll_ >= 30) {
    video_poll_ = 0;
    PollVideoMemory();
    TrimHdCache();
  }
  EvictOverBudget(completed_fence);
  hd_frame_bytes_.store(0, std::memory_order_relaxed);
  // Local change: pgr4_native_texture_hd switched: every texture with an HD version follows at
  // once (the draws keep their bindings while the texture's generation stays, so Get would not).
  // Local change: pgr4_native_texture_hd_reload: every texture with an HD version takes it
  // again from the files (TakeHd at its next Get), the old one kept until the texture is freed.
  if (REXCVAR_GET(pgr4_native_texture_hd_reload)) {
    REXCVAR_SET(pgr4_native_texture_hd_reload, false);
    HdForget();
    hd_cache_.clear();  // the files may have changed
    hd_cache_bytes_ = 0;
    uint32_t n = 0;
    // The textures given an HD version since they were uploaded too (a new file in textures_hd).
    const HdFiles& files = Hd();
    for (auto& [key, t] : textures_) {
      if (!t->hd_hash && t->content_hash && files.files.count(t->content_hash))
        t->hd_hash = t->content_hash;
      if (!t->hd_hash) continue;
      if (t->alias && t->alias == t->hd.Get()) SetAlias(*t, nullptr);
      if (t->hd) t->other_hd.push_back(std::move(t->hd));
      t->hd_current = false;
      t->hd_layers = 1;
      t->hd_pending = t->hd_hash;
      ++n;
    }
    REXLOG_INFO("PGR4 native textures: HD reloaded ({} textures)", n);
  }
  const bool hd = HdOn();
  if (hd != hd_on_) {
    hd_on_ = hd;
    for (auto& [key, t] : textures_) {
      if (!hd && BoundToHd(*t)) SetAlias(*t, nullptr);  // whichever HD version, current or not
      else if (hd && t->hd_current && !t->alias && HdOnFor(t->hd_hash)) SetAlias(*t, t->hd.Get());
    }
  }
}

}  // namespace pgr4::native
