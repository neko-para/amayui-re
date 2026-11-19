// emulator-lift · **`EditSink`** —— 插件侧唯一的 guest 写口（C1c）
//
// 合同：`docs/03-emulator/plugin-runtime.md` §4.4a 522–524（`EditSink` 立即、同步；**不排 Input**）、
//       §4.5 702（`ctx.edit.global.set` 同步立即；暂停 / 控制点也可；⛔ 不进 `IntentQueue`）、
//       §4.3 490（`global.set` 失败 ⇒ receipt + 一行；核心**不** `exit 5`）。
// 施工图：`.tmp/plugin-runtime/c1-design-review.md` §4.3（为什么立即写**不会**重入 trigger / 读点）/
//         §5.7（签名）。
//
// ★ **立即**的含义（本切片最容易做错的一处）：`globalSet` 在**调用栈里**就把值写进 guest 内存 ⇒
//   同一个回调里紧接着的 `read.globalGet` 读回**新值**。⛔ 不许"排进 Input 队列"、⛔ 不许"下一读点
//   生效"：合同 §4.4 明文是 `read → edit → input`（Edit 先于 Input，且 Edit 不等任何读点）。
//   ⇒ 由此推出两条可判性质（`tests/trigger_runner.cpp` 的 `global-read-modify-write-click` 断言它们）：
//     ① 同拍读回 = 新值；② Edit **不占** Input 的写预算（同一拍到期的那条抬起照旧落地）。
//
// ★ **不会重入**，而且是**结构性**的（⛔ 不需要 `inCallback` 标志）：
//   · `writeGlobal` 只碰 `GuestMemory`：不 post 消息、不调 shim、不调 `input::*`、不看窗口；
//   · `Context` 里没有 `WindowManager`（拿不到 `atPoint` 的入口）；
//   · `Context::trigger` 是工厂而不是 runner（拿不到唯一 pump）。
//   ⇒ 这一个写口就是全部：`GuestEditSink` 内部持 `GuestMemory&`（`writeGlobal` 要非 const），
//     而 `Context` 只暴露一个 `EditSink*` ⇒ "只读上下文 + 一个可写口"在类型层面成立。
//
// ⚠ `GuestEditSink` 在 C1 里是**测试用**实现（也是 C4 生产实现的底子）：它不认识 owner / Run /
//   暂停。⛔ C1 不把它接进生产 `WindowManager` / `control_ws`（合同 §5 C1 989–991）。
#pragma once

#include <cstdint>

#include "runtime/intent.h"         // Receipt（回执形状与 Input 共用一份）
#include "runtime/script_globals.h"  // GlobalKind / GlobalValue / GlobalError

namespace lift {

class GuestMemory;

}  // namespace lift

namespace lift::plugin {

class EditSink {
public:
    virtual ~EditSink() = default;

    // 同步写一个 script global。`Receipt::accepted` = 写成功；失败时 `code` = `globalErrorName(err)`
    // （`"id_out_of_range"` / `"type_mismatch"` / `"encode_failed"`）。`seq/queued/id` 恒 0：
    // Edit 不进队列 ⇒ 没有序号 / 深度 / id（⛔ 不要为了"形式上像 Input"编一个）。
    virtual Receipt globalSet(GlobalKind kind, std::uint32_t id, const GlobalValue& value) = 0;
};

// C1 的落地点（`mem` 由夹具 / C4 的宿主注入；本类**不拥有**它）。
class GuestEditSink final : public EditSink {
public:
    explicit GuestEditSink(GuestMemory& mem) : mem_(mem) {}
    Receipt globalSet(GlobalKind kind, std::uint32_t id, const GlobalValue& value) override;

private:
    GuestMemory& mem_;
};

}  // namespace lift::plugin
