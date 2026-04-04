#include "GrpcServerThread.hh"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <absl/strings/str_format.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/health_check_service_interface.h>
#include "Assert.hh"
#include "Log.hh"

using namespace std;
using grpc::Server;
using grpc::ServerBuilder;


namespace {

std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open file: " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // anonymous namespace


GrpcServerThread::GrpcServerThread(int port,
                                   std::optional<TlsConfig> tls,
                                   std::vector<std::string> allowlist)
    : m_port(port)
{
    string server_address = absl::StrFormat("0.0.0.0:%d", port);

    grpc::EnableDefaultHealthCheckService(true);
    grpc::reflection::InitProtoReflectionServerBuilderPlugin();
    ServerBuilder builder;

    // Select credentials: TLS when config supplied, insecure otherwise.
    shared_ptr<grpc::ServerCredentials> creds;
    if (tls) {
        grpc::SslServerCredentialsOptions ssl_opts;
        ssl_opts.pem_key_cert_pairs.push_back({
            readFile(tls->keyFile),
            readFile(tls->certFile)
        });
        if (!tls->caFile.empty()) {
            ssl_opts.pem_root_certs = readFile(tls->caFile);
            ssl_opts.client_certificate_request =
                GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY;
        }
        creds = grpc::SslServerCredentials(ssl_opts);
        Log(RS::info, "TLS enabled (cert={} key={} ca={})",
            tls->certFile, tls->keyFile, tls->caFile);
    } else {
        creds = grpc::InsecureServerCredentials();
    }
    builder.AddListeningPort(server_address, creds);

    m_service = make_unique<RS::RemSvcServiceImpl>(RS::runInProcess, std::move(allowlist));
    builder.RegisterService(m_service.get());

    m_server = std::move(builder.BuildAndStart());
    RS::Expects(m_server != nullptr);
    Log(RS::info, "gRPC Server listening on {}", server_address);
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
