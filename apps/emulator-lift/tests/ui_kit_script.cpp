// emulator-lift · JS 组件库 `scripts/ui/kit.js` 的 pure 判据（`REQ-01M7QF01A00F9946DX0GPH23JQ` 切片 2 后半）
//
// 判据（红了意味着什么）：
//   ① `button-states`：经**现有 module loader**（`ScriptRuntime::load`，不是另一条读文件路径）
//      加载**发行件** `scripts/ui/kit.js`（入口是一段 `import { Button } from './kit.js'` 的
//      inline module）⇒ `Button({key,rect,label,on})` 在 idle / hover / active 三态产出**不同**的
//      display list（同一个 key 的 Rect 几何不变、填充色三态互不相同），`disabled` 的按钮
//      **没有 key**（= 不可命中）。
//      红了 ⇒ 组件库没跟着 module loader 跑起来 / 状态机不认 `game.hover|active` / 悬停高亮
//      分支失效（三态里有两态同色）/ 禁用件仍然吃点击。
//   ② **hit list 的形状**：`buildHitRegions` 从同一份 display list 拿到**恰好一条**命中区
//      （key + resolved rect 与 `rect` 入参一致）；disabled 那条不进命中表。
//      红了 ⇒ 组件画出来的矩形与命中区是两套（`key` 没挂在 `ui.rect` 上）。
//   ③ **事件钩子**：`game.click` 一次性对象带着这个 key 出现时，`on.click(ev)` 被调用一次
//      （ev.key / ev.x / ev.y 原样）；`on.hover(bool)` 每帧报告是否悬停。
//      红了 ⇒ 组件库没有把宿主的命中原语接到自己的回调上。
//
// ★ 入口源码是**内联**的（不是新的夹具文件）：它只多一句 `import`；`scripts/ui/kit.js` 本身是
//   发行件（`LIFT_UI_SCRIPT_DIR`，与 `hud.js` 同目录）—— 改 kit.js ⇒ 这条用例立刻看见。
// ★ 宿主字体 = nullptr：Button 的几何由调用方给（w/h），所以**不需要** `ui.measure`；
//   若 kit 擅自去量字，这条会红（渲染失败并打印原因）—— 那正是"不做第二套布局引擎"的判据之一。
// ★ 顶层无条件注册 = **pure** 档（AGENTS.md §12.4：判据 = 注册位置）。
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "check.h"
#include "ui/element.h"
#include "ui/hit.h"
#include "ui/script.h"

namespace {

using lift::ui::Element;
using lift::ui::ElementKind;
using lift::ui::UiBuilder;
using lift::ui::UiFrameReadings;
using lift::ui::UiScript;

// 发行件目录（CMake 注入；`hud.js` / `kit.js` 都在这儿 —— 组件库是发行件，⛔ 不是测试夹具）。
std::string kitEntryOrigin() { return std::string(LIFT_UI_SCRIPT_DIR) + "/kit-test-entry.js"; }

// 入口：module 语法（`import` 让 classic 编译探针失败 ⇒ 走 module loader）；入口仍是显式
// `globalThis.render`。`clicks` 不跨帧保存（每帧 render 只报告这一帧的 click）。
const char* kEntry = R"JS(
import { Button } from './kit.js';

globalThis.render = function render() {
  var state = Button({
    key: 'profile',
    rect: { x: 10, y: 20, w: 100, h: 30 },
    label: 'PROFILE',
    on: {
      click: function (ev) {
        ui.text({ x: 0, y: 0, text: 'click=' + ev.key + '@' + ev.x + ',' + ev.y, size: 1 });
      },
      hover: function (on) {
        ui.text({ x: 0, y: 0, text: 'hover-cb=' + on, size: 1 });
      },
    },
  });
  ui.text({ x: 0, y: 0, text: 'state=' + state, size: 1 });
  var off = Button({ key: 'off', rect: { x: 0, y: 60, w: 40, h: 20 }, label: 'OFF', disabled: true });
  ui.text({ x: 0, y: 0, text: 'off=' + off, size: 1 });
};
)JS";

std::unique_ptr<UiScript> loadEntry(lift::ui::ScriptRuntime& runtime) {
    std::string err;
    std::unique_ptr<UiScript> script = runtime.load(kEntry, kitEntryOrigin(), nullptr, &err);
    ++lift_test::g_checks;
    if (!script) {
        lift_test::failf(__FILE__, __LINE__,
                         "加载 `import { Button } from './kit.js'` 失败（module loader / 发行件缺失）：%s",
                         err.c_str());
    }
    return script;
}

bool renderOk(UiScript& script, const UiFrameReadings& readings, UiBuilder* out) {
    std::string err;
    const bool ok = script.render(*out, readings, &err);
    ++lift_test::g_checks;
    if (!ok) lift_test::failf(__FILE__, __LINE__, "render 失败：%s", err.c_str());
    return ok;
}

const Element* findRectByKey(const UiBuilder& builder, const std::string& key) {
    for (const Element& e : builder.elements()) {
        if (e.kind == ElementKind::Rect && e.key == key) return &e;
    }
    return nullptr;
}

const Element* findRectAt(const UiBuilder& builder, int x, int y) {
    for (const Element& e : builder.elements()) {
        if (e.kind == ElementKind::Rect && e.x == x && e.y == y) return &e;
    }
    return nullptr;
}

bool hasText(const UiBuilder& builder, const std::string& text) {
    for (const Element& e : builder.elements()) {
        if (e.kind == ElementKind::Text && e.text == text) return true;
    }
    return false;
}

void expectText(const UiBuilder& builder, const std::string& text, const char* what) {
    ++lift_test::g_checks;
    if (!hasText(builder, text)) {
        std::string got;
        for (const Element& e : builder.elements()) {
            if (e.kind != ElementKind::Text) continue;
            if (!got.empty()) got += " / ";
            got += e.text;
        }
        lift_test::failf(__FILE__, __LINE__, "%s：缺少文本 `%s`（实际 %s）", what, text.c_str(), got.c_str());
    }
}

bool sameColor(const lift::ui::Color& a, const lift::ui::Color& b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

// 一帧的形状：Button 三态共同的部分（几何不变、命中区一条、禁用件没有 key）。
struct Sample {
    lift::ui::Color fill{};
    bool have = false;
};

Sample sampleFrame(UiScript& script, const UiFrameReadings& readings, const char* state_text, const char* what) {
    UiBuilder builder;
    Sample sample;
    if (!renderOk(script, readings, &builder)) return sample;
    expectText(builder, state_text, what);
    const Element* profile = findRectByKey(builder, "profile");
    ++lift_test::g_checks;
    if (profile == nullptr) {
        lift_test::failf(__FILE__, __LINE__, "%s：display list 里没有 key=='profile' 的 Rect（命中区丢了）", what);
        return sample;
    }
    // 几何 = 调用方给的 rect（参考像素解析后的窗口像素；这里 scale=1）。
    ++lift_test::g_checks;
    if (profile->x != 10 || profile->y != 20 || profile->w != 100 || profile->h != 30) {
        lift_test::failf(__FILE__, __LINE__, "%s：按钮几何应是 (10,20,100,30)，实际 (%d,%d,%d,%d)", what,
                         profile->x, profile->y, profile->w, profile->h);
    }
    sample.fill = profile->fill;
    sample.have = true;
    // disabled 的按钮：画出来（有 Rect / Text），但**没有 key** ⇒ 不进命中表。
    const Element* off = findRectAt(builder, 0, 60);
    ++lift_test::g_checks;
    if (off == nullptr) {
        lift_test::failf(__FILE__, __LINE__, "%s：disabled 按钮的 Rect 不见了", what);
    } else {
        ++lift_test::g_checks;
        if (!off->key.empty()) {
            lift_test::failf(__FILE__, __LINE__, "%s：disabled 按钮不许带 key（会吃点击），实际 `%s`", what,
                             off->key.c_str());
        }
    }
    expectText(builder, "off=disabled", what);
    // ② hit list 的形状：恰好一条命中区（profile），resolved rect 与 rect 入参一致。
    const std::vector<lift::ui::HitRegion> hits = lift::ui::buildHitRegions(
        builder.elements(), lift::ui::CanvasOrigin{1.0, 0, 0}, 0, 0);
    ++lift_test::g_checks;
    if (hits.size() != 1) {
        lift_test::failf(__FILE__, __LINE__, "%s：命中表应恰好 1 条（disabled 不进表），实际 %zu", what,
                         hits.size());
    }
    if (hits.size() == 1) {
        ++lift_test::g_checks;
        if (hits[0].key != "profile" || hits[0].x != 10 || hits[0].y != 20 || hits[0].w != 100 ||
            hits[0].h != 30) {
            lift_test::failf(__FILE__, __LINE__, "%s：命中区应是 profile@(10,20,100,30)，实际 `%s`@(%d,%d,%d,%d)",
                             what, hits[0].key.c_str(), hits[0].x, hits[0].y, hits[0].w, hits[0].h);
        }
    }
    return sample;
}

// ---------------------------------------------------------------------------------------
// ① + ② + ③ Button 的三态 / 禁用 / 事件钩子
// ---------------------------------------------------------------------------------------
void caseButtonStates() {
    lift_test::g_current_case = "ui-kit-script/button-states";
    auto runtime = lift::ui::makeQuickJsRuntime();
    std::unique_ptr<UiScript> script = loadEntry(*runtime);
    if (!script) return;

    // idle：没有 hover / active / click。
    UiFrameReadings idle;
    idle.frame = 1;
    const Sample idle_s = sampleFrame(*script, idle, "state=idle", "idle");
    // （fixture 的 hover 回调在 idle 帧写 `hover-cb=false`；这里用单独的一帧取 builder 再断言。）
    {
        UiBuilder builder;
        if (renderOk(*script, idle, &builder)) {
            expectText(builder, "PROFILE", "idle：按钮 label");
            expectText(builder, "hover-cb=false", "idle：on.hover(false)");
        }
    }

    // hover：`game.hover === key` ⇒ 高亮态。
    UiFrameReadings hover;
    hover.frame = 2;
    hover.hover_key = "profile";
    const Sample hover_s = sampleFrame(*script, hover, "state=hover", "hover");
    {
        UiBuilder builder;
        if (renderOk(*script, hover, &builder)) expectText(builder, "hover-cb=true", "hover：on.hover(true)");
    }

    // active：`game.active === key` ⇒ 按下态（优先级高于 hover）。
    UiFrameReadings active;
    active.frame = 3;
    active.hover_key = "profile";
    active.active_key = "profile";
    const Sample active_s = sampleFrame(*script, active, "state=active", "active");

    // ① 三态**互不相同**（hover 高亮分支失效 / active 分支失效都会在这里红）。
    ++lift_test::g_checks;
    if (!idle_s.have || !hover_s.have || !active_s.have) {
        lift_test::failf(__FILE__, __LINE__, "三态里有一帧没画出来 ⇒ 无法比较填充色");
    } else {
        ++lift_test::g_checks;
        if (sameColor(idle_s.fill, hover_s.fill)) {
            lift_test::failf(__FILE__, __LINE__, "hover 填充色与 idle 相同 ⇒ 悬停高亮分支失效");
        }
        ++lift_test::g_checks;
        if (sameColor(active_s.fill, hover_s.fill) || sameColor(active_s.fill, idle_s.fill)) {
            lift_test::failf(__FILE__, __LINE__, "active 填充色必须与 idle / hover 都不同");
        }
    }

    // ③ 一次性 click 对象带着这个 key ⇒ on.click 恰好被调用一次（载荷原样）。
    UiFrameReadings clicked;
    clicked.frame = 4;
    clicked.hover_key = "profile";
    clicked.click_ok = true;
    clicked.click_key = "profile";
    clicked.click_x = 7;
    clicked.click_y = 9;
    {
        UiBuilder builder;
        if (renderOk(*script, clicked, &builder)) {
            expectText(builder, "click=profile@7,9", "on.click(ev) 的载荷");
            int click_lines = 0;
            for (const Element& e : builder.elements()) {
                if (e.kind == ElementKind::Text && e.text.rfind("click=", 0) == 0) ++click_lines;
            }
            ++lift_test::g_checks;
            if (click_lines != 1) {
                lift_test::failf(__FILE__, __LINE__, "on.click 每帧只许调用一次，实际写了 %d 行", click_lines);
            }
        }
    }

    // ③ 别的 key 的 click 不该触发这个按钮的回调。
    UiFrameReadings other;
    other.frame = 5;
    other.click_ok = true;
    other.click_key = "somebody-else";
    other.click_x = 1;
    other.click_y = 2;
    {
        UiBuilder builder;
        if (renderOk(*script, other, &builder)) {
            int click_lines = 0;
            for (const Element& e : builder.elements()) {
                if (e.kind == ElementKind::Text && e.text.rfind("click=", 0) == 0) ++click_lines;
            }
            ++lift_test::g_checks;
            if (click_lines != 0) {
                lift_test::failf(__FILE__, __LINE__, "别的 key 的 click 不许触发本按钮的 on.click");
            }
        }
    }

    // idle 帧的显示 list 里**没有** click 回调那一行（一次性的事实：回调只在带 click 的帧跑）。
    {
        UiBuilder builder;
        if (renderOk(*script, idle, &builder)) {
            ++lift_test::g_checks;
            if (hasText(builder, "click=profile@7,9")) {
                lift_test::failf(__FILE__, __LINE__, "idle 帧不应残留上一帧的 click 回调");
            }
        }
    }

    script->shutdown();
}

}  // namespace

int main() {
    caseButtonStates();
    return lift_test::finish("ui-kit-script");
}
