// emulator-lift · QuickJS 值的 **RAII 包装 + 转换 helper**（A：QuickJS 内核的私有层）
//
// ★ 本头是**唯一**允许 `#include "quickjs.h"` 的头文件；唯一的消费者是
//   `src/ui/script_quickjs.cpp`（公开头 `script.h` / `script_quickjs.h` 零 qjs 类型）。
// ★ 为什么有这一层（用户 2026-11-18 裁决）：QuickJS 的 C API 是"每个 JSValue 恰好一个所有者"
//   + "出错时异常挂在 ctx 上"两条纪律。裸 `JSValue` 局部 + 手工 `JS_FreeValue` 很容易在
//   提前 return / catch 路径上漏释放或把 `JS_EXCEPTION` 当值释放。这里的形状照 MaaFramework 的
//   `spec/quickjs/wrapper.h:QjsValue` + `spec/convert.h:JSConvert<T>`：**一个 JSValue 一个所有者**、
//   拷贝 = `JS_DupValue`、move 置 `JS_UNDEFINED`、析构 = `JS_FreeValue`。
// ★ 每个 `Value` 都带 `JSContext*`（`JS_FreeValue` 需要 ctx）⇒ 跨 runtime 的拷贝是不合法的，
//   也不该出现：宿主只在一次 load / 一帧 render 的调用栈内持有值。
// ★ 转换 helper 的失败口径 = **异常已挂 ctx**（`false` ⇒ 调用方必须走 `takeException`）。
//   `setProperty` 取得值所有权（`take()`），失败也一样消费掉 —— 与 JS_SetPropertyStr 的契约一致。
#pragma once

#include "quickjs.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace lift::ui::qjs {

// =======================================================================================
// Value：一个 JSValue 一个所有者
// =======================================================================================
class Value {
public:
    Value() = default;
    // 接管 raw（调用方把所有权交出来；⛔ 之后不要再 FreeValue）。
    Value(JSContext* ctx, JSValue raw) : ctx_(ctx), raw_(raw) {}

    ~Value() { reset(); }

    Value(const Value& other) : ctx_(other.ctx_), raw_(dupRaw(other)) {}
    Value& operator=(const Value& other) {
        if (this == &other) return *this;
        reset();
        ctx_ = other.ctx_;
        raw_ = dupRaw(other);
        return *this;
    }
    Value(Value&& other) noexcept : ctx_(other.ctx_), raw_(other.raw_) {
        other.ctx_ = nullptr;
        other.raw_ = JS_UNDEFINED;
    }
    Value& operator=(Value&& other) noexcept {
        if (this == &other) return *this;
        reset();
        ctx_ = other.ctx_;
        raw_ = other.raw_;
        other.ctx_ = nullptr;
        other.raw_ = JS_UNDEFINED;
        return *this;
    }

    // 释放持有的值（恢复成"没有值"状态；ctx_ 保留以便继续接管新值）。
    void reset() {
        if (ctx_ != nullptr) JS_FreeValue(ctx_, raw_);
        raw_ = JS_UNDEFINED;
    }

    // 释放持有的值并**忘掉 ctx**（用于 ctx 即将被 JS_FreeContext 的场合：析构时不再碰 dangling ctx）。
    void invalidate() {
        reset();
        ctx_ = nullptr;
    }

    // 借出（不转移所有权）：给只读 C API（JSValueConst）。
    JSValueConst raw() const { return raw_; }
    // 转移所有权：给"吃所有权"的 C API（JS_SetPropertyStr / JS_DefinePropertyGetSet…）。
    // ★ take() 之后本对象仍可析构（持有 JS_UNDEFINED，FreeValue 是 no-op）。
    JSValue take() {
        const JSValue value = raw_;
        raw_ = JS_UNDEFINED;
        return value;
    }
    // 一个新的持有引用（refcount +1）。
    Value dup() const { return Value(ctx_, dupRaw(*this)); }

    JSContext* ctx() const { return ctx_; }
    bool ok() const { return ctx_ != nullptr; }

    bool isUndefined() const { return JS_IsUndefined(raw_); }
    bool isNull() const { return JS_IsNull(raw_); }
    bool isException() const { return JS_IsException(raw_); }
    bool isObject() const { return JS_IsObject(raw_); }
    bool isString() const { return JS_IsString(raw_); }
    bool isNumber() const { return JS_IsNumber(raw_); }
    bool isFunction() const { return ctx_ != nullptr && JS_IsFunction(ctx_, raw_); }

    static Value undefined(JSContext* ctx) { return Value(ctx, JS_UNDEFINED); }
    static Value null(JSContext* ctx) { return Value(ctx, JS_NULL); }
    static Value exception(JSContext* ctx) { return Value(ctx, JS_EXCEPTION); }

private:
    static JSValue dupRaw(const Value& other) {
        return other.ctx_ != nullptr ? JS_DupValue(other.ctx_, other.raw_) : JS_UNDEFINED;
    }

    JSContext* ctx_ = nullptr;
    JSValue raw_ = JS_UNDEFINED;
};

// =======================================================================================
// 转换 helper（全 UTF-8；失败 = 异常已挂 ctx，⛔ 不吞）
// =======================================================================================
// ★ 出参形态：`JS_ToBool` 异常时返回 **-1**，不能直接 `!= 0`（那会把异常当 true）。
inline bool toBool(JSContext* ctx, JSValueConst v, bool* out) {
    const int rc = JS_ToBool(ctx, v);
    if (rc < 0) return false;
    *out = rc != 0;
    return true;
}

inline bool toInt32(JSContext* ctx, JSValueConst v, std::int32_t* out) {
    return JS_ToInt32(ctx, out, v) >= 0;
}

inline bool toUint32(JSContext* ctx, JSValueConst v, std::uint32_t* out) {
    return JS_ToUint32(ctx, out, v) >= 0;
}

inline bool toDouble(JSContext* ctx, JSValueConst v, double* out) { return JS_ToFloat64(ctx, out, v) >= 0; }

inline bool toString(JSContext* ctx, JSValueConst v, std::string* out) {
    std::size_t len = 0;
    const char* text = JS_ToCStringLen2(ctx, &len, v, false);
    if (text == nullptr) return false;
    out->assign(text, len);
    JS_FreeCString(ctx, text);
    return true;
}

inline Value newString(JSContext* ctx, const std::string& text) {
    return Value(ctx, JS_NewStringLen(ctx, text.data(), text.size()));
}
inline Value newObject(JSContext* ctx) { return Value(ctx, JS_NewObject(ctx)); }
inline Value newInt32(JSContext* ctx, std::int32_t v) { return Value(ctx, JS_NewInt32(ctx, v)); }
inline Value newInt64(JSContext* ctx, std::int64_t v) { return Value(ctx, JS_NewInt64(ctx, v)); }
inline Value newDouble(JSContext* ctx, double v) { return Value(ctx, JS_NewFloat64(ctx, v)); }
inline Value newBool(JSContext* ctx, bool v) { return Value(ctx, JS_NewBool(ctx, v)); }

inline Value getGlobalObject(JSContext* ctx) { return Value(ctx, JS_GetGlobalObject(ctx)); }

inline Value getProperty(JSContext* ctx, JSValueConst obj, const char* name) {
    return Value(ctx, JS_GetPropertyStr(ctx, obj, name));
}

// 写属性：取得 value 的所有权（失败也消费 —— 与 JS_SetPropertyStr 一致）。false ⇒ 异常已挂 ctx。
inline bool setProperty(JSContext* ctx, JSValueConst obj, const char* name, Value value) {
    return JS_SetPropertyStr(ctx, obj, name, value.take()) >= 0;
}

// 清掉 ctx 上的异常（只在"我们准备返回宿主错误"的路径上用；正常失败路径必须经 takeException）。
inline void clearException(JSContext* ctx) {
    if (!JS_HasException(ctx)) return;
    Value exc(ctx, JS_GetException(ctx));
}

// =======================================================================================
// 异常 → err（取走异常，ctx 上不再挂它）
//   err 形状 = `<origin>: <message>\n<stack>`；`stack` 由引擎给 `<origin>:<line>:<col>`
//   （语法错 `    at hud.js:2:6`；运行期 `    at render (hud.js:1:20)`）。
// ★ 只在 `JS_IsException(v)` 为真时调用；内部无条件 `JS_GetException`（取走 = 清掉）。
// =======================================================================================
inline void takeException(JSContext* ctx, const std::string& origin, std::string* err) {
    std::string text;
    try {
        Value exc(ctx, JS_GetException(ctx));
        std::string message;
        std::string stack;
        if (exc.isObject()) {
            Value msg = getProperty(ctx, exc.raw(), "message");
            if (msg.isException()) {
                clearException(ctx);  // 清掉取 message 时的次生异常
                msg = Value::undefined(ctx);
            }
            if (!msg.isUndefined() && !msg.isNull() && !toString(ctx, msg.raw(), &message)) {
                clearException(ctx);
            }
            Value stack_v = getProperty(ctx, exc.raw(), "stack");
            if (stack_v.isException()) {
                clearException(ctx);
                stack_v = Value::undefined(ctx);
            }
            // 非字符串的 `stack`（用户自己挂的）按"没有位置"处理，⛔ 不画蛇添足。
            if (stack_v.isString() && !toString(ctx, stack_v.raw(), &stack)) {
                clearException(ctx);
            }
        }
        if (message.empty()) {
            // 抛的是字符串 / 数字 / 普通对象 / undefined。
            if (!toString(ctx, exc.raw(), &message)) {
                clearException(ctx);
            }
        }
        if (message.empty()) message = "(no message)";
        text = origin.empty() ? message : (origin + ": " + message);
        if (!stack.empty()) text += "\n" + stack;
    } catch (...) {
        text = origin.empty() ? "quickjs: 取异常时宿主异常" : (origin + ": quickjs: 取异常时宿主异常");
    }
    if (err != nullptr) *err = std::move(text);
}

// 版本钉死：升级 quickjs-ng 时必须同时改这里与 `third_party/quickjs-ng/README.md`。
static_assert(QJS_VERSION_MAJOR == 0 && QJS_VERSION_MINOR == 18 && QJS_VERSION_PATCH == 0,
              "vendored quickjs-ng 必须是 v0.18.0（third_party/quickjs-ng/README.md）");

}  // namespace lift::ui::qjs
