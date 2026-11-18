// emulator-lift · UI 脚本的**语言无关**接口（A：QuickJS 内核）
//
// ★ 本头是**唯一**的接口面：宿主（overlay / HUD）只认 `UiScript` / `ScriptRuntime`，
//   ⛔ 不认任何 JS 引擎类型。`quickjs.h` 只允许出现在 `src/ui/script_quickjs.cpp`。
// ★ 值面只有两种：**每帧只读读数**（`UiFrameReadings`，宿主 → 脚本）与
//   **display list**（`UiBuilder`，脚本 → 宿主）。⛔ 没有 host 句柄 / native 指针给脚本。
// ★ 预算 / 失败口径：`render` 返回 false ⇒ err 必须填（带 `<origin>:<line>:<col>`）；
//   `overran()` = 本帧因为 deadline / op_limit 被打断（是 `render` false 的**原因**之一，
//   不是另一种失败：内存超限也是 false + err，但 overran 仍为 false）。
// ★ 本头**不 include** quickjs.h，也不 include 任何 UI 实现头：`UiBuilder` / `FontEngine`
//   只做前向声明（保持"接口语言无关、可被纯 CTest 直接 include"）。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lift {
class FontEngine;
}  // namespace lift

namespace lift::ui {

class UiBuilder;

// ---------------------------------------------------------------------------------------
// 帧列表（`game.local_frames()`）：guest 帧区（40 槽，`Engine+0x5D894`，步长 0x78）的**只读**视图。
// ★ 语义（取证见 `.tmp/ui-qjs/frames-research.md` 的 C1–C14；⛔ 不要在这里重新定义）：
//   · **活动** = `record+0x00 != 0` 且该地址已提交（C3）—— ⛔ 不是"id 非 0"、也⛔ 不是"0..cur 全算"；
//   · **top** = `slot == cur`（C8：引擎没有独立的栈顶变量，`cur` 是唯一执行点）；
//   · **顺序 = top-first**：`cur`（若活动）第一，其余活动槽按**槽号降序**（最小版不走 parent 链）。
// ★ `parent` / `script_id` 都是记录里的 **uint32 原值**（含 `0xFFFFFFFF` 根 / `-10` / `-11` 哨兵，C12），
//   ⛔ 不在宿主侧把负数一律当"根"。
// ---------------------------------------------------------------------------------------
struct UiFrameInfo {
    std::uint32_t slot = 0;        // 0..39
    std::uint32_t script_id = 0;   // 记录 +0x3C（统一文件 id）原样
    std::uint32_t parent = 0;      // 记录 +0x38 原样（0xFFFFFFFF = 根；0xFFFFFFF5/6 = 特殊装载）
    std::string script;            // 名字；降级时 = "id:0x%08X"
    bool name_degraded = false;    // script 是降级形态（⛔ 不代表 id / 列表无效）
    bool top = false;              // slot == cur
};

// 帧列表的**宿主取数口**（非 owning；只在 `render` 期间有效）。
// ★ `localFrames(&err)`：`err` 非空 ⇒ 这一刻**整表不可用**（脚本侧看到 `undefined`，⛔ 不编空数组）；
//   `err` 为空 ⇒ 活动帧列表（top-first）。单槽读不到 / 非活动 ⇒ **不进表**（保守跳过，不是猜）。
class UiFrameSource {
public:
    virtual ~UiFrameSource() = default;
    virtual std::vector<UiFrameInfo> localFrames(std::string* err) = 0;
};

// 宿主每帧给脚本的只读读数（⛔ 不含 raster / present —— 用户 2026-11-18 已裁决删除）。
// ★ `*_ok` = "这一项这一帧有值"；没有值时脚本侧看到 `undefined`，⛔ 不编 0 / 空串。
struct UiFrameReadings {
    std::uint64_t frame = 0;
    bool mouse_ok = false;
    int mouse_x = 0;
    int mouse_y = 0;
    bool fps_ok = false;
    double fps = 0.0;
    bool script_ok = false;
    std::string script;   // 已格式化好的载荷（名字或 `id:0x%08X（降级）`）
    bool worst_ok = false;
    double worst_wall_ms = 0.0;
    double worst_guest_ms = 0.0;
    bool worst_guest_ok = false;
    double worst_shim_ms = 0.0;
    // ★ 帧列表读数源（`game.local_frames()`；**非 owning**，只在 render 期间有效）。
    //   nullptr = 宿主没接（headless / 测试替身 / 叠层没开）⇒ 脚本侧 `undefined`。
    UiFrameSource* frames = nullptr;
};

class UiScript {
public:
    virtual ~UiScript() = default;
    // 把这一帧的 display list 追加进 builder（调用前 builder 已 clear）。
    // false = 脚本抛错 / 超预算 ⇒ err 必须填（带 origin:line:col）。
    virtual bool render(UiBuilder& builder, const UiFrameReadings& readings, std::string* err) = 0;
    // 本帧预算：deadline_ns = steady_clock 的绝对纳秒（0 = 不限）；op_limit = 字节码条数上限（0 = 不限）。
    virtual void setBudget(std::uint64_t deadline_ns, std::uint32_t op_limit) = 0;
    virtual bool overran() const = 0;
    virtual void shutdown() = 0;
};

class ScriptRuntime {
public:
    virtual ~ScriptRuntime() = default;
    // source = UTF-8 JS 源码；origin = 错误里的文件名（`--ui <path>` 的原样路径 / `builtin:hud`）。
    // font 只给 `ui.measure` 用，可为 nullptr。
    virtual std::unique_ptr<UiScript> load(const std::string& source, const std::string& origin,
                                           FontEngine* font, std::string* err) = 0;
};

std::unique_ptr<ScriptRuntime> makeQuickJsRuntime();

}  // namespace lift::ui
