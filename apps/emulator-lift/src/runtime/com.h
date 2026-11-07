// emulator-lift · guest 可见的 COM 对象 / vtable / refcount（G2；plan §2.4 G2 / §6 P0）
//
// ★ 为什么需要这一层：引擎按 `(*(…)(obj + off))(...)` 调 DDraw / Surface 的方法 ——
//   它在 guest 侧**读一张 vtable** 再间接调用。IAT 桩页只解决"导入调不到"，
//   解决不了"对象方法调不到"：必须真的在 guest 内存里造出 [vtable 指针 + refcount] 的对象。
//
// ★ 布局（都在 `X86::heap`（guest 堆）上分配，guest 地址空间里可见）：
//     obj + 0 : vtable 的 guest EA
//     obj + 4 : refcount（DWORD）
//     vtable  : slot_count 个 DWORD；每个是**桩页里的一条唯一地址**（`ShimRegistry::addRuntime`）
//   ⇒ guest 的 `call dword ptr [eax+off]` 落到 dispatcher → shim 块 → `comTrampoline`
//     → 按 `call.def->stub_ea` 路由到真正的实现。
//
// ★ `argc` 约定：`ComMethod::argc` **不含 `this`**；登记给 ShimRegistry 时用 `argc + 1`
//   （stdcall 由被调方弹，`invoke` 按 argc*4 结算）。于是方法 shim 里
//   `call.u32(0)` = `this`，`call.u32(1..argc)` = 声明的参数。
//
// ★ 未填的 vtable 槽 = **loud trap**（`comUnsupported`：打印 iface + 偏移 + 寄存器 / guest 栈，
//   然后 `cpu.running = false`）。⛔ 绝不静默返回 0 —— 那会把"方法没实现"伪装成"方法返回 0/FALSE"。
//
// ★ refcount：宿主侧 `objects_` 里的镜像是**唯一权威**，每次变化同步写回 guest `obj+4`
//   （guest 看到同一份值；我们的 AddRef / Release shim 都通过本类改它）。
//   ⚠ 如果 guest 自己写 `obj+4`（本仓没有这种代码），宿主镜像不会跟着变。
//
// ★ 释放：`release` 到 0 ⇒ `heap->free(vtable)` + `heap->free(obj)` 并从对象表里删掉。
//   重复 release / 未知对象 ⇒ 打一行 + 返回 false（⛔ 不静默）。
//
// ★ 本类**不**放进 `Machine`：`cpu.com` 由装配方（主代理）指向一台机器持有的实例；
//   纯档夹具手动设 `m.cpu().com = &com`。⚠ 一台 Machine 配一个 ComRegistry ——
//   方法 thunk 的 stub EA 是 `ShimRegistry` 全局的，两台 ComRegistry 共享同一批 stub 会找不到路由。
//
// ★ 线程 / 重入：没有锁（本模拟器是单线程解释器，SCOPE §1）。
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "runtime/shims.h"

namespace lift {

struct X86;
struct ShimCall;

// 一个 vtable 槽的声明（**不含** IUnknown 的自动实现：要哪些槽就写哪些槽）。
struct ComMethod {
    std::uint32_t offset = 0;  // vtable 字节偏移（必须 4 字节对齐）
    const char* name = nullptr;  // 方法名（**在同一个 iface 内**唯一；登记成 `com!<iface>::<name>`）
    Conv conv = Conv::Stdcall;
    int argc = 0;              // ⚠ **不含 this**；登记时用 argc + 1
    ShimFn fn = nullptr;       // nullptr ⇒ 这个槽也是 loud trap（等同"没填"）
};

// 所有 COM 方法槽共用的 shim 入口（写进 `ShimDef::fn`）：按 `call.def->stub_ea` 路由。
// ★ 为什么只能这样：方法数 / 名字是**运行期**定的，而 `ShimFn` 是不带上下文的函数指针
//   ⇒ 不能"每个方法一个静态函数"；共用一条入口 + 注册表路由是唯一不做代码生成的写法。
void comTrampoline(X86& cpu, ShimCall& call);

// 未实现的 vtable 槽的 loud trap（同样是一条 ShimFn）。打印 iface + 偏移后停机器。
void comUnsupported(X86& cpu, ShimCall& call);

class ComRegistry {
public:
    // 在 guest 内存里建对象：[0] = vtable EA，[4] = refcount(1)。返回对象 guest EA；失败 ⇒ 0 + 一行日志。
    // ★ 每个 method 通过 `ShimRegistry::addRuntime("com", iface::name, conv, argc+1, comTrampoline)`
    //   拿唯一 stub EA，写进 vtable；并**替调用方**做 `cpu.cache->installShim(stub_ea, id)`
    //   （否则 guest 的 `call [vtbl+off]` 会去解码桩页字节，而不是走 shim）。
    // ★ 没填的槽（0..max_offset 之间没有 ComMethod 的偏移）写 `comUnsupported`（loud trap）。
    // ★ 依赖：`cpu.shims` / `cpu.heap`（ready）/ `cpu.cache` 都得在，且 ShimRegistry 已经有桩页
    //   （`Machine::prepare()` 的 bind() 或夹具的 `configureStubPage`）。
    //
    // ★ `user`（**追加式扩展**，P1 DDraw 起的）：对象在 guest 里多挂一个 DWORD `obj+8`，
    //   放调用方自己的 **guest 扩展块 EA**（DDraw 的 surface 用它放 arena EA / 色键 / caps）。
    //   `user == 0` ⇒ 对象仍是 P0 的 8 字节布局（⛔ 既有语义一字不改）；非 0 ⇒ 12 字节。
    //   为什么要有它：对象的宿主侧状态若放在 ComRegistry 之外，就会撞上"一台机器一份 + 生命周期"
    //   的问题（测试里 Machine 反复建在同一栈地址）；放进 guest 块则由对象的 refcount 生命周期管。
    std::uint32_t createObject(X86& cpu, const char* iface, const std::vector<ComMethod>& methods,
                               std::uint32_t user = 0);

    // 对象的扩展块 EA（**宿主编表里的权威值**，不读 guest 内存）；未知对象 / 没挂扩展 ⇒ 0。
    std::uint32_t userOf(std::uint32_t obj) const;

    // refcount +1 / -1；未知对象 ⇒ false + 一行日志。
    bool addRef(X86& cpu, std::uint32_t obj);
    // 到 0 ⇒ 释放 vtable 与对象（guest 堆）并从对象表里删掉。未知对象 / 重复 release ⇒ false + 日志。
    bool release(X86& cpu, std::uint32_t obj);
    // 这个 guest EA 是我们建出来的对象吗（不再看内存里写的是什么）
    bool find(std::uint32_t obj) const;

    // ---- 运行期路由（comTrampoline 用；也允许夹具直接调）----
    void dispatch(X86& cpu, ShimCall& call);

    // ---- 查询（诊断 / 夹具）----
    std::uint32_t refCount(std::uint32_t obj) const;  // 未知对象 ⇒ 0
    std::uint32_t vtableOf(std::uint32_t obj) const;
    const char* ifaceOf(std::uint32_t obj) const;  // 未知对象 ⇒ nullptr
    std::size_t objectCount() const;
    std::size_t methodCount() const;

private:
    struct Object {
        std::uint32_t vtable = 0;
        std::uint32_t slots = 0;     // vtable 里的槽数（释放时要知道大小）
        std::uint32_t refcount = 0;  // 权威值（同步写 obj+4）
        std::uint32_t bytes = 8;     // 对象头大小（8 = P0 布局；12 = 挂了扩展块 EA）
        std::uint32_t user = 0;      // 扩展块 EA（obj+8；0 = 没有）
        std::string iface;
    };
    struct MethodEntry {
        std::string iface;
        std::string name;          // `com!` 后面的完整名字（`<iface>::<m>` / `<iface>::unsupported+0xNN`）
        std::uint32_t stub = 0;    // 桩页里的唯一地址（路由键）
        std::uint32_t offset = 0;  // vtable 字节偏移
        Conv conv = Conv::Stdcall;
        int argc = 0;              // **不含 this**
        ShimFn fn = nullptr;       // nullptr ⇒ 未实现的槽（loud trap）
    };

    // 给一个槽登记 thunk（幂等：同一 (iface, 方法) 的第二、第三个对象复用同一个 stub）。
    // 返回 stub EA；0 = 失败（已经打过日志）。
    std::uint32_t registerSlot(X86& cpu, const std::string& iface, const MethodEntry& want, bool has_method);

    // iface + 方法名 → 注册名（`<iface>::<name>`；已经带前缀就不重复加）
    static std::string qualifiedName(const std::string& iface, const char* name);

    std::unordered_map<std::uint32_t, Object> objects_;        // key = obj guest EA
    std::unordered_map<std::uint32_t, MethodEntry> methods_;   // key = stub EA（桩页地址）
    // ★ 方案 C：feeder 线程与主线程都会查/建 COM 对象（主线程建 SE、feeder 查 buffer 状态）。
    //   宿主表必须真互斥；dispatch 里会重入 userOf/release ⇒ recursive。
    mutable std::recursive_mutex mutex_;
};

}  // namespace lift
