// emulator-lift · `rep movs*` / `rep stos*` 的**宿主侧快路径**：闸门（本文件是它的唯一真源）
//
// ★ 它**不是** native override。override 在**块 / 函数入口、解码之前**拦（`machine.cpp:1320`），
//   本快路径在 `execInsn` 之后、`execString` 里面 —— 优先级照旧：
//       override > 本快路径 > 逐元素臂（`exec_string.cpp` 的宿主 `while`）
//   ⇒ override 吃掉的流量（`_memcpy` / `_memcpy_0` 那 13.6 万次调用）不会在这里再付一遍；
//   本快路径的增量收益 = override 结构上碰不到的那些内联 `rep movs*` / `rep stos*` 站点。
//   规格与裁决：REQ-01M75XK0XRTSWCZ4G8A5W2GW8（"rep movs/stos 解释器快路径"）；
//   评估（落点 / 成本 / 299 站点 / 方向×重叠的风险）：`.tmp/audio-perf/rep.md`。
//
// ★ 为什么闸门单独一个可调用的函数（而不是埋在 `execString` 的 if 里）：
//   其中两条闸门（`CrossBoundary` / `Uncommitted`）在**guest 可见状态上观察不到** —— 闸门失效时
//   逐元素臂会 fault / 读出界，那样"证明闸门在"的夹具自己先崩，红得没有信息量。
//   所以把**判定**提出来，让 `tests/rep_bulk.cpp` 直接断言"这个组合必须被判为退回"。
//   ⛔ 这不是给快路径开后门：改闸门 ⇒ 那份夹具当场红（变异实测见本单报告）。
#pragma once

#include <cstdint>

#include "runtime/x86.h"

namespace lift {

// 快路径为什么不能用（`Ok` = 可以用）。
enum class RepBulkWhy : std::uint8_t {
    Ok = 0,
    NotApplicable,   // 不是 `rep`+`movs/stos`、宿主开关关了、或 `ECX == 0`（没东西要搬）
    CrossBoundary,   // `ECX × 元素宽度` 或 `dst+` / `src+` 跨 4 GiB（宿主平坦映射只有 4 GiB）
    Uncommitted,     // safe mode 下源/目标的跨度里有未提交页 ⇒ 必须让逐元素臂去 fault
    ReverseOverlap,  // DF 与 dst/src 的相对位置给出"逆向涂抹"（4 B 分块会与逐字节不同）
};

struct RepBulkPlan {
    RepBulkWhy why = RepBulkWhy::NotApplicable;
    std::uint64_t bytes = 0;   // `ECX × 元素宽度`（★ 64 位算出来的那个值，已过 4 GiB 闸门）
    // 两条**实际访问区间**的低端地址（区间 = `[lo, lo + bytes)`）。
    // ★ 不是 `ESI`/`EDI` 本身：DF=1 时起点在**高端**，块是 `[start-bytes+size, start+size)`。
    //   搬字节 / 判重叠 / 判 committed 全部用这两个值（见 `exec_string.cpp` 的 `bulkBlock`）。
    std::uint64_t dst_lo = 0;
    std::uint64_t src_lo = 0;
    bool overlap = false;      // 仅 `movs`：两条区间相交
};

// 闸门判定：**只判，不读写内存**（连 `committed()` 这种只读查询也是判定的一部分）。
// ★ 传进来的 `size` 必须是解码器归一化后的元素宽度（1 / 2 / 4，见 `decode.cpp`）。
RepBulkPlan repBulkPlan(const X86& cpu, const Insn& in, std::uint8_t size);

}  // namespace lift
