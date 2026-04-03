#include <gtest/gtest.h>
#include <string>
#include <string_view>

#include "RemSvcServiceImpl.hh"


// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Fake runner that immediately returns controlled output — no process spawned.
static RS::CmdRunner makeFakeRunner(std::string out, std::string err, int rc)
{
    return [out, err, rc](std::string_view, int, std::string& o, std::string& e) {
        o = out;
        e = err;
        return rc;
    };
}


// ---------------------------------------------------------------------------
// RemCmd — output / return code
// ---------------------------------------------------------------------------

TEST(RemSvcServiceImpl, RemCmdSetsOutputOnSuccess)
{
    RS::RemSvcServiceImpl svc(makeFakeRunner("hello", "", 0));
    RS::RemCmdMsg req;  req.set_cmd("echo hello");
    RS::RemResMsg res;
    grpc::ServerContext ctx;

    auto status = svc.RemCmd(&ctx, &req, &res);

    EXPECT_TRUE(status.ok());
    EXPECT_EQ(res.out(), "hello");
    EXPECT_EQ(res.err(), "");
    EXPECT_EQ(res.rc(), 0);
}

TEST(RemSvcServiceImpl, RemCmdSetsNonZeroReturnCode)
{
    RS::RemSvcServiceImpl svc(makeFakeRunner("", "not found", 1));
    RS::RemCmdMsg req;  req.set_cmd("bad_cmd");
    RS::RemResMsg res;
    grpc::ServerContext ctx;

    auto status = svc.RemCmd(&ctx, &req, &res);

    EXPECT_TRUE(status.ok());   // gRPC status is OK — rc lives in the message
    EXPECT_EQ(res.rc(), 1);
    EXPECT_EQ(res.err(), "not found");
}


// ---------------------------------------------------------------------------
// RemCmd — runner forwarding (spy)
// ---------------------------------------------------------------------------

TEST(RemSvcServiceImpl, RemCmdForwardsCommandToRunner)
{
    std::string captured;
    auto spy = [&captured](std::string_view cmd, int, std::string& out, std::string& err) {
        captured = std::string(cmd);
        out = "";  err = "";
        return 0;
    };

    RS::RemSvcServiceImpl svc(spy);
    RS::RemCmdMsg req;  req.set_cmd("dir /b");
    RS::RemResMsg res;
    grpc::ServerContext ctx;

    svc.RemCmd(&ctx, &req, &res);

    EXPECT_EQ(captured, "dir /b");
}

TEST(RemSvcServiceImpl, RemCmdForwardsCmdTypToRunner)
{
    int capturedTyp = -1;
    auto spy = [&capturedTyp](std::string_view, int cmdtyp, std::string& out, std::string& err) {
        capturedTyp = cmdtyp;
        out = "";  err = "";
        return 0;
    };

    RS::RemSvcServiceImpl svc(spy);
    grpc::ServerContext ctx;

    {
        RS::RemCmdMsg req;  req.set_cmd("dir");  req.set_cmdtyp(0);
        RS::RemResMsg res;
        svc.RemCmd(&ctx, &req, &res);
        EXPECT_EQ(capturedTyp, 0);
    }
    {
        RS::RemCmdMsg req;  req.set_cmd("Get-Date");  req.set_cmdtyp(1);
        RS::RemResMsg res;
        svc.RemCmd(&ctx, &req, &res);
        EXPECT_EQ(capturedTyp, 1);
    }
}


// ---------------------------------------------------------------------------
// RemCmd — tid / hsh echo
// ---------------------------------------------------------------------------

TEST(RemSvcServiceImpl, RemCmdEchoesTransactionId)
{
    RS::RemSvcServiceImpl svc(makeFakeRunner("", "", 0));
    RS::RemCmdMsg req;  req.set_cmd("echo");  req.set_tid(9876);
    RS::RemResMsg res;
    grpc::ServerContext ctx;

    svc.RemCmd(&ctx, &req, &res);

    EXPECT_EQ(res.tid(), 9876);
}

TEST(RemSvcServiceImpl, RemCmdEchoesZeroTid)
{
    RS::RemSvcServiceImpl svc(makeFakeRunner("", "", 0));
    RS::RemCmdMsg req;  req.set_cmd("echo");  req.set_tid(0);
    RS::RemResMsg res;
    grpc::ServerContext ctx;

    svc.RemCmd(&ctx, &req, &res);

    EXPECT_EQ(res.tid(), 0);
}

TEST(RemSvcServiceImpl, RemCmdEchoesHash)
{
    // Hash must be the correct CRC32 of the command, otherwise the call is rejected.
    RS::RemSvcServiceImpl svc(makeFakeRunner("", "", 0));
    const std::string cmd = "echo";
    RS::RemCmdMsg req;  req.set_cmd(cmd);  req.set_hsh(RS::crc32Hex(cmd));
    RS::RemResMsg res;
    grpc::ServerContext ctx;

    auto status = svc.RemCmd(&ctx, &req, &res);

    EXPECT_TRUE(status.ok());
    EXPECT_EQ(res.hsh(), RS::crc32Hex(cmd));
}

TEST(RemSvcServiceImpl, RemCmdEchoesEmptyHash)
{
    RS::RemSvcServiceImpl svc(makeFakeRunner("", "", 0));
    RS::RemCmdMsg req;  req.set_cmd("echo");
    RS::RemResMsg res;
    grpc::ServerContext ctx;

    svc.RemCmd(&ctx, &req, &res);

    EXPECT_EQ(res.hsh(), "");
}


// ---------------------------------------------------------------------------
// Ping
// ---------------------------------------------------------------------------

TEST(RemSvcServiceImpl, PingEchoesSeqId)
{
    RS::RemSvcServiceImpl svc;   // default runner — Ping doesn't use it
    RS::PingMsg ping;  ping.set_seq(42);
    RS::PongMsg pong;
    grpc::ServerContext ctx;

    auto status = svc.Ping(&ctx, &ping, &pong);

    EXPECT_TRUE(status.ok());
    EXPECT_EQ(pong.seq(), 42);
}

TEST(RemSvcServiceImpl, PingEchoesZeroSeq)
{
    RS::RemSvcServiceImpl svc;
    RS::PingMsg ping;  ping.set_seq(0);
    RS::PongMsg pong;
    grpc::ServerContext ctx;

    auto status = svc.Ping(&ctx, &ping, &pong);

    EXPECT_TRUE(status.ok());
    EXPECT_EQ(pong.seq(), 0);
}

TEST(RemSvcServiceImpl, PingEchoesTime)
{
    RS::RemSvcServiceImpl svc;
    RS::PingMsg ping;  ping.set_seq(1);  ping.set_time(1234567890LL);
    RS::PongMsg pong;
    grpc::ServerContext ctx;

    auto status = svc.Ping(&ctx, &ping, &pong);

    EXPECT_TRUE(status.ok());
    EXPECT_EQ(pong.time(), 1234567890LL);
}

TEST(RemSvcServiceImpl, PingEchoesData)
{
    RS::RemSvcServiceImpl svc;
    RS::PingMsg ping;  ping.set_seq(2);  ping.set_data("hello");
    RS::PongMsg pong;
    grpc::ServerContext ctx;

    auto status = svc.Ping(&ctx, &ping, &pong);

    EXPECT_TRUE(status.ok());
    EXPECT_EQ(pong.data(), "hello");
}


// ---------------------------------------------------------------------------
// GetStatus
// ---------------------------------------------------------------------------

TEST(RemSvcServiceImpl, GetStatusReturnsOk)
{
    RS::RemSvcServiceImpl svc;
    RS::Empty req;
    RS::StatusMsg res;
    grpc::ServerContext ctx;

    auto status = svc.GetStatus(&ctx, &req, &res);

    EXPECT_TRUE(status.ok());
    EXPECT_EQ(res.rc(), 0);
}

TEST(RemSvcServiceImpl, GetStatusMsgIsNonEmpty)
{
    RS::RemSvcServiceImpl svc;
    RS::Empty req;
    RS::StatusMsg res;
    grpc::ServerContext ctx;

    svc.GetStatus(&ctx, &req, &res);

    EXPECT_FALSE(res.msg().empty());
}


// ---------------------------------------------------------------------------
// RemCmd — CRC32 hash verification
// ---------------------------------------------------------------------------

TEST(RemSvcServiceImpl, RemCmdHashMismatchReturnsError)
{
    RS::RemSvcServiceImpl svc(makeFakeRunner("out", "", 0));
    RS::RemCmdMsg req;  req.set_cmd("dir /b");  req.set_hsh("00000000");
    RS::RemResMsg res;
    grpc::ServerContext ctx;

    auto status = svc.RemCmd(&ctx, &req, &res);

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    // Runner must NOT have been called — res.out stays empty.
    EXPECT_EQ(res.out(), "");
}

TEST(RemSvcServiceImpl, RemCmdValidHashAllowsExecution)
{
    RS::RemSvcServiceImpl svc(makeFakeRunner("result", "", 0));
    const std::string cmd = "dir /b";
    RS::RemCmdMsg req;  req.set_cmd(cmd);  req.set_hsh(RS::crc32Hex(cmd));
    RS::RemResMsg res;
    grpc::ServerContext ctx;

    auto status = svc.RemCmd(&ctx, &req, &res);

    EXPECT_TRUE(status.ok());
    EXPECT_EQ(res.rc(), 0);
    EXPECT_EQ(res.out(), "result");
}

TEST(RemSvcServiceImpl, RemCmdEmptyHashSkipsVerification)
{
    // Empty hsh means no integrity check — command always runs.
    RS::RemSvcServiceImpl svc(makeFakeRunner("ok", "", 0));
    RS::RemCmdMsg req;  req.set_cmd("anything");  // hsh left empty
    RS::RemResMsg res;
    grpc::ServerContext ctx;

    auto status = svc.RemCmd(&ctx, &req, &res);

    EXPECT_TRUE(status.ok());
    EXPECT_EQ(res.out(), "ok");
}

TEST(RemSvcServiceImpl, Crc32HexIsEightLowercaseHexDigits)
{
    std::string h = RS::crc32Hex("hello world");
    ASSERT_EQ(h.size(), 8u);
    for (char c : h)
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
}

TEST(RemSvcServiceImpl, Crc32HexKnownValue)
{
    // CRC-32 of "123456789" is 0xCBF43926 — standard check value.
    EXPECT_EQ(RS::crc32Hex("123456789"), "cbf43926");
}


// ---------------------------------------------------------------------------
// RemCmdStrm — fake stream helper
// ---------------------------------------------------------------------------

// Plays back a fixed list of requests; captures written responses.
struct FakeCmdStream : RS::ICmdStream {
    std::vector<RS::RemCmdMsg> requests;
    std::vector<RS::RemResMsg> responses;
    std::size_t idx = 0;

    bool Read(RS::RemCmdMsg* msg) override {
        if (idx >= requests.size()) return false;
        *msg = requests[idx++];
        return true;
    }
    bool Write(const RS::RemResMsg& msg) override {
        responses.push_back(msg);
        return true;
    }
};

static RS::RemCmdMsg makeReq(std::string cmd, int tid = 0, std::string hsh = "")
{
    RS::RemCmdMsg r;
    r.set_cmd(std::move(cmd));
    r.set_tid(tid);
    r.set_hsh(std::move(hsh));
    return r;
}


// ---------------------------------------------------------------------------
// RemCmdStrm — processStream tests
// ---------------------------------------------------------------------------

TEST(RemSvcServiceImpl, RemCmdStrmEmptyStreamReturnsOk)
{
    RS::RemSvcServiceImpl svc(makeFakeRunner("", "", 0));
    FakeCmdStream stream;  // no requests

    auto status = svc.processStream(stream);

    EXPECT_TRUE(status.ok());
    EXPECT_TRUE(stream.responses.empty());
}

TEST(RemSvcServiceImpl, RemCmdStrmSingleCommand)
{
    RS::RemSvcServiceImpl svc(makeFakeRunner("hello", "", 0));
    FakeCmdStream stream;
    stream.requests = { makeReq("echo hello") };

    auto status = svc.processStream(stream);

    EXPECT_TRUE(status.ok());
    ASSERT_EQ(stream.responses.size(), 1u);
    EXPECT_EQ(stream.responses[0].out(), "hello");
    EXPECT_EQ(stream.responses[0].rc(),  0);
}

TEST(RemSvcServiceImpl, RemCmdStrmMultipleCommandsInOrder)
{
    // Runner returns "out_N" for the Nth call.
    int call = 0;
    auto runner = [&call](std::string_view, int, std::string& out, std::string& err) {
        out = "out_" + std::to_string(++call);
        err = "";
        return 0;
    };

    RS::RemSvcServiceImpl svc(runner);
    FakeCmdStream stream;
    stream.requests = { makeReq("cmd1"), makeReq("cmd2"), makeReq("cmd3") };

    auto status = svc.processStream(stream);

    EXPECT_TRUE(status.ok());
    ASSERT_EQ(stream.responses.size(), 3u);
    EXPECT_EQ(stream.responses[0].out(), "out_1");
    EXPECT_EQ(stream.responses[1].out(), "out_2");
    EXPECT_EQ(stream.responses[2].out(), "out_3");
}

TEST(RemSvcServiceImpl, RemCmdStrmEchoesTidPerMessage)
{
    RS::RemSvcServiceImpl svc(makeFakeRunner("", "", 0));
    FakeCmdStream stream;
    stream.requests = { makeReq("a", 10), makeReq("b", 20) };

    svc.processStream(stream);

    ASSERT_EQ(stream.responses.size(), 2u);
    EXPECT_EQ(stream.responses[0].tid(), 10);
    EXPECT_EQ(stream.responses[1].tid(), 20);
}

TEST(RemSvcServiceImpl, RemCmdStrmEchoesHashPerMessage)
{
    RS::RemSvcServiceImpl svc(makeFakeRunner("", "", 0));
    const std::string cmd = "dir";
    FakeCmdStream stream;
    stream.requests = { makeReq(cmd, 0, RS::crc32Hex(cmd)) };

    svc.processStream(stream);

    ASSERT_EQ(stream.responses.size(), 1u);
    EXPECT_EQ(stream.responses[0].hsh(), RS::crc32Hex(cmd));
}

TEST(RemSvcServiceImpl, RemCmdStrmHashMismatchAbortsStream)
{
    // First message has bad hash — stream must stop; second must NOT run.
    int callCount = 0;
    auto runner = [&callCount](std::string_view, int, std::string& out, std::string& err) {
        ++callCount;  out = "";  err = "";  return 0;
    };

    RS::RemSvcServiceImpl svc(runner);
    FakeCmdStream stream;
    stream.requests = { makeReq("cmd1", 0, "00000000"),
                        makeReq("cmd2") };

    auto status = svc.processStream(stream);

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(callCount, 0);          // runner never called
    EXPECT_TRUE(stream.responses.empty());
}

TEST(RemSvcServiceImpl, RemCmdStrmValidHashAllowsExecution)
{
    RS::RemSvcServiceImpl svc(makeFakeRunner("ok", "", 0));
    const std::string cmd = "dir /b";
    FakeCmdStream stream;
    stream.requests = { makeReq(cmd, 0, RS::crc32Hex(cmd)) };

    auto status = svc.processStream(stream);

    EXPECT_TRUE(status.ok());
    ASSERT_EQ(stream.responses.size(), 1u);
    EXPECT_EQ(stream.responses[0].out(), "ok");
}

TEST(RemSvcServiceImpl, RemCmdStrmNonZeroReturnCode)
{
    RS::RemSvcServiceImpl svc(makeFakeRunner("", "bad", 42));
    FakeCmdStream stream;
    stream.requests = { makeReq("bad_cmd") };

    auto status = svc.processStream(stream);

    EXPECT_TRUE(status.ok());   // gRPC status OK — rc lives in the message
    ASSERT_EQ(stream.responses.size(), 1u);
    EXPECT_EQ(stream.responses[0].rc(),  42);
    EXPECT_EQ(stream.responses[0].err(), "bad");
}
