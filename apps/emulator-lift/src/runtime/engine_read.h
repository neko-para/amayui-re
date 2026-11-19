// emulator-lift · **`EngineRead`** —— 插件侧对引擎数据的**同步只读**口（C1c）
//
// 合同：`docs/03-emulator/plugin-runtime.md` §4.4（引擎数据读；**不是** sample / screenshot）、
//       §4.5 704（`ctx.read.*` 同步只读、**无暂停**、与 Edit 同点组合 `read → edit → input`）。
// 施工图：`.tmp/plugin-runtime/c1-design-review.md` §5.7（本头就是那一段的落地）/ §8（越界清单）。
//
// ★ **C1 只有 `globalGet`**。另外三个读口各有各的**未完成前提**，⛔ 本切片不声明：
//   · `scriptPos()`：要 `MarkerSampler`（`engine_marker.h` 有），但它的接线是 C4 的 control-point 面；
//   · `frames()`：要 `EngineFrameSource` 的 UI 帧视图（C2/C4）；
//   · `frameHash()`：runtime 里**没有**帧 hash 源（唯一实现在宿主的 `Presenter::lastHash()`，
//     实例归 `AotHost`）⇒ 只能由宿主注入一个非 owning 指针（C4）。
//   声明了却返回 0 的"占位实现"比没有声明更糟：调用方会以为自己在读真实数据。
//
// ★ **无暂停**（合同 §4.4 507–509）：本口不许 pause / 不许要 `state_seq` / 不许序列化 ——
//   那是 `emu-ctl sample` 的**组合命令**，不是插件回调 API。
//
// ★ 返回 `bool + out + err`（而不是合同草稿的 `GlobalValue globalGet(...)`）：它逐字对得上
//   `runtime/script_globals.h` 的 `readGlobal`（唯一的读实现），且失败原因走**同一份** `GlobalError`
//   词表（`globalErrorName`）⇒ 不需要第二套错误码。
#pragma once

#include <cstdint>

#include "runtime/script_globals.h"  // GlobalKind / GlobalValue / GlobalError（唯一的引擎层词汇）

namespace lift::plugin {

class EngineRead {
public:
    virtual ~EngineRead() = default;

    // 读一个 script global（`Int` / `Uint` / `Float`；`String` 本实现未实现 ⇒ `TypeMismatch`）。
    // 成功 ⇒ true 且 `*err = None`；失败 ⇒ false 且 `*err` 是 `globalErrorName()` 认的那三个之一。
    virtual bool globalGet(GlobalKind kind, std::uint32_t id, GlobalValue* out, GlobalError* err) = 0;
};

}  // namespace lift::plugin
