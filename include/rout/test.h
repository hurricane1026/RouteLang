#pragma once

// Lightweight test framework — zero stdlib dependency.
//
// Usage:
//   #include "rout/test.h"
//
//   TEST(suite, name) {
//       CHECK(1 + 1 == 2);
//       CHECK_EQ(foo(), 42);
//       REQUIRE(ptr != nullptr);  // stops test on failure
//   }
//
//   int main(int argc, char** argv) { return rout::test::run_all(argc, argv); }
//
// Filtering:
//   ./test_foo                       # run all
//   ./test_foo timer                 # run tests where suite or name contains "timer"
//   ./test_foo timer.refresh         # run suite "timer", test "refresh"
//   ./test_foo -l                    # list all tests without running

#include <unistd.h>  // write

namespace rout::test {

// --- Output ---

inline void out(const char* s) {
    int len = 0;
    while (s[len]) len++;
    (void)::write(1, s, len);
}

inline void out_int(int v) {
    if (v == 0) {
        (void)::write(1, "0", 1);
        return;
    }
    // Use unsigned to avoid INT_MIN overflow on negation
    unsigned uv;
    if (v < 0) {
        (void)::write(1, "-", 1);
        uv = static_cast<unsigned>(-(v + 1)) + 1u;
    } else {
        uv = static_cast<unsigned>(v);
    }
    char buf[16];
    int n = 0;
    while (uv > 0) {
        buf[n++] = static_cast<char>('0' + uv % 10);
        uv /= 10;
    }
    for (int i = n - 1; i >= 0; i--) (void)::write(1, &buf[i], 1);
}

// --- String helpers (no stdlib) ---

inline bool str_eq(const char* a, const char* b) {
    for (;; a++, b++) {
        if (*a != *b) return false;
        if (*a == '\0') return true;
    }
}

inline bool str_contains(const char* haystack, const char* needle) {
    if (!needle[0]) return true;
    for (int i = 0; haystack[i]; i++) {
        bool match = true;
        for (int j = 0; needle[j]; j++) {
            if (haystack[i + j] == '\0' || haystack[i + j] != needle[j]) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

// --- Test registry ---

struct TestCase {
    const char* suite;
    const char* name;
    void (*fn)(TestCase*);
    TestCase* next;
    int checks_passed;
    int checks_failed;
    const char* fail_file;
    int fail_line;
    const char* fail_expr;
};

inline TestCase* g_head = nullptr;
inline TestCase** g_tail = &g_head;

inline void register_test(TestCase* tc) {
    tc->next = nullptr;
    *g_tail = tc;
    g_tail = &tc->next;
}

// --- Check macros ---

#define CHECK(expr)                    \
    do {                               \
        if (expr) {                    \
            _tc->checks_passed++;      \
        } else {                       \
            _tc->checks_failed++;      \
            _tc->fail_file = __FILE__; \
            _tc->fail_line = __LINE__; \
            _tc->fail_expr = #expr;    \
        }                              \
    } while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))
#define CHECK_NE(a, b) CHECK((a) != (b))
#define CHECK_GT(a, b) CHECK((a) > (b))
#define CHECK_GE(a, b) CHECK((a) >= (b))
#define CHECK_LT(a, b) CHECK((a) < (b))
#define CHECK_LE(a, b) CHECK((a) <= (b))

#define REQUIRE(expr)                  \
    do {                               \
        if (expr) {                    \
            _tc->checks_passed++;      \
        } else {                       \
            _tc->checks_failed++;      \
            _tc->fail_file = __FILE__; \
            _tc->fail_line = __LINE__; \
            _tc->fail_expr = #expr;    \
            return;                    \
        }                              \
    } while (0)

#define REQUIRE_EQ(a, b) REQUIRE((a) == (b))
#define REQUIRE_NE(a, b) REQUIRE((a) != (b))

// --- Test definition ---

#define TEST(suite, name)                                                          \
    static void test_##suite##_##name(rout::test::TestCase*);                      \
    static rout::test::TestCase tc_##suite##_##name = {                            \
        #suite, #name, test_##suite##_##name, nullptr, 0, 0, nullptr, 0, nullptr}; \
    __attribute__((constructor)) static void reg_##suite##_##name() {              \
        rout::test::register_test(&tc_##suite##_##name);                           \
    }                                                                              \
    static void test_##suite##_##name([[maybe_unused]] rout::test::TestCase* _tc)

// --- Filter matching ---
// "timer"         → suite or name contains "timer"
// "timer.refresh" → suite contains "timer" AND name contains "refresh"

struct Filter {
    const char* suite_filter;  // nullptr = match all
    const char* name_filter;   // nullptr = match all

    bool matches(const TestCase* tc) const {
        if (!suite_filter && !name_filter) return true;
        if (suite_filter && name_filter) {
            return str_contains(tc->suite, suite_filter) && str_contains(tc->name, name_filter);
        }
        const char* f = suite_filter ? suite_filter : name_filter;
        return str_contains(tc->suite, f) || str_contains(tc->name, f);
    }
};

inline Filter parse_filter(const char* arg) {
    // Find '.' separator
    for (int i = 0; arg[i]; i++) {
        if (arg[i] == '.') {
            // Split: suite = arg[0..i-1], name = arg[i+1..]
            // We need null-terminated strings, use static buffers
            static char sbuf[128];
            static char nbuf[128];
            int j = 0;
            for (; j < i && j < 127; j++) sbuf[j] = arg[j];
            sbuf[j] = '\0';
            j = 0;
            for (int k = i + 1; arg[k] && j < 127; k++, j++) nbuf[j] = arg[k];
            nbuf[j] = '\0';
            return {sbuf, nbuf};
        }
    }
    return {nullptr, arg};  // no dot: match against both suite and name
}

// --- Runner ---

inline int run_all(int argc = 0, char** argv = nullptr) {
    // Parse args
    bool list_only = false;
    Filter filter = {nullptr, nullptr};

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] == 'l') {
            list_only = true;
        } else {
            filter = parse_filter(argv[i]);
        }
    }

    // List mode
    if (list_only) {
        for (TestCase* tc = g_head; tc; tc = tc->next) {
            if (filter.matches(tc)) {
                out(tc->suite);
                out(".");
                out(tc->name);
                out("\n");
            }
        }
        return 0;
    }

    // Run
    int total_pass = 0;
    int total_fail = 0;
    int total_skip = 0;
    const char* prev_suite = nullptr;

    for (TestCase* tc = g_head; tc; tc = tc->next) {
        if (!filter.matches(tc)) {
            total_skip++;
            continue;
        }

        // Suite header
        if (!prev_suite || !str_eq(prev_suite, tc->suite)) {
            out("=== ");
            out(tc->suite);
            out(" ===\n");
            prev_suite = tc->suite;
        }

        tc->checks_passed = 0;
        tc->checks_failed = 0;
        tc->fail_file = nullptr;

        tc->fn(tc);

        if (tc->checks_failed == 0) {
            out("  PASS: ");
            out(tc->name);
            out("\n");
            total_pass++;
        } else {
            out("  FAIL: ");
            out(tc->name);
            if (tc->fail_file) {
                out(" (");
                out(tc->fail_file);
                out(":");
                out_int(tc->fail_line);
                out(": ");
                out(tc->fail_expr);
                out(")");
            }
            out("\n");
            total_fail++;
        }
    }

    out("\n");
    out_int(total_pass);
    out(" passed, ");
    out_int(total_fail);
    out(" failed");
    if (total_skip > 0) {
        out(", ");
        out_int(total_skip);
        out(" skipped");
    }
    out("\n");

    return total_fail > 0 ? 1 : 0;
}

}  // namespace rout::test
