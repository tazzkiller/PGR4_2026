// PGR4Launcher: the PC release's launcher, beside pgr4.exe. Drawn with Direct2D in the style of
// the game's front end (light backdrop, heavy italic type, PGR red, slanted selection bar), driven
// by mouse, keyboard or an Xbox controller.
//
// It checks what the game needs (its files, Title Update 2, the Visual C++ runtime, the graphics
// card), sets the game folder, the title update, the language and the system link network in
// pgr4.toml beside the executables (game_data_root, update_data_root, content_root, user_language,
// net_lan_ip: pgr4.exe reads them the same when started on its own), and starts pgr4.exe.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <tlhelp32.h>

#include <d2d1.h>
#include <d3d12.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <dxgi1_6.h>
#include <iphlpapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <xinput.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_set>
#include <vector>

#include <SDL3/SDL.h>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------------------------
// Texts, in the game's 13 languages: the one chosen on the LANGUAGE row (at first Windows'
// display language).

enum Str {
  kPlay,
  kPlayHelp,
  kPlayBlocked,
  kGameDir,
  kGameDirHelp,
  kUpdate,
  kUpdateHelp,
  kLanguage,
  kLanguageHelp,
  kNetwork,
  kNetworkHelp,
  kAuto,
  kQuit,
  kQuitHelp,
  kVcRedist,
  kVcRedistHelp,
  kChecks,
  kCheckGame,
  kCheckUpdate,
  kCheckVc,
  kCheckGpu,
  kNotSet,
  kFound,
  kMissing,
  kTuExtracted,
  kTuPackage,
  kTuWrong,
  kVcOk,
  kVcMissing,
  kDlss,
  kFsr,
  kNoGpu,
  kSelect,
  kBack,
  kPickGame,
  kPickUpdate,
  kNoXex,
  kNoTu,
  kLaunchFail,
  kLaunchHint,
  kName,
  kNameHelp,
  kNameEditHelp,
  kNameInvalid,
  kGameWrong,
  kTuBuiltIn,
  kWaitGame,
  kGpuUnsupported,
  kDlc,
  kDlcHelp,
  kPickDlc,
  kNoDlc,
  kCheckPad,
  kPadMissing,
  kPrecompile,  // the precompile step's bar: "<text> 1240 / 3639"
  kStrCount
};

const wchar_t* const kTexts[][kStrCount] = {
    {  // en
        L"PLAY",
        L"Start Project Gotham Racing 4. Y starts it from anywhere.",
        L"Something is missing: see the checks on the right.",
        L"GAME FOLDER",
        L"The folder of your PGR4 copy, the one holding default.xex.",
        L"TITLE UPDATE 2",
        L"The Title Update 2 folder: extracted (default.xexp) or as an Xbox package.",
        L"LANGUAGE",
        L"The language of the game and of this launcher.",
        L"SYSTEM LINK NETWORK",
        L"The system link's main network. Automatic searches every network (LAN, Radmin VPN…): start the VPN before the game.",
        L"AUTOMATIC",
        L"QUIT",
        L"Close the launcher.",
        L"INSTALL VISUAL C++",
        L"The game needs the Visual C++ Redistributable 2015-2022 (x64). Opens Microsoft's download page.",
        L"CHECKS",
        L"Game",
        L"Title Update 2",
        L"Visual C++",
        L"Graphics card",
        L"Not set: A to choose",
        L"Found",
        L"Not found",
        L"Extracted",
        L"Xbox package",
        L"Unknown version",
        L"Installed",
        L"Missing",
        L"DLSS available",
        L"FSR (no DLSS)",
        L"No DirectX 12 card",
        L"SELECT",
        L"QUIT",
        L"Choose the game folder (with default.xex)",
        L"Choose the Title Update 2 folder",
        L"This folder has no default.xex.",
        L"No Title Update 2 in this folder (default.xexp or Xbox package).",
        L"Could not start pgr4.exe: it must be beside the launcher.",
        L"PLAY",
        L"PLAYER NAME",
        L"Your gamertag in the game (player 2's: the same with a 2). A to change it.",
        L"Type the name, or ↑↓ letter, ←→ position, X erase. A to confirm, B to cancel.",
        L"1 to 15 letters, digits or spaces, starting with a letter.",
        L"Not PGR4, or an unknown version",
        L"Built into the game",
        L"Choose the game first",
        L"Too old for the game: needs DirectX 12 with resource binding tier 2",
        L"DLC",
        L"The folder of your PGR4 downloadable content (Xbox packages), installed when the game starts.",
        L"Choose the DLC folder",
        L"No PGR4 DLC package in this folder.",
        L"Controller",
        L"None: connect a controller",
        L"Preparing the shaders",
    },
    {  // fr
        L"JOUER",
        L"Lancer Project Gotham Racing 4. Y le lance depuis n'importe où.",
        L"Il manque quelque chose pour jouer : voir les vérifications à droite.",
        L"DOSSIER DU JEU",
        L"Le dossier de votre copie de PGR4, celui qui contient default.xex.",
        L"TITLE UPDATE 2",
        L"Le dossier de la Title Update 2 : extraite (default.xexp) ou en paquet Xbox.",
        L"LANGUE",
        L"La langue du jeu et de ce lanceur.",
        L"RÉSEAU SYSTEM LINK",
        L"Le réseau principal du System Link. En automatique, le jeu cherche sur tous les réseaux (box, Radmin VPN…) : lancez le VPN avant le jeu.",
        L"AUTOMATIQUE",
        L"QUITTER",
        L"Fermer le lanceur.",
        L"INSTALLER VISUAL C++",
        L"Le jeu a besoin du Visual C++ Redistributable 2015-2022 (x64). Ouvre la page de téléchargement de Microsoft.",
        L"VÉRIFICATIONS",
        L"Jeu",
        L"Title Update 2",
        L"Visual C++",
        L"Carte graphique",
        L"Non défini : A pour choisir",
        L"Trouvé",
        L"Introuvable",
        L"Extraite",
        L"Paquet Xbox",
        L"Version non reconnue",
        L"Installé",
        L"Manquant",
        L"DLSS disponible",
        L"FSR (pas de DLSS)",
        L"Aucune carte DirectX 12",
        L"VALIDER",
        L"QUITTER",
        L"Choisissez le dossier du jeu (avec default.xex)",
        L"Choisissez le dossier de la Title Update 2",
        L"Ce dossier ne contient pas default.xex.",
        L"Aucune Title Update 2 dans ce dossier (default.xexp ou paquet Xbox).",
        L"Impossible de lancer pgr4.exe : il doit être à côté du lanceur.",
        L"JOUER",
        L"NOM DU JOUEUR",
        L"Votre pseudo dans le jeu (celui du joueur 2 : le même suivi de 2). A pour le modifier.",
        L"Tapez le nom, ou ↑↓ lettre, ←→ position, X effacer. A pour valider, B pour annuler.",
        L"De 1 à 15 lettres, chiffres ou espaces, en commençant par une lettre.",
        L"Pas PGR4 ou version inconnue",
        L"Intégrée au jeu",
        L"Choisissez d'abord le jeu",
        L"Trop ancienne pour le jeu : il faut DirectX 12 avec le resource binding tier 2",
        L"DLC",
        L"Le dossier de vos contenus téléchargeables PGR4 (paquets Xbox), installés au démarrage du jeu.",
        L"Choisir le dossier des DLC",
        L"Aucun paquet DLC de PGR4 dans ce dossier.",
        L"Manette",
        L"Aucune : branchez une manette",
        L"Préparation des shaders",
    },
    {  // de
        L"SPIELEN",
        L"Project Gotham Racing 4 starten. Y startet es von überall.",
        L"Es fehlt etwas: siehe die Prüfungen rechts.",
        L"SPIELORDNER",
        L"Der Ordner deiner PGR4-Kopie, der default.xex enthält.",
        L"TITLE UPDATE 2",
        L"Der Ordner des Title Update 2: entpackt (default.xexp) oder als Xbox-Paket.",
        L"SPRACHE",
        L"Die Sprache des Spiels und dieses Launchers.",
        L"SYSTEM-LINK-NETZWERK",
        L"Das Hauptnetzwerk für System Link. Automatisch sucht in allen Netzwerken (LAN, Radmin VPN…): Starte das VPN vor dem Spiel.",
        L"AUTOMATISCH",
        L"BEENDEN",
        L"Den Launcher schließen.",
        L"VISUAL C++ INSTALLIEREN",
        L"Das Spiel braucht das Visual C++ Redistributable 2015-2022 (x64). Öffnet die Download-Seite von Microsoft.",
        L"PRÜFUNGEN",
        L"Spiel",
        L"Title Update 2",
        L"Visual C++",
        L"Grafikkarte",
        L"Nicht festgelegt: A zum Auswählen",
        L"Gefunden",
        L"Nicht gefunden",
        L"Entpackt",
        L"Xbox-Paket",
        L"Unbekannte Version",
        L"Installiert",
        L"Fehlt",
        L"DLSS verfügbar",
        L"FSR (kein DLSS)",
        L"Keine DirectX-12-Karte",
        L"AUSWÄHLEN",
        L"BEENDEN",
        L"Wähle den Spielordner (mit default.xex)",
        L"Wähle den Ordner des Title Update 2",
        L"Dieser Ordner enthält keine default.xex.",
        L"Kein Title Update 2 in diesem Ordner (default.xexp oder Xbox-Paket).",
        L"pgr4.exe konnte nicht gestartet werden: Es muss neben dem Launcher liegen.",
        L"SPIELEN",
        L"SPIELERNAME",
        L"Dein Gamertag im Spiel (Spieler 2: derselbe mit einer 2). A zum Ändern.",
        L"Tippe den Namen, oder ↑↓ Buchstabe, ←→ Position, X löschen. A bestätigen, B abbrechen.",
        L"1 bis 15 Buchstaben, Ziffern oder Leerzeichen, beginnend mit einem Buchstaben.",
        L"Kein PGR4 oder unbekannte Version",
        L"Im Spiel enthalten",
        L"Wähle zuerst das Spiel",
        L"Zu alt für das Spiel: DirectX 12 mit Resource Binding Tier 2 nötig",
        L"DLC",
        L"Der Ordner deiner PGR4-Zusatzinhalte (Xbox-Pakete), beim Spielstart installiert.",
        L"DLC-Ordner wählen",
        L"Kein PGR4-DLC-Paket in diesem Ordner.",
        L"Controller",
        L"Keiner: Controller anschließen",
        L"Shader werden vorbereitet",
    },
    {  // es
        L"JUGAR",
        L"Iniciar Project Gotham Racing 4. Y lo inicia desde cualquier lugar.",
        L"Falta algo para jugar: mira las comprobaciones a la derecha.",
        L"CARPETA DEL JUEGO",
        L"La carpeta de tu copia de PGR4, la que contiene default.xex.",
        L"TITLE UPDATE 2",
        L"La carpeta de la Title Update 2: extraída (default.xexp) o como paquete de Xbox.",
        L"IDIOMA",
        L"El idioma del juego y de este lanzador.",
        L"RED SYSTEM LINK",
        L"La red principal del System Link. En automático, el juego busca en todas las redes (router, Radmin VPN…): inicia la VPN antes del juego.",
        L"AUTOMÁTICA",
        L"SALIR",
        L"Cerrar el lanzador.",
        L"INSTALAR VISUAL C++",
        L"El juego necesita Visual C++ Redistributable 2015-2022 (x64). Abre la página de descarga de Microsoft.",
        L"COMPROBACIONES",
        L"Juego",
        L"Title Update 2",
        L"Visual C++",
        L"Tarjeta gráfica",
        L"Sin definir: A para elegir",
        L"Encontrado",
        L"No encontrado",
        L"Extraída",
        L"Paquete de Xbox",
        L"Versión no reconocida",
        L"Instalado",
        L"Falta",
        L"DLSS disponible",
        L"FSR (sin DLSS)",
        L"Ninguna tarjeta DirectX 12",
        L"ACEPTAR",
        L"SALIR",
        L"Elige la carpeta del juego (con default.xex)",
        L"Elige la carpeta de la Title Update 2",
        L"Esta carpeta no contiene default.xex.",
        L"No hay Title Update 2 en esta carpeta (default.xexp o paquete de Xbox).",
        L"No se pudo iniciar pgr4.exe: debe estar junto al lanzador.",
        L"JUGAR",
        L"NOMBRE DEL JUGADOR",
        L"Tu gamertag en el juego (el del jugador 2: el mismo con un 2). A para cambiarlo.",
        L"Escribe el nombre, o ↑↓ letra, ←→ posición, X borrar. A para aceptar, B para cancelar.",
        L"De 1 a 15 letras, números o espacios, empezando por una letra.",
        L"No es PGR4 o versión desconocida",
        L"Integrada en el juego",
        L"Elige primero el juego",
        L"Demasiado antigua para el juego: requiere DirectX 12 con resource binding tier 2",
        L"DLC",
        L"La carpeta de tu contenido descargable de PGR4 (paquetes de Xbox), instalado al iniciar el juego.",
        L"Elegir la carpeta de DLC",
        L"No hay ningún paquete DLC de PGR4 en esta carpeta.",
        L"Mando",
        L"Ninguno: conecta un mando",
        L"Preparando los shaders",
    },
    {  // it
        L"GIOCA",
        L"Avvia Project Gotham Racing 4. Y lo avvia da qualsiasi punto.",
        L"Manca qualcosa per giocare: vedi i controlli a destra.",
        L"CARTELLA DEL GIOCO",
        L"La cartella della tua copia di PGR4, quella che contiene default.xex.",
        L"TITLE UPDATE 2",
        L"La cartella della Title Update 2: estratta (default.xexp) o come pacchetto Xbox.",
        L"LINGUA",
        L"La lingua del gioco e di questo launcher.",
        L"RETE SYSTEM LINK",
        L"La rete principale del System Link. In automatico, il gioco cerca su tutte le reti (router, Radmin VPN…): avvia la VPN prima del gioco.",
        L"AUTOMATICA",
        L"ESCI",
        L"Chiudi il launcher.",
        L"INSTALLA VISUAL C++",
        L"Il gioco richiede Visual C++ Redistributable 2015-2022 (x64). Apre la pagina di download di Microsoft.",
        L"CONTROLLI",
        L"Gioco",
        L"Title Update 2",
        L"Visual C++",
        L"Scheda video",
        L"Non impostata: A per scegliere",
        L"Trovato",
        L"Non trovato",
        L"Estratta",
        L"Pacchetto Xbox",
        L"Versione non riconosciuta",
        L"Installato",
        L"Mancante",
        L"DLSS disponibile",
        L"FSR (niente DLSS)",
        L"Nessuna scheda DirectX 12",
        L"CONFERMA",
        L"ESCI",
        L"Scegli la cartella del gioco (con default.xex)",
        L"Scegli la cartella della Title Update 2",
        L"Questa cartella non contiene default.xex.",
        L"Nessuna Title Update 2 in questa cartella (default.xexp o pacchetto Xbox).",
        L"Impossibile avviare pgr4.exe: deve trovarsi accanto al launcher.",
        L"GIOCA",
        L"NOME GIOCATORE",
        L"Il tuo gamertag nel gioco (quello del giocatore 2: lo stesso con un 2). A per cambiarlo.",
        L"Scrivi il nome, oppure ↑↓ lettera, ←→ posizione, X cancella. A per confermare, B per annullare.",
        L"Da 1 a 15 lettere, cifre o spazi, iniziando con una lettera.",
        L"Non è PGR4 o versione sconosciuta",
        L"Integrata nel gioco",
        L"Scegli prima il gioco",
        L"Troppo vecchia per il gioco: serve DirectX 12 con resource binding tier 2",
        L"DLC",
        L"La cartella dei contenuti scaricabili di PGR4 (pacchetti Xbox), installati all'avvio del gioco.",
        L"Scegli la cartella dei DLC",
        L"Nessun pacchetto DLC di PGR4 in questa cartella.",
        L"Controller",
        L"Nessuno: collega un controller",
        L"Preparazione degli shader",
    },
    {  // cz
        L"HRÁT",
        L"Spustit Project Gotham Racing 4. Tlačítko Y ji spustí odkudkoli.",
        L"Něco chybí: viz kontroly vpravo.",
        L"SLOŽKA HRY",
        L"Složka vaší kopie PGR4, ta se souborem default.xex.",
        L"TITLE UPDATE 2",
        L"Složka Title Update 2: rozbalená (default.xexp) nebo jako balíček Xbox.",
        L"JAZYK",
        L"Jazyk hry a tohoto spouštěče.",
        L"SÍŤ SYSTEM LINK",
        L"Hlavní síť pro System Link. Automaticky hra hledá ve všech sítích (LAN, Radmin VPN…): spusťte VPN před hrou.",
        L"AUTOMATICKY",
        L"KONEC",
        L"Zavřít spouštěč.",
        L"INSTALOVAT VISUAL C++",
        L"Hra potřebuje Visual C++ Redistributable 2015-2022 (x64). Otevře stránku pro stažení od Microsoftu.",
        L"KONTROLY",
        L"Hra",
        L"Title Update 2",
        L"Visual C++",
        L"Grafická karta",
        L"Nenastaveno: A pro výběr",
        L"Nalezeno",
        L"Nenalezeno",
        L"Rozbalená",
        L"Balíček Xbox",
        L"Neznámá verze",
        L"Nainstalováno",
        L"Chybí",
        L"DLSS k dispozici",
        L"FSR (bez DLSS)",
        L"Žádná karta DirectX 12",
        L"POTVRDIT",
        L"KONEC",
        L"Vyberte složku hry (s default.xex)",
        L"Vyberte složku Title Update 2",
        L"Tato složka neobsahuje default.xex.",
        L"V této složce není Title Update 2 (default.xexp ani balíček Xbox).",
        L"pgr4.exe nelze spustit: musí být vedle spouštěče.",
        L"HRÁT",
        L"JMÉNO HRÁČE",
        L"Vaše jméno ve hře (hráč 2: stejné s číslem 2). A pro změnu.",
        L"Napište jméno, nebo ↑↓ písmeno, ←→ pozice, X smazat. A potvrdit, B zrušit.",
        L"1 až 15 písmen, číslic nebo mezer, na začátku písmeno.",
        L"Není to PGR4 nebo neznámá verze",
        L"Součástí hry",
        L"Nejprve vyberte hru",
        L"Pro hru příliš stará: vyžaduje DirectX 12 s resource binding tier 2",
        L"DLC",
        L"Složka se stahovatelným obsahem PGR4 (balíčky Xbox), instalovaným při spuštění hry.",
        L"Vyberte složku s DLC",
        L"V této složce není žádný balíček DLC pro PGR4.",
        L"Ovladač",
        L"Žádný: připojte ovladač",
        L"Příprava shaderů",
    },
    {  // hu
        L"JÁTÉK",
        L"A Project Gotham Racing 4 indítása. Az Y bárhonnan elindítja.",
        L"Valami hiányzik: lásd az ellenőrzéseket jobbra.",
        L"JÁTÉK MAPPÁJA",
        L"A PGR4-másolatod mappája, amelyben a default.xex van.",
        L"TITLE UPDATE 2",
        L"A Title Update 2 mappája: kicsomagolva (default.xexp) vagy Xbox-csomagként.",
        L"NYELV",
        L"A játék és az indító nyelve.",
        L"SYSTEM LINK HÁLÓZAT",
        L"A System Link fő hálózata. Automatikus módban a játék minden hálózaton keres (LAN, Radmin VPN…): a VPN-t a játék előtt indítsd el.",
        L"AUTOMATIKUS",
        L"KILÉPÉS",
        L"Az indító bezárása.",
        L"VISUAL C++ TELEPÍTÉSE",
        L"A játékhoz Visual C++ Redistributable 2015-2022 (x64) kell. Megnyitja a Microsoft letöltési oldalát.",
        L"ELLENŐRZÉSEK",
        L"Játék",
        L"Title Update 2",
        L"Visual C++",
        L"Videókártya",
        L"Nincs beállítva: A a kiválasztáshoz",
        L"Megvan",
        L"Nem található",
        L"Kicsomagolva",
        L"Xbox-csomag",
        L"Ismeretlen verzió",
        L"Telepítve",
        L"Hiányzik",
        L"DLSS elérhető",
        L"FSR (nincs DLSS)",
        L"Nincs DirectX 12-es kártya",
        L"KIVÁLASZTÁS",
        L"KILÉPÉS",
        L"Válaszd ki a játék mappáját (default.xex-szel)",
        L"Válaszd ki a Title Update 2 mappáját",
        L"Ebben a mappában nincs default.xex.",
        L"Ebben a mappában nincs Title Update 2 (default.xexp vagy Xbox-csomag).",
        L"A pgr4.exe nem indítható: az indító mellett kell lennie.",
        L"JÁTÉK",
        L"JÁTÉKOSNÉV",
        L"A játékosneved a játékban (a 2. játékosé: ugyanez 2-essel). A a módosításhoz.",
        L"Írd be a nevet, vagy ↑↓ betű, ←→ pozíció, X törlés. A jóváhagyás, B mégse.",
        L"1–15 betű, számjegy vagy szóköz, betűvel kezdve.",
        L"Nem PGR4, vagy ismeretlen verzió",
        L"A játék tartalmazza",
        L"Előbb válaszd ki a játékot",
        L"Túl régi a játékhoz: DirectX 12 kell resource binding tier 2-vel",
        L"DLC",
        L"A PGR4 letölthető tartalmainak mappája (Xbox-csomagok), a játék indításakor települnek.",
        L"Válaszd ki a DLC mappát",
        L"Ebben a mappában nincs PGR4 DLC-csomag.",
        L"Kontroller",
        L"Nincs: csatlakoztass egy kontrollert",
        L"Árnyalók előkészítése",
    },
    {  // pl
        L"GRAJ",
        L"Uruchom Project Gotham Racing 4. Y uruchamia grę z dowolnego miejsca.",
        L"Czegoś brakuje: zobacz sprawdzenia po prawej.",
        L"FOLDER GRY",
        L"Folder twojej kopii PGR4, ten z plikiem default.xex.",
        L"TITLE UPDATE 2",
        L"Folder Title Update 2: rozpakowanej (default.xexp) lub jako pakiet Xbox.",
        L"JĘZYK",
        L"Język gry i tego launchera.",
        L"SIEĆ SYSTEM LINK",
        L"Główna sieć System Link. W trybie automatycznym gra szuka we wszystkich sieciach (LAN, Radmin VPN…): uruchom VPN przed grą.",
        L"AUTOMATYCZNIE",
        L"WYJDŹ",
        L"Zamknij launcher.",
        L"ZAINSTALUJ VISUAL C++",
        L"Gra wymaga Visual C++ Redistributable 2015-2022 (x64). Otwiera stronę pobierania Microsoftu.",
        L"SPRAWDZENIA",
        L"Gra",
        L"Title Update 2",
        L"Visual C++",
        L"Karta graficzna",
        L"Nie ustawiono: A, aby wybrać",
        L"Znaleziono",
        L"Nie znaleziono",
        L"Rozpakowana",
        L"Pakiet Xbox",
        L"Nieznana wersja",
        L"Zainstalowano",
        L"Brak",
        L"DLSS dostępny",
        L"FSR (bez DLSS)",
        L"Brak karty DirectX 12",
        L"WYBIERZ",
        L"WYJDŹ",
        L"Wybierz folder gry (z default.xex)",
        L"Wybierz folder Title Update 2",
        L"Ten folder nie zawiera default.xex.",
        L"Brak Title Update 2 w tym folderze (default.xexp lub pakiet Xbox).",
        L"Nie można uruchomić pgr4.exe: musi być obok launchera.",
        L"GRAJ",
        L"NAZWA GRACZA",
        L"Twoja nazwa w grze (gracz 2: ta sama z cyfrą 2). A, aby zmienić.",
        L"Wpisz nazwę lub ↑↓ litera, ←→ pozycja, X usuń. A zatwierdź, B anuluj.",
        L"Od 1 do 15 liter, cyfr lub spacji, zaczynając od litery.",
        L"To nie PGR4 lub nieznana wersja",
        L"Wbudowana w grę",
        L"Najpierw wybierz grę",
        L"Zbyt stara dla gry: wymaga DirectX 12 z resource binding tier 2",
        L"DLC",
        L"Folder z zawartością do pobrania PGR4 (pakiety Xbox), instalowaną przy uruchomieniu gry.",
        L"Wybierz folder DLC",
        L"W tym folderze nie ma żadnego pakietu DLC do PGR4.",
        L"Kontroler",
        L"Brak: podłącz kontroler",
        L"Przygotowywanie shaderów",
    },
    {  // ru
        L"ИГРАТЬ",
        L"Запустить Project Gotham Racing 4. Кнопка Y запускает игру откуда угодно.",
        L"Чего-то не хватает: см. проверки справа.",
        L"ПАПКА ИГРЫ",
        L"Папка вашей копии PGR4, в которой лежит default.xex.",
        L"TITLE UPDATE 2",
        L"Папка Title Update 2: распакованная (default.xexp) или пакет Xbox.",
        L"ЯЗЫК",
        L"Язык игры и этого лаунчера.",
        L"СЕТЬ SYSTEM LINK",
        L"Основная сеть для System Link. В автоматическом режиме игра ищет во всех сетях (LAN, Radmin VPN…): запустите VPN до игры.",
        L"АВТОМАТИЧЕСКИ",
        L"ВЫХОД",
        L"Закрыть лаунчер.",
        L"УСТАНОВИТЬ VISUAL C++",
        L"Игре нужен Visual C++ Redistributable 2015-2022 (x64). Открывает страницу загрузки Microsoft.",
        L"ПРОВЕРКИ",
        L"Игра",
        L"Title Update 2",
        L"Visual C++",
        L"Видеокарта",
        L"Не задано: A, чтобы выбрать",
        L"Найдено",
        L"Не найдено",
        L"Распакована",
        L"Пакет Xbox",
        L"Неизвестная версия",
        L"Установлен",
        L"Отсутствует",
        L"DLSS доступен",
        L"FSR (без DLSS)",
        L"Нет видеокарты DirectX 12",
        L"ВЫБРАТЬ",
        L"ВЫХОД",
        L"Выберите папку игры (с default.xex)",
        L"Выберите папку Title Update 2",
        L"В этой папке нет default.xex.",
        L"В этой папке нет Title Update 2 (default.xexp или пакета Xbox).",
        L"Не удалось запустить pgr4.exe: он должен лежать рядом с лаунчером.",
        L"ИГРАТЬ",
        L"ИМЯ ИГРОКА",
        L"Ваше имя в игре (у игрока 2 — то же с цифрой 2). A — изменить.",
        L"Введите имя или ↑↓ буква, ←→ позиция, X стереть. A — подтвердить, B — отмена.",
        L"От 1 до 15 латинских букв, цифр или пробелов, первой — буква.",
        L"Не PGR4 или неизвестная версия",
        L"Встроена в игру",
        L"Сначала выберите игру",
        L"Слишком старая для игры: нужен DirectX 12 с resource binding tier 2",
        L"DLC",
        L"Папка с загружаемым контентом PGR4 (пакеты Xbox), устанавливается при запуске игры.",
        L"Выберите папку DLC",
        L"В этой папке нет пакетов DLC для PGR4.",
        L"Геймпад",
        L"Нет: подключите геймпад",
        L"Подготовка шейдеров",
    },
    {  // ja
        L"プレイ",
        L"Project Gotham Racing 4 を起動します。Y ボタンでどこからでも起動できます。",
        L"足りないものがあります。右のチェックを確認してください。",
        L"ゲームフォルダー",
        L"PGR4 のコピーのフォルダー（default.xex があるフォルダー）。",
        L"TITLE UPDATE 2",
        L"Title Update 2 のフォルダー：展開済み（default.xexp）または Xbox パッケージ。",
        L"言語",
        L"ゲームとこのランチャーの言語。",
        L"システムリンクのネットワーク",
        L"システムリンクのメインネットワーク。自動ではすべてのネットワーク（LAN、Radmin VPN…）を検索します。ゲームの前に VPN を起動してください。",
        L"自動",
        L"終了",
        L"ランチャーを閉じます。",
        L"VISUAL C++ をインストール",
        L"ゲームには Visual C++ Redistributable 2015-2022 (x64) が必要です。Microsoft のダウンロードページを開きます。",
        L"チェック",
        L"ゲーム",
        L"Title Update 2",
        L"Visual C++",
        L"グラフィックカード",
        L"未設定：A で選択",
        L"見つかりました",
        L"見つかりません",
        L"展開済み",
        L"Xbox パッケージ",
        L"不明なバージョン",
        L"インストール済み",
        L"未インストール",
        L"DLSS 対応",
        L"FSR（DLSS なし）",
        L"DirectX 12 対応カードなし",
        L"決定",
        L"終了",
        L"ゲームフォルダーを選択（default.xex を含む）",
        L"Title Update 2 のフォルダーを選択",
        L"このフォルダーには default.xex がありません。",
        L"このフォルダーに Title Update 2 がありません（default.xexp または Xbox パッケージ）。",
        L"pgr4.exe を起動できません。ランチャーと同じフォルダーに置いてください。",
        L"プレイ",
        L"プレイヤー名",
        L"ゲーム内のゲーマータグ（プレイヤー 2 は同じ名前に 2 が付きます）。A で変更。",
        L"名前を入力、または ↑↓ 文字、←→ 位置、X 削除。A で決定、B でキャンセル。",
        L"1〜15 文字の英字・数字・スペースで、先頭は英字にしてください。",
        L"PGR4 ではないか、不明なバージョンです",
        L"ゲームに適用済み",
        L"先にゲームを選択してください",
        L"ゲームには古すぎます：resource binding tier 2 対応の DirectX 12 が必要です",
        L"DLC",
        L"PGR4 のダウンロードコンテンツ (Xbox パッケージ) のフォルダー。ゲームの起動時にインストールされます。",
        L"DLC フォルダーを選択",
        L"このフォルダーに PGR4 の DLC パッケージがありません。",
        L"コントローラー",
        L"なし：コントローラーを接続してください",
        L"シェーダーを準備中",
    },
    {  // ko
        L"플레이",
        L"Project Gotham Racing 4를 시작합니다. 어디서든 Y 버튼으로 시작할 수 있습니다.",
        L"무언가 빠져 있습니다. 오른쪽 확인 항목을 보세요.",
        L"게임 폴더",
        L"default.xex가 있는 PGR4 사본 폴더입니다.",
        L"TITLE UPDATE 2",
        L"Title Update 2 폴더: 압축 해제됨(default.xexp) 또는 Xbox 패키지.",
        L"언어",
        L"게임과 이 런처의 언어입니다.",
        L"시스템 링크 네트워크",
        L"시스템 링크의 기본 네트워크입니다. 자동으로 두면 모든 네트워크(LAN, Radmin VPN…)를 검색합니다. 게임 전에 VPN을 시작하세요.",
        L"자동",
        L"종료",
        L"런처를 닫습니다.",
        L"VISUAL C++ 설치",
        L"게임에 Visual C++ Redistributable 2015-2022 (x64)가 필요합니다. Microsoft 다운로드 페이지를 엽니다.",
        L"확인 항목",
        L"게임",
        L"Title Update 2",
        L"Visual C++",
        L"그래픽 카드",
        L"설정 안 됨: A로 선택",
        L"찾음",
        L"찾을 수 없음",
        L"압축 해제됨",
        L"Xbox 패키지",
        L"알 수 없는 버전",
        L"설치됨",
        L"없음",
        L"DLSS 사용 가능",
        L"FSR (DLSS 없음)",
        L"DirectX 12 카드 없음",
        L"선택",
        L"종료",
        L"게임 폴더 선택 (default.xex 포함)",
        L"Title Update 2 폴더 선택",
        L"이 폴더에 default.xex가 없습니다.",
        L"이 폴더에 Title Update 2가 없습니다 (default.xexp 또는 Xbox 패키지).",
        L"pgr4.exe를 시작할 수 없습니다. 런처와 같은 폴더에 있어야 합니다.",
        L"플레이",
        L"플레이어 이름",
        L"게임 속 게이머태그입니다(플레이어 2는 같은 이름 뒤에 2). A로 변경.",
        L"이름을 입력하거나 ↑↓ 글자, ←→ 위치, X 지우기. A 확인, B 취소.",
        L"영문자, 숫자, 공백 1~15자이며 영문자로 시작해야 합니다.",
        L"PGR4가 아니거나 알 수 없는 버전",
        L"게임에 포함됨",
        L"먼저 게임을 선택하세요",
        L"게임에 비해 너무 오래되었습니다: resource binding tier 2를 지원하는 DirectX 12가 필요합니다",
        L"DLC",
        L"PGR4 다운로드 콘텐츠(Xbox 패키지) 폴더입니다. 게임 시작 시 설치됩니다.",
        L"DLC 폴더 선택",
        L"이 폴더에 PGR4 DLC 패키지가 없습니다.",
        L"컨트롤러",
        L"없음: 컨트롤러를 연결하세요",
        L"셰이더 준비 중",
    },
    {  // tc
        L"開始遊戲",
        L"啟動 Project Gotham Racing 4。在任何位置按 Y 即可啟動。",
        L"缺少必要內容：請查看右側的檢查項目。",
        L"遊戲資料夾",
        L"你的 PGR4 副本所在的資料夾（包含 default.xex）。",
        L"TITLE UPDATE 2",
        L"Title Update 2 的資料夾：已解壓縮（default.xexp）或 Xbox 套件。",
        L"語言",
        L"遊戲與本啟動器的語言。",
        L"系統連結網路",
        L"系統連結的主要網路。自動模式會搜尋所有網路（區域網路、Radmin VPN…）：請在遊戲前啟動 VPN。",
        L"自動",
        L"結束",
        L"關閉啟動器。",
        L"安裝 VISUAL C++",
        L"遊戲需要 Visual C++ Redistributable 2015-2022 (x64)。開啟 Microsoft 下載頁面。",
        L"檢查",
        L"遊戲",
        L"Title Update 2",
        L"Visual C++",
        L"顯示卡",
        L"未設定：按 A 選擇",
        L"已找到",
        L"找不到",
        L"已解壓縮",
        L"Xbox 套件",
        L"無法辨識的版本",
        L"已安裝",
        L"缺少",
        L"支援 DLSS",
        L"FSR（無 DLSS）",
        L"沒有 DirectX 12 顯示卡",
        L"確認",
        L"結束",
        L"選擇遊戲資料夾（包含 default.xex）",
        L"選擇 Title Update 2 資料夾",
        L"此資料夾中沒有 default.xex。",
        L"此資料夾中沒有 Title Update 2（default.xexp 或 Xbox 套件）。",
        L"無法啟動 pgr4.exe：它必須與啟動器位於同一資料夾。",
        L"開始遊戲",
        L"玩家名稱",
        L"你在遊戲中的玩家代號（玩家 2 為同名加 2）。按 A 修改。",
        L"輸入名稱，或 ↑↓ 字母、←→ 位置、X 刪除。A 確認，B 取消。",
        L"1 到 15 個英文字母、數字或空格，並以字母開頭。",
        L"不是 PGR4 或版本不明",
        L"已整合至遊戲",
        L"請先選擇遊戲",
        L"對本遊戲而言過舊：需要支援 resource binding tier 2 的 DirectX 12",
        L"DLC",
        L"PGR4 下載內容 (Xbox 套件) 的資料夾，於遊戲啟動時安裝。",
        L"選擇 DLC 資料夾",
        L"此資料夾中沒有 PGR4 的 DLC 套件。",
        L"控制器",
        L"無：請連接控制器",
        L"正在準備著色器",
    },
    {  // sc
        L"开始游戏",
        L"启动 Project Gotham Racing 4。在任何位置按 Y 即可启动。",
        L"缺少必要内容：请查看右侧的检查项。",
        L"游戏文件夹",
        L"你的 PGR4 副本所在的文件夹（包含 default.xex）。",
        L"TITLE UPDATE 2",
        L"Title Update 2 的文件夹：已解压（default.xexp）或 Xbox 安装包。",
        L"语言",
        L"游戏和本启动器的语言。",
        L"系统链接网络",
        L"系统链接的主网络。自动模式会搜索所有网络（局域网、Radmin VPN…）：请在游戏前启动 VPN。",
        L"自动",
        L"退出",
        L"关闭启动器。",
        L"安装 VISUAL C++",
        L"游戏需要 Visual C++ Redistributable 2015-2022 (x64)。打开 Microsoft 下载页面。",
        L"检查",
        L"游戏",
        L"Title Update 2",
        L"Visual C++",
        L"显卡",
        L"未设置：按 A 选择",
        L"已找到",
        L"未找到",
        L"已解压",
        L"Xbox 安装包",
        L"无法识别的版本",
        L"已安装",
        L"缺失",
        L"支持 DLSS",
        L"FSR（无 DLSS）",
        L"没有 DirectX 12 显卡",
        L"确认",
        L"退出",
        L"选择游戏文件夹（包含 default.xex）",
        L"选择 Title Update 2 文件夹",
        L"此文件夹中没有 default.xex。",
        L"此文件夹中没有 Title Update 2（default.xexp 或 Xbox 安装包）。",
        L"无法启动 pgr4.exe：它必须与启动器位于同一文件夹。",
        L"开始游戏",
        L"玩家名称",
        L"你在游戏中的玩家代号（玩家 2 为同名加 2）。按 A 修改。",
        L"输入名称，或 ↑↓ 字母、←→ 位置、X 删除。A 确认，B 取消。",
        L"1 到 15 个英文字母、数字或空格，并以字母开头。",
        L"不是 PGR4 或版本未知",
        L"已集成到游戏中",
        L"请先选择游戏",
        L"对本游戏而言过旧：需要支持 resource binding tier 2 的 DirectX 12",
        L"DLC",
        L"PGR4 下载内容 (Xbox 包) 的文件夹，在游戏启动时安装。",
        L"选择 DLC 文件夹",
        L"此文件夹中没有 PGR4 的 DLC 包。",
        L"控制器",
        L"无：请连接控制器",
        L"正在准备着色器",
    },
};

// The LANGUAGE spinner: each language's own name, its XLanguage (user_language) and, for Czech and
// Hungarian (no XLanguage of their own: English on a console of that country), its user_country;
// the launcher's texts in it (kTexts).
struct Language {
  const wchar_t* name;
  int value;
  int country;
};
const Language kLanguages[] = {
    {L"ENGLISH", 1, 0},
    {L"FRANÇAIS", 4, 0},
    {L"DEUTSCH", 3, 0},
    {L"ESPAÑOL", 5, 0},
    {L"ITALIANO", 6, 0},
    {L"ČEŠTINA", 1, 23},
    {L"MAGYAR", 1, 42},
    {L"POLSKI", 11, 0},
    {L"РУССКИЙ", 12, 0},
    {L"日本語", 2, 0},
    {L"한국어", 7, 0},
    {L"繁體中文", 8, 0},
    {L"简体中文", 10, 0},
};

const wchar_t* const* g_texts = kTexts[0];
// The controller in use is a PlayStation one (PollPad), or pgr4.toml's force_playstation_layout
// (the game's too, for testing without one): its symbols in place of A, B, X and Y.
bool g_playstation = false;
bool g_force_playstation = false;

// A text; with a PlayStation controller, the button letters of those that name some (a capital A,
// B, X or Y on its own, not first) as its symbols.
const wchar_t* T(Str s) {
  if (!g_playstation || (s != kPlayHelp && s != kNotSet && s != kNameHelp && s != kNameEditHelp))
    return g_texts[s];
  static std::wstring texts[kStrCount];
  static const wchar_t* sources[kStrCount] = {};
  std::wstring& out = texts[s];
  if (sources[s] != g_texts[s]) {
    sources[s] = g_texts[s];
    out = g_texts[s];
    auto letter = [](wchar_t c) { return iswalpha(c) != 0; };
    for (size_t i = 0; i < out.size(); ++i) {
      const wchar_t c = out[i];
      // Not first: Hungarian's article A starts some (no text starts with a button).
      if ((c != L'A' && c != L'B' && c != L'X' && c != L'Y') || i == 0 || letter(out[i - 1]) ||
          (i + 1 < out.size() && letter(out[i + 1])))
        continue;
      out[i] = c == L'A' ? L'✕' : c == L'B' ? L'○' : c == L'X' ? L'□' : L'△';
    }
  }
  return out.c_str();
}

// The player's name when none was chosen yet.
constexpr wchar_t kDefaultName[] = L"PGR4 PLAYER";
constexpr size_t kMaxName = 15;

// The game's versions (XEX execution info): the disc's, and with Title Update 2 (its patch's
// delta descriptor goes from the first to the second), the one the code was recompiled with.
constexpr uint32_t kBaseVersion = 0x00000001;
constexpr uint32_t kTu2Version = 0x00000201;
constexpr uint32_t kTitleId = 0x4D5307F9;
constexpr uint32_t kInstallerContent = 0x000B0000;  // title updates' STFS content type

// ---------------------------------------------------------------------------------------------
// Settings in pgr4.toml (flat key = value lines; other lines and comments kept).

fs::path g_dir;  // the launcher's (and pgr4.exe's) folder

std::wstring Widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

std::string Narrow(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr,
                                    nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

std::vector<fs::path> TomlPaths() {
  std::vector<fs::path> paths = {g_dir / "pgr4.toml"};
#ifdef PGR4_SOURCE_TOML
  // The project's copy too: the build copies it beside the executables.
  // Local change (31/10/2026): only from the build folder. A copy elsewhere (the desktop
  // release) wrote its own folders into the project's, which the next build copied back.
  if (std::error_code ec; fs::exists(PGR4_SOURCE_TOML) && fs::equivalent(g_dir, PGR4_BUILD_DIR, ec))
    paths.push_back(PGR4_SOURCE_TOML);
#endif
  return paths;
}

std::string ReadSetting(const std::string& key) {
  std::ifstream in(g_dir / "pgr4.toml");
  for (std::string line; std::getline(in, line);) {
    const size_t start = line.find_first_not_of(" \t");
    if (start == std::string::npos || line.compare(start, key.size(), key) != 0) continue;
    size_t eq = line.find_first_not_of(" \t", start + key.size());
    if (eq == std::string::npos || line[eq] != '=') continue;
    std::string value = line.substr(line.find_first_not_of(" \t", eq + 1) == std::string::npos
                                        ? line.size()
                                        : line.find_first_not_of(" \t", eq + 1));
    if (!value.empty() && (value[0] == '"' || value[0] == '\'')) {
      const size_t end = value.find(value[0], 1);
      value = value.substr(1, end == std::string::npos ? std::string::npos : end - 1);
    } else {
      value = value.substr(0, value.find_first_of(" \t#"));
    }
    return value;
  }
  return {};
}

// `value` as written (quoted by the caller when a string).
void WriteSetting(const std::string& key, const std::string& value) {
  for (const fs::path& path : TomlPaths()) {
    std::vector<std::string> lines;
    {
      std::ifstream in(path);
      for (std::string line; std::getline(in, line);) lines.push_back(line);
    }
    bool found = false;
    for (std::string& line : lines) {
      const size_t start = line.find_first_not_of(" \t");
      if (start == std::string::npos || line.compare(start, key.size(), key) != 0) continue;
      const size_t eq = line.find_first_not_of(" \t", start + key.size());
      if (eq == std::string::npos || line[eq] != '=') continue;
      line = key + " = " + value;
      found = true;
    }
    if (!found) lines.push_back(key + " = " + value);
    std::ofstream out(path, std::ios::trunc);
    for (const std::string& line : lines) out << line << '\n';
  }
}

// pgr4.toml beside the executables written from the built-in defaults (pgr4.default.toml) when
// it is missing, before anything reads it: without it the game has neither its folders nor the
// settings it needs past "Press Start".
constexpr char kDefaultToml[] = {
#embed PGR4_DEFAULT_TOML
    , 0};

void WriteDefaultToml() {
  std::error_code ec;
  if (fs::exists(g_dir / "pgr4.toml", ec)) return;
  std::ofstream out(g_dir / "pgr4.toml", std::ios::binary | std::ios::trunc);
  out << kDefaultToml;
}

// A TOML literal string: no escapes, so Windows paths stay as they are.
std::string Literal(const std::wstring& w) { return "'" + Narrow(w) + "'"; }

// ---------------------------------------------------------------------------------------------
// What the game needs.

struct Adapter {
  std::wstring name;
  std::wstring ip;
};

struct State {
  std::wstring game_dir, update_dir, content_dir, dlc_dir;
  int dlc_count = 0;  // PGR4's DLC packages in dlc_dir
  int language = 0;  // index in kLanguages
  int network = 0;   // 0 automatic, else 1 + index in adapters
  std::vector<Adapter> adapters;
  std::wstring profile;

  bool game_found = false;  // default.xex there
  uint32_t game_version = 0;  // PGR4's, 0 for another game
  bool game_ok = false;       // PGR4, the disc's version or with TU2
  bool update_ok = false;
  std::wstring update_text;
  bool vc_ok = false;
  std::wstring gpu_name;
  bool rtx = false;
  bool gpu_ok = false;
  bool gpu_unsupported = false;  // a Direct3D 12 card without what the game needs
  // A controller connected (the game needs one: PollPad's), and its name.
  bool pad_ok = false;
  std::wstring pad_name;

  std::wstring message;  // the last action's error, shown as help
} g;

bool IsTu2Package(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  uint8_t header[0x364] = {};
  if (!in.read(reinterpret_cast<char*>(header), sizeof(header))) return false;
  const bool stfs = !memcmp(header, "LIVE", 4) || !memcmp(header, "PIRS", 4) ||
                    !memcmp(header, "CON ", 4);
  auto be32 = [&](size_t o) {
    return uint32_t(header[o]) << 24 | uint32_t(header[o + 1]) << 16 |
           uint32_t(header[o + 2]) << 8 | header[o + 3];
  };
  return stfs && be32(0x344) == kInstallerContent && be32(0x360) == kTitleId;
}

// PGR4's DLC packages (STFS, marketplace content) in `dir`.
constexpr uint32_t kMarketplaceContent = 0x00000002;
int DlcPackagesIn(const fs::path& dir) {
  std::error_code ec;
  if (dir.empty() || !fs::is_directory(dir, ec)) return 0;
  int count = 0;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (!entry.is_regular_file(ec)) continue;
    std::ifstream in(entry.path(), std::ios::binary);
    uint8_t header[0x364] = {};
    if (!in.read(reinterpret_cast<char*>(header), sizeof(header))) continue;
    const bool stfs = !memcmp(header, "LIVE", 4) || !memcmp(header, "PIRS", 4) ||
                      !memcmp(header, "CON ", 4);
    auto be32 = [&](size_t o) {
      return uint32_t(header[o]) << 24 | uint32_t(header[o + 1]) << 16 |
             uint32_t(header[o + 2]) << 8 | header[o + 3];
    };
    if (stfs && be32(0x344) == kMarketplaceContent && be32(0x360) == kTitleId) ++count;
  }
  return count;
}

bool FolderHasTu2Package(const fs::path& dir) {
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return false;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (entry.is_regular_file(ec) && entry.file_size(ec) > 0x364 && IsTu2Package(entry.path())) {
      return true;
    }
  }
  return false;
}

uint32_t Be32(const std::vector<uint8_t>& d, size_t o) {
  return o + 4 <= d.size() ? uint32_t(d[o]) << 24 | uint32_t(d[o + 1]) << 16 |
                                 uint32_t(d[o + 2]) << 8 | d[o + 3]
                           : 0;
}

// An XEX2 file's optional header `key`'s value (offset or inline data), 0 if absent.
uint32_t XexHeader(const std::vector<uint8_t>& d, uint32_t key) {
  if (d.size() < 0x18 || memcmp(d.data(), "XEX2", 4) != 0) return 0;
  const uint32_t count = Be32(d, 0x14);
  for (uint32_t i = 0; i < count && 0x20 + i * 8 <= d.size(); ++i)
    if (Be32(d, 0x18 + i * 8) == key) return Be32(d, 0x1C + i * 8);
  return 0;
}

std::vector<uint8_t> ReadHead(const fs::path& file) {
  std::vector<uint8_t> d(0x10000);
  std::ifstream in(file, std::ios::binary);
  in.read(reinterpret_cast<char*>(d.data()), std::streamsize(d.size()));
  d.resize(size_t(in.gcount()));
  return d;
}

// default.xex's version if it is PGR4's (execution info: media id, version, base version, title
// id), else 0.
uint32_t GameVersion(const fs::path& xex) {
  const std::vector<uint8_t> d = ReadHead(xex);
  const uint32_t info = XexHeader(d, 0x00040006);
  if (!info || Be32(d, info + 12) != kTitleId) return 0;
  return Be32(d, info + 4);
}

// Whether a default.xexp is Title Update 2's: its delta descriptor (size, target version, source
// version) from the disc's version to TU2's.
bool IsTu2Patch(const fs::path& xexp) {
  const std::vector<uint8_t> d = ReadHead(xexp);
  const uint32_t delta = XexHeader(d, 0x000005FF);
  return delta && Be32(d, delta + 4) == kTu2Version && Be32(d, delta + 8) == kBaseVersion;
}

// The title update from a folder: 1 extracted TU2, -1 another default.xexp, 2 a TU2 package,
// 0 none.
int UpdateIn(const fs::path& dir) {
  std::error_code ec;
  if (dir.empty() || !fs::is_directory(dir, ec)) return 0;
  const fs::path xexp = dir / "default.xexp";
  if (fs::is_regular_file(xexp, ec)) return IsTu2Patch(xexp) ? 1 : -1;
  return FolderHasTu2Package(dir) ? 2 : 0;
}

void FindAdapters() {
  g.adapters.clear();
  ULONG size = 16 * 1024;
  std::vector<uint8_t> buffer;
  ULONG ret;
  do {
    buffer.resize(size);
    ret = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                            GAA_FLAG_SKIP_DNS_SERVER,
                               nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()),
                               &size);
  } while (ret == ERROR_BUFFER_OVERFLOW);
  if (ret != NO_ERROR) return;
  for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); a; a = a->Next) {
    if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
    for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
      const auto* sin = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
      if (sin->sin_family != AF_INET || (ntohl(sin->sin_addr.s_addr) >> 16) == 0xA9FE) continue;
      wchar_t ip[INET_ADDRSTRLEN] = {};
      InetNtopW(AF_INET, &sin->sin_addr, ip, INET_ADDRSTRLEN);
      g.adapters.push_back({a->FriendlyName, ip});
    }
  }
}

// The card the game will use (pgr4-native's d3d12_device.cpp: the first by high performance
// preference) and whether it can run it: a Direct3D 12 device at feature level 11_0, and resource
// binding tier 2 (the shader translator's bindless descriptors).
std::wstring g_driver;  // FindGpu: the GPU and its driver's version

void FindGpu() {
  ComPtr<IDXGIFactory6> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return;
  ComPtr<IDXGIAdapter1> adapter;
  if (FAILED(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                 IID_PPV_ARGS(&adapter)))) {
    return;
  }
  DXGI_ADAPTER_DESC1 desc;
  adapter->GetDesc1(&desc);
  g.gpu_name = desc.Description;
  // The driver's version (the precompiled pipelines are the driver's own: a new one compiles
  // them again).
  LARGE_INTEGER umd = {};
  if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
    g_driver = g.gpu_name + L" " + std::to_wstring(HIWORD(umd.HighPart)) + L"." +
               std::to_wstring(LOWORD(umd.HighPart)) + L"." + std::to_wstring(HIWORD(umd.LowPart)) +
               L"." + std::to_wstring(LOWORD(umd.LowPart));
  g.rtx = desc.VendorId == 0x10DE && g.gpu_name.find(L"RTX") != std::wstring::npos;
  ComPtr<ID3D12Device> device;
  if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
    g.gpu_name.clear();
    return;
  }
  D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
  device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options));
  g.gpu_unsupported = options.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_2;
  g.gpu_ok = !g.gpu_unsupported;
}

bool HasDll(const wchar_t* name) {
  if (HMODULE m = LoadLibraryExW(name, nullptr, LOAD_LIBRARY_AS_DATAFILE)) {
    FreeLibrary(m);
    return true;
  }
  return false;
}

void CheckUpdate();
void CheckDlc();

void Check() {
  std::error_code ec;
  const fs::path xex = fs::path(g.game_dir) / "default.xex";
  g.game_found = !g.game_dir.empty() && fs::is_regular_file(xex, ec);
  g.game_version = g.game_found ? GameVersion(xex) : 0;
  g.game_ok = g.game_version == kBaseVersion || g.game_version == kTu2Version;

  if (!g.game_ok) {
    g.update_ok = false;
    g.update_text = T(kWaitGame);
  } else if (g.game_version == kTu2Version) {
    // Already updated: nothing to apply.
    g.update_ok = true;
    g.update_text = T(kTuBuiltIn);
  } else {
    CheckUpdate();
  }
  g.vc_ok = HasDll(L"msvcp140.dll") && HasDll(L"vcruntime140_1.dll") &&
            HasDll(L"msvcp140_atomic_wait.dll");
  CheckDlc();
}

struct EmbeddedFile {
  const char* name;
  const unsigned char* data;
  size_t size;
};

// Title Update 2 built into the launcher (PGR4_LAUNCHER_EMBED_TITLE_UPDATE): its extracted
// files, written beside the launcher when the game has none.
#ifdef PGR4_TU_DEFAULT_XEXP
const unsigned char kTuDefaultXexp[] = {
#embed PGR4_TU_DEFAULT_XEXP
};
const unsigned char kTuGwXexp[] = {
#embed PGR4_TU_GW_XEXP
};
const unsigned char kTuBin[] = {
#embed PGR4_TU_BIN
};
const EmbeddedFile kEmbeddedUpdate[] = {
    {"default.xexp", kTuDefaultXexp, sizeof(kTuDefaultXexp)},
    {"gw.xexp", kTuGwXexp, sizeof(kTuGwXexp)},
    {"Pgr4TitleUpdate.bin", kTuBin, sizeof(kTuBin)},
};

bool WriteEmbeddedUpdate(const fs::path& dir) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  for (const EmbeddedFile& file : kEmbeddedUpdate) {
    std::ofstream out(dir / file.name, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(file.data), std::streamsize(file.size));
    if (!out) return false;
  }
  return UpdateIn(dir) == 1;
}

// The built-in title update installed: title-update\ beside the launcher, else (a folder it
// cannot write to, like Program Files) the user's local app data. Returns its folder, or empty.
std::wstring InstallEmbeddedUpdate() {
  wchar_t* local = nullptr;
  std::vector<fs::path> dirs = {g_dir / "title-update"};
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
    dirs.push_back(fs::path(local) / "PGR4" / "title-update");
    CoTaskMemFree(local);
  }
  for (const fs::path& dir : dirs)
    if (WriteEmbeddedUpdate(dir)) return dir.wstring();
  return {};
}
constexpr bool kHasBuiltInUpdate = true;
#else
std::wstring InstallEmbeddedUpdate() { return {}; }
constexpr bool kHasBuiltInUpdate = false;
#endif

// DLC packages built into the launcher (PGR4_LAUNCHER_EMBED_DLC, launcher_dlc.inc: kEmbeddedDlc),
// written into dlc\ beside it (else the user's local app data) when missing there, and given to the
// game as its dlc_root, which installs them at its start.
#ifdef PGR4_DLC_INC
#include PGR4_DLC_INC

bool WriteEmbeddedDlc(const fs::path& dir) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  for (const EmbeddedFile& file : kEmbeddedDlc) {
    const fs::path path = dir / file.name;
    if (fs::is_regular_file(path, ec) && fs::file_size(path, ec) == file.size) continue;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(file.data), std::streamsize(file.size));
    if (!out) return false;
  }
  return true;
}

// Returns the folder the built-in DLC are in, or empty.
std::wstring InstallEmbeddedDlc() {
  wchar_t* local = nullptr;
  std::vector<fs::path> dirs = {g_dir / "dlc"};
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
    dirs.push_back(fs::path(local) / "PGR4" / "dlc");
    CoTaskMemFree(local);
  }
  for (const fs::path& dir : dirs)
    if (WriteEmbeddedDlc(dir)) return dir.wstring();
  return {};
}
constexpr bool kHasBuiltInDlc = true;
constexpr int kBuiltInDlcCount = int(std::size(kEmbeddedDlc));
#else
std::wstring InstallEmbeddedDlc() { return {}; }
constexpr bool kHasBuiltInDlc = false;
constexpr int kBuiltInDlcCount = 0;
#endif

// The built-in DLC, once the game is there: written and set as dlc_root unless the chosen folder
// already has as many.
void CheckDlc() {
  if (!kHasBuiltInDlc || !g.game_ok || g.dlc_count >= kBuiltInDlcCount) return;
  const std::wstring dir = InstallEmbeddedDlc();
  if (dir.empty()) return;
  g.dlc_dir = dir;
  g.dlc_count = DlcPackagesIn(dir);
  WriteSetting("dlc_root", Literal(dir));
}

void CheckUpdate() {
  int update = 0;
  if (!g.update_dir.empty()) update = UpdateIn(g.update_dir);
  if (!update && !g.content_dir.empty()) update = FolderHasTu2Package(g.content_dir) ? 2 : 0;
  if (!update && g.game_ok) {
    update = UpdateIn(g.game_dir);
    if (!update) update = FolderHasTu2Package(fs::path(g.game_dir) / "Content") ? 2 : 0;
  }
  if (update <= 0 && g.game_ok) {
    // The disc's version without TU2: the launcher's own, when it has it.
    const std::wstring dir = InstallEmbeddedUpdate();
    if (!dir.empty()) {
      g.update_dir = dir;
      g.content_dir.clear();
      WriteSetting("update_data_root", Literal(dir));
      WriteSetting("content_root", "''");
      update = 1;
    }
  }
  g.update_ok = update > 0;
  g.update_text = update == 1   ? T(kTuExtracted)
                  : update == 2 ? T(kTuPackage)
                  : update < 0  ? T(kTuWrong)
                                : T(kMissing);
}

// The LANGUAGE row's entry for pgr4.toml's user_language and user_country, else for Windows'
// display language.
int InitialLanguage() {
  const std::string language = ReadSetting("user_language");
  const int country = std::atoi(ReadSetting("user_country").c_str());
  if (!language.empty()) {
    const int value = std::atoi(language.c_str());
    int found = -1;
    for (int i = 0; i < int(std::size(kLanguages)); ++i) {
      if (kLanguages[i].value != value) continue;
      if (kLanguages[i].country == country) return i;
      if (!kLanguages[i].country && found < 0) found = i;
    }
    if (found >= 0) return found;
  }
  const LANGID windows = GetUserDefaultUILanguage();
  const wchar_t* name = L"ENGLISH";
  switch (PRIMARYLANGID(windows)) {
    case LANG_FRENCH: name = L"FRANÇAIS"; break;
    case LANG_GERMAN: name = L"DEUTSCH"; break;
    case LANG_SPANISH: name = L"ESPAÑOL"; break;
    case LANG_ITALIAN: name = L"ITALIANO"; break;
    case LANG_CZECH: name = L"ČEŠTINA"; break;
    case LANG_HUNGARIAN: name = L"MAGYAR"; break;
    case LANG_POLISH: name = L"POLSKI"; break;
    case LANG_RUSSIAN: name = L"РУССКИЙ"; break;
    case LANG_JAPANESE: name = L"日本語"; break;
    case LANG_KOREAN: name = L"한국어"; break;
    case LANG_CHINESE:
      name = SUBLANGID(windows) == SUBLANG_CHINESE_SIMPLIFIED ||
                     SUBLANGID(windows) == SUBLANG_CHINESE_SINGAPORE
                 ? L"简体中文"
                 : L"繁體中文";
      break;
  }
  for (int i = 0; i < int(std::size(kLanguages)); ++i)
    if (std::wstring(kLanguages[i].name) == name) return i;
  return 0;
}

// An Xbox 360 gamertag: 1 to 15 ASCII letters, digits or spaces, a letter first; empty if not.
std::wstring CheckName(std::wstring name) {
  while (!name.empty() && name.back() == L' ') name.pop_back();
  while (!name.empty() && name.front() == L' ') name.erase(name.begin());
  auto letter = [](wchar_t c) { return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z'); };
  if (name.empty() || name.size() > kMaxName || !letter(name[0])) return {};
  for (wchar_t c : name)
    if (!letter(c) && !(c >= L'0' && c <= L'9') && c != L' ') return {};
  return name;
}

// Player 1's name and player 2's ("<name> 2", cut to 15 characters), as pgr4.exe makes them.
void SaveName(const std::wstring& name) {
  g.profile = name;
  std::wstring second = name.substr(0, kMaxName - 2);
  while (!second.empty() && second.back() == L' ') second.pop_back();
  second += L" 2";
  WriteSetting("pgr4_profile_name", "\"" + Narrow(name) + "\"");
  WriteSetting("pgr4_profile2_name", "\"" + Narrow(second) + "\"");
}

void Load() {
  g.game_dir = Widen(ReadSetting("game_data_root"));
  g.update_dir = Widen(ReadSetting("update_data_root"));
  g.content_dir = Widen(ReadSetting("content_root"));
  g.dlc_dir = Widen(ReadSetting("dlc_root"));
  g_force_playstation = ReadSetting("force_playstation_layout") == "true";
  g_playstation = g_force_playstation;
  g.dlc_count = DlcPackagesIn(g.dlc_dir);
  g.profile = Widen(ReadSetting("pgr4_profile_name"));
  if (g.profile.empty()) SaveName(kDefaultName);
  g.language = InitialLanguage();
  g_texts = kTexts[g.language];
  // Windows' language (English when it is none of the game's), for the game too.
  if (ReadSetting("user_language").empty()) {
    WriteSetting("user_language", std::to_string(kLanguages[g.language].value));
    if (kLanguages[g.language].country)
      WriteSetting("user_country", std::to_string(kLanguages[g.language].country));
  }
  FindAdapters();
  const std::wstring lan_ip = Widen(ReadSetting("net_lan_ip"));
  g.network = 0;
  for (size_t i = 0; i < g.adapters.size(); ++i)
    if (g.adapters[i].ip == lan_ip) g.network = int(i) + 1;

  // The release's layout found on its own: game\ and title-update\ beside the launcher (the
  // folders pgr4.exe looks for itself).
  std::error_code ec;
  // Local change (31/10/2026): any folder beside the launcher holding default.xex (pgr4\, game\,
  // "Project Gotham Racing 4\"…), also when the folder set in pgr4.toml has no game any more
  // (moved). game\ and pgr4\ first, then the others in name order.
  if (g.game_dir.empty() || !fs::is_regular_file(fs::path(g.game_dir) / "default.xex", ec)) {
    std::vector<fs::path> folders = {g_dir / "game", g_dir / "pgr4"};
    std::vector<fs::path> others;
    for (const auto& entry : fs::directory_iterator(g_dir, ec))
      if (entry.is_directory(ec)) others.push_back(entry.path());
    std::sort(others.begin(), others.end());
    folders.insert(folders.end(), others.begin(), others.end());
    for (const fs::path& folder : folders) {
      if (!fs::is_regular_file(folder / "default.xex", ec)) continue;
      g.game_dir = folder.wstring();
      WriteSetting("game_data_root", Literal(g.game_dir));
      break;
    }
  }
  // The folders beside the launcher, the named ones first: the title update's and the DLC's are
  // found the same way as the game's (31/10/2026), also when the folder set has none any more.
  std::vector<fs::path> beside;
  for (const auto& entry : fs::directory_iterator(g_dir, ec))
    if (entry.is_directory(ec)) beside.push_back(entry.path());
  std::sort(beside.begin(), beside.end());
  if (g.content_dir.empty() && (g.update_dir.empty() || UpdateIn(g.update_dir) == 0)) {
    std::vector<fs::path> folders = {g_dir / "title-update", g_dir / "update"};
    folders.insert(folders.end(), beside.begin(), beside.end());
    for (const fs::path& folder : folders) {
      if (UpdateIn(folder) == 0) continue;
      g.update_dir = folder.wstring();
      WriteSetting("update_data_root", Literal(g.update_dir));
      break;
    }
  }
  if (g.dlc_count == 0) {
    std::vector<fs::path> folders = {g_dir / "dlc"};
    folders.insert(folders.end(), beside.begin(), beside.end());
    for (const fs::path& folder : folders) {
      if (DlcPackagesIn(folder.wstring()) == 0) continue;
      g.dlc_dir = folder.wstring();
      g.dlc_count = DlcPackagesIn(g.dlc_dir);
      WriteSetting("dlc_root", Literal(g.dlc_dir));
      break;
    }
  }
  FindGpu();
  Check();
}

// ---------------------------------------------------------------------------------------------
// The menu.

enum class Row { kPlay, kVcRedist, kName, kGameDir, kUpdate, kDlc, kLanguage, kNetwork, kQuit };

std::vector<Row> g_rows;
int g_selected = 0;

bool Ready() { return g.game_ok && g.update_ok && g.vc_ok && g.gpu_ok && g.pad_ok; }

// The rows for where things stand: PLAY once everything is there, the game folder until then,
// the title update only when the game is there without one. The selection stays on its row, or
// goes to what is missing next (PLAY once ready).
void BuildRows() {
  const bool had_rows = !g_rows.empty();
  const Row selected = had_rows ? g_rows[std::clamp(g_selected, 0, int(g_rows.size()) - 1)]
                                : Row::kPlay;
  const bool was_ready = had_rows && g_rows.front() == Row::kPlay;
  g_rows.clear();
  if (Ready()) g_rows.push_back(Row::kPlay);
  if (!g.vc_ok) g_rows.push_back(Row::kVcRedist);
  g_rows.push_back(Row::kName);
  g_rows.push_back(Row::kGameDir);
  if (g.game_ok && !g.update_ok) g_rows.push_back(Row::kUpdate);
  // With the DLC built in, only when writing them failed.
  if (g.game_ok && (!kHasBuiltInDlc || g.dlc_count == 0)) g_rows.push_back(Row::kDlc);
  g_rows.push_back(Row::kLanguage);
  g_rows.push_back(Row::kNetwork);
  g_rows.push_back(Row::kQuit);
  auto index = [](Row row) {
    for (int i = 0; i < int(g_rows.size()); ++i)
      if (g_rows[i] == row) return i;
    return -1;
  };
  int next = -1;
  if (Ready() && !was_ready) {
    next = index(Row::kPlay);
  } else if (!g.game_ok && (!had_rows || selected == Row::kGameDir)) {
    next = index(Row::kGameDir);
  } else if (g.game_ok && !g.update_ok && (!had_rows || selected == Row::kGameDir)) {
    next = index(Row::kUpdate);
  }
  if (next < 0) next = index(selected);
  g_selected = next >= 0 ? next : std::clamp(g_selected, 0, int(g_rows.size()) - 1);
}

std::wstring PickFolder(HWND hwnd, const wchar_t* title) {
  ComPtr<IFileOpenDialog> dialog;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&dialog)))) {
    return {};
  }
  DWORD options = 0;
  dialog->GetOptions(&options);
  dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
  dialog->SetTitle(title);
  if (FAILED(dialog->Show(hwnd))) return {};
  ComPtr<IShellItem> item;
  if (FAILED(dialog->GetResult(&item))) return {};
  PWSTR path = nullptr;
  if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return {};
  std::wstring result = path;
  CoTaskMemFree(path);
  return result;
}

void Step(Row row, int delta) {
  if (row == Row::kLanguage) {
    const int n = int(std::size(kLanguages));
    g.language = (g.language + delta + n) % n;
    const Language& language = kLanguages[g.language];
    WriteSetting("user_language", std::to_string(language.value));
    // Czech and Hungarian by the country; another language puts back the default one (the US)
    // when the country was theirs.
    const int country = std::atoi(ReadSetting("user_country").c_str());
    if (language.country) {
      WriteSetting("user_country", std::to_string(language.country));
    } else if (country == 23 || country == 42) {
      WriteSetting("user_country", "103");
    }
    g_texts = kTexts[g.language];
    Check();
  } else if (row == Row::kNetwork) {
    const int n = int(g.adapters.size()) + 1;
    g.network = (g.network + delta + n) % n;
    WriteSetting("net_lan_ip",
                 g.network ? Literal(g.adapters[g.network - 1].ip) : std::string("''"));
  }
}

HANDLE g_precompile = nullptr;  // pgr4.exe compiling the shaders (PollPrecompile)

bool Launch(bool precompile) {
  const fs::path exe = g_dir / "pgr4.exe";
  // The folders on the command line too (pgr4.toml has them as well).
  auto arg = [](const wchar_t* name, std::wstring value) {
    while (!value.empty() && (value.back() == L'\\' || value.back() == L'/')) value.pop_back();
    return value.empty() ? std::wstring() : std::wstring(L" --") + name + L"=\"" + value + L"\"";
  };
  std::wstring command = L"\"" + exe.wstring() + L"\"" + arg(L"game_data_root", g.game_dir) +
                         arg(L"update_data_root", g.update_dir) +
                         arg(L"content_root", g.content_dir) + arg(L"dlc_root", g.dlc_dir) +
                         L" --pgr4_launcher=true";  // pgr4.exe starts only from here (15/11/2026)
  STARTUPINFOW si = {sizeof(si)};
  if (precompile) {
    command += L" --pgr4_native_precompile=true --window_hidden=true";
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
  }
  PROCESS_INFORMATION pi = {};
  if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                      g_dir.c_str(), &si, &pi)) {
    return false;
  }
  if (precompile) {
    CloseHandle(pi.hThread);
    g_precompile = pi.hProcess;
    return true;
  }
  // The game may take the foreground from us (it brings its window to the front once made).
  AllowSetForegroundWindow(pi.dwProcessId);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}

// ---------------------------------------------------------------------------------------------
// Shader precompile (31/10/2026): before the game, every pipeline the game recorded (its
// Documents\pgr4\cache\pgr4-native* folders, one per draw resolution scale) compiled for the
// scale in use by pgr4.exe --pgr4_native_precompile, which then exits; a bar shows its progress.
// At each start (Tazz's choice: checked by the game itself, hidden): the game looks each one up in
// its library and compiles those missing; a bar shows its progress.

fs::path CacheDir() {
  // The game's own folder when the toml names another (user_data_root: the development build's).
  if (const std::string root = ReadSetting("user_data_root"); !root.empty())
    return fs::path(root) / "cache";
  wchar_t* docs = nullptr;
  fs::path dir;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
    dir = fs::path(docs) / "pgr4" / "cache";
    CoTaskMemFree(docs);
  }
  return dir;
}

fs::path ScaleDir() {
  const int scale =
      std::clamp(std::atoi(ReadSetting("pgr4_native_resolution_scale").c_str()), 1, 4);
  return CacheDir() / (scale > 1 ? "pgr4-native-scale" + std::to_string(scale) : "pgr4-native");
}

// The distinct pipelines recorded in every scale's pipelines.bin (the game imports the others'),
// as the FNV-1a of each record's hashes, modifications and key (as compiled.txt has them):
// header (P4PS, format, key size, key version, session), then records: 'S' type hash dwords ucode,
// 'P' 32 bytes of hashes, the key and the session, 'T' hash session.
std::unordered_set<uint64_t> RecordedPipelines() {
  std::unordered_set<uint64_t> records;
  std::error_code ec;
  // The base list shipped beside the launcher (every city's), then the player's own.
  std::vector<fs::path> files = {g_dir / "pipelines_base.bin"};
  for (const auto& entry : fs::directory_iterator(CacheDir(), ec))
    if (entry.is_directory(ec) && entry.path().filename().string().rfind("pgr4-native", 0) == 0)
      files.push_back(entry.path() / "pipelines.bin");
  for (const fs::path& file : files) {
    std::ifstream in(file, std::ios::binary);
    const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.size() < 20 || data.compare(0, 4, "P4PS") != 0) continue;
    uint32_t key_size = 0;
    std::memcpy(&key_size, data.data() + 8, 4);
    size_t pos = 20;
    while (pos + 4 <= data.size()) {
      uint32_t tag;
      std::memcpy(&tag, data.data() + pos, 4);
      pos += 4;
      if (tag == 'S') {
        if (pos + 16 > data.size()) break;
        uint32_t dwords;
        std::memcpy(&dwords, data.data() + pos + 12, 4);
        pos += 16 + size_t(dwords) * 4;
      } else if (tag == 'P') {
        if (pos + 36 + key_size > data.size()) break;
        uint64_t h = 0xCBF29CE484222325ull;
        for (size_t i = 0; i < 32 + key_size; ++i) {
          h ^= uint8_t(data[pos + i]);
          h *= 0x100000001B3ull;
        }
        records.insert(h);
        pos += 36 + key_size;
      } else if (tag == 'T') {
        pos += 12;
      } else {
        break;
      }
    }
  }
  return records;
}

size_t g_precompile_done = 0, g_precompile_total = 0;

bool Launch(bool precompile);

bool StartPrecompile() {
  std::error_code ec;
  fs::remove(ScaleDir() / "precompile_progress.txt", ec);
  g_precompile_done = 0;
  g_precompile_total = 0;  // unknown until the game reports it (ENHANCED REFLECTIONS doubles some)
  return Launch(true);
}

// Each timer tick while it runs: its progress; once it exited, the game.
void PollPrecompile(HWND hwnd) {
  if (!g_precompile) return;
  {
    std::ifstream in(ScaleDir() / "precompile_progress.txt");
    size_t done = 0, total = 0;
    if (in >> done >> total && total) {
      g_precompile_done = done;
      g_precompile_total = total;
    }
  }
  if (WaitForSingleObject(g_precompile, 0) != WAIT_OBJECT_0) return;
  DWORD code = 1;
  GetExitCodeProcess(g_precompile, &code);
  CloseHandle(g_precompile);
  g_precompile = nullptr;
  // The game even if the step failed: it compiles what it needs as it plays.
  if (Launch(false)) {
    DestroyWindow(hwnd);
  } else {
    g.message = T(kLaunchFail);
  }
}

void Activate(HWND hwnd, Row row);

// The name being edited (g_editing): its text and the cursor (a character's index, or the size
// for the next one).
bool g_editing = false;
std::wstring g_edit;
size_t g_cursor = 0;
constexpr wchar_t kNameChars[] = L" ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

void BeginEdit() {
  g_editing = true;
  g_edit = g.profile;
  g_cursor = g_edit.size();
  g.message.clear();
}

void EndEdit(bool keep) {
  if (keep) {
    const std::wstring name = CheckName(g_edit);
    if (name.empty()) {
      g.message = T(kNameInvalid);
      return;
    }
    SaveName(name);
  }
  g_editing = false;
  g.message.clear();
}

void EditType(wchar_t c) {
  const bool ok = (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') ||
                  c == L' ';
  if (!ok || g_edit.size() >= kMaxName) return;
  g_edit.insert(g_cursor++, 1, c);
}

// The controller's letter at the cursor: the next or previous of kNameChars.
void EditCycle(int delta) {
  const int n = int(std::size(kNameChars)) - 1;
  if (g_cursor == g_edit.size()) {
    if (g_edit.size() >= kMaxName) return;
    g_edit.push_back(L'A');
    return;
  }
  const wchar_t* at = std::wcschr(kNameChars, g_edit[g_cursor]);
  const int i = at ? int(at - kNameChars) : 0;
  g_edit[g_cursor] = kNameChars[(i + delta + n) % n];
}

void EditMove(int delta) {
  g_cursor = size_t(std::clamp(int(g_cursor) + delta, 0, int(g_edit.size())));
}

void EditErase() {
  if (g_cursor < g_edit.size()) {
    g_edit.erase(g_cursor, 1);
  } else if (!g_edit.empty()) {
    g_edit.pop_back();
    g_cursor = g_edit.size();
  }
}

void PlayNow(HWND hwnd) {
  if (Ready()) Activate(hwnd, Row::kPlay);
}

void Activate(HWND hwnd, Row row) {
  g.message.clear();
  switch (row) {
    case Row::kPlay:
      if (g_precompile) {
        break;  // already preparing the shaders
      } else if (!Ready()) {
        g.message = T(kPlayBlocked);
      } else if (!RecordedPipelines().empty() && StartPrecompile()) {
        break;  // the game once the shaders are ready (PollPrecompile)
      } else if (Launch(false)) {
        DestroyWindow(hwnd);
      } else {
        g.message = T(kLaunchFail);
      }
      break;
    case Row::kVcRedist:
      ShellExecuteW(hwnd, L"open", L"https://aka.ms/vs/17/release/vc_redist.x64.exe", nullptr,
                    nullptr, SW_SHOWNORMAL);
      break;
    case Row::kGameDir: {
      const std::wstring dir = PickFolder(hwnd, T(kPickGame));
      if (dir.empty()) break;
      std::error_code ec;
      if (!fs::is_regular_file(fs::path(dir) / "default.xex", ec)) {
        g.message = T(kNoXex);
        break;
      }
      g.game_dir = dir;
      WriteSetting("game_data_root", Literal(dir));
      break;
    }
    case Row::kUpdate: {
      const std::wstring dir = PickFolder(hwnd, T(kPickUpdate));
      if (dir.empty()) break;
      const int update = UpdateIn(dir);
      if (update == 1 || update == -1) {
        // Extracted (an unknown default.xexp is kept, and shown as such).
        g.update_dir = dir;
        g.content_dir.clear();
      } else if (update == 2) {
        // A package: installed from there at start (content_root).
        g.content_dir = dir;
        g.update_dir.clear();
      } else {
        g.message = T(kNoTu);
        break;
      }
      WriteSetting("update_data_root", Literal(g.update_dir));
      WriteSetting("content_root", Literal(g.content_dir));
      break;
    }
    case Row::kDlc: {
      const std::wstring dir = PickFolder(hwnd, T(kPickDlc));
      if (dir.empty()) break;
      const int count = DlcPackagesIn(dir);
      if (!count) {
        g.message = T(kNoDlc);
        break;
      }
      g.dlc_dir = dir;
      g.dlc_count = count;
      WriteSetting("dlc_root", Literal(dir));
      break;
    }
    case Row::kName:
      BeginEdit();
      break;
    case Row::kLanguage:
    case Row::kNetwork:
      Step(row, 1);
      break;
    case Row::kQuit:
      DestroyWindow(hwnd);
      break;
  }
  Check();
  BuildRows();
}

// ---------------------------------------------------------------------------------------------
// Drawing.

constexpr float kWidth = 1040, kHeight = 600;  // DIPs
constexpr D2D1_COLOR_F kRed = {0.80f, 0.05f, 0.09f, 1};
constexpr D2D1_COLOR_F kDarkRed = {0.55f, 0.02f, 0.05f, 1};
constexpr D2D1_COLOR_F kInk = {0.10f, 0.10f, 0.11f, 1};
constexpr D2D1_COLOR_F kGrey = {0.42f, 0.42f, 0.45f, 1};
constexpr D2D1_COLOR_F kWhite = {1, 1, 1, 1};
constexpr D2D1_COLOR_F kGreen = {0.13f, 0.62f, 0.22f, 1};
constexpr D2D1_COLOR_F kYellow = {0.93f, 0.66f, 0.04f, 1};

ComPtr<ID2D1Factory> g_d2d;
ComPtr<IDWriteFactory> g_dwrite;
ComPtr<ID2D1HwndRenderTarget> g_target;
ID2D1RenderTarget* g_render = nullptr;  // the target being drawn on
ComPtr<ID2D1SolidColorBrush> g_brush;
ComPtr<ID2D1LinearGradientBrush> g_backdrop;
ComPtr<IDWriteTextFormat> g_title_small, g_title_big, g_menu, g_menu_small, g_menu_big, g_value,
    g_name, g_help,
    g_small, g_badge;
ComPtr<IDWriteInlineObject> g_ellipsis;
float g_dpi = 96;
int g_hover_button = -1;  // 0 minimize, 1 close
double g_time = 0;

ComPtr<IDWriteTextFormat> Format(const wchar_t* family, DWRITE_FONT_WEIGHT weight,
                                 DWRITE_FONT_STYLE style, float size) {
  ComPtr<IDWriteTextFormat> format;
  g_dwrite->CreateTextFormat(family, nullptr, weight, style, DWRITE_FONT_STRETCH_NORMAL, size,
                             L"", &format);
  format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  return format;
}

void CreateFormats() {
  g_title_small = Format(L"Segoe UI Black", DWRITE_FONT_WEIGHT_BLACK, DWRITE_FONT_STYLE_ITALIC, 24);
  g_title_big = Format(L"Segoe UI Black", DWRITE_FONT_WEIGHT_BLACK, DWRITE_FONT_STYLE_ITALIC, 62);
  g_menu = Format(L"Segoe UI Black", DWRITE_FONT_WEIGHT_BLACK, DWRITE_FONT_STYLE_ITALIC, 25);
  g_menu_small = Format(L"Segoe UI Black", DWRITE_FONT_WEIGHT_BLACK, DWRITE_FONT_STYLE_ITALIC, 19);
  g_menu_big = Format(L"Segoe UI Black", DWRITE_FONT_WEIGHT_BLACK, DWRITE_FONT_STYLE_ITALIC, 36);
  g_value = Format(L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_ITALIC, 15);
  g_name = Format(L"Segoe UI", DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, 18);
  g_help = Format(L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_ITALIC, 17);
  g_help->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
  g_help->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
  g_small = Format(L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_ITALIC, 14);
  g_badge = Format(L"Segoe UI Black", DWRITE_FONT_WEIGHT_BLACK, DWRITE_FONT_STYLE_ITALIC, 20);
  g_badge->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
  g_dwrite->CreateEllipsisTrimmingSign(g_value.Get(), &g_ellipsis);
  DWRITE_TRIMMING trimming = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
  g_value->SetTrimming(&trimming, g_ellipsis.Get());
  g_small->SetTrimming(&trimming, g_ellipsis.Get());
}

void CreateTarget(HWND hwnd) {
  if (g_target) return;
  RECT rc;
  GetClientRect(hwnd, &rc);
  g_d2d->CreateHwndRenderTarget(
      D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                   D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_UNKNOWN),
                                   g_dpi, g_dpi),
      D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(rc.right, rc.bottom)), &g_target);
  g_target->CreateSolidColorBrush(kInk, &g_brush);
  const D2D1_GRADIENT_STOP stops[] = {{0, {0.97f, 0.97f, 0.97f, 1}},
                                      {0.6f, {0.90f, 0.90f, 0.91f, 1}},
                                      {1, {0.80f, 0.80f, 0.82f, 1}}};
  ComPtr<ID2D1GradientStopCollection> collection;
  g_target->CreateGradientStopCollection(stops, 3, &collection);
  g_target->CreateLinearGradientBrush(
      D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, kHeight)),
      collection.Get(), &g_backdrop);
}

ID2D1SolidColorBrush* Brush(D2D1_COLOR_F color, float alpha = 1) {
  color.a *= alpha;
  g_brush->SetColor(color);
  return g_brush.Get();
}

// A parallelogram leaning right by `skew` DIPs over its height.
void FillSlanted(float left, float top, float right, float bottom, float skew, ID2D1Brush* brush) {
  ComPtr<ID2D1PathGeometry> path;
  g_d2d->CreatePathGeometry(&path);
  ComPtr<ID2D1GeometrySink> sink;
  path->Open(&sink);
  sink->BeginFigure(D2D1::Point2F(left + skew, top), D2D1_FIGURE_BEGIN_FILLED);
  sink->AddLine(D2D1::Point2F(right + skew, top));
  sink->AddLine(D2D1::Point2F(right, bottom));
  sink->AddLine(D2D1::Point2F(left, bottom));
  sink->EndFigure(D2D1_FIGURE_END_CLOSED);
  sink->Close();
  g_render->FillGeometry(path.Get(), brush);
}

void FillTriangle(D2D1_POINT_2F a, D2D1_POINT_2F b, D2D1_POINT_2F c, ID2D1Brush* brush) {
  ComPtr<ID2D1PathGeometry> path;
  g_d2d->CreatePathGeometry(&path);
  ComPtr<ID2D1GeometrySink> sink;
  path->Open(&sink);
  sink->BeginFigure(a, D2D1_FIGURE_BEGIN_FILLED);
  sink->AddLine(b);
  sink->AddLine(c);
  sink->EndFigure(D2D1_FIGURE_END_CLOSED);
  sink->Close();
  g_render->FillGeometry(path.Get(), brush);
}

void Text(const std::wstring& text, IDWriteTextFormat* format, D2D1_RECT_F rect,
          D2D1_COLOR_F color, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
  format->SetTextAlignment(align);
  g_render->DrawTextW(text.c_str(), UINT32(text.size()), format, rect, Brush(color),
                      D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

// The game's outlined headings: the outline drawn around, then the fill.
void OutlinedText(const std::wstring& text, IDWriteTextFormat* format, D2D1_RECT_F rect,
                  D2D1_COLOR_F fill, D2D1_COLOR_F outline, float width) {
  format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
  for (int i = 0; i < 12; ++i) {
    const float a = float(i) * 3.14159265f / 6;
    const D2D1_RECT_F r = {rect.left + width * std::cos(a), rect.top + width * std::sin(a),
                           rect.right + width * std::cos(a), rect.bottom + width * std::sin(a)};
    g_render->DrawTextW(text.c_str(), UINT32(text.size()), format, r, Brush(outline));
  }
  g_render->DrawTextW(text.c_str(), UINT32(text.size()), format, rect, Brush(fill));
}

float TextWidth(const std::wstring& text, IDWriteTextFormat* format) {
  ComPtr<IDWriteTextLayout> layout;
  g_dwrite->CreateTextLayout(text.c_str(), UINT32(text.size()), format, 2000, 200, &layout);
  DWRITE_TEXT_METRICS metrics = {};
  layout->GetMetrics(&metrics);
  return metrics.widthIncludingTrailingWhitespace;
}

// Layout.
constexpr float kMenuLeft = 48, kMenuRight = 590, kMenuTop = 182, kRowHeight = 41,
                kPlayHeight = 58;
constexpr D2D1_RECT_F kPanel = {632, 186, 1000, 470};
constexpr D2D1_RECT_F kMinimize = {948, 10, 984, 40}, kClose = {990, 10, 1030, 40};

D2D1_RECT_F RowRect(int i) {
  float top = kMenuTop;
  for (int k = 0; k < i; ++k) top += g_rows[k] == Row::kPlay ? kPlayHeight + 8 : kRowHeight;
  const float height = g_rows[i] == Row::kPlay ? kPlayHeight : kRowHeight - 6;
  return {kMenuLeft, top, kMenuRight, top + height};
}

std::wstring RowLabel(Row row) {
  switch (row) {
    case Row::kPlay: return T(kPlay);
    case Row::kVcRedist: return T(kVcRedist);
    case Row::kGameDir: return T(kGameDir);
    case Row::kUpdate: return T(kUpdate);
    case Row::kDlc: return T(kDlc);
    case Row::kName: return T(kName);
    case Row::kLanguage: return T(kLanguage);
    case Row::kNetwork: return T(kNetwork);
    case Row::kQuit: return T(kQuit);
  }
  return {};
}

std::wstring RowHelp(Row row) {
  switch (row) {
    case Row::kPlay: return T(kPlayHelp);
    case Row::kVcRedist: return T(kVcRedistHelp);
    case Row::kGameDir: return T(kGameDirHelp);
    case Row::kUpdate: return T(kUpdateHelp);
    case Row::kDlc: return T(kDlcHelp);
    case Row::kName: return T(kNameHelp);
    case Row::kLanguage: return T(kLanguageHelp);
    case Row::kNetwork: return T(kNetworkHelp);
    case Row::kQuit: return T(kQuitHelp);
  }
  return {};
}

std::wstring RowValue(Row row) {
  switch (row) {
    case Row::kGameDir: return g.game_dir.empty() ? T(kNotSet) : g.game_dir;
    case Row::kUpdate: {
      const std::wstring& dir = !g.update_dir.empty() ? g.update_dir : g.content_dir;
      if (!dir.empty()) return dir;
      return g.update_ok ? g.update_text : T(kNotSet);
    }
    case Row::kDlc:
      return g.dlc_dir.empty() ? T(kNotSet)
                               : std::to_wstring(g.dlc_count) + L" DLC  " + g.dlc_dir;
    case Row::kName: return g.profile;
    case Row::kLanguage: return kLanguages[g.language].name;
    case Row::kNetwork:
      return g.network ? g.adapters[g.network - 1].name + L" " + g.adapters[g.network - 1].ip
                       : T(kAuto);
    default: return {};
  }
}

bool IsSpinner(Row row) { return row == Row::kLanguage || row == Row::kNetwork; }

void DrawBackdrop() {
  g_render->FillRectangle(D2D1::RectF(0, 0, kWidth, kHeight), g_backdrop.Get());
  // Wide pale bands drifting right, like the front end's backdrops.
  const float drift = float(std::fmod(g_time * 12.0, 260.0));
  for (int i = -2; i < 7; ++i) {
    const float x = i * 260.0f + drift;
    FillSlanted(x, 0, x + 120, kHeight, 340, Brush(kWhite, 0.35f));
  }
  // Speed lines in the bottom right corner.
  for (int i = 0; i < 6; ++i) {
    const float x = 760 + i * 46.0f;
    const float phase = float(std::fmod(g_time * 0.8 + i * 0.37, 1.0));
    FillSlanted(x, 492, x + 18 + 10 * phase, kHeight, -90, Brush(kRed, 0.05f + 0.07f * i / 5));
  }
  // The red rule under the heading.
  FillSlanted(0, 160, 700, 166, 6, Brush(kRed));
  FillSlanted(700, 160, kWidth, 166, 6, Brush(kInk, 0.12f));
}

// The title screen's logo with its 2026 tag, decoded here: the launcher's own version
// (textures\launcher_logo.bin, the tag centred under PROJECT GOTHAM RACING; no texture hash, so the
// game leaves it), else the game's logo texture as the release replaces it
// (textures\7463904A23B98655.bin, the tag under the 4). Both 1024x512 BC3, one level; made by
// tools/logo_2026/make_logo.py. The part holding the logo and the tag.
constexpr uint32_t kLogoWidth = 1024, kLogoHeight = 512;
constexpr D2D1_RECT_F kLaunchLogoSource = {12, 128, 1022, 474};
constexpr D2D1_RECT_F kGameLogoSource = {12, 128, 1022, 508};
D2D1_RECT_F g_logo_source = kLaunchLogoSource;

// One BC3 (DXT5) block's 16 texels as premultiplied BGRA.
void DecodeBc3Block(const uint8_t* block, uint8_t out[16][4]) {
  uint8_t alpha[8] = {block[0], block[1]};
  if (alpha[0] > alpha[1]) {
    for (int i = 1; i < 7; ++i) alpha[i + 1] = uint8_t(((7 - i) * alpha[0] + i * alpha[1]) / 7);
  } else {
    for (int i = 1; i < 5; ++i) alpha[i + 1] = uint8_t(((5 - i) * alpha[0] + i * alpha[1]) / 5);
    alpha[6] = 0;
    alpha[7] = 255;
  }
  uint64_t alpha_bits = 0;
  for (int i = 0; i < 6; ++i) alpha_bits |= uint64_t(block[2 + i]) << (8 * i);
  const uint16_t c0 = uint16_t(block[8] | block[9] << 8), c1 = uint16_t(block[10] | block[11] << 8);
  int colors[4][3];
  for (int k = 0; k < 2; ++k) {
    const uint16_t c = k ? c1 : c0;
    colors[k][0] = (c >> 11 & 31) * 255 / 31;
    colors[k][1] = (c >> 5 & 63) * 255 / 63;
    colors[k][2] = (c & 31) * 255 / 31;
  }
  for (int j = 0; j < 3; ++j) {
    colors[2][j] = (2 * colors[0][j] + colors[1][j]) / 3;
    colors[3][j] = (colors[0][j] + 2 * colors[1][j]) / 3;
  }
  const uint32_t color_bits = uint32_t(block[12]) | uint32_t(block[13]) << 8 |
                              uint32_t(block[14]) << 16 | uint32_t(block[15]) << 24;
  for (int t = 0; t < 16; ++t) {
    const int a = alpha[alpha_bits >> (3 * t) & 7];
    const int* rgb = colors[color_bits >> (2 * t) & 3];
    out[t][0] = uint8_t(rgb[2] * a / 255);  // B
    out[t][1] = uint8_t(rgb[1] * a / 255);  // G
    out[t][2] = uint8_t(rgb[0] * a / 255);  // R
    out[t][3] = uint8_t(a);
  }
}

// The logo's bitmap for the target being drawn on (made once per target), null without the file.
ID2D1Bitmap* LogoBitmap() {
  static ID2D1RenderTarget* made_for = nullptr;
  static ComPtr<ID2D1Bitmap> bitmap;
  static std::vector<uint8_t> pixels;
  static bool read = false;
  if (!read) {
    read = true;
    const size_t want = size_t(kLogoWidth / 4) * (kLogoHeight / 4) * 16;
    std::vector<uint8_t> blocks;
    for (const char* file : {"launcher_logo.bin", "7463904A23B98655.bin"}) {
      std::ifstream in(g_dir / "textures" / file, std::ios::binary);
      blocks.assign(std::istreambuf_iterator<char>(in), {});
      if (blocks.size() == want) break;
      g_logo_source = kGameLogoSource;
    }
    if (blocks.size() == want) {
      pixels.resize(size_t(kLogoWidth) * kLogoHeight * 4);
      uint8_t texels[16][4];
      for (uint32_t by = 0; by < kLogoHeight / 4; ++by)
        for (uint32_t bx = 0; bx < kLogoWidth / 4; ++bx) {
          DecodeBc3Block(blocks.data() + (size_t(by) * (kLogoWidth / 4) + bx) * 16, texels);
          for (int t = 0; t < 16; ++t)
            std::memcpy(&pixels[((size_t(by) * 4 + t / 4) * kLogoWidth + bx * 4 + t % 4) * 4],
                        texels[t], 4);
        }
    }
  }
  if (pixels.empty()) return nullptr;
  if (made_for != g_render) {
    made_for = g_render;
    bitmap.Reset();
    g_render->CreateBitmap(D2D1::SizeU(kLogoWidth, kLogoHeight), pixels.data(), kLogoWidth * 4,
                           D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                                                    D2D1_ALPHA_MODE_PREMULTIPLIED)),
                           &bitmap);
  }
  return bitmap.Get();
}

void DrawHeader() {
  if (ID2D1Bitmap* logo = LogoBitmap()) {
    // The logo over the heading's height (the red rule at 160).
    const float height = 150;
    const D2D1_RECT_F& source = g_logo_source;
    const float width = height * (source.right - source.left) / (source.bottom - source.top);
    g_render->DrawBitmap(logo, {36, 6, 36 + width, 6 + height}, 1.0f,
                         D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, source);
  } else {
    Text(L"PROJECT GOTHAM", g_title_small.Get(), {50, 20, 600, 52}, kInk);
    OutlinedText(L"RACING 4", g_title_big.Get(), {44, 44, 600, 128}, kWhite, kRed, 3);
    const float x = 52 + TextWidth(L"RACING 4", g_title_big.Get()) + 10;
    FillSlanted(x, 80, x + 54, 112, 10, Brush(kRed));
    Text(L"PC", g_badge.Get(), {x + 2, 80, x + 62, 112}, kWhite, DWRITE_TEXT_ALIGNMENT_CENTER);
  }
  if (!g.profile.empty()) {
    Text(g.profile, g_menu.Get(), {600, 112, 1000, 150}, kInk, DWRITE_TEXT_ALIGNMENT_TRAILING);
  }
  // Minimize and close.
  if (g_hover_button == 0) g_render->FillRectangle(kMinimize, Brush(kInk, 0.10f));
  if (g_hover_button == 1) g_render->FillRectangle(kClose, Brush(kRed));
  g_render->DrawLine({960, 25}, {972, 25}, Brush(kInk), 1.5f);
  const D2D1_COLOR_F cross = g_hover_button == 1 ? kWhite : kInk;
  g_render->DrawLine({1004, 19}, {1016, 31}, Brush(cross), 1.5f);
  g_render->DrawLine({1016, 19}, {1004, 31}, Brush(cross), 1.5f);
}

void DrawMenu() {
  for (int i = 0; i < int(g_rows.size()); ++i) {
    const Row row = g_rows[i];
    const D2D1_RECT_F r = RowRect(i);
    const bool selected = i == g_selected;
    const bool play = row == Row::kPlay;
    const bool disabled = play && !Ready();
    if (selected) {
      FillSlanted(r.left - 14, r.top, r.right, r.bottom, 14, Brush(disabled ? kGrey : kRed));
      FillSlanted(r.right + 4, r.top, r.right + 14, r.bottom, 14,
                  Brush(disabled ? kGrey : kDarkRed));
    } else if (play) {
      FillSlanted(r.left - 14, r.top, r.right, r.bottom, 14, Brush(kInk, 0.08f));
    }
    const D2D1_COLOR_F ink = selected ? kWhite : disabled ? kGrey : kInk;
    IDWriteTextFormat* format = play                                        ? g_menu_big.Get()
                                : TextWidth(RowLabel(row), g_menu.Get()) > 280 ? g_menu_small.Get()
                                                                               : g_menu.Get();
    if (play && !disabled && !selected) {
      OutlinedText(RowLabel(row), format, {r.left + 12, r.top, r.right, r.bottom}, kWhite, kRed,
                   2);
    } else {
      Text(RowLabel(row), format, {r.left + 12, r.top, r.right, r.bottom}, ink);
    }
    const float label_width = TextWidth(RowLabel(row), format);
    const float value_left = std::min(std::max(r.left + 40 + label_width, 330.0f), 400.0f);
    if (row == Row::kName && g_editing) {
      const float right = r.right - 16;
      const float width = TextWidth(g_edit, g_name.Get());
      const float left = right - std::max(width, 12.0f) - 14;
      g_render->FillRectangle({left - 8, r.top + 5, right + 6, r.bottom - 5}, Brush(kWhite, 0.95f));
      const float x = left + TextWidth(g_edit.substr(0, g_cursor), g_name.Get());
      const float cw = g_cursor < g_edit.size()
                           ? TextWidth(g_edit.substr(g_cursor, 1), g_name.Get())
                           : 12.0f;
      if (std::fmod(g_time, 1.0) < 0.6) {
        g_render->FillRectangle({x - 1, r.bottom - 11, x + cw + 1, r.bottom - 8}, Brush(kRed));
      }
      g_render->FillRectangle({x - 1, r.top + 8, x + cw + 1, r.bottom - 11}, Brush(kRed, 0.18f));
      Text(g_edit, g_name.Get(), {left, r.top, right + 40, r.bottom}, kInk);
      continue;
    }
    const std::wstring value = RowValue(row);
    if (value.empty()) continue;
    if (IsSpinner(row)) {
      // The game's spinner: the value between two red arrows.
      const float right = r.right - 16, cy = (r.top + r.bottom) / 2;
      const D2D1_COLOR_F arrow = selected ? kWhite : kRed;
      FillTriangle({value_left, cy}, {value_left + 9, cy - 8}, {value_left + 9, cy + 8},
                   Brush(arrow));
      FillTriangle({right, cy}, {right - 9, cy - 8}, {right - 9, cy + 8}, Brush(arrow));
      Text(value, g_value.Get(), {value_left + 14, r.top, right - 14, r.bottom}, ink,
           DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
      Text(value, g_value.Get(), {value_left, r.top, r.right - 16, r.bottom},
           selected ? kWhite : kGrey, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
  }
}

void DrawCheck(float y, const wchar_t* label, bool ok, const std::wstring& detail) {
  const D2D1_ELLIPSE dot = {{kPanel.left + 30, y + 14}, 9, 9};
  g_render->FillEllipse(dot, Brush(ok ? kGreen : kRed));
  if (ok) {
    g_render->DrawLine({dot.point.x - 4.5f, dot.point.y}, {dot.point.x - 1, dot.point.y + 4},
                       Brush(kWhite), 2);
    g_render->DrawLine({dot.point.x - 1, dot.point.y + 4}, {dot.point.x + 5, dot.point.y - 4},
                       Brush(kWhite), 2);
  } else {
    g_render->DrawLine({dot.point.x - 4, dot.point.y - 4}, {dot.point.x + 4, dot.point.y + 4},
                       Brush(kWhite), 2);
    g_render->DrawLine({dot.point.x + 4, dot.point.y - 4}, {dot.point.x - 4, dot.point.y + 4},
                       Brush(kWhite), 2);
  }
  Text(label, g_menu.Get(), {kPanel.left + 50, y - 2, kPanel.right - 16, y + 30}, kInk);
  Text(detail, g_small.Get(), {kPanel.left + 50, y + 28, kPanel.right - 16, y + 48},
       ok ? kGrey : kRed);
}

void DrawPanel() {
  const bool show_update = !kHasBuiltInUpdate || (g.game_ok && !g.update_ok);
  const float bottom = kPanel.top + 50 + (show_update ? 5 : 4) * 58.0f + 8;
  FillSlanted(kPanel.left, kPanel.top, kPanel.right, bottom, 0, Brush(kWhite, 0.75f));
  const float badge = TextWidth(T(kChecks), g_badge.Get()) + 40;
  FillSlanted(kPanel.left, kPanel.top, kPanel.left + badge, kPanel.top + 34, 10, Brush(kRed));
  Text(T(kChecks), g_badge.Get(), {kPanel.left + 6, kPanel.top, kPanel.left + badge + 16, kPanel.top + 34},
       kWhite, DWRITE_TEXT_ALIGNMENT_CENTER);
  float y = kPanel.top + 50;
  DrawCheck(y, T(kCheckGame), g.game_ok,
            g.game_ok ? T(kFound) : g.game_found ? T(kGameWrong) : T(kMissing));
  y += 58;
  // With the title update built in, the player never needs to know about it: shown only if
  // installing it failed.
  if (!kHasBuiltInUpdate || (g.game_ok && !g.update_ok)) {
    DrawCheck(y, T(kCheckUpdate), g.update_ok, g.update_text);
    y += 58;
  }
  DrawCheck(y, T(kCheckVc), g.vc_ok, g.vc_ok ? T(kVcOk) : T(kVcMissing));
  y += 58;
  DrawCheck(y, T(kCheckGpu), g.gpu_ok,
            g.gpu_ok            ? g.gpu_name + L" · " + (g.rtx ? T(kDlss) : T(kFsr))
            : g.gpu_unsupported ? std::wstring(T(kGpuUnsupported))
                                : std::wstring(T(kNoGpu)));
  y += 58;
  // The game is played with a controller: PLAY waits for one.
  DrawCheck(y, T(kCheckPad), g.pad_ok, g.pad_ok ? g.pad_name : std::wstring(T(kPadMissing)));
}

// A PlayStation face button: a dark disc and its symbol in its colour, for an Xbox letter.
void DrawPlayStationButton(float x, float y, wchar_t letter) {
  const D2D1_POINT_2F c = {x + 13, y + 13};
  g_render->FillEllipse({c, 13, 13}, Brush(D2D1::ColorF(0.13f, 0.13f, 0.15f)));
  const float k = 6.0f, w = 2.4f;
  switch (letter) {
    case L'A': {  // cross
      const D2D1_COLOR_F blue = D2D1::ColorF(0.49f, 0.70f, 0.91f);
      g_render->DrawLine({c.x - k, c.y - k}, {c.x + k, c.y + k}, Brush(blue), w);
      g_render->DrawLine({c.x - k, c.y + k}, {c.x + k, c.y - k}, Brush(blue), w);
      break;
    }
    case L'B':  // circle
      g_render->DrawEllipse({c, k, k}, Brush(D2D1::ColorF(1.0f, 0.40f, 0.40f)), w);
      break;
    case L'X':  // square
      g_render->DrawRectangle({c.x - k * 0.86f, c.y - k * 0.86f, c.x + k * 0.86f, c.y + k * 0.86f},
                              Brush(D2D1::ColorF(0.90f, 0.63f, 0.82f)), w);
      break;
    case L'Y': {  // triangle
      const D2D1_COLOR_F green = D2D1::ColorF(0.25f, 0.89f, 0.63f);
      const D2D1_POINT_2F p[3] = {{c.x, c.y - k * 1.05f}, {c.x + k * 1.05f, c.y + k * 0.75f},
                                  {c.x - k * 1.05f, c.y + k * 0.75f}};
      for (int i = 0; i < 3; ++i) g_render->DrawLine(p[i], p[(i + 1) % 3], Brush(green), w);
      break;
    }
  }
}

void DrawButtonHint(float x, float y, const wchar_t* letter, D2D1_COLOR_F color,
                    const wchar_t* label) {
  if (g_playstation) {
    DrawPlayStationButton(x, y, letter[0]);
  } else {
    g_render->FillEllipse({{x + 13, y + 13}, 13, 13}, Brush(color));
    Text(letter, g_badge.Get(), {x, y - 1, x + 26, y + 26}, kWhite, DWRITE_TEXT_ALIGNMENT_CENTER);
  }
  Text(label, g_menu.Get(), {x + 32, y - 4, x + 400, y + 30}, kInk);
}

// The precompile step's bar, in place of the help line.
void DrawPrecompile(float top) {
  const float left = kMenuLeft, right = 610, y = top + 28;
  const float part =
      std::min(1.0f, float(g_precompile_done) / float(std::max<size_t>(g_precompile_total, 1)));
  Text(g_precompile_total ? std::wstring(T(kPrecompile)) + L"  " + std::to_wstring(g_precompile_done) +
                                L" / " + std::to_wstring(g_precompile_total)
                          : std::wstring(T(kPrecompile)),
       g_help.Get(), {left, top, right, top + 24}, kInk);
  FillSlanted(left, y, right, y + 12, 4, Brush(kInk, 0.15f));
  if (part > 0) FillSlanted(left, y, left + (right - left) * part, y + 12, 4, Brush(kRed));
}

void DrawFooter() {
  if (g_precompile) {
    DrawPrecompile(std::max(498.0f, RowRect(int(g_rows.size()) - 1).bottom + 8));
    return;
  }
  const std::wstring help = !g.message.empty() ? g.message
                           : g_editing         ? std::wstring(T(kNameEditHelp))
                                               : RowHelp(g_rows[std::clamp(g_selected, 0, int(g_rows.size()) - 1)]);
  const float top = std::max(498.0f, RowRect(int(g_rows.size()) - 1).bottom + 8);
  Text(help, g_help.Get(), {kMenuLeft, top, 610, top + 50}, g.message.empty() ? kInk : kRed);
  struct Hint {
    const wchar_t* letter;
    D2D1_COLOR_F color;
    Str label;
  };
  std::vector<Hint> hints = {{L"A", kGreen, kSelect}};
  if (Ready()) hints.push_back({L"Y", kYellow, kLaunchHint});
  hints.push_back({L"B", kRed, kBack});
  float x = 1000;
  for (int i = int(hints.size()) - 1; i >= 0; --i) {
    x -= 32 + TextWidth(T(hints[i].label), g_menu.Get());
    DrawButtonHint(x, 560, hints[i].letter, hints[i].color, T(hints[i].label));
    x -= 22;
  }
}

void Draw() {
  g_render->SetTransform(D2D1::Matrix3x2F::Identity());
  DrawBackdrop();
  DrawHeader();
  DrawMenu();
  DrawPanel();
  DrawFooter();
}

// --render <file.png>: the launcher drawn into an image at 2x, for checking its look.
bool RenderToPng(const fs::path& file) {
  ComPtr<IWICImagingFactory> wic;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&wic)))) {
    return false;
  }
  const UINT width = UINT(kWidth * 2), height = UINT(kHeight * 2);
  ComPtr<IWICBitmap> bitmap;
  wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap);
  ComPtr<ID2D1RenderTarget> target;
  g_d2d->CreateWicBitmapRenderTarget(
      bitmap.Get(),
      D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                   D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                                     D2D1_ALPHA_MODE_PREMULTIPLIED),
                                   192, 192),
      &target);
  target->CreateSolidColorBrush(kInk, &g_brush);
  const D2D1_GRADIENT_STOP stops[] = {{0, {0.97f, 0.97f, 0.97f, 1}},
                                      {0.6f, {0.90f, 0.90f, 0.91f, 1}},
                                      {1, {0.80f, 0.80f, 0.82f, 1}}};
  ComPtr<ID2D1GradientStopCollection> collection;
  target->CreateGradientStopCollection(stops, 3, &collection);
  target->CreateLinearGradientBrush(
      D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, kHeight)),
      collection.Get(), &g_backdrop);
  g_render = target.Get();
  target->BeginDraw();
  Draw();
  target->EndDraw();
  g_render = nullptr;
  ComPtr<IWICStream> stream;
  wic->CreateStream(&stream);
  if (FAILED(stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE))) return false;
  ComPtr<IWICBitmapEncoder> encoder;
  wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
  encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
  ComPtr<IWICBitmapFrameEncode> frame;
  encoder->CreateNewFrame(&frame, nullptr);
  frame->Initialize(nullptr);
  frame->SetSize(width, height);
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
  frame->SetPixelFormat(&format);
  frame->WriteSource(bitmap.Get(), nullptr);
  frame->Commit();
  encoder->Commit();
  return true;
}

void Render(HWND hwnd) {
  CreateTarget(hwnd);
  g_render = g_target.Get();
  g_render->BeginDraw();
  Draw();
  if (g_render->EndDraw() == D2DERR_RECREATE_TARGET) {
    g_target.Reset();
    g_brush.Reset();
    g_backdrop.Reset();
  }
}

// ---------------------------------------------------------------------------------------------
// Input.

D2D1_POINT_2F ToDips(LPARAM lparam) {
  return {GET_X_LPARAM(lparam) * 96.0f / g_dpi, GET_Y_LPARAM(lparam) * 96.0f / g_dpi};
}

bool Inside(D2D1_RECT_F r, D2D1_POINT_2F p) {
  return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
}

int RowAt(D2D1_POINT_2F p) {
  for (int i = 0; i < int(g_rows.size()); ++i) {
    D2D1_RECT_F r = RowRect(i);
    r.left -= 14;
    if (Inside(r, p)) return i;
  }
  return -1;
}

void Move(int delta) {
  g.message.clear();
  g_selected = (g_selected + delta + int(g_rows.size())) % int(g_rows.size());
}

// The controller, polled each frame: the first one connected, with key repeat on the d-pad and
// the left stick.
struct Pad {
  WORD buttons = 0;
  ULONGLONG repeat_at = 0;
} g_pad;

// SDL's gamepads (local change): PlayStation controllers too, which XInput does not see, and
// their family for the button hints (g_playstation). XInput if SDL does not start.
bool g_sdl = false;
SDL_Gamepad* g_gamepad = nullptr;

void InitPads() {
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  g_sdl = SDL_Init(SDL_INIT_GAMEPAD);
}

// The first gamepad's buttons, as XInput's; false without one.
bool ReadSdlPad(WORD& buttons, SHORT& lx, SHORT& ly) {
  SDL_UpdateGamepads();
  if (g_gamepad && !SDL_GamepadConnected(g_gamepad)) {
    SDL_CloseGamepad(g_gamepad);
    g_gamepad = nullptr;
  }
  if (!g_gamepad) {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (ids && count > 0) g_gamepad = SDL_OpenGamepad(ids[0]);
    SDL_free(ids);
    if (!g_gamepad) return false;
  }
  const SDL_GamepadType type = SDL_GetGamepadType(g_gamepad);
  g_playstation = g_force_playstation || type == SDL_GAMEPAD_TYPE_PS3 ||
                  type == SDL_GAMEPAD_TYPE_PS4 || type == SDL_GAMEPAD_TYPE_PS5;
  struct Map {
    SDL_GamepadButton sdl;
    WORD xinput;
  };
  static constexpr Map kMap[] = {
      {SDL_GAMEPAD_BUTTON_SOUTH, XINPUT_GAMEPAD_A},       {SDL_GAMEPAD_BUTTON_EAST, XINPUT_GAMEPAD_B},
      {SDL_GAMEPAD_BUTTON_WEST, XINPUT_GAMEPAD_X},        {SDL_GAMEPAD_BUTTON_NORTH, XINPUT_GAMEPAD_Y},
      {SDL_GAMEPAD_BUTTON_DPAD_UP, XINPUT_GAMEPAD_DPAD_UP},
      {SDL_GAMEPAD_BUTTON_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_DOWN},
      {SDL_GAMEPAD_BUTTON_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_LEFT},
      {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT},
  };
  buttons = 0;
  for (const Map& m : kMap)
    if (SDL_GetGamepadButton(g_gamepad, m.sdl)) buttons |= m.xinput;
  lx = SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_LEFTX);
  // SDL's y grows downward, XInput's upward.
  ly = SHORT(std::clamp(-int(SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_LEFTY)), -32768, 32767));
  return true;
}

// Whether a controller is connected, each frame: SDL's first gamepad (its name), else an XInput
// pad. A change makes the rows again (PLAY needs one).
void CheckPad() {
  bool ok = false;
  std::wstring name;
  if (g_sdl) {
    SDL_UpdateGamepads();
    if (g_gamepad && !SDL_GamepadConnected(g_gamepad)) {
      SDL_CloseGamepad(g_gamepad);
      g_gamepad = nullptr;
    }
    if (!g_gamepad) {
      int count = 0;
      SDL_JoystickID* ids = SDL_GetGamepads(&count);
      if (ids && count > 0) g_gamepad = SDL_OpenGamepad(ids[0]);
      SDL_free(ids);
    }
    if (g_gamepad) {
      ok = true;
      const char* n = SDL_GetGamepadName(g_gamepad);
      name = Widen(n ? n : "");
    }
  }
  if (!ok) {
    XINPUT_STATE state = {};
    for (DWORD i = 0; i < XUSER_MAX_COUNT && !ok; ++i) ok = XInputGetState(i, &state) == ERROR_SUCCESS;
    if (ok) name = L"XInput";
  }
  g.pad_name = name;
  if (ok != g.pad_ok) {
    g.pad_ok = ok;
    BuildRows();
  }
}

void PollPad(HWND hwnd) {
  WORD buttons = 0;
  SHORT lx = 0, ly = 0;
  if (!g_sdl || !ReadSdlPad(buttons, lx, ly)) {
    XINPUT_STATE state = {};
    DWORD i = 0;
    for (; i < XUSER_MAX_COUNT; ++i)
      if (XInputGetState(i, &state) == ERROR_SUCCESS) break;
    if (i == XUSER_MAX_COUNT) {
      g_pad.buttons = 0;
      g_playstation = g_force_playstation;  // no controller: Xbox icons
      return;
    }
    g_playstation = g_force_playstation;
    buttons = state.Gamepad.wButtons;
    lx = state.Gamepad.sThumbLX;
    ly = state.Gamepad.sThumbLY;
  }
  constexpr SHORT kDead = 16000;
  if (ly > kDead) buttons |= XINPUT_GAMEPAD_DPAD_UP;
  if (ly < -kDead) buttons |= XINPUT_GAMEPAD_DPAD_DOWN;
  if (lx > kDead) buttons |= XINPUT_GAMEPAD_DPAD_RIGHT;
  if (lx < -kDead) buttons |= XINPUT_GAMEPAD_DPAD_LEFT;
  const WORD pressed = buttons & ~g_pad.buttons;
  constexpr WORD kDirections = XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN |
                               XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT;
  WORD act = pressed;
  const ULONGLONG now = GetTickCount64();
  if (pressed & kDirections) {
    g_pad.repeat_at = now + 400;
  } else if ((buttons & kDirections) && now >= g_pad.repeat_at) {
    act |= buttons & kDirections;
    g_pad.repeat_at = now + 120;
  }
  g_pad.buttons = buttons;
  if (g_editing) {
    if (act & XINPUT_GAMEPAD_DPAD_UP) EditCycle(1);
    if (act & XINPUT_GAMEPAD_DPAD_DOWN) EditCycle(-1);
    if (act & XINPUT_GAMEPAD_DPAD_LEFT) EditMove(-1);
    if (act & XINPUT_GAMEPAD_DPAD_RIGHT) EditMove(1);
    if (act & XINPUT_GAMEPAD_X) EditErase();
    if (act & XINPUT_GAMEPAD_A) EndEdit(true);
    if (act & XINPUT_GAMEPAD_B) EndEdit(false);
    return;
  }
  if (act & XINPUT_GAMEPAD_DPAD_UP) Move(-1);
  if (act & XINPUT_GAMEPAD_DPAD_DOWN) Move(1);
  if (act & XINPUT_GAMEPAD_DPAD_LEFT) Step(g_rows[g_selected], -1);
  if (act & XINPUT_GAMEPAD_DPAD_RIGHT) Step(g_rows[g_selected], 1);
  if (act & XINPUT_GAMEPAD_A) Activate(hwnd, g_rows[g_selected]);
  if (act & XINPUT_GAMEPAD_Y) PlayNow(hwnd);
  if (act & XINPUT_GAMEPAD_B) DestroyWindow(hwnd);
}

static int g_autostart_ticks = 0;  // --autostart (wWinMain)

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_TIMER:
      g_time += 0.016;
      PollPrecompile(hwnd);
      if (!IsWindow(hwnd)) return 0;
      CheckPad();
      // Local change (pgr4 project): --autostart, for some 5 seconds at most: a check still
      // missing then (no controller, say) leaves the launcher open, as PLAY would.
      if (g_autostart_ticks > 0) {
        --g_autostart_ticks;
        if (Ready()) {
          g_autostart_ticks = 0;
          PlayNow(hwnd);
          if (!IsWindow(hwnd)) return 0;
        }
      }
      if (GetForegroundWindow() == hwnd) PollPad(hwnd);
      if (IsWindow(hwnd)) InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      BeginPaint(hwnd, &ps);
      Render(hwnd);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_SIZE:
      if (g_target) g_target->Resize(D2D1::SizeU(LOWORD(lparam), HIWORD(lparam)));
      return 0;
    case WM_DPICHANGED: {
      g_dpi = float(HIWORD(wparam));
      if (g_target) g_target->SetDpi(g_dpi, g_dpi);
      const RECT* r = reinterpret_cast<const RECT*>(lparam);
      SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
    }
    case WM_NCHITTEST: {
      POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ScreenToClient(hwnd, &pt);
      const D2D1_POINT_2F p = {pt.x * 96.0f / g_dpi, pt.y * 96.0f / g_dpi};
      if (p.y < 160 && !Inside(kMinimize, p) && !Inside(kClose, p)) return HTCAPTION;
      return HTCLIENT;
    }
    case WM_MOUSEMOVE: {
      const D2D1_POINT_2F p = ToDips(lparam);
      g_hover_button = Inside(kMinimize, p) ? 0 : Inside(kClose, p) ? 1 : -1;
      const int row = RowAt(p);
      if (!g_editing && row >= 0 && row != g_selected) {
        g.message.clear();
        g_selected = row;
      }
      TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd, 0};
      TrackMouseEvent(&track);
      return 0;
    }
    case WM_MOUSELEAVE:
      g_hover_button = -1;
      return 0;
    case WM_LBUTTONUP: {
      const D2D1_POINT_2F p = ToDips(lparam);
      if (Inside(kMinimize, p)) {
        ShowWindow(hwnd, SW_MINIMIZE);
        return 0;
      }
      if (Inside(kClose, p)) {
        DestroyWindow(hwnd);
        return 0;
      }
      const int row = RowAt(p);
      if (g_editing) {
        // A click elsewhere keeps the name typed, if it is one.
        if (row < 0 || g_rows[row] != Row::kName) EndEdit(true);
        return 0;
      }
      if (row < 0) return 0;
      g_selected = row;
      if (IsSpinner(g_rows[row])) {
        // Left of the value's middle steps back, right of it forward.
        const D2D1_RECT_F r = RowRect(row);
        Step(g_rows[row], p.x < (330.0f + r.right) / 2 ? -1 : 1);
      } else {
        Activate(hwnd, g_rows[row]);
      }
      return 0;
    }
    case WM_CHAR:
      if (g_editing && wparam >= 32) EditType(wchar_t(wparam));
      return 0;
    case WM_KEYDOWN:
      if (g_editing) {
        switch (wparam) {
          case VK_LEFT: EditMove(-1); break;
          case VK_RIGHT: EditMove(1); break;
          case VK_UP: EditCycle(1); break;
          case VK_DOWN: EditCycle(-1); break;
          case VK_HOME: g_cursor = 0; break;
          case VK_END: g_cursor = g_edit.size(); break;
          case VK_BACK:
            if (g_cursor > 0) g_edit.erase(--g_cursor, 1);
            break;
          case VK_DELETE: EditErase(); break;
          case VK_RETURN: EndEdit(true); break;
          case VK_ESCAPE: EndEdit(false); break;
        }
        return 0;
      }
      switch (wparam) {
        case VK_UP: Move(-1); break;
        case VK_DOWN: Move(1); break;
        case VK_LEFT: Step(g_rows[g_selected], -1); break;
        case VK_RIGHT: Step(g_rows[g_selected], 1); break;
        case VK_RETURN:
        case VK_SPACE: Activate(hwnd, g_rows[g_selected]); break;
        case VK_F5: PlayNow(hwnd); break;
        case VK_ESCAPE: DestroyWindow(hwnd); break;
      }
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace

// --autostart (the game, after a change of render scale): the game gone first, then as PLAY once
// the checks are done (the controller's comes with the timer: WM_TIMER).
static void WaitForGameExit() {
  for (int tries = 0; tries < 300; ++tries) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    bool found = false;
    PROCESSENTRY32W e = {sizeof(e)};
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e))
      if (_wcsicmp(e.szExeFile, L"pgr4.exe") == 0) found = true;
    CloseHandle(snap);
    if (!found) return;
    Sleep(100);
  }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  InitPads();

  // The executable's path, however long (MAX_PATH would cut a deep folder's).
  std::wstring exe(MAX_PATH, L'\0');
  for (;;) {
    const DWORD n = GetModuleFileNameW(nullptr, exe.data(), DWORD(exe.size()));
    if (n < exe.size()) {
      exe.resize(n);
      break;
    }
    exe.resize(exe.size() * 2);
  }
  g_dir = fs::path(exe).parent_path();

  D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, g_d2d.GetAddressOf());
  DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                      reinterpret_cast<IUnknown**>(g_dwrite.GetAddressOf()));
  CreateFormats();
  WriteDefaultToml();
  Load();
  BuildRows();

  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argc >= 3 && std::wstring(argv[1]) == L"--render") {
    if (argc >= 4) g_selected = std::clamp(_wtoi(argv[3]), 0, int(g_rows.size()) - 1);
    if (argc >= 5) {
      g.language = std::clamp(_wtoi(argv[4]), 0, int(std::size(kLanguages)) - 1);
      g_texts = kTexts[g.language];
      Check();
    }
    const bool ok = RenderToPng(argv[2]);
    CoUninitialize();
    return ok ? 0 : 1;
  }

  WNDCLASSEXW wc = {sizeof(wc)};
  wc.style = CS_DROPSHADOW;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
  wc.lpszClassName = L"PGR4Launcher";
  RegisterClassExW(&wc);

  // Centered on the primary monitor, at its scale.
  const POINT origin = {0, 0};
  HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
  UINT dpi_x = 96, dpi_y = 96;
  using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
  if (HMODULE shcore = LoadLibraryW(L"shcore.dll")) {
    if (auto get = reinterpret_cast<GetDpiForMonitorFn>(GetProcAddress(shcore, "GetDpiForMonitor")))
      get(monitor, 0, &dpi_x, &dpi_y);
  }
  g_dpi = float(dpi_x);
  MONITORINFO info = {sizeof(info)};
  GetMonitorInfoW(monitor, &info);
  const int width = int(kWidth * g_dpi / 96), height = int(kHeight * g_dpi / 96);
  const int x = (info.rcWork.left + info.rcWork.right - width) / 2;
  const int y = (info.rcWork.top + info.rcWork.bottom - height) / 2;
  HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"Project Gotham Racing 4",
                              WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU, x, y, width, height, nullptr,
                              nullptr, instance, nullptr);
  const DWORD round = 2;  // DWMWCP_ROUND (Windows 11)
  DwmSetWindowAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof(round));
  ShowWindow(hwnd, show);
  SetTimer(hwnd, 1, 16, nullptr);
  // Local change (pgr4 project): the game asked to run again: as PLAY, its shaders first.
  for (int i = 1; i < argc; ++i)
    if (std::wstring(argv[i]) == L"--autostart") {
      WaitForGameExit();
      g_autostart_ticks = 300;
    }

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  CoUninitialize();
  return 0;
}
