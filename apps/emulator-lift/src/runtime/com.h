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
// ★ 释放：`release` 到 0 ⇒ **先跑 `onZero` 钩子**（回收对象独占的子资源：纹理状态块 + arena 像素），
//   再 `heap->free(vtable)` + `heap->free(obj)` 并从对象表里删掉。这是对象销毁的**唯一收敛点**
//   （内部裸 release 与 iface Release shim 没有区别）。
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

// refcount 到 0 时的**析构钩子**（在 `ComRegistry::release` 的唯一收敛点上调用）。
//   `user` = createObject 时登记的扩展块 EA（obj+8；0 = 没有）。
// ★ 为什么要有它：`Release` 只说明"引用到 0"，但对象**拥有**的 guest 资源（纹理状态块 +
//   arena 像素块）必须在这时回收。以前这一步挂在"调用者是否走 iface 的 Release shim"上 ——
//   而内部有多处裸 `cpu.com->release`（SetTexture 换绑 / SetRenderTarget 换绑 /
//   destroyForDevice / Sprite 批次 / dshow 宿主对象）绕过 shim ⇒ **所有权挂在调用点**，
//   漏一处就漏一块像素。⛔ 不在这里写"共 N 处"：那个数会随实现漂（`rg -n "com->release\(" src`
//   才是真源）。
//   钩子把它挪回对象：无论谁把引用降到 0，子资源都跟着回收。
// ★ 钩子只能回收**对象独占**的资源。`IDirect3DSurface9` 的 `user` 常指向**别人**的状态块
//   （`GetSurfaceLevel` 复用纹理状态块）⇒ 那种对象**不登记**钩子（⛔ 会提前释放纹理像素）。
//   ★ 那它的寿命怎么保？靠**引用**而不是钩子：surface 建的时候就替父纹理 `addRef`，到 0 时由
//   `owns_owner`（见 `createObject`）成对还回去（`REQ-01M6ZZZ6PEVSFB93C6VJPKC04C`）。
using ComOnZero = void (*)(X86& cpu, std::uint32_t user);

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
    // ★ `owner`（宿主侧**父对象**的 guest EA；⛔ 不写进 guest 内存、不进对象布局）：
    //   "这个对象代表谁"。目前只有一种用法：`IDirect3DSurface9`（`GetSurfaceLevel` 建的）记下
    //   **父纹理对象**。为什么需要它：surface 的 `user` 指向纹理状态块（`'TEX1'`），但 surface
    //   **不拥有**那块像素（见文件头：surface 不登记 on_zero 钩子）；像素块 + 状态块的寿命挂在
    //   **纹理对象**的 refcount 上。`SetRenderTarget` 把表面绑成渲染目标 ⇒ 必须对"像素块的 owner"
    //   做语义 addRef/release，而 owner 要在建对象时固化（⛔ 不能解绑时按 `user` 反查：那个 EA
    //   会随对象表复用而指向别人）。
    //   `release` 到 0 **不会**去释放 owner（这不是所有权转移，只是"我代表谁"）。
    //   ★ 例外只有一处、且必须显式开口：`owns_owner`（见下）。
    //
    // ★ `owns_owner`（宿主侧的**引用所有权**；默认 false ⇒ 上面那条"不释放 owner"不变）：
    //   true 表示"这个对象替 `owner` 持一份引用"，到 0 时**还回去**（`releaseIf(owner, owner_generation)`）。
    //   为什么需要它：`IDirect3DSurface9`（`GetSurfaceLevel` 建的）的 `user` 指向**父纹理**的状态块，
    //   像素块 + 状态块只由父纹理 refcount 到 0 时的 `on_zero` 回收（`destroyTextureState`）。
    //   surface 不持父纹理 ⇒ guest 合法地 `Release(纹理)` 之后 surface 还活着（读到已 free / 被复用的
    //   状态块）—— `REQ-01M6ZZZ6PEVSFB93C6VJPKC04C`。所以建 surface 时对父纹理 addRef、到 0 时成对释放。
    //   ★ 为什么是 `releaseIf`（带代际）而不是裸 `release`：`owner` 是宿主表里**不持引用时**唯一的
    //     EA 缓存；持引用期间它钉得住，但 guest **越界 Release**（把父纹理减到 0）之后 EA 会被下一个
    //     对象拿走 ⇒ 裸 `release` 会减到**当前占有者**身上。代际在建对象时固化 ⇒ 不符就只出声、不减。
    //   ★ 归还走的是 `releaseImpl` —— 与 guest `Release` / 内部裸 `cpu.com->release` **同一条**收敛点，
    //     ⛔ 不是另写的一条影子回收（否则内部裸 release 又会漏掉这条路）。
    //
    // ★ **代际**（`generation`：宿主表内单调自增、非 0；REQ-01M6X4NFHCW1Y6YDJ4DYFA92PS）：
    //   对象表的键是 guest EA，而 guest 堆会**复用** EA —— `create A → release A（到 0 删表 + 归还堆）
    //   → create B` 之后 B 常常拿到 A 的 EA（首次适配，确定性）。于是"对旧 EA 再 Release"与
    //   "Release 新对象"在宿主看来**完全一样**：Release shim 只拿得到 `this` 这个 EA。
    //   ⇒ 代际**不能**用于 guest 发的 Release（那条路径不可校验，见 `release`）；它只服务
    //   **宿主侧缓存**：谁缓存了一个 EA，就把它的代际一起缓存，释放时走 `releaseIf(EA, 代际)`。
    //   ★ 代际只在宿主表里：⛔ 不写进 guest 内存、不改对象布局、不改 guest ABI。
    //   ★ 与 `GpuRasterTexture::generation`（`gpu_raster.h:207`，"序列号只增不复用"）是同一个理由。
    std::uint32_t createObject(X86& cpu, const char* iface, const std::vector<ComMethod>& methods,
                               std::uint32_t user = 0, ComOnZero on_zero = nullptr,
                               std::uint32_t owner = 0, bool owns_owner = false);

    // 对象的扩展块 EA（**宿主编表里的权威值**，不读 guest 内存）；未知对象 / 没挂扩展 ⇒ 0。
    std::uint32_t userOf(std::uint32_t obj) const;

    // 对象的父对象 EA（`createObject` 的 `owner`；0 = 没有 / 未知对象）。**不判活** ——
    // 调用方要用它做释放，必须自己保证"绑定期间持有它的一份引用"（见 `d3d9_pipeline.cpp`）。
    // ★ `owns_owner`（见 `createObject`）就是"对象自己替 owner 持一份"的那一档：纹理 surface 走它。
    std::uint32_t ownerOf(std::uint32_t obj) const;

    // 对象的**代际**（宿主表内单调自增；未知对象 ⇒ 0）。见 `createObject` 的说明。
    // ★ 这是"宿主侧缓存 (EA, 代际)"里的那一半：把代际和 EA 一起存下来，释放时走 `releaseIf`。
    std::uint32_t generationOf(std::uint32_t obj) const;

    // `owner` 在**建这个对象那一刻**的代际（0 = 没有 owner，或建的时候 owner 不在表里）。
    // ★ 用途 = 让 `Object::owner` 这个 EA 缓存在**持引用期间**也能被校验：纹理 surface 现在替父纹理
    //   持一份引用（`owns_owner`，见 `createObject`）⇒ 正常情况下 owner 死不了；但 guest **越界
    //   Release** 之后 owner 的 EA 会被下一个对象拿走 ⇒ 归还 / 绑定时按代际判"这还是不是那个对象"。
    //   （`d3d9_pipeline.cpp` 的 `releaseIf(owner, ownerGenerationOf(surface))` 与本类的
    //   `owns_owner` 归还走的是同一条判据。）
    std::uint32_t ownerGenerationOf(std::uint32_t obj) const;

    // refcount +1 / -1；未知对象 ⇒ false + 一行日志。
    bool addRef(X86& cpu, std::uint32_t obj);
    // 到 0 ⇒ **先跑 on_zero 钩子**（回收对象独占的子资源），再释放 vtable 与对象（guest 堆）
    // 并从对象表里删掉。未知对象 / 重复 release ⇒ false + 日志。
    // ★ 这是对象销毁的**唯一收敛点**：内部裸 release 与 guest 走 iface Release 在这里没有区别。
    // ★ ⚠ **这个重载不可校验**：它只拿得到 EA。guest 自己持旧 EA 发 Release（use-after-release，
    //   真机同样 UB）在这里与"Release 新对象"无法区分 ⇒ 会减到当前占有者身上。宿主侧缓存
    //   请用下面的 `releaseIf`。
    bool release(X86& cpu, std::uint32_t obj);
    // **带代际校验的 release（宿主侧缓存专用）**：只有 `obj` 当前表项的代际 == `generation` 才减引用。
    // 代际不符（EA 已被复用）⇒ 一行 loud 日志 + false，**不碰**当前占有该 EA 的对象。
    // `generation == 0` ⇒ 调用方没有代际 ⇒ 退化成 `release`（只有"从来没缓存过代际"的调用点才该这么用）。
    bool releaseIf(X86& cpu, std::uint32_t obj, std::uint32_t generation);
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
        std::uint32_t owner = 0;     // 父对象 EA（见 createObject 的 `owner`；0 = 没有）
        // ★ 真 = 本对象**替 owner 持一份引用**（见 createObject 的 `owns_owner`）：到 0 时还回去。
        bool owns_owner = false;
        // ★ 下面两个只在宿主表里（⛔ 不进 guest 内存；见 createObject 的代际说明）。
        std::uint32_t generation = 0;        // 本对象的代际（单调自增、非 0）
        std::uint32_t owner_generation = 0;  // 建这个对象时 `owner` 的代际（0 = 没有 owner / 当时就不在表里）
        ComOnZero on_zero = nullptr;  // refcount 到 0 时回收子资源（见文件头；nullptr = 没有子资源）
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

    // `release` / `releaseIf` 的**唯一实现**：`expect_generation == 0` ⇒ 不校验代际（guest 路径）。
    // ★ 代际比较必须与"减引用"在同一把 `mutex_` 里（否则 feeder 线程可以在两步之间让对象换人）。
    bool releaseImpl(X86& cpu, std::uint32_t obj, std::uint32_t expect_generation);

    std::unordered_map<std::uint32_t, Object> objects_;        // key = obj guest EA
    std::unordered_map<std::uint32_t, MethodEntry> methods_;   // key = stub EA（桩页地址）
    // 下一个要发的代际（只增不复用；0 保留给"没有代际"）。
    std::uint32_t next_generation_ = 1;
    // ★ 方案 C：feeder 线程与主线程都会查/建 COM 对象（主线程建 SE、feeder 查 buffer 状态）。
    //   宿主表必须真互斥；dispatch 里会重入 userOf/release ⇒ recursive。
    mutable std::recursive_mutex mutex_;
};

}  // namespace lift
