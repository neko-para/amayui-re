// emulator-lift · 命中 / 捕获路由的实现（`REQ-01M7QF01A00F9946DX0GPH23JQ` 切片 2 后半）
//
// ★ 判据与口径见 `ui/hit.h` 的文件头（本文件只落实现，不重新定义语义）。
// ★ 命中矩形走 `raster.h` 的 `resolveRectPixels` / `drawsBefore` —— 与 `rasterize` **同一份**
//   算术与**同一份**绘制序；这里 ⛔ 不另写 `* scale + origin`。
#include "ui/hit.h"

#include <algorithm>
#include <utility>

#include "ui/raster.h"

namespace lift::ui {

std::vector<HitRegion> buildHitRegions(const std::vector<Element>& elements, const CanvasOrigin& canvas,
                                       int picture_x, int picture_y) {
    std::vector<HitRegion> out;
    // 几何不可用（窗口最小化到 0 / 没有画面）⇒ 没有命中区（⛔ 不猜一个默认缩放）。
    if (canvas.scale <= 0.0) return out;
    std::vector<const Element*> ordered;
    ordered.reserve(elements.size());
    for (const Element& e : elements) {
        // 只有 **key 非空的 Rect** 是命中区：Text 不是（它的尺寸由字体度量），空 key = 纯背景。
        if (e.kind != ElementKind::Rect || e.key.empty()) continue;
        ordered.push_back(&e);
    }
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const Element* a, const Element* b) { return drawsBefore(*a, *b); });
    out.reserve(ordered.size());
    for (const Element* e : ordered) {
        const RectPixels rect = resolveRectPixels(*e, canvas, picture_x, picture_y);
        HitRegion region;
        region.key = e->key;
        region.x = rect.x;
        region.y = rect.y;
        region.w = rect.w;
        region.h = rect.h;
        out.push_back(std::move(region));
    }
    return out;
}

const HitRegion* hitTest(const std::vector<HitRegion>& regions, int px, int py) {
    // 逆序 = **后画的先命中**（绘制序的最后一条压在最上面）。
    for (auto it = regions.rbegin(); it != regions.rend(); ++it) {
        if (it->contains(px, py)) return &*it;
    }
    return nullptr;
}

void ClickRouter::commitFrame(std::vector<HitRegion> regions) {
    regions_ = std::move(regions);
    frame_committed_ = true;
}

void ClickRouter::clearFrame() {
    regions_.clear();
    frame_committed_ = false;
    // 命中区已经不在屏幕上了 ⇒ capture 也作废（它的 rect 只是一份快照）。
    active_.reset();
}

UiPointerDecision ClickRouter::handlePointer(UiPointerPhase phase, UiPointerButton button, int px, int py) {
    UiPointerDecision decision;
    // motion / wheel 一律放行（对 guest 是普通输入）；右键 / 中键同样放行。
    if (phase != UiPointerPhase::Down && phase != UiPointerPhase::Up) return decision;
    if (button != UiPointerButton::Left) return decision;

    if (phase == UiPointerPhase::Down) {
        // ★ 新的左键 down **无条件**清 capture（不论是命中还是落到空白）：按住 A 时再按 B，
        //   capture 必须换成 B，抬起时只发 B。
        active_.reset();
        const HitRegion* hit = hitTest(regions_, px, py);
        if (hit == nullptr) return decision;  // 没命中 ⇒ 放行给 guest（HUD 之外的地方必须能玩）
        active_ = *hit;
        decision.swallow = true;
        return decision;
    }

    // Up：只有"这次 down 是我们接的"才吞。没 capture 的 up 必须放行 —— 否则 guest 的那个 down
    // 永远等不到配对的 up（按住状态卡住）。
    if (!active_) return decision;
    decision.swallow = true;  // ★ 捕获中的 up **无条件**吞掉（哪怕抬起点已在 rect 外）
    if (active_->contains(px, py)) {
        UiClick click;
        click.key = active_->key;
        click.x = px;
        click.y = py;
        pending_ = std::move(click);
        decision.emit_click = true;
    }
    // 抬起 = 一次交互结束 ⇒ capture 清空（拖出 = 取消，不发 click）。
    active_.reset();
    return decision;
}

void ClickRouter::focusLost() { active_.reset(); }

std::string ClickRouter::activeKey() const { return active_ ? active_->key : std::string(); }

std::string ClickRouter::hoverKey(int px, int py) const {
    const HitRegion* hit = hitTest(regions_, px, py);
    return hit != nullptr ? hit->key : std::string();
}

bool ClickRouter::takeClick(UiClick* out) {
    if (!pending_) return false;
    if (out != nullptr) *out = std::move(*pending_);
    pending_.reset();
    return true;
}

}  // namespace lift::ui
