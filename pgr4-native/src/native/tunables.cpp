// pgr4-native PGR4 native renderer: the game's settings forced live. See tunables.h.
#include "tunables.h"

#include "log.h"

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

REXCVAR_DECLARE(std::string, pgr4_native_capture_dir);
REXCVAR_DECLARE(double, pgr4_draw_distance);  // src/pgr4_hooks.cpp
REXCVAR_DECLARE(int32_t, pgr4_draw_distance_mask);
REXCVAR_DECLARE(double, pgr4_pvs_ahead);
REXCVAR_DECLARE(bool, pgr4_pvs_refresh);
REXCVAR_DECLARE(bool, pgr4_native_original_look);  // renderer.cpp
// Local change (pgr4 project): the game's Graphics page, ENHANCED DETAILS: 0 original, 1 high, 2 ultra.
REXCVAR_DEFINE_INT32(pgr4_native_details, 0, "PGR4 native",
                     "The scenery's details and distance: 0 the game's, 1 high, 2 ultra")
    .range(0, 2);
REXCVAR_DEFINE_STRING(pgr4_native_tunable_set, "", "PGR4 native",
                      "Development: the game's settings forced (\"name=value;name=value\"; "
                      "reset: none)");

std::atomic<int32_t> g_pgr4_world_max_lod{-1};
std::atomic<int32_t> g_pgr4_force_world_max_lod{-1};

namespace pgr4::native {
namespace {

enum class Kind { kBool, kInt, kFloat };

struct Tunable {
  const char* group;
  const char* name;
  uint32_t address;  // its value; the name's pointer 16 bytes before it, its flags 8
  Kind kind;
  float min, max;
  const char* note;
  // State.
  bool found = false, checked = false, forced = false, saved = false;
  float value = 0, original = 0;
};

// The settings' table (work/rea/variables_moteur.tsv, the title update's image): only those the
// game's code reads. Which are read at a race's load only is not known: try reloading.
constexpr const char* kLoad = "Streaming : chargement du decor";
constexpr const char* kAhead = "Streaming : anticipation";
constexpr const char* kScenery = "Decor : distances et LOD";
constexpr const char* kVisible = "Visibilite";
constexpr const char* kCrowd = "Public";
constexpr const char* kCars = "Voitures";
Tunable g_tunables[] = {
    {kLoad, "World.Streaming.ForceMemoryLimitInMb", 0x82AA2628, Kind::kInt, 64, 2048, "Plafond memoire des LOD (jeu : 256 Mo)"},
    {kLoad, "World.Streaming.ReduceLodsIfLowMemory", 0x82AA2730, Kind::kBool, 0, 1, "Decoche : LOD maximal fixe a 10"},
    {kLoad, "World.Streaming.MaxLod", 0x82A99FB8, Kind::kInt, 0, 10, ""},
    {kLoad, "World.Streaming.LodOffset", 0x82A99F84, Kind::kInt, -10, 10, ""},
    {kLoad, "World.Streaming.ForceLod", 0x82A99F2C, Kind::kInt, -1, 10, "-1 : aucun"},
    {kLoad, "World.Streaming.ScreenSizeScale", 0x82AA2754, Kind::kFloat, 0, 4, ""},
    {kLoad, "World.Streaming.DebugLoadAllBlocks", 0x82AB4D80, Kind::kBool, 0, 1, "Tout charger : memoire a surveiller"},
    {kLoad, "World.Streaming.ForceRequest", 0x82AA2690, Kind::kBool, 0, 1, ""},
    {kLoad, "World.Streaming.PrioritiseByLodDifference", 0x82A99FEC, Kind::kBool, 0, 1, ""},
    {kLoad, "World.Streaming.BlockLargeRequestSize", 0x82AA2788, Kind::kFloat, 0, 8000, ""},
    {kLoad, "World.Streaming.DisableStreaming", 0x82A99F60, Kind::kBool, 0, 1, "Coupe le chargement"},
    {kAhead, "World.Streaming.LookAhead.LookAheadTime", 0x82AA265C, Kind::kFloat, 0, 20, "Secondes"},
    {kAhead, "World.Streaming.LookAhead.VelocitySmoothing", 0x82AAC704, Kind::kFloat, 0, 1, ""},
    {kAhead, "World.Streaming.LookAhead.AccelerationSmoothing", 0x82AAC738, Kind::kFloat, 0, 1, ""},
    {kAhead, "World.Streaming.LookAhead.SteerSmoothing", 0x82AAC76C, Kind::kFloat, 0, 1, ""},
    {kScenery, "World.Instances.GeneralInstanceCullScale", 0x82A9856C, Kind::kFloat, 0.1f, 8, ""},
    {kScenery, "World.Instances.OverrideInstanceCullScale", 0x82A98548, Kind::kBool, 0, 1, ""},
    {kScenery, "World.Instances.CubemapInstanceCullScale", 0x82A985A0, Kind::kFloat, 0, 4, "Decor dans le cube de reflets"},
    {kScenery, "World.Instances.InstanceLodMaxThresholdScale", 0x82A994A8, Kind::kFloat, 0, 4, ""},
    {kScenery, "World.Instances.InstanceLodMinThresholdScale", 0x82A99474, Kind::kFloat, 0, 4, ""},
    {kScenery, "World.Instances.MeshLodMaxThresholdScale", 0x82A99440, Kind::kFloat, 0, 4, ""},
    {kScenery, "World.Instances.MeshLodMinThresholdScale", 0x82A9940C, Kind::kFloat, 0, 4, ""},
    {kScenery, "World.Instances.LowLodOffset", 0x82A99EE4, Kind::kFloat, 0, 1, ""},
    {kScenery, "World.Instances.HideTreeBillboardsLods", 0x82A98E7C, Kind::kBool, 0, 1, ""},
    {kScenery, "World.Instances.ShowCityLodMesh", 0x82A98EA0, Kind::kBool, 0, 1, ""},
    {kVisible, "World.PrecalcPVS.UsePVS", 0x82AA214C, Kind::kBool, 0, 1, "Decoche : textures manquantes"},
    {kVisible, "World.PrecalcPVS.UpdatePVS", 0x82AA2128, Kind::kBool, 0, 1, ""},
    {kVisible, "World.DisableFrustrumCulling", 0x82A996A0, Kind::kBool, 0, 1, "Couteux"},
    {kCrowd, "Crowds.Render.LODScalingFactor", 0x82AA3B4C, Kind::kFloat, 0, 8, ""},
    {kCrowd, "Crowds.Layout.RowLODBias", 0x82AA3364, Kind::kFloat, 0, 2, ""},
    {kCrowd, "Crowds.Layout.RowLODJitter", 0x82AA3398, Kind::kFloat, 0, 1, ""},
    {kCrowd, "Crowds.Flags.NearFlagLodFadeDistance", 0x82AA3B80, Kind::kFloat, 0, 500, ""},
    {kCrowd, "Crowds.Flags.FarFlagLodFadeDistance", 0x82AA3BB4, Kind::kFloat, 0, 500, ""},
    {kCars, "Car.Render.Shadow.DrawDistance", 0x82A94404, Kind::kFloat, 0, 8, ""},
    {kCars, "Car.Renderer.LoddedInteriorScreenSize", 0x82AAB86C, Kind::kFloat, 0, 2, ""},
};

uint32_t Be32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void PutBe32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}
uint8_t* Base() {
  auto* const memory = rex::system::kernel_memory();
  return memory ? memory->virtual_membase() : nullptr;
}
float Read(const uint8_t* base, const Tunable& t) {
  const uint8_t* const p = base + t.address;
  if (t.kind == Kind::kBool) return p[0] ? 1.0f : 0.0f;
  const uint32_t v = Be32(p);
  if (t.kind == Kind::kInt) return float(int32_t(v));
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
// As the game does when it changes one (sub_823CE2F8): its flag, and the changes' flag.
void Write(uint8_t* base, const Tunable& t, float value) {
  uint8_t* const p = base + t.address;
  if (t.kind == Kind::kBool) {
    p[0] = value != 0 ? 1 : 0;
  } else if (t.kind == Kind::kInt) {
    PutBe32(p, uint32_t(int32_t(value)));
  } else {
    uint32_t v;
    std::memcpy(&v, &value, 4);
    PutBe32(p, v);
  }
  PutBe32(p - 8, Be32(p - 8) | 4);
  base[0x82C91B9Du] = 1;
}
// Its name's pointer names it: the table is of this image.
bool Check(const uint8_t* base, Tunable& t) {
  if (t.checked) return t.found;
  t.checked = true;
  const uint32_t name = Be32(base + t.address - 16);
  t.found = name >= 0x82000000u && name < 0x83000000u &&
            std::strcmp(reinterpret_cast<const char*>(base + name), t.name) == 0;
  if (!t.found) REXLOG_WARN("PGR4 tunables: {} not at {:08X}", t.name, t.address);
  return t.found;
}
void Force(const uint8_t* base, Tunable& t, float value) {
  if (!Check(base, t)) return;
  if (!t.saved) {
    t.original = Read(base, t);
    t.saved = true;
  }
  t.forced = true;
  t.value = value;
  REXLOG_INFO("PGR4 tunables: {} forced to {} (the game's: {})", t.name, value, t.original);
}
void Release(uint8_t* base, Tunable& t) {
  if (!t.forced) return;
  t.forced = false;
  if (t.saved) Write(base, t, t.original);
  REXLOG_INFO("PGR4 tunables: {} back to {}", t.name, t.original);
}
void ReleaseAll(uint8_t* base) {
  for (Tunable& t : g_tunables) Release(base, t);
  g_pgr4_force_world_max_lod.store(-1, std::memory_order_relaxed);
  REXCVAR_SET(pgr4_draw_distance, 1.0);
  REXCVAR_SET(pgr4_draw_distance_mask, 1);
  REXCVAR_SET(pgr4_pvs_ahead, 0.0);
  REXCVAR_SET(pgr4_pvs_refresh, false);
  REXLOG_INFO("PGR4 tunables: all back to the game's");
}

// The Graphics page's ENHANCED DETAILS, applied when it changes.
void DetailsApply(uint8_t* base) {
  static int32_t applied = -1;
  // The game as it was (the overlay's box, a release's F2): the game's details too.
  const int32_t details =
      REXCVAR_GET(pgr4_native_original_look) ? 0 : std::clamp(REXCVAR_GET(pgr4_native_details), 0, 2);
  if (details == applied) return;
  applied = details;
  Tunable* limit = nullptr;
  // The scenery's instances (World.Instances.GeneralInstanceCullScale, the game's 1) and the crowds' detail
  // (Crowds.Render.LODScalingFactor, the game's 1.2, times it) follow the draw distance (Tazz, 14/11/2026).
  Tunable* instances = nullptr;
  Tunable* crowds = nullptr;
  // Local change (23/11/2026, Tazz, chosen in the overlay: « pour le mode ultra … que maintenant ces
  // valeurs pour cette partie soient comme ça »): in Ultra, World.Streaming.LodOffset and ForceLod at
  // 10. The game copies them to its world (+0x724, +0x720) and makes the streaming's choice again
  // when they change (sub_823D6F68, sub_823D7258 -> sub_823D89A8). High and the game's: released.
  Tunable* lod_offset = nullptr;
  Tunable* force_lod = nullptr;
  for (Tunable& t : g_tunables) {
    if (t.address == 0x82AA2628) limit = &t;  // World.Streaming.ForceMemoryLimitInMb
    if (t.address == 0x82A99F84) lod_offset = &t;
    if (t.address == 0x82A99F2C) force_lod = &t;
    if (t.address == 0x82A9856C) instances = &t;
    if (t.address == 0x82AA3B4C) crowds = &t;
  }
  if (details == 0) {
    g_pgr4_force_world_max_lod.store(-1, std::memory_order_relaxed);
    REXCVAR_SET(pgr4_draw_distance, 1.0);
    REXCVAR_SET(pgr4_draw_distance_mask, 1);
    if (limit) Release(base, *limit);
    if (instances) Release(base, *instances);
    if (crowds) Release(base, *crowds);
    if (lod_offset) Release(base, *lod_offset);
    if (force_lod) Release(base, *force_lod);
  } else {
    const float distance = details == 1 ? 1.5f : 2.0f;
    g_pgr4_force_world_max_lod.store(details == 1 ? 5 : 10, std::memory_order_relaxed);
    REXCVAR_SET(pgr4_draw_distance, double(distance));
    REXCVAR_SET(pgr4_draw_distance_mask, 127);
    if (limit) Force(base, *limit, 1024.0f);
    if (instances) Force(base, *instances, distance);
    // The crowds' factor is the game's (1.2) times the distance's: 1.8 high, 2.4 ultra.
    if (crowds && Check(base, *crowds)) {
      if (!crowds->saved) {
        crowds->original = Read(base, *crowds);
        crowds->saved = true;
      }
      Force(base, *crowds, crowds->original * distance);
    }
    if (details == 2) {
      if (lod_offset) Force(base, *lod_offset, 10.0f);
      if (force_lod) Force(base, *force_lod, 10.0f);
    } else {
      if (lod_offset) Release(base, *lod_offset);
      if (force_lod) Release(base, *force_lod);
    }
  }
  REXLOG_INFO("PGR4 details: {}", details == 0 ? "original" : details == 1 ? "high" : "ultra");
}

}  // namespace

bool TunablesEnabled() { return !REXCVAR_GET(pgr4_native_capture_dir).empty(); }

void TunablesApply() {
  uint8_t* const base = Base();
  if (!base) return;
  DetailsApply(base);
  static std::string applied;
  const std::string text = TunablesEnabled() ? REXCVAR_GET(pgr4_native_tunable_set) : std::string();
  if (text != applied) {
    applied = text;
    for (size_t at = 0; at < text.size();) {
      size_t end = text.find(';', at);
      if (end == std::string::npos) end = text.size();
      const std::string item = text.substr(at, end - at);
      at = end + 1;
      if (item == "reset") {
        ReleaseAll(base);
        continue;
      }
      const size_t eq = item.find('=');
      if (eq == std::string::npos) continue;
      const std::string name = item.substr(0, eq);
      bool known = false;
      for (Tunable& t : g_tunables)
        if (name == t.name) {
          known = true;
          Force(base, t, float(std::atof(item.c_str() + eq + 1)));
        }
      if (!known) REXLOG_WARN("PGR4 tunables: {} not in the table", name);
    }
  }
  for (Tunable& t : g_tunables)
    if (t.forced && Read(base, t) != t.value) Write(base, t, t.value);
}

void TunablesTab() {
  uint8_t* const base = Base();
  if (!base) return;
  ImGui::TextWrapped(
      "Reglages internes du jeu, en direct, non enregistres. Certains ne sont peut-etre lus qu'au "
      "chargement : recharge la course si rien ne change.");
  if (ImGui::Button("Tout remettre comme le jeu")) ReleaseAll(base);
  ImGui::Separator();
  ImGui::Text("LOD maximal du decor (monde +0x728) : %d",
              g_pgr4_world_max_lod.load(std::memory_order_relaxed));
  int force_lod = g_pgr4_force_world_max_lod.load(std::memory_order_relaxed);
  if (ImGui::SliderInt("Forcer ce LOD (-1 : non)", &force_lod, -1, 10))
    g_pgr4_force_world_max_lod.store(force_lod, std::memory_order_relaxed);
  float draw_distance = float(REXCVAR_GET(pgr4_draw_distance));
  if (ImGui::SliderFloat("Distance du decor, vue principale (1 : jeu)", &draw_distance, 1.0f, 8.0f,
                         "%.1f"))
    REXCVAR_SET(pgr4_draw_distance, double(draw_distance));
  int mask = REXCVAR_GET(pgr4_draw_distance_mask);
  bool mask_changed = false;
  for (int i = 0; i < 7; ++i) {
    bool on = (mask >> i) & 1;
    if (i) ImGui::SameLine();
    char label[8];
    std::snprintf(label, sizeof(label), "D%d", i);
    if (ImGui::Checkbox(label, &on)) {
      mask = on ? (mask | (1 << i)) : (mask & ~(1 << i));
      mask_changed = true;
    }
  }
  if (mask_changed) REXCVAR_SET(pgr4_draw_distance_mask, mask);
  float ahead = float(REXCVAR_GET(pgr4_pvs_ahead));
  if (ImGui::SliderFloat("Visibilite en avance (m ; 0 : jeu)", &ahead, 0.0f, 400.0f, "%.0f"))
    REXCVAR_SET(pgr4_pvs_ahead, double(ahead));
  bool refresh = REXCVAR_GET(pgr4_pvs_refresh);
  if (ImGui::Checkbox("Recalculer la visibilite a chaque image", &refresh))
    REXCVAR_SET(pgr4_pvs_refresh, refresh);
  const char* group = nullptr;
  bool open = false;
  for (Tunable& t : g_tunables) {
    if (group != t.group) {
      group = t.group;
      open = ImGui::CollapsingHeader(group, ImGuiTreeNodeFlags_DefaultOpen);
    }
    if (!open) continue;
    if (!Check(base, t)) {
      ImGui::TextDisabled("%s : introuvable", t.name);
      continue;
    }
    ImGui::PushID(t.name);
    bool forced = t.forced;
    const float current = forced ? t.value : Read(base, t);
    if (ImGui::Checkbox("##forced", &forced)) {
      if (forced) Force(base, t, current);
      else Release(base, t);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Coche : force la valeur");
    ImGui::SameLine();
    float value = current;
    bool changed = false;
    const char* const dot = std::strrchr(t.name, '.');
    const char* const label = dot ? dot + 1 : t.name;
    ImGui::BeginDisabled(!t.forced);
    if (t.kind == Kind::kBool) {
      bool on = value != 0;
      changed = ImGui::Checkbox(label, &on);
      value = on ? 1.0f : 0.0f;
    } else if (t.kind == Kind::kInt) {
      int v = int(value);
      changed = ImGui::SliderInt(label, &v, int(t.min), int(t.max));
      value = float(v);
    } else {
      changed = ImGui::SliderFloat(label, &value, t.min, t.max, "%.2f");
    }
    ImGui::EndDisabled();
    if (changed) t.value = value;
    if (ImGui::IsItemDeactivatedAfterEdit())
      REXLOG_INFO("PGR4 tunables: {} = {}", t.name, t.value);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip("%s\n%s", t.name, t.note);
    ImGui::PopID();
  }
}

}  // namespace pgr4::native
