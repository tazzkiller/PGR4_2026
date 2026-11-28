// Project Gotham Racing 4 - ReXGlue recompiled project.
// App class from pgr4-native (MIT, games/pgr4/src/pgr4_recompiled_app.h), named "pgr4" here so
// the settings file is pgr4.toml and the generated code (pgr4_init.h) matches.
#pragma once

#include <rex/rex_app.h>

#include "title_launch.h"

// src/pgr4_profile.cpp: the player's gamertag, asked at the first launch.
void Pgr4SetupProfile(rex::Runtime* runtime);
// src/pgr4_profile.cpp: the first launch's renderer settings (definition from the screen).
void Pgr4DumpImage();
void Pgr4FirstLaunchConfig();
void Pgr4MigrateSettings();  // src/pgr4_profile.cpp: settings whose defaults changed
// src/pgr4_menu.cpp: the game's window brought to the front (the launcher allows it).
void Pgr4BringWindowToFront();
// src/pgr4_profile.cpp: started by the launcher to compile the shaders only.
bool Pgr4Precompiling();
// src/pgr4_profile.cpp: the log file started afresh (pgr4_log_fresh), before the SDK opens it.
void Pgr4FreshLog(rex::LogConfig& config);
// src/pgr4_profile.cpp: false (the launcher opened instead) when pgr4.exe was started on its own.
bool Pgr4StartedByLauncher();
// src/pgr4_hooks.cpp: the game's 16:9 made the frame's own aspect (1280 x render height).
void Pgr4ApplyRenderAspect();
void Pgr4HideWindow(void* hwnd);
// src/pgr4_guest_access.cpp (built in pgr4_recomp): which guest memory accesses the recompiled code uses.
void Pgr4LogGuestAccess();

class Pgr4App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Pgr4App>(new Pgr4App(ctx, "pgr4", PPCImageConfig));
  }

  // No log file unless one is asked for (log_file in pgr4.toml, or --log_file): without a name for
  // them, the SDK makes no numbered logs beside the executable.
  void OnConfigureLogging(rex::LogConfig& config) override {
    config.app_name.clear();
    Pgr4FreshLog(config);
  }

  // Started on its own (not by PGR4Launcher): the launcher opened, nothing set up, the game exits.
  bool SetupEnvironment() override {
    return Pgr4StartedByLauncher() && rex::ReXApp::SetupEnvironment();
  }

  void OnPostLoadXexImage() override { title_launch::Restore(runtime()); }

  void OnPreLaunchModule() override {
    Pgr4LogGuestAccess();
    // The launcher's precompile step (--pgr4_native_precompile, pgr4-native draw.cpp): the
    // window hidden at once, nothing asked or brought to the front; the process exits once the
    // shaders are compiled.
    if (Pgr4Precompiling()) {
      if (rex::ui::Window* w = window()) Pgr4HideWindow(w->GetNativeWindowHandle());
      return;
    }
    // On the UI thread, the window made: the release's title, the mouse cursor hidden after 3 s
    // without motion (shown again when it moves), and the window in front of the launcher.
    if (rex::ui::Window* w = window()) {
      w->SetTitle("PROJECT GOTHAM RACING 4 Edition 2026");
      w->SetCursorAutoHideDelayMs(3000);
      w->SetCursorVisibility(rex::ui::Window::CursorVisibility::kAutoHidden);
    }
    Pgr4BringWindowToFront();
    Pgr4ApplyRenderAspect();
    Pgr4DumpImage();
    Pgr4FirstLaunchConfig();
    Pgr4MigrateSettings();
    Pgr4SetupProfile(runtime());
  }

  // The garage's arcade cabinet launches gw.xex.
  void OnGuestThreadExit(rex::system::XThread*) override { title_launch::Continue(runtime()); }

  // The toast is an ImGui dialog, and one registered dialog keeps the presenter on the UI
  // thread's on-request path for the whole session instead of presenting from the guest output
  // thread at once.
  std::unique_ptr<rex::ui::AchievementNotificationDialog> CreateAchievementNotificationDialog()
      override {
    return nullptr;
  }
};
