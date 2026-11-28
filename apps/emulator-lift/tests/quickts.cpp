// emulator-lift · quickts E1（TypeScript 纯类型擦除）的引擎级 **pure** 判据。
//
// 判据（红了意味着什么）：
//   ① js-bytecode-identical：`scripts/**` 下 16 个发行 `.js`（模块）用**同一个**引擎编两次
//      （flags 0 / `JS_EVAL_FLAG_TYPESCRIPT`），`JS_WriteObject(BYTECODE|STRIP_SOURCE)` 必须
//      **逐字节相同**。红了 ⇒ 某个 TS 钩子没有全部关在 `if (s->ts)` 里 —— 即 flags off 的
//      JS 解析/字节码被改了（这是本移植最强的不变量）。⚠ 故意 **不** 用 STRIP_DEBUG：
//      pc2line 必须留在比较里，否则 `ts_next_gt` 的行/列保真（`>>` 拆成两个 `>`）测不出来。
//   ② js-global-bytecode-identical：同上，但用一组内联 GLOBAL 脚本（含 `a < b` / `b > c`、
//      计算字段、static block、模板串、for-of、try/catch）——模块清单走不到 GLOBAL 解析路径，
//      这里补上；`f < a > (b)` 是上游自认"flags on 会改含义"的唯一一处，**故意不放进清单**。
//   ③ ts-examples-compile：`scripts/plugins/examples/*.ts` 9/9 用
//      `MODULE|COMPILE_ONLY|TYPESCRIPT` 编过；再用 `JS_ResolveModule` 要求**零** module request
//      —— 这才证明 `import type { … } from "@amayui/lift-plugin"` 是**整条擦掉**的（COMPILE_ONLY
//      本身不解析依赖，只看"编过"证明不了擦除）。同时要求这 9 个文件**不带** flag 时全部报错，
//      证明 flag 是承重的、不是被别的路径吸收。
//   ④ ts-hello：一个 `.ts` 模块（interface / type 别名 / 泛型声明与调用 / `as` / `as const` /
//      `satisfies` / `import type` 裸 specifier）**求值**并调导出函数断言数值 —— 证明擦除后
//      不但能编，还能跑。⑤ ts-class：R1（index signature vs ng 的计算字段）/ R2（static block）
//      的实跑；类里的 `private readonly` / `static readonly` / `?:` / `!` / 计算字段 / 返回类型
//      全部走 quickts 钩子，断言的是**运行期值**而不是"编过了"。
//   ⑥ ts-rejects：E2 边界（enum / 构造参数属性 / 含值的 namespace）在 TYPESCRIPT 下必须
//      **报错**（`JS_IsException`），且进程不崩 —— 把"不做"钉成可执行判据，防止将来有人以为
//      支持了。注意：`enum E { A }` 这类写法在 tsc 的 `erasableSyntaxOnly` 下也会被拒，
//      但"tsc 拒"不等于"引擎拒"，所以这里必须是引擎侧读数。
//   ⑦ ts-flag-load-bearing（S1 复审 I1 的阳性对照）：带类型注解的片段 flags 0 必须报错、
//      TYPESCRIPT 必须求值成功（default = 42）；另一个两腿都能编的片段（`f<a>(b)`）两腿
//      字节码必须**不同** ⇒ `s->ts=false` 的变异会当场打红，而不是只靠 examples 用例兜底。
//
// ★ 顶层无条件注册（`if(LIFT_BUILD_TESTS)` 之外还有一层透明条件）⇒ **pure** 档
//   （AGENTS.md §12.4：判据 = 注册位置）。零资产：只读 `scripts/**` 里的文本，无窗口 / 无字体 /
//   无安装树 / 无 LFS。
// ★ 每个文件用**独立 `JSRuntime`/`JSContext`**：ng 的 `JS_FreeValueRT` 对 `JS_TAG_MODULE` 是
//   `abort()`（`quickjs.c` 的 `case JS_TAG_MODULE`），而 COMPILE_ONLY 的 module 结果正是这个 tag
//   ⇒ 统一交给 `JS_FreeContext`（它 `js_free_modules(…, JS_FREE_MODULE_ALL)`）。GLOBAL 的
//   `JS_TAG_FUNCTION_BYTECODE` 则正常 `JS_FreeValue`。
// ★ CTest 名 = `quickts`，target = `lift-quickts`（`tools/mutate-check.mjs` 的 `buildTarget`
//   要写后者）。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "quickjs.h"

#ifndef LIFT_SCRIPTS_DIR
#error "LIFT_SCRIPTS_DIR must be defined by CMake (points at apps/emulator-lift/scripts)"
#endif
#ifndef LIFT_EXAMPLES_DIR
#error "LIFT_EXAMPLES_DIR must be defined by CMake (points at scripts/plugins/examples)"
#endif

namespace {

constexpr const char* kScriptsDir = LIFT_SCRIPTS_DIR;
constexpr const char* kExamplesDir = LIFT_EXAMPLES_DIR;

bool readFile(const std::string& path, std::string* out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    out->clear();
    char buf[8192];
    for (;;) {
        const std::size_t n = std::fread(buf, 1, sizeof(buf), f);
        if (n > 0) out->append(buf, n);
        if (n < sizeof(buf)) break;
    }
    const bool ok = std::ferror(f) == 0;
    std::fclose(f);
    return ok;
}

std::string takeException(JSContext* ctx) {
    JSValue exc = JS_GetException(ctx);
    std::string text = "<non-string exception>";
    const char* s = JS_ToCString(ctx, exc);
    if (s != nullptr) {
        text = s;
        JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, exc);
    return text;
}

void drainJobs(JSRuntime* rt) {
    JSContext* ctx = nullptr;
    while (JS_ExecutePendingJob(rt, &ctx) > 0) {
    }
}

// ---------------------------------------------------------------------------------------
// 极简 module loader（只给 I1 用）：`JS_Eval(MODULE|COMPILE_ONLY)` 在 ng 里**会**
// `js_resolve_module()`（`quickjs.c` 的 `if (m) { … js_resolve_module(ctx, m) … }`），所以发行
// `.js` 的 `import './lib/x.js'` 必须能解析 —— 否则连编译都过不去（实测：不带 loader 时
// 6/16 编过、10/16 报 `could not load module`）。这里只做 host 同形的两件事：
//   · normalize：只允许 `./` / `../` 相对 specifier，按 base 的目录拼接并消 `.`/`..`；
//   · load：读 <scripts>/<name>，用**同一个 flags** 编译（flags-on 的依赖图也全走 TS 解析）。
// ★ 依赖模块的字节码不嵌进根模块的 `JS_WriteObject` 输出，所以它只影响"能不能解析"，
//   不影响被比较的字节 —— 但两边必须用同一份 loader 与同一份名字。
struct LoaderCtx {
    const char* root = nullptr;
    int flags = 0;
};

bool normalizeRelative(const std::string& base, const std::string& name, std::string* out) {
    if (name.empty() || name[0] != '.') return false;  // bare / 绝对 / 协议一律拒
    std::string joined;
    const std::size_t slash = base.rfind('/');
    if (slash != std::string::npos) joined = base.substr(0, slash + 1);
    joined += name;
    std::vector<std::string> parts;
    std::size_t begin = 0;
    while (begin <= joined.size()) {
        const std::size_t end = joined.find('/', begin);
        const std::string part = joined.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        if (part == "..") {
            if (parts.empty()) return false;
            parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    out->clear();
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) out->push_back('/');
        out->append(parts[i]);
    }
    return !out->empty();
}

char* moduleNormalize(JSContext* ctx, const char* base_name, const char* module_name, void* opaque) {
    (void)opaque;
    const std::string base = base_name != nullptr ? base_name : "";
    const std::string name = module_name != nullptr ? module_name : "";
    std::string resolved;
    if (!normalizeRelative(base, name, &resolved)) {
        JS_ThrowSyntaxError(ctx, "quickts test loader: only relative specifiers are supported ('%s')",
                            name.c_str());
        return nullptr;
    }
    return js_strdup(ctx, resolved.c_str());
}

JSModuleDef* moduleLoad(JSContext* ctx, const char* module_name, void* opaque) {
    LoaderCtx* loader = static_cast<LoaderCtx*>(opaque);
    const std::string name = module_name != nullptr ? module_name : "";
    std::string source;
    if (loader == nullptr || !readFile(std::string(loader->root) + "/" + name, &source)) {
        JS_ThrowReferenceError(ctx, "quickts test loader: cannot read module '%s'", name.c_str());
        return nullptr;
    }
    JSValue value = JS_Eval(ctx, source.data(), source.size(), name.c_str(),
                            JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY | loader->flags);
    if (JS_IsException(value)) return nullptr;
    // module 值不能 FreeValue；它挂在 ctx 的 module 列表上，由 JS_FreeContext 释放。
    if (JS_ResolveModule(ctx, value) < 0) return nullptr;
    return static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(value));
}

// 编译一次（COMPILE_ONLY）并写出字节码。module 结果**不** FreeValue（见文件头）。
struct CompileResult {
    bool ok = false;
    std::vector<std::uint8_t> bytes;
    std::string error;
};

CompileResult compileOnly(const std::string& source, const char* filename, int base_type, int extra_flags) {
    CompileResult result;
    JSRuntime* rt = JS_NewRuntime();
    if (rt == nullptr) {
        result.error = "JS_NewRuntime failed";
        return result;
    }
    JSContext* ctx = JS_NewContext(rt);
    if (ctx == nullptr) {
        JS_FreeRuntime(rt);
        result.error = "JS_NewContext failed";
        return result;
    }
    LoaderCtx loader;
    loader.root = kScriptsDir;
    loader.flags = extra_flags;
    if (base_type == JS_EVAL_TYPE_MODULE) {
        JS_SetModuleLoaderFunc(rt, moduleNormalize, moduleLoad, &loader);
    }
    JSValue value = JS_Eval(ctx, source.data(), source.size(), filename,
                            base_type | JS_EVAL_FLAG_COMPILE_ONLY | extra_flags);
    if (JS_IsException(value)) {
        result.error = takeException(ctx);
    } else {
        std::size_t len = 0;
        std::uint8_t* buf =
            JS_WriteObject(ctx, &len, value, JS_WRITE_OBJ_BYTECODE | JS_WRITE_OBJ_STRIP_SOURCE);
        if (buf == nullptr) {
            result.error = takeException(ctx);
        } else {
            result.ok = true;
            result.bytes.assign(buf, buf + len);
            js_free(ctx, buf);
        }
        if (JS_VALUE_GET_TAG(value) != JS_TAG_MODULE) JS_FreeValue(ctx, value);
    }
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return result;
}

// 编译 + `JS_ResolveModule`（要求零 module request 或 loader 在场时能解析）。
struct ResolveResult {
    bool compiled = false;
    bool resolved = false;
    std::string error;
};

ResolveResult compileAndResolve(const std::string& source, const char* filename, int extra_flags) {
    ResolveResult result;
    JSRuntime* rt = JS_NewRuntime();
    if (rt == nullptr) {
        result.error = "JS_NewRuntime failed";
        return result;
    }
    JSContext* ctx = JS_NewContext(rt);
    if (ctx == nullptr) {
        JS_FreeRuntime(rt);
        result.error = "JS_NewContext failed";
        return result;
    }
    JSValue value = JS_Eval(ctx, source.data(), source.size(), filename,
                            JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY | extra_flags);
    if (JS_IsException(value)) {
        result.error = takeException(ctx);
    } else {
        result.compiled = true;
        // module 值不能 FreeValue；ResolveModule 失败时它会按 FREE_MODULE_NOT_RESOLVED 释放。
        if (JS_IsModule(value) && JS_ResolveModule(ctx, value) < 0) {
            result.error = takeException(ctx);
        } else {
            result.resolved = true;
        }
    }
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return result;
}

/** 两腿（flags 0 / TYPESCRIPT）的字节码必须**不同**（flag 真的改了编译产物）。 */
void expectDifferent(const char* what, const CompileResult& off, const CompileResult& on) {
    ++lift_test::g_checks;
    if (off.bytes.size() != on.bytes.size()) return;
    for (std::size_t i = 0; i < off.bytes.size(); ++i) {
        if (off.bytes[i] != on.bytes[i]) return;
    }
    lift_test::failf(__FILE__, __LINE__,
                     "%s: 两腿字节码逐字节相同（len=%zu）⇒ TYPESCRIPT flag 没有改变编译产物",
                     what, off.bytes.size());
}

void expectIdentical(const char* what, const CompileResult& off, const CompileResult& on) {
    ++lift_test::g_checks;
    if (off.bytes.size() != on.bytes.size()) {
        lift_test::failf(__FILE__, __LINE__, "%s: len flags-off=%zu flags-on=%zu（字节码不同 ⇒ 钩子漏了 if (s->ts)）",
                         what, off.bytes.size(), on.bytes.size());
        return;
    }
    for (std::size_t i = 0; i < off.bytes.size(); ++i) {
        if (off.bytes[i] != on.bytes[i]) {
            lift_test::failf(__FILE__, __LINE__, "%s: 首个差异在 byte %zu（off=0x%02X on=0x%02X）", what, i,
                             off.bytes[i], on.bytes[i]);
            return;
        }
    }
}

// ---------------------------------------------------------------------------------------
// ① 15 个发行 .js（全部是 ES module；经 `rg '^\s*(import|export)\s'` 取证）
//   ★ 2026-11-28：v1 的 `plugins/lib/chains.js`（`route`）已随插件 v2 迁移删除 ⇒ 清单 16 → 15。
// ---------------------------------------------------------------------------------------
constexpr const char* kLiftScripts[] = {
    "plugins/field-point-visible.js",
    "plugins/load-save.js",
    "plugins/quit-at-title.js",
    "plugins/scenario-fellow-switch.js",
    "plugins/scenario-save07-battle.js",
    "plugins/scenario-save10-field.js",
    "plugins/lib/boot-to-title.js",
    "plugins/lib/gates.js",
    "plugins/lib/guest-globals.js",
    "plugins/lib/save.js",
    "plugins/lib/script-positions.js",
    "plugins/lib/settings.js",
    "ui/hud.js",
    "ui/kit.js",
    "ui/settings-page.js",
};

void caseJsBytecodeIdentical() {
    lift_test::g_current_case = "quickts/js-bytecode-identical";
    int compared = 0;
    for (const char* rel : kLiftScripts) {
        const std::string path = std::string(kScriptsDir) + "/" + rel;
        std::string source;
        ++lift_test::g_checks;
        if (!readFile(path, &source)) {
            lift_test::failf(__FILE__, __LINE__, "读不到 %s（清单过期？）", path.c_str());
            continue;
        }
        const CompileResult off = compileOnly(source, rel, JS_EVAL_TYPE_MODULE, 0);
        const CompileResult on = compileOnly(source, rel, JS_EVAL_TYPE_MODULE, JS_EVAL_FLAG_TYPESCRIPT);
        ++lift_test::g_checks;
        if (!off.ok) {
            lift_test::failf(__FILE__, __LINE__, "%s: flags 0 编译失败: %s", rel, off.error.c_str());
        }
        ++lift_test::g_checks;
        if (!on.ok) {
            lift_test::failf(__FILE__, __LINE__, "%s: TYPESCRIPT flag 下编译失败: %s", rel, on.error.c_str());
        }
        if (off.ok && on.ok) {
            expectIdentical(rel, off, on);
            ++compared;
        }
    }
    LIFT_CHECK_EQ_I64(compared, 15);
}

// ---------------------------------------------------------------------------------------
// ② GLOBAL 脚本：补齐模块清单走不到的解析路径（全是合法 JS；flags on 必须逐字节相同）
// ---------------------------------------------------------------------------------------
struct Snippet {
    const char* name;
    const char* source;
};

constexpr Snippet kGlobalSnippets[] = {
    {"vars", "var a = 1; let b = 2; const c = 3;\n"},
    {"functions", "function f(a, b) { return a + b; }\n"},
    {"function-expressions", "const f = function named(x) { return x; };\n"},
    {"classes", "class A { x = 1; static y = 2; m() { return this.x; } }\n"},
    {"computed-field-static-block",
     "const K = 'k';\nclass B { [K] = 1; static { this.z = 2; } }\n"},
    {"arrows", "const f = (a) => a + 1; const g = x => x * 2;\n"},
    {"destructuring", "const { a, b: c } = { a: 1, b: 2 }; const [d] = [1];\n"},
    {"for-of", "let s = 0; for (const x of [1, 2, 3]) { s += x; }\n"},
    {"try-catch", "try { throw new Error('x'); } catch (e) { }\n"},
    {"template", "const n = 1; const s = `a${n}b${n + 1}`;\n"},
    {"conditional", "const a = 1, b = 2, c = 3; const r = a < b ? b : c;\n"},
    {"comparisons", "const a = 1, b = 2, c = 3; const r = a < b && b > c;\n"},
    {"optional-chain", "const o = { a: { b: 1 } }; const r = o?.a?.b;\n"},
};

void caseJsGlobalBytecodeIdentical() {
    lift_test::g_current_case = "quickts/js-global-bytecode-identical";
    int compared = 0;
    for (const Snippet& snippet : kGlobalSnippets) {
        const CompileResult off = compileOnly(snippet.source, snippet.name, JS_EVAL_TYPE_GLOBAL, 0);
        const CompileResult on =
            compileOnly(snippet.source, snippet.name, JS_EVAL_TYPE_GLOBAL, JS_EVAL_FLAG_TYPESCRIPT);
        ++lift_test::g_checks;
        if (!off.ok) {
            lift_test::failf(__FILE__, __LINE__, "%s: flags 0 编译失败: %s", snippet.name, off.error.c_str());
        }
        ++lift_test::g_checks;
        if (!on.ok) {
            lift_test::failf(__FILE__, __LINE__, "%s: TYPESCRIPT flag 下编译失败: %s", snippet.name,
                             on.error.c_str());
        }
        if (off.ok && on.ok) {
            expectIdentical(snippet.name, off, on);
            ++compared;
        }
    }
    LIFT_CHECK_EQ_I64(compared, static_cast<int>(sizeof(kGlobalSnippets) / sizeof(kGlobalSnippets[0])));
}

// ---------------------------------------------------------------------------------------
// ③ 9 个 examples：编过 + 零 module request（`import type` 整条擦掉）+ 不带 flag 必红
// ---------------------------------------------------------------------------------------
constexpr const char* kTsExamples[] = {
    "concurrent.ts", "dispatch.ts", "global-edit.ts", "hud-at-pos.ts", "keep-others.ts",
    "load-save.ts",  "maxfires.ts", "rearm.ts",        "timeout.ts",
};

void caseTsExamplesCompile() {
    lift_test::g_current_case = "quickts/ts-examples-compile";
    int compiled = 0;
    int resolved = 0;
    int without_flag_rejected = 0;
    for (const char* name : kTsExamples) {
        const std::string path = std::string(kExamplesDir) + "/" + name;
        std::string source;
        ++lift_test::g_checks;
        if (!readFile(path, &source)) {
            lift_test::failf(__FILE__, __LINE__, "读不到 %s（清单过期？）", path.c_str());
            continue;
        }
        const ResolveResult with_flag = compileAndResolve(source, name, JS_EVAL_FLAG_TYPESCRIPT);
        ++lift_test::g_checks;
        if (!with_flag.compiled) {
            lift_test::failf(__FILE__, __LINE__, "%s: TYPESCRIPT 下编译失败: %s", name, with_flag.error.c_str());
        } else {
            ++compiled;
            ++lift_test::g_checks;
            if (!with_flag.resolved) {
                lift_test::failf(__FILE__, __LINE__,
                                 "%s: resolve 失败（`import type` 没被整条擦掉？）: %s", name,
                                 with_flag.error.c_str());
            } else {
                ++resolved;
            }
        }
        const ResolveResult without_flag = compileAndResolve(source, name, 0);
        ++lift_test::g_checks;
        if (without_flag.compiled) {
            lift_test::failf(__FILE__, __LINE__, "%s: 不带 TYPESCRIPT flag 竟然编过了（flag 没起作用？）", name);
        } else {
            ++without_flag_rejected;
        }
    }
    LIFT_CHECK_EQ_I64(compiled, 9);
    LIFT_CHECK_EQ_I64(resolved, 9);
    LIFT_CHECK_EQ_I64(without_flag_rejected, 9);
}

// ---------------------------------------------------------------------------------------
// ④/⑤ 求值型模块：Eval → Resolve → JS_EvalFunction → drain → namespace
// ---------------------------------------------------------------------------------------
struct TsModule {
    JSRuntime* rt = nullptr;
    JSContext* ctx = nullptr;
    JSModuleDef* module = nullptr;
    bool ok = false;
    std::string error;
};

TsModule evalTsModule(const std::string& source, const char* filename) {
    TsModule out;
    out.rt = JS_NewRuntime();
    if (out.rt == nullptr) {
        out.error = "JS_NewRuntime failed";
        return out;
    }
    out.ctx = JS_NewContext(out.rt);
    if (out.ctx == nullptr) {
        JS_FreeRuntime(out.rt);
        out.rt = nullptr;
        out.error = "JS_NewContext failed";
        return out;
    }
    JSValue module_value = JS_Eval(out.ctx, source.data(), source.size(), filename,
                                   JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY | JS_EVAL_FLAG_TYPESCRIPT);
    if (JS_IsException(module_value)) {
        out.error = takeException(out.ctx);
        JS_FreeContext(out.ctx);
        JS_FreeRuntime(out.rt);
        out.ctx = nullptr;
        out.rt = nullptr;
        return out;
    }
    out.module = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(module_value));
    if (JS_ResolveModule(out.ctx, module_value) < 0) {
        out.error = takeException(out.ctx);
        JS_FreeContext(out.ctx);  // 释放 module（不能 JS_FreeValue）
        JS_FreeRuntime(out.rt);
        out.ctx = nullptr;
        out.rt = nullptr;
        out.module = nullptr;
        return out;
    }
    JSValue eval_ret = JS_EvalFunction(out.ctx, module_value);  // 接管 module 值
    if (JS_IsException(eval_ret)) {
        out.error = takeException(out.ctx);
        JS_FreeContext(out.ctx);
        JS_FreeRuntime(out.rt);
        out.ctx = nullptr;
        out.rt = nullptr;
        out.module = nullptr;
        return out;
    }
    drainJobs(out.rt);
    const JSPromiseStateEnum state = JS_PromiseState(out.ctx, eval_ret);
    if (state == JS_PROMISE_REJECTED) {
        JSValue reason = JS_PromiseResult(out.ctx, eval_ret);
        const char* s = JS_ToCString(out.ctx, reason);
        out.error = s != nullptr ? s : "<non-string rejection>";
        if (s != nullptr) JS_FreeCString(out.ctx, s);
        JS_FreeValue(out.ctx, reason);
    } else if (state != JS_PROMISE_FULFILLED) {
        out.error = "module 求值未完成（没有可执行的异步来源）";
    }
    JS_FreeValue(out.ctx, eval_ret);
    out.ok = out.error.empty();
    if (!out.ok) {
        JS_FreeContext(out.ctx);
        JS_FreeRuntime(out.rt);
        out.ctx = nullptr;
        out.rt = nullptr;
        out.module = nullptr;
    }
    return out;
}

void closeTsModule(TsModule* m) {
    if (m->ctx != nullptr) JS_FreeContext(m->ctx);
    if (m->rt != nullptr) JS_FreeRuntime(m->rt);
    m->ctx = nullptr;
    m->rt = nullptr;
    m->module = nullptr;
}

JSValue moduleExport(TsModule& m, const char* name) {
    JSValue ns = JS_GetModuleNamespace(m.ctx, m.module);
    if (JS_IsException(ns)) return JS_EXCEPTION;
    JSValue value = JS_GetPropertyStr(m.ctx, ns, name);
    JS_FreeValue(m.ctx, ns);
    return value;
}

void expectModuleNumber(TsModule& m, const char* export_name, std::int32_t want) {
    const std::string what = std::string("export ") + export_name;
    JSValue fn = moduleExport(m, export_name);
    ++lift_test::g_checks;
    if (!JS_IsFunction(m.ctx, fn)) {
        lift_test::failf(__FILE__, __LINE__, "%s 不是函数", what.c_str());
        JS_FreeValue(m.ctx, fn);
        return;
    }
    JSValue value = JS_Call(m.ctx, fn, JS_UNDEFINED, 0, nullptr);
    JS_FreeValue(m.ctx, fn);
    ++lift_test::g_checks;
    if (JS_IsException(value)) {
        lift_test::failf(__FILE__, __LINE__, "%s 调用失败: %s", what.c_str(), takeException(m.ctx).c_str());
        return;
    }
    std::int32_t actual = 0;
    if (JS_ToInt32(m.ctx, &actual, value) < 0) {
        lift_test::failf(__FILE__, __LINE__, "%s 的返回值不是 int32: %s", what.c_str(),
                         takeException(m.ctx).c_str());
        JS_FreeValue(m.ctx, value);
        return;
    }
    JS_FreeValue(m.ctx, value);
    ++lift_test::g_checks;
    if (actual != want) lift_test::failf(__FILE__, __LINE__, "%s: 期望 %d，得到 %d", what.c_str(), want, actual);
}

void expectModuleBool(TsModule& m, const char* export_name, bool want) {
    const std::string what = std::string("export ") + export_name;
    JSValue fn = moduleExport(m, export_name);
    ++lift_test::g_checks;
    if (!JS_IsFunction(m.ctx, fn)) {
        lift_test::failf(__FILE__, __LINE__, "%s 不是函数", what.c_str());
        JS_FreeValue(m.ctx, fn);
        return;
    }
    JSValue value = JS_Call(m.ctx, fn, JS_UNDEFINED, 0, nullptr);
    JS_FreeValue(m.ctx, fn);
    ++lift_test::g_checks;
    if (JS_IsException(value)) {
        lift_test::failf(__FILE__, __LINE__, "%s 调用失败: %s", what.c_str(), takeException(m.ctx).c_str());
        return;
    }
    const int converted = JS_ToBool(m.ctx, value);
    if (converted < 0) {
        lift_test::failf(__FILE__, __LINE__, "%s 的返回值不是 bool: %s", what.c_str(),
                         takeException(m.ctx).c_str());
        JS_FreeValue(m.ctx, value);
        return;
    }
    const bool actual = converted != 0;
    JS_FreeValue(m.ctx, value);
    ++lift_test::g_checks;
    if (actual != want) {
        lift_test::failf(__FILE__, __LINE__, "%s: 期望 %s，得到 %s", what.c_str(), want ? "true" : "false",
                         actual ? "true" : "false");
    }
}

// hello.ts：E1 的语法面最小集（interface / type 别名 / 泛型声明+调用 / as / as const /
// satisfies / import type 裸 specifier）。求值后 run() 必须返回 45 = 40+1+2+2。
constexpr const char* kHelloTs = R"TS(import type { FiredEvent, PluginModule } from "@amayui/lift-plugin";

interface Box<T> {
    readonly value: T;
    readonly tag?: string;
}

type Pair<A, B> = readonly [A, B];

function id<T>(v: T): T {
    return v;
}

function first<A, B>(p: Pair<A, B>): A {
    return p[0];
}

const N = { start: 1, end: 2 } as const;
const PARAMS: Readonly<Record<string, string>> = { slot: "0" };

const box: Box<number> = { value: id<number>(40) };
const pair: Pair<number, string> = [1, "x"];
const options = { retries: 2 } satisfies { retries: number };

export function run(): number {
    return box.value + first(pair) + N.end + options.retries;
}

export default { run } satisfies PluginModule;
)TS";

void caseTsHello() {
    lift_test::g_current_case = "quickts/ts-hello";
    TsModule m = evalTsModule(kHelloTs, "hello.ts");
    ++lift_test::g_checks;
    if (!m.ok) {
        lift_test::failf(__FILE__, __LINE__, "hello.ts 求值失败: %s", m.error.c_str());
        closeTsModule(&m);
        return;
    }
    expectModuleNumber(m, "run", 45);
    // default 导出的形状 = 插件契约（对象 + run 函数）。
    JSValue def = moduleExport(m, "default");
    ++lift_test::g_checks;
    if (!JS_IsObject(def)) {
        lift_test::failf(__FILE__, __LINE__, "default 导出不是对象（plugin 契约）");
    } else {
        JSValue run = JS_GetPropertyStr(m.ctx, def, "run");
        ++lift_test::g_checks;
        if (!JS_IsFunction(m.ctx, run)) lift_test::failf(__FILE__, __LINE__, "default.run 不是函数");
        JS_FreeValue(m.ctx, run);
    }
    JS_FreeValue(m.ctx, def);
    closeTsModule(&m);
}

// class.ts：R1（index signature 被擦 vs ng 计算字段保留）/ R2（static block 原样）。
// 期望：bump0=3 / bump4=9 / computed=5 / staticBlock=true / nonNull=3 / bagValue=7 /
//       dictValue=1 / indexSignatureErased=true
constexpr const char* kClassTs = R"TS(interface Dict {
    readonly [k: string]: number;
}

class Counter {
    private readonly base: number = 1;
    static readonly STEP: number = 2;
    ["dyn" + "amic"]: number = 5;

    bump(n?: number): number {
        return this.base + (n ?? 1) * Counter.STEP;
    }
}

class Bag {
    [k: string]: number;
    static { this.ready = true; }
    value: number = 7;
}

const counter = new Counter();
const bag = new Bag();
const maybe: { v: number } | null = { v: 3 };
const casted = counter as { dynamic: number };
const dict: Dict = { a: 1 };

export function bump0(): number { return counter.bump(); }
export function bump4(): number { return counter.bump(4); }
export function computed(): number { return casted.dynamic; }
export function staticBlock(): boolean { return (Bag as { ready?: boolean }).ready === true; }
export function nonNull(): number { return maybe!.v; }
export function bagValue(): number { return bag.value; }
export function dictValue(): number { return dict["a"]; }
export function indexSignatureErased(): boolean { return !("k" in (bag as Record<string, unknown>)); }
)TS";

void caseTsClass() {
    lift_test::g_current_case = "quickts/ts-class";
    TsModule m = evalTsModule(kClassTs, "class.ts");
    ++lift_test::g_checks;
    if (!m.ok) {
        lift_test::failf(__FILE__, __LINE__, "class.ts 求值失败: %s", m.error.c_str());
        closeTsModule(&m);
        return;
    }
    expectModuleNumber(m, "bump0", 3);
    expectModuleNumber(m, "bump4", 9);
    expectModuleNumber(m, "computed", 5);
    expectModuleBool(m, "staticBlock", true);
    expectModuleNumber(m, "nonNull", 3);
    expectModuleNumber(m, "bagValue", 7);
    expectModuleNumber(m, "dictValue", 1);
    expectModuleBool(m, "indexSignatureErased", true);
    closeTsModule(&m);
}

// ---------------------------------------------------------------------------------------
// ⑦ flag 承重（**阳性对照**）：证明 `JS_EVAL_FLAG_TYPESCRIPT` 不是"加了没人读"
//
//   ★ 为什么需要（S1 复审 I1）：①/② 只证明 flags-on 对**合法 JS** 不改字节码 —— 把 `s->ts`
//     钉成 false 时它们照样绿（flag 成了静默无效）。这条从反方向钉住：
//     · 同一个带类型注解的片段：flags 0 ⇒ **必须报错**；TYPESCRIPT ⇒ 求值成功且值正确；
//     · 一个两腿都能编的片段（`f<a>(b)`：TS = 泛型调用、JS = 两次比较）⇒ 两腿的
//       `JS_WriteObject` 字节**必须不同**（"flag 只改了错误路径"也会被抓到）。
//   ⚠ `f < a > (b)` 正是上游自认"flags on 会改含义"的那一处；①/② 故意不放它（那里要比的是
//     "逐字节相同"），这里用它当**差异探针**是另一回事。
// ---------------------------------------------------------------------------------------
constexpr const char* kFlagLoadBearingTs = "const n: number = 41 + 1;\nexport default n;\n";
// 两腿都能编：flags 0 = `(f < a) > (b)`（布尔）；TYPESCRIPT = `f<a>(b)`（泛型调用）。
constexpr const char* kFlagDualParse =
    "function f(x) { return x; }\nconst a = 1;\nconst b = 2;\nexport const r = f < a > (b);\n";

void expectModuleDefaultNumber(TsModule& m, std::int32_t want) {
    JSValue def = moduleExport(m, "default");
    ++lift_test::g_checks;
    if (JS_IsException(def)) {
        lift_test::failf(__FILE__, __LINE__, "default 导出读取失败: %s", takeException(m.ctx).c_str());
        return;
    }
    std::int32_t actual = 0;
    ++lift_test::g_checks;
    if (JS_ToInt32(m.ctx, &actual, def) < 0) {
        lift_test::failf(__FILE__, __LINE__, "default 导出不是 int32: %s", takeException(m.ctx).c_str());
        JS_FreeValue(m.ctx, def);
        return;
    }
    JS_FreeValue(m.ctx, def);
    ++lift_test::g_checks;
    if (actual != want) lift_test::failf(__FILE__, __LINE__, "default: 期望 %d，得到 %d", want, actual);
}

void caseTsFlagLoadBearing() {
    lift_test::g_current_case = "quickts/ts-flag-load-bearing";
    // ① flags 0 ⇒ 必须报错（类型注解只有 TYPESCRIPT 解析器认识）。
    const CompileResult off = compileOnly(kFlagLoadBearingTs, "flag-load-bearing.ts", JS_EVAL_TYPE_MODULE, 0);
    ++lift_test::g_checks;
    if (off.ok) {
        lift_test::failf(__FILE__, __LINE__, "flags 0 竟然编过了（TYPESCRIPT flag 不承重）");
    } else if (off.error.empty()) {
        lift_test::failf(__FILE__, __LINE__, "flags 0 报错了但异常消息为空");
    }
    // ② TYPESCRIPT ⇒ 求值成功且 default = 42。
    TsModule m = evalTsModule(kFlagLoadBearingTs, "flag-load-bearing.ts");
    ++lift_test::g_checks;
    if (!m.ok) {
        lift_test::failf(__FILE__, __LINE__, "TYPESCRIPT 下求值失败: %s", m.error.c_str());
        closeTsModule(&m);
        return;
    }
    expectModuleDefaultNumber(m, 42);
    closeTsModule(&m);
    // ③ 两腿都能编的片段 ⇒ 字节码必须不同（"flag 只改了错误路径"也会被抓到）。
    const CompileResult dual_off = compileOnly(kFlagDualParse, "flag-dual.ts", JS_EVAL_TYPE_MODULE, 0);
    const CompileResult dual_on =
        compileOnly(kFlagDualParse, "flag-dual.ts", JS_EVAL_TYPE_MODULE, JS_EVAL_FLAG_TYPESCRIPT);
    ++lift_test::g_checks;
    if (!dual_off.ok) lift_test::failf(__FILE__, __LINE__, "差异探针 flags 0 编译失败: %s", dual_off.error.c_str());
    ++lift_test::g_checks;
    if (!dual_on.ok) lift_test::failf(__FILE__, __LINE__, "差异探针 TYPESCRIPT 编译失败: %s", dual_on.error.c_str());
    if (dual_off.ok && dual_on.ok) expectDifferent("f<a>(b)", dual_off, dual_on);
}

// ---------------------------------------------------------------------------------------
// ⑥ E2 边界：TYPESCRIPT flag 下必须报错（不崩）
// ---------------------------------------------------------------------------------------
bool rejectsUnderTs(const char* source, const char* filename) {
    JSRuntime* rt = JS_NewRuntime();
    JSContext* ctx = JS_NewContext(rt);
    JSValue value = JS_Eval(ctx, source, std::strlen(source), filename,
                            JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY | JS_EVAL_FLAG_TYPESCRIPT);
    const bool rejected = JS_IsException(value);
    std::string message;
    if (rejected) {
        message = takeException(ctx);
    } else {
        if (JS_VALUE_GET_TAG(value) != JS_TAG_MODULE) JS_FreeValue(ctx, value);
    }
    ++lift_test::g_checks;
    if (!rejected) {
        lift_test::failf(__FILE__, __LINE__, "%s: E2 语法竟然被接受了（应报错）", filename);
    } else if (message.empty()) {
        lift_test::failf(__FILE__, __LINE__, "%s: 报错了但异常消息为空", filename);
    }
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return rejected;
}

void caseTsRejects() {
    lift_test::g_current_case = "quickts/ts-rejects";
    // enum 要 lower 成对象 + IIFE（生成代码），E1 只做擦除 ⇒ 必须拒。
    LIFT_CHECK(rejectsUnderTs("enum E { A, B }\n", "reject-enum.ts"));
    // 构造参数属性要发射 `this.x = x`（lowering）⇒ 必须拒。
    LIFT_CHECK(rejectsUnderTs("class A { constructor(public readonly x: number) {} }\n",
                             "reject-param-property.ts"));
    // 含值的 namespace 需要 IIFE + 合并对象 ⇒ 必须拒。
    LIFT_CHECK(rejectsUnderTs("namespace N { export const x = 1; }\n", "reject-namespace-values.ts"));
    // R8：病态深嵌套类型必须**干净报错**（TS_MAX_DEPTH = 96），不是崩 / 不是静默接受。
    std::string deep = "type Deep = ";
    for (int i = 0; i < 200; ++i) deep += "Array<";
    deep += "number";
    for (int i = 0; i < 200; ++i) deep += ">";
    deep += ";\n";
    LIFT_CHECK(rejectsUnderTs(deep.c_str(), "reject-deep-type.ts"));
}

}  // namespace

int main() {
    caseJsBytecodeIdentical();
    caseJsGlobalBytecodeIdentical();
    caseTsExamplesCompile();
    caseTsHello();
    caseTsClass();
    caseTsFlagLoadBearing();
    caseTsRejects();
    return lift_test::finish("quickts");
}
