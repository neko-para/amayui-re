// emulator-lift · **临时诊断**（2026-11-24）：`_memcpy` 的调用画像 + `rep_bulk` 命中/decline。
//
// ★ 开关：环境变量 `LIFT_MEMCPY_DIAG=1`（缺省完全不收集、不输出）。
//   * `LIFT_MEMCPY_DIAG_WINDOW=<label>` 只在指定的 `profile.begin` 窗口里收集（缺省 `move11`；
//     `*` = 所有窗口）。
// ★ 零开销口径（缺省档）：
//   * `aot_run.cpp` 的派发循环：每派发只多一次 bool 判定（`memcpy_diag::on()` 在循环外取一次）；
//   * `exec_string.cpp` 的 `repBulkTry`：每个 rep 只多一次 `g_rep_bulk_diag.on` 判定；
//   * 没有任何输出（`print()` 在 `on == false` 时第一行返回）。
// ★ 这不是产品语义：只读 `cpu_.insns` / 栈上的参数 / `rep_bulk` 的计数器，⛔ 不写 guest 状态。
#pragma once

#include <cstdint>
#include <cstdio>

namespace lift {
namespace memcpy_diag {

// `_memcpy` / `_memset` 的 EA 区间（`.lst` 的 COLLAPSED 声明：0x361 / 0x7A 字节）。
constexpr std::uint32_t kMemcpyBegin = 0x004E74B0u;
constexpr std::uint32_t kMemcpyEnd = 0x004E7811u;
constexpr std::uint32_t kMemsetBegin = 0x004E7400u;
constexpr std::uint32_t kMemsetEnd = 0x004E747Au;

bool on();

// ★ 热路径（`aot_run.cpp` 的派发循环）按引用读这个 bool：开档时每派发只多一次 load，
//   ⛔ 不是一次跨 TU 调用。
extern bool g_active_state;
inline bool active() { return g_active_state; }

// `0x55F2E8`（`_memcpy` 里 `cmp dword ptr [0x55F2E8], 0` 那个全局开关）在本轮的读数。
void setSseSwitch(std::uint32_t value);

void beginWindow(const char* label);
void endWindow();

// 派发到 `_memcpy` 入口的**每一次**（含栈参数读不出来的；报告里 `entry=` 与 `calls=` 的差
// 就是"这个入口的派发不全是 cdecl 调用"的量）。`ok=false` 时 dst/src/n 无意义。
void noteMemcpyEntry(bool ok, std::uint32_t sp, std::uint32_t dst, std::uint32_t src, std::uint32_t n);
void noteMemcpyCall(std::uint32_t dst, std::uint32_t src, std::uint32_t n);
void noteMemsetCall(std::uint32_t n);
// 一次派发（EA 落在 `_memcpy` 区间）里执行的 guest 指令数 + 按入口 EA 的派发计数。
void noteDispatch(std::uint32_t ea, std::uint64_t insns);
// Present 之后调用；按帧累计 `_memcpy` 的 insns / calls / bytes（报告只打 top-N）。
void notePresent(std::uint64_t frame_no);

// 停机摘要附近**一次性**打印（⛔ 不每帧打）。
void print(std::FILE* out);

}  // namespace memcpy_diag
}  // namespace lift
