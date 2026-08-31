find_program(SC2_PROTOC NAMES protoc)
if(NOT SC2_PROTOC)
  message(FATAL_ERROR
    "protoc was not found on PATH, and it is a HARD build requirement (ADR-0006).\n"
    "  What needs it: third_party/s2client-proto/*.proto are compiled to C++ at build time, into\n"
    "                 ${CMAKE_BINARY_DIR}/generated/. The generated sources are deliberately NOT\n"
    "                 committed, so a checkout without protoc cannot build crafter-sc's API server.\n"
    "  Fix on this toolchain (MSYS2 UCRT64):\n"
    "      pacman -S mingw-w64-ucrt-x86_64-protobuf\n"
    "      export PATH=\"/c/msys64/ucrt64/bin:$PATH\"\n"
    "  Then re-run: cmake -S . -B build -G Ninja")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SC2_PROTOBUF QUIET protobuf)
if(NOT SC2_PROTOBUF_FOUND)
  message(FATAL_ERROR
    "the protobuf RUNTIME (libprotobuf) was not found by pkg-config, and it is a HARD build\n"
    "requirement (ADR-0006): the generated bindings link against it.\n"
    "  Fix on this toolchain (MSYS2 UCRT64):\n"
    "      pacman -S mingw-w64-ucrt-x86_64-protobuf\n"
    "  Then re-run: cmake -S . -B build -G Ninja")
endif()

set(SC2_PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/s2client-proto)
set(SC2_PROTO_OUT ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${SC2_PROTO_OUT}/s2clientprotocol)

set(SC2_PROTO_FILES common data debug error query raw sc2api score spatial ui)
set(SC2_PROTO_SRCS "")
foreach(p ${SC2_PROTO_FILES})
  list(APPEND SC2_PROTO_SRCS ${SC2_PROTO_OUT}/s2clientprotocol/${p}.pb.cc)
endforeach()

add_custom_command(
  OUTPUT ${SC2_PROTO_SRCS}
  COMMAND ${SC2_PROTOC} --proto_path=${SC2_PROTO_DIR} --cpp_out=${SC2_PROTO_OUT}
          s2clientprotocol/common.proto s2clientprotocol/data.proto
          s2clientprotocol/debug.proto s2clientprotocol/error.proto
          s2clientprotocol/query.proto s2clientprotocol/raw.proto
          s2clientprotocol/sc2api.proto s2clientprotocol/score.proto
          s2clientprotocol/spatial.proto s2clientprotocol/ui.proto
  DEPENDS ${SC2_PROTO_DIR}/s2clientprotocol/common.proto
          ${SC2_PROTO_DIR}/s2clientprotocol/data.proto
          ${SC2_PROTO_DIR}/s2clientprotocol/debug.proto
          ${SC2_PROTO_DIR}/s2clientprotocol/error.proto
          ${SC2_PROTO_DIR}/s2clientprotocol/query.proto
          ${SC2_PROTO_DIR}/s2clientprotocol/raw.proto
          ${SC2_PROTO_DIR}/s2clientprotocol/sc2api.proto
          ${SC2_PROTO_DIR}/s2clientprotocol/score.proto
          ${SC2_PROTO_DIR}/s2clientprotocol/spatial.proto
          ${SC2_PROTO_DIR}/s2clientprotocol/ui.proto
  COMMENT "protoc s2clientprotocol (vendored schema -> build/generated)"
  VERBATIM)

add_library(sc2proto STATIC ${SC2_PROTO_SRCS})
target_include_directories(sc2proto SYSTEM PUBLIC ${SC2_PROTO_OUT} ${SC2_PROTOBUF_INCLUDE_DIRS})
target_link_directories(sc2proto PUBLIC ${SC2_PROTOBUF_STATIC_LIBRARY_DIRS})
target_link_libraries(sc2proto PUBLIC ${SC2_PROTOBUF_STATIC_LIBRARIES})
