cmake_minimum_required(VERSION 3.16)
project(overmindcore VERSION 0.1.9 LANGUAGES CXX)

set(OVERMINDCORE_STAGE "alpha")
set(OMC_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
configure_file(include/omc/core/version.hpp.in
               ${OMC_GENERATED_DIR}/omc/core/version.hpp @ONLY)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)

option(OMC_WARNINGS_AS_ERRORS
       "Treat every -Wall -Wextra warning as an error" ON)
if(OMC_WARNINGS_AS_ERRORS)
  add_compile_options(-Werror)
endif()

if(MINGW)
  add_link_options(-static -static-libgcc -static-libstdc++)
endif()

find_program(OMC_PROTOC NAMES protoc)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(OMC_PROTOBUF QUIET protobuf)
endif()
if(OMC_PROTOC AND OMC_PROTOBUF_FOUND)
  set(OMC_API_DEFAULT ON)
else()
  set(OMC_API_DEFAULT OFF)
endif()
option(OMC_API "Build the API protocol server into the client (needs protoc and libprotobuf)" ${OMC_API_DEFAULT})
if(OMC_API AND NOT (OMC_PROTOC AND OMC_PROTOBUF_FOUND))
  message(FATAL_ERROR
    "OMC_API=ON needs protoc on PATH and libprotobuf visible to pkg-config.\n"
    "  MSYS2 UCRT64: pacman -S mingw-w64-ucrt-x86_64-protobuf\n"
    "  Or configure with -DOMC_API=OFF to build the client without the API server.")
endif()
message(STATUS "OvermindCore API protocol server: ${OMC_API}")

set(EMBED_OUT ${OMC_GENERATED_DIR})
file(MAKE_DIRECTORY ${EMBED_OUT})
foreach(pair "emblem.dds:kEmblemDds" "emblem_sm.dds:kEmblemSmDds")
  string(REPLACE ":" ";" pair_list "${pair}")
  list(GET pair_list 0 embed_file)
  list(GET pair_list 1 embed_name)
  add_custom_command(
    OUTPUT ${EMBED_OUT}/${embed_file}.h
    COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_CURRENT_SOURCE_DIR}/assets/${embed_file} -DOUT=${EMBED_OUT}/${embed_file}.h -DNAME=${embed_name}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/bin2h.cmake
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/${embed_file} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/bin2h.cmake
    COMMENT "bin2h ${embed_file}"
    VERBATIM)
endforeach()

add_library(omc_core STATIC
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
  src/formats/map_file.cpp
  src/formats/tga.cpp
  src/formats/terrain.cpp
  src/formats/terrain_grid.cpp
  src/render/fog.cpp
  src/render/unit_overlay.cpp
  src/formats/minimap.cpp
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
  src/ai/hd_builds.cpp
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
  src/app/order_cues.cpp
  src/app/effect_actor_host.cpp
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
  src/render/cloak_look.cpp
  src/render/texture_decode.cpp
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
add_library(omc::core ALIAS omc_core)
target_include_directories(omc_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include ${OMC_GENERATED_DIR})
target_compile_features(omc_core PUBLIC cxx_std_17)
if(WIN32)
  target_link_libraries(omc_core PUBLIC ole32)
endif()

find_package(Vulkan COMPONENTS glslc QUIET)
if(Vulkan_FOUND AND Vulkan_GLSLC_EXECUTABLE)
  set(SHADER_OUT ${CMAKE_CURRENT_BINARY_DIR}/shaders)
  file(MAKE_DIRECTORY ${SHADER_OUT})
  set(OMC_SHADER_HEADERS "")
  foreach(shader mesh.vert mesh.frag scene.vert scene.frag terrain_splat.frag shadow.vert shadow.frag particle.vert particle.frag overlay.vert overlay.frag tonemap.frag ssao.frag bloom.frag)
    add_custom_command(
      OUTPUT ${SHADER_OUT}/${shader}.spv.h
      COMMAND ${Vulkan_GLSLC_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}
              -mfmt=c -o ${SHADER_OUT}/${shader}.spv.h
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}
      COMMENT "glslc ${shader}"
      VERBATIM)
    list(APPEND OMC_SHADER_HEADERS ${SHADER_OUT}/${shader}.spv.h)
  endforeach()
  foreach(tq 1 2)
    add_custom_command(
      OUTPUT ${SHADER_OUT}/terrain_splat_q${tq}.frag.spv.h
      COMMAND ${Vulkan_GLSLC_EXECUTABLE} -DTQ=${tq} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/terrain_splat.frag
              -mfmt=c -o ${SHADER_OUT}/terrain_splat_q${tq}.frag.spv.h
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/terrain_splat.frag
      COMMENT "glslc terrain_splat.frag TQ=${tq}"
      VERBATIM)
    list(APPEND OMC_SHADER_HEADERS ${SHADER_OUT}/terrain_splat_q${tq}.frag.spv.h)
  endforeach()
  add_library(omc_render STATIC
    src/render/vulkan_scene.cpp
    src/render/scene_renderer.cpp
    src/render/vulkan_offscreen.cpp
    src/render/window.cpp
    src/render/game_window.cpp
    src/render/game_offscreen.cpp
    src/render/overlay.cpp
    src/render/overlay_pass.cpp
    src/render/terrain_tex.cpp
    ${OMC_SHADER_HEADERS})
  add_library(omc::render ALIAS omc_render)
  target_include_directories(omc_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include PRIVATE ${SHADER_OUT} ${CMAKE_CURRENT_SOURCE_DIR}/src/render)
  target_link_libraries(omc_render PUBLIC omc_core Vulkan::Vulkan)
  set(OMC_HAVE_VULKAN ON)
endif()

if(OMC_API)
  add_subdirectory(third_party)
  add_library(omc_api STATIC
    src/api/transport/ws.cpp
    src/api/server.cpp
    src/api/dispatch.cpp
    src/api/pack_readers.cpp
    src/api/bridge/game_info.cpp
    src/api/bridge/observation.cpp
    src/api/bridge/orders.cpp
    src/api/bridge/data.cpp
  )
  add_library(omc::api ALIAS omc_api)
  target_include_directories(omc_api PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include ${OMC_GENERATED_DIR})
  target_link_libraries(omc_api PUBLIC omc_core omc_proto)
  if(WIN32)
    target_link_libraries(omc_api PUBLIC ws2_32)
  endif()
endif()

if(OMC_HAVE_VULKAN)
  set(OVERMINDCORE_SOURCES tools/overmindcore.cpp src/app/platform_win32.cpp)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/overmindcore.ico")
    enable_language(RC)
    list(APPEND OVERMINDCORE_SOURCES assets/overmindcore.rc)
  endif()
  add_executable(overmindcore ${OVERMINDCORE_SOURCES})
  target_link_libraries(overmindcore PRIVATE omc_render omc_core ole32 shell32 ws2_32)
  target_link_options(overmindcore PRIVATE -mwindows)
  set_target_properties(overmindcore PROPERTIES
    OUTPUT_NAME "overmindcore_v${PROJECT_VERSION}-${OVERMINDCORE_STAGE}")
  if(OMC_API)
    target_compile_definitions(overmindcore PRIVATE OMC_HAVE_API=1)
    target_link_libraries(overmindcore PRIVATE omc_api)
  else()
    target_compile_definitions(overmindcore PRIVATE OMC_HAVE_API=0)
  endif()
endif()
