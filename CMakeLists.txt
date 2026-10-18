cmake_minimum_required(VERSION 3.16)
project(sc2engine VERSION 0.0.1 LANGUAGES CXX)

set(SC2_ENGINE_BUILD "617")
set(SC2_ENGINE_STAGE "pre-alfa")
configure_file(include/sc2/core/version.hpp.in
               ${CMAKE_BINARY_DIR}/generated/sc2/core/version.hpp @ONLY)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)

option(SC2_WARNINGS_AS_ERRORS
       "Treat every -Wall -Wextra warning as an error (R91's regression gate)" ON)
if(SC2_WARNINGS_AS_ERRORS)
  add_compile_options(-Werror)
endif()

if(MINGW)
  add_link_options(-static -static-libgcc -static-libstdc++)
endif()

set(EMBED_OUT ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${EMBED_OUT})
foreach(pair "emblem.dds:kEmblemDds" "emblem_sm.dds:kEmblemSmDds")
  string(REPLACE ":" ";" pair_list "${pair}")
  list(GET pair_list 0 embed_file)
  list(GET pair_list 1 embed_name)
  add_custom_command(
    OUTPUT ${EMBED_OUT}/${embed_file}.h
    COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_SOURCE_DIR}/assets/${embed_file} -DOUT=${EMBED_OUT}/${embed_file}.h -DNAME=${embed_name}
            -P ${CMAKE_SOURCE_DIR}/cmake/bin2h.cmake
    DEPENDS ${CMAKE_SOURCE_DIR}/assets/${embed_file} ${CMAKE_SOURCE_DIR}/cmake/bin2h.cmake
    COMMENT "bin2h ${embed_file}"
    VERBATIM)
endforeach()

add_library(sc2core STATIC
    ${EMBED_OUT}/emblem.dds.h
    ${EMBED_OUT}/emblem_sm.dds.h
    src/app/shell_emblem.cpp
    src/app/shell_ui.cpp
    src/app/shell_widgets.cpp
    src/app/shell_chrome.cpp
    src/app/shell_screens_home.cpp
    src/app/shell_screens_setup.cpp
    src/app/shell_screens_loading.cpp
    src/app/shell_screens_result.cpp
    src/app/shell_screens_settings.cpp
    src/app/shell_screens_profile.cpp
    src/app/shell_screens_replays.cpp
    src/app/shell_screens_about.cpp
    src/app/shell_screens_widgets.cpp
    src/app/portrait_catalog.cpp
  src/core/inflate.cpp
  src/core/xml.cpp
  src/formats/m3.cpp
  src/formats/dds.cpp
  src/formats/mpq.cpp
  src/formats/replay_events.cpp
  src/formats/sc2map.cpp
  src/formats/tga.cpp
  src/formats/terrain.cpp
  src/formats/terrain_grid.cpp
  src/render/fog.cpp
  src/render/unit_overlay.cpp
  src/formats/minimap.cpp
  src/app/synth_map.cpp
  src/app/test_map.cpp
  src/casc/idx.cpp
  src/casc/blte.cpp
  src/casc/encoding.cpp
  src/casc/mndx.cpp
  src/casc/storage.cpp
  src/data/catalog.cpp
  src/data/requirements.cpp
  src/data/production.cpp
  src/data/environment.cpp
  src/data/economy.cpp
  src/data/veterancy.cpp
  src/data/upgrades.cpp
  src/data/actor_model.cpp
  src/data/actor_catalog.cpp
  src/data/strings.cpp
  src/data/audio.cpp
  src/data/sound_events.cpp
  src/sim/world.cpp
  src/sim/vision.cpp
  src/sim/detection.cpp
  src/sim/trigger.cpp
  src/campaign/flow.cpp
  src/campaign/bank.cpp
  src/campaign/mission_load.cpp
  src/campaign/triggers.cpp
  src/campaign/trigger_vm.cpp
  src/campaign/mission_runtime.cpp
  src/ai/melee_ai.cpp
  src/app/spawn.cpp
  src/app/ability.cpp
  src/app/install.cpp
  src/app/boot.cpp
  src/app/melee_commands.cpp
  src/app/match_result.cpp
  src/app/replay.cpp
  src/app/replay_orders.cpp
  src/app/control.cpp
  src/app/melee.cpp
  src/app/melee_game.cpp
  src/app/sandbox_game.cpp
  src/app/front_end.cpp
  src/app/shipping_wiring.cpp
  src/app/hud.cpp
  src/app/hud_render.cpp
  src/app/hud_layout.cpp
  src/app/hud_buttons.cpp
  src/app/hud_menu_ui.cpp
  src/app/hud_minimap_ui.cpp
  src/app/hud_selection_ui.cpp
  src/app/hud_topbar_ui.cpp
  src/app/hud_chat_ui.cpp
  src/app/ui_layout_source.cpp
  src/app/console_panel.cpp
  src/app/menu.cpp
  src/app/glue_screen.cpp
  src/app/screen_router.cpp
  src/app/control_groups.cpp
  src/render/anim.cpp
  src/render/actor.cpp
  src/render/creep_layer.cpp
  src/render/actor_rules.cpp
  src/render/font_ttf.cpp
  src/render/font_cff.cpp
  src/formats/png.cpp
  src/asset/manager.cpp
  src/app/melee_models.cpp
  src/ui/layout.cpp
  src/ui/interact.cpp
  src/audio/mixer.cpp
  src/audio/ogg.cpp
  src/audio/vorbis.cpp
  src/audio/wasapi.cpp
  src/platform/file_desktop.cpp
)
target_include_directories(sc2core PUBLIC include ${CMAKE_BINARY_DIR}/generated)
if(WIN32)
  target_link_libraries(sc2core PUBLIC ole32)
endif()

add_executable(sc2casc tools/sc2casc.cpp)
target_link_libraries(sc2casc PRIVATE sc2core)

add_executable(sc2probe tools/sc2probe.cpp)
target_link_libraries(sc2probe PRIVATE sc2core)

add_executable(sc2sepsim tools/sc2sepsim.cpp)
target_link_libraries(sc2sepsim PRIVATE sc2core)

add_executable(sc2navmap tools/sc2navmap.cpp)
target_link_libraries(sc2navmap PRIVATE sc2core)

add_executable(sc2oracle_dump tools/sc2oracle_dump.cpp)
target_link_libraries(sc2oracle_dump PRIVATE sc2core)

add_executable(sc2sound tools/sc2sound.cpp)
target_link_libraries(sc2sound PRIVATE sc2core)

find_package(Vulkan COMPONENTS glslc QUIET)
if(Vulkan_FOUND AND Vulkan_GLSLC_EXECUTABLE)
  set(SHADER_OUT ${CMAKE_BINARY_DIR}/shaders)
  file(MAKE_DIRECTORY ${SHADER_OUT})
  foreach(shader mesh.vert mesh.frag scene.vert scene.frag terrain_splat.frag shadow.vert shadow.frag particle.vert particle.frag overlay.vert overlay.frag tonemap.frag ssao.frag bloom.frag)
    add_custom_command(
      OUTPUT ${SHADER_OUT}/${shader}.spv.h
      COMMAND ${Vulkan_GLSLC_EXECUTABLE} ${CMAKE_SOURCE_DIR}/shaders/${shader}
              -mfmt=c -o ${SHADER_OUT}/${shader}.spv.h
      DEPENDS ${CMAKE_SOURCE_DIR}/shaders/${shader}
      COMMENT "glslc ${shader}"
      VERBATIM)
  endforeach()
  add_library(sc2render STATIC
    src/render/vulkan_scene.cpp
    src/render/scene_renderer.cpp
    src/render/vulkan_offscreen.cpp
    src/render/window.cpp
    src/render/game_window.cpp
    src/render/overlay.cpp
    src/render/overlay_pass.cpp
    src/render/terrain_tex.cpp
    ${SHADER_OUT}/mesh.vert.spv.h
    ${SHADER_OUT}/mesh.frag.spv.h
    ${SHADER_OUT}/scene.vert.spv.h
    ${SHADER_OUT}/scene.frag.spv.h
    ${SHADER_OUT}/terrain_splat.frag.spv.h
    ${SHADER_OUT}/shadow.vert.spv.h
    ${SHADER_OUT}/shadow.frag.spv.h
    ${SHADER_OUT}/particle.vert.spv.h
    ${SHADER_OUT}/particle.frag.spv.h
    ${SHADER_OUT}/overlay.vert.spv.h
    ${SHADER_OUT}/overlay.frag.spv.h
    ${SHADER_OUT}/tonemap.frag.spv.h
    ${SHADER_OUT}/ssao.frag.spv.h
    ${SHADER_OUT}/bloom.frag.spv.h)
  target_include_directories(sc2render PUBLIC include PRIVATE ${SHADER_OUT} src/render)
  target_link_libraries(sc2render PUBLIC sc2core Vulkan::Vulkan)
  add_executable(sc2view tools/sc2view.cpp)
  target_link_libraries(sc2view PRIVATE sc2render sc2core)

  add_executable(sc2scene tools/sc2scene.cpp)
  target_link_libraries(sc2scene PRIVATE sc2render sc2core)

  add_executable(sc2mapframe tools/sc2mapframe.cpp)
  target_link_libraries(sc2mapframe PRIVATE sc2render sc2core)

  add_executable(sc2console tools/sc2console.cpp)
  target_link_libraries(sc2console PRIVATE sc2render sc2core)

  add_executable(sc2cliffframe tools/sc2cliffframe.cpp)
  target_link_libraries(sc2cliffframe PRIVATE sc2render sc2core)

  add_executable(sc2campaign tools/sc2campaign.cpp)
  target_link_libraries(sc2campaign PRIVATE sc2render sc2core)

  add_executable(sc2render_frames tools/sc2render_frames.cpp)
  target_link_libraries(sc2render_frames PRIVATE sc2render sc2core)

  add_executable(sc2window tools/sc2window.cpp)
  target_link_libraries(sc2window PRIVATE sc2render sc2core)

  add_executable(sc2melee tools/sc2melee.cpp)
  target_link_libraries(sc2melee PRIVATE sc2render sc2core)

  set(CRAFTER_SC_SOURCES tools/crafter_sc.cpp src/app/platform_win32.cpp)
  if(EXISTS "${CMAKE_SOURCE_DIR}/assets/app_icon.ico")
    enable_language(RC)
    list(APPEND CRAFTER_SC_SOURCES assets/crafter_sc.rc)
    message(STATUS "crafter-sc: embedding app icon from assets/app_icon.ico")
  endif()
  add_executable(crafter-sc ${CRAFTER_SC_SOURCES})
  target_link_libraries(crafter-sc PRIVATE sc2render sc2core ole32 shell32)
  target_link_options(crafter-sc PRIVATE -mwindows)
  set_target_properties(crafter-sc PROPERTIES
    OUTPUT_NAME "crafter-sc_v${PROJECT_VERSION}.${SC2_ENGINE_BUILD}-${SC2_ENGINE_STAGE}")
  add_custom_command(TARGET crafter-sc POST_BUILD
    COMMAND ${CMAKE_COMMAND}
            -DKEEP=$<TARGET_FILE_NAME:crafter-sc>
            -DDIR=$<TARGET_FILE_DIR:crafter-sc>
            -P ${CMAKE_SOURCE_DIR}/cmake/prune_stale_clients.cmake
    VERBATIM)
  set(SC2_HAVE_VULKAN ON)
endif()

add_subdirectory(third_party)
add_library(sc2api STATIC
  src/api/transport/ws.cpp
  src/api/server.cpp
  src/api/dispatch.cpp
  src/api/truth_pack.cpp
  src/api/bridge/game_info.cpp
  src/api/bridge/observation.cpp
  src/api/bridge/orders.cpp
  src/api/bridge/data.cpp
)
target_include_directories(sc2api PUBLIC include ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(sc2api PUBLIC sc2core sc2proto)
if(TARGET crafter-sc)
  target_link_libraries(crafter-sc PRIVATE sc2api ws2_32)
endif()

enable_testing()
add_subdirectory(tests)
