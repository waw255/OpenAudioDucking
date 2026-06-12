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

**Static linking is the default** (`target_link_options ... -static` in CMakeLists.txt:50). No extra DLLs needed at runtime.

## Release

```powershell
cmake --build build -j4 --config Release
Copy-Item -LiteralPath "build\OpenAudioDucking.exe" -Destination "release\"
```

Single static exe, no dependencies beyond standard Windows DLLs.

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
  Handles `TaskbarCreated` (Explorer restart) — resets `g_TrayAdded` to force re-add with `NIM_ADD`.

## ImGui notes

- ImGui version pinned via `GIT_TAG` in `CMakeLists.txt:12`. Update it there to bump versions.
- To force a clean ImGui re-fetch, delete `build/_deps/imgui-src` and `build/_deps/imgui-build`, then re-run cmake.

## Testing

No real tests. `tests/CMakeLists.txt` is a placeholder. Do not run `ctest`.

## Git workflow

- Commit: `git add -A; git commit -m "..."` — stage everything, single commit.
- Revert: `git reset --soft HEAD~n` — undo commits, keep changes in working tree.
- Tag: `git tag -a vX.Y.Z -m "vX.Y.Z: <summary>"` — annotated tags.
