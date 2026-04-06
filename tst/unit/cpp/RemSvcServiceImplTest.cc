#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <thread>
#include <chrono>

#include "RemSvcServiceImpl.hh"


// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Fake runner that immediately returns controlled output — no process spawned.
// The cmdusr parameter is captured for inspection by tests that need it.
static RS::CmdRunner makeFakeRunner(std::string out, std::string err, int rc)
{
    return [out, err, rc](std::string_view, int, std::string_view,
                          std::string& o, std::string& e) {
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
    auto spy = [&captured](std::string_view cmd, int, std::string_view,
                           std::string& out, std::string& err) {
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
    auto spy = [&capturedTyp](std::string_view, int cmdtyp, std::string_view,
                              std::string& out, std::string& err) {
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
// RemCmd — cmdusr forwarding
// ---------------------------------------------------------------------------

TEST(RemSvcServiceImpl, RemCmdForwardsCmdusr)
{
    std::string capturedUser;
    auto spy = [&capturedUser](std::string_view, int, std::string_view cmdusr,
                               std::string& out, std::string& err) {
        capturedUser = std::string(cmdusr);
        out = "";  err = "";
        return 0;
    };

    RS::RemSvcServiceImpl svc(spy);
    RS::RemCmdMsg req;
    req.set_cmd("echo hi");
    req.set_cmdusr("deploy");
    RS::RemResMsg res;
    grpc::ServerContext ctx;

    svc.RemCmd(&ctx, &req, &res);

    EXPECT_EQ(capturedUser, "deploy");
}

TEST(RemSvcServiceImpl, RemCmdForwardsEmptyCmdusr)
{
    std::string capturedUser{"unset"};
    auto spy = [&capturedUser](std::string_view, int, std::string_view cmdusr,
                               std::string& out, std::string& err) {
        capturedUser = std::string(cmdusr);
        out = "";  err = "";
        return 0;
    };

    RS::RemSvcServiceImpl svc(spy);
    RS::RemCmdMsg req;  req.set_cmd("echo hi");  // cmdusr left empty
    RS::RemResMsg res;
    grpc::ServerContext ctx;

    svc.RemCmd(&ctx, &req, &res);

    EXPECT_EQ(capturedUser, "");
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

TEST(RemSvcServiceImpl, GetStatusMsgContainsStateOK)
{
    RS::RemSvcServiceImpl svc;
    RS::Empty req;
    RS::StatusMsg res;
    grpc::ServerContext ctx;

    svc.GetStatus(&ctx, &req, &res);

    EXPECT_NE(res.msg().find("state=OK"), std::string::npos);
}

TEST(RemSvcServiceImpl, GetStatusMsgContainsUptime)
{
    RS::RemSvcServiceImpl svc;
    RS::Empty req;
    RS::StatusMsg res;
    grpc::ServerContext ctx;

    svc.GetStatus(&ctx, &req, &res);

    EXPECT_NE(res.msg().find("uptime="), std::string::npos);
}

TEST(RemSvcServiceImpl, GetStatusMsgContainsCommandCount)
{
    RS::RemSvcServiceImpl svc(makeFakeRunner("", "", 0));
    grpc::ServerContext ctx;

    // Execute two commands so the counter advances.
    { RS::RemCmdMsg req; req.set_cmd("a"); RS::RemResMsg res; svc.RemCmd(&ctx, &req, &res); }
    { RS::RemCmdMsg req; req.set_cmd("b"); RS::RemResMsg res; svc.RemCmd(&ctx, &req, &res); }

    RS::Empty sreq;
    RS::StatusMsg sres;
    svc.GetStatus(&ctx, &sreq, &sres);

    EXPECT_NE(sres.msg().find("commands=2"), std::string::npos);
}


// ---------------------------------------------------------------------------
// Allowlist
// ---------------------------------------------------------------------------

TEST(RemSvcServiceImpl, AllowlistEmptyPermitsAllCommands)
{
    // No allowlist — any command runs.
    RS::RemSvcServiceImpl svc(makeFakeRunner("ok", "", 0), {});
    RS::RemCmdMsg req;  req.set_cmd("anything");
    RS::RemResMsg res;
    grpc::ServerContext ctx;

    auto status = svc.RemCmd(&ctx, &req, &res);

    EXPECT_TRUE(status.ok());
    EXPECT_EQ(res.out(), "ok");
}

TEST(RemSvcServiceImpl, AllowlistPermitsMatchingPattern)
{
    // Regex: command must start with "echo" or "dir"
    RS::RemSvcServiceImpl svc(makeFakeRunner("ok", "", 0), {"^echo\\b", "^dir\\b"});
    RS::RemCmdMsg req;  req.set_cmd("echo hello");
    RS::RemResMsg res;
    grpc::ServerContext ctx;

    auto status = svc.RemCmd(&ctx, &req, &res);

    EXPECT_TRUE(status.ok());
    EXPECT_EQ(res.out(), "ok");
}

TEST(RemSvcServiceImpl, AllowlistBlocksNonMatchingCommand)
{
    RS::RemSvcServiceImpl svc(makeFakeRunner("ok", "", 0), {"^echo\\b"});
    RS::RemCmdMsg req;  req.set_cmd("rm -rf /");
    RS::RemResMsg res;
    grpc::ServerContext ctx;

    auto status = svc.RemCmd(&ctx, &req, &res);

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    EXPECT_EQ(res.out(), "");   // runner must NOT have been called
}

TEST(RemSvcServiceImpl, AllowlistRegexMatchesSubstring)
{
    // Pattern without anchoring matches anywhere in the command.
    RS::RemSvcServiceImpl svc(makeFakeRunner("ok", "", 0), {"Get-\\w+"});
    RS::RemCmdMsg req;  req.set_cmd("Get-Date");
    RS::RemResMsg res;
    grpc::ServerContext ctx;

    EXPECT_TRUE(svc.RemCmd(&ctx, &req, &res).ok());
}

TEST(RemSvcServiceImpl, AllowlistBadRegexActivatesDenyAll)
{
    // "[" is an invalid ECMAScript regex — construction must not throw but must
    // silently activate deny-all mode so the server stays up while rejecting
    // every command until the config is corrected.
    RS::RemSvcServiceImpl svc(makeFakeRunner("ok", "", 0), {"["}); // invalid pattern
    RS::RemCmdMsg req;  req.set_cmd("echo hello");
    RS::RemResMsg res;
    grpc::ServerContext ctx;

    auto status = svc.RemCmd(&ctx, &req, &res);

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    EXPECT_NE(status.error_message().find("invalid"), std::string::npos);
    EXPECT_EQ(res.out(), "");   // runner must NOT have been called
}

TEST(RemSvcServiceImpl, AllowlistBadRegexBlocksStreamToo)
{
    // Same deny-all must apply to processStream, not just RemCmd.
    RS::RemSvcServiceImpl svc(makeFakeRunner("ok", "", 0), {"["}); // invalid
    FakeCmdStream stream;
    stream.requests = { makeReq("echo hi") };

    auto status = svc.processStream(stream);

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    EXPECT_TRUE(stream.responses.empty());
}

TEST(RemSvcServiceImpl, AllowlistMixedValidAndBadPatternDeniesAll)
{
    // Even if the first pattern is valid, a later bad pattern triggers deny-all.
    RS::RemSvcServiceImpl svc(makeFakeRunner("ok", "", 0), {"^echo\\b", "[bad"});
    RS::RemCmdMsg req;  req.set_cmd("echo hello");
    RS::RemResMsg res;
    grpc::ServerContext ctx;

    auto status = svc.RemCmd(&ctx, &req, &res);

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
}

// ---------------------------------------------------------------------------
// Output truncation (256 KB limit)
// ---------------------------------------------------------------------------

TEST(RemSvcServiceImpl, OutputTruncatedAt256KB)
{
    const std::string big(RS::kMaxOutputBytes + 100, 'x');
    RS::RemSvcServiceImpl svc(makeFakeRunner(big, "", 0));
    RS::RemCmdMsg req;  req.set_cmd("flood");
    RS::RemResMsg res;
    grpc::ServerContext ctx;

    svc.RemCmd(&ctx, &req, &res);

    // Truncated at kMaxOutputBytes then the sentinel marker appended.
    EXPECT_EQ(res.out().size(), RS::kMaxOutputBytes + RS::kTruncationMarker.size());
    EXPECT_NE(res.out().find(std::string(RS::kTruncationMarker)), std::string::npos);
    // SOH sentinel bytes present at start and end of the marker.
    EXPECT_NE(res.out().find('\x01'), std::string::npos);
}

TEST(RemSvcServiceImpl, ErrorTruncatedAt256KB)
{
    const std::string big(RS::kMaxOutputBytes + 100, 'e');
    RS::RemSvcServiceImpl svc(makeFakeRunner("", big, 1));
    RS::RemCmdMsg req;  req.set_cmd("flood");
    RS::RemResMsg res;
    grpc::ServerContext ctx;

    svc.RemCmd(&ctx, &req, &res);

    EXPECT_EQ(res.err().size(), RS::kMaxOutputBytes + RS::kTruncationMarker.size());
    EXPECT_NE(res.err().find(std::string(RS::kTruncationMarker)), std::string::npos);
}

TEST(RemSvcServiceImpl, OutputBelowLimitNotTruncated)
{
    const std::string small(100, 'x');
    RS::RemSvcServiceImpl svc(makeFakeRunner(small, "", 0));
    RS::RemCmdMsg req;  req.set_cmd("small");
    RS::RemResMsg res;
    grpc::ServerContext ctx;

    svc.RemCmd(&ctx, &req, &res);

    EXPECT_EQ(res.out(), small);
    EXPECT_EQ(res.out().find('\x01'), std::string::npos);  // no sentinel
}

TEST(RemSvcServiceImpl, TruncationMarkerContainsSentinelBytes)
{
    // Verify the marker itself has the SOH bytes that make it unambiguous.
    EXPECT_EQ(RS::kTruncationMarker.front(), '\x01');
    EXPECT_EQ(RS::kTruncationMarker.back(),  '\n');
    EXPECT_NE(RS::kTruncationMarker.find('\x01'), std::string_view::npos);
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

TEST(RemSvcServiceImpl, AllowlistBlocksInStream)
{
    RS::RemSvcServiceImpl svc(makeFakeRunner("ok", "", 0), {"^echo\\b"});
    FakeCmdStream stream;
    stream.requests = { makeReq("rm -rf /") };

    auto status = svc.processStream(stream);

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    EXPECT_TRUE(stream.responses.empty());
}

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
    auto runner = [&call](std::string_view, int, std::string_view,
                          std::string& out, std::string& err) {
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
    auto runner = [&callCount](std::string_view, int, std::string_view,
                               std::string& out, std::string& err) {
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

TEST(RemSvcServiceImpl, RemCmdStrmForwardsCmdusr)
{
    std::string capturedUser;
    auto spy = [&capturedUser](std::string_view, int, std::string_view cmdusr,
                               std::string& out, std::string& err) {
        capturedUser = std::string(cmdusr);
        out = "";  err = "";
        return 0;
    };

    RS::RemSvcServiceImpl svc(spy);
    FakeCmdStream stream;
    RS::RemCmdMsg req;
    req.set_cmd("echo hi");
    req.set_cmdusr("alice");
    stream.requests = { req };

    svc.processStream(stream);

    EXPECT_EQ(capturedUser, "alice");
}

TEST(RemSvcServiceImpl, RemCmdStrmEmptyCmdusrForwarded)
{
    std::string capturedUser{"unset"};
    auto spy = [&capturedUser](std::string_view, int, std::string_view cmdusr,
                               std::string& out, std::string& err) {
        capturedUser = std::string(cmdusr);
        out = "";  err = "";
        return 0;
    };

    RS::RemSvcServiceImpl svc(spy);
    FakeCmdStream stream;
    stream.requests = { makeReq("echo hi") };   // cmdusr left empty

    svc.processStream(stream);

    EXPECT_EQ(capturedUser, "");
}


// ---------------------------------------------------------------------------
// stdin handling — closeWriteChannel() delivers EOF to child
// ---------------------------------------------------------------------------

// A command that reads from stdin must exit promptly (receiving EOF) rather
// than blocking until the timeout deadline.  We measure wall-clock time and
// require it to complete well under the 30 s default timeout.
//
// Platform-specific commands that read all of stdin and exit on EOF:
//   Linux:   "cat"   — reads stdin until EOF, exits 0
//   Windows: "more"  — reads stdin until EOF, exits 0
//
// Both exit immediately when stdin is closed (EOF), which is exactly what
// closeWriteChannel() achieves.
TEST(RemSvcServiceImpl, StdinReadingCommandExitsPromptlyOnEof)
{
    std::string out, err;

#ifdef _WIN32
    // "more" on Windows reads from stdin when no file argument is given.
    const int rc = RS::runInProcess("more", 0, "", out, err, /*timeoutMs=*/30000);
#else
    // "cat" with no arguments reads from stdin until EOF.
    const int rc = RS::runInProcess("cat", 0, "", out, err, /*timeoutMs=*/30000);
#endif

    // The process must NOT have been killed by the timeout.
    // "process timed out" in err is the sentinel set by runInProcess on expiry.
    EXPECT_NE(err, "process timed out")
        << "stdin-reading command was not given EOF — closeWriteChannel() may be missing";

    // cat/more exit 0 on clean EOF
    EXPECT_EQ(rc, 0);
}
