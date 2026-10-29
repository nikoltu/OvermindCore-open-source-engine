set(OMC_PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/s2client-proto)
set(OMC_PROTO_OUT ${OMC_GENERATED_DIR})
file(MAKE_DIRECTORY ${OMC_PROTO_OUT}/s2clientprotocol)

set(OMC_PROTO_FILES common data debug error query raw sc2api score spatial ui)
set(OMC_PROTO_SRCS "")
set(OMC_PROTO_INPUTS "")
set(OMC_PROTO_DEPS "")
foreach(p ${OMC_PROTO_FILES})
  list(APPEND OMC_PROTO_SRCS ${OMC_PROTO_OUT}/s2clientprotocol/${p}.pb.cc)
  list(APPEND OMC_PROTO_INPUTS s2clientprotocol/${p}.proto)
  list(APPEND OMC_PROTO_DEPS ${OMC_PROTO_DIR}/s2clientprotocol/${p}.proto)
endforeach()

add_custom_command(
  OUTPUT ${OMC_PROTO_SRCS}
  COMMAND ${OMC_PROTOC} --proto_path=${OMC_PROTO_DIR} --cpp_out=${OMC_PROTO_OUT} ${OMC_PROTO_INPUTS}
  DEPENDS ${OMC_PROTO_DEPS}
  COMMENT "protoc s2clientprotocol"
  VERBATIM)

add_library(omc_proto STATIC ${OMC_PROTO_SRCS})
add_library(omc::proto ALIAS omc_proto)
target_include_directories(omc_proto SYSTEM PUBLIC ${OMC_PROTO_OUT} ${OMC_PROTOBUF_INCLUDE_DIRS})
target_link_directories(omc_proto PUBLIC ${OMC_PROTOBUF_STATIC_LIBRARY_DIRS})
target_link_libraries(omc_proto PUBLIC ${OMC_PROTOBUF_STATIC_LIBRARIES})
