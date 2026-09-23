# Shader products stay in the build tree and are copied beside the packaged assets.
set(PAPER_SHADER_OUTPUT_DIR "${PROJECT_BINARY_DIR}/generated/shaders")
set(paper_shader_constants "${PROJECT_SOURCE_DIR}/include/paper/render/shader_constants.h")
set(paper_shader_source "${PROJECT_SOURCE_DIR}/shaders/scene.hlsl")
if(APPLE)
    # SDL compiles native MSL when creating the device's pipelines; no external SDK executable.
    add_custom_command(OUTPUT "${PAPER_SHADER_OUTPUT_DIR}/scene.metal"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${PAPER_SHADER_OUTPUT_DIR}"
        COMMAND ${CMAKE_COMMAND}
            "-DSHADER_SOURCE=${PROJECT_SOURCE_DIR}/shaders/scene.metal"
            "-DSHADER_CONSTANTS=${paper_shader_constants}"
            "-DSHADER_OUTPUT=${PAPER_SHADER_OUTPUT_DIR}/scene.metal"
            -P "${PROJECT_SOURCE_DIR}/cmake/AssembleMetal.cmake"
        DEPENDS "${PROJECT_SOURCE_DIR}/shaders/scene.metal" "${paper_shader_constants}"
            "${PROJECT_SOURCE_DIR}/cmake/AssembleMetal.cmake"
        VERBATIM)
    add_custom_target(paper_shaders DEPENDS "${PAPER_SHADER_OUTPUT_DIR}/scene.metal")
else()
    find_program(PAPER_DXC NAMES dxc REQUIRED)
    set(paper_shader_outputs)
    foreach(entry world_vertex world_fragment shadow_vertex shadow_fragment particle_vertex particle_fragment screen_vertex screen_fragment)
        string(TOUPPER "${entry}" entry_define)
        if(entry MATCHES "_vertex$")
            set(shader_profile vs_6_0)
        else()
            set(shader_profile ps_6_0)
        endif()
        set(spirv_file "${PAPER_SHADER_OUTPUT_DIR}/${entry}.spv")
        add_custom_command(OUTPUT "${spirv_file}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${PAPER_SHADER_OUTPUT_DIR}"
            COMMAND "${PAPER_DXC}" -spirv -fspv-target-env=vulkan1.0 -fvk-use-dx-layout
                -D PAPER_SPIRV -D "${entry_define}" -E "${entry}" -T "${shader_profile}"
                -Fo "${spirv_file}" "${paper_shader_source}"
            DEPENDS "${paper_shader_source}" "${paper_shader_constants}" VERBATIM)
        list(APPEND paper_shader_outputs "${spirv_file}")
        if(WIN32)
            set(dxil_file "${PAPER_SHADER_OUTPUT_DIR}/${entry}.dxil")
            add_custom_command(OUTPUT "${dxil_file}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${PAPER_SHADER_OUTPUT_DIR}"
                COMMAND "${PAPER_DXC}" -D "${entry_define}" -E "${entry}" -T "${shader_profile}"
                    -Fo "${dxil_file}" "${paper_shader_source}"
                DEPENDS "${paper_shader_source}" "${paper_shader_constants}" VERBATIM)
            list(APPEND paper_shader_outputs "${dxil_file}")
        endif()
    endforeach()
    add_custom_target(paper_shaders DEPENDS ${paper_shader_outputs})
endif()

set_property(TARGET paper_shaders PROPERTY PAPER_SHADER_OUTPUT_DIR "${PAPER_SHADER_OUTPUT_DIR}")
