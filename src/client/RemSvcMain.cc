/*
 * Entry point for the RemSvc CLI client.
 */

#include "RemSvcClient.hh"

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
    string_view appName{argv[0]};

    CLI::App cli_app;
    string server{"localhost"};
    cli_app.add_option("-s,--server", server, "Server hostname");
    int port{50051};
    cli_app.add_option("-p,--port", port, "Port");
    int ping{0};
    cli_app.add_option("-n,--ping", ping, "Ping sequence number");
    string cmd{};
    cli_app.add_option("-c,--cmd", cmd, "Command to execute");

    rc = ConfigureCLI(cli_app, argc, argv);
    if (rc) return rc;

    CliLogFile = DefaultCLiLogFileName(appName);
    InitLogging();
    Log(info, "Starting {}", appName);

    QCoreApplication qt_app(argc, argv);

    string target = format("{}:{}", server, port);
    RemSvcClient client(
        grpc::CreateChannel(target, grpc::InsecureChannelCredentials()));

    while (ping > 0) {
        PingResult pr = client.doPing(ping--);
        Log(info, "Ping {}", pr.ok ? "ok" : "failed");
    }

    if (!cmd.empty())
        rc = client.doRemCmd(cmd);

    Log(info, "Stopping {}", appName);
    TermLogging();
    return rc;
}
