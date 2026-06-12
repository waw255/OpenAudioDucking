# OpenAudioDucking — AGENTS.md

Windows audio ducking tool: user configures primary apps (keep volume) and secondary apps (duck volume).
When any primary app plays audio, secondary app volumes are smoothly lowered. Unconfigured apps are unaffected.

## Build

```powershell
# MinGW (recommended for dev)
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4

# MSVC
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

First CMake configure fetches Dear ImGui from GitHub (FetchContent). Subsequent builds are incremental.

**Runtime DLLs (MinGW builds):** `libgcc_s_seh-1.dll`, `libstdc++-6.dll`, `libwinpthread-1.dll` must be in
the same directory as `OpenAudioDucking.exe` or on PATH.

## Architecture

- `src/main.cpp` — Win32 window + D3D11 renderer + ImGui GUI. Single-file, ~420 lines.
- `src/AudioDucker.cpp` — Core Audio COM engine. Session enumeration, volume ducking, phase state machine.
- `include/AudioDucker.h` — `AudioDucker`, `DuckConfig`, `AppConfig`, `AppStatus`, `VolState` structs.
- `resource/` — Icons (`app.ico`, `app_green.ico`, `app_red.ico`) + `resource.rc` + `resource.h`.

## Key conventions

- **UTF-8 with BOM** for all source files (Windows compatibility).
- New `.cpp` → `src/`, new `.h` → `include/`.
- Globals use `g_` prefix (e.g. `g_Run`, `g_Ducker`).
- Compact style: single-line functions, short names, no comments.
- `<initguid.h>` must be included **before** `<AudioDucker.h>` in `AudioDucker.cpp` — this defines
  `IID_IAudioMeterInformation` needed to query peak meters on MinGW.

## Runtime behavior

- **Config stored in `%APPDATA%\OpenAudioDucking\`** — `settings.json` (last config path),
  `config.json` (default config). Exported/imported configs go wherever the user picks.
- **Single-instance:** mutex named `OpenAudioDucking_SingleInstance`. Second launch restores the existing window.
- **Font loading:** tries `msyh.ttc` then `simsun.ttc` from Windows Fonts directory for CJK glyph support.
  Systems without these fonts (e.g. English-only) fail silently — ImGui falls back to the default font.
- **Tray icon:** minimize/close hides to tray. Green icon = running, red = stopped. Right-click menu.

## ImGui notes

- ImGui version pinned via `GIT_TAG` in `CMakeLists.txt:12`. Update it there to bump versions.
- To force a clean ImGui re-fetch, delete `build/_deps/imgui-src` and `build/_deps/imgui-build`, then re-run cmake.

## Testing

No real tests. `tests/CMakeLists.txt` is a placeholder. Do not run `ctest`.
