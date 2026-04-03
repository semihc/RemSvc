#pragma once
#ifndef REMSVCSERVICEIMPL_HH
#define REMSVCSERVICEIMPL_HH

#include <functional>
#include <string>
#include <string_view>

#include <grpcpp/grpcpp.h>
#include "RemSvc.grpc.pb.h"
#include "Hash.hh"


namespace RS {

// Signature of a function that runs a shell command and captures output.
// cmdtyp: 0 = native shell (cmd.exe / bash), 1 = PowerShell (powershell.exe / pwsh).
// Returns the process exit code.
using CmdRunner = std::function<int(std::string_view cmd, int cmdtyp,
                                    std::string&     out,
                                    std::string&     err)>;

// Default runner — routes to native shell or PowerShell based on cmdtyp.
int runInProcess(std::string_view cmd, int cmdtyp, std::string& out, std::string& err);


// Minimal interface for a bidirectional RemCmd stream.
// The real gRPC ServerReaderWriter and test fakes both satisfy this.
struct ICmdStream {
    virtual ~ICmdStream() = default;
    virtual bool Read(RemCmdMsg* msg) = 0;
    virtual bool Write(const RemResMsg& msg) = 0;
};


// gRPC service implementation.
// CmdRunner is injected at construction; defaults to runInProcess.
class RemSvcServiceImpl final : public RemSvc::Service {
public:
    explicit RemSvcServiceImpl(CmdRunner runner = runInProcess);

    grpc::Status Ping(grpc::ServerContext*  context,
                      const RS::PingMsg*    ping,
                      RS::PongMsg*          pong) override;

    grpc::Status GetStatus(grpc::ServerContext* context,
                           const RS::Empty*     req,
                           RS::StatusMsg*       res) override;

    grpc::Status RemCmd(grpc::ServerContext*   context,
                        const RS::RemCmdMsg*   req,
                        RS::RemResMsg*         res) override;

    grpc::Status RemCmdStrm(grpc::ServerContext*                                      context,
                            grpc::ServerReaderWriter<RS::RemResMsg, RS::RemCmdMsg>*   stream) override;

    // Testable core — process commands from any ICmdStream.
    grpc::Status processStream(ICmdStream& stream);

private:
    CmdRunner m_runner;
};

} // namespace RS

#endif // REMSVCSERVICEIMPL_HH
