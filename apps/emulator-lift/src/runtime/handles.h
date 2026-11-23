// emulator-lift · guest 句柄表
//
// ★ guest 看到的是**不透明整数**（不是宿主指针）；宿主侧把整数映射到自己的对象。
// ★ 判据：**无效句柄必须响亮失败**（打印 + 停止），⛔ 不许静默返回 0 / 成功 ——
//   这是 S8 变异清单里"句柄校验"那一条要能被机械检出的前提。
// ★ 值是**确定性的**（0x100 起，4 字节步进）⇒ trace 可复现。
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace lift {

enum class HandleKind : std::uint8_t {
    None = 0, Event, Mutex, Semaphore, CriticalSection, File, Find, Thread, Timer, Module, Other,
    // ★ P2：GDI 对象（DC / 字体 / 位图 / AddFontMemResourceEx 的资源句柄）也走同一张表。
    GdiDc, GdiFont, GdiBitmap, GdiFontMem,
    // ★ P4：GDI 区域（`Create*Rgn` 的 HRGN；payload = `runtime/region.h` 的 `GdiRegion`）。
    //   `DeleteObject` 认它（见 gdi32_object.cpp）。
    GdiRegion,
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
//   对象在**最后一个**句柄关闭时销毁"。
// ★ 实现：`add()` 每见到一个非空 payload 就把它在 `payload_refs_` 里的计数 +1；本函数据计数判
//   "还有没有别的活句柄"（计数 1 = 最后一个 ⇒ `destroy`；>1 ⇒ 只减一、⛔ 不销毁）。
//   ⛔ 不许各处直接 delete payload（会 double-free）；⛔ 减一**只能**发生在本函数里 ——
//   `entry->payload` 在这里被置空，若放进 `close()` 就永远看到 nullptr ⇒ 计数只增不减（泄漏，
//   而且下一个同地址的新对象会被误判成"共享"）。
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
    // ★ 清空表 + 两份派生索引（⛔ 不许分叉：索引里留着已关闭的值 = `DeleteObject` 白扫死槽）。
    void clear();

    // 活 DC 的句柄值（`kind == GdiDc` 且未关闭）。`DeleteObject` 问"这个对象还被某个 DC
    // 选中吗"时**只**遍历这一份，⛔ 不再按句柄值扫全表（槽位只增不减 ⇒ 那是 O(累计槽数)）。
    // ★ 维护点只有三处：`add`（DC ⇒ push）、`close`（DC ⇒ swap-pop）、`clear`。⛔ 别在别处改。
    // ★ 索引只回答"**哪些 DC 还活着**"，**不**回答"它的 payload 有效"：DC 可以先 `releasePayload`
    //   （payload 置空）再 `close` ⇒ 索引里会出现一条 `payload == nullptr` 的活 DC。
    //   ⇒ `selectedAnywhere` 仍然要求 `kind == GdiDc && payload != nullptr`（它本来就要 `get()` 一层
    //   防御）；⛔ 别把"索引里的每一条都必然有 payload"当成不变式。
    const std::vector<std::uint32_t>& liveDcs() const { return live_dcs_; }

    // 活**文件**句柄的值（`kind == File` 且未关闭）。共享冲突判定（`kernel32_file.cpp` 的
    // `shareViolation`）**只**遍历这一份，⛔ 不再按句柄值扫全表（槽位只增不减 ⇒ 那是 O(累计槽数)；
    // 实测 SAVE11 一趟累计 5334 槽，每次 `CreateFileA` 白扫 ~10 µs）。
    // ★ 维护点与 `live_dcs_` 同一套：`add`（File ⇒ push）、`close`（File ⇒ swap-pop）、`clear`。
    // ★ 判据不变：索引只回答"**哪些文件句柄还活着**"，`shareViolation` 仍旧自己 `get()` +
    //   校验 `kind == File && payload != nullptr`（索引里可能出现"先 releasePayload 再 close"的条目）。
    const std::vector<std::uint32_t>& liveFiles() const { return live_files_; }

    // ---- 标准句柄三元组（`GetStdHandle` / `SetStdHandle` 的**进程级表**）----
    // ★ 文档：`SetStdHandle` 改的是进程级表，之后 `GetStdHandle` 要返回它；`GetFileType` /
    //   `GetConsoleMode` 要按句柄的真实种类回答（REQ-01M6K69K9HM3ETCDNMJWPHGXJC）。
    // ★ 默认值是本模拟器的三个**假句柄**（0x0C / 0x10 / 0x14）：CRT 只用它们算 `isatty` /
    //   `GetFileType`，不真的读写；⛔ 它们**不占**句柄表的槽（`CloseHandle` 对它们会按
    //   "无效句柄"响亮失败 —— 与真机对伪标准句柄的行为一致）。
    // ★ 为什么挂在句柄表上而不是静态变量：**一台机器一份** ⇒ 同一个测试进程里两个 Fixture
    //   不会互相污染（`ShimState` 在 `x86.h`，本轮的文件清单不动它）。
    static constexpr int kStdSlots = 3;  // 0 = STD_INPUT / 1 = STD_OUTPUT / 2 = STD_ERROR
    static constexpr std::uint32_t kStdDefault[kStdSlots] = {0x0000000Cu, 0x00000010u, 0x00000014u};

    std::uint32_t stdHandle(int slot) const {
        return (slot >= 0 && slot < kStdSlots) ? std_handles_[slot] : 0u;
    }
    bool setStdHandle(int slot, std::uint32_t h) {
        if (slot < 0 || slot >= kStdSlots) return false;
        std_handles_[slot] = h;
        return true;
    }
    // h 命中某个标准槽的**当前值** ⇒ 槽号；否则 -1
    int stdSlotOf(std::uint32_t h) const {
        for (int i = 0; i < kStdSlots; ++i) {
            if (std_handles_[i] == h) return i;
        }
        return -1;
    }
    void resetStdHandles() {
        for (int i = 0; i < kStdSlots; ++i) std_handles_[i] = kStdDefault[i];
    }

    // 无效句柄的诊断（由 shim 调用；打印 kind/值 + 已分配的句柄范围后停止）
    [[noreturn]] void invalidHandle(std::uint32_t h, const char* api) const;

private:
    // ★ 引用计数的**唯一**读改写点就是 `releasePayload`（free function）⇒ 只给它开一个口子。
    friend bool releasePayload(HandleTable& table, std::uint32_t h);
    struct Slot {
        std::uint32_t value = 0;
        HandleEntry entry;
    };
    std::vector<Slot> entries_;
    // ★ B：活 DC 的句柄值（派生索引；与"`entries_` 里 kind==GdiDc 且 used"的集合恒等）。
    std::vector<std::uint32_t> live_dcs_;
    std::vector<std::uint32_t> live_files_;  // 活文件句柄（口径见上面的 `liveFiles()`）
    // ★ C：payload → 指向它的**活句柄数**（只在 `add` 里 +1、`releasePayload` 里 -1）。
    //   ⛔ 这是实现细节：任何断言都不许读它 / 断言它的值。语义判据是"关一个不销毁、
    //   最后一个才销毁"（`tests/sync_ext.cpp` 的 duplicate-handle + `tests/mechanism.cpp`
    //   的 handle-table-payload-refcount）。
    std::unordered_map<void*, std::uint32_t> payload_refs_;
    std::uint32_t std_handles_[kStdSlots] = {kStdDefault[0], kStdDefault[1], kStdDefault[2]};
};

}  // namespace lift
