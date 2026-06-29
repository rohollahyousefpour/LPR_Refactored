#pragma once
// LPR_CHECK - an assertion that stays live even in Release/NDEBUG builds. Plain assert()
// is compiled out when NDEBUG is defined (the full / full-vcpkg presets use -DNDEBUG),
// which would make ctest pass vacuously. Tests should use LPR_CHECK instead of assert,
// and must never hide side-effecting calls inside the condition.
#include <cstdio>

namespace lpr_test {
inline int& failures() { static int f = 0; return f; }
}

#define LPR_CHECK(cond)                                                              \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::fprintf(stderr, "CHECK FAILED: %s  (%s:%d)\n", #cond, __FILE__, __LINE__); \
            ++lpr_test::failures();                                                  \
        }                                                                            \
    } while (0)

// Use as the return value of main(): nonzero (test failure) if any LPR_CHECK failed.
#define LPR_TEST_RESULT() (lpr_test::failures() == 0 ? 0 : 1)
