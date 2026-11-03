// emulator-lift · Machine：装配 + dispatcher + fault 诊断
//
// ★ 执行循环（README §2.2）：`eip → 块缓存 → 顺序执行 → 控制流只改 eip 并回循环`。
//   shim 桩地址也是块缓存里的条目（`Block::is_shim`），所以 `call [IAT]` 与 `call 0x401234`
//   在 dispatcher 眼里是同一件事。
// ★ 停下来的原因全部进 `RunResult`（D4 的 L1/L2 就是 `FirstShim` / `UntilEa` 两个原因）。
#include "runtime/machine.h"

#include <cstdio>
#include <cstring>

#include "runtime/exec.h"
#include "runtime/sha256.h"
#include "shims/register.h"

namespace lift {
namespace {
Machine* g_active = nullptr;
}

Machine* activeMachine() { return g_active; }
void setActiveMachine(Machine* m) { g_active = m; }

const char* stopReasonName(StopReason r) {
    switch (r) {
        case StopReason::None: return "none";
        case StopReason::UntilEa: return "until-ea";
        case StopReason::MaxInsns: return "max-insns";
        case StopReason::FirstShim: return "first-shim";
        case StopReason::Unimplemented: return "unimplemented";
        case StopReason::DecodeError: return "decode-error";
        case StopReason::Stopped: return "stopped";
        case StopReason::NoCode: return "no-code";
    }
    return "?";
}

// fault 诊断钩子：只读**符号表/字符串**（不反汇编 —— 反汇编要读内存，可能再 fault）
void liftFaultHook(const AccessSite& site) {
    Machine* m = g_active;
    if (!m) return;
    const X86& cpu = m->cpu();
    std::fprintf(stderr, "  eip        : %08X\n", cpu.eip);
    const std::string text = m->lst().textAt(cpu.eip);
    std::fprintf(stderr, "  instr      : %s\n", text.empty() ? "(不在 .lst 索引里)" : text.c_str());
    if (m->mem().regionOf(site.ea)) {
        const Region* r = m->mem().regionOf(site.ea);
        std::fprintf(stderr, "  region     : %s %08X-%08X %s\n", regionKindName(r->kind), r->begin, r->end, r->name);
    }
    dumpRegisters(cpu, stderr);
    dumpGuestStack(cpu, stderr, 32);
}

Machine::Machine() : cache_(&decoder_, &mem_) {}
Machine::~Machine() {
    if (g_active == this) g_active = nullptr;
}

void Machine::wire() {
    cpu_.mem = &mem_;
    cpu_.heap = &heap_;
    cpu_.handles = &handles_;
    cpu_.clock = &clock_;
    cpu_.shims = &shims_;
    cpu_.cache = &cache_;
    cpu_.lst = &lst_;
    cpu_.fpu.reset();
}

bool Machine::init(std::string* err) {
    if (!mem_.reserve()) {
        if (err) *err = mem_.lastError();
        return false;
    }
    mem_.setSafeMode(GuestMemory::safeModeFromEnv());
    wire();
    setActiveMachine(this);
    setGuestFaultHook(&liftFaultHook);
    initialized_ = true;
    return true;
}

bool Machine::loadImage(const std::string& exe_path, std::string* err) {
    if (!initialized_) {
        if (err) *err = "Machine 未 init";
        return false;
    }
    if (!loadPeFile(exe_path, &image_, err)) return false;
    exe_path_ = exe_path;
    if (image_.image_base != layout_.image_base) {
        std::fprintf(stderr, "[lift] 镜像基址 %.8X 与缺省 %.8X 不同，按 PE 头为准（M1 不做重定位）\n",
                     image_.image_base, layout_.image_base);
        layout_.image_base = image_.image_base;
    }
    if (!mapPeImage(mem_, image_, err)) return false;
    image_loaded_ = true;
    cpu_.shim.module_handle = image_.image_base;
    return true;
}

bool Machine::loadLst(const std::string& lst_path, std::string* err) { return lst_.load(lst_path, err); }

bool Machine::prepare(std::uint32_t entry_override, std::string* err) {
    if (!image_loaded_) {
        if (err) *err = "还没有载入镜像";
        return false;
    }
    if (!setupProcessStructures(mem_, image_, layout_, &setup_, err)) return false;
    if (!heap_.init(&mem_, layout_.heap_base, layout_.heap_size)) {
        if (err) *err = heap_.lastError();
        return false;
    }
    if (!mem_.commitZeroed(layout_.shim_page, static_cast<std::size_t>(layout_.shim_slots) * 4u,
                           RegionKind::ShimPage, "shim-page")) {
        if (err) *err = "提交 shim 桩页失败";
        return false;
    }
    registerKernel32Shims(shims_);
    registerBootstrapShims(shims_);
    if (!shims_.bind(mem_, image_, layout_.shim_page, layout_.shim_slots, err)) return false;
    for (const ShimDef& d : shims_.all()) {
        if (d.stub_ea) cache_.installShim(d.stub_ea, d.id);
    }

    cpu_.eip = entry_override ? entry_override : image_.entry_ea;
    cpu_.fs_base = setup_.teb;
    cpu_.gs_base = 0;
    cpu_.esp() = setup_.stack_top - 4u;
    mem_.wr32(cpu_.esp(), 0xFFFFFFFFu);  // 栈顶的返回地址哨兵：真回到它就停
    cpu_.shim.cmdline_ea = setup_.cmdline_ea;
    cpu_.shim.exe_path_ea = setup_.exe_path_ea;
    cpu_.shim.va_cursor = 0x20000000u;
    cpu_.shim.va_limit = 0x28000000u;
    return true;
}

bool Machine::initBare(std::string* err) {
    if (!init(err)) return false;
    bare_ = true;
    return true;
}

bool Machine::placeCode(std::uint32_t ea, const void* bytes, std::size_t n, std::string* err) {
    if (!initialized_) {
        if (err) *err = "Machine 未 init";
        return false;
    }
    if (!mem_.commit(ea, n, RegionKind::Image, "fixture-code")) {
        if (err) *err = mem_.lastError();
        return false;
    }
    mem_.writeBytes(ea, bytes, n);
    if (cpu_.eip == 0) cpu_.eip = ea;
    return true;
}

bool Machine::placeData(std::uint32_t ea, const void* bytes, std::size_t n, std::string* err) {
    // ★ 与 placeCode 的唯一区别：**不动 eip**（数据摆放不该决定从哪开始执行）
    const std::uint32_t saved_eip = cpu_.eip;
    if (!placeCode(ea, bytes, n, err)) return false;
    cpu_.eip = saved_eip;
    return true;
}

bool Machine::prepareStack(std::string* err) {
    if (!initialized_) {
        if (err) *err = "Machine 未 init";
        return false;
    }
    const std::uint32_t top = layout_.stack_top;
    const std::uint32_t size = layout_.stack_size;
    if (!mem_.commitZeroed(top - size, size, RegionKind::Stack, "fixture-stack")) {
        if (err) *err = mem_.lastError();
        return false;
    }
    mem_.annotate(top - size - layout_.stack_guard, layout_.stack_guard, RegionKind::StackGuard, "stack-guard");
    cpu_.esp() = top - 4u;
    mem_.wr32(cpu_.esp(), 0xFFFFFFFFu);  // 哨兵
    return true;
}

RunResult Machine::run(const RunOptions& opts) {
    RunResult r;
    shims_.setTrace(opts.trace_shims, stdout);
    // 诊断开关：LIFT_TRACE_FULL=1 ⇒ `--trace-blocks` 每条带操作数文本 + 寄存器快照（缺省关）
    const bool traceFull = std::getenv("LIFT_TRACE_FULL") != nullptr;
    cpu_.running = true;

    while (cpu_.running) {
        if (opts.until_ea && cpu_.eip == opts.until_ea) {
            r.reason = StopReason::UntilEa;
            break;
        }
        if (opts.max_insns && cpu_.insns >= opts.max_insns) {
            r.reason = StopReason::MaxInsns;
            break;
        }

        Block* b = cache_.get(cpu_.eip);
        if (!b) {
            r.reason = StopReason::DecodeError;
            r.detail = cache_.lastError();
            std::fprintf(stderr, "\n=== 最近执行的 %d 条指令（停在 %08X）===\n", cpu_.last_count, cpu_.eip);
            for (int i = 0; i < cpu_.last_count; ++i) {
                const int idx = (cpu_.last_head - 1 - i + 2 * X86::kLastInsns) % X86::kLastInsns;
                std::fprintf(stderr, "  %08X %s   (eflags=%08X)\n", cpu_.last_eip[idx], cpu_.last_mnemonic[idx] ? cpu_.last_mnemonic[idx] : "?", cpu_.last_flags[idx]);
            }
            std::fprintf(stderr, "  --- 反汇编（含操作数）---\n");
            for (int i = 0; i < 8 && i < cpu_.last_count; ++i) {
                const int idx = (cpu_.last_head - 1 - i + 2 * X86::kLastInsns) % X86::kLastInsns;
                std::fprintf(stderr, "  %08X %s\n", cpu_.last_eip[idx], decoder_.text(mem_, cpu_.last_eip[idx]).c_str());
            }
            break;
        }
        if (!b->is_shim && b->insns.empty()) {
            // 空块只可能是"解码成功却没有指令"（不该发生）⇒ 响亮
            r.reason = StopReason::NoCode;
            r.detail = "空块（解码没有产出任何指令）";
            break;
        }
        if (b->is_shim) {
            if (opts.stop_on_first_shim) {
                r.reason = StopReason::FirstShim;
                break;
            }
            ShimDef* def = const_cast<ShimDef*>(shims_.byId(b->shim_id));
            if (!def) {
                r.reason = StopReason::NoCode;
                r.detail = "shim 块指向了不存在的 id";
                break;
            }
            const bool had_impl = def->fn != nullptr;
            shims_.invoke(cpu_, *def);
            if (!cpu_.running) {
                // ★ 两种"停"要分清：**未实现**是缺口（红），**实现里主动停**（ExitProcess / ExitThread）
                //   是正常结局。混在一起会让 ExitProcess 看起来像缺 shim。
                if (!had_impl) {
                    r.reason = StopReason::Unimplemented;
                    r.detail = std::string("未实现的导入 ") + def->module + "!" + def->name;
                } else {
                    r.reason = StopReason::Stopped;
                    r.detail = std::string("shim 主动结束运行：") + def->module + "!" + def->name;
                }
                break;
            }
            continue;
        }

        bool left_block = false;
        for (const Insn& in : b->insns) {
            if (opts.until_ea && in.ea == opts.until_ea) {
                r.reason = StopReason::UntilEa;
                left_block = true;
                break;
            }
            if (opts.max_insns && cpu_.insns >= opts.max_insns) {
                r.reason = StopReason::MaxInsns;
                left_block = true;
                break;
            }
            if (opts.trace_blocks) {
                // ★ 诊断开关（只在 env 存在时改变输出；缺省格式一字不改）：
                //   LIFT_TRACE_FULL=1 ⇒ 每条带上**带操作数的反汇编文本** + esp/eflags/关键寄存器，
                //   因为只打助记符看不出"这条 call 的目标是谁 / 立即数是多少"。
                if (traceFull) {
                    std::fprintf(stdout, "[insn] %08X esp=%08X eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X eflags=%08X | %s\n",
                                 in.ea, cpu_.esp(), cpu_.readReg(Reg::EAX), cpu_.readReg(Reg::EBX),
                                 cpu_.readReg(Reg::ECX), cpu_.readReg(Reg::EDX), cpu_.readReg(Reg::ESI),
                                 cpu_.readReg(Reg::EDI), cpu_.eflags, decoder_.text(mem_, in.ea).c_str());
                } else {
                    std::fprintf(stdout, "[insn] %08X %s\n", in.ea, in.mnemonic ? in.mnemonic : "?");
                }
            }
            const bool control = opIsControl(in.op);
            cpu_.noteInsn(in.ea, in.mnemonic);
            cpu_.eip = in.ea;
            if (!control) cpu_.eip = in.ea + in.length;
            const bool ok = execInsn(cpu_, in);
            if (!ok) {
                // 失败的这条不算"执行过"：eip 指回它，方便诊断与 A4 的指令计数
                cpu_.eip = in.ea;
                r.reason = StopReason::Unimplemented;
                r.detail = cpu_.stop_why ? cpu_.stop_why : "指令执行失败";
                left_block = true;
                break;
            }
            ++cpu_.insns;
            clock_.tick();
            if (control) {
                left_block = true;
                break;
            }
        }
        if (left_block) continue;
    }
    if (r.reason == StopReason::UntilEa && opts.until_ea == 0xFFFFFFFFu) {
        // 岗哨返回地址 = 夹具的正常结束，不改口径（调用方自己看 reason）
    }
    r.ea = cpu_.eip;
    r.insns = cpu_.insns;
    return r;
}

void Machine::reportBaseGaps(std::FILE* out) const {
    const std::vector<const ShimDef*> gaps = shims_.importedButUnimplemented();
    std::fprintf(out, "导入总数 = %zu, 已实现 = %zu, 缺口 = %zu\n", shims_.importedCount(),
                 shims_.importedCount() - gaps.size(), gaps.size());
    for (const ShimDef* d : gaps) {
        std::fprintf(out, "  缺口 %s!%s  —— %s\n", d->module.c_str(), d->name.c_str(),
                     d->reason ? d->reason : "(未登记理由)");
    }
}

void Machine::dumpRunSummary(const RunResult& r, std::FILE* out) const {
    if (r.reason != StopReason::UntilEa && r.reason != StopReason::MaxInsns && cpu_.last_count > 0) {
        std::fprintf(out, "--- 最近执行的指令（最新在前）---\n");
        for (int i = 0; i < cpu_.last_count && i < 12; ++i) {
            const int idx = (cpu_.last_head - 1 - i + 2 * X86::kLastInsns) % X86::kLastInsns;
            std::fprintf(out, "  %08X %s\n", cpu_.last_eip[idx], cpu_.last_mnemonic[idx] ? cpu_.last_mnemonic[idx] : "?");
        }
    }
    std::fprintf(out, "stop      : %s @ eip=%08X\n", stopReasonName(r.reason), r.ea);
    if (!r.detail.empty()) std::fprintf(out, "detail    : %s\n", r.detail.c_str());
    std::fprintf(out, "insns     : %llu\n", (unsigned long long)r.insns);
    std::fprintf(out, "blocks    : %zu (insns cached %zu, hits %zu, misses %zu)\n", cache_.blockCount(),
                 cache_.insnCount(), cache_.hits(), cache_.misses());
    std::fprintf(out, "committed : %llu KiB\n", (unsigned long long)(mem_.committedBytes() / 1024));
    std::fprintf(out, "shims     : imported %zu / called-unimplemented %zu\n", shims_.importedCount(),
                 shims_.calledUnimplemented().size());
    std::fprintf(out, "heap      : in_use %u peak %u allocs %u\n", heap_.bytesInUse(), heap_.peak(),
                 heap_.allocCount());
}

}  // namespace lift
