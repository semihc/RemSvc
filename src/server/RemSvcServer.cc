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

// Abseil includes
#include <absl/flags/flag.h>
#include <absl/flags/parse.h>
#include <absl/strings/str_format.h>

// gRPC std includes
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>

// Prj includes
#include "RemSvc.grpc.pb.h"
#include <QProcess>
#include <QCoreApplication>
#include <QThread>
#include <QDebug>
#include "CLI.hh"
#include "Log.hh"
#include "Assert.hh"
#include "SignalHandler.hh"



// Std
using grpc::Server;
using grpc::ServerBuilder; 
using grpc::ServerContext;
using grpc::Status;

// Prj
using namespace std;
using namespace RS;
using RS::RemSvc;

/* ERASE:
using Rem::PingMsg;
using Rem::PongMsg;
using Rem::StatusMsg;
using Rem::RemCmdMsg;
using Rem::RemResMsg;
*/

ABSL_FLAG(uint16_t, port, 50051, "Server port for the service");



int runInProcess(std::string_view cmd, string& out, string& err)
{
    QProcess prg;

#ifdef Q_OS_WIN
    // On Windows, run through cmd.exe so built-ins like 'dir' work
    prg.setProgram("cmd.exe");
    prg.setArguments({"/C", QString::fromUtf8(cmd.data(),
                                              static_cast<qsizetype>(cmd.size()))});
#else
    prg.setProgram("/bin/sh");
    prg.setArguments({"-c", QString::fromUtf8(cmd.data(),
                                              static_cast<qsizetype>(cmd.size()))});
#endif


    //-prg.startCommand(cmd.data());
    prg.start();

    int waitPeriod = 30000; // msecs
    bool finished = prg.waitForFinished(waitPeriod);
    //-bool readyRead = prg.waitForReadyRead(waitPeriod);
    //-Log(debug,"f={} r={}", finished, readyRead);
    
    QByteArray errStr = prg.readAllStandardError();
    QByteArray outStr = prg.readAllStandardOutput();    
    int rv = prg.exitCode();

    out = outStr.constData();
    Log(debug) << outStr.data();
    if(!errStr.isEmpty()) {
	  err = errStr.constData();
	  Log(error) << errStr.data();
    }
    
    return rv;
}


// Logic and data behind the server's behavior.
class RemSvcServiceImpl final : public RemSvc::Service {

    grpc::Status Ping(ServerContext* context, 
		      const RS::PingMsg* ping,
		      RS::PongMsg* pong) override {
	
	Log(info) << "Received with ping seq id=" << ping->seq() 
		<< " time=" << ping->time()
		<< " data=" << ping->data() ;	
	pong->set_seq( ping->seq() );
	return grpc::Status::OK;
    }

  
    grpc::Status RemCmd(ServerContext* context,
			const RS::RemCmdMsg* req,
			RS::RemResMsg* res) override {
      
	string cmd = req->cmd();
	Log(info, "Received cmd={}", cmd);
	string out, err;
	int rc = runInProcess(cmd, out, err);
      
	res->set_out(out);
	res->set_err(err);
	res->set_rc(rc);
	
	Log(debug,"rc={}, out={} err={}", rc, out, err);

	return grpc::Status::OK;
    }
  


};



//ERASE
void RunServer(uint16_t port) {
  
  //
  RemSvcServiceImpl service;
  
  // Lustening port of the service
  std::string server_address = absl::StrFormat("0.0.0.0:%d", port);

  grpc::EnableDefaultHealthCheckService(true);
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();
  ServerBuilder builder;
  
  // Listen on the given address without any authentication mechanism.
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  
  // Register "service" as the instance through which we'll communicate with
  // clients. In this case it corresponds to an *synchronous* service.  
  builder.RegisterService(&service);
  
  // Finally assemble the server.
  std::unique_ptr<Server> server(builder.BuildAndStart());
  Log(info) << "Server listening on " << server_address;
  //- << std::endl;

  // Wait for the server to shutdown. Note that some other thread must be
  // responsible for shutting down the server for this call to ever return.
  server->Wait();
}






class GrpcServerThread : public QThread {
    Q_OBJECT
public:
    // MEMBERS
    int m_port{};
    unique_ptr<Server> m_server{};
    unique_ptr<RemSvcServiceImpl> m_service{};

    // CREATORS
    GrpcServerThread(int port);

    // MODIFIERS
    void run() override;
    void stop();

signals:
    void shutdownRequested();
};


GrpcServerThread::GrpcServerThread(int port) : m_port(port)
{
 
  //-static RemSvcServiceImpl service;
  
  // Listening port of the service
  string server_address = absl::StrFormat("0.0.0.0:%d", port);

  grpc::EnableDefaultHealthCheckService(true);
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();
  ServerBuilder builder;
  
  // Listen on the given address without any authentication mechanism.
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());

  m_service = make_unique<RemSvcServiceImpl>();
  
  // Register "service" as the instance through which we'll communicate with
  // clients. In this case it corresponds to an *synchronous* service.  
  builder.RegisterService(m_service.get());
  
  // Finally assemble the server.
  m_server = std::move( builder.BuildAndStart() );
  Expects( m_server != nullptr );
  Log(info) << "gRPC Server created to listen on " << server_address;
    
}


void GrpcServerThread::run() {
    Log(info, "Starting gRPC Server in a separate thread...");

    // Run and wait Wait for the server to shutdown.
    // Note that some other thread must be
    // responsible for shutting down the server for this call to ever return.
    m_server->Wait();
}

void GrpcServerThread::stop() {
    Log(info, "Stopping gRPC Server...");
    m_server->Shutdown(); // Signal the server to shut down
    wait(); // Wait for the thread to exit
}



class GracefulShutdownHandler : public QObject {
    Q_OBJECT

public:
    GracefulShutdownHandler(QObject* parent = nullptr) : QObject(parent) {
        connect(qApp, &QCoreApplication::aboutToQuit, this, &GracefulShutdownHandler::handleQuit);
    }

private slots:
    void handleQuit() {
        Log(info) << "Ctrl+C or application close requested. Performing graceful shutdown...";

        // Perform your cleanup tasks here:
        // - Save settings
        // - Close files
        // - Release resources
        // - ...

        // Example: Save some data
        // ...

        // If you absolutely need to prevent the application from quitting (usually NOT recommended):
        // QCoreApplication::exit(0); // Use with caution!

	//? emit workerThread->shutdownRequested();
	//-qApp->quit();
    }
};



int main0(int argc, char** argv) {
  absl::ParseCommandLine(argc, argv);
  RunServer(absl::GetFlag(FLAGS_port));
  return 0;
}

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
    
    // Create an instance to handle the signal
    GracefulShutdownHandler shutdownHandler; 

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


// Qt spefic include, must be at the end
#include "RemSvcServer.moc"
