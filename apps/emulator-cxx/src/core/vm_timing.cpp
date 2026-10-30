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
    // 逐格揭示的节拍服务（每迭代一次，与场景窗是否推进无关：暂停只跳过场景推进，不跳过主循环的服务）。
    service_text_cell_reveal();
    if (!advance_windows) return;  // 暂停：只更新时间戳，不推进任何窗（`CORE-VM-LOOP`）
    scene_animating_ = false;
    // ★ 渲染步入（`0x4B40AA` / `0x4B40B0`）把脏位与"待处理"闩一起清 0，**紧接着**才推进窗；
    //   本模型的"渲染步"就是这一段（`step()` 在门之前调用）。⇒ 先清后置：
    //   窗还在走 ⇒ 下面的推进把闩重新置起（`0x4B326A` / `0x4B3CCB`）；窗没了 ⇒ 闩就真的清掉。
    //   ⛔ 别把它绑到宿主的 `end_render()`：影片 / 无渲染的宿主路径不呈现场景帧，
    //   闩会永远不清（实测：LOGO 影片路径的 `0x21c` 会因此永不放行）。
    scene_pending_ = false;

    // 共享推进：盖章 → 延迟内 / 推进中 → 到期收尾（`CORE-TIMING-WINDOW`）。
    // 返回本步派生色（只写进出参，不写记录字段；到期那一步才写当前颜色）。
    // ★ 推进中同时置"待处理"闩（`+0B5B4`）：引擎在窗推进的那一步把它写成 1
    //   （`sub_4B06D0` 的 `0x4B326A` / `0x4B3CCB`），渲染步入（`0x4B40B0`）再清 0。
    const auto advance_window = [&](std::uint32_t& flags, std::uint32_t& current,
                                    std::uint32_t& target, std::int32_t& delay,
                                    std::int32_t& duration, std::uint32_t& start,
                                    bool keep_window = false) {
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
            scene_pending_ = true;  // `+0B5B4`：这一帧还在推进 ⇒ 消息子系统不算完成
            if (now_ms <= start + static_cast<std::uint32_t>(delay)) return current;
            const auto elapsed = now_ms - start - static_cast<std::uint32_t>(delay);
            const auto progress = static_cast<double>(elapsed) / static_cast<double>(duration);
            return lerp_argb(current, target, progress);
        }
        if (duration <= 0) {
            // 持续时间 ≤ 0：盖章之后立刻到期。**不跑**"当前颜色 ← 终色"那段共享收尾
            // （那条合同的适用范围是持续时间 > 0，见 `CORE-TIMING-WINDOW`）；只清延迟与窗位 / 起点，
            // 否则这道窗会一直留着、每一步都重走一遍到期支。
            // ★ 窗位与起点是**与源矩形窗共用**的 ⇒ 源矩形窗还在走（`keep_window`）时不清它们，
            //   也不清延迟（共用期里这道窗的记录留给源矩形窗的到期段收尾）。
            if (!keep_window) {
                delay = 0;
                flags &= ~2u;
                start = 0;
            }
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
        // （`advance_window` 自己也会盖，但对"只排了源矩形窗"的项必须在这里先有起点）。
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
        (void)advance_window(item.flags, item.color, item.target_color, item.delay, item.duration,
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
        const bool had_window = (mesh.flags & 2u) != 0;
        const auto derived = advance_window(mesh.flags, mesh.color, mesh.target_color, mesh.delay,
                                            mesh.duration, mesh.window_start);
        // 网格只在被推进的那一步按派生色重烘顶点缓冲（`CORE-TIMING-WINDOW`）。
        if (had_window && !mesh.colors.empty()) {
            for (auto& value : mesh.colors) value = derived;
        }
    }
    if (scene_animating_) scene_dirty_ = true;  // 有动画 ⇒ 下一迭代重绘
}

}  // namespace core
