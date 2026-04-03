#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <thread>
#include <chrono>

#include <grpcpp/grpcpp.h>
#include "GrpcServerThread.hh"
#include "RemSvcClient.hh"
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


// ── GetStatus ────────────────────────────────────────────────────────────

TEST_F(GrpcRoundTripTest, GetStatusReturnsOk)
{
    auto client = makeClient();
    RS::StatusResult sr = client.doGetStatus();
    EXPECT_TRUE(sr.ok);
    EXPECT_EQ(sr.rc, 0);
}


// ── RemCmdStrm ───────────────────────────────────────────────────────────

TEST_F(GrpcRoundTripTest, RemCmdStrmMultipleCommandsReturnZero)
{
    // "echo" is available on both Windows (cmd.exe) and Linux (/bin/sh).
    auto client = makeClient();
    EXPECT_EQ(client.doRemCmdStrm({"echo hello", "echo world"}), 0);
}
