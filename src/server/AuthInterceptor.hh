#pragma once
#ifndef AUTH_INTERCEPTOR_HH
#define AUTH_INTERCEPTOR_HH

#include <map>
#include <string>

#include <grpcpp/grpcpp.h>
#include <grpcpp/security/auth_metadata_processor.h>

namespace RS {

// ── BearerTokenAuthProcessor ──────────────────────────────────────────────────
//
// gRPC AuthMetadataProcessor that enforces bearer-token authentication on
// every incoming RPC call.
//
// Token map (Model B — one identity per token):
//   key   = identity label (e.g. "airflow-prod", "dev-semih")
//   value = secret bearer token string
//
// Client sends:   Authorization: Bearer <token>
// Server checks:  token ∈ values(m_tokens)
//                 → on match:  adds "x-remsvc-identity" to AuthContext; proceeds
//                 → on miss:   returns UNAUTHENTICATED immediately; RPC never runs
//
// If the token map is empty (no [auth] section in the config), every call is
// permitted — this preserves backward compatibility with unauthenticated
// deployments.
//
// Thread safety: the processor is constructed once before the server starts and
// its token map is never mutated during serving.  All member access is read-only
// and therefore safe to call concurrently from multiple gRPC threads.

class BearerTokenAuthProcessor : public grpc::AuthMetadataProcessor {
public:
    // identity_to_token: map from human-readable identity label to secret token.
    // Pass an empty map to disable authentication (allow all callers).
    explicit BearerTokenAuthProcessor(std::map<std::string, std::string> identity_to_token);

    // AuthMetadataProcessor contract.
    // Called by gRPC for every incoming RPC before any service handler runs.
    grpc::Status Process(const InputMetadata&  auth_metadata,
                         grpc::AuthContext*     context,
                         OutputMetadata*        consumed_auth_metadata,
                         OutputMetadata*        response_metadata) override;

    // True if this processor will actually enforce authentication.
    // False means the token map is empty and all calls are allowed through.
    bool IsEnabled() const { return !m_tokens.empty(); }

private:
    // Reverse map: token value → identity label.
    // Built from the constructor's identity→token map so that lookup is O(n)
    // in the number of tokens (typically tiny) and the secret tokens are never
    // stored as map keys that could be inadvertently logged.
    std::map<std::string, std::string> m_tokens;  // token → identity

    static constexpr std::string_view kAuthHeader   = "authorization";
    static constexpr std::string_view kBearerPrefix = "Bearer ";
    static constexpr std::string_view kIdentityKey  = "x-remsvc-identity";

    // Constant-time string equality — compares every byte of both strings
    // regardless of where they first differ, preventing timing side-channels.
    static bool constTimeEqual(const std::string& a, const std::string& b) noexcept;
};

} // namespace RS

#endif // AUTH_INTERCEPTOR_HH
