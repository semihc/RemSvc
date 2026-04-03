#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>
#include "GrpcServerThread.hh"
#include "RemSvcClient.hh"
#include "Log.hh"

// Distinct port from integ_grpc (50999) so both suites can run concurrently.
static constexpr int kTestPort = 50998;

// kLoadCalls and kMinCmdPerSec are deliberately conservative so the suite
// passes on any CI hardware without tuning.  Raise them locally for real
// profiling, e.g. kLoadCalls=500, kMinCmdPerSec=50.
static constexpr int    kWarmup       = 5;
static constexpr int    kLoadCalls    = 50;
static constexpr double kMinCmdPerSec = 5.0;   // floor — detects total breakdown


// ── Fixture ──────────────────────────────────────────────────────────────

class ThroughputTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        RS::InitLogging();
        server_ = std::make_unique<GrpcServerThread>(kTestPort);
        server_->start();
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

std::unique_ptr<GrpcServerThread> ThroughputTest::server_;


// ── Helpers ───────────────────────────────────────────────────────────────

static double percentile(std::vector<double> v, int pct)
{
    std::sort(v.begin(), v.end());
    return v[v.size() * static_cast<std::size_t>(pct) / 100];
}


// ── Unary throughput ──────────────────────────────────────────────────────

TEST_F(ThroughputTest, RemCmdUnaryThroughput)
{
    auto client = makeClient();

    for (int i = 0; i < kWarmup; ++i)
        client.doRemCmd("echo warmup");

    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kLoadCalls; ++i)
        client.doRemCmd("echo hello");
    double secs = std::chrono::duration<double>(
                      std::chrono::steady_clock::now() - t0).count();

    double cps = kLoadCalls / secs;
    Log(RS::info, "RemCmd unary: {} calls in {:.3f}s = {:.1f} cmd/s",
        kLoadCalls, secs, cps);

    EXPECT_GT(cps, kMinCmdPerSec)
        << "throughput " << cps << " cmd/s is below floor " << kMinCmdPerSec;
}


// ── Streaming throughput ──────────────────────────────────────────────────

TEST_F(ThroughputTest, RemCmdStrmThroughput)
{
    auto client = makeClient();

    client.doRemCmdStrm(std::vector<std::string>(kWarmup, "echo warmup"));

    std::vector<std::string> cmds(kLoadCalls, "echo hello");
    auto t0 = std::chrono::steady_clock::now();
    client.doRemCmdStrm(cmds);
    double secs = std::chrono::duration<double>(
                      std::chrono::steady_clock::now() - t0).count();

    double cps = kLoadCalls / secs;
    Log(RS::info, "RemCmdStrm: {} calls in {:.3f}s = {:.1f} cmd/s",
        kLoadCalls, secs, cps);

    EXPECT_GT(cps, kMinCmdPerSec)
        << "throughput " << cps << " cmd/s is below floor " << kMinCmdPerSec;
}


// ── Unary latency percentiles ────────────────────────────────────────────

TEST_F(ThroughputTest, RemCmdUnaryLatency)
{
    auto client = makeClient();

    for (int i = 0; i < kWarmup; ++i)
        client.doRemCmd("echo warmup");

    std::vector<double> ms;
    ms.reserve(kLoadCalls);
    for (int i = 0; i < kLoadCalls; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        client.doRemCmd("echo hello");
        ms.push_back(std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - t0).count());
    }

    double p50 = percentile(ms, 50);
    double p95 = percentile(ms, 95);
    double p99 = percentile(ms, 99);
    Log(RS::info, "RemCmd latency ms: p50={:.1f} p95={:.1f} p99={:.1f}",
        p50, p95, p99);

    // Sanity ceiling only — real threshold depends on hardware.
    EXPECT_LT(p50, 5000.0) << "p50 " << p50 << "ms exceeds 5 s sanity limit";
}


// ── Streaming latency percentiles ────────────────────────────────────────

TEST_F(ThroughputTest, RemCmdStrmLatency)
{
    auto client = makeClient();

    client.doRemCmdStrm(std::vector<std::string>(kWarmup, "echo warmup"));

    // Each single-command stream gives one end-to-end latency sample.
    std::vector<double> ms;
    ms.reserve(kLoadCalls);
    for (int i = 0; i < kLoadCalls; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        client.doRemCmdStrm({"echo hello"});
        ms.push_back(std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - t0).count());
    }

    double p50 = percentile(ms, 50);
    double p95 = percentile(ms, 95);
    double p99 = percentile(ms, 99);
    Log(RS::info, "RemCmdStrm latency ms: p50={:.1f} p95={:.1f} p99={:.1f}",
        p50, p95, p99);

    EXPECT_LT(p50, 5000.0) << "p50 " << p50 << "ms exceeds 5 s sanity limit";
}


// ── Concurrent clients ───────────────────────────────────────────────────

// Verifies the server handles multiple simultaneous clients without deadlock.
// kConcurrentClients threads each send kCallsPerClient unary RemCmd calls;
// all calls must succeed and total throughput must exceed kMinCmdPerSec.
static constexpr int kConcurrentClients = 4;
static constexpr int kCallsPerClient    = 10;  // conservative — safe on any CI

TEST_F(ThroughputTest, ConcurrentClientsUnaryThroughput)
{
    std::atomic<int> totalCalls{0};

    auto t0 = std::chrono::steady_clock::now();

    std::vector<std::thread> threads;
    threads.reserve(kConcurrentClients);
    for (int t = 0; t < kConcurrentClients; ++t) {
        threads.emplace_back([&] {
            auto client = makeClient();
            for (int i = 0; i < kCallsPerClient; ++i) {
                client.doRemCmd("echo hello");
                ++totalCalls;
            }
        });
    }
    for (auto& th : threads) th.join();

    double secs = std::chrono::duration<double>(
                      std::chrono::steady_clock::now() - t0).count();
    double cps = totalCalls.load() / secs;
    Log(RS::info, "Concurrent {}-client unary: {} calls in {:.3f}s = {:.1f} cmd/s",
        kConcurrentClients, totalCalls.load(), secs, cps);

    EXPECT_EQ(totalCalls.load(), kConcurrentClients * kCallsPerClient);
    EXPECT_GT(cps, kMinCmdPerSec)
        << "throughput " << cps << " cmd/s is below floor " << kMinCmdPerSec;
}
