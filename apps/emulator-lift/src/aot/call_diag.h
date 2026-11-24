// emulator-lift · **临时诊断**：库函数候选的 per-function 动态计数（`LIFT_CALL_DIAG=1`）。
//
// ★ 为什么有它：`--profile` 的 `window-dispatch-top` 是**派发份额**（1/4096 采样、`ms` 列不是该 EA
//   的实测成本），给不出「调用了几次 / 每次多少条 guest 指令」；`memcpy_diag` 只画像 `_memcpy` 一个。
//   内建普查（REQ-01M86NP9S7BEGSP1QR4VX51G9N）要按 `calls × ns/次` 排序 ⇒ 需要一张**小 EA 表**的精确计数。
//
// ★ 口径（与评测单 REQ-01M86VSP8CXH998HTJTD50MEGC 一致）：
//   * **calls** = 派发（dispatch）到该候选**入口 EA** 的次数 —— 与 `memcpy_diag` 同一处判定
//     （`aot_run.cpp` 的派发循环）。⛔ 不是"派发份额换算"，⛔ 不是静态 call 站点数。
//   * **dispatches / insns** = 派发 EA 落在 `[ea, ea+size)` 的次数 / 该派发执行的 guest 指令数之和
//     （`cpu_.insns` 前后差；`lift_tick` 在块边界结算 ⇒ 前后差 = 该派发执行的条数）。
//   * **avg_n / bytes / max_n** = 入口派发那一次读 cdecl 实参（`[esp+4]=dst [esp+8]=src [esp+12]=n`；
//     `_memset` 的 n 也在 `[esp+12]`）。没读法 / 没读到的候选只给 calls（n 记 `-`）。
//   * 窗口边界 = 宿主的 `profile_begin/end`（`AotHost::beginWindow/endWindowText`）。
//   * **一次性输出**：只在 `printRunSummary` 末尾打；⛔ 不每帧 / 不周期。
//
// ★ 零开销口径（缺省档 = env 未设）：
//   * `aot_run.cpp`：`call_diag::on()` 在 `run()` 开头取一次；循环里是 `call_diag_on && active()`；
//   * `beginWindow` / `endWindow` / `print` 第一行就返回 ⇒ 不分配、不计数、**一个字节都不输出**。
//   * 诊断只读 guest 状态（栈上的实参），⛔ 不写任何 guest 内存 / 寄存器 / EFLAGS。
//
// ★ 这不是产品语义：候选表是**测量面**，不是内建表（`aot/intrinsics.txt` 不因它变化）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace lift {
namespace call_diag {

// 候选（EA 的字节数来自 `.lst` 的 `COLLAPSED FUNCTION` 声明；区间半开）。
struct Candidate {
    std::uint32_t ea;
    std::uint32_t end;
    const char* name;
    bool has_n;  // true = 入口处 `[esp+12]` 是长度 n（memcpy / memset 家族）
};

// ★ 开关（进程级、只读）：`LIFT_CALL_DIAG` 的静态判定结果，只由 `call_diag.cpp` 的初始化写一次。
//   读它是**普通全局 bool**（不碰 TLS）⇒ 缺省档的每一处判定只多一次 load + 分支。
extern bool g_on_state;
inline bool on() { return g_on_state; }

// ★ 窗口状态**只属于开窗的那个线程**（= 宿主主线程）：`thread_local`。
//   为什么必须：`aot_dispatch.cpp` 的 `lookupUncounted` 也会被**音频 feeder 线程**调用
//   （`aot_feeder.cpp` 的 IR 执行器）⇒ 共享一份无锁 `g_cur` 是数据竞争（主线程同时在
//   `noteDispatch` 里写它）。`thread_local` 之后 feeder 线程看到的是它自己那份（恒 false /
//   空窗口）⇒ 一个字节都不写、不竞争。
//   ⚠ 代价：feeder 线程里的调用**不计入** ⇒ **本诊断只保证 `--audio off` 下的计数**；
//   `--audio` 非 off 且 opt-in 时 `aot_run.cpp` 会打**一行** stderr 说明"本 run 计数不可信"。
extern thread_local bool g_active_state;
inline bool active() { return g_active_state; }

// 候选表（只读；`print` 与 `rangeIndex` 共用这一份）。
std::size_t candidateCount();
const Candidate& candidate(std::size_t i);

// EA 落在某个候选的 `[ea, end)` ⇒ 下标；否则 -1。
int rangeIndex(std::uint32_t ea);

// 一次派发：`ea` = 本次派发的入口 EA，`insns` = 本次派发执行的 guest 指令数。
// `n_ok / n` 只在 `ea == candidate(idx).ea` 且该候选 `has_n` 时有意义（否则传 false / 0）。
void noteDispatch(int idx, std::uint32_t ea, std::uint64_t insns, bool n_ok, std::uint32_t n);

// ★ S7 宿主调用路径：`lift_dyn`（`lookupUncounted`）里的一次 `EA → 函数` 查找。
//   为什么必须单独记：2026-11-24 的 D3 之后，`call` 站点发的是普通 `call @lift_dyn` +
//   return-check（**不回 trampoline**）⇒ 派发循环看不到这些调用。实测（本轮整跑窗口）：
//   `_memcpy` 的派发可见调用 50,099 次，而同一次 run 的 `intrinsics: fast+decline` = 26,721,505
//   ⇒ **99.81% 的调用走这条**。⛔ 不把这条与派发计数相加成"份额"，它就是"另一次调用"。
void noteHostCall(std::uint32_t ea);

void beginWindow(const char* label);
void endWindow();

// 停机摘要附近**一次性**打印（⛔ 不每帧打）。
void print(std::FILE* out);

}  // namespace call_diag
}  // namespace lift
