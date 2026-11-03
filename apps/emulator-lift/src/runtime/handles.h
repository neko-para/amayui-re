// emulator-lift · guest 句柄表
//
// ★ guest 看到的是**不透明整数**（不是宿主指针）；宿主侧把整数映射到自己的对象。
// ★ 判据：**无效句柄必须响亮失败**（打印 + 停止），⛔ 不许静默返回 0 / 成功 ——
//   这是 S8 变异清单里"句柄校验"那一条要能被机械检出的前提。
// ★ 值是**确定性的**（0x100 起，4 字节步进）⇒ trace 可复现。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lift {

enum class HandleKind : std::uint8_t {
    None = 0, Event, Mutex, Semaphore, CriticalSection, File, Find, Thread, Timer, Module, Other,
};

const char* handleKindName(HandleKind kind);

struct HandleEntry {
    HandleKind kind = HandleKind::None;
    std::string name;      // 诊断 / trace 用（可空）
    void* payload = nullptr;
    std::uint32_t aux = 0;  // 计数 / 标志
    bool used = false;
};

class HandleTable {
public:
    static constexpr std::uint32_t kFirst = 0x100u;
    static constexpr std::uint32_t kStep = 4u;

    std::uint32_t add(HandleKind kind, const char* name, void* payload, std::uint32_t aux = 0);
    HandleEntry* get(std::uint32_t h);
    const HandleEntry* get(std::uint32_t h) const;
    bool valid(std::uint32_t h) const { return get(h) != nullptr; }
    bool close(std::uint32_t h);
    std::size_t size() const { return entries_.size(); }
    void clear() { entries_.clear(); }

    // 无效句柄的诊断（由 shim 调用；打印 kind/值 + 已分配的句柄范围后停止）
    [[noreturn]] void invalidHandle(std::uint32_t h, const char* api) const;

private:
    struct Slot {
        std::uint32_t value = 0;
        HandleEntry entry;
    };
    std::vector<Slot> entries_;
};

}  // namespace lift
