/*
 * tst/unit/cpp/ServerConfigTest.cc
 * ==================================
 * Unit tests for ServerConfig struct and loadServerConfig().
 *
 * Tests write real temporary INI files so that QSettings/QFileInfo behave
 * identically to production use.  Each test creates a uniquely named file
 * under std::filesystem::temp_directory_path() and removes it on teardown.
 */

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <string>

#include "ServerConfig.hh"

namespace fs = std::filesystem;


// ---------------------------------------------------------------------------
// Helper: write a string to a temp file; return the path.
// ---------------------------------------------------------------------------

static fs::path writeTempIni(const std::string& content)
{
    auto path = fs::temp_directory_path() /
                ("remsvc_test_" + std::to_string(
                    std::hash<std::string>{}(content + __FILE__)) + ".ini");
    std::ofstream out(path, std::ios::trunc);
    out << content;
    return path;
}


// RAII wrapper: deletes the temp file when it goes out of scope.
struct TempFile {
    fs::path path;
    explicit TempFile(const std::string& content) : path(writeTempIni(content)) {}
    ~TempFile() { std::error_code ec; fs::remove(path, ec); }
    std::string str() const { return path.string(); }
};


// ---------------------------------------------------------------------------
// loadServerConfig — file-not-found
// ---------------------------------------------------------------------------

TEST(ServerConfig, ReturnsErrorWhenFileNotFound)
{
    ServerConfig cfg;
    auto err = loadServerConfig("/nonexistent/path/remsvc.ini", cfg);
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("not found"), std::string::npos);
}


// ---------------------------------------------------------------------------
// loadServerConfig — defaults preserved when keys absent
// ---------------------------------------------------------------------------

TEST(ServerConfig, DefaultsPreservedWhenKeysAbsent)
{
    TempFile ini("[server]\n");
    ServerConfig cfg;  // constructed with defaults
    auto err = loadServerConfig(ini.str(), cfg);

    EXPECT_TRUE(err.empty());
    EXPECT_EQ(cfg.port,         50051);
    EXPECT_EQ(cfg.cmdTimeoutMs, 30000);
    EXPECT_FALSE(cfg.tlsEnabled);
    EXPECT_TRUE(cfg.certFile.empty());
    EXPECT_TRUE(cfg.keyFile.empty());
    EXPECT_TRUE(cfg.caFile.empty());
    EXPECT_TRUE(cfg.allowlist.empty());
    EXPECT_TRUE(cfg.authTokens.empty());
}


// ---------------------------------------------------------------------------
// [server] section
// ---------------------------------------------------------------------------

TEST(ServerConfig, ParsesPort)
{
    TempFile ini("[server]\nport=9090\n");
    ServerConfig cfg;
    EXPECT_TRUE(loadServerConfig(ini.str(), cfg).empty());
    EXPECT_EQ(cfg.port, 9090);
}

TEST(ServerConfig, ParsesCmdTimeoutMs)
{
    TempFile ini("[server]\ncmd_timeout_ms=5000\n");
    ServerConfig cfg;
    EXPECT_TRUE(loadServerConfig(ini.str(), cfg).empty());
    EXPECT_EQ(cfg.cmdTimeoutMs, 5000);
}

TEST(ServerConfig, AbsentCmdTimeoutMsKeepsDefault)
{
    TempFile ini("[server]\nport=1234\n");
    ServerConfig cfg;
    EXPECT_TRUE(loadServerConfig(ini.str(), cfg).empty());
    EXPECT_EQ(cfg.cmdTimeoutMs, 30000);
}


// ---------------------------------------------------------------------------
// [tls] section
// ---------------------------------------------------------------------------

TEST(ServerConfig, ParsesTlsEnabled)
{
    TempFile ini("[tls]\nenabled=true\n");
    ServerConfig cfg;
    EXPECT_TRUE(loadServerConfig(ini.str(), cfg).empty());
    EXPECT_TRUE(cfg.tlsEnabled);
}

TEST(ServerConfig, ParsesTlsCertAndKeyPaths)
{
    TempFile ini("[tls]\nenabled=true\ncert=/etc/ssl/server.crt\nkey=/etc/ssl/server.key\n");
    ServerConfig cfg;
    EXPECT_TRUE(loadServerConfig(ini.str(), cfg).empty());
    EXPECT_EQ(cfg.certFile, "/etc/ssl/server.crt");
    EXPECT_EQ(cfg.keyFile,  "/etc/ssl/server.key");
}

TEST(ServerConfig, ParsesTlsCaPath)
{
    TempFile ini("[tls]\nca=/etc/ssl/ca.pem\n");
    ServerConfig cfg;
    EXPECT_TRUE(loadServerConfig(ini.str(), cfg).empty());
    EXPECT_EQ(cfg.caFile, "/etc/ssl/ca.pem");
}


// ---------------------------------------------------------------------------
// [auth] section
// ---------------------------------------------------------------------------

TEST(ServerConfig, ParsesAuthTokens)
{
    TempFile ini(
        "[auth]\n"
        "airflow-prod    = tok-prod\n"
        "airflow-staging = tok-staging\n"
    );
    ServerConfig cfg;
    EXPECT_TRUE(loadServerConfig(ini.str(), cfg).empty());
    ASSERT_EQ(cfg.authTokens.size(), 2u);
    EXPECT_EQ(cfg.authTokens.at("airflow-prod"),    "tok-prod");
    EXPECT_EQ(cfg.authTokens.at("airflow-staging"), "tok-staging");
}

TEST(ServerConfig, EmptyAuthSectionLeavesMapEmpty)
{
    TempFile ini("[auth]\n");
    ServerConfig cfg;
    EXPECT_TRUE(loadServerConfig(ini.str(), cfg).empty());
    EXPECT_TRUE(cfg.authTokens.empty());
}

TEST(ServerConfig, AuthEmptyTokenValueIsSkipped)
{
    // A key with an empty value must be silently dropped — an empty token
    // would match any "Bearer " header and must not be stored.
    TempFile ini("[auth]\nvalid-id = real-token\nbad-id   =\n");
    ServerConfig cfg;
    EXPECT_TRUE(loadServerConfig(ini.str(), cfg).empty());
    ASSERT_EQ(cfg.authTokens.size(), 1u);
    EXPECT_EQ(cfg.authTokens.at("valid-id"), "real-token");
    EXPECT_EQ(cfg.authTokens.count("bad-id"), 0u);
}

TEST(ServerConfig, AuthTokenEnvVarExpanded)
{
    // Set a known env var, reference it in the INI, verify expansion.
#ifdef _WIN32
    _putenv_s("REMSVC_TEST_TOKEN", "expanded-secret");
#else
    setenv("REMSVC_TEST_TOKEN", "expanded-secret", 1);
#endif
    TempFile ini("[auth]\nmy-service = $REMSVC_TEST_TOKEN\n");
    ServerConfig cfg;
    EXPECT_TRUE(loadServerConfig(ini.str(), cfg).empty());
    EXPECT_EQ(cfg.authTokens.at("my-service"), "expanded-secret");
}

TEST(ServerConfig, AuthTokenBracedEnvVarExpanded)
{
#ifdef _WIN32
    _putenv_s("REMSVC_TEST_TOKEN2", "braced-secret");
#else
    setenv("REMSVC_TEST_TOKEN2", "braced-secret", 1);
#endif
    TempFile ini("[auth]\nmy-service = ${REMSVC_TEST_TOKEN2}\n");
    ServerConfig cfg;
    EXPECT_TRUE(loadServerConfig(ini.str(), cfg).empty());
    EXPECT_EQ(cfg.authTokens.at("my-service"), "braced-secret");
}


// ---------------------------------------------------------------------------
// [allowlist] section
// ---------------------------------------------------------------------------

TEST(ServerConfig, ParsesAllowlistPatterns)
{
    TempFile ini("[allowlist]\n1=^echo\\b\n2=^dir\\b\n");
    ServerConfig cfg;
    EXPECT_TRUE(loadServerConfig(ini.str(), cfg).empty());
    ASSERT_EQ(cfg.allowlist.size(), 2u);
    EXPECT_EQ(cfg.allowlist[0], "^echo\\b");
    EXPECT_EQ(cfg.allowlist[1], "^dir\\b");
}

TEST(ServerConfig, EmptyAllowlistSectionLeavesListEmpty)
{
    TempFile ini("[allowlist]\n");
    ServerConfig cfg;
    EXPECT_TRUE(loadServerConfig(ini.str(), cfg).empty());
    EXPECT_TRUE(cfg.allowlist.empty());
}


// ---------------------------------------------------------------------------
// [log] section
// ---------------------------------------------------------------------------

TEST(ServerConfig, ParsesLogSettings)
{
    TempFile ini("[log]\nfile=mylog.log\nlevel=warn\ndebug_level=3\n");
    ServerConfig cfg;
    EXPECT_TRUE(loadServerConfig(ini.str(), cfg).empty());
    EXPECT_EQ(cfg.logFile,    "mylog.log");
    EXPECT_EQ(cfg.logLevel,   "warn");
    EXPECT_EQ(cfg.debugLevel, 3);
}


// ---------------------------------------------------------------------------
// Full config round-trip
// ---------------------------------------------------------------------------

TEST(ServerConfig, FullConfigRoundTrip)
{
    TempFile ini(
        "[server]\n"
        "port=9999\n"
        "cmd_timeout_ms=10000\n"
        "[tls]\n"
        "enabled=true\n"
        "cert=/certs/server.crt\n"
        "key=/certs/server.key\n"
        "[auth]\n"
        "prod = secret-prod\n"
        "[allowlist]\n"
        "1=^echo\n"
        "[log]\n"
        "level=debug\n"
    );
    ServerConfig cfg;
    EXPECT_TRUE(loadServerConfig(ini.str(), cfg).empty());

    EXPECT_EQ(cfg.port,         9999);
    EXPECT_EQ(cfg.cmdTimeoutMs, 10000);
    EXPECT_TRUE(cfg.tlsEnabled);
    EXPECT_EQ(cfg.certFile,    "/certs/server.crt");
    EXPECT_EQ(cfg.authTokens.at("prod"), "secret-prod");
    ASSERT_EQ(cfg.allowlist.size(), 1u);
    EXPECT_EQ(cfg.allowlist[0], "^echo");
    EXPECT_EQ(cfg.logLevel, "debug");
}
