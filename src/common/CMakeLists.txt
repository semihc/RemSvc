#
# gRPC Common Library
#

file(GLOB libSrcs *.cc)
file(GLOB libHdrs *.h *.hh *.hpp)

# Target Library name
set(tgt common)

add_library(${tgt} STATIC)

target_sources(${tgt}
    PRIVATE ${libSrcs}
    PUBLIC FILE_SET HEADERS FILES ${libHdrs}
  )
  
target_include_directories(${tgt} PUBLIC .)

target_link_libraries(${tgt} PUBLIC
  #gRPC::grpc
  #gRPC::grpc++
  #gRPC::grpc++_reflection
  #protobuf::libprotobuf
  absl::flags
  absl::flags_parse
  CLI11::CLI11
  spdlog::spdlog
  #Qt6::Core
)
