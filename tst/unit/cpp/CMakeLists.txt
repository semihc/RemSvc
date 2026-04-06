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

# AuthInterceptor (BearerTokenAuthProcessor — no gRPC server started)
add_executable(unit_auth AuthInterceptorTest.cc)
target_link_libraries(unit_auth PRIVATE GTest::gtest_main remsvc_impl)
add_test(NAME unit_auth COMMAND unit_auth)

# ServerConfig (loadServerConfig writes real INI files via QSettings)
add_executable(unit_server_config ServerConfigTest.cc
    ${CMAKE_SOURCE_DIR}/src/server/ServerConfig.cc)
target_include_directories(unit_server_config PRIVATE
    ${CMAKE_SOURCE_DIR}/src/server)
target_link_libraries(unit_server_config PRIVATE
    GTest::gtest_main Qt6::Core)
set_target_properties(unit_server_config PROPERTIES AUTOMOC ON)
add_test(NAME unit_server_config COMMAND unit_server_config)

set_tests_properties(unit_common unit_server unit_client unit_auth unit_server_config
    PROPERTIES LABELS "unit")
