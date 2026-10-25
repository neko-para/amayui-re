// 输入 / 等待 / 空操作：轮询与鼠标 · 按键回调 · 光标不可用 · 主循环模式字；等待位；
// comment / dev_ukn 两个 no-op。
//
// ★ 本文件只有 handler 本体；分派仍在 vm.cpp 的 execute()，一个 opcode 一行。
// ★ 本文件的切分是**按职责**聚合，不严格等于规范章节边界（章节归属以 spec 正文为准）。
#include "core/vm.h"

namespace core {

void Machine::op_109(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    write_int(ops[0], -100000); write_int(ops[1], -100000); return;  // 无窗口：光标不可用
}

void Machine::op_108(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    write_int(ops[0], 0); return;  // 无窗口：按钮位图 0
}

void Machine::op_c8(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    const auto ms = read_int(ops[0]); if (ms >= 10) state_flags_ |= 1u; return;
}

void Machine::op_cc(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    mouse_interval_ = read_int(ops[0]); if (mouse_interval_ == 0) mouse_interval_ = 1; mouse_position_ = read_int(ops[1]); return;
}

void Machine::op_cd(const Instruction& instruction) {
    (void)instruction;
    return;  // 鼠标回调时间闸：无时钟/无回调时不派发
}

void Machine::op_fb(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 登记按键回调
    const auto key = read_int(ops[0]);
    const auto position = read_int(ops[1]);
    if (key < 0 || key > 31) throw CoreError("key-register-out-of-range");
    frame_mut().key_jumps[key] = position;
    return;
}

void Machine::op_24e(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    input_mode_ = static_cast<std::uint32_t>(read_int(ops[0])); return;  // 主循环模式字
}

void Machine::op_21c(const Instruction& instruction) {
    (void)instruction;
    state_flags_ |= (1u << 10); return;  // 置等待位；放行由 step 的等待服务做
}

void Machine::op_101(const Instruction& instruction) {
    (void)instruction;
    state_flags_ &= ~(1u << 27); input_word_ = 0; input_a_ = 1; input_b_ = 0; return;
}

void Machine::op_1a7(const Instruction& instruction) {
    (void)instruction;
    // comment：带一个操作数但不读它
    return;
}

void Machine::op_1a8(const Instruction& instruction) {
    (void)instruction;
    // dev_ukn：无操作数 no-op
    return;
}

void Machine::op_ff(const Instruction& instruction) {
    (void)instruction;
    // 采样按键位图（CORE-INPUT-WORD）：次序固定 = 清字 → 平台采样 → 游标 ← 0 → 上限 ← 键总数。
    input_word_ = 0;
    if (key_sampler_) input_word_ = key_sampler_();  // 没有设备 ⇒ 保持 0（不是偷偷补值）
    Frame& f = frame_mut();
    f.key_cursor = 0;
    // ★ 上限是**键总数在这一次采样时的快照**，与 `0x100` 的扫描上界（键总数本身）不是同一格。
    f.key_limit = key_total_;
    return;
}

void Machine::op_100(const Instruction& instruction) {
    (void)instruction;
    // 派发按键回调（CORE-INPUT-KEY-DISPATCH）：按输入字与游标派发**一个**回调。
    Frame& f = frame_mut();
    const auto word = input_word_;
    // ★ 扫描上界是**键总数本身**（不是 `0xff` 抄下的按帧上限）。
    const auto limit = static_cast<std::uint32_t>(key_total_);
    if (word == 0) {
        // 无按键 ⇒ 走"无按键"表项（编号 = 键总数）；返回位置 = 下一条。
        const auto it = f.key_jumps.find(static_cast<std::int32_t>(limit));
        if (it == f.key_jumps.end()) return;  // 未登记 ⇒ 只前进一条
        push_return(static_cast<std::uint32_t>(f.ip / 4 + 1));
        jump_to(static_cast<std::uint32_t>(it->second));
        return;
    }
    std::uint32_t cursor = static_cast<std::uint32_t>(f.key_cursor);
    while (cursor < limit && ((word >> cursor) & 1u) == 0) ++cursor;  // 左闭右开 [游标, 键总数)
    if (cursor >= limit) return;                                  // 没有置位 ⇒ 只前进一条
    f.key_cursor = static_cast<std::int32_t>(cursor + 1);          // 先推进游标（未登记也推进）
    const auto it = f.key_jumps.find(static_cast<std::int32_t>(cursor));
    if (it == f.key_jumps.end()) return;  // 未登记 ⇒ 放弃本轮剩余置位
    // ★ 返回位置 = **当前位置**（有意重派发），不是下一条。
    push_return(static_cast<std::uint32_t>(f.ip / 4));
    jump_to(static_cast<std::uint32_t>(it->second));
    return;
}

void Machine::op_12e(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 鼠标命中查找：**显式部分实现** —— 条目表 / 偏移数组的布局还没成 spec（audit gap）。
    // 这里不猜命中，只写 -1（没有命中）；第 2…8 个操作数**不读不写**。
    write_int(ops[0], -1);
    if (!hit_test_reported_) {
        hit_test_reported_ = true;
        report_error("unmodeled-input-hit-test:0x12e");
    }
    return;
}

void Machine::op_2fc(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 取一条鼠标事件（CORE-INPUT-MOUSE-EVENT）：没有事件 ⇒ 只写结果 0，坐标/标志/附加量**不写**。
    MouseEvent event;
    if (!mouse_event_source_ || !mouse_event_source_(event)) {
        write_int(ops[0], 0);
        return;
    }
    write_int(ops[0], 1);
    write_int(ops[1], event.x);      // 客户区坐标（映射属平台边界）
    write_int(ops[2], event.y);
    write_int(ops[3], event.flags);  // 位段本版不规定 ⇒ 原样传递
    write_int(ops[4], event.extra);
    return;
}

}  // namespace core
