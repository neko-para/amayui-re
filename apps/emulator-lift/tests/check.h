// emulator-lift · 测试的公共断言宏（所有 CTest 共用；不引第三方框架）
//
// ★ 判据是"它会红"：任何一条 `LIFT_CHECK_*` 失败都会让进程退出码非 0，
//   并且打印 `<用例名> <文件>:<行>` —— ctest 的 output-on-failure 直接能看到。
// ★ 缺资产时用 `LIFT_SKIP`（return 77），CMake 侧用 `SKIP_RETURN_CODE 77` 认它；
//   ⛔ 不许"什么都不做然后 return 0"——那在输出里跟"跑绿了"长得一样（AGENTS.md §5）。
#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace lift_test {

inline int g_failures = 0;
inline int g_checks = 0;
inline const char* g_current_case = "(none)";

inline void failf(const char* file, int line, const char* fmt, ...) {
    std::fprintf(stderr, "FAIL [%s] %s:%d: ", g_current_case, file, line);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "\n");
    ++g_failures;
}

inline int finish(const char* suite) {
    std::printf("%s: %d checks, %d failures\n", suite, g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

}  // namespace lift_test

#define LIFT_CHECK(cond)                                                                  \
    do {                                                                                  \
        ++::lift_test::g_checks;                                                          \
        if (!(cond)) ::lift_test::failf(__FILE__, __LINE__, "CHECK failed: %s", #cond);    \
    } while (0)

#define LIFT_CHECK_EQ_U32(actual, expected)                                                       \
    do {                                                                                          \
        ++::lift_test::g_checks;                                                                  \
        const std::uint32_t a_ = (actual);                                                        \
        const std::uint32_t e_ = (expected);                                                      \
        if (a_ != e_)                                                                              \
            ::lift_test::failf(__FILE__, __LINE__, "expected 0x%08X, got 0x%08X  (%s)", e_, a_, #actual); \
    } while (0)

#define LIFT_CHECK_NE_U32(actual, unexpected)                                                     \
    do {                                                                                          \
        ++::lift_test::g_checks;                                                                  \
        const std::uint32_t a_ = (actual);                                                        \
        const std::uint32_t u_ = (unexpected);                                                    \
        if (a_ == u_)                                                                             \
            ::lift_test::failf(__FILE__, __LINE__, "value 0x%08X should differ  (%s)", a_, #actual); \
    } while (0)

#define LIFT_CHECK_EQ_I64(actual, expected)                                                        \
    do {                                                                                           \
        ++::lift_test::g_checks;                                                                   \
        const std::int64_t a_ = (actual);                                                          \
        const std::int64_t e_ = (expected);                                                        \
        if (a_ != e_)                                                                              \
            ::lift_test::failf(__FILE__, __LINE__, "expected %lld, got %lld  (%s)", (long long)e_,  \
                               (long long)a_, #actual);                                            \
    } while (0)

#define LIFT_CHECK_MEM_U32(memobj, ea, expected)                                                       \
    do {                                                                                               \
        ++::lift_test::g_checks;                                                                       \
        const std::uint32_t a_ = (memobj).rd32(ea);                                                    \
        const std::uint32_t e_ = (expected);                                                           \
        if (a_ != e_)                                                                                  \
            ::lift_test::failf(__FILE__, __LINE__, "mem[0x%08X] expected 0x%08X, got 0x%08X",          \
                               (unsigned)(ea), e_, a_);                                                \
    } while (0)

// 缺资产：跳过（CTest 侧靠 SKIP_RETURN_CODE 77 认）
#define LIFT_SKIP(why)                                  \
    do {                                                \
        std::printf("SKIP: %s\n", (why));               \
        return 77;                                      \
    } while (0)
