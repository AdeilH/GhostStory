// ghost_story — tests/test_json.cpp
#include "test_framework.hpp"

#include <nlohmann/json.hpp>

#include <string>

using nlohmann::json;

TEST(json, roundtrip_basic) {
    json j;
    j["name"] = "mpk-mini-mk2";
    j["vendor_id"] = 0x09e8;
    j["ports"] = json::array({{{"name", "MPK Mini 2"}, {"caps", 36}}});
    auto const s = j.dump();
    auto const parsed = json::parse(s);
    CHECK_EQ(parsed["vendor_id"].get<int>(), 0x09e8);
    CHECK_EQ(parsed["ports"].size(), 1u);
    CHECK_EQ(parsed["ports"][0]["name"].get<std::string>(), "MPK Mini 2");
}

TEST(json, rejects_garbage_without_crash) {
    auto const result = json::parse("{not json!!", nullptr, false);
    CHECK(result.is_discarded());
}

TEST(json, dump_is_stable_for_manifest) {
    json j = {{"schema", 1}, {"model", "unknown"}};
    CHECK_EQ(j.dump(), R"({"model":"unknown","schema":1})");
}

TEST_MAIN()
