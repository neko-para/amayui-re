// emulator-lift · UI **命中原语**的 pure 判据（`REQ-01M7QF01A00F9946DX0GPH23JQ` 切片 2 后半）
//
// 判据（红了意味着什么，缺一条都不算）：
//   ① `order-and-bounds`：命中表的顺序 = **绘制序**（z 升序、同 z 按构造序），命中 = **逆序**
//      （后画的先命中）。红了 ⇒ 点到了被压住的那个按钮（同一个位置两个 key，行为反了）。
//   ② **resolved rect 的边界**：命中区是**缩放 / 居中偏移之后的窗口像素**矩形（不是参考像素）；
//      左闭右开（`x` 内、`x+w` 外），1 px 内 / 外都要对。红了 ⇒ 高 DPI / 非 16:9 下点偏一格或
//      点不中。
//   ③ **无成功帧 ⇒ 不命中**：从没 `commitFrame`（或 `clearFrame` 之后）⇒ 命中表空、down 不吞。
//      红了 ⇒ 叠层没画出来时点击仍在被吞（guest 收不到输入却也没有任何按钮）。
//   ④ **key 为空 ⇒ 不可命中**：`ui.rect` 不传 `key`（或 `key: ''`）只是画一个矩形；`Text` 即使
//      带了 key 也不是命中区（命中区只有 Rect）。红了 ⇒ 整个 HUD 面板 / 文本都成了按钮。
// ★ 顶层无条件注册 = **pure** 档（AGENTS.md §12.4：判据 = 注册位置）。只 include 纯头（无 SDL / JS）。
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "check.h"
#include "ui/canvas.h"
#include "ui/element.h"
#include "ui/hit.h"

namespace {

using lift::ui::CanvasOrigin;
using lift::ui::ClickRouter;
using lift::ui::HitRegion;
using lift::ui::UiBuilder;
using lift::ui::UiPointerButton;
using lift::ui::UiPointerPhase;

// 追加一条 Rect 并**立刻**写 key（`UiBuilder` 的 vector 会扩容 ⇒ 不要留着引用跨 push）。
void addRect(UiBuilder& builder, const std::string& key, int x, int y, int w, int h, int z = 0) {
    lift::ui::Element& e = builder.rect(x, y, w, h, lift::ui::Color{10, 20, 30, 255}, z);
    e.key = key;
}

void addText(UiBuilder& builder, const std::string& key, int x, int y, const char* text) {
    lift::ui::Element& e = builder.text(x, y, text, 16, lift::ui::Color{255, 255, 255, 255});
    e.key = key;
}

void expectKey(const HitRegion* region, const char* want, const char* what) {
    ++lift_test::g_checks;
    if (region == nullptr) {
        lift_test::failf(__FILE__, __LINE__, "%s: 期望命中 `%s`，实际没命中", what, want);
        return;
    }
    if (region->key != want) {
        lift_test::failf(__FILE__, __LINE__, "%s: 期望命中 `%s`，实际 `%s`", what, want, region->key.c_str());
    }
}

void expectMiss(const HitRegion* region, const char* what) {
    ++lift_test::g_checks;
    if (region != nullptr) {
        lift_test::failf(__FILE__, __LINE__, "%s: 期望不命中，实际命中 `%s`", what, region->key.c_str());
    }
}

// ---------------------------------------------------------------------------------------
// ① + ② + ④ 逆序命中 / resolved rect 边界 / key 为空不可命中
// ---------------------------------------------------------------------------------------
void caseOrderAndBounds() {
    lift_test::g_current_case = "ui-hit/order-and-bounds";

    // ---- ① 绘制序（构造序）与逆序命中 ----
    {
        UiBuilder builder;
        addRect(builder, "a", 0, 0, 10, 10);
        addRect(builder, "b", 5, 5, 10, 10);  // 后画：重叠区里它在上层
        addRect(builder, "", 20, 20, 10, 10);  // 没 key ⇒ 只是背景
        addText(builder, "t", 0, 0, "not-a-hit-region");
        // 命中表：只收 key 非空的 Rect，顺序 = 绘制序。
        const std::vector<HitRegion> hits = lift::ui::buildHitRegions(
            builder.elements(), CanvasOrigin{1.0, 0, 0}, 0, 0);
        ++lift_test::g_checks;
        if (hits.size() != 2) {
            lift_test::failf(__FILE__, __LINE__, "命中表应只有 2 条（Text / 无 key 的 Rect 不进表），实际 %zu",
                             hits.size());
        }
        ++lift_test::g_checks;
        if (hits.size() == 2 && (hits[0].key != "a" || hits[1].key != "b")) {
            lift_test::failf(__FILE__, __LINE__, "命中表顺序应是绘制序 [a, b]，实际 [%s, %s]",
                             hits[0].key.c_str(), hits[1].key.c_str());
        }
        expectKey(lift::ui::hitTest(hits, 7, 7), "b", "重叠区里后画的先命中");
        expectKey(lift::ui::hitTest(hits, 1, 1), "a", "只有 a 覆盖的点命中 a");
        expectKey(lift::ui::hitTest(hits, 4, 4), "a", "b 的左边界 (5,5) 之外仍命中 a");
        expectMiss(lift::ui::hitTest(hits, 50, 50), "空白处不命中");
        // 左闭右开：b 的右/下边界是 (15,15)，所以 14 在内、15 在外（a 只到 9）。
        expectKey(lift::ui::hitTest(hits, 14, 14), "b", "rect 最后一像素 (x+w-1) 在命中区内");
        expectMiss(lift::ui::hitTest(hits, 15, 14), "rect 右边界 (x+w) 不在命中区内");
    }

    // ---- ① z 参与绘制序（与 rasterize 同一份比较）----
    {
        UiBuilder builder;
        addRect(builder, "high-z", 0, 0, 10, 10, /*z=*/5);
        addRect(builder, "low-z", 0, 0, 10, 10, /*z=*/1);  // 构造在后，但 z 小 ⇒ 先画
        const std::vector<HitRegion> hits = lift::ui::buildHitRegions(
            builder.elements(), CanvasOrigin{1.0, 0, 0}, 0, 0);
        expectKey(lift::ui::hitTest(hits, 5, 5), "high-z", "z 大的后画 ⇒ 先命中");
    }

    // ---- ② resolved rect：缩放 / 画布偏移 / 画面原点之后的窗口像素 ----
    {
        UiBuilder builder;
        addRect(builder, "scaled", 10, 20, 5, 6);
        // x = picture_x(1) + offset_x(3) + round(10*0.5)=5  ⇒ 9
        // y = picture_y(2) + offset_y(4) + round(20*0.5)=10 ⇒ 16
        // w = round(5*0.5)=3、h = round(6*0.5)=3（min 1 由 scaleRef 保证）
        const std::vector<HitRegion> hits = lift::ui::buildHitRegions(
            builder.elements(), CanvasOrigin{0.5, 3, 4}, 1, 2);
        ++lift_test::g_checks;
        if (hits.size() != 1) {
            lift_test::failf(__FILE__, __LINE__, "resolved 用例应恰好 1 条，实际 %zu", hits.size());
        }
        if (hits.size() == 1) {
            ++lift_test::g_checks;
            if (hits[0].x != 9 || hits[0].y != 16 || hits[0].w != 3 || hits[0].h != 3) {
                lift_test::failf(__FILE__, __LINE__, "resolved rect 应是 (9,16,3,3)，实际 (%d,%d,%d,%d)",
                                 hits[0].x, hits[0].y, hits[0].w, hits[0].h);
            }
        }
        expectKey(lift::ui::hitTest(hits, 9, 16), "scaled", "resolved 左上角在命中区内");
        expectKey(lift::ui::hitTest(hits, 11, 18), "scaled", "resolved 右下角 1 px 内");
        expectMiss(lift::ui::hitTest(hits, 12, 18), "resolved 右边界 1 px 外");
        expectMiss(lift::ui::hitTest(hits, 11, 19), "resolved 下边界 1 px 外");
        // 几何不可用（scale == 0）⇒ 空表（⛔ 不猜一个默认缩放）。
        ++lift_test::g_checks;
        if (!lift::ui::buildHitRegions(builder.elements(), CanvasOrigin{}, 0, 0).empty()) {
            lift_test::failf(__FILE__, __LINE__, "scale == 0 时命中表必须为空");
        }
    }

    // ---- ④ key 为空 / 只有 Text ⇒ 不可命中 ----
    {
        UiBuilder builder;
        addRect(builder, "", 0, 0, 100, 100);
        addRect(builder, "", 0, 0, 100, 100);
        addText(builder, "text-key", 0, 0, "x");
        const std::vector<HitRegion> hits = lift::ui::buildHitRegions(
            builder.elements(), CanvasOrigin{1.0, 0, 0}, 0, 0);
        ++lift_test::g_checks;
        if (!hits.empty()) {
            lift_test::failf(__FILE__, __LINE__, "key 为空 / Text 不许进命中表，实际 %zu 条", hits.size());
        }
        expectMiss(lift::ui::hitTest(hits, 10, 10), "没有 key 的矩形不可命中");
    }

    // ---- ③ 无成功帧 ⇒ 不命中（down 不吞）----
    {
        ClickRouter router;
        ++lift_test::g_checks;
        if (router.hasFrame()) lift_test::failf(__FILE__, __LINE__, "新建的 router 不应有成功帧");
        ++lift_test::g_checks;
        if (router.handlePointer(UiPointerPhase::Down, UiPointerButton::Left, 0, 0).swallow) {
            lift_test::failf(__FILE__, __LINE__, "没有成功帧时左键 down 不许被吞");
        }
        ++lift_test::g_checks;
        if (!router.hoverKey(0, 0).empty()) lift_test::failf(__FILE__, __LINE__, "没有成功帧时 hover 必须为空");
        // 有帧 → 清帧（叠层关掉）之后同样不命中。
        std::vector<HitRegion> committed;
        committed.push_back(HitRegion{"a", 0, 0, 10, 10});
        router.commitFrame(std::move(committed));
        router.clearFrame();
        ++lift_test::g_checks;
        if (router.handlePointer(UiPointerPhase::Down, UiPointerButton::Left, 5, 5).swallow) {
            lift_test::failf(__FILE__, __LINE__, "clearFrame 之后左键 down 不许被吞");
        }
    }
}

}  // namespace

int main() {
    caseOrderAndBounds();
    return lift_test::finish("ui-hit");
}
