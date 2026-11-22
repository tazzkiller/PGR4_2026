// Local addition: the player's gamertag. The SDK emulates one signed-in profile, named "User",
// whose saves live under <user_data_root>\<xuid>\4D5307F9. The name comes from pgr4.toml
// (pgr4_profile_name, set on PGR4Launcher's PLAYER NAME row); at the first launch without one
// (no save yet) the account is made as "PGR4 PLAYER", with player 2's ("PGR4 PLAYER 2"). It is
// given to the profile at every launch, so the game creates its save with it.

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xam/user_profile.h>

#include <rex/hook.h>
#include <rex/ppc/func.h>

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

REXCVAR_DEFINE_STRING(pgr4_profile_name, "", "PGR4",
                      "The player's gamertag (1 to 15 letters, digits or spaces, starting with a "
                      "letter); empty: PGR4 PLAYER at the first launch without a save, else "
                      "\"User\"");
REXCVAR_DEFINE_BOOL(pgr4_first_launch_done, false, "PGR4",
                    "The first launch's settings were made (definition from the screen, no "
                    "upscaler, no frame generation); false: made again at the next launch");
REXCVAR_DECLARE(int32_t, pgr4_native_resolution_scale);
namespace pgr4::native {
uint32_t FfxSceneHeight();  // pgr4-native fidelityfx.cpp: the render height
}
REXCVAR_DECLARE(bool, pgr4_native_upscaler);
REXCVAR_DECLARE(bool, pgr4_native_dlss_fg);
REXCVAR_DECLARE(bool, pgr4_native_frame_generation);
REXCVAR_DECLARE(double, pgr4_native_gtao_radius);     // pgr4-native scene_effects.cpp
REXCVAR_DECLARE(double, pgr4_native_gtao_intensity);
REXCVAR_DECLARE(double, pgr4_native_gtao_distance);
REXCVAR_DECLARE(bool, pgr4_native_gtao_full);
REXCVAR_DECLARE(bool, pgr4_native_gpu_serialize_fg);  // pgr4-native d3d12_device.cpp
REXCVAR_DECLARE(std::string, pgr4_native_dlss_mode);  // pgr4-native upscaler.cpp
REXCVAR_DECLARE(std::string, pgr4_native_fsr_mode);
REXCVAR_DECLARE(int32_t, pgr4_native_dlss_fg_mvec_x);  // pgr4-native upscaler.cpp
REXCVAR_DECLARE(int32_t, pgr4_native_dlss_fg_mvec_y);
REXCVAR_DECLARE(double, pgr4_native_tune_flare);       // pgr4-native shader_replace.cpp
REXCVAR_DECLARE(double, pgr4_native_tune_flare_halo);
REXCVAR_DECLARE(std::string, pgr4_native_tune_water_light_rain);
REXCVAR_DECLARE(std::string, pgr4_native_xegtao_fog);   // pgr4-native scene_effects.cpp
REXCVAR_DECLARE(double, pgr4_native_xegtao_intensity);
REXCVAR_DECLARE(double, pgr4_native_volumetric_light_strength);  // pgr4-native volumetrics.cpp
REXCVAR_DECLARE(double, pgr4_native_volumetric_light_cone);
REXCVAR_DECLARE(double, pgr4_native_volumetric_light_cone_around);
// Local change (16/11/2026): the version of the settings in pgr4.toml, for Pgr4MigrateSettings.
// A change of default reaches a pgr4.toml an earlier version wrote once, then the version is
// raised: a value the player sets afterwards, the old default included, stays.
REXCVAR_DEFINE_INT32(pgr4_settings_version, 0, "PGR4",
                     "The version of the settings in pgr4.toml (0: written before V1.4); the game "
                     "brings an older one up to date once");
// Local change (22/11/2026, Tazz: a friend's very old pgr4.toml kept the settings overlay on F2 and no
// free camera on F5): settings version 6, the release's keys a pgr4.toml lacks added to it.
REXCVAR_DEFINE_BOOL(pgr4_migrate_release_keys, true, "PGR4",
                    "Settings version 6: a release's pgr4.toml without its keys lines (the free "
                    "camera, the development keys left empty) gets them (off: nothing added)");
REXCVAR_DEFINE_STRING(pgr4_profile2_name, "", "PGR4",
                      "The second player's gamertag (split screen); empty: player 1's with \" 2\" "
                      "(made at the first launch)");

namespace {

constexpr int kMaxName = 15;
// The name of an account made without one chosen (pgr4.exe started on its own).
constexpr char kDefaultName[] = "PGR4 PLAYER";

// Player 2's gamertag from player 1's: "<name> 2" (the name cut to keep 15 characters).
std::string SecondPlayerName(std::string first) {
  if (first.empty()) first = "User";
  if (first.size() > kMaxName - 2) first.resize(kMaxName - 2);
  while (!first.empty() && first.back() == ' ') first.pop_back();
  return first + " 2";
}

// `key = value` (`value` as TOML writes it) in pgr4.toml beside the executable and the project's
// copy; a new key goes at the end after `comment`.
void SaveSetting(const std::string& key, const std::string& value, const char* comment) {
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::vector<std::filesystem::path> paths = {std::filesystem::path(exe).parent_path() /
                                              "pgr4.toml"};
#ifdef PGR4_SOURCE_TOML
  // Local change (31/10/2026): only from the build folder. A copy elsewhere (the desktop
  // release) wrote its own folders into the project's, which the next build copied back.
  if (std::error_code ec; std::filesystem::exists(PGR4_SOURCE_TOML) &&
      std::filesystem::equivalent(paths.front().parent_path(), PGR4_BUILD_DIR, ec))
    paths.push_back(PGR4_SOURCE_TOML);
#endif
  for (const auto& path : paths) {
    std::vector<std::string> file;
    {
      std::ifstream in(path);
      for (std::string line; std::getline(in, line);) file.push_back(line);
    }
    bool found = false;
    for (std::string& line : file) {
      const size_t start = line.find_first_not_of(" \t");
      if (start == std::string::npos || line.compare(start, key.size(), key) != 0) continue;
      const size_t eq = line.find_first_not_of(" \t", start + key.size());
      if (eq == std::string::npos || line[eq] != '=') continue;
      line = key + " = " + value;
      found = true;
    }
    if (!found) {
      file.push_back(comment);
      file.push_back(key + " = " + value);
    }
    std::ofstream out(path, std::ios::trunc);
    for (const std::string& line : file) out << line << '\n';
  }
}

// pgr4_profile_name = "<name>" (and the second player's).
void SaveName(const std::string& key, const std::string& name) {
  SaveSetting(key, "\"" + name + "\"", "# Gamertag set by src/pgr4_profile.cpp.");
}

}  // namespace

bool Pgr4SignInSecondPlayer(HWND owner);

namespace {

// The system sign-in UI's stand-in, beside the game as the console's: XN_SYS_UI on, the dialog
// on a thread of its own in front of the game's window, then FinishSigninUI (XN_SYS_SIGNINCHANGED,
// XN_SYS_UI off). One at a time.
std::atomic<bool> g_signin_open{false};

void OpenSignin(rex::system::KernelState* kernel, bool notify_open) {
  if (g_signin_open.exchange(true)) return;
  // XN_SYS_UI (on)
  if (notify_open) kernel->BroadcastNotification(0x00000009, 1);
  HWND game = GetForegroundWindow();
  std::thread([kernel, game] {
    Pgr4SignInSecondPlayer(game);
    kernel->FinishSigninUI();
    g_signin_open = false;
  }).detach();
}

}  // namespace

// Pgr4App::OnPreLaunchModule: before the game's first instruction.
void Pgr4SetupProfile(rex::Runtime* runtime) {
  rex::system::KernelState* kernel = runtime ? runtime->kernel_state() : nullptr;
  if (!kernel || !kernel->user_profile()) return;
  rex::system::xam::UserProfile* profile = kernel->user_profile();
  // The game's own sign-in UI requests (Intro\ChangeUsers.lua); XamShowSigninUI already
  // reported the UI open.
  kernel->set_signin_ui_handler([kernel](uint32_t) { OpenSignin(kernel, false); });
  std::string name = REXCVAR_GET(pgr4_profile_name);
  if (name.empty()) {
    // A save already there: the profile it was made with keeps its name.
    char xuid[17];
    std::snprintf(xuid, sizeof(xuid), "%016llX", (unsigned long long)profile->xuid());
    const std::filesystem::path saves =
        runtime->user_data_root() / xuid / "4D5307F9" / "00000001";
    std::error_code ec;
    const bool has_save =
        std::filesystem::exists(saves, ec) && !std::filesystem::is_empty(saves, ec);
    if (has_save) return;
    // No account yet: made with the default name (the launcher writes one before).
    name = kDefaultName;
    REXCVAR_SET(pgr4_profile_name, name);
    SaveName("pgr4_profile_name", name);
    // Player 2's (split screen) made with it: "<name> 2".
    REXCVAR_SET(pgr4_profile2_name, SecondPlayerName(name));
    SaveName("pgr4_profile2_name", REXCVAR_GET(pgr4_profile2_name));
  }
  profile->set_name(name);
  REXLOG_INFO("PGR4 profile: gamertag \"{}\"", name);
}

// The system sign-in UI's stand-in (XamShowSigninUI, from the game's Intro\ChangeUsers.lua):
// the second player signed in at user index 1 (controller 2), named as before or asked now. The
// game then sets its profiles up as on a console (SetupGamerProfiles, ProfileLoad.lua: player 2
// on controller 2). `owner`: unused since player 2's name comes from player 1's.
bool Pgr4SignInSecondPlayer(HWND owner) {
  rex::Runtime* runtime = rex::Runtime::instance();
  rex::system::KernelState* kernel = runtime ? runtime->kernel_state() : nullptr;
  if (!kernel) return false;
  if (kernel->user_profile(1)) return true;
  // Player 2's profile is player 1's name with a 2 ("Tazz 2"), made with it at the first launch
  // (or by the launcher) or here for a profile made before.
  std::string name = REXCVAR_GET(pgr4_profile2_name);
  if (name.empty()) {
    name = SecondPlayerName(kernel->user_profile()->name());
    REXCVAR_SET(pgr4_profile2_name, name);
    SaveName("pgr4_profile2_name", name);
  }
  kernel->SignInUser(1, name);
  REXLOG_INFO("PGR4 profile: player 2 \"{}\" signed in", name);
  return true;
}

namespace {
// Back + Start asked for player 2's sign-out: the game's popup opens (pgr4_menu.cpp,
// Pgr4SignOutAsked from GUIBank's GlobalUpdate).
std::atomic<bool> g_signout_asked{false};
}  // namespace

bool Pgr4SignOutAsked() { return g_signout_asked.exchange(false); }
bool Pgr4InGameScreen();  // pgr4_menu.cpp: the last screen was a race's

// The popup's yes (Intro\PGR4_SignOut2.lua): player 2 signed out, the game told
// (XN_SYS_SIGNINCHANGED), the title screen next.
void Pgr4SignOutSecondPlayer() {
  rex::Runtime* runtime = rex::Runtime::instance();
  rex::system::KernelState* kernel = runtime ? runtime->kernel_state() : nullptr;
  if (kernel && kernel->SignOutUser(1)) REXLOG_INFO("PGR4 profile: player 2 signed out");
}

// The console's Guide on controllers 1 and 2 (user indexes 0 and 1), as Back + Start held (or
// Guide, with guide_button pass-through). Without player 2: on controller 2, the sign-in UI there;
// the game reacts to the sign-in as on a console (back to its start screen, profiles loaded, the
// split screen unlocked). With player 2: on either, the game's popup asking whether to sign player
// 2 out and go back to one player (A yes, B no; pgr4_menu.cpp). The buttons held are kept from the
// game until released (Start would open the pause menu). In the menus, controller 2's Back and
// Start never reach the game: they are this combination's (in a race, Start pauses for player 2).
// sub_826A8F00(user, state) is the game's XamInputGetState thunk.
bool Pgr4AutopilotInput(uint8_t* base, uint32_t state);  // src/pgr4_autopilot.cpp
bool Pgr4FreeCamInput(uint8_t* base, uint32_t state);    // src/pgr4_freecam.cpp
REX_EXTERN(__imp__sub_826A8F00);
REX_HOOK_RAW(sub_826A8F00) {
  const uint32_t user = ctx.r3.u32, state = ctx.r4.u32;
  __imp__sub_826A8F00(ctx, base);
  // Development: the free camera takes the controller while it is on (src/pgr4_freecam.cpp).
  if (user == 0 && state && Pgr4FreeCamInput(base, state)) {
    ctx.r3.u64 = 0;
    return;
  }
  // Development: the test driver answers for player 1 (off without pgr4_autopilot_dir).
  if (user == 0 && state && Pgr4AutopilotInput(base, state)) {
    ctx.r3.u64 = 0;
    return;
  }
  static bool held[4], swallow[4];
  if (user >= 4 || ctx.r3.u32 != 0 || !state) return;
  // X_INPUT_STATE: packet number, then XINPUT_GAMEPAD (big-endian buttons first, 12 bytes).
  const uint16_t buttons = uint16_t(base[state + 4] << 8 | base[state + 5]);
  constexpr uint16_t kStart = 0x0010, kBack = 0x0020, kGuide = 0x0400;
  const bool pressed = (buttons & kGuide) || (buttons & (kStart | kBack)) == (kStart | kBack);
  const bool fresh = pressed && !held[user];
  held[user] = pressed;
  if (user == 1 && (buttons & (kStart | kBack)) && !Pgr4InGameScreen()) {
    const uint16_t kept = buttons & ~(kStart | kBack);
    base[state + 4] = uint8_t(kept >> 8);
    base[state + 5] = uint8_t(kept);
  }
  if (swallow[user]) {
    if (!buttons) swallow[user] = false;
    std::memset(base + state + 4, 0, 12);
    return;
  }
  if (!fresh || user > 1) return;
  rex::Runtime* runtime = rex::Runtime::instance();
  rex::system::KernelState* kernel = runtime ? runtime->kernel_state() : nullptr;
  if (!kernel) return;
  if (kernel->user_profile(1)) {
    g_signout_asked = true;
    swallow[user] = true;
    std::memset(base + state + 4, 0, 12);
    REXLOG_INFO("PGR4 profile: sign player 2 out? (asked on controller {})", user + 1);
  } else if (user == 1) {
    OpenSignin(kernel, true);
  }
}

// Development: --pgr4_dump_image=<file> writes the loaded executable (Title Update 2 applied,
// 0x82000000, 0xE50000 bytes) before the game starts, for finding code by its data.
REXCVAR_DEFINE_STRING(pgr4_dump_image, "", "PGR4",
                      "Development: the loaded executable's image written to this file at start");

void Pgr4DumpImage() {
  const std::string path = REXCVAR_GET(pgr4_dump_image);
  if (path.empty()) return;
  constexpr uint32_t kBase = 0x82000000, kSize = 0xE50000;
  const uint8_t* image = rex::system::kernel_memory()->TranslateVirtual<const uint8_t*>(kBase);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(image), kSize);
  REXLOG_INFO("PGR4: image written to {} ({})", path, out ? "ok" : "failed");
}

namespace {

// The main display's mode (1920x1080 when Windows does not say).
void ScreenSize(uint32_t& width, uint32_t& height) {
  DEVMODEW mode = {};
  mode.dmSize = sizeof(mode);
  width = 1920;
  height = 1080;
  if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmPelsWidth &&
      mode.dmPelsHeight) {
    width = mode.dmPelsWidth;
    height = mode.dmPelsHeight;
  }
}

}  // namespace

// Pgr4App::OnPreLaunchModule, before the renderer reads its settings: at the first launch, the
// definition from the screen (the smallest multiple of the game's frame, 1280 x render height, at
// least the screen's size: 1080p screen -> 1440p, 2160p -> 2160p), no upscaler and no frame
// generation, kept in pgr4.toml.
void Pgr4FirstLaunchConfig() {
  if (REXCVAR_GET(pgr4_first_launch_done)) return;
  uint32_t width, height;
  ScreenSize(width, height);
  const uint32_t rows = pgr4::native::FfxSceneHeight();  // pgr4_render_height, 0: the screen's
  const uint32_t need = std::max((height + rows - 1) / rows, (width + 1279) / 1280);
  const int32_t scale = std::clamp(int32_t(need), 1, 4);
  REXCVAR_SET(pgr4_native_resolution_scale, scale);
  REXCVAR_SET(pgr4_native_upscaler, false);
  REXCVAR_SET(pgr4_native_dlss_fg, false);
  REXCVAR_SET(pgr4_native_frame_generation, false);
  REXCVAR_SET(pgr4_first_launch_done, true);
  const char* comment = "# First launch's settings (src/pgr4_profile.cpp).";
  SaveSetting("pgr4_native_resolution_scale", std::to_string(scale), comment);
  SaveSetting("pgr4_native_upscaler", "false", comment);
  SaveSetting("pgr4_native_dlss_fg", "false", comment);
  SaveSetting("pgr4_native_frame_generation", "false", comment);
  SaveSetting("pgr4_first_launch_done", "true", comment);
  REXLOG_INFO("PGR4 first launch: screen {}x{}, definition {}x{}, no upscaler, no frame "
              "generation", width, height, 1280 * scale, rows * scale);
}

// Pgr4App::OnPreLaunchModule, before the renderer reads its settings: settings whose defaults
// changed, in a pgr4.toml an earlier version wrote. The game's Graphics page saves every renderer
// setting (SaveRendererSettings), so a player who changed any of them has the GTAO's tuning too,
// which only the developer overlay sets: its V1.3 defaults (radius 1 m, intensity 1, 300 m, half
// size) exactly, all four, are taken for the old defaults and become the new ones (16/11/2026,
// Tazz: 5 m, 0.15, 1500 m, full size). Any other value is a choice, kept.
// Whether pgr4.toml beside the executable sets `key` (a line `key = ...`, not commented out).
static bool HasSetting(const std::string& key) {
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::ifstream in(std::filesystem::path(exe).parent_path() / "pgr4.toml");
  for (std::string line; std::getline(in, line);) {
    const size_t start = line.find_first_not_of(" \t");
    if (start == std::string::npos || line.compare(start, key.size(), key) != 0) continue;
    const size_t eq = line.find_first_not_of(" \t", start + key.size());
    if (eq != std::string::npos && line[eq] == '=') return true;
  }
  return false;
}

// Version 1 (V1.4): from V1.3's.
static void MigrateToVersion1() {
  // Local change (16/11/2026): a pgr4.toml written before the setting was in pgr4.default.toml
  // gets it, with the value it already had (the built-in default), so the player can see it.
  if (!HasSetting("pgr4_native_gpu_serialize_fg")) {
    SaveSetting("pgr4_native_gpu_serialize_fg",
                REXCVAR_GET(pgr4_native_gpu_serialize_fg) ? "true" : "false",
                "# With DLSS Frame Generation, wait for the GPU at the end of every frame (CPU and "
                "GPU in step):\n# against a wrong sky in the cars' reflections, at the cost of "
                "some frames per second. Off.");
    REXLOG_INFO("PGR4 settings: pgr4_native_gpu_serialize_fg added to pgr4.toml");
  }
  if (REXCVAR_GET(pgr4_native_gtao_radius) == 1.0 &&
      REXCVAR_GET(pgr4_native_gtao_intensity) == 1.0 &&
      REXCVAR_GET(pgr4_native_gtao_distance) == 300.0 && !REXCVAR_GET(pgr4_native_gtao_full)) {
    REXCVAR_SET(pgr4_native_gtao_radius, 5.0);
    REXCVAR_SET(pgr4_native_gtao_intensity, 0.15);
    REXCVAR_SET(pgr4_native_gtao_distance, 1500.0);
    REXCVAR_SET(pgr4_native_gtao_full, true);
    const char* comment = "# Ambient occlusion's new defaults (src/pgr4_profile.cpp).";
    SaveSetting("pgr4_native_gtao_radius", "5.000000", comment);
    SaveSetting("pgr4_native_gtao_intensity", "0.150000", comment);
    SaveSetting("pgr4_native_gtao_distance", "1500.000000", comment);
    SaveSetting("pgr4_native_gtao_full", "true", comment);
    REXLOG_INFO("PGR4 settings: the GTAO's old defaults in pgr4.toml replaced by the new ones");
  }
}

// Version 6: the release's keys lines a pgr4.toml lacks, added to it and applied now (also to the
// keys registered later, the renderer's: the SDK keeps a config value for a key not yet registered
// and gives it at its registration).
static void MigrateToVersion6() {
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  const std::filesystem::path dir = std::filesystem::path(exe).parent_path();
#ifdef PGR4_BUILD_DIR
  if (std::error_code ec; std::filesystem::equivalent(dir, PGR4_BUILD_DIR, ec)) {
    REXLOG_INFO("PGR4 settings: version 6 (the release's keys) skipped (the development build)");
    return;
  }
#endif
  if (!REXCVAR_GET(pgr4_migrate_release_keys)) {
    REXLOG_INFO("PGR4 settings: version 6 (the release's keys) off (pgr4_migrate_release_keys)");
    return;
  }
  static const std::pair<const char*, const char*> kKeys[] = {
      {"pgr4_free_camera", "true"},     {"bind_screenshot", "\"\""},
      {"bind_overlay", "\"\""},         {"bind_renderer_switch", "\"\""},
      {"bind_achievements", "\"\""},    {"bind_upscaler", "\"\""},
      {"bind_upscaler_debug", "\"\""},  {"bind_debug_overlay", "\"\""},
      {"bind_settings", "\"\""}};
  std::vector<std::string> added;
  for (const auto& [key, value] : kKeys)
    if (!HasSetting(key)) added.push_back(std::string(key) + " = " + value);
  if (added.empty()) {
    REXLOG_INFO("PGR4 settings: version 6, the release's keys already in pgr4.toml");
    return;
  }
  // Into pgr4.toml beside the executable, under one comment (SaveSetting writes its comment for
  // each new key).
  const std::filesystem::path toml = dir / "pgr4.toml";
  std::vector<std::string> file;
  {
    std::ifstream in(toml);
    for (std::string line; std::getline(in, line);) file.push_back(line);
  }
  file.push_back("# The release's keys (src/pgr4_profile.cpp, settings version 6): F2 switches between "
                 "the game as it was");
  file.push_back("# and the enhanced version, F5 the free camera; no development key.");
  for (const std::string& line : added) file.push_back(line);
  {
    std::ofstream out(toml, std::ios::trunc);
    for (const std::string& line : file) out << line << '\n';
  }
  // Applied now, as the config file's values: those registered already set, the others kept by the
  // SDK for their registration. A file of these lines only (the rest of pgr4.toml not read again).
  std::string text;
  for (const std::string& line : added) text += line + "\n";
  const std::filesystem::path temp = std::filesystem::temp_directory_path() / "pgr4_release_keys.toml";
  {
    std::ofstream out(temp, std::ios::trunc);
    out << text;
  }
  rex::cvar::LoadConfig(temp);
  std::error_code ec;
  std::filesystem::remove(temp, ec);
  std::string list;
  for (const std::string& line : added) list += (list.empty() ? "" : ", ") + line;
  REXLOG_INFO("PGR4 settings: the release's keys added to pgr4.toml (version 6): {}", list);
}

void Pgr4MigrateSettings() {
  // Local change (16/11/2026): each step once, for a pgr4.toml older than it (pgr4_settings_version;
  // pgr4.default.toml has the current one). Before, the steps ran at every start: a player who
  // chose the GTAO's old values again had them replaced at the next.
  constexpr int32_t kSettingsVersion = 7;
  const int32_t from = REXCVAR_GET(pgr4_settings_version);
  if (from >= kSettingsVersion) return;
  if (from < 1) MigrateToVersion1();
  // Version 2 (17/11/2026): FSR's own quality mode (pgr4_native_fsr_mode), FSR having taken
  // DLSS's until then: a pgr4.toml without it gets the one FSR used, DLSS's.
  if (from < 2 && !HasSetting("pgr4_native_fsr_mode")) {
    const std::string mode = REXCVAR_GET(pgr4_native_dlss_mode);
    REXCVAR_SET(pgr4_native_fsr_mode, mode);
    SaveSetting("pgr4_native_fsr_mode", "\"" + mode + "\"",
                "# FSR's own quality mode (the same choices as DLSS's).");
    REXLOG_INFO("PGR4 settings: pgr4_native_fsr_mode added to pgr4.toml ({}, DLSS's)", mode);
  }
  // Version 3 (19/11/2026, V1.4): DLSS-G's motion vectors as DLSS SR's (pgr4_native_dlss_fg_mvec_x
  // and _y 1, with V1.4's motion vectors): a pgr4.toml holding V1.3's default (-1, -1) gets 1, 1;
  // another value the player chose stays.
  if (from < 3 && REXCVAR_GET(pgr4_native_dlss_fg_mvec_x) == -1 &&
      REXCVAR_GET(pgr4_native_dlss_fg_mvec_y) == -1) {
    REXCVAR_SET(pgr4_native_dlss_fg_mvec_x, 1);
    REXCVAR_SET(pgr4_native_dlss_fg_mvec_y, 1);
    const char* comment = "# DLSS Frame Generation's motion vectors as DLSS's (V1.4).";
    SaveSetting("pgr4_native_dlss_fg_mvec_x", "1", comment);
    SaveSetting("pgr4_native_dlss_fg_mvec_y", "1", comment);
    REXLOG_INFO("PGR4 settings: DLSS-G's motion vectors in pgr4.toml from V1.3's -1 to 1");
  }
  // Version 4 (21/11/2026, Tazz's tunings as the new defaults): a value exactly the old default
  // (the game's Graphics page saves every renderer setting, so most pgr4.toml files hold them)
  // gets the new one; another value the player chose stays.
  if (from < 4) {
    const char* comment = "# Tazz's tunings as the new defaults (src/pgr4_profile.cpp).";
    const auto number = [comment](const char* key, double now, double old_default,
                                  double new_default, auto set) {
      if (now != old_default) return;
      set(new_default);
      char text[32];
      std::snprintf(text, sizeof(text), "%.6f", new_default);
      SaveSetting(key, text, comment);
      REXLOG_INFO("PGR4 settings: {} in pgr4.toml from {} to {}", key, old_default, new_default);
    };
    const auto text = [comment](const char* key, const std::string& now, const char* old_default,
                                const char* new_default, auto set) {
      if (now != old_default) return;
      set(std::string(new_default));
      SaveSetting(key, std::string("\"") + new_default + "\"", comment);
      REXLOG_INFO("PGR4 settings: {} in pgr4.toml from \"{}\" to \"{}\"", key, old_default,
                  new_default);
    };
    // The light flares with the volumetric fog's lamps (shader_replace.cpp).
    number("pgr4_native_tune_flare", REXCVAR_GET(pgr4_native_tune_flare), 0.75, 1.0,
           [](double v) { REXCVAR_SET(pgr4_native_tune_flare, v); });
    number("pgr4_native_tune_flare_halo", REXCVAR_GET(pgr4_native_tune_flare_halo), 0.0, 0.02,
           [](double v) { REXCVAR_SET(pgr4_native_tune_flare_halo, v); });
    // The water in light rain (empty: the rain's until now).
    text("pgr4_native_tune_water_light_rain", REXCVAR_GET(pgr4_native_tune_water_light_rain), "",
         "wave_strength=1 wave_steepness=0.08 wave_noise=0.3 wave_size=1 wave_speed=0 "
         "wind_angle=0.01 sun_waves=0",
         [](const std::string& v) { REXCVAR_SET(pgr4_native_tune_water_light_rain, v); });
    // XeGTAO: 0.1 in fog, 0.2 in every other weather.
    text("pgr4_native_xegtao_fog", REXCVAR_GET(pgr4_native_xegtao_fog), "intensity=0.2 falloff=1",
         "intensity=0.1 falloff=1",
         [](const std::string& v) { REXCVAR_SET(pgr4_native_xegtao_fog, v); });
    number("pgr4_native_xegtao_intensity", REXCVAR_GET(pgr4_native_xegtao_intensity), 0.3, 0.2,
           [](double v) { REXCVAR_SET(pgr4_native_xegtao_intensity, v); });
  }
  // Version 5 (21/11/2026, Tazz's volumetric lamps saved at 20 h 57 as the new defaults): the same
  // rule, a value exactly the old default.
  if (from < 5) {
    const char* comment = "# Tazz's volumetric lamps as the new defaults (src/pgr4_profile.cpp).";
    const auto number = [comment](const char* key, double now, double old_default,
                                  double new_default, auto set) {
      if (now != old_default) return;
      set(new_default);
      char text[32];
      std::snprintf(text, sizeof(text), "%.6f", new_default);
      SaveSetting(key, text, comment);
      REXLOG_INFO("PGR4 settings: {} in pgr4.toml from {} to {}", key, old_default, new_default);
    };
    number("pgr4_native_volumetric_light_strength", REXCVAR_GET(pgr4_native_volumetric_light_strength),
           15.0, 10.0, [](double v) { REXCVAR_SET(pgr4_native_volumetric_light_strength, v); });
    number("pgr4_native_volumetric_light_cone", REXCVAR_GET(pgr4_native_volumetric_light_cone), 50.0,
           65.0, [](double v) { REXCVAR_SET(pgr4_native_volumetric_light_cone, v); });
    number("pgr4_native_volumetric_light_cone_around",
           REXCVAR_GET(pgr4_native_volumetric_light_cone_around), 0.15, 0.1,
           [](double v) { REXCVAR_SET(pgr4_native_volumetric_light_cone_around, v); });
  }
  // Version 6 (22/11/2026): the release's keys (pgr4.default.toml, its lines since V1.1): the free
  // camera on, the development keys empty. The game's code has them on F2 (the settings overlay),
  // F3, F4, F7 and the free camera off; only these lines change that, and the launcher writes
  // pgr4.default.toml only when there is no pgr4.toml, so an older one kept the development keys.
  // A line already there stays (the player's choice). Not in the development build's folder (its
  // pgr4.toml has none of these lines on purpose).
  if (from < 6) MigrateToVersion6();
  // Version 7 (22/11/2026, Tazz: "normalement ça doit être false maintenant par défaut"): the wait
  // for the GPU at each frame with DLSS Frame Generation off by default, as decided on 16/11 (branch
  // perf-cpu, never carried over); the same rule as versions 4 and 5: the old default (true, which
  // version 1 wrote into the pgr4.toml files without the line) gets the new one.
  if (from < 7 && REXCVAR_GET(pgr4_native_gpu_serialize_fg)) {
    REXCVAR_SET(pgr4_native_gpu_serialize_fg, false);
    SaveSetting("pgr4_native_gpu_serialize_fg", "false",
                "# With DLSS Frame Generation, wait for the GPU at the end of every frame: off "
                "(src/pgr4_profile.cpp).");
    REXLOG_INFO("PGR4 settings: pgr4_native_gpu_serialize_fg in pgr4.toml from true to false");
  }
  REXCVAR_SET(pgr4_settings_version, kSettingsVersion);
  SaveSetting("pgr4_settings_version", std::to_string(kSettingsVersion),
              "# The version of these settings (src/pgr4_profile.cpp: brought up to date once).");
  REXLOG_INFO("PGR4 settings: pgr4.toml brought up to version {}", kSettingsVersion);
}

// --pgr4_unlock_all: every Cool Wall pack unlocked in the signed-in profiles, so every car and
// bike, every city and route, the race modes and the cheats the packs hold. The pack database
// (0x82B58478: +468 its packs, +476 their number, a pack's id at +4) and the profiles (4 of
// 0x22AA0 bytes from 0x82BFE2A8, signed in when +84 is not 0) hold a byte per pack id at +19180,
// 1 when bought (UnlockCurrentPack, sub_825E7030). The bytes are set before the game reads them:
// sub_82504890 (is a pack unlocked in a profile), sub_82504BC0 (the packs' rewards), the garage
// and Cool Wall lists (sub_8259A4F8, sub_825052F0, sub_82604368, sub_82604AA0). The game saves
// them with the profile: they stay unlocked with the setting off.
REXCVAR_DECLARE(bool, pgr4_native_precompile);  // pgr4-native draw.cpp

// The launcher's precompile step (pgr4_app.h): the window hidden, no profile asked.
bool Pgr4Precompiling() { return REXCVAR_GET(pgr4_native_precompile); }

// Local change (22/11/2026, Tazz): a fresh log file at each start. The SDK's file sink appends
// (spdlog's basic_file_sink, truncate off): the development build's run.log had grown to 1.2 GB,
// every start since 16/11 after the others. The launcher's precompile writes its own beside it
// (run_precompile.log), else the game's start, which follows it, would remove its lines. Before the
// SDK opens the file (Pgr4App::OnConfigureLogging); the log_file setting as pgr4.toml or the command
// line gives it.
REXCVAR_DEFINE_BOOL(pgr4_log_fresh, true, "PGR4",
                    "The log file started afresh at each start, the launcher's precompile in its own "
                    "(<name>_precompile) (off: appended to, one file)");
void Pgr4FreshLog(rex::LogConfig& config) {
  const std::string file = REXCVAR_GET(log_file);
  if (file.empty() || !REXCVAR_GET(pgr4_log_fresh)) return;
  std::filesystem::path path(file);
  if (Pgr4Precompiling()) {
    path = path.parent_path() / (path.stem().string() + "_precompile" + path.extension().string());
    REXCVAR_SET(log_file, path.string());
  }
  config.log_file = path;
  std::error_code ec;
  std::filesystem::remove(path, ec);
}
void Pgr4HideWindow(void* hwnd) { ShowWindow(static_cast<HWND>(hwnd), SW_HIDE); }

// Local change (15/11/2026, Tazz): the game starts only from PGR4Launcher, which passes
// --pgr4_launcher=true with the folders it found (the game's, the title update's, the DLC's). Started
// on its own, pgr4.exe opens the launcher beside it and exits before setting anything up: a
// game\ folder beside it would otherwise make the SDK keep the saves in userdata\ beside it,
// apart from the launcher's (Documents\pgr4). The development build (its build folder) may still
// start on its own.
REXCVAR_DEFINE_BOOL(pgr4_launcher, false, "PGR4",
                    "Started by PGR4Launcher (set by the launcher on the command line only)");

bool Pgr4StartedByLauncher() {
  if (REXCVAR_GET(pgr4_launcher)) return true;
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  const std::filesystem::path dir = std::filesystem::path(exe).parent_path();
#ifdef PGR4_BUILD_DIR
  if (std::error_code ec; std::filesystem::equivalent(dir, PGR4_BUILD_DIR, ec)) return true;
#endif
  const std::filesystem::path launcher = dir / L"PGR4Launcher.exe";
  std::error_code ec;
  if (std::filesystem::is_regular_file(launcher, ec)) {
    std::wstring command = L"\"" + launcher.wstring() + L"\"";
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(launcher.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                       dir.c_str(), &si, &pi)) {
      AllowSetForegroundWindow(pi.dwProcessId);
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
      return false;
    }
  }
  MessageBoxW(nullptr, L"Start the game with PGR4Launcher.exe, beside pgr4.exe.",
              L"PROJECT GOTHAM RACING 4", MB_OK | MB_ICONINFORMATION);
  return false;
}

REXCVAR_DEFINE_BOOL(pgr4_unlock_all, false, "PGR4",
                    "Every Cool Wall pack (cars, bikes, cities, routes, race modes, cheats) "
                    "unlocked in the signed-in profiles, kept in the save");

namespace {

uint32_t LoadU32(const uint8_t* base, uint32_t address) {
  const uint8_t* p = base + address;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

void UnlockAllPacks(uint8_t* base) {
  if (!REXCVAR_GET(pgr4_unlock_all)) return;
  constexpr uint32_t kPacks = 0x82B58478, kProfiles = 0x82BFE2A8, kProfileSize = 0x22AA0;
  constexpr uint32_t kUnlocked = 19180, kMaxId = 4096;
  const uint32_t count = LoadU32(base, kPacks + 476), packs = LoadU32(base, kPacks + 468);
  if (!count || count > kMaxId || !packs) return;
  static uint32_t logged_mask = 0;
  for (uint32_t slot = 0; slot < 4; ++slot) {
    const uint32_t profile = kProfiles + slot * kProfileSize;
    if (!LoadU32(base, profile + 84)) continue;
    uint32_t newly = 0, max_id = 0;
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t pack = LoadU32(base, packs + 4 * i);
      if (!pack) continue;
      const uint32_t id = LoadU32(base, pack + 4);
      if (id >= kMaxId) continue;
      max_id = std::max(max_id, id);
      uint8_t& flag = base[profile + kUnlocked + id];
      if (!flag) ++newly;
      flag = 1;
    }
    if (newly && !(logged_mask & (1u << slot))) {
      logged_mask |= 1u << slot;
      REXLOG_INFO("PGR4 unlock all: profile {}: {} of {} packs unlocked (ids up to {})", slot,
                  newly, count, max_id);
    }
  }
}

}  // namespace

REX_EXTERN(__imp__sub_82504890);
REX_HOOK_RAW(sub_82504890) {
  UnlockAllPacks(base);
  __imp__sub_82504890(ctx, base);
}
REX_EXTERN(__imp__sub_82504BC0);
REX_HOOK_RAW(sub_82504BC0) {
  UnlockAllPacks(base);
  __imp__sub_82504BC0(ctx, base);
}
REX_EXTERN(__imp__sub_8259A4F8);
REX_HOOK_RAW(sub_8259A4F8) {
  UnlockAllPacks(base);
  __imp__sub_8259A4F8(ctx, base);
}
REX_EXTERN(__imp__sub_825052F0);
REX_HOOK_RAW(sub_825052F0) {
  UnlockAllPacks(base);
  __imp__sub_825052F0(ctx, base);
}
REX_EXTERN(__imp__sub_82604368);
REX_HOOK_RAW(sub_82604368) {
  UnlockAllPacks(base);
  __imp__sub_82604368(ctx, base);
}
REX_EXTERN(__imp__sub_82604AA0);
REX_HOOK_RAW(sub_82604AA0) {
  UnlockAllPacks(base);
  __imp__sub_82604AA0(ctx, base);
}
