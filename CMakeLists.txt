cmake_minimum_required(VERSION 3.20)
project(OpenAudioDucking VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# ── Dear ImGui ──────────────────────────────────────────
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.8
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)

# ── 应用目标 ─────────────────────────────────────────────
add_executable(OpenAudioDucking
    src/main.cpp
    src/AudioDucker.cpp
    resource/resource.rc
    # ImGui core
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    # ImGui backends
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)

target_include_directories(OpenAudioDucking PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/resource
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

target_link_libraries(OpenAudioDucking PRIVATE
    d3d11
    dxgi
    d3dcompiler
    dwmapi
    ole32
    uuid
)

# ImGui 需要 Win32 窗口，设置链接器子系统为 Windows
set_target_properties(OpenAudioDucking PROPERTIES
    WIN32_EXECUTABLE TRUE
)

# ── 测试 ──
enable_testing()
add_subdirectory(tests)
