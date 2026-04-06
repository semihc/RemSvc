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

// Maximum bytes returned in out/err fields.  Output beyond this is truncated
// and a sentinel notice is appended.  Keeps gRPC messages bounded to a sane size.
static constexpr std::size_t kMaxOutputBytes = 256 * 1024;

// Appended to truncated output.  The U+0001 SOH bytes act as sentinels: this
// exact sequence is practically impossible to appear in real command output
// (SOH is never emitted by shells, compilers, or standard text tools).
// Any downstream consumer seeing this marker can reliably detect truncation
// without risking a false positive from legitimate output that happens to
// contain the bracketed message.
static constexpr std::string_view kTruncationMarker =
    "\n\x01[remSvc: output truncated at 262144 bytes]\x01\n";

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
// timeoutMs: the child process is forcibly killed if it has not exited within
//            this many milliseconds (default 30 000 ms = 30 s).
int runInProcess(std::string_view cmd, int cmdtyp, std::string_view cmdusr,
                 std::string& out, std::string& err, int timeoutMs = 30000);


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
    // True if any allowlist pattern failed to compile.  When set, checkAllowed()
    // denies every command — fail-safe behaviour prevents bad config from opening
    // an unintended hole.
    bool                    m_denyAll{false};
    std::atomic<int64_t>    m_commandsExecuted{0};
    std::chrono::steady_clock::time_point m_startTime;

    // Returns empty string if allowed, or a denial message if not.
    std::string checkAllowed(std::string_view cmd) const;
};

} // namespace RS

#endif // REMSVCSERVICEIMPL_HH