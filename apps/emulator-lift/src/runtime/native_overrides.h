// emulator-lift · guest 函数级 native override（试点原型）
//
// ★ 口径（REQ-01M6NZSAAD0WCNBE3RKMFF84R5）：
//   - 只在 **dispatcher 的 block 入口**按 EA 拦截（不 patch guest 字节 ⇒ 天然避开 SMC / 代码页保护）；
//   - native 实现只能经 `GuestMemory` 读写 guest；⛔ 宿主指针不进 guest 状态；
//   - 返回后由本框架按 ABI 设置 `EAX` / `ESP` / `EIP`，未覆盖路径照旧走解释器；
//   - 每一项都**默认关闭**，由 `--native-overrides=<list>` 显式启用。
// ★ 这里只放"机制 + 少量 CRT 叶子"；候选由 profile 选出，不先猜（见 requirement 的测试计划）。
#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace lift {

struct X86;
class GuestMemory;

enum class GuestConv : std::uint8_t {
    Cdecl = 0,    // 调用方清栈
    Stdcall = 1,  // 被调用方清栈
    Thiscall = 2, // ECX = this；栈参数由被调用方清（MSVC x86）
};

const char* guestConvName(GuestConv c);

// native 实现：读参数（cpu.arg(i) / cpu.ecx()），经 mem 访存，写 *result（= 返回 EAX）。
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
};

class NativeOverrideTable {
public:
    static constexpr std::size_t kMax = 8;

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
    bool invoke(NativeOverride& ov, X86& cpu, GuestMemory& mem, const char** why);

private:
    std::array<NativeOverride, kMax> entries_{};
    std::size_t count_ = 0;
};

// 已知的 CRT 叶子（按名字解析 EA；EA 来自 `.lst` 的 `proc near` / `COLLAPSED FUNCTION` 行）。
struct KnownNative {
    const char* name;
    GuestConv conv;
    std::uint32_t arg_bytes;
    NativeFn fn;
};
const KnownNative* findKnownNative(const std::string& name);
std::size_t knownNativeCount();
const KnownNative& knownNativeAt(std::size_t i);

// 具体实现（也直接给测试用）
bool nativeMemcpy(X86& cpu, GuestMemory& mem, std::uint32_t* result);
bool nativeMemset(X86& cpu, GuestMemory& mem, std::uint32_t* result);
// `sub_401000` 的语义是**符号**（`.lst` 401066: `sar eax,1Fh; or eax,1` ⇒ -1/0/+1），
// 不是字节差。`_memcmp` 的 CRT 实现是 COLLAPSED（没有可观察字节）⇒ 暂不覆盖。
bool nativeSignMemcmp(X86& cpu, GuestMemory& mem, std::uint32_t* result);
bool nativeStrlen(X86& cpu, GuestMemory& mem, std::uint32_t* result);

}  // namespace lift
