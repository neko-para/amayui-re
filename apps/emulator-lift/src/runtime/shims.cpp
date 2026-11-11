// emulator-lift · shim 框架实现（唯一 guest 地址 + 调用约定 + trace + 缺口报告）
//
// ★ 机制回顾（shims.h）：每个导入在**桩页**里有唯一地址（4 字节步进），IAT 槽写这个地址。
//   dispatcher 见到这个 EA 就调 `invoke()`：读返回地址 → 调宿主实现 → 按约定弹参 → 回 eip。
// ★ 弹参规则只有一条：`stdcall_bytes = (conv == Stdcall) ? argc*4 : 0`。
//   ⛔ 别统一按 stdcall 弹：cdecl 每调一次会多抬 4 字节栈，几百次后炸在毫不相干的地方。
#include "runtime/shims.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "runtime/exec.h"
#include "runtime/guest_token.h"
#include "runtime/image.h"
#include "runtime/memory.h"
#include "runtime/sha256.h"
#include "runtime/x86.h"

namespace lift {

// ★ 只读计量（**只给 `LIFT_AUDIO_TRACE` 的 fill 时间分解**；⛔ 不是契约、⛔ 不参与任何判定）：
//   `ShimRegistry::invoke` 在**内层**取令牌（`GuestToken::Guard`）时，如果这一取真的被挡住，
//   把那段墙钟与次数累加到**调用线程**自己的账上。
//   · `thread_local` 是有意的：feeder 的 fill 收尾**在同一条线程**上读它（audio.cpp 顶部有
//     `extern` 声明）⇒ 读到的就是"这一刀在 shim 边界上等了多久"，**不会混进主线程的等待**。
//   · 只有 `LIFT_AUDIO_TRACE` 打开时才累计（读一次缓存的静态 bool），否则热路径只多一次
//     `lastAcquireWaitNs()`（那是 TLS 读，0 成本）之后的判断 —— ⛔ 不加时钟读。
//   · 口径：`GuestToken::lastAcquireWaitNs()` 在"未竞争 / 重入"时是 0 ⇒ 只有真被挡的那次才计。
//     ⛔ 它与 `defs_[id].ns` **不可相加**（`ns` 的计时起点在取到令牌之后，见下面 invoke 的注释）。
thread_local std::uint64_t t_shim_token_wait_ns = 0;
thread_local std::uint64_t t_shim_token_wait_count = 0;

namespace {
// `LIFT_AUDIO_TRACE` 只读一次（默认关 ⇒ 默认行为与读数都不变）。
bool audioTraceEnabled() {
    static const bool on = std::getenv("LIFT_AUDIO_TRACE") != nullptr;
    return on;
}
}  // namespace

const char* convName(Conv c) {
    switch (c) {
        case Conv::Stdcall: return "stdcall";
        case Conv::Cdecl: return "cdecl";
        case Conv::Varargs: return "varargs";
    }
    return "?";
}

// ---------------------------------------------------------------------------------------
// ShimCall：参数都从 guest 栈上读（`[esp]` = 返回地址，`[esp+4+4i]` = 第 i 个参数）
// ---------------------------------------------------------------------------------------
std::uint32_t ShimCall::u32(int i) const {
    return cpu->mem->rd32(cpu->esp() + 4u + 4u * static_cast<std::uint32_t>(i));
}

std::uint16_t ShimCall::u16(int i) const {
    return static_cast<std::uint16_t>(u32(i) & 0xFFFFu);
}

std::string ShimCall::str(int i) const {
    const std::uint32_t ea = u32(i);
    if (ea == 0) return {};
    return cpu->mem->readCString(ea);
}

std::u16string ShimCall::wstr(int i) const {
    const std::uint32_t ea = u32(i);
    if (ea == 0) return {};
    std::u16string s;
    for (std::size_t k = 0; k < 4096; ++k) {
        const char16_t c = static_cast<char16_t>(cpu->mem->rd16(ea + static_cast<std::uint32_t>(k) * 2));
        if (c == 0) break;
        s.push_back(c);
    }
    return s;
}

std::vector<std::uint8_t> ShimCall::bytes(int i, std::size_t n) const {
    const std::uint32_t ea = u32(i);
    std::vector<std::uint8_t> v(n);
    if (ea == 0 || n == 0) return v;
    cpu->mem->readBytes(ea, v.data(), n);
    return v;
}

// ---------------------------------------------------------------------------------------
// ShimRegistry
// ---------------------------------------------------------------------------------------
namespace {
// ★ PE 的导入表写的是 `KERNEL32.dll`，host 侧注册时写的是 `kernel32` —— 必须归一化。
//   规则：小写 + 去掉尾部的 `.dll`。⛔ 不要靠"注册时抄对名字"，那会让漏配变成运行时缺口。
std::string normalizeModule(std::string_view s) {
    std::string o = toLower(std::string(s));
    if (o.size() > 4 && o.compare(o.size() - 4, 4, ".dll") == 0) o.resize(o.size() - 4);
    return o;
}
}  // namespace

void ShimRegistry::add(const char* module, const char* name, Conv conv, int argc, ShimFn fn) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    ShimDef def;
    def.module = module ? module : "";
    def.name = name ? name : "";
    def.conv = conv;
    def.argc = argc;
    def.fn = fn;
    def.id = static_cast<std::uint32_t>(defs_.size());
    def.declared = true;
    defs_.push_back(std::move(def));
}

void ShimRegistry::addUnimplemented(const char* module, const char* name, const char* reason) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    ShimDef def;
    def.module = module ? module : "";
    def.name = name ? name : "";
    def.conv = Conv::Stdcall;
    def.argc = 0;
    def.fn = nullptr;
    def.reason = reason;
    def.id = static_cast<std::uint32_t>(defs_.size());
    def.declared = true;
    defs_.push_back(std::move(def));
}

void ShimRegistry::addStub(const char* module, const char* name, Conv conv, int argc, ShimFn fn,
                           const char* reason) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    ShimDef def;
    def.module = module ? module : "";
    def.name = name ? name : "";
    def.conv = conv;
    def.argc = argc;
    def.fn = fn;
    def.reason = reason;
    def.id = static_cast<std::uint32_t>(defs_.size());
    def.declared = true;
    defs_.push_back(std::move(def));
}

void ShimRegistry::configureStubPage(std::uint32_t page, std::uint32_t slots) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (next_slot_ != 0) {
        // ⛔ 已经发过地址了：重置槽位会让新 def 拿到**别人已占的 EA**（两个 def 抢同一个 stub）。
        std::fprintf(stderr,
                     "[shim] configureStubPage(%08X, %u) 被拒绝：已经有 %u 个 stub 发出去（槽位不能重置）\n",
                     page, slots, next_slot_);
        return;
    }
    if (page == 0 || slots == 0) {
        std::fprintf(stderr, "[shim] configureStubPage(%08X, %u) 参数非法 ⇒ 忽略\n", page, slots);
        return;
    }
    shim_page_ = page;
    shim_slots_ = slots;
}

ShimDef* ShimRegistry::addRuntime(const char* module, const char* name, Conv conv, int argc, ShimFn fn) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const std::string mod = module ? module : "";
    const std::string nm = name ? name : "";
    // ① 幂等：已经登记过、而且**已经有唯一地址** ⇒ 原样返回（⛔ 不覆盖 stub_ea、不换实现）
    ShimDef* existing = findMutable(mod.c_str(), nm.c_str());
    if (existing && existing->stub_ea != 0) {
        if (existing->fn != fn || existing->argc != argc || existing->conv != conv) {
            std::fprintf(stderr,
                         "[shim] addRuntime: %s!%s 已登记（stub %08X）但这次的 (fn,argc,conv) 与它不同 ⇒ "
                         "保留已有实现（幂等；⛔ 不静默换实现）\n",
                         mod.c_str(), nm.c_str(), existing->stub_ea);
        }
        return existing;
    }
    if (shim_slots_ == 0) {
        std::fprintf(stderr,
                     "[shim] addRuntime(%s!%s) 失败：还没有桩页（bind() 没跑过、也没 configureStubPage）\n",
                     mod.c_str(), nm.c_str());
        return nullptr;
    }
    if (next_slot_ >= shim_slots_) {
        // ⛔ 不覆盖别人已占的 EA（那会让两个导入共用一个 stub，trace 与弹参都错）
        std::fprintf(stderr, "[shim] addRuntime(%s!%s) 失败：桩页槽位耗尽（%u/%u）\n", mod.c_str(), nm.c_str(),
                     next_slot_, shim_slots_);
        return nullptr;
    }
    const std::uint32_t ea = shim_page_ + next_slot_ * 4u;
    ++next_slot_;
    // ② bind() 之前登记过、但那时没有桩页（bind 成功后本该都有地址）⇒ 复用这一条，补上地址
    if (existing) {
        existing->conv = conv;
        existing->argc = argc;
        existing->fn = fn;
        existing->reason = nullptr;
        existing->declared = true;
        existing->stub_ea = ea;
        return existing;
    }
    ShimDef def;
    def.module = mod;
    def.name = nm;
    def.conv = conv;
    def.argc = argc;
    def.fn = fn;
    def.reason = nullptr;  // 不是"未实现的导入"（缺理由的 stub 另有 addUnimplemented）
    def.stub_ea = ea;
    def.id = static_cast<std::uint32_t>(defs_.size());
    def.declared = true;   // host 侧显式登记过
    def.imported = false;  // IAT 里没有它（动态导出 / vtable 槽）
    defs_.push_back(std::move(def));
    return &defs_.back();
}

const ShimDef* ShimRegistry::find(const char* module, const char* name) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const std::string m = normalizeModule(module ? module : "");
    for (const ShimDef& d : defs_) {
        if (normalizeModule(d.module) == m && equalsIgnoreCase(d.name, name ? name : "")) return &d;
    }
    return nullptr;
}

ShimDef* ShimRegistry::findMutable(const char* module, const char* name) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const std::string m = normalizeModule(module ? module : "");
    for (ShimDef& d : defs_) {
        if (normalizeModule(d.module) == m && equalsIgnoreCase(d.name, name ? name : "")) return &d;
    }
    return nullptr;
}

const ShimDef* ShimRegistry::byStubEa(std::uint32_t ea) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (const ShimDef& d : defs_)
        if (d.stub_ea == ea && ea != 0) return &d;
    return nullptr;
}

std::uint32_t ShimRegistry::idForStub(std::uint32_t ea) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (const ShimDef& d : defs_)
        if (d.stub_ea == ea && ea != 0) return d.id;
    return 0;
}

const ShimDef* ShimRegistry::byId(std::uint32_t id) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return id < defs_.size() ? &defs_[id] : nullptr;
}

std::vector<ShimDef> ShimRegistry::snapshot() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return defs_;
}

std::size_t ShimRegistry::size() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return defs_.size();
}

bool ShimRegistry::bind(GuestMemory& mem, const PeImage& img, std::uint32_t shim_page, std::uint32_t shim_slots,
                        std::string* err) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::uint32_t slot = 0;
    std::size_t unresolved = 0;
    for (const PeImportInfo& imp : img.imports) {
        std::string name = imp.by_ordinal ? ("#" + std::to_string(imp.ordinal)) : imp.name;
        ShimDef* def = findMutable(imp.module.c_str(), name.c_str());
        if (!def) {
            // 导入表里有、但我们既没实现也没登记 ⇒ 现场补一条（缺口必须可见，⛔ 不能悄悄跳过）
            ShimDef d;
            d.module = imp.module;
            d.name = name;
            d.conv = Conv::Stdcall;
            d.argc = 0;
            d.fn = nullptr;
            d.reason = "导入表里有，但既没有实现也没有 addUnimplemented 登记";
            d.declared = false;  // 连一行登记都没有 ⇒ 这是"裸缺口"
            d.id = static_cast<std::uint32_t>(defs_.size());
            defs_.push_back(std::move(d));
            def = &defs_.back();
        }
        if (slot >= shim_slots) {
            if (err) *err = "shim 桩页槽位不够（shim_slots=" + std::to_string(shim_slots) + "）";
            return false;
        }
        def->imported = true;
        if (def->stub_ea == 0) def->stub_ea = shim_page + slot * 4u;
        ++slot;
        if (!def->fn) ++unresolved;
        // IAT 槽指向桩页里的唯一地址
        const std::uint32_t iat_ea = img.image_base + imp.iat_rva;
        if (!mem.committed(iat_ea, 4)) {
            if (err) *err = "IAT 槽不在镜像里（导入表 RVA 失效）";
            return false;
        }
        mem.wr32(iat_ea, def->stub_ea);
    }
    (void)unresolved;
    // ★ 非导入的显式桩（addStub / addUnimplemented，但 IAT 里没有它）：同样分到唯一地址。
    //   典型：`GetProcAddress` 对 AGERC 导出返回的 thunk（guest 侧 `call eax` 落到 dispatcher）。
    //   它们不进 IAT，所以不影响导入槽位；顺序 = 注册顺序（确定性）。
    for (ShimDef& d : defs_) {
        if (d.stub_ea != 0 || !d.declared) continue;
        if (slot >= shim_slots) {
            if (err) *err = "shim 桩页槽位不够（extras 也要唯一地址，shim_slots=" + std::to_string(shim_slots) + "）";
            return false;
        }
        d.stub_ea = shim_page + slot * 4u;
        ++slot;
    }
    // ★ 记下桩页状态：`addRuntime` 从这里**接着往后**发地址（⛔ 绝不覆盖 bind 已发的 EA）。
    shim_page_ = shim_page;
    shim_slots_ = shim_slots;
    next_slot_ = slot;
    return true;
}

std::vector<const ShimDef*> ShimRegistry::importedButUnimplemented() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<const ShimDef*> out;
    for (const ShimDef& d : defs_)
        if (d.imported && !d.fn) out.push_back(&d);
    std::sort(out.begin(), out.end(), [](const ShimDef* a, const ShimDef* b) {
        if (a->module != b->module) return a->module < b->module;
        return a->name < b->name;
    });
    return out;
}

std::vector<const ShimDef*> ShimRegistry::calledUnimplemented() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<const ShimDef*> out;
    for (const ShimDef& d : defs_)
        if (d.called && !d.fn) out.push_back(&d);
    return out;
}

std::size_t ShimRegistry::importedCount() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::size_t n = 0;
    for (const ShimDef& d : defs_)
        if (d.imported) ++n;
    return n;
}

bool ShimRegistry::invokeById(X86& cpu, std::uint32_t id) {
    ShimDef copy;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (id >= defs_.size()) return false;
        copy = defs_[id];
    }
    invoke(cpu, copy);
    return true;
}

void ShimRegistry::invoke(X86& cpu, ShimDef& def) {
    // ★ 方案 C：**不要**在 fn 执行期间持有 shims 锁（主线程的 Present/D3D shim 可能很长，
    //   feeder 的 Lock/Unlock 会被它卡住）。只锁住"拷贝 def + 计数"，fn 在锁外跑。
    ShimDef local;
    bool trace_on = false;
    bool timing_on = false;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        local = def;
        trace_on = trace_;
        timing_on = timing_;
        if (local.id < defs_.size()) {
            defs_[local.id].called = true;
            ++defs_[local.id].call_count;
        }
    }
    const std::uint32_t ret_ea = cpu.mem->rd32(cpu.esp());
    ShimCall call;
    call.cpu = &cpu;
    call.conv = local.conv;
    call.argc = local.argc;
    call.ret_ea = ret_ea;
    // ★ 运行期注册的 thunk（COM vtable 槽）靠它认出"我是哪个槽"（见 shims.h 的 ShimCall::def）。
    //   指向**栈上的拷贝**：fn 执行期间不会因为 addRuntime 扩容而悬垂。
    call.def = &local;
    call.stdcall_bytes = (local.conv == Conv::Stdcall) ? static_cast<std::uint32_t>(local.argc) * 4u : 0u;

    const std::uint32_t trace_id = local.id;
    const std::string trace_module = local.module;
    const std::string trace_name = local.name;
    const int trace_argc = local.argc;

    if (!local.fn) {
        // **响亮失败**：未实现的导入。这里是 A5 缺口的执行期落点。
        std::fprintf(stderr, "\n=== unimplemented import (base gap) ===\n");
        std::fprintf(stderr, "  %s!%s  (stub 0x%08X, conv=%s, argc=%d)\n", local.module.c_str(), local.name.c_str(),
                     local.stub_ea, convName(local.conv), local.argc);
        std::fprintf(stderr, "  reason: %s\n", local.reason ? local.reason : "(未登记理由)");
        std::fprintf(stderr, "  返回地址 = 0x%08X\n", ret_ea);
        dumpRegisters(cpu, stderr);
        dumpGuestStack(cpu, stderr, 24);
        cpu.running = false;
        cpu.stop_why = "unimplemented import";
        ++cpu.shim.unimplemented_calls;
        return;
    }

    // ★ 并发契约（guest_token.h）：**所有 guest 可见的共享容器访问都发生在 shim 实现里**
    //   （GuestHeap / HandleTable / Vfs / ComRegistry / SurfaceArena / guest 内存账本 …
    //   都经由 shim 的 `fn` 走到）。⇒ shim 边界就是"容器临界区"：
    //   主线程本来就按块持有令牌；feeder 线程**不**在整次 fill 期间持令牌（那样等于单核
    //   串行，实测 wall ≥ 两侧 CPU 之和），只在每次 shim 调用时取 —— 与主线程的块互斥。
    //   ⛔ 不要在 `fn` 里再手写"绕过令牌"的共享容器访问：新增容器入口要么走 shim，要么单独加锁。
    //
    // ★ M1（REQ-01M6XVZG4T9WY8SQ1WS7P8RNW9）：`defs_[id].ns` 的口径是「**shim 实现自身**」，
    //   所以计时起点必须在**取到令牌之后**（见下面 `time_begin`）。"等令牌"的墙钟不属于这里 ——
    //   它另记在 `GuestToken::waits()/waitNs()`（运行摘要的 `tokenwait` 行 + `--profile` 的
    //   token-wait 块）。★ 口径影响：同一场景下 `ns` 比旧读数**小**（少掉等待那段）——
    //   不是回归，是这条注释终于与事实一致（旧代码把等待记进了 shim 自己的墙钟）。
    std::chrono::steady_clock::time_point time_begin{};
    {
        GuestToken::Guard shim_token(guestToken());
        // ★ 只读计量（见文件上方 `t_shim_token_wait_ns`）：**不改**这一取/放的任何语义，
        //   只在本线程"真的被挡住"时把那段时间记到自己的账上（`lastAcquireWaitNs()` 是 TLS 读）。
        if (audioTraceEnabled()) {
            const std::uint64_t waited_ns = guestToken().lastAcquireWaitNs();
            if (waited_ns != 0) {
                t_shim_token_wait_ns += waited_ns;
                ++t_shim_token_wait_count;
            }
        }
        // ★ 检查点：Guard 被删/短路时，**第一次 shim 调用**就计数（不依赖 heap 恰好被碰到）。
        guestToken().check("shim.invoke");
        // ★ M1：计时起点在**取到令牌之后**（等令牌的时间⛔ 不进 ns，见上方注释）。
        if (timing_on) time_begin = std::chrono::steady_clock::now();
        local.fn(cpu, call);
    }
    if (timing_on) {
        const std::uint64_t ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - time_begin)
                .count());
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (trace_id < defs_.size()) defs_[trace_id].ns += ns;
    }
    cpu.eax() = call.result;

    if (trace_on) {
        // ★ 确定性：只打参数原始字 + 返回值，不打时间、不打宿主地址
        std::fprintf(trace_out_, "[shim] %3u %s!%s(", trace_id, trace_module.c_str(), trace_name.c_str());
        for (int i = 0; i < trace_argc; ++i) {
            std::fprintf(trace_out_, "%s%08X", i ? "," : "", call.u32(i));
        }
        std::fprintf(trace_out_, ") -> %08X  ret=%08X\n", call.result, ret_ea);
    }

    // ★ shim 接管了控制流（guest 回调 / 续延）：esp 与 eip 由它自己安排好，
    //   这里**不许**再弹参或改写 eip（否则回调还没跑就被 return 掉了）。
    if (call.take_control) return;

    // `ret N`（stdcall）或 `ret`（cdecl）：先弹返回地址，再抬 argc*4
    cpu.esp() += 4u + call.stdcall_bytes;
    cpu.eip = ret_ea;
}

}  // namespace lift
