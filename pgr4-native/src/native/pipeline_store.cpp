// pgr4-native PGR4 native renderer: pipelines that survive runs (see pipeline_store.h).
#include "pipeline_store.h"

#include <rex/logging.h>
#include <rex/types.h>

#include <xxhash.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cwchar>
#include <system_error>

namespace pgr4::native {

namespace {

constexpr char kMagic[4] = {'P', '4', 'P', 'S'};
constexpr uint32_t kFormat = 2;
constexpr uint32_t kShaderTag = 'S', kPipelineTag = 'P', kTouchTag = 'T';

struct Header {
  char magic[4];
  uint32_t format, key_size, key_version, session;
};

std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
  std::vector<uint8_t> data;
  if (std::FILE* f = _wfopen(path.c_str(), L"rb")) {
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size > 0) {
      data.resize(size_t(size));
      if (std::fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
    }
    std::fclose(f);
  }
  return data;
}

// Local change (pgr4 project): another store's records (another draw resolution scale's
// directory), its touches folded in, for Open's import. False if the file is not of this format,
// key size and version.
struct Sibling {
  uint32_t session = 0;
  std::unordered_map<uint64_t, PipelineStore::Shader> shaders;
  std::vector<PipelineStore::Pipeline> pipelines;
  std::vector<uint64_t> hashes;
};
bool ReadSibling(const std::filesystem::path& path, uint32_t key_size, uint32_t key_version,
                 Sibling* out) {
  const std::vector<uint8_t> data = ReadFile(path);
  size_t pos = 0;
  auto take = [&](void* o, size_t n) {
    if (data.size() - pos < n) return false;
    std::memcpy(o, data.data() + pos, n);
    pos += n;
    return true;
  };
  Header h;
  if (!take(&h, sizeof(h)) || std::memcmp(h.magic, kMagic, 4) || h.format != kFormat ||
      h.key_size != key_size || h.key_version != key_version)
    return false;
  out->session = h.session;
  std::unordered_map<uint64_t, uint32_t> touches;
  for (;;) {
    uint32_t tag;
    if (!take(&tag, 4)) break;
    if (tag == kShaderTag) {
      uint32_t type, dwords;
      uint64_t hash;
      if (!take(&type, 4) || !take(&hash, 8) || !take(&dwords, 4) ||
          data.size() - pos < size_t(dwords) * 4)
        break;
      PipelineStore::Shader s;
      s.type = type;
      s.ucode.resize(dwords);
      take(s.ucode.data(), size_t(dwords) * 4);
      out->shaders.emplace(hash, std::move(s));
    } else if (tag == kPipelineTag) {
      if (data.size() - pos < 36 + key_size) break;
      const uint64_t record_hash = XXH3_64bits(data.data() + pos, 32 + key_size);
      PipelineStore::Pipeline p;
      p.key.resize(key_size);
      take(&p.vs_hash, 8);
      take(&p.vs_mod, 8);
      take(&p.ps_hash, 8);
      take(&p.ps_mod, 8);
      take(p.key.data(), key_size);
      take(&p.last_session, 4);
      out->hashes.push_back(record_hash);
      out->pipelines.push_back(std::move(p));
    } else if (tag == kTouchTag) {
      uint64_t record_hash;
      uint32_t session;
      if (!take(&record_hash, 8) || !take(&session, 4)) break;
      uint32_t& t = touches[record_hash];
      t = std::max(t, session);
    } else {
      break;
    }
  }
  for (size_t i = 0; i < out->pipelines.size(); ++i)
    if (auto t = touches.find(out->hashes[i]); t != touches.end())
      out->pipelines[i].last_session = std::max(out->pipelines[i].last_session, t->second);
  return true;
}

}  // namespace

PipelineStore::~PipelineStore() {
  if (file_) std::fclose(file_);
}

uint64_t PipelineStore::RecordHash(uint64_t vs_hash, uint64_t vs_mod, uint64_t ps_hash,
                                   uint64_t ps_mod, const void* key, uint32_t key_size) {
  uint8_t record[32 + kMaxKeySize];
  std::memcpy(record, &vs_hash, 8);
  std::memcpy(record + 8, &vs_mod, 8);
  std::memcpy(record + 16, &ps_hash, 8);
  std::memcpy(record + 24, &ps_mod, 8);
  std::memcpy(record + 32, key, key_size);
  return XXH3_64bits(record, 32 + key_size);
}

void PipelineStore::Open(const std::filesystem::path& dir, uint32_t key_size,
                         uint32_t key_version, const std::filesystem::path& base_list) {
  if (key_size > kMaxKeySize) {
    REXLOG_WARN("PGR4 native pipelines: key of {} bytes is over {}, no pipeline store", key_size,
                kMaxKeySize);
    return;
  }
  key_size_ = key_size;
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const std::filesystem::path path = dir / "pipelines.bin";
  const std::vector<uint8_t> data = ReadFile(path);
  size_t pos = 0;
  auto take = [&](void* out, size_t n) {
    if (data.size() - pos < n) return false;
    std::memcpy(out, data.data() + pos, n);
    pos += n;
    return true;
  };
  Header h;
  session_ = 1;
  size_t good = 0;  // the end of the last whole record read
  // Touch record hash -> newest session it names.
  std::unordered_map<uint64_t, uint32_t> touches;
  if (take(&h, sizeof(h)) && !std::memcmp(h.magic, kMagic, 4) && h.format == kFormat &&
      h.key_size == key_size && h.key_version == key_version) {
    session_ = h.session + 1;
    good = pos;
    for (;;) {
      uint32_t tag;
      if (!take(&tag, 4)) break;
      if (tag == kShaderTag) {
        uint32_t type, dwords;
        uint64_t hash;
        Shader s;
        if (!take(&type, 4) || !take(&hash, 8) || !take(&dwords, 4) ||
            data.size() - pos < size_t(dwords) * 4)
          break;
        s.type = type;
        s.ucode.resize(dwords);
        take(s.ucode.data(), size_t(dwords) * 4);
        loaded_shaders_.emplace(hash, std::move(s));
      } else if (tag == kPipelineTag) {
        Pipeline p;
        uint64_t record_hash;
        p.key.resize(key_size);
        if (data.size() - pos < 36 + key_size) break;
        record_hash = XXH3_64bits(data.data() + pos, 32 + key_size);
        if (!take(&p.vs_hash, 8) || !take(&p.vs_mod, 8) || !take(&p.ps_hash, 8) ||
            !take(&p.ps_mod, 8) || !take(p.key.data(), key_size) || !take(&p.last_session, 4))
          break;
        record_hashes_.push_back(record_hash);
        loaded_pipelines_.push_back(std::move(p));
      } else if (tag == kTouchTag) {
        uint64_t record_hash;
        uint32_t session;
        if (!take(&record_hash, 8) || !take(&session, 4)) break;
        auto it = touches.find(record_hash);
        if (it == touches.end()) {
          touches.emplace(record_hash, session);
        } else {
          it->second = std::max(it->second, session);
        }
      } else {
        break;
      }
      good = pos;
    }
  }
  // Local change (pgr4 project): the recent records of the other draw resolution scales' stores
  // (sibling pgr4-native* directories), which this scale has not created yet: the translation
  // differs by scale, the guest shaders and pipeline keys do not. They count as used by the
  // last session, so this run prewarms them and a scale switch does not start over.
  size_t imported = 0;
  {
    std::unordered_set<uint64_t> have(record_hashes_.begin(), record_hashes_.end());
    std::error_code dec;
    // Local change (31/10/2026): the game's base list too (base_list, shipped beside the
    // executable: every city recorded), so a new install precompiles them all.
    std::vector<std::filesystem::path> files;
    if (!base_list.empty()) files.push_back(base_list);
    for (const auto& entry : std::filesystem::directory_iterator(dir.parent_path(), dec))
      if (entry.is_directory() && entry.path() != dir &&
          entry.path().filename().wstring().starts_with(L"pgr4-native"))
        files.push_back(entry.path() / "pipelines.bin");
    for (const std::filesystem::path& file : files) {
      Sibling sib;
      if (!ReadSibling(file, key_size, key_version, &sib)) continue;
      for (size_t i = 0; i < sib.pipelines.size(); ++i) {
        PipelineStore::Pipeline& p = sib.pipelines[i];
        if (uint64_t(sib.session) > uint64_t(p.last_session) + kHotSessions ||
            !have.insert(sib.hashes[i]).second)
          continue;
        auto vs = sib.shaders.find(p.vs_hash);
        auto ps = p.ps_hash ? sib.shaders.find(p.ps_hash) : sib.shaders.end();
        if (vs == sib.shaders.end() || (p.ps_hash && ps == sib.shaders.end())) continue;
        loaded_shaders_.try_emplace(p.vs_hash, vs->second);
        if (p.ps_hash) loaded_shaders_.try_emplace(p.ps_hash, ps->second);
        p.last_session = session_ > 1 ? session_ - 1 : session_;
        record_hashes_.push_back(sib.hashes[i]);
        loaded_pipelines_.push_back(std::move(p));
        ++imported;
      }
    }
  }
  if (imported)
    REXLOG_INFO("PGR4 native pipelines: {} pipelines imported from the other scales' stores",
                imported);
  // The touches fold into the records, and what no recent session used leaves the store: the
  // pipelines of content the player has not played in kKeepSessions never cost a start again.
  size_t evicted = 0;
  std::vector<Pipeline> kept;
  std::vector<uint64_t> kept_hashes;
  kept.reserve(loaded_pipelines_.size());
  kept_hashes.reserve(loaded_pipelines_.size());
  for (size_t i = 0; i < loaded_pipelines_.size(); ++i) {
    Pipeline& p = loaded_pipelines_[i];
    auto t = touches.find(record_hashes_[i]);
    if (t != touches.end()) p.last_session = std::max(p.last_session, t->second);
    if (uint64_t(session_) > uint64_t(p.last_session) + kKeepSessions) {
      ++evicted;
      continue;
    }
    kept_hashes.push_back(record_hashes_[i]);
    kept.push_back(std::move(p));
  }
  // Most recently used first: prewarm replays the player's current rotation, not their history.
  std::stable_sort(kept.begin(), kept.end(), [](const Pipeline& a, const Pipeline& b) {
    return a.last_session > b.last_session;
  });
  std::unordered_set<uint64_t> referenced;
  for (const Pipeline& p : kept) {
    referenced.insert(p.vs_hash);
    if (p.ps_hash) referenced.insert(p.ps_hash);
  }
  for (auto it = loaded_shaders_.begin(); it != loaded_shaders_.end();) {
    if (!referenced.count(it->first)) {
      it = loaded_shaders_.erase(it);
    } else {
      ++it;
    }
  }
  loaded_pipelines_ = std::move(kept);
  record_hashes_ = std::move(kept_hashes);
  for (size_t i = 0; i < loaded_pipelines_.size(); ++i)
    entry_by_hash_.emplace(record_hashes_[i], uint32_t(i));
  pipelines_written_.insert(record_hashes_.begin(), record_hashes_.end());
  for (const auto& s : loaded_shaders_) shaders_written_.insert(s.first);
  hot_count_ = 0;
  for (const Pipeline& p : loaded_pipelines_) {
    if (uint64_t(session_) >= uint64_t(p.last_session) + kHotSessions) break;
    ++hot_count_;
  }
  hot_count_ = std::min(hot_count_, kPrewarmMax);

  // The file, again: the counted-up session and only the kept records, atomically, so a kill
  // mid-write loses nothing.
  std::filesystem::path tmp = path;
  tmp += L".tmp";
  bool wrote = false;
  if (std::FILE* f = _wfopen(tmp.c_str(), L"wb")) {
    Header nh;
    std::memcpy(nh.magic, kMagic, 4);
    nh.format = kFormat;
    nh.key_size = key_size;
    nh.key_version = key_version;
    nh.session = session_;
    wrote = std::fwrite(&nh, sizeof(nh), 1, f) == 1;
    for (const auto& [hash, s] : loaded_shaders_) {
      const uint32_t type = s.type, dwords = uint32_t(s.ucode.size());
      wrote = wrote && std::fwrite(&kShaderTag, 4, 1, f) == 1 && std::fwrite(&type, 4, 1, f) == 1 &&
              std::fwrite(&hash, 8, 1, f) == 1 && std::fwrite(&dwords, 4, 1, f) == 1 &&
              std::fwrite(s.ucode.data(), 4, dwords, f) == dwords;
    }
    for (const Pipeline& p : loaded_pipelines_) {
      wrote = wrote && std::fwrite(&kPipelineTag, 4, 1, f) == 1 &&
              std::fwrite(&p.vs_hash, 8, 1, f) == 1 && std::fwrite(&p.vs_mod, 8, 1, f) == 1 &&
              std::fwrite(&p.ps_hash, 8, 1, f) == 1 && std::fwrite(&p.ps_mod, 8, 1, f) == 1 &&
              std::fwrite(p.key.data(), 1, key_size, f) == key_size &&
              std::fwrite(&p.last_session, 4, 1, f) == 1;
    }
    wrote = wrote && std::fflush(f) == 0;
    std::fclose(f);
    if (wrote) {
      std::filesystem::rename(tmp, path, ec);
      if (ec) wrote = false;
    }
  }
  if (!wrote) {
    // No rewrite: cut the file at the last whole record so the next start parses every append
    // (a torn tail would hide whatever lands behind it), and start it over if nothing parsed.
    // The records read stay for this run's prewarm and are written again when next created.
    std::error_code rec;
    std::filesystem::remove(tmp, rec);
    REXLOG_WARN("PGR4 native pipelines: cannot rewrite {} ({}), appending to it as it is",
                path.string(), ec.message());
    if (good && good != data.size()) std::filesystem::resize_file(path, good, ec);
    if (good) {
      file_ = _wfopen(path.c_str(), L"ab");
    } else {
      file_ = _wfopen(path.c_str(), L"wb");
      // The records the file held are gone from it: let them be written again when next created.
      shaders_written_.clear();
      pipelines_written_.clear();
      if (file_) {
        Header nh;
        std::memcpy(nh.magic, kMagic, 4);
        nh.format = kFormat;
        nh.key_size = key_size;
        nh.key_version = key_version;
        nh.session = session_;
        std::fwrite(&nh, sizeof(nh), 1, file_);
        dirty_ = true;
      }
    }
  } else {
    file_ = _wfopen(path.c_str(), L"ab");
  }
  if (!file_) {
    REXLOG_WARN("PGR4 native pipelines: cannot open {} for writing", path.string());
    return;
  }
  REXLOG_INFO(
      "PGR4 native pipelines: {} pipelines, {} shaders from earlier runs ({} to prewarm, {} "
      "dropped) ({})",
      loaded_pipelines_.size(), loaded_shaders_.size(), hot_count_, evicted, path.string());
}

void PipelineStore::RecordShader(uint32_t type, uint64_t hash, const uint32_t* host_ucode,
                                 size_t dwords) {
  if (!file_ || !shaders_written_.insert(hash).second) return;
  std::vector<uint32_t> guest(dwords);
  for (size_t i = 0; i < dwords; ++i) guest[i] = rex::byte_swap(host_ucode[i]);
  const uint32_t n = uint32_t(dwords);
  std::fwrite(&kShaderTag, 4, 1, file_);
  std::fwrite(&type, 4, 1, file_);
  std::fwrite(&hash, 8, 1, file_);
  std::fwrite(&n, 4, 1, file_);
  std::fwrite(guest.data(), 4, dwords, file_);
  dirty_ = true;
}

void PipelineStore::RecordPipeline(uint64_t vs_hash, uint64_t vs_mod, uint64_t ps_hash,
                                   uint64_t ps_mod, const void* key) {
  if (!file_) return;
  if (!pipelines_written_.insert(RecordHash(vs_hash, vs_mod, ps_hash, ps_mod, key, key_size_))
           .second)
    return;
  std::fwrite(&kPipelineTag, 4, 1, file_);
  std::fwrite(&vs_hash, 8, 1, file_);
  std::fwrite(&vs_mod, 8, 1, file_);
  std::fwrite(&ps_hash, 8, 1, file_);
  std::fwrite(&ps_mod, 8, 1, file_);
  std::fwrite(key, 1, key_size_, file_);
  std::fwrite(&session_, 4, 1, file_);
  dirty_ = true;
}

void PipelineStore::TouchRecord(uint64_t vs_hash, uint64_t vs_mod, uint64_t ps_hash,
                                uint64_t ps_mod, const void* key) {
  if (!file_) return;
  auto it = entry_by_hash_.find(RecordHash(vs_hash, vs_mod, ps_hash, ps_mod, key, key_size_));
  if (it == entry_by_hash_.end()) return;
  TouchEntry(it->second);
}

void PipelineStore::TouchEntry(uint32_t index) {
  if (!file_ || index >= loaded_pipelines_.size()) return;
  Pipeline& p = loaded_pipelines_[index];
  if (p.last_session >= session_) return;
  p.last_session = session_;
  AppendTouch(record_hashes_[index]);
}

void PipelineStore::AppendTouch(uint64_t record_hash) {
  std::fwrite(&kTouchTag, 4, 1, file_);
  std::fwrite(&record_hash, 8, 1, file_);
  std::fwrite(&session_, 4, 1, file_);
  dirty_ = true;
}

void PipelineStore::Flush() {
  if (!dirty_) return;
  dirty_ = false;
  std::fflush(file_);
}

void PipelineLibrary::Open(ID3D12Device* device, const std::filesystem::path& path,
                           uint64_t root_signature_hash) {
  device_ = device;
  path_ = path;
  root_signature_hash_ = root_signature_hash;
  if (path.empty()) return;  // cache off: Create compiles every pipeline
  Microsoft::WRL::ComPtr<ID3D12Device1> device1;
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device1)))) {
    REXLOG_WARN("PGR4 native pipelines: no ID3D12Device1, no pipeline library");
    return;
  }
  // One library from its file, or a new one (blob emptied then).
  auto open = [&](const std::filesystem::path& file, std::vector<uint8_t>& blob,
                  Microsoft::WRL::ComPtr<ID3D12PipelineLibrary>& lib) {
    blob = ReadFile(file);
    HRESULT hr = E_FAIL;
    if (!blob.empty()) {
      hr = device1->CreatePipelineLibrary(blob.data(), blob.size(), IID_PPV_ARGS(&lib));
      if (FAILED(hr))
        REXLOG_INFO("PGR4 native pipelines: library {} not usable ({:08X}), starting over",
                    file.string(), uint32_t(hr));
    }
    if (FAILED(hr)) {
      blob.clear();
      lib.Reset();
      hr = device1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&lib));
      if (FAILED(hr)) {
        REXLOG_WARN("PGR4 native pipelines: CreatePipelineLibrary failed {:08X}", uint32_t(hr));
        lib.Reset();
      }
    }
  };
  open(path, blob_, library_);
  if (!library_) return;
  REXLOG_INFO("PGR4 native pipelines: library {} ({} KB)", path.string(), blob_.size() / 1024);
  if (delta_on_) {
    delta_path_ = path.parent_path() / (path.stem().wstring() + L".delta" + path.extension().wstring());
    open(delta_path_, delta_blob_, delta_);
    REXLOG_INFO("PGR4 native pipelines: the small library {} ({} KB){}", delta_path_.string(),
                delta_blob_.size() / 1024,
                fold_ ? ": its pipelines found stored in the big one (the precompile)" : "");
  }
}

HRESULT PipelineLibrary::Create(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
                                ID3D12PipelineState** out, bool* loaded) {
  *loaded = false;
  if (!library_) return device_->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(out));
  // Name: the root signature, the description without its pointers, then every shader's bytecode.
  D3D12_GRAPHICS_PIPELINE_STATE_DESC plain;
  std::memcpy(&plain, &desc, sizeof(plain));  // with the padding the caller zeroed
  plain.pRootSignature = nullptr;
  for (D3D12_SHADER_BYTECODE* b : {&plain.VS, &plain.PS, &plain.DS, &plain.HS, &plain.GS})
    b->pShaderBytecode = nullptr;
  plain.StreamOutput = {};
  plain.InputLayout = {};
  plain.CachedPSO = {};
  uint64_t h = XXH3_64bits_withSeed(&plain, sizeof(plain), root_signature_hash_);
  for (const D3D12_SHADER_BYTECODE* b : {&desc.VS, &desc.PS, &desc.DS, &desc.HS, &desc.GS})
    if (b->BytecodeLength) h = XXH3_64bits_withSeed(b->pShaderBytecode, b->BytecodeLength, h);
  {
    std::lock_guard<std::mutex> lock(busy_mutex_);
    if (!busy_.insert(h).second)  // the same pipeline on another worker: bypass the library
      return device_->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(out));
  }
  wchar_t name[20];
  std::swprintf(name, 20, L"%016llX", static_cast<unsigned long long>(h));
  HRESULT hr = library_->LoadGraphicsPipeline(name, &desc, IID_PPV_ARGS(out));
  if (SUCCEEDED(hr)) {
    *loaded = true;
    loaded_.fetch_add(1, std::memory_order_relaxed);
  } else if (delta_ && SUCCEEDED(hr = delta_->LoadGraphicsPipeline(name, &desc, IID_PPV_ARGS(out)))) {
    // Local change (22/11/2026): from the small library; the precompile stores it in the big one.
    *loaded = true;
    delta_loaded_.fetch_add(1, std::memory_order_relaxed);
    if (fold_ && SUCCEEDED(library_->StorePipeline(name, *out))) {
      folded_.fetch_add(1, std::memory_order_relaxed);
      dirty_.store(true, std::memory_order_relaxed);
    }
  } else {
    hr = device_->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(out));
    // Stored in the small library while playing (local change), in the big one by the precompile.
    ID3D12PipelineLibrary* target = delta_ && !fold_ ? delta_.Get() : library_.Get();
    if (SUCCEEDED(hr) && SUCCEEDED(target->StorePipeline(name, *out))) {
      stored_.fetch_add(1, std::memory_order_relaxed);
      (target == library_.Get() ? dirty_ : delta_dirty_).store(true, std::memory_order_relaxed);
    }
  }
  std::lock_guard<std::mutex> lock(busy_mutex_);
  busy_.erase(h);
  return hr;
}

bool PipelineLibrary::Write(ID3D12PipelineLibrary* lib, const std::filesystem::path& path,
                            const char* what) {
  const auto t0 = std::chrono::steady_clock::now();
  std::vector<uint8_t> data(lib->GetSerializedSize());
  const HRESULT hr = lib->Serialize(data.data(), data.size());
  const auto t1 = std::chrono::steady_clock::now();
  if (FAILED(hr)) {
    // A pipeline stored since GetSerializedSize (another worker) makes the buffer too small.
    REXLOG_WARN("PGR4 native pipelines: {} Serialize failed {:08X}", what, uint32_t(hr));
    return false;
  }
  std::filesystem::path tmp = path;
  tmp += L".tmp";
  std::FILE* f = _wfopen(tmp.c_str(), L"wb");
  if (!f) return false;
  const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
  std::fclose(f);
  std::error_code ec;
  if (ok) std::filesystem::rename(tmp, path, ec);
  const auto t2 = std::chrono::steady_clock::now();
  REXLOG_INFO("PGR4 native pipelines: {} saved, {} KB ({} loaded, {} from the small library, {} "
              "moved into the big one, {} stored this run), serialized in {:.0f} ms, written in "
              "{:.0f} ms{}",
              what, data.size() / 1024, loaded_.load(), delta_loaded_.load(), folded_.load(),
              stored_.load(), std::chrono::duration<double, std::milli>(t1 - t0).count(),
              std::chrono::duration<double, std::milli>(t2 - t1).count(), ok && !ec ? "" : " FAILED");
  return ok && !ec;
}

void PipelineLibrary::Save() {
  if (!library_) return;
  const bool big_dirty = dirty_.exchange(false, std::memory_order_relaxed);
  const bool small_dirty = delta_dirty_.exchange(false, std::memory_order_relaxed);
  if (!big_dirty && !small_dirty && !(fold_ && delta_ && !delta_removed_)) return;
  // Development (22/11/2026): timed for the race probe (draw.cpp).
  const auto t0 = std::chrono::steady_clock::now();
  saving_.store(true, std::memory_order_relaxed);
  if (big_dirty && !Write(library_.Get(), path_, "library")) dirty_.store(true, std::memory_order_relaxed);
  if (small_dirty && delta_ && !fold_ && !Write(delta_.Get(), delta_path_, "small library"))
    delta_dirty_.store(true, std::memory_order_relaxed);
  // The precompile: the small library's pipelines it asked for are in the big one now, saved; the
  // small one removed (what it held and nothing asked for compiles again when drawn, into a new one).
  if (fold_ && delta_ && !delta_removed_ && !dirty_.load(std::memory_order_relaxed) &&
      !delta_path_.empty()) {
    std::error_code ec;
    if (std::filesystem::remove(delta_path_, ec))
      REXLOG_INFO("PGR4 native pipelines: the small library removed, {} of its pipelines moved into "
                  "the big one", folded_.load());
    // Once (its blob kept: the library object made from it reads it until the exit).
    delta_removed_ = true;
  }
  save_us_.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::steady_clock::now() - t0).count()),
                     std::memory_order_relaxed);
  saves_.fetch_add(1, std::memory_order_relaxed);
  saving_.store(false, std::memory_order_relaxed);
}

}  // namespace pgr4::native
