// emulator-lift · **读点观察者**（投递层唯一的通用缝）
//
// 合同：`docs/03-emulator/plugin-runtime.md` §4.2（trigger poll 在**每个 guest 读点**）、
//       §5 C2（`plugin/headless` 必须在真 guest 读点上 poll）。
// 父级裁决：`.tmp/plugin-runtime/c2-decisions.md` D-C2-1（**批准**这条缝，但不叫 `pluginPump`）。
//
// ★ 为什么是"读点观察者"而不是 `pluginPump`：这是"一个读点发生了"的**通用**通知面
//   （谁想在这个拍子上做事就实现它），不是插件专属的私线。C2 只**借用**它把
//   `IntentExecutor::atPoint` 接到 `input.h` 的四个投递口上；C4 收编时把 `ControlInput` 也变成
//   观察者（或删掉双指针）—— 那时**只剩一个泵**。⇒ ⛔ 不要在这里写"插件"三个字以外的语义，
//   ⛔ 也不要给它加 `armed()` / 预算 / 时钟（那些是被通知方自己的账）。
//
// ★ 与 `WindowManager::setControlInput` **同形**：非拥有裸指针、`nullptr` = 没有观察者。
//   空指针路径只多**一次判空**（`input.h` 的四个 inline 各加一句）⇒ 现有输出/行为逐字节不变。
//
// ★ 守卫：本头属于**核心面** ⇒ 在 `tools/test/core-shape.test.mjs` 的 `CORE_FILES` 里逐文件列出
//   （禁用词不变：核心不许长出 plan 状态机）。
#pragma once

#include "runtime/input_point.h"

namespace lift {

class WindowManager;

class ReadPointObserver {
public:
    virtual ~ReadPointObserver() = default;
    // 一个 guest 读点发生了（`point` = 11 个投递点里的一个；每读点恰好一次）。
    // ⛔ 实现方不许在这里改 guest 语义（输入是外部数据）：它只能排队 / 采样 / 计数。
    virtual void onReadPoint(WindowManager& wm, InputPoint point) = 0;
};

}  // namespace lift
