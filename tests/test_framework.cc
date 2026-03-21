// Verify the test framework itself works.
#include "test.h"

TEST(framework, check_pass) {
    CHECK(1 + 1 == 2);
    CHECK_EQ(3 * 3, 9);
    CHECK_NE(0, 1);
    CHECK_GT(5, 3);
    CHECK_LT(3, 5);
}

TEST(framework, check_fail_reported) {
    // This test intentionally has a failing check.
    // The framework should report it but continue.
    CHECK(true);
    CHECK(2 + 2 == 5);  // will fail
    CHECK(true);        // still runs after failure
}

TEST(framework, require_stops) {
    CHECK(true);
    REQUIRE(true);
    // If REQUIRE failed, we wouldn't reach here
    CHECK(true);
}

TEST(math, addition) {
    CHECK_EQ(1 + 1, 2);
    CHECK_EQ(0 + 0, 0);
    CHECK_EQ(-1 + 1, 0);
}

TEST(math, multiplication) {
    CHECK_EQ(2 * 3, 6);
    CHECK_EQ(0 * 100, 0);
}

int main(int argc, char** argv) {
    return rout::test::run_all(argc, argv);
}
