// emulator-lift · 注入指针的**叠层路由**接口（S9；SDL-free）
//
// ★ 判据来源：`docs/03-emulator/overlay.md` 的 D6（输入单一 router）与 D2（`--ui-input`）；
//   设计：`.tmp/input-s9/design.md`。D6 的三个入口 = OS 事件 / 注入 down / 注入 up，
//   三者都在调用 `input::*` **之前**问**同一个** `UiClickRouter` 状态机（OS 入口见
//   `present_sdl.cpp:Impl::handleEvent`；注入入口见 `input_plan.cpp:InputPlan::armClickAt` /
//   `emitRelease`）。本头只描述"注入"那一半需要的最小接口 —— ⛔ 不是第二套命中 / 捕获实现：
//   实现者（`SdlWindow::Impl`）内部仍然只调 `ui/hit.h` 的 `ClickRouter::handlePointer`。
//
// ★ **坐标口径**：`client_x/client_y` 是 **guest 客户区**（与 `input::mouseMove` 同一个空间，
//   = `GetCursorPos` − 窗口原点）。实现者负责自己那一跳（`overlayPointerWindowPixels`：
//   guest 客户区 → letterbox 窗口点 → DPR 窗口像素 = 命中表的空间）—— 本接口不定义两套坐标。
//
// ★ **调用时机**：实现方保证 router 只在调用 `input::*` **之前**被问；返回 true = 叠层吞掉这条
//   指针事件（调用方 **不得**调用 `input::*`）。返回 false = 放行（照旧施加）。
//   ⛔ 不要把这个类做成"第二个 ClickRouter"：它只是 `ClickRouter` 在注入路径上的转接点。
//
// ★ 为什么放在 `runtime/` 而不是 `ui/`：`InputPlan`（`runtime/input_plan.cpp`）必须能在**不 include
//   SDL / 不 include ui/hit.h** 的形状下问路由（pure CTest `input-plan-read-points` 直接用它）；
//   本头只依赖 `input_device.h` 的 `MouseButton`。
#pragma once

#include <cstdint>

#include "runtime/input_device.h"

namespace lift {

// 注入指针的两个相（与 OS 路径的 `ui::UiPointerPhase` 同义；⛔ 不在这里带 SDL 类型）。
enum class UiPointerPhase : std::uint8_t { Down = 0, Up = 1 };

// 注入指针的叠层路由（非拥有；生命周期 = 装配者，通常是 `SdlWindow::Impl`）。
class UiInputRouter {
public:
    virtual ~UiInputRouter() = default;
    // 返回 true = 被叠层吞掉（调用方不得调用 `input::*`）；`client_x/y` = guest 客户区。
    // ★ 只问左键：右键 / 中键 / 滚轮 / motion 一律放行（D6）。实现方自己判按钮，调用方不必先过滤。
    virtual bool routeInjectedPointer(UiPointerPhase phase, MouseButton button, int client_x, int client_y) = 0;
};

// =====================================================================================
// `--ui-input=consume|pass`（CLI 值域；唯一一份）
//
// ★ 缺省 = `Consume`（与 D2 / R3 一致：默认档带 UI 时"点 HUD 就是点 HUD"）；
//   `Pass` = 注入点击**完全不问** router ⇒ guest 逐位与没有 UI 相同（R3 的可复现性开关）。
// ★ **只在 `--overlay` / `--ui` 打开时才有意义**：没有叠层 ⇒ 没有 router ⇒ 两种模式都是 no-op。
// ★ 纯函数 / 零分配：CLI（`aot_main.cpp`）与 `SdlWindowOptions` 共用这一份值域，
//   ⛔ 不在 CLI 里另写一套字符串比较（那是第二份真源）。
// =====================================================================================
enum class UiInputMode : std::uint8_t { Consume = 0, Pass = 1 };

struct UiInputModeParse {
    bool ok = false;
    UiInputMode mode = UiInputMode::Consume;  // ok == false 时无意义
    const char* error = nullptr;              // ok == false 时点名合法值
};

inline constexpr const char* kUiInputModeValues = "consume|pass";

inline UiInputModeParse parseUiInputMode(const char* text) {
    UiInputModeParse out;
    if (text == nullptr) {
        out.error = "只能是 consume 或 pass";
        return out;
    }
    // 逐字符比较（不构造 std::string；纯函数在 pure CTest 里直接可用）。
    const char* p = text;
    if (p[0] == 'c' && p[1] == 'o' && p[2] == 'n' && p[3] == 's' && p[4] == 'u' && p[5] == 'm' &&
        p[6] == 'e' && p[7] == '\0') {
        out.ok = true;
        out.mode = UiInputMode::Consume;
        return out;
    }
    if (p[0] == 'p' && p[1] == 'a' && p[2] == 's' && p[3] == 's' && p[4] == '\0') {
        out.ok = true;
        out.mode = UiInputMode::Pass;
        return out;
    }
    out.error = "只能是 consume 或 pass";
    return out;
}

}  // namespace lift
