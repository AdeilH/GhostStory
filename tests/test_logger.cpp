// addu94 — tests/test_logger.cpp
#include "test_framework.hpp"

#include "log/logger.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

using addu94::log::Level;

namespace {
std::filesystem::path temp_log_path() {
    auto p = std::filesystem::temp_directory_path() /
             ("addu94_test_log_" +
              std::to_string(
                  std::chrono::steady_clock::now().time_since_epoch().count()) +
              ".jsonl");
    return p;
}
}  // namespace

TEST(logger, writes_jsonl_lines) {
    auto path = temp_log_path();
    addu94::log::Logger::init(path.string(), Level::Error);
    addu94::log::info("test", "hello {} = {}", 42, "answer");
    addu94::log::warn("test", "careful {}", 3.5);
    addu94::log::Logger::shutdown();

    std::ifstream f(path);
    CHECK(f.good());
    std::string line;
    int lines = 0;
    while (std::getline(f, line)) {
        ++lines;
        CHECK(line.starts_with("{\"ts\":"));
        CHECK(line.find("\"lvl\":") != std::string::npos);
        CHECK(line.ends_with("}"));
    }
    CHECK_EQ(lines, 2);
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(logger, truncates_long_subsystem_and_message) {
    auto path = temp_log_path();
    addu94::log::Logger::init(path.string(), Level::Error);
    std::string const long_sub(200, 's');
    std::string const long_msg(4000, 'm');
    addu94::log::info(long_sub, "{}", long_msg);
    addu94::log::Logger::shutdown();

    std::ifstream f(path);
    std::string line;
    CHECK(static_cast<bool>(std::getline(f, line)));
    CHECK(line.size() < 500);  // bounded entry size
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(logger, escaped_content_survives_json) {
    auto path = temp_log_path();
    addu94::log::Logger::init(path.string(), Level::Error);
    addu94::log::info("test", "quote \" backslash \\ newline \n end");
    addu94::log::Logger::shutdown();

    std::ifstream f(path);
    std::string line;
    CHECK(static_cast<bool>(std::getline(f, line)));
    // Line must still be a single JSON object (newlines escaped).
    CHECK(line.find("\"quote \\\" backslash") != std::string::npos);
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(logger, concurrent_producers_never_crash) {
    auto path = temp_log_path();
    addu94::log::Logger::init(path.string(), Level::Error);
    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&stop, t] {
            int i = 0;
            while (!stop.load(std::memory_order_relaxed) && i < 200) {
                addu94::log::info("thread", "t{} i{}", t, i++);
            }
        });
    }
    for (auto& th : threads) th.join();
    addu94::log::Logger::shutdown();
    CHECK(std::filesystem::file_size(path) > 0);
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_MAIN()
