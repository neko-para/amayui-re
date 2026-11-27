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
    std::uint32_t pc = 0;  // ★ S2a：命中分发要在 PC 上读 opcode（`InstrHit::opcode`）
    // ★ S2c：opcode 的读闸门（`{opcode}` 订阅存在 / 命中后要 materialize）与读到的值。
    std::uint32_t opcode = 0;
    bool opcode_ok = false;
    std::uint32_t int_base = 0;
    std::uint32_t int_count = 0;
    std::uint32_t ptr_base = 0;
    std::uint32_t ptr_count = 0;
    std::uint32_t key = 0;
    bool has_key = false;
    // ★ S2c：脚本名缓存 —— 一次派发**最多**解析一次（`MarkerSampler::sampleIdentity` + `resolveName`）。
    //   `{script}` / 未缓存的 `script_id` 匹配都靠它；`InstrHit::script`（= Tick.now.script）也取它。
    bool name_tried = false;
    bool name_ok = false;
    std::string name;
};

struct Entry {
    std::uint32_t id = 0;
    // ★ S0：归属（`PluginHost` 传本 Run 的序号）⇒ Run 收口能只撤自己那批（见 `instrHookReleaseOwner`）。
    std::uint64_t owner = 0;
    // ★ S2c：用途 + 形状（去重 / 链 / matching 各读自己那几位；见头文件的三种 kind）。
    //   ⛔ 不能叫 `kind`：下面还有 **patch** 的 `InstrPatchKind kind`（v1 语义，逐字保留）。
    InstrHookKind hook_kind = InstrHookKind::V1At;
    InstrHookForm form = InstrHookForm::Pos;
    std::string script;
    std::uint32_t script_id = 0;  // 0 = 还没解析（首次命中时按名字解析并缓存）
    std::uint32_t pos = 0;        // Pos（其余 form 保持 0 —— form 决定哪个字段有意义）
    std::uint32_t lo = 0;         // Range
    std::uint32_t hi = 0;         // Range
    std::uint32_t opcode = 0;     // Opcode
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
    // ★ S2a：反向 sink（runtime → ui）。**不拥有** `user`；分发与通知都在本文件的锁外口径上
    //   （与 entries 同一份"无锁"事实，见施工图 §未证 1/3）。
    std::vector<InstrHookSink> sinks;
    // ★ S0（缺陷 3）：本构建注入的 `lift_instr_hook` 站点数。缺省 1 = "视作可用"
    //   （纯 runtime / 测试夹具没有 AOT 形状；只有 AOT 的初始化路径会把它改成真实值）。
    std::uint32_t injection_sites = 1;
    // ★ S2c：表里**有没有** `{opcode}` 订阅（probe 的 opcode 读闸门）。
    //   `recomputeGates()` 在 add / remove / releaseOwner / clear 之后维护；没有 opcode 订阅时
    //   probe 不必每拍多付一次 `rd32(pc)`（`Want sink` 那一次仍然只在**命中后**才付）。
    bool has_opcode_entry = false;
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
    f.pc = pc;  // ★ S2a：命中分发要靠它读 opcode（`InstrHit::opcode`）
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

/**
 * ★ S2c：`Frame` 的名字缓存（一次派发**最多**解析一次）。
 * 为什么需要：`{script}` / `{opcode}` 形态（以及首次命中前的 `{script,pos}`）都要按名字判定；
 * 而 `sampleIdentity` + `resolveName` 是十几次 guest 读 —— 多条订阅同拍命中时不能各付一次。
 * 命中缓存（`script_id`）之后只比 id，连这一次都不付。
 */
bool frameName(X86& cpu, Frame* f) {
    if (f->name_tried) return f->name_ok;
    f->name_tried = true;
    std::string out;
    f->name_ok = resolveScriptName(cpu, f->id, &out);
    if (f->name_ok) f->name = std::move(out);
    return f->name_ok;
}

/**
 * ★ S2c：一条 Entry 是否命中这一拍。`f` **非 const**：名字缓存要写回 `Frame`。
 * ① form 的**零读**判据先走；② `{opcode}` 才判 opcode（读不到 ⇒ 不命中，⛔ 不编 0）；
 * ③ 身份：注册时按名字解析一次并缓存 `script_id`，之后只比 id（缓存写回留回 probe 的循环体 ——
 *   `e` 是 const，这里只判定）。
 * ★ 四种 form 在 S2c-1 一次写全（纯判定、零额外成本，C++ 用例直接驱动）；**JS 面只放开 Pos**。
 */
bool matchEntry(X86& cpu, Frame& f, const Entry& e) {
    switch (e.form) {
        case InstrHookForm::Pos:
            if (f.pos != e.pos) return false;
            break;
        case InstrHookForm::Range:
            if (f.pos < e.lo || f.pos >= e.hi) return false;
            break;
        case InstrHookForm::Script:
            break;  // 没有 pos 判据（整本脚本每一拍都命中）
        case InstrHookForm::Opcode:
            break;  // 交给 ②
    }
    if (e.form == InstrHookForm::Opcode) {
        if (!f.opcode_ok) return false;
        if (f.opcode != e.opcode) return false;
    }
    if (!e.script.empty()) {
        if (e.script_id == 0) {
            if (!frameName(cpu, &f)) return false;
            if (f.name != e.script) return false;
        } else if (e.script_id != f.id) {
            return false;
        }
    }
    return true;
}

/** 维护 `State::has_opcode_entry`（O(n)，n = 订阅数个位数）。add / remove / releaseOwner / clear 之后调。 */
void recomputeGates() {
    State& st = state();
    bool opcode = false;
    for (const Entry& e : st.entries) {
        if (e.form == InstrHookForm::Opcode) {
            opcode = true;
            break;
        }
    }
    st.has_opcode_entry = opcode;
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

/**
 * ★ S2a：条目被摘掉时逐条通知 sink（ui 层据此释放对应的 JS handler）。
 * 调用纪律：**必须在 `erase` / `remove_if` / `clear` 之后**调（⛔ 不要在谓词里调 ——
 * sink 会跑 JS，JS 一旦能改表就会让遍历中的容器失效）；`id` 由调用方先存成局部值。
 */
void notifyReleased(std::uint32_t handle) {
    for (const InstrHookSink& s : state().sinks) {
        if (s.on_released != nullptr) s.on_released(s.user, handle);
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

void instrHookAddSink(const InstrHookSink& sink) {
    State& st = state();
    // 同一个 `user` 幂等（重复注册 = 替换）：保证"实例重建后不会留下两条同 user 的 sink"。
    for (InstrHookSink& s : st.sinks) {
        if (s.user == sink.user) {
            s = sink;
            return;
        }
    }
    st.sinks.push_back(sink);
}

void instrHookRemoveSink(void* user) {
    State& st = state();
    st.sinks.erase(std::remove_if(st.sinks.begin(), st.sinks.end(),
                                  [user](const InstrHookSink& s) { return s.user == user; }),
                   st.sinks.end());
}

bool instrHookHasSink() { return !state().sinks.empty(); }

std::uint32_t instrHookAdd(std::uint64_t owner, const std::string& script, std::uint32_t pos, const char** err) {
    // ★ S2c：v1 的 `at(script,pos)` = `instrHookAddEx` 的**薄包装**（Pos + V1At）。判据逐字不变：
    //   v1 的 `duplicate` 只对 `kind == V1At` 生效（见 `instrHookAddEx`）⇒ 别的 shape 不受影响。
    InstrHookWhere where;
    where.form = InstrHookForm::Pos;
    where.script = script;
    where.pos = pos;
    return instrHookAddEx(owner, where, InstrHookKind::V1At, err);
}

std::uint32_t instrHookAddEx(std::uint64_t owner, const InstrHookWhere& where, InstrHookKind kind,
                             const char** err) {
    auto fail = [err](const char* why) {
        if (err != nullptr) *err = why;
        return 0u;
    };
    // ★ S0（缺陷 3）：注入面不存在 ⇒ **当场拒绝**（⛔ 不返回一个永远 hits=0 的句柄）。
    if (state().injection_sites == 0) return fail("no_injection");
    // 形状校验：`form` 决定哪几个字段有意义（⛔ 不做"模糊推断"）。
    switch (where.form) {
        case InstrHookForm::Pos:
            if (where.script.empty()) return fail("script_empty");
            if (where.pos == 0) return fail("pos_zero");
            break;
        case InstrHookForm::Range:
            if (where.script.empty()) return fail("script_empty");
            if (where.lo >= where.hi) return fail("range_empty");
            break;
        case InstrHookForm::Script:
            // ★ 整本脚本：`script` 空 = "所有脚本的每一拍"，那是**没有过滤**的全局订阅 —— 不是
            //   一个可用的形状（S2c 的 JS 面也不开放）⇒ 与 `{}` 同一条 `bad_where`。
            if (where.script.empty()) return fail("bad_where");
            break;
        case InstrHookForm::Opcode:
            break;  // opcode 可带可选 script（`{script, opcode}`）
        default:
            return fail("bad_where");
    }
    // ★ 去重**只对 v1**：`(script,pos)` 一条是已发布语义（合同 §4.5）；v2 的 Sub / Once 是
    //   新面，同点多条正是它的用法（多 handler / 链）。
    if (kind == InstrHookKind::V1At) {
        for (const Entry& e : state().entries) {
            if (e.hook_kind == InstrHookKind::V1At && e.pos == where.pos && e.script == where.script) {
                return fail("duplicate");
            }
        }
    }
    Entry entry;
    entry.id = state().next_id++;
    entry.owner = owner;
    entry.hook_kind = kind;
    entry.form = where.form;
    entry.script = where.script;
    entry.pos = where.form == InstrHookForm::Pos ? where.pos : 0u;
    entry.lo = where.lo;
    entry.hi = where.hi;
    entry.opcode = where.opcode;
    state().entries.push_back(std::move(entry));
    g_active.store(true, std::memory_order_relaxed);
    recomputeGates();
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
    s.hook_kind = e->hook_kind;
    s.form = e->form;
    s.lo = e->lo;
    s.hi = e->hi;
    s.opcode = e->opcode;
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
            recomputeGates();
            // ★ S2a：**摘完再通知** —— sink 会跑 JS，JS 在 S2c 之后可能改表 ⇒ 不能让它在
            //   遍历 / 谓词里被调（见 `notifyReleased` 的调用纪律）。
            notifyReleased(handle);
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
    // ★ S2a：先把要摘的 id 收集到局部（⛔ 不在 `remove_if` 的谓词里调 sink）；
    //   摘完之后逐条 `notifyReleased` ⇒ ui 释放对应的 JS handler。
    std::vector<std::uint32_t> released;
    for (const Entry& e : st.entries) {
        if (e.owner == owner) released.push_back(e.id);
    }
    st.entries.erase(std::remove_if(st.entries.begin(), st.entries.end(),
                                    [owner](const Entry& e) { return e.owner == owner; }),
                     st.entries.end());
    if (st.entries.empty()) g_active.store(false, std::memory_order_relaxed);
    recomputeGates();
    for (const std::uint32_t id : released) notifyReleased(id);
}

void instrHookClear() {
    restorePendingLocked();
    State& st = state();
    // ★ S2a：同上 —— 先存 id、再 `clear`、最后逐条通知（sink 会跑 JS，⛔ 不在清表过程中调）。
    std::vector<std::uint32_t> released;
    released.reserve(st.entries.size());
    for (const Entry& e : st.entries) released.push_back(e.id);
    st.entries.clear();
    g_active.store(false, std::memory_order_relaxed);
    recomputeGates();
    for (const std::uint32_t id : released) notifyReleased(id);
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
        s.hook_kind = e.hook_kind;
        s.form = e.form;
        s.lo = e.lo;
        s.hi = e.hi;
        s.opcode = e.opcode;
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

    // ★ S2a：有 sink 才收集命中 id。为什么**不在遍历里**直接分发：
    //   sink 会跑 JS；JS 一旦能调 `instrHookAdd`（`push_back` ⇒ 重分配）或 `instrHookRemove`
    //   （`erase`），遍历中的 `Entry& e` 立刻悬垂 ⇒ UB 且没有症状。把分发挪到循环结束之后，
    //   就把它从"语义门"变成**结构保证**（S2c 放开写能力也不会踩）。
    const bool want_sink = instrHookHasSink();
    // ★ S2c：opcode 读闸门 —— 只有表里**存在** `{opcode}` 订阅时才每拍多付一次 `rd32(pc)`
    //   （`recomputeGates()` 维护；S2c-1 的 JS 面不开这个形态 ⇒ 正常为 false）。命中之后要
    //   materialize `InstrHit::opcode` 时另读一次（与 S2a 同一次开销，见下）。
    if (st.has_opcode_entry) f.opcode_ok = read32(*cpu.mem, f.pc, &f.opcode);

    std::uint32_t hit_ids[8];
    std::size_t n_hits = 0;
    std::vector<std::uint32_t> hit_overflow;  // 只在 >8 命中时才分配（S2c 起可达：同点多条）

    for (Entry& e : st.entries) {
        // ★ S2c：四种 form 的统一判定（v1 的 Pos 仍是"先比 pos"那一支，代价同阶）。
        if (!matchEntry(cpu, f, e)) continue;
        // 身份缓存写回：首次命中按名字解析过（`matchEntry` 里），之后只比 id。
        if (e.script_id == 0 && !e.script.empty()) e.script_id = f.id;

        e.hits += 1;
        e.last_id = f.id;
        // ★★ S2a：记命中 id 必须在**这里**（`e.hits += 1` 之后、任何 `continue` / `break` 之前）：
        //    只读订阅（没装 patch）会在下面的 `if (!e.patch) continue;` 走掉 ⇒ 写在循环体尾部
        //    就永远记不到（`instr-hook/sink-hit-fields` 会红）。
        if (want_sink) {
            if (n_hits < 8) {
                hit_ids[n_hits++] = e.id;
            } else {
                hit_overflow.push_back(e.id);
            }
        }
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
    if (!want_sink || (n_hits == 0 && hit_overflow.empty())) return;

    // ★ 命中之后才付的两次读：
    //   ① opcode（`InstrHit::opcode`；`{opcode}` 订阅已经读过就复用）—— 读不到 ⇒ 缺键，⛔ 不编 0；
    //   ② 本次派发的脚本名（`Tick.now.script`）—— `{script,pos}` 命中缓存之后 `matchEntry` 只比 id，
    //      `Frame::name` 可能还没解析；这里补一次（`Frame` 缓存 ⇒ 每拍最多一次）。
    if (!f.opcode_ok) f.opcode_ok = read32(*cpu.mem, f.pc, &f.opcode);
    if (!f.name_ok) (void)frameName(cpu, &f);

    // ★ ④ 物化 InstrHit（**遍历之后**：此时没有任何 `Entry&` 活着；后续 handler 可以安全改表）。
    const std::size_t total = n_hits + hit_overflow.size();
    InstrHit small[8];
    std::vector<InstrHit> big;
    if (total > 8) big.resize(total);  // 只在 > 8 命中时才分配
    InstrHit* hits = total > 8 ? big.data() : small;
    std::size_t n = 0;
    const auto fill = [&](std::uint32_t id) {
        // 重新查（S2a 的原写法）：遍历之后表可能已被 patch 路径之外的东西改过。
        Entry* e = find(id);
        if (e == nullptr) return;
        InstrHit& h = hits[n++];
        h.handle = e->id;
        h.script_id = f.id;
        h.script = f.name_ok ? f.name.c_str() : "";  // ★ 这次派发的脚本名（见 InstrHit 头注）
        h.pos = f.pos;
        h.opcode = f.opcode;
        h.opcode_ok = f.opcode_ok;
        h.hits = e->hits;
        h.int_base = f.int_base;
        h.int_count = f.int_count;
        h.ptr_base = f.ptr_base;
        h.ptr_count = f.ptr_count;
        h.codec_key = f.key;
        h.codec_ok = f.has_key;
    };
    for (std::size_t i = 0; i < n_hits; ++i) fill(hit_ids[i]);
    for (const std::uint32_t id : hit_overflow) fill(id);
    if (n == 0) return;
    // ★ ⑤ 一次派发**一次** `on_hits`（注册序 = `hits[0..n)`）—— 链需要"这批就是全部"这个事实。
    for (const InstrHookSink& s : st.sinks) {
        if (s.on_hits != nullptr) s.on_hits(s.user, hits, n);
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
