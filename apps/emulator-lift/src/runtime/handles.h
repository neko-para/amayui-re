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
    void (*destroy)(void*) = nullptr;  // CloseHandle / 族内 close 回收 payload 用（空 = 不管）
    std::uint32_t aux = 0;  // 计数 / 标志
    // ★ `Open*` 授予的**访问位**（EVENT_MODIFY_STATE / SYNCHRONIZE …）。`Create*` 记 ALL_ACCESS。
    //   修改类 API（SetEvent / ReleaseSemaphore / ReleaseMutex）按文档要求的那一位检查。
    std::uint32_t access = 0;
    // ★ 可继承标志（`bInheritHandle`）。本模拟器**不创建子进程**（CreateProcess 是显式 stub）
    //   ⇒ 这一位只被记录、目前没有可观察后果；记下来是为了"复制句柄时标志也一起复制"这条合同。
    bool inherit = false;
    bool used = false;
};

// 关闭一条句柄项时的**唯一**回收点（定义见 handles.cpp）。
// ★ Win32 的合同："重复的句柄（`DuplicateHandle`）/ 同名对象（`Open*`）指向**同一个对象**，
//   对象在**最后一个**句柄关闭时销毁"。⇒ 这里不数引用计数，而是问一句"还有别的活句柄
//   指着同一个 payload 吗"：没有才调 `destroy`。⛔ 不许各处直接 delete payload（会 double-free）。
// 返回 true = 找到了这条句柄并断开（是否销毁见上面）。
class HandleTable;
bool releasePayload(HandleTable& table, std::uint32_t h);

class HandleTable {
public:
    static constexpr std::uint32_t kFirst = 0x100u;
    static constexpr std::uint32_t kStep = 4u;

    std::uint32_t add(HandleKind kind, const char* name, void* payload, std::uint32_t aux = 0);
    HandleEntry* get(std::uint32_t h);
    const HandleEntry* get(std::uint32_t h) const;
    bool valid(std::uint32_t h) const { return get(h) != nullptr; }
    bool close(std::uint32_t h);
    // 按"种类 + 名字"找第一个活着的句柄（命名内核对象用；**大小写敏感**，见 kernel32_sync.cpp）。
    // 返回句柄值；0 = 没找到（句柄值从 0x100 起 ⇒ 0 不可能是有效句柄）。
    std::uint32_t findNamed(HandleKind kind, const std::string& name) const;
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
