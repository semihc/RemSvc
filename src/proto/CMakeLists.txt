#
#
#

set(PROTO_FILES
    RemSvc.proto
)

# protobuf source files go into the lib just like any other CPP source file
add_library(RemSvc ${PROTO_FILES})

target_link_libraries(RemSvc
    PUBLIC        
        gRPC::grpc
        gRPC::grpc++
		gRPC::grpc++_reflection
		protobuf::libprotobuf
)

target_include_directories(RemSvc
    PUBLIC
        ${CMAKE_CURRENT_BINARY_DIR}
)

get_target_property(grpc_cpp_plugin_location gRPC::grpc_cpp_plugin LOCATION)

# compile the message types
protobuf_generate(TARGET RemSvc LANGUAGE cpp)

# compile the GRPC services
protobuf_generate(
    TARGET
        RemSvc
    LANGUAGE
        grpc
    GENERATE_EXTENSIONS
        .grpc.pb.h
        .grpc.pb.cc
    PLUGIN
        "protoc-gen-grpc=${grpc_cpp_plugin_location}"
)
