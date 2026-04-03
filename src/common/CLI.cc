/** \file CLI.cc
 * Definitions for CLI Command Line Flags support.
 *
 *
 *
 */

#include "CLI.hh"
#include <filesystem>
#include <iostream>
#include "Log.hh"

using namespace std;
namespace fs = std::filesystem;

namespace RS {

  
  // Globals
  string CliLogFile{};
  //-int CliLogLevel1{info};
  //-log_level_enum CliLogLevel2 { info };
  string CliLogLevel;
  int CliDbgLevel{-1};



  // Convert string value to the enum value for Log Levels
  int StrToLogLevel(std::string_view lvlstr) {
    if(lvlstr == "trace") return trace;
    if(lvlstr == "debug") return debug;
    if(lvlstr == "info") return info;
    if(lvlstr == "warn") return warn;
    if(lvlstr == "error") return error;
    // Unrecognized strings default to info
    return info;     
  }


  std::string DefaultCLiLogFileName(std::string_view appName)
  {
    fs::path ap = appName;
    ap.replace_extension(".log");
    return ap.string();
  }
  

  int ConfigureCLI(CLI::App& app, int argc, char* const argv[])
  {
    auto vn = app.add_flag("-v,--version", "Version number");

    auto lf = app.add_option("--log-file", CliLogFile, "Log file")
      ->take_last();

    auto dl = app.add_option("-d,--debug", CliDbgLevel, "Debug level (0-9) 0=off 9=highest")
      ->take_last()->check(CLI::Range(0, 9))->envname("DBGLVL");

    auto ll = app.add_option("-l,--log-level", CliLogLevel, "Set log level (error, warn, info, debug, trace)")
      ->take_last();
      //? ll->check(CLI::IsMember({trace,debug,info,warn, error}));
      //? ->transform(CLI::CheckedTransformer(StrToLogLevel, {"error", "warn", "info", "debug", "trace"}));

    try {
      app.footer(""); // Disable automatic generation of help message
      app.parse(argc, argv); // Parse the command line arguments, ini files, env variables
    } 
    catch (const CLI::ParseError& e) {
      //? Log(error, "CLI::ParseError Ex: {} {}", e.get_name(), e.what());
      app.exit(e);
      return 1; // Exception caught, note to the caller
    }


    if(app.get_option("--version")->as<bool>()) { 
        const char* Ver = "1.0.0";
        cout << argv[0] << " Version: " << Ver << endl;
        return 1; // Similar to --help, demand exit from app
    }

    return 0; // Successful parsing
  }


} // namespace




