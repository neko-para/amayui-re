#pragma once

// 动态 / 静态**足迹**（诊断口：`emulator-cxx --dump-footprint <out.jsonl>`）。
//
// ★ 它是**观测**，不是引擎语义：只计数，不改变任何执行结果（log-only）。
// ★ 输出必须是**逐字节确定**的（同 route 两次运行相同）：只记数字，⛔ 不写时间戳 / 路径 / 墙钟。
// ★ 三样东西的键都是机械量：
//   · `op`  = 真的被**取指并派发**的 opcode（不是静态出现过的）；
//   · `gate`= 门服务链上**命中并交给服务体**的位号（位号从 0 起）；
//   · `cell`= 五个访问原语真正读 / 写过的**引擎格地址**（模拟器的池地址空间；⛔ 不写中文名）。
// ★ `static`（出现过的 opcode 集合）**不是**本文件自己算的：由 boot.cpp 对"这次运行装载过的
//   脚本"逐份解码后填进来（上界 = 可能出现，下界 = 一定执行过；两者都要）。
//
// ★ **两个写的统计**（`--dump-footprint` 的 `writer` 记录；只做写，不做读）：
//   · `bit_sets` / `bit_clears`：`state_flags_` 的**净变化**按（位号, 执行它的 opcode）计数。
//     判据是"一条指令执行前后 `before ^ after`"——所以**同一条指令内先置后清**的瞬态看不到
//     （净值为 0）；这是"按指令归因"的已知边界，不是漏记。
//   · `cell_writers`：VM 池格被**写**时，按（格地址, 执行它的 opcode）计数。
//     门服务 / 装载等"不在某条指令里"的写只进 `cells[].writes`，不进 `cell_writers`
//     （`current_opcode == 0` 时无归属）。
//
// ⚠ 边界（写在这里，免得把"没看见"当成"没发生"）：
//   本的 `cell` 只覆盖 **VM 的池格**（五个访问原语 / 地址解析 / 裸 dword）。引擎对象里的字段
//   （spec 的 `shape:` 成员，例如场景记录的 `+0xA5D8`）**没有**被逐一插桩 ⇒ 那些残差记成
//   "判不了"，不是"路径外"。
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace core {

struct Footprint {
    bool enabled = false;

    struct OpStat {
        std::uint64_t count = 0;
        std::uint32_t first_script = 0;
    };
    struct GateStat {
        std::uint64_t count = 0;
    };
    struct CellStat {
        std::uint64_t reads = 0;
        std::uint64_t writes = 0;
    };

    std::map<std::uint32_t, OpStat> ops;          // 动态：opcode → 次数 / 首见脚本
    std::map<std::int32_t, GateStat> gates;       // 动态：位号 → 服务次数
    std::map<std::uint32_t, CellStat> cells;      // 动态：格地址 → 读 / 写次数
    // 两个**写**的统计（键里的 opcode 是执行这次写的那条指令；0 不会出现在这里）：
    std::map<std::pair<std::int32_t, std::uint32_t>, std::uint64_t> bit_sets;    // (bit, opcode)
    std::map<std::pair<std::int32_t, std::uint32_t>, std::uint64_t> bit_clears;  // (bit, opcode)
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint64_t> cell_writers;  // (addr, opcode)
    // 当前正在执行的 opcode（0 = 不在指令里）。只给 `note_cell_write` 归因用。
    std::uint32_t current_opcode = 0;
    std::map<std::uint32_t, std::uint64_t> static_ops;  // 静态：脚本里出现过的 opcode → 出现次数
    std::vector<std::uint32_t> scripts;           // 这次运行**成为过当前帧**的脚本（首次出现顺序）

    // ★ 每步都会调：只有"当前帧脚本变了"才追加（按首次出现顺序去重）。
    void note_script(std::uint32_t script) {
        if (!enabled) return;
        for (const auto seen : scripts) {
            if (seen == script) return;
        }
        scripts.push_back(script);
    }

    void note_op(std::uint32_t opcode, std::uint32_t script) {
        if (!enabled) return;
        auto [it, inserted] = ops.emplace(opcode, OpStat{});
        if (inserted) it->second.first_script = script;
        ++it->second.count;
    }

    void note_gate(std::int32_t bit) {
        if (!enabled) return;
        ++gates[bit].count;
    }

    void note_cell_read(std::uint32_t address) {
        if (!enabled) return;
        ++cells[address].reads;
    }

    void note_cell_write(std::uint32_t address) {
        if (!enabled) return;
        ++cells[address].writes;
        // 只有"正在执行某条指令"的写才有 opcode 归属；服务 / 装载路径（current_opcode == 0）不记。
        if (current_opcode != 0) ++cell_writers[{address, current_opcode}];
    }

    // 一条指令执行前后 `state_flags_` 的净变化 ⇒ 按（位号, opcode）记 set / clear。
    void note_state_delta(std::uint32_t before, std::uint32_t after, std::uint32_t opcode) {
        if (!enabled) return;
        const std::uint32_t changed = before ^ after;
        for (std::int32_t bit = 0; bit < 32; ++bit) {
            if ((changed & (1u << bit)) == 0) continue;
            auto& table = ((after >> bit) & 1u) != 0 ? bit_sets : bit_clears;
            ++table[{bit, opcode}];
        }
    }
};

}  // namespace core
