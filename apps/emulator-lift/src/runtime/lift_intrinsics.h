// emulator-lift · AOT 的**构建期库内建函数**（v1 只收 `_memcpy` @ 0x004E74B0）
//
// ★ 它是什么（与 `--native-overrides` **无关**，那套已整条删除）：
//   单元入口的**带闸门快路径**：`lift-gen` 在 `_memcpy` 的单元根入口发一次
//   `lift_intrinsic_memcpy_gate(...)`，闸门 Ok 就直接用宿主 `memmove` 干完并 `ret`，
//   闸门拒绝就**回退到原 lifted 函数体**（那条路逐字节不变）。
//   ⇒ 判据仍然是"只有 IR 与 shim"，没有第三份执行来源；`rep_bulk` 是同一类（判 + 执行 + 回退）。
//
// ★ 为什么闸门与执行拆成两个函数：
//   闸门**不写任何 guest 状态**（只读 `cpu.mem` / `cpu.mem->committed`），夹具可以逐条断言
//   "这个组合必须 decline"；`--no-intrinsics` 也只让闸门返回 false（执行体不会被调用）。
//   这样"未提交页 / 跨 4 GiB"这类在 guest 可见状态上观察不到的闸门，可以直接被 `lift-intrinsics`
//   的夹具证明，而不必让夹具自己去触发 guest fault。
//
// ★ 契约（`_memcpy` 的静态 CRT 形态，反汇编 + `.lst` FLIRT 名已核）：
//   cdecl：`dst = [esp+4]`、`src = [esp+8]`、`n = [esp+12]`；返回 `EAX = dst`；
//   callee-saved 全部恢复；重叠走 `std; rep movsl; cld`（⇒ 等价 `memmove`，返回时 DF=0）。
//   ⛔ **ECX / EDX 的残留值不拟合**：cdecl 调用者不保证依赖被调方留下的 ECX/EDX，
//   本快路径不模拟它们（显式裁决；契约测试也不断言这两个寄存器）。
//   `EBX / ESI / EDI / EBP` 不动（快路径根本没执行 prologue，它们仍是调用者的值）；
//   `ESP` 只 `+4`（pop 返回地址；实参由调用方清）；`EFLAGS` 不被触碰（DF 不会由本路径置位）。
#pragma once

#include <cstddef>
#include <cstdint>

namespace lift {

struct X86;

// ★ 阈值 = **单点常量**（实测后可调，只改这一处）。
//   依据（2026-11-24 / SAVE11 点击后 2.5 s）：`_memcpy` 2,293,913 次、avg 4.39 B、
//   p50 = p90 = 4，99.993% 落在 4–7 B 桶，最大 400 KB。16 覆盖到"8–15 B"这一档；
//   更大的 n 走原 lifted 函数体（v1 只收小拷贝：16 B 以内 `committed`/边界检查与复制本身同阶，
//   阈值再放大只会让大帧的占比变高、把小拷贝的收益摊薄 —— 先按形状收，实测后再调）。
inline constexpr std::uint32_t kMemcpyFastMaxBytes = 16;

// 闸门为什么放行 / 不放行（`Ok` = 可以走宿主 `memmove`）。
// ★ 顺序 = 判定的**短路顺序**（夹具按它断言；⛔ 不要为了"多一个理由"插到前面让别的判据永不命中）。
enum class IntrinsicWhy : std::uint8_t {
    Ok = 0,
    Disabled,       // `--no-intrinsics`（只给 A/B 与诊断；缺省开）
    SizeTooLarge,   // `n > kMemcpyFastMaxBytes`
    CrossBoundary,  // `dst+n` / `src+n` 越过 4 GiB（宿主平坦映射只有 4 GiB）
    Uncommitted,    // safe / 非 safe 都查：目的或源的跨度里有未提交页 ⇒ 必须让原函数体去 fault
    NoMemory,       // `cpu.mem == nullptr`（夹具 / 装配错误；⛔ 不许当成功）
};

struct MemcpyGatePlan {
    IntrinsicWhy why = IntrinsicWhy::Disabled;
    // 两条**实际访问区间**的高端（排他；`uint64` ⇒ 不会被 32 位回绕骗过）。
    // 只给读数 / 夹具用；执行体不用它们（`Ok` 时高端必然 <= 4 GiB）。
    std::uint64_t dst_end = 0;
    std::uint64_t src_end = 0;
};

// ★ 闸门判定的**唯一真源**：只读 `cpu.mem`，不写 guest 状态，也不碰任何寄存器。
//   `n == 0` 直接算 `Ok`（一个字节都不碰；原函数体也是"什么都不搬"）。
MemcpyGatePlan memcpyGatePlan(const X86& cpu, std::uint32_t dst, std::uint32_t src,
                              std::uint32_t n);

// 进程级开关（`--no-intrinsics` 只改这里）。缺省 **true**。
// ★ 只由主线程在 `AotHost::run` 入口 / 夹具里 set；闸门热路径只做一次 relaxed load。
bool intrinsicsEnabled();
void setIntrinsicsEnabled(bool on);

// 诊断计数（一次性读数用；⛔ 不是判据）。只在 `lift_intrinsic_memcpy_*` 的 C 入口更新。
struct IntrinsicCounters {
    std::uint64_t fast = 0;      // 闸门放行（= 真的走了宿主 memmove）
    std::uint64_t decline = 0;   // 闸门放行失败（SizeTooLarge / CrossBoundary / Uncommitted / NoMemory）
    std::uint64_t disabled = 0;  // 被 `--no-intrinsics` 关掉
};
IntrinsicCounters intrinsicCounters();
void resetIntrinsicCounters();

}  // namespace lift
