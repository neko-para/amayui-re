// emulator-lift · `PluginReadings` 的实现（口径见头注）
#include "plugin/readings.h"

#include <cstdint>

#include "runtime/host_window.h"  // WindowManager（primaryWindow / cursorClientPos）
#include "runtime/memory.h"       // GuestMemory（只有 marker 采样器用它）

namespace lift::plugin {

PluginReadings::PluginReadings(const lift::GuestMemory& mem, lift::WindowManager& wm)
    : mem_(mem), wm_(wm) {}

TriggerReadings PluginReadings::sample(InputPoint point) {
    ++samples_;
    TriggerReadings readings;
    // `point` / `read_index` 由 `IntentExecutor` 覆盖成本读点的真值（采样源不许编序号）；
    // 这里也填一份 ⇒ 单独用本类（测试）时字段不是脏的。
    readings.point = point;

    // ---- ① 身份（`cur` / 文件 id → 名字）----
    const MarkerSample identity = marker_.sampleIdentity(mem_);
    const bool same = identity.valid && identity_.valid && identity.cur == identity_.cur &&
                      identity.id == identity_.id &&
                      identity_.name_table_sig == marker_.nameTableSignature(mem_, identity.id);
    if (!same) {
        if (identity.valid) {
            MarkerSample resolved = identity;
            marker_.resolveName(mem_, &resolved);
            identity_ = std::move(resolved);
        } else {
            identity_ = MarkerSample{};  // 这一格读不到 ⇒ 没有身份（⛔ 不沿用旧名字冒充当前）
        }
    }
    if (identity_.valid) {
        readings.script = identity_.name;
        readings.script_ok = !identity_.name.empty();
        // ---- ② 脚本内位置（与 `await position` 同一份公式）----
        const ScriptPositionSample pos = readScriptPositionSample(mem_, identity_);
        readings.pos = pos.pos;
        readings.pos_ok = pos.valid;
    }

    // ---- ③ 鼠标（客户区坐标；没有主窗口 ⇒ 没有读数，⛔ 不编 0）----
    const std::uint32_t hwnd = wm_.primaryWindow();
    if (hwnd != 0) {
        int x = 0;
        int y = 0;
        wm_.cursorClientPos(hwnd, &x, &y);
        readings.mouse_x = x;
        readings.mouse_y = y;
        readings.mouse_ok = true;
    }
    return readings;
}

}  // namespace lift::plugin
