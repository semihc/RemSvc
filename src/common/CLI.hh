#ifndef RS_CLI_HH
#define RS_CLI_HH
#pragma once


// Std
#include <string>
#include <string_view>
// Prj
#include <CLI/CLI.hpp>



namespace RS { 

  // Globals to store
  extern std::string CliLogFile;
  extern std::string CliLogLevel;
  extern int CliDbgLevel;

  // Convert string reprentation of a log level to enum (int) value
  int StrToLogLevel(std::string_view lvlstr);

  // Get default Cli Log File Name
  std::string DefaultCLiLogFileName(std::string_view appName);

  // Configure CLI options for a given application
  int ConfigureCLI(CLI::App& app, int argc, char* const argv[]);

}

#endif /* Include guard */
