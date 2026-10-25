// 字体与文本：字体对象两格 / 主副色 / 字号 / 对位 / 绘制模式 / 名称查表；
// set-string · 脚本字符串键（save / load）· 配置版本 · 十进制文本→整数 · 文本请求标记。
//
// ★ 本文件只有 handler 本体；分派仍在 vm.cpp 的 execute()，一个 opcode 一行。
// ★ 本文件的切分是**按职责**聚合，不严格等于规范章节边界（章节归属以 spec 正文为准）。
#include "core/vm.h"
#include "core/vm_internal.h"

namespace core {

void Machine::op_2ec(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    write_int(ops[0], parse_int_text(read_text(ops[1]))); return;  // 十进制文本转整数
}

void Machine::op_2eb(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    write_text(ops[0], config_.get_string("set:GameVersion")); return;  // 配置版本写进字符串记录
}

void Machine::op_1a4(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 字体对象两格：读序 2 → 1
    font_pair_b_ = read_int(ops[1]);
    font_pair_a_ = read_int(ops[0]);
    return;
}

void Machine::op_76(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 字体主色/副色：低 24 位字节倒序
    const auto value = as_u32(read_int(ops[0])) & 0xFFFFFFu;
    const auto swapped = ((value & 0xFFu) << 16) | (value & 0xFF00u) | ((value >> 16) & 0xFFu);
    if (instruction.opcode == 0x76) font_primary_color_ = as_i32(swapped);
    else font_secondary_color_ = as_i32(swapped);
    return;
}

void Machine::op_2bd(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 两格 = 700 / 0
    const auto value = read_int(ops[0]) != 0 ? 700 : 0;
    font_metric_a_ = value;
    font_metric_b_ = value;
    return;
}

void Machine::op_2de(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 字体名查表：宿主字体表未接，空表 ⇒ 一律未命中 -1
    (void)read_text(ops[1]);
    write_int(ops[0], -1);
    return;
}

void Machine::op_192(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // set-string：源按文本读，写入全局/局部字符串记录
    write_text(ops[0], read_text(ops[1]));
    return;
}

void Machine::op_1a9(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // save-string：把源文本写进脚本字符串键
    if (ops[0].type != 5) throw CoreError("config-key-unsupported-tag");
    config_.save_script_string(ops[0].raw, read_text(ops[0]));
    return;
}

void Machine::op_1aa(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // load-string：读脚本字符串键并写回落点
    if (ops[0].type != 5) throw CoreError("config-key-unsupported-tag");
    write_text(ops[0], config_.load_script_string(ops[0].raw));
    return;
}

void Machine::op_1a3(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // load-int：键 = 全局整型池下标；结果无条件写回操作数
    const auto key = script_int_key_index(ops[0]);
    const auto value = config_.load_script_int(key);
    write_int(ops[0], value);
    return;
}

void Machine::op_1a2(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // save-int：把操作数的整数值写进脚本键
    const auto key = script_int_key_index(ops[0]);
    config_.save_script_int(key, read_int(ops[0]));
    return;
}

void Machine::op_2db(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 字体对位模式 ← 操作数；重建入口未建模
    font_align_mode_ = read_int(ops[0]);
    return;
}

void Machine::op_78(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 字体绘制模式 ← 操作数；重建入口未建模
    font_draw_mode_ = read_int(ops[0]);
    return;
}

void Machine::op_71(const Instruction& instruction) {
    (void)instruction;
    // 文本请求：写当前帧“本条位置”；文本子系统未建模
    frame_mut().position = static_cast<std::uint32_t>(frame_mut().ip / 4);
    return;
}

void Machine::op_1a5(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    font_string_ = read_text(ops[0]); return;  // 两个字符串入口，内部未建模
}

void Machine::op_197(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    const auto value = read_int(ops[0]); font_size_ = value; return;
}

void Machine::op_75(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    font_size_ = read_int(ops[0]); return;  // 字体重建请求未建模
}

}  // namespace core
