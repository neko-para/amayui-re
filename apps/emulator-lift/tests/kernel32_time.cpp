// emulator-lift · **墙钟 vs uptime**（BUG-30 的回归口；pure：注入时钟 + 空机器，不碰游戏 / LFS）
//
// 判据（红了意味着什么）：
//   ① **uptime 仍是 uptime**：注入 raw = 1 h 后 `GetTickCount` == 3,600,000，⛔ 不受墙钟 base 影响；
//      `now100ns()` / `nowMs()` / `qpc()` 只由单调读数决定。
//   ② **墙钟是 epoch**：注入 base 使 wall = `2024-03-05T12:34:56.789Z` ⇒ `GetSystemTime` 的八个字段
//      逐位一致（⛔ 不断言"当前年份"——年份是注入的）。
//   ③ **FILETIME 往返**：`GetSystemTimeAsFileTime` == wall + 1601→1970 delta，减回去 == wall；
//      `GetThreadTimes` 的创建时间用**同一份**墙钟（⛔ 不是 uptime + 1970）。
//   ④ **本地时间 = UTC + 宿主偏移，且 DST 按被换算的时刻判**：注入一条假规则（冬 +1 h / 夏 +2 h），
//     两个注入时刻的 `GetLocalTime` 分别比 `GetSystemTime` 多 1 h / 2 h（⛔ 不依赖本机时区）。
//   ⑤ **冻结语义对墙钟同样成立**（回归）：暂停区间对 `wall100ns()` 也不可见，解冻后只随单调增量走。
//
// ★ 修前的形状（BUG-30）：`GetSystemTime`/`GetLocalTime` 直接展开 `now100ns()`（宿主 uptime）⇒
//   年份停在 1970 + 开机时长。⇒ 本文件第 ② 条与第 ① 条必须**同时**绿：把任一条读回 uptime 都会红。
#include <cstdint>
#include <cstdio>

#include "check.h"
#include "fixture.h"
#include "runtime/clock.h"
#include "shims/register.h"
#include "shims/time_zone.h"

namespace {

using lift_fixture::Fixture;

constexpr std::uint64_t kTicksPerSec = 10'000'000ull;  // 1 s = 10^7 × 100 ns
constexpr std::uint64_t kTicksPerMs = 10'000ull;       // 1 ms = 10^4 × 100 ns
// FILETIME 的 1601 → 1970 偏移（100 ns）。与 kernel32_time.cpp 的 kFiletimeEpochDelta 同值；
// ⛔ 不从被测代码里 include 一个匿名 namespace 常量（那就把 expected 抄成 actual 了）。
constexpr std::uint64_t kFiletimeEpochDelta = 0x019DB1DED53E8000ull;

// 注入的单调读数 = 1 h（宿主 uptime）；墙钟 = 2024-03-05T12:34:56.789Z。
constexpr std::uint64_t kInjectedUptime = 3600ull * kTicksPerSec;
constexpr std::uint64_t kInjectedWall = 1709642096ull * kTicksPerSec + 789ull * kTicksPerMs;

std::uint64_t g_fake_raw_100ns = 0;
std::uint64_t fakeRawNow() { return g_fake_raw_100ns; }

// 假的宿主时区规则（只为证明"偏移按被换算的时刻取"）：
//   2024-03-31T01:00:00Z .. 2024-10-27T01:00:00Z = 夏令时 +2 h，其余 +1 h。
// ⛔ 不用本机真实时区：那会让读数随机器漂（CI 的 TZ 是隐变量）。
std::int32_t fakeDstOffset(std::int64_t unix_seconds_utc) {
    constexpr std::int64_t kSummerStart = 1711846800LL;  // 2024-03-31T01:00:00Z
    constexpr std::int64_t kSummerEnd = 1729990800LL;    // 2024-10-27T01:00:00Z
    return (unix_seconds_utc >= kSummerStart && unix_seconds_utc < kSummerEnd) ? 7200 : 3600;
}

struct SysTime {
    std::uint32_t year = 0;
    std::uint32_t month = 0;
    std::uint32_t day_of_week = 0;
    std::uint32_t day = 0;
    std::uint32_t hour = 0;
    std::uint32_t minute = 0;
    std::uint32_t second = 0;
    std::uint32_t millis = 0;
};

SysTime readSysTime(const lift::GuestMemory& mem, std::uint32_t ea) {
    SysTime t;
    t.year = mem.rd16(ea + 0x00);
    t.month = mem.rd16(ea + 0x02);
    t.day_of_week = mem.rd16(ea + 0x04);
    t.day = mem.rd16(ea + 0x06);
    t.hour = mem.rd16(ea + 0x08);
    t.minute = mem.rd16(ea + 0x0A);
    t.second = mem.rd16(ea + 0x0C);
    t.millis = mem.rd16(ea + 0x0E);
    return t;
}

void expectSysTime(const SysTime& t, std::uint32_t year, std::uint32_t month,
                   std::uint32_t day_of_week, std::uint32_t day, std::uint32_t hour,
                   std::uint32_t minute, std::uint32_t second, std::uint32_t millis) {
    LIFT_CHECK_EQ_U32(t.year, year);
    LIFT_CHECK_EQ_U32(t.month, month);
    LIFT_CHECK_EQ_U32(t.day_of_week, day_of_week);
    LIFT_CHECK_EQ_U32(t.day, day);
    LIFT_CHECK_EQ_U32(t.hour, hour);
    LIFT_CHECK_EQ_U32(t.minute, minute);
    LIFT_CHECK_EQ_U32(t.second, second);
    LIFT_CHECK_EQ_U32(t.millis, millis);
}

// 注入墙钟 base（= 单调零点那一刻的墙钟）＋单调读数；base 由 target wall - raw 反推。
void armClock(lift::Clock& clock, std::uint64_t wall_100ns, std::uint64_t raw_100ns) {
    g_fake_raw_100ns = raw_100ns;
    clock.setRawNowForTest(&fakeRawNow);
    clock.setWallBaseForTest(wall_100ns - raw_100ns);
}

}  // namespace

int main() {
    using namespace lift_test;

    // ① uptime 与墙钟是**两条**读数：同一时刻，GetTickCount = 1 h，墙钟 = 2024-03-05。
    {
        g_current_case = "wall-vs-uptime-split";
        Fixture f;
        lift::registerTimeShims(f.m.shims());
        LIFT_CHECK(f.ok);
        armClock(f.m.clock(), kInjectedWall, kInjectedUptime);

        LIFT_CHECK(f.m.clock().now100ns() == kInjectedUptime);
        LIFT_CHECK_EQ_U32(f.m.clock().nowMs(), 3600000u);
        LIFT_CHECK(f.m.clock().wall100ns() == kInjectedWall);

        const std::uint32_t tick = f.use("GetTickCount");
        LIFT_CHECK_EQ_U32(f.call(tick, {}), 3600000u);

        const std::uint32_t st_ea = f.zeroData(16);
        const std::uint32_t sys_time = f.use("GetSystemTime");
        LIFT_CHECK_EQ_U32(f.call(sys_time, {st_ea}), 0u);
        expectSysTime(readSysTime(f.m.mem(), st_ea), 2024u, 3u, 2u, 5u, 12u, 34u, 56u, 789u);

        // qpc 也只跟 uptime 走（墙钟 base 不进它）。
        LIFT_CHECK(f.m.clock().qpc() ==
                   static_cast<std::int64_t>((kInjectedUptime * 3579545ull) / 10000000ull));
        LIFT_CHECK(f.ok);
    }

    // ② FILETIME 往返 + GetThreadTimes 的创建时间用墙钟。
    {
        g_current_case = "filetime-and-thread-creation-use-wall";
        Fixture f;
        lift::registerTimeShims(f.m.shims());
        LIFT_CHECK(f.ok);
        armClock(f.m.clock(), kInjectedWall, kInjectedUptime);

        const std::uint32_t ft_ea = f.zeroData(8);
        const std::uint32_t ft_shim = f.use("GetSystemTimeAsFileTime");
        f.call(ft_shim, {ft_ea});
        const std::uint64_t ft = f.m.mem().rd64(ft_ea);
        LIFT_CHECK(ft == kInjectedWall + kFiletimeEpochDelta);
        LIFT_CHECK(ft - kFiletimeEpochDelta == kInjectedWall);  // 往返一致

        const std::uint32_t created_ea = f.zeroData(8);
        const std::uint32_t exit_ea = f.zeroData(8);
        const std::uint32_t kernel_ea = f.zeroData(8);
        const std::uint32_t user_ea = f.zeroData(8);
        const std::uint32_t thread_times = f.use("GetThreadTimes");
        // hThread=0（当前线程）；四个出参都给（Win32 允许 NULL，但这里要读）。
        LIFT_CHECK_EQ_U32(f.call(thread_times,
                                 {0u, created_ea, exit_ea, kernel_ea, user_ea}),
                          1u);
        LIFT_CHECK(f.m.mem().rd64(created_ea) == ft);
        LIFT_CHECK(f.m.mem().rd64(exit_ea) == 0ull);
        LIFT_CHECK(f.m.mem().rd64(kernel_ea) == 0ull);
        LIFT_CHECK(f.m.mem().rd64(user_ea) == 0ull);
        LIFT_CHECK(f.ok);
    }

    // ③ 宿主时区：本地 = UTC + 注入偏移；DST 按**被换算的时刻**判（冬 +1 h / 夏 +2 h）。
    {
        g_current_case = "local-time-host-zone-dst";
        Fixture f;
        lift::registerTimeShims(f.m.shims());
        LIFT_CHECK(f.ok);
        lift::time_zone::setOffsetForTest(&fakeDstOffset);

        const std::uint32_t utc_ea = f.zeroData(16);
        const std::uint32_t local_ea = f.zeroData(16);
        const std::uint32_t sys_time = f.use("GetSystemTime");
        const std::uint32_t local_time = f.use("GetLocalTime");

        struct Case {
            std::uint64_t wall;  // 注入的 UTC 墙钟（100 ns）
            std::uint32_t local_hour;
        };
        const Case cases[] = {
            {1705320000ull * kTicksPerSec, 13u},  // 2024-01-15T12:00:00Z（冬 +1 h）
            {1721044800ull * kTicksPerSec, 14u},  // 2024-07-15T12:00:00Z（夏 +2 h）
        };
        for (const Case& c : cases) {
            armClock(f.m.clock(), c.wall, kInjectedUptime);
            f.call(sys_time, {utc_ea});
            f.call(local_time, {local_ea});
            const SysTime utc = readSysTime(f.m.mem(), utc_ea);
            const SysTime local = readSysTime(f.m.mem(), local_ea);
            LIFT_CHECK_EQ_U32(utc.hour, 12u);
            LIFT_CHECK_EQ_U32(utc.minute, 0u);
            LIFT_CHECK_EQ_U32(local.hour, c.local_hour);
            LIFT_CHECK_EQ_U32(local.minute, utc.minute);
            LIFT_CHECK_EQ_U32(local.second, utc.second);
            LIFT_CHECK_EQ_U32(local.day, utc.day);
            LIFT_CHECK_EQ_U32(local.month, utc.month);
            LIFT_CHECK_EQ_U32(local.year, utc.year);
            // GetLocalTime - GetSystemTime == 偏移（两个注入点都在中午 ⇒ 当日秒差无跨日）。
            const std::uint32_t utc_sec = utc.hour * 3600u + utc.minute * 60u + utc.second;
            const std::uint32_t local_sec = local.hour * 3600u + local.minute * 60u + local.second;
            LIFT_CHECK_EQ_U32(local_sec - utc_sec, (c.local_hour - 12u) * 3600u);
        }
        lift::time_zone::setOffsetForTest(nullptr);
        LIFT_CHECK(f.ok);
    }

    // ④ 墙钟 = 注入 base + 注入的单调读数；⛔ base 不进 uptime。
    {
        g_current_case = "wall-base-plus-uptime";
        lift::Clock clock;
        g_fake_raw_100ns = 1234567ull;
        clock.setRawNowForTest(&fakeRawNow);
        LIFT_CHECK(clock.now100ns() == 1234567ull);
        clock.setWallBaseForTest(1709642096ull * kTicksPerSec);
        LIFT_CHECK(clock.wall100ns() == 1709642096ull * kTicksPerSec + 1234567ull);
        LIFT_CHECK(clock.now100ns() == 1234567ull);
        g_fake_raw_100ns += 10ull * kTicksPerSec;
        LIFT_CHECK(clock.wall100ns() == 1709642106ull * kTicksPerSec + 1234567ull);
        LIFT_CHECK(clock.now100ns() == 101234567ull);
    }

    // ⑤ 冻结语义回归：暂停区间对墙钟也不可见（与 now100ns() 同源）。
    {
        g_current_case = "freeze-freezes-wall-too";
        lift::Clock clock;
        armClock(clock, kInjectedWall, kInjectedUptime);
        LIFT_CHECK(clock.wall100ns() == kInjectedWall);
        clock.freeze();
        g_fake_raw_100ns += 5ull * kTicksPerSec;  // 暂停 5 s
        LIFT_CHECK(clock.wall100ns() == kInjectedWall);
        LIFT_CHECK(clock.now100ns() == kInjectedUptime);
        LIFT_CHECK_EQ_U32(clock.pausedTotalMs(), 5000u);
        clock.unfreeze();
        LIFT_CHECK(clock.wall100ns() == kInjectedWall);
        LIFT_CHECK_EQ_U32(clock.pausedCommittedMs(), 5000u);
        g_fake_raw_100ns += kTicksPerSec;  // 解冻后走 1 s
        LIFT_CHECK(clock.wall100ns() == kInjectedWall + kTicksPerSec);
        LIFT_CHECK(clock.now100ns() == kInjectedUptime + kTicksPerSec);
    }

    return finish("kernel32-time");
}
