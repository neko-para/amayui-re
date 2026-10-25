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
    child.local_depth = static_cast<std::int32_t>(file_id);
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
    slot.local_depth = static_cast<std::int32_t>(file_id);
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

void Machine::op_09(const Instruction& instruction) {
    (void)instruction;
    // exit-script（CORE-OPS-FRAME-EXIT-SCRIPT）：**不是返回** —— 丢掉全部帧、复位引擎级状态，
    // 再以统一文件 id 0 重装根脚本、从它的 dword 0 开始执行。调用者永远拿不回控制权。
    // ★ 顺序照站点：show-logo → 帧释放 → 池复位 → 重装（装载失败才停在最后一步）。
    show_logo_ = 0;
    state_flags_ = 0;
    driven_elsewhere_ = false;
    bracket_depth_ = 0;
    for (auto& slot : frames_) slot = Frame{};
    // 只清**当前选中 bank** 的三个全局池（`0x6d` 的另一组保留原值）。
    clear_global_pool_bank();
    // ⛔ `ref_slots_` 不清：三个引用族全局池是否复位本版不规定（别顺手复位）。
    // 待核（规范未规定，故本版不建模）：站点把"整型池逐格写初值 + 浮点池 memset"挂在
    // `set:EnableMemFlip` 上、把"纹理槽释放"挂在 `set:AutoFreeTex` 上 ⇒ 这里的无条件 clear
    // 是**最简实现**，不是"引擎无条件复位"的结论。
    if (install_ == nullptr) throw CoreError("exit-script-loader-missing");
    const Bytes script = read_install_file(*install_, 0u);
    // 重装进 0 号槽：ip ← 0、长度槽 ← 0 ⇒ 本迭代不再推进（从新根脚本的 dword 0 继续）。
    load_root(script, 0u);
    return;
}

void Machine::op_1f4(const Instruction& instruction) {
    (void)instruction;
    // 执行区间进入（CORE-OPS-RUN-BRACKET）：忙 ⇒ 只 +1（不读时钟、不动时间戳）。
    if (driven_elsewhere_) {
        bracket_depth_ += 1;
        return;
    }
    driven_elsewhere_ = true;                 // 置忙（早于取时钟）
    const std::uint32_t now = scene_clock_ms_;  // 本迭代的宿主毫秒时钟读数
    engine_previous_ms_ = engine_now_ms_;
    engine_now_ms_ = now;
    return;
}

void Machine::op_1f5(const Instruction& instruction) {
    (void)instruction;
    // 执行区间离开（CORE-OPS-RUN-BRACKET）：深度 > 0 ⇒ 只 −1（不清忙、不泵）。
    if (bracket_depth_ > 0) {
        bracket_depth_ -= 1;
        return;
    }
    // 未进入就离开 ⇒ 空操作（⛔ 不许无条件清忙）。
    if (!driven_elsewhere_) return;
    driven_elsewhere_ = false;
    // 队列占用标记恒 0（本实现没有待处理命令队列）⇒ 泵在空队列上是空操作。
    return;
}

void Machine::op_08(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // call-frame（CORE-OPS-FRAME-CALL-FRAME）：切到**已装载**的目标帧，从它的开头执行。
    // ★ 只改三格（父帧号 / ip / 长度槽），**不清返回栈**。
    write_length(3);
    const auto target = read_int(ops[0]);
    if (target < 0 || target >= static_cast<int>(kFrameCount)) {
        // ≥40 / 负数在规范里是未定义行为（按项目口径记诊断，不做夹取）。
        report_error("call-frame-slot-out-of-range:" + std::to_string(target));
        return;
    }
    Frame& slot = frames_[static_cast<std::size_t>(target)];
    if (!slot.loaded) {
        report_error("call-frame-unloaded:" + std::to_string(target));
        return;  // 当前帧号没被动过 ⇒ 等价于"恢复成调用者"
    }
    if (target == static_cast<int>(current_frame_)) {
        report_error("call-frame-self:" + std::to_string(target));
    }
    slot.parent = static_cast<std::int32_t>(current_frame_);
    slot.ip = 0;
    slot.length_slot = 0;
    current_frame_ = static_cast<std::size_t>(target);
    return;
}

void Machine::op_7b(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 进入本地块的两个目标（CORE-OPS-FRAME-LOCAL-SET-7B）：读序 1 → 2，按帧各一格、初值 −1。
    write_length(5);
    Frame& f = frame_mut();
    f.local_main = read_int(ops[0]);
    f.local_sub = read_int(ops[1]);
    return;
}

void Machine::op_199(const Instruction& instruction) {
    (void)instruction;
    // 进入本地块（CORE-OPS-STATE-199）：无操作数。
    // 先写推进格数 1、**无条件**清输入字；两条"什么都不做"的路径也要清。
    write_length(1);
    input_word_ = 0;
    Frame& f = frame_mut();
    const bool inside = (local_save_word_ & (1u << 26)) != 0;  // 已在块内 ⇒ 走副目标
    const auto target = inside ? f.local_sub : f.local_main;
    if (target == -1) return;  // 未登记 ⇒ 退化空指令（推进格数留 1）
    if (inside) {
        local_save_word_ = (local_save_word_ & ~(1u << 26)) | (1u << 25);  // 读-改-写
    } else {
        // 首次进入：state 整字搬进 save 并置第 25/26 位，state ← 0；返回点 = here + 1。
        local_save_word_ = state_flags_ | (1u << 25) | (1u << 26);
        state_flags_ = 0;
        local_save_pos_ = static_cast<std::uint32_t>(f.ip / 4 + 1);  // ★ +1 不能省
        local_save_depth_ = f.local_depth;
    }
    jump_to(static_cast<std::uint32_t>(target));
    return;
}

void Machine::op_7c(const Instruction& instruction) {
    (void)instruction;
    // 离开本地块（CORE-OPS-FRAME-LOCAL-RET-7C）：无操作数，**不先写推进格数**。
    write_length(1);  // 失败支的值；成功支下面写 0
    if ((local_save_word_ & (1u << 25)) == 0) {
        report_error("local-ret-outside-block");
        return;
    }
    if (frame_mut().local_depth != local_save_depth_) {
        report_error("local-ret-depth-mismatch");
        return;  // 这一支不写推进格数（预写的 1 即原值）
    }
    jump_to(local_save_pos_);  // ip ← 4×存档位置；长度槽 ← 0
    // 整字覆盖：保留第 26 位（⛔ 不是 &= ~位25）。
    state_flags_ = local_save_word_ & ~(1u << 25);
    local_save_word_ = 0;
    local_save_pos_ = 0;
    local_save_depth_ = 0;
    local_cell_a_ = 0;
    local_cell_b_ = 0;
    local_cell_c_ = 0;
    local_cell_d_ = 0;
    local_pending_ = 0;
    return;
}

}  // namespace core
