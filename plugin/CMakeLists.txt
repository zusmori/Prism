cmake_minimum_required(VERSION 3.20)
project(ZLauncherItems LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

# externe Teile (werden im Build-Skript bereitgestellt)
set(EXT ${CMAKE_SOURCE_DIR}/external)
set(BMSDK ${EXT}/BakkesModSDK)
set(IMGUI ${EXT}/imgui)

add_library(ZLauncherItems SHARED
    src/ZLauncherItems.cpp
    src/Window.cpp
    ${IMGUI}/imgui.cpp
    ${IMGUI}/imgui_draw.cpp
    ${IMGUI}/imgui_widgets.cpp
)
target_compile_options(ZLauncherItems PRIVATE /EHsc /O2 /utf-8 /bigobj /W3)
target_compile_definitions(ZLauncherItems PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_include_directories(ZLauncherItems PRIVATE src ${BMSDK}/include ${IMGUI} ${EXT}/json)
target_link_libraries(ZLauncherItems PRIVATE ${BMSDK}/lib/pluginsdk.lib)
