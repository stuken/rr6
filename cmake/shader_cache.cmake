# rr6_shader_cache() pulls the shader containers out of the XEX at build time,
# runs the Xenos shader recompiler over them into OUTPUT_CPP, and defines
# rr6_shader_cache_gen. It also defines rr6_shader_hlsl_dump for inspecting the
# intermediate HLSL.
#
# RR6 ships its shaders inside the encrypted default.xex rather than as loose
# files, so scripts/extract_shaders.py writes them into SHADER_DIR first. That
# directory lives in the build tree, keeping game data out of the source tree.
function(rr6_shader_cache)
    cmake_parse_arguments(ARG "" "RECOMP_TARGET;XEX;SHADER_DIR;INCLUDE_FILE;OUTPUT_CPP" "" ${ARGN})
    foreach(arg RECOMP_TARGET XEX SHADER_DIR INCLUDE_FILE OUTPUT_CPP)
        if(NOT ARG_${arg})
            message(FATAL_ERROR "rr6_shader_cache: missing ${arg}")
        endif()
    endforeach()

    execute_process(
        COMMAND ${Python3_EXECUTABLE} -c "import cryptography"
        RESULT_VARIABLE crypto_missing OUTPUT_QUIET ERROR_QUIET)
    if(crypto_missing)
        message(FATAL_ERROR
            "rr6: scripts/extract_shaders.py needs the Python 'cryptography' "
            "package to decrypt default.xex. Install it with:\n"
            "  ${Python3_EXECUTABLE} -m pip install cryptography")
    endif()

    set(extract_script "${CMAKE_CURRENT_SOURCE_DIR}/scripts/extract_shaders.py")
    set(shader_index "${ARG_SHADER_DIR}/shaders.csv")
    add_custom_command(
        OUTPUT "${shader_index}"
        COMMAND ${Python3_EXECUTABLE} "${extract_script}" "${ARG_XEX}" "${ARG_SHADER_DIR}"
        DEPENDS "${extract_script}" "${ARG_XEX}"
        COMMENT "Extracting Xenos shaders from ${ARG_XEX}"
        VERBATIM)

    cmake_path(GET ARG_OUTPUT_CPP PARENT_PATH output_dir)
    file(MAKE_DIRECTORY "${output_dir}")

    add_custom_command(
        OUTPUT "${ARG_OUTPUT_CPP}"
        COMMAND $<TARGET_FILE:${ARG_RECOMP_TARGET}>
                "${ARG_SHADER_DIR}" "${ARG_OUTPUT_CPP}" "${ARG_INCLUDE_FILE}"
        DEPENDS ${ARG_RECOMP_TARGET} "${ARG_INCLUDE_FILE}" "${shader_index}"
        COMMENT "Recompiling Xenos shaders from ${ARG_SHADER_DIR}"
        USES_TERMINAL VERBATIM)
    add_custom_target(rr6_shader_cache_gen DEPENDS "${ARG_OUTPUT_CPP}")

    set(RR6_HLSL_DUMP_DIR "${CMAKE_BINARY_DIR}/hlsl_dump" CACHE PATH
        "Directory rr6_shader_hlsl_dump writes recompiled HLSL into")
    add_custom_target(rr6_shader_hlsl_dump
        COMMAND ${CMAKE_COMMAND} -E make_directory "${RR6_HLSL_DUMP_DIR}"
        COMMAND $<TARGET_FILE:${ARG_RECOMP_TARGET}>
                "${ARG_SHADER_DIR}" "${output_dir}/shader_cache.hlsldump.cpp"
                "${ARG_INCLUDE_FILE}" "${RR6_HLSL_DUMP_DIR}"
        DEPENDS "${shader_index}"
        COMMENT "Dumping recompiled HLSL to ${RR6_HLSL_DUMP_DIR}"
        USES_TERMINAL VERBATIM)
    add_dependencies(rr6_shader_hlsl_dump ${ARG_RECOMP_TARGET})
endfunction()
