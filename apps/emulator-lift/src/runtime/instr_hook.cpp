#include "runtime/instr_hook.h"

#include <atomic>
#include <cstring>

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
    InstrHookSpec spec;
    std::uint32_t script_id = 0;  // 0 = 还没解析（首次命中时按名字解析并缓存）
    std::uint64_t hits = 0;
    std::uint64_t restores = 0;
    std::uint64_t skips = 0;
    std::uint32_t last_id = 0;
    std::vector<std::uint32_t> last_ints;
    std::vector<std::uint32_t> last_ptrs;
    std::vector<std::uint32_t> last_targets;
};

struct Pending {
    bool active = false;
    std::size_t entry = 0;
    std::uint32_t addr = 0;
    std::uint32_t raw = 0;
};

struct State {
    std::vector<Entry> entries;
    Pending pending;
    // 最近一次 probe 用到的 guest 内存（`clear()` 在没有 X86 时也能把未还原的写恢复 ⇒ 净变化 0）。
    GuestMemory* mem = nullptr;
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

bool readPointer(const GuestMemory& mem, std::uint32_t ea, std::uint32_t* out) {
    return read32(mem, ea, out);
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

void restorePending(X86& cpu) {
    State& st = state();
    if (!st.pending.active) return;
    if (cpu.mem != nullptr && write32(*cpu.mem, st.pending.addr, st.pending.raw)) {
        if (st.pending.entry < st.entries.size()) st.entries[st.pending.entry].restores += 1;
    }
    st.pending.active = false;
}

bool targetAddress(X86& cpu, const Frame& f, const InstrHookSpec& spec, std::uint32_t* addr) {
    GuestMemory& mem = *cpu.mem;
    switch (spec.kind) {
        case InstrPatchKind::PtrDword: {
            if (spec.index >= f.ptr_count) return false;
            std::uint32_t p = 0;
            if (!readPointer(mem, f.ptr_base + 4 * spec.index, &p) || p == 0) return false;
            *addr = p;
            return true;
        }
        case InstrPatchKind::LocalInt: {
            if (spec.index >= f.int_count) return false;
            *addr = f.int_base + 4 * spec.index;
            return true;
        }
        case InstrPatchKind::GlobalInt: {
            std::uint32_t base = 0;
            std::uint32_t count = 0;
            if (!read32(mem, f.engine + kGlobalIntBase, &base) || base == 0) return false;
            if (!read32(mem, f.engine + kGlobalIntCount, &count)) return false;
            if (spec.index > count) return false;
            *addr = base + 4 * spec.index;
            return true;
        }
    }
    return false;
}

void captureLocals(const GuestMemory& mem, const Frame& f, const InstrHookSpec& spec, Entry* entry) {
    entry->last_ints.clear();
    entry->last_ptrs.clear();
    entry->last_targets.clear();
    for (const std::uint32_t n : spec.capture_ints) {
        std::uint32_t raw = 0;
        if (n >= f.int_count || !read32(mem, f.int_base + 4 * n, &raw)) {
            entry->last_ints.push_back(0u);
            continue;
        }
        entry->last_ints.push_back(f.has_key ? decodeInt(raw, f.key) : raw);
    }
    for (const std::uint32_t n : spec.capture_ptrs) {
        std::uint32_t p = 0;
        std::uint32_t v = 0;
        const bool have_p = n < f.ptr_count && read32(mem, f.ptr_base + 4 * n, &p);
        entry->last_ptrs.push_back(have_p ? p : 0u);
        if (have_p && p != 0 && read32(mem, p, &v) && f.has_key) v = decodeInt(v, f.key);
        entry->last_targets.push_back(have_p && p != 0 ? v : 0u);
    }
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

bool instrHookInstall(const InstrHookSpec& spec, const char** err) {
    auto fail = [err](const char* why) {
        if (err != nullptr) *err = why;
        return false;
    };
    if (spec.script.empty()) return fail("script_empty");
    if (spec.pos == 0) return fail("pos_zero");
    for (const Entry& e : state().entries) {
        if (e.spec.pos == spec.pos && e.spec.script == spec.script) return fail("duplicate");
    }
    Entry entry;
    entry.spec = spec;
    state().entries.push_back(std::move(entry));
    g_active.store(true, std::memory_order_relaxed);
    if (err != nullptr) *err = nullptr;
    return true;
}

void instrHookClear() {
    State& st = state();
    // ★ 不能只清表：未还原的写会变成**持久**改动（那正是本能力要避免的）。
    //   用最近一次 probe 的 guest 内存把 raw 写回；没有内存（还没跑过）⇒ 没有未还原的写。
    if (st.pending.active && st.mem != nullptr) {
        if (write32(*st.mem, st.pending.addr, st.pending.raw)) {
            if (st.pending.entry < st.entries.size()) st.entries[st.pending.entry].restores += 1;
        }
    }
    st.pending.active = false;
    st.entries.clear();
    g_active.store(false, std::memory_order_relaxed);
}

std::vector<InstrHookStatus> instrHookStatus() {
    std::vector<InstrHookStatus> out;
    out.reserve(state().entries.size());
    for (const Entry& e : state().entries) {
        InstrHookStatus s;
        s.script = e.spec.script;
        s.pos = e.spec.pos;
        s.patch = e.spec.patch;
        s.kind = e.spec.kind;
        s.index = e.spec.index;
        s.value = e.spec.value;
        s.hits = e.hits;
        s.restores = e.restores;
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
    restorePending(cpu);

    Frame f;
    if (!sampleFrame(cpu, &f)) return;

    for (std::size_t i = 0; i < st.entries.size(); i += 1) {
        Entry& e = st.entries[i];
        if (e.spec.pos != f.pos) continue;
        // ② 身份：首次命中按名字解析并缓存 id；之后只比 id（⛔ 不每次解析名字）。
        if (e.script_id == 0) {
            std::string name;
            if (!resolveScriptName(cpu, f.id, &name)) continue;
            if (name != e.spec.script) continue;
            e.script_id = f.id;
        } else if (e.script_id != f.id) {
            continue;
        }

        e.hits += 1;
        e.last_id = f.id;
        if (!e.spec.capture_ints.empty() || !e.spec.capture_ptrs.empty()) {
            captureLocals(*cpu.mem, f, e.spec, &e);
        }
        if (!e.spec.patch) continue;
        if (e.spec.maxHits != 0 && e.hits > e.spec.maxHits) continue;

        std::uint32_t addr = 0;
        if (!targetAddress(cpu, f, e.spec, &addr)) {
            e.skips += 1;
            continue;
        }
        std::uint32_t raw = 0;
        if (!read32(*cpu.mem, addr, &raw)) {
            e.skips += 1;
            continue;
        }
        if (!f.has_key) {
            e.skips += 1;
            continue;
        }
        if (!write32(*cpu.mem, addr, encodeInt(e.spec.value, f.key))) {
            e.skips += 1;
            continue;
        }
        st.pending.active = true;
        st.pending.entry = i;
        st.pending.addr = addr;
        st.pending.raw = raw;
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
