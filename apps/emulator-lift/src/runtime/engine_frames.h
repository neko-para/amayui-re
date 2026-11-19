// emulator-lift · guest 帧列表（`game.local_frames()` 的宿主取数口）—— **纯读 GuestMemory**
//
// ★ 语义真源 = `.tmp/ui-qjs/frames-research.md` 的 C1–C14（⛔ 本文件不重新取证、不新造判据）：
//   · 帧区 = `Engine+0x5D894` 起 **40 条记录**、步长 `0x78`（C1：`operator new(0xAABC8)` @0x4BAA11，
//     发布 @0x4BAA3A；初始化循环 `0x415BFE` / `cmp eax,28h` @0x415C0A）；
//   · **活动** = `record+0x00 != 0` 且该地址已提交（C3：`0x41A92E` / `0x41C946` / `0x40EA27` 三处一致）。
//     ⛔ 不是"id 非 0"（id=0 是根脚本的合法 id）、也⛔ 不是"0..cur 全算"（C5：槽可乱序装载 / 留空洞）；
//   · `parent` = `record+0x38`（C7：写 `0x40EE05`，读 `0x41A834`；-1/-10/-11 三个哨兵见 C12）；
//   · `id` = `record+0x3C`（C9：写 `0x40EDEB`）。
// ★ **top = `slot == cur`**（C8：`0x6` 临时改 cur 再还原、`0x8` 可切到任意已装载槽 ⇒ 引擎没有独立的
//   栈顶变量。"最高活动槽"是另一个定义，⛔ 不用）。`cur` 的值读不到 / 越界 ⇒ **整表不可用**；
//   `cur` 可读但那一格不活动 ⇒ 列表照给、`top` 全 false（⛔ 不挑最高槽冒充）。
// ★ **安全读取顺序**（报告「读之前必须检查什么」1..8）：engine 非 0 → `+0xAABC8` 不跨 4 GiB →
//   `cur` 已提交且 < 40 → 第 0 条记录已提交（否则整区不可用）→ 逐槽 `committed(rec,0x78)` +
//   `body != 0` + `committed(body,1)` + id 读 → 读完再看一眼 engine（变了 ⇒ 丢弃）。
//   任何一步失败都**不猜**：整表不可用 ⇒ 空表 + 静态字面量 `err`（⛔ 不编 0 / 空对象）。
// ★ 名字：复用 `MarkerSampler::resolveName` / `nameTableSignature`（C13：解析器不看 `cur`），
//   ⛔ **不**在本文件复制名字解析。每槽缓存 `(id, sig)` ⇒ 名字表从"未装载"→"已装载"而 id 不变时会自动刷新。
// ★ 位置读数（切片 3）：**只有 top 帧**给 `pos`（判据 = `slot == cur`，与 `top` 同一个布尔）；
//   公式 / 门 / 错误串的唯一实现在 `engine_marker.cpp:readScriptPositionAt`（本文件只调用它，
//   ⛔ 不写第二份公式）。非 top 帧的 `+0x04` 是挂起时保存的续延 PC ⇒ 不暴露（`pos_ok=false`）。
//   ★ 单槽读不到只让那一项 `pos_ok=false` ⇒ **仍进表**（⛔ 不把单槽读不到升级成整表 err）。
// ★ ⛔ 不写 guest 内存；⛔ 不影响既有 `script` 行（`game.script` 仍是"当前帧"的既有语义）。
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "runtime/engine_marker.h"
#include "ui/script.h"

namespace lift {

class GuestMemory;

// 帧记录的字段偏移（`packages/age-format/src/engine/layout.mts` 的 FRAME_LAYOUT + `engine_marker.h`）。
// ★ `kFrameParentOffset` 只在这里定义：报告 C7 已证 `+0x38`，而 `engine_marker.h` 只暴露身份 /
//   位置读数用到的字段（位置读数切片 3 抽到那里之后仍不需要 parent）⇒ 由本头补齐，锚 EA 写在上面。
inline constexpr std::uint32_t kFrameParentOffset = 0x00000038u;  // 0x40EE05 写 / 0x41A834 读

class EngineFrameSource final : public ui::UiFrameSource {
public:
    // 装配：`mem` = guest 地址空间（非 owning）；`sampler` = 名字解析器（非 owning；
    //   nullptr ⇒ 名字一律降级成 `id:0x%08X`，⛔ 不让整表因此不可用）。
    void bind(const GuestMemory* mem, MarkerSampler* sampler) {
        mem_ = mem;
        sampler_ = sampler;
    }

    std::vector<ui::UiFrameInfo> localFrames(std::string* err) override;

    // 诊断：本帧源打过多少次 `[frames] why=…`（每个原因各一次；读点 = 测试 / 未来 readout）。
    std::uint32_t warningsEmitted() const { return warnings_; }

private:
    // 每槽名字缓存：`(id, sig)` 变了才重新解析（与 marker 的既有口径一致）。
    struct NameCacheEntry {
        bool valid = false;
        std::uint32_t id = 0;
        std::uint64_t sig = 0;
        std::string name;
        bool degraded = false;
    };

    // 整表不可用：填 err（静态字面量原样）+ 一次性 stderr，返回空表。
    std::vector<ui::UiFrameInfo> unavailable(std::string* err, const char* why);

    const GuestMemory* mem_ = nullptr;
    MarkerSampler* sampler_ = nullptr;
    std::array<NameCacheEntry, kFrameCount> cache_{};
    // ★ 每个原因各打**一次**（不是"整条源只打一次"）：boot 期先到的 `engine-unpublished` 不能把
    //   后面的 `cur-out-of-range` / `frame-region-uncommitted` 永久遮住。⛔ 仍然不周期刷屏。
    std::uint32_t warned_mask_ = 0;
    std::uint32_t warnings_ = 0;
};

}  // namespace lift
