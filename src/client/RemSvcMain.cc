/*
 * Entry point for the RemSvc CLI client.
 */

#include "RemSvcClient.hh"

#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <format>

#include <grpcpp/grpcpp.h>
#include <QCoreApplication>

#include "CLI.hh"
#include "Log.hh"

using namespace std;
using namespace RS;


int main(int argc, char *argv[])
{
    int rc{};
    std::string_view appName{argv[0]};

    CLI::App cli_app;
    string server{"localhost"};
    cli_app.add_option("-s,--server", server, "Server hostname");
    int port{50051};
    cli_app.add_option("-p,--port", port, "Port");
    int ping{0};
    cli_app.add_option("-n,--ping", ping, "Ping sequence number");
    string cmd{};
    cli_app.add_option("-c,--cmd", cmd, "Command to execute");
    string cmdusr{};
    cli_app.add_option("-u,--user", cmdusr, "OS user to execute the command as (Linux only)");

    // TLS options
    bool   tls{false};
    string caFile;
    cli_app.add_flag  ("--tls",     tls,    "Connect using TLS");
    cli_app.add_option("--ca-cert", caFile, "CA certificate PEM file (empty = system roots)");

    rc = ConfigureCLI(cli_app, argc, argv);
    if (rc) return rc;

    CliLogFile = DefaultCLiLogFileName(appName);
    InitLogging();
    Log(info, "Starting {}", appName);

    QCoreApplication qt_app(argc, argv);

    string target = format("{}:{}", server, port);

    shared_ptr<grpc::ChannelCredentials> creds;
    if (tls) {
        grpc::SslCredentialsOptions ssl_opts;
        if (!caFile.empty()) {
            ifstream f(caFile, ios::binary);
            if (!f) { Log(error, "Cannot open CA cert file: {}", caFile); return 1; }
            ostringstream ss;
            ss << f.rdbuf();
            ssl_opts.pem_root_certs = ss.str();
        }
        creds = grpc::SslCredentials(ssl_opts);
        Log(info, "TLS enabled (ca={})", caFile.empty() ? "system roots" : caFile);
    } else {
        creds = grpc::InsecureChannelCredentials();
    }

    RemSvcClient client(grpc::CreateChannel(target, creds));

    while (ping > 0) {
        PingResult pr = client.doPing(ping--);
        Log(info, "Ping {}", pr.ok ? "ok" : "failed");
    }

    if (!cmd.empty())
        rc = client.doRemCmd(cmd, /*cmdtyp=*/0, /*tid=*/0, cmdusr);

    Log(info, "Stopping {}", appName);
    TermLogging();
    return rc;
}
