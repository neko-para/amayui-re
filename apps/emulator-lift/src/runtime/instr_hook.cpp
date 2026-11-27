#include "runtime/instr_hook.h"

#include <algorithm>
#include <atomic>

#include "runtime/engine_marker.h"   // 脚本身份（id → 名字）与位置公式的唯一来源
#include "runtime/memory.h"
#include "runtime/value_codec.h"     // int 槽是 ENC/DEC 编码的
#include "runtime/x86.h"

namespace lift {
namespace {

// ---- guest 状态偏移（与 `engine_marker.h` / `age-format` 的 FRAME_LAYOUT 同一份事实）----
constexpr std::uint32_t kEngineSingletonGlobal = 0x0055E1BCu;
constexpr std::uint32_t kEngineCurOffset = 0x0005D880u;
constexpr std::uint32_t kFrameBase = 0x0005D894u;
constexpr std::uint32_t kFrameStride = 0x00000078u;
constexpr std::uint32_t kFrameBodyBaseOffset = 0x00000000u;
constexpr std::uint32_t kFramePcOffset = 0x00000004u;
constexpr std::uint32_t kFrameArgOffset = 0x0000003Cu;
constexpr std::uint32_t kFrameCount = 40u;
constexpr std::uint32_t kScriptHeaderLen = 0x0000003Cu;
constexpr std::uint32_t kMaxScriptBodyBytes = 0x01000000u;

constexpr std::uint32_t kLocalIntCount = 0x00000008u;
constexpr std::uint32_t kLocalIntBase = 0x00000020u;
constexpr std::uint32_t kLocalPtrCount = 0x00000014u;
constexpr std::uint32_t kLocalPtrBase = 0x0000002Cu;

constexpr std::uint32_t kGlobalIntBase = 0x0005D800u;   // `GLOBAL_SLOTS.base.int`
constexpr std::uint32_t kGlobalIntCount = 0x0005D7E8u;  // `GLOBAL_SLOTS.count.int`
constexpr std::uint32_t kCodecKeyOffset = 0x0005EC8Cu;

/** 一次命中的现场（只在 probe 内部用）。 */
struct Frame {
    std::uint32_t engine = 0;
    std::uint32_t cur = 0;
    std::uint32_t id = 0;
    std::uint32_t pos = 0;
    std::uint32_t int_base = 0;
    std::uint32_t int_count = 0;
    std::uint32_t ptr_base = 0;
    std::uint32_t ptr_count = 0;
    std::uint32_t key = 0;
    bool has_key = false;
};

struct Entry {
    std::uint32_t id = 0;
    // ★ S0：归属（`PluginHost` 传本 Run 的序号）⇒ Run 收口能只撤自己那批（见 `instrHookReleaseOwner`）。
    std::uint64_t owner = 0;
    std::string script;
    std::uint32_t script_id = 0;  // 0 = 还没解析（首次命中时按名字解析并缓存）
    std::uint32_t pos = 0;
    // ③ 改（缺省关：`at` 只装触发，⛔ 不写）
    bool patch = false;
    InstrPatchKind kind = InstrPatchKind::PtrDword;
    std::uint32_t index = 0;
    std::uint32_t value = 0;
    std::uint32_t max_hits = 0;
    // 读数
    std::uint64_t hits = 0;
    std::uint64_t restores = 0;
    std::uint64_t restore_conflicts = 0;  // ★ S0：CAS 判定"窗口里有人写过"的次数（不写回）
    std::uint64_t skips = 0;
    std::uint32_t last_id = 0;
    // ② 读（采样清单 + 最近一次读数）
    std::vector<std::uint32_t> cap_ints;
    std::vector<std::uint32_t> cap_ptrs;
    std::vector<std::uint32_t> last_ints;
    std::vector<std::uint32_t> last_ptrs;
    std::vector<std::uint32_t> last_targets;
};

struct Pending {
    bool active = false;
    std::uint32_t entry_id = 0;
    std::uint32_t addr = 0;
    std::uint32_t raw = 0;      // 覆盖**之前**的 4 字节（恢复目标）
    std::uint32_t written = 0;  // ★ S0：我**写进去**的编码值（恢复前比对它 = CAS）
};

struct State {
    std::vector<Entry> entries;
    Pending pending;
    // 最近一次 probe 用到的 guest 内存（`clear()` / `remove()` 在没有 X86 时也能把未还原的写恢复）。
    GuestMemory* mem = nullptr;
    std::uint32_t next_id = 1;
    // ★ S0（缺陷 3）：本构建注入的 `lift_instr_hook` 站点数。缺省 1 = "视作可用"
    //   （纯 runtime / 测试夹具没有 AOT 形状；只有 AOT 的初始化路径会把它改成真实值）。
    std::uint32_t injection_sites = 1;
};

State& state() {
    static State s;
    return s;
}

std::atomic<bool> g_active{false};

bool read32(const GuestMemory& mem, std::uint32_t ea, std::uint32_t* out) {
    if (!mem.committed(ea, 4u)) return false;
    *out = mem.rd32(ea);
    return true;
}

bool write32(GuestMemory& mem, std::uint32_t ea, std::uint32_t raw) {
    if (!mem.committed(ea, 4u)) return false;
    mem.wr32(ea, raw);
    return true;
}

// ★ S0（缺陷 2）：恢复 = **CAS**，不是无条件写回。
//   为什么必须比对：窗口 = "写入覆盖值" → "下一条脚本指令派发前"。那一段时间里**整条指令**都在跑
//   （shim / 读点 / 另一个 Run 回调 / 音频 feeder 都可能写同一个 dword）。无条件写回 `raw` 会把
//   别人的写**静默抹掉**（文档里的"净变化 0"只在这个窗口没人写过时才成立）。
//   判据：`当前值 == pending.written`（我写进去的编码值）⇒ 写回 `raw` 并计 `restores`；
//   否则**不写**、计 `restore_conflicts`（别人的写赢）。
//   读不到（未提交）⇒ 既不写也不计数：那不是"有人写过"，是"这个地址现在不可读"（与旧口径一致）。
void restorePendingLocked() {
    State& st = state();
    if (!st.pending.active) return;
    Entry* owner_entry = nullptr;
    for (Entry& e : st.entries) {
        if (e.id == st.pending.entry_id) {
            owner_entry = &e;
            break;
        }
    }
    if (st.mem != nullptr) {
        std::uint32_t current = 0;
        if (read32(*st.mem, st.pending.addr, &current)) {
            if (current == st.pending.written) {
                if (write32(*st.mem, st.pending.addr, st.pending.raw) && owner_entry != nullptr) {
                    owner_entry->restores += 1;
                }
            } else if (owner_entry != nullptr) {
                owner_entry->restore_conflicts += 1;
            }
        }
    }
    st.pending.active = false;
}

bool sampleFrame(X86& cpu, Frame* out) {
    if (cpu.mem == nullptr) return false;
    GuestMemory& mem = *cpu.mem;
    Frame f;
    if (!read32(mem, kEngineSingletonGlobal, &f.engine) || f.engine == 0) return false;
    if (!read32(mem, f.engine + kEngineCurOffset, &f.cur) || f.cur >= kFrameCount) return false;
    const std::uint32_t frame = f.engine + kFrameBase + kFrameStride * f.cur;
    std::uint32_t body = 0;
    std::uint32_t pc = 0;
    if (!read32(mem, frame + kFrameBodyBaseOffset, &body)) return false;
    if (!read32(mem, frame + kFramePcOffset, &pc)) return false;
    if (body == 0 || pc < body || (pc - body) > kMaxScriptBodyBytes) return false;
    f.pos = kScriptHeaderLen + (pc - body);
    if (!read32(mem, frame + kFrameArgOffset, &f.id)) return false;
    if (!read32(mem, frame + kLocalIntCount, &f.int_count)) return false;
    if (!read32(mem, frame + kLocalIntBase, &f.int_base)) return false;
    if (!read32(mem, frame + kLocalPtrCount, &f.ptr_count)) return false;
    if (!read32(mem, frame + kLocalPtrBase, &f.ptr_base)) return false;
    f.has_key = read32(mem, f.engine + kCodecKeyOffset, &f.key);
    *out = f;
    return true;
}

/** `id → 名字`（只在某条 hook 首次命中时做一次；之后按 id 比）。 */
bool resolveScriptName(X86& cpu, std::uint32_t id, std::string* out) {
    if (cpu.mem == nullptr) return false;
    MarkerSampler sampler;
    MarkerSample sample = sampler.sampleIdentity(*cpu.mem);
    if (!sample.valid || sample.id != id) return false;
    sampler.resolveName(*cpu.mem, &sample);
    if (sample.name_degraded || sample.name.empty()) return false;
    *out = sample.name;
    return true;
}

bool targetAddress(X86& cpu, const Frame& f, const Entry& e, std::uint32_t* addr) {
    GuestMemory& mem = *cpu.mem;
    switch (e.kind) {
        case InstrPatchKind::PtrDword: {
            if (e.index >= f.ptr_count) return false;
            std::uint32_t p = 0;
            if (!read32(mem, f.ptr_base + 4 * e.index, &p) || p == 0) return false;
            *addr = p;
            return true;
        }
        case InstrPatchKind::LocalInt: {
            if (e.index >= f.int_count) return false;
            *addr = f.int_base + 4 * e.index;
            return true;
        }
        case InstrPatchKind::GlobalInt: {
            std::uint32_t base = 0;
            std::uint32_t count = 0;
            if (!read32(mem, f.engine + kGlobalIntBase, &base) || base == 0) return false;
            if (!read32(mem, f.engine + kGlobalIntCount, &count)) return false;
            if (e.index > count) return false;
            *addr = base + 4 * e.index;
            return true;
        }
    }
    return false;
}

void captureLocals(const GuestMemory& mem, const Frame& f, Entry* e) {
    e->last_ints.clear();
    e->last_ptrs.clear();
    e->last_targets.clear();
    for (const std::uint32_t n : e->cap_ints) {
        std::uint32_t raw = 0;
        if (n >= f.int_count || !read32(mem, f.int_base + 4 * n, &raw)) {
            e->last_ints.push_back(0u);
            continue;
        }
        e->last_ints.push_back(f.has_key ? decodeInt(raw, f.key) : raw);
    }
    for (const std::uint32_t n : e->cap_ptrs) {
        std::uint32_t p = 0;
        std::uint32_t v = 0;
        const bool have_p = n < f.ptr_count && read32(mem, f.ptr_base + 4 * n, &p);
        e->last_ptrs.push_back(have_p ? p : 0u);
        if (have_p && p != 0 && read32(mem, p, &v) && f.has_key) v = decodeInt(v, f.key);
        e->last_targets.push_back(have_p && p != 0 ? v : 0u);
    }
}

Entry* find(std::uint32_t handle) {
    for (Entry& e : state().entries) {
        if (e.id == handle) return &e;
    }
    return nullptr;
}

}  // namespace

const char* instrPatchKindName(InstrPatchKind kind) {
    switch (kind) {
        case InstrPatchKind::PtrDword:
            return "ptrDword";
        case InstrPatchKind::LocalInt:
            return "localInt";
        case InstrPatchKind::GlobalInt:
            return "globalInt";
    }
    return "?";
}

bool instrHooksActive() { return g_active.load(std::memory_order_relaxed); }

std::uint32_t instrHookAdd(std::uint64_t owner, const std::string& script, std::uint32_t pos, const char** err) {
    auto fail = [err](const char* why) {
        if (err != nullptr) *err = why;
        return 0u;
    };
    // ★ S0（缺陷 3）：注入面不存在 ⇒ **当场拒绝**（⛔ 不返回一个永远 hits=0 的句柄）。
    if (state().injection_sites == 0) return fail("no_injection");
    if (script.empty()) return fail("script_empty");
    if (pos == 0) return fail("pos_zero");
    for (const Entry& e : state().entries) {
        if (e.pos == pos && e.script == script) return fail("duplicate");
    }
    Entry entry;
    entry.id = state().next_id++;
    entry.owner = owner;
    entry.script = script;
    entry.pos = pos;
    state().entries.push_back(std::move(entry));
    g_active.store(true, std::memory_order_relaxed);
    if (err != nullptr) *err = nullptr;
    return state().entries.back().id;
}

bool instrHookSetPatch(std::uint32_t handle, bool enabled, InstrPatchKind kind, std::uint32_t index,
                       std::uint32_t value, std::uint32_t max_hits, const char** err) {
    auto fail = [err](const char* why) {
        if (err != nullptr) *err = why;
        return false;
    };
    Entry* e = find(handle);
    if (e == nullptr) return fail("no_such_hook");
    // 换覆盖 / 撤覆盖之前先把还没还原的那次写回去（否则会留下一个半新半旧的窗口）。
    if (state().pending.active && state().pending.entry_id == handle) restorePendingLocked();
    if (!enabled) {
        e->patch = false;
        if (err != nullptr) *err = nullptr;
        return true;
    }
    if (kind == InstrPatchKind::PtrDword && index > 63) return fail("bad_target");
    e->patch = true;
    e->kind = kind;
    e->index = index;
    e->value = value;
    e->max_hits = max_hits;
    if (err != nullptr) *err = nullptr;
    return true;
}

bool instrHookSetCapture(std::uint32_t handle, const std::vector<std::uint32_t>& ints,
                         const std::vector<std::uint32_t>& ptrs) {
    Entry* e = find(handle);
    if (e == nullptr) return false;
    e->cap_ints.assign(ints.begin(), ints.begin() + static_cast<std::ptrdiff_t>(ints.size() < 8 ? ints.size() : 8));
    e->cap_ptrs.assign(ptrs.begin(), ptrs.begin() + static_cast<std::ptrdiff_t>(ptrs.size() < 8 ? ptrs.size() : 8));
    return true;
}

bool instrHookStats(std::uint32_t handle, InstrHookStatus* out) {
    const Entry* e = find(handle);
    if (e == nullptr) return false;
    InstrHookStatus s;
    s.id = e->id;
    s.script = e->script;
    s.pos = e->pos;
    s.patch = e->patch;
    s.kind = e->kind;
    s.index = e->index;
    s.value = e->value;
    s.max_hits = e->max_hits;
    s.hits = e->hits;
    s.restores = e->restores;
    s.restore_conflicts = e->restore_conflicts;
    s.skips = e->skips;
    s.last_id = e->last_id;
    s.last_ints = e->last_ints;
    s.last_ptrs = e->last_ptrs;
    s.last_ptr_targets = e->last_targets;
    *out = std::move(s);
    return true;
}

bool instrHookRemove(std::uint32_t handle) {
    State& st = state();
    if (st.pending.active && st.pending.entry_id == handle) restorePendingLocked();
    for (auto it = st.entries.begin(); it != st.entries.end(); ++it) {
        if (it->id == handle) {
            st.entries.erase(it);
            if (st.entries.empty()) g_active.store(false, std::memory_order_relaxed);
            return true;
        }
    }
    return false;
}

void instrHookReleaseOwner(std::uint64_t owner) {
    State& st = state();
    // ① 未还原的那次覆盖若属于本 owner ⇒ 先按 CAS 结算（别人的 pending 一个字节都不动）。
    //    找不到对应 entry（不该发生）⇒ 也结算：⛔ 不许把一次待恢复的写留在 guest 里。
    if (st.pending.active) {
        const Entry* pending_entry = nullptr;
        for (const Entry& e : st.entries) {
            if (e.id == st.pending.entry_id) {
                pending_entry = &e;
                break;
            }
        }
        if (pending_entry == nullptr || pending_entry->owner == owner) restorePendingLocked();
    }
    // ② 撤掉本 owner 的全部 hook（别的 owner 原样）。
    st.entries.erase(std::remove_if(st.entries.begin(), st.entries.end(),
                                    [owner](const Entry& e) { return e.owner == owner; }),
                     st.entries.end());
    if (st.entries.empty()) g_active.store(false, std::memory_order_relaxed);
}

void instrHookClear() {
    restorePendingLocked();
    State& st = state();
    st.entries.clear();
    g_active.store(false, std::memory_order_relaxed);
}

void instrHookSetInjectionSites(std::uint32_t n) { state().injection_sites = n; }

void instrHookCancelPendingAt(std::uint32_t addr) {
    State& st = state();
    if (!st.pending.active || st.pending.addr != addr) return;
    // ★ 持久写赢过瞬时覆盖：**不**写回 raw（这次写是真的），也**不**记 restore / conflict
    //   —— 什么都没发生，只是那次待恢复作废了（见头文件的口径）。
    st.pending.active = false;
}

std::vector<InstrHookStatus> instrHookList() {
    std::vector<InstrHookStatus> out;
    out.reserve(state().entries.size());
    for (const Entry& e : state().entries) {
        InstrHookStatus s;
        s.id = e.id;
        s.script = e.script;
        s.pos = e.pos;
        s.patch = e.patch;
        s.kind = e.kind;
        s.index = e.index;
        s.value = e.value;
        s.max_hits = e.max_hits;
        s.hits = e.hits;
        s.restores = e.restores;
        s.restore_conflicts = e.restore_conflicts;
        s.skips = e.skips;
        s.last_id = e.last_id;
        s.last_ints = e.last_ints;
        s.last_ptrs = e.last_ptrs;
        s.last_ptr_targets = e.last_targets;
        out.push_back(std::move(s));
    }
    return out;
}

void instrHookProbe(X86& cpu) {
    if (!g_active.load(std::memory_order_relaxed)) return;
    State& st = state();
    if (cpu.mem == nullptr) return;
    st.mem = cpu.mem;

    // ① 先恢复上一条指令的临时写（窗口 = 一条脚本指令）。
    restorePendingLocked();

    Frame f;
    if (!sampleFrame(cpu, &f)) return;

    for (Entry& e : st.entries) {
        if (e.pos != f.pos) continue;
        // ② 身份：首次命中按名字解析并缓存 id；之后只比 id（⛔ 不每次解析名字）。
        if (e.script_id == 0) {
            std::string name;
            if (!resolveScriptName(cpu, f.id, &name)) continue;
            if (name != e.script) continue;
            e.script_id = f.id;
        } else if (e.script_id != f.id) {
            continue;
        }

        e.hits += 1;
        e.last_id = f.id;
        if (!e.cap_ints.empty() || !e.cap_ptrs.empty()) captureLocals(*cpu.mem, f, &e);
        if (!e.patch) continue;
        if (e.max_hits != 0 && e.hits > e.max_hits) continue;

        std::uint32_t addr = 0;
        if (!targetAddress(cpu, f, e, &addr)) {
            e.skips += 1;
            continue;
        }
        std::uint32_t raw = 0;
        if (!read32(*cpu.mem, addr, &raw) || !f.has_key) {
            e.skips += 1;
            continue;
        }
        // ★ S0：`written` 必须留下来 —— 恢复时用它做 CAS 比对（`restorePendingLocked`）。
        const std::uint32_t written = encodeInt(e.value, f.key);
        if (!write32(*cpu.mem, addr, written)) {
            e.skips += 1;
            continue;
        }
        st.pending.active = true;
        st.pending.entry_id = e.id;
        st.pending.addr = addr;
        st.pending.raw = raw;
        // ★ S0：记下**我写进去的**编码值 —— 下一次恢复先比它（CAS，见 `restorePendingLocked`）。
        st.pending.written = written;
        break;  // 一轮派发只允许一条 hook 生效（位置唯一 ⇒ 正常也只有一条命中）
    }
}

}  // namespace lift

// ★ AOT 在 3 个脚本派发点注入的入口（`aot/aot_hook_ea.h` 的 `kScriptDispatchEas`）。
//   缺省只读一个 atomic<bool>；⛔ 不要挪进 `lift_dyn`（那会让每次间接跳转都付费）。
extern "C" void lift_instr_hook(void* cpu_ptr) {
    if (!lift::instrHooksActive()) return;
    if (cpu_ptr == nullptr) return;
    lift::instrHookProbe(*static_cast<lift::X86*>(cpu_ptr));
}
