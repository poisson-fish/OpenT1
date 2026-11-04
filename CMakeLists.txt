cmake_minimum_required(VERSION 3.24)

project(
    OpenT1
    VERSION 1.11.0
    DESCRIPTION "Starsiege: Tribes 1 Modern C++20 Cross-Platform Engine & Game (OpenT1)"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
option(OPENT1_FETCH_SDL "Fetch pinned SDL3 if no installed package is available" ON)
option(OPENT1_BUILD_EXAMPLES "Build native OpenT1 demonstration applications" OFF)
option(OPENT1_BUILD_TERRAIN2 "Build the optional native Terrain2 authoring/render branch" ON)

# -----------------------------------------------------------------------------
# Compiler Warning & Standards Configuration
# -----------------------------------------------------------------------------
add_library(opent1_compiler_flags INTERFACE)
target_compile_features(opent1_compiler_flags INTERFACE cxx_std_20)

if(MSVC)
    target_compile_options(
        opent1_compiler_flags
        INTERFACE $<$<COMPILE_LANGUAGE:CXX>:/W4>
                  $<$<COMPILE_LANGUAGE:CXX>:/WX>
                  $<$<COMPILE_LANGUAGE:CXX>:/permissive->
                  $<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>
                  $<$<COMPILE_LANGUAGE:CXX>:/Zc:preprocessor>
                  $<$<COMPILE_LANGUAGE:CXX>:/utf-8>
                  $<$<COMPILE_LANGUAGE:CXX>:/EHsc>
    )
    target_compile_definitions(
        opent1_compiler_flags INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
    )
else()
    # Legacy rasterization and collision fixtures require separate multiply/add
    # rounding; ARM's default FMA contraction changes boundary decisions.
    target_compile_options(
        opent1_compiler_flags
        INTERFACE $<$<COMPILE_LANGUAGE:CXX>:-Wall> $<$<COMPILE_LANGUAGE:CXX>:-Wextra>
                  $<$<COMPILE_LANGUAGE:CXX>:-Wpedantic> $<$<COMPILE_LANGUAGE:CXX>:-Werror>
                  $<$<COMPILE_LANGUAGE:CXX>:-ffp-contract=off>
    )
endif()

# -----------------------------------------------------------------------------
# Layer 1: opent1_core (Foundational C++20 types, bitstreams, streams, VOL, persist)
# -----------------------------------------------------------------------------
add_library(
    opent1_core STATIC
    src/core/types.cpp
    src/core/base.cpp
    src/core/bitset.cpp
    src/core/crc16.cpp
    src/core/filename.cpp
    src/core/stream_io.cpp
    src/core/mem_stream.cpp
    src/core/bit_stream.cpp
    src/core/block_stream.cpp
    src/core/rle_stream.cpp
    src/core/lzh_stream.cpp
    src/core/vol_stream.cpp
    src/core/mapped_file.cpp
    src/core/file_stream.cpp
    src/core/string_table.cpp
    src/core/timer.cpp
    src/core/thread_base.cpp
    src/core/registry.cpp
    src/core/res_manager.cpp
    src/core/persist.cpp
)

target_include_directories(opent1_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(opent1_core PUBLIC opent1_compiler_flags)
find_package(Threads REQUIRED)
target_link_libraries(opent1_core PUBLIC Threads::Threads)

include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_LIBRARIES Threads::Threads)
check_cxx_source_compiles(
    "#include <stop_token>
     #include <thread>
     int main() {
         std::jthread worker([](std::stop_token) {});
         worker.request_stop();
     }"
    OPENT1_HAS_CXX20_THREADS
)
unset(CMAKE_REQUIRED_LIBRARIES)
if(NOT OPENT1_HAS_CXX20_THREADS)
    message(
        FATAL_ERROR
            "OpenT1 requires C++20 std::jthread and std::stop_token support. "
            "On macOS, use Homebrew LLVM 20 or newer with its matching libc++ headers and runtime; "
            "see docs/development.md."
    )
endif()

add_library(
    opent1_math STATIC
    src/math/math_Fix16_and_int.cpp
    src/math/trig.cpp
    src/math/euler.cpp
    src/math/quat.cpp
    src/math/matrix.cpp
    src/math/plane.cpp
    src/math/collision.cpp
    src/math/random.cpp
)
target_link_libraries(opent1_math PUBLIC opent1_core)
if(MSVC
   AND CMAKE_SIZEOF_VOID_P EQUAL 8
   AND CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|amd64|x86_64|X86_64"
)
    enable_language(ASM_MASM)
    target_sources(opent1_math PRIVATE src/math/x87_x64.asm)
    target_compile_definitions(opent1_math PUBLIC OPENT1_NATIVE_X87=1)
endif()

# -----------------------------------------------------------------------------
# Layer 2: opent1_console (tags, script front end and runtime)
# -----------------------------------------------------------------------------
add_library(
    opent1_console STATIC src/console/tag_dictionary.cpp src/console/lexer_parser.cpp
                          src/console/eval.cpp src/console/console.cpp
)
target_link_libraries(opent1_console PUBLIC opent1_core)

# -----------------------------------------------------------------------------
# Layer 3: opent1_platform (SDL3 windowing, input events and callback main loop)
# -----------------------------------------------------------------------------
add_library(
    opent1_net STATIC
    src/platform/net_transport.cpp src/platform/udp_transport.cpp src/platform/ipx_transport.cpp
    src/platform/loop_transport.cpp src/platform/tcp_stream.cpp
)
target_link_libraries(opent1_net PUBLIC opent1_core)
if(WIN32)
    target_link_libraries(opent1_net PRIVATE ws2_32)
endif()
find_package(SDL3 3.2 CONFIG QUIET)
if(NOT SDL3_FOUND)
    if(NOT OPENT1_FETCH_SDL)
        message(FATAL_ERROR "Install SDL3 >= 3.2, set SDL3_DIR, or enable OPENT1_FETCH_SDL")
    endif()
    include(FetchContent)
    set(SDL_SHARED
        OFF
        CACHE BOOL "" FORCE
    )
    set(SDL_STATIC
        ON
        CACHE BOOL "" FORCE
    )
    set(SDL_TEST_LIBRARY
        OFF
        CACHE BOOL "" FORCE
    )
    set(SDL_TESTS
        OFF
        CACHE BOOL "" FORCE
    )
    set(SDL_EXAMPLES
        OFF
        CACHE BOOL "" FORCE
    )
    fetchcontent_declare(
        SDL3
        URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.2/SDL3-3.4.2.tar.gz
        URL_HASH SHA256=ef39a2e3f9a8a78296c40da701967dd1b0d0d6e267e483863ce70f8a03b4050c
        DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/downloads"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        TIMEOUT 120
        INACTIVITY_TIMEOUT 30
    )
    fetchcontent_makeavailable(SDL3)
    include(cmake/SDL3EventWatch.cmake)
endif()
add_library(opent1_platform STATIC src/platform/sdl_window.cpp)
target_link_libraries(opent1_platform PUBLIC opent1_core opent1_net SDL3::SDL3)

# -----------------------------------------------------------------------------
# Layer 4: simulation object graph, event scheduling and services
# -----------------------------------------------------------------------------
# Palette assets are the small Layer 5 prerequisite of SimPalette/world canvases.
add_library(opent1_palette STATIC src/gfx/palette.cpp)
target_link_libraries(opent1_palette PUBLIC opent1_core)
add_library(
    opent1_sim STATIC
    src/sim/sim_base.cpp
    src/sim/sim_resource_volume.cpp
    src/sim/sim_interior_grouping.cpp
    src/sim/bayes_ai.cpp
    src/sim/sim_input_action.cpp
    src/sim/sim_world_game.cpp
    src/sim/sim_collision_movement.cpp
    src/sim/sim_dynamic_light.cpp
    src/sim/common_irc_shadow.cpp
    src/sim/sim_environment_render.cpp
    src/sim/sim_native_shadow.cpp
    src/sim/sim_net.cpp
    src/sim/sim_net_ghost.cpp
    src/sim/sim_net_packet.cpp
    src/sim/sim_net_delegate.cpp
    src/sim/sim_net_demo.cpp
    src/sim/sim_net_browser.cpp
    src/sim/sim_telnet.cpp
    src/sim/sim_console_data.cpp
    src/sim/sim_irc.cpp
    src/sim/sim_irc_plugin.cpp
    src/sim/sim_canvas.cpp
    src/sim/sim_palette.cpp
    src/sim/sim_inspection.cpp
    src/sim/sim_scene.cpp
    src/sim/sim_native_media.cpp
    src/platform/native_media.cpp
)
target_link_libraries(opent1_sim PUBLIC opent1_console opent1_math opent1_net opent1_palette)
if(WIN32)
    target_link_libraries(opent1_sim PRIVATE winmm ole32 oleaut32 strmiids)
endif()
include(cmake/OpenALSoft.cmake)
add_library(opent1_sim_sdl STATIC src/sim/sim_sdl_input.cpp src/sim/sim_sdl_canvas.cpp)
target_link_libraries(opent1_sim_sdl PUBLIC opent1_sim opent1_platform)

# -----------------------------------------------------------------------------
# Layer 5: graphics assets and portable indexed drawing (independent of SDL/sim)
# -----------------------------------------------------------------------------
add_library(
    opent1_gfx STATIC
    src/gfx/bitmap_palette_font.cpp
    src/gfx/software_rasterizer.cpp
    src/gfx/software_transform.cpp
    src/gfx/software_polygon.cpp
    src/gfx/software_visibility.cpp
    src/gfx/graphics_support.cpp
    src/gfx/texture_cache.cpp
    src/gfx/hardware_batch.cpp
)
target_link_libraries(opent1_gfx PUBLIC opent1_palette opent1_math)
add_library(
    opent1_sim_gfx STATIC src/sim/sim_software_environment.cpp src/sim/sim_hardware_environment.cpp
                          src/sim/sim_inspector.cpp
)
target_link_libraries(opent1_sim_gfx PUBLIC opent1_sim opent1_gfx)
include(cmake/embed_gpu_shaders.cmake)
add_library(opent1_gfx_sdl STATIC src/gfx/sdl_gpu_surface.cpp src/gfx/sdl_gpu_material.cpp)
target_include_directories(opent1_gfx_sdl PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(opent1_gfx_sdl PUBLIC opent1_gfx opent1_platform)
add_library(opent1_sim_gfx_sdl STATIC src/sim/sim_gpu_canvas.cpp src/sim/sim_sdl_inspector.cpp)
target_link_libraries(opent1_sim_gfx_sdl PUBLIC opent1_sim_sdl opent1_gfx_sdl opent1_sim_gfx)
if(OPENT1_BUILD_EXAMPLES)
    add_executable(opent1_inspector_demo examples/inspector_demo.cpp)
    target_link_libraries(opent1_inspector_demo PRIVATE opent1_sim_gfx_sdl)
    add_executable(opent1_renderer_demo WIN32 examples/renderer_demo.cpp)
    target_link_libraries(opent1_renderer_demo PRIVATE opent1_gfx_sdl)
    if(WIN32 AND TARGET SDL3::SDL3-shared)
        foreach(example IN ITEMS opent1_inspector_demo opent1_renderer_demo)
            add_custom_command(
                TARGET ${example}
                POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3-shared>
                        $<TARGET_FILE_DIR:${example}>
            )
        endforeach()
    endif()
endif()

# -----------------------------------------------------------------------------
# Layer 6: owned TS shape/material assets and world rendering adapters
# -----------------------------------------------------------------------------
add_library(
    opent1_world STATIC
    src/world/ts_shape.cpp
    src/world/ts_shape_pose.cpp
    src/world/ts_shape_instance.cpp
    src/world/ts_static_shape.cpp
    src/world/ts_lighting.cpp
    src/world/ts_collision.cpp
    src/world/terrain_collision.cpp
    src/world/interior_collision.cpp
    src/world/terrain_grid.cpp
    src/world/terrain_lighting.cpp
    src/world/terrain_textures.cpp
    src/world/interior_shape.cpp
    src/world/interior_detail.cpp
    src/world/interior_lighting.cpp
    src/world/interior_light_player.cpp
    src/world/interior_visibility.cpp
    src/world/interior_pvs.cpp
    src/world/interior_links.cpp
    src/world/interior_containers.cpp
)
target_link_libraries(opent1_world PUBLIC opent1_gfx)
add_executable(opent1_asset_tool tools/asset_tool.cpp)
target_link_libraries(opent1_asset_tool PRIVATE opent1_world)
add_library(
    opent1_world_sim STATIC
    src/world/world_resources.cpp
    src/world/world_scene.cpp
    src/world/ts_collision_sim.cpp
    src/world/world_motion.cpp
    src/world/world_collision_sim.cpp
    src/world/world_surfaces.cpp
    src/world/shadow_receivers.cpp
    src/world/world_shadows.cpp
    src/world/terrain_lod.cpp
    src/world/terrain_stream.cpp
    src/world/terrain_surface_sim.cpp
    src/world/interior_surface_sim.cpp
    src/world/interior_scene_lighting.cpp
)
target_sources(opent1_world_sim PRIVATE src/world/terrain_scene_lighting.cpp)
target_sources(opent1_world_sim PRIVATE src/world/world_mission.cpp)
target_sources(opent1_world PRIVATE src/world/terrain_collision_snapshot.cpp)
target_sources(opent1_world PRIVATE src/world/terrain_texture_detail.cpp)
target_sources(opent1_world PRIVATE src/world/terrain_geometry.cpp)
target_sources(opent1_world PRIVATE src/world/terrain_visibility.cpp)
target_sources(opent1_world PRIVATE src/world/terrain_authoring.cpp)
target_sources(opent1_world PRIVATE src/world/interior_authoring.cpp)
target_sources(opent1_world PRIVATE src/world/landscape.cpp)
target_sources(opent1_world PRIVATE src/world/landscape_mapper.cpp)
if(OPENT1_BUILD_TERRAIN2)
    target_sources(opent1_world PRIVATE src/world/terrain2.cpp)
    target_compile_definitions(opent1_world PUBLIC OPENT1_HAS_TERRAIN2=1)
endif()
target_link_libraries(opent1_world_sim PUBLIC opent1_world opent1_sim_gfx)
target_sources(
    opent1_world_sim PRIVATE src/world/scene_objects.cpp src/world/mission_selection.cpp
                             src/world/object_plugins.cpp
)
target_sources(opent1_world_sim PRIVATE src/world/terrain_editor.cpp)
add_library(opent1_world_audio STATIC src/world/sound_marker.cpp)
target_link_libraries(opent1_world_audio PUBLIC opent1_world_sim opent1_audio_sim)
add_library(
    opent1_gui STATIC
    src/gui/gui.cpp
    src/gui/controls.cpp
    src/gui/containers.cpp
    src/gui/text_format.cpp
    src/gui/edit_control.cpp
    src/gui/runtime.cpp
    src/gui/plugin.cpp
)
target_link_libraries(opent1_gui PUBLIC opent1_sim opent1_gfx)
add_library(
    opent1_gui_world STATIC src/gui/terrain_tools.cpp src/gui/terrain_dialogs.cpp
                            src/gui/ts_control.cpp src/gui/mission_view.cpp
)
target_sources(opent1_gui_world PRIVATE src/gui/terrain_ui.cpp)
target_sources(opent1_gui_world PRIVATE src/gui/file_browser.cpp src/gui/tool_dialogs.cpp)
target_sources(opent1_world_sim PRIVATE src/world/input_commands.cpp)
target_sources(opent1_world_sim PRIVATE src/world/network_host.cpp)
target_link_libraries(opent1_gui_world PUBLIC opent1_gui opent1_world_sim)
add_library(opent1_video STATIC src/video/smacker.cpp src/video/player.cpp)
target_link_libraries(opent1_video PUBLIC opent1_gfx)
add_library(
    opent1_game STATIC
    src/game/version.cpp
    src/game/cpuSpeed.cpp
    src/game/BanList.cpp
    src/game/filePlugin.cpp
    src/game/legacy_sound.cpp
    src/game/scriptPlugin.cpp
    src/game/dataBlockManager.cpp
    src/game/FearDynamicDataPlugin.cpp
    src/game/fearCSDelegate.cpp
    src/game/FearPlugin.cpp
    src/game/game_demo.cpp
    src/game/game_network.cpp
    src/game/game_network_commands.cpp
    src/game/game_script_bindings.cpp
    src/game/player_script_bindings.cpp
    src/game/game_utility_commands.cpp
    src/game/game_gui_bindings.cpp
    src/game/f_base.cpp
    src/game/shapeCollision.cpp
    src/game/PlayerCollision.cpp
    src/game/simMoveObj.cpp
    src/game/gameBase.cpp
    src/game/shapebase.cpp
    src/game/staticBase.cpp
    src/game/moveable.cpp
    src/game/InputHandler.cpp
    src/game/ChatMenu.cpp
    src/game/fearPlayerPSC.cpp
    src/game/PlayerManager.cpp
    src/game/player.cpp
    src/game/playerUpdate.cpp
    src/game/playerInventory.cpp
    src/game/smokePuff.cpp
    src/game/explosion.cpp
    src/game/debris.cpp
    src/game/partDebris.cpp
    src/game/projAttatch.cpp
    src/game/projectile.cpp
    src/game/projectileFactory.cpp
    src/game/projBullet.cpp
    src/game/projGrenade.cpp
    src/game/projRocket.cpp
    src/game/projLaser.cpp
    src/game/projTargetLaser.cpp
    src/game/projSeekMissile.cpp
    src/game/projLightning.cpp
    src/game/projRepair.cpp
    src/game/item.cpp
    src/game/sensor.cpp
    src/game/sensorManager.cpp
    src/game/turret.cpp
    src/game/Mine.cpp
    src/game/vehicle.cpp
    src/game/Flier.cpp
    src/game/Sky.cpp
    src/game/StarField.cpp
    src/game/planet.cpp
    src/game/snowfall.cpp
    src/game/interiorShape.cpp
    src/game/sound.cpp
    src/game/marker.cpp
    src/game/trigger.cpp
    src/game/dropPoints.cpp
    src/game/teamGroup.cpp
    src/game/decalManager.cpp
    src/game/tribesShadowRenderImage.cpp
    src/game/FlyingCamera.cpp
    src/game/observerCamera.cpp
    src/game/editCamera.cpp
    src/game/ai.cpp
    src/game/aiObjectives.cpp
    src/game/aiManager.cpp
    src/game/aiRespawn.cpp
    src/game/aiPlugin.cpp
    src/game/FearMissionEditor.cpp
    src/game/fearMissionPlugin.cpp
    src/game/MissionObjectList.cpp
    src/game/HudManager.cpp
    src/game/fearGuiHudCtrl.cpp
    src/game/FearGuiHudArrayCtrl.cpp
    src/game/FearGuiHudList.cpp
    src/game/FearHudArrayCtrl.cpp
    src/game/FearHudMatrixCtrl.cpp
    src/game/FearHudPalTable.cpp
    src/game/HealthHud.cpp
    src/game/FearGuiJetHud.cpp
    src/game/clockhud.cpp
    src/game/compasshud.cpp
    src/game/CurWeapHud.cpp
    src/game/fearGuiCrosshair.cpp
    src/game/FearHudRadarPing.cpp
    src/game/fearGuiLag.cpp
    src/game/FearGuiCenterPrint.cpp
    src/game/FearGuiChat.cpp
    src/game/FearGuiChatDisplay.cpp
    src/game/FearGuiChatMenu.cpp
    src/game/FearGuiMenu.cpp
    src/game/FearGuiMenuCtrl.cpp
    src/game/FearGuiMenuDisplay.cpp
    src/game/fearGuiInventory.cpp
    src/game/FearGuiInvList.cpp
    src/game/FearHudBuySell.cpp
    src/game/FearHudInvList.cpp
    src/game/FHinventory.cpp
    src/game/FearGuiScoreList.cpp
    src/game/FearGuiPage.cpp
    src/game/FearGuiTSCommander.cpp
    src/game/FearGuiCmdView.cpp
    src/game/FearGuiCmdPanel.cpp
    src/game/FearGuiCmdRadio.cpp
    src/game/FearGuiCmdTurretList.cpp
    src/game/FearGuiCommandItemList.cpp
    src/game/FearGuiCommandTeamList.cpp
    src/game/fearGuiCMDObserve.cpp
    src/game/FearGuiCurCmd.cpp
    src/game/FearHudCmdObj.cpp
    src/game/fearGuiControl.cpp
    src/game/FearGuiBox.cpp
    src/game/FearGuiShellBorder.cpp
    src/game/FearGuiShellPal.cpp
    src/game/FearGuiPaletteCtrl.cpp
    src/game/FearGuiUnivButton.cpp
    src/game/FearGuiCFGButton.cpp
    src/game/FearGuiRadio.cpp
    src/game/FearGuiSwitch.cpp
    src/game/FearGuiTabMenu.cpp
    src/game/fearGuiSlider.cpp
    src/game/FearGuiScrollCtrl.cpp
    src/game/FearGuiArrayCtrl.cpp
    src/game/FearGuiColumns.cpp
    src/game/fearGuiTextList.cpp
    src/game/FearGuiSimpleText.cpp
    src/game/FearGuiTextFMT.cpp
    src/game/fearGuiTextFormat.cpp
    src/game/FearGuiTestEdit.cpp
    src/game/FearGuiBitmapCtrl.cpp
    src/game/fearGuiShapeView.cpp
    src/game/fearGuiSkinView.cpp
    src/game/fearGuiSmacker.cpp
    src/game/fearGuiPopUpMenu.cpp
    src/game/FearGuiCombo.cpp
    src/game/FearGuiStandardCombo.cpp
    src/game/FearGuiControlsCombo.cpp
    src/game/FearGuiGenderCombo.cpp
    src/game/FearGuiPlayerCombo.cpp
    src/game/FearGuiBuddyCombo.cpp
    src/game/FearGuiMissionCombo.cpp
    src/game/FearGuiFilterCombo.cpp
    src/game/FearGuiGenericCB.cpp
    src/game/FearGuiServerFilterCB.cpp
    src/game/FearGuiFilterCBList.cpp
    src/game/FearGuiFilters.cpp
    src/game/FearGuiHelpCtrl.cpp
    src/game/fearGuiServerList.cpp
    src/game/fearGuiMasterList.cpp
    src/game/FearGuiLobbyList.cpp
    src/game/FearGuiServerInfo.cpp
    src/game/FearGuiServerDetail.cpp
    src/game/FearGuiPlayerList.cpp
    src/game/FearGuiMissionList.cpp
    src/game/FearGuiRecList.cpp
    src/game/FearGuiRecordings.cpp
    src/game/ircShell.cpp
    src/game/fearGuiIRCClient.cpp
    src/game/FearGuiIRCView.cpp
    src/game/FearGuiIRCChanCtrl.cpp
    src/game/fearGuiIRCChannelList.cpp
    src/game/fearGuiIRCChannelPropsCtrl.cpp
    src/game/FearGuiIRCNicks.cpp
    src/game/FearGuiIRCTopic.cpp
    src/game/FearGuiIRCTextEdit.cpp
    src/game/fearGuiIRCActiveTextEdit.cpp
    src/game/fearGuiIRCActiveTextFormat.cpp
    src/game/FearGuiIRCScroll.cpp
    src/game/FearGuiIRCServersCB.cpp
    src/game/fearGuiIRCBanControl.cpp
    src/game/fearGuiIRCHelpControl.cpp
    src/game/fearGuiIRCJoinControl.cpp
    src/game/FearGuiDialog.cpp
    src/game/FearGuiDlgBox.cpp
    src/game/FearGuiAbort.cpp
    src/game/dlgMainMenu.cpp
    src/game/dlgPlay.cpp
    src/game/dlgConnect.cpp
    src/game/dlgJoinGame.cpp
    src/game/dlgCreateServer.cpp
    src/game/dlgMultiSetup.cpp
    src/game/dlgPlayerSetup.cpp
    src/game/dlgMissionSelect.cpp
    src/game/dlgOptions.cpp
    src/game/dlgRecordings.cpp
    src/game/dlgIRCLogin.cpp
    src/game/dlgIRCChat.cpp
    src/game/dlgQuit.cpp
    src/main/main.cpp
)
target_link_libraries(opent1_game PUBLIC opent1_gui_world opent1_video)
add_library(opent1_video_sdl STATIC src/video/sdl_player.cpp src/video/audio_player.cpp)
target_link_libraries(opent1_video_sdl PUBLIC opent1_video opent1_gfx_sdl opent1_audio)
if(OPENT1_BUILD_EXAMPLES)
    add_executable(opent1_smacker_player examples/smacker_player.cpp)
    target_link_libraries(opent1_smacker_player PRIVATE opent1_video_sdl)
    add_custom_command(
        TARGET opent1_smacker_player
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:OpenAL>
                $<TARGET_FILE_DIR:opent1_smacker_player>
    )
    if(WIN32 AND TARGET SDL3::SDL3-shared)
        add_custom_command(
            TARGET opent1_smacker_player
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3-shared>
                    $<TARGET_FILE_DIR:opent1_smacker_player>
        )
    endif()
endif()
add_library(opent1_gui_sdl STATIC src/gui/sdl_gui.cpp)
target_link_libraries(opent1_gui_sdl PUBLIC opent1_gui opent1_platform)
add_library(
    opent1_gui_tools_sdl STATIC
    src/gui/sdl_presentation.cpp
    src/gui/sdl_terrain_tools.cpp
    src/gui/native_object_hosts.cpp
    src/gui/native_input_host.cpp
    src/gui/native_tree_host.cpp
    src/gui/native_editor_host.cpp
    src/gui/native_tool_host.cpp
    src/gui/sdl_modal.cpp
)
target_link_libraries(
    opent1_gui_tools_sdl PUBLIC opent1_gui_world opent1_gui_sdl opent1_sim_gfx_sdl
)
if(WIN32)
    target_link_libraries(opent1_gui_tools_sdl PRIVATE shell32)
endif()
target_link_libraries(opent1_game INTERFACE opent1_gui_tools_sdl opent1_sim_gfx_sdl)
# The runtime source in this library includes SDL directly. Package aliases
# may forward their headers through SDL3::Headers rather than expose paths.
target_link_libraries(opent1_game PRIVATE SDL3::SDL3)
target_link_libraries(opent1_game INTERFACE opent1_world_audio)
target_link_libraries(opent1_game INTERFACE opent1_video_sdl)
add_executable(opent1 src/main/main.cpp)
target_compile_definitions(opent1 PRIVATE OPENT1_STANDALONE_EXECUTABLE=1)
target_link_libraries(opent1 PRIVATE opent1_game)
# Source-derived AI parity checks and optional installed-content traces.
add_executable(opent1_shadow_benchmark EXCLUDE_FROM_ALL tools/shadow_benchmark.cpp)
target_link_libraries(opent1_shadow_benchmark PRIVATE opent1_world_sim)
add_executable(opent1_shadow_allocation_audit EXCLUDE_FROM_ALL tools/shadow_benchmark.cpp)
target_compile_definitions(opent1_shadow_allocation_audit PRIVATE OPENT1_SHADOW_ALLOCATIONS=1)
target_link_libraries(opent1_shadow_allocation_audit PRIVATE opent1_world_sim)

add_executable(opent1_ai_behavior_audit EXCLUDE_FROM_ALL tools/ai_behavior_audit.cpp)
target_link_libraries(opent1_ai_behavior_audit PRIVATE opent1_game)
foreach(audit_runtime IN ITEMS OpenAL SDL3::SDL3-shared)
    if(WIN32 AND TARGET ${audit_runtime})
        add_custom_command(
            TARGET opent1_ai_behavior_audit
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:${audit_runtime}>
                    $<TARGET_FILE_DIR:opent1_ai_behavior_audit>
        )
    endif()
endforeach()
add_executable(opent1_render_benchmark EXCLUDE_FROM_ALL tools/render_benchmark.cpp)
target_link_libraries(opent1_render_benchmark PRIVATE opent1_game opent1_gfx_sdl)
if(WIN32 AND TARGET SDL3::SDL3-shared)
    add_custom_command(
        TARGET opent1_render_benchmark
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3-shared>
                $<TARGET_FILE_DIR:opent1_render_benchmark>
    )
endif()
# Explicit diagnostic target: allocation tracking is confined to this executable.
add_executable(opent1_memory_audit EXCLUDE_FROM_ALL tools/memory_audit.cpp src/audio/wave.cpp)
target_link_libraries(opent1_memory_audit PRIVATE opent1_game)
if(WIN32)
    target_link_libraries(opent1_memory_audit PRIVATE psapi dxgi)
endif()
target_compile_definitions(opent1_memory_audit PRIVATE OPENT1_MEMORY_GPU=1)
target_include_directories(opent1_memory_audit PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx")
target_link_libraries(opent1_memory_audit PRIVATE opent1_gfx_sdl)
add_custom_command(
    TARGET opent1_memory_audit
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:OpenAL>
            $<TARGET_FILE_DIR:opent1_memory_audit>
)
if(WIN32 AND TARGET SDL3::SDL3-shared)
    add_custom_command(
        TARGET opent1_memory_audit
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3-shared>
                $<TARGET_FILE_DIR:opent1_memory_audit>
    )
endif()
add_custom_command(
    TARGET opent1
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:OpenAL>
            $<TARGET_FILE_DIR:opent1>
)
if(WIN32 AND TARGET SDL3::SDL3-shared)
    add_custom_command(
        TARGET opent1
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3-shared>
                $<TARGET_FILE_DIR:opent1>
    )
endif()
if(TARGET opent1_renderer_demo)
    target_link_libraries(opent1_renderer_demo PRIVATE opent1_world opent1_sim_gfx)
endif()
if(OPENT1_BUILD_EXAMPLES)
    add_executable(opent1_ts_shape_demo examples/ts_shape_demo.cpp)
    target_link_libraries(opent1_ts_shape_demo PRIVATE opent1_world_sim opent1_gfx_sdl)
    add_executable(opent1_world_surface_demo examples/world_surface_demo.cpp)
    target_link_libraries(opent1_world_surface_demo PRIVATE opent1_world_sim opent1_gfx_sdl)
    if(WIN32 AND TARGET SDL3::SDL3-shared)
        add_custom_command(
            TARGET opent1_ts_shape_demo
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3-shared>
                    $<TARGET_FILE_DIR:opent1_ts_shape_demo>
        )
    endif()
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
