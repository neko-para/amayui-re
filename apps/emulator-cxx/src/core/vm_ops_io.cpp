// 输入 / 等待 / 空操作：轮询与鼠标 · 按键回调 · 光标不可用 · 主循环模式字；等待位；
// comment / dev_ukn 两个 no-op。
//
// ★ 本文件只有 handler 本体；分派仍在 vm.cpp 的 execute()，一个 opcode 一行。
// ★ 本文件的切分是**按职责**聚合，不严格等于规范章节边界（章节归属以 spec 正文为准）。
#include "core/vm.h"

#include <limits>

namespace core {

void Machine::op_109(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 读光标位置（CORE-INPUT-MOUSE-POS）：设备可用 ⇒ 写客户区坐标；不可用 ⇒ 两个都写 −100000。
    std::int32_t x = 0;
    std::int32_t y = 0;
    const bool available = cursor_source_ && cursor_source_(x, y);
    write_int(ops[0], available ? x : -100000);
    write_int(ops[1], available ? y : -100000);
    return;
}

void Machine::op_108(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 读鼠标按钮位图（CORE-INPUT-MOUSE-BUTTON）：位 0 左 / 位 1 右；设备不可用 ⇒ 写 0。
    std::int32_t mask = 0;
    const bool available = mouse_button_source_ && mouse_button_source_(mask);
    write_int(ops[0], available ? mask : 0);
    return;
}

void Machine::op_c8(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    const auto ms = read_int(ops[0]); if (ms >= 10) state_flags_ |= 1u; return;
}

void Machine::op_cc(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 登记鼠标回调（CORE-INPUT-CALLBACK-REGISTER）：间隔（0 写成 1）、目标位置、
    // 登记时的当前帧脚本身份、上次派发时刻（= 当前时刻）。
    const auto interval = read_int(ops[0]);
    mouse_interval_ = interval == 0 ? 1 : interval;
    mouse_target_ = read_int(ops[1]);
    mouse_owner_ = frame_mut().local_depth;
    mouse_last_ms_ = scene_clock_ms_;
    return;
}

void Machine::op_cd(const Instruction& instruction) {
    (void)instruction;
    // 定时派发鼠标回调（CORE-INPUT-MOUSE-CALLBACK-PUMP）：
    // 时间闸（elapsed >= interval，含等号；或门字第 27 位）→ 压返回位置（当前位置 + 1）
    // → 目标 −1 撤销 → 脚本身份校验 → 跳到目标、推进格数 0。
    Frame& f = frame_mut();
    const auto elapsed = static_cast<std::int64_t>(static_cast<std::uint32_t>(scene_clock_ms_ - mouse_last_ms_));
    const bool force = (state_flags_ & (1u << 27)) != 0;
    if (elapsed < static_cast<std::int64_t>(mouse_interval_) && !force) return;  // 只前进一条
    mouse_last_ms_ = scene_clock_ms_;
    push_return(static_cast<std::uint32_t>(f.ip / 4 + 1));
    if (mouse_target_ == -1) {
        f.ret_stack.pop_back();  // 目标 −1 = 取消登记：撤销刚压的返回位置，只前进一条
        return;
    }
    if (mouse_owner_ != frame_mut().local_depth) {
        throw CoreError("mouse-callback-owner-mismatch");
    }
    jump_to(static_cast<std::uint32_t>(mouse_target_));
    return;
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
    // 采样输入并丢弃（CORE-OPS-WAIT-POLL-INPUT）：先请求一次平台采样，再清第 27 位 / 清字 / 写 A、B。
    input_word_ = key_sampler_ ? key_sampler_() : 0;  // 采样结果是外部请求；随后被丢弃
    state_flags_ &= ~(1u << 27);
    input_word_ = 0;
    input_a_ = 1;
    input_b_ = 0;
    return;
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
    // 命中查找（CORE-INPUT-HIT-TEST-12E）：第 2 / 5 / 6 / 7 个操作数是数组地址，
    // 第 1 / 3 / 4 / 8 个按整数读。从 start+1 起扫到 count-1，命中写项下标，否则写 -1。
    // ★ 池里的整数是编码位模式：按地址读原始 dword 后必须过 decode_int。
    const auto start = static_cast<std::int64_t>(read_int(ops[0]));
    const auto margin_address = address_of_operand(ops[1]);
    const auto x = static_cast<std::int64_t>(read_int(ops[2]));
    const auto y = static_cast<std::int64_t>(read_int(ops[3]));
    const auto boxes_address = address_of_operand(ops[4]);
    const auto plane_x_address = address_of_operand(ops[5]);
    const auto plane_y_address = address_of_operand(ops[6]);
    const auto count = static_cast<std::int64_t>(read_int(ops[7]));

    const auto decode_at = [this](std::uint32_t address) {
        return static_cast<std::int64_t>(decode_int(read_raw(address)));
    };

    // 补偿四元组（第 2 个操作数指向的四个连续整数）。
    const auto m0 = decode_at(margin_address + 0u);
    const auto m1 = decode_at(margin_address + 4u);
    const auto m2 = decode_at(margin_address + 8u);
    const auto m3 = decode_at(margin_address + 12u);

    std::int32_t result = -1;
    if (count > 0 && start + 1 < count) {
        for (std::int64_t i = start + 1; i < count; ++i) {
            const auto index = static_cast<std::uint32_t>(i);
            const auto record_address = boxes_address + index * 16u;
            // 引擎的同址门：补偿四元组与当前框记录同址 ⇒ 跳过该记录（不是命中、也不是终止）。
            if (record_address != margin_address) {
                const auto s0 = decode_at(record_address + 0u);
                const auto s1 = decode_at(record_address + 4u);
                const auto s2 = decode_at(record_address + 8u);
                const auto s3 = decode_at(record_address + 12u);
                const auto dx = x - decode_at(plane_x_address + index * 4u);
                const auto dy = y - decode_at(plane_y_address + index * 4u);
                if (m1 + dx - s0 >= 0 && m3 + dy - s2 >= 0 && s1 - dx - m0 >= 0 &&
                    s3 - dy - m2 >= 0) {
                    result = static_cast<std::int32_t>(i);
                    break;
                }
            }
        }
    }
    write_int(ops[0], result);
    return;
}

void Machine::op_2fc(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 取一个触点（CORE-INPUT-TOUCH-POINT）：没有触点 ⇒ 只写结果 0，坐标/标志/编号**不写**。
    TouchPoint event;
    if (!touch_source_ || !touch_source_(event)) {
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

// ── 按住派发族（`0x102`..`0x105` / `0xfc`） ──

void Machine::op_fc(const Instruction& instruction) {
    (void)instruction;
    // 输入复位（CORE-INPUT-RESET-FC）：两张回调表**所有帧**清空，再把按住游标模数写 0。
    // ⛔ 不清：输入字 / 门字 / 键总数 / 掩码 / 任何游标 / 按键扫描上限；也不切帧。
    // ⛔ 不是 `slot = Frame{}`（那会连 ip / key_limit / key_cursor / ret_stack 一起清）。
    for (auto& f : frames_) {
        f.key_jumps.clear();
        f.hold_slots.clear();
    }
    hold_modulus_ = 0;  // 有意的复位：之后必须重新 0x103，否则下一次 0x104 除零
    return;
}

void Machine::op_102(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 登记按住派发槽（CORE-INPUT-102-SLOT）：读序 1 → 2 → 3，三个读完再判越界。
    const auto key = read_int(ops[0]);      // 键编号 0..31（与键总数无关）
    const auto slot = read_int(ops[1]);     // 槽号 0..9（与模数无关）
    const auto target = read_int(ops[2]);   // 目标位置（dword 下标）；−1 = 取消登记，不检查范围
    if (key < 0 || key > 31) throw CoreError("hold-slot-key-out-of-range");
    if (slot < 0 || slot > 9) throw CoreError("hold-slot-index-out-of-range");
    // 通过检查才写（越界一个槽都不写）；键用 64 位算，避免游标越界时的有符号溢出。
    frame_mut().hold_slots[static_cast<std::int64_t>(key) * 10 + slot] = target;
    return;
}

void Machine::op_103(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 设模数（CORE-INPUT-103-MODULUS）：**不做任何范围检查**（0 / 负数都照写）。
    hold_modulus_ = read_int(ops[0]);  // 0 ⇒ 下一次 0x104 在出口处响亮失败
    hold_cursor_by_slot_[current_frame_] = 0;  // 只清**当前帧槽**；其它槽不动
    return;
}

void Machine::op_105(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 设按住掩码（CORE-INPUT-105-MASK）：32 位位集，原样、不归一、不截断。
    hold_mask_ = static_cast<std::uint32_t>(read_int(ops[0]));
    return;
}

void Machine::advance_hold_cursor() {
    const auto modulus = hold_modulus_;
    if (modulus == 0) throw CoreError("hold-cursor-divide-by-zero");  // 目标机 = 整数除零
    auto& slot = hold_cursor_by_slot_[current_frame_];
    if (!slot) throw CoreError("hold-cursor-uninitialized");  // 引擎不建立初值 ⇒ UB，响亮失败
    // 目标机：先 32 位回绕 +1，再按向零截断取余（余数符号随被除数）。
    const auto wrapped = static_cast<std::int32_t>(static_cast<std::uint32_t>(*slot) + 1u);
    if (modulus == -1) {
        if (wrapped == std::numeric_limits<std::int32_t>::min()) {
            throw CoreError("hold-cursor-divide-overflow");  // INT_MIN / -1 商溢出
        }
        *slot = 0;  // 任何 x % -1 == 0
    } else {
        *slot = wrapped % modulus;
    }
    return;
}

void Machine::op_104(const Instruction& instruction) {
    (void)instruction;
    // 按住派发按键回调（CORE-OPS-INPUT-104）：反向找编号 → 上下文闸 → 掩码夹上限 →
    // 查表（跳 / 不跳）→ **每条出口都推进游标**。
    Frame& f = frame_mut();
    const auto here = static_cast<std::uint32_t>(f.ip / 4);  // 本条自身的 dword 位置
    const std::uint32_t word = input_word_;
    const auto limit = f.key_limit;

    // ① 反向取最高置位；limit ≤ 0 不扫描；找不到 ⇒ 编号 = 键总数（"无按键"编号）。
    std::int32_t key = -1;
    if (limit > 0) {
        for (std::int32_t i = limit - 1; i >= 0; --i) {
            if (i < 32 && ((word >> i) & 1u) != 0u) {
                key = i;
                break;
            }
        }
    }
    const bool found = key >= 0;
    const auto index = found ? key : key_total_;

    // ② 上下文闸：编号变了 ⇒ 只清**当前帧槽**的游标，然后把编号写进静态格。
    if (hold_context_key_ != index) hold_cursor_by_slot_[current_frame_] = 0;
    hold_context_key_ = index;

    // ③ 掩码只在"找到键"时判；夹上限在**查表之前**、且**不撤销**。
    const bool masked = found && ((hold_mask_ >> key) & 1u) != 0u;
    if (masked) f.key_limit = key;
    push_return(masked ? here : here + 1);

    // ④ 查表：槽号 = 当前游标（可能刚被闸清 0）；缺格与写进去的 −1 都算未登记。
    const auto& cursor = hold_cursor_by_slot_[current_frame_];
    if (!cursor) {
        f.ret_stack.pop_back();  // 撤销返回点；长度槽留在 1
        throw CoreError("hold-cursor-uninitialized");
    }
    const auto it = f.hold_slots.find(static_cast<std::int64_t>(index) * 10 + *cursor);
    if (it == f.hold_slots.end() || it->second == -1) {
        f.ret_stack.pop_back();
    } else {
        jump_to(static_cast<std::uint32_t>(it->second));  // ip ← 4×目标；长度槽 ← 0
    }
    // ⑤ 每条出口都推进游标（跳 / 不跳、登记 / 未登记）。
    advance_hold_cursor();
    return;
}

}  // namespace core
