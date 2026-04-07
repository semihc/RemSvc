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
    f.seekg(0, std::ios::end);
    auto size = f.tellg();
    if (size > 1 * 1024 * 1024)  // 1 MB limit — PEM certs are never this large
        throw std::runtime_error("file too large (> 1 MB): " + path);
    f.seekg(0, std::ios::beg);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // anonymous namespace


GrpcServerThread::GrpcServerThread(int port,
                                   std::optional<TlsConfig>          tls,
                                   std::vector<std::string>           allowlist,
                                   std::vector<std::string>           denylist,
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
    // SetAuthMetadataProcessor() is only supported by TLS (SSL) credentials;
    // calling it on InsecureServerCredentials triggers "Not Supported" in the
    // gRPC core.  Attach the processor only when TLS is active.  When running
    // insecure (dev/test), auth-token enforcement is unavailable and a warning
    // is emitted if tokens were configured.
    auto authProc = std::make_shared<RS::BearerTokenAuthProcessor>(
        std::move(authTokens));
    if (tls) {
        creds->SetAuthMetadataProcessor(authProc);
    } else if (authProc->IsEnabled()) {
        Log(RS::warn,
            "GrpcServerThread: bearer-token auth is configured but TLS is "
            "disabled — SetAuthMetadataProcessor is not supported on insecure "
            "credentials; authentication will NOT be enforced.");
    }

    builder.AddListeningPort(server_address, creds);

    // Wrap runInProcess with the configured per-command timeout so the
    // CmdRunner signature stays unchanged.
    int timeout = cmdTimeoutMs;
    RS::CmdRunner runner = [timeout](std::string_view cmd, int cmdtyp,
                                     std::string_view cmdusr,
                                     std::string& out, std::string& err) {
        return RS::runInProcess(cmd, cmdtyp, cmdusr, out, err, timeout);
    };
    m_service = make_unique<RS::RemSvcServiceImpl>(std::move(runner), std::move(allowlist),
                                                   cmdTimeoutMs, std::move(denylist));
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
                    std::chrono::milliseconds(static_cast<int64_t>(m_cmdTimeoutMs) * 2);
    m_server->Shutdown(deadline);
    wait();
}
