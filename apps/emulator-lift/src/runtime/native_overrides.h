// emulator-lift · guest 函数级 native override（试点原型）
//
// ★ 口径（REQ-01M6NZSAAD0WCNBE3RKMFF84R5）：
//   - 只在 **dispatcher 的 block 入口**按 EA 拦截（不 patch guest 字节 ⇒ 天然避开 SMC / 代码页保护）；
//   - native 实现只能经 `GuestMemory` 读写 guest；⛔ 宿主指针不进 guest 状态；
//     ★ 例外（有契约）：准入门逐段 `committed()` 之后，允许用 `GuestMemory::host()` 的平坦映射裸指针
//       就地读写一段 guest 内存（先例 `nativeLzss436A80`；`sub_4367B0` 的纯核原地改 `0xD430` 对象）。
//       ⛔ 裸指针**不**绕过准入门 —— 没做 `committed()` 就不许用。
//   - 返回后由本框架按 ABI 设置 `EAX` / `ESP` / `EIP`，未覆盖路径照旧走解释器；
//   - 每一项都**默认关闭**，由 `--native-overrides=<list>` 显式启用。
// ★ 本文件 = **契约**（机制 + 纯核声明）；实现按域拆在 `src/runtime/native_*.cpp`（2026-11-08，
//   纯搬运）—— `native_overrides.cpp` 只留机制 / 已知表 / 段级锚校验。
// ★ 候选由 profile 选出，不先猜（见 requirement 的测试计划）。
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "runtime/guest_call.h"

namespace lift {

struct X86;
class GuestMemory;
class LstIndex;

enum class GuestConv : std::uint8_t {
    Cdecl = 0,    // 调用方清栈
    Stdcall = 1,  // 被调用方清栈
    Thiscall = 2, // ECX = this；栈参数由被调用方清（MSVC x86）
};

const char* guestConvName(GuestConv c);

// native 实现：读参数（cpu.arg(i) / cpu.ecx()），经 mem 访存，**按 ABI 写返回值**：
//   `*result` = 返回 EAX；需要第二个返回寄存器（`EDX:EAX` 的 64 位返回）时 fn 自己写 `cpu.edx()`
//   —— `Cdecl` 下 guest 侧的返回值通路就是 `EDX:EAX`（先例：`__ftol2_sse`，`0x4E8311/0x4E8334`）。
//   ⚠ 写 `EDX`（或任何别的寄存器）**必须放在所有闸门之后**：回退前不留任何 guest 可见副作用
//   （准入门 ②）；ECX/EBX/ESI/EDI/EBP/ST0 等 call-clobbered 寄存器仍然**不会**被框架改写。
//   需要整组寄存器 / EFLAGS / EIP 的段级替换走 NativeBlockFn。
// 返回 false ⇒ 本次**不做** override，由 dispatcher 回退解释器（例如 safe mode 下参数越界）。
using NativeFn = bool (*)(X86& cpu, GuestMemory& mem, std::uint32_t* result);

struct NativeOverride {
    std::uint32_t ea = 0;
    std::string name;
    GuestConv conv = GuestConv::Cdecl;
    std::uint32_t arg_bytes = 0;  // 栈参数总字节（不含返回地址；thiscall 不含 ECX）
    NativeFn fn = nullptr;
    std::uint64_t hits = 0;
    std::uint64_t fallbacks = 0;
    // ★ 条件式 override 的**台账**（2026-11-10，REQ-01M6XVZF3VDC5NSBJ20J2451VJ）：
    //   · `fast_hits` = 走**零写短路**快路径的次数（`hits` 的子集；无条件项恒 0。对 `sub_4BBAB0`
    //     它 == `hits`：该项唯一的接管路径就是"三槽全惰性"的纯查询短路）。
    //   · `declines[]` = fn 自己回退时的**原因分桶**（`fallbacks` 的细分；桶 0 = 未分类 ——
    //     invoke 自己判的回退（栈参不在已提交页 / 没有实现）落进 `fallbacks` 但不落桶）。
    //   ⛔ 纯诊断：不参与任何判据、不改 guest 可见语义、不进 guest 内存。
    //   ★ 写入通道 = `X86::current_override`（invoke 在调 fn 期间设置，见 x86.h）。
    //   ★ 2026-11-11：8 → 12（`sub_4E0B40` 一项就用满 9 个原因：init/尺寸/幂次/log2/io/trig/bitrev 各一
    //     + `bitrev` 值域越界；留三格给后续）。桶只是诊断计数，扩容不影响任何既有下标。
    static constexpr std::size_t kDeclineSlots = 12;
    std::uint64_t fast_hits = 0;
    std::uint64_t declines[kDeclineSlots] = {};
};

class NativeOverrideTable {
public:
    // ★ 2026-11-07：8 → 12（BOOT profile 选出的 sub_429020 / sub_48AE90 两项 + 给后续留坑）。
    // ★ 2026-11-08：sub_4CCC60 用掉最后一格 ⇒ **表已满**；再加项必须同时抬高 kMax
    //   （下面的 static_assert 会先红，别等到 `all` 起不来）。
    // ★ 2026-11-10：12 → 13（`sub_4BBAB0` = Voice::Tick 的 stateless 快路径）。
    // ★ 2026-11-11：13 → 14（`sub_4367B0` = 退出写存档的 LZSS Encode 整函数）。
    // ★ 2026-11-11：14 → 15（`sub_4E0B40` = libvorbis `mdct_backward`；**数值容忍档**，
    //   不满足"函数级字节等价"，已知偏差见 native_mdct.h）。
    // ★ 2026-11-12：15 → 16（`__ftol2_sse` = MSVC 的 x87 → int64 截断辅助，`0x4E82C0`；
    //   本项是**唯一**写 `EDX` 的函数级项 —— 契约段已改成"按 ABI 写返回值"，见上面的 NativeFn）。
    // ★ **16 = 表满，全部稳定项**（`debug` 机制保留，但当前**没有** debug 项 ⇒ `--native-overrides all`
    //   会把每一项都开）。再加项必须同时抬高 kMax（`kKnown[]` 的 static_assert 会先红，别等到 `all` 起不来）。
    static constexpr std::size_t kMax = 16;

    bool add(std::uint32_t ea, const std::string& name, GuestConv conv, std::uint32_t arg_bytes, NativeFn fn,
             std::string* err);
    void clear();
    bool empty() const { return count_ == 0; }
    std::size_t size() const { return count_; }
    NativeOverride* at(std::size_t i) { return i < count_ ? &entries_[i] : nullptr; }
    const NativeOverride* at(std::size_t i) const { return i < count_ ? &entries_[i] : nullptr; }

    NativeOverride* find(std::uint32_t ea);
    const NativeOverride* find(std::uint32_t ea) const;

    // 执行一次命中：成功 ⇒ 设好 EAX / EIP / ESP 并返回 true；false ⇒ 原因写进 *why（调用方回退解释器）。
    // suspended=true ⇒ native 实现已经用 startNestedGuestCall() 铺了一个 guest 回调帧，
    //   EIP/ESP 已由 GuestCallStack 设置，调用方必须直接 continue（⛔ 不要回退解释器）。
    bool invoke(NativeOverride& ov, X86& cpu, GuestMemory& mem, const char** why, bool* suspended = nullptr);

private:
    std::array<NativeOverride, kMax> entries_{};
    std::size_t count_ = 0;
};

// ---------------------------------------------------------------------------------------
// 段级 override（**函数中段**的固定 EA，不是函数入口）
//
// ★ 为什么现有机制够不着：函数级 override 只在 `call` 目标（= 下一条 EIP）拦，所以只覆盖
//   "整个函数被一次调用替换"。`sub_414AC0` 的 1.18 s 全在函数中段的填池循环（`.lst` 0x415384，
//   ~7.37M 次迭代）—— 入口进去也没用，克隆整函数（文件 I/O + guest 容器/堆）不值得。
// ★ 前提（用户口径）：**guest 代码只读** ⇒ 某个固定 EA 处的代码是固定的，可以按 EA 拦。
//   ⛔ 不 patch guest 字节；⛔ 不每迭代拦一次 —— 一次调用只拦一次，然后跳到段的出口。
// ★ 命中点：dispatcher 在**每个 block 入口**按 EIP 查表（不是只在 call 目标）。
// ★ 语义契约（fn 必须自己保证）：
//   · 先判断"现在是不是段入口"（例如循环入口的 `xor eax,eax` ⇒ EAX==0）；不匹配 ⇒ Declined；
//   · 只在**所有前置条件**（参数 / 范围 / 计数 / 提交状态）匹配时才替 guest 执行；
//   · Replaced 时必须已经写好 **EIP（= 段出口）/ EAX / EDX / ECX / EFLAGS** 等 guest 可观察状态；
//     `replaced_insns` 报告被替代的 guest 指令数（dispatcher 用它补 `insns` 与虚拟时钟，
//     见 README §4.3 —— 否则 `timeGetTime` 会少走一截，首帧可能与解释器路径不同）。
// ---------------------------------------------------------------------------------------
enum class BlockOverrideOutcome : std::uint8_t {
    Replaced = 0,  // 已整段替换：fn 写好了 EIP / 寄存器 / EFLAGS
    Declined = 1,  // 前置条件不匹配（例如不是段入口）⇒ 回退解释器；不计 fallback
    Failed = 2,    // 前置条件匹配但执行失败 ⇒ 回退解释器；计 fallback + 打一行诊断
};

using NativeBlockFn = BlockOverrideOutcome (*)(X86& cpu, GuestMemory& mem, std::uint64_t* replaced_insns);

struct NativeBlockOverride {
    std::uint32_t entry_ea = 0;   // 固定 EA（`.lst` 锚；必须是一条指令的起点）
    std::uint32_t resume_ea = 0;  // 段的出口 EA（Replaced 后 EIP 落到这里）
    std::string name;
    NativeBlockFn fn = nullptr;
    std::uint64_t hits = 0;
    std::uint64_t declines = 0;
    std::uint64_t fallbacks = 0;
    // ★ 第一次 Declined 之后**永久停用**（本进程内）：段级 override 的前提是静态的
    //   （同一个 EA + 同一段代码），一次不匹配就说明这次运行的形状不是我们认得的那种；
    //   继续尝试只会在热路径上每迭代白付一次函数调用。
    bool disabled = false;
};

class NativeBlockTable {
public:
    static constexpr std::size_t kMax = 4;

    bool add(std::uint32_t entry_ea, std::uint32_t resume_ea, const std::string& name, NativeBlockFn fn,
             std::string* err);
    void clear();
    bool empty() const { return count_ == 0; }
    std::size_t size() const { return count_; }
    NativeBlockOverride* at(std::size_t i) { return i < count_ ? &entries_[i] : nullptr; }
    const NativeBlockOverride* at(std::size_t i) const { return i < count_ ? &entries_[i] : nullptr; }

    // 按 EIP 找**仍然启用**的项（disabled 的项不返回 ⇒ dispatcher 走解释器）。
    NativeBlockOverride* find(std::uint32_t eip);
    const NativeBlockOverride* find(std::uint32_t eip) const;

private:
    std::array<NativeBlockOverride, kMax> entries_{};
    std::size_t count_ = 0;
};

// 已知的 CRT 叶子（按名字解析 EA；EA 来自 `.lst` 的 `proc near` / `COLLAPSED FUNCTION` 行）。
// ★ 分层：`debug == false` 是**稳定**项，`--native-overrides all` 只含这一层；
//   `debug == true` 是保留用于复现/调试的原型，必须**显式点名**才能启用
//   （例如 `--native-overrides all,某个debug项`），⛔ 不会被 `all` 带进来。
struct KnownNative {
    const char* name;
    GuestConv conv;
    std::uint32_t arg_bytes;
    NativeFn fn;
    bool debug = false;
};
// ---------------------------------------------------------------------------------------
// ★ 函数级 override 的**准入门**（进 `all` = `debug == false` 的判据；五条缺一不可）
//
//   ① **契约可核**：函数身份（EA）、ABI（调用约定 + 清栈字节）、每个参数与返回值的来源都能在
//      `.lst` 里逐字复核。⛔ 不许"看起来是这样"；`.c` 只能提假设，锚只有 EA。
//   ② **准入零写**：任何前置条件不成立就 `return false`，且回退之前**没有任何 guest 可见副作用**
//      —— guest 内存 / 寄存器 / x87 状态字与栈 / 堆 / 锁 / 宿主全局的软浮点状态都算。
//      判据是"直接调 fn + 冻结快照"；⛔ 不是两臂差分（回退后解释器会重放整个函数，把回退前的写修回去）。
//   ③ **纯核 + 分支覆盖**：把与 `X86`/ABI 无关的算术抽成纯函数（只吃宿主缓冲 / 标量），并用合成输入
//      覆盖回绕 / 别名（src == dst）/ 极值 / 空输入这些分支。
//   ④ **有一档函数级真字节等价**：让**同一份 guest 机器码**跑一遍解释器当 oracle，与 native 实现逐位
//      比较输出（返回值 + 被改对象/内存 + 相关的 FPU/寄存器状态）。**只在输出确定时要求**
//      （数据或时间相关的输出先由 oracle 定值；⛔ 不锁当前数字）。
//   ⑤ **已知偏差登记 + 可隔离**：凡与 guest 不完全一致的地方（相位/时序偏移、故意更保守的回退、
//      不复刻的死区写…）都要在实现处登记成**已知边界**，且必须能用 `all,-<名字>` 单独排除；
//      `hits` / `fallbacks` / `declines[]` ⛔ **不得作为唯一判据**（可以作为"这一项真的被接管 /
//      真的回退"的**伴随断言**）。
//
//   ★ 为什么"帧级 / 逐像素 A/B"**不是**准入门：函数级 override 在 dispatcher 里只记 **1 条指令 +
//     1 个 tick** ⇒ 跳过 N 条 guest 指令就少走 `(N−1) × 100 ns` 虚拟时间，相位必然移动，
//     逐帧相同在结构上不成立。相位偏移 = 登记的已知边界，不是缺陷。
// ---------------------------------------------------------------------------------------
const KnownNative* findKnownNative(const std::string& name);
std::size_t knownNativeCount();
const KnownNative& knownNativeAt(std::size_t i);

// ★ 段级项（函数中段）的已知表：`name` 同时是 `--native-overrides` 里写的名字。
//   `enclosing_fn` 只用于**启用时的锚校验**（EA 必须落在这个函数里）——EA 是固定的 `.lst` 锚，
//   ⛔ 不从名字推 EA（中段没有符号）；校验失败 ⇒ 拒绝启用（⛔ 不静默换一个 EA）。
struct KnownBlockNative {
    const char* name;
    const char* enclosing_fn;
    std::uint32_t entry_ea;
    std::uint32_t resume_ea;
    NativeBlockFn fn;
    bool debug = false;
};
const KnownBlockNative* findKnownBlockNative(const std::string& name);
std::size_t knownBlockNativeCount();
const KnownBlockNative& knownBlockNativeAt(std::size_t i);
// 用 `.lst` 校验段级项的锚（entry 是一条真实指令、属于 `enclosing_fn`、出口也是一条指令）。
// 失败 ⇒ false + err（点名哪一条校验没过）。
bool verifyKnownBlockAnchor(const LstIndex& lst, const KnownBlockNative& k, std::string* err);

// ---------------------------------------------------------------------------------------
// thiscall 原语（MSVC x86）：`ECX = this`，栈参数从返回地址之后按 **0 基** 展开。
//
// ★ 为什么单独抽出来：本文件的 thiscall 项越来越多（`sub_436A80` / `sub_48B090` / `sub_429020` /
//   `sub_48AE90` / `sub_487C60` / `sub_46D9F0`）。每个实现各写一遍 `cpu.ecx()` + `cpu.arg(i)` 的话，
//   "第几个 arg 对应第几个形参"就散落在各处、没有一处可复核；顺序错一位不会编译报错，只会在
//   某个分支上静默读错参数。⇒ 把"读参数的顺序"钉在一处：`self` 之后是 `a[0..arg_count-1]`，
//   与 `retn N` 的字节数无关（栈帧的提交状态由 `NativeOverrideTable::invoke()` 统一校验）。
// ★ 只**读**：不改栈、不弹参、不判 ABI。⛔ 不要在这里做安全判断（那是各实现的事）。
// ---------------------------------------------------------------------------------------
struct ThisCallArgs {
    static constexpr std::size_t kMax = 8;  // 本线 thiscall 项最多的栈参数数（sub_46D9F0 = 8）
    std::uint32_t self = 0;                 // ECX
    std::uint32_t a[kMax] = {};             // 栈参数（0 基，`a[0]` = 返回地址之后的第一个 dword）
    std::uint32_t operator[](std::size_t i) const { return i < kMax ? a[i] : 0u; }
};
// `arg_count` 超过 `ThisCallArgs::kMax` 时**截断**（不会越界读；调用方自己保证够用）。
ThisCallArgs thisCall(const X86& cpu, std::size_t arg_count);

// 具体实现（也直接给测试用）
bool nativeMemcpy(X86& cpu, GuestMemory& mem, std::uint32_t* result);
bool nativeMemset(X86& cpu, GuestMemory& mem, std::uint32_t* result);
// `sub_401000` 的语义是**符号**（`.lst` 401066: `sar eax,1Fh; or eax,1` ⇒ -1/0/+1），
// 不是字节差。`_memcmp` 的 CRT 实现是 COLLAPSED（没有可观察字节）⇒ 暂不覆盖。
bool nativeSignMemcmp(X86& cpu, GuestMemory& mem, std::uint32_t* result);
bool nativeStrlen(X86& cpu, GuestMemory& mem, std::uint32_t* result);

// ---------------------------------------------------------------------------------------
// `__ftol2_sse`（`0x4E82C0`，`__cdecl`、**0 个栈参**、返回 `EDX:EAX` = 64 位截断值）
//
// ★ 身份 / 形状（`.lst` 381874 的 COLLAPSED 行，28 B 入口 + `0x4E82F6` 起的 117 B 本体）：
//   `0x4E82C0 cmp ds:dword_55F2E8,0 / jz 0x4E82F6`；另一支 `0x4E82C9..0x4E82DB` 是
//   `fstp qword [esp]; cvttsd2si eax,[esp]`（本线走不到）。`0x4E82F6 = __ftol2` 是**函数内**的
//   `jz` 目标 ⇒ 挂它 = hits 恒 0（本单的反面判据）。
// ★ 实现（`native_crt.cpp`）：`x87ToI64(st0, truncate=true)` + 一次 `x87RegToMem(st0, scratch, 4)`
//   （同时给出 PE/UE/OE 与 guest 可见的那 4 字节）+ 弹 1 层 + tag/状态字同步。
//   纯核没有单独的 `.h`：本项就是"两次 softfloat 调用 + 弹栈"，抽出去只剩一个转调。
// ★ 闸门 4+1 条（全部在**任何**副作用之前；不做窄档）：`[0x55F2E8] == 0` 且已提交 ·
//   `st(0)` 非空（`empty(phys(0))`，⛔ 不是 `empty(0)`）· x87 空闲槽 ≥ 2 · 栈 scratch
//   `[align16(esp-0x24), +0x20)` 已提交 · **越界边界**（`x ∈ (2^63−1, 2^63)` 上 guest 的
//   "就近"与我们的"截断"不同 ⇒ 交解释器，见 `nativeFtol2Sse` 的 G-5）。
// ★ 已知边界（登记，不是缺陷）：① 时钟相位（函数级 override 只记 1 条指令）；
//   ② scratch 里另两段（`[E+10h,E+18h)` 的就近 int64、非零支的 `[E,E+4)`）**不复刻** ——
//      它们是 caller 栈帧以下的死 scratch；③ 被消费掉的物理槽残留字节不同（tag 已 empty）；
//   ④ `fpu.status` 的 oracle 是**解释器**（它的 `fistp` 不置 PE、`fld/fild` 不置 C1）⇒
//    "逐位一致"指的是与解释器一致，⛔ 不是与 SDM/真机一致。
// ★ 隔离：`all,-__ftol2_sse`（`-name` 是通用机制）。
// ---------------------------------------------------------------------------------------
//
// 回退原因（**枚举值就是 `NativeOverride::declines[]` 的下标**；0 = 未分类，不落桶）：
enum class FtolWhy : std::uint8_t {
    FlagUnreadable = 1,     // `[0x55F2E8]` 不在已提交页 ⇒ 读不出"走哪条路"（非 safe mode 会静默给 0）
    Sse2Path = 2,           // `[0x55F2E8] != 0` ⇒ 真机走 `cvttsd2si`（只写 EAX，越界语义不同）
    StackEmpty = 3,         // `st(0)` 空（tag[phys(0)] == empty）⇒ 没有输入
    StackFull = 4,          // 空闲槽 < 2 ⇒ guest 的 `fld st(0)` 可能走上溢支（不压栈 + C1/IE）
    ScratchUnreadable = 5,  // `[align16(esp-0x24), +0x20)` 越界或未提交 ⇒ guest 会 fault，不替它决定
    TruncRoundDiverge = 6,  // `x ∈ (2^63−1, 2^63)`：guest 的 `fistp`（就近）与我们的截断在越界边界上不同
};
bool nativeFtol2Sse(X86& cpu, GuestMemory& mem, std::uint32_t* result);

// `sub_436A80`：S4IC / LZSS 解压（profile 选出的 boot 第一热点）。
bool nativeLzss436A80(X86& cpu, GuestMemory& mem, std::uint32_t* result);
// `sub_48B090`（NormalTexture::UploadImage）：thiscall（ECX = NormalTexture），栈参数 (img, alpha, key)。
// 准入失败（不支持的源格式 / 色键未实现 / 目标函数认不出）⇒ 返回 false，dispatcher 回退解释器。
bool nativeUpload48B090(X86& cpu, GuestMemory& mem, std::uint32_t* result);
// `sub_429020`（Map::findIndex(hash, key)）：thiscall `retn 8`。开放寻址表；槽 28 B = MSVC
// `std::string`（16 B SSO + len@+0x10 + cap@+0x14）；表指针 = `[this+0x18]`、容量 = `[this+8]`；
// 空槽判据 `len==0`；命中 = 先比 `min(len)` 字节、再比长度；不中线性探测；返回 index / -1。
// ★ 只读 guest 内存；所有前置条件（this / 表 / 容量 / index / 提交状态）不成立 ⇒ 返回 false 回退。
bool nativeFindIndex429020(X86& cpu, GuestMemory& mem, std::uint32_t* result);
// `sub_4CCC60`（Live2D `BDAffine` 虚表第 6 项，`__stdcall retn 1Ch` = 28 B）：逐顶点仿射变换。
// 纯核在 `native_affine.h`；已进 `all`（2026-11-11；x87 逐位判据见 tests/native_override_assets.cpp，
// 回退零写见 tests/native_override.cpp 的 `affine/decline-no-write`）。
// ★ 空循环（bound <= i0）一律回退解释器：`.lst` 0x4CCC71..0x4CCCDB 的序言在 `0x4CCCDE jge` 之前，
//   guest 即使不进循环也会置 `fpu.status` 的 PE/ES、清 C0..C3（见 `native_affine.cpp` 那一支的注释）。
bool nativeAffine4CCC60(X86& cpu, GuestMemory& mem, std::uint32_t* result);
// `sub_4E0B40`（libvorbis `mdct_backward`，`__cdecl` 12 B）：语音起播那一下的 FFT/MDCT 内核。
// 纯核 + 已知偏差（**数值容忍档**，本仓第一项）+ 9 个回退原因都在 `native_mdct.h`。
bool nativeMdct4E0B40(X86& cpu, GuestMemory& mem, std::uint32_t* result);
// `sub_487C60`（CMovieTextureRenderer::DoRenderSample，thiscall `retn 4`）：影片样本 → A8R8G8B8 纹理。
// 只覆盖主路径（无 alpha 面 / 无 alpha 拷贝 / 3 或 4 字节源）；其余回退解释器。
bool nativeRender487C60(X86& cpu, GuestMemory& mem, std::uint32_t* result);
// ---------------------------------------------------------------------------------------
// `sub_4BBAB0` = `Voice::Tick(now)`（每帧固定税；REQ-01M6XVZF3VDC5NSBJ20J2451VJ 的 **A 案**）
//
// ★ 身份 / 形状（`.lst` 0x4BBAB0..0x4BBC24，thiscall / `retn 4`）：`ecx = this`（= T_Engine+0x148A0）、
//   1 个栈参 = `now`(ms)；三个槽 = `this+0x424 + 4*i`（i = 0..2 ⇔ 声道 12/13/14）；
//   每个槽只按两个 dword 分流：`[slot+0x30] & 3`（== 3 ⇒ 内层扫描）与 `[slot+0]`（非 0 ⇒ 已启动）。
// ★ 只接管**三槽全惰性**的那一种调用（`(slot[0x30] & 3) != 3` 且 `slot[0] == 0`，三个槽都如此）：
//   那条路径**一条写都没有、一个调用都没有**（只有读 + 循环尾），所以整条调用退化成
//   "读 6 个槽 dword（+ 1 个设备指针 dword）+ 返回 0" ⇒ `*result = 0` 直接返回。
// ★ 其它一切（内层扫描 / 已启动 / 首 tick / 到期 / 无设备 / 读不到）⇒ 返回 false **回退解释器**：
//   ⛔ 不重写 `sub_4BB840`，⛔ 不重写整个 Tick。**有写时**跨槽顺序本身承载语义 ⇒ 更不能跳读 / 换序。
// ★ 回退分支**不写任何 guest 字节**（判据：tests/voice_poll.cpp 的 ⑧）。
// ★ 时钟补偿见 `kVoiceTickPureInsns`（**判据**，不是优化细节）。
//
// 回退原因（**枚举值就是 `NativeOverride::declines[]` 的下标**；0 是 `Pure`，不落桶）：
enum class VoicePollWhy : std::uint8_t {
    Pure = 0,       // 三槽全惰性 ⇒ 唯一可短路的形态
    NoDevice = 1,   // `[this+0x408] == 0`（无设备）⇒ 回退（保守：设备都没了就交给解释器）
    BadThis = 2,    // this == 0 / 会回绕 ⇒ 或 `this+0x408..0x460` 不在已提交页 ⇒ 回退
    InnerScan = 3,  // `(slot[0x30] & 3) == 3` ⇒ 内层扫描 + 可能写 ⇒ 回退
    FirstTick = 4,  // 已启动且 `[slot+0xC] == 0` ⇒ 解释器会写 `last = now` ⇒ 回退
    NotDue = 5,     // 已启动、未到期 ⇒ **语义上**是纯查询，但**指令数不同**（k 不成立）⇒ 回退
    Due = 6,        // 已启动、到期 ⇒ 会清标志 / 调 `sub_4BB840` ⇒ 回退
};
struct VoicePollDecision {
    bool pure = false;      // true ⇒ 只有这一个形态可以短路
    VoicePollWhy why = VoicePollWhy::BadThis;
    unsigned slot = 0;      // 让它不纯的槽（0..2）；pure 时为 3（= 全部走完）
};
// 纯核（测试直接用）：**只读** guest 内存（committed 检查先行），不写、不调任何 guest 代码。
VoicePollDecision voiceTickIsPureQuery(const GuestMemory& mem, std::uint32_t self, std::uint32_t now_ms);
// `sub_4BBAB0` 的**三槽全惰性**路径执行的 guest 指令数（= 时钟补偿的定值 k）。
// ★ 为什么它是判据：`now` 来自 `timeGetTime`，而本线虚拟时钟按**指令数**推进（clock.h）⇒
//   override 少走/多走一条指令就改了"下一次 tick 看到的 now"这个**输入序列**。
// ★ 推导（`.lst` 0x4BBAB0..0x4BBC24 逐条数）：
//     prologue 15（`push ebp` .. `jmp loc_4BBAD7`）
//   + 3 × 12（槽内 6：`mov eax,[esi+30h]` / `and` / `cmp` / `jnz` / `cmp [esi],0` / `jz`
//               循环尾 6：`mov eax,ArgList` / `inc` / `add esi,4` / `mov ArgList,eax` / `cmp` / `jl`）
//   + 2 × 1（`loc_4BBAD5` 的 `xor ecx,ecx`；第 3 次 `jl` 不跳、不进它）
//   + epilogue 7（`mov eax,[ebp-0xC]` / `pop edi` / `pop esi` / `pop ebx` / `mov esp,ebp` /
//                 `pop ebp` / `retn 4`）
//   = 15 + 36 + 2 + 7 = 60。
constexpr std::uint32_t kVoiceTickPureInsns = 60;
bool nativeVoiceTick4BBAB0(X86& cpu, GuestMemory& mem, std::uint32_t* result);
// `sub_48AE90`（NormalTexture 虚表第 6 项 = 纯色填充 Fill(rect, color888, alpha)）：thiscall `retn 0Ch`。
// 对 `[self+0x40C]` 的纹理 `LockRect(rect)`（vtable +0x4C）→ 按**虚拟显示器 bpp**（guest 全局
// `ds:dword_5518C0`，⛔ **不是** `ddraw::displayMode`）走 16bpp（A4R4G4B4）或 32bpp →
// `UnlockRect`（+0x50）；返回 0。
bool nativeFill48AE90(X86& cpu, GuestMemory& mem, std::uint32_t* result);
// 纯像素核（只碰宿主缓冲；测试可直接用）。`base` = LockRect 返回的 pBits（**已按 rect.left/top
// 偏移**），`pitch` = LockRect 返回的 Pitch。语义逐字照 `.lst` 的四个分支 —— 包括 16bpp
// `alpha<0` 分支里 `2*(pitch*row/4)`（= pitch 的一半）这个**与 32bpp 同形、但不是 pitch 步进**的
// 写法：⛔ 不要"顺手修正"，那是与 guest 不一致。
void fillRect16(std::uint8_t* base, std::int32_t pitch, std::int32_t width, std::int32_t height,
                std::uint32_t color888, std::int32_t alpha);
void fillRect32(std::uint8_t* base, std::int32_t pitch, std::int32_t width, std::int32_t height,
                std::uint32_t color888, std::int32_t alpha);
// `sub_46D9F0`：字形位图 → **已锁定**纹理表面的逐像素混合叶子（thiscall `retn 20h`，8 个栈参数 = 32 B）。
// 参数：ECX = T_Font*；a2 = 目标 pitch（字节）· a3 = 目标像素基址（调用方已 Lock）· a4 = 4 个 dword
// （宽/高/原点 x/原点 y）· a5 = 源行跨度 · a6 = 源基址（GetGlyphOutlineA 的字形缓冲）· a7/a8 = 目标 x/y ·
// a9 = 颜色（**低字节是第一通道**，与 `.lst` 0x46DC76 的字节反转一致）。返回 EAX = `(a4[1] > 0) ? a5 : 0`。
// ★ 语义逐字照 `.lst` 0x46D9F0..0x46DEC2（16bpp / 32bpp 两支）；裁剪矩形 = guest `sub_46CE20` 的成功路径
//   （`[mgr+0xA5D8+4*texid]+0x41C..0x428`，mgr=`[Font+0x410]`、texid=`[mgr+0xB648]`）。
// ★ 准入失败（this/mgr/texid/裁剪对象/参数跨度/级数越界…）⇒ 返回 false，dispatcher 回退解释器。
bool nativeBlend46D9F0(X86& cpu, GuestMemory& mem, std::uint32_t* result);
// 源字形格式的选择来自 Font 的两个字段（⛔ 不是宿主"抗锯齿开关"）：
//   `[Font+0x548] == 0` ⇒ 1bpp 位图；非 0 ⇒ 8bpp 级数（每像素 1 字节）。
enum class GlyphMaskSource : std::uint8_t {
    BitMask1 = 0,  // 0x46DAC2..0x46DB08：位序 `7 - col%8`，置位 ⇒ 255
    Level8 = 1,    // 0x46DA89..0x46DAC0：字节 × 255 后除 65 / 17
};
// `[Font+0x35584]`：== 1 ⇒ 65 级（除 65，满覆盖 251）；否则 17 级（除 17，满覆盖 240）。
enum class GlyphLevels : std::uint8_t { Levels17 = 0, Levels65 = 1 };
// 纯像素核的入参（`sub_46D9F0` 的 8 个栈参数 + 裁剪矩形 + 两个格式位）。
// ★ 契约：调用方保证 `[dst, dst + 最后一行偏移 + 像素)` 与 `[src, src + 最后一行偏移 + 行字节)`
//   都在**同一段不回绕**的地址里（`nativeBlend46D9F0` 已经逐行校验过）；核里只做 guest 的
//   32 位地址算术（回绕），不做准入、不做提交检查。
struct GlyphBlit {
    std::uint8_t* dst = nullptr;        // a3（已按 pitch/bits 定位的像素基址，**不**预先偏移 x/y）
    std::int32_t dst_pitch = 0;         // a2（字节）
    std::int32_t width = 0;             // a4[0]
    std::int32_t height = 0;            // a4[1]
    std::int32_t origin_x = 0;          // a4[2]
    std::int32_t origin_y = 0;          // a4[3]
    const std::uint8_t* src = nullptr;  // a6
    std::int32_t src_stride = 0;        // a5（字节；guest 逐行 `add`，可为负）
    std::int32_t dst_x = 0;             // a7
    std::int32_t dst_y = 0;             // a8
    std::uint32_t color = 0;            // a9
    std::int32_t clip_left = 0;         // sub_46CE20 的 rect[0..3]
    std::int32_t clip_top = 0;
    std::int32_t clip_right = 0;
    std::int32_t clip_bottom = 0;
    bool sixteen_bpp = true;            // `ds:dword_5518C0 == 0x10`
    GlyphMaskSource source = GlyphMaskSource::BitMask1;
    GlyphLevels levels = GlyphLevels::Levels17;
};
// 返回 guest 的 EAX（= `(height > 0) ? src_stride : 0`）。⛔ 不做 clamp：`.lst` 里没有 clamp。
std::uint32_t blendGlyph46D9F0(const GlyphBlit& b);

// 段级：`sub_414AC0` 中段填池循环（entry 0x415384 / resume 0x41539F）。
// 语义（`.lst` 0x41537E..0x41539D）：`ecx=[esi-0x10]`（目标数组）、`edx=[ebx+0x5EC90]`（填充值）、
// count = `[ebx+0x5D7E8]+edi+1`、EAX 从 0 起每次写 `[ecx+eax*4]`。完成后 guest 状态：
// EAX=EDX=count、ECX=[ebx+0x5D7E8]、EFLAGS = `cmp eax,edx` 相等的结果（ZF=1/PF=1，其余状态位 0）。
BlockOverrideOutcome nativeFillPool414AC0(X86& cpu, GuestMemory& mem, std::uint64_t* replaced_insns);
// 纯解码核（测试直接用；src/comp = 输入，dst/out = 输出，ring = 4096 B 字典）。
bool lzssDecode(const std::uint8_t* src, std::uint32_t comp_size, std::uint8_t* dst, std::uint32_t out_size,
                std::uint8_t ring[4096]);
// sub_48B090 的像素核（纯函数：只碰宿主缓冲，测试可以直接用）。
// ★ 语义逐字照 `.lst` 的 `sub_48B090`：
//   - 源是 **bottom-up** DIB（源 row = height-1-目标 row），目标是 **top-down**（行按 `dst_pitch` 前进）；
//   - `alpha`（可为空）**按目标行序线性消费**（guest 在行循环里只 `inc` 指针、不重置）
//     ⇒ index = row*width+col；32bpp 源**不读** alpha（用源像素自带的第 4 字节）；
//   - 每个像素的 32 位值就是内存里的字节序（R 在最低字节、A 在最高字节）；
//   - 8bpp 的调色板项 = 4 字节步长、只用前 3 字节（byte0→bit0 / byte1→bit8 / byte2→bit16），
//     没有 alpha 时补 0xFF。
// 4bpp / 1bpp / 16bpp **不支持**（本线没有证据级用例；`convertImageTo32` 显式拒绝）。
void convertPal8To32(const std::uint8_t* src, std::uint32_t stride, std::uint32_t w, std::uint32_t h,
                     const std::uint8_t* pal, std::uint8_t* dst, std::uint32_t pitch, const std::uint8_t* alpha);
void convert24To32(const std::uint8_t* src, std::uint32_t stride, std::uint32_t w, std::uint32_t h,
                   std::uint8_t* dst, std::uint32_t pitch, const std::uint8_t* alpha);
// 32bpp 源：**逐字节搬运**（guest 0x48B3xx 的 default/32 分支就是 `memcpy(dst, &Src, 4)`），
// alpha 参数不存在 —— 源像素的第 4 字节就是目标 alpha。
void convert32To32(const std::uint8_t* src, std::uint32_t stride, std::uint32_t w, std::uint32_t h,
                   std::uint8_t* dst, std::uint32_t pitch);

// 源像素描述（宿主指针；由调用方从 guest 内存里解析出来）。
struct ImageSource {
    std::uint32_t bpp = 0;                  // 8 / 24 / 32
    const std::uint8_t* pixels = nullptr;   // 源像素首字节（行按 stride、bottom-up）
    std::uint32_t stride = 0;               // 源行字节数（DIB 的 4 字节对齐）
    const std::uint8_t* palette = nullptr;  // 8bpp 才用：256 项 × 4 字节
    const std::uint8_t* alpha = nullptr;    // 8/24bpp 才用：h*w 字节，按目标行序；32bpp 忽略
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};
// 校验 + 分发到上面三个核。返回 false + why（**不写** dst 之外的任何东西）。
bool convertImageTo32(const ImageSource& src, std::uint8_t* dst, std::uint32_t dst_pitch, std::string* why);

// 把源像素写进一个**已存在**的纹理：走 `d3d9::lockRect` / `unlockRect`（与 guest 0x48B234 /
// 0x48B06C 同一路径），所以 pitch / pBits / 锁定计数 / 已锁拒绝都是那一份语义。
// ★ 前置条件（guest 侧的观察，不是本函数编的）：目标必须是 32bpp、尺寸与 src 相符。
// ★ 失败时不写像素、并保证**不留下锁**（除非锁本来就不是我们加的）。
bool uploadPixelsToTexture(X86& cpu, std::uint32_t texture_object, const ImageSource& src, std::string* why);

// ---------------------------------------------------------------------------------------
// `sub_4367B0` = 退出写存档的 **LZSS Encode 整函数**（REQ-01M7375DARF1QFZEFNWX3CAK6T）
//
// ★ 形状（`.lst` 0x4367B0..0x436A75，thiscall `retn 8`）：`ecx = this` = `Compress` 对象（长 `0xD430`）、
//   栈参 `a2` = 输入、`a3` = 3 个 dword 的 out-params（`a3[0]` 读、`a3[1]`/`a3[2]` 写）；**返回 EAX = `[this+0xD42C]`**。
//   `a3[1]` = **实际消费掉的输入字节数**（初值 = 首灌条数，逐轮 `+= n`；放弃压缩时整体 = 输入长度）。
//   `a3[2]` = 压缩后字节数。
// ★ 分层（**为什么不是"宿主数组 + 最后整块拷回"**）：纯核拿到的是 `this` 的**宿主视图**并**原地改**它
//   ⇒ 校验过的"未触碰字节天然相同"是结构保证，不需要逐字段复刻。★ 诚实清单（复审更正）：
//   guest 会写 `dad[4096]`（`0x43662D/0x436634` 的 `dad[lson[p]]`/`dad[rson[p]]`，lson/rson 可为 NIL）、
//   `text_buf[4096..4112]`（`0x43699D`，s<17）、以及 `match_pos`/`match_len`；**真正不写的**只有
//   vftable（+0）、`lson[4096]`、`rson[0..4096]`、`text_buf[4113..)` —— 这些靠"没碰"保证相同。
// ★ 宿主视图的**契约**：`obj`/`in`/`out` 都是 `GuestMemory::host()` 的平坦映射裸指针（`base_ + ea`），
//   先例 `nativeLzss436A80` 已这么写；代价是绕过 `g_access` 与 safe-mode 的逐次检查 ⇒ **调用方必须先
//   逐段 `committed()`**（`nativeLzss4367B0` 的**准入 7 条**就是这件事），纯核本身不做提交检查。
// ★ 分配：`cpu.heap->alloc(a3[0] + 0x1000, /*zero=*/false)` 与 guest 的 `new[] → _malloc → HeapAlloc`
//   是同一条路（同一个 `GuestHeap`、同一请求字节数、同一个 zero 标志）。分配失败 ⇒ `return false`
//   （**在任何 guest 写之前**）—— ⛔ 不许编造 `EAX=0`：`0x436811` 的 `if (!p) return 0` 在真机上
//   是**走不到的死代码**（`operator new` 的 `ret` 只在 `_malloc` 非 0 时到达）。
// ★ 准入（不满足 ⇒ false + `declines[]` 分桶；**零写**）：`a3` 12 B 已提交 ∧ `[this, this+0xD430)` 已提交
//   ∧ 输入跨度已提交（`a2+a3[0]` **不**回绕时）∧ **`a3[0] <= 0xFFFFEFFF`**（`a3[0]+0x1000` 不回绕；
//   `0xFFFFF000` 那格会让 guest 走 `new[](0)`）∧ **`[this+0xD42C] == 0`**（只接管"新对象"，见
//   `LzssWhy::ReusedOut`）∧ 有可用的 guest 堆。
// ★ 不补虚拟时钟：本函数省掉的 guest 指令数**随数据变**（150k 次 `InsertNode` 级别），没有定值 k 可补
//   （需求单已裁决：存档时间戳仍会比基线早 ~12 s，属已知边界）。
// ★ 撤销：删 `kKnown` 里那一项 + 把 `kMax` 调回 13。
enum class LzssWhy : std::uint8_t {
    Ok = 0,          // 接管（不落桶）
    ReusedOut = 1,   // `[this+0xD42C] != 0`（对象被复用 / 未构造）⇒ 拒
    NoInfo = 2,      // `a3 == 0` 或那 12 B 不在已提交页
    BadSelf = 3,     // `this == 0` / `this+0xD430` 回绕 / 不在已提交页
    BadInput = 4,    // 输入跨度不在已提交页
    Overflow = 5,    // `a3[0] + 0x1000` 溢出
    NoHeap = 6,      // 没有可用的 guest 堆
    AllocFailed = 7, // `cpu.heap->alloc` 失败（⇒ 回退，由真正的 `_malloc`/`__callnewh` 自己表态）
};
struct LzssEncodeOutcome {
    std::uint32_t consumed = 0;  // -> `a3[1]`（**按计数**写，⛔ 不写死 = a3[0]）
    std::uint32_t comp = 0;      // -> `a3[2]`
    bool gave_up = false;        // `comp >= a3[0]` ⇒ 已把输入**从 a2 起点**原样拷进 out
};
// 纯核（测试直接用）：完全在宿主侧，不碰 `X86` / ABI / 分配。`obj` = `this` 的宿主视图（**原地改**）、
// `in`/`in_size` = 输入宿主视图、`out` = 出缓冲宿主视图（容量 = `in_size + 0x1000`）。
// ★ `in_ea` **只**用来复现 guest 的 **32 位地址比较**（首灌循环的 `cmp/jnb`：`a2+a3[0]` 回绕时
//   首灌 0 次 ⇒ `consumed == 0`、`comp == 0`、只做 InitTree + text_buf 清零）。⛔ 不当指针用。
LzssEncodeOutcome lzssEncodeObject(std::uint8_t* obj, std::uint32_t in_ea, const std::uint8_t* in,
                                   std::uint32_t in_size, std::uint8_t* out);
bool nativeLzss4367B0(X86& cpu, GuestMemory& mem, std::uint32_t* result);

// 从 native override 里铺一个 guest 回调帧（必须由 NativeOverrideTable::invoke 的 suspended 机制接住）。
// `callee_ecx` = 进入 callee 前要写进 ECX 的 `this`（thiscall）；0 = 不动。
bool startNestedGuestCall(X86& cpu, std::uint32_t fn, const std::vector<std::uint32_t>& args,
                          std::uint32_t cleanup_bytes, const char* api, GuestContinuation cont, std::string* err,
                          std::uint32_t callee_ecx = 0);

}  // namespace lift
