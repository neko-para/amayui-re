// emulator-lift · 命中 / 捕获路由的实现（切片 2 后半）
//
// ★ **stub 阶段**（测试先红）：本文件先只有签名与空行为，测试会在"命中表空 / 不吞 / 没有 click"
//   上红；实现落在下一条 `feat(ui):` 提交里。判据与口径见 `ui/hit.h` 的文件头。
#include "ui/hit.h"

namespace lift::ui {

std::vector<HitRegion> buildHitRegions(const std::vector<Element>& /*elements*/, const CanvasOrigin& /*canvas*/,
                                       int /*picture_x*/, int /*picture_y*/) {
    return {};
}

const HitRegion* hitTest(const std::vector<HitRegion>& /*regions*/, int /*px*/, int /*py*/) { return nullptr; }

void ClickRouter::commitFrame(std::vector<HitRegion> /*regions*/) {}

void ClickRouter::clearFrame() {}

UiPointerDecision ClickRouter::handlePointer(UiPointerPhase /*phase*/, UiPointerButton /*button*/, int /*px*/,
                                             int /*py*/) {
    return {};
}

void ClickRouter::focusLost() {}

std::string ClickRouter::activeKey() const { return {}; }

std::string ClickRouter::hoverKey(int /*px*/, int /*py*/) const { return {}; }

bool ClickRouter::takeClick(UiClick* /*out*/) { return false; }

}  // namespace lift::ui
