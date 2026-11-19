// emulator-lift · `PluginReadings` —— 生产 `ReadingsSource`（每读点一次）
//
// 合同：`docs/03-emulator/plugin-runtime.md` §4.2（`TriggerReadings` 的字段语义）、
//       §4.5（`Context::now` 的形状）、§5 C1/C2（`ctx.now` 必须有值）。
// 父级裁决：`.tmp/plugin-runtime/c2-decisions.md` D-C2-10（`start` 之前补一次 `sampleNow`）。
// 测绘：`.tmp/plugin-runtime/c2-survey.md` §7（C1 只有测试替身 `tests/trigger_runner.cpp` 的 `Readings`）。
//
// ★ 三个真源，一个都不新造：
//   · 脚本身份 = `MarkerSampler::sampleIdentity`（`runtime/engine_marker.h`；⛔ 不在这里读
//     `Engine+0x5D880` —— 那是 marker 模块的唯一职责）；
//   · 脚本内位置 = `readScriptPositionSample`（同头；与 `await position` **同一份**公式）；
//   · 鼠标 = `WindowManager::primaryWindow()` + `cursorClientPos`（客户区坐标，与
//     `SetCursorPos` / 配方 click 同一坐标系）。
//
// ★ 缓存：同一 `(cur,id)` + 名字表签名不变 ⇒ 复用上次解析出的名字（否则每个读点都要付一次
//   CP932 解析 —— `InputPlan::refreshIdentity` 同形）。⛔ 这不是"第二份身份状态"：真源仍是
//   marker 采样器，这里只缓存**字符串**。
//
// ★ 与 `InputPlan::atPoint` 的采样**互斥**：`--plugin` 与 `--input-plan` / `--control-root`
//   启动即 exit 2（D-C2-2）⇒ 同一个读点不会两次付身份采样的钱。
#pragma once

#include <cstdint>

#include "runtime/engine_marker.h"
#include "runtime/intent_executor.h"  // ReadingsSource（C1c 的测试缝）

namespace lift {

class GuestMemory;
class WindowManager;

}  // namespace lift

namespace lift::plugin {

class PluginReadings final : public ReadingsSource {
public:
    // 都是**非拥有**引用：`mem` 归宿主（`AotHost::mem()`）、`wm` 归宿主的窗口管理器。
    // ⚠ 只能用在本树的窗口管理器上（`cursorClientPos` 有主线程亲和性守卫）。
    PluginReadings(const lift::GuestMemory& mem, lift::WindowManager& wm);

    TriggerReadings sample(InputPoint point) override;

    // 读数（trace / 用例；⛔ 不是性能判据）。
    std::uint64_t samples() const { return samples_; }
    bool had_identity() const { return identity_.valid; }

private:
    const lift::GuestMemory& mem_;
    lift::WindowManager& wm_;
    MarkerSampler marker_;
    MarkerSample identity_;  // 上次解析过名字的身份（含 `name_table_sig`）
    std::uint64_t samples_ = 0;
};

}  // namespace lift::plugin
