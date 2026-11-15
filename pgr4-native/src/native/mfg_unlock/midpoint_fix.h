#pragma once

#include <Windows.h>

#include <cstdint>

#ifndef MFG_UNLOCK_OUTPUT_PULL_MASK_ONLY
#define MFG_UNLOCK_OUTPUT_PULL_MASK_ONLY 0
#endif
#ifndef MFG_UNLOCK_OUTPUT_PULL_MASK_OCCUPANCY
#define MFG_UNLOCK_OUTPUT_PULL_MASK_OCCUPANCY 0
#endif

namespace midpoint_fix
{
using LogCallback = void (*)(const wchar_t* message);

void SetLogCallback(LogCallback callback) noexcept;
bool ObserveD3D12Device(void* device) noexcept;
bool PatchProvider(HMODULE module, const wchar_t* path) noexcept;
uint64_t AdapterLuid() noexcept;
bool AdapterVerified() noexcept;
bool Ready() noexcept;
uint32_t FailureCode() noexcept;

}
