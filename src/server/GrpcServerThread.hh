#pragma once
#ifndef GRPC_SERVER_THREAD_HH
#define GRPC_SERVER_THREAD_HH

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <QThread>
#include <grpcpp/grpcpp.h>
#include "AuthInterceptor.hh"
#include "RemSvcServiceImpl.hh"


// TLS credentials for the gRPC server.
// certFile and keyFile are required when TLS is enabled.
// caFile is optional — supply it to require mutual TLS (client certificates).
struct TlsConfig {
    std::string certFile;  // path to PEM server certificate
    std::string keyFile;   // path to PEM private key
    std::string caFile;    // path to PEM CA cert (empty = server-side TLS only)
};


// Runs the gRPC server in a dedicated QThread.
// Construction binds and starts the server; call start() to begin serving.
// Call stop() for graceful shutdown.
//
// tls:          if present, enables TLS using the supplied cert/key files.
// allowlist:    forwarded to RemSvcServiceImpl — see its header for semantics.
// authTokens:   identity→token map forwarded to BearerTokenAuthProcessor.
//               Empty map = authentication disabled.
// cmdTimeoutMs: per-command child-process timeout in milliseconds (default 30 s).
class GrpcServerThread : public QThread {
    Q_OBJECT
public:
    explicit GrpcServerThread(int port,
                              std::optional<TlsConfig>          tls          = std::nullopt,
                              std::vector<std::string>           allowlist    = {},
                              std::map<std::string, std::string> authTokens   = {},
                              int                                cmdTimeoutMs = 30000);

    void run() override;
    void stop();

signals:
    void shutdownRequested();

private:
    int m_port{};
    int m_cmdTimeoutMs{30000};
    std::unique_ptr<grpc::Server>          m_server{};
    std::unique_ptr<RS::RemSvcServiceImpl> m_service{};
};

#endif // GRPC_SERVER_THREAD_HH
