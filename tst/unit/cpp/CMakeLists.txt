# Unit tests — C++ (GTest)
find_package(GTest CONFIG REQUIRED)

# common utilities (Assert, CLI, Log)
add_executable(unit_common CommonTest.cc)
target_link_libraries(unit_common PRIVATE GTest::gtest_main common)
add_test(NAME unit_common COMMAND unit_common)

# RemSvcServiceImpl (injected CmdRunner — no real process spawned)
add_executable(unit_server RemSvcServiceImplTest.cc)
target_link_libraries(unit_server PRIVATE GTest::gtest_main remsvc_impl)
add_test(NAME unit_server COMMAND unit_server)

# RemSvcClient (injected IRemSvcStub fake — no real gRPC connection)
add_executable(unit_client RemSvcClientTest.cc)
target_link_libraries(unit_client PRIVATE GTest::gtest_main remsvc_client_lib)
add_test(NAME unit_client COMMAND unit_client)

set_tests_properties(unit_common unit_server unit_client
    PROPERTIES LABELS "unit")
