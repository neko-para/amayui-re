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

#include "runtime/exec.h"
#include "runtime/heap.h"
#include "runtime/memory.h"
#include "runtime/x86.h"

namespace lift {
namespace {

// 一个 iface 的 vtable 最多允许多少槽（偏移 0..1023*4）。
// ★ 存在的意义是抓"把字节偏移当槽号"这类笔误：0x400 已经远超 DDraw/Surface 的方法数。
constexpr std::uint32_t kMaxVtableSlots = 1024u;

// 对象头：`obj+0` = vtable EA，`obj+4` = refcount
constexpr std::uint32_t kObjectBytes = 8u;
constexpr std::uint32_t kRefcountOffset = 4u;

void comLog(const char* iface, const char* fmt, ...) {
    // 形状与 `shimLog` 一致（`[shim] <module>!<name>: ...`），但 runtime/ 不依赖 shims/ 目录
    // ⇒ 这里直接落 stderr。⛔ 不要改成静默返回。
    std::fprintf(stderr, "[shim] com!%s: ", iface ? iface : "?");
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
    std::fflush(stderr);
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
    return stub;
}

std::uint32_t ComRegistry::createObject(X86& cpu, const char* iface, const std::vector<ComMethod>& methods) {
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

    // ③ 分配对象头（[0] = vtable，[4] = refcount = 1）
    const std::uint32_t obj = cpu.heap->alloc(kObjectBytes, /*zero=*/true);
    if (obj == 0) {
        comLog(face.c_str(), "guest 堆分配对象（%u 字节）失败：%s", kObjectBytes, cpu.heap->lastError());
        cpu.heap->free(vtable);
        return 0;
    }
    cpu.mem->wr32(obj, vtable);
    cpu.mem->wr32(obj + kRefcountOffset, 1u);

    Object o;
    o.vtable = vtable;
    o.slots = slots;
    o.refcount = 1;
    o.iface = face;
    objects_.emplace(obj, std::move(o));
    comLog(face.c_str(), "建对象 0x%08X（vtable 0x%08X，%u 槽，refcount=1）", obj, vtable, slots);
    return obj;
}

bool ComRegistry::addRef(X86& cpu, std::uint32_t obj) {
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
    const auto it = objects_.find(obj);
    if (it == objects_.end()) {
        // ★ 重复 release / 未知对象**必须出声**：静默 false 会让"释放两次"看起来像"正常释放"。
        comLog("?", "release(0x%08X)：不是 ComRegistry 建出来的对象（重复 release / 野指针）⇒ 返回 false",
               obj);
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

    // ④ 到 0：释放 vtable + 对象（guest 堆），并从表里删掉
    const std::uint32_t vtable = it->second.vtable;
    const std::uint32_t slots = it->second.slots;
    const std::string iface = it->second.iface;
    objects_.erase(it);
    if (!cpu.heap) {
        comLog(iface.c_str(), "release(0x%08X)：cpu.heap 为空 ⇒ vtable 0x%08X 与对象都泄漏", obj, vtable);
        return false;
    }
    // 先把对象头抹掉：残留的 guest 指针再被解引用时能看出"已经不是对象了"
    if (cpu.mem) {
        cpu.mem->wr32(obj, 0);
        cpu.mem->wr32(obj + kRefcountOffset, 0);
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

bool ComRegistry::find(std::uint32_t obj) const { return objects_.find(obj) != objects_.end(); }

std::uint32_t ComRegistry::refCount(std::uint32_t obj) const {
    const auto it = objects_.find(obj);
    return it == objects_.end() ? 0u : it->second.refcount;
}

std::uint32_t ComRegistry::vtableOf(std::uint32_t obj) const {
    const auto it = objects_.find(obj);
    return it == objects_.end() ? 0u : it->second.vtable;
}

const char* ComRegistry::ifaceOf(std::uint32_t obj) const {
    const auto it = objects_.find(obj);
    return it == objects_.end() ? nullptr : it->second.iface.c_str();
}

void ComRegistry::dispatch(X86& cpu, ShimCall& call) {
    const std::uint32_t stub = call.def ? call.def->stub_ea : 0;
    const auto it = methods_.find(stub);
    if (it == methods_.end()) {
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
        ++cpu.shim.unimplemented_calls;
        return;
    }
    const MethodEntry& e = it->second;
    if (!e.fn) {
        // 未实现的槽：走到这里就是 loud trap（见下面的 comUnsupported）
        comUnsupported(cpu, call);
        return;
    }
    // 诊断（不改变行为）：`this` 应该是我们建出来的对象 —— vtable 只可能从 createObject 流出去。
    const std::uint32_t self = call.u32(0);
    if (objects_.find(self) == objects_.end()) {
        comLog(e.iface.c_str(),
               "%s(+0x%X) 的 this=0x%08X 不是 ComRegistry 建出来的对象 —— 仍转发（可能是 guest 自己造的 vtable）",
               e.name.c_str(), e.offset, self);
    }
    e.fn(cpu, call);
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
    ++cpu.shim.unimplemented_calls;
}

}  // namespace lift
