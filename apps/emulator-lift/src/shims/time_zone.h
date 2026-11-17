// emulator-lift · 宿主时区：UTC 墙钟 → 本地时间（`GetLocalTime` 的唯一换算口）
//
// ★ 口径（BUG-30）：`GetLocalTime` 必须做**宿主时区**换算，且 DST 按**被换算的那个时刻**判定
//   （不是"按现在是不是夏令时"一刀切）。本文件只回答一个问题：
//
//       给定一个 UTC 秒（Unix epoch）⇒ 宿主的 UTC 偏移（秒，local = UTC + offset）
//
//   · Windows：`SystemTimeToTzSpecificLocalTime(nullptr, …)` —— 用当前时区的**规则**
//     （含动态 DST / 历史规则）换算出该时刻的本地 SYSTEMTIME，再与 UTC 相减。
//   · POSIX（Linux / macOS）：`localtime_r()` 的 `tm_gmtoff` —— 同样按该时刻的 tz 规则。
//   ★ 为什么两平台都走"给定时刻"的 API 而不是 `GetTimeZoneInformation` / `tm_isdst` 猜：
//     后者只看**当前**状态；把注入的历史/未来墙钟按当前 DST 展开会错一小时（静默）。
//   ★ 这是**宿主**事实（跟游戏 / guest 无关）；⛔ 不写死 +08、⛔ 不从 guest 语言推断。
//
// ★ 测试缝（`setOffsetForTest`）：注入 `(UTC 秒) → 偏移秒` 的函数指针。
//   ⇒ 用例可以钉一条**假的 DST 规则**（例如冬 +1h / 夏 +2h）并断言两个时刻的本地字段，
//     ⛔ 不依赖宿主机器的时区 / 不断言"当前年份"。生产路径从不调用它（缺省 nullptr = 宿主时区）。
#pragma once

#include <cstdint>
#include <ctime>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace lift {
namespace time_zone {

// UTC 秒（1970 起算）→ 宿主 UTC 偏移秒（local = UTC + 返回值）。DST 按**该时刻**判定。
using OffsetFn = std::int32_t (*)(std::int64_t unix_seconds_utc);

namespace detail {

// C++17 起 inline 函数的局部 static 跨 TU 是**同一个**对象 ⇒ 测试与 shim 看到同一份注入。
inline OffsetFn& testFn() {
    static OffsetFn fn = nullptr;
    return fn;
}

#ifdef _WIN32
// 公历 → 1970 起算的天数（Howard Hinnant 的 days_from_civil，纯整数、无时区）。
inline std::int64_t daysFromCivil(std::int64_t y, unsigned m, unsigned d) {
    y -= m <= 2u ? 1 : 0;
    const unsigned mp = (m + 9u) % 12u;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153u * mp + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

inline std::int64_t systemTimeToUnixSeconds(const SYSTEMTIME& st) {
    return daysFromCivil(st.wYear, st.wMonth, st.wDay) * 86400 +
           static_cast<std::int64_t>(st.wHour) * 3600 +
           static_cast<std::int64_t>(st.wMinute) * 60 + static_cast<std::int64_t>(st.wSecond);
}

// Unix 秒 → FILETIME → SYSTEMTIME（UTC）。超出 FILETIME 的值域 ⇒ false（调用方回 0 = 不做换算）。
inline bool unixToSystemTime(std::int64_t unix_seconds, SYSTEMTIME* out) {
    constexpr std::int64_t kUnixToFiletime = 11644473600LL;  // 1601 → 1970 的秒数
    const std::int64_t filetime_100ns = (unix_seconds + kUnixToFiletime) * 10000000LL;
    if (filetime_100ns < 0) return false;
    FILETIME ft;
    ft.dwLowDateTime = static_cast<DWORD>(static_cast<std::uint64_t>(filetime_100ns) & 0xFFFFFFFFu);
    ft.dwHighDateTime = static_cast<DWORD>(static_cast<std::uint64_t>(filetime_100ns) >> 32);
    return ::FileTimeToSystemTime(&ft, out) != 0;
}
#endif  // _WIN32

}  // namespace detail

// 宿主时区：给定 UTC 秒，返回该时刻的本地偏移秒（含 DST）。失败 ⇒ 0（= 按 UTC，⛔ 不猜）。
inline std::int32_t hostUtcOffsetSeconds(std::int64_t unix_seconds_utc) {
#ifdef _WIN32
    SYSTEMTIME utc{};
    if (!detail::unixToSystemTime(unix_seconds_utc, &utc)) return 0;
    SYSTEMTIME local{};
    // nullptr = 当前时区；该 API 按**传入时刻**套用 tz 规则（含动态 DST）。
    if (::SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local) == 0) return 0;
    const std::int64_t delta =
        detail::systemTimeToUnixSeconds(local) - detail::systemTimeToUnixSeconds(utc);
    return static_cast<std::int32_t>(delta);
#else
    const std::time_t t = static_cast<std::time_t>(unix_seconds_utc);
    std::tm tm{};
    if (::localtime_r(&t, &tm) == nullptr) return 0;
    return static_cast<std::int32_t>(tm.tm_gmtoff);
#endif
}

// 读数入口：测试注入优先，否则问宿主。`kernel32_time.cpp` 只经过这一个函数。
inline std::int32_t utcOffsetSecondsFor(std::int64_t unix_seconds_utc) {
    if (detail::testFn() != nullptr) return detail::testFn()(unix_seconds_utc);
    return hostUtcOffsetSeconds(unix_seconds_utc);
}

// ★ 测试缝：nullptr = 恢复宿主时区。
inline void setOffsetForTest(OffsetFn fn) { detail::testFn() = fn; }

}  // namespace time_zone
}  // namespace lift
