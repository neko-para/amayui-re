// emulator-lift · `GuestEngineRead` —— 插件侧 `ctx.read.global.get` 的**生产**实现
//
// 合同：`docs/03-emulator/plugin-runtime.md` §4.4（EngineRead：引擎数据读；**同步、无暂停、便宜**）、
//       §4.5 704（`ctx.read.*` 与 Edit 同点组合 `read → edit → input`）。
// 父级裁决：`.tmp/plugin-runtime/c2-decisions.md` D-C2-6（C2 **不安装** `frameHash`；其余读口随 C2b）、
//       测绘 `.tmp/plugin-runtime/c2-survey.md` §2（`EngineRead::globalGet` 无实现）。
//
// ★ 本类只有一件事：把 `runtime/script_globals.h` 的 `readGlobal`（**唯一**读实现）接到
//   `EngineRead` 接口上。⛔ 不在这里做地址算术 / 不缓存池基址（每次调用重新读 ⇒ 与
//   `emu-ctl global-get` 同一条路径、同一个结果）。
// ★ **无暂停**（合同 §4.4 507–509）：本类不认识 `RunnerControl` / `state_seq` / 序列化。
#pragma once

#include "runtime/engine_read.h"

namespace lift {

class GuestMemory;

}  // namespace lift

namespace lift::plugin {

class GuestEngineRead final : public EngineRead {
public:
    explicit GuestEngineRead(const lift::GuestMemory& mem) : mem_(mem) {}

    bool globalGet(GlobalKind kind, std::uint32_t id, GlobalValue* out, GlobalError* err) override;

private:
    const lift::GuestMemory& mem_;  // 非拥有：生命周期 = 宿主（`AotHost::mem()` / 测试夹具）
};

}  // namespace lift::plugin
