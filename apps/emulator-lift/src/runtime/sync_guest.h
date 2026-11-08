// emulator-lift · 以**指定 guest 线程身份**操作临界区（给方案 D 的音频泵用）
//
// ★ 为什么需要单独一层：`CRITICAL_SECTION` 的 guest 可见字段（RecursionCount / OwningThread）
//   由 `kernel32_sync.cpp` 维护，布局常量也在那里。音频泵要"以 feeder 线程身份"进入某个槽的
//   CS，但**不能**让主线程的 `EnterCriticalSection` shim 看到 owner=feeder 就 abort —— 泵在
//   返回 dispatcher 之前一定会 Leave，所以主线程看到的 owner 仍是 0。
//   ⇒ 这里只暴露"按 owner 身份 Enter/Leave / 查 owner"三件事，布局仍只有一份。
#pragma once

#include <cstdint>
#include <string>

namespace lift {

struct X86;

// 返回 false + err：`ea` 未提交 / 已被**别的**线程持有 / RecursionCount 形态坏。
bool guestEnterCriticalSectionAs(X86& cpu, std::uint32_t ea, std::uint32_t owner_thread, std::string* err);
// 非阻塞版（真线程 feeder 用）：拿不到 ⇒ false，**不阻塞**、不写 guest 字段。
// ★ 为什么 feeder 要用它：feeder 持令牌时不能阻塞在 CS 上（主线程可能握着该 CS 的另一侧，
//   而它要令牌才能往下走 ⇒ 死锁）。拿不到 ⇒ 放开令牌重试（见 audio.cpp feederEvent）。
bool guestTryEnterCriticalSectionAs(X86& cpu, std::uint32_t ea, std::uint32_t owner_thread, std::string* err);
bool guestLeaveCriticalSectionAs(X86& cpu, std::uint32_t ea, std::uint32_t owner_thread, std::string* err);
std::uint32_t guestCriticalSectionOwner(X86& cpu, std::uint32_t ea);

}  // namespace lift
