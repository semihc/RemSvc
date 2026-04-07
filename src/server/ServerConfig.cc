#include "ServerConfig.hh"

#include <QSettings>
#include <QFileInfo>
#include <QString>
#include <QStringList>

#include <cstdlib>
#include <string>

namespace {

// Expand $VAR and ${VAR} patterns in `s` using the process environment.
// Unrecognised or empty variable names are left unexpanded.
// Example:  "$HOME/certs/server.crt"  →  "/home/user/certs/server.crt"
std::string expandEnvVars(const std::string& s)
{
    std::string result;
    result.reserve(s.size());

    for (std::size_t i = 0; i < s.size(); ) {
        if (s[i] != '$') {
            result += s[i++];
            continue;
        }

        // '$' found — determine variable name boundaries.
        ++i;  // skip '$'
        bool braced = (i < s.size() && s[i] == '{');
        if (braced) ++i;  // skip '{'

        std::size_t start = i;
        while (i < s.size()) {
            char c = s[i];
            // Variable names: alphanumeric + underscore.
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') ||  c == '_') {
                ++i;
            } else {
                break;
            }
        }

        std::size_t name_end = i;  // end of the variable name (before any '}')
        if (braced) {
            if (i < s.size() && s[i] == '}') ++i;  // skip '}'
            else {
                // Malformed ${...; emit literally and continue.
                result += "${";
                result.append(s, start, name_end - start);
                continue;
            }
        }

        std::string varname(s, start, name_end - start);
        if (varname.empty()) {
            result += '$';  // bare '$' with no name — keep as-is
            continue;
        }

        const char* val = std::getenv(varname.c_str());
        if (val)
            result += val;
        else {
            // Unknown variable — preserve the original token so the admin
            // can see what failed rather than silently using a wrong path.
            result += '$';
            if (braced) result += '{';
            result += varname;
            if (braced) result += '}';
        }
    }

    return result;
}

} // anonymous namespace

std::string loadServerConfig(const std::string& path, ServerConfig& cfg)
{
    if (!QFileInfo::exists(QString::fromStdString(path)))
        return "config file not found: " + path;

    QSettings ini(QString::fromStdString(path), QSettings::IniFormat);
    if (ini.status() != QSettings::NoError)
        return "failed to open config file: " + path;

    // ── [server] ─────────────────────────────────────────────────────────────
    ini.beginGroup("server");
    if (ini.contains("port"))
        cfg.port = ini.value("port").toInt();
    if (ini.contains("cmd_timeout_ms"))
        cfg.cmdTimeoutMs = ini.value("cmd_timeout_ms").toInt();
    ini.endGroup();

    if (cfg.port <= 0 || cfg.port > 65535)
        return "config: port must be in range 1-65535 (got " + std::to_string(cfg.port) + ")";
    if (cfg.cmdTimeoutMs <= 0)
        return "config: cmd_timeout_ms must be positive (got " + std::to_string(cfg.cmdTimeoutMs) + ")";

    // ── [tls] ─────────────────────────────────────────────────────────────────
    // Cert/key/CA paths support $VAR and ${VAR} environment-variable expansion
    // so that paths like "$HOME/certs/server.crt" work without hardcoding user
    // home directories into the config file.
    ini.beginGroup("tls");
    if (ini.contains("enabled"))
        cfg.tlsEnabled = ini.value("enabled").toBool();
    if (ini.contains("cert"))
        cfg.certFile = expandEnvVars(ini.value("cert").toString().toStdString());
    if (ini.contains("key"))
        cfg.keyFile  = expandEnvVars(ini.value("key").toString().toStdString());
    if (ini.contains("ca"))
        cfg.caFile   = expandEnvVars(ini.value("ca").toString().toStdString());
    ini.endGroup();

    // ── [allowlist] ───────────────────────────────────────────────────────────
    // Keys are arbitrary (1=, 2=, pattern1=, …); all values are collected.
    ini.beginGroup("allowlist");
    const QStringList keys = ini.childKeys();
    if (!keys.isEmpty()) {
        cfg.allowlist.clear();
        // Sort keys so numbered entries (1, 2, …) are applied in order.
        QStringList sorted = keys;
        sorted.sort();
        for (const QString& k : sorted) {
            std::string pat = ini.value(k).toString().toStdString();
            if (!pat.empty())
                cfg.allowlist.push_back(pat);
        }
    }
    ini.endGroup();

    // ── [denylist] ────────────────────────────────────────────────────────────
    ini.beginGroup("denylist");
    const QStringList denyKeys = ini.childKeys();
    if (!denyKeys.isEmpty()) {
        cfg.denylist.clear();
        QStringList sortedDeny = denyKeys;
        sortedDeny.sort();
        for (const QString& k : sortedDeny) {
            std::string pat = ini.value(k).toString().toStdString();
            if (!pat.empty())
                cfg.denylist.push_back(pat);
        }
    }
    ini.endGroup();

    // ── [auth] ────────────────────────────────────────────────────────────────
    // Each key is an identity label; each value is the secret bearer token.
    // Token values support $VAR / ${VAR} expansion so tokens can be stored in
    // environment variables rather than written in plaintext in the config file:
    //   airflow-prod = $REMSVC_PROD_TOKEN
    // An absent or empty section leaves authTokens empty (auth disabled).
    ini.beginGroup("auth");
    const QStringList authKeys = ini.childKeys();
    if (!authKeys.isEmpty()) {
        cfg.authTokens.clear();
        for (const QString& identity : authKeys) {
            std::string token = expandEnvVars(
                ini.value(identity).toString().toStdString());
            if (!token.empty())
                cfg.authTokens.emplace(identity.toStdString(), std::move(token));
        }
    }
    ini.endGroup();

    // ── [log] ─────────────────────────────────────────────────────────────────
    ini.beginGroup("log");
    if (ini.contains("file"))
        cfg.logFile = ini.value("file").toString().toStdString();
    if (ini.contains("level"))
        cfg.logLevel = ini.value("level").toString().toStdString();
    if (ini.contains("debug_level"))
        cfg.debugLevel = ini.value("debug_level").toInt();
    ini.endGroup();

    return {};  // success
}