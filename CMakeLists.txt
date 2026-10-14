# rr6 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(rr6 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(CMakeDependentOption)
# OFF keeps the SDK's xenos GPU plugin (Xenia-style emulation). ON replaces the
# guest D3D library with the native plume renderer and the recompiled shader
# cache. Hooks bind at link time, so this is a build switch, not a cvar.
# Windows builds D3D12 and Vulkan exes, Linux the Vulkan one; macOS is not
# ported yet.
cmake_dependent_option(RR6_NATIVE_GPU
    "Native plume renderer with recompiled shaders (OFF = SDK xenos plugin)"
    OFF "WIN32 OR LINUX" OFF)

set(REXGLUE_RECOMP_DEBUG_INFO "full")
include(generated/rexglue.cmake)

# Embedded window icon: extracted directly from assets/default.xex (already a
# required codegen input) so no extra asset needs to be checked in or fetched.
# src/icon.generated.h is a gitignored build artifact regenerated whenever the
# XEX or the generator script changes.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(RR6_ICON_HEADER ${CMAKE_SOURCE_DIR}/src/icon.generated.h)
set(RR6_ICON_ICO ${CMAKE_SOURCE_DIR}/src/icon.generated.ico)
add_custom_command(
    OUTPUT ${RR6_ICON_HEADER} ${RR6_ICON_ICO}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/gen-icon.py
    DEPENDS
        ${CMAKE_SOURCE_DIR}/scripts/gen-icon.py
        ${CMAKE_SOURCE_DIR}/assets/default.xex
    COMMENT "Generating src/icon.generated.h and src/icon.generated.ico"
    VERBATIM
)

# On Windows, also embed the same icon as the .exe's own resource (Explorer,
# taskbar before the window opens, Alt-Tab), separate from the in-window icon
# applied at runtime via Window::SetIcon (see RR6_app.h). The .rc
# needs an absolute path to the .ico since it's not necessarily emitted next
# to it in an out-of-source build.
if(WIN32)
    set(RR6_ICON_ICO_PATH ${RR6_ICON_ICO})
    set(RR6_ICON_RC ${CMAKE_SOURCE_DIR}/src/RR6.generated.rc)
    configure_file(
        ${CMAKE_SOURCE_DIR}/src/RR6.rc.in
        ${RR6_ICON_RC}
        @ONLY
    )
    # rc.exe needs icon.generated.ico to exist by the time it compiles this
    # .rc; configure_file only stamped the path into it above, it doesn't
    # establish a build-order dependency on the custom command that (re)writes
    # the .ico itself.
    set_source_files_properties(${RR6_ICON_RC} PROPERTIES OBJECT_DEPENDS ${RR6_ICON_ICO})
endif()

# Recompiled Xenos shaders: XenosRecomp turns every shader embedded in
# default.xex into a DXIL/SPIR-V cache compiled into rr6_shader_cache. Only the
# native renderer (RR6_NATIVE_GPU) links it; the plugin build still compiles it
# so the pipeline stays exercised.
set(RR6_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(RR6_SHADER_COMMON_H
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/XenosRecomp/XenosRecomp/shader_common.h")
cmake_path(GET RR6_SHADER_COMMON_H PARENT_PATH RR6_SHADER_COMMON_DIR)

add_subdirectory(thirdparty)
include(cmake/shader_cache.cmake)

rr6_shader_cache(
    RECOMP_TARGET XenosRecomp
    XEX           "${CMAKE_CURRENT_SOURCE_DIR}/assets/default.xex"
    SHADER_DIR    "${RR6_GEN_DIR}/shaders"
    INCLUDE_FILE  "${RR6_SHADER_COMMON_H}"
    OUTPUT_CPP    "${RR6_GEN_DIR}/shader_cache.cpp")

add_library(rr6_shader_cache OBJECT "${RR6_GEN_DIR}/shader_cache.cpp")
target_include_directories(rr6_shader_cache PRIVATE src)

# Sources
set(RR6_SOURCES
    src/main.cpp
    ${RR6_ICON_HEADER}
)

if(WIN32)
    add_executable(rr6 WIN32 ${RR6_SOURCES} ${RR6_ICON_RC})
else()
    add_executable(rr6 ${RR6_SOURCES})
endif()

# Test tooling both renderers share: scripted pad input (rr6_input_script).
target_sources(rr6 PRIVATE src/platform/input_script.cpp)
target_include_directories(rr6 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

if(RR6_NATIVE_GPU)
    rexglue_setup_target(rr6)
    include(cmake/native_gpu.cmake)
else()
    rexglue_setup_target(rr6 GPU_PLUGINS xenos)
    # Resolve-target dumps comparable with the native renderer's frame dumps.
    target_sources(rr6 PRIVATE src/platform/xenos_resolve_dump.cpp)
endif()

# The SDK stages its shared runtime libraries next to the exe on Windows only,
# and nothing populates the $ORIGIN rpath it sets on Linux, so librexruntime
# (and libTracyClient in a profiled build) would link but fail to load.
if(UNIX AND NOT APPLE)
    foreach(lib IN ITEMS rex::runtime rex::TracyClient)
        if(TARGET ${lib})
            add_custom_command(TARGET rr6 POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:${lib}> $<TARGET_FILE_DIR:rr6>
                VERBATIM)
        endif()
    endforeach()
endif()
