// emulator-lift · UI 画布原点的**纯函数**判据（`ui/canvas.h`；无 SDL、无 GPU、无资产）
//
// 判据（红了意味着什么）：
//   ① **16:9 ⇒ 居中偏移为 0**：1280×720 ⇒ `(s=1, 0, 0)`；640×360 ⇒ `(s=0.5, 0, 0)`。
//      红了 ⇒ 画布原点不再 == 画面矩形原点（16:9 的 HUD 会整体偏移）。
//   ② **非 16:9 ⇒ 居中偏移非 0**：320×240 ⇒ `(s=0.25, 0, 30)`；1024×768 ⇒ `(s=0.8, 0, 96)`；
//      256×128 ⇒ `ox > 0 && oy == 0`。红了 ⇒ 有人丢掉了 `((pic−ref·s)/2)` 那一项
//      （`overlay.md` B2 的裁决），非 16:9 下 HUD 会贴边 / 落到黑边里。
//   ③ **s = min(两轴)**：1280×360 ⇒ `s=0.5`（不是 1.0 / 不是 0.28125），画布宽 640、高 360。
//      红了 ⇒ 缩放开错轴（UI 会被拉伸）。
//   ④ 非法几何（0 / 负数）⇒ `scale == 0`、偏移 0（调用方据此跳过这一帧，⛔ 不猜）。
// ★ 本用例只 include 纯头 ⇒ 顶层无条件注册 = **pure** 档（`AGENTS.md` §12.4：判据 = 注册位置）。
#include <cmath>
#include <cstdint>

#include "check.h"
#include "ui/canvas.h"

namespace {

bool approxEq(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }

void expectOrigin(int pic_w, int pic_h, double want_s, int want_x, int want_y, const char* what) {
    const lift::ui::CanvasOrigin origin = lift::ui::uiCanvasOrigin(pic_w, pic_h);
    ++lift_test::g_checks;
    if (!approxEq(origin.scale, want_s) || origin.offset_x != want_x || origin.offset_y != want_y) {
        lift_test::failf(__FILE__, __LINE__,
                         "%s: uiCanvasOrigin(%d,%d) expected (s=%.6f, %d, %d), got (s=%.6f, %d, %d)", what, pic_w,
                         pic_h, want_s, want_x, want_y, origin.scale, origin.offset_x, origin.offset_y);
    }
}

// ---- ① 16:9：偏移 0（画布原点 == 画面矩形原点）----
void caseWide() {
    lift_test::g_current_case = "ui-canvas/origin-16x9";
    expectOrigin(1280, 720, 1.0, 0, 0, "1280×720（s=1）");
    expectOrigin(640, 360, 0.5, 0, 0, "640×360（s=0.5）");
    expectOrigin(1920, 1080, 1.5, 0, 0, "1920×1080（s=1.5）");
}

// ---- ② 非 16:9：居中偏移非 0（B2 的裁决）----
void caseTall() {
    lift_test::g_current_case = "ui-canvas/origin-centered";
    // 4:3：s = 0.25，画布 320×180 ⇒ 纵向居中 (0,30)。
    expectOrigin(320, 240, 0.25, 0, 30, "320×240（4:3 ⇒ 纵向居中 30）");
    // 4:3 的另一档：s = 1024/1280 = 0.8，画布 1024×576 ⇒ 纵向居中 (0,96)。
    expectOrigin(1024, 768, 0.8, 0, 96, "1024×768（4:3 ⇒ 纵向居中 96）");
    // 2:1：s = 128/720 ≈ 0.17778，画布 227.56×128 ⇒ 横向居中，纵向 0。
    const lift::ui::CanvasOrigin wide = lift::ui::uiCanvasOrigin(256, 128);
    ++lift_test::g_checks;
    if (!(wide.scale > 0.0 && wide.scale < 0.2) || wide.offset_x <= 0 || wide.offset_y != 0) {
        lift_test::failf(__FILE__, __LINE__, "256×128（2:1）应横向居中：got (s=%.6f, %d, %d)", wide.scale,
                         wide.offset_x, wide.offset_y);
    }
}

// ---- ③ s = min(两轴) ----
void caseMinAxis() {
    lift_test::g_current_case = "ui-canvas/min-axis";
    // 1280×360：x 轴 s=1、y 轴 s=0.5 ⇒ 取 0.5；画布 640×360；x 居中 (320,0)。
    expectOrigin(1280, 360, 0.5, 320, 0, "1280×360（高度是短边 ⇒ s=0.5）");
    // 640×720：x 轴 0.5、y 轴 1.0 ⇒ 取 0.5；画布 640×360；y 居中 (0,180)。
    expectOrigin(640, 720, 0.5, 0, 180, "640×720（宽度是短边 ⇒ s=0.5）");
}

// ---- ④ 非法几何 ----
void caseDegenerate() {
    lift_test::g_current_case = "ui-canvas/degenerate";
    expectOrigin(0, 720, 0.0, 0, 0, "宽 0");
    expectOrigin(1280, 0, 0.0, 0, 0, "高 0");
    expectOrigin(-1, -1, 0.0, 0, 0, "负数");
}

}  // namespace

int main() {
    caseWide();
    caseTall();
    caseMinAxis();
    caseDegenerate();
    return lift_test::finish("ui-canvas");
}
