// Local addition: the native renderer's settings in the game's own Options menu, on its GRAPHICS
// page.
//
// PGR4's front end is Lua 5.1 (the executable's "UIScript" resource, zlib-compressed files read
// through sub_82883CC0, which finds a file by name, and sub_82883D30, which reads it like fread
// for lua_load). The game's Options\Graphics.lua only shows a test card; here the game gets our
// own page in its place (upscaler, quality mode, frame generation, definition, MSAA, anisotropy;
// the DLSS preset is pgr4_native_dlss_preset's, E), Lua source generated each time it opens with the current settings, made
// like Options\General.lua (SetupVariableSpinner rows, UIButtons.AddItem/SetSelection).
//
// Saving (A) asks for the screen "Options\PN_<one digit per row>.lua": the find hook decodes the
// digits, queues the settings for the renderer (NativeRenderer::QueueSettings), writes pgr4.toml,
// and gives the game the real Options\MainMenu.lua under that name. The rows' texts are our own
// text ids (kTextBase + n) that the text lookup sub_8289B1D0 resolves to UTF-16BE strings in
// guest memory.

#include <windows.h>

#include "native/overlay.h"
#include "native/render_targets.h"
#include "native/renderer.h"
#include "native/streamline.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/input/input.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <chrono>
#include <atomic>
#include <cctype>
#include <cmath>
#include <limits>
#include <utility>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

REXCVAR_DECLARE(bool, pgr4_native_dlss);
REXCVAR_DECLARE(std::string, pgr4_native_dlss_mode);
REXCVAR_DECLARE(std::string, pgr4_native_dlss_preset);
REXCVAR_DECLARE(bool, pgr4_native_dlss_fg);
REXCVAR_DECLARE(int32_t, pgr4_native_dlss_fg_frames);
REXCVAR_DECLARE(bool, pgr4_native_frame_generation);
REXCVAR_DECLARE(int32_t, pgr4_native_resolution_scale);
REXCVAR_DECLARE(int32_t, pgr4_native_msaa);
REXCVAR_DECLARE(int32_t, pgr4_native_anisotropy);
REXCVAR_DECLARE(bool, fullscreen);
REXCVAR_DECLARE(bool, fullscreen_exclusive);
REXCVAR_DECLARE(std::string, fullscreen_resolution);
extern bool g_pgr4_race_screen;  // pgr4-native draw.cpp
REXCVAR_DECLARE(bool, pgr4_disable_motion_blur);
REXCVAR_DECLARE(int32_t, pgr4_native_present_interval);
REXCVAR_DECLARE(int32_t, pgr4_native_brightness);
REXCVAR_DECLARE(std::string, pgr4_script_dump_dir);
REXCVAR_DECLARE(bool, pgr4_unlock_all);  // src/pgr4_profile.cpp
REXCVAR_DECLARE(int32_t, pgr4_native_fps_limit);
REXCVAR_DECLARE(bool, pgr4_native_gtao);      // pgr4-native scene_effects.cpp
namespace pgr4::native {
REXCVAR_DECLARE(bool, pgr4_native_texture_hd);  // pgr4-native textures.cpp
}
using pgr4::native::FLAGS_pgr4_native_texture_hd_storage_;
REXCVAR_DECLARE(bool, pgr4_native_enhanced_reflections);  // pgr4-native shader_replace.cpp
REXCVAR_DECLARE(int32_t, pgr4_native_details);  // pgr4-native tunables.cpp
REXCVAR_DECLARE(double, pgr4_native_sharpen_dlss);  // pgr4-native upscaler.cpp
REXCVAR_DECLARE(double, pgr4_native_sharpen_fsr);

// pgr4_profile.cpp: Back + Start asked for player 2's sign-out (once), and the sign-out.
bool Pgr4SignOutAsked();
void Pgr4SignOutSecondPlayer();

namespace {

using pgr4::native::NativeRenderer;

// Our text ids: below 2^31 (the scripts pass them as Lua numbers), clear of the ranges the text
// resolvers handle themselves (0x1000-0x7FFE, from 0xFFFFFF9B) and of the .loc files' hashes.
constexpr uint32_t kTextBase = 0x7F5E0000;

const char* const kModes[] = {"auto", "dlaa", "quality", "balanced", "performance",
                              "ultra_performance"};
const int kMsaa[] = {1, 2, 4, 8};
const int kAniso[] = {1, 2, 4, 8, 16};
const int kBrightLevels[] = {60, 70, 80, 90, 100, 110, 120, 130, 140, 150};

// The page's texts, by id offset.
enum Text : uint32_t {
  kTitle,
  kUpscaler,
  kUpscalerOff,
  kUpscalerDlss,
  kUpscalerFsr,
  kQuality,
  kModeAuto,
  kModeDlaa,
  kModeQuality,
  kModeBalanced,
  kModePerformance,
  kModeUltra,
  kFrameGen,
  kOff,
  kOn,
  kScale,
  kScale1,
  kScale2,
  kScale3,
  kScale4,
  kMsaaRow,
  kMsaa1,
  kMsaa2,
  kMsaa4,
  kMsaa8,
  kAnisoRow,
  kAnisoGame,
  kAniso2,
  kAniso4,
  kAniso8,
  kAniso16,
  kDisplayTitle,
  kDisplayHelp,
  kGraphicsHelp,
  kWindowRow,
  kWindowed,
  kBorderless,
  kFullscreen,
  kBlurRow,
  kBlurOff,
  kBlurOn,
  kFpsRow,
  kFpsDiv1,  // the screen's refresh rate (ScreenHz) over 1 to 4, made at run time
  kFpsDiv2,
  kFpsDiv3,
  kFpsDiv4,
  kFpsLimit30,  // the fixed limits (kFpsLimits)
  kFpsLimit60,
  kFpsLimit90,
  kFpsLimit120,
  kFpsLimit144,
  kFpsLimit165,
  kFpsLimit240,
  kResolutionRow,  // the screen resolution row (ScreenModes)
  kRes0,           // its choices, made at run time
  kRes1,
  kRes2,
  kRes3,
  kRes4,
  kRes5,
  kRes6,
  kRes7,
  kRes8,
  kRes9,
  kFgX2,  // DLSS Multi Frame Generation's multipliers (the dlssfg row)
  kFgX3,
  kFgX4,
  kFgX5,
  kFgX6,
  kMsaaTitle,   // PC DISPLAY's MSAA row
  kDlssFgRow,   // PC DISPLAY's frame generation row: DLSS-G's (an NVIDIA GPU that has it)
  kFsrFgRow,    // or FidelityFX's
  kBrightRow,   // PC DISPLAY's brightness (pgr4_native_brightness), kBrightLevels' percents
  kBright60,
  kBright70,
  kBright80,
  kBright90,
  kBright100,
  kBright110,
  kBright120,
  kBright130,
  kBright140,
  kBright150,
  kVersionTag,  // the title screen's bottom left: the build's version and author
  kQuitTitle,    // the title screen's quit popup (StartScreenQuit)
  kQuitMessage,
  kQuitHint,     // its button help: the B button in the game's markup
  kSystemLinkHelp,  // in place of the game's IDS_OVER_A_SYSTEM_LINK_HELP (kIdsSystemLinkHelp)
  kMultiPlayerHelp,  // in place of the game's IDS_MULTI_PLAYER_HELP (kIdsMultiPlayerHelp)
  kSignOutTitle,     // player 2's sign-out popup (GuiBankLostHost's GlobalUpdate, Pgr4SignOutAsked)
  kSignOutMessage,
  kLocalXenonHelp,    // in place of the game's IDS_LOCAL_XENON_HELP (kIdsLocalXenonHelp)
  kLocalXenonHelpPs,  // the same with a PlayStation controller (its buttons' names)
  kGtaoRow,   // PC DISPLAY's GTAO (pgr4_native_gtao), off / on
  kSharpRow,  // PC GRAPHICS's sharpening, with an upscaler (pgr4_native_sharpen_dlss or _fsr)
  kSharp0,
  kSharp20 = kSharp0 + 20,
  kShotHint,  // the photo mode's help: Start takes a screenshot without its interface (PhotoModeShot)
  kHdRow,       // GRAPHICS's HD textures (pgr4_native_texture_hd), off / on
  kReflectRow,  // its enhanced effects (pgr4_native_enhanced_reflections), off / on
  kDetailsRow,  // its enhanced details (pgr4_native_details), kDetailOriginal to kDetailUltra
  kAoRow,       // its ambient occlusion (pgr4_native_gtao), off / GTAO
  kImageRow,    // its IMAGE AND SMOOTHNESS: the upscaler, A opens its page (kImagePage)
  kImageOpen,   // the same with "(A to configure)" (~~{A}~~: the button's icon), GRAPHICS's row
  kRestartTitle,    // the render scale changed: the popup asking to restart the game
  kRestartMessage,
  kWelcomeTitle,    // the first screen (Intro\TitleMovies.lua), in place of IDS_WARNING
  kWelcomeText,     // and of IDS_DISCLAIMER
  kVsyncRow,  // the same word in every language
  kDetailOriginal,  // the enhanced details' choices, the same words too
  kDetailHigh,
  kDetailUltra,
  kTextCount
};
// IDS_OVER_A_SYSTEM_LINK_HELP (PGR4UIStrings.loc), the Multiplayer menu's system link line:
// "linked Xbox 360 consoles", here a local network or a VPN.
constexpr uint32_t kIdsSystemLinkHelp = 0x7699B523;
// IDS_LOCAL_XENON_HELP, the Multiplayer menu's split screen line: how player 2 signs in or out
// (Back + Start, pgr4_profile.cpp).
constexpr uint32_t kIdsLocalXenonHelp = 0xBC1CBF62;
// IDS_MULTI_PLAYER_HELP, the main menu's Multiplayer line: Xbox LIVE no longer named.
constexpr uint32_t kIdsMultiPlayerHelp = 0x00B21147;
// The texts no language changes.
const char16_t* const kTexts[kTextCount] = {
    nullptr, nullptr, nullptr, u"NVIDIA DLSS", u"AMD FSR", nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, u"1X (720P)", u"2X (1440P)",
    u"3X (2160P)", u"4X (2880P)", nullptr, u"1X", u"2X", u"4X", u"8X", nullptr, nullptr, u"2X",
    u"4X", u"8X", u"16X", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, u"30",
    u"60",    u"90",    u"120",   u"144",   u"165",   u"240",   nullptr, nullptr,
    nullptr,  nullptr,  nullptr,  nullptr,  nullptr,  nullptr,  nullptr, nullptr,
    nullptr,  u"X2",    u"X3",    u"X4",    u"X5",    u"X6",    u"MSAA",
    u"DLSS FRAME GENERATION", u"FSR FRAME GENERATION", nullptr, u"60%", u"70%", u"80%", u"90%",
    u"100%", u"110%", u"120%", u"130%", u"140%", u"150%", u"V1.1 (TAZZ)",
};

// The others, in each of the game's languages (the loc folders' order), every glyph one the game's
// own texts of that language use (its fonts hold no others).
enum Lang : uint32_t { kCz, kDe, kEn, kEs, kFr, kHu, kIt, kJa, kKo, kPl, kRu, kSc, kTc, kLangCount };
constexpr Text kLocalized[] = {kTitle,     kUpscaler,     kUpscalerOff, kQuality,         kModeAuto,
                               kModeDlaa,  kModeQuality,  kModeBalanced, kModePerformance, kModeUltra,
                               kFrameGen,  kOff,          kOn,          kScale,           kMsaaRow,
                               kAnisoRow,  kAnisoGame,    kDisplayTitle, kDisplayHelp,     kGraphicsHelp,
                               kWindowRow, kWindowed,     kBorderless,  kFullscreen,
                               kBlurRow,   kBlurOff,      kBlurOn,      kFpsRow,
                               kQuitTitle, kQuitMessage,  kQuitHint,     kSystemLinkHelp,
                               kMultiPlayerHelp, kResolutionRow, kBrightRow,
                               kSignOutTitle, kSignOutMessage, kLocalXenonHelp,
                               kLocalXenonHelpPs, kSharpRow, kShotHint,
                               kHdRow, kReflectRow, kDetailsRow, kAoRow, kImageRow, kImageOpen,
                               kRestartTitle, kRestartMessage, kWelcomeTitle, kWelcomeText};
constexpr size_t kLocalizedCount = std::size(kLocalized);
const char16_t* const kTranslations[kLangCount][kLocalizedCount] = {
    {  // cz
        u"GRAFIKA",
        u"VYHLAZOVÁNÍ HRAN",
        u"MSAA",
        u"KVALITA",
        u"AUTO",
        u"NATIVNÍ (100 %)",
        u"KVALITA",
        u"VYVÁŽENÁ",
        u"VÝKON",
        u"ULTRA VÝKON",
        u"GENEROVÁNÍ SNÍMKŮ",
        u"VYPNUTO",
        u"ZAPNUTO",
        u"ŠKÁLOVÁNÍ ROZLIŠENÍ",
        u"MSAA",
        u"ANIZOTROPIE",
        u"HRA",
        u"ZOBRAZENÍ",
        u"Režim okna a rozlišení vykreslování",
        u"Obraz a plynulost, textury HD, efekty, anizotropie a rozmazání pohybem",
        u"REŽIM OKNA",
        u"V OKNĚ",
        u"BEZ OKRAJŮ",
        u"CELÁ OBRAZOVKA",
        u"ROZMAZÁNÍ POHYBEM",
        u"VYPNUTO",
        u"ZAPNUTO",
        u"LIMIT SNÍMKŮ/S",
        u"UKONČIT HRU",
        u"Opravdu chcete ukončit hru?",
        u"UKONČIT ~~{B}~~",
        u"Hrajte proti ostatním v místní síti (LAN) nebo přes VPN",
        u"Hrajte s ostatními na rozdělené obrazovce nebo v síti (LAN nebo VPN)",
        u"ROZLIŠENÍ OBRAZOVKY",
        u"JAS",
        u"HRÁČ 2",
        u"Odhlásit hráče 2 a vrátit se k jednomu hráči?",
        u"Hráč 2: BACK + START pro přihlášení nebo odhlášení",
        u"Hráč 2: SHARE + OPTIONS pro přihlášení nebo odhlášení",
        u"OSTROST",
        u"SNÍMEK OBRAZOVKY ~~{S}~~",
        u"TEXTURY HD",
        u"VYLEPŠENÉ EFEKTY",
        u"VYLEPŠENÉ DETAILY",
        u"OKOLNÍ OKLUZE",
        u"OBRAZ A PLYNULOST",
        u"OBRAZ A PLYNULOST (~~{A}~~ PRO NASTAVENÍ )",
        u"RESTARTOVAT HRU",
        u"Toto nastavení vyžaduje restart hry. Probíhající závod bude ztracen. Restartovat nyní?",
        u"VÍTEJTE",
        u"Project Gotham Racing 4 edice 2026 je vylepšená verze hry s novými vizuálními efekty a novými grafickými možnostmi. Tato verze by nevznikla bez práce těchto autorů: ~~/n~~Beatrixzy ~~/n~~github.com/beatrixzy/PGR4-Recomp ~~/n~~~~/n~~Shaunsingh ~~/n~~github.com/shaunsingh/pgr4-native ~~/n~~~~/n~~Velké díky jim oběma.",
    },
    {  // de
        u"GRAFIK",
        u"KANTENGLÄTTUNG",
        u"MSAA",
        u"QUALITÄT",
        u"AUTO",
        u"NATIV (100 %)",
        u"QUALITÄT",
        u"AUSGEWOGEN",
        u"LEISTUNG",
        u"ULTRA-LEISTUNG",
        u"FRAME-GENERIERUNG",
        u"AUS",
        u"EIN",
        u"AUFLÖSUNGSSKALA",
        u"MSAA",
        u"ANISOTROPIE",
        u"SPIEL",
        u"ANZEIGE",
        u"Fenstermodus und Render-Auflösung",
        u"Bild und Flüssigkeit, HD-Texturen, Effekte, Anisotropie und Bewegungsunschärfe",
        u"FENSTERMODUS",
        u"FENSTER",
        u"RAHMENLOS",
        u"VOLLBILD",
        u"BEWEGUNGSUNSCHÄRFE",
        u"AUS",
        u"EIN",
        u"BILDRATENLIMIT",
        u"SPIEL BEENDEN",
        u"Möchtest du das Spiel wirklich beenden?",
        u"BEENDEN ~~{B}~~",
        u"Spiele gegen andere über ein lokales Netzwerk (LAN) oder ein VPN",
        u"Spiele mit anderen im geteilten Bildschirm oder über ein Netzwerk (LAN oder VPN)",
        u"BILDSCHIRMAUFLÖSUNG",
        u"HELLIGKEIT",
        u"SPIELER 2",
        u"Spieler 2 abmelden und zu einem Spieler zurückkehren?",
        u"Spieler 2: BACK + START zum An- oder Abmelden",
        u"Spieler 2: SHARE + OPTIONS zum An- oder Abmelden",
        u"SCHÄRFE",
        u"SCREENSHOT ~~{S}~~",
        u"HD-TEXTUREN",
        u"VERBESSERTE EFFEKTE",
        u"VERBESSERTE DETAILS",
        u"UMGEBUNGSVERDECKUNG",
        u"BILD UND FLÜSSIGKEIT",
        u"BILD UND FLÜSSIGKEIT (~~{A}~~ ZUM EINSTELLEN )",
        u"SPIEL NEU STARTEN",
        u"Diese Einstellung erfordert einen Neustart des Spiels. Ein laufendes Rennen geht verloren. Jetzt neu starten?",
        u"WILLKOMMEN",
        u"Project Gotham Racing 4 Edition 2026 ist eine verbesserte Version des Spiels mit neuen visuellen Effekten und neuen Grafikoptionen. Diese Version gäbe es nicht ohne die Arbeit von: ~~/n~~Beatrixzy ~~/n~~github.com/beatrixzy/PGR4-Recomp ~~/n~~~~/n~~Shaunsingh ~~/n~~github.com/shaunsingh/pgr4-native ~~/n~~~~/n~~Vielen Dank an die beiden.",
    },
    {  // en
        u"GRAPHICS",
        u"ANTI-ALIASING",
        u"MSAA",
        u"QUALITY",
        u"AUTO",
        u"NATIVE (100 %)",
        u"QUALITY",
        u"BALANCED",
        u"PERFORMANCE",
        u"ULTRA PERFORMANCE",
        u"FRAME GENERATION",
        u"OFF",
        u"ON",
        u"RESOLUTION SCALE",
        u"MSAA",
        u"ANISOTROPY",
        u"GAME",
        u"DISPLAY",
        u"Window mode, resolution, frame rate limit and brightness",
        u"Image and smoothness, HD textures, effects, anisotropy and motion blur",
        u"WINDOW MODE",
        u"WINDOWED",
        u"BORDERLESS",
        u"FULLSCREEN",
        u"MOTION BLUR",
        u"OFF",
        u"ON",
        u"FRAME RATE LIMIT",
        u"QUIT GAME",
        u"Are you sure you want to quit the game?",
        u"QUIT ~~{B}~~",
        u"Play against others over a local network (LAN) or a VPN",
        u"Play with others in split screen or over a network (LAN or VPN)",
        u"SCREEN RESOLUTION",
        u"BRIGHTNESS",
        u"PLAYER 2",
        u"Sign player 2 out and go back to one player?",
        u"Player 2: BACK + START to sign in or sign out",
        u"Player 2: SHARE + OPTIONS to sign in or sign out",
        u"SHARPNESS",
        u"SCREENSHOT ~~{S}~~",
        u"HD TEXTURES",
        u"ENHANCED EFFECTS",
        u"ENHANCED DETAILS",
        u"AMBIENT OCCLUSION",
        u"IMAGE AND SMOOTHNESS",
        u"IMAGE AND SMOOTHNESS (~~{A}~~ TO CONFIGURE )",
        u"RESTART GAME",
        u"This setting requires restarting the game. A race in progress will be lost. Restart now?",
        u"WELCOME",
        u"Project Gotham Racing 4 Edition 2026 is an enhanced version of the game with new visual effects and new graphics options. This version would not exist without the work of: ~~/n~~Beatrixzy ~~/n~~github.com/beatrixzy/PGR4-Recomp ~~/n~~~~/n~~Shaunsingh ~~/n~~github.com/shaunsingh/pgr4-native ~~/n~~~~/n~~Many thanks to them.",
    },
    {  // es
        u"GRÁFICOS",
        u"ANTIALIASING",
        u"MSAA",
        u"CALIDAD",
        u"AUTO",
        u"NATIVA (100 %)",
        u"CALIDAD",
        u"EQUILIBRADO",
        u"RENDIMIENTO",
        u"ULTRA RENDIMIENTO",
        u"GENERACIÓN DE FOTOGRAMAS",
        u"DESACTIVADA",
        u"ACTIVADA",
        u"ESCALA DE RESOLUCIÓN",
        u"MSAA",
        u"ANISOTROPÍA",
        u"JUEGO",
        u"PANTALLA",
        u"Modo de ventana y resolución de renderizado",
        u"Imagen y fluidez, texturas HD, efectos, anisotropía y desenfoque de movimiento",
        u"MODO DE VENTANA",
        u"VENTANA",
        u"SIN BORDES",
        u"PANTALLA COMPLETA",
        u"DESENFOQUE DE MOVIMIENTO",
        u"DESACTIVADO",
        u"ACTIVADO",
        u"LÍMITE DE FPS",
        u"SALIR DEL JUEGO",
        u"¿Seguro que quieres salir del juego?",
        u"SALIR ~~{B}~~",
        u"Juega contra otros en una red local (LAN) o mediante una VPN",
        u"Juega con otros en pantalla dividida o en red (LAN o VPN)",
        u"RESOLUCIÓN DE PANTALLA",
        u"BRILLO",
        u"JUGADOR 2",
        u"¿Desconectar al jugador 2 y volver a un solo jugador?",
        u"Jugador 2: BACK + START para conectarse o desconectarse",
        u"Jugador 2: SHARE + OPTIONS para conectarse o desconectarse",
        u"NITIDEZ",
        u"CAPTURA DE PANTALLA ~~{S}~~",
        u"TEXTURAS HD",
        u"EFECTOS MEJORADOS",
        u"DETALLES MEJORADOS",
        u"OCLUSIÓN AMBIENTAL",
        u"IMAGEN Y FLUIDEZ",
        u"IMAGEN Y FLUIDEZ (~~{A}~~ PARA CONFIGURAR )",
        u"REINICIAR EL JUEGO",
        u"Este ajuste requiere reiniciar el juego. Se perderá la carrera en curso. ¿Reiniciar ahora?",
        u"BIENVENIDO",
        u"Project Gotham Racing 4 Edición 2026 es una versión mejorada del juego con nuevos efectos visuales y nuevas opciones gráficas. Esta versión no existiría sin el trabajo de: ~~/n~~Beatrixzy ~~/n~~github.com/beatrixzy/PGR4-Recomp ~~/n~~~~/n~~Shaunsingh ~~/n~~github.com/shaunsingh/pgr4-native ~~/n~~~~/n~~Muchas gracias a ambos.",
    },
    {  // fr
        u"GRAPHISMES",
        u"ANTI-ALIASING",
        u"MSAA",
        u"QUALITÉ",
        u"AUTO",
        u"NATIVE (100 %)",
        u"QUALITÉ",
        u"ÉQUILIBRÉ",
        u"PERFORMANCE",
        u"ULTRA PERFORMANCE",
        u"GÉNÉRATION D'IMAGES",
        u"DÉSACTIVÉE",
        u"ACTIVÉE",
        u"ÉCHELLE DE RÉSOLUTION",
        u"MSAA",
        u"ANISOTROPIE",
        u"JEU",
        u"AFFICHAGE",
        u"Mode d'affichage, résolution, limite d'images/s et luminosité",
        u"Image et fluidité, textures HD, effets, anisotropie et flou de mouvement",
        u"MODE D'AFFICHAGE",
        u"FENÊTRÉ",
        u"SANS BORDURE",
        u"PLEIN ÉCRAN",
        u"FLOU DE MOUVEMENT",
        u"DÉSACTIVÉ",
        u"ACTIVÉ",
        u"LIMITE D'IMAGES/S",
        u"QUITTER LE JEU",
        u"Voulez-vous vraiment quitter le jeu ?",
        u"QUITTER ~~{B}~~",
        u"Jouez contre d'autres joueurs en réseau local (LAN) ou par VPN",
        u"Jouez avec d'autres joueurs en écran partagé ou en réseau (LAN ou VPN)",
        u"RÉSOLUTION D'ÉCRAN",
        u"LUMINOSITÉ",
        u"JOUEUR 2",
        u"Déconnecter le joueur 2 et revenir à un seul joueur ?",
        u"Joueur 2 : BACK + START pour se connecter ou se déconnecter",
        u"Joueur 2 : SHARE + OPTIONS pour se connecter ou se déconnecter",
        u"NETTETÉ",
        u"CAPTURE D'ÉCRAN ~~{S}~~",
        u"TEXTURES HD",
        u"EFFETS AMÉLIORÉS",
        u"DÉTAILS AMÉLIORÉS",
        u"OCCLUSION AMBIANTE",
        u"IMAGE ET FLUIDITÉ",
        u"IMAGE ET FLUIDITÉ (~~{A}~~ POUR CONFIGURER )",
        u"REDÉMARRER LE JEU",
        u"Ce réglage nécessite de redémarrer le jeu. Une course en cours sera perdue. Redémarrer maintenant ?",
        u"BIENVENUE",
        u"Project Gotham Racing 4 édition 2026 est une version améliorée du jeu proposant de nouveaux effets visuels et de nouvelles options graphiques. Cette version n'existerait pas sans le travail effectué par : ~~/n~~Beatrixzy ~~/n~~github.com/beatrixzy/PGR4-Recomp ~~/n~~~~/n~~Shaunsingh ~~/n~~github.com/shaunsingh/pgr4-native ~~/n~~~~/n~~Un grand merci à eux.",
    },
    {  // hu
        u"GRAFIKA",
        u"ÉLSIMÍTÁS",
        u"MSAA",
        u"MINŐSÉG",
        u"AUTOMATIKUS",
        u"NATÍV (100 %)",
        u"MINŐSÉG",
        u"KIEGYENSÚLYOZOTT",
        u"TELJESÍTMÉNY",
        u"ULTRA TELJESÍTMÉNY",
        u"KÉPKOCKA-GENERÁLÁS",
        u"KI",
        u"BE",
        u"FELBONTÁS SKÁLÁZÁSA",
        u"MSAA",
        u"ANIZOTRÓPIA",
        u"JÁTÉK",
        u"KIJELZŐ",
        u"Ablakmód és megjelenítési felbontás",
        u"Kép és folyamatosság, HD textúrák, effektek, környezeti árnyékolás, anizotrópia és mozgási elmosódás",
        u"ABLAKMÓD",
        u"ABLAKOS",
        u"KERET NÉLKÜL",
        u"TELJES KÉPERNYŐ",
        u"MOZGÁSI ELMOSÓDÁS",
        u"KI",
        u"BE",
        u"FPS-KORLÁT",
        u"KILÉPÉS A JÁTÉKBÓL",
        u"Biztosan ki szeretnél lépni a játékból?",
        u"KILÉPÉS ~~{B}~~",
        u"Játssz mások ellen helyi hálózaton (LAN) vagy VPN-en keresztül",
        u"Játssz másokkal osztott képernyőn vagy hálózaton (LAN vagy VPN)",
        u"KÉPERNYŐFELBONTÁS",
        u"FÉNYERŐ",
        u"2. JÁTÉKOS",
        u"Kijelentkezteted a 2. játékost, és visszatérsz egy játékosra?",
        u"2. játékos: BACK + START a be- vagy kijelentkezéshez",
        u"2. játékos: SHARE + OPTIONS a be- vagy kijelentkezéshez",
        u"ÉLESSÉG",
        u"KÉPERNYŐKÉP ~~{S}~~",
        u"HD TEXTÚRÁK",
        u"JAVÍTOTT EFFEKTEK",
        u"JAVÍTOTT RÉSZLETEK",
        u"KÖRNYEZETI ÁRNYÉKOLÁS",
        u"KÉP ÉS FOLYAMATOSSÁG",
        u"KÉP ÉS FOLYAMATOSSÁG (~~{A}~~: BEÁLLÍTÁS )",
        u"JÁTÉK ÚJRAINDÍTÁSA",
        u"Ehhez a beállításhoz újra kell indítani a játékot. A folyamatban lévő verseny elvész. Újraindítás most?",
        u"ÜDVÖZÖLJÜK",
        u"A Project Gotham Racing 4 2026-os kiadása a játék továbbfejlesztett változata új vizuális effektekkel és új grafikai beállításokkal. Ez a változat nem jöhetett volna létre a következők munkája nélkül: ~~/n~~Beatrixzy ~~/n~~github.com/beatrixzy/PGR4-Recomp ~~/n~~~~/n~~Shaunsingh ~~/n~~github.com/shaunsingh/pgr4-native ~~/n~~~~/n~~Köszönjük nekik.",
    },
    {  // it
        u"GRAFICA",
        u"ANTIALIASING",
        u"MSAA",
        u"QUALITÀ",
        u"AUTO",
        u"NATIVA (100 %)",
        u"QUALITÀ",
        u"BILANCIATA",
        u"PRESTAZIONI",
        u"ULTRA PRESTAZIONI",
        u"GENERAZIONE FOTOGRAMMI",
        u"DISATTIVATA",
        u"ATTIVATA",
        u"SCALA DI RISOLUZIONE",
        u"MSAA",
        u"ANISOTROPIA",
        u"GIOCO",
        u"SCHERMO",
        u"Modalità finestra e risoluzione di rendering",
        u"Immagine e fluidità, texture HD, effetti, anisotropia e sfocatura di movimento",
        u"MODALITÀ FINESTRA",
        u"FINESTRA",
        u"SENZA BORDI",
        u"SCHERMO INTERO",
        u"SFOCATURA DI MOVIMENTO",
        u"DISATTIVATA",
        u"ATTIVATA",
        u"LIMITE FPS",
        u"ESCI DAL GIOCO",
        u"Vuoi davvero uscire dal gioco?",
        u"ESCI ~~{B}~~",
        u"Sfida altri giocatori su una rete locale (LAN) o tramite VPN",
        u"Gioca con altri in schermo condiviso o in rete (LAN o VPN)",
        u"RISOLUZIONE SCHERMO",
        u"LUMINOSITÀ",
        u"GIOCATORE 2",
        u"Disconnettere il giocatore 2 e tornare a un solo giocatore?",
        u"Giocatore 2: BACK + START per accedere o uscire",
        u"Giocatore 2: SHARE + OPTIONS per accedere o uscire",
        u"NITIDEZZA",
        u"SCREENSHOT ~~{S}~~",
        u"TEXTURE HD",
        u"EFFETTI MIGLIORATI",
        u"DETTAGLI MIGLIORATI",
        u"OCCLUSIONE AMBIENTALE",
        u"IMMAGINE E FLUIDITÀ",
        u"IMMAGINE E FLUIDITÀ (~~{A}~~ PER CONFIGURARE )",
        u"RIAVVIA IL GIOCO",
        u"Questa impostazione richiede il riavvio del gioco. Una gara in corso andrà persa. Riavviare ora?",
        u"BENVENUTO",
        u"Project Gotham Racing 4 Edizione 2026 è una versione migliorata del gioco con nuovi effetti visivi e nuove opzioni grafiche. Questa versione non esisterebbe senza il lavoro di: ~~/n~~Beatrixzy ~~/n~~github.com/beatrixzy/PGR4-Recomp ~~/n~~~~/n~~Shaunsingh ~~/n~~github.com/shaunsingh/pgr4-native ~~/n~~~~/n~~Un grande grazie a loro.",
    },
    {  // ja
        u"グラフィック",
        u"アンチエイリアス",
        u"MSAA",
        u"品質",
        u"自動",
        u"ネイティブ (100%)",
        u"品質",
        u"バランス",
        u"パフォーマンス",
        u"ウルトラパフォーマンス",
        u"フレーム生成",
        u"オフ",
        u"オン",
        u"描画スケール",
        u"MSAA",
        u"異方性フィルタリング",
        u"ゲーム",
        u"ディスプレイ",
        u"ウィンドウモードと描画サイズ",
        u"画質となめらかさ、HDテクスチャ、エフェクト、異方性フィルタリング、モーションブラー",
        u"ウィンドウモード",
        u"ウィンドウ",
        u"ボーダーレス",
        u"フルスクリーン",
        u"モーションブラー",
        u"オフ",
        u"オン",
        u"フレームレート上限",
        u"ゲーム終了",
        u"ゲームを終了しますか?",
        u"終了 ~~{B}~~",
        u"ローカルネットワーク (LAN) または VPN で他のプレイヤーと対戦",
        u"画面分割またはネットワーク (LAN または VPN) で他のプレイヤーと対戦",
        u"ディスプレイサイズ",
        u"GAMMA",
        u"PLAYER 2",
        u"Sign player 2 out and go back to one player?",
        u"プレイヤー2: BACK + START でサインイン/サインアウト",
        u"プレイヤー2: SHARE + OPTIONS でサインイン/サインアウト",
        u"シャープネス",
        u"スクリーンショット ~~{S}~~",
        u"HDテクスチャ",
        u"高品質なエフェクト",
        u"高品質なディテール",
        u"アンビエントオクルージョン",
        u"画質となめらかさ",
        u"画質となめらかさ (~~{A}~~で設定 )",
        u"ゲームを再起動",
        u"この設定にはゲームの再起動が必要です。進行中のレースは失われます。再起動しますか?",
        u"ようこそ",
        u"Project Gotham Racing 4 エディション 2026 は、新しい視覚効果とグラフィック設定を加えた強化バージョンです。このバージョンは次の方々の作業なしには存在しませんでした。 ~~/n~~Beatrixzy ~~/n~~github.com/beatrixzy/PGR4-Recomp ~~/n~~~~/n~~Shaunsingh ~~/n~~github.com/shaunsingh/pgr4-native ~~/n~~~~/n~~ありがとうございます。",
    },
    {  // ko
        u"그래픽",
        u"안티에일리어싱",
        u"MSAA",
        u"품질",
        u"자동",
        u"네이티브 (100%)",
        u"품질",
        u"균형",
        u"성능",
        u"울트라 성능",
        u"프레임 생성",
        u"끄기",
        u"켜기",
        u"해상도 스케일",
        u"MSAA",
        u"비등방성 필터링",
        u"게임",
        u"디스플레이",
        u"창 모드와 해상도",
        u"화질과 부드러움, HD 질감, 효과, 주변광 음영, 비등방성 필터링, 모션 블러",
        u"창 모드",
        u"창",
        u"테두리 없음",
        u"전체 화면",
        u"모션 블러",
        u"끄기",
        u"켜기",
        u"프레임 제한",
        u"게임 종료",
        u"게임을 종료하시겠습니까?",
        u"종료 ~~{B}~~",
        u"로컬 네트워크(LAN) 또는 VPN으로 다른 플레이어와 대전합니다.",
        u"화면 분할 또는 네트워크(LAN 또는 VPN)로 다른 플레이어와 함께 플레이합니다.",
        u"화면 해상도",
        u"GAMMA",
        u"PLAYER 2",
        u"Sign player 2 out and go back to one player?",
        u"플레이어 2: BACK + START로 로그인/로그아웃",
        u"플레이어 2: SHARE + OPTIONS로 로그인/로그아웃",
        u"선명도",
        u"화면 저장 ~~{S}~~",
        u"HD 질감",
        u"향상된 효과",
        u"향상된 디테일",
        u"주변광 음영",
        u"화질과 부드러움",
        u"화질과 부드러움 (~~{A}~~로 설정 )",
        u"게임 다시 시작",
        u"이 설정을 적용하려면 게임을 다시 시작해야 합니다. 진행 중인 레이스는 사라집니다. 지금 다시 시작하시겠습니까?",
        u"환영합니다",
        u"Project Gotham Racing 4 에디션 2026은 새로운 시각 효과와 그래픽 옵션을 더한 향상된 버전입니다. 이 버전은 다음 분들의 작업이 없었다면 존재하지 않았을 것입니다. ~~/n~~Beatrixzy ~~/n~~github.com/beatrixzy/PGR4-Recomp ~~/n~~~~/n~~Shaunsingh ~~/n~~github.com/shaunsingh/pgr4-native ~~/n~~~~/n~~두 분께 감사드립니다.",
    },
    {  // pl
        u"GRAFIKA",
        u"WYGŁADZANIE KRAWĘDZI",
        u"MSAA",
        u"JAKOŚĆ",
        u"AUTO",
        u"NATYWNA (100 %)",
        u"JAKOŚĆ",
        u"ZRÓWNOWAŻONY",
        u"WYDAJNOŚĆ",
        u"ULTRA WYDAJNOŚĆ",
        u"GENEROWANIE KLATEK",
        u"WYŁ.",
        u"WŁ.",
        u"SKALA ROZDZIELCZOŚCI",
        u"MSAA",
        u"ANIZOTROPIA",
        u"GRA",
        u"EKRAN",
        u"Tryb okna i rozdzielczość renderowania",
        u"Obraz i płynność, tekstury HD, efekty, anizotropia i rozmycie ruchu",
        u"TRYB OKNA",
        u"OKNO",
        u"BEZ RAMEK",
        u"PEŁNY EKRAN",
        u"ROZMYCIE RUCHU",
        u"WYŁ.",
        u"WŁ.",
        u"LIMIT KLATEK/S",
        u"WYJDŹ Z GRY",
        u"Czy na pewno chcesz wyjść z gry?",
        u"WYJDŹ ~~{B}~~",
        u"Graj z innymi w sieci lokalnej (LAN) lub przez VPN",
        u"Graj z innymi na podzielonym ekranie lub w sieci (LAN lub VPN)",
        u"ROZDZIELCZOŚĆ EKRANU",
        u"JASNOŚĆ",
        u"GRACZ 2",
        u"Wylogować gracza 2 i wrócić do jednego gracza?",
        u"Gracz 2: BACK + START, aby się zalogować lub wylogować",
        u"Gracz 2: SHARE + OPTIONS, aby się zalogować lub wylogować",
        u"OSTROŚĆ",
        u"ZRZUT EKRANU ~~{S}~~",
        u"TEKSTURY HD",
        u"ULEPSZONE EFEKTY",
        u"ULEPSZONE DETALE",
        u"OKLUZJA OTOCZENIA",
        u"OBRAZ I PŁYNNOŚĆ",
        u"OBRAZ I PŁYNNOŚĆ (~~{A}~~, ABY USTAWIĆ )",
        u"URUCHOM GRĘ PONOWNIE",
        u"To ustawienie wymaga ponownego uruchomienia gry. Trwający wyścig zostanie utracony. Uruchomić ponownie teraz?",
        u"WITAJ",
        u"Project Gotham Racing 4 Edycja 2026 to ulepszona wersja gry z nowymi efektami wizualnymi i nowymi opcjami graficznymi. Ta wersja nie powstałaby bez pracy: ~~/n~~Beatrixzy ~~/n~~github.com/beatrixzy/PGR4-Recomp ~~/n~~~~/n~~Shaunsingh ~~/n~~github.com/shaunsingh/pgr4-native ~~/n~~~~/n~~Wielkie dzięki dla nich.",
    },
    {  // ru
        u"ГРАФИКА",
        u"СГЛАЖИВАНИЕ",
        u"MSAA",
        u"КАЧЕСТВО",
        u"АВТО",
        u"НАТИВНОЕ (100 %)",
        u"КАЧЕСТВО",
        u"БАЛАНС",
        u"ПРОИЗВОДИТЕЛЬНОСТЬ",
        u"УЛЬТРА ПРОИЗВОДИТЕЛЬНОСТЬ",
        u"ГЕНЕРАЦИЯ КАДРОВ",
        u"ВЫКЛ.",
        u"ВКЛ.",
        u"МАСШТАБ РАЗРЕШЕНИЯ",
        u"MSAA",
        u"АНИЗОТРОПИЯ",
        u"ИГРА",
        u"ЭКРАН",
        u"Режим окна и разрешение рендера",
        u"Изображение и плавность, HD-текстуры, эффекты, анизотропия и размытие в движении",
        u"РЕЖИМ ОКНА",
        u"В ОКНЕ",
        u"БЕЗ РАМКИ",
        u"ПОЛНЫЙ ЭКРАН",
        u"РАЗМЫТИЕ В ДВИЖЕНИИ",
        u"ВЫКЛ.",
        u"ВКЛ.",
        u"ОГРАНИЧЕНИЕ FPS",
        u"ВЫХОД ИЗ ИГРЫ",
        u"Вы действительно хотите выйти из игры?",
        u"ВЫХОД ~~{B}~~",
        u"Играйте с другими по локальной сети (LAN) или через VPN",
        u"Играйте с другими на разделенном экране или по сети (LAN или VPN)",
        u"РАЗРЕШЕНИЕ ЭКРАНА",
        u"ЯРКОСТЬ",
        u"ИГРОК 2",
        u"Выйти из профиля игрока 2 и вернуться к одному игроку?",
        u"Игрок 2: BACK + START — вход или выход из профиля",
        u"Игрок 2: SHARE + OPTIONS — вход или выход из профиля",
        u"РЕЗКОСТЬ",
        u"СНИМОК ЭКРАНА ~~{S}~~",
        u"HD-ТЕКСТУРЫ",
        u"УЛУЧШЕННЫЕ ЭФФЕКТЫ",
        u"УЛУЧШЕННЫЕ ДЕТАЛИ",
        u"ФОНОВОЕ ЗАТЕНЕНИЕ",
        u"ИЗОБРАЖЕНИЕ И ПЛАВНОСТЬ",
        u"ИЗОБРАЖЕНИЕ И ПЛАВНОСТЬ (~~{A}~~: НАСТРОЙКА )",
        u"ПЕРЕЗАПУСК ИГРЫ",
        u"Для этого параметра нужно перезапустить игру. Текущая гонка будет потеряна. Перезапустить сейчас?",
        u"ДОБРО ПОЖАЛОВАТЬ",
        u"Project Gotham Racing 4 Издание 2026 — это улучшенная версия игры с новыми визуальными эффектами и новыми настройками графики. Эта версия не появилась бы без работы: ~~/n~~Beatrixzy ~~/n~~github.com/beatrixzy/PGR4-Recomp ~~/n~~~~/n~~Shaunsingh ~~/n~~github.com/shaunsingh/pgr4-native ~~/n~~~~/n~~Большое им спасибо.",
    },
    {  // sc
        u"图形",
        u"ANTI-ALIASING",
        u"MSAA",
        u"质量",
        u"自动",
        u"原生（100%）",
        u"质量",
        u"平衡",
        u"性能",
        u"超级性能",
        u"图像生成",
        u"关",
        u"开",
        u"图像尺寸",
        u"MSAA",
        u"各向异性过滤",
        u"游戏",
        u"显示",
        u"窗口模式与图像尺寸",
        u"画质与速度、高清材质、效果、各向异性过滤与动态模糊",
        u"窗口模式",
        u"窗口",
        u"无边框",
        u"全屏",
        u"动态模糊",
        u"关",
        u"开",
        u"每秒图像上限",
        u"退出游戏",
        u"确定要退出游戏吗?",
        u"退出 ~~{B}~~",
        u"通过局域网 (LAN) 或 VPN 与其他玩家对战",
        u"通过分屏或网络 (LAN 或 VPN) 与其他玩家对战",
        u"显示尺寸",
        u"GAMMA",
        u"PLAYER 2",
        u"Sign player 2 out and go back to one player?",
        u"玩家 2：按 BACK + START 登录或注销",
        u"玩家 2：按 SHARE + OPTIONS 登录或注销",
        u"锐化",
        u"截图 ~~{S}~~",
        u"高清材质",
        u"增强效果",
        u"增强细节",
        u"环境光阴影",
        u"画质与速度",
        u"画质与速度 (~~{A}~~ 设置 )",
        u"重新启动游戏",
        u"此设置需要重新启动游戏。正在进行的比赛将会丢失。现在重新启动吗?",
        u"欢迎",
        u"Project Gotham Racing 4 2026 版是游戏的增强版本,加入了新的视觉效果和新的图形选项。没有以下人员的工作,就没有这个版本: ~~/n~~Beatrixzy ~~/n~~github.com/beatrixzy/PGR4-Recomp ~~/n~~~~/n~~Shaunsingh ~~/n~~github.com/shaunsingh/pgr4-native ~~/n~~~~/n~~非常感谢他们。",
    },
    {  // tc
        u"視訊",
        u"ANTI-ALIASING",
        u"MSAA",
        u"品質",
        u"自動",
        u"原生 (100%)",
        u"品質",
        u"平衡",
        u"效能",
        u"極致效能",
        u"畫格生成",
        u"關閉",
        u"開啟",
        u"影像大小",
        u"MSAA",
        u"非等向性過濾",
        u"遊戲",
        u"顯示",
        u"視窗模式與影像大小",
        u"畫質與流暢度、高畫質材質、效果、非等向性過濾與動態模糊",
        u"視窗模式",
        u"視窗",
        u"無邊視窗",
        u"全螢幕",
        u"動態模糊",
        u"關閉",
        u"開啟",
        u"畫格率上限",
        u"離開遊戲",
        u"確定要離開遊戲嗎？",
        u"離開 ~~{B}~~",
        u"透過區域網路 (LAN) 或 VPN 與其他車手競速",
        u"透過分割畫面或網路 (LAN 或 VPN) 與其他車手競速",
        u"顯示大小",
        u"GAMMA",
        u"PLAYER 2",
        u"Sign player 2 out and go back to one player?",
        u"玩家 2：按 BACK + START 登入或登出",
        u"玩家 2：按 SHARE + OPTIONS 登入或登出",
        u"銳化",
        u"截圖 ~~{S}~~",
        u"高畫質材質",
        u"增強效果",
        u"增強細節",
        u"環境遮光",
        u"畫質與流暢度",
        u"畫質與流暢度 (~~{A}~~ 設定 )",
        u"重新啟動遊戲",
        u"此設定需要重新啟動遊戲。進行中的比賽將會遺失。現在重新啟動嗎?",
        u"歡迎",
        u"Project Gotham Racing 4 2026 版是遊戲的增強版本,加入了新的視覺效果和新的圖形選項。沒有以下人員的工作,就沒有這個版本: ~~/n~~Beatrixzy ~~/n~~github.com/beatrixzy/PGR4-Recomp ~~/n~~~~/n~~Shaunsingh ~~/n~~github.com/shaunsingh/pgr4-native ~~/n~~~~/n~~非常感謝他們。",
    },
};

// IDS_GRAPHICS in each language, then IDS_OPTIONS for the three that share GRAFIKA: how the text
// lookup tells the game's language.
constexpr uint32_t kIdsGraphicsText = 0x249A8385;
constexpr uint32_t kIdsOptionsText = 0xE34ABAC1;
const char16_t* const kGraphicsNames[kLangCount] = {
    u"GRAFIKA", u"GRAFIK",   u"GRAPHICS", u"GRÁFICOS", u"GRAPHISMES", u"GRAFIKA", u"GRAFICA",
    u"グラフィック", u"그래픽", u"GRAFIKA", u"ГРАФИКА", u"图形", u"視訊",
};
const char16_t* const kOptionsNames[kLangCount] = {
    u"MOŽNOSTI", nullptr, nullptr, nullptr, nullptr, u"OPCIÓK", nullptr,
    nullptr,     nullptr, u"OPCJE", nullptr, nullptr, nullptr,
};

// The refresh rate of the screen showing the game (the foreground window's monitor's current
// mode), the frame rate row's last choice; 60 if unknown.
int ScreenHz() {
  HMONITOR monitor = MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY);
  MONITORINFOEXW info = {};
  info.cbSize = sizeof(info);
  DEVMODEW mode = {};
  mode.dmSize = sizeof(mode);
  if (!GetMonitorInfoW(monitor, &info) ||
      !EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) ||
      mode.dmDisplayFrequency < 24) {
    return 60;
  }
  const int hz = int(mode.dmDisplayFrequency);
  constexpr int kRates[] = {60, 72, 75, 90, 100, 120, 144, 165, 170, 175, 180, 200, 240, 280, 360};
  for (int rate : kRates)
    if (hz + 1 == rate) return rate;
  return hz;
}

// The frame rate row's choices, the slowest first. The screen's rate over 1 to 4 of its refreshes
// per frame (at least 30 FPS; pgr4_native_present_interval is that count): exact fractions of the
// refresh, each frame shown for as many refreshes, smooth on any screen, with or without variable
// refresh. And fixed limits below the screen's rate, by a sleep (pgr4_native_fps_limit, the
// interval 1), but those 5 % or less from a fraction: without variable refresh, a rate between two
// fractions (120 on a 165 Hz screen) shows frames for 1 or 2 refreshes in turn, a steady judder.
// At most 10 (the page saves each row's choice as one digit).
struct FpsChoice {
  int interval, limit;  // limit 0: the screen's rate over interval
  bool operator==(const FpsChoice&) const = default;
};
constexpr Text kFpsTexts[] = {kFpsDiv1, kFpsDiv2, kFpsDiv3, kFpsDiv4};
constexpr int kFpsLimits[] = {30, 60, 90, 120, 144, 165, 240};
std::atomic<int> g_screen_hz{60};
// The screen resolution row's choices: the modes of the game's monitor (EnumDisplaySettings), at
// least 1280x720 and wider than high, the smallest first; at most 10, the largest kept (one digit
// per row), the desktop's among them. Exclusive fullscreen's mode (the SDK's
// fullscreen_resolution, empty for the desktop's); borderless is the desktop's own.
struct ScreenMode {
  int width = 0, height = 0;
  bool operator==(const ScreenMode&) const = default;
};
std::mutex g_modes_mutex;
std::vector<ScreenMode> g_modes;  // ScreenModes' last, for its texts
bool GameMonitor(MONITORINFOEXW& info) {
  HMONITOR monitor = MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY);
  info = {};
  info.cbSize = sizeof(info);
  return GetMonitorInfoW(monitor, &info) != 0;
}
// The desktop's mode (the registry's: the current one is exclusive fullscreen's while it shows).
ScreenMode DesktopMode() {
  MONITORINFOEXW info;
  DEVMODEW mode = {};
  mode.dmSize = sizeof(mode);
  if (GameMonitor(info) && EnumDisplaySettingsW(info.szDevice, ENUM_REGISTRY_SETTINGS, &mode))
    return {int(mode.dmPelsWidth), int(mode.dmPelsHeight)};
  return {int(info.rcMonitor.right - info.rcMonitor.left),
          int(info.rcMonitor.bottom - info.rcMonitor.top)};
}
std::vector<ScreenMode> ScreenModes() {
  // Enumerated again after two seconds only: a page asks for them several times as it is made, and
  // the monitor's hundreds of modes took a moment each time (the page opened with a hitch).
  static std::chrono::steady_clock::time_point last;
  const auto now = std::chrono::steady_clock::now();
  {
    std::lock_guard<std::mutex> lock(g_modes_mutex);
    if (!g_modes.empty() && now - last < std::chrono::seconds(2)) return g_modes;
  }
  last = now;
  std::vector<ScreenMode> modes;
  MONITORINFOEXW info;
  if (GameMonitor(info)) {
    DEVMODEW mode = {};
    mode.dmSize = sizeof(mode);
    for (DWORD i = 0; EnumDisplaySettingsW(info.szDevice, i, &mode); ++i) {
      const ScreenMode m{int(mode.dmPelsWidth), int(mode.dmPelsHeight)};
      if (m.width >= 1280 && m.height >= 720 && m.width > m.height &&
          std::find(modes.begin(), modes.end(), m) == modes.end())
        modes.push_back(m);
    }
  }
  const ScreenMode desktop = DesktopMode();
  if (std::find(modes.begin(), modes.end(), desktop) == modes.end()) modes.push_back(desktop);
  std::sort(modes.begin(), modes.end(), [](const ScreenMode& a, const ScreenMode& b) {
    return a.width * a.height != b.width * b.height ? a.width * a.height < b.width * b.height
                                                    : a.width < b.width;
  });
  while (modes.size() > 10) modes.erase(modes.begin());
  std::lock_guard<std::mutex> lock(g_modes_mutex);
  g_modes = modes;
  return modes;
}
bool IsResText(Text t) { return t >= kRes0 && t <= kRes9; }
ScreenMode ResMode(Text t) {
  std::lock_guard<std::mutex> lock(g_modes_mutex);
  const size_t i = size_t(t - kRes0);
  return i < g_modes.size() ? g_modes[i] : ScreenMode{};
}
std::string ModeName(const ScreenMode& m) {
  return std::to_string(m.width) + "x" + std::to_string(m.height);
}
// fullscreen_resolution's mode, the desktop's if empty or unreadable.
ScreenMode ChosenMode() {
  ScreenMode m;
  if (std::sscanf(REXCVAR_GET(fullscreen_resolution).c_str(), "%dx%d", &m.width, &m.height) == 2 &&
      m.width > 0 && m.height > 0)
    return m;
  return DesktopMode();
}
std::u16string g_res_text[10];

int FpsRate(const FpsChoice& c, int hz) {
  return c.limit ? c.limit : (hz + c.interval / 2) / c.interval;
}
std::vector<FpsChoice> FpsChoices() {
  const int hz = g_screen_hz = ScreenHz();
  std::vector<FpsChoice> choices;
  for (int n = 4; n >= 1; --n)
    if (n == 1 || hz / n >= 30) choices.push_back({n, 0});
  const size_t fractions = choices.size();
  for (int limit : kFpsLimits) {
    bool close = limit >= hz;
    for (size_t i = 0; i < fractions; ++i)
      close |= std::abs(FpsRate(choices[i], hz) - limit) * 20 <= limit;
    if (!close) choices.push_back({1, limit});
  }
  while (choices.size() > 10) choices.erase(choices.begin() + fractions);
  std::stable_sort(choices.begin(), choices.end(), [hz](const FpsChoice& a, const FpsChoice& b) {
    return FpsRate(a, hz) < FpsRate(b, hz);
  });
  return choices;
}
Text FpsText(const FpsChoice& c) {
  for (size_t i = 0; i < std::size(kFpsLimits); ++i)
    if (c.limit == kFpsLimits[i]) return Text(kFpsLimit30 + i);
  return kFpsTexts[std::clamp(c.interval, 1, 4) - 1];
}
bool IsFpsText(Text t) { return t >= kFpsDiv1 && t <= kFpsDiv4; }
std::u16string g_fps_text[4];

const char16_t* TextIn(Lang lang, Text t) {
  if (IsResText(t)) {
    const ScreenMode m = ResMode(t);
    const std::string name = std::to_string(m.width) + "X" + std::to_string(m.height);
    std::u16string& text = g_res_text[t - kRes0];
    text.assign(name.begin(), name.end());
    return text.c_str();
  }
  if (IsFpsText(t)) {
    const int interval = int(t - kFpsDiv1) + 1;
    const std::string fps = std::to_string((g_screen_hz.load() + interval / 2) / interval);
    std::u16string& text = g_fps_text[interval - 1];
    text.assign(fps.begin(), fps.end());
    return text.c_str();
  }
  if (t == kGtaoRow) return u"GTAO";
  if (t == kVsyncRow) return u"VSYNC";
  if (t == kDetailOriginal) return u"ORIGINAL";
  if (t == kDetailHigh) return u"HIGH";
  if (t == kDetailUltra) return u"ULTRA";
  if (t >= kSharp0 && t <= kSharp20) {
    static std::u16string sharp[21];
    std::u16string& text = sharp[t - kSharp0];
    if (text.empty()) {
      const int n = int(t - kSharp0) * 5;  // hundredths
      const std::string v = std::to_string(n / 100) + "." + char('0' + n / 10 % 10) + char('0' + n % 10);
      text.assign(v.begin(), v.end());
    }
    return text.c_str();
  }
  for (size_t i = 0; i < kLocalizedCount; ++i)
    if (kLocalized[i] == t) return kTranslations[lang][i];
  return kTexts[t];
}

template <size_t N, typename T>
int IndexOf(const T (&values)[N], const T& v, int fallback) {
  for (size_t i = 0; i < N; ++i)
    if (values[i] == v) return int(i);
  return fallback;
}
int ModeIndex(const std::string& v) {
  for (int i = 0; i < 6; ++i)
    if (v == kModes[i]) return i;
  return 0;
}

// The rows: their title, their items (value = index), and the current value.
struct Row {
  const char* name;  // GUI field
  Text title;
  std::vector<Text> items;
  int value;
};

// The page's values: the upscaler (0 native, 1 DLSS, 2 FSR) and the others as row indices.
struct Values {
  int upscaler = 0, mode = 0, framegen = 0, scale = 1, msaa = 1, aniso = 0;
  int window = 1;  // windowed, borderless, exclusive fullscreen (the SDK's window cvars)
  int blur = 0;    // motion blur off / on (pgr4_disable_motion_blur)
  int res = 0;     // index in ScreenModes (fullscreen_resolution)
  int dlssfg = 0;  // DLSS-G off, or its frames generated (the dlssfg row, MfgRow)
  int bright = 4;  // index in kBrightLevels (pgr4_native_brightness)
  int fps = 0;     // index in FpsChoices (pgr4_native_present_interval, pgr4_native_fps_limit)
  int vsync = 1;   // off / on (d3d12_present_vsync)
  int gtao = 0;    // off / on (pgr4_native_gtao)
  int hd = 1;      // HD textures off / on (pgr4_native_texture_hd)
  int reflect = 0;  // enhanced effects off / on (pgr4_native_enhanced_reflections)
  int details = 0;  // enhanced details: original, high, ultra (pgr4_native_details)
  // The sharpening of the upscaler shown, 0.00 to 1.00 in 0.05 steps (kSharp0..kSharp20).
  int sharp = 3;
};

// The pages: DISPLAY (window mode, definition, frame rate limit, brightness), GRAPHICS (image and
// smoothness, HD textures, reflections, anisotropy, motion blur) and, from its
// IMAGE AND SMOOTHNESS row, the upscaler's (anti-aliasing, quality, sharpening, frame generation,
// MSAA).
enum Page { kDisplayPage, kGraphicsPage, kImagePage };

Values CurrentValues() {
  NativeRenderer* r = NativeRenderer::Live();
  const bool dlss_ok = pgr4::native::streamline::Active();
  Values v;
  v.upscaler = !(r && r->upscaler_on()) ? 0 : (dlss_ok && REXCVAR_GET(pgr4_native_dlss) ? 1 : 2);
  v.mode = ModeIndex(REXCVAR_GET(pgr4_native_dlss_mode));
  v.framegen = (v.upscaler == 1 ? REXCVAR_GET(pgr4_native_dlss_fg)
                                : REXCVAR_GET(pgr4_native_frame_generation))
                   ? 1
                   : 0;
  v.dlssfg = REXCVAR_GET(pgr4_native_dlss_fg) ? int(pgr4::native::streamline::GeneratedFrames()) : 0;
  v.scale = std::clamp(REXCVAR_GET(pgr4_native_resolution_scale), 1, 4) - 1;
  // The level nearest the setting.
  for (int i = 0; i < int(std::size(kBrightLevels)); ++i)
    if (std::abs(kBrightLevels[i] - REXCVAR_GET(pgr4_native_brightness)) <
        std::abs(kBrightLevels[v.bright] - REXCVAR_GET(pgr4_native_brightness)))
      v.bright = i;
  v.msaa = IndexOf(kMsaa, int(REXCVAR_GET(pgr4_native_msaa)), 1);
  v.aniso = IndexOf(kAniso, int(REXCVAR_GET(pgr4_native_anisotropy)), 0);
  v.window = !REXCVAR_GET(fullscreen) ? 0 : (REXCVAR_GET(fullscreen_exclusive) ? 2 : 1);
  v.blur = REXCVAR_GET(pgr4_disable_motion_blur) ? 0 : 1;
  const std::string vsync = rex::cvar::GetFlagByName("d3d12_present_vsync");
  v.vsync = vsync == "true" || vsync == "1" ? 1 : 0;
  v.gtao = REXCVAR_GET(pgr4_native_gtao) ? 1 : 0;
  v.hd = REXCVAR_GET(pgr4_native_texture_hd) ? 1 : 0;
  v.reflect = REXCVAR_GET(pgr4_native_enhanced_reflections) ? 1 : 0;
  v.details = std::clamp(REXCVAR_GET(pgr4_native_details), 0, 2);
  // The upscaler's in use (FSR's when it is FSR, else DLSS's).
  const double sharp = v.upscaler == 2 ? REXCVAR_GET(pgr4_native_sharpen_fsr)
                                       : REXCVAR_GET(pgr4_native_sharpen_dlss);
  v.sharp = std::clamp(int(std::lround(sharp * 20)), 0, 20);
  const std::vector<ScreenMode> modes = ScreenModes();
  const ScreenMode chosen = ChosenMode();
  v.res = int(modes.size()) - 1;
  for (size_t i = 0; i < modes.size(); ++i)
    if (modes[i] == chosen) v.res = int(i);
  const std::vector<FpsChoice> fps = FpsChoices();
  const int limit = std::max(0, int(REXCVAR_GET(pgr4_native_fps_limit)));
  const FpsChoice now{limit ? 1 : int(REXCVAR_GET(pgr4_native_present_interval)), limit};
  v.fps = int(fps.size()) - 1;
  for (size_t i = 0; i < fps.size(); ++i)
    if (fps[i] == now) v.fps = int(i);
  return v;
}

// Whether DLSS-G generates more than one frame per drawn frame here: the dlssfg row.
bool MfgRow() {
  return pgr4::native::streamline::FrameGenerationActive() &&
         pgr4::native::streamline::MaxGeneratedFrames() > 1;
}

// The page's rows, all of them: the page shows MSAA without an upscaler, the quality and frame
// generation (when that upscaler has it) with one (PN_Layout).
std::vector<Row> RowsFor(Page page, const Values& v) {
  NativeRenderer* r = NativeRenderer::Live();
  const bool dlss_ok = pgr4::native::streamline::Active();
  std::vector<Row> rows;
  if (page == kDisplayPage) {
    rows.push_back({"window", kWindowRow, {kWindowed, kBorderless, kFullscreen}, v.window});
    std::vector<Text> res;
    for (size_t i = 0; i < ScreenModes().size(); ++i) res.push_back(Text(kRes0 + i));
    rows.push_back({"res", kResolutionRow, res, v.res});
    rows.push_back({"scale", kScale, {kScale1, kScale2, kScale3, kScale4}, v.scale});
    std::vector<Text> fps;
    for (const FpsChoice& c : FpsChoices()) fps.push_back(FpsText(c));
    rows.push_back({"fps", kFpsRow, fps, v.fps});
    rows.push_back({"vsync", kVsyncRow, {kOff, kOn}, v.vsync});
    std::vector<Text> bright;
    for (size_t i = 0; i < std::size(kBrightLevels); ++i) bright.push_back(Text(kBright60 + i));
    rows.push_back({"bright", kBrightRow, bright, v.bright});
    return rows;
  }
  const std::vector<Text> upscalers = dlss_ok
                                          ? std::vector<Text>{kUpscalerOff, kUpscalerDlss, kUpscalerFsr}
                                          : std::vector<Text>{kUpscalerOff, kUpscalerFsr};
  const int upscaler = dlss_ok ? v.upscaler : (v.upscaler ? 1 : 0);
  if (page == kGraphicsPage) {
    // IMAGE AND SMOOTHNESS: the upscaler, its page on A (PN_open).
    rows.push_back({"image", kImageOpen, upscalers, upscaler});
    rows.push_back({"hd", kHdRow, {kOff, kOn}, v.hd});
    rows.push_back({"reflect", kReflectRow, {kOff, kOn}, v.reflect});
    rows.push_back({"details", kDetailsRow, {kDetailOriginal, kDetailHigh, kDetailUltra}, v.details});
    // GTAO (in races, the garage, replays and the photo mode): hidden on 03/11/2026 (1.3 ms a frame at
    // scale 3 for little to see), back on 14/11/2026 at Tazz's request; off by default (pgr4_native_gtao).
    rows.push_back({"ao", kAoRow, {kOff, kGtaoRow}, v.gtao});
    rows.push_back(
        {"aniso", kAnisoRow, {kAnisoGame, kAniso2, kAniso4, kAniso8, kAniso16}, v.aniso});
    rows.push_back({"blur", kBlurRow, {kBlurOff, kBlurOn}, v.blur});
    return rows;
  }
  if (dlss_ok)
    rows.push_back({"upscaler", kUpscaler, {kUpscalerOff, kUpscalerDlss, kUpscalerFsr}, v.upscaler});
  else
    rows.push_back({"upscaler", kUpscaler, {kUpscalerOff, kUpscalerFsr}, v.upscaler ? 1 : 0});
  // The quality mode, DLSS's or FSR's (Upscaler::Initialize); the DLSS preset is the setting's (E).
  rows.push_back({"mode",
                  kQuality,
                  {kModeAuto, kModeDlaa, kModeQuality, kModeBalanced, kModePerformance,
                   kModeUltra},
                  v.mode});
  // The upscaler's sharpening (Upscaler::Sharpen, its CAS after DLSS or FSR), one per upscaler:
  // the one chosen on the page gets it. Restored 31/10/2026 (only in the overlay since the
  // per-upscaler settings, absent from both menus).
  {
    std::vector<Text> items;
    for (int i = 0; i <= 20; ++i) items.push_back(Text(kSharp0 + i));
    rows.push_back({"sharp", kSharpRow, items, v.sharp});
  }
  // Frame generation with the upscaler's own: DLSS-G with DLSS, FidelityFX's with FSR.
  if (pgr4::native::streamline::FrameGenerationActive() || (r && r->CanFsrFrameGeneration()))
    rows.push_back({"framegen", kFrameGen, {kOff, kOn}, v.framegen});
  // DLSS-G's multipliers in its place when it generates more than one frame (an RTX 50, or an
  // RTX 40 unlocked: mfg_unlock.h): off, x2 up to x(maximum + 1).
  if (MfgRow()) {
    std::vector<Text> items = {kOff};
    for (uint32_t n = 1; n <= std::min(5u, pgr4::native::streamline::MaxGeneratedFrames()); ++n)
      items.push_back(Text(kFgX2 + n - 1));
    rows.push_back({"dlssfg", kFrameGen, items, std::min(v.dlssfg, int(items.size()) - 1)});
  }
  rows.push_back({"msaa", kMsaaRow, {kMsaa1, kMsaa2, kMsaa4, kMsaa8}, v.msaa});
  return rows;
}

// The page being shown: its values (the settings', or as last chosen on it before it was made
// again for another upscaler) and its rows, which its digits follow.
Page g_page = kGraphicsPage;
// Opened from the race's pause menu (Ingame\PauseMenu.lua): drawn as its screens are, and back
// there; else from the main menu.
bool g_from_pause = false;
// The main menu's entry PC DISPLAY or PC GRAPHICS was opened from (its value), to select again on the
// way back; NaN: none.
double g_main_menu_return = std::numeric_limits<double>::quiet_NaN();
Values g_page_values;
std::vector<Row> g_page_rows;
// GRAPHICS's values when its IMAGE AND SMOOTHNESS page opened (as applied then), for its return.
Values g_graphics_values;

// `digits` (one per g_page_rows row) over g_page_values.
Values DecodePage(const std::string& digits) {
  Values v = g_page_values;
  const bool dlss_ok = pgr4::native::streamline::Active();
  for (size_t i = 0; i < g_page_rows.size() && i < digits.size(); ++i) {
    const char c = char(std::tolower(static_cast<unsigned char>(digits[i])));
    const int d = c >= 'a' && c <= 'z' ? c - 'a' + 10 : c - '0';
    if (d < 0 || d >= int(g_page_rows[i].items.size())) continue;
    const std::string_view name = g_page_rows[i].name;
    if (name == "upscaler" || name == "image") v.upscaler = dlss_ok ? d : (d ? 2 : 0);
    else if (name == "mode") v.mode = d;
    else if (name == "framegen") v.framegen = d;
    else if (name == "dlssfg") v.dlssfg = d;
    else if (name == "scale") v.scale = d;
    else if (name == "msaa") v.msaa = d;
    else if (name == "aniso") v.aniso = d;
    else if (name == "window") v.window = d;
    else if (name == "res") v.res = d;
    else if (name == "bright") v.bright = d;
    else if (name == "gtao" || name == "ao") v.gtao = d;
    else if (name == "hd") v.hd = d;
    else if (name == "reflect") v.reflect = d;
    else if (name == "details") v.details = d;
    else if (name == "sharp") v.sharp = d;
    else if (name == "blur") v.blur = d;
    else if (name == "fps") v.fps = d;
    else if (name == "vsync") v.vsync = d;
  }
  return v;
}

std::string Id(Text t) { return std::to_string(kTextBase + t); }

// The page, as Options\General.lua builds its own (same calls and layout).
std::string PageScript(Page page, const Values& values) {
  g_page = page;
  g_page_values = values;
  g_page_rows = RowsFor(page, values);
  const std::vector<Row>& rows = g_page_rows;
  std::string s;
  s += "GUI = { optionChosen = 0, maxOption = " + std::to_string(rows.size() - 1) +
       ", effects_scroll_group = { type = UIEnums.ButtonTypes.GROUPSTART, group_type = "
       "UIEnums.GroupType.OFFSET, x = 0, y = 0 } }\n";
  s += "PN_rows = {";
  for (const Row& row : rows) s += "\"" + std::string(row.name) + "_ID\",";
  s += "}\n";
  // The upscaler of each of its row's items (0 none, 1 DLSS, 2 FSR), and which has frame
  // generation.
  NativeRenderer* r = NativeRenderer::Live();
  s += pgr4::native::streamline::Active() ? "PN_kinds = {[0] = 0, [1] = 1, [2] = 2}\n"
                                          : "PN_kinds = {[0] = 0, [1] = 2}\n";
  s += std::string("PN_mfg = ") + (MfgRow() ? "true" : "false") + "\n";
  s += std::string("PN_fg = {[1] = ") +
       (pgr4::native::streamline::FrameGenerationActive() ? "true" : "false") + ", [2] = " +
       (r && r->CanFsrFrameGeneration() ? "true" : "false") + "}\n";
  if (g_from_pause) {
    // As Ingame\IngameOptions.lua: the race faded behind, the in-game slide group and frame, the
    // frame down to the main menu's bottom (its own, 38 points higher, left rows to scroll to).
    s += R"(function Init()
  SetupScreenTimers()
  SetupInfoText(UIText.IDS_BACK_B, 1, false)
  SetupFadedBackground(UIEnums.Layer.BACKGROUND)
  SetupIngameSlideGroup()
  local frame = GetDefaultFrame(113.33333333333334, UIGlobals.screen.safe_top + 40, 413.3333333333333, UIGlobals.screen.safe_bottom - 70, GUIBank.df_alternate_style_pu)
  frame.background_layer = UIEnums.Layer.LAYER_1
  frame.foreground_layer = UIEnums.Layer.LAYER_2
  UIButtons.AddButton(frame)
)";
  } else {
    s += R"(function Init()
  SetupFadePanel()
  SetupScreenTimers()
  SetupInfoText(UIText.IDS_BACK_B, 1, false)
  SetupSlideMenuInfo()
  local frame = GetDefaultFrame(113.33333333333334, UIGlobals.screen.safe_top, 453.3333333333333, UIGlobals.screen.safe_bottom - 30)
  frame.panel = 1
  UIButtons.AddButton(frame)
)";
  }
  s += "  SetupFrameTitle(frame, " +
       Id(page == kDisplayPage ? kDisplayTitle : page == kImagePage ? kImageRow : kTitle) + ")\n";
  s += R"(  local y = frame.y + UIGlobals.frame_mainbox_offset_y + 12
  local step = UIGlobals.options_cr / 2
  local x = 320
  local w = 350 - x * 0.5 - 16
  GUI.lower_clip = frame.height - y + 16
  -- The frame's bottom (its top plus its height).
  GUI.bottom = frame.y + frame.height
  SetClipToFrame(GUIBank.variable_spinner, frame)
  GUI.effects_scroll_group.ID = UIButtons.AddButton(GUI.effects_scroll_group)
  GUIBank.spinner_y = UIGlobals.screen.safe_top + 12
  GUIBank.spinner_cr = 40
)";
  for (const Row& row : rows) {
    const std::string field = std::string("GUI.") + row.name + "_ID";
    s += "  " + field + " = SetupVariableSpinner(" + Id(row.title) + ", y, x, w" +
         (g_from_pause ? ")\n" : ", 1)\n");
    // Added last to first: the game's spinners step through their items the other way round
    // (right went from 1440p down to 720p), so right goes up the list as written. The quality's
    // list runs from the best down, and right goes up it the other way (toward NATIVE).
    const bool forward = std::string_view(row.name) == "mode";
    for (size_t k = 0; k < row.items.size(); ++k) {
      const size_t i = forward ? k : row.items.size() - 1 - k;
      s += "  UIButtons.AddItem(" + field + ", " + std::to_string(i) + ", " + Id(row.items[i]) +
           ", false)\n";
    }
    s += "  UIButtons.SetSelection(" + field + ", " + std::to_string(row.value) + ")\n";
    s += "  y = y + step * 2\n";
  }
  // Saved: DISPLAY and GRAPHICS back to the menu they came from, the upscaler's page back to
  // GRAPHICS (PA_; B: PK_, nothing applied). GRAPHICS's IMAGE AND SMOOTHNESS opens it (PG_, the
  // page's other rows applied first).
  s += std::string("PN_save = \"") +
       (page == kDisplayPage ? "PD_" : page == kImagePage ? "PA_" : "PN_") + "\"\n";
  s += std::string("PN_back = \"") +
       (page == kImagePage ? "Intro\\\\PK_.lua"
        : g_from_pause     ? "Ingame\\\\PauseMenu.lua"
                           : "Intro\\\\MainMenu.lua") +
       "\"\n";
  s += std::string("PN_open = ") + (page == kGraphicsPage ? "\"image_ID\"" : "nil") + "\n";
  // Every page applied when left with B too (as with A): its help shows BACK (B) only.
  s += "PN_back_saves = true\n";
  if (page == kImagePage)
    s += "  GUI.upscaler_start = UIButtons.GetSelection(GUI.upscaler_ID)\n";
  // The render scale: applied by restarting the game, asked first (PN_AskRestart).
  if (page == kDisplayPage) {
    s += "  GUI.scale_start = UIButtons.GetSelection(GUI.scale_ID)\n";
    s += "  GUI.restart_title = " + Id(kRestartTitle) + "\n";
    s += "  GUI.restart_message = " + Id(kRestartMessage) + "\n";
  }
  s += R"(  -- Where the rows went, for PN_Layout to place them again.
  GUI.pos = {}
  for i = 1, #PN_rows do
    local px, py, pz = UIButtons.GetPosition(GUI[PN_rows[i]])
    GUI.pos[i] = {px, py, pz}
  end
  GUI.row_dy = 0
  if #PN_rows > 1 then GUI.row_dy = GUI.pos[2][2] - GUI.pos[1][2] end
  -- The rows the frame holds (the pause menu's is smaller): beyond, the page scrolls.
  GUI.capacity = #PN_rows
  if GUI.row_dy > 0 then
    GUI.capacity = math.max(4, math.floor((GUI.bottom - GUI.pos[1][2]) / GUI.row_dy))
  end
  GUI.scroll = 0
  PN_Layout()
  ActivateOptions()
end
-- Whether a row shows for the upscaler chosen.
function PN_Shows(name)
  local kind = 0
  if GUI.upscaler_ID ~= nil then kind = PN_kinds[UIButtons.GetSelection(GUI.upscaler_ID)] or 0 end
  if name == "mode_ID" then return kind ~= 0 end
  if name == "sharp_ID" then return kind ~= 0 end
  if name == "framegen_ID" then return kind ~= 0 and PN_fg[kind] == true and not (kind == 1 and PN_mfg) end
  if name == "dlssfg_ID" then return kind == 1 and PN_mfg end
  if name == "msaa_ID" then return kind == 0 end
  return true
end
-- The rows shown one under the other from the first row's place, the others out of the frame.
function PN_Layout()
  GUI.visible = {}
  for i = 1, #PN_rows do
    if PN_Shows(PN_rows[i]) then GUI.visible[#GUI.visible + 1] = PN_rows[i] end
  end
  GUI.maxOption = #GUI.visible - 1
  if GUI.optionChosen > GUI.maxOption then GUI.optionChosen = GUI.maxOption end
  PN_Place()
end
-- The shown rows one under the other from the first row's place, scrolled to keep the chosen
-- one in the frame with a row more past it (the next one shows before it is reached: rows below
-- are not left to guess); the others, and those scrolled out, out of the frame.
function PN_Place()
  local n = #GUI.visible
  local margin = 0
  if GUI.capacity >= 3 then margin = 1 end
  if GUI.optionChosen - margin < GUI.scroll then GUI.scroll = GUI.optionChosen - margin end
  if GUI.optionChosen + margin >= GUI.scroll + GUI.capacity then GUI.scroll = GUI.optionChosen + margin - GUI.capacity + 1 end
  if GUI.scroll < 0 then GUI.scroll = 0 end
  if GUI.scroll > math.max(0, n - GUI.capacity) then GUI.scroll = math.max(0, n - GUI.capacity) end
  local slot = {}
  for k = 1, n do slot[GUI.visible[k]] = k - 1 - GUI.scroll end
  for i = 1, #PN_rows do
    local p = GUI.pos[i]
    local s = slot[PN_rows[i]]
    if s ~= nil and s >= 0 and s < GUI.capacity then
      UIButtons.ChangePosition(GUI[PN_rows[i]], p[1], GUI.pos[1][2] + s * GUI.row_dy, p[3])
    else
      UIButtons.ChangePosition(GUI[PN_rows[i]], p[1], -4000, p[3])
    end
  end
end
function StartLoop(a) return true end
function MessageUpdate(msg, a, b, c, d)
  if GUI.finished == true then return end
  if (msg == "BUTTON_BACK" or msg == "BUTTON_NEXT") and PN_AskRestart() then return end
  if msg == "BUTTON_BACK" then
    GUI.finished = true
    PlayMenuBack()
    if PN_back_saves then
      UIScreen.SetNextScreen("Intro\\" .. PN_save .. PN_Digits() .. ".lua")
    else
      UIScreen.SetNextScreen(PN_back)
    end
  elseif msg == "BUTTON_UP" then
    PlayMenuMove()
    if GUI.optionChosen > 0 then GUI.optionChosen = GUI.optionChosen - 1 else GUI.optionChosen = GUI.maxOption end
    ActivateOptions()
  elseif msg == "BUTTON_DOWN" then
    PlayMenuMove()
    if GUI.optionChosen < GUI.maxOption then GUI.optionChosen = GUI.optionChosen + 1 else GUI.optionChosen = 0 end
    ActivateOptions()
  elseif msg == "BUTTON_NEXT" then
    GUI.finished = true
    PlayMenuNext()
    if PN_open ~= nil and GUI.visible[GUI.optionChosen + 1] == PN_open then
      UIScreen.SetNextScreen("Intro\\PG_" .. PN_Digits() .. ".lua")
    else
      UIScreen.SetNextScreen("Intro\\" .. PN_save .. PN_Digits() .. ".lua")
    end
  end
end
-- The render scale changed: the game restarts to apply it (its shaders compiled by the launcher);
-- the row back to the scale in use only if the player says no.
function PN_AskRestart()
  if GUI.scale_start == nil or CustomPopup == nil or SetupCustomPopup == nil then return false end
  if UIButtons.GetSelection(GUI.scale_ID) == GUI.scale_start then return false end
  CustomPopup.Data["PGR4_RESTART"] = {
    darken = true,
    title_text_ID = GUI.restart_title,
    message_text_ID = GUI.restart_message,
    next_text_ID = UIText.IDS_YES_A,
    back_text_ID = UIText.IDS_NO_B,
    next_screen = "Intro\\PX_" .. PN_Digits() .. ".lua",
    back_CB = function() UIButtons.SetSelection(GUI.scale_ID, GUI.scale_start) end,
  }
  SetupCustomPopup("PGR4_RESTART")
  return true
end
-- One character per row: 0-9, then a, b, c... from 10 (the sharpening's 21 steps).
PN_letters = {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o", "p"}
function PN_Digits()
  local digits = ""
  for i = 1, #PN_rows do
    local s = UIButtons.GetSelection(GUI[PN_rows[i]])
    if s >= 10 then s = PN_letters[s - 9] end
    digits = digits .. s
  end
  return digits
end
-- Another upscaler: its rows in place (nothing applied before A).
function FrameUpdate(a)
  if GUI.finished == true or GUI.upscaler_start == nil then return end
  local now = UIButtons.GetSelection(GUI.upscaler_ID)
  if now ~= GUI.upscaler_start then
    GUI.upscaler_start = now
    PN_Layout()
    ActivateOptions()
  end
end
function Render() end
function End() end
function EndLoop(a) return true end
function ActivateOptions()
  if GUI.capacity ~= nil and #GUI.visible > GUI.capacity then PN_Place() end
  local chosen = GUI.visible[GUI.optionChosen + 1]
  for i = 1, #PN_rows do
    UIButtons.SetSelected(GUI[PN_rows[i]], PN_rows[i] == chosen)
  end
end
)";
  return s;
}

// Applies the page's values on the renderer's thread.
// Local change (pgr4 project): the launcher beside the game started to run it again (its shaders
// first, at the new render scale), the game closed. False if there is no launcher.
void (*g_close_game)() = nullptr;  // CloseGameWindow, defined further on
bool Pgr4RestartThroughLauncher() {
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  const std::filesystem::path launcher = std::filesystem::path(exe).parent_path() / L"PGR4Launcher.exe";
  std::error_code ec;
  if (!std::filesystem::is_regular_file(launcher, ec)) return false;
  std::wstring command = L"\"" + launcher.wstring() + L"\" --autostart";
  STARTUPINFOW si = {sizeof(si)};
  PROCESS_INFORMATION pi = {};
  if (!CreateProcessW(launcher.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                      launcher.parent_path().c_str(), &si, &pi))
    return false;
  AllowSetForegroundWindow(pi.dwProcessId);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  if (g_close_game) g_close_game();
  return true;
}

void ApplyPage(const Values& want, bool restart = false) {
  NativeRenderer* r = NativeRenderer::Live();
  if (!r) return;
  const Values now = CurrentValues();
  // The screen resolution: exclusive fullscreen's mode, which a mode other than the desktop's
  // needs (borderless is at the desktop's; the window keeps its size).
  const std::vector<ScreenMode> modes = ScreenModes();
  const ScreenMode mode = modes[std::clamp(want.res, 0, int(modes.size()) - 1)];
  const bool desktop = mode == DesktopMode();
  const std::string res = desktop ? "" : ModeName(mode);
  if (res != REXCVAR_GET(fullscreen_resolution))
    rex::cvar::SetFlagByName("fullscreen_resolution", res);
  const int window = !desktop && want.window == 1 ? 2 : want.window;
  if (window != now.window) {
    rex::cvar::SetFlagByName("fullscreen_exclusive", window == 2 ? "true" : "false");
    rex::cvar::SetFlagByName("fullscreen", window != 0 ? "true" : "false");
  }
  // Read where they act (the motion blur flag, the frame limiter): set as they are.
  const std::vector<FpsChoice> fps = FpsChoices();
  const FpsChoice& chosen = fps[std::clamp(want.fps, 0, int(fps.size()) - 1)];
  REXCVAR_SET(pgr4_native_present_interval, chosen.interval);
  REXCVAR_SET(pgr4_native_fps_limit, chosen.limit);
  // Read at each frame.
  rex::cvar::SetFlagByName("d3d12_present_vsync", want.vsync ? "true" : "false");
  // Read at each present.
  REXCVAR_SET(pgr4_native_brightness, kBrightLevels[std::clamp(want.bright, 0, 9)]);
  // Read at each frame.
  REXCVAR_SET(pgr4_native_gtao, want.gtao != 0);
  REXCVAR_SET(pgr4_native_texture_hd, want.hd != 0);
  REXCVAR_SET(pgr4_native_enhanced_reflections, want.reflect != 0);
  REXCVAR_SET(pgr4_native_details, std::clamp(want.details, 0, 2));
  // Read where the game's motion blur is set up (src/pgr4_hooks.cpp).
  REXCVAR_SET(pgr4_disable_motion_blur, want.blur == 0);
  // Read at each upscale: the sharpening of the upscaler chosen.
  if (want.upscaler == 1) REXCVAR_SET(pgr4_native_sharpen_dlss, want.sharp * 0.05);
  if (want.upscaler == 2) REXCVAR_SET(pgr4_native_sharpen_fsr, want.sharp * 0.05);
  r->QueueSettings([r, want, now, restart] {
    bool recreate = false, rebuild = false, msaa = false;
    if (want.upscaler != now.upscaler) {
      if (want.upscaler) {
        const bool dlss = want.upscaler == 1;
        if (dlss != REXCVAR_GET(pgr4_native_dlss)) recreate = true;
        REXCVAR_SET(pgr4_native_dlss, dlss);
      }
      r->RequestUpscaler(want.upscaler != 0);
    }
    if (want.upscaler) {
      if (want.mode != now.mode) {
        REXCVAR_SET(pgr4_native_dlss_mode, std::string(kModes[want.mode]));
        recreate = true;
      }
      // Both frame generations follow the row, whichever upscaler it was shown for; DLSS-G its
      // own with Multi Frame Generation.
      if (MfgRow()) {
        REXCVAR_SET(pgr4_native_dlss_fg, want.dlssfg > 0);
        if (want.dlssfg > 0) REXCVAR_SET(pgr4_native_dlss_fg_frames, want.dlssfg);
      } else {
        REXCVAR_SET(pgr4_native_dlss_fg, want.framegen == 1);
      }
      if (r->CanFsrFrameGeneration()) {
        REXCVAR_SET(pgr4_native_frame_generation, want.framegen == 1);
        r->RequestFsrFrameGeneration();
      }
    } else {
      if (want.msaa != now.msaa) {
        REXCVAR_SET(pgr4_native_msaa, kMsaa[want.msaa]);
        msaa = true;
      }
    }
    if (want.scale != now.scale) {
      REXCVAR_SET(pgr4_native_resolution_scale, want.scale + 1);
      rebuild = true;
    }
    if (want.aniso != now.aniso) {
      REXCVAR_SET(pgr4_native_anisotropy, kAniso[want.aniso]);
      rebuild = true;
    }
    // The render scale asked for a restart: saved, the game started again (no launcher: live).
    const std::string saved = pgr4::native::SaveRendererSettings(want.upscaler != 0);
    REXLOG_INFO("PGR4 menu: settings applied; {}", saved);
    if (restart && Pgr4RestartThroughLauncher()) {
      REXLOG_INFO("PGR4 menu: restarting through the launcher");
      return;
    }
    if (recreate) r->RequestUpscalerRecreate();
    if (msaa) r->RequestMsaa();
    if (rebuild) r->RequestRebuild();
  });
}


// The script sub_82883D30 serves instead of the found file, empty when none.
std::string g_script;
size_t g_script_pos = 0;
int g_names_logged = 0;

bool EndsWith(std::string_view s, std::string_view end) {
  return s.size() >= end.size() && s.substr(s.size() - end.size()) == end;
}

// Guest UTF-16BE copies of our texts, made at their first lookup in a language.
std::mutex g_text_mutex;
uint32_t g_text_guest[kLangCount][kTextCount] = {};

uint32_t GuestText(Lang lang, Text t) {
  std::lock_guard<std::mutex> lock(g_text_mutex);
  static std::unordered_map<int, uint32_t> fps_guest;  // by rate and interval
  static std::unordered_map<int64_t, uint32_t> res_guest;  // by screen mode
  const ScreenMode res = IsResText(t) ? ResMode(t) : ScreenMode{};
  uint32_t& cached = IsFpsText(t)   ? fps_guest[g_screen_hz * 8 + int(t - kFpsDiv1)]
                     : IsResText(t) ? res_guest[int64_t(res.width) << 32 | res.height]
                                    : g_text_guest[lang][t];
  if (cached) return cached;
  const std::u16string_view text = TextIn(lang, t);
  auto* memory = rex::system::kernel_memory();
  const uint32_t bytes = uint32_t((text.size() + 1) * 2);
  const uint32_t addr = memory->SystemHeapAlloc(bytes);
  if (!addr) return 0;
  auto* out = memory->TranslateVirtual<uint8_t*>(addr);
  for (size_t i = 0; i <= text.size(); ++i) {
    const char16_t c = i < text.size() ? text[i] : u'\0';
    out[2 * i] = uint8_t(c >> 8);
    out[2 * i + 1] = uint8_t(c & 0xFF);
  }
  cached = addr;
  return addr;
}

// A guest UTF-16BE text (0: none).
std::u16string ReadGuestText(const uint8_t* base, uint32_t addr) {
  std::u16string text;
  if (!addr) return text;
  for (const uint8_t* p = base + addr; (p[0] | p[1]) && text.size() < 64; p += 2)
    text.push_back(char16_t((p[0] << 8) | p[1]));
  return text;
}

// The game's language, from its own IDS_GRAPHICS (and IDS_OPTIONS) text, kept for the text's
// address.
uint32_t g_lang_graphics = 0;
Lang g_lang = kEn;

// The last script the find hook was asked for.
std::string g_last_screen;
std::atomic<bool> g_in_game_screen{false};  // the last screen was a race's (InGame\...)
// The last screen shows a race, the garage (InGame\GarageHUD.lua), a replay or the photo mode
// over them: where the GTAO applies (pgr4-native scene_effects.cpp).
std::atomic<bool> g_gtao_screen{false};

// The pause menu's entry chosen (GetSelection answered OPTIONS for it): the page to serve in place
// of Ingame\IngameOptions.lua.
enum PauseTarget { kPauseNone, kPauseDisplay, kPauseGraphics };
std::atomic<PauseTarget> g_pause_target{kPauseNone};
// From Ingame\PauseMenu.lua's load to its first entry: its table.getn calls (both of
// GUI.currentUI, its frame's height and a debug print) count the two entries it will get.
std::atomic<bool> g_pause_sizing{false};
// Their values in the pause menu (its own are 1-8 and -99), and its OPTIONS (OP_OPTIONS).
constexpr double kPauseDisplayValue = 9001, kPauseGraphicsValue = 9002, kPauseOptionsValue = 4;

}  // namespace

// The title screen (Intro\StartScreen.lua) quits on B, after the game's own confirmation popup
// (CustomPopup.Data, SetupCustomPopup: its texts IDS_QUIT and IDS_ARE_YOU_SURE_QUIT, A yes, B no).
// The game's script is read as it would be (sub_82883D30 after the find) and given back inside a
// Lua chunk that loads it (loadstring takes compiled chunks), then wraps its MessageUpdate: B opens
// the popup, every other message goes to the game's. Yes goes to Intro\PGR4_Quit.lua, which closes
// the window as its close button does.
REX_EXTERN(__imp__sub_82883CC0);
REX_EXTERN(__imp__sub_82883D30);
bool EndsWithNoCase(std::string_view s, std::string_view end) {
  if (s.size() < end.size()) return false;
  for (size_t i = 0; i < end.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(s[s.size() - end.size() + i])) !=
        std::tolower(static_cast<unsigned char>(end[i])))
      return false;
  return true;
}
constexpr uint32_t kScriptReadPos = 0x82CA976C;  // sub_82883D30's offset in the found file (-1: none)
constexpr const char* kQuitScreen =
    "function Init() end function StartLoop(a) return true end function MessageUpdate(m,a,b,c,d) "
    "end function FrameUpdate(t) end function Render() end function End() end "
    "function EndLoop(a) return true end";
// The game's script `name` (the loader's find at hand in ctx), read as the loader would, inside a
// Lua chunk that runs it (loadstring takes compiled chunks) and then `after`; empty if it could
// not be read.
std::string WrapGameScript(PPCContext& ctx, uint8_t* base, const std::string& name,
                           const std::string& after, const std::string& before = {}) {
  const PPCContext saved = ctx;
  __imp__sub_82883CC0(ctx, base);  // the game's find, as the loader asked
  ctx = saved;
  static uint32_t buffer = 0;
  constexpr uint32_t kChunk = 0x10000;
  if (!buffer) buffer = rex::system::kernel_memory()->SystemHeapAlloc(kChunk);
  if (!buffer) return {};
  const uint32_t pos = rex::memory::load_and_swap<uint32_t>(base + kScriptReadPos);
  rex::memory::store_and_swap<uint32_t>(base + kScriptReadPos, 0);
  std::string bytes;
  for (int i = 0; i < 256; ++i) {
    ctx.r3.u64 = buffer;
    ctx.r4.u64 = kChunk;
    __imp__sub_82883D30(ctx, base);
    const uint32_t n = ctx.r3.u32;
    if (!n || n > kChunk) break;
    bytes.append(reinterpret_cast<const char*>(base + buffer), n);
  }
  ctx = saved;
  rex::memory::store_and_swap<uint32_t>(base + kScriptReadPos, pos);
  if (bytes.size() < 12 || bytes.compare(0, 4, "\x1bLua") != 0) {
    REXLOG_WARN("PGR4 menu: {} read as {} bytes, not a Lua chunk: left as it is", name,
                bytes.size());
    return {};
  }
  std::string s = "local pgr4_chunk = loadstring(\"";
  s.reserve(bytes.size() * 4 + 1024);
  for (const unsigned char c : bytes) {
    if (c >= 0x20 && c < 0x7F && c != '"' && c != '\\') {
      s += char(c);
    } else {
      char esc[5];
      std::snprintf(esc, sizeof(esc), "\\%03u", unsigned(c));
      s += esc;
    }
  }
  s += "\")\n" + before + "\npgr4_chunk()\n" + after;
  return s;
}

std::string StartScreenQuit(PPCContext& ctx, uint8_t* base, const std::string& name) {
  std::string s = WrapGameScript(ctx, base, name, R"lua(
if PGR4_warning ~= nil then
  UIText.IDS_WARNING, UIText.IDS_DISCLAIMER = PGR4_warning, PGR4_disclaimer
  PGR4_warning = nil
end
CustomPopup.Data["PGR4_QUIT_GAME"] = {
  darken = true,
  title_text_ID = )lua" + Id(kQuitTitle) + R"lua(,
  message_text_ID = )lua" + Id(kQuitMessage) + R"lua(,
  next_text_ID = UIText.IDS_YES_A,
  back_text_ID = UIText.IDS_NO_B,
  next_screen = "Intro\\PGR4_Quit.lua",
}
local pgr4_init = Init
function Init()
  pgr4_init()
  SetupInfoText()lua" + Id(kQuitHint) + R"lua(, 1.0, false)
  SetupInfoText()lua" + Id(kVersionTag) + R"lua(, 1.0, true)
end
local pgr4_message_update = MessageUpdate
function MessageUpdate(message, a, b, c, d)
  if message == "BUTTON_BACK" then
    SetupCustomPopup("PGR4_QUIT_GAME")
    return
  end
  return pgr4_message_update(message, a, b, c, d)
end
)lua");
  if (!s.empty()) REXLOG_INFO("PGR4 menu: the title screen with B to quit");
  return s;
}

// The multiplayer menu (Intro\MultiPlayerMenu.lua): its help line, which GUIBank's SetupSlideMenu
// draws dark grey ("255 45 45 45") for every slide menu, white over this one's dark picture. The
// menu's table goes through UIButtons.AddButton during Init: its help_colour changed there.
// The first screen (Intro\TitleMovies.lua): our welcome in place of the game's warning. Set before
// its script runs (it reads IDS_DISCLAIMER into a table as it loads, and hands its Init to the game
// itself, SetInitFunction: a wrapped Init would not run). The next screen, the title screen, gives the two texts back (StartScreenQuit): other screens
// use IDS_WARNING too.
std::string TitleMoviesWelcome(PPCContext& ctx, uint8_t* base, const std::string& name) {
  return WrapGameScript(ctx, base, name, {}, R"lua(
if PGR4_warning == nil then
  PGR4_warning, PGR4_disclaimer = UIText.IDS_WARNING, UIText.IDS_DISCLAIMER
end
UIText.IDS_WARNING = )lua" + Id(kWelcomeTitle) + R"lua(
UIText.IDS_DISCLAIMER = )lua" + Id(kWelcomeText) + R"lua(
)lua");
}

// The controls screen (Options\Controllers.lua) with a PlayStation controller: its labels sit
// around the controller's picture at GUI.button_positions (1 left stick, 7 d-pad, ...), set for
// the Xbox 360 controller, whose left stick is above its d-pad. The DualSense's d-pad is above
// its left stick: those two places swapped. The picture is textures\playstation's.
std::string ControllersPlayStation(PPCContext& ctx, uint8_t* base, const std::string& name) {
  return WrapGameScript(ctx, base, name, R"lua(
if GUI ~= nil and GUI.button_positions ~= nil then
  local p = GUI.button_positions
  p[1], p[7] = p[7], p[1]
end
)lua");
}

// The career's garage choice (CareerMode\Garage.lua) with --pgr4_unlock_all: its Init makes the
// garages above the career's Rating (0 amateur, 1 pro, 2 hotshot, 3 master) unselectable
// (UIButtons.SetMenuItemActive(menu, i, false)); left selectable. OfflineCareer.GoingToGarage
// takes any of the 4.
std::string GarageAllUnlocked(PPCContext& ctx, uint8_t* base, const std::string& name) {
  return WrapGameScript(ctx, base, name, R"lua(
local pgr4_init = Init
function Init()
  local set_active = UIButtons.SetMenuItemActive
  UIButtons.SetMenuItemActive = function(menu, item, active, ...)
    if active == false then return end
    return set_active(menu, item, active, ...)
  end
  pgr4_init()
  UIButtons.SetMenuItemActive = set_active
end
)lua");
}

// The career's home (CareerMode\CareerHome.lua) with --pgr4_unlock_all: its Init adds the test
// track (item 6, IDS_CARSELECT_MY_TESTTRACK, which opens CareerMode\TestTrack\TestTrackHome.lua)
// only above the amateur Rating (vars.Rating > 0), just before the difficulty (item 4); added
// there whatever the Rating, the career's own Rating left as it is.
std::string CareerHomeTestTrack(PPCContext& ctx, uint8_t* base, const std::string& name) {
  return WrapGameScript(ctx, base, name, R"lua(
local pgr4_init = Init
function Init()
  local add, help = UIButtons.AddItem, UIButtons.AddItemHelp
  local test_track = false
  UIButtons.AddItem = function(menu, item, text, ...)
    if item == 6 then test_track = true end
    if item == 4 and not test_track then
      test_track = true
      add(menu, 6, UIText.IDS_CARSELECT_MY_TESTTRACK, false)
      help(menu, UIText.IDS_CARSELECT_MY_TESTTRACK_HELP)
    end
    return add(menu, item, text, ...)
  end
  pgr4_init()
  UIButtons.AddItem = add
end
)lua");
}

// The photo mode (PhotoMode\PhotoMode.lua): Start takes a screenshot without its interface, at
// the render resolution (pgr4-native's F1 screenshot: Pgr4AskScreenshot). The game's own Start
// (a development build's: its interface hidden) does nothing in its final build. Its help, one
// line above Y's (SetupInfoText's third), and the controls' frame (the effects' one moved right,
// foreground FrameForeground_Muck) one line (27) higher, as far from it as it was from Y's.
// Start hides the interface as A does (UIButtons.SetActive of GUI.store_id), asks the screenshot
// 3 frames later (table.getn of a 2312-long probe), and shows it again once its frame is copied
// (a 2313-long one: 1 then), as the game's own Start did (SetActive, then RemainInactive).
std::string PhotoModeShot(PPCContext& ctx, uint8_t* base, const std::string& name) {
  return WrapGameScript(ctx, base, name, R"lua(
local pgr4_ask, pgr4_taken = {}, {}
for i = 1, 2312 do pgr4_ask[i] = true end
for i = 1, 2313 do pgr4_taken[i] = true end
local pgr4_shot, pgr4_wait = 0, 0
local pgr4_init = Init
function Init()
  local add = UIButtons.AddButton
  UIButtons.AddButton = function(button, ...)
    if type(button) == "table" and button.foreground_shader ==
        UIEnums.ShaderIndex.FrameForeground_Muck and type(button.x) == "number" and
        type(button.y) == "number" and button.x > UIGlobals.screen.safe_left + 200 then
      button.y = button.y - 27
    end
    return add(button, ...)
  end
  pgr4_init()
  UIButtons.AddButton = add
  StoreID(SetupInfoText()lua" + Id(kShotHint) + R"lua(, 3.0, false))
end
local pgr4_message_update = MessageUpdate
function MessageUpdate(message, a, b, c, d)
  if message == "BUTTON_START" then
    if pgr4_shot == 0 and GUI.take_photo ~= true and GUI.display_off ~= true then
      for _, id in ipairs(GUI.store_id) do UIButtons.SetActive(id, false) end
      pgr4_shot, pgr4_wait = 1, 3
    end
    return
  end
  if pgr4_shot ~= 0 then return end
  return pgr4_message_update(message, a, b, c, d)
end
local pgr4_frame_update = FrameUpdate
function FrameUpdate(t)
  if pgr4_shot == 1 then
    pgr4_wait = pgr4_wait - 1
    if pgr4_wait <= 0 then
      table.getn(pgr4_ask)
      UISystem.PlaySound(UIEnums.FrontendSound.PhotoClick)
      pgr4_shot, pgr4_wait = 2, 120
    end
  elseif pgr4_shot == 2 then
    pgr4_wait = pgr4_wait - 1
    if table.getn(pgr4_taken) == 1 or pgr4_wait <= 0 then
      for _, id in ipairs(GUI.store_id) do UIButtons.SetActive(id, true) end
      RemainInactive()
      pgr4_shot = 0
    end
  end
  if pgr4_frame_update ~= nil then return pgr4_frame_update(t) end
end
)lua");
}

std::string MultiPlayerMenuWhiteHelp(PPCContext& ctx, uint8_t* base, const std::string& name) {
  return WrapGameScript(ctx, base, name, R"lua(
local pgr4_init = Init
function Init()
  local add = UIButtons.AddButton
  UIButtons.AddButton = function(button, ...)
    if type(button) == "table" and button.help_colour ~= nil then
      button.help_colour = "255 255 255 255"
    end
    return add(button, ...)
  end
  pgr4_init()
  UIButtons.AddButton = add
end
)lua");
}

// GUIBank's GlobalUpdate (Screens\GUIBank.lua, run every frame): during a system link game it
// watches the cable, and once it is unplugged it closes the sessions and goes to the title screen
// (through UILoad from a race). The SDK reports the cable unplugged when every PC the game talks
// to is lost (src/system/xsocket.cpp): for a client, the host. Here a client then goes back to the
// system link menu (PlayTime\SystemLinkMenu.lua), from a race, the lobby or a menu, and the watch
// resumes once the link is back. The host (NetRaceCore.HostInfo's second value: the host is this
// console) keeps its race, the cable taken as plugged.
std::string GuiBankLostHost(PPCContext& ctx, uint8_t* base, const std::string& name) {
  return WrapGameScript(ctx, base, name, R"lua(
local pgr4_update = GlobalUpdate
local pgr4_cable = XboxLive.EthernetCableConnected
local function pgr4_plugged() return true end
-- Player 2's sign-out (local change): Back + Start with player 2 signed in, which the host tells
-- through table.getn of pgr4_probe, 2311 long (Lua 5.1: its length; 1 when asked, else 0); the
-- game's popup, A yes (to Intro\PGR4_SignOut2.lua), B no.
local pgr4_probe = {}
for i = 1, 2311 do pgr4_probe[i] = true end
local function pgr4_signout_popup()
  if table.getn(pgr4_probe) ~= 1 then return end
  if UIGlobals.IsLoading == true or UIGlobals.CriticalUISection == true then return end
  if CustomPopup == nil or SetupCustomPopup == nil then return end
  CustomPopup.Data["PGR4_SIGNOUT_P2"] = {
    darken = true,
    title_text_ID = )lua" + Id(kSignOutTitle) + R"lua(,
    message_text_ID = )lua" + Id(kSignOutMessage) + R"lua(,
    next_text_ID = UIText.IDS_YES_A,
    back_text_ID = UIText.IDS_NO_B,
    next_screen = "Intro\\PGR4_SignOut2.lua",
  }
  SetupCustomPopup("PGR4_SIGNOUT_P2")
end
function GlobalUpdate(...)
  pgr4_signout_popup()
  if pgr4_rewatch_cable == true and pgr4_cable() == true then
    pgr4_rewatch_cable = false
    UIGlobals.MonitorSysLinkCable = true
  end
  if UIGlobals.MonitorSysLinkCable == true and UIGlobals.IsLoading == false and
     UIGlobals.CriticalUISection == false and pgr4_cable() == false then
    local active, local_host = NetRaceCore.HostInfo()
    if local_host == true then
      XboxLive.EthernetCableConnected = pgr4_plugged
      local result = pgr4_update(...)
      XboxLive.EthernetCableConnected = pgr4_cable
      return result
    end
    UIGlobals.MonitorSysLinkCable = false
    pgr4_rewatch_cable = true
    CleanUpGoingBackToStartScreen()
    NetParty.Close()
    NetRaceCore.Close()
    if UIGlobals.IsInGame == true then
      UIScreen.StoreScreen(UIEnums.ScreenStorage.FE_QUIT, "PlayTime\\SystemLinkMenu.lua")
      UIScreen.StoreScreen(UIEnums.ScreenStorage.FE_RETURN, "PlayTime\\SystemLinkMenu.lua")
      UIGlobals.QuitRaceSelected = false
      GothamSystem.QuitDebugEvent()
      UIScreen.SetNextScreen("Loading\\UILoad.lua")
    else
      UIScreen.SetNextScreen("PlayTime\\SystemLinkMenu.lua")
    end
    return false
  end
  return pgr4_update(...)
end
)lua");
}
// The game's window: its process's visible top-level window, else null.
HWND GameWindow() {
  HWND found = nullptr;
  EnumWindows(
      [](HWND hwnd, LPARAM out) -> BOOL {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER))
          return TRUE;
        *reinterpret_cast<HWND*>(out) = hwnd;
        return FALSE;
      },
      reinterpret_cast<LPARAM>(&found));
  return found;
}
// Closes the game's window as its close button does (the app shuts the title down).
void CloseGameWindow() {
  if (HWND hwnd = GameWindow()) PostMessageW(hwnd, WM_CLOSE, 0, 0);
}
// The game's window in front (src/pgr4_app.h, once it is made): the launcher that started the game
// allowed it (AllowSetForegroundWindow) before closing.
void Pgr4BringWindowToFront() {
  HWND hwnd = GameWindow();
  if (!hwnd) return;
  if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
  SetForegroundWindow(hwnd);
  BringWindowToTop(hwnd);
  SetActiveWindow(hwnd);
}

// sub_82883CC0(ui, name): finds the UIScript file `name` (its index at ui+27540).
REX_HOOK_RAW(sub_82883CC0) {
  const std::string name(reinterpret_cast<const char*>(base + ctx.r4.u32));
  if (g_names_logged < 40) {
    ++g_names_logged;
    REXLOG_INFO("PGR4 menu: script {}", name);
  }
  g_script.clear();
  g_script_pos = 0;
  if (const std::string dir = REXCVAR_GET(pgr4_script_dump_dir); !dir.empty()) {
    std::string file = name;
    for (char& c : file)
      if (c == '\\' || c == '/') c = '_';
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(dir) / file, ec);  // read again from the start
  }
  const std::string previous = std::exchange(g_last_screen, name);
  // A race's screens are Screens\Ingame\... (3-2-1-Go, HUD, pause and its pages). Only screens
  // in a folder of Screens\ count: helpers (Screens\GUIBank.lua), popups and shared pieces show
  // over either.
  {
    std::string lower(name);
    for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
    constexpr std::string_view kScreens = "screens\\";
    if (lower.rfind(kScreens, 0) == 0) {
      const std::string_view rest = std::string_view(lower).substr(kScreens.size());
      if (rest.find('\\') != std::string_view::npos && rest.rfind("shared\\", 0) != 0) {
        g_in_game_screen = rest.rfind("ingame\\", 0) == 0;
        constexpr std::string_view kSceneScreens[] = {
            "replay\\watchreplay.lua", "photomode\\photomode.lua", "photomode\\photoeffects.lua",
            "photomode\\savephoto.lua", "photomode\\uploadphoto.lua"};
        g_gtao_screen = g_in_game_screen || std::find(std::begin(kSceneScreens),
                                                      std::end(kSceneScreens),
                                                      rest) != std::end(kSceneScreens);
        g_pgr4_race_screen = g_gtao_screen;
      }
    }
  }
  g_pause_sizing = EndsWith(name, "Ingame\\PauseMenu.lua");
  if (EndsWith(name, "Ingame\\IngameOptions.lua") && g_pause_target != kPauseNone) {
    const Page page = g_pause_target == kPauseDisplay ? kDisplayPage : kGraphicsPage;
    g_pause_target = kPauseNone;
    g_from_pause = true;
    REXLOG_INFO("PGR4 menu: {} from the pause menu", page == kDisplayPage ? "DISPLAY" : "GRAPHICS");
    g_script = PageScript(page, CurrentValues());
    return;
  }
  if (EndsWith(name, "Intro\\MainMenu.lua")) g_from_pause = false;
  // The title screen: B asks to quit (StartScreenQuit). Its confirmation, once given.
  if (EndsWithNoCase(name, "Intro\\StartScreen.lua")) {
    if (std::string script = StartScreenQuit(ctx, base, name); !script.empty()) {
      g_script = std::move(script);
      return;
    }
  }
  if (EndsWithNoCase(name, "Intro\\TitleMovies.lua")) {
    if (std::string script = TitleMoviesWelcome(ctx, base, name); !script.empty()) {
      g_script = std::move(script);
      return;
    }
  }
  if (EndsWithNoCase(name, "Options\\Controllers.lua") && rex::input::PlayStationLayout()) {
    if (std::string script = ControllersPlayStation(ctx, base, name); !script.empty()) {
      g_script = std::move(script);
      return;
    }
  }
  if (EndsWithNoCase(name, "CareerMode\\Garage.lua") && REXCVAR_GET(pgr4_unlock_all)) {
    if (std::string script = GarageAllUnlocked(ctx, base, name); !script.empty()) {
      REXLOG_INFO("PGR4 menu: every garage selectable");
      g_script = std::move(script);
      return;
    }
  }
  if (EndsWithNoCase(name, "CareerMode\\CareerHome.lua") && REXCVAR_GET(pgr4_unlock_all)) {
    if (std::string script = CareerHomeTestTrack(ctx, base, name); !script.empty()) {
      REXLOG_INFO("PGR4 menu: the test track in the career's home");
      g_script = std::move(script);
      return;
    }
  }
  if (EndsWithNoCase(name, "PhotoMode\\PhotoMode.lua")) {
    if (std::string script = PhotoModeShot(ctx, base, name); !script.empty()) {
      REXLOG_INFO("PGR4 menu: the photo mode with Start for a screenshot");
      g_script = std::move(script);
      return;
    }
  }
  if (EndsWithNoCase(name, "Intro\\MultiPlayerMenu.lua")) {
    if (std::string script = MultiPlayerMenuWhiteHelp(ctx, base, name); !script.empty()) {
      g_script = std::move(script);
      return;
    }
  }
  if (EndsWithNoCase(name, "Screens\\GUIBank.lua")) {
    if (std::string script = GuiBankLostHost(ctx, base, name); !script.empty()) {
      g_script = std::move(script);
      return;
    }
  }
  if (EndsWith(name, "Intro\\PGR4_SignOut2.lua")) {
    Pgr4SignOutSecondPlayer();
    g_script = R"lua(
function Init()
  CleanUpGoingBackToStartScreen()
  if UIGlobals.IsInGame == true then
    UIScreen.StoreScreen(UIEnums.ScreenStorage.FE_QUIT, "Intro\\StartScreen.lua")
    UIScreen.StoreScreen(UIEnums.ScreenStorage.FE_RETURN, "Intro\\StartScreen.lua")
    UIGlobals.QuitRaceSelected = false
    GothamSystem.QuitDebugEvent()
    UIScreen.SetNextScreen("Loading\\UILoad.lua")
  else
    UIScreen.SetNextScreen("Intro\\StartScreen.lua")
  end
end
function StartLoop(a) return true end
function MessageUpdate(m, a, b, c, d) end
function FrameUpdate(t) end
function Render() end
function End() end
function EndLoop(a) return true end
)lua";
    return;
  }
  if (EndsWith(name, "Intro\\PGR4_Quit.lua")) {
    REXLOG_INFO("PGR4 menu: quit confirmed on the title screen");
    CloseGameWindow();
    g_script = kQuitScreen;
    return;
  }
  // PC GRAPHICS: the main menu's value 6 (the debug builds' Intro\TestArea.lua), or
  // Options\Graphics.lua.
  if (EndsWith(name, "Intro\\TestArea.lua") || EndsWith(name, "Options\\Graphics.lua")) {
    g_from_pause = false;
    if (EndsWith(name, "Intro\\TestArea.lua")) g_main_menu_return = 6.0;
    g_script = PageScript(kGraphicsPage, CurrentValues());
    return;
  }
  // PC DISPLAY: the main menu's value 99 (Xbox LIVE's session error screen, which needs a LIVE
  // session to come up otherwise).
  if (EndsWith(name, "Shared\\XboxLiveSessionError.lua") &&
      EndsWith(previous, "Intro\\MainMenu.lua")) {
    g_from_pause = false;
    g_main_menu_return = 99.0;
    g_script = PageScript(kDisplayPage, CurrentValues());
    return;
  }
  // GRAPHICS's IMAGE AND SMOOTHNESS (A on it): GRAPHICS applied, its upscaler too, and the
  // upscaler's page for it.
  if (const size_t at = name.find("Intro\\PG_"); at != std::string::npos && EndsWith(name, ".lua")) {
    const size_t first = at + std::strlen("Intro\\PG_");
    const Values v = DecodePage(name.substr(first, name.size() - 4 - first));
    ApplyPage(v);
    g_graphics_values = v;
    g_script = PageScript(kImagePage, v);
    return;
  }
  // The upscaler's page saved: applied, GRAPHICS again with it.
  if (const size_t at = name.find("Intro\\PA_"); at != std::string::npos && EndsWith(name, ".lua")) {
    const size_t first = at + std::strlen("Intro\\PA_");
    const Values v = DecodePage(name.substr(first, name.size() - 4 - first));
    ApplyPage(v);
    g_graphics_values = v;
    g_script = PageScript(kGraphicsPage, v);
    return;
  }
  // ... or left (B): GRAPHICS as it was.
  if (EndsWith(name, "Intro\\PK_.lua")) {
    g_script = PageScript(kGraphicsPage, g_graphics_values);
    return;
  }
  // The page made again for the upscaler chosen on it (nothing applied).
  if (const size_t at = name.find("Intro\\PR_"); at != std::string::npos && EndsWith(name, ".lua")) {
    const size_t first = at + std::strlen("Intro\\PR_");
    g_script = PageScript(kGraphicsPage, DecodePage(name.substr(first, name.size() - 4 - first)));
    return;
  }
  // The render scale, confirmed: saved, and the game restarts.
  if (const size_t at = name.find("Intro\\PX_"); at != std::string::npos && EndsWith(name, ".lua")) {
    const size_t first = at + std::strlen("Intro\\PX_");
    g_close_game = CloseGameWindow;
    ApplyPage(DecodePage(name.substr(first, name.size() - 4 - first)), true);
    g_script = kQuitScreen;
    return;
  }
  // Saved: applied, and the real main menu under the name the page asked for.
  size_t at = name.find("Intro\\PN_");
  if (at == std::string::npos) at = name.find("Intro\\PD_");
  if (at != std::string::npos && EndsWith(name, ".lua")) {
    const size_t first = at + std::strlen("Intro\\PN_");
    ApplyPage(DecodePage(name.substr(first, name.size() - 4 - first)));
    static uint32_t main_menu = 0;
    const std::string real =
        name.substr(0, at) + (g_from_pause ? "Ingame\\PauseMenu.lua" : "Intro\\MainMenu.lua");
    g_last_screen = real;  // the main menu shown, for PC DISPLAY's value 99 from it
    // Back to the pause menu: its frame sized for our entry too, as when it loads by its name.
    g_pause_sizing = g_from_pause;
    if (!main_menu) main_menu = rex::system::kernel_memory()->SystemHeapAlloc(256);
    if (main_menu && real.size() < 256) {
      std::memcpy(base + main_menu, real.c_str(), real.size() + 1);
      ctx.r4.u64 = main_menu;
    }
  }
  __imp__sub_82883CC0(ctx, base);
}

// sub_82883D30(buffer, size): reads the found file on, returns the bytes read. Development:
// --pgr4_script_dump_dir copies the game's scripts as read (Lua 5.1 bytecode), one file each.
REXCVAR_DEFINE_STRING(pgr4_script_dump_dir, "", "PGR4",
                      "Development: the game's UI scripts written to this folder as it reads them");
REX_HOOK_RAW(sub_82883D30) {
  if (g_script.empty()) {
    const uint32_t buffer = ctx.r3.u32;
    __imp__sub_82883D30(ctx, base);
    if (const std::string dir = REXCVAR_GET(pgr4_script_dump_dir); !dir.empty() && ctx.r3.u32) {
      std::string file = g_last_screen;
      for (char& c : file)
        if (c == '\\' || c == '/') c = '_';
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
      std::ofstream(std::filesystem::path(dir) / file, std::ios::binary | std::ios::app)
          .write(reinterpret_cast<const char*>(base + buffer), ctx.r3.u32);
    }
    return;
  }
  const size_t n = std::min<size_t>(ctx.r4.u32, g_script.size() - g_script_pos);
  std::memcpy(base + ctx.r3.u32, g_script.data() + g_script_pos, n);
  g_script_pos += n;
  ctx.r3.u64 = n;
}

// sub_8289B1D0(locmgr, hash): the UTF-16BE text of a text id.
REX_EXTERN(__imp__sub_8289B1D0);
REX_HOOK_RAW(sub_8289B1D0) {
  const uint32_t asked = ctx.r4.u32;
  // The game's texts given ours instead.
  const uint32_t id =
      asked == kIdsSystemLinkHelp    ? kTextBase + kSystemLinkHelp
      : asked == kIdsMultiPlayerHelp ? kTextBase + kMultiPlayerHelp
      : asked == kIdsLocalXenonHelp
          ? kTextBase + (rex::input::PlayStationLayout() ? kLocalXenonHelpPs : kLocalXenonHelp)
          : asked;
  if (id >= kTextBase && id < kTextBase + kTextCount) {
    const uint64_t locmgr = ctx.r3.u64;
    ctx.r4.u64 = kIdsGraphicsText;
    __imp__sub_8289B1D0(ctx, base);
    if (const uint32_t graphics = ctx.r3.u32; graphics != g_lang_graphics) {
      g_lang_graphics = graphics;
      const std::u16string name = ReadGuestText(base, graphics);
      std::u16string options;
      bool options_read = false;
      // A text manager without IDS_GRAPHICS (in a race) keeps the language the menus had.
      const Lang before = g_lang;
      for (uint32_t l = 0; l < kLangCount; ++l) {
        if (name != kGraphicsNames[l]) continue;
        if (kOptionsNames[l]) {
          if (!std::exchange(options_read, true)) {
            ctx.r3.u64 = locmgr;
            ctx.r4.u64 = kIdsOptionsText;
            __imp__sub_8289B1D0(ctx, base);
            options = ReadGuestText(base, ctx.r3.u32);
          }
          if (options != kOptionsNames[l]) continue;
        }
        g_lang = Lang(l);
        break;
      }
      if (g_lang != before) REXLOG_INFO("PGR4 menu: page language {}", uint32_t(g_lang));
    }
    if (const uint32_t addr = GuestText(g_lang, Text(id - kTextBase))) {
      ctx.r3.u64 = addr;
      return;
    }
    ctx.r3.u64 = locmgr;
    ctx.r4.u64 = asked;
  }
  __imp__sub_8289B1D0(ctx, base);
}

// ---------------------------------------------------------------------------------------------
// Main menu entries taken out: the Lua binding UIButtons.AddItem(menu, value, text, isdata) skips
// the entries whose text id is listed (an Xbox Live service that is gone), and the
// UIButtons.AddItemHelp that follows each of them.

REX_EXTERN(sub_82505EE0);  // lua_tonumber(L, index)
REX_EXTERN(sub_82505BE0);  // lua_type(L, index)

namespace {

// IDS_TOURNAMENTS (PGR4UIStrings.loc), the Title Update's XBOX LIVE MARKETPLACE (its own text
// 0x1074, main menu value 7), and the Multiplayer menu's IDS_ON_XBOX_LIVE (PLAY ON XBOX LIVE):
// its split screen and system link stay. The Options menu's IDS_CHANGE_SAVE_LOCATION (value 10)
// too: the save always goes to the one storage device the PC has.
constexpr uint32_t kHiddenTexts[] = {0x5375EAB6, 0x1074, 0x930DCDD9, 0x1C44C3BE};
// The main menu's MULTIPLAYER (value 1), after which go the game's PGR SHOP (IDS_ACHIEVEMENTS,
// value 2), PC DISPLAY (value 99) and PC GRAPHICS (value 6), our texts, then the game's OPTIONS
// (value 5); the game's own PGR SHOP and OPTIONS, added later, go. PGR ON DEMAND stays last. The
// Options menu's own GRAPHICS (value 0) goes.
constexpr uint32_t kIdsMultiPlayer = 0x49E2E16D, kIdsGraphics = 0x249A8385;
struct Entry {
  double value;
  uint32_t text, help;
  bool moved;  // the game's own entry, which it adds again later
};
constexpr Entry kEntries[] = {
    {2.0, 0x6F2C457B, 0x6A4A6D74, true},  // IDS_ACHIEVEMENTS (PGR SHOP), its help
    {99.0, kTextBase + kDisplayTitle, kTextBase + kDisplayHelp, false},
    {6.0, kTextBase + kTitle, kTextBase + kGraphicsHelp, false},
    {5.0, 0xE34ABAC1, 0x6C1C662A, true},  // IDS_OPTIONS, its help
};
bool g_insert_graphics = false;
// The main menu the entries went in, until the game's own moved ones are skipped (NaN: none).
double g_moved_menu = std::numeric_limits<double>::quiet_NaN();
int g_moved_left = 0;

uint32_t LoadBe32(const uint8_t* base, uint32_t a) {
  return uint32_t(base[a]) << 24 | uint32_t(base[a + 1]) << 16 | uint32_t(base[a + 2]) << 8 |
         base[a + 3];
}
void StoreBe32(uint8_t* base, uint32_t a, uint32_t v) {
  for (int i = 0; i < 4; ++i) base[a + i] = uint8_t(v >> (24 - 8 * i));
}
// Lua 5.1's stack (lua_State: top at +8, base at +12; TValue: big-endian double, then tt at +8,
// 16 bytes): argument `index` (from 1) of the C call on `L` set to the number `v`.
void SetLuaNumber(uint8_t* base, uint32_t L, int index, double v) {
  const uint32_t slot = LoadBe32(base, L + 12) + 16 * uint32_t(index - 1);
  uint64_t bits;
  std::memcpy(&bits, &v, 8);
  StoreBe32(base, slot, uint32_t(bits >> 32));
  StoreBe32(base, slot + 4, uint32_t(bits));
  StoreBe32(base, slot + 8, 3);  // LUA_TNUMBER
}
// Argument `index` set to the boolean `v` (Value's int b first, tt LUA_TBOOLEAN).
void SetLuaBoolean(uint8_t* base, uint32_t L, int index, bool v) {
  const uint32_t slot = LoadBe32(base, L + 12) + 16 * uint32_t(index - 1);
  StoreBe32(base, slot, v ? 1 : 0);
  StoreBe32(base, slot + 4, 0);
  StoreBe32(base, slot + 8, 1);  // LUA_TBOOLEAN
}
bool g_skip_help = false;

// Argument `index` of the Lua call on `L` as a number, NaN if it is not one; ctx kept.
double LuaNumber(PPCContext& ctx, uint8_t* base, uint32_t L, int index) {
  const PPCContext saved = ctx;
  ctx.r3.u64 = L;
  ctx.r4.s64 = index;
  sub_82505BE0(ctx, base);
  double v = std::numeric_limits<double>::quiet_NaN();
  if (ctx.r3.s32 == 3) {  // LUA_TNUMBER
    ctx = saved;
    ctx.r3.u64 = L;
    ctx.r4.s64 = index;
    sub_82505EE0(ctx, base);
    v = ctx.f1.f64;
  }
  ctx = saved;
  return v;
}

}  // namespace

REX_EXTERN(__imp__sub_82893F68);
REX_HOOK_RAW(sub_82893F68) {
  const uint32_t L = ctx.r3.u32;
  const double text = LuaNumber(ctx, base, L, 3);
  g_skip_help = false;
  if (!std::isnan(text)) {
    const uint32_t id = uint32_t(int64_t(text));
    if (id == kIdsMultiPlayer && LuaNumber(ctx, base, L, 2) == 1.0) g_insert_graphics = true;
    g_pause_sizing = false;
    if (id == kIdsOptionsText && EndsWith(g_last_screen, "PauseMenu.lua") &&
        LuaNumber(ctx, base, L, 2) == kPauseOptionsValue) {
      // The pause menu adds (menu, op, text, false): the same call's stack made each entry.
      const PPCContext saved = ctx;
      __imp__sub_82893F68(ctx, base);
      const std::pair<double, Text> entries[] = {{kPauseDisplayValue, kDisplayTitle},
                                                 {kPauseGraphicsValue, kTitle}};
      for (const auto& [value, text] : entries) {
        SetLuaNumber(base, L, 2, value);
        SetLuaNumber(base, L, 3, double(kTextBase + text));
        ctx = saved;
        __imp__sub_82893F68(ctx, base);
      }
      ctx = saved;
      ctx.r3.u64 = 0;
      return;
    }
    if (g_moved_left > 0 && LuaNumber(ctx, base, L, 1) == g_moved_menu) {
      const double value = LuaNumber(ctx, base, L, 2);
      for (const Entry& entry : kEntries)
        if (entry.moved && entry.text == id && entry.value == value) {
          if (--g_moved_left == 0) g_moved_menu = std::numeric_limits<double>::quiet_NaN();
          g_skip_help = true;
          ctx.r3.u64 = 0;
          return;
        }
    }
    if (id == kIdsGraphics && LuaNumber(ctx, base, L, 2) == 0.0) {
      g_skip_help = true;
      ctx.r3.u64 = 0;
      return;
    }
    for (const uint32_t hidden : kHiddenTexts)
      if (id == hidden) {
        g_skip_help = true;
        ctx.r3.u64 = 0;  // no results
        return;
      }
  }
  __imp__sub_82893F68(ctx, base);
}

// pgr4_ui_message_probe (development): the screens' update (sub_82882AC8: GlobalUpdate,
// StartLoop, FrameUpdate, then MessageUpdate for each queued message, the queue then emptied).
// The queue: 48-byte records from +140 (the message's name, then four floats at +32..+44), their
// number at +6284. With the byte at +10520 set, the update empties the queue and returns at once.
// Logged: each call that has a message or the byte set, with the time since the previous call.
REXCVAR_DEFINE_BOOL(pgr4_ui_message_probe, false, "PGR4",
                    "Development: the screens' queued messages (buttons) and their update, in the "
                    "log");
REX_EXTERN(__imp__sub_82882AC8);
REX_HOOK_RAW(sub_82882AC8) {
  if (REXCVAR_GET(pgr4_ui_message_probe)) {
    const uint32_t ui = ctx.r3.u32;
    const uint8_t skip = base[ui + 10520];
    const uint32_t count = LoadBe32(base, ui + 6284);
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(now - last).count();
    last = now;
    if (count || skip) {
      std::string names;
      for (uint32_t i = 0; i < std::min<uint32_t>(count, 8); ++i) {
        const char* name = reinterpret_cast<const char*>(base + ui + 140 + 48 * i);
        names += " ";
        names.append(name, strnlen(name, 32));
      }
      REXLOG_INFO("PGR4 ui messages: {} queued{}, skip {}, {:.1f} ms since the last update ({})",
                  count, names, skip, ms, g_last_screen);
    }
  }
  __imp__sub_82882AC8(ctx, base);
}

// UIButtons.GetSelection(button) (sub_82894880): its value pushed. One of the pause menu's entries
// reads as OPTIONS (its handler then asks for Ingame\IngameOptions.lua), the page noted for it.
REX_EXTERN(__imp__sub_82894880);
REX_EXTERN(sub_82894948);  // UIButtons.SetSelection(button, value)
REX_HOOK_RAW(sub_82894880) {
  const uint32_t L = ctx.r3.u32;
  // Back from PC DISPLAY or PC GRAPHICS, the main menu's first look at its menu: its entry selected
  // again, with SetSelection(menu, value) on this call's stack.
  if (!std::isnan(g_main_menu_return) && EndsWith(g_last_screen, "Intro\\MainMenu.lua")) {
    const uint32_t top = LoadBe32(base, L + 8);
    uint64_t chosen;
    const double value = g_main_menu_return;
    std::memcpy(&chosen, &value, 8);
    StoreBe32(base, top, uint32_t(chosen >> 32));
    StoreBe32(base, top + 4, uint32_t(chosen));
    StoreBe32(base, top + 8, 3);  // LUA_TNUMBER
    StoreBe32(base, L + 8, top + 16);
    const PPCContext saved = ctx;
    sub_82894948(ctx, base);
    ctx = saved;
    StoreBe32(base, L + 8, top);
    g_main_menu_return = std::numeric_limits<double>::quiet_NaN();
    REXLOG_INFO("PGR4 menu: main menu back on its entry {}", value);
  }
  __imp__sub_82894880(ctx, base);
  if (ctx.r3.u32 != 1) return;
  const uint32_t slot = LoadBe32(base, L + 8) - 16;
  if (LoadBe32(base, slot + 8) != 3) return;  // LUA_TNUMBER
  uint64_t bits = uint64_t(LoadBe32(base, slot)) << 32 | LoadBe32(base, slot + 4);
  double value;
  std::memcpy(&value, &bits, 8);
  PauseTarget target = kPauseNone;
  if (value == kPauseDisplayValue) target = kPauseDisplay;
  else if (value == kPauseGraphicsValue) target = kPauseGraphics;
  else if (value != kPauseOptionsValue) return;
  g_pause_target = target;
  if (target == kPauseNone) return;
  const double options = kPauseOptionsValue;
  std::memcpy(&bits, &options, 8);
  StoreBe32(base, slot, uint32_t(bits >> 32));
  StoreBe32(base, slot + 4, uint32_t(bits));
}

// table.getn(t) (sub_82521A00): its count pushed. While the pause menu sizes its frame, two more
// (PC DISPLAY, PC GRAPHICS) when it has more than CONTINUE and QUIT (every list but that one has OPTIONS).
// pgr4-native's renderer.cpp: the photo mode's screenshot asked, and whether it was taken.
void Pgr4AskScreenshot();
bool Pgr4ScreenshotTaken();
REX_EXTERN(__imp__sub_82521A00);
REX_HOOK_RAW(sub_82521A00) {
  const uint32_t L = ctx.r3.u32;
  __imp__sub_82521A00(ctx, base);
  if (ctx.r3.u32 != 1) return;
  const uint32_t slot = LoadBe32(base, L + 8) - 16;
  if (LoadBe32(base, slot + 8) != 3) return;  // LUA_TNUMBER
  uint64_t bits = uint64_t(LoadBe32(base, slot)) << 32 | LoadBe32(base, slot + 4);
  double count;
  std::memcpy(&count, &bits, 8);
  // Local change: GlobalUpdate's question (table.getn of its 2311-long probe): 1 once Back + Start
  // asked for player 2's sign-out (Pgr4SignOutAsked), else 0.
  // The photo mode's screenshot (PhotoModeShot): asked (2312), then taken (2313: 1 once its
  // frame is copied).
  if (count == 2312.0 || count == 2313.0) {
    if (count == 2312.0) Pgr4AskScreenshot();
    count = count == 2312.0 ? 1.0 : (Pgr4ScreenshotTaken() ? 1.0 : 0.0);
    std::memcpy(&bits, &count, 8);
    StoreBe32(base, slot, uint32_t(bits >> 32));
    StoreBe32(base, slot + 4, uint32_t(bits));
    return;
  }
  if (count == 2311.0) {
    count = Pgr4SignOutAsked() ? 1.0 : 0.0;
    std::memcpy(&bits, &count, 8);
    StoreBe32(base, slot, uint32_t(bits >> 32));
    StoreBe32(base, slot + 4, uint32_t(bits));
    return;
  }
  if (!g_pause_sizing || count <= 2) return;
  count += 2;
  std::memcpy(&bits, &count, 8);
  StoreBe32(base, slot, uint32_t(bits >> 32));
  StoreBe32(base, slot + 4, uint32_t(bits));
}

REX_EXTERN(__imp__sub_828941C8);
REX_HOOK_RAW(sub_828941C8) {
  if (std::exchange(g_skip_help, false)) {
    ctx.r3.u64 = 0;
    return;
  }
  if (std::exchange(g_insert_graphics, false)) {
    // MULTIPLAYER's help, then each entry and its help with the same call's stack: (menu, help)
    // made (menu, value, text, false) for AddItem, then (menu, help) for this.
    const PPCContext saved = ctx;
    const uint32_t L = ctx.r3.u32;
    const double menu = LuaNumber(ctx, base, L, 1);
    __imp__sub_828941C8(ctx, base);
    const uint32_t top = LoadBe32(base, L + 8), stack = LoadBe32(base, L + 12);
    // The layout checked: argument 1 where it should be.
    double first;
    const uint64_t bits = uint64_t(LoadBe32(base, stack)) << 32 | LoadBe32(base, stack + 4);
    std::memcpy(&first, &bits, 8);
    if (top != stack + 32 || LoadBe32(base, stack + 8) != 3 || first != menu) {
      REXLOG_WARN("PGR4 menu: unexpected Lua stack; no PC entries");
      return;
    }
    for (const Entry& entry : kEntries) {
      SetLuaNumber(base, L, 1, menu);
      SetLuaNumber(base, L, 2, entry.value);
      SetLuaNumber(base, L, 3, double(entry.text));
      SetLuaBoolean(base, L, 4, false);  // isdata, as the game's own calls
      StoreBe32(base, L + 8, stack + 64);
      ctx = saved;
      __imp__sub_82893F68(ctx, base);
      SetLuaNumber(base, L, 1, menu);
      SetLuaNumber(base, L, 2, double(entry.help));
      StoreBe32(base, L + 8, stack + 32);
      ctx = saved;
      __imp__sub_828941C8(ctx, base);
    }
    StoreBe32(base, L + 8, top);
    g_moved_menu = menu;
    g_moved_left = 0;
    for (const Entry& entry : kEntries) g_moved_left += entry.moved ? 1 : 0;
    REXLOG_INFO("PGR4 menu: PGR SHOP, PC DISPLAY, PC GRAPHICS and OPTIONS entries added");
    ctx = saved;
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_828941C8(ctx, base);
}

// Whether the screen shown is a race's (Ingame\...).
bool Pgr4InGameScreen() { return g_in_game_screen; }
bool Pgr4GtaoScreen() { return g_gtao_screen; }
