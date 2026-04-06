#pragma once
#ifndef SERVER_CONFIG_HH
#define SERVER_CONFIG_HH

#include <map>
#include <string>
#include <vector>

// ── ServerConfig ──────────────────────────────────────────────────────────────
//
// All server settings in one place.  Values are populated from an INI config
// file (loadServerConfig, via QSettings) and then CLI flags overlay individual
// fields on top.
//
// INI schema (all keys optional — absent keys keep their default values):
//
//   [server]
//   port=50051
//
//   [tls]
//   enabled=false
//   cert=/path/to/server.crt
//   key=/path/to/server.key
//   ca=                          ; empty = no mutual TLS
//
//   [allowlist]
//   ; Each pattern entry is a std::regex matched against the command string.
//   ; An empty section means all commands are permitted.
//   1=^echo\b
//   2=^dir\b
//
//   [auth]
//   ; Bearer-token authentication (Model B: one token per caller identity).
//   ; Key = identity label (logged on each authenticated call).
//   ; Value = secret bearer token (clients send as "Authorization: Bearer <value>").
//   ; An empty section disables authentication — all callers are permitted.
//   airflow-prod    = secret-prod-token
//   airflow-staging = secret-staging-token
//   dev-semih       = secret-dev-token
//
//   [log]
//   file=RemSvc_server.log
//   level=info                   ; trace|debug|info|warn|error
//   debug_level=-1               ; -1 = off; 0-9 = enable debug logging
//                                ; Rotating sink: 10 files x 10 MB each.

struct ServerConfig {
    // Network
    int port{50051};

    // Per-command process timeout in milliseconds.
    // Each child process is forcibly killed if it has not exited within this
    // window.  Must be ≤ stream_timeout to be meaningful.  Default: 30 s.
    int cmdTimeoutMs{30000};

    // TLS
    bool        tlsEnabled{false};
    std::string certFile;
    std::string keyFile;
    std::string caFile;

    // Authorization — regex patterns; empty list = allow all commands.
    std::vector<std::string> allowlist;

    // Authentication — identity → bearer-token map.
    // Empty map = authentication disabled (all callers permitted).
    // Populated from the [auth] section; passed to BearerTokenAuthProcessor.
    std::map<std::string, std::string> authTokens;

    // Logging (fed into RS::CliLogFile / RS::CliLogLevel / RS::CliDbgLevel
    // before InitLogging(); file sink rotates: 10 files x 10 MB each)
    std::string logFile;
    std::string logLevel;
    int         debugLevel{-1};
};


// Load an INI config file into cfg using QSettings.
// Only keys present in the file are modified; absent keys keep their values.
// Returns empty string on success, or an error description on failure.
std::string loadServerConfig(const std::string& path, ServerConfig& cfg);

#endif // SERVER_CONFIG_HH