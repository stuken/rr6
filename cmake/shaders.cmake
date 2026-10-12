# Host HLSL for the native renderer, compiled at build time with the DXC that
# XenosRecomp vendors (dxc-bin sets DIRECTX_DXC_TOOL). Ported from reblue's
# cmake/shaders.cmake.
if(NOT RR6_DXC)
    if(DIRECTX_DXC_TOOL)
        set(RR6_DXC "${DIRECTX_DXC_TOOL}" CACHE STRING "dxc compiler command" FORCE)
    else()
        find_program(RR6_DXC NAMES dxc dxc.exe REQUIRED)
    endif()
endif()

set(RR6_HLSL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/shaders/hlsl")

# rr6_host_shader(<stem> <profile> [dxc args...]) emits
# <stem>.hlsl.<dxil|spirv>.h exposing g_<stem>_<dxil|spirv>, once per backend
# in the build, and adds each header to the exes in RR6_D3D12_TARGETS or
# RR6_VULKAN_TARGETS.
function(rr6_host_shader STEM PROFILE)
    # dxc emits no header deps, so every sibling .hlsli is listed by hand.
    file(GLOB hlsl_includes "${RR6_HLSL_DIR}/*.hlsli")

    foreach(target_list IN ITEMS RR6_D3D12_TARGETS RR6_VULKAN_TARGETS)
        if(NOT ${target_list})
            continue()
        endif()

        if(target_list STREQUAL "RR6_D3D12_TARGETS")
            set(ext dxil)
            set(format_args "")
        else()
            set(ext spirv)
            # Keep cbuffer byte offsets identical to D3D, and match D3D clip space.
            set(format_args -spirv -fvk-use-dx-layout)
            if(PROFILE MATCHES "^vs")
                list(APPEND format_args -fvk-invert-y)
            endif()
        endif()

        set(out "${RR6_GEN_DIR}/src/gpu/shaders/hlsl/${STEM}.hlsl.${ext}.h")
        add_custom_command(
            OUTPUT "${out}"
            COMMAND ${RR6_DXC}
                    -T ${PROFILE} -HV 2021 -all-resources-bound
                    -Wno-ignored-attributes ${format_args}
                    -I "${CMAKE_CURRENT_SOURCE_DIR}"
                    -Fh "${out}" "${RR6_HLSL_DIR}/${STEM}.hlsl"
                    -Vn g_${STEM}_${ext} ${ARGN}
            DEPENDS "${RR6_HLSL_DIR}/${STEM}.hlsl" ${hlsl_includes} "${RR6_SHADER_COMMON_H}"
            COMMENT "Compiling ${STEM}.hlsl (${PROFILE}, ${ext})"
            VERBATIM)
        foreach(target IN LISTS ${target_list})
            target_sources(${target} PRIVATE "${out}")
        endforeach()
    endforeach()
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
