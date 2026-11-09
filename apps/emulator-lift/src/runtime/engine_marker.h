// emulator-lift · 引擎脚本身份（marker）采样器 —— **纯读 GuestMemory**，不依赖 Machine / X86
//
// ★ 这是 `TriggerKind::Marker` 的**唯一**取数口：读 `cur` 与 `frame[cur].+0x3C`（= 统一文件 id），
//   再按引擎自己的 `sub_454FA0` 语义把 id 解析成名字。事件源（边沿 / 超时 / 降级）在
//   `runtime/input_script.h`；本文件只回答"现在这一格是什么"。
//
// ★★ 采样点（逐字取证 `.tmp/reveq/01M6XVZ4ANQRC215ACG90A35NR/analysis.md` §1）：
//   - VM 循环头 = **`0x4123F0`**（`loc_4123F0`，属 `sub_412290`；唯一调用方 `_WinMain` @`0x4BB517`）。
//     派发表 `[esi+eax*4+0A509Ch]` 全语料 3 个派发点（`0x411A66` / `0x412843` / `0x413464`）都回跳它。
//   - **`0x4123E5`** 是消息泵"无消息"出口（`0x412385 jz loc_4123E5`）的**块入口**：它线性跨过
//     `0x4123F0`（`0x4123E5 mov [0AAB4Ch],1` / `0x4123EF nop` / `0x4123F0 mov ecx,[0AAB4Ch]`），
//     而块缓存按控制指令切块（`Decoder::decodeBlock` + `opIsControl` 含 `Jcc`）⇒
//     `0x4123FC jnz` 不跳时，这一 pass 的块入口是 **`0x4123E5`**，`eip==0x4123F0` 的钩子**看不到**。
//     所以两个 EA 都采：多采不造事件（边沿按 `(cur,id)` 去重），而消息泵里跑过 guest WndProc
//     之后的那一 pass 不会漏。（二者都不在帧装载器 `sub_40ED40` 内；`0x6` 的 handler 会把目标槽号
//     **临时写进 `cur`**（`0x41C854`）再还原（`0x41C8A6`）⇒ ⛔ 不许在任意块边界采。）
//
// ★★★ 为什么必须判 `base != 0`：`ds:dword_55E1BC`（Engine 单例）只有两个写者 ——
//   发布 `0x4BAA3A` 与收尾置 0；boot 到发布之前 / 退出之后读到 0。
#pragma once

#include <cstdint>
#include <string>

namespace lift {

class GuestMemory;

// ---- 采样点（EA 锚；`.lst` 的指令原文见头注）----
inline constexpr std::uint32_t kMarkerLoopHeadEa = 0x004123F0u;    // loc_4123F0（VM 循环头）
inline constexpr std::uint32_t kMarkerPumpReturnEa = 0x004123E5u;  // loc_4123E5（消息泵回来的块入口）

// ---- Engine 单例与帧记录（`packages/age-format/src/engine/layout.mts` 的 FRAME_LAYOUT）----
inline constexpr std::uint32_t kEngineSingletonGlobal = 0x0055E1BCu;  // ds:dword_55E1BC（发布点 0x4BAA3A）
inline constexpr std::uint32_t kEngineCurOffset = 0x0005D880u;         // cur（0x412818 读）
inline constexpr std::uint32_t kEngineFrameBase = 0x0005D894u;         // 第 0 条记录（0x412825 用 120*cur 索引）
inline constexpr std::uint32_t kFrameStride = 0x00000078u;             // 记录大小 = 索引步长
inline constexpr std::uint32_t kFrameIdentityOffset = 0x0000003Cu;     // 统一文件 id（0x40EDEB 写）
inline constexpr std::uint32_t kFrameCount = 40u;                      // 0x41C85A cmp eax,28h / 0x41C6C1 cmp ...,27h

// ---- id → 名字（内嵌在 Engine+0xA609C 的 FileDB；解析器 sub_454FA0）----
inline constexpr std::uint32_t kFileIndexOffset = 0x000A609Cu;         // Engine+0xA609C（lea ecx,[esi+0A609Ch]）
inline constexpr std::uint32_t kFileRootCountOffset = 0x0000040Cu;     // 根条目数（0x454FCC cmp eax,[ecx+40Ch]）
inline constexpr std::uint32_t kFileRootNameBaseOffset = 0x00000414u;  // 根名字数组基址（0x454FDE add eax,[ecx+414h]）
inline constexpr std::uint32_t kFileVolumeTableOffset = 0x00003028u;   // 卷表（0x454FB2 [ecx+edx*4+3028h]，0x100 项）
inline constexpr std::uint32_t kFileSubIndexCountOffset = 0x00000008u;   // 子索引条目数（0x401506 cmp eax,[ecx+8]）
inline constexpr std::uint32_t kFileSubIndexNameBaseOffset = 0x0000011Cu; // 子索引名字数组（0x401515 add eax,[ecx+11Ch]）
inline constexpr std::uint32_t kFileNameStride = 80u;                  // 80 = 5*16（lea eax,[eax+eax*4]; shl eax,4）
inline constexpr std::uint32_t kFileNameMaxBytes = 80u;                // 条目只有 80 字节：读到 NUL 或 80 为止

// 条目数的合理上界：超过它视为"TOC 未装载 / 读到分配器垃圾"（`+0x40C` 构造期**没有**清零）。
inline constexpr std::uint32_t kMaxFileEntries = 1u << 20;

// 一次采样的结果。
//   `valid=false` ⇒ 这一格现在不能读（base 未发布 / cur 越界 / 记录未提交 / 记录 +0x00 不是已提交页）；
//   `valid=true` 且 `name_degraded=true` ⇒ 身份可读、**事件仍按 id 触发**，只是名字读不到（已告警）。
struct MarkerSample {
    bool valid = false;
    std::uint32_t cur = 0;
    std::uint32_t id = 0;  // 原始 32 位（负数 = 高位置 1，见 §3.7：引擎会走区号臂读表外；宿主自己降级）
    std::string name;      // 解析出的名字；降级时 = `id:0x%08X`
    bool name_degraded = false;
    // 解析这个名字时**名字表的状态指纹**（根条目数/基址 [+ 区号臂的子索引]）。
    //   ⇒ 同一 `(cur,id)` 复用缓存名字时必须先比它：TOC 从"未装载"变成"已装载"而身份没变时，
    //     名字必须重新解析（否则会永远停在 `id:0x…`）。
    std::uint64_t name_table_sig = 0;
    const char* why = nullptr;  // valid=false / name_degraded 的原因（静态字面量；只进日志）
};

// marker 采样器（有状态：告警**每类只打一次**；计数供 run summary / CTest 读）。
// ★ 纯档夹具直接 `MarkerSampler s; s.sample(machine.mem())` 即可驱动一次采样。
class MarkerSampler {
public:
    // 完整一次采样（身份 + 名字）：CTest / 首采样 / 身份变化时用。
    MarkerSample sample(const GuestMemory& mem);
    // ★ 只读到身份（`cur` / `id`）：采样点在 VM 循环头 ⇒ **每条主循环指令一次**，
    //   同一 `(cur,id)` 重复采样时不该反复做名字解析（两次 std::string + CP932 转换）。
    //   拿到 valid 的 identity 之后，只有（首次 / 身份变了）才调 `resolveName()`。
    MarkerSample sampleIdentity(const GuestMemory& mem);
    // 给 `sampleIdentity()` 的结果补名字（降级时写 `id:0x%08X` + 一次性告警）。
    void resolveName(const GuestMemory& mem, MarkerSample* s);
    // 当前名字表对 `raw_id` 的状态指纹（只读 2..5 个 dword；用于"同身份要不要重解析名字"）。
    std::uint64_t nameTableSignature(const GuestMemory& mem, std::uint32_t raw_id) const;

    // 每轮 run 重置计数（告警去重位**不**重置：同一进程里重复告警没有信息量）。
    void resetCounters();
    std::uint64_t attempts() const { return attempts_; }        // 采样调用次数
    std::uint64_t unpublished() const { return unpublished_; }  // base == 0（boot / 退出后）
    std::uint64_t published() const { return published_; }      // base != 0 的采样次数
    std::uint64_t invalid() const { return invalid_; }          // 其余 valid=false 的次数

private:
    enum WarnBit : std::uint32_t {
        WarnCur = 1u << 0,
        WarnRecord = 1u << 1,
        WarnNameTable = 1u << 2,
        WarnZeroId = 1u << 3,
    };
    bool warnOnce(WarnBit bit) {
        if ((warned_ & static_cast<std::uint32_t>(bit)) != 0) return false;
        warned_ |= static_cast<std::uint32_t>(bit);
        return true;
    }

    std::uint32_t warned_ = 0;
    std::uint64_t attempts_ = 0;
    std::uint64_t unpublished_ = 0;
    std::uint64_t published_ = 0;
    std::uint64_t invalid_ = 0;
};

}  // namespace lift
