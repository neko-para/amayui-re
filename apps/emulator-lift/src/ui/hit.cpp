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
    // 命中区已经不在屏幕上了 ⇒ capture 也作废（它的 rect 只是一份快照），欠着的 up 也不再收
    // （叠层都关了 = 路由不在，放行才是"既有语义"）。
    active_.reset();
    up_owed_ = false;
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
        up_owed_ = false;  // 新的 down 重新决定"这次抬起归谁"
        const HitRegion* hit = hitTest(regions_, px, py);
        if (hit == nullptr) return decision;  // 没命中 ⇒ 放行给 guest（HUD 之外的地方必须能玩）
        active_ = *hit;
        up_owed_ = true;  // 这次 down 被我们接了 ⇒ 它的 up 必须由我们收掉
        decision.swallow = true;
        return decision;
    }

    // Up：**只要这次 down 是我们接的**就吞（哪怕抬起点已在 rect 外、哪怕 capture 已被失焦清掉）。
    // ★ 不能改回"看 active_ 有没有值"：失焦清 capture 之后那次 up 若放行，guest 会收到一个
    //   **没有 down 的 up**（菜单类界面常在 up 上触发）。
    if (!up_owed_) return decision;
    decision.swallow = true;
    up_owed_ = false;
    if (active_ && active_->contains(px, py)) {
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

void ClickRouter::focusLost() {
    // 失焦 ⇒ **取消这次点击**（capture 清空 ⇒ up 时不会发 click），但 up 仍由我们收掉
    // （`up_owed_` 不清）：⛔ 不把"没有 down 的 up"漏给 guest。
    active_.reset();
}

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
