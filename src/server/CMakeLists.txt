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

# ── Shared library deployment ──────────────────────────────────────────────
# On Linux, collect all non-system shared library dependencies (gRPC, protobuf,
# Qt6, absl, ICU, …) into a self-contained deploy/ directory using ldd.
# A generated launcher script sets LD_LIBRARY_PATH before exec-ing the binary.
# Falls back to a bare binary install on non-Linux platforms.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_deploy_dir ${CMAKE_BINARY_DIR}/deploy/RemSvc_server)
    set(_deploy_script ${CMAKE_SOURCE_DIR}/cmake/DeployLinux.cmake)

    add_custom_command(TARGET server POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${_deploy_dir}
        COMMAND ${CMAKE_COMMAND}
                -DBINARY=$<TARGET_FILE:server>
                -DDEPLOY_DIR=${_deploy_dir}
                -P ${_deploy_script}
        COMMENT "Collecting all shared library dependencies for RemSvc_server"
        VERBATIM
    )

    # Install deployed layout:
    #   bin/RemSvc_server      — binary
    #   lib/lib*.so.*          — all bundled shared libs
    #   bin/RemSvc_server.sh   — launcher (sets LD_LIBRARY_PATH)
    install(DIRECTORY ${_deploy_dir}/bin/
        DESTINATION bin
        COMPONENT Runtime
        USE_SOURCE_PERMISSIONS
    )
    install(DIRECTORY ${_deploy_dir}/lib/
        DESTINATION lib
        COMPONENT Runtime
    )
    install(PROGRAMS ${_deploy_dir}/RemSvc_server.sh
        DESTINATION bin
        COMPONENT Runtime
    )
else()
    install(TARGETS server
        RUNTIME DESTINATION bin
        COMPONENT Runtime
    )
endif()
