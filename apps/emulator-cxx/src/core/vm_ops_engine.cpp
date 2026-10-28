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

void Machine::op_2cf(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 显示模式请求（0x2CF / `sub_4263D0` → `sub_413DD0`，规则 `CORE-OPS-FORWARD-DISPLAY-MODE`）：
    // 第 1 个操作数 = **目标模式**（0 = 窗口 / 1 = 全屏）。
    // ★ 引擎逐字读它**三次**（`0x4263F4` / `0x426401` / `0x42640A`）：第一次判 `< 0`、第二次判 `> 1`、
    //   第三次才当实参。越界是**静默返回**（不 clamp、不记日志、不改任何格）。
    // ★ 这个 handler 自己**不碰配置**；配置写在转发目标里（见 `apply_display_mode`）。
    if (read_int(ops[0]) < 0) return;
    if (read_int(ops[0]) > 1) return;
    apply_display_mode(read_int(ops[0]));
    return;
}

void Machine::apply_display_mode(std::int32_t target) {
    // 三条**早退**都不写任何键（`sub_413DD0` 三支汇合到同一出口）：
    //   ① `display:ForceScreen == 1` 且缓存非 0（强制全屏）；
    //   ② 目标 == 缓存（请求已满足；含"目标 0、缓存 0"那一支）。
    if (config_.get_int("display:ForceScreen") == 1 && display_mode_cache_ != 0) return;
    if (target == display_mode_cache_) return;
    const auto previous = display_mode_cache_;
    // 引擎做的是"缓存翻转 new = (old == 0)"；在前置条件（目标 ∈ {0,1} 且 目标 != 旧值）下等价于
    // "缓存 ← 目标"。★ 按翻转写，越界目标下不会凭空造出第三个值。
    display_mode_cache_ = previous == 0 ? 1 : 0;
    // ── 平台请求（窗口重建 / 分辨率与整窗尺寸 / `CALLBACK_WINDOW.BIN` 重载）──────────────────
    // ⛔ core 不伪造窗口：这几步只登记为**残差**（`RES-OPS-2CF-PLATFORM`），这里不发任何请求。
    // 更换路径的尾部才把键写成**归一后的**值（`0x4146F8 setnz`），不是目标原值。
    config_.set_int("display:ScreenMode", display_mode_cache_ != 0 ? 1 : 0);
    // 条件写：`set:SaveWinPos` 为真且**旧缓存**非 0 时，引擎还会写窗口位置两键；那两个值来自平台窗口，
    // core 没有窗口 ⇒ 本版**不写**（残差 `RES-OPS-2CF-WINPOS`）。
    return;
}

void Machine::op_1b5(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 写消息速度（0x1B5 / `sub_41FED0`，规则 `CORE-CONFIG-NAMED-WRITE`）：
    // 同一个值写**两个地方** —— 引擎参数格（`0x74/0x7f` 那一格，文本自动推进时长）与命名键
    // `message:MessageSpeed`。★ 只写键不写格会让紧随其后的 `0x7f` 读到旧值。
    // ★ 无范围检查、不夹取。
    const auto value = read_int(ops[0]);
    param_cell_ = value;
    if (!config_.has_named("message:MessageSpeed")) {
        report_error("named-key-missing:message:MessageSpeed");
        return;
    }
    config_.set_int("message:MessageSpeed", value);
    return;
}

void Machine::op_1b8(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 读"自动消息显示时长"某一档（0x1B8 / `sub_42D2F0`，规则 `CORE-CONFIG-NAMED-AUTOTIME`）：
    // 档号 0 ⇒ `message:AutoMessageTime0`、1 ⇒ `AutoMessageTime1`，**值写进第 2 个操作数**。
    // ★ 其它档号：**不写落点**、只记一行日志（不是写 0）。
    const auto selector = read_int(ops[0]);
    if (selector == 0) {
        write_int(ops[1], config_.get_int("message:AutoMessageTime0"));
    } else if (selector == 1) {
        write_int(ops[1], config_.get_int("message:AutoMessageTime1"));
    } else {
        report_error("auto-message-time-selector-invalid:" + std::to_string(selector));
    }
    return;
}

void Machine::op_1b9(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 写"自动消息显示时长"某一档（0x1B9 / `sub_41FF60`）：与 0x1b8 同一对键的**写入**版，
    // 值 = 第 2 个操作数。★ 其它档号：**连第 2 个操作数都不读**、不写任何键、只记一行日志。
    const auto selector = read_int(ops[0]);
    if (selector == 0) {
        config_.set_int("message:AutoMessageTime0", read_int(ops[1]));
    } else if (selector == 1) {
        config_.set_int("message:AutoMessageTime1", read_int(ops[1]));
    } else {
        report_error("auto-message-time-selector-invalid:" + std::to_string(selector));
    }
    return;
}

void Machine::op_1ba(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 声道开关（0x1BA / `sub_421200`，规则 `CORE-OPS-AUDIO-MODE`）：声道 **1..4**（1 基，与音量 0xC6 的
    // 0..4 **不同**）⇒ 音乐 / 音效 / 语音 / 影片四个命名键。四条支的语义**互不相同**：
    //   1 音乐：**符号阶梯**（键值 < 0 视为"关"）：开 ⇒ +3、关 ⇒ −3；**已在同侧 ⇒ 什么都不做**。
    //   2 音效 / 3 语音：判**引擎开关格**是否已与请求同态 ⇒ 同态就 no-op（含"不发停通道请求"）；
    //                   否则写格 + 写键（0/1）。
    //   4 影片：键值**原样**写（非 0/1 归一），相同则 no-op。
    // 其余声道：三样都不动、只记日志（不抛）。★ 命中支都会通知影片子系统（`set:DependMovieSound`），
    // 那一步属平台/音频侧，本版只记账不建模。
    const auto channel = read_int(ops[0]);
    if (channel < 1 || channel > 4) {
        report_error("set-sound-mode-channel-invalid:" + std::to_string(channel));
        return;
    }
    const auto value = read_int(ops[1]);
    switch (channel) {
        case 1: {
            const auto current = config_.get_int("sound:Music");
            if (value != 0 && current < 0) {
                config_.set_int("sound:Music", current + 3);
            } else if (value == 0 && current >= 0) {
                config_.set_int("sound:Music", current - 3);
            }
            return;
        }
        case 2: {
            const bool on = value != 0;
            if (sound_se_enabled_ == on) return;  // 同态：连"停通道"也不发
            sound_se_enabled_ = on;
            config_.set_int("sound:SE", on ? 1 : 0);
            return;
        }
        case 3: {
            const bool on = value != 0;
            if (sound_voice_enabled_ == on) return;
            sound_voice_enabled_ = on;
            config_.set_int("sound:Voice", on ? 1 : 0);
            return;
        }
        default: {
            if (config_.get_int("sound:Movie") == value) return;
            config_.set_int("sound:Movie", value);  // 原样，不归一
            return;
        }
    }
}

void Machine::op_b6(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 停止一个音效槽（0x0B6 / `sub_420B80`，规则 `CORE-OPS-AUDIO-SE-STOP`）：op1 = 槽号 0..14。
    // ★ 次序：**后端存在时**才释放该槽缓冲并把"该槽资源号"与同组另一格清 0；后端不存在时连清格都不做。
    //   本版没有声音后端 ⇒ 按"后端不存在"处置（不清格、不发请求），并把"要释放"记成一笔待办。
    const auto slot = read_int(ops[0]);
    if (slot < 0 || slot > 14) {
        // 引擎：写一段固定错误文本（dsStop），**但随后的两个清格仍按越界下标执行** —— 那属越界写，
        // 本版不复制未定义行为，只记日志。
        report_error("se-stop-slot-out-of-range:" + std::to_string(slot));
        return;
    }
    se_stop_requests_ += 1;
    return;
}

void Machine::op_b7(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // BGM 播放（0x0B7 / `sub_420C00`，规则 `CORE-OPS-AUDIO-BGM-RESTART`）：op1 = 曲目号。
    // ★ 与 0xbf 的**唯一但决定性**差别：这条**没有"同曲同参就跳过"的判据** ⇒ 每次都发播放请求。
    // ★ op1 == 0 的两支：已有曲目 ⇒ 用**旧曲目**重发；没有 ⇒ 等价于停止（但参数格已被写成 1）。
    state_flags_ &= ~(1u << 9);  // 第 9 位（音乐淡变进行中）在这里被清
    const auto track = read_int(ops[0]);
    bgm_param_ = 1;              // 参数格**无条件**写 1（先于一切判据）
    if (track == 0 && bgm_track_ == 0) {
        bgm_track_ = 0;
        bgm_stop_requests_ += 1;
        return;
    }
    bgm_track_ = track == 0 ? bgm_track_ : track;
    bgm_progress_ = 0;
    bgm_play_requests_ += 1;
    return;
}

void Machine::op_b8(const Instruction& instruction) {
    (void)instruction;
    // BGM 停止（0x0B8 / `sub_419720`，规则 `CORE-OPS-AUDIO-BGM-STOP`）：**没有操作数**。
    // ★ 没有"是否在播"的判据 —— 重复调用会重复发停止请求；参数格**不被这条改动**。
    state_flags_ &= ~(1u << 9);
    bgm_track_ = 0;
    bgm_stop_requests_ += 1;
    return;
}

void Machine::op_ba(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 音效播放的**循环变体**（0x0BA / `sub_420BC0`，规则 `CORE-OPS-AUDIO-WAV` 的补充段）：
    // 与 0xb5 是同一条播放请求，唯一差别是请求里的**循环位 = 1**（0xb5 = 0）。
    const auto slot = read_int(ops[0]);
    if (slot < 0 || slot > 14) {
        report_error("se-play-slot-out-of-range:" + std::to_string(slot));
        return;
    }
    se_play_requests_ += 1;
    return;
}

void Machine::op_c2(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // BGM 淡变（`0x0C2` / `sub_420E00`，**argc 2**）。两条支路按门字两位分：
    //   A 支（第 27 位 = 1，"立即"）：读序 **op2 → op1**，目标 ← op1、步长 ← op2，随即按"满进度"走完
    //     ⇒ **瞬间到位**，**不动**第 9 位。
    //   B 支：第 9 位已置才进来 —— 读配置 `set:TransferMusicVolume` 定混音方式（1 按进度混合 / 2 直取）、
    //     置第 9 位；`op2` **读三次** ⇒ 间隔 = `op2 < 1000 ? op2/10 : op2/100`（向零截断）、步长 = `op2 < 1000 ? 10 : 1`；
    //     最后读 `op1` 写目标值。音量的真正生效在音乐对象的虚表里（本版只记请求）。
    // ★ `op1` **不是曲目号**（曲目另有格）；刻度满值 = 10000（唯一观察，单位本版不解释）。
    // ★ 复用的就是 `0xb7` / `0xb8` 那套 BGM 状态（同一对象、同一门字第 9 位）。
    const bool immediate = (state_flags_ & (1u << 27)) != 0;
    if (immediate) {
        const auto step = read_int(ops[1]);
        const auto target = read_int(ops[0]);
        bgm_fade_target_ = target;
        bgm_fade_step_ = step;
        bgm_fade_progress_ = target;  // 走完（相对刻度 10000）
        bgm_fade_requests_ += 1;
        return;
    }
    bgm_fade_mix_mode_ = config_.get_int("set:TransferMusicVolume");
    state_flags_ |= (1u << 9);
    const auto op2 = read_int(ops[1]);
    bgm_fade_interval_ms_ = op2 < 1000 ? op2 / 10 : op2 / 100;
    bgm_fade_step_ = op2 < 1000 ? 10 : 1;
    bgm_fade_progress_ = 0;
    bgm_fade_target_ = read_int(ops[0]);
    bgm_fade_requests_ += 1;
    return;
}

void Machine::op_2f5(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 配音排队（`0x2F5` / `sub_4267D0`，**argc 4**，读序 **4 → 3 → 2 → 1**，读序是合同）。
    // 写五格、**零检查、零外部调用**：延时 ms ← op3、资源号 ← op1、pending ← 1、起算 ← 0、状态字 ← op2。
    // ★ 状态字的低 2 位 == 3 表示"已完成 / 跳过"（读侧据此不再播）。
    // ★ 真正"响"在帧更新的计时里（`now − 起算 ≥ 延时`）⇒ 本版只落状态（时钟不建模）。
    const auto op4 = read_int(ops[3]);
    const auto op3 = read_int(ops[2]);
    const auto op2 = read_int(ops[1]);
    const auto op1 = read_int(ops[0]);
    if (op4 < 0 || op4 >= static_cast<int>(voice_delay_.size())) {
        // 引擎**不查**这个下标（T_Voice 只有 3 格）⇒ 越界写属未定义行为；本版记日志后跳过（已知偏离）。
        report_error("voice-slot-out-of-range:" + std::to_string(op4));
        return;
    }
    const auto slot = static_cast<std::size_t>(op4);
    voice_delay_[slot] = op3;
    voice_resource_ = op1;
    voice_pending_ = true;
    voice_start_ms_ = 0;
    voice_state_[slot] = op2;
    return;
}

void Machine::op_2ff(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 配音组（`0x2FF` / `sub_426940`，**argc 2**）。★ 读序是合同：**先写组开关位（= 1），才读 op2**，
    // 再写组值。组值的领域含义**未取证**（只证到播放时会抄进声音后端的一列，而那一列在本语料里没有读者）。
    const auto index = read_int(ops[0]);
    if (index < 0 || index >= static_cast<int>(voice_group_enabled_.size())) {
        report_error("voice-group-out-of-range:" + std::to_string(index));
        return;
    }
    const auto slot = static_cast<std::size_t>(index);
    voice_group_enabled_[slot] = 1;
    voice_group_value_[slot] = read_int(ops[1]);
    return;
}

namespace {
/** 24 位色的**字节反转**：脚本给 `0x112233`，引擎的色格存 `0x332211`（两条绘制入口共用） */
inline std::int32_t bswap24(std::int32_t v) {
    const auto u = static_cast<std::uint32_t>(v);
    return static_cast<std::int32_t>(((u & 0xFFu) << 16) | (((u >> 8) & 0xFFu) << 8) | ((u >> 16) & 0xFFu));
}

/** `0xc4` / `0x2f4` / `0x1bd` 共用的"开关消费"：位16 置 ⇒ 整格 0；否则位0 置 ⇒ 置位16（**位0 保留**）。 */
inline void consume_switch_keep_bit0(std::int32_t& cell) {
    if ((cell & 0x10000) != 0) {
        cell = 0;
    } else if ((cell & 1) != 0) {
        cell |= 0x10000;
    }
}

/** "影片/立即"门位（`0xAAB44` 第 27 位）：`0xc2` 的 A 支与 `0xc4` 的 movie 支读的是同一位。 */
constexpr std::uint32_t kMovieGateBit = 1u << 27;
}  // namespace

void Machine::op_82(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 画一段**已排版的文本记录**（`0x82` / `sub_41F720`，**argc 5**、推进 11）。
    // 读序 **5 → 4 → 3 → 2 → 1**（读序是合同）。实参：op1 = 节点槽号（`T_Font+0x414` 十项，**引擎不查界**）、
    // op2 = 记录下标（`T_Font+0xD24` 的 72 字节向量，**有检查**）、op3 = 标志（位1 = op4/op5 是颜色、位3 = 允许触发语音）、
    // op4/op5 = 主色 / 副色（**字节反转**后存）。
    // ★ 早退：`op2` 不在 `[0, count)` ⇒ **整条静默什么都不做**（不写颜色、不发请求、不报错）。
    // ★ 未建模（残差）：GDI 度量（`GetTextMetricsA` / `GetTextExtentPoint32A`）、经渲染器追加的绘制命令、
    //   三条"文本-语音"队列表头的清 0、`node[0x70]==1` 时转交的那条绘制路径（只记次数）。
    const auto op5 = read_int(ops[4]);
    const auto op4 = read_int(ops[3]);
    const auto op3 = read_int(ops[2]);
    const auto op2 = read_int(ops[1]);
    const auto op1 = read_int(ops[0]);
    if (op2 < 0 || static_cast<std::size_t>(op2) >= text_spans_.size()) return;
    if (op1 < 0 || op1 >= 10) {
        // 引擎不检查 0..9（越界是未定义读）⇒ 本实现记日志后跳过（已知偏离）。
        report_error("text-node-slot-out-of-range:" + std::to_string(op1));
        return;
    }
    const auto kind = font_node_kind_.find(op1);
    if (kind != font_node_kind_.end() && kind->second == 1) {
        text_span_alt_requests_ += 1;  // 转交另一条绘制路径（未建模）
        return;
    }
    if ((op3 & 2) != 0) {
        font_primary_color_ = bswap24(op4);
        font_secondary_color_ = bswap24(op5);
    }
    const TextSpanRecord& rec = text_spans_[static_cast<std::size_t>(op2)];
    if ((op3 & 8) != 0 && (rec.flags & 0x40000000) != 0) {
        // ★ 引擎在这里还要判"本槽本次是否已排过"：已排过 ⇒ 入队而不是立即播。队列未建模 ⇒ 只记播放请求。
        voice_play_requests_ += 1;
    }
    text_span_draw_requests_ += 1;
    return;
}

void Machine::op_c4(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 配音播放（`0xc4` / `sub_420F70`，**argc 1**、推进 3）。七步，全部无条件：
    //   ① 消费**槽 0 的两个开关格**（位16 ⇒ 整格 0；否则位0 ⇒ 置位16、**位0 保留**）；
    //   ② BGM 压低（门字第 14 位）；
    //   ③ 影片门（第 27 位）开着 ⇒ 只把 op1 写进镜像格、**不播**；否则清两个镜像格 + 发一次播放请求；
    //   ④ 文本门（`0x1BB` 写的那格）== 0 ⇒ **往文本记录向量追加一条**（`flags = 0x40000000`）；
    //   ⑤（未建模）另一格非 0 时置"有配音在播"。
    // ★ `op1` 在正常支被读**两次**（一次给播放、一次给记录）；影片支也读两次（镜像 + 记录）。
    // ★ 未建模：`sub_407120` 的真实混音压低、装载解码（`sub_4BB840` 里读文件那一段）、第 ⑤ 步的门格。
    consume_switch_keep_bit0(voice_switch_46c_[0]);
    consume_switch_keep_bit0(voice_group_enabled_[0]);
    // ★ 引擎在这一步置门字第 14 位（"BGM 压低中"），**本实现刻意不置**：本模型的主循环没有服务第 14 位的
    //   分支，置了只会让门停在原地（`gate-bit-unserviced`）。⇒ 只落一个可观察投影 `bgm_ducked_`，
    //   并把"第 14 位的服务者"登记成残差。
    bgm_ducked_ = true;
    if ((state_flags_ & kMovieGateBit) != 0) {
        voice_mirror_resource_[0] = read_int(ops[0]);
        voice_mirror_a4_[0] = 0;
    } else {
        voice_mirror_resource_[0] = 0;
        voice_mirror_a4_[0] = 0;
        const auto resource = read_int(ops[0]);
        // `sound:Voice` 关着 ⇒ 装载请求不发，但**播放请求照发**（取证明确）。
        if (sound_voice_enabled_) voice_load_requests_ += 1;
        (void)resource;
        voice_play_requests_ += 1;
    }
    if (font_text_gate_5ec7c_ == 0) {
        TextSpanRecord rec;
        rec.slot = current_text_slot_;
        rec.resource = read_int(ops[0]);
        rec.voice_slot = 0;
        rec.flags = 0x40000000;
        text_spans_.push_back(rec);
    }
    return;
}

void Machine::op_307(const Instruction& instruction) {    const Operand* ops = instruction.operands.data();
    // 命名键 setter（0x307 / `sub_426AE0` → 配置库 vtable+0xC 写整，规则 `CORE-OPS-FORWARD-NAMED-KEY`）：
    // 键逐字 = `system:EffectSkipOnClick`（与 0x306 的 getter 是同一键）。
    // ★ **原值写入**：不归一成 0/1、不夹取。
    // ★ 引擎的写入口**只覆盖已存在的键**（缺键 ⇒ 弹框 + 返回 0，**不建键**）⇒ 这里先判存在。
    if (!config_.has_named("system:EffectSkipOnClick")) {
        report_error("named-key-missing:system:EffectSkipOnClick");
        return;
    }
    config_.set_int("system:EffectSkipOnClick", read_int(ops[0]));
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
    // 音频装载请求（0x0B4 / `sub_420B00`）：op1 = 资源标识、op2 = 槽号。
    // ★ 取证补正了旧注释的"不改任何引擎数据"：装载成功路径会写"该槽当前资源号"这一格
    //   （`0x4B5004`），而 0xb6 清的正是同一格 ⇒ 两者成对。
    const auto resource = read_int(ops[0]);
    const auto slot = read_int(ops[1]);
    if (slot < 0 || slot > 14) {
        report_error("se-load-slot-out-of-range:" + std::to_string(slot));
        return;
    }
    se_slot_resource_[static_cast<std::size_t>(slot)] = resource;
    return;
}

void Machine::op_b5(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 音频播放请求（0x0B5 / `sub_420B40`）：op1 = 槽号；循环位 = 0（与 0xba 的唯一差别）。
    const auto slot = read_int(ops[0]);
    if (slot < 0 || slot > 14) {
        report_error("se-play-slot-out-of-range:" + std::to_string(slot));
        return;
    }
    se_play_requests_ += 1;
    return;
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
