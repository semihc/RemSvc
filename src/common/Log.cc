
#include "Log.hh"
// Std includes
#include <exception>
#include <stdexcept>
#include <vector>
// SpdLog includes
#include <spdlog/spdlog.h>
#include <spdlog/cfg/env.h>
#include <spdlog/cfg/argv.h>
#include <spdlog/fmt/ostr.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/basic_file_sink.h>
// Prj includes
#include "CLI.hh"

using namespace std;


namespace RS {

// Currently allowed debug level, inclusive
DebugLevels AllowedDebugLevel{ DebugLevels::debugmax };

// Current log file if defined
string LogFileName;

// Currently used Log Sinks
vector<spdlog::sink_ptr> LogSinks;

// Main (default) Logger, used application wide
LoggerPtr_t MainLogger;



int InitLogging(int lvl)
{
    // Transfer values from the CLI variables
    if(!CliLogFile.empty()) LogFileName = CliLogFile;
    if(CliDbgLevel >=0)  { 
        AllowedDebugLevel = static_cast<DebugLevels>(CliDbgLevel);
        if(CliDbgLevel > 0) lvl = debug; // Non zero debug level assumes debug logging
        else lvl = info; // Zero debug level assumes info or less
    }
    if(!CliLogLevel.empty()) { 
        lvl = StrToLogLevel(CliLogLevel);
        spdlog::info("Setting Log level to {} -> {}", CliLogLevel, lvl);
        spdlog::set_level(static_cast<spdlog::level::level_enum>(lvl));
    }

    // Logging to multiple sinks
    // https://github.com/gabime/spdlog/wiki/2.-Creating-loggers
    spdlog::sink_ptr conSink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    //? conSink->set_level( static_cast<spdlog::level::level_enum>(lvl) );
    conSink->set_level( spdlog::level::trace ); // Make it highest level
    conSink->set_pattern("%C%m%d %H%M%S [%^%l%$] %v");
    LogSinks.push_back(conSink);
    

    if(!LogFileName.empty()) {
        spdlog::sink_ptr fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(LogFileName, 10*1024*1024, 10);
        //? fileSink->set_level( static_cast<spdlog::level::level_enum>(lvl) );
        fileSink->set_level( spdlog::level::trace ); // Make it highest level
        //? fileSink->set_pattern("%C%m%d %H%M%S.%e[%n-%P-%t-%l] %v");
        fileSink->set_pattern("%C%m%d %H%M%S.%e[%t-%l] %v"); 
        LogSinks.push_back(fileSink); 
    }


    MainLogger = std::make_shared<spdlog::logger>(MainLogName, begin(LogSinks), end(LogSinks));
    MainLogger->set_level( static_cast<spdlog::level::level_enum>(lvl) );
    // Register the combined logger to access it globally
    spdlog::register_logger(MainLogger);
    // Replace the default logger:
    // https://github.com/gabime/spdlog/wiki/Default-logger
    spdlog::set_default_logger(MainLogger);

    return 0;
}


int TermLogging()
{
    MainLogger->flush();
    LogSinks.clear();
    MainLogger.reset();
    spdlog::drop(MainLogName);
    //? spdlog::drop_all();

    return 0;
}


// Get a logger specified by a name. If name not specfified then get the main logger
LoggerPtr_t GetLogger(const char* logger_name)
{
    auto logger = spdlog::get( string(logger_name ? logger_name : MainLogName) );
    if(!logger)
        throw runtime_error("Spdlog not initialized.");

    return logger;
}


} // end namespace
