# Host HLSL for the native renderer, compiled at build time with the DXC that
# XenosRecomp vendors (dxc-bin sets DIRECTX_DXC_TOOL). Ported from reblue's
# cmake/shaders.cmake; rr6 builds the D3D12 backend only for now.
if(NOT RR6_DXC)
    if(DIRECTX_DXC_TOOL)
        set(RR6_DXC "${DIRECTX_DXC_TOOL}" CACHE STRING "dxc compiler command" FORCE)
    else()
        find_program(RR6_DXC NAMES dxc dxc.exe REQUIRED)
    endif()
endif()

set(RR6_HLSL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/shaders/hlsl")

# rr6_host_shader(<target> <stem> <profile> [dxc args...]) emits
# <stem>.hlsl.dxil.h exposing g_<stem>_dxil and adds it to <target>.
function(rr6_host_shader TARGET STEM PROFILE)
    # dxc emits no header deps, so every sibling .hlsli is listed by hand.
    file(GLOB hlsl_includes "${RR6_HLSL_DIR}/*.hlsli")
    set(out "${RR6_GEN_DIR}/src/gpu/shaders/hlsl/${STEM}.hlsl.dxil.h")
    add_custom_command(
        OUTPUT "${out}"
        COMMAND ${RR6_DXC}
                -T ${PROFILE} -HV 2021 -all-resources-bound
                -Wno-ignored-attributes
                -I "${CMAKE_CURRENT_SOURCE_DIR}"
                -Fh "${out}" "${RR6_HLSL_DIR}/${STEM}.hlsl"
                -Vn g_${STEM}_dxil ${ARGN}
        DEPENDS "${RR6_HLSL_DIR}/${STEM}.hlsl" ${hlsl_includes} "${RR6_SHADER_COMMON_H}"
        COMMENT "Compiling ${STEM}.hlsl (${PROFILE}, dxil)"
        VERBATIM)
    target_sources(${TARGET} PRIVATE "${out}")
endfunction()

# The runtime shader linker and rr6_prelink both load dxcompiler/dxil.
function(rr6_stage_dxc TARGET)
    get_filename_component(dxc_dir "${DIRECTX_DXC_TOOL}" DIRECTORY)
    add_custom_command(TARGET ${TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${dxc_dir}/dxcompiler.dll" "${dxc_dir}/dxil.dll"
            "$<TARGET_FILE_DIR:${TARGET}>"
        VERBATIM)
endfunction()
