// pgr4-native PGR4 native renderer: DLSS Multi Frame Generation on RTX 40 (mfg_unlock.h).
#include "mfg_unlock.h"

#include <d3d12.h>
#include <windows.h>

#include "mfg_unlock/midpoint_fix.h"
#include "mfg_unlock/ngx_mfg_gate.h"
#include "mfg_unlock/universal_wrapper_profile.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "version.lib")

REXCVAR_DEFINE_BOOL(pgr4_native_dlss_mfg_unlock, true, "PGR4 native",
                    "On an RTX 40, patch the loaded sl.dlss_g.dll and nvngx_dlssg.dll in memory "
                    "for DLSS Multi Frame Generation, as MFG Unlocker does (mfg_unlock.h); "
                    "restart to apply")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace pgr4::native::mfg_unlock {
namespace {

uint32_t g_max_frames = 1;

std::string Narrow(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr,
                                    nullptr);
  std::string s(size_t(std::max(n, 0)), '\0');
  if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

void LogMidpoint(const wchar_t* message) {
  REXLOG_INFO("PGR4 native MFG: {}", Narrow(message ? message : L""));
}

// sl.dlss_g's `mov edx,max; cmp ecx,edx; cmovb edx,ecx` (ecx: the provider's maximum): the cmovb
// becomes three nops, the cached maximum the compiled one (MFG Unlocker's
// PatchUniqueWrapperMaximumPattern, its Ada branch). Returns that maximum, 0 when not patched.
uint32_t PatchWrapperMaximum(HMODULE module) {
  auto* base = reinterpret_cast<uint8_t*>(module);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
  const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
  uint8_t* match = nullptr;
  size_t matches = 0;
  for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
    if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE) ||
        section->VirtualAddress >= nt->OptionalHeader.SizeOfImage)
      continue;
    uint8_t* begin = base + section->VirtualAddress;
    const size_t size =
        std::min<size_t>(nt->OptionalHeader.SizeOfImage - section->VirtualAddress,
                         std::max<size_t>(section->Misc.VirtualSize, section->SizeOfRawData));
    for (size_t offset = 0; offset + universal_wrapper_profile::kPatternSize <= size; ++offset) {
      if (!universal_wrapper_profile::Matches(begin + offset, size - offset)) continue;
      match = begin + offset;
      ++matches;
    }
  }
  if (matches != 1) {
    REXLOG_WARN("PGR4 native MFG: sl.dlss_g.dll: {} maximum clamps found (one expected)", matches);
    return 0;
  }
  uint32_t maximum = 0;
  std::memcpy(&maximum, match + universal_wrapper_profile::kMaximumOffset, sizeof(maximum));
  uint8_t* const address = match + universal_wrapper_profile::kPatchOffset;
  static constexpr uint8_t kOriginal[] = {0x0F, 0x42, 0xD1};
  static constexpr uint8_t kNops[] = {0x90, 0x90, 0x90};
  if (std::memcmp(address, kNops, sizeof(kNops)) == 0) return maximum;
  if (std::memcmp(address, kOriginal, sizeof(kOriginal)) != 0) return 0;
  DWORD old_protection = 0;
  if (!VirtualProtect(address, sizeof(kNops), PAGE_EXECUTE_READWRITE, &old_protection)) {
    REXLOG_WARN("PGR4 native MFG: sl.dlss_g.dll: VirtualProtect failed ({})", GetLastError());
    return 0;
  }
  std::memcpy(address, kNops, sizeof(kNops));
  FlushInstructionCache(GetCurrentProcess(), address, sizeof(kNops));
  DWORD ignored = 0;
  VirtualProtect(address, sizeof(kNops), old_protection, &ignored);
  REXLOG_INFO("PGR4 native MFG: sl.dlss_g.dll maximum clamp patched at RVA {:#x} (compiled "
              "maximum {} frames)",
              size_t(address - base), maximum);
  return maximum;
}

}  // namespace

bool Apply(ID3D12Device* device) {
  if (!REXCVAR_GET(pgr4_native_dlss_mfg_unlock) || !device) return false;
  midpoint_fix::SetLogCallback(&LogMidpoint);
  // The renderer's adapter an RTX 40 (CUDA compute capability 8.9, matched by LUID).
  if (!midpoint_fix::ObserveD3D12Device(device)) {
    REXLOG_INFO("PGR4 native MFG: not an RTX 40 adapter; frame generation keeps one frame");
    return false;
  }
  const HMODULE wrapper = GetModuleHandleW(L"sl.dlss_g.dll");
  if (!wrapper) {
    REXLOG_WARN("PGR4 native MFG: sl.dlss_g.dll not loaded before slSetD3DDevice");
    return false;
  }
  // The provider NGX loads: beside the executable (no over-the-air updates, streamline.cpp).
  // Loaded here so that it is patched before sl.dlss_g's startup and its first frame generation;
  // the reference is kept for the process's life.
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  const std::wstring path = (std::filesystem::path(exe).parent_path() / L"nvngx_dlssg.dll").wstring();
  const HMODULE provider = LoadLibraryW(path.c_str());
  if (!provider) {
    REXLOG_WARN("PGR4 native MFG: LoadLibrary(nvngx_dlssg.dll) failed ({})", GetLastError());
    return false;
  }
  const ngx_mfg_gate::Result gate = ngx_mfg_gate::Patch(provider, true);
  if (!gate.patched) {
    REXLOG_WARN("PGR4 native MFG: nvngx_dlssg.dll frame count gate not patched (version or "
                "layout not supported)");
    return false;
  }
  if (!midpoint_fix::PatchProvider(provider, path.c_str())) {
    REXLOG_WARN("PGR4 native MFG: nvngx_dlssg.dll midpoint fix not applied (failure {})",
                midpoint_fix::FailureCode());
    return false;
  }
  // The wrapper last: its maximum only raised over a provider that accepts it.
  const uint32_t maximum = PatchWrapperMaximum(wrapper);
  if (!universal_wrapper_profile::IsSupportedMaximum(maximum)) return false;
  g_max_frames = maximum;
  REXLOG_INFO("PGR4 native MFG: multi frame generation unlocked, up to x{}", maximum + 1);
  return true;
}

uint32_t MaxGeneratedFrames() { return g_max_frames; }

}  // namespace pgr4::native::mfg_unlock
