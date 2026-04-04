#include "ServerConfig.hh"

#include <QSettings>
#include <QFileInfo>
#include <QString>
#include <QStringList>

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
    ini.endGroup();

    // ── [tls] ─────────────────────────────────────────────────────────────────
    ini.beginGroup("tls");
    if (ini.contains("enabled"))
        cfg.tlsEnabled = ini.value("enabled").toBool();
    if (ini.contains("cert"))
        cfg.certFile = ini.value("cert").toString().toStdString();
    if (ini.contains("key"))
        cfg.keyFile = ini.value("key").toString().toStdString();
    if (ini.contains("ca"))
        cfg.caFile = ini.value("ca").toString().toStdString();
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