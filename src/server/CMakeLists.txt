#
# gRPC Server: exe-target = server
#

# Static library containing the service implementation.
# Linked by both the server binary and unit tests.
add_library(remsvc_impl STATIC
  RemSvcServiceImpl.cc
  AuthInterceptor.cc
)
target_include_directories(remsvc_impl PUBLIC .)
target_link_libraries(remsvc_impl PUBLIC
  RemSvc
  common
  gRPC::grpc
  gRPC::grpc++
  gRPC::grpc++_reflection
  protobuf::libprotobuf
  Qt6::Core
)

add_executable(server)
target_sources(server
  PRIVATE
  GrpcServerThread.cc
  RemSvcServer.cc
  ServerConfig.cc
  SignalHandler.cc
)
set_target_properties(server PROPERTIES
  OUTPUT_NAME "RemSvc_server"
  AUTOMOC ON
)

target_include_directories(server PUBLIC .)

target_link_libraries(server PUBLIC remsvc_impl
  absl::check
  absl::flags
  absl::flags_parse
  absl::log
  Qt6::Core
)

install(TARGETS server
    RUNTIME DESTINATION bin
    COMPONENT Runtime
)
