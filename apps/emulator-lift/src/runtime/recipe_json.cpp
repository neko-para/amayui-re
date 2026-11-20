// emulator-lift · `Step[]` 的 JSON 实现（**只读** `Step`；口径见 `recipe_json.h` 的文件头）
//
// ⛔ 本文件不出现任何"执行"语义：没有门判定、没有时钟、没有 `WindowManager`、没有 `InputPlan`。
//    它唯一的输入是 `const Step&` / `const std::vector<Step>&`。
#include "runtime/recipe_json.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace lift {
namespace {

// JSON 字符串转义。★ 只转 RFC 8259 要求的那几个：`"` / `\` / 控制字符（< 0x20）。
//   ⛔ 不做 \u 转义（配方里的名字 / label 都是 ASCII 单词；非 ASCII 原样按 UTF-8 字节输出，
//   而 UTF-8 字节在 JSON 字符串里是合法的）。
std::string quoted(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('"');
    for (const char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (u < 0x20u) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04X", static_cast<unsigned>(u));
                    out += buf;
                } else {
                    out.push_back(c);
                }
                break;
        }
    }
    out.push_back('"');
    return out;
}

std::string u32s(std::uint32_t v) { return std::to_string(static_cast<unsigned long long>(v)); }
std::string u64s(std::uint64_t v) { return std::to_string(static_cast<unsigned long long>(v)); }
std::string is(std::int64_t v) { return std::to_string(static_cast<long long>(v)); }

// 按钮词（与 `Step::button` 的完整映射；⛔ 不把 middle 静默写成 left）。
const char* buttonWord(MouseButton b) {
    switch (b) {
        case MouseButton::Left: return "left";
        case MouseButton::Right: return "right";
        case MouseButton::Middle: return "middle";
    }
    return "left";
}

}  // namespace

std::string stepJson(const Step& s) {
    std::string out = "{";
    out += "\"id\":" + quoted(s.id);
    // ★ `kind` 用 `stepKindName()` 的同一份字符串（闭集合的唯一名字源）。
    out += ",\"kind\":" + quoted(stepKindName(s.kind));
    out += ",\"line\":" + is(s.line);
    out += ",\"script\":" + quoted(s.script);
    out += ",\"posFrom\":" + u32s(s.pos_from);
    out += ",\"posTo\":" + u32s(s.pos_to);
    out += ",\"waitMs\":" + u64s(s.wait_ms);
    out += ",\"x\":" + is(s.x);
    out += ",\"y\":" + is(s.y);
    out += ",\"button\":" + quoted(buttonWord(s.button));
    out += ",\"label\":" + quoted(s.label);
    out += ",\"timeoutMs\":" + u64s(s.timeout_ms);
    out += "}";
    return out;
}

std::string stepsJson(const std::vector<Step>& steps) {
    std::string out = "[";
    for (std::size_t i = 0; i < steps.size(); ++i) {
        if (i != 0) out += ",";
        out += stepJson(steps[i]);
    }
    out += "]";
    return out;
}

std::string stepsJson(const Recipe& recipe) {
    return "{\"origin\":" + quoted(recipe.origin()) + ",\"steps\":" + stepsJson(recipe.steps()) + "}";
}

}  // namespace lift
