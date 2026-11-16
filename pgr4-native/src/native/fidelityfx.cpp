// pgr4-native PGR4 native renderer: the FidelityFX API loader. See fidelityfx.h.
#include "fidelityfx.h"

#include "d3d12_device.h"  // windows.h

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <cmath>

REXCVAR_DECLARE(int32_t, pgr4_render_height);  // renderer.cpp

namespace pgr4::native {

// Local change (pgr4 project): 0 is the screen's shape, the game's 1280 columns and as many rows
// as fill the main display (16:9 -> 720, 16:10 -> 800, 21:9 -> about 540), even, within 360
// (32:9) and 800 (the tallest frame tested). Decided once: the setting takes a restart, and the
// game's hooks ask it at every viewport.
uint32_t FfxSceneHeight() {
  static const uint32_t height = [] {
    const int32_t set = REXCVAR_GET(pgr4_render_height);
    if (set != 0) return uint32_t(std::clamp<int32_t>(set, 360, 2160));
    DEVMODEW mode = {};
    mode.dmSize = sizeof(mode);
    uint32_t w = 1920, h = 1080;
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmPelsWidth &&
        mode.dmPelsHeight) {
      w = mode.dmPelsWidth;
      h = mode.dmPelsHeight;
    }
    const int32_t rows = int32_t(std::lround(1280.0 * h / w / 2.0)) * 2;
    const uint32_t r = uint32_t(std::clamp(rows, 360, 800));
    REXLOG_INFO("PGR4 render height: {} (screen {}x{})", r, w, h);
    return r;
  }();
  return height;
}

const FfxApi* LoadFfxApi() {
  static const FfxApi api = [] {
    FfxApi a;
    HMODULE dll = LoadLibraryW(L"amd_fidelityfx_loader_dx12.dll");
    if (!dll) {
      REXLOG_ERROR("PGR4 native: cannot load amd_fidelityfx_loader_dx12.dll ({})",
                   uint32_t(GetLastError()));
      return a;
    }
    a.create = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(dll, "ffxCreateContext"));
    a.destroy = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(dll, "ffxDestroyContext"));
    a.configure = reinterpret_cast<PfnFfxConfigure>(GetProcAddress(dll, "ffxConfigure"));
    a.query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(dll, "ffxQuery"));
    a.dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(dll, "ffxDispatch"));
    if (!a.create || !a.destroy || !a.configure || !a.query || !a.dispatch) {
      REXLOG_ERROR("PGR4 native: amd_fidelityfx_loader_dx12.dll lacks the ffx API");
      a = {};
    }
    return a;
  }();
  return api.create ? &api : nullptr;
}

bool CameraFromClipRows(const float* m, FfxCamera& out) {
  const float *x = m, *y = m + 4, *z = m + 8, *w = m + 12;
  const float wn2 = w[0] * w[0] + w[1] * w[1] + w[2] * w[2];
  if (wn2 < 1e-8f) return false;
  // z row = a * w row + (0, 0, 0, b): z_ndc = a + b / view depth.
  const float a = (z[0] * w[0] + z[1] * w[1] + z[2] * w[2]) / wn2;
  const float b = z[3] - a * w[3];
  if (b == 0.0f) return false;
  const float ex = z[0] - a * w[0], ey = z[1] - a * w[1], ez = z[2] - a * w[2];
  const float scale = std::max(std::abs(a), 1e-4f) * std::sqrt(wn2);
  if (ex * ex + ey * ey + ez * ez > 1e-6f * scale * scale) return false;
  const float near_plane = b / (1.0f - a), far_plane = -b / a;
  if (!(near_plane > 0.0f && near_plane <= 1.0f && far_plane >= 100.0f)) return false;
  const float fy = std::sqrt((y[0] * y[0] + y[1] * y[1] + y[2] * y[2]) / wn2);
  const float fx = std::sqrt((x[0] * x[0] + x[1] * x[1] + x[2] * x[2]) / wn2);
  const float fov_y = 2.0f * std::atan(1.0f / fy);
  // Local change (pgr4 project): fy / fx is the frame's aspect, up to 32:9 (3.56) since the
  // game takes it from the render height (pgr4_hooks.cpp, Pgr4ApplyRenderAspect).
  if (!(fov_y > 0.15f && fov_y < 2.6f && fy / fx > 1.2f && fy / fx < 4.0f)) return false;
  out = {near_plane, far_plane, fov_y};
  return true;
}

}  // namespace pgr4::native
