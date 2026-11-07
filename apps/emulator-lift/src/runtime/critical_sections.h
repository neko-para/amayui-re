// emulator-lift · guest CRITICAL_SECTION ↔ 宿主递归锁（方案 C 的同步边界）
//
// ★ 为什么需要：方案 D 的泵是"主线程协程"，没有并发；真线程要让 guest 的
//   `EnterCriticalSection` 真的**阻塞**另一条 guest 执行流（音频 feeder）。guest 结构里的
//   `RecursionCount` / `OwningThread` 仍是权威账本，但互斥由宿主的 per-EA 递归锁提供。
//   ⛔ 不是"把 guest 字段当锁"——那在并发下没有任何阻塞语义。
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace lift {

struct X86;

class CriticalSectionTable {
public:
    // 建 / 删（guest `InitializeCriticalSection*` / `DeleteCriticalSection`）。
    void ensure(std::uint32_t ea);
    void erase(std::uint32_t ea);

    // `blocking=false` = TryEnter：拿不到锁 ⇒ false（不写 guest 字段）。
    bool enter(X86& cpu, std::uint32_t ea, std::uint32_t thread_id, bool blocking, std::string* err);
    bool leave(X86& cpu, std::uint32_t ea, std::uint32_t thread_id, std::string* err);
    // 主线程停止 / join feeder 前：把**本线程**持有的 guest CS 全部放开，避免 feeder 永久阻塞。
    // ⚠ 只能由持锁的那个宿主线程调用（这里就是主线程）。
    void releaseAll(X86& cpu, std::uint32_t thread_id);

private:
    std::shared_ptr<std::recursive_mutex> lockFor(std::uint32_t ea);
    std::mutex map_mutex_;
    std::unordered_map<std::uint32_t, std::shared_ptr<std::recursive_mutex>> locks_;
};

}  // namespace lift
