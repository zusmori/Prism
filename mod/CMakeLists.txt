cmake_minimum_required(VERSION 3.20)
project(zlmod LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")   # statisch: keine Laufzeit-DLLs noetig

set(EXT ${CMAKE_SOURCE_DIR}/external)
set(IMGUI ${EXT}/imgui)
set(MINHOOK ${EXT}/minhook)

add_library(zlmod SHARED
    src/dllmain.cpp
    src/ue.cpp
    src/items.cpp
    src/hooks.cpp
    src/gui.cpp
    src/maps.cpp
    src/tweaks.cpp
    src/camera.cpp
    ${IMGUI}/imgui.cpp
    ${IMGUI}/imgui_draw.cpp
    ${IMGUI}/imgui_tables.cpp
    ${IMGUI}/imgui_widgets.cpp
    ${IMGUI}/backends/imgui_impl_dx11.cpp
    ${IMGUI}/backends/imgui_impl_win32.cpp
    ${MINHOOK}/src/buffer.c
    ${MINHOOK}/src/hook.c
    ${MINHOOK}/src/trampoline.c
    ${MINHOOK}/src/hde/hde64.c
)
target_include_directories(zlmod PRIVATE src ${IMGUI} ${IMGUI}/backends ${MINHOOK}/include)
target_compile_definitions(zlmod PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
target_compile_options(zlmod PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/EHa /O2 /utf-8 /W3>)
target_link_libraries(zlmod PRIVATE d3d11 dxgi psapi windowscodecs ole32 advapi32)
