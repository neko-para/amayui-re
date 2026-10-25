// 控制流 / 帧：abort · exit · call-script · load-frame · ret · 无条件跳转 / call / 条件跳转 · 长度槽覆盖。
//
// ★ 本文件只有 handler 本体：读操作数 / 写池 / 标脏都走 Machine 的既有原语；
//   分派仍在 vm.cpp 的 execute()，一个 opcode 一行。
// ★ 本文件的切分是**按职责**聚合，不严格等于规范章节边界（章节归属以 spec 正文为准）。
#include "core/vm.h"
#include "core/install.h"

namespace core {

void Machine::op_01(const Instruction& instruction) {
    (void)instruction;
    // abort：退出型异常，不写长度槽
    halted_ = true;
    stop_.reason = StopReason::Error;
    stop_.message = "abort";
    return;
}

void Machine::op_02(const Instruction& instruction) {
    (void)instruction;
    Frame& f = frame_mut();
    // exit：父帧号 >= 0 回到父帧；根帧结束
    if (f.parent >= 0) {
        current_frame_ = static_cast<std::size_t>(f.parent);
    } else {
        halted_ = true;
        stop_.reason = StopReason::Halted;
        stop_.message = "root-exit";
    }
    return;
}

void Machine::op_03(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // call-script：父帧挂起，子帧 = 父帧号 + 1
    if (install_ == nullptr) throw CoreError("call-script-loader-missing");
    if (current_frame_ >= kFrameCount - 1) throw CoreError("frame-depth-limit");
    const auto file_id = read_int(ops[0]);
    Frame& parent = frame_mut();
    parent.position = static_cast<std::uint32_t>(parent.ip / 4);
    parent.length_slot = 3;
    const core::Bytes script = core::read_install_file(
        *install_, static_cast<std::uint32_t>(file_id));
    const core::ScriptHeader header = core::parse_script_header(script);
    const std::size_t child_index = current_frame_ + 1;
    Frame& child = frames_[child_index];
    child = Frame{};
    child.loaded = true;
    child.code.assign(script.begin() + header.body_offset, script.end());
    child.ip = 0;
    child.length_slot = 0;
    child.parent = static_cast<std::int32_t>(current_frame_);
    child.script_id = static_cast<std::uint32_t>(file_id);
    current_frame_ = child_index;
    return;
}

void Machine::op_06(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // load-frame：装进目标槽，不切换当前帧
    write_length(5);
    if (install_ == nullptr) throw CoreError("load-frame-loader-missing");
    const auto file_id = read_int(ops[0]);
    const auto target = read_int(ops[1]);
    if (target < 0 || target >= static_cast<int>(kFrameCount)) throw CoreError("frame-depth-limit");
    const core::Bytes script = core::read_install_file(*install_, static_cast<std::uint32_t>(file_id));
    const core::ScriptHeader header = core::parse_script_header(script);
    Frame& slot = frames_[static_cast<std::size_t>(target)];
    slot = Frame{};
    slot.loaded = true;
    slot.code.assign(script.begin() + header.body_offset, script.end());
    slot.ip = 0;
    slot.length_slot = 0;
    slot.parent = static_cast<std::int32_t>(current_frame_);
    slot.script_id = static_cast<std::uint32_t>(file_id);
    return;
}

void Machine::op_143(const Instruction& instruction) {
    (void)instruction;
    write_length(0); frame_mut().ip += 4; return;
}

void Machine::op_05(const Instruction& instruction) {
    (void)instruction;
    Frame& f = frame_mut();
    // ret
    write_length(1);
    if (f.ret_stack.empty()) return;
    const auto position = f.ret_stack.back();
    f.ret_stack.pop_back();
    if (position == -1) return;
    jump_to(static_cast<std::uint32_t>(position));
    return;
}

void Machine::op_8c(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 无条件跳转
    write_length(3);
    const auto target = read_int(ops[0]);
    if (target != -1) jump_to(static_cast<std::uint32_t>(target));
    return;
}

void Machine::op_8f(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    Frame& f = frame_mut();
    // call：压“下一条”的位置（本条位置 + 3）
    const auto target = read_int(ops[0]);
    if (target == -1) {
        write_length(3);
        return;
    }
    push_return(static_cast<std::uint32_t>(f.ip / 4 + 3));
    jump_to(static_cast<std::uint32_t>(target));
    return;
}

void Machine::op_a0(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 条件跳转：只读命中的那一支
    const auto condition = read_int(ops[0]);
    const auto target = read_int(condition != 0 ? ops[1] : ops[2]);
    write_length(7);
    if (target != -1) jump_to(static_cast<std::uint32_t>(target));
    return;
}

}  // namespace core
