/*
 *
 * Copyright Bupa Australia Pty Ltd
 *
 * Author: Semih Cemiloglu
 *
 */

// Std includes
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

// Prj includes
#include "GrpcServerThread.hh"
#include "ServerConfig.hh"
#include <QCoreApplication>
#include "CLI.hh"
#include "Log.hh"
#include "SignalHandler.hh"


using namespace std;
using namespace RS;


int main(int argc, char *argv[]) {
    int rc{};
    std::string_view appName{ argv[0] };

#ifdef _WIN32
    // Switch the console output codepage to UTF-8 so that log messages
    // containing non-ASCII characters (em dashes, arrows, …) render
    // correctly instead of appearing as mojibake (e.g. "ÔÇö").
    SetConsoleOutputCP(CP_UTF8);
#endif

    // ── Step 1: pre-parse --config only, to load the INI file first ──────────
    // CLI11 allow_extras lets us ignore all other flags at this stage.
    std::string configFile;
    {
        CLI::App pre;
        pre.add_option("--config", configFile, "INI configuration file");
        pre.allow_extras(true);
        try { pre.parse(argc, argv); } catch (const CLI::ParseError&) {}
    }

    // ── Step 2: load INI config (provides defaults for all settings) ─────────
    ServerConfig cfg;
    if (!configFile.empty()) {
        auto err = loadServerConfig(configFile, cfg);
        if (!err.empty()) {
            std::cerr << "Error loading config: " << err << "\n";
            return 1;
        }
    }

    // ── Step 3: full CLI parse — explicitly supplied flags override cfg ───────
    CLI::App cli_app;
    cli_app.add_option("--config", configFile, "INI configuration file");

    int port = cfg.port;
    cli_app.add_option("--port", port, "Port");

    // TLS options
    bool        tls     = cfg.tlsEnabled;
    std::string certFile = cfg.certFile;
    std::string keyFile  = cfg.keyFile;
    std::string caFile   = cfg.caFile;
    cli_app.add_flag  ("--tls",      tls,      "Enable TLS");
    cli_app.add_option("--cert",     certFile,  "Server certificate PEM file (required with --tls)");
    cli_app.add_option("--key",      keyFile,   "Server private key PEM file (required with --tls)");
    cli_app.add_option("--ca-cert",  caFile,    "CA certificate PEM file (enables mutual TLS)");

    // Per-command child-process timeout
    int cmdTimeoutMs = cfg.cmdTimeoutMs;
    cli_app.add_option("--cmd-timeout-ms", cmdTimeoutMs,
                       "Per-command child-process kill timeout in milliseconds "
                       "(default: 30000)");

    // Authorization: regex patterns (CLI list replaces config list when supplied)
    std::vector<std::string> allowlist = cfg.allowlist;
    cli_app.add_option("--allow", allowlist,
                       "Allowed command regex pattern (repeatable; empty = allow all)");

    std::vector<std::string> denylist = cfg.denylist;
    cli_app.add_option("--deny", denylist,
                       "Denied command regex pattern (repeatable; deny takes precedence over allow)");

    // Logging overrides
    std::string logFile  = cfg.logFile;
    std::string logLevel = cfg.logLevel;
    int         dbgLevel = cfg.debugLevel;
    cli_app.add_option("--log-file-override",  logFile,
                       "Override log file path from config");
    cli_app.add_option("--log-level-override", logLevel,
                       "Override log level from config (trace|debug|info|warn|error)");

    rc = ConfigureCLI(cli_app, argc, argv);
    if (rc) return rc;

    if (tls && (certFile.empty() || keyFile.empty())) {
        std::cerr << "--tls requires both --cert and --key\n";
        return 1;
    }

    if (!tls && !cfg.authTokens.empty()) {
        std::cerr << "Error: bearer-token authentication requires TLS (--tls --cert ... --key ...)\n";
        return 1;
    }

    // ── Step 4: configure logging ────────────────────────────────────────────
    // Config file and CLI overrides feed the global CLI vars that InitLogging
    // reads.  The file sink uses a rotating policy: 10 files x 10 MB each.
    if (!logFile.empty())
        CliLogFile = logFile;
    else
        CliLogFile = DefaultCLiLogFileName(appName);

    if (!logLevel.empty())
        CliLogLevel = logLevel;

    if (dbgLevel >= 0)
        CliDbgLevel = dbgLevel;

    InitLogging();
    Log(info, "Starting {}", appName);
    if (!configFile.empty())
        Log(info, "Loaded config from {}", configFile);

    // ── Step 5: Qt event loop + gRPC server ──────────────────────────────────
    QCoreApplication qt_app(argc, argv);

    SignalHandler::setupSignalHandlers();
    SignalHandler signalHandler;
    QObject::connect(&signalHandler, &SignalHandler::quitRequested, &qt_app, &QCoreApplication::quit);

    std::optional<TlsConfig> tlsConfig;
    if (tls) tlsConfig = TlsConfig{certFile, keyFile, caFile};

    GrpcServerThread grpcThread{port, tlsConfig, allowlist, denylist, cfg.authTokens, cmdTimeoutMs};
    Log(info, "Starting gRPC Server at port {}", port);
    grpcThread.start();

    rc = qt_app.exec();

    Log(info, "Stopping gRPC Server");
    grpcThread.stop();

    Log(info, "Stopping application {}", appName);
    TermLogging();
    return rc;
}