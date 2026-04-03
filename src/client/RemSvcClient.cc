/*
 *
 * Copyright Bupa Australia Pty Ltd
 *
 * Author: Semih Cemiloglu
 *
 */

#include "RemSvcClient.hh"

// Std
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

// Abseil
#include <absl/time/time.h>

// gRPC
#include <grpcpp/grpcpp.h>

// Prj
#include "Hash.hh"
#include "Log.hh"

using namespace std;
using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using namespace RS;


// ── Adapters wrapping the real gRPC stub ──────────────────────────────────

namespace {

struct GrpcCmdStrmClient : RS::ICmdStrmClient {
    // Client writes RemCmdMsg, reads RemResMsg  →  <W=RemCmdMsg, R=RemResMsg>
    unique_ptr<grpc::ClientReaderWriter<RS::RemCmdMsg, RS::RemResMsg>> rw;
    explicit GrpcCmdStrmClient(
        unique_ptr<grpc::ClientReaderWriter<RS::RemCmdMsg, RS::RemResMsg>> s)
        : rw(std::move(s)) {}
    bool Write(const RS::RemCmdMsg& msg) override { return rw->Write(msg); }
    bool Read(RS::RemResMsg* msg)        override { return rw->Read(msg); }
    void WritesDone()                    override { rw->WritesDone(); }
    grpc::Status Finish()                override { return rw->Finish(); }
};

struct GrpcRemSvcStub : RS::IRemSvcStub {
    unique_ptr<RS::RemSvc::Stub> stub;
    explicit GrpcRemSvcStub(shared_ptr<Channel> ch)
        : stub(RS::RemSvc::NewStub(ch)) {}

    grpc::Status Ping(grpc::ClientContext* ctx,
                      const RS::PingMsg& req,
                      RS::PongMsg* res) override
    { return stub->Ping(ctx, req, res); }

    grpc::Status GetStatus(grpc::ClientContext* ctx,
                           const RS::Empty& req,
                           RS::StatusMsg* res) override
    { return stub->GetStatus(ctx, req, res); }

    grpc::Status RemCmd(grpc::ClientContext* ctx,
                        const RS::RemCmdMsg& req,
                        RS::RemResMsg* res) override
    { return stub->RemCmd(ctx, req, res); }

    unique_ptr<RS::ICmdStrmClient> RemCmdStrm(grpc::ClientContext* ctx) override
    { return make_unique<GrpcCmdStrmClient>(stub->RemCmdStrm(ctx)); }
};

} // anonymous namespace


// ── RemSvcClient ──────────────────────────────────────────────────────────

RemSvcClient::RemSvcClient(shared_ptr<Channel> channel)
    : stub_(make_unique<GrpcRemSvcStub>(std::move(channel)))
{}

RemSvcClient::RemSvcClient(unique_ptr<IRemSvcStub> stub)
    : stub_(std::move(stub))
{}


PingResult RemSvcClient::doPing(int seq)
{
    PingMsg ping;
    PongMsg pong;

    ping.set_seq(seq);

    absl::Time now = absl::Now();
    ping.set_time(absl::ToUnixSeconds(now));
    string isoStr = absl::FormatTime(absl::RFC3339_full, now, absl::LocalTimeZone());
    Log(info, "Ping time: {}", isoStr);
    ping.set_data(isoStr.data());

    ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(30));
    grpc::Status status = stub_->Ping(&ctx, ping, &pong);
    if (status.ok()) {
        Log(info, "Ping seqId: {}", pong.seq());
        return {true, pong.seq()};
    }
    Log(error, "Ping failed {}: {}", (int)status.error_code(), status.error_message());
    return {false, 0};
}


StatusResult RemSvcClient::doGetStatus()
{
    Empty req;
    StatusMsg res;

    ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(30));
    grpc::Status status = stub_->GetStatus(&ctx, req, &res);
    if (status.ok()) {
        Log(info, "GetStatus rc={} msg={}", res.rc(), res.msg());
        return {true, res.rc(), res.msg()};
    }
    Log(error, "GetStatus RPC failed {}: {}", (int)status.error_code(), status.error_message());
    return {false, -1, {}};
}


int RemSvcClient::doRemCmd(std::string_view cmd, int cmdtyp, int tid)
{
    RemCmdMsg req;
    RemResMsg res;

    req.set_cmd(string(cmd));
    req.set_cmdtyp(cmdtyp);
    req.set_tid(tid);
    req.set_hsh(crc32Hex(cmd));   // integrity hash — server verifies this

    Log(info, "RemCmd sending cmd={}", cmd);
    ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(30));
    grpc::Status status = stub_->RemCmd(&ctx, req, &res);
    if (status.ok()) {
        int rv = res.rc();
        Log(info, "RemCmd rc={} out={}", rv, res.out());
        if (!res.err().empty())
            Log(error, "RemCmd err={}", res.err());
        return rv;
    }
    Log(error, "RemCmd RPC failed {}: {}", (int)status.error_code(), status.error_message());
    return 1;
}


int RemSvcClient::doRemCmdStrm(const vector<string>& cmds, int cmdtyp)
{
    ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(30));
    auto stream = stub_->RemCmdStrm(&ctx);

    // Send all commands (each with its CRC32 hash), then signal end-of-writes.
    for (const auto& cmd : cmds) {
        RemCmdMsg req;
        req.set_cmd(cmd);
        req.set_cmdtyp(cmdtyp);
        req.set_hsh(crc32Hex(cmd));
        Log(info, "RemCmdStrm sending cmd={}", cmd);
        if (!stream->Write(req)) {
            Log(error, "RemCmdStrm write failed");
            break;
        }
    }
    stream->WritesDone();

    // Read all responses.
    RemResMsg res;
    while (stream->Read(&res)) {
        Log(info, "RemCmdStrm rc={} out={}", res.rc(), res.out());
        if (!res.err().empty())
            Log(error, "RemCmdStrm err={}", res.err());
    }

    grpc::Status status = stream->Finish();
    if (!status.ok()) {
        Log(error, "RemCmdStrm RPC failed {}: {}",
            (int)status.error_code(), status.error_message());
        return 1;
    }
    return 0;
}


