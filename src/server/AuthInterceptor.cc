#include "AuthInterceptor.hh"
#include "Log.hh"

#include <string>

namespace RS {

BearerTokenAuthProcessor::BearerTokenAuthProcessor(
    std::map<std::string, std::string> identity_to_token)
{
    // Invert the identity→token map into token→identity for O(1) lookup.
    // Building a reverse map keeps secret tokens out of map keys in the
    // original config structure (avoids inadvertent key-space logging).
    for (auto& [identity, token] : identity_to_token) {
        if (!token.empty())
            m_tokens.emplace(std::move(token), identity);
    }

    if (m_tokens.empty()) {
        Log(RS::warn,
            "BearerTokenAuthProcessor: token map is empty — "
            "authentication is DISABLED.  All callers are permitted.");
    } else {
        Log(RS::info,
            "BearerTokenAuthProcessor: loaded {} token(s).  "
            "Bearer-token authentication is ENABLED.",
            m_tokens.size());
    }
}


bool BearerTokenAuthProcessor::constTimeEqual(
    const std::string& a, const std::string& b) noexcept
{
    // Different lengths can be revealed immediately — length is not secret.
    if (a.size() != b.size()) return false;

    // XOR every byte pair and accumulate into diff.
    // volatile prevents the compiler from short-circuiting once diff != 0.
    volatile int diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    return diff == 0;
}


grpc::Status BearerTokenAuthProcessor::Process(
    const InputMetadata& auth_metadata,
    grpc::AuthContext*   context,
    OutputMetadata*      consumed_auth_metadata,
    OutputMetadata*      /*response_metadata*/)
{
    // Auth disabled — let all calls through without touching the context.
    if (m_tokens.empty())
        return grpc::Status::OK;

    // ── 1. Extract the Authorization header ──────────────────────────────────
    auto it = auth_metadata.find(grpc::string_ref(kAuthHeader.data(),
                                                   kAuthHeader.size()));
    if (it == auth_metadata.end()) {
        Log(RS::warn, "AuthProcessor: rejected call — missing Authorization header");
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                            "missing Authorization header");
    }

    grpc::string_ref header_value = it->second;

    // ── 2. Parse "Bearer <token>" ─────────────────────────────────────────────
    std::string_view value(header_value.data(), header_value.size());
    if (value.size() < kBearerPrefix.size() ||
        value.substr(0, kBearerPrefix.size()) != kBearerPrefix) {
        Log(RS::warn, "AuthProcessor: rejected call — malformed Authorization header "
                      "(expected 'Bearer <token>')");
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                            "Authorization header must use 'Bearer' scheme");
    }

    std::string token(value.substr(kBearerPrefix.size()));

    // ── 3. Look up the token (constant-time) ─────────────────────────────────
    // Iterate the entire map every time — no early exit on first match —
    // so the number of iterations does not leak how many tokens are configured.
    const std::string* identity = nullptr;
    for (const auto& [stored_token, stored_identity] : m_tokens) {
        if (constTimeEqual(token, stored_token))
            identity = &stored_identity;
    }
    if (!identity) {
        // Log only that a call was rejected, never log the token itself.
        Log(RS::warn, "AuthProcessor: rejected call — unrecognised bearer token");
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                            "invalid bearer token");
    }

    // ── 4. Stamp the verified identity onto the AuthContext ──────────────────
    // Service handlers can retrieve it via:
    //   context->FindPropertyValues("x-remsvc-identity")
    context->AddProperty(std::string(kIdentityKey), *identity);
    context->SetPeerIdentityPropertyName(std::string(kIdentityKey));

    // Mark the Authorization header as consumed (gRPC strips it from metadata
    // visible to service handlers, keeping the raw token off the handler layer).
    consumed_auth_metadata->insert(
        std::make_pair(std::string(it->first.data(),  it->first.size()),
                       std::string(it->second.data(), it->second.size())));

    Log(RS::info, "AuthProcessor: authenticated identity='{}'", *identity);
    return grpc::Status::OK;
}

} // namespace RS
