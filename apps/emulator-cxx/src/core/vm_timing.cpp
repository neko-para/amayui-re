// 场景计时窗的推进（70-vm §时钟 / CORE-TIMING-WINDOW）：颜色窗与源矩形窗共用一次盖章。
//
// ★ `step()` 在门之前调用它；它只改派生态（当前颜色、四条边）与"有动画 / 待处理"两个标记，
//   不判脏、不请求渲染 —— 那些是主循环的事。
// ★ "待处理"闩（`+0B5B4`）的生命周期与"有动画"（`+0B5AC` 的派生态）**不是同一格**：
//   排窗指令（`0x202` / `0x323` / `0x239` / `0x220` / `0x21e` / `0x21f`）直接置它，
//   这里在"窗还在走"的每一步再置一次；**清 0 就是本函数的开头**（引擎 0x4B40B0 在推进之前清）。
#include "core/vm.h"

#include "core/color.h"

namespace core {

void Machine::service_bgm_fade() {
    // ★ 残差（`RES-AUDIO-BGM-FADE-SERVICE`）：**规范没有规定状态字第 9 位的服务分支**
    //   （60-ops-voice 的残差只登记"别处会清它"）。这里是**观测驱动的最小实现**：
    //   按 `0xC2` 自己算出的间隔（毫秒）与步长推进，到位即清第 9 位（"淡变进行中"结束）。
    //   ⛔ 不声称与引擎逐位一致；主循环取证后再校。
    if ((state_flags_ & (1u << 9)) == 0) {
        bgm_fade_last_ms_ = 0;
        return;
    }
    const std::uint32_t now = scene_clock_ms_;
    if (bgm_fade_last_ms_ == 0) {
        bgm_fade_last_ms_ = now;
        return;
    }
    const auto interval = bgm_fade_interval_ms_ > 0 ? static_cast<std::uint32_t>(bgm_fade_interval_ms_) : 1u;
    auto step = bgm_fade_step_;
    if (step <= 0) step = 1;
    while (now >= bgm_fade_last_ms_ + interval) {
        bgm_fade_last_ms_ += interval;
        if (bgm_fade_progress_ < bgm_fade_target_) {
            bgm_fade_progress_ = std::min(bgm_fade_target_, bgm_fade_progress_ + step);
        } else if (bgm_fade_progress_ > bgm_fade_target_) {
            bgm_fade_progress_ = std::max(bgm_fade_target_, bgm_fade_progress_ - step);
        }
        if (bgm_fade_progress_ == bgm_fade_target_) break;
    }
    if (bgm_fade_progress_ == bgm_fade_target_) {
        state_flags_ &= ~(1u << 9);  // "淡变进行中"结束
        bgm_fade_last_ms_ = 0;
    }
}

// ── 时间交付：**两套到期计时器**（`CORE-VM-LOOP` 的「一次迭代做哪些事」）──
// ★ 交付**不是混音**：它只推进引擎自己的到期记录，并把「到期」交给该子系统自己的释放 / 启动入口；
//   混音 / 输出 / 音量属音频子系统与平台（`70-vm.md:23` 的范围声明）⇒ 这里**一处宿主请求都不发**。
void Machine::deliver_sound_time(std::uint32_t now_ms) {
    // 入口判据：「无待办 ⇒ 直接返回」（`0x4b5239`：`cmp [ebx+4B8h],0`）。没有这一格，「未到期 ⇒
    // 重新置待办」就没有读者，10 个通道会每迭代被无谓地扫一遍。
    if (!se_work_pending_) return;
    // 「待办」是**单格**（`T_SE+0x4B8`），不是每通道一格：进入时清 0（`0x4b524a mov [ebx+4B8h],eax`），
    // 后面**任一**通道未到期时再置 1（`0x4b5296`）。⇒ 全部到期的那一趟结束时它保持 0。
    se_work_pending_ = false;
    for (auto& channel : se_channels_) {
        if (!channel.active) continue;
        // 懒盖章：**只判起点 = 0**（`0x4b526b`）。★ 0 是"未盖章"哨兵 ⇒ 盖章只发生一次，⛔ 不是每迭代都盖。
        if (channel.start == 0) channel.start = now_ms;
        // 到期判据 = **有符号**「读数 − 起点 ≥ 时长」（`0x4b5275` 的比较；`70-vm.md` 的「时钟」条明说
        // 音效 / 语音的到期是有符号，⛔ 不许并进颜色窗那一族）。⇒ 相等已到期、差 1 未到期，
        // 起点落在"未来"时差为负 ⇒ 未到期。
        const auto elapsed = static_cast<std::int32_t>(now_ms - channel.start);
        if (elapsed >= channel.duration) {
            // 到期 ⇒ 清活动，并把该通道的值交给**释放入口**（子系统内部的步骤，⛔ 不是宿主请求）。
            channel.active = false;
            se_last_released_value_ = channel.value;
            ++se_release_steps_;
        } else {
            se_work_pending_ = true;  // 未到期 ⇒ 重新置「仍有一件未完成的工作」（`0x4b5296`）
        }
    }
}

bool Machine::deliver_voice_time(std::uint32_t now_ms) {
    bool advanced = false;
    for (auto& slot : voice_slot_timers_) {
        if (!slot.pending) continue;
        // ★ 状态低两位 = 3（"已完成 / 跳过"，`60-ops-voice.md:130`）的槽，spec 要求**先按另外两个语音槽
        //   的占用重排**再进懒盖章 + 到期（`70-vm.md:20`）。那一步与「启动该槽队列里的下一段」的判据
        //   在 spec 的「配音排队与配音组」里**没有正文**（该条只规定 `0x2f5` 写五格）⇒ 本实现**不做**
        //   （⛔ 不拍近似值）：这样的槽按"没有在播的段"跳过 —— 不盖章、不到期、不清状态。
        if ((slot.state & 3) == 3) continue;
        if (slot.start == 0) slot.start = now_ms;  // 懒盖章（只判起点 = 0）
        const auto elapsed = static_cast<std::int32_t>(now_ms - slot.start);
        if (elapsed >= slot.duration) {  // 有符号，与音效通道**同一形状**
            // 到期 ⇒ 清槽状态；「按该槽的待办标记调整 / 启动下一段」的判据未成篇 ⇒ 只清这一段的待办。
            slot.state = 0;
            slot.pending = false;
            slot.start = 0;
            ++voice_advance_steps_;
            advanced = true;  // `0x4bbc01`：启动了语音 ⇒ 交付返回「有推进」
        } else {
            voice_work_pending_ = true;  // 还有没到期的段 ⇒「该子系统仍有一件未完成的工作」
        }
    }
    if (advanced) request_bgm_duck();
    return advanced;
}

void Machine::request_bgm_duck() {
    // 音乐压低的**进入条件**（`70-vm.md:21`；四个都成立才置位，否则**什么都不做**）：
    //   ①「音乐淡出相关状态」格（`0xc0` 读的那一格；写者 `0xc3` 未实现）非 0；
    //   ② 配置 `sound:MusicFadeOnVoicePlaying` 非 0；
    //   ③ 状态选择字第 9 位未置（音乐音量渐变进行中）；
    //   ④ 主循环模式字（`0x24e` 写的那个字）的第 0 位为 0。
    if (engine_aa9e4_ == 0) return;
    if (config_.get_int("sound:MusicFadeOnVoicePlaying") == 0) return;
    if ((state_flags_ & (1u << 9)) != 0) return;
    if ((input_mode_ & 1u) != 0) return;
    state_flags_ |= (1u << 14);  // 写回状态选择字（`0x40716c`）
    voice_work_pending_ = true;  // 「该子系统仍有一件未完成的工作」置起（`70-vm.md:21`）
}

void Machine::service_bgm_duck() {
    // 位序表第 3 行（`70-vm.md:112`）：前置 = 语音子系统空闲 ∧「该子系统仍有一件未完成的工作」≠ 0
    // ∧ 第 9 位未置；**不成立 ⇒ 第 14 位保留、下个迭代再判**（⛔ 不许在这里清它）。
    if (voice_busy_ != 0) return;
    if (!voice_work_pending_) return;
    if ((state_flags_ & (1u << 9)) != 0) return;
    state_flags_ &= ~(1u << 14);
    state_flags_ |= (1u << 9);
    // 压低后的恢复与 `0xc2` 的淡变**共用同一个渐变对象与计时器**（`60-ops-voice.md:121` 的
    // 「同一生命周期」）：交接只把目标设成满值、步长 1，并**起计时**（下一次第 9 位服务从此相位步进）。
    bgm_fade_target_ = 10000;  // 刻度满值 10000（唯一观察；单位本版不解释，见 `60-ops-voice.md:174`）
    bgm_fade_step_ = 1;
    bgm_fade_last_ms_ = 0;
}

void Machine::service_text_cell_reveal() {
    // 逐格揭示当前文本（`CORE-OPS-TEXT-CELL-REVEAL-1CE`）：活动位（门字第 30 位）未置 ⇒ 不服务。
    // ★ 每迭代读一次时钟（`advance_scene` 刚把 `scene_clock_ms_` 推到本迭代的读数），按毫秒节拍器
    //   判"够没够一拍"：够 ⇒ **只请求画一格**（画推进前的当前格号），然后格号 ← (格号+1) mod 格数。
    // ★ 一次服务最多推进一格（掉拍不补）：引擎只拿"够没够"当判据，返回的"过了几拍"被丢弃。
    // ★ 格数 0 ⇒ **不推进**（⛔ 不夹成 1）：真机在推进侧是一次除零，行为未观察
    //   （残差 `RES-TEXT-CELL-REVEAL-COUNT0`）；本条只保留"请求了一格、没有推进"这个诚实边界。
    // ★ 画格的**参数与像素**不在本条（残差 `RES-TEXT-CELL-REVEAL-DRAW`）：这里只记一次请求。
    if ((state_flags_ & (1u << 30)) == 0) return;
    const std::uint32_t period = reveal_tick_period_ms_ > 0 ? reveal_tick_period_ms_ : 1u;
    const std::uint32_t elapsed = scene_clock_ms_ >= reveal_tick_start_ms_
                                      ? scene_clock_ms_ - reveal_tick_start_ms_
                                      : 0u;
    const std::uint32_t ticks = elapsed / period;
    if (ticks < reveal_tick_counter_) return;  // 本拍不够一个周期
    reveal_tick_counter_ = ticks + 1u;
    // ★ 请求画的是**推进前**的格号：先把这一刻的当前格号记下来（守卫的观察口），再计数、再推进。
    //   ⛔ 把推进语句挪到这两行之前 ⇒ 记下的是"推进后"的格号，`emulator-core-text-cell-reveal` 当场红。
    text_cell_last_draw_cursor_ = reveal_cursor_;
    ++text_cell_draw_requests_;
    if (reveal_count_ > 0) reveal_cursor_ = (reveal_cursor_ + 1) % reveal_count_;
}

void Machine::advance_scene(std::uint32_t now_ms, bool advance_windows) {
    scene_previous_ms_ = scene_clock_ms_;
    scene_clock_ms_ = now_ms;
    // ★ 逐格揭示（第 30 位）的**服务体**在门的服务链上（`CORE-VM-STATE` 位序表第 8 项），不在这里：
    //   它是"位上的工作"，只能在扫描轮到它时跑（别的位有可能先把控制交回迭代头）。
    //   这里只读一次时钟 —— 本函数是"本迭代的现在"，不是服务入口。
    if (!advance_windows) return;  // 暂停：只更新时间戳，不推进任何窗（`CORE-VM-LOOP`）
    advance_scene_windows();
}

// 渲染步的**推进子步**（同一渲染步的更早子步）。`CORE-TIMING-WINDOW` 的「暂停时」那一段：
// 跳过点分两类，次序**固定** —— 三个"连盖钟一起跳过" → 盖钟 → 五个"只挡推进"。
//   (a) 场景绘制模式 ≠ 1（配置 `set:DrawMode` 决定的那个格）；
//   (b) 画面暂停闩 ≠ 0（窗口消息置 / 清的引擎格，与区间忙标记不是同一格；消息号未取证）；
//   (c) `0x21b` 的标量为 0 **且** 等待位（第 10 位）与影片位（第 13 位）都为 0
//       （掩码 `2400h` 是**或**：任一位为 1 就必然放行）。
// ★ 这三个是**持续状态** ⇒ 成立期间所有以场景时钟计的量都停走（消息等待计时器 / 颜色窗 /
//   源矩形窗）⇒ **连盖钟一起跳过**：不判据、不发请求、不清帧标志、不推窗。
// ★ (c) **只取消门前渲染步这一个时钟写者**（`CORE-TIMING-CLOCK`）：场景时钟仍由 `0x1f4` /
//   `0x20c` / `0x23c` 推进 ⇒ **标量恒为 0 不会让时间停走**；迭代也不因此结束（照常过门）。
//   ⛔ 别把它实现成"整个迭代 / 整个时钟"的否决（那会让等待谓词自己等不到头）。
// ★ 后五个只挡推进、**盖钟照常**：第 24 位 / 影片缓冲被另一处持有 / 场景既不需要重绘且无
//   "有待消费的新帧" / 渲染器未就绪 / **执行区间的暂停**（忙 ∧ 等待位 = 0 —— 这一条在盖钟
//   **之后**，所以钟照盖、只跳请求；它与 (b) 的窗口消息闩不是同一格）。
Machine::SceneAdvance Machine::advance_render_step(std::uint32_t now_ms) {
    // ── ① 三个"连盖钟一起跳过"（次序 = spec 的列举）──
    if (draw_mode_ != 1) return SceneAdvance::Skipped;                    // (a)
    if (scene_hold_latch_ != 0) return SceneAdvance::Skipped;            // (b)
    // (c) 标量 = 0 且等待位 / 影片位都为 0 ⇒ 本迭代不盖门前这一次钟、不判据、不发请求、
    //     不清帧标志；**迭代继续**（门照走）。等待位或影片位任一为 1 ⇒ 放行。
    if (scalar_21b_ == 0 && (state_flags_ & (0x400u | 0x2000u)) == 0) return SceneAdvance::Skipped;

    // ── ② 盖钟（先搬旧、再写新 —— 与 `0x1f4` / `0x20c` / `0x23c` 同序，且是同一对）──
    stamp_scene_clock(now_ms);

    // ── ③ 五个"只挡推进"：任一成立 ⇒ 窗不前进、两个标记不重算，但**时钟照盖** ──
    // ★ 「有待消费的新帧」**读一次即消费**：判据在哪个分支被读到，那一读就是消费点。
    //   这里先读再判 —— 否则别的条件把它短路掉时，这个"待消费"会莫名其妙地活到下一步。
    const bool new_frame = movie_new_frame_;
    movie_new_frame_ = false;
    if ((state_flags_ & (1u << 24)) != 0) return SceneAdvance::ClockOnly;
    if (movie_held_) return SceneAdvance::ClockOnly;
    if (!render_ready_) return SceneAdvance::ClockOnly;
    // ★ 「场景既不需要重绘」＝**门前那条请求判据的内层条件**（脏位 / 有动画 / 命令表在动）。
    //   ★ 为什么把后两个也算进来：spec 把"需要重绘"的判据留给残差
    //   `RES-RENDER-STEP-NEEDS-REDRAW`（脏 / 有动画 / 命令表在动 / 影片新帧），而 70-vm 明说
    //   「有动画」的唯一读者就是门前的「需要重绘」判据 —— 只看脏位的话，一个**还在走的窗**
    //   会在"本步没有新脏位"的迭代里停住（窗走不完 ⇒ 等待位永远放不开）。
    //   两个标记在这里读的是**上一迭代算出的值**（这一步不重算它们）。
    if (!scene_dirty_ && !scene_animating_ && !command_animating_ && !new_frame) {
        return SceneAdvance::ClockOnly;
    }
    // (d) 执行区间的暂停（区间忙标记 ≠ 0 **且** 等待位 = 0）。★ 忙判定在**盖钟之后**
    //   （`0x4127BB` 在 `0x41279F` 之后）⇒ **钟已盖、只跳请求**；它与 (b) 的窗口消息闩不是同一格。
    //   ⛔ 别把它写回"连盖钟一起跳过"（S4 的错归因：那会让暂停期间整个时钟停走）。
    if (driven_elsewhere_ && (state_flags_ & (1u << 10)) == 0) return SceneAdvance::ClockOnly;

    advance_scene_windows();
    return SceneAdvance::Windows;
}

// 渲染步的推进本体：**先清两个标记、再推进各项**（`CORE-TIMING-WINDOW`：清与重置在同一个
// 迭代里、门服务等待位之前）。跳过它 ⇒ 两个标记停在上一迭代的值。
void Machine::advance_scene_windows() {
    const std::uint32_t now_ms = scene_clock_ms_;
    scene_animating_ = false;
    // ★ 渲染步入（`0x4B40AA` / `0x4B40B0`）把脏位与"待处理"闩一起清 0，**紧接着**才推进窗；
    //   本模型的"渲染步"就是这一段（`step()` 在门之前调用）。⇒ 先清后置：
    //   窗还在走 ⇒ 下面的推进把闩重新置起（`0x4B326A` / `0x4B3CCB`）；窗没了 ⇒ 闩就真的清掉。
    //   ⛔ 别把它绑到宿主的 `end_render()`：影片 / 无渲染的宿主路径不呈现场景帧，
    //   闩会永远不清（实测：LOGO 影片路径的 `0x21c` 会因此永不放行）。
    scene_pending_ = false;

    // ── 共享推进（`CORE-TIMING-WINDOW`）：盖章 → 延迟内 / 推进中 → 到期收尾 ──
    // ★ 返回本步派生色（只写进出参，不写记录字段；到期那一步才写当前颜色）。
    // ★ 绘制项与网格的"持续时间 ≤ 0"分叉不同 ⇒ 两个 lambda：
    //   · 绘制项：颜色窗那一段**整段跳过**（不插值、不写派生色、不改当前 / 终色、不清起止与
    //     「有窗」位）—— 它的"到期"只是**读者视角**（`现在 ≥ 起点 + 延迟 + 0` 成立）。
    //   · 网格：清窗起点 + 清「有窗」位 + 置「有动画」，同样不做终色 → 当前色的收尾、不重烘。
    const auto advance_item_window = [&](std::uint32_t& flags, std::uint32_t& current,
                                         std::uint32_t& target, std::int32_t& delay,
                                         std::int32_t& duration, std::uint32_t& start,
                                         bool keep_window) -> std::uint32_t {
        if ((flags & 2u) == 0) return current;
        // ★ 盖章**不看持续时间**：引擎只判"起点为 0"（`0x4B085E` / `0x4B087C`），
        //   所以持续时间 ≤ 0 的窗也在第一次推进时盖章，随后由下面的到期判据立刻判到期。
        if (start == 0) start = now_ms;
        // ★ 到期判据 = `现在 >= 起点 + 延迟 + 持续时间`（引擎 `0x4B17F9` 的 `jge`）⇒ 相等**已到期**；
        //   延迟段（`现在 <= 起点 + 延迟`）**算窗内**。⛔ 别写成 `<=`：那会比引擎晚 1 ms。
        //   （引擎的 `jge` 是有符号比较；这里沿用既有的无符号算术 —— 差别只在时钟越过 2^31 ms 后出现。）
        if (now_ms < start + static_cast<std::uint32_t>(delay) +
                        static_cast<std::uint32_t>(duration)) {
            scene_animating_ = true;
            scene_pending_ = true;  // `+0B5B4`：这一帧还在推进
            // ★ 持续时间 ≤ 0：延迟段仍**算窗内**（上面这一支），但**不插值**（整段跳过）——
            //   "到期"之后同样什么都不清（见下）。⛔ 别在这一支里写 current / target / flags / start。
            if (duration <= 0) return current;
            if (now_ms <= start + static_cast<std::uint32_t>(delay)) return current;
            const auto elapsed = now_ms - start - static_cast<std::uint32_t>(delay);
            const auto progress = static_cast<double>(elapsed) / static_cast<double>(duration);
            return lerp_argb(current, target, progress);
        }
        if (duration <= 0) {
            // 持续时间 ≤ 0：颜色窗那一段**整段跳过** —— 不插值、不写派生色、不改当前 / 终色、
            // 不清起止与「有窗」位（`CORE-TIMING-WINDOW`）。⛔ 别把这条写成"走正常收尾"：
            // 那会写 current ← target、target ← 无颜色、并清掉窗位与起点。
            return current;
        }
        // 到期：当前颜色 ← 终色；终色 ← 无颜色；延迟 / 持续时间清 0；清「有窗」位与起点。
        current = target;
        target = kNoColor;
        delay = 0;
        duration = 0;
        if (!keep_window) {
            flags &= ~2u;
            start = 0;
        }
        return current;
    };

    for (auto& [key, item] : draw_items_) {
        (void)key;
        // 源矩形窗与颜色窗共用一次盖章；这里先盖是因为下面的到期判定要读起点
        // （`advance_item_window` 自己也会盖，但对"只排了源矩形窗"的项必须在这里先有起点）。
        if ((item.flags & 2u) != 0 && item.window_start == 0 &&
            (item.duration > 0 || item.rect_duration > 0)) {
            item.window_start = now_ms;  // 源矩形窗与颜色窗共用一次盖章
        }
        const bool rect_active = (item.flags & 2u) != 0 && item.rect_duration > 0;
        // 源矩形窗与颜色窗同一边界（引擎 `0x4B3C8B` 的 `jge`）：`现在 >= 起点+延迟+持续时间` 已到期。
        const bool rect_expired =
            rect_active && now_ms >= item.window_start + static_cast<std::uint32_t>(item.rect_delay) +
                                         static_cast<std::uint32_t>(item.rect_duration);
        // ★ “窗还在走”**不按种类**：引擎的通用拉闩在种类分派之前（`0x4B1804`），所以只排了源矩形窗的项
        //   同样把「待处理」闩拉起来。少了这一条 ⇒ 只排 `0x239` 的项在第一次推进之后就放掉门。
        if (rect_active && !rect_expired) {
            scene_animating_ = true;
            scene_pending_ = true;
        }
        (void)advance_item_window(item.flags, item.color, item.target_color, item.delay, item.duration,
                                  item.window_start, rect_active && !rect_expired);
        if (rect_expired) {
            // 到期那一档把结果写回条目的四条边（末帧或空矩形）。
            const int width = std::max(0, item.right - item.left);
            const int height = std::max(0, item.bottom - item.top);
            if ((item.rect_switch & 1) != 0 && item.rect_row_steps > 0 &&
                item.rect_step_total > 0) {
                const std::int64_t step = item.rect_step_total - 1;
                const int dx = width * static_cast<int>(step % item.rect_row_steps);
                const int dy = height * static_cast<int>(step / item.rect_row_steps);
                item.left += dx;
                item.right += dx;
                item.top += dy;
                item.bottom += dy;
            } else {
                item.right = item.left;
                item.bottom = item.top;
            }
            item.rect_delay = 0;
            item.rect_duration = 0;
            if (item.duration <= 0) {
                item.flags &= ~2u;
                item.window_start = 0;
            }
        }
    }
    for (auto& [key, mesh] : meshes_) {
        (void)key;
        if ((mesh.flags & 2u) == 0) continue;
        // ★ 网格的持续时间 ≤ 0 分叉（`CORE-TIMING-WINDOW`）：清**窗起点** + 清「有窗」位 + 置
        //   「有动画」，**不**做终色 → 当前色的收尾，也**不重烘**。⛔ 与绘制项那一支不同：
        //   这边确实要清位与起点（下一道真窗才能重新盖章）。
        if (mesh.duration <= 0) {
            mesh.window_start = 0;
            mesh.flags &= ~2u;
            scene_animating_ = true;
            continue;
        }
        auto& start = mesh.window_start;
        const auto& delay = mesh.delay;
        const auto& duration = mesh.duration;
        if (start == 0) start = now_ms;  // 盖章（只判"起点为 0"）
        // 与绘制项同一边界（引擎 `0x4B17F9` 的 `jge`：相等已到期）。这里把判据先算成一个局部量：
        // 网格这一支还要用它选"延迟段 / 推进中"的派生色，写成一行也更清楚。
        const bool in_window =
            now_ms < start + static_cast<std::uint32_t>(delay) + static_cast<std::uint32_t>(duration);
        if (in_window) {
            scene_animating_ = true;
            scene_pending_ = true;  // `+0B5B4`：网格这一帧还在推进
            const auto derived = now_ms <= start + static_cast<std::uint32_t>(delay)
                                     ? mesh.color
                                     : lerp_argb(mesh.color, mesh.target_color,
                                                 static_cast<double>(now_ms - start -
                                                                     static_cast<std::uint32_t>(delay)) /
                                                     static_cast<double>(duration));
            // 网格只在被推进的那一步按派生色重烘顶点缓冲；重烘**唯一的跳过条件**是
            // "顶点缓冲未建 / 顶点数 ≤ 0"（⛔ 不按颜色值分支）。
            if (!mesh.colors.empty()) {
                for (auto& value : mesh.colors) value = derived;
            }
            continue;
        }
        // 正常到期的共享收尾（持续时间 > 0）：取终色 → 当前颜色 ← 终色 →（网格按该值重烘一次）
        // → 终色 ← "无颜色" → 延迟与持续时间清 0；窗位与起点清掉（本模型在到期支里清，残差
        // `RES-WINDOW-FLAG-CLEAR` 记着引擎的清点未取证）。
        mesh.color = mesh.target_color;
        if (!mesh.colors.empty()) {
            for (auto& value : mesh.colors) value = mesh.color;
        }
        mesh.target_color = kNoColor;
        mesh.delay = 0;
        mesh.duration = 0;
        mesh.flags &= ~2u;
        mesh.window_start = 0;
    }
    if (scene_animating_) scene_dirty_ = true;  // 有动画 ⇒ 下一迭代重绘
}

}  // namespace core
