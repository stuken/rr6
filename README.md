# rr6

rr6 rebuilds Ridge Racer 6 (Xbox 360) as a native PC program using static recompilation. The game's PowerPC code is translated to C++ with the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) and compiled for the host, so the game runs directly instead of inside an emulated console.

> [!IMPORTANT]
> This repository contains no game assets. You need the files from your own retail copy of Ridge Racer 6, and both building and running need them.

## Renderers

rr6 has two renderers. You pick one when you configure the build.

- **xenos plugin** (`RR6_NATIVE_GPU=OFF`, the default). This is the SDK's Xenia-derived GPU emulation. It is the reference that the native renderer is checked against.
- **Native renderer** (`RR6_NATIVE_GPU=ON`). This replaces the game's Direct3D library with a renderer built on [plume](https://github.com/renderbag/plume). The game's Xenos shaders are recompiled at build time with [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) and embedded in the executable. On Windows this builds two executables from one tree:
  - `rr6.exe`: Direct3D 12.
  - `rr6_vk.exe`: Vulkan.

`docs/xenosrecomp-integration-plan.md` tracks how the native renderer was designed and where it stands.

## Platform status

| Platform | State |
|---|---|
| Windows x64 | Builds and runs, with both renderers and both native backends. |
| Linux x64 | CMake support is written but has not been built yet. It uses the Vulkan backend. |
| macOS | Not ported. |

## Building from scratch (Windows)

The build has three stages:

1. Build and install the ReXGlue SDK.
2. Clone rr6 and add your game files.
3. Configure, run codegen, and build.

The first build takes a while. Codegen turns the whole executable into C++, and there is a lot of it to compile.

### 1. Prerequisites

- **Visual Studio 2022** (Community is fine) with these components:
  - Desktop development with C++.
  - C++ Clang Compiler for Windows. rr6 and the SDK build with clang, not MSVC.
  - C++ CMake tools for Windows. This provides CMake and Ninja; standalone CMake 3.25+ and Ninja also work.
  - A Windows SDK. 10.0.26100 or later is recommended: with older headers the D3D12 backend bakes depth bias into pipelines instead of setting it per draw. That path works, but it compiles more pipelines.
- **Git**.
- **Python 3**, with the `cryptography` package. The build uses it to decrypt `default.xex` and extract the shaders.

  ```bat
  python -m pip install cryptography
  ```

No Vulkan SDK or DirectX Shader Compiler install is needed. XenosRecomp vendors DXC for Windows and Linux.

Run every command below from an **x64 Native Tools Command Prompt for VS 2022**, with VS's clang on `PATH`:

```bat
set "PATH=%VCINSTALLDIR%Tools\Llvm\x64\bin;%PATH%"
clang --version
```

### 2. Build and install the ReXGlue SDK

rr6 needs SDK version 0.10.0. It is developed against the `port/xenia-sep-2026-zpd` branch of [stuken/rexglue-sdk](https://github.com/stuken/rexglue-sdk), which adds GPU, kernel and XAM fixes on top of upstream.

```bat
git clone --recursive -b port/xenia-sep-2026-zpd https://github.com/stuken/rexglue-sdk.git
cd rexglue-sdk
cmake --preset win-amd64
cmake --build out/build/win-amd64 --config RelWithDebInfo --target install
```

This installs the SDK to `rexglue-sdk\out\install\win-amd64`, which step 4 points rr6 at. The SDK uses a multi-config generator. Install each configuration you plan to build rr6 in: run the second command again with `--config Release` or `--config Debug`.

### 3. Clone rr6 and add the game files

```bat
git clone --recursive <rr6 repository URL> rr6
cd rr6
```

`--recursive` fetches `thirdparty/plume` and `thirdparty/XenosRecomp`. If you cloned without it, run:

```bat
git submodule update --init --recursive
```

Extract the contents of your Ridge Racer 6 disc into `assets/`, using a tool such as extract-xiso, so that you end up with `assets/default.xex`. Next to it should be `RRM.DAT`, `RRM2.DAT`, `RRM3.DAT`, the `pack_*.bin` files and the `.sfd` movies. `assets/` is gitignored.

The build needs only `default.xex`, which feeds codegen, the shader cache and the window icon. Running the game needs everything. The function addresses in `rr6_config.toml` and `config/` are specific to one executable:

| | |
|---|---|
| Title ID | `4E4D07D3` |
| `default.xex` MD5 | `e09586f12a663d4ea7ba864c1510d764` |
| Regions | NTSC/U and PAL |

Check your copy against these. A different `default.xex` will not recompile correctly against these configs.

### 4. Configure, run codegen, and build

Set `CMAKE_PREFIX_PATH` to the SDK install from step 2. Add `-DRR6_NATIVE_GPU=ON` for the native renderer, or leave it off for the xenos plugin.

```bat
set REXSDK=C:\path\to\rexglue-sdk\out\install\win-amd64

cmake --preset win-amd64-relwithdebinfo -DCMAKE_PREFIX_PATH=%REXSDK% -DRR6_NATIVE_GPU=ON
cmake --build --preset win-amd64-relwithdebinfo --target rr6_codegen
cmake --preset win-amd64-relwithdebinfo
cmake --build --preset win-amd64-relwithdebinfo
```

The commands do the following:

1. **Configure.**
2. **Codegen.** The SDK's `rexglue codegen` recompiles `default.xex` into `generated/default/`, following `rr6_manifest.toml`.
3. **Reconfigure.** CMake only picks up the generated sources on a configure that runs after they exist. Without this step the first build fails to link. Later builds re-run codegen by themselves when its inputs change, so you need the reconfigure only once per fresh tree.
4. **Build.** This builds everything, including the shader cache:
   - `scripts/extract_shaders.py` pulls the 176 Xenos shaders out of `default.xex`.
   - XenosRecomp turns them into DXIL and SPIR-V.
   - The result is compiled into the executable.

Debug and Release presets work the same way (`win-amd64-debug`, `win-amd64-release`). The output goes to `out\build\<preset>\`, along with the DLLs it needs from the SDK and DXC:

- With `RR6_NATIVE_GPU=ON`: `rr6.exe` (D3D12) and `rr6_vk.exe` (Vulkan).
- With it off: `rr6.exe`, using the xenos plugin.

These options apply to the native build:

| Option | Default | Effect |
|---|---|---|
| `RR6_NATIVE_GPU` | `OFF` | Native renderer instead of the xenos plugin. |
| `RR6_D3D12` | `ON` on Windows | `rr6` uses D3D12. With `OFF`, `rr6` itself uses Vulkan. |
| `RR6_VULKAN_EXE` | `ON` | Also build `rr6_vk` (Windows). |

## Running

`rr6.exe` looks for the game in `assets\default.xex` next to itself. Either copy or link your `assets` folder into the build directory, or point the game at it:

```bat
out\build\win-amd64-relwithdebinfo\rr6.exe --game_data_root="C:\path\to\rr6\assets"
```

Next to the executable, rr6 uses:

- `rr6.toml`: the config file. Any cvar can go there, and the same cvar can be given on the command line as `--name=value`.
- `user\`: saves and the frame dumps.
- `cache\`: the shader and pipeline caches.

### Native renderer settings

| Cvar | Default | |
|---|---|---|
| `rr6_resolution_scale` | `0` | Integer scale over 1280x720. `0` picks it from the window height; `1` is native. |
| `rr6_supersampling` | `1` | `2` renders at twice the scale and downsamples. Takes effect after a restart. |
| `rr6_anisotropy` | `16` | Anisotropic filtering level, `0` to `16`. |
| `rr6_fps_limit` | `60` | Presentation cap. `0` means no cap. |
| `rr6_vsync` | `true` | Vertical sync. |
| `rr6_pso_precache` | `true` | Compile the recorded pipelines in the background at boot. |
| `rr6_output_16bit` | `true` | Keep the game's back buffer at 16 bits per channel and dither to 8 bits at present, which smooths gradients at high resolution scales. `false` uses the console's 8 bits. Takes effect after a restart. |
| `rr6_scaled_post_filters` | `true` | At a resolution scale above 1, fit the race anti-aliasing resolve to the host pixel and upscale the glow smoothly. `false` keeps the guest's filters as they are. |

### Testing and diagnostics

| Cvar | |
|---|---|
| `rr6_null_audio` | Discard audio output (for machines without an audio device). |
| `rr6_input_script` | Scripted pad input, `button@frame[+hold],...`. |
| `rr6_dump_frames` | Comma-separated present frame numbers to save as `user\rr6_frame_N.bmp`. This works in both renderers. |
| `rr6_trace_frame` | Log every draw and resolve of present frame N. |
| `rr6_perf_stats` | Log frame and renderer timings every N presents. |
| `rr6_pso_capture` | Write the precache list, plus every pipeline a draw still had to compile, to `cache\pipeline_state_cache.h`. Copy it over `src\gpu\pipeline\cache\pipeline_state_cache.h` to extend the precache. |
| `rr6_devmode` | Extra renderer diagnostics, including a log line for each pipeline compiled on the drawing thread. |
| `rr6_d3d12_debug` | D3D12 debug layer. |
| `rr6_baked_depth_bias` | Force the older-Windows path that bakes depth bias into pipelines. |

## Linux (not yet built)

The CMake support for Linux is written but has not been built yet. These steps are the intended route and may need fixes along the way. They use Ubuntu 24.04 or later package names:

```sh
sudo apt install clang-20 lld-20 cmake ninja-build build-essential git git-lfs \
  python3-cryptography pkg-config libvulkan-dev libgtk-3-dev libx11-xcb-dev \
  libxss-dev libwayland-dev libwayland-bin wayland-protocols libxkbcommon-dev \
  libdecor-0-dev libasound2-dev libpulse-dev libpipewire-0.3-dev \
  libcurl4-openssl-dev autoconf zip unzip curl python3-venv

# SDK
git clone --recursive -b port/xenia-sep-2026-zpd https://github.com/stuken/rexglue-sdk.git
cd rexglue-sdk
cmake --preset linux-amd64
cmake --build out/build/linux-amd64 --config RelWithDebInfo --target install
cd ..

# rr6 (game files in rr6/assets as on Windows)
cd rr6
cmake --preset linux-amd64-relwithdebinfo \
  -DCMAKE_PREFIX_PATH=$HOME/rexglue-sdk/out/install/linux-amd64 -DRR6_NATIVE_GPU=ON
cmake --build --preset linux-amd64-relwithdebinfo --target rr6_codegen
cmake --preset linux-amd64-relwithdebinfo
cmake --build --preset linux-amd64-relwithdebinfo
```

On Linux the `rr6` executable is the Vulkan backend. Its window and surface come from the SDK's SDL3 window, so it runs on both X11 and Wayland.

## Repository layout

| Path | |
|---|---|
| `rr6_manifest.toml`, `rr6_config.toml`, `config/` | Codegen inputs: the entry point, function overrides, and the named D3D and XAPI functions that the native renderer hooks. |
| `generated/rexglue.cmake` | SDK glue. `generated/default/` holds the codegen output and is gitignored. |
| `src/rr6_app.h`, `src/main.cpp` | App entry point and path setup. |
| `src/gpu/` | The native renderer: D3D hooks, resource mirrors, pipelines, the frame ring, and presentation. |
| `src/gpu/shaders/hlsl/` | Host HLSL for the native renderer's own passes, compiled to DXIL and SPIR-V at build time. |
| `src/platform/` | Input scripting, silent audio, window and threading helpers. |
| `cmake/` | The native renderer and the shader build. |
| `scripts/` | Shader extraction, icon generation, and `re/funcdb.py` for querying the codegen output. |
| `tools/prelink_shader_cache.cpp` | `rr6_prelink`, which links the recompiled shaders ahead of time for D3D12. |
| `thirdparty/` | plume, XenosRecomp (on a `rr6` branch with the XDK 2.0.2135 container support), miniz, zstd. |

## Credits

- The [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) team, for the recompilation toolchain.
- [hedge-dev](https://github.com/hedge-dev), for [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) and [Unleashed Recompiled](https://github.com/hedge-dev/UnleashedRecomp).
- [plume](https://github.com/renderbag/plume), the rendering hardware interface.
- [re:Blue](https://github.com/zolaware/reblue), whose native renderer this one is ported from.
- The [Xenia](https://github.com/xenia-project/xenia) project and the wider Xbox 360 emulation scene.
