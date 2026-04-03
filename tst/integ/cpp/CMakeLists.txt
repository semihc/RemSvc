# Integration tests — C++ (GTest)
# Spins up a real in-process gRPC server (GrpcServerThread) and exercises
# the full client↔server round-trip over a localhost connection.

find_package(GTest CONFIG REQUIRED)

add_executable(integ_grpc GrpcRoundTripTest.cc)

# GrpcServerThread.cc is compiled directly into this target (no separate lib).
target_sources(integ_grpc PRIVATE
    ${CMAKE_SOURCE_DIR}/src/server/GrpcServerThread.cc
)
set_target_properties(integ_grpc PROPERTIES AUTOMOC ON)

target_include_directories(integ_grpc PRIVATE
    ${CMAKE_SOURCE_DIR}/src/server
    ${CMAKE_SOURCE_DIR}/src/client
)

target_link_libraries(integ_grpc PRIVATE
    GTest::gtest_main
    remsvc_impl
    remsvc_client_lib
    absl::strings
    gRPC::grpc++_reflection
    Qt6::Core
)

add_test(NAME integ_grpc COMMAND integ_grpc)

set_tests_properties(integ_grpc
    PROPERTIES LABELS  "integ"
               DEPENDS "unit_common;unit_server;unit_client")
