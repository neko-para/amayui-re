// emulator-lift · QuickJS 后端（A：QuickJS 内核）
//
// ★ `#include "quickjs.h"` **只**出现在私有头 `ui/quickjs_value.h` 里；本文件通过它拿到 C API，
//   并且**所有拥有所有权的 JSValue 局部一律是 `qjs::Value`**（用户 2026-11-18 裁决）：
//   一个 JSValue 一个所有者、拷贝 = JS_DupValue、move 置 undefined、析构 = JS_FreeValue。
//   ⛔ 本文件里不再出现"裸 JSValue 局部 + 手工 JS_FreeValue"（`qjs::Value` 实现内部除外）。
//
// 一、沙箱形状（context §2.3 的**实测反转**，与 `overlay-quickjs.md` §2.1 相反，以 context 为准）
//   1. `JS_NewContextRaw` + `JS_AddIntrinsicBaseObjects` 之后，**宿主**调 `JS_Eval` 会失败
//      （`TypeError: eval is not supported`）：`JS_Eval` 走 `JS_EvalInternal`，它要求
//      `ctx->eval_internal` 非空，而该字段**只由** `JS_AddIntrinsicEval` 设置（0.10.1
//      `quickjs.c` 的 `JS_AddIntrinsicEval` 全部内容就是 `ctx->eval_internal = __JS_EvalInternal`）。
//   2. ⇒ 必须 `JS_AddIntrinsicEval`（只为让**宿主**能编译脚本），随后把脚本可达的编译入口删掉：
//      `globalThis.eval` / `globalThis.Function` / `Function.prototype.constructor`。
//   3. ⛔ 只删 `globalThis.Function` **不够**：`(function(){}).constructor` 会从
//      `Function.prototype.constructor` 拿到同一个构造器，`('return 1')()` 即可重新编译源码。
//      ★ 删掉后 `(function(){}).constructor` **不是 undefined**（它会沿原型链落到
//      `Object.prototype.constructor` = `Object`，本文件不删它）；被钉住的是**行为**：
//      拿到的任何 `.constructor` 都不能把字符串编译成函数（调用 ⇒ TypeError）。
//   4. `Math.random` 也删（种子 = 宿主时间 ⇒ 不可复现）。`Date` / `RegExp` / `JSON` / `Proxy` /
//      `Promise` / `BigInt` / `WeakRef` / TypedArrays / `performance` **没加**（`JS_NewContextRaw`
//      不装它们，本文件只加 BaseObjects）。
//
// 二、预算（context §5）
//   · 内存 16 MiB / 栈 512 KiB / 中断回调每 ~10000 条字节码一次（`JS_INTERRUPT_COUNTER_INIT`）。
//   · ★ 超预算的报错不是我们抛的：0.10.1 的 `JS_ThrowInterrupted` 是 `quickjs.c` 内的 **static**
//     （`quickjs.h` 未导出）；上游约定是**中断回调返非 0** 时由引擎自己
//     `JS_ThrowInternalError("interrupted")` + `JS_SetUncatchableError`（`__js_poll_interrupts`）。
//     ⇒ 本文件的回调只做"查 deadline / op_limit，超了返 1"，得到的就是合同要的不可 catch 错误。
//   · op_limit 以"中断回调次数 × 10000"计（引擎不暴露精确字节码计数）：实际执行量落在
//     `[op_limit, op_limit + 10000)` 条之间 —— 这是**预算**，不是精确计量。
//
// 三、失败与异常（context §3 / §5）
//   · `JS_Eval` / `JS_Call` 返回 `JS_EXCEPTION` ⇒ **必须**取走（`qjs::takeException` 读 message +
//     stack），否则脏异常留在 ctx 上，下一次调用会读到上一次的异常。
//   · err 形状 = `<origin>: <message>\n<stack>`；`stack` 由引擎给 `<origin>:<line>:<col>`
//     （语法错与运行期错都带调用点）。同实例的下一次 `render` 必须在取走异常后照常工作。
//   · 宿主 C 函数不接 C++ 异常：`catch (...)` → `JS_ThrowTypeError` + 返 `JS_EXCEPTION`。
//
// 四、生命周期：`render` 的句柄在 `load` 时由 `qjs::Value` 持有；`shutdown` 先释放它（与 `game`），
//   再 `JS_FreeContext` / `JS_FreeRuntime`。`JS_DUMP_LEAKS` 只在 `#ifndef NDEBUG` 打开。
#include "ui/script_quickjs.h"

#include "ui/quickjs_value.h"  // ★ 唯一 include quickjs.h 的头（本文件是它唯一的消费者）

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "runtime/font.h"
#include "ui/element.h"
#include "ui/raster.h"

namespace lift::ui {
namespace {

using qjs::Value;

// =======================================================================================
// 预算常量（context §5；数字写死，⛔ 不做 CLI 旋钮）
// =======================================================================================
constexpr std::size_t kMemoryLimitBytes = 16u << 20;  // 16 MiB
constexpr std::size_t kStackLimitBytes = 512u << 10;  // 512 KiB
// 引擎每 `JS_INTERRUPT_COUNTER_INIT` 条字节码调一次回调（0.10.1 `quickjs.c`）；op_limit 以它为步长。
constexpr std::uint64_t kOpcodesPerInterrupt = 10000;

std::uint64_t steadyNowNs() {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch());
    return static_cast<std::uint64_t>(ns.count());
}

struct Impl;

Impl* implOf(JSContext* ctx) { return static_cast<Impl*>(JS_GetContextOpaque(ctx)); }

std::string toUtf8(JSContext* ctx, JSValueConst v) {
    std::string out;
    if (!qjs::toString(ctx, v, &out)) {
        qjs::clearException(ctx);
        return {};
    }
    return out;
}

// =======================================================================================
// Impl：一个脚本实例的全部状态（JS 值 + 本帧注入的读数 + 预算）。
// ★ 持有者 = `QuickJsScript`（`UiScript` 的实现），构造后不再移动 ⇒ 中断回调的 opaque 稳定。
// =======================================================================================
struct Impl {
    JSRuntime* rt = nullptr;
    JSContext* ctx = nullptr;
    Value render;        // globalThis.render（持有引用）
    Value game_obj;      // 全局 `game` 的 getter 返回它（⛔ 每帧不重建）
    std::string origin;  // 错误里的文件名（`--ui <path>` / `builtin:hud`）

    // 本帧注入（getter 读它们；render 结束一律复位）
    UiBuilder* builder = nullptr;
    const UiFrameReadings* current = nullptr;
    FontEngine* font = nullptr;

    // 预算（`setBudget` 写入；interrupt 回调读）
    std::uint64_t deadline_ns = 0;
    std::uint32_t op_limit = 0;
    std::uint64_t ops = 0;  // 已执行的"中断步长"数（× 10000 = 近似字节码条数）
    bool overran = false;   // 本帧是否被预算打断（`render` false 的原因之一）

    ~Impl() { shutdown(); }

    // 幂等：释放持有的 JS 值 → 释放 context → 释放 runtime。
    void shutdown() {
        if (ctx != nullptr) {
            qjs::clearException(ctx);
            // ★ 先释放值、再忘掉 ctx：`Value` 析构时不能碰到已经被 Free 的 ctx。
            render.invalidate();
            game_obj.invalidate();
            JS_FreeContext(ctx);
            ctx = nullptr;
        }
        if (rt != nullptr) {
            JS_FreeRuntime(rt);
            rt = nullptr;
        }
        builder = nullptr;
        current = nullptr;
    }
};

// ---------------------------------------------------------------------------------------
// 中断回调：每 ~10000 条字节码一次。返非 0 ⇒ 引擎自己抛 "interrupted" 并置不可 catch
// （见文件头第二节；`JS_ThrowInterrupted` 在 0.10.1 是 static，宿主无法直接调）。
// ---------------------------------------------------------------------------------------
int interruptHandler(JSRuntime* /*rt*/, void* opaque) {
    Impl* impl = static_cast<Impl*>(opaque);
    if (impl == nullptr) return 0;
    impl->ops += kOpcodesPerInterrupt;
    if (impl->deadline_ns != 0 && steadyNowNs() >= impl->deadline_ns) {
        impl->overran = true;
        return 1;
    }
    if (impl->op_limit != 0 && impl->ops >= impl->op_limit) {
        impl->overran = true;
        return 1;
    }
    return 0;
}

// =======================================================================================
// 宿主侧小工具（每个都查返回码；失败 = 返回 false + 填 err，且 ctx 上的异常已清）
// =======================================================================================
bool setProperty(JSContext* ctx, JSValueConst obj, const char* name, Value value, std::string* err) {
    if (!qjs::setProperty(ctx, obj, name, std::move(value))) {
        qjs::clearException(ctx);
        if (err != nullptr) *err = std::string("quickjs: 写属性 ") + name + " 失败";
        return false;
    }
    return true;
}

// 删除属性。★ false 覆盖两种情况：删除抛错（<0）与"删不掉"（0，属性不可配置）。
// 「删不掉」也算失败 —— 沙箱的每一个删除都是**承重**的，⛔ 不能静默留下一个入口。
bool deleteProperty(JSContext* ctx, JSValueConst obj, const char* name, std::string* err) {
    const JSAtom atom = JS_NewAtom(ctx, name);
    if (atom == JS_ATOM_NULL) {
        qjs::clearException(ctx);
        if (err != nullptr) *err = std::string("quickjs: 无法创建属性名 ") + name;
        return false;
    }
    const int rc = JS_DeleteProperty(ctx, obj, atom, 0);
    JS_FreeAtom(ctx, atom);
    if (rc < 0) {
        qjs::clearException(ctx);
        if (err != nullptr) *err = std::string("quickjs: 删除属性 ") + name + " 抛错";
        return false;
    }
    if (rc == 0) {
        if (err != nullptr) *err = std::string("quickjs: 属性 ") + name + " 删不掉（沙箱安装失败）";
        return false;
    }
    return true;
}

// 静态 C trampoline 注册（⛔ 不用 JS_NewCFunctionData 塞 std::function：那会把 C++ 生命周期
// 带进引擎的 func_data）。函数体从 `JS_GetContextOpaque` 拿 Impl。
bool setFunction(JSContext* ctx, JSValueConst obj, const char* name, JSCFunction* fn, int length,
                 std::string* err) {
    Value value(ctx, JS_NewCFunction(ctx, fn, name, length));
    if (value.isException()) {
        qjs::clearException(ctx);
        if (err != nullptr) *err = std::string("quickjs: 创建函数 ") + name + " 失败";
        return false;
    }
    return setProperty(ctx, obj, name, std::move(value), err);
}

// =======================================================================================
// 沙箱安装（context §2.3 / §5）
// =======================================================================================
bool installSandbox(Impl& impl, std::string* err) {
    JSContext* ctx = impl.ctx;
    Value global = qjs::getGlobalObject(ctx);
    if (global.isException()) {
        qjs::clearException(ctx);
        if (err != nullptr) *err = "quickjs: 取 globalThis 失败";
        return false;
    }
    bool ok = true;
    Value function_ctor = qjs::getProperty(ctx, global.raw(), "Function");
    Value function_proto = Value::undefined(ctx);
    if (function_ctor.isException()) {
        qjs::clearException(ctx);
        ok = false;
        if (err != nullptr) *err = "quickjs: 取 Function 失败（沙箱安装失败）";
    } else if (function_ctor.isObject()) {
        function_proto = qjs::getProperty(ctx, function_ctor.raw(), "prototype");
        if (function_proto.isException()) {
            qjs::clearException(ctx);
            function_proto = Value::undefined(ctx);
        }
        if (!function_proto.isObject()) {
            ok = false;
            if (err != nullptr) *err = "quickjs: 取 Function.prototype 失败（沙箱安装失败）";
        }
    } else {
        ok = false;
        if (err != nullptr) *err = "quickjs: BaseObjects 没有装 Function（沙箱安装失败）";
    }
    if (ok) {
        // ★ 只删 globalThis.Function 不够：`.constructor` 仍能拿到同一个构造器（文件头 §1.3）。
        //   变异条目 `ui-script-quickjs-fn-ctor` 就施加在这一行（删掉它 ⇒ 测试红）。
        if (!deleteProperty(ctx, function_proto.raw(), "constructor", err)) ok = false;
    }
    if (ok) ok = deleteProperty(ctx, global.raw(), "Function", err);
    if (ok) ok = deleteProperty(ctx, global.raw(), "eval", err);
    if (ok) {
        Value math = qjs::getProperty(ctx, global.raw(), "Math");
        if (math.isException()) {
            qjs::clearException(ctx);
            ok = false;
            if (err != nullptr) *err = "quickjs: 取 Math 失败（沙箱安装失败）";
        } else if (!math.isObject()) {
            ok = false;
            if (err != nullptr) *err = "quickjs: BaseObjects 没有装 Math（沙箱安装失败）";
        } else {
            ok = deleteProperty(ctx, math.raw(), "random", err);
        }
    }
    return ok;
}

// =======================================================================================
// ui.* 的参数解析（context §4）
// =======================================================================================
// `#RRGGBB` / `#RRGGBBAA`；其他 ⇒ false（调用方转 TypeError）。undefined ⇒ 默认色。
bool parseColor(JSContext* ctx, JSValueConst v, const Color& dflt, Color* out, std::string* why) {
    if (JS_IsUndefined(v)) {
        *out = dflt;
        return true;
    }
    if (!JS_IsString(v)) {
        *why = "颜色必须是 '#RRGGBB' / '#RRGGBBAA' 字符串";
        return false;
    }
    const std::string text = toUtf8(ctx, v);
    if (text.size() != 7 && text.size() != 9) {
        *why = "颜色必须是 '#RRGGBB' / '#RRGGBBAA'（含 # 共 7 或 9 字符）";
        return false;
    }
    if (text[0] != '#') {
        *why = "颜色必须以 # 开头";
        return false;
    }
    const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    int bytes[4] = {0, 0, 0, 255};
    for (int i = 0; i < static_cast<int>(text.size() / 2); ++i) {
        const int hi = hex(text[1 + static_cast<std::size_t>(i) * 2]);
        const int lo = hex(text[2 + static_cast<std::size_t>(i) * 2]);
        if (hi < 0 || lo < 0) {
            *why = "颜色里有非十六进制字符";
            return false;
        }
        bytes[i] = (hi << 4) | lo;
    }
    *out = Color{static_cast<std::uint8_t>(bytes[0]), static_cast<std::uint8_t>(bytes[1]),
                 static_cast<std::uint8_t>(bytes[2]), static_cast<std::uint8_t>(bytes[3])};
    return true;
}

// 读一个整数属性：undefined ⇒ required ? 失败 : 默认值；非数字 / 非有限 ⇒ 失败。
// ★ 非有限必须显式拒绝：`JS_ToInt32(NaN) == 0`（ECMAScript ToInt32 的定义），静默变 0 是错。
bool readInt32(JSContext* ctx, JSValueConst obj, const char* name, bool required, std::int32_t dflt,
               std::int32_t* out, std::string* why) {
    Value v = qjs::getProperty(ctx, obj, name);
    if (v.isException()) {
        qjs::clearException(ctx);
        *why = std::string(name) + " 读取失败";
        return false;
    }
    if (v.isUndefined()) {
        if (required) {
            *why = std::string(name) + " 缺失（必须是数字）";
            return false;
        }
        *out = dflt;
        return true;
    }
    if (!v.isNumber()) {
        *why = std::string(name) + " 必须是数字";
        return false;
    }
    double d = 0.0;
    if (!qjs::toDouble(ctx, v.raw(), &d) || !std::isfinite(d)) {
        qjs::clearException(ctx);
        *why = std::string(name) + " 必须是有限数字";
        return false;
    }
    std::int32_t i = 0;
    if (!qjs::toInt32(ctx, v.raw(), &i)) {
        qjs::clearException(ctx);
        *why = std::string(name) + " 无法转成 32 位整数";
        return false;
    }
    *out = i;
    return true;
}

bool readBool(JSContext* ctx, JSValueConst obj, const char* name, bool dflt, bool* out, std::string* why) {
    Value v = qjs::getProperty(ctx, obj, name);
    if (v.isException()) {
        qjs::clearException(ctx);
        *why = std::string(name) + " 读取失败";
        return false;
    }
    if (v.isUndefined()) {
        *out = dflt;
        return true;
    }
    if (!qjs::toBool(ctx, v.raw(), out)) {
        qjs::clearException(ctx);
        *why = std::string(name) + " 无法转成布尔";
        return false;
    }
    return true;
}

bool readText(JSContext* ctx, JSValueConst obj, const char* name, std::string* out, std::string* why) {
    Value v = qjs::getProperty(ctx, obj, name);
    if (v.isException()) {
        qjs::clearException(ctx);
        *why = std::string(name) + " 读取失败";
        return false;
    }
    if (v.isUndefined()) {
        *why = std::string(name) + " 缺失（必须能转成 UTF-8 字符串）";
        return false;
    }
    if (!qjs::toString(ctx, v.raw(), out)) {
        qjs::clearException(ctx);
        *why = std::string(name) + " 无法转成 UTF-8 字符串";
        return false;
    }
    return true;
}

// =======================================================================================
// 宿主函数：ui.rect / ui.text / ui.measure（三个 static C trampoline，全部在边界上 catch(...)）
// =======================================================================================
JSValue jsUiRect(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    try {
        Impl* impl = implOf(ctx);
        if (impl == nullptr || impl->builder == nullptr) {
            return JS_ThrowTypeError(ctx, "ui.rect: 只能在 render() 内调用");
        }
        if (argc < 1 || !JS_IsObject(argv[0])) {
            return JS_ThrowTypeError(ctx, "ui.rect: 参数必须是一个对象");
        }
        const JSValueConst arg = argv[0];
        std::int32_t x = 0;
        std::int32_t y = 0;
        std::int32_t w = 0;
        std::int32_t h = 0;
        std::int32_t z = 0;
        Color fill;
        std::string why;
        bool ok = readInt32(ctx, arg, "x", true, 0, &x, &why);
        if (ok) ok = readInt32(ctx, arg, "y", true, 0, &y, &why);
        if (ok) ok = readInt32(ctx, arg, "w", true, 0, &w, &why);
        if (ok) ok = readInt32(ctx, arg, "h", true, 0, &h, &why);
        if (ok) ok = readInt32(ctx, arg, "z", false, 0, &z, &why);
        if (ok) {
            Value fill_v = qjs::getProperty(ctx, arg, "fill");
            if (fill_v.isException()) {
                qjs::clearException(ctx);
                why = "fill 读取失败";
                ok = false;
            } else {
                ok = parseColor(ctx, fill_v.raw(), Color{0, 0, 0, 255}, &fill, &why);
            }
        }
        if (!ok) return JS_ThrowTypeError(ctx, "ui.rect: %s", why.c_str());
        impl->builder->rect(x, y, w, h, fill, z);
        return JS_UNDEFINED;
    } catch (...) {
        return JS_ThrowTypeError(ctx, "ui.rect: 宿主异常（C++ exception at script boundary）");
    }
}

JSValue jsUiText(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    try {
        Impl* impl = implOf(ctx);
        if (impl == nullptr || impl->builder == nullptr) {
            return JS_ThrowTypeError(ctx, "ui.text: 只能在 render() 内调用");
        }
        if (argc < 1 || !JS_IsObject(argv[0])) {
            return JS_ThrowTypeError(ctx, "ui.text: 参数必须是一个对象");
        }
        const JSValueConst arg = argv[0];
        std::int32_t x = 0;
        std::int32_t y = 0;
        std::int32_t size = 16;
        std::int32_t z = 0;
        bool bold = false;
        std::string text;
        Color color;
        std::string why;
        bool ok = readInt32(ctx, arg, "x", true, 0, &x, &why);
        if (ok) ok = readInt32(ctx, arg, "y", true, 0, &y, &why);
        if (ok) ok = readText(ctx, arg, "text", &text, &why);
        if (ok) ok = readInt32(ctx, arg, "size", false, 16, &size, &why);
        if (ok) ok = readInt32(ctx, arg, "z", false, 0, &z, &why);
        if (ok) ok = readBool(ctx, arg, "bold", false, &bold, &why);
        if (ok) {
            Value color_v = qjs::getProperty(ctx, arg, "color");
            if (color_v.isException()) {
                qjs::clearException(ctx);
                why = "color 读取失败";
                ok = false;
            } else {
                ok = parseColor(ctx, color_v.raw(), Color{255, 255, 255, 255}, &color, &why);
            }
        }
        if (!ok) return JS_ThrowTypeError(ctx, "ui.text: %s", why.c_str());
        impl->builder->text(x, y, std::move(text), size, color, bold, z);
        return JS_UNDEFINED;
    } catch (...) {
        return JS_ThrowTypeError(ctx, "ui.text: 宿主异常（C++ exception at script boundary）");
    }
}

JSValue jsUiMeasure(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    try {
        Impl* impl = implOf(ctx);
        if (impl == nullptr) {
            return JS_ThrowTypeError(ctx, "ui.measure: 宿主未初始化");
        }
        if (impl->font == nullptr) {
            // context §4：font == nullptr ⇒ TypeError（⛔ 不返回 0 尺寸冒充可用）。
            return JS_ThrowTypeError(ctx, "ui.measure: 宿主字体不可用（font == nullptr）");
        }
        if (argc < 2) return JS_ThrowTypeError(ctx, "ui.measure: 需要 (text, size)");
        if (!JS_IsString(argv[0])) return JS_ThrowTypeError(ctx, "ui.measure: text 必须是字符串");
        if (!JS_IsNumber(argv[1])) return JS_ThrowTypeError(ctx, "ui.measure: size 必须是数字");
        std::string text;
        if (!qjs::toString(ctx, argv[0], &text)) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ui.measure: text 无法转成 UTF-8 字符串");
        }
        double size_d = 0.0;
        if (!qjs::toDouble(ctx, argv[1], &size_d) || !std::isfinite(size_d)) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ui.measure: size 必须是有限数字");
        }
        std::int32_t size = 0;
        if (!qjs::toInt32(ctx, argv[1], &size)) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ui.measure: size 无法转成 32 位整数");
        }
        if (size <= 0) return JS_ThrowTypeError(ctx, "ui.measure: size 必须为正（参考像素）");
        int w = 0;
        int h = 0;
        std::string merr;
        if (!measureText(text, size, false, *impl->font, &w, &h, &merr)) {
            return JS_ThrowTypeError(ctx, "ui.measure: 宿主字体度量失败：%s", merr.c_str());
        }
        Value out = qjs::newObject(ctx);
        if (out.isException()) return JS_EXCEPTION;
        if (!qjs::setProperty(ctx, out.raw(), "w", qjs::newInt32(ctx, w)) ||
            !qjs::setProperty(ctx, out.raw(), "h", qjs::newInt32(ctx, h))) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ui.measure: 结果对象写入失败");
        }
        return out.take();
    } catch (...) {
        return JS_ThrowTypeError(ctx, "ui.measure: 宿主异常（C++ exception at script boundary）");
    }
}

// =======================================================================================
// game.* 的 getter（context §4）：读数由 render 的 readings 当帧注入，getter 读 Impl::current。
// ★ 只在 render 期间有值；不可用 ⇒ undefined（⛔ 不编 0）；mouse / worst 每次调用新建对象。
// =======================================================================================
enum class GameProp : int { Frame = 0, Fps = 1, Mouse = 2, Script = 3, Worst = 4 };

Value makeMouseObject(JSContext* ctx, const UiFrameReadings& r) {
    Value obj = qjs::newObject(ctx);
    if (obj.isException()) return obj;
    if (!qjs::setProperty(ctx, obj.raw(), "x", qjs::newInt32(ctx, r.mouse_x)) ||
        !qjs::setProperty(ctx, obj.raw(), "y", qjs::newInt32(ctx, r.mouse_y))) {
        qjs::clearException(ctx);
        return Value::exception(ctx);
    }
    return obj;
}

Value makeWorstObject(JSContext* ctx, const UiFrameReadings& r) {
    Value obj = qjs::newObject(ctx);
    if (obj.isException()) return obj;
    if (!qjs::setProperty(ctx, obj.raw(), "wall_ms", qjs::newDouble(ctx, r.worst_wall_ms))) {
        qjs::clearException(ctx);
        return Value::exception(ctx);
    }
    // guest 未计时 ⇒ **没有** guest_ms 键（context §4）。
    if (r.worst_guest_ok &&
        !qjs::setProperty(ctx, obj.raw(), "guest_ms", qjs::newDouble(ctx, r.worst_guest_ms))) {
        qjs::clearException(ctx);
        return Value::exception(ctx);
    }
    if (!qjs::setProperty(ctx, obj.raw(), "shim_ms", qjs::newDouble(ctx, r.worst_shim_ms))) {
        qjs::clearException(ctx);
        return Value::exception(ctx);
    }
    return obj;
}

// =======================================================================================
// `game.local_frames()`（E）：**方法**（不是 getter），返回活动帧数组（top-first）或 undefined。
// ★ 全对象**唯一**一个非 getter 的属性（报告「game.local_frames 的注册」）：宿主每帧不推表，
//   脚本**按需**查询；`JS_DefinePropertyValueStr(..., 0)` ⇒ 不可写 / 不可枚举 / 不可配置
//   （strict 下赋值 ⇒ TypeError）。
// ★ 取不到的口径：`frames == nullptr` / 宿主 `err` 非空 / 空表 ⇒ **undefined**
//   （⛔ 不编 0 / 空对象 / 空数组冒充读数）。字段 = 报告的最小版：slot / script_id / parent /
//   script / name_degraded / top；`script_id` / `parent` 是 **uint32 原值**（`0xFFFFFFFF` 打印成
//   `4294967295`，⛔ 不转成 -1）。
// =======================================================================================
Value newArray(JSContext* ctx) { return Value(ctx, JS_NewArray(ctx)); }

// 数组元素赋值：`JS_SetPropertyUint32` 取得值所有权（与 qjs::setProperty 的契约一致）。
bool setElement(JSContext* ctx, JSValueConst arr, std::uint32_t index, Value value) {
    return JS_SetPropertyUint32(ctx, arr, index, value.take()) >= 0;
}

bool setFrameProperty(JSContext* ctx, JSValueConst obj, const char* name, Value value) {
    if (!qjs::setProperty(ctx, obj, name, std::move(value))) {
        qjs::clearException(ctx);
        return false;
    }
    return true;
}

Value makeFrameObject(JSContext* ctx, const UiFrameInfo& frame) {
    Value obj = qjs::newObject(ctx);
    if (obj.isException()) return obj;
    // ★ uint32 原值 ⇒ 用 int64（`newInt32` 会把 0xFFFFFFFF 变成 -1，那是另一条口径）。
    if (!setFrameProperty(ctx, obj.raw(), "slot", qjs::newInt64(ctx, static_cast<std::int64_t>(frame.slot))) ||
        !setFrameProperty(ctx, obj.raw(), "script_id",
                          qjs::newInt64(ctx, static_cast<std::int64_t>(frame.script_id))) ||
        !setFrameProperty(ctx, obj.raw(), "parent", qjs::newInt64(ctx, static_cast<std::int64_t>(frame.parent))) ||
        !setFrameProperty(ctx, obj.raw(), "script", qjs::newString(ctx, frame.script)) ||
        !setFrameProperty(ctx, obj.raw(), "name_degraded", qjs::newBool(ctx, frame.name_degraded)) ||
        !setFrameProperty(ctx, obj.raw(), "top", qjs::newBool(ctx, frame.top))) {
        return Value::exception(ctx);
    }
    return obj;
}

JSValue jsGameLocalFrames(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/, JSValueConst* /*argv*/) {
    try {
        const Impl* impl = implOf(ctx);
        // 只在 render 期间有值（`impl->current` 由 render 复位 ⇒ 这里不需要第二个"复位"字段）。
        if (impl == nullptr || impl->current == nullptr) return JS_UNDEFINED;
        const UiFrameReadings& r = *impl->current;
        if (r.frames == nullptr) return JS_UNDEFINED;
        std::string frame_err;
        const std::vector<UiFrameInfo> frames = r.frames->localFrames(&frame_err);
        if (!frame_err.empty() || frames.empty()) return JS_UNDEFINED;
        Value arr = newArray(ctx);
        if (arr.isException()) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "game.local_frames: 分配数组失败");
        }
        for (std::size_t i = 0; i < frames.size(); ++i) {
            Value item = makeFrameObject(ctx, frames[i]);
            if (item.isException()) {
                qjs::clearException(ctx);
                return JS_ThrowTypeError(ctx, "game.local_frames: 写帧对象失败");
            }
            if (!setElement(ctx, arr.raw(), static_cast<std::uint32_t>(i), std::move(item))) {
                qjs::clearException(ctx);
                return JS_ThrowTypeError(ctx, "game.local_frames: 写数组元素失败");
            }
        }
        return arr.take();
    } catch (...) {
        return JS_ThrowTypeError(ctx, "game.local_frames: 宿主异常（C++ exception at script boundary）");
    }
}

// 全局 `game` 的 getter：返回持有的 game 对象（只读绑定；strict 下赋值 ⇒ TypeError）。
JSValue jsGameObject(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/, JSValueConst* /*argv*/) {
    const Impl* impl = implOf(ctx);
    if (impl == nullptr) return JS_UNDEFINED;
    return impl->game_obj.dup().take();
}

JSValue jsGameProperty(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/, JSValueConst* /*argv*/,
                       int magic) {
    try {
        const Impl* impl = implOf(ctx);
        if (impl == nullptr || impl->current == nullptr) return JS_UNDEFINED;
        const UiFrameReadings& r = *impl->current;
        switch (static_cast<GameProp>(magic)) {
            case GameProp::Frame:
                return qjs::newInt64(ctx, static_cast<std::int64_t>(r.frame)).take();
            case GameProp::Fps:
                if (!r.fps_ok) return JS_UNDEFINED;
                return qjs::newDouble(ctx, r.fps).take();
            case GameProp::Mouse:
                if (!r.mouse_ok) return JS_UNDEFINED;
                return makeMouseObject(ctx, r).take();
            case GameProp::Script:
                // `script_ok && 非空`（context §4 的括号）——空载荷 ⇒ undefined，⛔ 不画空行。
                if (!r.script_ok || r.script.empty()) return JS_UNDEFINED;
                return qjs::newString(ctx, r.script).take();
            case GameProp::Worst:
                if (!r.worst_ok) return JS_UNDEFINED;
                return makeWorstObject(ctx, r).take();
        }
        return JS_UNDEFINED;
    } catch (...) {
        return JS_ThrowTypeError(ctx, "game: 宿主异常（C++ exception at script boundary）");
    }
}

// 在 obj 上装一个"只有 getter"的属性（⛔ 不用普通数据属性；strict 赋值 ⇒ TypeError）。
// `magic < 0` = 普通 4 参 trampoline（jsGameObject）；否则走 5 参 magic trampoline（jsGameProperty）。
bool defineGetterOnly(JSContext* ctx, JSValueConst obj, const char* name, JSCFunction* fn,
                      JSCFunctionMagic* magic_fn, int magic, std::string* err) {
    Value getter(ctx, magic < 0 ? JS_NewCFunction(ctx, fn, name, 0)
                                : JS_NewCFunctionMagic(ctx, magic_fn, name, 0, JS_CFUNC_generic_magic, magic));
    if (getter.isException()) {
        qjs::clearException(ctx);
        if (err != nullptr) *err = std::string("quickjs: 创建 getter ") + name + " 失败";
        return false;
    }
    const JSAtom atom = JS_NewAtom(ctx, name);
    if (atom == JS_ATOM_NULL) {
        qjs::clearException(ctx);
        if (err != nullptr) *err = std::string("quickjs: 属性名 ") + name + " 失败";
        return false;
    }
    // JS_DefinePropertyGetSet 取得 getter / setter 的所有权。
    const int rc = JS_DefinePropertyGetSet(ctx, obj, atom, getter.take(), JS_UNDEFINED,
                                           JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, atom);
    if (rc < 0) {
        qjs::clearException(ctx);
        if (err != nullptr) *err = std::string("quickjs: 定义 getter ") + name + " 失败";
        return false;
    }
    return true;
}

// 装 ui（三个函数）+ game（只读 getter 绑定）。
bool installHostApi(Impl& impl, std::string* err) {
    JSContext* ctx = impl.ctx;
    Value global = qjs::getGlobalObject(ctx);
    if (global.isException()) {
        qjs::clearException(ctx);
        if (err != nullptr) *err = "quickjs: 取 globalThis 失败";
        return false;
    }

    bool ok = true;
    Value ui = qjs::newObject(ctx);
    if (ui.isException()) {
        qjs::clearException(ctx);
        ok = false;
        if (err != nullptr) *err = "quickjs: 分配 ui 对象失败";
    }
    if (ok) ok = setFunction(ctx, ui.raw(), "rect", jsUiRect, 1, err);
    if (ok) ok = setFunction(ctx, ui.raw(), "text", jsUiText, 1, err);
    if (ok) ok = setFunction(ctx, ui.raw(), "measure", jsUiMeasure, 2, err);
    if (ok) ok = setProperty(ctx, global.raw(), "ui", std::move(ui), err);

    if (ok) {
        Value game = qjs::newObject(ctx);
        if (game.isException()) {
            qjs::clearException(ctx);
            ok = false;
            if (err != nullptr) *err = "quickjs: 分配 game 对象失败";
        }
        if (ok) {
            ok = defineGetterOnly(ctx, game.raw(), "frame", nullptr, jsGameProperty,
                                  static_cast<int>(GameProp::Frame), err);
        }
        if (ok) ok = defineGetterOnly(ctx, game.raw(), "fps", nullptr, jsGameProperty,
                                          static_cast<int>(GameProp::Fps), err);
        if (ok) ok = defineGetterOnly(ctx, game.raw(), "mouse", nullptr, jsGameProperty,
                                          static_cast<int>(GameProp::Mouse), err);
        if (ok) {
            ok = defineGetterOnly(ctx, game.raw(), "script", nullptr, jsGameProperty,
                                  static_cast<int>(GameProp::Script), err);
        }
        if (ok) ok = defineGetterOnly(ctx, game.raw(), "worst", nullptr, jsGameProperty,
                                          static_cast<int>(GameProp::Worst), err);
        // ★ `local_frames` 是**方法**（全对象唯一的非 getter 属性）：按需查询帧列表，宿主不推表。
        //   `JS_DefinePropertyValueStr(..., 0)` ⇒ 不可写 / 不可枚举 / 不可配置（strict 赋值 ⇒ TypeError）。
        //   变异：把它改成普通数据属性 / 删掉注册 ⇒ `ui-frames` 的 JS 面用例红。
        if (ok) {
            Value local_frames(ctx, JS_NewCFunction(ctx, jsGameLocalFrames, "local_frames", 0));
            if (local_frames.isException()) {
                qjs::clearException(ctx);
                ok = false;
                if (err != nullptr) *err = "quickjs: 创建 game.local_frames 失败";
            } else if (JS_DefinePropertyValueStr(ctx, game.raw(), "local_frames", local_frames.take(), 0) < 0) {
                qjs::clearException(ctx);
                ok = false;
                if (err != nullptr) *err = "quickjs: 定义 game.local_frames 失败";
            }
        }
        if (ok) {
            impl.game_obj = std::move(game);  // 持有（getter 每次 dup 返回）
            if (!defineGetterOnly(ctx, global.raw(), "game", jsGameObject, nullptr, -1, err)) {
                impl.game_obj.reset();
                ok = false;
            }
        }
    }
    return ok;
}

// =======================================================================================
// UiScript / ScriptRuntime 的实现
// =======================================================================================
class QuickJsScript final : public UiScript {
public:
    explicit QuickJsScript(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
    ~QuickJsScript() override {
        if (impl_) impl_->shutdown();
    }

    bool render(UiBuilder& builder, const UiFrameReadings& readings, std::string* err) override {
        Impl& impl = *impl_;
        if (impl.ctx == nullptr) {
            if (err != nullptr) *err = "quickjs: 脚本已 shutdown";
            return false;
        }
        impl.builder = &builder;
        impl.current = &readings;
        impl.ops = 0;
        impl.overran = false;
        bool ok = false;
        try {
            Value result(impl.ctx, JS_Call(impl.ctx, impl.render.raw(), JS_UNDEFINED, 0, nullptr));
            ok = !result.isException();
            if (!ok) qjs::takeException(impl.ctx, impl.origin, err);
        } catch (...) {
            if (err != nullptr) *err = impl.origin + ": 宿主异常（C++ exception at script boundary）";
            ok = false;
        }
        // ★ 无论成败都复位指针：下一次 render 不能看到上一次的 builder / readings。
        impl.builder = nullptr;
        impl.current = nullptr;
        return ok;
    }

    void setBudget(std::uint64_t deadline_ns, std::uint32_t op_limit) override {
        impl_->deadline_ns = deadline_ns;
        impl_->op_limit = op_limit;
    }

    bool overran() const override { return impl_->overran; }

    void shutdown() override { impl_->shutdown(); }

private:
    std::unique_ptr<Impl> impl_;
};

class QuickJsRuntime final : public ScriptRuntime {
public:
    std::unique_ptr<UiScript> load(const std::string& source, const std::string& origin, FontEngine* font,
                                   std::string* err) override {
        auto impl = std::make_unique<Impl>();
        impl->origin = origin;
        impl->font = font;

        impl->rt = JS_NewRuntime();
        if (impl->rt == nullptr) {
            if (err != nullptr) *err = origin + ": quickjs: JS_NewRuntime 失败";
            return nullptr;
        }
        // ★ 三个预算在**建 context 之前**设（context §5）。
        JS_SetMemoryLimit(impl->rt, kMemoryLimitBytes);
        JS_SetMaxStackSize(impl->rt, kStackLimitBytes);
        JS_SetInterruptHandler(impl->rt, &interruptHandler, impl.get());
#ifndef NDEBUG
        // 开发构建：JS_FreeRuntime 时把泄漏的对象 / 字符串打出来；产品构建（NDEBUG）关。
        JS_SetDumpFlags(impl->rt, JS_DUMP_LEAKS);
#endif

        impl->ctx = JS_NewContextRaw(impl->rt);
        if (impl->ctx == nullptr) {
            if (err != nullptr) *err = origin + ": quickjs: JS_NewContextRaw 失败";
            return nullptr;
        }
        JS_SetContextOpaque(impl->ctx, impl.get());
        JS_AddIntrinsicBaseObjects(impl->ctx);
        // ★ 必须加：只为让**宿主**的 JS_Eval 能编译脚本（文件头 §1）；随后把脚本入口删掉。
        JS_AddIntrinsicEval(impl->ctx);

        std::string setup_err;
        if (!installSandbox(*impl, &setup_err) || !installHostApi(*impl, &setup_err)) {
            // 沙箱 / 注入是**宿主**的失败：有引擎异常就经 takeException 取走（带 origin:line:col），
            // 没有就用我们自己点名的文本。⛔ 两条路径都必须让 ctx 干净地走出 load。
            if (JS_HasException(impl->ctx)) {
                qjs::takeException(impl->ctx, origin, err);
            } else if (err != nullptr) {
                *err = origin + ": " + setup_err;
            }
            return nullptr;
        }

        Value eval_result(impl->ctx, JS_Eval(impl->ctx, source.c_str(), source.size(), origin.c_str(),
                                             JS_EVAL_FLAG_STRICT));
        if (eval_result.isException()) {
            qjs::takeException(impl->ctx, origin, err);
            return nullptr;
        }

        Value global = qjs::getGlobalObject(impl->ctx);
        if (global.isException()) {
            qjs::takeException(impl->ctx, origin, err);
            return nullptr;
        }
        Value render = qjs::getProperty(impl->ctx, global.raw(), "render");
        if (render.isException()) {
            qjs::takeException(impl->ctx, origin, err);
            return nullptr;
        }
        if (!render.isFunction()) {
            if (err != nullptr) {
                *err = origin +
                       ": 脚本没有定义 `globalThis.render`（入口约定 = globalThis.render = function "
                       "render() { … }；它必须是函数）";
            }
            return nullptr;
        }
        impl->render = std::move(render);
        return std::make_unique<QuickJsScript>(std::move(impl));
    }
};

}  // namespace

std::unique_ptr<ScriptRuntime> makeQuickJsRuntime() { return std::make_unique<QuickJsRuntime>(); }

}  // namespace lift::ui
