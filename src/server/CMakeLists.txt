#
# gRPC Server: exe-target = server
# HTTP/REST Server: exe-target = restserver
#

add_executable(server)
target_sources(server
  PRIVATE
  RemSvcServer.cc
  SignalHandler.cc
)
set_target_properties(server PROPERTIES
  OUTPUT_NAME "RemSvc_server"
  AUTOMOC ON
)

target_include_directories(server PUBLIC .)

target_link_libraries(server PUBLIC RemSvc
  common
  gRPC::grpc
  gRPC::grpc++
  gRPC::grpc++_reflection
  protobuf::libprotobuf
  absl::check
  absl::flags
  absl::flags_parse
  absl::log
  Qt6::Core
)



add_executable(restserver)
target_sources(restserver
  PRIVATE
  RestServer.cc
  SignalHandler.cc
)
set_target_properties(restserver PROPERTIES
  OUTPUT_NAME "Rest_server"
  AUTOMOC ON
)

target_link_libraries(restserver PUBLIC RemSvc
  common
  Qt6::Core
  Qt6::HttpServer
)
