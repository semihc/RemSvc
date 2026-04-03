# Top level CMake Project
cmake_minimum_required(VERSION 3.18.4) 
project(RemSvc LANGUAGES C CXX VERSION 0.1)
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
  # Hints to find_package() call below
  set(absl_DIR /data/local/stow/grpc-1.67.0/lib64/cmake/absl)
  set(utf8_range_DIR /data/local/stow/grpc-1.67.0/lib64/cmake/utf8_range)
  set(protobuf_DIR /data/local/stow/grpc-1.67.0/lib64/cmake/protobuf)
  set(gRPC_DIR /data/local/stow/grpc-1.67.0/lib/cmake/grpc)
  set(Qt6_DIR /data/local/stow/qt-6.8.1/lib/cmake/Qt6)
  set(CLI11_DIR /data/local/stow/CLI11-2.4.1/share/cmake/CLI11)    
  set(GTest_DIR /data/local/stow/gtest-1.17.0/lib64/cmake/GTest)
endif()


# Locate the required packages
find_package(Threads)
find_package(CLI11 CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)


find_package(Qt6 REQUIRED COMPONENTS Core Protobuf Grpc HttpServer)
qt_standard_project_setup()



# Subdirectories
add_subdirectory(src)

enable_testing()
add_subdirectory(tst)
