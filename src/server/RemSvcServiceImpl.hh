#pragma once
#ifndef REMSVCSERVICEIMPL_HH
#define REMSVCSERVICEIMPL_HH

#include <atomic>
#include <chrono>
#include <functional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

#include <grpcpp/grpcpp.h>
#include "RemSvc.grpc.pb.h"
#include "Hash.hh"


namespace RS {

// Maximum bytes returned in out/err fields.  Output beyond this is replaced
// with a truncation notice.  Keeps gRPC messages bounded to a sane size.
static constexpr std::size_t kMaxOutputBytes = 256 * 1024;

// Signature of a function that runs a shell command and captures output.
// cmd:    the command string to execute.
// cmdtyp: 0 = native shell (cmd.exe / sh), 1 = PowerShell (powershell.exe / pwsh).
// cmdusr: if non-empty, the process is executed as this OS user (Linux only;
//         ignored on Windows with a warning logged).
// Returns the process exit code.
using CmdRunner = std::function<int(std::string_view cmd,
                                    int              cmdtyp,
                                    std::string_view cmdusr,
                                    std::string&     out,
                                    std::string&     err)>;

// Default runner — routes to native shell or PowerShell based on cmdtyp,
// and switches to cmdusr (if non-empty) on Linux via setuid/setgid.
int runInProcess(std::string_view cmd, int cmdtyp, std::string_view cmdusr,
                 std::string& out, std::string& err);


// Minimal interface for a bidirectional RemCmd stream.
// The real gRPC ServerReaderWriter and test fakes both satisfy this.
struct ICmdStream {
    virtual ~ICmdStream() = default;
    virtual bool Read(RemCmdMsg* msg) = 0;
    virtual bool Write(const RemResMsg& msg) = 0;
};


// gRPC service implementation.
// CmdRunner and an optional command allowlist are injected at construction.
//
// allowlist: if non-empty, each entry is a std::regex pattern; a command
//            is permitted only if it matches (regex_search) at least one
//            pattern.  Empty allowlist = accept all commands.
class RemSvcServiceImpl final : public RemSvc::Service {
public:
    explicit RemSvcServiceImpl(CmdRunner                runner    = runInProcess,
                               std::vector<std::string> allowlist = {});

    grpc::Status Ping(grpc::ServerContext*  context,
                      const RS::PingMsg*    ping,
                      RS::PongMsg*          pong) override;

    // Returns rc=0, msg="state=OK uptime=Ns commands=N".
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
    CmdRunner               m_runner;
    std::vector<std::regex> m_allowlist;   // compiled from constructor patterns
    std::atomic<int64_t>    m_commandsExecuted{0};
    std::chrono::steady_clock::time_point m_startTime;

    // Returns empty string if allowed, or a denial message if not.
    std::string checkAllowed(std::string_view cmd) const;
};

} // namespace RS

#endif // REMSVCSERVICEIMPL_HH