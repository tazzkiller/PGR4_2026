// Ucode -> DXBC cache of the native renderer. Shaders are keyed by the XXH3 hash of their
// guest (big-endian) ucode, the same ucode_data_hash the Xenos plugin uses in its dumps and in
// its shader storage (.xsh), and translations by (shader, modification).
// Threading: Translate from any thread (local change, 22/11/2026: the prewarm's own thread too),
// under translate_mutex_ (one translator; a shader's translations map). Load and Find take a lock: Load
// also runs on the guest thread (DrawRenderer::Prepare in render thread mode, and
// BoolSpecializer::Create) while the recording thread calls it from Prewarm, and Find serves
// DrawRenderer::ShaderDisassembly.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include <rex/string/buffer.h>

#include "shader_translate.h"

namespace pgr4::native {

struct ShaderCacheStats {
  uint64_t shaders = 0;       // distinct ucode loaded
  uint64_t translations = 0;  // distinct (ucode, modification) translated
  uint64_t failures = 0;      // translations that came out invalid
  double analyze_us = 0;      // total ucode analysis time
  double translate_us = 0;    // total translation time
  double max_translate_us = 0;
};

class ShaderCache {
 public:
  ShaderCache();
  ~ShaderCache();

  const DxbcShaderTranslator& translator() const { return translator_; }

  // ucode: dword_count dwords as they are in guest memory (big-endian). Analyzes new ucode.
  DxbcShader* Load(rex::graphics::xenos::ShaderType type, const uint32_t* guest_ucode,
                   uint32_t dword_count);

  // Returns the translation for the modification, translating it on first use; nullptr if it is
  // invalid (counted in stats().failures and logged once). Local change (22/11/2026): with `own`,
  // that translator (one per thread: MakeTranslator), else the cache's; a shader translated by one
  // thread at a time (the first translation sets its bindings), the others wait for it.
  DxbcShader::Translation* Translate(DxbcShader& shader, uint64_t modification,
                                     DxbcShaderTranslator* own = nullptr);
  // Local change (22/11/2026): a translator made as the cache's (the same bytecode), for a thread.
  std::unique_ptr<DxbcShaderTranslator> MakeTranslator() const;

  // The loaded shader with ucode hash `hash`, or nullptr.
  const DxbcShader* Find(uint64_t hash);

  // A copy, under both locks (Load and Translate write their halves).
  ShaderCacheStats stats() const {
    std::lock_guard<std::mutex> load_lock(load_mutex_);
    std::lock_guard<std::mutex> translate_lock(translate_mutex_);
    return stats_;
  }

 private:
  DxbcShaderTranslator translator_;
  rex::string::StringBuffer disasm_buffer_;
  mutable std::mutex load_mutex_;  // shaders_, disasm_buffer_, loading_, the Load half of stats_
  // Local change (22/11/2026, pgr4_native_shader_analyze_unlocked): the ucode being analyzed
  // outside load_mutex_ (each with its own text buffer); another Load of it waits for it.
  std::condition_variable load_cv_;
  std::unordered_set<uint64_t> loading_;
  // Local change (22/11/2026): each shader's translations, busy_, the Translate half of stats_;
  // translator_ under its own lock (held while it translates, outside translate_mutex_).
  mutable std::mutex translate_mutex_;
  std::mutex own_translator_mutex_;
  std::condition_variable busy_cv_;
  std::unordered_set<const DxbcShader*> busy_;  // shaders being translated
  std::unordered_map<uint64_t, std::unique_ptr<DxbcShader>> shaders_;
  ShaderCacheStats stats_;
};

}  // namespace pgr4::native
