// emulator-lift · 控制层对 script global 的**请求 / 答复形状**（只给 `control_ws.cpp` 与 pure CTest）
//
// ★ 分层（父级 2026-11-19 裁决）：寻址 / ENC-DEC / 类型编码在 `runtime/script_globals.h`
//   （**AGE 引擎层**，可替换；⚠ 是否被其它 AGE 作品复用 = 待证）；
//   本头只放**控制层口径**：JSON 请求字段的解析、十六进制字面量（CLI / 线上的唯一写法）、
//   以及 `data.global{...}` 的 JSON 文本。⛔ 本头不 include beast / nlohmann（实现只在 control_ws.cpp）。
//
// ★ 十六进制口径（父级 2026-11-19 追加裁决，覆盖早先"十进制或 0x 都接受"）：
//   · `index`（global id）与 int/uint 的 `value` **必须是带 `0x` 前缀的十六进制字符串**；
//     纯十进制一律拒（脚本里这些字段都是 hex raw，支持十进制造的是歧义，不是便利）。
//   · `float` 的 `value` 是**唯一例外**：JSON number（十进制浮点字面量）—— 它不是脚本字段编码，
//     而是 IEEE-754 裸 32 位的语义值。
//
// ★ 答复：`data.global{kind, index, value, raw}`；`value` / `raw` 一律 `0x%08X`（便于与脚本 / 日志
//   逐位对照）。float 的 `value` 是十进制 JSON number（非有限 ⇒ `null`；位模式看 `raw`）。
#pragma once

#include <cstdint>
#include <string>

#include "runtime/script_globals.h"

namespace lift {

// JSON 文本 → 控制层请求。`want_value` = op 是 `global-set`。
//   ★ 这是**唯一的请求解析实现**：服务端 `parseCommand` 与 pure CTest 都走它（⛔ 不另写一份）。
//   ★ `error != None` 时其余字段无意义；`detail` 是给人看的一句话（含"该怎么写"）。
struct ControlGlobalRequest {
    GlobalKind kind = GlobalKind::Int;
    std::uint32_t index = 0;
    bool has_value = false;
    std::uint32_t u32 = 0;  // int / uint（hex 字面量解析后的 32 位）
    double f64 = 0.0;       // float（JSON number）
    GlobalError error = GlobalError::None;
    std::string detail;
};

ControlGlobalRequest parseControlGlobalRequest(const std::string& json_text, bool want_value);

// `data.global{...}` 的 JSON 文本（纯函数；同输入 ⇒ 逐字节同输出）。
std::string controlGlobalJson(const GlobalValue& value);

// 十六进制字面量：**只认 `0x` 前缀**（大小写不敏感），且必须落在 32 位内。
//   ★ 两个入口只是为了错误信息能点名是 `index` 还是 `value`；解析规则同一份。
GlobalError parseGlobalIndexLiteral(const std::string& text, std::uint32_t* out);
GlobalError parseGlobalValueLiteral(const std::string& text, std::uint32_t* out);

}  // namespace lift
