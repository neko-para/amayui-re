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
// ★ 语义（取证见 `docs/03-emulator/ui-frames.md` 的 C1–C14；⛔ 不要在这里重新定义）：
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
    // ★ 脚本内位置（`REQ-01M7QF01A00F9946DX0GPH23JQ` 切片 3）：label 坐标 = `0x3C + (pc − base)`，
    //   与 `await position` 同一套公式（唯一实现在 `engine_marker.cpp:readScriptPositionAt`）。
    //   **只有 top 帧**给（判据 = `slot == cur`，与 `top` 同一个布尔）：非 top 帧的 `+0x04` 是
    //   "该脚本被挂起时保存的续延 PC"，**不是**执行点 ⇒ `pos_ok=false`（脚本侧 `undefined`）。
    bool pos_ok = false;
    std::uint32_t pos = 0;         // pos_ok=false 时值无意义（⛔ 不拿 0 冒充读数）
};

// 帧列表的**宿主取数口**（非 owning；只在 `render` 期间有效）。
// ★ `localFrames(&err)`：`err` 非空 ⇒ 这一刻**整表不可用**（脚本侧看到 `undefined`，⛔ 不编空数组）；
//   `err` 为空 ⇒ 活动帧列表（top-first）。单槽读不到 / 非活动 ⇒ **不进表**（保守跳过，不是猜）。
class UiFrameSource {
public:
    virtual ~UiFrameSource() = default;
    virtual std::vector<UiFrameInfo> localFrames(std::string* err) = 0;
};

// 宿主能力表（`host.*` 的**唯一**来源；`REQ-01M7QF01A00F9946DX0GPH23JQ` 切片 3）。
// ★ **非 owning**：生命周期 = 填它的那一帧（生产 = `AotHost` 成员；测试 = 栈上替身；
//   见 `UiFrameReadings::host`）。
// ★ 为什么是"结构体 + 函数指针"而不是 `std::function`：本头是**语言无关的纯接口头**，
//   `std::function` 会把 `<functional>` / 分配器 / "宿主可以塞任意可调用对象（含捕获悬垂引用的
//   lambda）"带进来 —— 那正好是要防的生命周期模糊；而空槽在这里**编译期可判、运行期可测**
//   （`ops->profile_begin == nullptr` ⇒ 一句 TypeError，测试能钉住"没接线是响亮失败"）。
struct UiHostOps {
    void* ctx = nullptr;  // 实现方自己的上下文（生产 = `AotHost*`；测试 = 任意 POD 指针）。
    // label = UTF-8（空串也接受，实现方自己决定缺省 label）。
    // true = 窗口已开；false = 没开（`*err` 填人可读的 UTF-8 原因）。
    bool (*profile_begin)(void* ctx, const char* label, std::string* err) = nullptr;
    // true = 窗口已关、`*text` = 报告全文；`*err` 只在"附带一步失败"时非空
    //   （剪贴板写失败：`text` 仍然有效、仍要交给 HUD —— 需求单明文"INVALID 仍复制"）。
    // false = 没有开着的窗口（`*err` 填原因）。
    bool (*profile_end)(void* ctx, std::string* text, std::string* err) = nullptr;
};

// 宿主每帧给脚本的只读读数（⛔ 不含 raster / present —— 用户 2026-11-18 已裁决删除）。
// ★ `*_ok` = "这一项这一帧有值"；没有值时脚本侧看到 `undefined`，⛔ 不编 0 / 空串。
struct UiFrameReadings {
    std::uint64_t frame = 0;
    bool mouse_ok = false;
    int mouse_x = 0;
    int mouse_y = 0;
    // ---- 三键电平（`game.mouse().buttons`；C2b-2）----
    // ★ `mouse_buttons_ok == false` ⇒ 宿主这一帧**没有读数源** ⇒ 脚本侧 `mouse()` 里**缺
    //   `buttons` 键**（⛔ 不编 `false`：那会让"鼠标按着"与"没有源"长得一样；d.ts 的
    //   `buttons?` 就是这条口径）。
    // ★ 生产来源 = `WindowManager::mouseButtons()`（`InputDevice` 的 `key_level_` 里三个 VK 的
    //   **只读派生**；`runtime/input_device.h` 的唯一电平表）⇒ ⛔ 不在这里新开输入状态源。
    //   与 `mouse_ok` 同一格来源（同一次 `drawOverlay`、同一帧）。
    bool mouse_buttons_ok = false;
    bool mouse_left = false;
    bool mouse_right = false;
    bool mouse_middle = false;
    bool fps_ok = false;
    double fps = 0.0;
    bool script_ok = false;
    std::string script;   // 已格式化好的载荷（名字或 `id:0x%08X（降级）`）
    bool worst_ok = false;
    double worst_wall_ms = 0.0;
    double worst_guest_ms = 0.0;
    bool worst_guest_ok = false;
    double worst_shim_ms = 0.0;
    // ---- 命中 / 捕获（切片 2 后半；`game.click|hover|active` 的**唯一**来源）----
    // ★ 全部是**这一帧的只读快照**：宿主在 render 之前填好，render 结束即作废（下一次 render 重填）。
    //   `hover_key` / `active_key` 空串 = 没有值（脚本侧 `undefined`，⛔ 不编空字符串）。
    std::string hover_key;
    std::string active_key;
    // ★ `click` 是**一次性**的：宿主每帧 `takeClick` 取走一次 ⇒ 这一帧看得到，下一次就没有。
    //   x/y = **抬起点**（窗口像素，与命中表同一空间）。
    bool click_ok = false;
    std::string click_key;
    int click_x = 0;
    int click_y = 0;
    // ★ 帧列表读数源（`game.local_frames()`；**非 owning**，只在 render 期间有效）。
    //   nullptr = 宿主没接（headless / 测试替身 / 叠层没开）⇒ 脚本侧 `undefined`。
    UiFrameSource* frames = nullptr;
    // ★ 本帧的宿主能力表（`host.*` 的**唯一**来源）。nullptr = 宿主没接（测试替身 / 非窗口臂）
    //   ⇒ `host.profileBegin/End` 抛 TypeError（不是"`host` 是 undefined"：`host` 对象**恒存在**，
    //   缺接线是**调用时**的响亮错，而不是画 HUD 时的形状差异）。
    //   ★ 非 owning；宿主保证它在 render 期间存活（生产：`AotHost` 成员，sink 先于它析构）。
    const UiHostOps* host = nullptr;
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
    // source = UTF-8 JS 源码；origin = 错误里的文件名（hud 回退脚本路径 / `builtin:hud`）。
    // font 只给 `ui.measure` 用，可为 nullptr。
    virtual std::unique_ptr<UiScript> load(const std::string& source, const std::string& origin,
                                           FontEngine* font, std::string* err) = 0;
};

std::unique_ptr<ScriptRuntime> makeQuickJsRuntime();

}  // namespace lift::ui
