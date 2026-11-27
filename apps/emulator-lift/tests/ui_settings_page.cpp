// emulator-lift · 二级设置页 `scripts/ui/settings-page.js` 的 pure 判据
// （父级裁决 `.tmp/settings-overlay/decision.md` D2/D4；S6）
//
// 判据（红了意味着什么，缺一条都不算）：
//   ① **版式 / 命中 key**：2 个域 3 项 ⇒ `SETTINGS` + live 行 + 每域小节标题 + 每项一行；
//      bool 行有一个整行 `set:<domain>:<key>` 命中区（26px 高），number 行有 `dec:` / `inc:` 两个
//      24×24 按钮（**没有**整行热区）；`BACK` = 112×28 / `settings.back`；全屏背板 `settings.bg`
//      是 **elements[0]**（先画 = 模态）。红了 ⇒ 设置页没画全 / 命中键拼错 / 背板不再吞点击。
//   ② **点击 bool 行**：`set` **恰好一次**、值取反、回执 applied ⇒ **同一帧**页面就显示 `on`。
//      红了 ⇒ 点击被重复消费 / 值没写 / 写了但页面仍读缓存（违反 D3 的读点）。
//   ③ **number**：`inc` 越过 `max` ⇒ 写回 `max`；`dec` 越过 `min` ⇒ 写回 `min`；非法存储值 ⇒
//      显示 `<invalid>`，第一次 `inc` 写回 `default + step`。红了 ⇒ 页面把越界值/坏值直接 set 回盘上。
//   ④ **失败不冒出 render**：`set` 抛错 ⇒ `drawPage` 不抛、返回 `{status}` 且带 `settings:` 前缀；
//      `applied:false, code:"read_only"` ⇒ 状态行 `settings: 只读`。
//   ⑤ **没有桥就不打开**：`h.config === undefined` ⇒ `canOpen(h) === false`，`drawPage` 返回
//      `{closed:true, status:"没有插件在跑"}` 且**不画控件**（D2：预设模式没有 config）。
//   ⑥ **12 行上限**：13 项 ⇒ 只画 12 行（item12 不在）+ 一行 `… 还有 1 项`。
//   ⑦ **任意位置右键 = 返回**：`h.rightClick === true` ⇒ `drawPage` 返回 `closed:true`，即使同帧带着
//      一个 `set:<domain>:<key>` 的左键载荷也**一个 set 都不许**发生（左键逻辑由 ② 继续覆盖）。
//   ⑧ **归属前缀**：schema 带 `owner` ⇒ 小节标题 `<owner> · <title>`；无 `owner` ⇒ 保持旧 `<title>`
//      （⛔ 不回退成 domain id）。
//
// ★ 夹具 = **inline module 入口**（与 `ui_kit_script.cpp` 同形）：`import { canOpen, drawPage } from
//   './settings-page.js'` ⇒ 经**现有 module loader** 加载**发行件**（改 settings-page.js / kit.js
//   这条用例立刻看见）。假 `h` 的 `measure/rect/text` 直接转发到 `ui.*`，`config` 是纯 JS 假桥
//   （记录 set / 可编排回执与抛错）。`game.frame` 选用例（每帧一条，输出用 `ui.text` 带出来）。
// ★ 零宿主字体（`measure` 由假 `h` 给固定值）⇒ 无 SKIP、无窗口、无安装树。
// ★ **顶层无条件** `add_test`（CMake 里在顶层 `if(LIFT_BUILD_TESTS)`）⇒ **pure** 档
//   （`AGENTS.md` §12.4：判据 = 注册位置）。
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "check.h"
#include "ui/element.h"
#include "ui/script.h"

namespace {

using lift::ui::Element;
using lift::ui::ElementKind;
using lift::ui::UiBuilder;
using lift::ui::UiFrameReadings;
using lift::ui::UiScript;

std::string entryOrigin() { return std::string(LIFT_UI_SCRIPT_DIR) + "/settings-page-test-entry.js"; }

// ---- 判据里用的小工具（都在 display list 上做，⛔ 不靠截图）-------------------------------
const Element* rectByKey(const UiBuilder& b, const std::string& key) {
    for (const Element& e : b.elements()) {
        if (e.kind == ElementKind::Rect && e.key == key) return &e;
    }
    return nullptr;
}

bool hasText(const UiBuilder& b, const std::string& text) {
    for (const Element& e : b.elements()) {
        if (e.kind == ElementKind::Text && e.text == text) return true;
    }
    return false;
}

bool hasTextContaining(const UiBuilder& b, const std::string& needle) {
    for (const Element& e : b.elements()) {
        if (e.kind == ElementKind::Text && e.text.find(needle) != std::string::npos) return true;
    }
    return false;
}

std::string textContaining(const UiBuilder& b, const std::string& needle) {
    for (const Element& e : b.elements()) {
        if (e.kind == ElementKind::Text && e.text.find(needle) != std::string::npos) return e.text;
    }
    return {};
}

void dumpTexts(const UiBuilder& b) {
    for (const Element& e : b.elements()) {
        if (e.kind == ElementKind::Text) std::fprintf(stderr, "  text: `%s`\n", e.text.c_str());
        else std::fprintf(stderr, "  rect: key=`%s` (%d,%d,%d,%d)\n", e.key.c_str(), e.x, e.y, e.w, e.h);
    }
}

void expectText(const UiBuilder& b, const std::string& text, const char* what) {
    ++lift_test::g_checks;
    if (!hasText(b, text)) {
        lift_test::failf(__FILE__, __LINE__, "%s：缺少文本 `%s`", what, text.c_str());
        dumpTexts(b);
    }
}

void expectTextContaining(const UiBuilder& b, const std::string& needle, const char* what) {
    ++lift_test::g_checks;
    if (!hasTextContaining(b, needle)) {
        lift_test::failf(__FILE__, __LINE__, "%s：没有含 `%s` 的文本", what, needle.c_str());
        dumpTexts(b);
    }
}

void expectNoText(const UiBuilder& b, const std::string& text, const char* what) {
    ++lift_test::g_checks;
    if (hasText(b, text)) lift_test::failf(__FILE__, __LINE__, "%s：不许出现文本 `%s`", what, text.c_str());
}

void expectRect(const UiBuilder& b, const std::string& key, const char* what) {
    ++lift_test::g_checks;
    const Element* e = rectByKey(b, key);
    if (e == nullptr) {
        lift_test::failf(__FILE__, __LINE__, "%s：缺少命中区 `%s`", what, key.c_str());
        dumpTexts(b);
    }
}

bool renderFrame(UiScript& script, std::uint64_t frame, UiBuilder* out) {
    UiFrameReadings r;
    r.frame = frame;
    std::string err;
    const bool ok = script.render(*out, r, &err);
    ++lift_test::g_checks;
    if (!ok) lift_test::failf(__FILE__, __LINE__, "frame %llu render 失败：%s",
                              static_cast<unsigned long long>(frame), err.c_str());
    return ok;
}

// ---------------------------------------------------------------------------------------
// inline module 入口：只多一句 import；`game.frame` 选一条用例，结果用 `ui.text`（size 1）带出来。
// ---------------------------------------------------------------------------------------
const char* kEntry = R"JS(
import { canOpen, drawPage } from './settings-page.js';

function measure(text, size) {
  return { w: String(text).length * (size >= 16 ? 8 : 6), h: 18 };
}
function paintRect(o) { ui.rect(o); }
function paintText(o) { ui.text(o); }
function note(s) { ui.text({ x: 0, y: 0, text: s, size: 1, color: '#FFFFFFFF' }); }

function makeHost(opts) {
  var store = opts.store;
  var sets = opts.sets;
  var config = {
    available: true,
    domain: function (id) {
      return {
        get: function (k, dflt) {
          var d = store[id];
          if (d === undefined || d[k] === undefined) return dflt;
          return d[k];
        },
        has: function (k) {
          var d = store[id];
          return d !== undefined && d[k] !== undefined;
        },
        set: function (k, v) {
          if (opts.throwSet !== undefined) throw new Error(opts.throwSet);
          sets.push(id + '.' + k + '=' + String(v));
          if (store[id] === undefined) store[id] = {};
          store[id][k] = v;
          return opts.receipt === undefined ? { applied: true } : opts.receipt;
        },
        delete: function (k) { return false; },
        all: function () {
          var d = store[id];
          return d === undefined ? {} : d;
        }
      };
    }
  };
  return {
    measure: measure, rect: paintRect, text: paintText,
    hover: undefined, active: undefined, click: opts.click,
    rightClick: opts.rightClick, config: config
  };
}

function storeWith(schema, values) {
  var store = { 'settings.schema': schema };
  for (var k in values) store[k] = values[k];
  return store;
}

var SCHEMA_2 = {
  // ★ d1 带 owner（插件名）⇒ 小节标题 `<owner> · <title>`；d2 无 owner ⇒ 回退旧 `<title>`。
  'd1': { owner: 'field-point-visible', title: 'FIELD points', live: 'run', entries: {
    'flag': { type: 'bool', label: 'flag label', default: false }
  }},
  'd2': { title: 'Domain Two', live: 'restart', entries: {
    'count': { type: 'number', label: 'count label', default: 5, min: 0, max: 10, step: 2 },
    'extra': { type: 'bool', label: 'extra label', default: true }
  }}
};

function schemaSolo() {
  return { 'solo': { title: 'Solo', live: 'run', entries: {
    'flag': { type: 'bool', label: 'flag label', default: false }
  }}};
}
function schemaNum() {
  return { 'num': { title: 'Num', live: 'run', entries: {
    'count': { type: 'number', label: 'count label', default: 5, min: 0, max: 10, step: 2 }
  }}};
}

function caseCanOpen() {
  note('canopen-undefined=' + (canOpen({ config: undefined }) ? 1 : 0));
  note('canopen-null=' + (canOpen(null) ? 1 : 0));
  note('canopen-available=' + (canOpen(makeHost({ store: {}, sets: [] })) ? 1 : 0));
  var r = drawPage({ measure: measure, rect: paintRect, text: paintText, config: undefined }, {});
  note('closed-no-config=' + (r.closed === true ? 1 : 0));
  note('status-no-config=' + String(r.status));
}

function caseLayout() {
  var sets = [];
  var host = makeHost({
    store: storeWith(SCHEMA_2, { 'd1': { 'flag': false }, 'd2': { 'count': 5, 'extra': true } }),
    sets: sets
  });
  drawPage(host, {});
  note('sets=' + sets.length);
}

function caseBoolToggle() {
  var sets = [];
  var store = storeWith(schemaSolo(), { 'solo': { 'flag': false } });
  drawPage(makeHost({ store: store, sets: sets, click: { key: 'set:solo:flag', x: 4, y: 4 } }), {});
  note('sets=' + sets.length);
  note('set0=' + (sets.length > 0 ? sets[0] : 'none'));
  note('store=' + (store['solo']['flag'] === true ? 'true' : 'false'));
}

function caseClampInc() {
  var sets = [];
  drawPage(makeHost({
    store: storeWith(schemaNum(), { 'num': { 'count': 10 } }), sets: sets,
    click: { key: 'inc:num:count', x: 1, y: 1 }
  }), {});
  note('sets=' + sets.length);
  note('set0=' + (sets.length > 0 ? sets[0] : 'none'));
}

function caseClampDec() {
  var sets = [];
  drawPage(makeHost({
    store: storeWith(schemaNum(), { 'num': { 'count': 0 } }), sets: sets,
    click: { key: 'dec:num:count', x: 1, y: 1 }
  }), {});
  note('sets=' + sets.length);
  note('set0=' + (sets.length > 0 ? sets[0] : 'none'));
}

function caseInvalidIdle() {
  var sets = [];
  drawPage(makeHost({ store: storeWith(schemaNum(), { 'num': { 'count': 'oops' } }), sets: sets }), {});
  note('sets=' + sets.length);
}

function caseInvalidInc() {
  var sets = [];
  drawPage(makeHost({
    store: storeWith(schemaNum(), { 'num': { 'count': 'oops' } }), sets: sets,
    click: { key: 'inc:num:count', x: 1, y: 1 }
  }), {});
  note('sets=' + sets.length);
  note('set0=' + (sets.length > 0 ? sets[0] : 'none'));
}

function caseThrow() {
  var sets = [];
  var threw = 0;
  var status = '';
  try {
    var r = drawPage(makeHost({
      store: storeWith(schemaNum(), { 'num': { 'count': 5 } }), sets: sets, throwSet: 'boom',
      click: { key: 'inc:num:count', x: 1, y: 1 }
    }), {});
    status = String(r.status);
  } catch (e) {
    threw = 1;
  }
  note('threw=' + threw);
  note('status=' + status);
}

function caseReadOnly() {
  var sets = [];
  var r = drawPage(makeHost({
    store: storeWith(schemaNum(), { 'num': { 'count': 5 } }), sets: sets,
    receipt: { applied: false, code: 'read_only' },
    click: { key: 'inc:num:count', x: 1, y: 1 }
  }), {});
  note('status=' + String(r.status));
}

function schemaMany() {
  var entries = {};
  for (var i = 0; i < 13; i += 1) {
    entries['k' + i] = { type: 'bool', label: 'item' + i, default: false };
  }
  return { 'many': { title: 'Many', live: 'run', entries: entries } };
}

function caseOverflow() {
  var sets = [];
  drawPage(makeHost({ store: storeWith(schemaMany(), {}), sets: sets }), {});
  note('sets=' + sets.length);
}

function caseBadShapes() {
  var sets = [];
  var schema = {
    'bad': null,
    'worse': { title: 'Worse' },
    'ok': { title: 'Ok', live: 'run', entries: {
      'fine': { type: 'bool', label: 'fine label', default: true }
    }}
  };
  drawPage(makeHost({ store: storeWith(schema, {}), sets: sets }), {});
  note('done=1');
}

function caseRightClick() {
  var sets = [];
  var store = storeWith(schemaSolo(), { 'solo': { 'flag': false } });
  // ★ 同帧还给一个**左键 set** 的载荷：右键必须赢（先于 `h.click`），且一个 set 都不许发生。
  var r = drawPage(makeHost({
    store: store, sets: sets, rightClick: true,
    click: { key: 'set:solo:flag', x: 4, y: 4 }
  }), {});
  note('closed=' + (r.closed === true ? 1 : 0));
  note('sets=' + sets.length);
  note('store=' + (store['solo']['flag'] === true ? 'true' : 'false'));
}

globalThis.render = function render() {
  var f = game.frame;
  if (f === 1) return caseCanOpen();
  if (f === 2) return caseLayout();
  if (f === 3) return caseBoolToggle();
  if (f === 4) return caseClampInc();
  if (f === 5) return caseClampDec();
  if (f === 6) return caseInvalidIdle();
  if (f === 7) return caseInvalidInc();
  if (f === 8) return caseThrow();
  if (f === 9) return caseReadOnly();
  if (f === 10) return caseOverflow();
  if (f === 11) return caseBadShapes();
  if (f === 12) return caseRightClick();
};
)JS";

}  // namespace

int main() {
    using namespace lift_test;

    auto runtime = lift::ui::makeQuickJsRuntime();
    LIFT_CHECK(runtime != nullptr);
    if (runtime == nullptr) return finish("ui-settings-page");
    std::string err;
    std::unique_ptr<UiScript> script = runtime->load(kEntry, entryOrigin(), nullptr, &err);
    LIFT_CHECK(script != nullptr);
    if (!script) {
        std::fprintf(stderr, "加载设置页失败：%s\n", err.c_str());
        return finish("ui-settings-page");
    }

    // ---- ① 版式 / 命中 key（2 域 3 项）----
    {
        g_current_case = "ui-settings-page/layout";
        UiBuilder b;
        if (renderFrame(*script, 2, &b)) {
            expectText(b, "sets=0", "layout");
            // 背板 = elements[0]（先画，模态）。
            LIFT_CHECK(!b.elements().empty());
            if (!b.elements().empty()) {
                const Element& backdrop = b.elements().front();
                ++g_checks;
                if (backdrop.kind != ElementKind::Rect || backdrop.key != "settings.bg") {
                    failf(__FILE__, __LINE__, "elements[0] 应是 key=settings.bg 的背板（先画）");
                }
                LIFT_CHECK_EQ_I64(backdrop.w, 1280);
                LIFT_CHECK_EQ_I64(backdrop.h, 720);
                LIFT_CHECK_EQ_U32(backdrop.fill.a, 166u);  // #000000A6
            }
            expectRect(b, "set:d1:flag", "bool 行的整行命中区");
            expectRect(b, "set:d2:extra", "第二个 bool 行");
            expectRect(b, "dec:d2:count", "number 的 [−]");
            expectRect(b, "inc:d2:count", "number 的 [+]");
            expectRect(b, "settings.back", "BACK");
            // 版式：bool 行 26px 高、x=PAD(8)、w = 页板 - 2*PAD；BACK = 112×28。
            const Element* row = rectByKey(b, "set:d1:flag");
            const Element* back = rectByKey(b, "settings.back");
            // 页板 = 第一条**无 key** 的 Rect（背板之后）。
            const Element* panel = nullptr;
            for (const Element& e : b.elements()) {
                if (e.kind == ElementKind::Rect && e.key.empty()) {
                    panel = &e;
                    break;
                }
            }
            LIFT_CHECK(panel != nullptr);
            if (row != nullptr && panel != nullptr) {
                LIFT_CHECK_EQ_I64(row->h, 26);
                LIFT_CHECK_EQ_I64(row->x, 8);
                LIFT_CHECK_EQ_I64(row->w, panel->w - 16);
            }
            if (back != nullptr) {
                LIFT_CHECK_EQ_I64(back->w, 112);
                LIFT_CHECK_EQ_I64(back->h, 28);
            }
            expectText(b, "SETTINGS", "标题");
            expectText(b, "live: 正在运行的插件当拍生效；其余为预配置", "live 固定文案");
            expectTextContaining(b, "field-point-visible · FIELD points", "带 owner 的域小节标题");
            // ★ 无 owner 的旧域：标题仍是 `<title>`（⛔ 不回退成 domain id）。
            expectText(b, "Domain Two  [restart]", "无 owner 的域标题保持旧形状");
            expectText(b, "flag label", "bool 标签");
            expectText(b, "count label", "number 标签");
            expectText(b, "extra label", "bool 标签 2");
            expectText(b, "5", "number 值");
            expectText(b, "on", "bool 值 on");
            expectText(b, "off", "bool 值 off");
            expectText(b, "BACK", "BACK 标签");
            // number 行**没有**整行热区（只有 dec/inc 两个按钮）。
            ++g_checks;
            if (rectByKey(b, "set:d2:count") != nullptr) {
                failf(__FILE__, __LINE__, "number 行不许有整行热区 `set:d2:count`");
            }
        }
    }

    // ---- ② 点 bool 行：set 恰好一次、值取反、同帧显示 on ----
    {
        g_current_case = "ui-settings-page/bool-toggle";
        UiBuilder b;
        if (renderFrame(*script, 3, &b)) {
            expectText(b, "sets=1", "set 恰好一次");
            expectText(b, "set0=solo.flag=true", "写回取反后的值");
            expectText(b, "store=true", "假 store 真的变了");
            expectText(b, "on", "同帧显示新值");
            expectNoText(b, "off", "同帧不该还显示旧值");
        }
    }

    // ---- ③ number clamp / 非法值 ----
    {
        g_current_case = "ui-settings-page/number-clamp-inc";
        UiBuilder b;
        if (renderFrame(*script, 4, &b)) {
            expectText(b, "sets=1", "inc 恰好一次");
            expectText(b, "set0=num.count=10", "inc 越过 max ⇒ 写回 max");
        }
    }
    {
        g_current_case = "ui-settings-page/number-clamp-dec";
        UiBuilder b;
        if (renderFrame(*script, 5, &b)) {
            expectText(b, "sets=1", "dec 恰好一次");
            expectText(b, "set0=num.count=0", "dec 越过 min ⇒ 写回 min");
        }
    }
    {
        g_current_case = "ui-settings-page/number-invalid-idle";
        UiBuilder b;
        if (renderFrame(*script, 6, &b)) {
            expectText(b, "sets=0", "非法值不点不写");
            expectText(b, "<invalid>", "非法存储值显示 <invalid>");
        }
    }
    {
        g_current_case = "ui-settings-page/number-invalid-inc";
        UiBuilder b;
        if (renderFrame(*script, 7, &b)) {
            expectText(b, "sets=1", "非法值 + inc 写一次");
            expectText(b, "set0=num.count=7", "第一次 inc 写回 default + step");
        }
    }

    // ---- ④ set 抛错 / 只读：不冒到 render 外，返回 status ----
    {
        g_current_case = "ui-settings-page/set-throws";
        UiBuilder b;
        if (renderFrame(*script, 8, &b)) {
            expectText(b, "threw=0", "drawPage 不许抛");
            const std::string status = textContaining(b, "settings:");
            // `status=` 诊断行本身含 "settings:"；取的是**值**那一行（`status=settings: ...`）。
            const std::string diag = textContaining(b, "status=");
            ++g_checks;
            if (diag.find("boom") == std::string::npos || diag.find("settings:") == std::string::npos) {
                failf(__FILE__, __LINE__, "status 应是 `settings: <message>`（拿到 `%s`）", diag.c_str());
            }
            (void)status;
        }
    }
    {
        g_current_case = "ui-settings-page/set-read-only";
        UiBuilder b;
        if (renderFrame(*script, 9, &b)) {
            const std::string diag = textContaining(b, "status=");
            ++g_checks;
            if (diag.find("settings: 只读") == std::string::npos) {
                failf(__FILE__, __LINE__, "applied:false/read_only ⇒ `settings: 只读`（拿到 `%s`）", diag.c_str());
            }
        }
    }

    // ---- ⑤ h.config === undefined ⇒ 不打开 ----
    {
        g_current_case = "ui-settings-page/no-config";
        UiBuilder b;
        if (renderFrame(*script, 1, &b)) {
            expectText(b, "canopen-undefined=0", "h.config === undefined ⇒ canOpen=false");
            expectText(b, "canopen-null=0", "h 为 null ⇒ canOpen=false");
            expectText(b, "canopen-available=1", "有 config + available ⇒ canOpen=true");
            expectText(b, "closed-no-config=1", "不可用 ⇒ drawPage 返回 closed");
            expectText(b, "status-no-config=没有插件在跑", "不可用 ⇒ 状态行");
            ++g_checks;
            if (hasTextContaining(b, "SETTINGS")) {
                failf(__FILE__, __LINE__, "没有 config 时不许画设置页（假开关）");
            }
        }
    }

    // ---- ⑥ 13 项 ⇒ 12 行 + `… 还有 1 项` ----
    {
        g_current_case = "ui-settings-page/row-limit";
        UiBuilder b;
        if (renderFrame(*script, 10, &b)) {
            for (int i = 0; i < 12; ++i) {
                const std::string label = "item" + std::to_string(i);
                expectText(b, label, "前 12 项要画出来");
            }
            expectNoText(b, "item12", "第 13 项不许画");
            expectText(b, "… 还有 1 项", "溢出行");
        }
    }

    // ---- schema 坏形状 ⇒ 跳过该域（不抛）----
    {
        g_current_case = "ui-settings-page/bad-schema-shapes";
        UiBuilder b;
        if (renderFrame(*script, 11, &b)) {
            expectText(b, "done=1", "坏形状不抛");
            expectText(b, "fine label", "好形状的域照画");
        }
    }

    // ---- ⑦ 任意位置右键 = 返回（即使同一帧有一个 set 点击）----
    {
        g_current_case = "ui-settings-page/right-click-back";
        UiBuilder b;
        if (renderFrame(*script, 12, &b)) {
            expectText(b, "closed=1", "右键 ⇒ drawPage 返回 closed");
            expectText(b, "sets=0", "右键不许触发任何 set");
            expectText(b, "store=false", "右键不许改 store");
            // 左键点击控件仍照旧：`ui-settings-page/bool-toggle`（frame 3）已断言 set 一次 + 同帧 on。
        }
    }

    script->shutdown();
    return finish("ui-settings-page");
}
