// emulator-lift · 计时族（kernel32 + winmm）
//
// ★ 口径：**时间只来自 `cpu.clock`（VirtualClock）**。⛔ 不读宿主时钟、不 sleep、不随机 ⇒
//   同一份 guest 输入在任意机器 / 任意时刻跑出来的 tick / FILETIME / SYSTEMTIME 逐位相同
//   （这是判据 A4 的前提，见 clock.h）。
//
// ★ 本文件用的两个常数：
//   · 虚拟时钟的零点 = **1970-01-01 00:00:00Z**（`GetSystemTime` 的固定 epoch）。
//   · `0x019DB1DED53E8000` = 116444736000000000 = 1601-01-01 → 1970-01-01 的 100 ns 数。
//     `GetSystemTimeAsFileTime` 输出 `now100ns() + 这个常数`（FILETIME 的起点是 1601，见 Win32）。
//     ⇒ 两条 API 说的是同一天：SYSTEMTIME 按 1970 epoch 展开，FILETIME 平移 1601→1970。
//
// ⚠ 偏差（逐条）：
//   1. **`GetLocalTime` == `GetSystemTime`**（不做时区）：本模拟器没有 tz 数据库，也不读宿主
//      时区 ⇒ 本地时间恒等于 UTC。影响：guest 若用本地时间做显示（标题栏 / 存档时间戳）会看到
//      UTC；游戏逻辑里"按日期分支"的代码只有在跨时区边界时才可能有差异。
//   2. **`QueryPerformanceFrequency` 是仿真值 3,579,545 Hz**（VirtualClock 的固定频率，不是
//      宿主 TSC / 高精度计时器）。QPC 因此也是虚拟的、单调的、可复现的。
//   3. **`GetTickCount64` 走 EAX:EDX 两个寄存器返回**（`ShimCall` 只有一个 32 位 `result`）；
//      低 32 位在 EAX、高 32 位在 EDX。这是本 shim 层约定的 64 位返回形态。
//   4. **`timeBeginPeriod` / `timeEndPeriod` 恒返回 TIMERR_NOERROR(0)** 且什么也不改：本模拟器
//      的定时分辨率由虚拟时钟决定，没有"系统定时器精度"这回事。影响：无（调用方只检查返回值）。
//   5. **`timeSetEvent` / `timeKillEvent` / `timeGetDevCaps` 是**协作式**实现**（不是 stub）：
//      定时器登记进 `cpu.shim.timers`，到期的回调由 **dispatcher 在块边界**按虚拟时钟触发
//      （机制见 x86.h 的 GuestTimer / kTimerCallbackReturnEa）。与真机的唯一差别：回调不会抢占
//      一条 basic block（真机由 winmm 的服务线程抢占）。`TIME_CALLBACK_EVENT_SET/PULSE` 两种
//      回调类型不支持（要求 dwUser 是事件句柄）⇒ 按文档的失败值返回 0。
//   6. `GetSystemTimeAsFileTime` / `QueryPerformanceCounter` / `QueryPerformanceFrequency` /
//      `GetSystemTime` / `GetLocalTime` 的**空指针**按参数非法处理（打印 + abort）；
//      真机会崩在 guest 里，我们提前响亮失败。
#include <cstddef>
#include <cstdint>
#include <vector>

#include "runtime/x86.h"
#include "shims/register.h"

namespace lift {
namespace {

// FILETIME 的 1601 → 1970 偏移（100 ns 单位）
constexpr std::uint64_t kFiletimeEpochDelta = 0x019DB1DED53E8000ull;
constexpr std::uint64_t kTicksPerMs = 10000ull;   // 1 ms = 10,000 × 100 ns
constexpr std::uint64_t kTicksPerSec = 10000000ull;

void requireMem(X86& cpu, const char* api) {
    if (!cpu.mem) shimAbort("kernel32", api, "cpu.mem 为空（Machine 没装配 guest 内存）");
}

void requireClock(X86& cpu, const char* api) {
    if (!cpu.clock) shimAbort("kernel32", api, "cpu.clock 为空（Machine 没装配虚拟时钟）");
}

// 把一个 64 位值写进 guest 的 8 字节槽（FILETIME / LARGE_INTEGER）
void writeU64(X86& cpu, std::uint32_t ea, std::uint64_t v) { cpu.mem->wr64(ea, v); }

// 1970 epoch 起算的 100 ns → 公历字段（Howard Hinnant 的 civil_from_days，纯整数、确定性）
struct CivilTime {
    std::uint32_t year = 1970;
    std::uint32_t month = 1;
    std::uint32_t day = 1;
    std::uint32_t hour = 0;
    std::uint32_t minute = 0;
    std::uint32_t second = 0;
    std::uint32_t millis = 0;
    std::uint32_t day_of_week = 4;  // 1970-01-01 是周四
};

CivilTime civilFrom100ns(std::uint64_t t) {
    const std::uint64_t total_secs = t / kTicksPerSec;
    const std::uint64_t millis = (t % kTicksPerSec) / kTicksPerMs;
    const std::int64_t days = static_cast<std::int64_t>(total_secs / 86400ull);
    const std::uint32_t second_of_day = static_cast<std::uint32_t>(total_secs % 86400ull);

    const std::int64_t z = days + 719468;  // 移到"0000-03-01"纪元
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const std::uint32_t doe = static_cast<std::uint32_t>(z - era * 146097);
    const std::uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
    const std::uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    const std::uint32_t mp = (5u * doy + 2u) / 153u;
    const std::uint32_t d = doy - (153u * mp + 2u) / 5u + 1u;
    const std::int32_t m_signed = static_cast<std::int32_t>(mp) + (mp < 10u ? 3 : -9);
    const std::uint32_t m = static_cast<std::uint32_t>(m_signed);
    const std::int64_t year = y + (m <= 2u ? 1 : 0);

    CivilTime c;
    c.year = static_cast<std::uint32_t>(year);
    c.month = m;
    c.day = d;
    c.hour = second_of_day / 3600u;
    c.minute = (second_of_day / 60u) % 60u;
    c.second = second_of_day % 60u;
    c.millis = static_cast<std::uint32_t>(millis);
    c.day_of_week = static_cast<std::uint32_t>(((days % 7) + 7 + 4) % 7);
    return c;
}

void writeSystemTime(X86& cpu, std::uint32_t ea, const CivilTime& c, const char* api) {
    if (ea == 0) shimAbort("kernel32", api, "lpSystemTime 为 NULL");
    cpu.mem->wr16(ea + 0x00, static_cast<std::uint16_t>(c.year));
    cpu.mem->wr16(ea + 0x02, static_cast<std::uint16_t>(c.month));
    cpu.mem->wr16(ea + 0x04, static_cast<std::uint16_t>(c.day_of_week));
    cpu.mem->wr16(ea + 0x06, static_cast<std::uint16_t>(c.day));
    cpu.mem->wr16(ea + 0x08, static_cast<std::uint16_t>(c.hour));
    cpu.mem->wr16(ea + 0x0A, static_cast<std::uint16_t>(c.minute));
    cpu.mem->wr16(ea + 0x0C, static_cast<std::uint16_t>(c.second));
    cpu.mem->wr16(ea + 0x0E, static_cast<std::uint16_t>(c.millis));
}

// ---- kernel32 ----
void shimGetTickCount(X86& cpu, ShimCall& call) {
    requireClock(cpu, "GetTickCount");
    call.result = cpu.clock->nowMs();
}

void shimGetTickCount64(X86& cpu, ShimCall& call) {
    requireClock(cpu, "GetTickCount64");
    const std::uint64_t ms = cpu.clock->now100ns() / kTicksPerMs;
    // 偏差 3：64 位返回走 EAX:EDX
    call.result = static_cast<std::uint32_t>(ms & 0xFFFFFFFFull);
    cpu.edx() = static_cast<std::uint32_t>(ms >> 32);
}

void shimGetSystemTimeAsFileTime(X86& cpu, ShimCall& call) {
    requireMem(cpu, "GetSystemTimeAsFileTime");
    requireClock(cpu, "GetSystemTimeAsFileTime");
    const std::uint32_t ea = call.u32(0);
    if (ea == 0) shimAbort("kernel32", "GetSystemTimeAsFileTime", "lpSystemTimeAsFileTime 为 NULL");
    writeU64(cpu, ea, cpu.clock->now100ns() + kFiletimeEpochDelta);
    call.result = 0;  // void
}

void shimQueryPerformanceCounter(X86& cpu, ShimCall& call) {
    requireMem(cpu, "QueryPerformanceCounter");
    requireClock(cpu, "QueryPerformanceCounter");
    const std::uint32_t ea = call.u32(0);
    if (ea == 0) {
        cpu.shim.last_error = 87u;  // ERROR_INVALID_PARAMETER
        shimLog("kernel32", "QueryPerformanceCounter", "lpPerformanceCount 为 NULL ⇒ 返回 0");
        call.result = 0;
        return;
    }
    writeU64(cpu, ea, static_cast<std::uint64_t>(cpu.clock->qpc()));
    call.result = 1;
}

void shimQueryPerformanceFrequency(X86& cpu, ShimCall& call) {
    requireMem(cpu, "QueryPerformanceFrequency");
    requireClock(cpu, "QueryPerformanceFrequency");
    const std::uint32_t ea = call.u32(0);
    if (ea == 0) {
        cpu.shim.last_error = 87u;
        shimLog("kernel32", "QueryPerformanceFrequency", "lpFrequency 为 NULL ⇒ 返回 0");
        call.result = 0;
        return;
    }
    writeU64(cpu, ea, static_cast<std::uint64_t>(cpu.clock->qpcFrequency()));
    call.result = 1;
}

void shimGetSystemTime(X86& cpu, ShimCall& call) {
    requireMem(cpu, "GetSystemTime");
    requireClock(cpu, "GetSystemTime");
    writeSystemTime(cpu, call.u32(0), civilFrom100ns(cpu.clock->now100ns()), "GetSystemTime");
    call.result = 0;
}

void shimGetLocalTime(X86& cpu, ShimCall& call) {
    requireMem(cpu, "GetLocalTime");
    requireClock(cpu, "GetLocalTime");
    // 偏差 1：没有时区 ⇒ 本地时间 == UTC
    writeSystemTime(cpu, call.u32(0), civilFrom100ns(cpu.clock->now100ns()), "GetLocalTime");
    call.result = 0;
}

// `BOOL GetThreadTimes(hThread, lpCreationTime, lpExitTime, lpKernelTime, lpUserTime)`
// ★ 为什么必须实现（不是"跳过"）：CRT 的 C++ 静态初始化器里有 ConcRT 的
//   `Concurrency::details::Security::InitializeCookie`（0x4EC82C），它拿 `GetThreadTimes` 的
//   返回值去搅 cookie —— 不实现它就停在静态初始化里，永远到不了 `_WinMain@16`。
// ★ 时间同源 = `cpu.clock`（和 GetSystemTimeAsFileTime 同一个 100 ns 零点 + 1601 偏移）：
//   · lpCreationTime = **进程/线程创建时刻** = 虚拟时钟的当前值（guest 单线程 ⇒ 线程与进程同刻创建）；
//   · lpExitTime     = 0（还没退出；真机上线程没结束时这个值是未定义的，这里取 0）；
//   · lpKernelTime / lpUserTime = 0（偏差 8：本模拟器**不统计 guest 的 CPU 时间**）。
//   ⇒ 逐位可复现（判据 A4 的前提），且四个输出都写满 8 字节（FILETIME）。
void shimGetThreadTimes(X86& cpu, ShimCall& call) {
    requireMem(cpu, "GetThreadTimes");
    requireClock(cpu, "GetThreadTimes");
    const std::uint32_t h = call.u32(0);
    if (h != 0 && h != cpu.shim.thread_handle && h != cpu.shim.process_handle) {
        cpu.shim.last_error = 6u;  // ERROR_INVALID_HANDLE
        shimLog("kernel32", "GetThreadTimes",
                "hThread=0x%08X 不是本进程的线程伪句柄（0x%08X）⇒ 返回 0 + 错误 6", h,
                cpu.shim.thread_handle);
        call.result = 0;
        return;
    }
    const std::uint64_t created = cpu.clock->now100ns() + kFiletimeEpochDelta;
    const std::uint32_t slots[4] = {call.u32(1), call.u32(2), call.u32(3), call.u32(4)};
    for (int i = 0; i < 4; ++i) {
        if (slots[i] == 0) continue;  // 四个出参都可为 NULL（Win32 允许）
        writeU64(cpu, slots[i], i == 0 ? created : 0ull);
    }
    call.result = 1;
}

// ---- winmm ----
void shimTimeGetTime(X86& cpu, ShimCall& call) {
    requireClock(cpu, "timeGetTime");
    call.result = cpu.clock->nowMs();
}

void shimTimeBeginPeriod(X86& cpu, ShimCall& call) {
    (void)cpu;
    (void)call;
    call.result = 0;  // TIMERR_NOERROR（偏差 4：分辨率由虚拟时钟决定）
}

void shimTimeEndPeriod(X86& cpu, ShimCall& call) {
    (void)cpu;
    (void)call;
    call.result = 0;  // TIMERR_NOERROR
}

// ---- 多媒体定时器（协作式；机制见 x86.h 的 GuestTimer / kTimerCallbackReturnEa）----
// ★ 口径（D5 单线程）：没有 winmm 的定时器服务线程 ⇒ 到期的回调由 **dispatcher 在块边界**触发
//   （按虚拟时钟判定）。回调收到文档规定的五个参数；`TIME_KILL_SYNCHRONOUS` 无意义（不存在异步）。
//   与真机唯一的差别：回调**不会抢占一条 basic block**（真机才会）。
constexpr std::uint32_t kTimePeriodic = 0x0001u;
constexpr std::uint32_t kTimeCallbackEventSet = 0x0010u;
constexpr std::uint32_t kTimeCallbackEventPulse = 0x0020u;
constexpr std::uint32_t kTimeKillSynchronous = 0x0100u;
constexpr std::uint32_t kMmsyserrNoError = 0u;
constexpr std::uint32_t kMmsyserrInvalidParam = 11u;
constexpr std::uint32_t kMmsyserrError = 12u;

void shimTimeSetEvent(X86& cpu, ShimCall& call) {
    const char* api = "timeSetEvent";
    requireClock(cpu, api);
    const std::uint32_t delay = call.u32(0);
    const std::uint32_t resolution = call.u32(1);
    const std::uint32_t proc = call.u32(2);
    const std::uint32_t user = call.u32(3);
    const std::uint32_t event = call.u32(4);
    // 文档：失败 ⇒ 返回 0（不是 -1）
    if (proc == 0 || delay == 0) {
        shimLog("winmm", api, "lpTimeProc=%u / uDelay=%u 非法 ⇒ 返回 0", proc, delay);
        call.result = 0;
        return;
    }
    if ((event & ~(kTimePeriodic | kTimeCallbackEventSet | kTimeCallbackEventPulse |
                   kTimeKillSynchronous)) != 0) {
        shimLog("winmm", api, "fuEvent=0x%X 含未知位 ⇒ 返回 0", event);
        call.result = 0;
        return;
    }
    if ((event & (kTimeCallbackEventSet | kTimeCallbackEventPulse)) != 0) {
        // 这两种回调类型要求 dwUser 是**事件句柄**、由 winmm 去 SetEvent/PulseEvent；
        // 本模拟器不支持（需要跨族访问同步对象）⇒ 按文档的失败值返回，⛔ 不假装登记成功。
        shimLog("winmm", api,
                "TIME_CALLBACK_EVENT_SET/PULSE 不实现（dwUser 是事件句柄；本模拟器只支持"
                " TIME_CALLBACK_FUNCTION）⇒ 返回 0");
        call.result = 0;
        return;
    }
    GuestTimer t;
    t.id = cpu.shim.next_timer_id++;
    t.due_100ns = cpu.clock->now100ns() + static_cast<std::uint64_t>(delay) * 10000ull;
    t.oneshot = (event & kTimePeriodic) == 0;
    t.period_ms = t.oneshot ? 0u : delay;
    t.callback = proc;
    t.user = user;
    t.resolution = resolution;
    cpu.shim.timers.push_back(t);
    shimLog("winmm", api,
            "登记%s定时器 id=%u：%u ms 后触发（协作式：dispatcher 在块边界按虚拟时钟触发回调）",
            t.oneshot ? "一次性" : "周期", t.id, delay);
    call.result = t.id;
}

void shimTimeKillEvent(X86& cpu, ShimCall& call) {
    const char* api = "timeKillEvent";
    const std::uint32_t id = call.u32(0);
    std::vector<GuestTimer>& timers = cpu.shim.timers;
    for (std::size_t i = 0; i < timers.size(); ++i) {
        if (timers[i].id != id) continue;
        timers.erase(timers.begin() + static_cast<std::ptrdiff_t>(i));
        call.result = kMmsyserrNoError;
        return;
    }
    // 文档：未知 id ⇒ MMSYSERR_INVALPARAM（11）
    shimLog("winmm", api, "定时器 id=%u 不存在（可能已经一次性触发过）⇒ 返回 11", id);
    call.result = kMmsyserrInvalidParam;
}

void shimTimeGetDevCaps(X86& cpu, ShimCall& call) {
    const char* api = "timeGetDevCaps";
    requireMem(cpu, api);
    const std::uint32_t caps_ea = call.u32(0);
    const std::uint32_t size = call.u32(1);
    // TIMECAPS { UINT wPeriodMin; UINT wPeriodMax; } = 8 字节（x86）
    if (caps_ea == 0 || size < 8u) {
        shimLog("winmm", api, "ptc=0x%08X / cbtc=%u 非法（需要 8 字节 TIMECAPS）⇒ 返回 11", caps_ea,
                size);
        call.result = kMmsyserrInvalidParam;
        return;
    }
    if (!cpu.mem->committed(caps_ea, 8)) {
        shimAbort("winmm", api, "ptc=0x%08X 未提交（guest 缓冲区非法）", caps_ea);
    }
    // 分辨率范围是**宿主能力**；本模拟器的时钟由虚拟时钟决定 ⇒ 报一个宽而确定的范围。
    cpu.mem->wr32(caps_ea, 1u);          // wPeriodMin = 1 ms
    cpu.mem->wr32(caps_ea + 4u, 1000000u);  // wPeriodMax = 1e6 ms
    shimLog("winmm", api, "报告固定分辨率范围 1..1000000 ms（虚拟时钟；不接宿主定时器）");
    call.result = kMmsyserrNoError;
}

}  // namespace

void registerTimeShims(ShimRegistry& shims) {
    // ---- kernel32 计时 ----
    shims.add("kernel32", "GetTickCount", Conv::Stdcall, 0, shimGetTickCount);
    shims.add("kernel32", "GetTickCount64", Conv::Stdcall, 0, shimGetTickCount64);
    shims.add("kernel32", "GetSystemTimeAsFileTime", Conv::Stdcall, 1, shimGetSystemTimeAsFileTime);
    shims.add("kernel32", "QueryPerformanceCounter", Conv::Stdcall, 1, shimQueryPerformanceCounter);
    shims.add("kernel32", "QueryPerformanceFrequency", Conv::Stdcall, 1, shimQueryPerformanceFrequency);
    shims.add("kernel32", "GetSystemTime", Conv::Stdcall, 1, shimGetSystemTime);
    shims.add("kernel32", "GetLocalTime", Conv::Stdcall, 1, shimGetLocalTime);
    shims.add("kernel32", "GetThreadTimes", Conv::Stdcall, 5, shimGetThreadTimes);

    // ---- winmm（模块名小写；时间同源 = cpu.clock）----
    shims.add("winmm", "timeGetTime", Conv::Stdcall, 0, shimTimeGetTime);
    shims.add("winmm", "timeBeginPeriod", Conv::Stdcall, 1, shimTimeBeginPeriod);
    shims.add("winmm", "timeEndPeriod", Conv::Stdcall, 1, shimTimeEndPeriod);

    // ---- 多媒体定时器（协作式：登记进 cpu.shim.timers，由 dispatcher 在块边界触发）----
    shims.add("winmm", "timeSetEvent", Conv::Stdcall, 5, shimTimeSetEvent);
    shims.add("winmm", "timeKillEvent", Conv::Stdcall, 1, shimTimeKillEvent);
    shims.add("winmm", "timeGetDevCaps", Conv::Stdcall, 2, shimTimeGetDevCaps);
}

}  // namespace lift
