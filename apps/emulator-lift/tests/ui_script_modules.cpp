// emulator-lift · UI 脚本层的 **ES module** 判据（pure CTest：无 SDL / 无窗口 / 无游戏资产）
//
// 判据（红了意味着什么）：
//   ① `import-and-entry`：相对 `import` 能拿到导出，且被 `render` 使用（入口约定仍是
//      `globalThis.render`，module 里显式赋值）。红了 ⇒ 脚本层没有接上 module loader，
//      或相对解析错了（发行件 `hud.js` 将来要 `import { Button } from './kit.js'`）。
//   ② `nested-chain`：A → B → C 的嵌套 import（相对路径按**导入方的模块名**解析，不是按 cwd /
//      入口目录硬套）。红了 ⇒ 嵌套一层就解析到错误目录。
//   ③ `bare-rejected`：`import x from 'react'` 被**拒绝**，err 里点名 bare specifier 与那个名字。
//      红了 ⇒ 宿主会去 cwd / 系统里猜一个模块（不可复现，且把发布目录之外的东西拉进脚本面）。
//   ④ `dotdot-rejected`：`../outside.js` 解析结果越出 UI 根目录 ⇒ 拒绝，err 点名根目录。
//      红了 ⇒ 脚本能读 UI 根之外的任意文件（发行件布局被穿透）。
//   ⑤ `absolute-rejected`：绝对路径被拒绝。红了 ⇒ 同上（只是换了写法）。
//   ⑥ `data-url-rejected`：`data:` 协议被拒绝。红了 ⇒ 脚本面能绕过文件/根目录判据。
//   ⑥b `missing-file-rejected`：相对路径合法但文件不存在 ⇒ loader 读失败（点名模块与
//      `origin:line:col`）。红了 ⇒ 读不到文件被静默跳过（脚本少了依赖还在跑）。
//   ⑦ `module-throw-surfaced`：module 顶层抛错 ⇒ `load` 失败，err 含 `origin:line:col` 且
//      **点名那条异常**（`module-boom`）。红了 ⇒ module 求值返回的 Promise 没 drain / 没查
//      rejection ⇒ 异常被静默吞掉（脚本明明坏了，overlay 只是"不画"）。
//   ⑧ `classic-unchanged`：同一份**经典**脚本（顶层 `function render(){}`）仍能 load 并 render。
//      红了 ⇒ classic 路径被 module 路径顶掉（hud 回退的 classic 语义变了：顶层函数声明不再挂 globalThis）。
//   ⑨ `cache-shared-module`：同一模块被两条路径导入只求值一次。红了 ⇒ 每次 import 都重跑顶层
//      （副作用翻倍），或解析结果不稳定（同一个文件被当成两个模块）。
//   ⑩ `mixed-loads`：同一个 runtime 里 module 之后 classic 仍能 load / render（状态不串）。
//   ★ ⑪ `ui-script-quickjs` 的沙箱断言（含 `Promise` 的新表面）由那条既有 CTest 继续钉住。
// ★ 顶层无条件注册 = **pure** 档（AGENTS.md §12.4：判据 = 注册位置）。
// ★ 只 include 接口头（⛔ 不 include quickjs.h —— 那是 `script_quickjs.cpp` 的特权）。
// ★ 夹具在 `tests/ui_modules/`（跟着测试走；⛔ 不是发行的 `scripts/ui/`）。
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

#include "check.h"
#include "runtime/vfs.h"  // openHostFileUtf8（Windows 的 UTF-8 路径）
#include "ui/element.h"
#include "ui/script.h"

namespace {

using lift::ui::ScriptRuntime;
using lift::ui::UiBuilder;
using lift::ui::UiFrameReadings;
using lift::ui::UiScript;

// CMake 注入（`target_compile_definitions`）⇒ 用例不依赖 ctest 的 cwd。
std::string moduleDir() { return std::string(LIFT_UI_MODULES_DIR); }
std::string modulePath(const char* name) { return moduleDir() + "/" + name; }

bool readAll(const std::string& path, std::string* out) {
    std::FILE* f = lift::openHostFileUtf8(path, "rb");
    if (f == nullptr) return false;
    char buf[8192];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out->append(buf, n);
    const bool bad = std::ferror(f) != 0;
    std::fclose(f);
    return !bad;
}

// err 里是否**某处**出现 `<prefix><digit>`（与 `ui-script-quickjs` 同一形状判据：⛔ 不钉列号精度）。
// ★ 扫描**所有**出现：`err` 的第一行是 `<origin>: <message>`（origin 后面跟的是空格），
//   带位置的形态在后面的 `    at <origin>:<line>:<col>` 里。
bool hasOriginLineCol(const std::string& err, const std::string& prefix) {
    for (std::size_t at = err.find(prefix); at != std::string::npos; at = err.find(prefix, at + 1)) {
        const std::size_t digit = at + prefix.size();
        if (digit < err.size() && err[digit] >= '0' && err[digit] <= '9') return true;
    }
    return false;
}

std::string textsJoined(const UiBuilder& builder) {
    std::string out;
    for (const lift::ui::Element& e : builder.elements()) {
        if (e.kind != lift::ui::ElementKind::Text) continue;
        if (!out.empty()) out += " / ";
        out += e.text;
    }
    return out;
}

// 读夹具文件 + 以该文件路径为 origin（module 名 / 错误前缀都是它）加载。
std::unique_ptr<UiScript> loadFixture(ScriptRuntime& runtime, const char* name, std::string* err) {
    const std::string path = modulePath(name);
    std::string source;
    if (!readAll(path, &source)) {
        *err = "读不到夹具：" + path;
        return nullptr;
    }
    return runtime.load(source, path, nullptr, err);
}

void expectRenderedText(ScriptRuntime& runtime, const char* fixture, const char* want) {
    std::string err;
    std::unique_ptr<UiScript> script = loadFixture(runtime, fixture, &err);
    LIFT_CHECK(script != nullptr);
    if (script == nullptr) {
        std::fprintf(stderr, "load(%s) 失败：%s\n", fixture, err.c_str());
        return;
    }
    UiBuilder builder;
    std::string rerr;
    LIFT_CHECK(script->render(builder, UiFrameReadings{}, &rerr));
    if (!rerr.empty()) std::fprintf(stderr, "render(%s)：%s\n", fixture, rerr.c_str());
    const std::string got = textsJoined(builder);
    LIFT_CHECK(got == want);
    if (got != want) {
        std::fprintf(stderr, "render(%s)：期望 `%s`，得到 `%s`\n", fixture, want, got.c_str());
    }
}

// 一次 load 必须失败，且 err 同时命中给定的子串；返回 err 供调用方继续钉形状。
std::string expectRejected(ScriptRuntime& runtime, const std::string& source, const std::string& origin,
                           const char* const* wants, std::size_t want_count) {
    std::string err;
    std::unique_ptr<UiScript> script = runtime.load(source, origin, nullptr, &err);
    LIFT_CHECK(script == nullptr);
    LIFT_CHECK(!err.empty());
    for (std::size_t i = 0; i < want_count; ++i) {
        LIFT_CHECK(err.find(wants[i]) != std::string::npos);
        if (err.find(wants[i]) == std::string::npos) {
            std::fprintf(stderr, "err 应含 `%s`，得到：%s\n", wants[i], err.c_str());
        }
    }
    return err;
}

// ---------------------------------------------------------------------------------------
// ① 相对 import 的导出能被 render 使用（入口仍是 globalThis.render）
// ---------------------------------------------------------------------------------------
void caseImportAndEntry(ScriptRuntime& runtime) {
    lift_test::g_current_case = "ui-script-modules/import-and-entry";
    expectRenderedText(runtime, "entry.js", "hello-from-import");
}

// ---------------------------------------------------------------------------------------
// ② 嵌套：A → B → C（相对路径按导入方模块名解析）
// ---------------------------------------------------------------------------------------
void caseNestedChain(ScriptRuntime& runtime) {
    lift_test::g_current_case = "ui-script-modules/nested-chain";
    expectRenderedText(runtime, "entry-nested.js", "kit:nested");
}

// ---------------------------------------------------------------------------------------
// ③ 拒绝 bare specifier（点名原因与名字）
// ---------------------------------------------------------------------------------------
void caseBareRejected(ScriptRuntime& runtime) {
    lift_test::g_current_case = "ui-script-modules/bare-rejected";
    const char* wants[] = {"bare specifier", "react"};
    const std::string err =
        expectRejected(runtime, "import x from 'react';\nglobalThis.render = function () {};\n",
                       modulePath("inline-bare.js"), wants, 2);
    // 同一类拒绝也要带 `<origin>:line:col` 形状（错误里能定位到 import 那一行）。
    LIFT_CHECK(hasOriginLineCol(err, "inline-bare.js:"));
    if (!hasOriginLineCol(err, "inline-bare.js:")) {
        std::fprintf(stderr, "bare 拒绝的 err 应带 origin:line:col，得到：%s\n", err.c_str());
    }
}

// ---------------------------------------------------------------------------------------
// ④ `..` 逃出 UI 根目录被拒
// ---------------------------------------------------------------------------------------
void caseDotDotRejected(ScriptRuntime& runtime) {
    lift_test::g_current_case = "ui-script-modules/dotdot-rejected";
    const char* wants[] = {"根目录", "../outside.js"};
    expectRejected(runtime, "import x from '../outside.js';\nglobalThis.render = function () {};\n",
                   modulePath("inline-dotdot.js"), wants, 2);
}

// ---------------------------------------------------------------------------------------
// ⑤ 绝对路径被拒
// ---------------------------------------------------------------------------------------
void caseAbsoluteRejected(ScriptRuntime& runtime) {
    lift_test::g_current_case = "ui-script-modules/absolute-rejected";
    const std::string absolute = modulePath("text.js");
    const char* wants[] = {"绝对", "text.js"};
    expectRejected(runtime, "import x from '" + absolute + "';\nglobalThis.render = function () {};\n",
                   modulePath("inline-absolute.js"), wants, 2);
}

// ---------------------------------------------------------------------------------------
// ⑥ `data:` 协议被拒
// ---------------------------------------------------------------------------------------
void caseDataUrlRejected(ScriptRuntime& runtime) {
    lift_test::g_current_case = "ui-script-modules/data-url-rejected";
    const char* wants[] = {"data:"};
    expectRejected(runtime,
                   "import x from 'data:text/javascript,export default 1';\n"
                   "globalThis.render = function () {};\n",
                   modulePath("inline-data.js"), wants, 1);
}

// ---------------------------------------------------------------------------------------
// ⑥b 相对路径合法但文件不存在 ⇒ loader 读失败（点名模块与 origin:line:col，不静默跳过）
// ---------------------------------------------------------------------------------------
void caseMissingFileRejected(ScriptRuntime& runtime) {
    lift_test::g_current_case = "ui-script-modules/missing-file-rejected";
    const char* wants[] = {"读不到模块", "missing.js"};
    const std::string err =
        expectRejected(runtime, "import x from './missing.js';\nglobalThis.render = function () {};\n",
                       modulePath("inline-missing.js"), wants, 2);
    LIFT_CHECK(hasOriginLineCol(err, "inline-missing.js:"));
    if (!hasOriginLineCol(err, "inline-missing.js:")) {
        std::fprintf(stderr, "loader 读失败的 err 应带 origin:line:col，得到：%s\n", err.c_str());
    }
}

// ---------------------------------------------------------------------------------------
// ⑦ module 顶层抛错 ⇒ load 失败 + err 含 origin:line:col + 不被静默吞
// ---------------------------------------------------------------------------------------
void caseModuleThrowSurfaced(ScriptRuntime& runtime) {
    lift_test::g_current_case = "ui-script-modules/module-throw-surfaced";
    std::string err;
    std::unique_ptr<UiScript> script = loadFixture(runtime, "thrower.js", &err);
    LIFT_CHECK(script == nullptr);
    LIFT_CHECK(err.find("module-boom") != std::string::npos);
    if (err.find("module-boom") == std::string::npos) {
        std::fprintf(stderr, "module 顶层的异常被吞了？err=%s\n", err.c_str());
    }
    LIFT_CHECK(hasOriginLineCol(err, "thrower.js:"));
    if (!hasOriginLineCol(err, "thrower.js:")) {
        std::fprintf(stderr, "module 顶层抛错的 err 应带 origin:line:col，得到：%s\n", err.c_str());
    }
}

// ---------------------------------------------------------------------------------------
// ⑧ classic 路径不变（顶层 function render(){} 仍挂 globalThis，能被 load 认到）
// ---------------------------------------------------------------------------------------
void caseClassicUnchanged(ScriptRuntime& runtime) {
    lift_test::g_current_case = "ui-script-modules/classic-unchanged";
    std::string err;
    std::unique_ptr<UiScript> script = runtime.load(
        "function render() { ui.rect({ x: 0, y: 0, w: 1, h: 1 }); }\n", "classic-inline.js", nullptr, &err);
    LIFT_CHECK(script != nullptr);
    if (script == nullptr) {
        std::fprintf(stderr, "classic 脚本 load 失败（旧语义被 module 路径顶掉了？）：%s\n", err.c_str());
        return;
    }
    UiBuilder builder;
    std::string rerr;
    LIFT_CHECK(script->render(builder, UiFrameReadings{}, &rerr));
    if (!rerr.empty()) std::fprintf(stderr, "classic render：%s\n", rerr.c_str());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(builder.elements().size()), 1);
    LIFT_CHECK(!builder.elements().empty() &&
               builder.elements().front().kind == lift::ui::ElementKind::Rect);
}

// ---------------------------------------------------------------------------------------
// ⑨ 缓存：同一模块被两条路径导入只求值一次
// ---------------------------------------------------------------------------------------
void caseCacheSharedModule(ScriptRuntime& runtime) {
    lift_test::g_current_case = "ui-script-modules/cache-shared-module";
    expectRenderedText(runtime, "cache-entry.js", "counter|wrapped:counter|evals=1");
}

// ---------------------------------------------------------------------------------------
// ⑩ 同一个 runtime 连续 load 两种脚本（module 之后 classic / classic 之后 module）不串状态
// ---------------------------------------------------------------------------------------
void caseMixedLoads(ScriptRuntime& runtime) {
    lift_test::g_current_case = "ui-script-modules/mixed-loads";
    std::string module_err;
    std::unique_ptr<UiScript> module_script = loadFixture(runtime, "entry.js", &module_err);
    LIFT_CHECK(module_script != nullptr);
    std::string classic_err;
    std::unique_ptr<UiScript> classic_script = runtime.load(
        "function render() { ui.text({ x: 0, y: 0, text: 'classic', size: 1 }); }\n", "classic-after.js",
        nullptr, &classic_err);
    LIFT_CHECK(classic_script != nullptr);
    if (classic_script != nullptr) {
        UiBuilder builder;
        std::string rerr;
        LIFT_CHECK(classic_script->render(builder, UiFrameReadings{}, &rerr));
        LIFT_CHECK(textsJoined(builder) == "classic");
    }
}

}  // namespace

int main() {
    auto runtime = lift::ui::makeQuickJsRuntime();
    LIFT_CHECK(runtime != nullptr);
    if (runtime == nullptr) return lift_test::finish("ui-script-modules");

    caseImportAndEntry(*runtime);
    caseNestedChain(*runtime);
    caseBareRejected(*runtime);
    caseDotDotRejected(*runtime);
    caseAbsoluteRejected(*runtime);
    caseDataUrlRejected(*runtime);
    caseMissingFileRejected(*runtime);
    caseModuleThrowSurfaced(*runtime);
    caseClassicUnchanged(*runtime);
    caseCacheSharedModule(*runtime);
    caseMixedLoads(*runtime);
    return lift_test::finish("ui-script-modules");
}
