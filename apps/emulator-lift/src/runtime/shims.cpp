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
// 未实现导入 / 未实现形态的**唯一**响亮停机点（见 shims.h 的声明）。
// ★ 抽出来的理由：缺口路径（`fn == nullptr`）与"已注册、但某个**形态**拒绝"的条件 trap
//   （例：`imm32!ImmAssociateContext` 的 `hIMC != 0`）必须打印**同一份**现场，否则两处会
//   各漂一套格式，`shims: called-unimplemented` 之类的读数也就对不上了。
// ★ `stop_kind = Trap` 只对"已注册的 fn"那条路生效（`Machine::run` 先按 `!had_impl` 判
//   `Unimplemented`，与旧行为逐字节相同）；否则 `had_impl == true` + 种类 None 会被当成
//   `Stopped`（**正常结局**）—— 那正是"响亮停"要消掉的缝。
// ---------------------------------------------------------------------------------------
void trapUnimplemented(X86& cpu, const char* module, const char* name, const char* reason,
                       std::uint32_t ret_ea, std::uint32_t stub_ea, Conv conv, int argc) {
    std::fprintf(stderr, "\n=== unimplemented import (base gap) ===\n");
    if (stub_ea != 0) {
        std::fprintf(stderr, "  %s!%s  (stub 0x%08X, conv=%s, argc=%d)\n", module ? module : "?",
                     name ? name : "?", stub_ea, convName(conv), argc);
    } else {
        std::fprintf(stderr, "  %s!%s  (conv=%s, argc=%d)\n", module ? module : "?", name ? name : "?",
                     convName(conv), argc);
    }
    std::fprintf(stderr, "  reason: %s\n", reason ? reason : "(未登记理由)");
    std::fprintf(stderr, "  返回地址 = 0x%08X\n", ret_ea);
    dumpRegisters(cpu, stderr);
    dumpGuestStack(cpu, stderr, 24);
    cpu.running = false;
    cpu.stop_why = "unimplemented import";
    cpu.stop_kind = HostStopKind::Trap;
    ++cpu.shim.unimplemented_calls;
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
    // ★ 索引表按新桩页重建（此刻 `next_slot_ == 0` ⇒ 还没有任何 stub EA，表必然是空的）+ 预留容量：
    //   bind 之后 `addRuntime` 还会追加 def；预留到"现有登记 + 槽位上限"⇒ 运行期**不再重分配**
    //   （并发读 `defs_` 的读者 —— 例如 feeder 的 `idForStub` —— 不会看到被释放的旧缓冲）。
    defs_.reserve(defs_.size() + shim_slots_);
    rebuildSlotIndexLocked();
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
    // ★ 登记期断言（响亮停）：发出去的 EA 必须满足 `page + slot*4` 的形状。
    checkStubEaInPageLocked(ea, "addRuntime");
    // ② bind() 之前登记过、但那时没有桩页（bind 成功后本该都有地址）⇒ 复用这一条，补上地址
    if (existing) {
        existing->conv = conv;
        existing->argc = argc;
        existing->fn = fn;
        existing->reason = nullptr;
        existing->declared = true;
        existing->stub_ea = ea;
        rebuildSlotIndexLocked();  // 新槽 → 索引表（O(defs_)；登记不是热路径）
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
    rebuildSlotIndexLocked();  // 新条目 → 索引表
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
    // ★ 热路径（AOT：每次 shim 派发 / 每次 `lift_shim`）：**无锁**，一次无符号减 + 一次边界比较 +
    //   一次数组取 + 一次 `stub_ea` 二次校验。为什么不需要锁：
    //   · `shim_page_` / `shim_slots_` 在 bind() 之后不再改；
    //   · `slot_index_` 在 bind()/configureStubPage() 时定长，运行期只写已有格子（4 字节标量）；
    //   · 命中后还要 `d->stub_ea == ea`：表过期（夹具直写 stub_ea）时**不会**给出错答案，只会走慢路径。
    //   ⛔ 不要在这里加 `size()` 之类的"顺手一致性检查" —— 那就把锁/扫描又请回来了。
    const std::uint32_t slots = shim_slots_;
    const std::uint32_t off = ea - shim_page_;  // 无符号：ea < page ⇒ 巨大值 ⇒ 一定落在页外
    if (slots != 0u && off < slots * 4u) {
        const std::uint32_t slot = off >> 2;
        if (slot < slot_index_.size()) {
            const std::uint32_t id1 = slot_index_[slot];
            if (id1 != 0u) {
                const ShimDef* d = defByIdUnlocked(id1 - 1u);
                if (d != nullptr && d->stub_ea == ea) return d;
            }
        }
    }
    // ---- 慢路径：**与改动前逐字相同**的线性扫描（仍在锁内）----
    //   什么时候到这儿：未登记的槽（随后 lift_shim 响亮停）/ 页外 EA / 未对齐 EA /
    //   `slot_index_` 还没建 / 有人直写了 `stub_ea` 让表过期。
    stub_index_slow_.fetch_add(1u, std::memory_order_relaxed);
    return byStubEaLinearLocked(ea);
}

const ShimDef* ShimRegistry::byStubEaLinearLocked(std::uint32_t ea) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (const ShimDef& d : defs_)
        if (d.stub_ea == ea && ea != 0) return &d;
    return nullptr;
}

std::uint32_t ShimRegistry::idForStub(std::uint32_t ea) const {
    // ★ 同一份解析路径（旧实现也是"扫一遍取 d.id"）：不再单独拿锁 / 再扫一遍。
    const ShimDef* d = byStubEa(ea);
    return d != nullptr ? d->id : 0u;
}

const ShimDef* ShimRegistry::byId(std::uint32_t id) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return defByIdUnlocked(id);
}

const ShimDef* ShimRegistry::defByIdUnlocked(std::uint32_t id) const {
    // 与 `byId()` **同一条**解析路径：一次边界检查 + `&defs_[id]`（⛔ 表里不存指针）。
    return id < defs_.size() ? &defs_[id] : nullptr;
}

void ShimRegistry::rebuildSlotIndexLocked() {
    slot_index_.assign(shim_slots_, 0u);
    for (std::size_t i = 0; i < defs_.size(); ++i) {
        const ShimDef& d = defs_[i];
        if (d.stub_ea == 0) continue;
        // ★ 这里**不**做"必须在桩页内"的断言：登记路径（`bind` / `addRuntime`）各自在**发号点**
        //   已经响亮停地断言过（那才是"桩 EA 只有一种形状"的证明）；而夹具会**直写** `stub_ea`
        //   （`tests/fixture.h` 的 `kStubBase = 0x7FFE2000` 与 `configureStubPage(0x7FF80000,…)` 就是
        //   两个不同的页）⇒ 那些 def **不进索引表**，查询落回"与旧实现逐字相同"的线性扫描。
        //   ⛔ 不要为了让它们进表而放宽页判据（那会把"页外 ⇒ 一定不是桩 EA"的结论弄丢）。
        if (d.stub_ea < shim_page_ || (d.stub_ea - shim_page_) >= shim_slots_ * 4u || (d.stub_ea & 3u) != 0u) {
            continue;
        }
        // ★ `byStubEa` 用**下标**当 id（表里存 i+1）⇒ 这里必须钉住 `id == 下标`
        //   （全部登记路径都是 `def.id = defs_.size()` 后 push_back；违反就是内部不一致）。
        if (d.id != i) {
            std::fprintf(stderr,
                         "[shim] ⛔ 内部不一致：defs_[%zu].id=%u（槽位索引表按**下标**解析）\n", i, d.id);
            std::fflush(stderr);
            std::abort();
        }
        const std::uint32_t slot = (d.stub_ea - shim_page_) >> 2;
        // 同一个槽被两条登记抢 ⇒ **先登记者胜**（与旧 `byStubEa` 的"扫到第一个"逐位相同）。
        if (slot_index_[slot] == 0u) slot_index_[slot] = static_cast<std::uint32_t>(i) + 1u;
    }
}

void ShimRegistry::checkStubEaInPageLocked(std::uint32_t stub_ea, const char* where) const {
    checkStubEaShape(stub_ea, shim_page_, shim_slots_, where);
}

void ShimRegistry::checkStubEaShape(std::uint32_t stub_ea, std::uint32_t page, std::uint32_t slots,
                                    const char* where) {
    // ★ **可证明的保命闸**（不是"观察到"）：桩 EA 只由 bind()/addRuntime() 发号，形状恒为
    //   `page + slot*4`。有了这条断言，"非桩页 ⇒ 一定不是桩 EA"与"桩页内未对齐 ⇒ 一定不是
    //   桩 EA"才是**前提**（AOT 的派发热路径据此只用一次无符号比较，见 `aot_run.cpp`）。
    if (stub_ea == 0u) return;
    const bool in_page = slots != 0u && stub_ea >= page && (stub_ea - page) < slots * 4u;
    if (in_page && (stub_ea & 3u) == 0u) return;
    std::fprintf(stderr,
                 "[shim] ⛔ 桩 EA 违反登记期不变量（%s）：stub_ea=%08X，桩页=%08X+%u 槽（%u 字节）\n"
                 "  要求：stub_ea == 0 或 [page, page+slots*4) 内且 4 字节对齐。\n"
                 "  ⛔ 不静默继续：AOT 派发快路径以这条不变量为前提（见 aot_run.cpp / shims.h）。\n",
                 where, stub_ea, page, slots, slots * 4u);
    std::fflush(stderr);
    std::abort();
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
        if (def->stub_ea == 0) {
            def->stub_ea = shim_page + slot * 4u;
            checkStubEaShape(def->stub_ea, shim_page, shim_slots, "bind/import");
        }
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
        checkStubEaShape(d.stub_ea, shim_page, shim_slots, "bind/extras");
        ++slot;
    }
    // ★ 记下桩页状态：`addRuntime` 从这里**接着往后**发地址（⛔ 绝不覆盖 bind 已发的 EA）。
    shim_page_ = shim_page;
    shim_slots_ = shim_slots;
    next_slot_ = slot;
    // ★ 槽位索引表（见 shims.h 的注释）：bind 是**唯一**批量发号的地方 ⇒ 在这里重建一次。
    //   预留容量 = 现有登记 + 剩余槽位（之后 addRuntime 只会消耗这些槽位、不会再扩容）。
    defs_.reserve(defs_.size() + (shim_slots_ - next_slot_));
    rebuildSlotIndexLocked();
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
        // **响亮失败**：未实现的导入。这里是 A5 缺口的执行期落点（与条件 trap 共用停机点，
        // 见上面的 `trapUnimplemented`）。
        trapUnimplemented(cpu, local.module.c_str(), local.name.c_str(), local.reason, ret_ea, local.stub_ea,
                          local.conv, local.argc);
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
    //
    // ★ Phase-0 读数（REQ-01M75WWEDC20GEH7QVSHPHH4GP）：`per-shim max_ns` —— 「哪一类 shim
    //   是长活」（Present / `fread` / DS 的 `mutex_` / harness tick）直接决定 M0 的靶子。
    //   只在**已经打开**的诊断下读时钟：`timing_on`（`--profile` / 帧 trace / 窗口 profiler /
    //   叠层，见 `Machine::run` 的 `setTiming`）∪ `LIFT_AUDIO_TRACE`。⛔ 默认两臂都假 ⇒ 与旧路径
    //   逐字节相同（不读时钟、不进 `noteShimSpan`）。与 `ns` 的关系：**起点同一处**（取到令牌之后），
    //   只是 `ns` 记累计、`max_ns` 只记**最大值**（⛔ 不存每次读数）。
    const bool audio_trace = audioTraceEnabled();
    const bool shim_max_wanted = timing_on || audio_trace;
    std::chrono::steady_clock::time_point time_begin{};
    {
        GuestToken::Guard shim_token(guestToken());
        // ★ 只读计量（见文件上方 `t_shim_token_wait_ns`）：**不改**这一取/放的任何语义，
        //   只在本线程"真的被挡住"时把那段时间记到自己的账上（`lastAcquireWaitNs()` 是 TLS 读）。
        if (audio_trace) {
            const std::uint64_t waited_ns = guestToken().lastAcquireWaitNs();
            if (waited_ns != 0) {
                t_shim_token_wait_ns += waited_ns;
                ++t_shim_token_wait_count;
            }
        }
        // ★ 检查点：Guard 被删/短路时，**第一次 shim 调用**就计数（不依赖 heap 恰好被碰到）。
        guestToken().check("shim.invoke");
        // ★ M1：计时起点在**取到令牌之后**（等令牌的时间⛔ 不进 ns，见上方注释）。
        if (shim_max_wanted) time_begin = std::chrono::steady_clock::now();
        local.fn(cpu, call);
    }
    if (shim_max_wanted) {
        const std::uint64_t ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - time_begin)
                .count());
        if (timing_on) {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            if (trace_id < defs_.size()) defs_[trace_id].ns += ns;
        }
        // ★ Phase-0：每个 shim 的**最长一次**宿主墙钟（只记 max + 次数；内部还有一道
        //   `phase0_enabled_` 门 ⇒ 只开帧 trace / 叠层时不记）。
        guestToken().noteShimSpan(trace_id, ns);
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
