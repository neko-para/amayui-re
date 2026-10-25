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

}  // namespace core
