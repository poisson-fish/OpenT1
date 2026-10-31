# Generate private byte arrays from committed bytecode; no runtime file lookup.
set(OPENT1_GPU_SHADERS
    "#pragma once\n#include <cstddef>\nnamespace opent1::gfx::detail::shaders {\n"
)
foreach(stage IN ITEMS vert frag)
    foreach(format IN ITEMS spv dxil)
        set(shader "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/shaders/material.${stage}.${format}")
        set_property(
            DIRECTORY
            APPEND
            PROPERTY CMAKE_CONFIGURE_DEPENDS "${shader}"
        )
        file(READ "${shader}" bytes HEX)
        string(REGEX REPLACE "(..)" "0x\\1," bytes "${bytes}")
        string(APPEND OPENT1_GPU_SHADERS
               "alignas(4) inline constexpr unsigned char ${stage}_${format}[] = {${bytes}};\n"
        )
    endforeach()
    set(shader "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/shaders/material.${stage}.msl")
    set_property(
        DIRECTORY
        APPEND
        PROPERTY CMAKE_CONFIGURE_DEPENDS "${shader}"
    )
    file(READ "${shader}" source)
    string(APPEND OPENT1_GPU_SHADERS
           "inline constexpr char ${stage}_msl[] = R\"shader(${source})shader\";\n"
    )
endforeach()
string(APPEND OPENT1_GPU_SHADERS "}\n")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/gpu_shaders.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/gpu_shaders.hpp" @ONLY
)
