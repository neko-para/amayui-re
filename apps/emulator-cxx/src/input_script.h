// 宿主的**脚本化输入**装置（测试 / 无人值守）：把一份指令式时间线归约成引擎要的四个量。
//
// ★ 这是**平台边界**的替身，不是引擎语义：规范只规定「平台把当前键 / 鼠标状态归约成一个
//   32 位输入字 + 光标 + 按钮位图 + 一条触点」，物理键 / 设备事件怎么来属宿主策略
//   （`data/spec/chapters/60-ops-input.md` 开头）。窗口路径用 SDL，无窗口路径用本文件。
// ★ 与 `apps/emulator-new/src/platform/node/input.ts` 的 `parseInputScript` / `ScriptedInputHost`
//   是同一套脚本语法（两条实现线互相可读，改一处时另一处就在眼前）。
// ⛔ 不要把它当产品输入路径：没有 `--input-script` 时宿主根本不创建它。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/vm.h"

namespace app {

// 一条脚本事件。`at_ms` 是**仿真毫秒**（不是宿主墙钟）；同一时刻按书写顺序应用。
struct InputEvent {
    enum class Kind {
        Move,    // `move x y`：光标移到客户区坐标 (x, y) 并标记"设备可用"
        Button,  // `down N` / `up N`：鼠标按钮 N（0..31）的位
        Key,     // `key N down|up`：输入位 N（0..31）
        Wheel,   // `wheel D`：滚轮增量（本层只解析、不消费）
    };
    Kind kind = Kind::Wheel;
    std::uint32_t at_ms = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t button = 0;
    std::int32_t key = 0;
    std::int32_t delta = 0;
    bool down = false;
};

// 解析指令式输入脚本（换行或 `;` 分隔，`#` 起注释）：
//
//   100 move 812 622
//   620 down 1
//   760 up 1
//
// 语法错 → 抛 `std::runtime_error`（带行号）；⛔ 不静默跳过一行。
std::vector<InputEvent> parse_input_script(const std::string& text);
std::vector<InputEvent> load_input_script(const std::string& path);

// 脚本化输入后端：`advance(now)` 应用所有 `at <= now` 的事件；采样读的是"当前状态"。
// ★ 光标在第一条 `move` 之前**不可用**（`0x109` 写 −100000、`0x108` 写 0、`0x2fc` 写 0）——
//   这是宿主对"输入可用标志"的替身（规范只说该标志启动为 1、没有清 0 写点的取证）。
// ⛔ 不做任何 hover 去重 / 边沿合成：规范里 `0x12e` 是纯计算，边沿由脚本自己保存上一次位图，
//   宿主替它做等于替引擎发明语义。
class ScriptedInput {
public:
    explicit ScriptedInput(std::vector<InputEvent> events);

    // `now_ms < 0` = 还没到锚点：一条事件都不应用（只由 `--input-anchor-script` 用）。
    void advance(std::int64_t now_ms);

    std::uint32_t keys() const { return keys_; }
    bool cursor(std::int32_t& x, std::int32_t& y) const;
    bool buttons(std::int32_t& mask) const;
    bool touch(core::Machine::TouchPoint& point) const;  // 桌面鼠标-only：恒无触点

    std::size_t applied() const { return applied_; }
    std::size_t size() const { return events_.size(); }
    bool exhausted() const { return next_ >= events_.size(); }
    // 滚轮增量：`wheel D` 事件累加在这里，宿主每轮把它推给引擎（`0x10d` 读并清）。
    std::int32_t take_wheel_delta() {
        const std::int32_t delta = wheel_delta_;
        wheel_delta_ = 0;
        return delta;
    }
    std::int64_t applied_until() const { return last_event_at_; }
    // 把三个采样口接上 core（`0xff` / `0x109` / `0x108` / `0x2fc` 的外部请求）。
    void bind(core::Machine& machine);
    // 输入脚本里出现过的最高时刻（CLI 用来判"还要跑多久输入才播完"）。
    std::uint32_t last_time_ms() const { return last_time_ms_; }

private:
    std::vector<InputEvent> events_;
    std::size_t next_ = 0;
    std::size_t applied_ = 0;
    std::int64_t last_event_at_ = -1;
    std::uint32_t last_time_ms_ = 0;
    std::uint32_t keys_ = 0;
    std::int32_t buttons_ = 0;
    bool cursor_available_ = false;
    std::int32_t x_ = 0;
    std::int32_t y_ = 0;
    std::int32_t wheel_delta_ = 0;
};

}  // namespace app
