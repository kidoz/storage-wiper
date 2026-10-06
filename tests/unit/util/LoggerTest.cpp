#include "util/Logger.hpp"

#include "fixtures/TestFixtures.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

class LoggerTest : public testing::Test {
protected:
    TempTestFile temporary;
    std::filesystem::path directory;
    util::Logger& logger = util::Logger::instance();

    void SetUp() override {
        ASSERT_TRUE(temporary.valid());
        directory = temporary.path() + "-logs";
        logger.shutdown();
        logger.set_console_output(false);
    }
    void TearDown() override {
        logger.shutdown();
        std::filesystem::remove_all(directory);
    }
    auto read(const std::string& name) -> std::string {
        std::ifstream file{directory / name};
        std::ostringstream text;
        text << file.rdbuf();
        return text.str();
    }
};

TEST_F(LoggerTest, FailedReinitializationClearsStateAndCanRecover) {
    ASSERT_TRUE(logger.initialize(directory, "test"));
    EXPECT_FALSE(logger.initialize(temporary.path(), "test"));
    EXPECT_FALSE(logger.is_initialized());
    EXPECT_TRUE(logger.get_log_file_path().empty());
    ASSERT_TRUE(logger.initialize(directory, "test"));
    logger.info("test", "recovered message");
    EXPECT_NE(read("test.log").find("recovered message"), std::string::npos);
}

TEST_F(LoggerTest, RotationKeepsNewestLogsAndRemovesOldest) {
    ASSERT_TRUE(logger.initialize(directory, "test", util::LogLevel::DEBUG,
                                  {.max_file_size_bytes = 1, .max_files = 2}));
    for (const auto* message :
         {"first message", "second message", "third message", "fourth message"}) {
        logger.info("test", message);
    }
    EXPECT_NE(read("test.log").find("fourth message"), std::string::npos);
    EXPECT_NE(read("test.1.log").find("third message"), std::string::npos);
    EXPECT_NE(read("test.2.log").find("second message"), std::string::npos);
    EXPECT_EQ(read("test.2.log").find("first message"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(directory / "test.3.log"));
}

TEST_F(LoggerTest, FailedRotationKeepsLoggingAndDoesNotRetryEveryLine) {
    ASSERT_TRUE(logger.initialize(directory, "test", util::LogLevel::DEBUG,
                                  {.max_file_size_bytes = 1, .max_files = 2}));
    logger.info("test", "first message");
    const auto blocked = directory / "test.2.log";
    std::filesystem::create_directory(blocked);
    std::ofstream{blocked / "entry"} << "blocks removal";
    logger.info("test", "second message");
    std::filesystem::remove_all(blocked);
    logger.info("test", "third message");
    EXPECT_TRUE(logger.is_initialized());
    EXPECT_NE(read("test.log").find("second message"), std::string::npos);
    EXPECT_NE(read("test.log").find("third message"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(directory / "test.1.log"));
}
