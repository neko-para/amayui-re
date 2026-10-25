// 引擎级状态与宿主请求：标量格 · 列表记录 · 语音音量 · 存档版本标记；
// 以及音频 / ADV / Live2D / 宿主接口的"受理但未建模"占位请求。
//
// ★ 本文件只有 handler 本体；分派仍在 vm.cpp 的 execute()，一个 opcode 一行。
// ★ 本文件的切分是**按职责**聚合，不严格等于规范章节边界（章节归属以 spec 正文为准）。
#include "core/vm.h"
#include "core/vm_internal.h"

#include <algorithm>

namespace core {

void Machine::op_88(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 成对写 + 条件清门字第 27 位
    const auto value = read_int(ops[0]);
    scalar_88a_ = value;
    scalar_88b_ = value;
    if (value != 0) {
        scalar_88c_ = 1;
    } else {
        state_flags_ &= ~(1u << 27);
    }
    return;
}

void Machine::op_308(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    (void)read_int(ops[0]); return;  // 宿主接口转发请求；字段未建模
}

void Machine::op_bf(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    (void)read_int(ops[0]); state_flags_ &= ~(1u << 9); return;  // BGM 请求；配置音量路径未建模
}

void Machine::op_b4(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    (void)read_int(ops[0]); (void)read_int(ops[1]); return;  // 音频装载请求
}

void Machine::op_b5(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    (void)read_int(ops[0]); return;  // 音频播放请求
}

void Machine::op_2c7(const Instruction& instruction) {
    (void)instruction;
    return;  // ADV 文字工具请求（内部未建模）
}

void Machine::op_341(const Instruction& instruction) {
    (void)instruction;
    return;  // Live2D 请求边界（内部未建模）
}

void Machine::op_ae(const Instruction& instruction) {
    (void)instruction;
    write_length(1); if (save_version_flag_ == 0) return; throw CoreError("save-version-repair-unmodeled");
}

void Machine::op_2ee(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    scalar_2ee_ = read_int(ops[0]); return;
}

void Machine::op_10f(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    scalar_10f_ = read_int(ops[0]); return;
}

void Machine::op_248(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    scalar_248_ = read_int(ops[0]); return;
}

void Machine::op_8b(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    font_8b_ = read_int(ops[0]); return;
}

void Machine::op_261(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    engine5_[0] = read_int(ops[0]); return;
}

void Machine::op_260(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    engine5_[4] = read_int(ops[3]); engine5_[3] = read_int(ops[2]); engine5_[2] = read_int(ops[1]); engine5_[1] = read_int(ops[0]); return;
}

void Machine::op_25b(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    scalar_25b_b_ = read_int(ops[0]); scalar_25b_a_ = 2; return;
}

void Machine::op_130(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    write_int(ops[0], show_logo_); return;
}

void Machine::op_303(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 写 f120 / f124；记录必须已存在
    const auto third = read_int(ops[2]);
    const auto second = read_int(ops[1]);
    auto group = read_int(ops[0]);
    if (group == 0) group = default_group_;
    if (group < 0 || group >= 10) throw CoreError("list-group-out-of-range");
    auto& rec = list_records_[static_cast<std::size_t>(group)];
    rec.f120 = second;
    rec.f124 = third;
    return;
}

void Machine::op_212(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 记录已存在才写 f64
    auto group = read_int(ops[0]);
    if (group == 0) group = default_group_;
    if (group < 0 || group >= 10) throw CoreError("list-group-out-of-range");
    list_records_[static_cast<std::size_t>(group)].f64 = read_int(ops[1]);
    return;
}

void Machine::op_213(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 写 f68 / f6c
    const auto b = read_int(ops[2]);
    const auto a = read_int(ops[1]);
    auto group = read_int(ops[0]);
    if (group == 0) group = default_group_;
    if (group < 0 || group >= 10) throw CoreError("list-group-out-of-range");
    auto& rec = list_records_[static_cast<std::size_t>(group)];
    rec.f68 = a;
    rec.f6c = b;
    return;
}

void Machine::op_25d(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 写 f114 / f118
    const auto b = read_int(ops[2]);
    const auto a = read_int(ops[1]);
    auto group = read_int(ops[0]);
    if (group == 0) group = default_group_;
    if (group < 0 || group >= 10) throw CoreError("list-group-out-of-range");
    auto& rec = list_records_[static_cast<std::size_t>(group)];
    rec.f114 = a;
    rec.f118 = b;
    return;
}

void Machine::op_1c1(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 写列表项另一对字段（与 0x70 的第四对共享）
    const auto b = read_int(ops[2]);
    const auto a = read_int(ops[1]);
    auto group = read_int(ops[0]);
    if (group == 0) group = default_group_;
    if (group < 0 || group >= 10) throw CoreError("list-group-out-of-range");
    auto& rec = list_records_[static_cast<std::size_t>(group)];
    rec.f24 = a;
    rec.f28 = b;
    return;
}

void Machine::op_79(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 写列表项一对字段：读序 3 → 2 → 1
    const auto b = read_int(ops[2]);
    const auto a = read_int(ops[1]);
    auto group = read_int(ops[0]);
    if (group == 0) group = default_group_;
    if (group < 0 || group >= 10) throw CoreError("list-group-out-of-range");
    auto& rec = list_records_[static_cast<std::size_t>(group)];
    rec.f1c = a;
    rec.f20 = b;
    return;
}

void Machine::op_73(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 写列表记录十格 + 固定格
    const auto op10 = read_int(ops[9]);
    scalar_d107656_ = op10 == 0 ? 1 : op10;
    std::array<std::int32_t, 9> args{};
    for (int k = 9; k >= 1; --k) args[static_cast<std::size_t>(k - 1)] = read_int(ops[static_cast<std::size_t>(k - 1)]);
    auto group = args[0];
    if (group == 0) group = default_group_;
    if (group < 0 || group >= 10) throw CoreError("list-group-out-of-range");
    auto& rec = list_records_[static_cast<std::size_t>(group)];
    rec.f3c = args[3];
    rec.f40 = args[4];
    rec.f44 = args[5];
    rec.f48 = as_i32(as_u32(args[4]) + as_u32(args[6]));
    rec.f4c = as_i32(as_u32(args[5]) + as_u32(args[7]));
    rec.f50 = args[1];
    rec.f54 = args[2];
    rec.f58 = 1;
    rec.f5c = args[8];
    rec.f60 = args[8];
    return;
}

void Machine::op_80(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 默认列表组 ← 第 1 个操作数（不归一、不夹取）
    default_group_ = read_int(ops[0]);
    return;
}

void Machine::op_70(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 设置列表项：组号 0 = 默认组；四对字段
    auto group = read_int(ops[0]);
    if (group == 0) group = default_group_;
    if (group < 0 || group >= 10) throw CoreError("list-group-out-of-range");
    const auto a3 = read_int(ops[1]);
    const auto a4 = read_int(ops[2]);
    const auto a5 = read_int(ops[3]);
    const auto a6 = read_int(ops[4]);
    auto& record = list_records_[static_cast<std::size_t>(group)];
    record.f0c = a5;
    record.f10 = a6;
    record.f14 = a3;
    record.f18 = a4;
    record.f24 = a3;
    record.f28 = a4;
    record.f7c = a3;
    record.f80 = a4;
    record.registered = 1;
    return;
}

void Machine::op_1ca(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 命名键 message:ReadTextSkip ← 第 1 个操作数
    named_message_read_text_skip_ = read_int(ops[0]);
    return;
}

void Machine::op_149(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 薄标量写：原样
    scalar_149_ = read_int(ops[0]);
    return;
}

void Machine::op_21b(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 薄标量写：归一成 0/1
    scalar_21b_ = read_int(ops[0]) == 0 ? 0 : 1;
    return;
}

void Machine::op_252(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 薄标量写：原样
    scalar_252_ = read_int(ops[0]);
    return;
}

void Machine::op_2f8(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // set-voice-volume：先读第 2 个（音量），再读第 1 个（组号）
    const auto volume = read_int(ops[1]);
    const auto group = read_int(ops[0]);
    if (group < 0 || group > 2) throw CoreError("voice-group-out-of-range");
    voice_volumes_[static_cast<std::size_t>(group)] =
        std::clamp<std::int32_t>(volume, -10000, 10000);
    return;
}

void Machine::op_2f6(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // stop-voice-group：无声音后端时只清引擎级 4 格并重算标志
    const auto group = read_int(ops[0]);
    if (group < 0 || group > 2) throw CoreError("voice-group-out-of-range");
    voice_engine_cells_[static_cast<std::size_t>(group)].fill(0);
    voice_playing_ = false;  // 后端不存在：不询问任何通道
    return;
}

}  // namespace core
