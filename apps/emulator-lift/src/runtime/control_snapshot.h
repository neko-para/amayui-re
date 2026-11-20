// emulator-lift · 控制层**快照**（S3）：主线程填值、控制线程只序列化
//
// ★ 两条硬约束（`design-review.md` §7 的内部矛盾裁决 / `design-ws` §2.3）：
//   ① 本头**只放值**：没有 guest 指针、没有 `GuestMemory` / `X86` / `Machine` 的任何引用。
//      ⇒ 控制线程拿到的是一份**拷贝**，⛔ 它永远不读 guest 内存。
//   ② 本头**不 include** beast / nlohmann / SDL（`control.h` 与 `aot_host.h` 都 include 它）。
//      JSON 序列化在最外层（`control_ws.cpp`，唯一 include nlohmann 的 TU）。
//
// ★ 快照的**逐位稳定性**（判据 3b：同一暂停点连采两次逐位相同）：
//   · `state_seq` 只在进入 / 离开暂停时变（⛔ 不在每次采样时自增）；
//   · ⛔ 不放 `wall_ms` / `now` / 任何"现在几点"字段；
//   · `clock.paused_total_ms` 用 **已结算** 口径（`VirtualClock::pausedCommittedMs()`，
//     不含当前冻结区间）⇒ 暂停中隔多久采，这个数都不动（含当前区间的口径在 pause/resume 的
//     `res` 里，S1 不变 —— 见 `control_ws.cpp` 的 `serve()`）。
#pragma once

#include <cstdint>
#include <string>

#include "runtime/script_globals.h"  // GlobalKind / GlobalValue / GlobalError（值类型；⛔ 无 guest 指针）

namespace lift {

// =====================================================================================
// `script` 块：当前执行点（位置读取**唯一**走 `readScriptPositionSample`）
// =====================================================================================
// `why` 只允许 `readScriptPositionSample`（`engine_marker.cpp`）里**已经存在**的 8 个静态字面量：
//   identity-invalid / engine-unpublished / frame-record-uncommitted / body-base-unpublished /
//   pc-outside-body / pc-outside-region / pc-offset-over-max / pos-overflow
// ⛔ 不新造 `identity_unpublished` / `no_sha` / `header_unreadable` 这类设计稿里的名字。
//
// `script_sha_why` 是**本层新增的最小枚举**（`sha256` 走宿主文件，见下）：
//   · `none`         = sha 在场
//   · `no_identity`  = 身份读不到（`available == false`）⇒ 不尝试
//   · `unresolved`   = VFS 把该脚本名解析不到宿主文件（本安装的 TITLE.BIN 在 DATA*.ALF 里就是这一档）
//   · `unreadable`   = 宿主路径解析到了，但打开 / 读失败
// 事实字段 `script_sha_source` 恒 `host-file`：guest 侧拿不到脚本体长度（`engine_marker.h` 只有上界
// `kScriptBodyMaxBytes`）⇒ 对 guest 后缓冲哈希**不可实现**（`design-review.md` §8-1）。
struct ControlScriptSample {
    bool available = false;
    const char* why = nullptr;      // available=false 时的枚举字面量（见上）
    std::string name;               // 身份名（`MarkerSample::name`；降级时可能是 `id:0x…`）
    std::uint32_t script_id = 0;    // `MarkerSample::id`
    std::uint32_t cur = 0;          // `MarkerSample::cur`（诊断）
    std::string sha;                // 64 hex；空 = 缺省
    const char* sha_why = "no_identity";
    std::uint32_t pos = 0;          // label 坐标 = `kScriptHeaderLen + (pc - base)`
    std::uint32_t pos_dwords = 0;   // (pos - kScriptHeaderLen) / 4
    std::uint32_t header_len = 0;   // = `kScriptHeaderLen`（编译期常量，⛔ 不从 guest 头里读）
};

// =====================================================================================
// 一次 `sample` 的全部值（POD 语义：只含值 / 可拷贝；没有指针进 guest）
// =====================================================================================
struct ControlSnapshot {
    bool paused = false;
    const char* paused_at = "";      // "block" | "present"
    std::uint64_t state_seq = 0;
    std::uint64_t insns = 0;
    std::uint64_t frame = 0;         // `HeadlessPresenter::frameCount()`
    std::uint64_t last_hash = 0;     // `HeadlessPresenter::lastHash()`
    std::string present;             // "visible" | "hidden"（`windowHidden()` 的同一判据）
    std::uint64_t guest_ms = 0;
    std::uint64_t paused_total_ms = 0;  // **已结算**暂停毫秒（见头注）
    ControlScriptSample script;
};

// =====================================================================================
// `screenshot` 的结果（S2）
// =====================================================================================
// `source` 恒 `guest-backbuffer`：读的是 **`present()` 当场冻结的 guest 后缓冲副本**
// （`dumpLastFrame`；没冻结 / 冻结失败才退回读现场描述符），⛔ 不是窗口回读 —— 两者差
// 叠层 / letterbox / 光标（`design-review.md` §2a）。
struct ControlScreenshot {
    std::string path;
    std::uint64_t bytes = 0;
    std::uint64_t frame = 0;
    // 语义 = **写出的这份字节仍等于 presenter 记录的那一帧**（`last_surface_hash_`），与暂停无关。
    // ★ 冻结路径下**恒 true** ⇒ 它只回答"冻结有没有生效"，⛔ 不能当"图对不对"的判据
    //   （判内容要判非全黑 / 与同帧 `--dump-frame` 对照）。
    bool hash_still_matches = false;
    const char* source = "guest-backbuffer";
};

// ★ `screenshot` 的**三态**：`io` 只留给"有像素但写不出去"。
//   · `NoFrameYet` = 数据**没到**（一次 Present 都还没发生）⇒ 协议层回 `unavailable`（⛔ 不是 io）；
//   · `IoFailed`   = 有帧、写盘失败（打不开 / 写不进去）⇒ `io` + 人话。
enum class ControlShotStatus : std::uint8_t { Ok = 0, NoFrameYet, IoFailed };

// =====================================================================================
// 主线程侧的取数口（`Control` 只调这些；⛔ 控制线程不实现、不调）
// =====================================================================================
class ControlHostSide {
public:
    virtual ~ControlHostSide() = default;
    // 填一份快照（主线程；只读 guest 内存 + 宿主只读状态）。
    virtual void fillControlSnapshot(ControlSnapshot& out) = 0;
    // 写最后呈现的那一帧（主线程）。非 `Ok` ⇒ `err` 填人话（`Control` 按三态翻成 `unavailable` / `io`）。
    virtual ControlShotStatus controlScreenshot(const std::string& path, ControlScreenshot& out,
                                                std::string* err) = 0;
    // `resume` 的宿主即时动作（窗口档：记"恢复时刻"⇒ 丢弃陈旧 OS 输入；默认实现什么都不做）。
    virtual void controlOnResume() {}
    // 当前 Present 帧号（`resume --for frames` 的谓词用；没有呈现层 ⇒ 0）。
    virtual std::uint64_t controlPresentFrame() const { return 0; }
    // `global-get` / `global-set` 的主线程执行口（`runtime/script_globals.h` 是唯一实现）。
    // ★ 默认 = 这个宿主没接（只会出现在测试假宿主上）⇒ 控制层按 `type_mismatch` 回；
    //   `AotHost` 覆写为直接调模块（`readGlobal` / `writeGlobal`）。
    // ★ ⛔ 实现里不许碰 `cpu_`：写的是**一个 guest 数据槽**，不是执行流。
    virtual GlobalError controlGlobalGet(GlobalKind kind, std::uint32_t id, GlobalValue* out) {
        (void)kind;
        (void)id;
        (void)out;
        return GlobalError::TypeMismatch;
    }
    virtual GlobalError controlGlobalSet(GlobalKind kind, std::uint32_t id, const GlobalValue& value) {
        (void)kind;
        (void)id;
        (void)value;
        return GlobalError::TypeMismatch;
    }
};

// ★ 脚本位置 + 身份 + `script_sha` 的**唯一**填充点（主线程调）。
//   只依赖 `GuestMemory` / `Vfs` / `sha256`，**不碰 GPU / presenter** ⇒ pure CTest 可直接断言
//   "boot 前 => available:false + why ∈ 既有 8 个字面量" 与 "`header_len == 0x3C` / `pos % 4 == 0`"。
//   `sampler` 是调用方持有的有状态采样器（名字解析缓存 / 一次告警去重）。
class GuestMemory;
class Vfs;
class MarkerSampler;
void fillControlScriptSample(const GuestMemory& mem, MarkerSampler& sampler, const Vfs& vfs,
                             ControlScriptSample* out);

// ★ `ControlSnapshot` → **`sample` 对象**的 JSON 文本（纯函数：同输入 ⇒ 逐字节同输出）。
//   实现放在 `control_ws.cpp`（唯一 include nlohmann 的 TU）；本声明让 pure CTest 能测
//   "没有自增 / 墙钟字段"（判据 3b 的机制面），⛔ 不必把 nlohmann 引进测试。
std::string controlSnapshotJson(const ControlSnapshot& snapshot);

}  // namespace lift
