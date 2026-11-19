// emulator-lift · 插件回调的**上下文**（C1b 只落指针聚合的形状；C1c/C2 往里填实现）
//
// 合同：`docs/03-emulator/plugin-runtime.md` §4.5（`Context` 的字段语义）、§6 行 5 / 22 / 26
//       （操作类唯一路径 / 无第二 Input 施加点 / sample·screenshot 不进回调）。
// 施工图：`.tmp/plugin-runtime/c1-design-review.md` §4.3（为什么这样堵重入）/ §5.7（本头就是那一段）。
//
// ★ **指针聚合**（合同草稿用的是引用）：指针可默认构造 / 可赋值 / 可判空，而且"没接线"能响亮失败
//   （`wired()` 是唯一判据）。⛔ 不要改成引用：那就不能"先建 Context 再接线"，测试也没法只接一半。
//
// ★ 两个**结构性**的边界（⛔ 都不是运行期标志，别加 `paused` / `headless` / `inCallback`）：
//   ① 本头里**没有** `WindowManager` / `GuestMemory`：回调拿不到施加输入的入口（合同 §4.5 711–713）；
//   ② `trigger` 是 **`TriggerFactory*`**，⛔ 不是 `TriggerRunner*`：唯一 pump 不交给回调
//      （否则回调能递归 `atPoint`，也能绕过"返回值替换集合"的语义）。本头**不认识** TriggerRunner。
//
// ★ C1b 只前向声明 `EditSink` / `IntentQueue` / `EngineRead`（它们由 C1c 定义）：
//   这里只放指针 ⇒ 不需要它们的定义，也不会把 C1c 的形状拖进本切片。
#pragma once

#include "runtime/trigger.h"  // TriggerReadings / TriggerFactory（`now` 与 `trigger` 的指向类型）

namespace lift::plugin {

class EditSink;     // C1c：Edit（立即写；⛔ 不排队）
class IntentQueue;  // C1c：Input（串行；一读点一条）
class EngineRead;   // C1c：EngineRead（同步只读；无暂停）

struct Context {
    // 本读点的读数快照（每个 pump 由驱动方指向自己那份 ⇒ 回调只读、不带可变句柄）。
    const TriggerReadings* now = nullptr;
    // Edit：立即生效（暂停 / 控制点也可）；Input：进队列（下一读点 / 一读点一条）。
    EditSink* edit = nullptr;
    IntentQueue* input = nullptr;
    EngineRead* read = nullptr;
    // ★ 工厂（造 trigger），⛔ 不是 runner：见头注 ②。
    TriggerFactory* trigger = nullptr;

    // 五根线全接上才算可用。⚠ C1b 的 trigger 用例只接 `now` + `trigger`（其余留空）：
    //   本切片的 handler 不碰 Edit/Input/Read，所以**不**用 `wired()` 当门（那是 C1c/C2 的事）。
    bool wired() const { return now && edit && input && read && trigger; }
};

}  // namespace lift::plugin
