#include <gtest/gtest.h>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>
#include "RemSvcClient.hh"
#include "Hash.hh"


// ---------------------------------------------------------------------------
// Fake stub infrastructure
// ---------------------------------------------------------------------------

// Fake stub: controls Ping / RemCmd / RemCmdStrm behaviour.
// After the client is constructed with a FakeStub, keep a raw pointer to
// inspect captured state (the client owns the unique_ptr).
struct FakeStub : RS::IRemSvcStub {
    // Ping
    grpc::Status pingStatus{grpc::Status::OK};
    RS::PongMsg  pingResponse;

    // GetStatus
    grpc::Status   statusRpcStatus{grpc::Status::OK};
    RS::StatusMsg  statusResponse;

    // RemCmd
    grpc::Status  remCmdStatus{grpc::Status::OK};
    RS::RemResMsg remCmdResponse;
    RS::RemCmdMsg capturedRemCmdReq;

    // RemCmdStrm — results stored here so they outlive the stream object
    std::vector<RS::RemCmdMsg> strmWritten;
    std::vector<RS::RemResMsg> strmResponses;
    grpc::Status strmFinishStatus{grpc::Status::OK};

    grpc::Status Ping(grpc::ClientContext*,
                      const RS::PingMsg&  req,
                      RS::PongMsg*        res) override
    {
        if (pingStatus.ok()) *res = pingResponse;
        return pingStatus;
    }

    grpc::Status GetStatus(grpc::ClientContext*,
                           const RS::Empty&,
                           RS::StatusMsg* res) override
    {
        if (statusRpcStatus.ok()) *res = statusResponse;
        return statusRpcStatus;
    }

    grpc::Status RemCmd(grpc::ClientContext*,
                        const RS::RemCmdMsg& req,
                        RS::RemResMsg*       res) override
    {
        capturedRemCmdReq = req;
        if (remCmdStatus.ok()) *res = remCmdResponse;
        return remCmdStatus;
    }

    std::unique_ptr<RS::ICmdStrmClient> RemCmdStrm(grpc::ClientContext*) override;
};

// Stream view: writes directly into FakeStub's storage (which outlives this object).
struct FakeStreamView : RS::ICmdStrmClient {
    FakeStub& parent;
    std::size_t read_idx = 0;
    explicit FakeStreamView(FakeStub& p) : parent(p) {}

    bool Write(const RS::RemCmdMsg& msg) override {
        parent.strmWritten.push_back(msg); return true;
    }
    bool Read(RS::RemResMsg* msg) override {
        if (read_idx >= parent.strmResponses.size()) return false;
        *msg = parent.strmResponses[read_idx++]; return true;
    }
    void WritesDone() override {}
    grpc::Status Finish() override { return parent.strmFinishStatus; }
};

std::unique_ptr<RS::ICmdStrmClient> FakeStub::RemCmdStrm(grpc::ClientContext*)
{
    return std::make_unique<FakeStreamView>(*this);
}


// Helper: build a RemSvcClient with a FakeStub and return the raw stub pointer.
static std::pair<RS::RemSvcClient, FakeStub*> makeClient()
{
    auto stub = std::make_unique<FakeStub>();
    FakeStub* ptr = stub.get();
    return { RS::RemSvcClient(std::move(stub)), ptr };
}


// ---------------------------------------------------------------------------
// doPing
// ---------------------------------------------------------------------------

TEST(RemSvcClient, PingReturnsTrueOnOkStatus)
{
    auto [client, stub] = makeClient();
    stub->pingResponse.set_seq(7);

    RS::PingResult pr = client.doPing(7);
    EXPECT_TRUE(pr.ok);
    EXPECT_EQ(pr.seqEcho, 7);
}

TEST(RemSvcClient, PingReturnsFalseOnRpcError)
{
    auto [client, stub] = makeClient();
    stub->pingStatus = grpc::Status(grpc::StatusCode::UNAVAILABLE, "down");

    RS::PingResult pr = client.doPing(1);
    EXPECT_FALSE(pr.ok);
}


// ---------------------------------------------------------------------------
// doGetStatus
// ---------------------------------------------------------------------------

TEST(RemSvcClient, GetStatusReturnsTrueOnOkStatus)
{
    auto [client, stub] = makeClient();
    stub->statusResponse.set_rc(0);
    stub->statusResponse.set_msg("running");

    RS::StatusResult sr = client.doGetStatus();
    EXPECT_TRUE(sr.ok);
    EXPECT_EQ(sr.rc, 0);
    EXPECT_EQ(sr.msg, "running");
}

TEST(RemSvcClient, GetStatusReturnsFalseOnRpcError)
{
    auto [client, stub] = makeClient();
    stub->statusRpcStatus = grpc::Status(grpc::StatusCode::UNAVAILABLE, "down");

    RS::StatusResult sr = client.doGetStatus();
    EXPECT_FALSE(sr.ok);
}


// ---------------------------------------------------------------------------
// doRemCmd
// ---------------------------------------------------------------------------

TEST(RemSvcClient, RemCmdForwardsCommand)
{
    auto [client, stub] = makeClient();

    client.doRemCmd("dir /b");

    EXPECT_EQ(stub->capturedRemCmdReq.cmd(), "dir /b");
}

TEST(RemSvcClient, RemCmdSendsCrc32Hash)
{
    auto [client, stub] = makeClient();
    const std::string cmd = "dir /b";

    client.doRemCmd(cmd);

    EXPECT_EQ(stub->capturedRemCmdReq.hsh(), RS::crc32Hex(cmd));
}

TEST(RemSvcClient, RemCmdForwardsCmdtyp)
{
    auto [client, stub] = makeClient();

    client.doRemCmd("Get-Date", /*cmdtyp=*/1);

    EXPECT_EQ(stub->capturedRemCmdReq.cmdtyp(), 1);
}

TEST(RemSvcClient, RemCmdForwardsTid)
{
    auto [client, stub] = makeClient();

    client.doRemCmd("dir", /*cmdtyp=*/0, /*tid=*/42);

    EXPECT_EQ(stub->capturedRemCmdReq.tid(), 42);
}

TEST(RemSvcClient, RemCmdReturnsExitCode)
{
    auto [client, stub] = makeClient();
    stub->remCmdResponse.set_rc(5);

    EXPECT_EQ(client.doRemCmd("cmd"), 5);
}

TEST(RemSvcClient, RemCmdReturnsOneOnRpcFailure)
{
    auto [client, stub] = makeClient();
    stub->remCmdStatus = grpc::Status(grpc::StatusCode::UNAVAILABLE, "down");

    EXPECT_EQ(client.doRemCmd("cmd"), 1);
}


// ---------------------------------------------------------------------------
// doRemCmdStrm
// ---------------------------------------------------------------------------

TEST(RemSvcClient, RemCmdStrmSendsAllCommands)
{
    auto [client, stub] = makeClient();

    client.doRemCmdStrm({"cmd1", "cmd2", "cmd3"});

    ASSERT_EQ(stub->strmWritten.size(), 3u);
    EXPECT_EQ(stub->strmWritten[0].cmd(), "cmd1");
    EXPECT_EQ(stub->strmWritten[1].cmd(), "cmd2");
    EXPECT_EQ(stub->strmWritten[2].cmd(), "cmd3");
}

TEST(RemSvcClient, RemCmdStrmSendsCrc32HashPerCommand)
{
    auto [client, stub] = makeClient();
    const std::string cmd = "echo hello";

    client.doRemCmdStrm({cmd});

    ASSERT_EQ(stub->strmWritten.size(), 1u);
    EXPECT_EQ(stub->strmWritten[0].hsh(), RS::crc32Hex(cmd));
}

TEST(RemSvcClient, RemCmdStrmReturnsZeroOnSuccess)
{
    auto [client, stub] = makeClient();
    EXPECT_EQ(client.doRemCmdStrm({"cmd"}), 0);
}

TEST(RemSvcClient, RemCmdStrmReturnsOneOnRpcFailure)
{
    auto [client, stub] = makeClient();
    stub->strmFinishStatus = grpc::Status(grpc::StatusCode::UNAVAILABLE, "down");

    EXPECT_EQ(client.doRemCmdStrm({"cmd"}), 1);
}

TEST(RemSvcClient, RemCmdStrmEmptyCommandListReturnsZero)
{
    auto [client, stub] = makeClient();
    EXPECT_EQ(client.doRemCmdStrm({}), 0);
}
