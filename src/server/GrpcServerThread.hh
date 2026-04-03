#pragma once
#ifndef GRPC_SERVER_THREAD_HH
#define GRPC_SERVER_THREAD_HH

#include <memory>
#include <QThread>
#include <grpcpp/grpcpp.h>
#include "RemSvcServiceImpl.hh"

// Runs the gRPC server in a dedicated QThread.
// Construction binds and starts the server; call start() to begin serving.
// Call stop() for graceful shutdown.
class GrpcServerThread : public QThread {
    Q_OBJECT
public:
    explicit GrpcServerThread(int port);

    void run() override;
    void stop();

signals:
    void shutdownRequested();

private:
    int m_port{};
    std::unique_ptr<grpc::Server>          m_server{};
    std::unique_ptr<RS::RemSvcServiceImpl> m_service{};
};

#endif // GRPC_SERVER_THREAD_HH
