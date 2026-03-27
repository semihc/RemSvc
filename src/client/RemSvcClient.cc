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

// Absl includes
#include <absl/flags/flag.h>
#include <absl/flags/parse.h>
#include <absl/time/time.h>

// gRPC includes
#include <grpcpp/grpcpp.h>

// Prj includes
#include "RemSvc.grpc.pb.h"
#include <QCoreApplication>
#include <QDebug>
#include "CLI.hh"
#include "Log.hh"

// Std
using namespace std;
using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;

// Prj
using namespace RS;
using RS::RemSvc;


ABSL_FLAG(std::string, target, "localhost:50051", "Server address");

/*ERASE
using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using helloworld::Greeter;
using helloworld::HelloReply;
using helloworld::HelloRequest;
*/

class RemSvcClient {
 public:
  RemSvcClient(std::shared_ptr<Channel> channel)
      : stub_(RemSvc::NewStub(channel)) {}

    // Assembles the client's payload, sends it and presents the response back
    // from the server.
    // Send a Ping message
    string doPing(const int seq=0);

    // Send a RemCmd (Remote Command) message
    int doRemCmd(std::string_view cmd);

 private:
  std::unique_ptr<RemSvc::Stub> stub_;
};


string RemSvcClient::doPing(const int seq)
{
    PingMsg ping;
    PongMsg pong;
	 
    ping.set_seq(seq);
	 
    // Get the current time as an absl::Time object.
    absl::Time now = absl::Now();
    // Convert the absl::Time to a Unix timestamp (seconds since the epoch).
    int64_t timestamp = absl::ToUnixSeconds(now);
    ping.set_time( timestamp );
	
    // You can also use the local time zone:
    std::string isoStr = absl::FormatTime(absl::RFC3339_full, now, absl::LocalTimeZone());
	
    Log(info, "Current time in ISO 8601 format (Local): {}", isoStr);
	
    ping.set_data( isoStr.data() );
	
	

    // Context for the client. It could be used to convey extra information to
    // the server and/or tweak certain RPC behaviors.
    ClientContext context;

    // The actual RPC.
    grpc::Status status = stub_->Ping(&context, ping, &pong);
    // Act upon its status.
    if (status.ok()) {
	//-return pong.message();
	//-std::cerr << "seqId: " << pong.seq() << std::endl;
	Log(info, "seqId: {}", pong.seq());
	return "RPC successful";
    } else {
	//-std::cout << status.error_code() << ": " << status.error_message()
	//-<< std::endl;
	Log(error, "{}: {}",  (int)status.error_code(), status.error_message());
	return "RPC failed";
    }

}


int RemSvcClient::doRemCmd(std::string_view cmd)
{
    RemCmdMsg rcmd; // Remote command message
    RemResMsg rres; // Remote response message
    int rv{0};


    // Context for the client. 
    ClientContext context;
    // The actual RPC.
    Log(info,"Sending command: {}", cmd);
    rcmd.set_cmd( string(cmd) );
    grpc::Status status = stub_->RemCmd(&context, rcmd, &rres);
    if (status.ok()) {
	Log(info,"RPC successful");
	string out = rres.out();
	string err = rres.err();
	rv = rres.rc();
	Log(info, "rc={} out={}", rv, out);
	if(!err.empty())
	    Log(error, "err={}", err);
    } else {
	Log(error, "RPC failed {}: {}",  (int)status.error_code(), status.error_message());
	rv = 1; // Failure
    }

    
    return rv;
}


int main0(int argc, char** argv) {
  absl::ParseCommandLine(argc, argv);

  // Instantiate the client. It requires a channel, out of which the actual RPCs
  // are created. This channel models a connection to an endpoint specified by
  // the argument "--target=" which is the only expected argument.
  std::string target_str = absl::GetFlag(FLAGS_target);
  // We indicate that the channel isn't authenticated (use of
  // InsecureChannelCredentials()).
  RemSvcClient client(
      grpc::CreateChannel(target_str, grpc::InsecureChannelCredentials()));
	  
  /*ERASE
  std::string user("world");
  std::string reply = greeter.SayHello(user);
  std::cout << "Greeter received: " << reply << std::endl;
  reply = greeter.SayHelloAgain(user);
  std::cout << "Greeter received: " << reply << std::endl;
  */
  
  std::string reply = client.doPing(1001);
  std::cout << "Client received: " << reply << std::endl;
  
  return 0;
}




int main2(int argc, char *argv[]) {
    std::string filename;
    int verbosity = 0;

    CLI::App cli_app{"My Qt App"};
    cli_app.add_option("-f,--file", filename, "Input filename")->required();
    cli_app.add_option("-v,--verbose", verbosity, "Verbosity level");

    CLI11_PARSE(cli_app, argc, argv);

    QCoreApplication qt_app(argc, argv); // Qt app initialized

    qDebug() << "Filename:" << QString::fromStdString(filename);
    qDebug() << "Verbosity:" << verbosity;

    // ... rest of your Qt application ...

    return qt_app.exec();
}




int main(int argc, char *argv[]) {
    int rc{};
    std::string_view appName{ argv[0] };

    
    
    // Command line application
    CLI::App cli_app;
    string server{"localhost"};
    cli_app.add_option("-s,--server", server, "Server");
    int port{50051};
    cli_app.add_option("-p,--port", port, "Port");
    int ping{0};
    cli_app.add_option("-n,--ping", ping, "Ping Sequence");   
    string cmd{};
    cli_app.add_option("-c,--cmd", cmd, "Command");
    
    
    rc = ConfigureCLI(cli_app, argc, argv);
    if(rc) return rc;


    // Specify the default log file
    CliLogFile = DefaultCLiLogFileName(appName);
    InitLogging();  // Initialize logging components
    Log(info,"Starting {}", appName);

    // Qt core application
    QCoreApplication qt_app(argc, argv);
    //rc = qt_app.exec();


    // Construct target endpoint string
    string target = format("{}:{}", server, port);
  
   
    // We indicate that the channel isn't authenticated (use of
    // InsecureChannelCredentials()).
    RemSvcClient client(
      grpc::CreateChannel(target, grpc::InsecureChannelCredentials()));

    // Execute pings as required
    while(ping > 0) {
	string reply = client.doPing(ping--);
	Log("Client received: {}", reply);
    }

    if(! cmd.empty()) {
	int rv = client.doRemCmd(cmd);
    }
    
    Log(info,"Stopping {}", appName);
    TermLogging();  // Terminate logging 
    return rc;
}
