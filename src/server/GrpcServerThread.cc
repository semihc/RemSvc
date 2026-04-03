#include "GrpcServerThread.hh"

#include <string>
#include <absl/strings/str_format.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/health_check_service_interface.h>
#include "Assert.hh"
#include "Log.hh"

using namespace std;
using grpc::Server;
using grpc::ServerBuilder;


GrpcServerThread::GrpcServerThread(int port) : m_port(port)
{
    string server_address = absl::StrFormat("0.0.0.0:%d", port);

    grpc::EnableDefaultHealthCheckService(true);
    grpc::reflection::InitProtoReflectionServerBuilderPlugin();
    ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());

    m_service = make_unique<RS::RemSvcServiceImpl>();
    builder.RegisterService(m_service.get());

    m_server = std::move(builder.BuildAndStart());
    RS::Expects(m_server != nullptr);
    Log(RS::info) << "gRPC Server created to listen on " << server_address;
}


void GrpcServerThread::run()
{
    Log(RS::info, "Starting gRPC Server in a separate thread...");
    m_server->Wait();
}


void GrpcServerThread::stop()
{
    Log(RS::info, "Stopping gRPC Server...");
    m_server->Shutdown();
    wait();
}
