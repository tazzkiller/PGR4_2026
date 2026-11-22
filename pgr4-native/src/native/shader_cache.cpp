#include "shader_cache.h"

#include "hang_probe.h"  // local change: the freeze probe

#include "d3d12_device.h"    // GpuVendorId
#include "render_targets.h"  // DrawResolutionScale

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>

#include <xxhash.h>

#include <rex/cvar.h>
#include <rex/logging.h>

// Local change (22/11/2026): a new shader's analysis outside the cache's lock.
REXCVAR_DEFINE_BOOL(pgr4_native_shader_analyze_unlocked, true, "PGR4 native",
                    "A new shader's ucode analyzed outside the shader cache's lock, several at once "
                    "(off: one at a time, under the lock)");
REXCVAR_DEFINE_STRING(pgr4_native_shader_dump_dir, "", "PGR4 native",
                      "Write every translation (DXBC) to <dir>/<ucode hash>_<modification>.<vs|ps>"
                      ".dxbc as it is made, and every shader's ucode disassembly to "
                      "<dir>/<ucode hash>.<vs|ps>.txt (empty = off)");

namespace pgr4::native {

namespace {
double Us(std::chrono::steady_clock::time_point since) {
  return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - since)
      .count();
}

const char* Stage(const DxbcShader& shader) {
  return shader.type() == rex::graphics::xenos::ShaderType::kVertex ? "vs" : "ps";
}

void DumpFile(const char* name, const void* data, size_t size) {
  const std::filesystem::path dir(REXCVAR_GET(pgr4_native_shader_dump_dir));
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (std::FILE* out = _wfopen((dir / name).c_str(), L"wb")) {
    std::fwrite(data, 1, size, out);
    std::fclose(out);
  }
}
}  // namespace

// The Xenos plugin's settings on the RTX 3090 (bindless on, host render targets, gamma as
// unorm16, 2x MSAA supported, no resolution scale), so a translation here is byte-identical to
// the plugin's for the same modification. The vendor only changes code for Intel (no switch
// statements). Local change (pgr4 project): the device's own vendor (GpuVendorId), not NVIDIA's
// whatever the card.
// Local change (pgr4 project): built with the draw resolution scale, so pixel positions, and the
// unnormalized fetches, sizes and offsets of textures in textures_resolution_scaled, are in
// guest units (draw.cpp sets the mask).
ShaderCache::ShaderCache()
    : translator_(rex::ui::GraphicsProvider::GpuVendorID(GpuVendorId()), /*bindless=*/true,
                  /*edram_rov_used=*/false, /*gamma_render_target_as_unorm8=*/false,
                  /*msaa_2x_supported=*/true, DrawResolutionScale(), DrawResolutionScale()) {}

ShaderCache::~ShaderCache() = default;

DxbcShader* ShaderCache::Load(rex::graphics::xenos::ShaderType type, const uint32_t* guest_ucode,
                              uint32_t dword_count) {
  uint64_t hash = XXH3_64bits(guest_ucode, size_t(dword_count) * sizeof(uint32_t));
  std::unique_lock<std::mutex> lock(load_mutex_);
  // Local change (22/11/2026, pgr4_native_shader_analyze_unlocked): AnalyzeUcode touches only the
  // shader and the text buffer given: outside the lock, a buffer of its own, one ucode by one
  // thread at a time (another Load of it waits).
  {
    HangMark hang_mark(HangWhat::kLoadWait, hash);  // local change: the freeze probe
    load_cv_.wait(lock, [&] { return !loading_.count(hash); });
  }
  auto it = shaders_.find(hash);
  if (it != shaders_.end()) return it->second.get();
  auto t = std::chrono::steady_clock::now();
  auto shader = std::make_unique<DxbcShader>(type, hash, guest_ucode, dword_count);
  if (REXCVAR_GET(pgr4_native_shader_analyze_unlocked)) {
    loading_.insert(hash);
    lock.unlock();
    rex::string::StringBuffer buffer;
    HangMark hang_mark(HangWhat::kAnalyzing, hash);
    shader->AnalyzeUcode(buffer);
    lock.lock();
    loading_.erase(hash);
    load_cv_.notify_all();
  } else {
    shader->AnalyzeUcode(disasm_buffer_);
  }
  stats_.analyze_us += Us(t);
  if (!REXCVAR_GET(pgr4_native_shader_dump_dir).empty()) {
    char name[40];
    std::snprintf(name, sizeof(name), "%016llX.%s.txt", static_cast<unsigned long long>(hash),
                  Stage(*shader));
    const std::string& text = shader->ucode_disassembly();
    DumpFile(name, text.data(), text.size());
    // Local change (pgr4 project): the ucode itself too, as the guest has it (big-endian), to
    // match it against the game's files (tools/xpu_extract.py).
    std::snprintf(name, sizeof(name), "%016llX.%s.ucode", static_cast<unsigned long long>(hash),
                  Stage(*shader));
    DumpFile(name, guest_ucode, size_t(dword_count) * sizeof(uint32_t));
  }
  ++stats_.shaders;
  return shaders_.emplace(hash, std::move(shader)).first->second.get();
}

const DxbcShader* ShaderCache::Find(uint64_t hash) {
  std::lock_guard<std::mutex> lock(load_mutex_);
  auto it = shaders_.find(hash);
  return it != shaders_.end() ? it->second.get() : nullptr;
}

std::unique_ptr<DxbcShaderTranslator> ShaderCache::MakeTranslator() const {
  return std::make_unique<DxbcShaderTranslator>(
      rex::ui::GraphicsProvider::GpuVendorID(GpuVendorId()), /*bindless=*/true,
      /*edram_rov_used=*/false, /*gamma_render_target_as_unorm8=*/false,
      /*msaa_2x_supported=*/true, DrawResolutionScale(), DrawResolutionScale());
}

DxbcShader::Translation* ShaderCache::Translate(DxbcShader& shader, uint64_t modification,
                                                DxbcShaderTranslator* own) {
  // Local change (22/11/2026): the translation outside translate_mutex_, one shader by one thread
  // at a time (busy_): DxbcShaderTranslator::PostTranslation fills the shader's bindings at its
  // first translation, and they are read once Translate returns.
  std::unique_lock<std::mutex> lock(translate_mutex_);
  {
    HangMark hang_mark(HangWhat::kTranslateWait, shader.ucode_data_hash());  // the freeze probe
    busy_cv_.wait(lock, [&] { return !busy_.count(&shader); });
  }
  bool is_new = false;
  DxbcShader::Translation* translation = shader.GetOrCreateTranslation(modification, &is_new);
  if (is_new) {
    busy_.insert(&shader);
    lock.unlock();
    auto t = std::chrono::steady_clock::now();
    if (own) {
      HangMark hang_mark(HangWhat::kTranslating, shader.ucode_data_hash());
      own->TranslateAnalyzedShader(*translation);
    } else {
      std::unique_lock<std::mutex> own_lock(own_translator_mutex_, std::defer_lock);
      {
        HangMark hang_wait(HangWhat::kTranslatorLock, shader.ucode_data_hash());
        own_lock.lock();
      }
      HangMark hang_mark(HangWhat::kTranslating, shader.ucode_data_hash());
      translator_.TranslateAnalyzedShader(*translation);
    }
    double us = Us(t);
    if (!REXCVAR_GET(pgr4_native_shader_dump_dir).empty() && translation->is_valid()) {
      char name[64];
      std::snprintf(name, sizeof(name), "%016llX_%016llX.%s.dxbc",
                    static_cast<unsigned long long>(shader.ucode_data_hash()),
                    static_cast<unsigned long long>(modification), Stage(shader));
      const std::vector<uint8_t>& binary = translation->translated_binary();
      DumpFile(name, binary.data(), binary.size());
    }
    if (!translation->is_valid()) {
      REXLOG_ERROR("pgr4 native: {} shader {:016X} modification {:016X} failed to translate",
                   shader.type() == rex::graphics::xenos::ShaderType::kVertex ? "vertex" : "pixel",
                   shader.ucode_data_hash(), modification);
    }
    lock.lock();
    stats_.translate_us += us;
    stats_.max_translate_us = std::max(stats_.max_translate_us, us);
    ++stats_.translations;
    if (!translation->is_valid()) ++stats_.failures;
    busy_.erase(&shader);
    busy_cv_.notify_all();
  }
  return translation->is_valid() ? translation : nullptr;
}

}  // namespace pgr4::native
