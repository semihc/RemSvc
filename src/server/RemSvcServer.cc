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

// Prj includes
#include "GrpcServerThread.hh"
#include <QCoreApplication>
#include "CLI.hh"
#include "Log.hh"
#include "SignalHandler.hh"



using namespace std;
using namespace RS;




int main(int argc, char *argv[]) {
    int rc{};
    std::string_view appName{ argv[0] };

    
    
    // Command line application
    CLI::App cli_app;
    int port{50051};
    cli_app.add_option("--port", port, "Port"); 
    rc = ConfigureCLI(cli_app, argc, argv);
    if(rc) return rc;


    // Specify the default log file
    CliLogFile = DefaultCLiLogFileName(appName);
    InitLogging();  // Initialize logging components
    Log(info,"Starting {}", appName);


    // Qt core application
    QCoreApplication qt_app(argc, argv);

    // Setup signal handling
    SignalHandler::setupSignalHandlers();
    SignalHandler signalHandler;
    QObject::connect(&signalHandler, &SignalHandler::quitRequested, &qt_app, &QCoreApplication::quit);
    
    // Start the gRPC server thread
    GrpcServerThread grpcThread{port};
    Log(info, "Starting gRPC Server at port {}", port);
    grpcThread.start();

    // Run the Qt event loop
    rc = qt_app.exec();

    Log(info, "Stopping gRPC Server");
    grpcThread.stop();

    
    Log(info,"Stopping application {}", appName);
    TermLogging();  // Terminate logging 
    return rc;
}

