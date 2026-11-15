// pgr4-native PGR4 native renderer (local change, pgr4 project), development: the game's own
// settings (its engine's named variables) read and forced live, for the draw distance tests.
// Only with a capture directory (the development toml): an export has none of it.
#pragma once

#include <atomic>
#include <cstdint>

namespace pgr4::native {

// Render thread, once per Swap: what is forced written again where the game changed it, and
// --pgr4_native_tunable_set ("name=value;name=value", "reset": none) taken.
void TunablesApply();
// The overlay's tab.
void TunablesTab();
bool TunablesEnabled();

}  // namespace pgr4::native

// src/pgr4_hooks.cpp: the world's highest scenery LOD (+0x728) as last seen, and the one forced
// (-1: none).
extern std::atomic<int32_t> g_pgr4_world_max_lod;
extern std::atomic<int32_t> g_pgr4_force_world_max_lod;
