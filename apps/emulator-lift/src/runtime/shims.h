// emulator-lift · shim 框架（每个导入一个**唯一 guest 地址**）
//
// ★ 机制（S5）：导入表里每个导入分到桩页里的一个唯一地址（4 字节步进），IAT 槽写这个地址。
//   guest 的 `call dword ptr [IAT]` 于是落到 dispatcher → 块缓存里那个 EA 是 **shim 块** →
//   调用宿主函数，然后按调用约定弹参 + 回到返回地址。
//   ⇒ `call [IAT]` 与 `call 0x401234` 在 dispatcher 眼里是同一件事（README §2.4 的 table[EA]）。
// ★ 调用约定：stdcall 由被调方弹参（`ret N`），cdecl / varargs 由调用方弹（`ret`）——
//   ⛔ 不许"统一按 stdcall 弹"：那会让 cdecl 的栈每调一次涨 4 字节，几百次后炸在别处。
// ★ 未实现的导入**也分到唯一地址**，执行到就打印 module!name + EA + guest 返回栈并停止，
//   同时记进"基座缺口"清单（A5）。⛔ 不许静默 no-op（SCOPE §3）。
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace lift {

struct X86;
struct PeImage;
struct ShimDef;      // 注册项（含 std::string；只在注册 / 查询路径按值出现）
struct ShimCallDef;  // 热路径的 POD 视图（定义在 ShimFn 之后）
class GuestMemory;

enum class Conv : std::uint8_t { Stdcall = 0, Cdecl, Varargs };

const char* convName(Conv c);

// ★ 未实现导入 / 未实现形态的**唯一**响亮停机点（`ShimRegistry::invoke` 的缺口路径与
//   "已注册的 fn 里拒绝某个形态"的条件 trap 共用）：打印 module!name + reason + 返回地址 +
//   寄存器 + guest 栈，置 `running = false` / `stop_why = "unimplemented import"` /
//   `stop_kind = HostStopKind::Trap` / `++unimplemented_calls`。
//   `stub_ea == 0` ⇒ 打印里不写桩地址（条件 trap 不是"未实现的导入桩"）。
//   ★ 条件 trap 走这里时 `had_impl == true` ⇒ `Machine::run` 落 `StopReason::Trap`（⛔ 不是
//     正常结局）；无 fn 的缺口仍由 `!had_impl` 先判成 `Unimplemented`。
void trapUnimplemented(X86& cpu, const char* module, const char* name, const char* reason,
                       std::uint32_t ret_ea, std::uint32_t stub_ea = 0, Conv conv = Conv::Stdcall,
                       int argc = 0);

// 一次 shim 调用的现场。参数在 guest 栈上：`[esp]` = 返回地址，`[esp+4+4i]` = 第 i 个参数。
struct ShimCall {
    X86* cpu = nullptr;
    Conv conv = Conv::Stdcall;
    int argc = 0;                 // 声明的参数个数（仅用于弹参与诊断）
    std::uint32_t result = 0;     // 返回值（EAX）
    std::uint32_t ret_ea = 0;     // 返回地址（dispatcher 在调用前弹出）
    std::uint32_t stdcall_bytes = 0;  // 0 ⇒ argc*4
    // ★ `take_control = true` ⇒ **shim 自己接管下一条 esp / eip**，invoke 不再弹参、不再写 eip
    //   （返回值仍照常写 EAX）。只有"在 shim 里调用 guest 回调"这一种情形需要它：
    //   `EnumSystemLocalesA` 把控制权交给 guest 回调，guest 返回后落到内部续延桩
    //   （见 kernel32_string.cpp）。⛔ 普通实现不要设它 —— 设了就绕过了调用约定。
    bool take_control = false;

    // ★ 正在执行的**这一条登记**（热路径 POD 视图：module / name / stub_ea / id / reason）。
    //   为什么要给 shim 看它：运行期注册的 thunk（`addRuntime` / COM vtable 槽）**共用同一个
    //   ShimFn**，函数指针本身分不出"我是哪个槽"；靠 `def->stub_ea` 回查注册表才能路由。
    //   ⛔ 不要在 shim 里长期保存这个指针：它指向 `invokePrepared` **栈上的 POD 视图**，
    //      fn 返回即失效（与改动前指向栈上 `ShimDef` 拷贝的寿命逐字相同）。
    const ShimCallDef* def = nullptr;

    // 读参数（i 从 0 起）
    std::uint32_t u32(int i) const;
    std::int32_t i32(int i) const { return static_cast<std::int32_t>(u32(i)); }
    std::uint32_t ptr(int i) const { return u32(i); }
    std::uint16_t u16(int i) const;
    // 读 guest C 字符串（NULL ⇒ 空串；越界即 fault，不静默截断）
    std::string str(int i) const;
    // 读 guest UTF-16 字符串
    std::u16string wstr(int i) const;
    // 读 guest 缓冲区（n 字节）
    std::vector<std::uint8_t> bytes(int i, std::size_t n) const;
};

using ShimFn = void (*)(X86& cpu, ShimCall& call);

// ★ 热路径的 **POD 视图**：`prepareLocked` 每次调用只拷下面这些字段（x64 上 48 B），
//   ⛔ 不再按值拷 `ShimDef` 里的两个 `std::string`（旧形状里 `D3DXMatrixMultiply` 的 17 字符名
//   > SSO ⇒ 每调用一次就 malloc / free 一次；其余名字也至少两段 memcpy）。
//   `module` / `name` 指向登记项里的 `std::string::c_str()`：`defs_` 是 `std::deque`
//   （`push_back` 不让已有元素的引用失效）且这两个字段登记后**不再改写** ⇒ 指针稳定。
//   ⛔ 不要把 `defs_` 换回 `std::vector`：`push_back` 扩容会让这里缓存的字符串指针悬垂。
//   ★ 为什么还留 `id` / `stub_ea`：它们与 `ShimDef` 同名同义，读者（trace / trap / COM 路由）
//     一个字都不用改；`reason` 是失败路径才读的静态串。
struct ShimCallDef {
    const char* module = "";       // 稳定（见上）
    const char* name = "";         // 稳定（见上）
    const char* reason = nullptr;  // 未实现 / stub 的理由（只在失败路径读）
    ShimFn fn = nullptr;
    std::uint32_t stub_ea = 0;
    std::uint32_t id = 0;
    Conv conv = Conv::Stdcall;
    int argc = 0;
};

struct ShimDef {
    std::string module;
    std::string name;
    Conv conv = Conv::Stdcall;
    int argc = 0;
    ShimFn fn = nullptr;            // nullptr ⇒ 未实现（执行即响亮失败）
    const char* reason = nullptr;   // 未实现 / stub 的理由（SCOPE 偏差必须点名）
    // 唯一 guest 地址。★ 形状**恒为** `stubPage() + slot*4`（登记期断言；见 checkStubEaInPage）。
    // ⛔ 不要直接写它：索引表会过期 ⇒ 查询退到线性扫描（慢路径，语义仍对但热路径失效）。
    //   正常路径 = bind() / addRuntime() 发号。
    std::uint32_t stub_ea = 0;
    std::uint32_t id = 0;           // 从 0 起的稳定序号（trace 用）
    bool imported = false;          // 这个导入在本镜像的导入表里出现过
    bool declared = false;          // true = host 侧**显式**登记过（add / addUnimplemented）
                                    // false = 导入表里有、但我们连一行登记都没有（bind 现场补的）
    bool called = false;
    std::uint64_t call_count = 0;
    // ★ 诊断（`LIFT_FRAME_TRACE=1` / `--profile`）：宿主实现自身的墙钟累计（ns）。
    //   ⛔ 不是 guest 块 profile 的一部分；默认 0、默认不计时。
    std::uint64_t ns = 0;
};

class ShimRegistry {
public:
    void setTrace(bool on, std::FILE* out) { trace_ = on; trace_out_ = out ? out : stdout; }
    bool trace() const { return trace_; }
    // ★ 诊断：`LIFT_FRAME_TRACE=1` 或 `--profile` 时打开；逐次调用 steady_clock，默认零开销。
    void setTiming(bool on) { timing_ = on; }
    bool timing() const { return timing_; }

    // ---- host 侧声明 ----
    void add(const char* module, const char* name, Conv conv, int argc, ShimFn fn);
    void addUnimplemented(const char* module, const char* name, const char* reason);
    // ★ 有实现、但**按 SCOPE 的偏差整体 stub**（例：AGERC 的三个导出 thunk）。
    //   `fn` 非空（会返回合理值 + 打一行日志），`reason` 非空（覆盖清单里 kind=stub）。
    //   `stub_ea` 在 bind() 里分配（与导入同一个桩页；它不是导入，所以 IAT 里不会出现）。
    void addStub(const char* module, const char* name, Conv conv, int argc, ShimFn fn, const char* reason);

    // ---- 运行期注册（G1 动态导出 / G2 COM vtable 槽）----
    // ★ 与 `add()` 的区别：**在 bind() 之后**才登记 ⇒ 立刻拿到桩页里下一个空闲槽作为 `stub_ea`
    //   （bind() 结束时已把桩页起点 / 槽数 / 下一个空闲槽记下来），不需要再跑一遍 bind()。
    // ★ 登记出来的条目：`declared = true`（host 侧显式登记过）、`imported = false`（IAT 里没有它）、
    //   `reason = nullptr`（不是"未实现的导入"）、`id = defs_.size()`。
    // ★ **幂等**：同 (module, name) 已登记且**已有** stub_ea ⇒ 直接返回那一条（⛔ 绝不覆盖已有 stub EA）。
    //   这条对 COM 是必需的：同一种 iface 建第二个对象时方法名会重复，必须复用同一个 thunk。
    // ★ 槽位耗尽 / 没有桩页（bind 没跑过、也没 configureStubPage）⇒ 打一行到 stderr + 返回 nullptr。
    // ⚠ 返回的 `ShimDef*` 在**下一次 add/addRuntime** 之后仍然有效（`defs_` 现在是 `std::deque`，
    //   `push_back` 不让已有元素引用失效）；但**已有条目会被原地改写**（`conv/argc/fn/stub_ea`），
    //   ⇒ 跨多次注册仍请存 `id`，再用 `byId()` / `byStubEa()` 取（与改动前同一条建议）。
    // ⚠ 拿到 `stub_ea` 之后要 **`cpu.cache->installShim(def->stub_ea, def->id)`**，否则 guest 的
    //   `call [IAT]` / `call eax` 落到这个 EA 时会去**解码桩页字节**而不是走 shim。
    //   （`ComRegistry::createObject` 会自己装；裸 addRuntime 的调用方负责。）
    ShimDef* addRuntime(const char* module, const char* name, Conv conv, int argc, ShimFn fn);

    // 没有 PE 镜像的夹具（`Machine::initBare`）用：bind() 不会跑 ⇒ 先显式给一页桩页。
    // ★ bind() 成功时会用**自己的**（真）桩页值覆盖它；⛔ 已经有 stub 发出去之后再调会被拒绝（只打印）。
    void configureStubPage(std::uint32_t page, std::uint32_t slots);
    std::uint32_t stubPage() const { return shim_page_; }
    std::uint32_t stubSlots() const { return shim_slots_; }
    std::uint32_t nextStubSlot() const { return next_slot_; }

    // ---- 绑定：给镜像里每个导入分唯一地址，并把地址写回 IAT 槽 ----
    // `shim_page` / `shim_slots`：桩页起点与槽数（GuestLayout 提供）
    bool bind(GuestMemory& mem, const PeImage& img, std::uint32_t shim_page, std::uint32_t shim_slots,
              std::string* err);

    // ---- 查询 ----
    const ShimDef* find(const char* module, const char* name) const;
    ShimDef* findMutable(const char* module, const char* name);
    // ★ 桩 EA → 登记项。**O(1)、无锁**（槽位索引表）：AOT 的派发循环每次 shim 派发都走它。
    //   口径与旧实现（对 `defs_` 线性扫描、返回**先登记**的那条、`ea == 0` 恒 nullptr）**逐位相同**；
    //   索引未覆盖（未登记槽 / 页外 EA / 未对齐 EA / 有人直写了 `ShimDef::stub_ea`）时回退到
    //   **与旧实现逐字相同**的线性扫描（在锁内）⇒ 语义不靠"索引一定对"来保证。
    //   ★ 这就是"为什么 `lift_shim` 仍然可以拿它当唯一来源"：索引**不是**第二份真源。
    const ShimDef* byStubEa(std::uint32_t ea) const;
    const ShimDef* byId(std::uint32_t id) const;
    // ★ 方案 C：返回 id 而不是指针（feeder 线程不能被主线程的 addRuntime 扩容弄成悬垂）。
    std::uint32_t idForStub(std::uint32_t ea) const;  // 0 = 没有
    // ★ 诊断（只读计数器，⛔ 不参与任何判定）：`byStubEa` / `idForStub` 走**慢路径**（线性扫描）的次数。
    //   为什么要有它：热路径的"O(1) 且无锁"是**可被证伪的读数**，不是散文 —— 等价性用例拿它当判据
    //   （已登记桩查一遍之后它必须**不动**；索引坏了 / 表过期了它就会涨）。⛔ 不要拿它当性能断言。
    std::uint64_t stubIndexSlowPathHits() const {
        return stub_index_slow_.load(std::memory_order_relaxed);
    }
    void resetStubIndexStats() const { stub_index_slow_.store(0, std::memory_order_relaxed); }
    const std::deque<ShimDef>& all() const { return defs_; }
    // ★ 方案 C：给 feeder 的私有 BlockCache 预装 shim 块时用（拷贝，避免 defs_ 并发扩容）。
    std::vector<ShimDef> snapshot() const;
    std::size_t size() const;

    // ---- 缺口报告（A5）----
    std::vector<const ShimDef*> importedButUnimplemented() const;
    std::vector<const ShimDef*> calledUnimplemented() const;
    std::size_t importedCount() const;

    // ---- 执行（dispatcher 调用）----
    // 走进一个 shim 块：弹返回地址 → 调用实现（或响亮失败）→ 按约定弹参
    void invoke(X86& cpu, ShimDef& def);
    // 按 id 调（内部持锁取 def）——feeder 线程专用；返回 false = id 不存在。
    bool invokeById(X86& cpu, std::uint32_t id);

private:
    // ★ lift-opt #2/#3（2026-11-22）：`invokeById` / `invoke` 的公共下半段。
    //   为什么拆出来：旧形状里 `invokeById` 取一份 def 交给 `invoke`，`invoke` 再取一份
    //   ⇒ 每次 shim 调用付 **两把 `recursive_mutex` + 两次 `ShimDef` 拷贝**（每个 def 两个
    //   `std::string`）。现在"取 def + 记账"在**一次**加锁里完成（`prepareLocked`），
    //   然后把所有权交给这里 ⇒ **一次锁**。
    // ★ 2026-11-23（shim-fast #2）：`prepareLocked` **只拷 POD 字段**（`ShimCallDef`，48 B），
    //   两个 `std::string` 不再按值拷 —— `module` / `name` 改成指向 `defs_`（deque，引用稳定）
    //   里那两个字符串的 `const char*`。记账（`called` / `call_count`）仍在锁内、逐字未改。
    void prepareLocked(const ShimDef& src, ShimCallDef* out, bool* trace_on, bool* timing_on);
    void invokePrepared(X86& cpu, const ShimCallDef& def, bool trace_on, bool timing_on);
    std::deque<ShimDef> defs_;
    bool trace_ = false;
    bool timing_ = false;
    std::FILE* trace_out_ = stdout;
    // bind() 结束时记下来的桩页状态（`addRuntime` 从 next_slot_ 继续往后发地址）。
    // ★ 0 / 0 = 还没配置桩页（= 既没 bind 成功，也没 configureStubPage）⇒ addRuntime 拒绝发地址。
    std::uint32_t shim_page_ = 0;
    std::uint32_t shim_slots_ = 0;
    std::uint32_t next_slot_ = 0;  // 下一个空闲槽（**已发出的槽数**，也从 bind 里继承）
    // =================================================================================
    // ★ 桩槽索引表（S1 性能修，2026-11-15）：`slot → def id + 1`（0 = 空槽）。
    //
    // 为什么是这个形状：
    //   · `stub_ea` **只**由 bind() / addRuntime() 发号，形状恒为 `shim_page_ + slot*4`
    //     （登记期断言 `checkStubEaInPage` 把它从"观察"变成"前提"，违反即响亮停）；
    //   · 于是 `ea → slot` 是一次无符号减 + 一次移位，`slot → 登记项` 是**一次数组取**；
    //   · ⛔ 表里存 **id / 下标，不存 `ShimDef*`**：`defs_` 现在是 `std::deque`（引用稳定），
    //     但"表里不存指针"是既有契约：返回指针一律走 `defByIdUnlocked()`（与 `byId()` **同一条**
    //     解析路径：一次边界检查 + `&defs_[id]`），夹具直写 `stub_ea` 时也靠它重取。
    //   · 表长 = `shim_slots_`，在 configureStubPage / bind 时**定死**，运行期不再扩容 ⇒ 热路径
    //     读它不需要锁（读到旧值也不会错：命中后还要 `d->stub_ea == ea` 二次校验，不成立就走
    //     与旧实现逐字相同的线性扫描）。
    // =================================================================================
    std::vector<std::uint32_t> slot_index_;
    // 在 bind() / configureStubPage() 的**末尾**调（都在锁内）：按 `defs_` 重建整张表。
    void rebuildSlotIndexLocked();
    // 登记期断言（**响亮停**）：`stub_ea == 0 || (stub_ea 在 [page, page+slots*4) 且 4 字节对齐)`。
    //   ★ 两个重载：bind() 里比对的是**参数**（成员 `shim_page_` 要到 bind 末尾才更新），其余地方比成员。
    void checkStubEaInPageLocked(std::uint32_t stub_ea, const char* where) const;
    static void checkStubEaShape(std::uint32_t stub_ea, std::uint32_t page, std::uint32_t slots,
                                 const char* where);
    // 与 `byId()` 同一条解析路径（唯一差别：调用方**已经**持有锁或走的是只读快路径）。
    const ShimDef* defByIdUnlocked(std::uint32_t id) const;
    // ★ 旧实现（线性扫描 + 锁）—— 现在只作**慢路径**：索引未覆盖时兜底，语义与改动前逐字相同。
    const ShimDef* byStubEaLinearLocked(std::uint32_t ea) const;
    // 慢路径命中次数（诊断；见 `stubIndexSlowPathHits()` 的注释）。
    mutable std::atomic<std::uint64_t> stub_index_slow_{0};
    // ★ 方案 C：feeder 线程也会 invoke shim（Lock/Unlock/Stop/COM trampoline），defs_ 可能被
    //   主线程 addRuntime 扩容 ⇒ 所有注册/查询/invoke 都走这把递归锁（invoke 期间不允许扩容）。
    //   ⚠ 唯一例外是 `byStubEa` 的**快路径**（只读 `slot_index_` + `defs_`，不拿锁）：
    //     它读的表在 bind 之后不再重新分配，且命中后还有 `stub_ea` 二次校验 ⇒ 不依赖锁来保证正确性。
    mutable std::recursive_mutex mutex_;
};

}  // namespace lift
