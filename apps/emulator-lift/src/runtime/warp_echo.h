// emulator-lift · `--cursor-warp` 的回灌抑制队列（**窗口客户区像素**的小 FIFO）
//
// ★ 它回答的问题：guest 的 `SetCursorPos` 让宿主真光标动了一次之后，Win32/SDL 会把**同一次移动**
//   回灌成一条 `SDL_EVENT_MOUSE_MOTION`。若不拦，它会经 `mapMouse` 映射回 guest 客户区 ⇒ 反写
//   虚拟光标（`GetCursorPos` 变），并多 post 一条带**宿主墙钟**时间的 `WM_MOUSEMOVE`。需求判据
//   "开关不改变 guest 可见状态" 依赖这条 motion 被吞掉。
//
// ★ 为什么匹配在**窗口像素**空间：回灌到达 sink 时，手上只有窗口客户区像素（`event.motion.x/y`）；
//   要换成 guest 坐标必须过 `mapMouse`，而 `windowPixelToImage` 会 **clamp 到 0..image-1**。越界的
//   warp（Win32 合法：屏幕坐标可负 / 越界）clamp 之后信息已经丢了 ⇒ 在 guest 坐标里比永远对不上
//   （复审反例 B：1600×720 窗口 + image 1280×720、`SetCursorPos(1300,300)` ⇒ warp 落点窗口
//   (1460,300)，回灌 clamp 成 1279 ⇒ 差 21 px 不被吞）。所以 `note()` 记的必须是 **warp 目标在窗口
//   客户区里的那一对像素**（`imageToWindowPixel` 的输出，⛔ 不经过任何 clamp）。
//
// ★ 为什么是 FIFO 而不是单槽：同一帧 / 同一 pump 周期里可以连续 `SetCursorPos` 两次（复审反例 A）。
//   单槽会被后一条覆盖 ⇒ 第一条回灌没人认领 ⇒ 被当成真人移动接受。多槽让每条回灌各自认领一格。
//
// ★ 每条**只消费一次**（命中即弹出）：真人紧接着落在同一像素上的移动必须照旧生效 —— ⛔ 不做长期
//   去抖。容量 8 足够覆盖一帧里的多次 warp；满了丢**最旧**（新的更可能还在路上）。
//
// ★ 纯、**SDL-free**、零分配：`tests/cursor_warp.cpp` 的 `warp-echo/*` 直接 include（pure CTest）。
#pragma once

#include <array>
#include <cmath>
#include <cstdlib>

namespace lift {

class WarpEchoQueue {
public:
    // 一帧里最多记几条 warp 落点。超出 ⇒ 丢最旧（见类注释）。
    static constexpr int kCapacity = 8;
    // 命中容差（窗口像素）。±2 覆盖 SDL / 系统在窗口边界的取整；⛔ 不再放大：放大了会吞掉真人
    // 紧挨着 warp 落点的小幅移动（那正是"只吞一次"要保住的语义）。
    static constexpr int kTolerance = 2;

    // 登记一次**成功** warp 的目标（窗口客户区像素，clamp 之前）。
    void note(int window_px_x, int window_px_y) {
        if (count_ == kCapacity) {  // 满了：丢最旧，给这条腾位置
            head_ = (head_ + 1) % kCapacity;
            --count_;
        }
        const int tail = (head_ + count_) % kCapacity;
        entries_[tail].x = window_px_x;
        entries_[tail].y = window_px_y;
        ++count_;
    }

    // 这条窗口像素 motion 是不是某次 warp 的回灌？是 ⇒ 弹出该条并返回 true（每条只命中一次）。
    // ⛔ 未命中不消费：真人移动不能因为"落在别的 warp 落点附近"而顺带清掉别人的期望。
    bool consume(double window_px_x, double window_px_y) {
        if (count_ == 0) return false;
        // SDL motion 的 x/y 是浮点（窗口坐标）；与登记时的整数目标比较取整（与 warp 侧
        // `std::lround` 同一口径）。
        const long long mx = std::llround(window_px_x);
        const long long my = std::llround(window_px_y);
        for (int i = 0; i < count_; ++i) {
            const int slot = (head_ + i) % kCapacity;
            const Entry& e = entries_[slot];
            if (std::llabs(mx - e.x) <= kTolerance && std::llabs(my - e.y) <= kTolerance) {
                // 弹出：后面的前移一格（FIFO 顺序保持）。
                for (int j = i; j + 1 < count_; ++j) {
                    entries_[(head_ + j) % kCapacity] = entries_[(head_ + j + 1) % kCapacity];
                }
                --count_;
                return true;
            }
        }
        return false;
    }

    // 全部清空（**只在 `shutdown` 这类"整轮结束了"的地方用**）。
    // ★ ⛔ **不要按键 / 滚轮 / 键盘一进来就 clear**（2026-11-14 独立复审判过的错）：
    //   SDL 队列按 OS 顺序排，一条在 warp **之前**入队的键事件会在 warp **之后**才排空 ⇒ 它会把
    //   刚登记的期望抹掉，紧随其后的那条回灌被当成真人移动接受，反写虚拟光标（同型的洞，只是更窄）。
    //   ⇒ 老化只靠两条不变式：**每条只消费一次** + **容量 8（满了丢最旧）**。
    //   代价：一条**陈旧**登记可能吞掉一次落在它 ±2 px 内的真人移动（只吞一次，且只在
    //   `--cursor-warp` 开着时存在 —— 那时游戏本来就在不停 warp）。这个代价 < 反写虚拟光标。
    void clear() {
        head_ = 0;
        count_ = 0;
    }

private:
    struct Entry {
        int x = 0;
        int y = 0;
    };

    std::array<Entry, kCapacity> entries_{};
    int head_ = 0;
    int count_ = 0;
};

}  // namespace lift
