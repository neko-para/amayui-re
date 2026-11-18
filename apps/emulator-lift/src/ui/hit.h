// emulator-lift · UI 的**命中原语 / 捕获路由**（`REQ-01M7QF01A00F9946DX0GPH23JQ` 切片 2 后半）
//
// ★ 裁决（需求单「已裁决」第 2 条）：C++ **只做原语** —— `key` 挂在 `ui.rect` 上；
//   ⛔ 没有 C++ 的 `ui.button`，⛔ 没有 JS 的 `ui.hit` 模块。按钮外观 / 布局 / 状态 / 主题
//   全在 `scripts/ui/kit.js`。⇒ 本头**不是** API 面（脚本看不到它），只是宿主侧的纯状态机。
//
// ★ 命中区 = `ui.rect` 的 **resolved rect**（参考像素 → 窗口像素的最终矩形：画布缩放 + 居中偏移
//   + DPR 之后的窗口像素）。⛔ 不在这里重算第二份算术：`buildHitRegions` 走
//   `raster.h` 的 `resolveRectPixels` / `drawsBefore`，与 `rasterize` **同一份**。
//
// ★ 命中顺序 = **上一帧成功 present 的绘制序逆序**（后画的先命中）；没有成功帧 ⇒ 不命中。
//   ⛔ "成功" = `presentOverlay` 真的返回 true（不是"脚本跑了一遍"）。
//
// ★ 事件边界（需求单「设计」第 4 条；判据在 `tests/ui_click_router.cpp`）：
//   · 左键 down **命中** ⇒ 吞掉 + 记 capture；没命中 ⇒ 放行给 guest；
//   · 左键 up：**只要这次 down 是我们接的** ⇒ 无条件吞掉（哪怕抬起点已在 rect 外，哪怕 capture
//     已被失焦清掉）。⛔ 不让 guest 收到一个**没有 down 的 up**（菜单类界面常在 up 上触发）；
//   · 抬起点仍在 capture 那条 rect 内 ⇒ 产**一次** click（`{key,x,y}`，x/y = 抬起点）；
//   · motion / wheel / 右键 / 中键 ⇒ 一律放行；
//   · 失焦 / 关窗 / 新的左键 down ⇒ 清 capture（拖出 ⇒ 抬起时自然取消，不发 click）。
//
// ★ **纯数据 + 纯状态机**：SDL-free / JS-free / 无剪贴板。`handleEvent` 只调这里记 pending，
//   ⛔ 不跑 JS、不开 profile、不写剪贴板；pending click 由下一次 `render` 消费（一次性）。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ui/canvas.h"
#include "ui/element.h"

namespace lift::ui {

// 一个命中区 = 一条 `key` 非空的 Rect 的 **resolved rect**（窗口像素，绝对坐标）。
// ★ 顺序 = 绘制序（z 升序、同 z 按构造序）；`hitTest` 从末尾往前找 ⇒ "后画的先命中"。
struct HitRegion {
    std::string key;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    // 左闭右开：`x` 在、`x+w` 不在（与"1 px 内 / 外"的判据一致；w/h 已由 scaleRef 保证 ≥ 1）。
    bool contains(int px, int py) const {
        return w > 0 && h > 0 && px >= x && py >= y && px < x + w && py < y + h;
    }
};

// 把 display list 解析成命中表：只收 `kind == Rect && !key.empty()`，顺序 = **绘制序**。
// `canvas` / `picture_x` / `picture_y` 与 `rasterize` 的入参同一份（几何不可用 ⇒ 空表）。
std::vector<HitRegion> buildHitRegions(const std::vector<Element>& elements, const CanvasOrigin& canvas,
                                       int picture_x, int picture_y);

// 逆序命中：后画的先命中。没有命中 ⇒ nullptr（指针指向 `regions` 里的元素，生命周期 = 入参）。
const HitRegion* hitTest(const std::vector<HitRegion>& regions, int px, int py);

// 一次性 click 事件（`game.click` 的载荷）：x/y = **抬起点**（窗口像素）。
struct UiClick {
    std::string key;
    int x = 0;
    int y = 0;
};

// 指针事件的**宿主无关**分类（`present_sdl.cpp` 把 SDL 事件映射到它；判据是纯的）。
enum class UiPointerButton : std::uint8_t { Left = 0, Right = 1, Middle = 2 };
enum class UiPointerPhase : std::uint8_t { Down = 0, Up = 1, Motion = 2, Wheel = 3 };

// 一条指针事件的处置：`swallow` = 不进 guest；`emit_click` = 这一条产生了新的 pending click。
struct UiPointerDecision {
    bool swallow = false;
    bool emit_click = false;
};

// 捕获路由：上一帧成功 present 的命中表 + 当前 capture + 一次性 pending click。
// ★ 单线程（主线程）；⛔ 不做线程安全（与 `PresentSink` 的契约一致）。
class ClickRouter {
public:
    // 上一帧**成功 present** 时提交它的命中表（绘制序）。**没有**成功帧 ⇒ 不要调它
    // （初始 / `clearFrame()` 之后 = 空表 ⇒ 永不命中）。
    void commitFrame(std::vector<HitRegion> regions);
    // 叠层关掉 / 关闭 / 没有成功帧 ⇒ 清命中表 **且** 清 capture（命中区已经不在屏幕上了）。
    void clearFrame();
    bool hasFrame() const { return frame_committed_; }
    const std::vector<HitRegion>& regions() const { return regions_; }

    // 喂一条指针事件，返回"吞不吞 / 有没有新 click"。
    UiPointerDecision handlePointer(UiPointerPhase phase, UiPointerButton button, int px, int py);
    // 失焦 / 关窗 ⇒ 清 capture（不产生 click；pending 不动）。
    void focusLost();

    // 当前被捕获的 key；没有 ⇒ 空串（脚本侧 `game.active === undefined`）。
    std::string activeKey() const;
    // 当前指针下的 key；没有 ⇒ 空串（脚本侧 `game.hover === undefined`）。
    std::string hoverKey(int px, int py) const;

    // 取走 pending click（**一次性**）：取到 ⇒ true 并清空；再取 ⇒ false。
    // ★ 调用点 = 每次 render 之前（宿主"消费一帧"），因此下一次 render 就没有它了。
    bool takeClick(UiClick* out);
    bool hasPendingClick() const { return pending_.has_value(); }

private:
    std::vector<HitRegion> regions_;
    bool frame_committed_ = false;
    // capture = **down 那一刻**的 key + resolved rect。用快照而不是"每帧按 key 重查"：
    // 版式（HUD 面板宽度）会逐帧变，down / up 必须用**同一个**矩形判定，否则"按住时面板长了
    // 一格"会让抬起点凭空落到外面（点击静默丢失）。
    std::optional<HitRegion> active_;
    // ★ "这次左键 down 是我们接的、它的 up 还没到"（欠一个 up）。
    //   与 `active_` 分开：失焦 / 关窗清 capture（= 取消 click），但**不**把这次 up 还给 guest
    //   （否则 guest 收到没有 down 的 up）。新的左键 down 重新决定它。
    bool up_owed_ = false;
    std::optional<UiClick> pending_;
};

}  // namespace lift::ui
