cmake_minimum_required(VERSION 3.24)
project(PopaVisual VERSION 1.0.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "PopaVisual is a Windows-only application.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Build PopaVisual as x64: the Magnification API is not available to WOW64 processes.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)

# ---- Dear ImGui (no CMake project upstream -> we build the sources ourselves) ----
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.5
    GIT_SHALLOW    TRUE
)

# ---- nlohmann/json ----
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install    OFF CACHE INTERNAL "")
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(imgui nlohmann_json)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_compile_definitions(imgui PUBLIC UNICODE _UNICODE NOMINMAX)

# ---- PopaVisual ----
add_executable(PopaVisual WIN32
    src/main.cpp
    src/ColorMatrix.h
    src/ColorMatrix.cpp
    src/DriverColor.h
    src/DriverColor.cpp
    src/Config.h
    src/Config.cpp
    src/DxOverlay.h
    src/DxOverlay.cpp
    src/UIManager.h
    src/UIManager.cpp
    src/app.manifest
)

target_compile_definitions(PopaVisual PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINVER=0x0A00
)

if(MSVC)
    target_compile_options(PopaVisual PRIVATE /W4 /permissive- /utf-8 /MP)
endif()

target_link_libraries(PopaVisual PRIVATE
    imgui
    nlohmann_json::nlohmann_json
    d3d11
    dxgi
    dcomp
    d3dcompiler
    Magnification
    shell32
    ole32
    windowscodecs
    uuid
    user32
    gdi32
)

# Shaders are compiled at runtime (d3dcompiler_47.dll ships with Windows 10/11),
# so the .hlsl file is copied next to the executable.
add_custom_command(TARGET PopaVisual POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders/Shaders.hlsl
            $<TARGET_FILE_DIR:PopaVisual>/Shaders.hlsl
)
