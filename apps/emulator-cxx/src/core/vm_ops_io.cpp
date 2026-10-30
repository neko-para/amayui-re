// 输入 / 等待 / 空操作：轮询与鼠标 · 按键回调 · 光标不可用 · 主循环模式字；等待位；
// comment / dev_ukn 两个 no-op。
//
// ★ 本文件只有 handler 本体；分派仍在 vm.cpp 的 execute()，一个 opcode 一行。
// ★ 本文件的切分是**按职责**聚合，不严格等于规范章节边界（章节归属以 spec 正文为准）。
#include "core/vm.h"

#include <cstdio>
#include <cstdlib>
#include <limits>

namespace core {

void Machine::op_10d(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 读滚轮（0x10D `read-mouse-wheel` / `sub_42EF50`）：`op1 ← Engine+0x1E74`，**随后把那一格清 0**。
    // ★ 增量由**宿主**在窗口过程里推进来（WM_MOUSEWHEEL）；符号 / 刻度属平台边界，本层不解释。
    write_int(ops[0], wheel_accum_);
    wheel_accum_ = 0;
    return;
}

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
    // ★ 诊断（`EMULATOR_CXX_TRACE_HIT=1`）：把一次命中查找的输入与结果打出来。
    //   ⛔ 宿主观察口，不是引擎语义（默认关闭、不改任何状态）。
    if (std::getenv("EMULATOR_CXX_TRACE_HIT") != nullptr) {
        std::fprintf(stderr,
                     "[hit] start=%lld x=%lld y=%lld count=%lld boxes=0x%X px=0x%X py=0x%X -> %d\n",
                     static_cast<long long>(start), static_cast<long long>(x),
                     static_cast<long long>(y), static_cast<long long>(count), boxes_address,
                     plane_x_address, plane_y_address, result);
    }
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

void Machine::op_90(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 選択肢の登録（0x90 / `sub_420640`，argc 7、推进 15；规则 `ADV-CHOICE-ADD-90`）：
    // 前四个操作数拼矩形（存 `x, y, x+宽, y+高`），后三个是该选项的三条脚本入口（body 内字下标，-1 = 无）。
    // ★ 读序 **1 → 2 → 3 → 4 → 7 → 6 → 5**（多操作数非法时抛在哪一条由它决定）。
    // ★ 本条**不**改执行位置（决定 / 进入 / 离开的跳转在消费侧）；追加成功后把选中下标复位。
    write_length(15);
    const auto x = read_int(ops[0]);
    const auto y = read_int(ops[1]);
    const auto width = read_int(ops[2]);
    const auto height = read_int(ops[3]);
    const auto decide = read_int(ops[6]);
    const auto leave = read_int(ops[5]);
    const auto enter = read_int(ops[4]);
    // 容量 100（EA 0x403B39 的 `cmp eax, 64h`）：满了 ⇒ 一个字段都不写、抛显示消息型异常。
    if (choices_.size() >= kChoiceCapacity) throw CoreError("display-message:choice-table-full");
    ChoiceEntry entry;
    entry.left = x;
    entry.top = y;
    entry.right = x + width;
    entry.bottom = y + height;
    entry.enter = enter;
    entry.leave = leave;
    entry.decide = decide;
    entry.depth = 0;  // 本模型没有帧深度档（残差）
    choices_.push_back(entry);
    choice_index_ = -1;
    // ★ 追加成功也把 **arm 闸清 0**（引擎 `sub_403B30` 尾部 `0x403BBE: [ecx+74A4h]=0`）：
    //   下一次 0x94 才会重新用当前光标算悬停。
    choice_window_.arm = false;
    return;
}

void Machine::op_97(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 選択肢の付加整数（0x97 / `sub_420910`，argc 5、推进 11；规则 `ADV-CHOICE-SET-EXTRA-97`）：
    // 前四个操作数拼矩形（`x, y, x+宽, y+高`），拿**四条边全等**去选项表里找**第一条**命中的项，
    // 找到就把它的附加整数格 ← 第 5 个操作数；**找不到什么都不做**（不建项、不报错）。
    write_length(11);
    const auto left = read_int(ops[0]);
    const auto top = read_int(ops[1]);
    const auto width = read_int(ops[2]);
    const auto height = read_int(ops[3]);
    const auto value = read_int(ops[4]);
    const auto right = left + width;
    const auto bottom = top + height;
    for (auto& choice : choices_) {
        if (choice.left == left && choice.top == top && choice.right == right &&
            choice.bottom == bottom) {
            choice.extra = value;
            return;
        }
    }
    return;
}

// 选项窗口的**清表**（`sub_403EF0`）：条目数 0、选中 -1、步长 0、**兜底跳转目标 -1**、悬停 -1、
// 移动命中开关 0、变更挂起 0 —— 七格。
// ★ 兜底目标**在**这 7 格里：`0x403F07 [ecx+74ACh] = -1`，而 `[+74ACh]` = engine `0CA84h`，
//   正是 `0x92` 写（`0x420830`）、消费侧当跳转下标读（`0x4099EC`）的那一格 ⇒ **清表会复位它**，
//   ⛔ 不是"跨窗保留"（本仓 2026-10-30 的那版注释写反了，已按语料改正）。
void Machine::reset_choice_table() {
    choices_.clear();
    choice_index_ = -1;
    choice_window_.step = 0;
    choice_window_.fallback = -1;
    choice_window_.hover = -1;
    choice_window_.move_hit = false;
    choice_window_.pending = false;
}

// 选项**消费侧**（引擎在帧循环里、门之后的一个分支里做，`sub_4098E0`）：
// 窗口开着 + 左键位 + 命中一条且该条有**决定入口** ⇒ 执行位置 ← 该入口（引擎是 **jump**：
// `PC ← 帧基址 + 4×入口`，不压返回点），清表、关窗、置闩锁。
// ★ 与 spec 的 `ADV-CHOICE-CONSUMER` 一致：这里只落**机制**；"何时消费"的次序仍属残差。
void Machine::service_choice_click() {
    // 同样是**事件锁存**：光标停在某条上不动、或按住不放，都不该反复触发选中。
    // ★ 边沿状态**每拍都更新**（窗口没开也要更新）：否则"打开窗口的那一下点击"会被当成新按下、
    //   立刻选中第一条 —— 那正是"菜单一打开就被点掉"的形态。
    std::int32_t mask = 0;
    const bool have = mouse_button_source_ && mouse_button_source_(mask);
    const bool down = have && (mask & 1) != 0;
    const bool pressed = down && !choice_input_prev_;
    choice_input_prev_ = down;
    if (!choice_window_.input_gate || choices_.empty()) return;
    if (!pressed) return;
    const auto hit = choice_hit_test();
    if (hit < 0) return;
    const auto decide = choices_[static_cast<std::size_t>(hit)].decide;
    if (decide < 0) return;  // 该条没有决定入口 ⇒ 点了也不跳（引擎返回 -1 时同样不写 PC）
    frame_mut().ip = static_cast<std::uint32_t>(decide) * 4u;
    reset_choice_table();
    choice_window_.input_gate = false;
    choice_window_.latch = true;
    return;
}

// 用当前光标对选项表做一次**矩形命中**（`sub_403C50`）：返回第一条命中的下标，没有 ⇒ -1。
std::int32_t Machine::choice_hit_test() const {
    std::int32_t x = 0;
    std::int32_t y = 0;
    if (cursor_source_ == nullptr || !cursor_source_(x, y)) return -1;
    for (std::size_t i = 0; i < choices_.size(); ++i) {
        const ChoiceEntry& entry = choices_[i];
        if (x >= entry.left && x < entry.right && y >= entry.top && y < entry.bottom) {
            return static_cast<std::int32_t>(i);
        }
    }
    return -1;
}

void Machine::op_93(const Instruction& instruction) {
    (void)instruction;
    // 选项窗口：清表 + 关窗 / 置完成闩锁（0x93 / `sub_4191D0`，argc 0、推进 1；规则 `ADV-CHOICE-WINDOW`）。
    // ★ 永远推进 1（不阻塞）：先清表，然后**二选一** —— 输入开着（总闸 ≠ 0）⇒ 关掉；否则 ⇒ 置闩锁（喂 0x91 / 0x92）。
    write_length(1);
    reset_choice_table();
    if (choice_window_.input_gate) {
        choice_window_.input_gate = false;
    } else {
        choice_window_.latch = true;
    }
    return;
}

void Machine::op_94(const Instruction& instruction) {
    (void)instruction;
    // 选项窗口：开窗（鼠标可选、上下键翻页被禁用）（0x94 / `sub_419230`，argc 0、推进 1；规则 `ADV-CHOICE-WINDOW`）。
    // ★ 无任何分支、不阻塞：总闸置 1，然后 arm —— arm-once 闸为 0 时置 1 并用**当前光标**跑一次命中测试。
    // ★ 步长写 10000（≥ 条目数 ⇒ 上下键翻页实际失效），这是本族唯一的"时间量"来源，不是超时。
    write_length(1);
    choice_window_.input_gate = true;
    if (!choice_window_.arm) {
        choice_window_.arm = true;
        choice_window_.hover = choice_hit_test();
    }
    choice_window_.move_hit = true;
    choice_window_.step = 10000;
    return;
}

void Machine::op_91(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 选项窗口：等待（0x91 / `sub_420740`，argc 1、推进 **3 或 0**；规则 `ADV-CHOICE-WINDOW`）。
    // ★ 唯一"阻塞"形态：闩锁已置 ⇒ 消费它（闩锁 = 0、移动命中开关 = 0）、**不读操作数**、推进 3；
    //   否则置动作位、arm 一次、步长 ← 第 1 个操作数，再把推进格数改回 **0** ⇒ 下一拍重跑同一条。
    write_length(3);
    if (choice_window_.latch) {
        choice_window_.latch = false;
        choice_window_.move_hit = false;
        return;
    }
    const auto step = read_int(ops[0]);
    if (!choice_window_.arm) {
        choice_window_.arm = true;
        choice_window_.hover = choice_hit_test();
    }
    choice_window_.step = step;
    choice_window_.move_hit = true;
    write_length(0);  // 阻塞：本拍不前进，下一拍重新执行
    return;
}

void Machine::op_92(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 选项窗口：等待 + 兜底跳转目标（0x92 / `sub_4207D0`，argc 2、推进 **5 或 0**；规则 `ADV-CHOICE-WINDOW`）。
    // ★ 闩锁判断在**读数之前**（EA 0x4207EB → 0x4207FE 直接返回）⇒ 闩锁已置时一个操作数都不读、兜底目标也不写。
    // ★ 读数序 **2 → 1**（先兜底目标，再步长）。
    write_length(5);
    if (choice_window_.latch) {
        choice_window_.latch = false;
        choice_window_.move_hit = false;
        return;
    }
    const auto fallback = read_int(ops[1]);
    const auto step = read_int(ops[0]);
    choice_window_.fallback = fallback;
    if (!choice_window_.arm) {
        choice_window_.arm = true;
        choice_window_.hover = choice_hit_test();
    }
    choice_window_.step = step;
    choice_window_.move_hit = true;
    write_length(0);  // 阻塞
    return;
}

}  // namespace core
