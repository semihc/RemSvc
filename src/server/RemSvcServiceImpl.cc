#include "RemSvcServiceImpl.hh"

#include <QDeadlineTimer>
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

// Truncate s to kMaxOutputBytes and append the sentinel marker if it was longer.
// The marker uses SOH (\x01) delimiters — see kTruncationMarker in the header.
void truncateOutput(std::string& s) {
    if (s.size() > kMaxOutputBytes) {
        s.resize(kMaxOutputBytes);
        s.append(kTruncationMarker.data(), kTruncationMarker.size());
    }
}

} // anonymous namespace


// ── Default CmdRunner ─────────────────────────────────────────────────────────

int runInProcess(std::string_view cmd, int cmdtyp, std::string_view cmdusr,
                 std::string& out, std::string& err, int timeoutMs)
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
            if (::setgid(gid) != 0 || ::setuid(uid) != 0)
                ::_exit(125);  // 125: privilege-drop failed — child must not execute
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

    // Close the write end of stdin immediately so the child receives EOF on
    // its stdin file descriptor.  Without this, QProcess leaves the stdin pipe
    // open and any command that reads from stdin (cat, read, pause, Get-Content
    // …) will block until the cmdTimeoutMs deadline fires, stalling the entire
    // stream for that duration.  With EOF delivered at process start, such
    // commands exit promptly (typically rc=0 or rc=1) rather than hanging.
    prg.closeWriteChannel();

    // Drain stdout and stderr incrementally while the process runs.
    //
    // Calling waitForFinished() alone can deadlock when the child writes more
    // output than the OS pipe buffer (~64 KB on Linux): the child blocks trying
    // to write to a full pipe while the parent blocks waiting for the child to
    // exit.  Draining both channels in a polling loop avoids this.
    //
    // A short poll interval (50 ms) keeps CPU negligible while ensuring the
    // pipe never fills up between drains, even at full 256 KB output.
    QByteArray outAll, errAll;
    QDeadlineTimer deadline(timeoutMs);

    while (!deadline.hasExpired()) {
        // Wake on data-ready or 50 ms, whichever comes first.
        prg.waitForReadyRead(50);
        outAll += prg.readAllStandardOutput();
        errAll += prg.readAllStandardError();
        if (prg.state() == QProcess::NotRunning)
            break;
    }

    if (prg.state() != QProcess::NotRunning) {
        prg.kill();
        prg.waitForFinished(1000);   // brief wait for kill to take effect
        err = "process timed out";
        Log(RS::error, "runInProcess: process timed out after {}ms cmd={}", timeoutMs, cmd);
        return 1;
    }

    // Final drain — bytes buffered after the last waitForReadyRead.
    outAll += prg.readAllStandardOutput();
    errAll += prg.readAllStandardError();

    int rv = prg.exitCode();

    out = std::string(outAll.constData(), outAll.size());
    Log(RS::debug, "runInProcess out={}", out);
    if (!errAll.isEmpty()) {
        err = std::string(errAll.constData(), errAll.size());
        Log(RS::error, "runInProcess err={}", err);
    }

    return rv;
}


// ── RemSvcServiceImpl ─────────────────────────────────────────────────────────

RemSvcServiceImpl::RemSvcServiceImpl(CmdRunner runner, std::vector<std::string> allowlist,
                                     int timeoutMs, std::vector<std::string> denylist)
    : m_runner(runner ? std::move(runner)
                      : CmdRunner([timeoutMs](std::string_view cmd, int cmdtyp,
                                              std::string_view cmdusr,
                                              std::string& out, std::string& err) {
                            return runInProcess(cmd, cmdtyp, cmdusr, out, err, timeoutMs);
                        }))
    , m_startTime(std::chrono::steady_clock::now())
{
    // Compile each pattern string into a std::regex at construction time.
    //
    // If any pattern is syntactically invalid (std::regex_error), the entire
    // allowlist is disabled and m_denyAll is set to true.  This is fail-safe
    // behaviour: a misconfigured pattern must not silently create an open hole
    // that permits every command through.  The server will still start; every
    // command will be denied with PERMISSION_DENIED until the config is fixed
    // and the server restarted.
    m_allowlist.reserve(allowlist.size());
    for (const auto& pat : allowlist) {
        try {
            m_allowlist.emplace_back(pat,
                std::regex_constants::ECMAScript |
                std::regex_constants::optimize);
        } catch (const std::regex_error& e) {
            Log(RS::error,
                "RemSvcServiceImpl: invalid allowlist regex '{}': {} — "
                "ALL commands will be denied until the config is corrected.",
                pat, e.what());
            m_denyAll = true;
            m_allowlist.clear();
            break;
        }
    }
    if (!allowlist.empty() && m_denyAll) {
        Log(RS::warn,
            "RemSvcServiceImpl: allowlist compilation failed; "
            "deny-all mode is active ({} pattern(s) discarded).",
            allowlist.size());
    }

    m_denylist.reserve(denylist.size());
    for (const auto& pat : denylist) {
        try {
            m_denylist.emplace_back(pat,
                std::regex_constants::ECMAScript |
                std::regex_constants::optimize);
        } catch (const std::regex_error& e) {
            Log(RS::error,
                "RemSvcServiceImpl: invalid denylist regex '{}': {} — "
                "ALL commands will be denied until the config is corrected.",
                pat, e.what());
            m_denyAll = true;
            m_denylist.clear();
            break;
        }
    }
    if (!denylist.empty() && m_denyAll && m_denylist.empty()) {
        Log(RS::warn,
            "RemSvcServiceImpl: denylist compilation failed; "
            "deny-all mode is active ({} pattern(s) discarded).",
            denylist.size());
    }
}


std::string RemSvcServiceImpl::checkAllowed(std::string_view cmd) const {
    // Deny-all mode: triggered when any list pattern failed to compile.
    if (m_denyAll)
        return "all commands denied: allowlist configuration is invalid (check server logs)";
    std::string s(cmd);
    // Denylist takes precedence: if the command matches any deny pattern, reject it.
    for (const auto& rx : m_denylist)
        if (std::regex_search(s, rx))
            return "command denied by denylist: " + s;
    // Empty, valid allowlist = no allowlist restriction configured; permit all.
    if (m_allowlist.empty()) return {};
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