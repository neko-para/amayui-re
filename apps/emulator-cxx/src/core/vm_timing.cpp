// 场景计时窗的推进（70-vm §时钟 / CORE-TIMING-WINDOW）：颜色窗与源矩形窗共用一次盖章。
//
// ★ `step()` 在门之前调用它；它只改派生态（当前颜色、四条边）与"有动画"标记，
//   不判脏、不请求渲染 —— 那些是主循环的事。
#include "core/vm.h"

#include "core/color.h"

namespace core {

void Machine::advance_scene(std::uint32_t now_ms, bool advance_windows) {
    scene_previous_ms_ = scene_clock_ms_;
    scene_clock_ms_ = now_ms;
    if (!advance_windows) return;  // 暂停：只更新时间戳，不推进任何窗（`CORE-VM-LOOP`）
    scene_animating_ = false;

    // 共享推进：盖章 → 延迟内 / 推进中 → 到期收尾（`CORE-TIMING-WINDOW`）。
    // 返回本步派生色（只写进出参，不写记录字段；到期那一步才写当前颜色）。
    const auto advance_window = [&](std::uint32_t& flags, std::uint32_t& current,
                                    std::uint32_t& target, std::int32_t& delay,
                                    std::int32_t& duration, std::uint32_t& start,
                                    bool keep_window = false) {
        if ((flags & 2u) == 0) return current;
        if (duration <= 0) return current;  // 不盖章 / 不插值 / 不置动画 / 不到期
        if (start == 0) start = now_ms;
        if (now_ms <= start + static_cast<std::uint32_t>(delay) +
                          static_cast<std::uint32_t>(duration)) {
            scene_animating_ = true;
            if (now_ms <= start + static_cast<std::uint32_t>(delay)) return current;
            const auto elapsed = now_ms - start - static_cast<std::uint32_t>(delay);
            const auto progress = static_cast<double>(elapsed) / static_cast<double>(duration);
            return lerp_argb(current, target, progress);
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
        if ((item.flags & 2u) != 0 && item.window_start == 0 &&
            (item.duration > 0 || item.rect_duration > 0)) {
            item.window_start = now_ms;  // 源矩形窗与颜色窗共用一次盖章
        }
        const bool rect_active = (item.flags & 2u) != 0 && item.rect_duration > 0;
        const bool rect_expired =
            rect_active && now_ms > item.window_start + static_cast<std::uint32_t>(item.rect_delay) +
                                         static_cast<std::uint32_t>(item.rect_duration);
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
