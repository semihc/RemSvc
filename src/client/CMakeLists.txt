#
# gRPC Client
#

set(CLIENT_LIBS
  RemSvc
  common
  gRPC::grpc
  gRPC::grpc++
  gRPC::grpc++_reflection
  protobuf::libprotobuf
  absl::time
  CLI11::CLI11
  spdlog::spdlog
  Qt6::Core
)

# Static library (class only, no main) — linked by the client binary and unit tests.
add_library(remsvc_client_lib STATIC RemSvcClient.cc)
target_include_directories(remsvc_client_lib PUBLIC .)
target_link_libraries(remsvc_client_lib PUBLIC ${CLIENT_LIBS})

# CLI executable — links the library and adds the main entry point.
add_executable(client RemSvcMain.cc)
set_target_properties(client PROPERTIES OUTPUT_NAME "RemSvc_client")
target_link_libraries(client PRIVATE remsvc_client_lib)

install(TARGETS client
    RUNTIME DESTINATION bin
    COMPONENT Runtime
)
