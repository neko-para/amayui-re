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
    // exit（`CORE-OPS-FRAME-EXIT`）：按**当前帧记录的父帧号**分三支。本条不写长度槽。
    if (f.parent >= 0) {
        // 子脚本结束：当前帧号 ← 记录里的父帧号（不是"弹栈"）；父帧 ip 仍停在调用点。
        current_frame_ = static_cast<std::size_t>(f.parent);
        return;
    }
    if (f.parent == kPumpParentSentinel) {
        // 泵帧收场（`CORE-OPS-FRAME-EXIT` 的 −10 支 / `70-vm.md:312`）：五件事，**顺序固定**。
        // ★ 泵帧**不释放**自己的槽（释放判据是"父帧号 + 1 = 当前帧号"，−10 不满足）。
        if (suspended_frame_ < 0) {
            // 本模型里"−10 父帧但没有挂起帧"只能是内部状态不一致 ⇒ 不猜落点，记诊断并停。
            halted_ = true;
            stop_.reason = StopReason::Error;
            stop_.message = "frame-exit-suspended-missing";
            return;
        }
        // ③ 要取**活动字**的值（不是副本）：切帧之前先留下（①② 都不改状态选择字）。
        const std::uint32_t active = state_flags_;
        // ① 当前帧号 ← 挂起时记住的帧号
        current_frame_ = static_cast<std::size_t>(suspended_frame_);
        // ② 那一帧的长度槽 ← 0（⇒ 分派器不推进，它从**当时**的位置继续）
        frame_mut().length_slot = 0;
        // ③ 状态选择字 ← 待恢复副本；再做两处修正：第 14 位取**活动字**的值（不取副本）、第 21 位清 0
        state_flags_ = status_restore_;
        state_flags_ = (state_flags_ & ~(1u << 14)) | (active & (1u << 14));
        state_flags_ &= ~(1u << 21);
        // ④ 队列泵占用标记 ← 0
        pump_busy_ = false;
        // ⑤ 队列非空 ⇒ **立即再泵一条**（链式）；空 ⇒ 回主循环。
        //   ★ 链式只发生在泵帧收场这一步，⛔ 不是"在取命令的地方循环取空队列"（`70-vm.md:336`）。
        //   ★ 第二趟会覆盖挂起帧号与待恢复副本两格 —— 嵌套泵怎么收场未取证（`RES-VM-PUMP-NEST`）。
        if (queue_occupied()) pump_command_queue();
        return;
    }
    // 父帧号 = −1 ⇒ 整个执行结束；= −11 ⇒ 存档 / 读档专用入口（后续尚无合同，残差
    // `RES-FRAME-EXIT-SAVE`）；**其余负值走同一条退出路径** —— 引擎不区分它们，没有"错误"这一档。
    halted_ = true;
    stop_.reason = StopReason::Halted;
    stop_.message = "root-exit";
    return;
}

void Machine::op_03(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // call-script：父帧挂起，子帧 = 父帧号 + 1（新记录由**同一条装载路径**建出）。
    if (install_ == nullptr) throw CoreError("call-script-loader-missing");
    if (current_frame_ >= kFrameCount - 1) throw CoreError("frame-depth-limit");
    const auto file_id = read_int(ops[0]);
    Frame& parent = frame_mut();
    parent.position = static_cast<std::uint32_t>(parent.ip / 4);
    parent.length_slot = 3;
    // `0x41c724` 写父帧的「调用点位置」；随后按规范在**装帧之前**把「父帧候选」格写成
    // "调用发生时正在执行的帧号" —— 装载器只照抄它（⛔ 它不自己判断父帧号）。
    parent_candidate_ = static_cast<std::int32_t>(current_frame_);
    const std::size_t child_index = current_frame_ + 1;
    install_script_into(child_index, static_cast<std::uint32_t>(file_id));
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
    // `0x41c84e`：本条在调用装载器**之前**把「父帧候选」格写成装载时正在执行的帧号
    // （装载器只照抄 ⇒ 泵先写 −10 就能让泵帧的父帧号 = −10，`0x2` 的 −10 支才可达）。
    parent_candidate_ = static_cast<std::int32_t>(current_frame_);
    if (target < 0 || target >= static_cast<int>(kFrameCount)) throw CoreError("frame-depth-limit");
    install_script_into(static_cast<std::size_t>(target), static_cast<std::uint32_t>(file_id));
    return;
}

void Machine::op_143(const Instruction& instruction) {
    (void)instruction;
    // 事件刷新（`CORE-OPS-EVENT-143`）：四件事，顺序固定。
    // 1. 本帧的推进格数先写成 1（分派器已写过；本条自己再写一次，⛔ 不依赖分派器）。
    write_length(1);
    // 2. 队列泵占用标记 ← 1：借用泵自己"一趟正在跑"用的**同一个标记**，不是另一个格。
    pump_busy_ = true;
    // 3. 对**文件库的 256 格槽位表**逐槽检查（槽号 1..255）：非 0 的槽各投一条命令进待处理命令队列，
    //    投递值 = **槽号 << 24**（32 位有符号）。每次投递自带的"顺手泵一次"尾巴被上面的标记挡下。
    //    ★ 这张表不是本条私有的：文件库装载一个索引对象时按**它自带的区号**登记（`0x455948`），
    //    析构时逐格清（`0x454cd0`）；槽 0（基础索引）本条不检查。
    for (std::size_t index = 1; index < file_library_slots_.size(); ++index) {
        if (file_library_slots_[index] == 0) continue;
        // 格号 ≥ 128 ⇒ 最高位已置 ⇒ 该值按**负命令**解释（`60-ops-scalar.md:181`）；表上界仍是 255。
        const auto command = static_cast<std::int32_t>(static_cast<std::uint32_t>(index) << 24);
        enqueue_command(command);
    }
    // 4. 队列泵占用标记清回 0、推进格数清 0、**自己**把指令指针推进 4 字节，然后**尾跳**进队列泵。
    //    ★ 推进由本条自己做（⛔ 不许留给分派器再加一次 —— 那会让每条 0x143 跳过 8 字节）。
    pump_busy_ = false;
    write_length(0);
    frame_mut().ip += 4;
    // 尾跳的入口就是泵：两判（区间忙 / 空队列）在这一步照常生效 —— 早退是启动链的常见路径。
    pump_command_queue();
}

void Machine::op_05(const Instruction& instruction) {
    (void)instruction;
    Frame& f = frame_mut();
    // ret（`CORE-OPS-FRAME-RET`）：先把长度槽写 1、栈顶 −1，再弹位置。
    write_length(1);
    // 空返回栈（栈顶指针为 0）时引擎仍然减一 ⇒ 读到的是**相邻存储**，不是任何返回点。
    // 这是未定义行为：实现必须拒绝或记诊断，⛔ 不许把空栈当成"返回到 −1"，也不许静默返回下一条。
    if (f.ret_stack.empty()) {
        report_error("ret-empty-stack:frame=" + std::to_string(current_frame_));
        return;
    }
    const auto position = f.ret_stack.back();
    f.ret_stack.pop_back();
    if (!f.call_sites.empty()) f.call_sites.pop_back();
    if (position == -1) return;
    jump_to(static_cast<std::uint32_t>(position));
    // `0x41aa2c`：**真正跳转**的那一路弹「引擎级状态字保存栈」写回状态选择字（门重新关上）。
    // 栈空 ⇒ **不写**状态选择字（`0x41aa35` 的判据）；普通脚本调用/返回不碰这条栈。
    if (!engine_state_word_stack_.empty()) {
        state_flags_ = engine_state_word_stack_.back();
        engine_state_word_stack_.pop_back();
    }
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

void Machine::engine_reset_hook() {
    ++engine_reset_hook_calls_;
    // ① 当前帧的推进格数 ← 1（写的是**执行 `0x9` 的那一帧**；钩子在帧槽全释放之前跑）。
    write_length(1);
    // ② 清状态选择字的第 9 位（音乐音量渐变进行中；`CORE-OPS-FRAME-EXIT-SCRIPT`）。
    state_flags_ &= ~(1u << 9);
    // ③ 复位它自己的一个子系统：规范明说「该子系统的内部行为不在本条」，且本实现里
    //    它的身份与内部效果都未取证 ⇒ **不建模**。⛔ 不造空函数把它"看起来实现了"。
}

void Machine::op_09(const Instruction& instruction) {
    (void)instruction;
    // exit-script（CORE-OPS-FRAME-EXIT-SCRIPT）：**不是返回** —— 丢掉全部帧、复位引擎级状态，
    // 再以统一文件 id 0 重装根脚本、从它的 dword 0 开始执行。调用者永远拿不回控制权。
    // ★ 顺序照站点（`0x428a60`…`0x428d80`，EA 见 `data/spec/bindings.json`）：
    //   ① 启动时登记的重置钩子（`0x428a71`）→ ② show-logo ← 0（`0x428a75`）→
    //   ③ 40 个帧槽全释放 / 帧号 0 / 父帧号 −1 / 状态字 0（`0x428a80` 起）→
    //   ④ 值族与引用族池（`0x428ad9`…`0x428bc4`）→ ⑤ 输入子系统更新（`0x428c0c`）→
    //   ⑥ 绘制模式格 ← 配置 `set:DrawMode`（`0x428c34`）→ ⑦ 纹理槽 1000（`0x428d3d`/`0x428d57`/`0x428d67`）
    //   → ⑧ 以统一文件 id 0 重装根脚本（`0x428d7b`）。
    //   ★ 站点里还有三项本实现没有对应结构 / 宿主接口，**未建模**（S7 已交父级）：
    //     「两个文本子系统 + 数据子系统的复位入口」（`0x428c3a`/`0x428c45`/`0x428c50`：
    //       本实现里这三个子系统没有独立 reset，规范也没写它们的效果 ⇒ 不造空函数）；
    //     「四组向量格按一份基准搬回」（`0x428c5e`…`0x428d23`：本实现没有"基准"概念）；
    //     「模式格为负 ⇒ 向游戏窗口发 0x1400」（`0x428bdb`：本实现没有窗口消息钩子，宿主是 SDL）。
    if (engine_reset_hook_registered_) engine_reset_hook();
    show_logo_ = 0;
    state_flags_ = 0;
    driven_elsewhere_ = false;
    bracket_depth_ = 0;
    for (auto& slot : frames_) slot = Frame{};
    // 只清**当前选中 bank** 的三个值族全局池（`0x6d` 的另一组保留原值）与**三个引用族全局池**
    // （`0x428b94` / `0x428bac` / `0x428bc4` 逐池清 0，同一份 bank 口径）。
    clear_global_pool_bank();
    // 输入子系统执行一次更新（`0x428c0c`）：向平台采样一次并写进输入字（与 `0xff` 的采样同一路径；
    // 没有采样源 ⇒ 保持 0，⛔ 不偷偷补值）。
    input_word_ = key_sampler_ ? key_sampler_() : 0;
    // 场景绘制模式格 ← **重读配置** `set:DrawMode`（`0x428c34`；与 `0x200` 同口径，⛔ 不是沿用构造值）。
    draw_mode_ = config_.get_int("set:DrawMode");
    // 纹理槽：`set:CreateObject` 第 1 位 ∧ `set:AutoFreeTex` 第 0 位 ⇒ 逐槽释放 **1000** 个（0..999）。
    // ★ 每个槽的释放体与 `0x1fa` 同一个（`sub_49E980`），**受释放守卫**；记录本身保留。
    if ((config_.get_int("set:CreateObject") & 2) != 0 && (config_.get_int("set:AutoFreeTex") & 1) != 0) {
        for (std::uint32_t id = 0; id < 1000; ++id) release_texture_slot(id);
    }
    if (install_ == nullptr) throw CoreError("exit-script-loader-missing");
    const Bytes script = read_install_file(*install_, 0u);
    register_loaded_file(0u);  // 文件库装载登记（按区号；`0x143` 的投递源；`0x455948`）
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
    driven_elsewhere_ = true;  // 置忙（早于取时钟）
    // 不忙 ⇒ 读一次宿主毫秒时钟，写**场景时钟对**（`CORE-TIMING-CLOCK` 的第 1 个门后写者：
    // `0x41A0CF` 搬旧 / `0x41A0D5` 写新）。它与 `0x20c` / `0x23c` / 门前渲染步是**同一对** ——
    // ⛔ 不是另一对"引擎时刻"，否则 (c) 一落时间就停走（`70-vm.md:217` 的作用域）。
    stamp_scene_clock(host_now_ms());
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
    // 清忙之后：**队列泵占用标记为 0** ⇒ 把控制交给待处理命令队列的泵；否则直接返回
    // （泵帧运行期间这一步不许开闸 —— 否则出口会把队列套成两层）。
    if (!pump_busy_) pump_command_queue();
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
    // ★ 引擎尾段还清四对"从未写入有效登记"的格 + 一个回调派发闩 ⇒ 本仓 `audit` 判 **discarded**
    //   （`RES-LOCAL-RET-SLOTS` / `RES-LOCAL-RET-PENDING`：没有有效写者，清 0 不可达）⇒ 不建模。
    return;
}

void Machine::service_local_input_wait() {
    // `CORE-OPS-FRAME-LOCAL-RET-7C` 的「第 26 位 = 本地块内的输入等待」（`0x412a1f`–`0x412ac8`）。
    // ① 每迭代向输入子系统取一次输入字；为 0 ⇒ **第 26 位不清**（门继续关着，脚本停在原地等输入）。
    if (input_word_ == 0) return;
    // ② 非 0 ⇒ 清第 26 位、输入字 ← 0（两支都清）。
    state_flags_ &= ~(1u << 26);
    input_word_ = 0;
    Frame& f = frame_mut();
    // ③ 当前帧**没登记**副进入目标 ⇒ 只清第 26 位，脚本从**返回点**继续（ip 本来就在那里）。
    if (f.local_sub == -1) return;
    // ④ 登记过 ⇒ 记下"回来时用的状态"并按副目标重入（`0x412a75` / `0x412a89` / `0x412ac8`）：
    //    存档字 ← 状态选择字 | 第 25 位；状态选择字 ← 0（门打开）；
    //    返回点 ← **当前指令的 dword 位置**（不是指令指针本身）；入块深度 ← 本帧身份编号；
    //    ip ← 代码区起点 + 4 × 副目标；推进格数 ← 0。
    local_save_word_ = state_flags_ | (1u << 25);
    state_flags_ = 0;
    local_save_pos_ = static_cast<std::uint32_t>(f.ip / 4);
    local_save_depth_ = f.local_depth;
    jump_to(static_cast<std::uint32_t>(f.local_sub));
    return;
}

}  // namespace core
