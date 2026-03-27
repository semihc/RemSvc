#
# gRPC Client
#

add_executable(client)

target_sources(client
  PRIVATE
  RemSvcClient.cc
)
set_target_properties(client PROPERTIES
    OUTPUT_NAME "RemSvc_client"
)
target_include_directories(client PUBLIC .)

target_link_libraries(client PUBLIC RemSvc
  common
  gRPC::grpc
  gRPC::grpc++
  gRPC::grpc++_reflection
  protobuf::libprotobuf
  absl::flags
  absl::flags_parse
  CLI11::CLI11
  spdlog::spdlog
  Qt6::Core
)
