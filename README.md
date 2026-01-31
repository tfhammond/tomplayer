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
build\vs2022-debug\Debug\player.exe --repeat 3 --seconds 2 --frequency 440
```

The current demo feeds a sine tone into an `AudioRingBuffer` and cycles start/stop for validation.

Use `--stress` to run a CPU load during playback.

## WASAPI notes

- Event-driven shared-mode WASAPI with a dedicated render thread.
- `tomplayer::wasapi::WasapiOutput` consumes interleaved float32 frames from `AudioRingBuffer`.
- `init_default_device()` fails if the device mix format is unsupported.
- `tomplayer::wasapi::WasapiOutput` expects `CoInitializeEx(COINIT_MULTITHREADED)` on the calling thread.
- Linker inputs (already wired in CMake): `ole32`, `mmdevapi`, `audioclient`, `avrt`.

## Tests

- `tests/wasapi_output_tests.cpp` covers mix format detection, float->PCM16 conversion, and ring-buffer consumption without real audio devices.
- `tests/ring_buffer_tests.cpp` exercises the SPSC `AudioRingBuffer` including wrap-around and stress behavior.
- Run: `ctest --test-dir build\vs2022-debug -C Debug` (or `build\vs2022-release`).

## Scope (v1)

- Windows-only, C++20
- WASAPI shared-mode output
- FLAC and WAV playback
- CLI-first, no GUI
