#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <thread>
#include <chrono>
#include <vector>

#include <grpcpp/grpcpp.h>
#include "GrpcServerThread.hh"
#include "RemSvcClient.hh"
#include "Hash.hh"
#include "Log.hh"

// High port — avoids clashing with a running RemSvc server.
static constexpr int kTestPort = 50999;


// ── Test fixture ─────────────────────────────────────────────────────────

class GrpcRoundTripTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        RS::InitLogging();
        server_ = std::make_unique<GrpcServerThread>(kTestPort);
        server_->start();
        // Give the gRPC server a moment to begin accepting connections.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    static void TearDownTestSuite() {
        server_->stop();
        RS::TermLogging();
    }

    RS::RemSvcClient makeClient() {
        auto ch = grpc::CreateChannel(
            "localhost:" + std::to_string(kTestPort),
            grpc::InsecureChannelCredentials());
        return RS::RemSvcClient(std::move(ch));
    }

    static std::unique_ptr<GrpcServerThread> server_;
};

std::unique_ptr<GrpcServerThread> GrpcRoundTripTest::server_;


// ── Ping ─────────────────────────────────────────────────────────────────

TEST_F(GrpcRoundTripTest, PingReturnsSuccess)
{
    auto client = makeClient();
    RS::PingResult pr = client.doPing(1);
    EXPECT_TRUE(pr.ok);
    EXPECT_EQ(pr.seqEcho, 1);
}


// ── RemCmd ───────────────────────────────────────────────────────────────

TEST_F(GrpcRoundTripTest, RemCmdEchoReturnsZero)
{
    // "echo hello" works on both cmd.exe and /bin/sh.
    auto client = makeClient();
    EXPECT_EQ(client.doRemCmd("echo hello"), 0);
}

TEST_F(GrpcRoundTripTest, RemCmdNonExistentCommandReturnsNonZero)
{
    // A command that doesn't exist returns non-zero on all platforms.
    auto client = makeClient();
    int rc = client.doRemCmd("__no_such_command_remsvc_test__");
    EXPECT_NE(rc, 0);
}

// Gap 5: CRC32 mismatch must be rejected end-to-end.
// We build the request manually (bypassing the client helper) so we can
// send a deliberately wrong hash and inspect the gRPC status code.
TEST_F(GrpcRoundTripTest, RemCmdBadHashIsRejected)
{
    auto ch = grpc::CreateChannel(
        "localhost:" + std::to_string(kTestPort),
        grpc::InsecureChannelCredentials());
    auto stub = RS::RemSvc::NewStub(ch);

    RS::RemCmdMsg req;
    req.set_cmd("echo hello");
    req.set_hsh("deadbeef");   // wrong hash — correct would be crc32Hex("echo hello")

    RS::RemResMsg res;
    grpc::ClientContext ctx;
    grpc::Status status = stub->RemCmd(&ctx, req, &res);

    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    EXPECT_NE(status.error_message().find("hash mismatch"), std::string::npos);
}


// Gap 6: verify full RemResMsg response structure round-trip.
TEST_F(GrpcRoundTripTest, RemCmdResponseFieldsArePopulated)
{
    auto ch = grpc::CreateChannel(
        "localhost:" + std::to_string(kTestPort),
        grpc::InsecureChannelCredentials());
    auto stub = RS::RemSvc::NewStub(ch);

    const std::string cmd = "echo hello";
    const std::string hash = RS::crc32Hex(cmd);

    RS::RemCmdMsg req;
    req.set_cmd(cmd);
    req.set_tid(99);
    req.set_hsh(hash);

    RS::RemResMsg res;
    grpc::ClientContext ctx;
    ASSERT_TRUE(stub->RemCmd(&ctx, req, &res).ok());

    EXPECT_EQ(res.rc(),  0);
    EXPECT_EQ(res.tid(), 99);
    EXPECT_EQ(res.hsh(), hash);        // server echoes the hash back
    EXPECT_FALSE(res.out().empty());   // "echo hello" produces output
}


// ── GetStatus ────────────────────────────────────────────────────────────

TEST_F(GrpcRoundTripTest, GetStatusReturnsOk)
{
    auto client = makeClient();
    RS::StatusResult sr = client.doGetStatus();
    EXPECT_TRUE(sr.ok);
    EXPECT_EQ(sr.rc, 0);
    EXPECT_NE(sr.msg.find("state=OK"),  std::string::npos);
    EXPECT_NE(sr.msg.find("uptime="),   std::string::npos);
    EXPECT_NE(sr.msg.find("commands="), std::string::npos);
}


// ── RemCmdStrm ───────────────────────────────────────────────────────────

TEST_F(GrpcRoundTripTest, RemCmdStrmMultipleCommandsReturnZero)
{
    // "echo" is available on both Windows (cmd.exe) and Linux (/bin/sh).
    auto client = makeClient();
    EXPECT_EQ(client.doRemCmdStrm({"echo hello", "echo world"}), 0);
}

// Gap 7: responses must arrive in the same order as requests.
// We tag each command with a distinct tid and verify the response tids match.
TEST_F(GrpcRoundTripTest, RemCmdStrmResponsesAreInOrder)
{
    auto ch = grpc::CreateChannel(
        "localhost:" + std::to_string(kTestPort),
        grpc::InsecureChannelCredentials());
    auto stub = RS::RemSvc::NewStub(ch);

    grpc::ClientContext ctx;
    auto stream = stub->RemCmdStrm(&ctx);

    const std::vector<int> tids = {10, 20, 30};
    for (int tid : tids) {
        RS::RemCmdMsg req;
        req.set_cmd("echo hello");
        req.set_tid(tid);
        req.set_hsh(RS::crc32Hex("echo hello"));
        ASSERT_TRUE(stream->Write(req));
    }
    stream->WritesDone();

    std::vector<int> received;
    RS::RemResMsg res;
    while (stream->Read(&res))
        received.push_back(res.tid());

    ASSERT_TRUE(stream->Finish().ok());

    ASSERT_EQ(received.size(), tids.size());
    for (std::size_t i = 0; i < tids.size(); ++i)
        EXPECT_EQ(received[i], tids[i]) << "response " << i << " tid mismatch";
}
