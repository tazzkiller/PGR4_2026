// pgr4-native PGR4 native renderer (local change, pgr4 project): the game's pixel shaders
// replaced by HLSL of ours, by their ucode hash, as textures are by theirs (textures.cpp).
//
// <pgr4_native_shader_replace_dir>\<ucode hash>.ps.hlsl (16 hex digits, the hash the shader
// dumps and the draw logs show) defines
//   float4 Shade(XeInput i)
// and is compiled after a prelude made for each translation of the shader: the same bindings
// as the translated shader (DxbcShaderTranslator's ABI, shader_translate.h), so the draw code
// binds it unchanged.
// - XeInput: the translated shader's input signature, read from its DXBC (ISGN): r0..rN are the
//   guest's interpolators (the registers the guest shader starts with), pos SV_Position, front
//   SV_IsFrontFace.
// - c<N>: the guest's float constant cN (only those the guest shader reads; packed as the
//   translator packs them).
// - TEX2D(f, uv), TEX2D_LEVEL(f, uv, level), TEX2D_BIAS(f, uv, bias): a sample of the 2D texture
//   at fetch constant f with its sampler, where the guest shader fetches one (XE_HAS_TF<f>).
//   No fetch-constant offsets, exponent bias or sign handling: the plain texture.
// - xe_fetch_constants (uint4[48], the guest's 32 texture fetch constants of 6 dwords).
// - #include "name.hlsli" finds files beside it (several shaders' variants share one; a change to
//   an included file compiles every replacement again).
// The prelude writes the color times the render target's exponent bias, and the motion vectors
// when the translation writes them, as the translator does.
//
// Hot reload: Poll (once per frame) sees the files written, added or removed since, and counts
// generation() up; the draw code mixes the generation into the pipeline hash of a replaced shader,
// so its draws make pipelines again, with the new code.
//
// Threading: Get from any thread (the pipeline workers); the rest from the renderer's.
#pragma once

#include "shader_translate.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <thread>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pgr4::native {

class ShaderReplacer {
 public:
  // Lists the replacement files.
  void Initialize();
  // Every kPollFrames calls: the files changed (logged) count generation() up; so does
  // pgr4_native_shader_replace switched, at once.
  void Poll();
  // Local change (pgr4 project): the launcher's precompile step makes the pipelines of both states of
  // ENHANCED REFLECTIONS, the setting being changed in the game after it.
  void ForceReflections(bool on) {
    std::lock_guard<std::mutex> lock(mutex_);
    reflections_ = on;
  }
  uint64_t generation() const { return generation_.load(std::memory_order_acquire); }
  // Local change (pgr4 project): changes whenever Has may answer otherwise (the files, on/off,
  // the reflections, the decals' light): the draws' memo of their shaders' answers is keyed by it.
  uint64_t stamp() const {
    return generation() * 8 + (enabled_ ? 1 : 0) + (reflections_ ? 2 : 0) +
           (decal_light_off_ ? 4 : 0) + uint64_t(files_.size()) * 0x100000000ull;
  }
  ~ShaderReplacer();
  // The replacement bytecode for this translation of a pixel shader; nullptr: none (or its
  // compilation failed, logged once).
  const std::vector<uint8_t>* Get(const DxbcShader::Translation& translation);
  size_t count() const { return files_.size(); }
  // Local change (pgr4 project): a car paint's replacement (it includes car_paint_hooks.hlsli).
  bool IsCarPaint(uint64_t hash) const;
  bool IsCarShader(uint64_t hash) const;  // its paint or its other parts (car_reflect_hooks)
  // Local change (pgr4 project): whether this translation's replacement is compiling in the
  // background (Get returned nullptr for it meanwhile).
  bool Pending(const DxbcShader::Translation& translation) {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_.count(&translation) != 0;
  }
  // Whether the pixel shader of this ucode hash has a replacement file: its draws keep the
  // generic shader, not one specialized for their bool constants (shader_specialize.h), which
  // has a hash of its own.
  bool Has(uint64_t hash) const {
    return enabled_ && !files_.empty() && files_.count(hash) &&
           (reflections_ || !reflection_files_.count(hash)) &&
           (decal_light_off_ || !decal_files_.count(hash));
  }

 private:
  static constexpr uint32_t kPollFrames = 20;
  // The translation's replacement bytecode (empty: none), from <dir>\cache when this source was
  // compiled before; slow otherwise (no lock held).
  std::vector<uint8_t> Compile(const DxbcShader::Translation& translation, uint64_t hash,
                               bool cache_only = false);
  // List's key for the included files (*.hlsli): no shader's ucode hash.
  static constexpr uint64_t kIncludes = ~uint64_t(0);
  // The ucode hashes with a file, and the files' last write times.
  std::unordered_map<uint64_t, std::filesystem::file_time_type> List() const;

  std::filesystem::path dir_;
  bool enabled_ = false;
  // Local change (pgr4 project): the enhanced reflections (pgr4_native_enhanced_reflections, the
  // game menu's ENHANCED REFLECTIONS): the replacements of the cars, the water and the wet roads
  // (their files include car_*, water_ or road_reflect_hooks.hlsli), the others kept without them.
  bool reflections_ = false;
  std::unordered_set<uint64_t> reflection_files_;
  // Local change (pgr4 project): the replacements that leave the markings' baked light out
  // (pgr4_native_decal_light_off; their files name pgr4_decal_light), used only while it is on.
  bool decal_light_off_ = false;
  std::unordered_set<uint64_t> decal_files_;
  void ClassifyFiles();  // reflection_files_ from files_ (mutex_ held or not shared yet)
  std::string tuning_;  // TuningDefines as last compiled with
  std::string tuning_wanted_;   // a changed tuning, applied once it held kTuningFrames
  uint32_t tuning_frames_ = 0;
  int tuning_weather_ = -1;  // WaterWeather when tuning_ was taken
  static constexpr uint32_t kTuningFrames = 20;
  static std::string TuningDefines();
  std::unordered_map<uint64_t, std::filesystem::file_time_type> files_;
  // Counted up by a reload (Poll) and by each background compilation that finishes, so that the
  // replaced shaders' pipelines are made again with what is compiled.
  std::atomic<uint64_t> generation_{0};
  uint64_t files_epoch_ = 0;  // counted up by a reload: older background results are dropped
  uint32_t poll_ = 0;
  std::mutex mutex_;
  // Per translation (shader, modification): the bytecode, empty when it failed. A reload retires
  // them (a pipeline worker may still be reading one).
  std::unordered_map<const void*, std::unique_ptr<std::vector<uint8_t>>> compiled_;
  std::vector<std::unique_ptr<std::vector<uint8_t>>> retired_;
  // Background compilation (a shader not in the cache: Get returns nullptr, the game's shader is
  // drawn meanwhile): the translations queued or compiling, and two workers.
  struct Job {
    const DxbcShader::Translation* translation;
    uint64_t hash, epoch;
    bool replace = false;  // a tuning's recompile: the code replaces the one in use when done
  };
  std::unordered_set<const void*> pending_;
  std::unordered_set<const void*> stale_;  // pending when a tuning changed: compiled again once done
  std::deque<Job> jobs_;
  std::condition_variable jobs_cv_;
  std::vector<std::thread> workers_;
  bool stopping_ = false;
  void Work();
};

// Local change (17/11/2026, Tazz: the water glittered in heavy fog): the water's tuning
// (water_hooks.hlsli), the general one (the pgr4_native_tune_wave_* cvars) or the race's weather's:
// --pgr4_native_tune_water_<weather>, "key=value ..." over the general (keys: the fields' names),
// empty for none. The water's shaders are compiled again when the weather changes it.
struct WaterTuning {
  float wave_strength, wave_steepness, wave_noise, wave_size, wave_speed, wind_angle,
      water_reflect, sun_waves, haze;
};
// g_pgr4_weather_kind's 0 (clear) to 6 (heavy snow), then 7: light rain (its kind 3, a name with
// "Light Rain": g_pgr4_light_rain), which takes the rain's tuning while it has none of its own.
constexpr int kWaterWeathers = 8;
int WaterWeather();  // the race's weather, -1 out of a race or unknown
WaterTuning WaterGeneralTuning();
void WaterSetGeneral(const WaterTuning& t);
WaterTuning WaterTuningFor(int weather);  // the general one with the weather's overrides
bool WaterHasOwn(int weather);
void WaterSetOwn(int weather, const WaterTuning* t);  // nullptr: none (the general)

}  // namespace pgr4::native
