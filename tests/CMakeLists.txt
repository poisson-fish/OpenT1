# One executable, organized into subsystem suites. No game installation required.
add_executable(
    opent1_tests
    main.cpp
    core/bit_stream_test.cpp
    core/stream_test.cpp
    core/compression_test.cpp
    core/resource_test.cpp
    core/resource_retention_test.cpp
    core/mapped_file_test.cpp
    core/persist_test.cpp
    math/integer_test.cpp
    math/rotation_test.cpp
    math/geometry_test.cpp
    console/lexer_parser_test.cpp
    console/runtime_test.cpp
    network/net_transport_test.cpp
    network/net_protocol_test.cpp
    simulation/sim_base_test.cpp
    simulation/sim_collision_movement_test.cpp
    simulation/sim_net_test.cpp
    simulation/sim_telnet_test.cpp
    graphics/bitmap_font_test.cpp
    graphics/software_rasterizer_test.cpp
    graphics/hardware_batch_test.cpp
    world/terrain_grid_test.cpp
    world/ts_shape_test.cpp
    world/collision_test.cpp
    world/mission_test.cpp
    world/render_retention_test.cpp
    gui/canvas_test.cpp
    gui/routing_test.cpp
    gui/editor_test.cpp
    game/player_test.cpp
    game/player_collision_test.cpp
    game/item_collision_test.cpp
    game/weapons_test.cpp
    game/effects_test.cpp
    game/network_parity_test.cpp
    game/mission_editor_test.cpp
    game/e2e_integration_test.cpp
    game/gameplay_fix_pack_test.cpp
    game/trpg_behavior_test.cpp
    game/ai_behavior_test.cpp
    game/rpg_paths_test.cpp
    game/rpg_regressions_test.cpp
    game/compatibility_test.cpp
    game/shell_controls_test.cpp
    game/datablock_quantization_test.cpp
    game/model_rendering_test.cpp
    game/sky_test.cpp
    game/weather_test.cpp
    game/disconnect_test.cpp
    video/video_test.cpp
)
target_link_libraries(opent1_tests PRIVATE opent1_game)
add_dependencies(opent1_tests opent1_ai_behavior_audit)
target_include_directories(opent1_tests PRIVATE support)
include(fixtures.cmake)

set(suites
    core
    math
    console
    network
    simulation
    graphics
    world
    gui
    game
    video
)
if(OPENT1_BUILD_AUDIO)
    target_sources(opent1_tests PRIVATE audio/audio_test.cpp)
    target_link_libraries(opent1_tests PRIVATE opent1_audio_sim)
    add_custom_command(
        TARGET opent1_tests
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:OpenAL>
                $<TARGET_FILE_DIR:opent1_tests>
    )
    list(APPEND suites audio)
endif()
if(OPENT1_BUILD_PLATFORM)
    target_compile_definitions(opent1_tests PRIVATE OPENT1_SHELL_GPU=1)
    option(OPENT1_TEST_GPU "Register real-device GPU tests (requires a native video driver)" OFF)
    set(OPENT1_TEST_GPU_DRIVER
        "" CACHE STRING "SDL GPU backend for real-device tests (empty selects automatically)"
    )
    target_sources(opent1_tests PRIVATE platform/window_test.cpp platform/gpu_cache_test.cpp)
    if(WIN32 AND TARGET SDL3::SDL3-shared)
        add_custom_command(
            TARGET opent1_tests
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3-shared>
                    $<TARGET_FILE_DIR:opent1_tests>
        )
    endif()
    list(APPEND suites platform)
endif()

foreach(suite IN LISTS suites)
    # Separate working directories keep temporary files isolated under parallel CTest.
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${suite}")
    add_test(NAME ${suite} COMMAND opent1_tests --suite ${suite})
    set_tests_properties(
        ${suite}
        PROPERTIES TIMEOUT
                   120
                   LABELS
                   ${suite}
                   WORKING_DIRECTORY
                   "${CMAKE_CURRENT_BINARY_DIR}/${suite}"
                   ENVIRONMENT
                   "SDL_VIDEODRIVER=dummy;ALSOFT_DRIVERS=null"
    )
endforeach()

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/ai_parity")
add_test(NAME ai_parity COMMAND opent1_ai_behavior_audit)
set_tests_properties(
    ai_parity
    PROPERTIES TIMEOUT 120 LABELS game
               WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/ai_parity"
               ENVIRONMENT "SDL_VIDEODRIVER=dummy;ALSOFT_DRIVERS=null"
)

if(OPENT1_BUILD_PLATFORM AND OPENT1_TEST_GPU)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/gpu")
    add_test(NAME gpu COMMAND opent1_tests --suite gpu)
    set_tests_properties(
        gpu
        PROPERTIES TIMEOUT
                   120
                   LABELS
                   "gpu"
                   RUN_SERIAL
                   TRUE
                   WORKING_DIRECTORY
                   "${CMAKE_CURRENT_BINARY_DIR}/gpu"
                   ENVIRONMENT
                   "ALSOFT_DRIVERS=null"
    )
    if(OPENT1_TEST_GPU_DRIVER)
        set_property(TEST gpu APPEND PROPERTY ENVIRONMENT "SDL_GPU_DRIVER=${OPENT1_TEST_GPU_DRIVER}")
    endif()
endif()

add_test(NAME runtime COMMAND opent1 --smoke --headless --no-audio --no-assets --spawn-demo
                              --frames 4
)
set_tests_properties(
    runtime
    PROPERTIES TIMEOUT
               30
               LABELS
               integration
               ENVIRONMENT
               "SDL_VIDEODRIVER=dummy;ALSOFT_DRIVERS=null"
)
