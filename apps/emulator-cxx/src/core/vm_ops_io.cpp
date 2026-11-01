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
    // ★ 次序是合同（`CORE-INPUT-WHEEL-READ`）：**先清零、后写落点** —— 落点写因类型不符抛异常时，
    //   累加格**已经**被清 0（这一支可观察）。⛔ 不许交换这两步。
    // ★ 增量由**宿主**在窗口过程里推进来（WM_MOUSEWHEEL）；符号 / 刻度属平台边界，本层不解释。
    const std::int32_t value = wheel_accum_;
    wheel_accum_ = 0;
    write_int(ops[0], value);
    return;
}

void Machine::op_109(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 读光标位置（CORE-INPUT-MOUSE-POS）：输入可用标志非 0 ⇒ 向平台查询（写客户区坐标）；
    // 为 0 ⇒ **不查询**、两个操作数都写 −100000（「不可用」不是「不写」）。
    std::int32_t x = 0;
    std::int32_t y = 0;
    const bool available = input_available_ && cursor_source_ && cursor_source_(x, y);
    write_int(ops[0], available ? x : -100000);
    write_int(ops[1], available ? y : -100000);
    return;
}

void Machine::op_108(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 读鼠标按钮位图（CORE-INPUT-MOUSE-BUTTON）：位 0 左 / 位 1 右；输入可用标志为 0 ⇒ 不查询、写 0。
    // ★ 平台报告的是**逻辑左 / 右键**（`mask` 的位 0 / 1），引擎按交换设置把它们归约到固定的两位。
    std::int32_t bits = 0;
    std::int32_t mask = 0;
    if (input_available_ && mouse_button_source_ && mouse_button_source_(mask)) {
        const bool left = (mask & 1) != 0;
        const bool right = (mask & 2) != 0;
        if (left) bits |= mouse_swap_ ? 2 : 1;
        if (right) bits |= mouse_swap_ ? 1 : 2;
    }
    write_int(ops[0], bits);
    return;
}

// ── 一次输入采样（`CORE-INPUT-WORD` 的「各来源的位号合成」/「输入锁存」） ──

void Machine::add_slot_base_bit(std::uint32_t& word, std::int32_t slot) {
    // 从**槽**出发：键槽映射的格值 = 该槽的**基位号**，最终输入位号 = 基位号 + 4。
    if (slot < 0 || slot >= static_cast<std::int32_t>(array_d1383_.size())) {
        report_error("input-slot-out-of-range");
        return;
    }
    const std::int32_t base = array_d1383_[static_cast<std::size_t>(slot)];
    if (base < 0 || base > 31) {
        // 格里的值不做范围检查（`0x10b` 只检查写入值 ≤ 31，越界写是脚本侧的 UB）⇒ 显式诊断。
        report_error("input-slot-base-bit-out-of-range");
        return;
    }
    // ★ 位号 ≥ 32 的落点：写输入字是"1 左移位号"，目标机的移位**只取低 5 位** ⇒ 位 32 / 33 / 34 / 35
    //   实际落在 0 / 1 / 2 / 3（`CORE-INPUT-WORD` 的「位号 ≥ 32 的落点」）。⛔ 不是"超出就丢弃"。
    word |= 1u << ((base + 4) & 31);
}

// ── 摇杆轴的四条阈值与 20 毫秒查询缓存（`CORE-INPUT-WORD` 的「手柄的方向位」） ──

void Machine::set_joy_axis_range(int axis, std::uint32_t minimum, std::uint32_t maximum) {
    // 设备能力查询（`joyGetDevCapsA`）给的范围：**只是范围**，不参与阈值计算 ——
    // 阈值在初始化那次采样（`sample_joy_axis_origin`）里由"范围 + 当时的轴位置"一起算出。
    const auto index = static_cast<std::size_t>(axis) & 1u;
    joy_axis_min_[index] = minimum;
    joy_axis_max_[index] = maximum;
}

void Machine::sample_joy_axis_origin() {
    // 引擎初始化：向平台取**当时**的两轴位置 `(X0, Y0)`，然后算四条阈值：
    //   低 =（最小值 + 轴位置）÷ 2、高 =（最大值 + 轴位置）÷ 2（**向零截断**）。
    // ★ **只采一次**：轴位置此后不重估 ⇒ 阈值固定；第二次调用不再改任何一格（只记观察口）。
    if (joy_axis_thresholds_ready_) return;
    if (!joy_stick_source_) return;  // 没有设备 ⇒ 没有轴位置，也就没有阈值
    const auto origin = joy_stick_source_();
    for (std::size_t axis = 0; axis < 2; ++axis) {
        joy_axis_origin_[axis] = origin[axis];
        // 无符号值的 ÷2 就是**向零截断**的 ÷2（规范：除法向零截断）。
        joy_axis_low_[axis] = (joy_axis_min_[axis] + origin[axis]) / 2u;
        joy_axis_high_[axis] = (joy_axis_max_[axis] + origin[axis]) / 2u;
    }
    joy_axis_thresholds_ready_ = true;
    ++joy_axis_origin_samples_;
}

std::array<std::uint32_t, 2> Machine::query_joy_axes() {
    // ★ 缓存（合同）：距上次查询**不超过 20 毫秒（含边界）**⇒ 复用上次那批原始轴值，不重新查询。
    const std::uint32_t now = host_now_ms();
    if (joy_axis_cache_valid_ && static_cast<std::uint32_t>(now - joy_axis_last_query_ms_) <= 20u) {
        return joy_axis_cache_;
    }
    joy_axis_cache_ = joy_stick_source_ ? joy_stick_source_() : std::array<std::uint32_t, 2>{};
    joy_axis_cache_valid_ = true;
    joy_axis_last_query_ms_ = now;
    ++joy_axis_queries_;
    return joy_axis_cache_;
}

std::uint32_t Machine::sample_joystick_bits() {
    // 第一组手柄的**这一拍**结果：方向位 0..3 + 按钮位（槽序号 + 4）。
    // ★ 组头的按钮数 ≤ 0 ⇒ **整组**不参与采样（方向位也一起不采）。
    if (joy_button_count_ <= 0) return 0;
    std::uint32_t bits = 0;
    // ① 方向位 0..3（与键盘方向键**共用**这四位）：帽子四向可用且不居中 ⇒ 按**四个半平面**判据；
    //    否则（居中 / 设备不报四向）⇒ 改由**摇杆轴**越过四条阈值给出。
    int angle = -1;
    if (joy_hat_four_way_ && joy_hat_source_) angle = joy_hat_source_();
    if (joy_hat_four_way_ && angle >= 0) {
        if (angle <= 8999 || angle >= 27001) bits |= 1u << 0;  // 上
        if (angle >= 1 && angle <= 17999) bits |= 1u << 1;     // 右
        if (angle >= 9001 && angle <= 26999) bits |= 1u << 2;  // 下
        if (angle >= 18001) bits |= 1u << 3;                   // 左
    } else if (!joy_ignore_stick_ && joy_stick_source_ && joy_axis_thresholds_ready_) {
        // ★ 合同（`CORE-INPUT-WORD` 的「手柄的方向位」）：轴 1（横）< 低 ⇒ 左（位 3）、> 高 ⇒ 右（位 1）；
        //   轴 2（纵）< 低 ⇒ 上（位 0）、> 高 ⇒ 下（位 2）。比较是**严格的**（等于阈值不算命中）
        //   且按**无符号**（两端都按 `std::uint32_t` 比）。
        // ★ 阈值**未算出**（初始化没采到轴位置）⇒ 引擎没有"中位"这个量 ⇒ 不给方向位（⛔ 不补缺省值）。
        const auto stick = query_joy_axes();
        if (stick[1] < joy_axis_low_[1]) bits |= 1u << 0;   // 上（小值端 = 上）
        if (stick[0] > joy_axis_high_[0]) bits |= 1u << 1;  // 右（大值端 = 右）
        if (stick[1] > joy_axis_high_[1]) bits |= 1u << 2;  // 下（大值端 = 下）
        if (stick[0] < joy_axis_low_[0]) bits |= 1u << 3;   // 左（小值端 = 左）
    }
    // ② 按钮：槽 j（0 ≤ j < **键总数**）→ 位（槽序号 + 4，移位的低 5 位）。
    if (joy_button_source_) {
        const auto buttons = joy_button_source_();
        const std::int32_t limit = key_total_ < 0 ? 0 : (key_total_ > 31 ? 31 : key_total_);
        for (std::int32_t j = 0; j < limit; ++j) {
            const std::int32_t button = array_d551_[static_cast<std::size_t>(j)];
            if (button < 0 || button >= 32) continue;  // 按钮编号越界 = UB ⇒ 显式跳过（不夹取）
            if ((buttons & (1u << button)) != 0) bits |= 1u << ((j + 4) & 31);
        }
    }
    return bits;
}

std::uint32_t Machine::sample_level_input() {
    // **电平采样**（`0xff` 走它）。次序固定：键盘 → 鼠标左右键 → 第一组手柄（含方向位）→ 手势。
    std::uint32_t word = 0;
    if (input_available_) {
        // ① 键盘：**遍历全部 256 个系统键码**，逐键查询一次按下状态；位号 = **格里的值**（⛔ 不加 4）。
        if (key_code_source_) {
            for (std::int32_t code = 0; code < static_cast<std::int32_t>(kSystemKeyCount); ++code) {
                const std::int32_t bit = key_code_table_[static_cast<std::size_t>(code)];
                if (bit < 0 || bit >= 32) continue;  // −1 = 该键码未映射
                if (key_code_source_(code)) word |= 1u << bit;
            }
        }
        // ② 鼠标左右键：固定用槽 0 / 1（交换决定哪个按钮用哪一槽），位号 = **格里的值 + 4**。
        if (mouse_button_source_) {
            std::int32_t mask = 0;
            if (mouse_button_source_(mask)) {
                if ((mask & 1) != 0) add_slot_base_bit(word, mouse_swap_ ? 1 : 0);
                if ((mask & 2) != 0) add_slot_base_bit(word, mouse_swap_ ? 0 : 1);
            }
        }
        // ③ 手势来源：那张 8 格表由构造期整表写成「不并入」，运行期**没有第二个写者**
        //   （`CORE-INPUT-WORD` 的「手势来源的合成规则」）⇒ 发行初值下不点亮任何位 ⇒ 本模型恒不并入。
    }
    // ④ 第一组手柄：**不看**输入可用标志，只看组头的按钮数（含方向位）。
    word |= sample_joystick_bits();
    // ⑤ 宿主注入的输入位（测试装置 / 脚本化输入；⛔ 产品路径不用它 —— 窗口路径走逐键查询）。
    if (key_sampler_) word |= key_sampler_();
    return word;
}

std::uint32_t Machine::sample_event_input() {
    // **事件并入采样**（`0x101` 与主循环的输入处理走它）：并入两个锁存字、手柄的**按下沿**、
    // 手势锁存与引擎级标志。★ 它不是"只是读一下"：并入的锁存位被清掉、手柄按下沿的状态字被前移。
    std::uint32_t word = 0;
    // ① 位号锁存字：整字 OR 后清 0。
    word |= latch_bits_;
    latch_bits_ = 0;
    // ② 一次性吞并标记：一次采样之后检查鼠标左右键，只要发现左键或右键正按下就把标记写成 1。
    if (mouse_button_source_) {
        std::int32_t mask = 0;
        if (mouse_button_source_(mask) && (mask & 3) != 0) mouse_swallow_ = true;
    }
    // ③ 槽锁存字：按组头的**槽数**逐槽并入，并把该位清掉；槽数以外的位原样保留。
    for (std::int32_t j = 0; j < key_slot_count_ && j < 32; ++j) {
        if ((latch_slots_ & (1u << j)) == 0) continue;
        add_slot_base_bit(word, j);
        latch_slots_ &= ~(1u << j);
    }
    // ④ 手柄的**按下沿**：对第一组再采一次样（含方向 / 帽子 / 按钮），与**每设备一格的上一次结果**
    //    逐位比较，只在"上一拍没按、这一拍按下"时并入；状态字随之前移（这一拍没按的位自然清掉）。
    //    ★ 它是**状态字**，不是"并入后整字清 0"的锁存字。
    const std::uint32_t joy_bits = sample_joystick_bits();
    word |= joy_bits & ~joy_prev_bits_;
    joy_prev_bits_ = joy_bits;
    // ⑤ 引擎级标志 D 非 0 ⇒ 输入字第 6 位置起（`CORE-OPS-MODE-BIT` 的 D：注入"取消消息跳过"这一路）。
    if (mode_d_ != 0) word |= 1u << 6;
    // ⑥ 手势锁存（整字 OR 后清 0）：8 格表全「不并入」且没有第二个写者（`CORE-INPUT-WORD`）⇒ 锁存恒空。
    // ⑦ 宿主注入的输入位（测试装置；同电平采样那一支）。
    if (key_sampler_) word |= key_sampler_();
    return word;
}

void Machine::push_key_down_message(std::int32_t system_key_code) {
    // 引擎自己的窗口过程：某个系统键码的**按下**消息（按住产生的重复消息不算 —— 由调用方过滤）。
    // 若该键码在键码表里有映射，把**对应的输入位**写进位号锁存字。
    if (system_key_code < 0 || system_key_code >= static_cast<std::int32_t>(kSystemKeyCount)) return;
    const std::int32_t bit = key_code_table_[static_cast<std::size_t>(system_key_code)];
    if (bit < 0 || bit >= 32) return;  // 未映射的键码不写锁存
    latch_bits_ |= 1u << bit;
}

void Machine::push_mouse_button_message(bool right, bool down) {
    // 引擎自己的窗口过程：鼠标左右键消息。
    // ① 一次性吞并标记非 0 ⇒ **这一条**消息被丢掉（锁存字不写）并把标记清 0；
    // ② 否则按配置 `set:ClickOnUp` 选一条：为 0 用**按下**消息、非 0 用**抬起**消息；
    // ③ 选中的那条把**槽 0 / 1** 写进槽锁存字（交换设置决定左 / 右各用哪一槽）。
    if (mouse_swallow_) {
        mouse_swallow_ = false;
        return;
    }
    const bool selected = click_on_up_ == 0 ? down : !down;
    if (!selected) return;
    const std::int32_t slot = right ? (mouse_swap_ ? 0 : 1) : (mouse_swap_ ? 1 : 0);
    latch_slots_ |= 1u << slot;
}

void Machine::op_101(const Instruction& instruction) {
    (void)instruction;
    // 采样输入并丢弃（CORE-OPS-WAIT-POLL-INPUT）：走**事件并入**路径采样一次，再清第 27 位 / 清字 / 写 A、B。
    // ★ 这次采样**有副作用**：它消费了并入的事件（锁存字被清、手柄按下沿前移）⇒ 之后走同一路径的
    //   采样看不到这些事件。⛔ 不许把它写成"只是读一下"。
    input_word_ = sample_event_input();
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
    // 采样按键位图（CORE-INPUT-WORD）：次序固定 = 清字 → **电平采样** → 游标 ← 0 → 上限 ← 键总数。
    // ★ 这次采样**不并入输入锁存**（并入只发生在「事件并入」路径上）；没有设备 ⇒ 输入字保持 0。
    input_word_ = 0;
    input_word_ = sample_level_input();
    Frame& f = frame_mut();
    f.key_cursor = 0;
    // ★ 上限是**键总数在这一次采样时的快照**，与 `0x100` 的扫描上界（键总数本身）不是同一格。
    f.key_limit = key_total_;
    return;
}

void Machine::op_c8(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    const auto ms = read_int(ops[0]);
    if (ms >= 10) { state_flags_ |= 1u; return; }
    // 短睡眠（`CORE-OPS-SLEEP-C8`：n < 10 ⇒ **处理器内阻塞** n 毫秒）：
    // ⛔ **不得变成 no-op** —— spec 原文「实现可以用虚拟时钟代替真实阻塞，但必须让**时间前进
    // n 毫秒**、并保持次序…把 `sleep 1` 当作"什么都不做"会让轮询循环以 CPU 全速空转，属于违反本条」。
    // ★ **谁拥有时钟，就由谁让时间过去**（`Machine::set_sleep_host`）：装了宿主睡眠口就交给它
    //   （窗口路径 = 真的 `SDL_DelayPrecise`；无窗口路径 = 把仿真时钟推进 n），处理器体内**不再**自己
    //   写宿主时钟 —— 两个写者会把钟来回拨，而 `0x300` 的「现在 − 起点 ≥ 期限」按无符号减法判到期，
    //   钟往回走就等于期限当场到了（实测：设置界面示例文本偶尔连播 2–5 次）。
    // ★ 没有宿主口（自测 / 虚拟时钟）时仍按原口径自己推进**宿主采样时钟**：场景时钟对仍只由
    //   `CORE-TIMING-CLOCK` 登记的四个写者写（⛔ 别把睡眠塞成第五个写者）。读者是 `0xcd` 的时间闸
    //   —— 它读的是现场宿主时刻（`0x41ACF1 timeGetTime`），不是场景时钟对。
    // ⚠ 负值（按无符号解释 ⇒ 极长阻塞）本模型不阻塞：留作残差，不在这里发明行为。
    if (ms > 0) {
        const auto sleep_ms = static_cast<std::uint32_t>(ms);
        if (sleep_host_) {
            sleep_host_(sleep_ms);
        } else {
            host_clock_ms_ += sleep_ms;
        }
    }
    return;
}

void Machine::op_cc(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 登记鼠标回调（CORE-INPUT-CALLBACK-REGISTER）：间隔（0 写成 1）、目标位置、
    // 登记时的当前帧脚本身份、上次派发时刻（= 当前时刻）。
    // ★ 「当前时刻」= **现场宿主时刻**（`0xcd` 的同一路读数），⛔ 不是场景时钟对 —— 门前盖钟被
    //   (c) 取消时场景时钟会停走，而轮询节拍必须照走（否则 TITLE 的输入环等不到自己的回调）。
    const auto interval = read_int(ops[0]);
    mouse_interval_ = interval == 0 ? 1 : interval;
    mouse_target_ = read_int(ops[1]);
    mouse_owner_ = frame_mut().local_depth;
    mouse_last_ms_ = host_now_ms();
    return;
}

void Machine::op_cd(const Instruction& instruction) {
    (void)instruction;
    // 泵本体的**入口计数**（观察口，⛔ 不是引擎状态）：守卫用它把「未被派发时不跑」变成会红的断言 ——
    // 只盯「上次派发时刻」抓不住"第二入口进来却被时间闸挡回去"（那一支不改任何格）。
    ++mouse_pump_entries_;
    // 定时派发鼠标回调（CORE-INPUT-MOUSE-CALLBACK-PUMP）：
    // 时间闸（elapsed >= interval，含等号；或门字第 27 位）→ 压返回位置（当前位置 + 1）
    // → 目标 −1 撤销 → 脚本身份校验 → 跳到目标、推进格数 0。
    // ★ 「读一次当前时刻」= **现场宿主时刻**（`0x41ACF1 call timeGetTime` / `0x41ACF9 sub edx,
    //   [esi+68EF0h]`）⇒ elapsed **不依赖场景时钟**；(c) 取消门前盖钟也挡不住这条轮询节拍。
    Frame& f = frame_mut();
    const auto elapsed = static_cast<std::int64_t>(static_cast<std::uint32_t>(host_now_ms() - mouse_last_ms_));
    const bool force = (state_flags_ & (1u << 27)) != 0;
    if (elapsed < static_cast<std::int64_t>(mouse_interval_) && !force) return;  // 只前进一条
    mouse_last_ms_ = host_now_ms();
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
    // 第 1 / 3 / 4 / 8 个按整数读。
    // ★ 起始项与末项都按 **32 位回绕算术**算（规范「成立条件」）：
    //   起始项地址 =（start + 1）× 每项字节数 + 框表基址；末项指针 = 框表基址 + count × 每项字节数；
    //   扫描在"当前项指针**无符号 ≥** 末项指针"时结束（不是下标比较）。
    //   ⇒ 三个端点因此可逐字推出：`start` = 最大正数 ≡ −1；`start ≤ −2` 且偏移不回绕 ⇒ 首项落在表**之前**；
    //     `count` 使末项指针回绕到基址之前 ⇒ 立刻判"无命中"。
    // ★ 池里的整数是编码位模式：按地址读原始 dword 后必须过 decode_int。
    const auto start = static_cast<std::uint32_t>(read_int(ops[0]));
    const auto margin_address = address_of_operand(ops[1]);
    const auto x = static_cast<std::uint32_t>(read_int(ops[2]));
    const auto y = static_cast<std::uint32_t>(read_int(ops[3]));
    const auto boxes_address = address_of_operand(ops[4]);
    const auto plane_x_address = address_of_operand(ops[5]);
    const auto plane_y_address = address_of_operand(ops[6]);
    const auto count = static_cast<std::uint32_t>(read_int(ops[7]));

    const auto decode_at = [this](std::uint32_t address) {
        return static_cast<std::uint32_t>(decode_int(read_raw(address)));
    };
    const auto non_negative = [](std::uint32_t value) { return (value & 0x80000000u) == 0; };

    // 补偿四元组（第 2 个操作数指向的四个连续整数）。
    const auto m0 = decode_at(margin_address + 0u);
    const auto m1 = decode_at(margin_address + 4u);
    const auto m2 = decode_at(margin_address + 8u);
    const auto m3 = decode_at(margin_address + 12u);

    // `count ≤ 0` ⇒ **一次都不扫**（规范「成立条件」明文；`count` 为负时按 32 位回绕算出的末项
    // 指针可能落在基址之前 —— 这一条先挡，不必依赖回绕的巧合）。
    const auto begin = boxes_address + (start + 1u) * 16u;  // 32 位回绕
    const auto end = boxes_address + count * 16u;            // 32 位回绕

    std::int32_t result = -1;
    if (static_cast<std::int32_t>(count) > 0) {
        for (std::uint32_t pointer = begin; pointer < end; pointer += 16u) {  // 无符号比较
            // 落点 =（当前项指针 − 框表基址）÷ 16，按**带符号**算：首项落在表前时是负值
            // （与引擎的 `sar` + 向零截断除法同口径）。基准平面按**同一个项下标**取值。
            const auto index = static_cast<std::int32_t>(pointer - boxes_address) / 16;
            // 引擎的同址门：补偿四元组与当前框记录同址 ⇒ 跳过该记录（不是命中、也不是终止）。
            if (pointer == margin_address) continue;
            const auto s0 = decode_at(pointer + 0u);
            const auto s1 = decode_at(pointer + 4u);
            const auto s2 = decode_at(pointer + 8u);
            const auto s3 = decode_at(pointer + 12u);
            const auto plane_offset = static_cast<std::uint32_t>(index) * 4u;
            const auto dx = x - decode_at(plane_x_address + plane_offset);
            const auto dy = y - decode_at(plane_y_address + plane_offset);
            // 四项判据用 **32 位**加减后**合并符号位**判断（`or` + 与 0 比较）：
            // 溢出**回绕**，不扩宽、不饱和（规范「成立条件」）。
            const std::uint32_t merged = (m1 + dx - s0) | (m3 + dy - s2) | (s1 - dx - m0) |
                                         (s3 - dy - m2);
            if (non_negative(merged)) {
                result = index;
                break;
            }
        }
    }
    write_int(ops[0], result);
    // ★ 诊断（`EMULATOR_CXX_TRACE_HIT=1`）：把一次命中查找的输入与结果打出来。
    //   ⛔ 宿主观察口，不是引擎语义（默认关闭、不改任何状态）。
    if (std::getenv("EMULATOR_CXX_TRACE_HIT") != nullptr) {
        std::fprintf(stderr,
                     "[hit] start=%d x=%d y=%d count=%d boxes=0x%X px=0x%X py=0x%X -> %d\n",
                     static_cast<std::int32_t>(start), static_cast<std::int32_t>(x),
                     static_cast<std::int32_t>(y), static_cast<std::int32_t>(count), boxes_address,
                     plane_x_address, plane_y_address, result);
    }
    return;
}

void Machine::push_touch_message(const std::vector<TouchPoint>& points) {
    // 触点缓冲的**整体重建**（`CORE-INPUT-TOUCH-POINT`）—— 次序是合同：
    //   ① 触点计数先清 0 → ② 释放旧缓冲（指针清空）→ ③ 按**这条消息携带的**点数重新分配
    //   （每项固定 40 字节）→ ④ 把这批触点逐个读入并把取回个数写回计数。
    // ⇒ 上一次消息的触点**全部不可见**；**不设容量上限**、不排队、也不按新消息"淘汰最旧项"。
    // ⛔ 不许复用旧容量（那会让"从 N 降到 M"之后还看得见第 M..N 项）。
    touch_count_ = 0;  // ①
    // ② 释放旧缓冲 + ③ 按**这条消息的**点数重新分配（每项固定 40 字节）：
    //   ★ 换的是**一块新缓冲**（不是把旧那块 resize）⇒ 旧容量不可能被复用；
    //     容量观察口取的是这块新缓冲的**真实容量**（守卫据此判"从 N 降到 M 之后容量 = M"）。
    std::vector<TouchPoint> rebuilt(points.size());
    touch_buffer_ = std::move(rebuilt);
    touch_buffer_capacity_ = static_cast<std::int32_t>(touch_buffer_.capacity());
    for (std::size_t i = 0; i < points.size(); ++i) touch_buffer_[i] = points[i];  // ④ 读入
    touch_count_ = static_cast<std::int32_t>(points.size());
    ++touch_rebuilds_;
}

void Machine::op_2fc(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 取一个触点（CORE-INPUT-TOUCH-POINT）：输入可用标志为 0 / 没有有效触点 ⇒ 只写结果 0，
    // 坐标 / 标志 / 附加量**不写**（⛔ 不许清 0：那会把"没有触点"与"触点在原点"混为一谈）。
    if (!input_available_ || touch_count_ <= 0) {
        write_int(ops[0], 0);
        return;
    }
    // "有效"只看标志字的两位（**与**关系）：「抬起」位必须为 0、且「主触点」位必须为 1。
    constexpr std::int32_t kTouchFlagLifted = 0x4;
    constexpr std::int32_t kTouchFlagPrimary = 0x10;
    std::int32_t found = -1;
    for (std::int32_t i = 0; i < touch_count_ && i < static_cast<std::int32_t>(touch_buffer_.size()); ++i) {
        const auto& point = touch_buffer_[static_cast<std::size_t>(i)];
        if ((point.flags & kTouchFlagLifted) != 0) continue;
        if ((point.flags & kTouchFlagPrimary) == 0) continue;
        found = i;
        break;  // 多个有效触点 ⇒ 取**缓冲顺序第一个**（不按编号排、不保证最新）
    }
    if (found < 0) {
        // 缓冲里**全部无效** ⇒ 引擎把触点计数清 0（下一次仍返回"没有触点"）。
        touch_count_ = 0;
        write_int(ops[0], 0);
        return;
    }
    const auto& point = touch_buffer_[static_cast<std::size_t>(found)];
    // 坐标：平台给的是**百分之一像素** ⇒ 先 ÷100（向零截断），再交客户区 / 显示换算（平台接缝）。
    std::int32_t x = point.x / 100;
    std::int32_t y = point.y / 100;
    if (touch_client_mapping_) touch_client_mapping_(x, y);
    write_int(ops[0], 1);
    write_int(ops[1], x);
    write_int(ops[2], y);
    write_int(ops[3], point.flags);  // 第 4 个附加字段 = 该项的**标志字**（原样）
    write_int(ops[4], point.extra);  // 第 5 个 = 第二个附加量（原样）
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
