// ghost_story — tests/test_framework.hpp
//
// Minimal dependency-free test framework. No external deps by design:
// CI runs offline. Each .cpp registers tests via static constructors;
// TEST_MAIN() must appear exactly once in the test target.
#pragma once

#include <cmath>
#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace ghost_story::test {

// Describe a value for failure messages: anything with operator<<, else "?".
template <typename T>
std::string describe(T const& v) {
    if constexpr (requires(std::ostringstream& os) { os << v; }) {
        std::ostringstream os;
        os << v;
        return os.str();
    } else {
        return "?";
    }
}

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

inline int& failures() {
    static int f = 0;
    return f;
}

inline bool& current_ok() {
    static bool ok = true;
    return ok;
}

struct Registrar {
    Registrar(std::string_view name, std::function<void()> fn) {
        registry().push_back(TestCase{std::string{name}, std::move(fn)});
    }
};

inline void report_failure(char const* file, int line, std::string const& msg) {
    current_ok() = false;
    std::fprintf(stderr, "  FAIL %s:%d  %s\n", file, line, msg.c_str());
}

#define GS_CONCAT_INNER(a, b) a##b
#define GS_CONCAT(a, b) GS_CONCAT_INNER(a, b)

#define TEST(suite, name)                                                     \
    static void GS_CONCAT(gs_test_, __LINE__)();                            \
    static ::ghost_story::test::Registrar GS_CONCAT(gs_reg_, __LINE__)(          \
        #suite "." #name, &GS_CONCAT(gs_test_, __LINE__));                  \
    static void GS_CONCAT(gs_test_, __LINE__)()

#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond))                                                          \
            ::ghost_story::test::report_failure(__FILE__, __LINE__, #cond);        \
    } while (0)

#define CHECK_EQ(a, b)                                                        \
    do {                                                                      \
        auto gs_va_ = (a);  /* copy: may reference a temporary's element */  \
        auto gs_vb_ = (b);                                                   \
        if (!(gs_va_ == gs_vb_)) {                                          \
            ::ghost_story::test::report_failure(                                   \
                __FILE__, __LINE__,                                           \
                std::string(#a) + " == " + #b + "  (got " +                   \
                    ::ghost_story::test::describe(gs_va_) + " vs " +              \
                    ::ghost_story::test::describe(gs_vb_) + ")");                 \
        }                                                                     \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                 \
    do {                                                                      \
        double const gs_va_ = static_cast<double>(a);                        \
        double const gs_vb_ = static_cast<double>(b);                        \
        if (!(std::abs(gs_va_ - gs_vb_) <= (tol))) {                        \
            ::ghost_story::test::report_failure(                                   \
                __FILE__, __LINE__,                                           \
                std::string(#a) + " ~= " + #b + "  (" +                       \
                    std::to_string(gs_va_) + " vs " +                        \
                    std::to_string(gs_vb_) + ", tol " +                      \
                    std::to_string(tol) + ")");                               \
        }                                                                     \
    } while (0)

inline int run_all(std::string_view binary_name) {
    std::size_t passed = 0;
    for (auto const& tc : registry()) {
        current_ok() = true;
        tc.fn();
        if (current_ok()) {
            ++passed;
        } else {
            ++failures();
            std::fprintf(stderr, "[  FAILED  ] %s\n", tc.name.c_str());
        }
    }
    std::printf("[==========] %zu tests from %s\n", registry().size(),
                binary_name.data());
    std::printf("[  PASSED  ] %zu tests\n", passed);
    if (failures() > 0) {
        std::printf("[  FAILED  ] %d tests\n", failures());
        return 1;
    }
    return 0;
}

}  // namespace ghost_story::test

#define TEST_MAIN()                                                           \
    int main() { return ::ghost_story::test::run_all("ghost_story-tests"); }
