// emulator-lift · UI 脚本（QuickJS 后端）的**纯 CTest**判据（无 SDL / 无窗口 / 无游戏资产）
//
// 判据（红了意味着什么）：
//   ① sandbox：脚本拿不到编译 / 随机 / 墙钟入口 —— `eval` / `Function` / `Date` / `JSON` /
//      `RegExp` / `Proxy` / `WeakRef` 都是 undefined，`Math.random` 不存在；`eval(...)` 抛
//      ReferenceError，`(function(){}).constructor('return 1')()` 与
//      `globalThis.constructor.constructor('return 1')()` 都抛 TypeError。
//      ★ `Promise` **是 function**（0.18.0 的实测表面；module 求值需要它，用户 2026-11-19 裁决
//        "加而不删" —— 沙箱不是安全边界，⛔ 不为表面最小化删内建）。
//      红了 ⇒ 沙箱洞（脚本能重新编译源码 / 拿到不确定的随机或时间）；`Function` 的
//      `Function.prototype.constructor` 清除是**承重**的（tools/mutate-check.mjs 的
//      `ui-script-quickjs-fn-ctor` 就施加在那里）。
//   ② budget：deadline 到期 / op_limit 用尽 ⇒ `render` false + err 非空 + `overran()` true，
//      且脚本里的 `try/catch` 吞不掉（context §5：不可 catch）；内存超限（大数组）⇒ false + err，
//      **不崩**、`overran()` 仍 false（那是内存面，不是 CPU 预算面）。
//   ③ api：`ui.rect` / `ui.text` 的元素字段逐项正确（坐标 / 尺寸 / 字号 / 颜色 / bold / z / seq），
//      缺省值正确，未知键被忽略；`ui.measure` 走宿主 `FontEngine`（字体不可用时**只跳这一条**）；
//      `font == nullptr` ⇒ TypeError。
//   ④ readings：`game.*` 的 getter 值 / 缺省 `undefined` / 只读（strict 赋值抛 TypeError）/
//      `mouse` / `worst` 每帧新建对象；`guest_ms` 只在 `worst_guest_ok` 时有键。
//      红了 ⇒ 宿主读数没注入 / 用了普通数据属性（脚本能改）/ 用 0 冒充"没有值"。
//   ⑤ error：语法错的 err 含 `origin:line:col`；运行期抛错的 err 含调用点；**同实例下一次
//      `render` 仍成功**（异常已被取走）。红了 ⇒ 脏异常留在 ctx 上（下一次读到上一次的错）。
//   ⑥ entry：没有 `globalThis.render`（或它不是函数）⇒ `load` 失败并点名 origin。红了 ⇒
//      入口约定漂移（overlay 会静默不画）。★ 注意 `JS_EVAL_FLAG_STRICT` 的**全局代码**里
//      `function render(){}` 会挂到 globalThis（实测 v0.18.0，与 v0.10.1 相同）；合同只要求"认
//      globalThis.render 这个属性"，本文件不断言 function 声明挂不上（见 a-report.md 的差异记录）。
// ★ 顶层无条件注册 = **pure** 档（AGENTS.md §12.4：判据 = 注册位置）。
// ★ 只 include 接口头 + FontEngine；⛔ 不 include quickjs.h（那是 script_quickjs.cpp 的特权）。
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "check.h"
#include "runtime/font.h"
#include "ui/element.h"
#include "ui/script.h"

namespace {

using lift::ui::Element;
using lift::ui::ElementKind;
using lift::ui::ScriptRuntime;
using lift::ui::UiBuilder;
using lift::ui::UiFrameReadings;
using lift::ui::UiScript;

std::uint64_t nowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

std::unique_ptr<UiScript> loadOk(ScriptRuntime& runtime, const std::string& source, const std::string& origin,
                                 lift::FontEngine* font = nullptr) {
    std::string err;
    std::unique_ptr<UiScript> script = runtime.load(source, origin, font, &err);
    ++lift_test::g_checks;
    if (!script) lift_test::failf(__FILE__, __LINE__, "load(%s) 失败: %s", origin.c_str(), err.c_str());
    return script;
}

std::vector<std::string> renderTexts(UiScript& script, const UiFrameReadings& readings) {
    UiBuilder builder;
    std::string err;
    const bool ok = script.render(builder, readings, &err);
    ++lift_test::g_checks;
    if (!ok) lift_test::failf(__FILE__, __LINE__, "render 失败: %s", err.c_str());
    std::vector<std::string> texts;
    for (const Element& element : builder.elements()) {
        if (element.kind == ElementKind::Text) texts.push_back(element.text);
    }
    return texts;
}

void expectText(const std::vector<std::string>& texts, std::size_t index, const std::string& want, const char* what) {
    ++lift_test::g_checks;
    if (index >= texts.size()) {
        lift_test::failf(__FILE__, __LINE__, "%s: 缺第 %zu 条 text（共 %zu 条）", what, index, texts.size());
        return;
    }
    if (texts[index] != want) {
        lift_test::failf(__FILE__, __LINE__, "%s: 期望 \"%s\"，得到 \"%s\"", what, want.c_str(),
                         texts[index].c_str());
    }
}

void expectChannels(const lift::ui::Color& color, int r, int g, int b, int a, const char* what) {
    ++lift_test::g_checks;
    if (color.r != r || color.g != g || color.b != b || color.a != a) {
        lift_test::failf(__FILE__, __LINE__, "%s: 期望 (%d,%d,%d,%d)，得到 (%d,%d,%d,%d)", what, r, g, b, a,
                         static_cast<int>(color.r), static_cast<int>(color.g), static_cast<int>(color.b),
                         static_cast<int>(color.a));
    }
}

// ★ 只钉**形状**（"err 里有 `<origin>:<line>:`，且紧跟至少一个十进制数字"），⛔ 不钉上游的列定位精度：
//   同一条语法错 0.10.1 报 `bad.js:2:1`，0.18.0 报 `bad.js:2:9`（上游改进了列定位，不是 API 变化）；
//   `import x from 'y';` 同理 0.10.1 是 `import.js:1:1`，0.18.0 是 `import.js:1:8`。
bool hasOriginLineCol(const std::string& err, const std::string& prefix) {
    const std::size_t at = err.find(prefix);
    if (at == std::string::npos) return false;
    const std::size_t digit = at + prefix.size();
    return digit < err.size() && err[digit] >= '0' && err[digit] <= '9';
}

// =======================================================================================
// ① sandbox
// =======================================================================================
const char* kSandboxScript = R"JS(
globalThis.render = function render() {
  ui.text({ x: 0, y: 0, text: 'eval=' + typeof eval, size: 1 });
  ui.text({ x: 0, y: 0, text: 'Function=' + typeof Function, size: 1 });
  ui.text({ x: 0, y: 0, text: 'Date=' + typeof Date, size: 1 });
  ui.text({ x: 0, y: 0, text: 'performance=' + typeof performance, size: 1 });
  ui.text({ x: 0, y: 0, text: 'JSON=' + typeof JSON, size: 1 });
  ui.text({ x: 0, y: 0, text: 'Promise=' + typeof Promise, size: 1 });
  ui.text({ x: 0, y: 0, text: 'RegExp=' + typeof RegExp, size: 1 });
  ui.text({ x: 0, y: 0, text: 'Proxy=' + typeof Proxy, size: 1 });
  ui.text({ x: 0, y: 0, text: 'Map=' + typeof Map, size: 1 });
  ui.text({ x: 0, y: 0, text: 'Uint8Array=' + typeof Uint8Array, size: 1 });
  ui.text({ x: 0, y: 0, text: 'BigInt=' + typeof BigInt, size: 1 });
  ui.text({ x: 0, y: 0, text: 'WeakRef=' + typeof WeakRef, size: 1 });
  ui.text({ x: 0, y: 0, text: 'Math.random=' + typeof Math.random, size: 1 });
  var direct = 'ok';
  try { eval('1'); } catch (e) { direct = e.name; }
  ui.text({ x: 0, y: 0, text: 'direct-eval=' + direct, size: 1 });
  var fnCtor = 'compiled';
  try { (function(){}).constructor('return 1')(); } catch (e) { fnCtor = e.name; }
  ui.text({ x: 0, y: 0, text: 'fn-ctor=' + fnCtor, size: 1 });
  var globalCtor = 'compiled';
  try { globalThis.constructor.constructor('return 1')(); } catch (e) { globalCtor = e.name; }
  ui.text({ x: 0, y: 0, text: 'global-ctor=' + globalCtor, size: 1 });
};
)JS";

void caseSandbox() {
    lift_test::g_current_case = "ui-script-quickjs/sandbox";
    auto runtime = lift::ui::makeQuickJsRuntime();
    std::unique_ptr<UiScript> script = loadOk(*runtime, kSandboxScript, "sandbox.js");
    if (!script) return;
    const std::vector<std::string> texts = renderTexts(*script, UiFrameReadings{});
    // ★ **沙箱不是安全边界**（用户裁决 2026-11-18：JS 没有宿主权限 —— 能力只来自我们注入的
    //   `ui` / `game`，所以信任脚本）。下面这张表只记录**当前表面**，不是合同：
    //   上游把新内建并进 `JS_AddIntrinsicBaseObjects` 时**不追着补删除**。
    //   ★ 0.18.0 实测：BaseObjects 自带 `BigInt`（0.10.1 没有）⇒ 它的表面是 `function`，其余仍是 `undefined`。
    const char* undefineds[] = {"eval", "Function", "Date", "performance", "JSON", "Promise", "RegExp",
                                "Proxy", "Map", "Uint8Array", "BigInt", "WeakRef"};
    for (std::size_t i = 0; i < sizeof(undefineds) / sizeof(undefineds[0]); ++i) {
        if (std::string(undefineds[i]) == "BigInt") {
            expectText(texts, i, "BigInt=function", "BigInt 的当前表面（0.18.0 BaseObjects 自带，不是合同）");
            continue;
        }
        // ★ `Promise` 的当前表面（用户裁决 2026-11-19）：JS_AddIntrinsicPromise 之后**不删**
        //   globalThis.Promise。沙箱**不是安全边界**（用户裁决 2026-11-18/19）；保留的删除只有
        //   功能目的 —— `Math.random`（确定性 / replay）、`eval` / `Function` / `.constructor`
        //   （禁动态编译）；⛔ 不追删其它内建（WeakRef / performance 之类同样不在删除清单里）。
        if (std::string(undefineds[i]) == "Promise") {
            expectText(texts, i, "Promise=function", "Promise 的当前表面（module 求值需要它，不删）");
            continue;
        }
        expectText(texts, i, std::string(undefineds[i]) + "=undefined", "沙箱：不应存在的内建");
    }
    expectText(texts, 12, "Math.random=undefined", "沙箱：Math.random 必须删掉（种子是宿主时间）");
    expectText(texts, 13, "direct-eval=ReferenceError", "沙箱：eval 已被删除");
    expectText(texts, 14, "fn-ctor=TypeError", "沙箱：.constructor 不能再编译源码");
    expectText(texts, 15, "global-ctor=TypeError", "沙箱：globalThis.constructor.constructor 也不能");

    // ★ `import` 现在走 **module 路径**（`ui-script-modules` 单）；这里钉的是**边界**：`'y'` 是
    //   bare specifier ⇒ module loader 拒绝 ⇒ load 失败，且 err 带 `origin:line:col`。
    //   ⛔ 本用例不跑动态 `import('x')`：module 路径的 drain 在 load 里做（`ui-script-modules`
    //   已覆盖 module 求值）；classic 路径**不装** loader（旧语义：动态 import 不可用）。
    std::string err;
    std::unique_ptr<UiScript> imported = runtime->load("import x from 'y';\n", "import.js", nullptr, &err);
    ++lift_test::g_checks;
    if (imported) lift_test::failf(__FILE__, __LINE__, "import 语句不应能编译");
    ++lift_test::g_checks;
    if (!hasOriginLineCol(err, "import.js:1:")) {
        lift_test::failf(__FILE__, __LINE__, "import 失败的 err 应含 origin:line:col，得到: %s", err.c_str());
    }
}

// =======================================================================================
// ② budget（CPU：deadline / op_limit；内存：大数组）
// =======================================================================================
const char* kSpinScript = R"JS(
globalThis.render = function render() {
  try { while (true) {} } catch (e) { }  // ★ 吞不掉：预算是不可 catch 的
};
)JS";

const char* kOpLimitScript = R"JS(
globalThis.render = function render() {
  var s = 0;
  for (var i = 0; i < 100000000; i = i + 1) { s = s + i; }
  ui.rect({ x: 0, y: 0, w: 1, h: 1 });
};
)JS";

const char* kMemoryScript = R"JS(
globalThis.render = function render() {
  var a = new Array(40 * 1024 * 1024);
  a.fill(1);
};
)JS";

void caseBudget() {
    lift_test::g_current_case = "ui-script-quickjs/budget-deadline";
    auto runtime = lift::ui::makeQuickJsRuntime();
    {
        std::unique_ptr<UiScript> script = loadOk(*runtime, kSpinScript, "spin.js");
        if (script) {
            script->setBudget(nowNs() + 10ull * 1000ull * 1000ull, 0);
            UiBuilder builder;
            std::string err;
            const bool ok = script->render(builder, UiFrameReadings{}, &err);
            ++lift_test::g_checks;
            if (ok) {
                lift_test::failf(__FILE__, __LINE__,
                                 "deadline 到期后 render 仍成功（try/catch 吞掉了？沙箱预算失效）");
            }
            ++lift_test::g_checks;
            if (err.empty()) lift_test::failf(__FILE__, __LINE__, "预算打断后 err 必须非空");
            LIFT_CHECK(script->overran());
            ++lift_test::g_checks;
            if (!builder.elements().empty()) {
                lift_test::failf(__FILE__, __LINE__, "被打断的帧不应产出元素（得到 %zu 条）",
                                 builder.elements().size());
            }
        }
    }

    lift_test::g_current_case = "ui-script-quickjs/budget-op-limit";
    {
        std::unique_ptr<UiScript> script = loadOk(*runtime, kOpLimitScript, "oplimit.js");
        if (script) {
            script->setBudget(0, 200000);
            UiBuilder builder;
            std::string err;
            const bool ok = script->render(builder, UiFrameReadings{}, &err);
            ++lift_test::g_checks;
            if (ok) lift_test::failf(__FILE__, __LINE__, "op_limit 用尽后 render 仍成功");
            ++lift_test::g_checks;
            if (err.find("interrupted") == std::string::npos) {
                lift_test::failf(__FILE__, __LINE__, "预算打断的 err 应点名 interrupted，得到: %s", err.c_str());
            }
            LIFT_CHECK(script->overran());
        }
    }

    lift_test::g_current_case = "ui-script-quickjs/budget-memory";
    {
        std::unique_ptr<UiScript> script = loadOk(*runtime, kMemoryScript, "memory.js");
        if (script) {
            script->setBudget(0, 0);
            UiBuilder builder;
            std::string err;
            const bool ok = script->render(builder, UiFrameReadings{}, &err);
            ++lift_test::g_checks;
            if (ok) lift_test::failf(__FILE__, __LINE__, "16 MiB 内存上限内不可能分配 40M 元素的数组");
            ++lift_test::g_checks;
            if (err.find("out of memory") == std::string::npos) {
                lift_test::failf(__FILE__, __LINE__, "内存超限的 err 应点名 out of memory，得到: %s", err.c_str());
            }
            // 内存面不是 CPU 预算面：⛔ 不许把 OOM 记成 overran。
            ++lift_test::g_checks;
            if (script->overran()) lift_test::failf(__FILE__, __LINE__, "内存超限不应置 overran");
            // shutdown 幂等（重复调用 / 析构各一次）不崩。
            script->shutdown();
            script->shutdown();
        }
    }
}

// =======================================================================================
// ③ api：ui.rect / ui.text / ui.measure
// =======================================================================================
const char* kApiScript = R"JS(
globalThis.render = function render() {
  ui.rect({ x: 1, y: 2, w: 3, h: 4, fill: '#11223344', z: 5 });
  ui.text({ x: 6, y: 7, text: 'Hi 天結', size: 20, color: '#AABBCC', bold: true, z: -2 });
  ui.rect({ x: 0, y: 0, w: 0, h: 0 });
  ui.text({ x: 0, y: 0, text: 'd' });
  ui.rect({ x: 9, y: 9, w: 1, h: 1, unknownKey: 42 });
};
)JS";

const char* kMeasureScript = R"JS(
globalThis.render = function render() {
  var a = ui.measure('Ag', 16);
  var b = ui.measure('Ag', 16);
  var same = a.w === b.w && a.h === b.h;
  var ok = same && a.w > 0 && a.h > 0;
  ui.text({ x: 0, y: 0, text: ok ? 'measure-ok' : ('measure-bad:' + a.w + 'x' + a.h), size: 1 });
};
)JS";

const char* kMeasureNoFontScript = R"JS(
globalThis.render = function render() {
  ui.measure('Ag', 16);
};
)JS";

void caseApi() {
    lift_test::g_current_case = "ui-script-quickjs/api-elements";
    auto runtime = lift::ui::makeQuickJsRuntime();
    {
        std::unique_ptr<UiScript> script = loadOk(*runtime, kApiScript, "api.js");
        if (script) {
            UiBuilder builder;
            std::string err;
            const bool ok = script->render(builder, UiFrameReadings{}, &err);
            ++lift_test::g_checks;
            if (!ok) {
                lift_test::failf(__FILE__, __LINE__, "render 失败: %s", err.c_str());
            } else {
                ++lift_test::g_checks;
                if (builder.elements().size() != 5) {
                    lift_test::failf(__FILE__, __LINE__, "期望 5 条元素，得到 %zu 条", builder.elements().size());
                } else {
                    const std::vector<Element>& e = builder.elements();
                    ++lift_test::g_checks;
                    if (e[0].kind != ElementKind::Rect || e[0].x != 1 || e[0].y != 2 || e[0].w != 3 || e[0].h != 4 ||
                        e[0].z != 5 || e[0].seq != 0 || e[0].id != 0) {
                        lift_test::failf(__FILE__, __LINE__,
                                         "ui.rect #0 字段错：kind=%d x=%d y=%d w=%d h=%d z=%d seq=%d id=%d",
                                         static_cast<int>(e[0].kind), e[0].x, e[0].y, e[0].w, e[0].h, e[0].z,
                                         e[0].seq, e[0].id);
                    }
                    expectChannels(e[0].fill, 0x11, 0x22, 0x33, 0x44, "ui.rect 的 fill=#11223344");
                    ++lift_test::g_checks;
                    if (e[1].kind != ElementKind::Text || e[1].x != 6 || e[1].y != 7 || e[1].text != "Hi 天結" ||
                        e[1].font_size != 20 || e[1].z != -2 || e[1].seq != 1 || !e[1].bold) {
                        lift_test::failf(__FILE__, __LINE__,
                                         "ui.text #1 字段错：kind=%d x=%d y=%d text=\"%s\" size=%d z=%d bold=%d",
                                         static_cast<int>(e[1].kind), e[1].x, e[1].y, e[1].text.c_str(),
                                         e[1].font_size, e[1].z, e[1].bold ? 1 : 0);
                    }
                    expectChannels(e[1].color, 0xAA, 0xBB, 0xCC, 0xFF, "ui.text 的 color=#AABBCC");
                    // 缺省：fill #000000FF、z 0；text 的 size 16 / color #FFFFFFFF / bold false / z 0。
                    expectChannels(e[2].fill, 0, 0, 0, 0xFF, "ui.rect 缺省 fill");
                    ++lift_test::g_checks;
                    if (e[2].z != 0) lift_test::failf(__FILE__, __LINE__, "ui.rect 缺省 z 应为 0");
                    ++lift_test::g_checks;
                    if (e[3].font_size != 16 || e[3].bold || e[3].z != 0) {
                        lift_test::failf(__FILE__, __LINE__, "ui.text 缺省 size/bold/z 错：size=%d bold=%d z=%d",
                                         e[3].font_size, e[3].bold ? 1 : 0, e[3].z);
                    }
                    expectChannels(e[3].color, 255, 255, 255, 255, "ui.text 缺省 color");
                    // 未知键忽略（context §4）：still 一条普通 rect，字段照常。
                    ++lift_test::g_checks;
                    if (e[4].kind != ElementKind::Rect || e[4].x != 9 || e[4].y != 9 || e[4].w != 1 || e[4].h != 1) {
                        lift_test::failf(__FILE__, __LINE__, "未知键不应影响 ui.rect 的已知字段");
                    }
                }
            }
        }
    }

    lift_test::g_current_case = "ui-script-quickjs/api-measure";
    lift::FontEngine font;
    bool font_ready = font.fontFileAvailable(false);
    if (font_ready) {
        lift::HostTextMetrics metrics;
        font_ready = font.metrics(16, false, &metrics);
    }
    if (!font_ready) {
        // ★ 只跳这一条（父级的合同：不是整文件 skip）：其余用例不依赖宿主字体。
        std::printf("SKIP: ui-script-quickjs/api-measure（宿主字体不可用：LIFT_HOST_FONT_PATH）\n");
    } else {
        std::unique_ptr<UiScript> script = loadOk(*runtime, kMeasureScript, "measure.js", &font);
        if (script) {
            const std::vector<std::string> texts = renderTexts(*script, UiFrameReadings{});
            expectText(texts, 0, "measure-ok", "ui.measure 走宿主字体（两次同参结果一致且为正）");
        }
    }

    // font == nullptr ⇒ TypeError（context §4；⛔ 不返回 0 尺寸冒充可用）。
    lift_test::g_current_case = "ui-script-quickjs/api-measure-no-font";
    {
        std::unique_ptr<UiScript> script = loadOk(*runtime, kMeasureNoFontScript, "measure-nofont.js", nullptr);
        if (script) {
            UiBuilder builder;
            std::string err;
            const bool ok = script->render(builder, UiFrameReadings{}, &err);
            ++lift_test::g_checks;
            if (ok) lift_test::failf(__FILE__, __LINE__, "font == nullptr 时 ui.measure 必须 TypeError");
            ++lift_test::g_checks;
            if (err.find("font == nullptr") == std::string::npos) {
                lift_test::failf(__FILE__, __LINE__, "err 应点名字体不可用，得到: %s", err.c_str());
            }
        }
    }
}

// =======================================================================================
// ④ readings：game.*
// =======================================================================================
const char* kReadingsScript = R"JS(
globalThis.render = function render() {
  ui.text({ x: 0, y: 0, text: 'frame=' + game.frame, size: 1 });
  ui.text({ x: 0, y: 0, text: 'fps=' + game.fps, size: 1 });
  ui.text({ x: 0, y: 0, text: 'mouse=' + game.mouse.x + ',' + game.mouse.y, size: 1 });
  ui.text({ x: 0, y: 0, text: 'script=' + game.script, size: 1 });
  ui.text({ x: 0, y: 0, text: 'worst=' + game.worst.wall_ms + ',' + game.worst.guest_ms + ',' + game.worst.shim_ms, size: 1 });
  ui.text({ x: 0, y: 0, text: 'guest_key=' + ('guest_ms' in game.worst), size: 1 });
  ui.text({ x: 0, y: 0, text: 'fresh=' + (game.mouse !== game.mouse), size: 1 });
};
)JS";

const char* kReadingsAbsentScript = R"JS(
var show = function (v) { return v === undefined ? 'undefined' : 'defined'; };
globalThis.render = function render() {
  ui.text({ x: 0, y: 0, text: 'fps=' + show(game.fps), size: 1 });
  ui.text({ x: 0, y: 0, text: 'mouse=' + show(game.mouse), size: 1 });
  ui.text({ x: 0, y: 0, text: 'script=' + show(game.script), size: 1 });
  ui.text({ x: 0, y: 0, text: 'worst=' + show(game.worst), size: 1 });
  ui.text({ x: 0, y: 0, text: 'frame=' + game.frame, size: 1 });
};
)JS";

const char* kReadingsReadonlyScript = R"JS(
globalThis.render = function render() {
  var got = [];
  try { game.frame = 1; got.push('frame-writable'); } catch (e) { got.push(e.name); }
  try { game.mouse = {}; got.push('mouse-writable'); } catch (e) { got.push(e.name); }
  try { game = {}; got.push('game-writable'); } catch (e) { got.push(e.name); }
  ui.text({ x: 0, y: 0, text: got.join(','), size: 1 });
};
)JS";

void caseReadings() {
    lift_test::g_current_case = "ui-script-quickjs/readings-present";
    auto runtime = lift::ui::makeQuickJsRuntime();
    {
        std::unique_ptr<UiScript> script = loadOk(*runtime, kReadingsScript, "readings.js");
        if (script) {
            UiFrameReadings readings;
            readings.frame = 42;
            readings.fps_ok = true;
            readings.fps = 59.5;
            readings.mouse_ok = true;
            readings.mouse_x = 10;
            readings.mouse_y = 20;
            readings.script_ok = true;
            readings.script = "AGE.BIN";
            readings.worst_ok = true;
            readings.worst_wall_ms = 4.5;
            readings.worst_guest_ok = true;
            readings.worst_guest_ms = 2.5;
            readings.worst_shim_ms = 1.5;
            const std::vector<std::string> texts = renderTexts(*script, readings);
            expectText(texts, 0, "frame=42", "game.frame");
            expectText(texts, 1, "fps=59.5", "game.fps");
            expectText(texts, 2, "mouse=10,20", "game.mouse");
            expectText(texts, 3, "script=AGE.BIN", "game.script");
            expectText(texts, 4, "worst=4.5,2.5,1.5", "game.worst（guest 有计时）");
            expectText(texts, 5, "guest_key=true", "worst.guest_ms 的键存在");
            expectText(texts, 6, "fresh=true", "game.mouse 每次是新对象");
        }
    }

    lift_test::g_current_case = "ui-script-quickjs/readings-absent";
    {
        std::unique_ptr<UiScript> script = loadOk(*runtime, kReadingsAbsentScript, "readings-absent.js");
        if (script) {
            // frame 之外的读数都没有 ⇒ 对应 getter 必须 undefined（⛔ 不编 0）。
            const UiFrameReadings absent;
            const std::vector<std::string> texts = renderTexts(*script, absent);
            expectText(texts, 0, "fps=undefined", "无 fps ⇒ undefined");
            expectText(texts, 1, "mouse=undefined", "无 mouse ⇒ undefined");
            expectText(texts, 2, "script=undefined", "无 script ⇒ undefined");
            expectText(texts, 3, "worst=undefined", "无 worst ⇒ undefined");
            expectText(texts, 4, "frame=0", "frame 总是有值");
            // 没有 guest 计时 ⇒ `worst` 里**没有** guest_ms 键。
            UiFrameReadings no_guest;
            no_guest.worst_ok = true;
            no_guest.worst_wall_ms = 1.25;
            no_guest.worst_shim_ms = 0.5;
            std::unique_ptr<UiScript> script2 = loadOk(*runtime, R"JS(
globalThis.render = function render() {
  ui.text({ x: 0, y: 0, text: 'has_guest=' + ('guest_ms' in game.worst), size: 1 });
  ui.text({ x: 0, y: 0, text: 'wall=' + game.worst.wall_ms, size: 1 });
};
)JS",
                                                   "readings-noon-guest.js");
            if (script2) {
                const std::vector<std::string> t2 = renderTexts(*script2, no_guest);
                expectText(t2, 0, "has_guest=false", "guest 未计时 ⇒ 没有 guest_ms 键");
                expectText(t2, 1, "wall=1.25", "worst.wall_ms");
            }
        }
    }

    lift_test::g_current_case = "ui-script-quickjs/readings-readonly";
    {
        std::unique_ptr<UiScript> script = loadOk(*runtime, kReadingsReadonlyScript, "readings-ro.js");
        if (script) {
            UiFrameReadings readings;
            readings.mouse_ok = true;
            const std::vector<std::string> texts = renderTexts(*script, readings);
            expectText(texts, 0, "TypeError,TypeError,TypeError", "game / game.* 必须是只读 getter");
        }
    }
}

// =======================================================================================
// ⑤ error：语法错 / 运行期错 / 异常被清
// =======================================================================================
void caseError() {
    lift_test::g_current_case = "ui-script-quickjs/error-syntax";
    auto runtime = lift::ui::makeQuickJsRuntime();
    {
        std::string err;
        std::unique_ptr<UiScript> script = runtime->load("var a = 1;\nvar b = ;\n", "bad.js", nullptr, &err);
        ++lift_test::g_checks;
        if (script) lift_test::failf(__FILE__, __LINE__, "语法错的脚本不应 load 成功");
        ++lift_test::g_checks;
        if (!hasOriginLineCol(err, "bad.js:2:")) {
            lift_test::failf(__FILE__, __LINE__, "语法错的 err 应含 origin:line:col（bad.js:2:…），得到: %s",
                             err.c_str());
        }
    }

    lift_test::g_current_case = "ui-script-quickjs/error-runtime";
    {
        std::unique_ptr<UiScript> script = loadOk(*runtime, R"JS(
globalThis.render = function render() {
  throw new Error('boom');
};
)JS",
                                                 "runtime.js");
        if (script) {
            UiBuilder builder;
            std::string err;
            const bool ok = script->render(builder, UiFrameReadings{}, &err);
            ++lift_test::g_checks;
            if (ok) lift_test::failf(__FILE__, __LINE__, "运行期抛错时 render 必须 false");
            ++lift_test::g_checks;
            if (err.find("boom") == std::string::npos) {
                lift_test::failf(__FILE__, __LINE__, "运行期错的 err 应含 message，得到: %s", err.c_str());
            }
            ++lift_test::g_checks;
            if (err.find("runtime.js:3:") == std::string::npos) {
                lift_test::failf(__FILE__, __LINE__, "运行期错的 err 应含调用点（runtime.js:3:…），得到: %s",
                                 err.c_str());
            }
        }
    }

    lift_test::g_current_case = "ui-script-quickjs/error-cleared";
    {
        std::unique_ptr<UiScript> script = loadOk(*runtime, R"JS(
var n = 0;
globalThis.render = function render() {
  n = n + 1;
  if (n === 1) throw new Error('first');
  ui.rect({ x: 0, y: 0, w: 1, h: 1 });
};
)JS",
                                                 "recover.js");
        if (script) {
            UiBuilder first;
            std::string err1;
            const bool ok1 = script->render(first, UiFrameReadings{}, &err1);
            ++lift_test::g_checks;
            if (ok1 || err1.find("first") == std::string::npos) {
                lift_test::failf(__FILE__, __LINE__, "第 1 帧应失败并报 first，得到 ok=%d err=%s", ok1 ? 1 : 0,
                                 err1.c_str());
            }
            UiBuilder second;
            std::string err2;
            const bool ok2 = script->render(second, UiFrameReadings{}, &err2);
            ++lift_test::g_checks;
            if (!ok2) {
                lift_test::failf(__FILE__, __LINE__, "第 2 帧必须成功（异常已被取走），得到 err=%s", err2.c_str());
            }
            ++lift_test::g_checks;
            if (second.elements().size() != 1) {
                lift_test::failf(__FILE__, __LINE__, "第 2 帧应产出 1 条元素，得到 %zu 条", second.elements().size());
            }
            UiBuilder third;
            std::string err3;
            const bool ok3 = script->render(third, UiFrameReadings{}, &err3);
            ++lift_test::g_checks;
            if (!ok3 || third.elements().size() != 1) {
                lift_test::failf(__FILE__, __LINE__, "第 3 帧也必须成功（连续两帧不再抛错）");
            }
        }
    }

    lift_test::g_current_case = "ui-script-quickjs/error-entry";
    {
        std::string err;
        std::unique_ptr<UiScript> script = runtime->load("var x = 1;\n", "entry.js", nullptr, &err);
        ++lift_test::g_checks;
        if (script) lift_test::failf(__FILE__, __LINE__, "没有 globalThis.render 的脚本不应 load 成功");
        ++lift_test::g_checks;
        if (err.find("entry.js") == std::string::npos || err.find("render") == std::string::npos) {
            lift_test::failf(__FILE__, __LINE__, "入口缺失的 err 应点名 origin 与 render，得到: %s", err.c_str());
        }
        // `globalThis.render` 存在但**不是函数** ⇒ 同样必须失败并点名（入口必须是函数）。
        std::string err2;
        std::unique_ptr<UiScript> not_function = runtime->load("var render = 1;\n", "notfn.js", nullptr, &err2);
        ++lift_test::g_checks;
        if (not_function) lift_test::failf(__FILE__, __LINE__, "render 不是函数时 load 必须失败");
        ++lift_test::g_checks;
        if (err2.find("notfn.js") == std::string::npos || err2.find("render") == std::string::npos) {
            lift_test::failf(__FILE__, __LINE__, "render 非函数的 err 应点名 origin 与 render，得到: %s",
                             err2.c_str());
        }
    }
}

// =======================================================================================
// ⑦ value 包装的 RAII 压力：load 一次 + 200 次 render + shutdown 不崩
//    ★ 判据只是"不崩 + 每次帧结果正确"（Value 的所有权若漏释放，200 帧后内存会单调涨；
//      这里**不作**内存读数的硬判据 —— 读数不是断言，见 AGENTS.md §10）。
// =======================================================================================
const char* kStressScript = R"JS(
globalThis.render = function render() {
  ui.rect({ x: 0, y: 0, w: 1, h: 1, fill: '#01020304' });
  ui.text({ x: 2, y: 3, text: 'frame=' + game.frame, size: 1 });
  if (game.mouse !== undefined) { ui.text({ x: 0, y: 0, text: 'm=' + game.mouse.x, size: 1 }); }
  if (game.worst !== undefined) { ui.text({ x: 0, y: 0, text: 'w=' + game.worst.wall_ms, size: 1 }); }
};
)JS";

void caseValueStress() {
    lift_test::g_current_case = "ui-script-quickjs/value-raii-stress";
    auto runtime = lift::ui::makeQuickJsRuntime();
    std::unique_ptr<UiScript> script = loadOk(*runtime, kStressScript, "stress.js");
    if (!script) return;
    int rendered = 0;
    for (int i = 0; i < 200; ++i) {
        UiFrameReadings readings;
        readings.frame = static_cast<std::uint64_t>(i);
        readings.mouse_ok = (i % 2) == 0;
        readings.mouse_x = i;
        readings.mouse_y = i * 2;
        readings.worst_ok = (i % 3) == 0;
        readings.worst_wall_ms = i * 0.5;
        readings.worst_shim_ms = i * 0.25;
        UiBuilder builder;
        std::string err;
        if (!script->render(builder, readings, &err)) {
            lift_test::failf(__FILE__, __LINE__, "第 %d 次 render 失败: %s", i, err.c_str());
            break;
        }
        if (builder.elements().size() < 2) {
            lift_test::failf(__FILE__, __LINE__, "第 %d 次 render 的元素数不对: %zu", i, builder.elements().size());
            break;
        }
        ++rendered;
    }
    ++lift_test::g_checks;
    if (rendered != 200) lift_test::failf(__FILE__, __LINE__, "只完成 %d/200 次 render", rendered);
    // shutdown 幂等：这里一次 + 析构一次。
    script->shutdown();
}

}  // namespace

int main() {
    caseSandbox();
    caseBudget();
    caseApi();
    caseReadings();
    caseError();
    caseValueStress();
    return lift_test::finish("ui-script-quickjs");
}
