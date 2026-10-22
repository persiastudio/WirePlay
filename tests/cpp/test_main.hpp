// A ~60 line test harness. Pulling in a framework for eleven assertions would
// add a vcpkg dependency to a project that otherwise needs exactly two.
#pragma once

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace t {

struct Case {
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline int& failures() {
    static int f = 0;
    return f;
}

inline std::string& current() {
    static std::string c;
    return c;
}

inline void fail(const char* file, int line, const std::string& what) {
    ++failures();
    std::fprintf(stderr, "  FAIL %s:%d  %s\n", file, line, what.c_str());
}

inline int run() {
    int passed = 0;
    for (const auto& c : registry()) {
        current() = c.name;
        const int before = failures();
        std::printf("%-58s", c.name);
        std::fflush(stdout);
        c.fn();
        if (failures() == before) {
            std::printf("ok\n");
            ++passed;
        }
    }
    std::printf("\n%d/%zu passed", passed, registry().size());
    if (failures() > 0) std::printf(", %d FAILURE(S)", failures());
    std::printf("\n");
    return failures() == 0 ? 0 : 1;
}

}  // namespace t

#define T_CONCAT_(a, b) a##b
#define T_CONCAT(a, b) T_CONCAT_(a, b)

#define TEST(name)                                                       \
    static void name();                                                  \
    static ::t::Registrar T_CONCAT(reg_, name)(#name, name);             \
    static void name()

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) ::t::fail(__FILE__, __LINE__, "CHECK(" #cond ")");  \
    } while (0)

#define CHECK_EQ(a, b)                                                                  \
    do {                                                                                \
        auto va = (a);                                                                  \
        auto vb = (b);                                                                  \
        if (!(va == vb)) {                                                              \
            char buf[256];                                                              \
            std::snprintf(buf, sizeof(buf), "CHECK_EQ(%s, %s): %lld != %lld", #a, #b,   \
                          (long long)va, (long long)vb);                                \
            ::t::fail(__FILE__, __LINE__, buf);                                         \
        }                                                                               \
    } while (0)
