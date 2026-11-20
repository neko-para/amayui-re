// emulator-lift · `Step[]` 的**唯一** JSON 出口（C3：parser 只产数据，⛔ 不执行）
//
// 合同：`docs/03-emulator/plugin-runtime.md` §5 C3（`:1166` 的"`Recipe/Step` → `Step[]`（JSON/ABI），
// **不执行**"）。判据 = CTest `recipe-driver-parity`（`tests/recipe_driver_parity.cpp`）。
//
// ★ 为什么是**独立 TU**（不是往 `recipe.cpp` 里加函数）：`src/runtime/recipe.cpp` 是**冻结的解析器**
//   （合同 `:1167` 与 §6 行 13/15）⇒ "parser 没被改"这件事**不该靠审 diff 才知道**：本文件只
//   `#include "runtime/recipe.h"` 并**只读** `Step` 的字段，⛔ 不调用任何施加 / 推进 / 判定函数，
//   ⛔ 不在 `recipe.*` 里加一个字符。⇒ 冻结面的 diff 恒为空。
// ★ **不执行**：本文件产出的字符串只描述"有哪些指令、参数是什么"；它**不**决定门成不成立、
//   **不**算完成度、**不**给退出码（那些仍是 `recipe.h` 的唯一一份判据）。
//
// ★ 形状（driver 侧 `scripts/plugins/lib/recipe.js` 逐字按这个读；字段**全量**、不留 undefined）：
//
//     { "origin": "<配方路径>",
//       "steps": [ { "id": "step1", "kind": "await script", "line": 3,
//                    "script": "TITLE", "posFrom": 0, "posTo": 0, "waitMs": 0,
//                    "x": 0, "y": 0, "button": "left", "label": "", "timeoutMs": 0 } ] }
//
//   · `kind` = `stepKindName()` 的**同一份**字符串（⛔ 不用枚举整数：那会让"指令名"出现第二份，
//     而 `StepKind` 是闭集合 —— 名字漂不漂得有人看得见）；
//   · `button` = `"left" | "right" | "middle"`（`MouseButton` 的完整映射；缺省就是 `left`）；
//   · `posFrom` / `posTo` 是**十进制**无符号数（`stepText` 里的 `0x%08X` 只是显示口径）；
//   · `timeoutMs` = 0 ⇒ 这一行没写行尾 `timeout`（"没有守卫"，⛔ 不是"立即到期"）。
#pragma once

#include <string>
#include <vector>

#include "runtime/recipe.h"

namespace lift {

// 一条 `Step` 的 JSON 对象（不含外层 `{}` 之外的空白；调用方自己拼数组）。
std::string stepJson(const Step& step);

// `Step[]` 数组（`[ {...}, ... ]`）。
std::string stepsJson(const std::vector<Step>& steps);

// `{ "origin": …, "steps": [...] }`：parity 夹具用它生成 JS 侧的 steps 模块
// （`origin` 只用于 trace / 报告，⛔ 不参与任何判定）。
std::string stepsJson(const Recipe& recipe);

}  // namespace lift
