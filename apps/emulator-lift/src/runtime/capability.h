// emulator-lift · **control 能力表**（唯一真源；header-only，⛔ 不进 CMake 源列表）
//
// ★ 为什么有这张表：`hello.capabilities`（`control_ws.cpp` 的 `helloJson`）、`controlOpName`
//   （同文件）、启动 readout 的 `caps=`（`aot_main.cpp`）曾经各有一份字面量清单 ⇒ `caps=` 长期只有
//   6 项（漏 `move` / `global-get` / `global-set`）。见 `docs/03-emulator/plugin-runtime.md`
//   §4.9（能力矩阵 / "control 的 9 项不变"）与 §5 C0（`caps=` 6→9 的 readout 修正）。
//   ⇒ C0 把三处收敛成**一张表**：加一项只改这里；三处都从它生成。
//
// ★ 顺序 = 合同：`pause, resume, sample, screenshot, move, click, global-get, global-set, quit`
//   （= `hello.capabilities` 的数组顺序 = `ControlOp` 的值序）。
//   本头是 **leaf**（⛔ 不 include `control.h` / nlohmann / beast —— 谁 include 它都不会多花编译时间），
//   所以它看不见 `ControlOp`；两个枚举的**值序**由 `control_ws.cpp` 里那条 `static_assert` 绑住
//   （那是唯一同时看不见/看得见、且不制造循环依赖的落点）。
//
// ★ `wait` **不是**服务端能力：它是 `emu-ctl` 的客户端糖（= `resume --for-ms`，
//   `tools/lib/emu-ctl.mjs` 的 OPERATIONS）。⛔ 不进本表 —— 进了就等于给服务端加 op。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace lift::plugin {

// control 端点的 op（值序 = `ControlOp`；`None` 只是"没有命令"的哨兵，不是能力）。
enum class ControlCapability : std::uint8_t {
    None = 0,
    Pause,
    Resume,
    Sample,
    Screenshot,
    Move,
    Click,
    GlobalGet,
    GlobalSet,
    Quit,
};

// 能力项数 = `Quit` 的值（`None` 不算）。`Quit` 之后再加一项 ⇒ 这里跟着变。
inline constexpr std::size_t kControlCapabilityCount = static_cast<std::size_t>(ControlCapability::Quit);

// C0 的合同数字：**9 项**。加/删能力是合同变更（要同时改 §4.9 与 `.d.ts` 面），⛔ 不是随手调表。
static_assert(kControlCapabilityCount == 9u,
              "control 能力面 = 9 项（plugin-runtime §4.9）；`wait` 是 emu-ctl 客户端糖，⛔ 不进服务端");

struct ControlCapabilityRow {
    ControlCapability value;
    const char* name;
};

// ★ 表 = 一行一个能力，顺序即合同（`hello.capabilities` 的数组顺序 = `ControlOp` 值序）。
//   行数由下面那条 `static_assert` 绑到 `kControlCapabilityCount` ⇒ **多一项 / 少一项都是编译期错误**
//   （少一项时 C++ 会把剩下的元素值初始化成 `{None, nullptr}`，所以不能只靠"数组声明带长度"）。
//   ★ 为什么不用 `kControlCapabilities[kControlCapabilityCount]` 当长度：那**挡不住少一项**
//     （剩余元素被值初始化），而 `sizeof/sizeof` 那条断言两向都挡。读文本的守卫仍在
//     `tools/test/capability-matrix.test.mjs`（编译期挡不住的"值初始化出一行 nullptr"由它点名）。
inline constexpr ControlCapabilityRow kControlCapabilities[] = {
    {ControlCapability::Pause, "pause"},
    {ControlCapability::Resume, "resume"},
    {ControlCapability::Sample, "sample"},
    {ControlCapability::Screenshot, "screenshot"},
    {ControlCapability::Move, "move"},
    {ControlCapability::Click, "click"},
    {ControlCapability::GlobalGet, "global-get"},
    {ControlCapability::GlobalSet, "global-set"},
    {ControlCapability::Quit, "quit"},
};

// 表行数 == 枚举项数（两向：少一项 / 多一项都编译不过）。
static_assert(sizeof(kControlCapabilities) / sizeof(kControlCapabilities[0]) == kControlCapabilityCount,
              "能力表必须逐项覆盖 ControlCapability（顺序 = hello 的数组顺序 = ControlOp 值序）");

// 能力名（**唯一一份**；`controlOpName` / `hello.capabilities` 都从这里取）。
inline constexpr const char* controlCapabilityName(ControlCapability c) {
    for (const ControlCapabilityRow& row : kControlCapabilities) {
        if (row.value == c) return row.name;
    }
    return "none";
}

// `caps=` 启动 readout 的一行 CSV（"pause,resume,...,quit"）。
// 调用方可以直接写 `printf("%s", controlCapabilitiesCsv().c_str())`：临时 `std::string` 活到
// 这一条 full-expression 结束（printf 返回之后才析构）⇒ 安全。`aot_main.cpp` 用具名变量只是可读性。
inline std::string controlCapabilitiesCsv() {
    std::string out;
    for (const ControlCapabilityRow& row : kControlCapabilities) {
        if (!out.empty()) out.push_back(',');
        out += row.name;
    }
    return out;
}

}  // namespace lift::plugin
