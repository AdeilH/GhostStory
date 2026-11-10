// ghost_story — tests/test_logger.cpp
#include "test_framework.hpp"

#include "log/logger.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <thread>

using ghost_story::log::Level;

namespace {
std::filesystem::path temp_log_path() {
    auto p = std::filesystem::temp_directory_path() /
             ("ghost_story_test_log_" +
              std::to_string(
                  std::chrono::steady_clock::now().time_since_epoch().count()) +
              ".jsonl");
    return p;
}
}  // namespace

TEST(logger, writes_jsonl_lines) {
    auto path = temp_log_path();
    ghost_story::log::Logger::init(path.string(), Level::Error);
    ghost_story::log::info("test", "hello {} = {}", 42, "answer");
    ghost_story::log::warn("test", "careful {}", 3.5);
    ghost_story::log::Logger::shutdown();

    std::ifstream f(path);
    CHECK(f.good());
    std::string line;
    int lines = 0;
    while (std::getline(f, line)) {
        ++lines;
        // Schema v3: wall-clock ts_ns, ready-to-index ISO-8601 UTC
        // @timestamp (what Elasticsearch/Kibana index as the doc
        // timestamp), and the per-run 8-hex id.
        CHECK(line.starts_with("{\"ts_ns\":"));
        CHECK(line.find("\"@timestamp\":\"20") != std::string::npos);
        CHECK(line.find("\"lvl\":") != std::string::npos);
        std::size_t const run_pos = line.find("\"run\":\"");
        CHECK(run_pos != std::string::npos);
        if (run_pos != std::string::npos) {
            std::string const run =
                line.substr(run_pos + 7, 8);
            CHECK_EQ(run.size(), std::size_t{8});
            bool all_hex = true;
            for (char const c : run) {
                bool const hex = (c >= '0' && c <= '9') ||
                                 (c >= 'a' && c <= 'f');
                all_hex = all_hex && hex;
            }
            CHECK(all_hex);
        }
        CHECK(line.ends_with("}"));
    }
    CHECK_EQ(lines, 2);
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(logger, thread_name_lands_in_the_line) {
    auto path = temp_log_path();
    ghost_story::log::Logger::init(path.string(), Level::Error);
    ghost_story::log::Logger::set_thread_name("worker");
    ghost_story::log::info("test", "from a named thread");
    ghost_story::log::Logger::shutdown();

    std::ifstream f(path);
    std::string line;
    CHECK(static_cast<bool>(std::getline(f, line)));
    CHECK(line.find("\"th\":\"worker\"") != std::string::npos);
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(logger, run_id_is_stable_within_a_run) {
    auto path = temp_log_path();
    ghost_story::log::Logger::init(path.string(), Level::Error);
    std::string_view const id = ghost_story::log::Logger::run_id();
    CHECK_EQ(id.size(), std::size_t{8});
    ghost_story::log::info("test", "one");
    ghost_story::log::info("test", "two");
    ghost_story::log::Logger::shutdown();

    std::ifstream f(path);
    std::string line;
    int matches = 0;
    while (std::getline(f, line)) {
        if (line.find(std::format("\"run\":\"{}\"", id)) !=
            std::string::npos) {
            ++matches;
        }
    }
    CHECK_EQ(matches, 2);
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(logger, rotates_oversized_file_on_init) {
    auto path = temp_log_path();
    auto const rotated = path.string() + ".1";
    // Pre-existing file above the rotation cap must move to ".1"; the fresh
    // run then appends into a new file.
    {
        std::ofstream seed(path);
        CHECK(seed.good());
    }
    std::filesystem::resize_file(
        path, static_cast<std::uintmax_t>(ghost_story::log::Logger::kRotateBytes) + 1);
    ghost_story::log::Logger::init(path.string(), Level::Error);
    ghost_story::log::info("test", "after rotation");
    ghost_story::log::Logger::shutdown();

    CHECK(std::filesystem::file_size(rotated) >=
          static_cast<std::uintmax_t>(ghost_story::log::Logger::kRotateBytes));
    std::ifstream f(path);
    std::string line;
    CHECK(static_cast<bool>(std::getline(f, line)));
    CHECK(line.find("after rotation") != std::string::npos);
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(rotated, ec);
}

TEST(logger, truncates_long_subsystem_and_message) {
    auto path = temp_log_path();
    ghost_story::log::Logger::init(path.string(), Level::Error);
    std::string const long_sub(200, 's');
    std::string const long_msg(4000, 'm');
    ghost_story::log::info(long_sub, "{}", long_msg);
    ghost_story::log::Logger::shutdown();

    std::ifstream f(path);
    std::string line;
    CHECK(static_cast<bool>(std::getline(f, line)));
    CHECK(line.size() < 500);  // bounded entry size
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(logger, escaped_content_survives_json) {
    auto path = temp_log_path();
    ghost_story::log::Logger::init(path.string(), Level::Error);
    ghost_story::log::info("test", "quote \" backslash \\ newline \n end");
    ghost_story::log::Logger::shutdown();

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
    ghost_story::log::Logger::init(path.string(), Level::Error);
    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&stop, t] {
            int i = 0;
            while (!stop.load(std::memory_order_relaxed) && i < 200) {
                ghost_story::log::info("thread", "t{} i{}", t, i++);
            }
        });
    }
    for (auto& th : threads) th.join();
    ghost_story::log::Logger::shutdown();
    CHECK(std::filesystem::file_size(path) > 0);
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_MAIN()
