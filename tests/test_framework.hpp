#pragma once

// Deliberately not pulling in a third-party test framework (Catch2,
// GoogleTest, ...) -- for a project this size a ~30-line header keeps the
// build hermetic (no fetched dependency) and the failure output just as
// readable.
#include <iostream>
#include <string>

namespace testing {

inline int& failure_count() {
    static int count = 0;
    return count;
}

inline void check(bool condition, const std::string& description, const char* file, int line) {
    if (condition) {
        std::cout << "  [PASS] " << description << "\n";
    } else {
        std::cout << "  [FAIL] " << description << "  (" << file << ":" << line << ")\n";
        ++failure_count();
    }
}

} // namespace testing

#define CHECK(condition, description) ::testing::check((condition), (description), __FILE__, __LINE__)

#define RUN_TEST(fn)                        \
    do {                                    \
        std::cout << #fn << ":\n";          \
        fn();                               \
    } while (0)

#define TEST_MAIN_EXIT()                                                          \
    do {                                                                          \
        int failures = ::testing::failure_count();                               \
        std::cout << "\n" << (failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED") \
                  << " (" << failures << " failure" << (failures == 1 ? "" : "s") \
                  << ")\n";                                                       \
        return failures == 0 ? 0 : 1;                                             \
    } while (0)
