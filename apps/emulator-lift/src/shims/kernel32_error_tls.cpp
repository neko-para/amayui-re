// emulator-lift · kernel32 错误码 / TLS / Interlocked 族（S6 第一族）
//
// ★ 这一族的口径：
//   · 错误码 = **每 Machine 一份**的 `cpu.shim.last_error`（⛔ 不是宿主 errno / 宿主 GetLastError）。
//   · TLS 槽数组在 **guest** 里：`fs:[0x2C]` → 64 个 DWORD；槽 0 保留、1..63 可用
//     （Win32 的 TlsAlloc 最多给 64 个索引）。
//   · 分配位图（哪些槽被 TlsAlloc 占了）**也放 guest 内存**：TLS 数组页的余量
//     `array_ea + 0x100`（魔数 'TLSB' + 位图，2 个 DWORD）。放进 guest 地址空间是为了
//     "一台 Machine 一份"：⛔ 不用进程级 C++ 静态变量（换 Machine 会串味，trace 会依赖历史）。
//   · `FlsAlloc/FlsFree/FlsGetValue/FlsSetValue` → **与 TLS 同实现**（单纤程 guest；Fls 的
//     差别只有"纤程切换时回调"与"分配是按线程的"，本模拟器都没有）。
//   · `EncodePointer/DecodePointer/EncodeSystemPointer/DecodeSystemPointer` → **恒等**。
//   · `Interlocked*`：guest 单线程 ⇒ 普通读改写即等价（原子性由"只有一条执行流"保证）。
//     返回值语义按 Win32：Increment/Decrement 返回**新值**，Exchange / ExchangeAdd /
//     CompareExchange 返回**旧值**。
//
// ⚠ 偏差（逐条）：
//   1. **指针混淆是恒等**。理由：EncodePointer 的 cookie 是**进程内常量**，它只防"跨进程 / 跨
//      权限读别人的内存"。本模拟器是单进程 guest，4 GiB 平坦映射全在宿主可读 —— 混淆没有任何
//      安全收益，反而让 guest 写进结构里的"编码指针"与真实地址不一致，诊断更难。影响：
//      guest 若自己比较"EncodePointer(x) != x"（几乎没人这么做）会看到不同结果。
//   2. **TLS 分配位图寄居在 TLS 数组页的余量**（`array_ea+0x100`）。真实 Windows 的分配状态在
//      loader 里、guest 看不见；这里我们把自有记账放进 guest 页，好处是随地址空间一起生灭。
//      影响：若将来有人把 TLS 数组页 0x100 之后的余量拿去放别的东西，会与这里冲突
//      （该地址段在本项目里由本文件独占，见常量 kTlsBookOff）。
//   3. **TlsFree/TlsGetValue/TlsSetValue 的非法槽**：按 Windows 的失败值 + SetLastError 返回
//      （87 = ERROR_INVALID_PARAMETER），不 abort —— 这是"能返回真实失败值"的情形。
//   4. **FlsAlloc 的回调参数被丢弃**（不注册纤程回调）。影响：FlS 回调型析构不会发生；
//      guest 只把它当"另一个 TLS"用时无差别。
//   5. **Interlocked* 只有 32 位版本**（其余 64 位/指针版本本批不注册，执行到就响亮失败）。
#include <cstdint>
#include <string>

#include "runtime/x86.h"
#include "shims/register.h"

namespace lift {
namespace {

// TEB 里 TLS 数组指针的偏移（Win32 x86 TEB: ThreadLocalStoragePointer @ 0x2C）
constexpr std::uint32_t kTebTlsArrayOff = 0x2Cu;
constexpr std::uint32_t kTlsSlots = 64u;         // 槽 0 保留 ⇒ 可分配 63 个，与 Windows 同
constexpr std::uint32_t kTlsBookOff = 0x100u;    // 数组页余量：魔数 + 位图（本文件独占）
constexpr std::uint32_t kTlsBookMagic = 0x544C5342u;  // 'TLSB'
constexpr std::uint32_t kErrInvalidParameter = 87u;
constexpr std::uint32_t kErrNoSystemResources = 1450u;
constexpr std::uint32_t kTlsOutOfIndexes = 0xFFFFFFFFu;

void requireMem(X86& cpu, const char* api) {
    if (!cpu.mem) shimAbort("kernel32", api, "cpu.mem 为空（Machine 没装配 guest 内存）");
}

std::uint32_t tlsArrayEa(X86& cpu, const char* api) {
    requireMem(cpu, api);
    if (cpu.fs_base == 0) {
        shimAbort("kernel32", api, "fs_base(TEB) 为 0 —— guest 进程结构还没建立");
    }
    const std::uint32_t array_ea = cpu.mem->rd32(cpu.fs_base + kTebTlsArrayOff);
    if (array_ea == 0) {
        shimAbort("kernel32", api, "TEB+0x2C 的 TLS 数组指针为 0（setupProcessStructures 没写它）");
    }
    return array_ea;
}

// 返回位图 DWORD 的 guest 地址（首次使用时写入魔数并把位图清零）
std::uint32_t tlsBitmapEa(X86& cpu, std::uint32_t array_ea, const char* api) {
    const std::uint32_t magic_ea = array_ea + kTlsBookOff;
    if (cpu.mem->rd32(magic_ea) != kTlsBookMagic) {
        cpu.mem->wr32(magic_ea, kTlsBookMagic);
        cpu.mem->wr32(magic_ea + 4, 0);
    }
    (void)api;
    return magic_ea + 4;
}

bool tlsSlotOk(std::uint32_t slot) { return slot < kTlsSlots; }

// ---- 错误码 ----
void shimGetLastError(X86& cpu, ShimCall& call) { call.result = cpu.shim.last_error; }

void shimSetLastError(X86& cpu, ShimCall& call) { cpu.shim.last_error = call.u32(0); }

// ---- TLS（Fls* 也走这里）----
void shimTlsAlloc(X86& cpu, ShimCall& call) {
    const std::uint32_t array_ea = tlsArrayEa(cpu, "TlsAlloc");
    const std::uint32_t bits_ea = tlsBitmapEa(cpu, array_ea, "TlsAlloc");
    const std::uint32_t bits = cpu.mem->rd32(bits_ea);
    for (std::uint32_t slot = 1; slot < kTlsSlots; ++slot) {
        if ((bits & (1u << slot)) == 0) {
            cpu.mem->wr32(bits_ea, bits | (1u << slot));
            cpu.mem->wr32(array_ea + 4u * slot, 0);  // 新槽的值必须是 0（TlsGetValue 的初值）
            call.result = slot;
            return;
        }
    }
    cpu.shim.last_error = kErrNoSystemResources;
    shimLog("kernel32", "TlsAlloc", "%u 个槽全被占用（槽 0 保留）⇒ 返回 TLS_OUT_OF_INDEXES",
            kTlsSlots);
    call.result = kTlsOutOfIndexes;
}

void shimTlsFree(X86& cpu, ShimCall& call) {
    const std::uint32_t slot = call.u32(0);
    const std::uint32_t array_ea = tlsArrayEa(cpu, "TlsFree");
    const std::uint32_t bits_ea = tlsBitmapEa(cpu, array_ea, "TlsFree");
    const std::uint32_t bits = cpu.mem->rd32(bits_ea);
    if (!tlsSlotOk(slot) || slot == 0 || (bits & (1u << slot)) == 0) {
        cpu.shim.last_error = kErrInvalidParameter;
        shimLog("kernel32", "TlsFree", "槽 %u 不是 TlsAlloc 分配的（槽 0 保留，未分配槽也失败）",
                slot);
        call.result = 0;
        return;
    }
    cpu.mem->wr32(bits_ea, bits & ~(1u << slot));
    cpu.mem->wr32(array_ea + 4u * slot, 0);
    call.result = 1;
}

void shimTlsGetValue(X86& cpu, ShimCall& call) {
    const std::uint32_t slot = call.u32(0);
    const std::uint32_t array_ea = tlsArrayEa(cpu, "TlsGetValue");
    if (!tlsSlotOk(slot)) {
        cpu.shim.last_error = kErrInvalidParameter;
        shimLog("kernel32", "TlsGetValue", "槽 %u 越界（只有 0..%u）", slot, kTlsSlots - 1);
        call.result = 0;
        return;
    }
    // Windows 在成功时把 last error 清成 ERROR_SUCCESS（调用方靠它区分"值就是 0"与失败）。
    cpu.shim.last_error = 0;
    call.result = cpu.mem->rd32(array_ea + 4u * slot);
}

void shimTlsSetValue(X86& cpu, ShimCall& call) {
    const std::uint32_t slot = call.u32(0);
    const std::uint32_t value = call.u32(1);
    const std::uint32_t array_ea = tlsArrayEa(cpu, "TlsSetValue");
    if (!tlsSlotOk(slot)) {
        cpu.shim.last_error = kErrInvalidParameter;
        shimLog("kernel32", "TlsSetValue", "槽 %u 越界（只有 0..%u）", slot, kTlsSlots - 1);
        call.result = 0;
        return;
    }
    cpu.mem->wr32(array_ea + 4u * slot, value);
    cpu.shim.last_error = 0;
    call.result = 1;
}

// ---- Fls*：与 TLS 同实现（文件头偏差 4）----
void shimFlsAlloc(X86& cpu, ShimCall& call) { shimTlsAlloc(cpu, call); }

void shimFlsFree(X86& cpu, ShimCall& call) { shimTlsFree(cpu, call); }

void shimFlsGetValue(X86& cpu, ShimCall& call) { shimTlsGetValue(cpu, call); }

void shimFlsSetValue(X86& cpu, ShimCall& call) { shimTlsSetValue(cpu, call); }

// ---- 指针混淆：恒等（文件头偏差 1）----
void shimIdentity(X86& cpu, ShimCall& call) {
    (void)cpu;
    call.result = call.u32(0);
}

// ---- 进程 / 线程标识 ----
void shimGetCurrentThreadId(X86& cpu, ShimCall& call) { call.result = cpu.shim.thread_id; }

void shimGetCurrentProcessId(X86& cpu, ShimCall& call) { call.result = cpu.shim.process_id; }

void shimGetCurrentProcess(X86& cpu, ShimCall& call) { call.result = cpu.shim.process_handle; }

void shimGetCurrentThread(X86& cpu, ShimCall& call) { call.result = cpu.shim.thread_handle; }

// ---- Interlocked*（32 位；单线程 guest ⇒ 普通读改写）----
std::uint32_t interlockedEa(X86& cpu, ShimCall& call, const char* api) {
    (void)cpu;  // 地址校验不需要 cpu；读写在调用方用 cpu.mem 做
    const std::uint32_t ea = call.u32(0);
    if (ea == 0) {
        shimAbort("kernel32", api, "目标地址为 NULL");
    }
    if (ea > 0xFFFFFFFCu) {
        shimAbort("kernel32", api, "目标地址 0x%08X 的 4 字节会越过 4 GiB guest 空间", ea);
    }
    return ea;
}

void shimInterlockedIncrement(X86& cpu, ShimCall& call) {
    const std::uint32_t ea = interlockedEa(cpu, call, "InterlockedIncrement");
    const std::uint32_t v = cpu.mem->rd32(ea) + 1u;  // 新值 == 旧的 +1（按 32 位回绕）
    cpu.mem->wr32(ea, v);
    call.result = v;  // Win32：返回**新值**
}

void shimInterlockedDecrement(X86& cpu, ShimCall& call) {
    const std::uint32_t ea = interlockedEa(cpu, call, "InterlockedDecrement");
    const std::uint32_t v = cpu.mem->rd32(ea) - 1u;
    cpu.mem->wr32(ea, v);
    call.result = v;  // Win32：返回**新值**
}

void shimInterlockedExchange(X86& cpu, ShimCall& call) {
    const std::uint32_t ea = interlockedEa(cpu, call, "InterlockedExchange");
    const std::uint32_t old = cpu.mem->rd32(ea);
    cpu.mem->wr32(ea, call.u32(1));
    call.result = old;  // Win32：返回**旧值**
}

void shimInterlockedExchangeAdd(X86& cpu, ShimCall& call) {
    const std::uint32_t ea = interlockedEa(cpu, call, "InterlockedExchangeAdd");
    const std::uint32_t old = cpu.mem->rd32(ea);
    cpu.mem->wr32(ea, old + call.u32(1));
    call.result = old;  // Win32：返回**旧值**
}

void shimInterlockedCompareExchange(X86& cpu, ShimCall& call) {
    const std::uint32_t ea = interlockedEa(cpu, call, "InterlockedCompareExchange");
    const std::uint32_t exchange = call.u32(1);
    const std::uint32_t comparand = call.u32(2);
    const std::uint32_t old = cpu.mem->rd32(ea);
    if (old == comparand) {
        cpu.mem->wr32(ea, exchange);
    }
    call.result = old;  // Win32：返回**旧值**
}

}  // namespace

void registerErrorTlsShims(ShimRegistry& shims) {
    // ---- 错误码 ----
    shims.add("kernel32", "GetLastError", Conv::Stdcall, 0, shimGetLastError);
    shims.add("kernel32", "SetLastError", Conv::Stdcall, 1, shimSetLastError);

    // ---- TLS ----
    shims.add("kernel32", "TlsAlloc", Conv::Stdcall, 0, shimTlsAlloc);
    shims.add("kernel32", "TlsFree", Conv::Stdcall, 1, shimTlsFree);
    shims.add("kernel32", "TlsGetValue", Conv::Stdcall, 1, shimTlsGetValue);
    shims.add("kernel32", "TlsSetValue", Conv::Stdcall, 2, shimTlsSetValue);

    // ---- Fls*：与 TLS 同实现（FlsAlloc 多一个回调参数，丢弃；见文件头偏差 4）----
    shims.add("kernel32", "FlsAlloc", Conv::Stdcall, 1, shimFlsAlloc);
    shims.add("kernel32", "FlsFree", Conv::Stdcall, 1, shimFlsFree);
    shims.add("kernel32", "FlsGetValue", Conv::Stdcall, 1, shimFlsGetValue);
    shims.add("kernel32", "FlsSetValue", Conv::Stdcall, 2, shimFlsSetValue);

    // ---- 指针混淆：恒等（见文件头偏差 1）----
    shims.add("kernel32", "EncodePointer", Conv::Stdcall, 1, shimIdentity);
    shims.add("kernel32", "DecodePointer", Conv::Stdcall, 1, shimIdentity);
    shims.add("kernel32", "EncodeSystemPointer", Conv::Stdcall, 1, shimIdentity);
    shims.add("kernel32", "DecodeSystemPointer", Conv::Stdcall, 1, shimIdentity);

    // ---- 进程 / 线程 ----
    shims.add("kernel32", "GetCurrentThreadId", Conv::Stdcall, 0, shimGetCurrentThreadId);
    shims.add("kernel32", "GetCurrentProcessId", Conv::Stdcall, 0, shimGetCurrentProcessId);
    shims.add("kernel32", "GetCurrentProcess", Conv::Stdcall, 0, shimGetCurrentProcess);
    shims.add("kernel32", "GetCurrentThread", Conv::Stdcall, 0, shimGetCurrentThread);

    // ---- Interlocked*（32 位；返回语义按 Win32）----
    shims.add("kernel32", "InterlockedIncrement", Conv::Stdcall, 1, shimInterlockedIncrement);
    shims.add("kernel32", "InterlockedDecrement", Conv::Stdcall, 1, shimInterlockedDecrement);
    shims.add("kernel32", "InterlockedExchange", Conv::Stdcall, 2, shimInterlockedExchange);
    shims.add("kernel32", "InterlockedExchangeAdd", Conv::Stdcall, 2, shimInterlockedExchangeAdd);
    shims.add("kernel32", "InterlockedCompareExchange", Conv::Stdcall, 3,
              shimInterlockedCompareExchange);
}

}  // namespace lift
