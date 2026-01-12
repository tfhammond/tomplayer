# tomplayer

Lossless-only C++20 music player for Windows (WASAPI shared mode).

## Prerequisites

- Visual Studio 2022 with MSVC
- CMake 3.23+
- vcpkg (manifest mode)

## vcpkg setup

1. Clone vcpkg and bootstrap it.
2. Set `VCPKG_ROOT` to your vcpkg folder.

Example (PowerShell):
```powershell
git clone https://github.com/microsoft/vcpkg.git C:\dev\vcpkg
cd C:\dev\vcpkg
.\bootstrap-vcpkg.bat
$env:VCPKG_ROOT="C:\dev\vcpkg"
```

## Configure and build

Using presets:
```powershell
cmake --preset vs2022-debug
cmake --build --preset build-debug
cmake --preset vs2022-release
cmake --build --preset build-release
```

Without presets:
```powershell
cmake -S . -B build `
  -G "Visual Studio 17 2022" `
  -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Debug
cmake --build build --config Release
```

## Run

```powershell
build\Debug\player.exe <path-to-audio-file>
```

## Scope (v1)

- Windows-only, C++20
- WASAPI shared-mode output
- FLAC and WAV playback
- CLI-first, no GUI
