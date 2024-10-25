#   BAREOS® - Backup Archiving REcovery Open Sourced
#
#   Copyright (C) 2024-2024 Bareos GmbH & Co. KG
#
#   This program is Free Software; you can redistribute it and/or
#   modify it under the terms of version three of the GNU Affero General Public
#   License as published by the Free Software Foundation and included
#   in the file LICENSE.
#
#   This program is distributed in the hope that it will be useful, but
#   WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
#   Affero General Public License for more details.
#
#   You should have received a copy of the GNU Affero General Public License
#   along with this program; if not, write to the Free Software
#   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
#   02110-1301, USA.

include(BareosDisableWarnings)
include(BareosConfigureFile)

if(HAVE_WIN32)
  message(STATUS "Disabling grpc plugin as its not supported on windows")
elseif(HAVE_GRPC)
  message(STATUS "Using protobuf ${Protobuf_VERSION}")
  message(STATUS "Using gRPC ${gRPC_VERSION}")

  set(_PROTOBUF_LIBPROTOBUF protobuf::libprotobuf)
  set(_REFLECTION gRPC::grpc++_reflection)
  if(CMAKE_CROSSCOMPILING)
    find_program(_PROTOBUF_PROTOC protoc)
  else()
    set(_PROTOBUF_PROTOC $<TARGET_FILE:protobuf::protoc>)
  endif()

  set(_GRPC_GRPCPP gRPC::grpc++)
  if(CMAKE_CROSSCOMPILING)
    find_program(_GRPC_CPP_PLUGIN_EXECUTABLE grpc_cpp_plugin)
  else()
    set(_GRPC_CPP_PLUGIN_EXECUTABLE $<TARGET_FILE:gRPC::grpc_cpp_plugin>)
  endif()

  # Proto file
  macro(addgrpcprotofiles protosrcs protohdrs grpcsrcs grpchdrs)
    foreach(file ${ARGN})
      get_filename_component(absolute "${file}" ABSOLUTE)
      get_filename_component(path "${absolute}" PATH)
      get_filename_component(stem "${file}" NAME_WE)

      set(proto_src "${CMAKE_CURRENT_BINARY_DIR}/${stem}.pb.cc")
      set(proto_hdr "${CMAKE_CURRENT_BINARY_DIR}/${stem}.pb.h")
      set(grpc_src "${CMAKE_CURRENT_BINARY_DIR}/${stem}.grpc.pb.cc")
      set(grpc_hdr "${CMAKE_CURRENT_BINARY_DIR}/${stem}.grpc.pb.h")

      add_custom_command(
        OUTPUT "${proto_src}" "${proto_hdr}" "${grpc_src}" "${grpc_hdr}"
        COMMAND
          ${_PROTOBUF_PROTOC} ARGS --grpc_out "${CMAKE_CURRENT_BINARY_DIR}"
          --cpp_out "${CMAKE_CURRENT_BINARY_DIR}" -I "${path}"
          --plugin=protoc-gen-grpc="${_GRPC_CPP_PLUGIN_EXECUTABLE}"
          "${absolute}"
        DEPENDS "${absolute}"
      )

      list(APPEND protosrcs "${proto_src}")
      list(APPEND protohdrs "${proto_hdr}")
      list(APPEND grpcsrcs "${grpc_src}")
      list(APPEND grpchdrs "${grpc_hdr}")
    endforeach()
  endmacro()

  addgrpcprotofiles(
    protosrcs protohdrs grpcsrcs grpchdrs "proto/plugin.proto"
    "proto/events.proto" "proto/bareos.proto" "proto/common.proto"
  )

  add_library(grpc-proto ${protosrcs} ${protohdrs} ${grpcsrcs} ${grpchdrs})
  target_include_directories(grpc-proto PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")

  target_link_libraries(
    grpc-proto PUBLIC ${_REFLECTION} ${_GRPC_GRPCPP} ${_PROTOBUF_LIBPROTOBUF}
  )
  # add_library(grpc-plugin SHARED grpc.cc restore.cc config.cc database.cc) #
  # Include generated *.pb.h files target_link_libraries( grpc-plugin
  # ${_REFLECTION} ${_GRPC_GRPCPP} ${_PROTOBUF_LIBPROTOBUF} grpc-proto )
  #
  # add_executable(grpc-client gclient.cc) target_link_libraries(grpc-client
  # grpc-proto)

  add_library(grpc-fd MODULE grpc.cc grpc_impl.cc bareos_api.cc)
  set_target_properties(
    grpc-fd PROPERTIES PREFIX "" LIBRARY_OUTPUT_DIRECTORY ..
  )
  install(
    TARGETS grpc-fd
    DESTINATION ${plugindir}
    COMPONENT filedaemon
  )
  target_link_libraries(grpc-fd bareos fmt grpc-proto)

  add_executable(grpc-test-module grpc_test_module.cc plugin_service.cc)
  target_link_libraries(grpc-test-module fmt grpc-proto)
  install(
    TARGETS grpc-test-module
    DESTINATION ${plugindir}
    COMPONENT filedaemon
  )

  add_executable(
    grpc-python-module grpc_python_module.cc plugin_service_python.cc
  )
  target_link_libraries(grpc-python-module fmt grpc-proto ${Python3_LIBRARIES})
  target_include_directories(grpc-python-module PUBLIC ${Python3_INCLUDE_DIRS})
  install(
    TARGETS grpc-python-module
    DESTINATION ${plugindir}
    COMPONENT filedaemon
  )

  set_target_properties(
    grpc-fd PROPERTIES PREFIX "" LIBRARY_OUTPUT_DIRECTORY ..
  )

  include(DebugEdit)
endif()
