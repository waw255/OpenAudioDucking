# OpenAudioDucking — AGENTS.md

Windows 音频闪避工具：用户配置主应用（音量不变）和次要应用（音量降低），
多应用同时播放音频时自动闪避次要应用音量。未配置的应用不受影响。

## 技术栈

- 语言：C++17
- GUI 库：Dear ImGui（FetchContent 自动拉取）
- 渲染：DirectX 11（Windows 自带）
- 构建：CMake 3.20+
- Windows API：Core Audio（`IAudioSessionManager2`、`ISimpleAudioVolume`、`IAudioMeterInformation`）
- 代码风格：默认 IDE 设置，PascalCase 类/函数，kCamelCase 常量

## 构建

```powershell
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build -j4
.\build\OpenAudioDucking.exe
# 有 MSVC 的话：
# cmake -S . -B build -G "Visual Studio 17 2022" -A x64
# cmake --build build --config Release && .\build\Release\OpenAudioDucking.exe
```

首次 CMake 配置会自动从 GitHub 拉取 Dear ImGui（FetchContent），之后增量构建。

## 目录结构

```
OpenAudioDucking/
├── CMakeLists.txt        # 根 CMake，FetchContent 引入 ImGui
├── AGENTS.md
├── src/                  # 源文件
│   ├── main.cpp          # 入口 + GUI（Win32 + D3D11 + ImGui）
│   └── AudioDucker.cpp   # 核心逻辑
├── include/              # 公开头文件
│   └── AudioDucker.h     # AudioDucker + DuckConfig + AppConfig
├── tests/                # 测试（占位）
│   └── CMakeLists.txt
└── build/                # 构建产物（已 gitignore）
    └── _deps/imgui-src/  # ImGui 源码（FetchContent 下载）
```

## 关键 Windows API

- `IMMDeviceEnumerator` + `IMMDevice` — 枚举音频端点
- `IAudioSessionManager2` — 获取音频会话管理器
- `IAudioSessionControl` / `IAudioSessionControl2` — 会话控制
- `ISimpleAudioVolume` — 读取/设置会话音量 (0.0–1.0)
- `IAudioMeterInformation` — 获取会话峰值音量
- `IAudioEndpointVolume` — 控制主音量

## 约定

- 可执行文件：`OpenAudioDucking.exe`
- 源文件统一用 UTF-8 with BOM（Windows 兼容）
- 新增源文件放到 `src/`，头文件放到 `include/`
- CMake 链接库用 `target_link_libraries`

## 开发提示

- ImGui 版本由 CMakeLists.txt 中的 `GIT_TAG` 控制，更新改它即可
- 清理 ImGui 缓存：删除 `build/_deps/imgui-src` 和 `build/_deps/imgui-build` 后重新 cmake
- Core Audio 需要链接 `ole32.lib`（已通过 CMake 隐式链接）
- DirectX 11 渲染后端是 Windows 自带的，无需额外安装
- 调试音频会话可用 Windows 的 `AudioGraph` 工具或自己打印峰值
