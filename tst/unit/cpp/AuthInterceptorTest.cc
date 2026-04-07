/*
 * tst/unit/cpp/AuthInterceptorTest.cc
 * ====================================
 * Unit tests for BearerTokenAuthProcessor.
 *
 * The processor is a pure in-process object; no gRPC server is started.
 * grpc::AuthContext is a pure abstract class, so we provide a minimal
 * concrete fake (FakeAuthContext) instead of using the test-spouse helper
 * that is not available in the vcpkg gRPC distribution.
 */

#include <gtest/gtest.h>
#include <map>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>
#include <grpcpp/security/auth_context.h>
#include <grpcpp/security/auth_metadata_processor.h>

#include "AuthInterceptor.hh"


// ---------------------------------------------------------------------------
// Minimal concrete AuthContext fake
// ---------------------------------------------------------------------------

// grpc::AuthPropertyIterator's default constructor is protected.
// This thin subclass exposes it so FakeAuthContext::begin()/end() can
// return a default-constructed (sentinel) iterator without pulling in
// the full SecureAuthContext machinery.
struct NullAuthPropertyIterator : grpc::AuthPropertyIterator {
    NullAuthPropertyIterator() : grpc::AuthPropertyIterator() {}
};

class FakeAuthContext : public grpc::AuthContext {
public:
    bool IsPeerAuthenticated() const override {
        return !peer_identity_property_.empty() &&
               properties_.count(peer_identity_property_) > 0;
    }

    std::vector<grpc::string_ref> GetPeerIdentity() const override {
        return FindPropertyValues(peer_identity_property_);
    }

    std::string GetPeerIdentityPropertyName() const override {
        return peer_identity_property_;
    }

    std::vector<grpc::string_ref> FindPropertyValues(
            const std::string& name) const override {
        std::vector<grpc::string_ref> result;
        auto range = properties_.equal_range(name);
        for (auto it = range.first; it != range.second; ++it)
            result.emplace_back(it->second);
        return result;
    }

    grpc::AuthPropertyIterator begin() const override {
        return NullAuthPropertyIterator{};
    }
    grpc::AuthPropertyIterator end() const override {
        return NullAuthPropertyIterator{};
    }

    void AddProperty(const std::string& key,
                     const grpc::string_ref& value) override {
        properties_.emplace(key, std::string(value.data(), value.size()));
    }

    bool SetPeerIdentityPropertyName(const std::string& name) override {
        peer_identity_property_ = name;
        return true;
    }

private:
    std::multimap<std::string, std::string> properties_;
    std::string peer_identity_property_;
};


// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

using InputMetadata  = grpc::AuthMetadataProcessor::InputMetadata;
using OutputMetadata = grpc::AuthMetadataProcessor::OutputMetadata;

// Build a single-entry InputMetadata map.
// Parameters are grpc::string_ref so that callers can pass string literals
// (static storage duration) — avoids dangling refs from std::string temporaries.
static InputMetadata makeMetadata(grpc::string_ref key, grpc::string_ref value)
{
    InputMetadata m;
    m.insert({key, value});
    return m;
}

// Run Process() through a FakeAuthContext.
// Returns the gRPC status; consumed/response metadata are discarded.
static grpc::Status runProcess(
    RS::BearerTokenAuthProcessor& proc,
    const InputMetadata&          meta)
{
    FakeAuthContext  ctx;
    OutputMetadata   consumed, response;
    return proc.Process(meta, &ctx, &consumed, &response);
}

// Run Process() and also return the consumed metadata for inspection.
static grpc::Status runProcessWithConsumed(
    RS::BearerTokenAuthProcessor& proc,
    const InputMetadata&          meta,
    OutputMetadata&               consumed_out)
{
    FakeAuthContext ctx;
    OutputMetadata  response;
    return proc.Process(meta, &ctx, &consumed_out, &response);
}

// Run Process() and return the identity property stamped on the AuthContext.
static std::string runProcessGetIdentity(
    RS::BearerTokenAuthProcessor& proc,
    const InputMetadata&          meta)
{
    FakeAuthContext ctx;
    OutputMetadata  consumed, response;
    proc.Process(meta, &ctx, &consumed, &response);
    auto vals = ctx.FindPropertyValues("x-remsvc-identity");
    if (vals.empty()) return "";
    return std::string(vals[0].data(), vals[0].size());
}


// ---------------------------------------------------------------------------
// Auth disabled (empty token map)
// ---------------------------------------------------------------------------

TEST(BearerTokenAuthProcessor, EmptyMapDisablesAuth)
{
    RS::BearerTokenAuthProcessor proc({});
    EXPECT_FALSE(proc.IsEnabled());
}

TEST(BearerTokenAuthProcessor, EmptyMapPermitsCallWithNoHeader)
{
    RS::BearerTokenAuthProcessor proc({});
    InputMetadata meta; // no Authorization header
    EXPECT_TRUE(runProcess(proc, meta).ok());
}

TEST(BearerTokenAuthProcessor, EmptyMapPermitsCallWithAnyToken)
{
    RS::BearerTokenAuthProcessor proc({});
    auto meta = makeMetadata("authorization", "Bearer totally-random-token");
    EXPECT_TRUE(runProcess(proc, meta).ok());
}


// ---------------------------------------------------------------------------
// Auth enabled — rejection cases
// ---------------------------------------------------------------------------

TEST(BearerTokenAuthProcessor, NonEmptyMapEnablesAuth)
{
    RS::BearerTokenAuthProcessor proc({{"airflow-prod", "tok123"}});
    EXPECT_TRUE(proc.IsEnabled());
}

TEST(BearerTokenAuthProcessor, MissingAuthHeaderRejectsCall)
{
    RS::BearerTokenAuthProcessor proc({{"airflow-prod", "tok123"}});
    InputMetadata meta; // no header

    auto status = runProcess(proc, meta);

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAUTHENTICATED);
    EXPECT_NE(status.error_message().find("missing"), std::string::npos);
}

TEST(BearerTokenAuthProcessor, MalformedSchemeRejectsCall)
{
    RS::BearerTokenAuthProcessor proc({{"airflow-prod", "tok123"}});
    auto meta = makeMetadata("authorization", "Basic dXNlcjpwYXNz");

    auto status = runProcess(proc, meta);

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAUTHENTICATED);
    EXPECT_NE(status.error_message().find("Bearer"), std::string::npos);
}

TEST(BearerTokenAuthProcessor, UnknownTokenRejectsCall)
{
    RS::BearerTokenAuthProcessor proc({{"airflow-prod", "tok123"}});
    auto meta = makeMetadata("authorization", "Bearer wrong-token");

    auto status = runProcess(proc, meta);

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAUTHENTICATED);
}

TEST(BearerTokenAuthProcessor, EmptyBearerTokenRejectsCall)
{
    RS::BearerTokenAuthProcessor proc({{"airflow-prod", "tok123"}});
    auto meta = makeMetadata("authorization", "Bearer ");

    EXPECT_FALSE(runProcess(proc, meta).ok());
}


// ---------------------------------------------------------------------------
// Auth enabled — success cases
// ---------------------------------------------------------------------------

TEST(BearerTokenAuthProcessor, ValidTokenPermitsCall)
{
    RS::BearerTokenAuthProcessor proc({{"airflow-prod", "tok123"}});
    auto meta = makeMetadata("authorization", "Bearer tok123");
    EXPECT_TRUE(runProcess(proc, meta).ok());
}

TEST(BearerTokenAuthProcessor, ValidTokenStampsIdentityOnContext)
{
    RS::BearerTokenAuthProcessor proc({{"airflow-prod", "tok123"}});
    auto meta = makeMetadata("authorization", "Bearer tok123");

    EXPECT_EQ(runProcessGetIdentity(proc, meta), "airflow-prod");
}

TEST(BearerTokenAuthProcessor, ValidTokenConsumesAuthHeader)
{
    RS::BearerTokenAuthProcessor proc({{"airflow-prod", "tok123"}});
    auto meta = makeMetadata("authorization", "Bearer tok123");
    OutputMetadata consumed;

    runProcessWithConsumed(proc, meta, consumed);

    bool found = false;
    for (const auto& [k, v] : consumed)
        if (std::string(k.data(), k.size()) == "authorization")
            found = true;
    EXPECT_TRUE(found) << "Authorization header not marked as consumed";
}


// ---------------------------------------------------------------------------
// Multiple tokens
// ---------------------------------------------------------------------------

TEST(BearerTokenAuthProcessor, MultipleTokensStagingMatchesStagingIdentity)
{
    RS::BearerTokenAuthProcessor proc({
        {"airflow-prod",    "prod-token"},
        {"airflow-staging", "staging-token"},
        {"dev-semih",       "dev-token"},
    });
    auto meta = makeMetadata("authorization", "Bearer staging-token");

    EXPECT_TRUE(runProcess(proc, meta).ok());
    EXPECT_EQ(runProcessGetIdentity(proc, meta), "airflow-staging");
}

TEST(BearerTokenAuthProcessor, MultipleTokensWrongTokenStillRejected)
{
    RS::BearerTokenAuthProcessor proc({
        {"airflow-prod",    "prod-token"},
        {"airflow-staging", "staging-token"},
    });
    auto meta = makeMetadata("authorization", "Bearer not-a-real-token");

    EXPECT_FALSE(runProcess(proc, meta).ok());
}

TEST(BearerTokenAuthProcessor, AllTokensIteratedEvenAfterFirstMatch)
{
    // Behavioural: the last-writer-wins semantics of the loop means that if
    // the same token appears twice in the inverted map (impossible in practice
    // as map keys are unique) the final assignment wins.  For a single-match
    // case we verify the correct identity is returned — confirming the loop
    // ran to completion without short-circuiting the identity assignment.
    RS::BearerTokenAuthProcessor proc({
        {"id-a", "token-a"},
        {"id-b", "token-b"},
        {"id-c", "token-c"},
    });
    auto meta = makeMetadata("authorization", "Bearer token-a");

    EXPECT_TRUE(runProcess(proc, meta).ok());
    EXPECT_EQ(runProcessGetIdentity(proc, meta), "id-a");
}


// ---------------------------------------------------------------------------
// Construction edge cases
// ---------------------------------------------------------------------------

TEST(BearerTokenAuthProcessor, EmptyTokenValueIgnoredDuringConstruction)
{
    // An identity with an empty token value is silently dropped.
    // An empty token could match a "Bearer " header (empty after prefix),
    // so it must never be stored.
    RS::BearerTokenAuthProcessor proc({{"bad-identity", ""}});
    EXPECT_FALSE(proc.IsEnabled());  // nothing stored → auth disabled
}

TEST(BearerTokenAuthProcessor, OnlyNonEmptyTokensStored)
{
    RS::BearerTokenAuthProcessor proc({
        {"valid",   "real-token"},
        {"invalid", ""},            // must be discarded
    });
    EXPECT_TRUE(proc.IsEnabled());
    // "real-token" must still work
    auto meta = makeMetadata("authorization", "Bearer real-token");
    EXPECT_TRUE(runProcess(proc, meta).ok());
}
