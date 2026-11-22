# PROJECT GOTHAM RACING 4 — PC recompilation

A static recompilation of *Project Gotham Racing 4* (Xbox 360, Title Update 2) for Windows, built
on the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) and the D3D12 renderer of
[pgr4-native](https://github.com/shaunsingh/pgr4-native) (MIT, vendored in `pgr4-native/`, see
`pgr4-native/VERSION`), with its own launcher (`PGR4Launcher.exe`).

**No game files are included.** You need your own copy of the game: its `default.xex`, and the
Title Update 2 `default.xexp`. The code in `generated/` is produced on your PC from your
`default.xex`, so it is not in this repository.

## Requirements

- Windows 10 or 11, x64, a Direct3D 12 GPU
- Visual Studio 2022 (MSVC x64 libraries and headers, Windows SDK)
- LLVM (clang / clang++), CMake 3.25 or later, Ninja
- Git

## Folder layout

The build expects these folders side by side:

```
<root>\
  pgr4\                        this repository
  rexglue-sdk\                 ReXGlue SDK, commit bd833a2 + patches\rexglue-sdk.diff
  thirdparty\fidelityfx-sdk\   AMD FidelityFX SDK (FSR SDK 2.3.0)
  thirdparty\streamline\       NVIDIA Streamline SDK 2.14.16 (+ patch)
  thirdparty\XeGTAO\           Intel XeGTAO (XeGTAO.h, XeGTAO.hlsli, LICENSE)
```

## Build

Run every command from a *x64 Native Tools Command Prompt for VS 2022* with LLVM and Ninja on the
`PATH`.

1. **ReXGlue SDK**, patched and installed:

   ```
   git clone https://github.com/rexglue/rexglue-sdk.git
   cd rexglue-sdk
   git checkout bd833a2
   git submodule update --init --recursive
   git apply ..\pgr4\patches\rexglue-sdk.diff
   cd thirdparty\libmspack
   git apply ..\..\..\pgr4\patches\rexglue-sdk-libmspack.diff
   cd ..\..
   cmake --preset win-amd64
   cmake --build --preset win-amd64-release
   cmake --install out\build\win-amd64 --config Release
   cd ..
   ```

   The SDK is installed in `rexglue-sdk\out\install\win-amd64`.

2. **FidelityFX** (signed DLLs and headers): clone
   [GPUOpen-LibrariesAndSDKs/FidelityFX-SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK)
   (FSR SDK 2.3.0) into `thirdparty\fidelityfx-sdk`.

3. **Streamline** (DLSS, frame generation, Reflex): download the Streamline SDK 2.14.16 from
   [NVIDIA-RTX/Streamline](https://github.com/NVIDIA-RTX/Streamline/releases), unpack it into
   `thirdparty\streamline`, then make in `include\sl_pcl.h` the one-line change that
   `patches\streamline-sl_pcl-cxx23-to_underlying.diff` describes (`#if __cplusplus == 202302L`
   becomes `#if 0 && __cplusplus == 202302L`): the header does not compile in C++23 without it.

4. **XeGTAO** (ambient occlusion, built into `pgr4.exe`): from
   [GameTechDev/XeGTAO](https://github.com/GameTechDev/XeGTAO) (branch `master`), copy
   `Source\Rendering\Shaders\XeGTAO.h`, `Source\Rendering\Shaders\XeGTAO.hlsli` and `LICENSE` into
   `thirdparty\XeGTAO`.

5. **The game's executables**: copy the disc's `default.xex` and Title Update 2's
   `default.xexp` into `pgr4\assets\`.

6. **Code generation and build**:

   ```
   cd pgr4
   ..\rexglue-sdk\out\install\win-amd64\bin\rexglue.exe codegen pgr4_manifest.toml
   cmake --preset win-amd64-release -Drexglue_DIR=../rexglue-sdk/out/install/win-amd64/lib/cmake/rexglue
   cmake --build out\build\win-amd64-release
   ```

   `pgr4.exe`, `PGR4Launcher.exe` and their files are in `out\build\win-amd64-release`.

## Playing

Start **`PGR4Launcher.exe`**, not `pgr4.exe`: started on its own, `pgr4.exe` opens the launcher
and closes. The launcher finds (or lets you choose) the game's folder, the title update and the
DLC, writes its `pgr4.toml` from `pgr4.default.toml`, prepares the shaders, then starts the game.
Saves and caches go to `Documents\pgr4`.

A controller is required.

## Repository contents

| Folder | Contents |
|---|---|
| `src/` | the game's hooks, menus, profile and launcher |
| `pgr4-native/` | the D3D12 renderer (MIT, with local changes) |
| `config/` | codegen settings for Title Update 2 |
| `shaders/` | replacement pixel shaders (HLSL) |
| `textures/` | replacement textures |
| `patches/` | the changes to the ReXGlue SDK, libmspack and Streamline |
| `tools/` | development scripts (some have paths of the original setup in them) |
| `generated/rexglue.cmake` | the SDK's CMake boilerplate; the rest of `generated/` comes from codegen |

The optional HD texture pack (`textures_hd.pak`) is not part of this repository.

## Credits

- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk): the recompiler and runtime.
- [pgr4-native](https://github.com/shaunsingh/pgr4-native) (MIT): the D3D12 renderer this one
  started from.
- [RTX40MFG-Unlock](https://github.com/dashdogy/RTX40MFG-Unlock) by dashdogy (MIT): DLSS Multi
  Frame Generation on RTX 40 series GPUs (`pgr4-native/src/native/mfg_unlock*`).
- [XeGTAO](https://github.com/GameTechDev/XeGTAO) by Intel (MIT): the ambient occlusion (with DLSS,
  FSR and MSAA).
- [PGR4-ReGPU](https://github.com/zer0ducksgiven/PGR4-ReGPU) (MIT): the cars' projected shadow fix
  and the method of recording the driver's calls on their own thread.
