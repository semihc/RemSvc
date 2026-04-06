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
                                   std::optional<TlsConfig>          tls,
                                   std::vector<std::string>           allowlist,
                                   std::map<std::string, std::string> authTokens,
                                   int                                cmdTimeoutMs)
    : m_port(port)
    , m_cmdTimeoutMs(cmdTimeoutMs)
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

    // Attach the bearer-token auth processor to the credentials.
    // AuthMetadataProcessor::Process() is called before any service handler,
    // for every RPC, regardless of TLS mode.
    auto authProc = std::make_shared<RS::BearerTokenAuthProcessor>(
        std::move(authTokens));
    creds->SetAuthMetadataProcessor(authProc);

    builder.AddListeningPort(server_address, creds);

    // Wrap runInProcess with the configured per-command timeout so the
    // CmdRunner signature stays unchanged.
    int timeout = cmdTimeoutMs;
    RS::CmdRunner runner = [timeout](std::string_view cmd, int cmdtyp,
                                     std::string_view cmdusr,
                                     std::string& out, std::string& err) {
        return RS::runInProcess(cmd, cmdtyp, cmdusr, out, err, timeout);
    };
    m_service = make_unique<RS::RemSvcServiceImpl>(std::move(runner), std::move(allowlist));
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
    // Give active RPCs a grace window to finish before forcing shutdown.
    // Any stream still open after the deadline is aborted by gRPC.
    // The grace period is set to twice the configured per-command timeout so
    // that a command already running at shutdown time has a fair chance to
    // complete even if a new command started just before the signal arrived.
    auto deadline = std::chrono::system_clock::now() +
                    std::chrono::milliseconds(m_cmdTimeoutMs * 2);
    m_server->Shutdown(deadline);
    wait();
}
