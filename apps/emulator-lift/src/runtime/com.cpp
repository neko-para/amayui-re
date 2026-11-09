// emulator-lift · COM 对象 / vtable / refcount 的实现（G2）
//
// ★ 读 com.h 的头部注释先：布局（obj+0 vtable / obj+4 refcount）、argc 约定（不含 this）、
//   未填槽 = loud trap、释放规则（到 0 才 free）。
//
// ★ 一句话机制：`createObject` 在 guest 堆上建 [obj, vtable]，把每个槽写成**桩页里的唯一地址**
//   （`ShimRegistry::addRuntime` + `BlockCache::installShim`），于是 guest 的
//   `call dword ptr [eax+off]` 与 `call [IAT]` 在 dispatcher 眼里是同一件事。
//   `comTrampoline` 是这些槽共用的 ShimFn：靠 `call.def->stub_ea` 从 `methods_` 里找出真正的实现。
//
// ★ 确定性：对象 / vtable 的地址来自 guest 堆（首次适配 + 合并），同一串操作 ⇒ 同一串地址
//   （判据 A4 的前提）。⛔ 不用宿主指针、不用名字哈希做键。
#include "runtime/com.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "runtime/audio.h"
#include "runtime/exec.h"
#include "runtime/heap.h"
#include "runtime/memory.h"
#include "runtime/log.h"
#include "runtime/x86.h"

namespace lift {
namespace {

// 一个 iface 的 vtable 最多允许多少槽（偏移 0..1023*4）。
// ★ 存在的意义是抓"把字节偏移当槽号"这类笔误：0x400 已经远超 DDraw/Surface 的方法数。
constexpr std::uint32_t kMaxVtableSlots = 1024u;

// 对象头：`obj+0` = vtable EA，`obj+4` = refcount
constexpr std::uint32_t kObjectBytes = 8u;
constexpr std::uint32_t kRefcountOffset = 4u;
// ★ 追加式扩展（P1 DDraw）：`obj+8` = 调用方的 guest 扩展块 EA。只有 non-zero 时对象才是 12 字节。
constexpr std::uint32_t kUserOffset = 8u;
constexpr std::uint32_t kObjectBytesWithUser = 12u;

void comLog(const char* iface, const char* fmt, ...) {
    char prefix[256];
    std::snprintf(prefix, sizeof(prefix), "[shim] com!%s: ", iface ? iface : "?");
    va_list ap;
    va_start(ap, fmt);
    logV(LogLevel::Info, LogCom, prefix, fmt, ap);
    va_end(ap);
}

}  // namespace

std::string ComRegistry::qualifiedName(const std::string& iface, const char* name) {
    const std::string short_name = name ? name : "";
    const std::string prefix = iface + "::";
    // 调用方已经写成 `<iface>::<m>` 就不重复加前缀（两种写法都接受）
    if (short_name.compare(0, prefix.size(), prefix) == 0) return short_name;
    return prefix + short_name;
}

std::uint32_t ComRegistry::registerSlot(X86& cpu, const std::string& iface, const MethodEntry& want,
                                       bool has_method) {
    if (!cpu.shims) {
        comLog(iface.c_str(), "createObject 需要 cpu.shims（ShimRegistry）来发唯一 stub 地址");
        return 0;
    }
    // ★ 未实现的槽：thunk 直接用 `comUnsupported`（不必先路由一次）；argc 未知时按 0
    //   （trap 之后机器就停了，栈不需要平衡）。
    const bool trap = !has_method || !want.fn;
    ShimFn thunk = trap ? &comUnsupported : &comTrampoline;
    // ★ argc：`this` 是第 0 个栈参数 ⇒ 登记 argc + 1（COM 的 this 由被调方一起弹）
    const int registered_argc = has_method ? want.argc + 1 : 0;
    ShimDef* def = cpu.shims->addRuntime("com", want.name.c_str(), Conv::Stdcall, registered_argc, thunk);
    if (!def || def->stub_ea == 0) {
        comLog(iface.c_str(), "拿不到唯一 stub 地址（槽位耗尽？）⇒ 槽 +0x%X 失败", want.offset);
        return 0;
    }
    const std::uint32_t stub = def->stub_ea;
    const std::uint32_t def_id = def->id;  // ⚠ 指针在 addRuntime 之后可能失效 ⇒ 立刻把 id 取出来

    // ★ 同一个 (iface, 方法) 的第二个对象会**复用**同一个 stub（addRuntime 幂等）
    //   ⇒ 这里必须确认"同一 stub 绑的是同一份实现"，否则两个对象的调用会串线。
    const auto it = methods_.find(stub);
    if (it != methods_.end()) {
        const MethodEntry& e = it->second;
        if (e.fn != want.fn || e.argc != want.argc || e.offset != want.offset || e.iface != iface) {
            comLog(iface.c_str(),
                   "stub 0x%08X 已被 %s（+0x%X，argc=%d）占用，这次要求 +0x%X ⇒ 拒绝（两个对象会串线）",
                   stub, e.name.c_str(), e.offset, e.argc, want.offset);
            return 0;
        }
    } else {
        MethodEntry e = want;
        e.stub = stub;
        methods_.emplace(stub, std::move(e));
    }

    // ★ 必须装进块缓存：不装的话 guest `call [vtbl+off]` 会去**解码桩页字节**而不是走 shim。
    if (!cpu.cache) {
        comLog(iface.c_str(), "createObject 需要 cpu.cache（BlockCache）来把 stub 装成 shim 块");
        return 0;
    }
    cpu.cache->installShim(stub, def_id);
    // ★ 方案 C：feeder 的私有 BlockCache 也要有这条 vtable stub（它会在自己的线程里 call 它）。
    if (cpu.audio) cpu.audio->installFeederShim(stub, def_id);
    return stub;
}

std::uint32_t ComRegistry::createObject(X86& cpu, const char* iface, const std::vector<ComMethod>& methods,
                                       std::uint32_t user, ComOnZero on_zero, std::uint32_t owner) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const std::string face = iface ? iface : "";
    if (face.empty()) {
        comLog("?", "createObject 的 iface 为空（日志 / 注册名都要靠它）");
        return 0;
    }
    if (methods.empty()) {
        comLog(face.c_str(), "createObject 没有给任何方法 ⇒ 不建对象（空 vtable 让 guest 读到野指针）");
        return 0;
    }
    if (!cpu.mem) {
        comLog(face.c_str(), "createObject 需要 cpu.mem（guest 内存）");
        return 0;
    }
    if (!cpu.heap || !cpu.heap->ready()) {
        // ★ bare 夹具（Machine::initBare）不会初始化 guest 堆 ⇒ 必须先 `m.heap().init(...)`
        comLog(face.c_str(), "createObject 需要就绪的 guest 堆（X86::heap；initBare 夹具要自己 init）");
        return 0;
    }

    // ① 校验 offsets：对齐 / 重复 / 名字重复 / 上限
    std::uint32_t max_offset = 0;
    for (std::size_t i = 0; i < methods.size(); ++i) {
        const ComMethod& m = methods[i];
        if ((m.offset & 3u) != 0) {
            comLog(face.c_str(), "方法 \"%s\" 的 offset 0x%X 不是 4 的倍数（vtable 槽按 DWORD 排）",
                   m.name ? m.name : "?", m.offset);
            return 0;
        }
        if (m.offset > max_offset) max_offset = m.offset;
        for (std::size_t j = 0; j < i; ++j) {
            if (methods[j].offset == m.offset) {
                comLog(face.c_str(), "两个方法都写在 offset +0x%X（\"%s\" / \"%s\"）⇒ 拒绝",
                       m.offset, methods[j].name ? methods[j].name : "?", m.name ? m.name : "?");
                return 0;
            }
            if (methods[j].name && m.name && qualifiedName(face, methods[j].name) == qualifiedName(face, m.name)) {
                // 名字是 addRuntime 的键：同名不同 offset 会让两个槽指到同一条 stub（静默串线）
                comLog(face.c_str(), "两个方法同名 \"%s\"（offset +0x%X / +0x%X）⇒ 拒绝",
                       m.name, methods[j].offset, m.offset);
                return 0;
            }
        }
    }
    const std::uint32_t slots = max_offset / 4u + 1u;
    if (slots > kMaxVtableSlots) {
        comLog(face.c_str(), "vtable 需要 %u 个槽（最大偏移 +0x%X）> 上限 %u ⇒ 拒绝（偏移写错？）",
               slots, max_offset, kMaxVtableSlots);
        return 0;
    }

    // ② 分配 vtable（guest 堆；先用临时数组算好每个槽的 stub，再一次性写进去）
    const std::uint32_t vtable_bytes = slots * 4u;
    const std::uint32_t vtable = cpu.heap->alloc(vtable_bytes, /*zero=*/true);
    if (vtable == 0) {
        comLog(face.c_str(), "guest 堆分配 vtable（%u 字节）失败：%s", vtable_bytes, cpu.heap->lastError());
        return 0;
    }
    std::vector<std::uint32_t> slot_ea(slots, 0);
    for (std::uint32_t slot = 0; slot < slots; ++slot) {
        const std::uint32_t offset = slot * 4u;
        const ComMethod* m = nullptr;
        for (const ComMethod& cand : methods) {
            if (cand.offset == offset) {
                m = &cand;
                break;
            }
        }
        MethodEntry want;
        want.iface = face;
        want.offset = offset;
        if (m) {
            want.conv = m->conv;
            want.argc = m->argc;
            want.fn = m->fn;
        }
        if (m && m->fn) {
            want.name = qualifiedName(face, m->name);
        } else {
            // ★ 未实现的槽的注册名带上偏移：trap 只有这一处能可靠地打出 iface + 偏移
            //   （`com!<iface>::<label>+0xNN`；没声明过的方法 label 就是 unsupported）
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%s::%s+0x%X", face.c_str(),
                          m ? (m->name ? m->name : "?") : "unsupported", offset);
            want.name = buf;
            want.fn = nullptr;
        }
        const bool has_method = m != nullptr;
        const std::uint32_t stub = registerSlot(cpu, face, want, has_method);
        if (stub == 0) {
            cpu.heap->free(vtable);  // 半成品不留在 guest 堆里
            return 0;
        }
        slot_ea[slot] = stub;
    }
    for (std::uint32_t slot = 0; slot < slots; ++slot) {
        cpu.mem->wr32(vtable + slot * 4u, slot_ea[slot]);
    }

    // ③ 分配对象头（[0] = vtable，[4] = refcount = 1；挂了扩展块再加 [8] = user）
    const std::uint32_t obj_bytes = user != 0 ? kObjectBytesWithUser : kObjectBytes;
    const std::uint32_t obj = cpu.heap->alloc(obj_bytes, /*zero=*/true);
    if (obj == 0) {
        comLog(face.c_str(), "guest 堆分配对象（%u 字节）失败：%s", obj_bytes, cpu.heap->lastError());
        cpu.heap->free(vtable);
        return 0;
    }
    cpu.mem->wr32(obj, vtable);
    cpu.mem->wr32(obj + kRefcountOffset, 1u);
    if (user != 0) cpu.mem->wr32(obj + kUserOffset, user);

    Object o;
    o.vtable = vtable;
    o.slots = slots;
    o.refcount = 1;
    o.bytes = obj_bytes;
    o.user = user;
    o.owner = owner;
    // ★ 代际（宿主表内，⛔ 不进 guest 内存）：建对象时固化 owner 的**当前**代际。
    //   `generationOf(owner) == 0` ⇒ 建 surface 时父对象已经不在表里（调用方传了个死 EA）。
    //   ⛔ 不静默：那是调用点的实现错误，之后 `SetRenderTarget` 的代际判据会拒绝绑定。
    o.owner_generation = owner != 0 ? generationOf(owner) : 0u;
    if (owner != 0 && o.owner_generation == 0) {
        comLog(face.c_str(),
               "createObject 的 owner 0x%08X 不在对象表里（已释放 / 野指针）⇒ 这个对象没有可校验的父代际",
               owner);
    }
    o.generation = next_generation_++;  // 只增不复用（0 保留给"没有代际"）
    o.on_zero = on_zero;
    o.iface = face;
    const std::uint32_t generation = o.generation;
    objects_.emplace(obj, std::move(o));
    comLog(face.c_str(), "建对象 0x%08X（vtable 0x%08X，%u 槽，refcount=1%s%s；代际 %u）", obj, vtable, slots,
           user != 0 ? "，带扩展块" : "", owner != 0 ? "，带父对象" : "", generation);
    return obj;
}

std::uint32_t ComRegistry::userOf(std::uint32_t obj) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const auto it = objects_.find(obj);
    return it == objects_.end() ? 0u : it->second.user;
}

std::uint32_t ComRegistry::ownerOf(std::uint32_t obj) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const auto it = objects_.find(obj);
    return it == objects_.end() ? 0u : it->second.owner;
}

std::uint32_t ComRegistry::generationOf(std::uint32_t obj) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const auto it = objects_.find(obj);
    return it == objects_.end() ? 0u : it->second.generation;
}

std::uint32_t ComRegistry::ownerGenerationOf(std::uint32_t obj) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const auto it = objects_.find(obj);
    return it == objects_.end() ? 0u : it->second.owner_generation;
}

bool ComRegistry::addRef(X86& cpu, std::uint32_t obj) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const auto it = objects_.find(obj);
    if (it == objects_.end()) {
        comLog("?", "addRef(0x%08X)：不是 ComRegistry 建出来的对象（野指针？）⇒ 返回 false", obj);
        return false;
    }
    ++it->second.refcount;
    if (cpu.mem) cpu.mem->wr32(obj + kRefcountOffset, it->second.refcount);
    return true;
}

bool ComRegistry::release(X86& cpu, std::uint32_t obj) {
    // ⚠ 这个重载**不可校验**：它只拿得到 EA ⇒ 陈旧 EA（guest 的 use-after-release，真机同样 UB）
    //   与"Release 当前占有该 EA 的新对象"无法区分。宿主侧缓存请用 `releaseIf`。
    return releaseImpl(cpu, obj, /*expect_generation=*/0u);
}

bool ComRegistry::releaseIf(X86& cpu, std::uint32_t obj, std::uint32_t generation) {
    // `generation == 0` ⇒ 调用方没缓存代际 ⇒ 退化成不可校验的 `release`（见 com.h 的说明）。
    return releaseImpl(cpu, obj, generation);
}

bool ComRegistry::releaseImpl(X86& cpu, std::uint32_t obj, std::uint32_t expect_generation) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const auto it = objects_.find(obj);
    if (it == objects_.end()) {
        // ★ 重复 release / 未知对象**必须出声**：静默 false 会让"释放两次"看起来像"正常释放"。
        comLog("?", "release(0x%08X)：不是 ComRegistry 建出来的对象（重复 release / 野指针）⇒ 返回 false",
               obj);
        return false;
    }
    // ★ 代际校验（与"减引用"在同一把锁里 ⇒ feeder 线程不能在这两步之间让 EA 换人）：
    //   不符 ⇒ 这是一次**陈旧持有者的释放**，当前占有该 EA 的是**别人** ⇒ ⛔ 一个引用都不许减。
    if (expect_generation != 0 && it->second.generation != expect_generation) {
        comLog(it->second.iface.c_str(),
               "releaseIf(0x%08X)：缓存代际 %u ≠ 当前 %u（EA 已被复用，当前占有者是 %s）"
               "⇒ 拒绝释放（新对象一个引用都不减）",
               obj, expect_generation, it->second.generation, it->second.iface.c_str());
        return false;
    }
    if (it->second.refcount == 0) {
        // 到 0 时我们就删表了 ⇒ 这里只可能是宿主把镜像改坏了；不静默
        comLog(it->second.iface.c_str(), "release(0x%08X)：refcount 已经是 0（对象表没删干净？）", obj);
        return false;
    }
    --it->second.refcount;
    if (cpu.mem) cpu.mem->wr32(obj + kRefcountOffset, it->second.refcount);
    if (it->second.refcount != 0) return true;

    // ④ 到 0：**先跑析构钩子**（回收对象独占的子资源：纹理 = 状态块 + arena 像素块），
    //    再释放 vtable + 对象（guest 堆），并从表里删掉。
    //    ★ 顺序保持"子资源在前、对象头在后"（旧代码也是这个顺序）。
    //    ★ 这里是唯一收敛点 ⇒ 内部裸 `cpu.com->release`（SetTexture 换绑 / 设备销毁 /
    //      Sprite 批次）、带代际校验的 `releaseIf`、与 guest 走 iface Release 的路径
    //      在这一步之后**没有任何区别**。
    const std::uint32_t vtable = it->second.vtable;
    const std::uint32_t slots = it->second.slots;
    const std::uint32_t obj_bytes = it->second.bytes;
    const std::uint32_t user = it->second.user;
    const ComOnZero on_zero = it->second.on_zero;
    const std::string iface = it->second.iface;
    objects_.erase(it);
    // 钩子在对象表删除**之后**跑：它只该回收子资源，⛔ 不许再按 this 回调到本对象。
    if (on_zero) on_zero(cpu, user);
    if (!cpu.heap) {
        comLog(iface.c_str(), "release(0x%08X)：cpu.heap 为空 ⇒ vtable 0x%08X 与对象都泄漏", obj, vtable);
        return false;
    }
    // 先把对象头抹掉：残留的 guest 指针再被解引用时能看出"已经不是对象了"
    if (cpu.mem) {
        cpu.mem->wr32(obj, 0);
        cpu.mem->wr32(obj + kRefcountOffset, 0);
        if (obj_bytes > kObjectBytes) cpu.mem->wr32(obj + kUserOffset, 0);
    }
    const bool ok_vtable = cpu.heap->free(vtable);
    const bool ok_obj = cpu.heap->free(obj);
    if (!ok_vtable || !ok_obj) {
        comLog(iface.c_str(), "release(0x%08X)：释放失败（vtable 0x%08X %s / 对象 %s）—— guest 堆报错见上一行",
               obj, vtable, ok_vtable ? "ok" : "FAILED", ok_obj ? "ok" : "FAILED");
        return false;
    }
    comLog(iface.c_str(), "release(0x%08X)：refcount 到 0 ⇒ 已释放 vtable 0x%08X（%u 槽）与对象", obj,
           vtable, slots);
    return true;
}

bool ComRegistry::find(std::uint32_t obj) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return objects_.find(obj) != objects_.end();
}

std::size_t ComRegistry::objectCount() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return objects_.size();
}

std::size_t ComRegistry::methodCount() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return methods_.size();
}

std::uint32_t ComRegistry::refCount(std::uint32_t obj) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const auto it = objects_.find(obj);
    return it == objects_.end() ? 0u : it->second.refcount;
}

std::uint32_t ComRegistry::vtableOf(std::uint32_t obj) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const auto it = objects_.find(obj);
    return it == objects_.end() ? 0u : it->second.vtable;
}

const char* ComRegistry::ifaceOf(std::uint32_t obj) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const auto it = objects_.find(obj);
    return it == objects_.end() ? nullptr : it->second.iface.c_str();
}

void ComRegistry::dispatch(X86& cpu, ShimCall& call) {
    const std::uint32_t stub = call.def ? call.def->stub_ea : 0;
    ShimFn fn = nullptr;
    std::string iface;
    std::string name;
    std::uint32_t offset = 0;
    bool known_this = false;
    bool missing = false;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        const auto it = methods_.find(stub);
        if (it == methods_.end()) {
            missing = true;
        } else {
            const MethodEntry& e = it->second;
            fn = e.fn;
            iface = e.iface;
            name = e.name;
            offset = e.offset;
            known_this = objects_.find(call.u32(0)) != objects_.end();
        }
    }
    if (missing) {
        // ⛔ 不静默：能走到这里说明 vtable 里有一条我们不认识的地址（或 registry 不是同一个实例）
        std::fprintf(stderr,
                     "\n=== COM thunk 不在路由表里 ===\n"
                     "  stub 0x%08X（%s!%s）\n"
                     "  this=0x%08X\n"
                     "  原因：这条 stub 不是本 ComRegistry 发的（或 registry 与 ShimRegistry 不是一对）\n",
                     stub, call.def ? call.def->module.c_str() : "?", call.def ? call.def->name.c_str() : "?",
                     call.u32(0));
        dumpRegisters(cpu, stderr);
        dumpGuestStack(cpu, stderr, 24);
        call.result = 0;
        cpu.running = false;
        cpu.stop_why = "COM thunk 不在路由表里";
        // ★ 装配错 ⇒ 非正常结局（已注册的 thunk 但路由表对不上）：归 `Trap`，⛔ 不是 Stopped。
        cpu.stop_kind = HostStopKind::Trap;
        ++cpu.shim.unimplemented_calls;
        return;
    }
    if (!fn) {
        // 未实现的槽：走到这里就是 loud trap（见下面的 comUnsupported）
        comUnsupported(cpu, call);
        return;
    }
    // 诊断（不改变行为）：`this` 应该是我们建出来的对象 —— vtable 只可能从 createObject 流出去。
    if (!known_this) {
        comLog(iface.c_str(),
               "%s(+0x%X) 的 this=0x%08X 不是 ComRegistry 建出来的对象 —— 仍转发（可能是 guest 自己造的 vtable）",
               name.c_str(), offset, call.u32(0));
    }
    // ★ fn 在锁外执行：主线程的 D3D/DDraw 方法可能很长，feeder 的 DS Lock/Unlock 不能排在它后面。
    fn(cpu, call);
}

// 方法槽共用的入口：路由到真正的实现（见 com.h 的说明）
void comTrampoline(X86& cpu, ShimCall& call) {
    if (!cpu.com) {
        // 装配错误 ⇒ 响亮停下（⛔ 不能"返回 0 当作方法失败了"）
        std::fprintf(stderr,
                     "\n=== COM thunk 被调到，但 cpu.com 为空 ===\n"
                     "  stub 0x%08X（%s!%s）\n"
                     "  原因：装配方没把 ComRegistry 接到 X86::com 上（见 com.h）\n",
                     call.def ? call.def->stub_ea : 0, call.def ? call.def->module.c_str() : "?",
                     call.def ? call.def->name.c_str() : "?");
        call.result = 0;
        cpu.running = false;
        cpu.stop_why = "cpu.com 为空";
        // ★ 装配错 ⇒ 非正常结局：归 `Trap`（与 thunk 不在路由表同一类）。
        cpu.stop_kind = HostStopKind::Trap;
        return;
    }
    cpu.com->dispatch(cpu, call);
}

// 未实现的 vtable 槽：打印 iface + 偏移 + 现场后停机器。⛔ 不返回 0 假装"方法返回 FALSE"。
void comUnsupported(X86& cpu, ShimCall& call) {
    const std::uint32_t stub = call.def ? call.def->stub_ea : 0;
    // iface / 偏移从注册名里取（`com!<iface>::<label>+0xNN`）—— 即使 `this` 不是我们的对象也能打对
    const std::string name = call.def ? call.def->name : "?";
    std::string iface = "?";
    std::uint32_t offset = 0;
    const std::size_t plus = name.rfind("+0x");
    const std::size_t sep = name.find("::");
    if (plus != std::string::npos) {
        offset = static_cast<std::uint32_t>(std::strtoul(name.c_str() + plus + 3, nullptr, 16));
    }
    if (sep != std::string::npos) {
        iface = name.substr(0, sep);
    } else if (cpu.com) {
        const char* face = cpu.com->ifaceOf(call.u32(0));
        if (face) iface = face;
    }
    std::fprintf(stderr,
                 "\n=== unimplemented COM vtable slot ===\n"
                 "  %s  +0x%X（%s，stub 0x%08X，this=0x%08X）\n"
                 "  reason: 这个 iface 的这个偏移我们还没实现 ⇒ loud trap（⛔ 不静默返回 0）\n",
                 iface.c_str(), offset, name.c_str(), stub, call.u32(0));
    dumpRegisters(cpu, stderr);
    dumpGuestStack(cpu, stderr, 24);
    call.result = 0;
    cpu.running = false;
    cpu.stop_why = "unimplemented COM vtable slot";
    // ★ 已注册的槽（thunk 的 fn 就是本函数）⇒ 非正常结局：归 `Trap`。
    cpu.stop_kind = HostStopKind::Trap;
    ++cpu.shim.unimplemented_calls;
}

}  // namespace lift
