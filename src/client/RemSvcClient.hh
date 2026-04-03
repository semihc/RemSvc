#pragma once
#ifndef REMSVC_CLIENT_HH
#define REMSVC_CLIENT_HH

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <grpcpp/grpcpp.h>
#include "RemSvc.grpc.pb.h"

namespace RS {

// ── Value types ───────────────────────────────────────────────────────────

// Result returned by doPing.
struct PingResult {
    bool ok{false};    // true if RPC succeeded
    int  seqEcho{0};   // seq echoed back in PongMsg
};

// Result returned by doGetStatus.
struct StatusResult {
    bool        ok{false};   // true if RPC succeeded
    int         rc{-1};      // StatusMsg.rc from the server
    std::string msg{};       // StatusMsg.msg from the server
};


// ── Thin injectable interfaces ─────────────────────────────────────────────

// Bidirectional streaming handle — wraps ClientReaderWriter or a test fake.
struct ICmdStrmClient {
    virtual ~ICmdStrmClient() = default;
    virtual bool Write(const RS::RemCmdMsg& msg) = 0;
    virtual bool Read(RS::RemResMsg* msg) = 0;
    virtual void WritesDone() = 0;
    virtual grpc::Status Finish() = 0;
};

// Unary + streaming stub — wraps the gRPC Stub or a test fake.
struct IRemSvcStub {
    virtual ~IRemSvcStub() = default;
    virtual grpc::Status Ping(grpc::ClientContext*, const RS::PingMsg&, RS::PongMsg*) = 0;
    virtual grpc::Status GetStatus(grpc::ClientContext*, const RS::Empty&, RS::StatusMsg*) = 0;
    virtual grpc::Status RemCmd(grpc::ClientContext*, const RS::RemCmdMsg&, RS::RemResMsg*) = 0;
    virtual std::unique_ptr<ICmdStrmClient> RemCmdStrm(grpc::ClientContext*) = 0;
};


// ── Client ────────────────────────────────────────────────────────────────

class RemSvcClient {
public:
    // Production: build from a real gRPC channel.
    explicit RemSvcClient(std::shared_ptr<grpc::Channel> channel);

    // Test injection: supply a pre-built stub fake.
    explicit RemSvcClient(std::unique_ptr<IRemSvcStub> stub);

    // Health-check ping. Returns PingResult{ok, seqEcho}.
    PingResult doPing(int seq = 0);

    // Query server status. Returns StatusResult{ok, rc, msg}.
    StatusResult doGetStatus();

    // Execute a single command on the server.
    // Computes and sends CRC32 hash for server-side integrity verification.
    // Returns the process exit code, or 1 on RPC failure.
    int doRemCmd(std::string_view cmd, int cmdtyp = 0, int tid = 0);

    // Execute a stream of commands on the server (bidirectional streaming).
    // Sends each command with CRC32 hash; logs each result.
    // Returns 0 on success, 1 on RPC failure.
    int doRemCmdStrm(const std::vector<std::string>& cmds, int cmdtyp = 0);

private:
    std::unique_ptr<IRemSvcStub> stub_;
};

} // namespace RS

#endif // REMSVC_CLIENT_HH
