# Top level CMake Project
cmake_minimum_required(VERSION 3.18.4) 
project(RemSvc LANGUAGES C CXX VERSION 1.0.0)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED OFF)
#-set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=gnu++20") # Appends the flag

# Enforce out-of-source build, fail otherwise
set(CMAKE_DISABLE_IN_SOURCE_BUILD ON)
set(CMAKE_DISABLE_SOURCE_CHANGES  ON)


# Provide hints to package locations
#set(CMAKE_SYSTEM_NAME Windows)
message(STATUS "CMAKE_SYSTEM_NAME = ${CMAKE_SYSTEM_NAME}")

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  #
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  if(EXISTS /data/local/stow/grpc-1.67.0)
    # Original build host — custom stow prefix
    set(absl_DIR /data/local/stow/grpc-1.67.0/lib64/cmake/absl)
    set(utf8_range_DIR /data/local/stow/grpc-1.67.0/lib64/cmake/utf8_range)
    set(protobuf_DIR /data/local/stow/grpc-1.67.0/lib64/cmake/protobuf)
    set(gRPC_DIR /data/local/stow/grpc-1.67.0/lib/cmake/grpc)
    set(Qt6_DIR /data/local/stow/qt-6.8.1/lib/cmake/Qt6)
    set(CLI11_DIR /data/local/stow/CLI11-2.4.1/share/cmake/CLI11)
    set(GTest_DIR /data/local/stow/gtest-1.17.0/lib64/cmake/GTest)
  elseif(EXISTS /opt/stow/qt-6.8.3)
    # gobi — apt gRPC + /opt/stow packages
    set(Qt6_DIR /opt/stow/qt-6.8.3/lib/cmake/Qt6)
    set(CLI11_DIR /opt/stow/CLI11-v2.5.0/share/cmake/CLI11)
    set(spdlog_DIR /opt/stow/spdlog-v1.15.2/lib/cmake/spdlog)
    set(GTest_DIR /opt/stow/gtest-v1.16.0/lib/cmake/GTest)
  endif()
endif()


# Locate the required packages
find_package(Threads)
find_package(CLI11 CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(Protobuf REQUIRED)
find_package(gRPC CONFIG REQUIRED)


find_package(Qt6 REQUIRED COMPONENTS Core Protobuf Grpc HttpServer)
qt_standard_project_setup()



# Subdirectories
add_subdirectory(src)

enable_testing()
add_subdirectory(tst)


# ── Install rules ─────────────────────────────────────────────────
include(GNUInstallDirs)

install(TARGETS client
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT Runtime
)

# ── Qt runtime deployment ─────────────────────────────────────────
# windeployqt copies Qt DLLs, plugins, and translations needed at runtime.
# It runs at install time so that CPack picks up everything it copies.
if(WIN32)
    find_program(WINDEPLOYQT_EXECUTABLE windeployqt
        HINTS "${Qt6_DIR}/../../../bin"
    )
    if(WINDEPLOYQT_EXECUTABLE)
        # Deploy Qt dependencies for the server binary
        install(CODE "
            message(STATUS \"Running windeployqt for RemSvc_server...\")
            execute_process(
                COMMAND \"${WINDEPLOYQT_EXECUTABLE}\"
                    --no-translations
                    --no-system-d3d-compiler
                    --no-opengl-sw
                    --dir \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}\"
                    \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/RemSvc_server.exe\"
                RESULT_VARIABLE _wdq_result
            )
            if(NOT _wdq_result EQUAL 0)
                message(FATAL_ERROR \"windeployqt failed for RemSvc_server (exit \${_wdq_result})\")
            endif()
        "
        COMPONENT Runtime
        )
        # Deploy Qt dependencies for the client binary
        install(CODE "
            message(STATUS \"Running windeployqt for RemSvc_client...\")
            execute_process(
                COMMAND \"${WINDEPLOYQT_EXECUTABLE}\"
                    --no-translations
                    --no-system-d3d-compiler
                    --no-opengl-sw
                    --dir \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}\"
                    \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/RemSvc_client.exe\"
                RESULT_VARIABLE _wdq_result
            )
            if(NOT _wdq_result EQUAL 0)
                message(FATAL_ERROR \"windeployqt failed for RemSvc_client (exit \${_wdq_result})\")
            endif()
        "
        COMPONENT Runtime
        )
    else()
        message(WARNING "windeployqt not found — Qt DLLs will NOT be bundled. "
                        "Set Qt6_DIR or add Qt bin/ to PATH before configuring.")
    endif()
endif()

# ── CPack ─────────────────────────────────────────────────────────
set(CPACK_PACKAGE_NAME        "RemSvc")
set(CPACK_PACKAGE_VERSION     ${PROJECT_VERSION})
set(CPACK_PACKAGE_VENDOR      "Bupa")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Remote Execution Service — gRPC server and CLI client")
set(CPACK_PACKAGE_CONTACT     "semih.cemiloglu@bupa.com.au")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/lic/LICENSE.txt")

# Put all installable components into a single package
set(CPACK_COMPONENTS_ALL Runtime)

if(WIN32)
    # ZIP: no external installer tool required; easily extracted on any Windows host
    set(CPACK_GENERATOR "ZIP")
    set(CPACK_PACKAGE_FILE_NAME
        "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-win64")
elseif(UNIX)
    set(CPACK_GENERATOR "TGZ;DEB")
    set(CPACK_PACKAGE_FILE_NAME
        "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-linux-x64")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_CONTACT}")
    set(CPACK_DEBIAN_PACKAGE_SECTION    "utils")
    # gRPC and Qt are statically linked or bundled — no Debian deps declared.
    # Add entries here if you switch to shared linking on Linux.
    set(CPACK_DEBIAN_PACKAGE_DEPENDS    "")
endif()

include(CPack)
