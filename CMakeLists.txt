cmake_minimum_required(VERSION 3.16)
project(sc2engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)

if(MINGW)
  add_link_options(-static -static-libgcc -static-libstdc++)
endif()

add_library(sc2core STATIC
  src/core/inflate.cpp
  src/core/xml.cpp
  src/formats/m3.cpp
  src/formats/dds.cpp
  src/formats/mpq.cpp
  src/formats/sc2map.cpp
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
  src/ai/macro.cpp
  src/ai/melee_ai.cpp
  src/app/spawn.cpp
  src/app/install.cpp
  src/app/boot.cpp
  src/app/melee_commands.cpp
  src/app/match_result.cpp
  src/app/replay.cpp
  src/app/control.cpp
  src/app/game.cpp
  src/app/melee.cpp
  src/app/melee_game.cpp
  src/app/sandbox_game.cpp
  src/app/front_end.cpp
  src/app/hud.cpp
  src/app/hud_render.cpp
  src/app/menu.cpp
  src/app/control_groups.cpp
  src/render/anim.cpp
  src/render/actor.cpp
  src/render/actor_rules.cpp
  src/formats/png.cpp
  src/asset/manager.cpp
  src/app/melee_models.cpp
  src/ui/layout.cpp
  src/ui/interact.cpp
  src/audio/mixer.cpp
  src/platform/file_desktop.cpp
)
target_include_directories(sc2core PUBLIC include)

add_executable(sc2casc tools/sc2casc.cpp)
target_link_libraries(sc2casc PRIVATE sc2core)

find_package(Vulkan COMPONENTS glslc QUIET)
if(Vulkan_FOUND AND Vulkan_GLSLC_EXECUTABLE)
  set(SHADER_OUT ${CMAKE_BINARY_DIR}/shaders)
  file(MAKE_DIRECTORY ${SHADER_OUT})
  foreach(shader mesh.vert mesh.frag particle.vert particle.frag overlay.vert overlay.frag)
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
    src/render/vulkan_offscreen.cpp
    src/render/window.cpp
    src/render/game_window.cpp
    src/render/overlay.cpp
    src/render/overlay_pass.cpp
    src/render/terrain_tex.cpp
    ${SHADER_OUT}/mesh.vert.spv.h
    ${SHADER_OUT}/mesh.frag.spv.h
    ${SHADER_OUT}/particle.vert.spv.h
    ${SHADER_OUT}/particle.frag.spv.h
    ${SHADER_OUT}/overlay.vert.spv.h
    ${SHADER_OUT}/overlay.frag.spv.h)
  target_include_directories(sc2render PUBLIC include PRIVATE ${SHADER_OUT})
  target_link_libraries(sc2render PUBLIC sc2core Vulkan::Vulkan)
  target_compile_options(sc2render PRIVATE -Wno-missing-field-initializers)

  add_executable(sc2view tools/sc2view.cpp)
  target_link_libraries(sc2view PRIVATE sc2render sc2core)

  add_executable(sc2scene tools/sc2scene.cpp)
  target_link_libraries(sc2scene PRIVATE sc2render sc2core)

  add_executable(sc2window tools/sc2window.cpp)
  target_link_libraries(sc2window PRIVATE sc2render sc2core)

  add_executable(sc2game tools/sc2game.cpp)
  target_link_libraries(sc2game PRIVATE sc2render sc2core)

  add_executable(sc2melee tools/sc2melee.cpp)
  target_link_libraries(sc2melee PRIVATE sc2render sc2core)
  set(SC2_HAVE_VULKAN ON)
endif()

enable_testing()
add_subdirectory(tests)
