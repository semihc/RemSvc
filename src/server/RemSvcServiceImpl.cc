#include "RemSvcServiceImpl.hh"

#include <QProcess>
#include <QStandardPaths>
#include "Log.hh"

#ifndef Q_OS_WIN
#  include <pwd.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

using namespace std;

namespace RS {

// ── Helpers (file-local) ─────────────────────────────────────────────────────

namespace {

// Truncate s to kMaxOutputBytes and append a notice if it was longer.
void truncateOutput(std::string& s) {
    if (s.size() > kMaxOutputBytes) {
        s.resize(kMaxOutputBytes);
        s += "\n[output truncated]";
    }
}

} // anonymous namespace


// ── Default CmdRunner ─────────────────────────────────────────────────────────

int runInProcess(std::string_view cmd, int cmdtyp, std::string_view cmdusr,
                 std::string& out, std::string& err)
{
    QProcess prg;
    QString qcmd = QString::fromUtf8(cmd.data(), static_cast<qsizetype>(cmd.size()));

#ifdef Q_OS_WIN
    if (cmdtyp == 1) {
        prg.setProgram("powershell.exe");
        prg.setArguments({"-Command", qcmd});
    } else {
        prg.setProgram("cmd.exe");
        prg.setArguments({"/C", qcmd});
    }
    if (!cmdusr.empty())
        Log(RS::warn, "runInProcess: user switching is not supported on Windows (cmdusr={})", cmdusr);
#else
    if (cmdtyp == 1) {
        QString pwshPath = QStandardPaths::findExecutable("pwsh");
        if (pwshPath.isEmpty()) {
            err = "PowerShell (pwsh) is not installed on this system";
            Log(RS::error, "runInProcess: pwsh not found, cannot execute cmdtyp=1");
            return 1;
        }
        prg.setProgram(pwshPath);
        prg.setArguments({"-Command", qcmd});
    } else {
        prg.setProgram("/bin/sh");
        prg.setArguments({"-c", qcmd});
    }

    // User switching: resolve cmdusr to uid/gid, apply in child process.
    if (!cmdusr.empty()) {
        std::string user(cmdusr);
        struct passwd  pw{};
        struct passwd* result = nullptr;
        char           buf[4096];
        if (::getpwnam_r(user.c_str(), &pw, buf, sizeof(buf), &result) != 0 ||
            result == nullptr) {
            err = "user not found: " + user;
            Log(RS::error, "runInProcess: OS user not found cmdusr={}", cmdusr);
            return 1;
        }
        uid_t uid = pw.pw_uid;
        gid_t gid = pw.pw_gid;
        prg.setChildProcessModifier([uid, gid]() {
            ::setgid(gid);
            ::setuid(uid);
        });
        Log(RS::info, "runInProcess: will execute as uid={} gid={} ({})", uid, gid, cmdusr);
    }
#endif

    prg.start();
    if (prg.state() == QProcess::NotRunning &&
        prg.error() == QProcess::FailedToStart) {
        err = "process failed to start";
        Log(RS::error, "runInProcess: failed to start process for cmd={}", cmd);
        return 1;
    }

    static constexpr int kWaitPeriodMs = 30000;
    if (!prg.waitForFinished(kWaitPeriodMs)) {
        prg.kill();
        err = "process timed out";
        Log(RS::error, "runInProcess: process timed out after {}ms cmd={}", kWaitPeriodMs, cmd);
        return 1;
    }

    QByteArray errStr = prg.readAllStandardError();
    QByteArray outStr = prg.readAllStandardOutput();
    int rv = prg.exitCode();

    out = outStr.constData();
    Log(RS::debug, "runInProcess out={}", outStr.constData());
    if (!errStr.isEmpty()) {
        err = errStr.constData();
        Log(RS::error, "runInProcess err={}", errStr.constData());
    }

    return rv;
}


// ── RemSvcServiceImpl ─────────────────────────────────────────────────────────

RemSvcServiceImpl::RemSvcServiceImpl(CmdRunner runner, std::vector<std::string> allowlist)
    : m_runner(std::move(runner))
    , m_startTime(std::chrono::steady_clock::now())
{
    // Compile each pattern string into a std::regex at construction time so
    // that bad patterns fail early (throws std::regex_error).
    m_allowlist.reserve(allowlist.size());
    for (const auto& pat : allowlist)
        m_allowlist.emplace_back(pat);
}


std::string RemSvcServiceImpl::checkAllowed(std::string_view cmd) const {
    if (m_allowlist.empty()) return {};   // empty allowlist = permit all
    std::string s(cmd);
    for (const auto& rx : m_allowlist)
        if (std::regex_search(s, rx)) return {};
    return "command not permitted by allowlist: " + s;
}


grpc::Status RemSvcServiceImpl::Ping(grpc::ServerContext*,
                                     const RS::PingMsg* ping,
                                     RS::PongMsg*       pong)
{
    Log(info) << "Received ping seq id=" << ping->seq()
              << " time=" << ping->time()
              << " data=" << ping->data();
    pong->set_seq(ping->seq());
    pong->set_time(ping->time());
    pong->set_data(ping->data());
    return grpc::Status::OK;
}


grpc::Status RemSvcServiceImpl::GetStatus(grpc::ServerContext*,
                                          const RS::Empty*,
                                          RS::StatusMsg* res)
{
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - m_startTime).count();
    res->set_rc(0);
    res->set_msg("state=OK uptime=" + std::to_string(uptime) +
                 "s commands=" + std::to_string(m_commandsExecuted.load()));
    return grpc::Status::OK;
}


grpc::Status RemSvcServiceImpl::RemCmd(grpc::ServerContext*,
                                       const RS::RemCmdMsg* req,
                                       RS::RemResMsg*       res)
{
    string cmd    = req->cmd();
    int    cmdtyp = req->cmdtyp();
    string cmdusr = req->cmdusr();
    Log(info, "RemCmd cmd={} cmdtyp={} tid={} src={} usr={}",
        cmd, cmdtyp, req->tid(), req->src(), cmdusr);

    // Authorization: reject commands not matching the allowlist.
    if (auto deny = checkAllowed(cmd); !deny.empty()) {
        Log(warn, "RemCmd denied: {}", deny);
        return grpc::Status(grpc::StatusCode::PERMISSION_DENIED, deny);
    }

    // Integrity: verify CRC32 hash if the caller provided one.
    if (!req->hsh().empty()) {
        string expected = crc32Hex(cmd);
        if (req->hsh() != expected) {
            Log(warn, "Hash mismatch: received={} expected={} cmd={}", req->hsh(), expected, cmd);
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                "hash mismatch: command integrity check failed");
        }
    }

    string out, err;
    int rc = m_runner(cmd, cmdtyp, cmdusr, out, err);
    ++m_commandsExecuted;

    truncateOutput(out);
    truncateOutput(err);

    res->set_rc(rc);
    res->set_tid(req->tid());
    res->set_out(out);
    res->set_err(err);
    res->set_hsh(req->hsh());

    Log(debug, "rc={} out={} err={}", rc, out, err);
    return grpc::Status::OK;
}


// Thin adapter: wraps grpc::ServerReaderWriter to satisfy ICmdStream.
namespace {
struct GrpcCmdStream : RS::ICmdStream {
    grpc::ServerReaderWriter<RS::RemResMsg, RS::RemCmdMsg>* rw;
    explicit GrpcCmdStream(grpc::ServerReaderWriter<RS::RemResMsg, RS::RemCmdMsg>* s) : rw(s) {}
    bool Read(RS::RemCmdMsg* msg)        override { return rw->Read(msg); }
    bool Write(const RS::RemResMsg& msg) override { return rw->Write(msg); }
};
} // anonymous namespace


grpc::Status RemSvcServiceImpl::RemCmdStrm(grpc::ServerContext*,
    grpc::ServerReaderWriter<RS::RemResMsg, RS::RemCmdMsg>* stream)
{
    GrpcCmdStream adapter{stream};
    return processStream(adapter);
}


grpc::Status RemSvcServiceImpl::processStream(ICmdStream& stream)
{
    RS::RemCmdMsg req;
    while (stream.Read(&req)) {
        string cmd    = req.cmd();
        int    cmdtyp = req.cmdtyp();
        string cmdusr = req.cmdusr();
        Log(info, "RemCmdStrm cmd={} cmdtyp={} tid={} src={} usr={}",
            cmd, cmdtyp, req.tid(), req.src(), cmdusr);

        // Authorization.
        if (auto deny = checkAllowed(cmd); !deny.empty()) {
            Log(warn, "RemCmdStrm denied: {}", deny);
            return grpc::Status(grpc::StatusCode::PERMISSION_DENIED, deny);
        }

        // Integrity.
        if (!req.hsh().empty()) {
            string expected = crc32Hex(cmd);
            if (req.hsh() != expected) {
                Log(warn, "RemCmdStrm hash mismatch: received={} expected={} cmd={}",
                    req.hsh(), expected, cmd);
                return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                    "hash mismatch: command integrity check failed");
            }
        }

        string out, err;
        int rc = m_runner(cmd, cmdtyp, cmdusr, out, err);
        ++m_commandsExecuted;

        truncateOutput(out);
        truncateOutput(err);

        RS::RemResMsg res;
        res.set_rc(rc);
        res.set_tid(req.tid());
        res.set_out(out);
        res.set_err(err);
        res.set_hsh(req.hsh());
        Log(debug, "RemCmdStrm rc={} out={} err={}", rc, out, err);
        stream.Write(res);
    }
    return grpc::Status::OK;
}


} // namespace RS