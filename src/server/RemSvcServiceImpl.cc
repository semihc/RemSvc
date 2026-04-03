#include "RemSvcServiceImpl.hh"

#include <QProcess>
#include "Log.hh"

using namespace std;

namespace RS {


int runInProcess(std::string_view cmd, int cmdtyp, std::string& out, std::string& err)
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
#else
    if (cmdtyp == 1) {
        prg.setProgram("pwsh");
        prg.setArguments({"-Command", qcmd});
    } else {
        prg.setProgram("/bin/sh");
        prg.setArguments({"-c", qcmd});
    }
#endif

    prg.start();

    int waitPeriod = 30000; // msecs
    prg.waitForFinished(waitPeriod);

    QByteArray errStr = prg.readAllStandardError();
    QByteArray outStr = prg.readAllStandardOutput();
    int rv = prg.exitCode();

    out = outStr.constData();
    Log(debug) << outStr.data();
    if (!errStr.isEmpty()) {
        err = errStr.constData();
        Log(error) << errStr.data();
    }

    return rv;
}


RemSvcServiceImpl::RemSvcServiceImpl(CmdRunner runner)
    : m_runner(std::move(runner))
{}


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
    res->set_rc(0);
    res->set_msg("OK");
    return grpc::Status::OK;
}


grpc::Status RemSvcServiceImpl::RemCmd(grpc::ServerContext*,
                                       const RS::RemCmdMsg* req,
                                       RS::RemResMsg*       res)
{
    string cmd    = req->cmd();
    int    cmdtyp = req->cmdtyp();
    Log(info, "Received cmd={} cmdtyp={} tid={}", cmd, cmdtyp, req->tid());

    // If the caller provided a hash, verify it before executing anything.
    if (!req->hsh().empty()) {
        string expected = crc32Hex(cmd);
        if (req->hsh() != expected) {
            Log(warn, "Hash mismatch: received={} expected={} cmd={}", req->hsh(), expected, cmd);
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                "hash mismatch: command integrity check failed");
        }
    }

    string out, err;
    int rc = m_runner(cmd, cmdtyp, out, err);

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
        Log(info, "RemCmdStrm cmd={} cmdtyp={} tid={}", cmd, cmdtyp, req.tid());

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
        int rc = m_runner(cmd, cmdtyp, out, err);

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
