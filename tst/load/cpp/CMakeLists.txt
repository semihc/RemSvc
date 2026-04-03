# Load / stress tests — C++ (GTest)
# Measures RemCmd and RemCmdStrm throughput and latency percentiles over a
# real in-process gRPC connection.  Assertions use a conservative floor so
# the suite can run on CI without hardware-specific tuning.

find_package(GTest CONFIG REQUIRED)

add_executable(load_throughput ThroughputTest.cc)

target_sources(load_throughput PRIVATE
    ${CMAKE_SOURCE_DIR}/src/server/GrpcServerThread.cc
)
set_target_properties(load_throughput PROPERTIES AUTOMOC ON)

target_include_directories(load_throughput PRIVATE
    ${CMAKE_SOURCE_DIR}/src/server
    ${CMAKE_SOURCE_DIR}/src/client
)

target_link_libraries(load_throughput PRIVATE
    GTest::gtest_main
    remsvc_impl
    remsvc_client_lib
    absl::strings
    gRPC::grpc++_reflection
    Qt6::Core
)

add_test(NAME load_throughput COMMAND load_throughput)

set_tests_properties(load_throughput
    PROPERTIES LABELS  "load"
               DEPENDS "integ_grpc")
