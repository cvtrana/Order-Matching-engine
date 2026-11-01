#pragma once
// ---------------------------------------------------------------------------
// A ~40-line unit test framework, so the project builds with nothing but
// clang++ (no GoogleTest download). Usage:
//
//   TEST(my_test) { CHECK(1 + 1 == 2); }
//
// Each TEST registers itself in a global list via a static object's
// constructor; test_main.cpp runs them all.
// ---------------------------------------------------------------------------
#include <cstdio>
#include <vector>

namespace testfw {

struct Case { const char* name; void (*fn)(); };

inline std::vector<Case>& registry() { static std::vector<Case> r; return r; }
inline int& failures() { static int f = 0; return f; }

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline int run_all() {
    int failed_tests = 0;
    for (const Case& c : registry()) {
        const int before = failures();
        c.fn();
        const bool ok = failures() == before;
        if (!ok) ++failed_tests;
        std::printf("[%s] %s\n", ok ? " OK " : "FAIL", c.name);
    }
    std::printf("\n%zu tests, %d failed\n", registry().size(), failed_tests);
    return failed_tests == 0 ? 0 : 1;
}

}  // namespace testfw

#define TEST(name)                                                   \
    static void name();                                              \
    static const testfw::Registrar name##_registrar(#name, &name);   \
    static void name()

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("    check failed  %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++testfw::failures();                                                \
        }                                                                        \
    } while (0)
