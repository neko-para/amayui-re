// 引擎级状态与宿主请求：标量格 · 列表记录 · 语音音量 · 存档版本标记；
// 以及音频 / ADV / Live2D / 宿主接口的"受理但未建模"占位请求。
//
// ★ 本文件只有 handler 本体；分派仍在 vm.cpp 的 execute()，一个 opcode 一行。
// ★ 本文件的切分是**按职责**聚合，不严格等于规范章节边界（章节归属以 spec 正文为准）。
#include "core/vm.h"
#include "core/vm_internal.h"

#include <algorithm>

namespace core {

void Machine::op_c6(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 声道音量 setter（0xC6 / `sub_421070`）：op1 = 声道 0..4 ⇒ `sound:Volume{n} ← op2`。
    // ★ 越界（∉0..4）⇒ 记一行日志、**不写任何键/字段**、不抛。
    // ★ 引擎还会把增益立刻应用到在播声音/流（`sub_4885A0`/`sub_4879E0`）—— 音频层未建模（残差）。
    const auto channel = read_int(ops[0]);
    if (channel < 0 || channel > 4) {
        report_error("set-volume-channel-invalid:" + std::to_string(channel));
        return;
    }
    config_.set_int("sound:Volume" + std::to_string(channel), read_int(ops[1]));
    return;
}

void Machine::op_c7(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 声道开关 getter（0xC7 / `sub_42E670`）：op1 = 1..4（Music/SE/Voice/Movie）⇒ op2 ← 0/1。
    // ★ 只读 op1；op2 是**结果槽**（从不读旧值）。
    // ★ 负值分流**不对称**：Music 判 `>= 0`（负 ⇒ 0，**0 ⇒ 1**）；SE / Voice / Movie 判 `!= 0`（负 ⇒ 1）。
    const auto channel = read_int(ops[0]);
    if (channel < 1 || channel > 4) {
        report_error("get-sound-mode-channel-invalid:" + std::to_string(channel));
        return;
    }
    static const char* kKeys[] = {"", "sound:Music", "sound:SE", "sound:Voice", "sound:Movie"};
    const auto value = config_.get_int(kKeys[channel]);
    const bool on = channel == 1 ? (value >= 0) : (value != 0);
    write_int(ops[1], on ? 1 : 0);
    return;
}

void Machine::op_c0(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 写引擎标量（0xC0）：把引擎字段 `+0xAA9E4` 写进**第 1 个操作数**（落点按写原语的标签集：
    // 3/4/6/7/9/10/12；字符串族 / 0/1/2 / 13/14 ⇒ 类型异常）。★ 它**不读操作数的值**。
    // ⛔ 那条引擎字段的领域名未取证（只知复位为 0、写者是 `0xC3`）⇒ 本版只保存与写回。
    write_int(ops[0], engine_aa9e4_);
    return;
}

void Machine::op_142(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 写引擎标量（0x142 / `sub_422930`）：读 op1 → `Engine+0xAAB70 ← 值`；无分支、无检查。
    // ★ 值**不是布尔**（读者只判非 0，但写入的是原值）⇒ ⛔ 不许夹到 0/1。
    engine_aab70_ = read_int(ops[0]);
    return;
}

void Machine::op_1bb(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 文本显示门（0x1BB / `sub_420000`）：同一个操作数**读两次**（`.lst` `0x42001F` / `0x420038`）。
    //   0 ⇒ `Engine+0x5EC7C = 0x80000000`（bit31 = 抑制），**不记日志**；
    //   1 ⇒ `= 0`（正常显示）；
    //   其余 ⇒ **不写**（保旧值）+ 记一行日志（引擎是 WriteFile 到日志句柄，不是异常）。
    // ★ 三支都正常返回、不抛 ⇒ 日志走 `report_error` 的错误通道（不置停机）。
    const auto value = read_int(ops[0]);
    if (value == 0) {
        font_text_gate_5ec7c_ = static_cast<std::int32_t>(0x80000000u);
        return;
    }
    if (value == 1) {
        font_text_gate_5ec7c_ = 0;
        return;
    }
    report_error("set-text-gate-argument-invalid:" + std::to_string(value));
    return;
}

void Machine::op_2cc(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 配置 getter（0x2CC / `sub_4309E0`）：目标 ← `Reg::Get("message:AdvanceMesOnWheel")`。
    // ★ 它**不读任何源操作数**；键缺失时引擎弹 MessageBox 并返回 0（本版按配置缺省 0）。
    write_int(ops[0], config_.get_int("message:AdvanceMesOnWheel"));
    return;
}

void Machine::op_2cd(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 配置 setter（0x2CD / `sub_426390`）：`Reg::Set(同键, 操作数#1)`；操作数只读一次。
    config_.set_int("message:AdvanceMesOnWheel", read_int(ops[0]));
    return;
}

void Machine::op_2ce(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 画面模式 getter（0x2CE / `sub_430A20`）：目标 ← (引擎缓存字段 != 0)。
    // ★ 它**绕过配置**读缓存（脚本改了键但还没 apply 时，两者值会不同）⇒ ⛔ 不许实现成 `config_.get_int`。
    write_int(ops[0], display_mode_cache_ != 0 ? 1 : 0);
    return;
}

void Machine::op_2e6(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 读"自动消息间隔"某一档（0x2E6 / `sub_431110`）：op1 = 档号（0 / 1），op2 = **落点**（只写不读）。
    // 档号 ∉ {0,1} ⇒ 引擎写一行日志、**不写落点**、不抛。
    const auto selector = read_int(ops[0]);
    if (selector == 0) {
        write_int(ops[1], config_.get_int("message:AutoMessagePitch0"));
    } else if (selector == 1) {
        write_int(ops[1], config_.get_int("message:AutoMessagePitch1"));
    } else {
        report_error("auto-message-pitch-selector-invalid:" + std::to_string(selector));
    }
    return;
}

void Machine::op_2e7(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 写"自动消息间隔"某一档（0x2E7 / `sub_426540`）：op1 = 档号，op2 = 值；档号 ∉ {0,1} ⇒ 只记日志。
    const auto selector = read_int(ops[0]);
    if (selector == 0) {
        config_.set_int("message:AutoMessagePitch0", read_int(ops[1]));
    } else if (selector == 1) {
        config_.set_int("message:AutoMessagePitch1", read_int(ops[1]));
    } else {
        report_error("auto-message-pitch-selector-invalid:" + std::to_string(selector));
    }
    return;
}

void Machine::op_2e8(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 写"自动消息选项"（0x2E8 / `sub_4265E0`）：`message:AutoMessageOption ← op1`；没有档号校验。
    config_.set_int("message:AutoMessageOption", read_int(ops[0]));
    return;
}

void Machine::op_2ea(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 读"自动消息选项"（0x2EA / `sub_4311B0`）：**不读任何操作数**，只写落点。
    write_int(ops[0], config_.get_int("message:AutoMessageOption"));
    return;
}

void Machine::op_c5(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 声道音量 getter（0xC5 / `sub_42E540`）：op1 = 声道号（0..4）⇒ 设置表 `sound:Volume{n}` → 写 op2。
    // ★ 声道号越界（≥5 / 负）⇒ 引擎**记日志、不写落点、不抛**。
    const auto channel = read_int(ops[0]);
    if (channel < 0 || channel > 4) {
        report_error("get-volume-channel-invalid:" + std::to_string(channel));
        return;
    }
    write_int(ops[1], config_.get_int("sound:Volume" + std::to_string(channel)));
    return;
}

void Machine::op_306(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 点击跳过效果 getter（0x306 / `sub_431FC0`）：**不读任何操作数**，只把 `system:EffectSkipOnClick` 写进 op1。
    write_int(ops[0], config_.get_int("system:EffectSkipOnClick"));
    return;
}

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

// ── 命名表（CORE-OPS-TABLE-RESET-A1 / -BIND-A2 / -DISPATCH-A3） ──
// ★ 这张表与配置的命名键表**不是同一张**；键相等 = 完整文本相等。

void Machine::op_a1(const Instruction& instruction) {
    (void)instruction;
    // 重置：键与值一起失效（表变空，计数即 0）。
    named_table_.clear();
    return;
}

void Machine::op_a2(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 读序固定：先读**值**（第 2 个），再**按文本**读键（第 1 个）。
    const auto value = read_int(ops[1]);
    const std::string key = read_text(ops[0]);
    // 键已存在 ⇒ 改写值（不新增一项）；本版不设上限（表满后的行为规范不规定）。
    named_table_[key] = value;
    return;
}

void Machine::op_a3(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 先按键（按文本）查表；★ 命中时第 2 个操作数**不被读**。
    const std::string key = read_text(ops[0]);
    const auto it = named_table_.find(key);
    const auto target = it != named_table_.end() ? it->second : read_int(ops[1]);
    // 两支都清推进格数：`jump_to` 写 ip 并把长度槽清 0。登记值不做范围检查（越界由 jump_to 响亮失败）。
    jump_to(static_cast<std::uint32_t>(target));
    return;
}

// ── 状态字 / 模式族 / 清位 ──

void Machine::op_d9(const Instruction& instruction) {
    (void)instruction;
    // 清门字第 12 位（读-改-写；⛔ 不许整字写 0）。
    state_flags_ &= ~(1u << 12);
    // 那一格非 0 时，才对另一个引擎级字清它的第 12 位。
    if (d9_cell_ != 0) d9_word_ &= ~(1 << 12);
    return;
}

void Machine::op_19b(const Instruction& instruction) {
    (void)instruction;
    // 清模式位（CORE-OPS-MODE-BIT）：第 27 位按掩码清，再依次 D / A / B ← 0。
    state_flags_ &= ~(1u << 27);
    mode_d_ = 0;
    mode_a_ = 0;
    mode_b_ = 0;
    return;
}

void Machine::op_19c(const Instruction& instruction) {
    (void)instruction;
    // 按 C / E / F 条件置模式位；四支的落点不同（见 CORE-OPS-MODE-BIT）。
    mode_a_ = 1;
    mode_b_ = 0;
    if (mode_c_ != 0) {
        mode_d_ = 1;                       // C 支：置 D，进置位支
    } else if (mode_e_ != 0) {
        // E 支：★ 不碰 D
    } else if (mode_f_ != 0) {
        return;                            // F 支：A=1/B=0 保留，位与 D、G 都不动
    } else {
        mode_d_ = 0;                       // 三格皆 0 支
        state_flags_ &= ~(1u << 27);
        return;
    }
    state_flags_ |= 1u << 27;              // 置位支
    mode_g_ = 1;
    if ((state_flags_ & (1u << 29)) != 0) state_flags_ &= ~(1u << 29);
    return;
}

void Machine::op_1bf(const Instruction& instruction) {
    (void)instruction;
    // 消费状态字（CORE-OPS-STATUS-WORD）：第 16 位 ⇒ **整字**清 0（不是只清那一位）。
    if ((static_cast<std::uint32_t>(status_word_) & 0x10000u) != 0) status_word_ = 0;
    // 第 0 位的判据在清整字**之后**；命中才写相邻格为 1（否则不动那一格）。
    if ((static_cast<std::uint32_t>(status_word_) & 1u) == 0) status_neighbor_ = 1;
    return;
}

void Machine::op_1cf(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 原样覆盖状态字：不归一、不夹取、不做范围检查。
    status_word_ = read_int(ops[0]);
    return;
}

// ── 标志格 / 状态格 / 模式位读 / 层号（三格互不相同：⛔ 不要合并） ──

void Machine::op_19a(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 格 X 的**原值**写进第 1 个操作数（不归一）。
    write_int(ops[0], flag_x_);
    return;
}

void Machine::op_1b6(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 格 Y **归一成 0/1** 写进第 1 个操作数。
    write_int(ops[0], flag_y_ != 0 ? 1 : 0);
    return;
}

void Machine::op_1b7(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 格 Y ← 第 1 个操作数**先归一、再存**。
    flag_y_ = read_int(ops[0]) != 0 ? 1 : 0;
    return;
}

void Machine::op_1ad(const Instruction& instruction) {
    (void)instruction;
    // 层号写进引擎格：只搬值，不切层（不改当前层号）。落点格复位值是 −1。
    layer_cell_ = current_layer_;
    return;
}

void Machine::op_1c7(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 门字第 27 位 → 0/1（⛔ 不许写 0x08000000 这种掩码原值）。
    write_int(ops[0], static_cast<std::int32_t>((state_flags_ >> 27) & 1u));
    return;
}

void Machine::op_1cc(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 状态格的**原值**写进第 1 个操作数（不归一；与 0x1cf 的状态字是不同格）。
    write_int(ops[0], status_cell_);
    return;
}

}  // namespace core
