# CrossBaseCrafting C++ (live sources: cpp/ + headers in old/cpp/inc/).
# Official CMake route (see old/cpp/README.md):
#   cmake -S . -B build-v204 -G Ninja -DRE_UE4SS_DIR="C:/Dev/tools/compile/RE-UE4SS" -DCMAKE_BUILD_TYPE=Game__Shipping__Win64
#   cmake --build build-v204 --target CrossBaseCrafting
#   Copy-Item build-v204\Game__Shipping__Win64\bin\CrossBaseCrafting.dll dlls\main.dll -Force
# Run from an x64 Native Tools prompt (or plain PowerShell, MSVC auto-detected via vcvarsall).

cmake_minimum_required(VERSION 3.22)

project(CrossBaseCrafting LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(RE_UE4SS_DIR "" CACHE PATH "Path to a checked-out RE-UE4SS repository")
if(NOT RE_UE4SS_DIR OR NOT EXISTS "${RE_UE4SS_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Set RE_UE4SS_DIR to the RE-UE4SS source directory")
endif()

add_subdirectory("${RE_UE4SS_DIR}" RE-UE4SS)

add_library(CrossBaseCrafting SHARED
    cpp/dllmain.cpp
    cpp/common/function_hook_registry.cpp
    cpp/game/pal_base_camp_reflection.cpp
    cpp/base_resource_sharing/pal_base_resource_runtime.cpp
    cpp/base_resource_sharing/pal_base_resources.cpp
    cpp/base_resource_sharing/material_detour.cpp
    cpp/base_resource_sharing/hooks.cpp
    cpp/base_resource_sharing/persistent.cpp
    cpp/base_resource_sharing/live_union.cpp
    cpp/base_resource_sharing/lifecycle.cpp
)
target_include_directories(CrossBaseCrafting PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/cpp/inc")
target_link_libraries(CrossBaseCrafting PRIVATE UE4SS)
set_target_properties(CrossBaseCrafting PROPERTIES OUTPUT_NAME CrossBaseCrafting)
