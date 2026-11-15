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
REXCVAR_DECLARE(bool, pgr4_native_upscaler);
REXCVAR_DECLARE(bool, pgr4_native_dlss_fg);
REXCVAR_DECLARE(bool, pgr4_native_frame_generation);
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

// Pgr4App::OnPreLaunchModule, before the renderer reads its settings: at the first launch, the
// definition from the screen (the smallest multiple of the game's 720p at least the screen's
// height: 1080p screen -> 1440p, 2160p -> 2160p), no upscaler and no frame generation, kept in
// pgr4.toml.
void Pgr4FirstLaunchConfig() {
  if (REXCVAR_GET(pgr4_first_launch_done)) return;
  DEVMODEW mode = {};
  mode.dmSize = sizeof(mode);
  uint32_t height = 1080;
  if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmPelsHeight)
    height = mode.dmPelsHeight;
  const int32_t scale = std::clamp(int32_t((height + 719) / 720), 1, 4);
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
  REXLOG_INFO("PGR4 first launch: screen {}p, definition {}p, no upscaler, no frame generation",
              height, 720 * scale);
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
