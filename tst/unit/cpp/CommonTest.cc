#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <vector>

#include "Assert.hh"
#include "CLI.hh"
#include "Hash.hh"
#include "Log.hh"


// ---------------------------------------------------------------------------
// RS::Expects
// ---------------------------------------------------------------------------

TEST(Expects, PassesOnTrue)
{
    EXPECT_NO_THROW(RS::Expects(true));
}

TEST(Expects, ThrowsOnFalse)
{
    EXPECT_THROW(RS::Expects(false), std::runtime_error);
}

TEST(Expects, ErrorMessageContainsLocation)
{
    try {
        RS::Expects(false);
        FAIL() << "Expected std::runtime_error";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("Expect failure"), std::string::npos);
    }
}


// ---------------------------------------------------------------------------
// RS::Ensures
// ---------------------------------------------------------------------------

TEST(Ensures, PassesOnTrue)
{
    EXPECT_NO_THROW(RS::Ensures(true));
}

TEST(Ensures, ThrowsOnFalse)
{
    EXPECT_THROW(RS::Ensures(false), std::runtime_error);
}

TEST(Ensures, ErrorMessageContainsLocation)
{
    try {
        RS::Ensures(false);
        FAIL() << "Expected std::runtime_error";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("Ensure failure"), std::string::npos);
    }
}


// ---------------------------------------------------------------------------
// RS::StrToLogLevel
// ---------------------------------------------------------------------------

TEST(StrToLogLevel, KnownLevels)
{
    EXPECT_EQ(RS::StrToLogLevel("trace"),    RS::trace);
    EXPECT_EQ(RS::StrToLogLevel("debug"),    RS::debug);
    EXPECT_EQ(RS::StrToLogLevel("info"),     RS::info);
    EXPECT_EQ(RS::StrToLogLevel("warn"),     RS::warn);
    EXPECT_EQ(RS::StrToLogLevel("error"),    RS::error);
}

TEST(StrToLogLevel, UnknownLevelDefaultsToInfo)
{
    EXPECT_EQ(RS::StrToLogLevel("bogus"),    RS::info);
    EXPECT_EQ(RS::StrToLogLevel(""),         RS::info);
    EXPECT_EQ(RS::StrToLogLevel("TRACE"),    RS::info); // case-sensitive
}


// ---------------------------------------------------------------------------
// RS::DefaultCLiLogFileName
// ---------------------------------------------------------------------------

TEST(DefaultCLiLogFileName, ReturnsNonEmptyString)
{
    auto result = RS::DefaultCLiLogFileName("myapp");
    EXPECT_FALSE(result.empty());
}

TEST(DefaultCLiLogFileName, PreservesAppName)
{
    auto result = RS::DefaultCLiLogFileName("myapp");
    EXPECT_NE(result.find("myapp"), std::string::npos);
}

TEST(DefaultCLiLogFileName, AppendsLogExtension)
{
    EXPECT_EQ(RS::DefaultCLiLogFileName("myapp"), "myapp.log");
}

TEST(DefaultCLiLogFileName, ReplacesExistingExtension)
{
    EXPECT_EQ(RS::DefaultCLiLogFileName("myapp.exe"), "myapp.log");
}


// ---------------------------------------------------------------------------
// ConfigureCLI
// ---------------------------------------------------------------------------

// Resets CLI globals before each test; builds a mutable argv from strings.
class ConfigureCLITest : public ::testing::Test {
protected:
    void SetUp() override {
        RS::CliLogFile  = "";
        RS::CliLogLevel = "";
        RS::CliDbgLevel = -1;
    }

    // Returns {argc, argv} backed by the supplied string vector.
    static std::pair<int, std::vector<char*>> argv(std::vector<std::string>& args) {
        std::vector<char*> v;
        for (auto& s : args) v.push_back(s.data());
        return { static_cast<int>(v.size()), v };
    }
};

TEST_F(ConfigureCLITest, NoOptionsReturnsZero)
{
    std::vector<std::string> args = {"prog"};
    auto [argc, av] = argv(args);
    CLI::App app;
    EXPECT_EQ(RS::ConfigureCLI(app, argc, av.data()), 0);
}

TEST_F(ConfigureCLITest, LogFileOptionSetsGlobal)
{
    std::vector<std::string> args = {"prog", "--log-file", "out.log"};
    auto [argc, av] = argv(args);
    CLI::App app;
    ASSERT_EQ(RS::ConfigureCLI(app, argc, av.data()), 0);
    EXPECT_EQ(RS::CliLogFile, "out.log");
}

TEST_F(ConfigureCLITest, LogLevelOptionSetsGlobal)
{
    std::vector<std::string> args = {"prog", "--log-level", "warn"};
    auto [argc, av] = argv(args);
    CLI::App app;
    ASSERT_EQ(RS::ConfigureCLI(app, argc, av.data()), 0);
    EXPECT_EQ(RS::CliLogLevel, "warn");
}

TEST_F(ConfigureCLITest, DebugLevelOptionSetsGlobal)
{
    std::vector<std::string> args = {"prog", "-d", "5"};
    auto [argc, av] = argv(args);
    CLI::App app;
    ASSERT_EQ(RS::ConfigureCLI(app, argc, av.data()), 0);
    EXPECT_EQ(RS::CliDbgLevel, 5);
}

TEST_F(ConfigureCLITest, DebugLevelZeroIsValid)
{
    std::vector<std::string> args = {"prog", "-d", "0"};
    auto [argc, av] = argv(args);
    CLI::App app;
    ASSERT_EQ(RS::ConfigureCLI(app, argc, av.data()), 0);
    EXPECT_EQ(RS::CliDbgLevel, 0);
}

TEST_F(ConfigureCLITest, DebugLevelOutOfRangeReturnsOne)
{
    std::vector<std::string> args = {"prog", "-d", "10"};
    auto [argc, av] = argv(args);
    CLI::App app;
    EXPECT_EQ(RS::ConfigureCLI(app, argc, av.data()), 1);
}

TEST_F(ConfigureCLITest, UnknownOptionReturnsOne)
{
    std::vector<std::string> args = {"prog", "--no-such-option"};
    auto [argc, av] = argv(args);
    CLI::App app;
    EXPECT_EQ(RS::ConfigureCLI(app, argc, av.data()), 1);
}

TEST_F(ConfigureCLITest, VersionFlagReturnsOne)
{
    std::vector<std::string> args = {"prog", "--version"};
    auto [argc, av] = argv(args);
    CLI::App app;
    EXPECT_EQ(RS::ConfigureCLI(app, argc, av.data()), 1);
}


// ---------------------------------------------------------------------------
// InitLogging / TermLogging / GetLogger
// ---------------------------------------------------------------------------

class LoggingTest : public ::testing::Test {
protected:
    void TearDown() override {
        // Clean up if a test called InitLogging but didn't call TermLogging.
        if (RS::MainLogger)
            RS::TermLogging();
    }
};

TEST_F(LoggingTest, InitLoggingReturnsZero)
{
    EXPECT_EQ(RS::InitLogging(), 0);
}

TEST_F(LoggingTest, TermLoggingReturnsZero)
{
    RS::InitLogging();
    EXPECT_EQ(RS::TermLogging(), 0);
}

TEST_F(LoggingTest, GetLoggerThrowsBeforeInit)
{
    EXPECT_THROW(RS::GetLogger(), std::runtime_error);
}

TEST_F(LoggingTest, GetLoggerReturnsNonNullAfterInit)
{
    RS::InitLogging();
    EXPECT_NE(RS::GetLogger(), nullptr);
}

TEST_F(LoggingTest, GetLoggerThrowsAfterTerm)
{
    RS::InitLogging();
    RS::TermLogging();
    EXPECT_THROW(RS::GetLogger(), std::runtime_error);
}

TEST_F(LoggingTest, SetLogLevelDoesNotThrow)
{
    RS::InitLogging();
    EXPECT_NO_THROW(RS::SetLogLevel(RS::warn));
}


// ---------------------------------------------------------------------------
// RS::crc32Hex
// ---------------------------------------------------------------------------

TEST(HashTest, EmptyStringIsKnownValue)
{
    // CRC-32/ISO-HDLC of "" = 0x00000000
    EXPECT_EQ(RS::crc32Hex(""), "00000000");
}

TEST(HashTest, KnownVector)
{
    // CRC-32 of "123456789" = 0xCBF43926 (standard check vector)
    EXPECT_EQ(RS::crc32Hex("123456789"), "cbf43926");
}

TEST(HashTest, DifferentInputsDifferentHashes)
{
    EXPECT_NE(RS::crc32Hex("hello"), RS::crc32Hex("world"));
}

TEST(HashTest, SameInputSameHash)
{
    EXPECT_EQ(RS::crc32Hex("echo hi"), RS::crc32Hex("echo hi"));
}

TEST(HashTest, OutputIsEightHexChars)
{
    auto h = RS::crc32Hex("any string");
    EXPECT_EQ(h.size(), 8u);
    for (char c : h)
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
}
