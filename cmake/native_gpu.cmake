# The native renderer (RR6_NATIVE_GPU): reblue's plume-based Xenos translation
# layer, fed by the XenosRecomp shader cache, in place of the SDK's emulated
# GPU. The guest's D3D library is replaced by REX_HOOKs in src/gpu/hooks.
#
# Expects the rr6 target, RR6_GEN_DIR, RR6_SHADER_COMMON_H and the
# rr6_shader_cache library to exist already.
enable_language(C)

add_subdirectory(thirdparty/plume EXCLUDE_FROM_ALL)
include(cmake/shaders.cmake)

file(GLOB_RECURSE rr6_native_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/*.cpp")
# Built from the top-level CMakeLists: input_script for both renderers,
# xenos_resolve_dump for the plugin build only.
list(REMOVE_ITEM rr6_native_sources
    "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/input_script.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/xenos_resolve_dump.cpp")
target_sources(rr6 PRIVATE
    ${rr6_native_sources}
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/miniz/miniz.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/zstd/zstddeclib.c")
set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/miniz/miniz.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/zstd/zstddeclib.c"
    PROPERTIES COMPILE_OPTIONS "-w")

foreach(shader IN ITEMS copy_vs imgui_vs)
    rr6_host_shader(rr6 ${shader} vs_6_0)
endforeach()
foreach(shader IN ITEMS
        copy_color_ps copy_depth_ps gamma_correction_ps pfx_occlusion_count_ps
        imgui_ps
        resolve_msaa_color_2x resolve_msaa_color_4x resolve_msaa_color_8x
        resolve_msaa_depth_2x resolve_msaa_depth_4x resolve_msaa_depth_8x)
    rr6_host_shader(rr6 ${shader} ps_6_0)
endforeach()

# rr6_prelink DXC-links every specConstantsMask subset of the shader cache at
# build time, so the runtime never pays a DXC link for a shipped shader.
add_executable(rr6_prelink
    tools/prelink_shader_cache.cpp
    src/gpu/shaders/dxc_link.cpp
    "${RR6_GEN_DIR}/shader_cache.cpp"
    thirdparty/miniz/miniz.cpp
    thirdparty/zstd/zstddeclib.c)
target_include_directories(rr6_prelink PRIVATE
    src thirdparty/miniz thirdparty/zstd)
target_link_libraries(rr6_prelink PRIVATE Microsoft::DirectXShaderCompiler)
rr6_stage_dxc(rr6_prelink)

add_custom_command(
    OUTPUT "${RR6_GEN_DIR}/linked_shader_cache.cpp"
    COMMAND rr6_prelink "${RR6_GEN_DIR}/linked_shader_cache.cpp"
    DEPENDS rr6_prelink
    COMMENT "Pre-linking spec-constant shader variants"
    VERBATIM)
target_sources(rr6 PRIVATE "${RR6_GEN_DIR}/linked_shader_cache.cpp")

target_compile_definitions(rr6 PRIVATE RR6_NATIVE_GPU RR6_D3D12)
target_include_directories(rr6 PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${RR6_GEN_DIR}"
    "${RR6_SHADER_COMMON_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/miniz"
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/zstd")
target_link_libraries(rr6 PRIVATE
    plume
    rr6_shader_cache
    rex::xxhash
    rex::o1heap
    d3d12 dxgi
    Microsoft::DirectXShaderCompiler)
rr6_stage_dxc(rr6)
