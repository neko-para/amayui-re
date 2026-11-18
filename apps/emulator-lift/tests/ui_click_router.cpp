// emulator-lift · UI **捕获路由**的 pure 判据（`REQ-01M7QF01A00F9946DX0GPH23JQ` 切片 2 后半）
//
// 判据（需求单「设计」第 4 条逐条落地；红了意味着什么）：
//   ① 左键 **down 命中** ⇒ 吞掉 + 记 capture（`active` = 那个 key）。
//      红了 ⇒ 点按钮的同时 guest 也收到一次点击（游戏里那一下被穿过去）。
//   ② 左键 **up 无条件吞掉**（只要这次 down 是我们接的 —— 哪怕抬起点已在 rect 外）。
//      红了 ⇒ guest 收到一个没有 down 的 up（按钮按住状态卡住 / 拖拽半截）。
//   ③ 只有抬起点仍**在同一条 rect 内**才发 **一次** click；x/y = 抬起点。
//      红了 ⇒ 拖出去松手仍然触发按钮（"取消"变成了"点中"）。
//   ④ **拖出取消**：down 命中 → up 落在别的 key / 外面 ⇒ 吞掉 up，但**不**产生 click。
//   ⑤ **motion / wheel / 右键 / 中键放行**给 guest，且 motion **不清** capture（拖出去还能拖回来）。
//      红了 ⇒ HUD 把游戏里的移动 / 滚轮 / 右键吃掉了。
//   ⑥ **新的左键 down 清 capture**（capture 换成新命中的那个；没命中 ⇒ 没有 capture）。
//   ⑦ **click 一次性**：`takeClick` 取到一次之后立刻没有（下一次 render / 下一次调用都取不到）。
//   ⑧ 没命中 / 不在捕获中 ⇒ 放行；失焦 ⇒ 清 capture。红了 ⇒ HUD 在没有任何按钮的地方偷输入。
// ★ 顶层无条件注册 = **pure** 档（AGENTS.md §12.4：判据 = 注册位置）。只 include 纯头（无 SDL / JS）。
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "check.h"
#include "ui/hit.h"

namespace {

using lift::ui::ClickRouter;
using lift::ui::HitRegion;
using lift::ui::UiClick;
using lift::ui::UiPointerButton;
using lift::ui::UiPointerDecision;
using lift::ui::UiPointerPhase;

// 命中表（绘制序）：a 在上、b 在下，不重叠。
void commitTwo(ClickRouter& router) {
    std::vector<HitRegion> hits;
    hits.push_back(HitRegion{"a", 0, 0, 100, 20});
    hits.push_back(HitRegion{"b", 0, 30, 100, 20});
    router.commitFrame(std::move(hits));
}

UiPointerDecision pointer(ClickRouter& router, UiPointerPhase phase, UiPointerButton button, int x, int y) {
    return router.handlePointer(phase, button, x, y);
}

void expectActive(const ClickRouter& router, const char* want, const char* what) {
    ++lift_test::g_checks;
    if (router.activeKey() != want) {
        lift_test::failf(__FILE__, __LINE__, "%s: 期望 active=`%s`，实际 `%s`", what, want,
                         router.activeKey().c_str());
    }
}

void expectSwallow(const UiPointerDecision& d, bool want, const char* what) {
    ++lift_test::g_checks;
    if (d.swallow != want) {
        lift_test::failf(__FILE__, __LINE__, "%s: 期望 swallow=%d，实际 %d", what, want ? 1 : 0,
                         d.swallow ? 1 : 0);
    }
}

void expectClick(ClickRouter& router, const char* key, int x, int y, const char* what) {
    UiClick click;
    ++lift_test::g_checks;
    if (!router.takeClick(&click)) {
        lift_test::failf(__FILE__, __LINE__, "%s: 期望有一次 click，实际没有", what);
        return;
    }
    ++lift_test::g_checks;
    if (click.key != key || click.x != x || click.y != y) {
        lift_test::failf(__FILE__, __LINE__, "%s: 期望 click `%s`@(%d,%d)，实际 `%s`@(%d,%d)", what, key, x, y,
                         click.key.c_str(), click.x, click.y);
    }
}

void expectNoClick(ClickRouter& router, const char* what) {
    UiClick click;
    ++lift_test::g_checks;
    if (router.takeClick(&click)) {
        lift_test::failf(__FILE__, __LINE__, "%s: 期望没有 click，实际 `%s`@(%d,%d)", what, click.key.c_str(),
                         click.x, click.y);
    }
}

// ---------------------------------------------------------------------------------------
// ① down 吞 + capture / ② up 无条件吞 / ③ 同 rect 抬起才发 click / ⑦ 一次性
// ---------------------------------------------------------------------------------------
void caseCapture() {
    lift_test::g_current_case = "ui-click-router/capture";
    ClickRouter router;
    commitTwo(router);

    // ① down 命中 ⇒ 吞 + capture；hover 看的是指针位置（不是 capture）。
    expectSwallow(pointer(router, UiPointerPhase::Down, UiPointerButton::Left, 10, 10), true,
                  "down 命中必须吞掉");
    expectActive(router, "a", "down 命中后 active");
    ++lift_test::g_checks;
    if (router.hoverKey(10, 10) != "a") lift_test::failf(__FILE__, __LINE__, "hover 应是 `a`");
    ++lift_test::g_checks;
    if (!router.hoverKey(10, 100).empty()) lift_test::failf(__FILE__, __LINE__, "空白处 hover 必须为空");

    // ③ 同 rect 内抬起 ⇒ 吞 + 一次 click（x/y = 抬起点）。
    const UiPointerDecision up = pointer(router, UiPointerPhase::Up, UiPointerButton::Left, 50, 15);
    expectSwallow(up, true, "捕获中的 up 必须吞掉");
    ++lift_test::g_checks;
    if (!up.emit_click) lift_test::failf(__FILE__, __LINE__, "同一 rect 内抬起应 emit_click");
    expectClick(router, "a", 50, 15, "同一 rect 内抬起");
    expectActive(router, "", "up 之后 capture 清空");
    // ⑦ 一次性：再取没有。
    expectNoClick(router, "click 只能被消费一次");

    // ① 没命中 ⇒ 放行（guest 的 down），且不产生 capture。
    expectSwallow(pointer(router, UiPointerPhase::Down, UiPointerButton::Left, 10, 100), false,
                  "down 没命中必须放行给 guest");
    expectActive(router, "", "没命中的 down 不建立 capture");
    // ⑧ 不在捕获中的 up 必须放行（否则 guest 的 down/up 不对称）。
    expectSwallow(pointer(router, UiPointerPhase::Up, UiPointerButton::Left, 10, 100), false,
                  "没有 capture 的 up 必须放行给 guest");
    expectNoClick(router, "放行的 up 不产生 click");

    // ④ 拖出：up 落在别的 key 上 / 外面 ⇒ 吞 up、取消 click。
    expectSwallow(pointer(router, UiPointerPhase::Down, UiPointerButton::Left, 10, 10), true, "第二次 down 命中");
    const UiPointerDecision dragged = pointer(router, UiPointerPhase::Up, UiPointerButton::Left, 10, 40);
    expectSwallow(dragged, true, "拖到别的 key 上抬起：up 仍须吞掉");
    ++lift_test::g_checks;
    if (dragged.emit_click) lift_test::failf(__FILE__, __LINE__, "拖到别的 key 上不许发 click");
    expectNoClick(router, "拖出取消：没有 click");
    expectActive(router, "", "拖出取消后 capture 清空");

    expectSwallow(pointer(router, UiPointerPhase::Down, UiPointerButton::Left, 10, 10), true, "第三次 down 命中");
    const UiPointerDecision outside = pointer(router, UiPointerPhase::Up, UiPointerButton::Left, 500, 500);
    expectSwallow(outside, true, "拖到 rect 外抬起：up 仍须吞掉");
    ++lift_test::g_checks;
    if (outside.emit_click) lift_test::failf(__FILE__, __LINE__, "拖到 rect 外不许发 click");
    expectNoClick(router, "拖到外面取消：没有 click");

    // ⑤ motion / wheel 放行，且 **motion 不清 capture**（拖出去还能拖回来）。
    expectSwallow(pointer(router, UiPointerPhase::Down, UiPointerButton::Left, 10, 10), true, "第四次 down 命中");
    expectSwallow(pointer(router, UiPointerPhase::Motion, UiPointerButton::Left, 500, 500), false,
                  "motion 必须放行给 guest");
    expectActive(router, "a", "motion 不许清 capture（拖出去之后还能拖回来）");
    expectSwallow(pointer(router, UiPointerPhase::Wheel, UiPointerButton::Left, 500, 500), false,
                  "wheel 必须放行给 guest");
    expectActive(router, "a", "wheel 不许清 capture");
    expectSwallow(pointer(router, UiPointerPhase::Up, UiPointerButton::Left, 10, 10), true, "拖回来抬起");
    expectClick(router, "a", 10, 10, "拖出去再拖回来仍是同一次点击");

    // ⑤ 右键 / 中键一律放行，且不建立 capture（新的 **左键** down 才动 capture）。
    expectSwallow(pointer(router, UiPointerPhase::Down, UiPointerButton::Right, 10, 10), false,
                  "右键 down 必须放行");
    expectActive(router, "", "右键不建立 capture");
    expectSwallow(pointer(router, UiPointerPhase::Up, UiPointerButton::Right, 10, 10), false,
                  "右键 up 必须放行");
    expectSwallow(pointer(router, UiPointerPhase::Down, UiPointerButton::Middle, 10, 10), false,
                  "中键 down 必须放行");
    expectSwallow(pointer(router, UiPointerPhase::Up, UiPointerButton::Middle, 10, 10), false,
                  "中键 up 必须放行");
    expectNoClick(router, "右键 / 中键不产生 click");

    // ⑥ 新的左键 down 清 capture：按住 a 时再按 b ⇒ capture 换成 b，抬起只发 b。
    expectSwallow(pointer(router, UiPointerPhase::Down, UiPointerButton::Left, 10, 10), true, "按住 a");
    expectActive(router, "a", "按住 a");
    expectSwallow(pointer(router, UiPointerPhase::Down, UiPointerButton::Left, 10, 40), true,
                  "按住 a 时再按 b：命中 b");
    expectActive(router, "b", "新的 down 把 capture 换成 b");
    expectSwallow(pointer(router, UiPointerPhase::Up, UiPointerButton::Left, 10, 40), true, "抬起 b");
    expectClick(router, "b", 10, 40, "新的 down 之后只发 b 的 click");
    expectNoClick(router, "a 的 click 不会补发");

    // ⑧ 失焦 ⇒ 清 capture（**取消这次点击**），但这次 down 是我们接的 ⇒ up 仍须吞掉
    //    （⛔ 不让 guest 收到一个没有 down 的 up：菜单类界面常在 up 上触发）。
    expectSwallow(pointer(router, UiPointerPhase::Down, UiPointerButton::Left, 10, 10), true, "失焦前按住 a");
    router.focusLost();
    expectActive(router, "", "失焦必须清 capture");
    {
        const UiPointerDecision up = pointer(router, UiPointerPhase::Up, UiPointerButton::Left, 10, 10);
        expectSwallow(up, true, "失焦后的 up 仍须吞掉（这次 down 是我们接的）");
        ++lift_test::g_checks;
        if (up.emit_click) lift_test::failf(__FILE__, __LINE__, "失焦取消不许发 click");
    }
    expectNoClick(router, "失焦取消不产生 click");
    // 没有欠着的 down 的 up 必须放行（guest 自己的 down/up 必须对称）。
    expectSwallow(pointer(router, UiPointerPhase::Up, UiPointerButton::Left, 10, 10), false,
                  "没有欠 down 的 up 必须放行");

    // ⑧ 命中表被清掉（叠层关）⇒ 不命中、不吞、没有 hover。
    router.clearFrame();
    expectSwallow(pointer(router, UiPointerPhase::Down, UiPointerButton::Left, 10, 10), false,
                  "命中表清空后 down 必须放行");
    ++lift_test::g_checks;
    if (!router.hoverKey(10, 10).empty()) lift_test::failf(__FILE__, __LINE__, "命中表清空后 hover 必须为空");
}

}  // namespace

int main() {
    caseCapture();
    return lift_test::finish("ui-click-router");
}
