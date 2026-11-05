// emulator-lift · Machine：装配 + dispatcher + fault 诊断
//
// ★ 执行循环（README §2.2）：`eip → 块缓存 → 顺序执行 → 控制流只改 eip 并回循环`。
//   shim 桩地址也是块缓存里的条目（`Block::is_shim`），所以 `call [IAT]` 与 `call 0x401234`
//   在 dispatcher 眼里是同一件事。
// ★ 停下来的原因全部进 `RunResult`（D4 的 L1/L2 就是 `FirstShim` / `UntilEa` 两个原因）。
#include "runtime/machine.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

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
        case StopReason::TimeLimit: return "time-limit";
        case StopReason::MemoryFault: return "memory-fault";
    }
    return "?";
}

const char* cpuBackendName(CpuBackend b) {
    switch (b) {
        case CpuBackend::Interp: return "interp";
        case CpuBackend::Unicorn: return "unicorn";
    }
    return "?";
}

bool unicornAvailable() {
#if defined(LIFT_WITH_UNICORN)
    return true;
#else
    return false;
#endif
}

// ★ 没有 Unicorn 的构建：`runUnicorn` 仍然有定义（可读的失败，而不是链接错误）。
//   ⛔ 它**不会**回退到解释器 —— "选了 unicorn 却跑了 interp" 是最坏的静默失败。
#if !defined(LIFT_WITH_UNICORN)
RunResult Machine::runUnicorn(const RunOptions& opts) {
    (void)opts;
    RunResult r;
    r.reason = StopReason::Unimplemented;
    r.ea = cpu_.eip;
    r.insns_counted = false;
    r.detail = "本构建没有编译 Unicorn 后端（需 -DLIFT_ENABLE_UNICORN=ON + vcpkg feature unicorn）";
    return r;
}
#endif

// fault 诊断钩子：只读**符号表/字符串**（不反汇编 —— 反汇编要读内存，可能再 fault）。
// ★ **零分配**（REQ-01M6JFZFN15MBPG7Y89WNZACFW #4）：本函数在平台异常 / 信号上下文里被调用，
//   `std::string` 的分配在那里不安全 ⇒ 文本走 `LstIndex::textPtr`（内部指针），不拷不建。
void liftFaultHook(const AccessSite& site) {
    Machine* m = g_active;
    if (!m) return;
    const X86& cpu = m->cpu();
    std::fprintf(stderr, "  eip        : %08X\n", cpu.eip);
    const char* const text = m->lst().textPtr(cpu.eip);
    std::fprintf(stderr, "  instr      : %s\n", (text && text[0]) ? text : "(不在 .lst 索引里)");
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
    cpu_.vfs = &vfs_;
    cpu_.cache = &cache_;
    cpu_.lst = &lst_;
    cpu_.image = &image_;
    cpu_.calls = &guest_calls_;
    cpu_.windows = &windows_;
    cpu_.com = &com_;
    cpu_.fonts = &fonts_;
    cpu_.fpu.reset();
}

bool Machine::init(std::string* err) {
    if (!mem_.reserve()) {
        if (err) *err = mem_.lastError();
        return false;
    }
    mem_.setSafeMode(GuestMemory::safeModeFromEnv());
    if (!surfaces_.init(&mem_, 0x28000000u, 0x08000000u)) {
        if (err) *err = std::string("surface arena 初始化失败：") + surfaces_.lastError();
        return false;
    }
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
    cpu_.shim.module_size = image_.size_of_image;
    return true;
}

bool Machine::loadLst(const std::string& lst_path, std::string* err) { return lst_.load(lst_path, err); }

// ★ AGERC.DLL 的真实装载（2026-11-04 用户改口径）：把它当**普通 guest 模块**——
//   节映射进 guest、真实 ImageBase 当 HINSTANCE、资源 / 版本从这份镜像解析。
//   ⛔ 不再造"假实例 / 假句柄"；没有镜像时 LoadLibrary 返回 0 + 126（见 kernel32_process.cpp）。
bool Machine::loadAgercModule(const std::string& path, std::string* err) {
    auto setErr = [err](const char* step, const std::string& why) {
        if (err) *err = std::string("AGERC 装载失败（") + step + "）：" + why;
    };
    if (!initialized_) {
        setErr("init", "Machine 未 init");
        return false;
    }
    PeImage img;
    std::string perr;
    if (!loadPeFile(path, &img, &perr)) {
        setErr("读/解析 PE", perr + "（路径 " + path + "）");
        return false;
    }
    if (img.size_of_image == 0) {
        setErr("校验 PE", "SizeOfImage 为 0（无法确定映射长度）");
        return false;
    }

    // ---- 选一个不与现有 guest 布局冲突的 base ----
    // ★ 优先用 PE 头里的 ImageBase（真机 loader 也尽量尊重它）；冲突 ⇒ 用 0x30000000。
    //   冲突表 = 主镜像 / 堆 / 栈（含 guard）/ TEB·PEB·TLS / 命令行块 / 桩页 / VirtualAlloc 区间
    //   + **已经提交或登记过的所有区域**（bare 夹具的 placeCode、上一份 AGERC 等）。
    constexpr std::uint32_t kFallbackBase = 0x30000000u;
    struct Range {
        std::uint64_t begin = 0;
        std::uint64_t end = 0;
    };
    std::vector<Range> used;
    auto addRange = [&used](std::uint64_t begin, std::uint64_t end) {
        if (end > begin) used.push_back({begin, end});
    };
    if (image_loaded_) addRange(image_.image_base, static_cast<std::uint64_t>(image_.image_base) + image_.size_of_image);
    addRange(layout_.heap_base, static_cast<std::uint64_t>(layout_.heap_base) + layout_.heap_size);
    addRange(static_cast<std::uint64_t>(layout_.stack_top) - layout_.stack_size - layout_.stack_guard,
             layout_.stack_top);
    addRange(layout_.teb_ea, static_cast<std::uint64_t>(layout_.teb_ea) + 0x1000u);
    addRange(layout_.peb_ea, static_cast<std::uint64_t>(layout_.peb_ea) + 0x1000u);
    addRange(layout_.tls_array_ea, static_cast<std::uint64_t>(layout_.tls_array_ea) + 0x1000u);
    addRange(layout_.argv_block, static_cast<std::uint64_t>(layout_.argv_block) + 0x1000u);
    addRange(layout_.shim_page, static_cast<std::uint64_t>(layout_.shim_page) +
                                    static_cast<std::uint64_t>(layout_.shim_slots) * 4u);
    addRange(cpu_.shim.va_cursor, cpu_.shim.va_limit);
    // 假模块句柄空间（kernel32_process.cpp 的 `0x7F000000 + 序号*64KiB`）：AGERC 的 base 不能落在
    // 里面，否则 `GetProcAddress` 的"句柄 → 模块"反查会与某个假句柄撞（同一个整数两种含义）。
    addRange(0x7F000000u, 0x7FF00000u);
    for (const Region& r : mem_.regions()) addRange(r.begin, r.end);

    auto fits = [&](std::uint32_t base) {
        if (base == 0) return false;
        const std::uint64_t end = static_cast<std::uint64_t>(base) + img.size_of_image;
        if (end > GuestMemory::kSpace) return false;  // 越界（含 uint32 回绕）
        for (const Range& r : used) {
            if (static_cast<std::uint64_t>(base) < r.end && r.begin < end) return false;
        }
        return true;
    };

    std::uint32_t base = img.image_base;
    if (!fits(base)) {
        std::fprintf(stderr,
                     "[lift] AGERC 的 ImageBase 0x%08X（+0x%X）与现有 guest 布局冲突 ⇒ 改用 0x%08X\n",
                     img.image_base, img.size_of_image, kFallbackBase);
        base = kFallbackBase;
        if (!fits(base)) {
            setErr("选基址", "PE 的 ImageBase 0x... 冲突，备选 0x30000000 也冲突（没有可用的 guest 基址）");
            return false;
        }
    }
    if (!mapPeImageAt(mem_, img, base, &perr)) {
        setErr("映射镜像", perr);
        return false;
    }

    agerc_image_ = std::move(img);
    agerc_path_ = path;
    agerc_base_ = base;
    agerc_loaded_ = true;
    // 交给 shim 的**唯一**令牌：真实 ImageBase + 镜像指针（资源 / 版本从这里读）。
    // `agerc_loaded` 仍是 false：只有 guest 真的调 LoadLibrary("AGERC.DLL") 才置 true。
    cpu_.shim.agerc_image = &agerc_image_;
    cpu_.shim.agerc_base = agerc_base_;
    cpu_.shim.agerc_loaded = false;
    return true;
}

bool Machine::prepare(std::uint32_t entry_override, std::string* err) {
    if (!image_loaded_) {
        if (err) *err = "还没有载入镜像";
        return false;
    }
    // ★ VFS overlay（用户 2026-11-05 口径）：guest 打开 `…\AGE.EXE` 拿到的是**我们真正在跑的
    //   那份字节**（--exe 解析出的拆壳件），而不是 `--game-dir` 下那份加壳原件。
    //   放在 prepare 而不是 loadImage：此时 --game-dir / --guest-root 都已装配完。
    vfs_.setExeOverlay(vfs_.guestExePath(), exe_path_, image_.sha256);
    if (!setupProcessStructures(mem_, image_, layout_, vfs_, &setup_, err)) return false;
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
    cpu_.shim.env_ea = setup_.env_ea;
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

// ★ 入口分派（任务 3）：`opts.cpu` 缺省 = Interp ⇒ 走下面这个**原样保留**的解释器主体。
//   ⛔ 默认路径的行为一个字节都没变：runInterpreter 就是从前的 `Machine::run`，只是改了名。
RunResult Machine::run(const RunOptions& opts) {
    if (opts.cpu == CpuBackend::Unicorn) return runUnicorn(opts);
    return runInterpreter(opts);
}

RunResult Machine::runInterpreter(const RunOptions& opts) {
    RunResult r;
    shims_.setTrace(opts.trace_shims, opts.trace_out ? opts.trace_out : stdout);
    if (opts.real_time) clock_.setRealTime(true);
    if (opts.trace_inputs) dumpTraceInputs(stdout);
    // 诊断开关：LIFT_TRACE_FULL=1 ⇒ `--trace-blocks` 每条带操作数文本 + 寄存器快照（缺省关）
    const bool traceFull = std::getenv("LIFT_TRACE_FULL") != nullptr;
    cpu_.running = true;
    const auto wall_start = std::chrono::steady_clock::now();
    std::uint64_t next_time_check = 0;

    while (cpu_.running) {
        if (opts.time_limit_sec > 0.0 && cpu_.insns >= next_time_check) {
            const auto now = std::chrono::steady_clock::now();
            const double elapsed = std::chrono::duration<double>(now - wall_start).count();
            if (elapsed >= opts.time_limit_sec) {
                r.reason = StopReason::TimeLimit;
                r.detail = "达到 --time-limit（墙钟上限）";
                break;
            }
            next_time_check = cpu_.insns + 4096u;
        }
        // ★ 嵌套 guest 回调的续延点（guest_call.h）：先于定时器/块解码检查。
        if (cpu_.eip == GuestCallStack::kContinuationEa) {
            std::string call_err;
            if (!guest_calls_.resume(cpu_, &call_err)) {
                r.reason = StopReason::Unimplemented;
                r.detail = call_err;
                break;
            }
            continue;
        }
        // ★ winmm 定时器回调的返回点（协作式定时器；见 x86.h 的 kTimerCallbackReturnEa）
        if (cpu_.eip == kTimerCallbackReturnEa) {
            if (!cpu_.shim.timer_firing) {
                r.reason = StopReason::Unimplemented;
                r.detail = "定时器续延点被调到，但没有正在跑的定时器回调";
                break;
            }
            cpu_.esp() = cpu_.shim.timer_saved_esp;
            cpu_.eip = cpu_.shim.timer_resume_ea;
            cpu_.shim.timer_firing = false;
            continue;
        }
        if (opts.until_ea && cpu_.eip == opts.until_ea) {
            r.reason = StopReason::UntilEa;
            break;
        }
        if (opts.max_insns && cpu_.insns >= opts.max_insns) {
            r.reason = StopReason::MaxInsns;
            break;
        }
        // ★ 到期的 winmm 定时器在**块边界**触发（D5：没有定时器服务线程）。
        //   ⛔ 不嵌套：一个回调没跑完就不再触发下一个（与真机的单线程回调串行一致）。
        if (!cpu_.shim.timer_firing && !cpu_.shim.timers.empty() && !cpu_.shim.callback.active) {
            if (fireDueTimer(r)) continue;
            if (r.reason != StopReason::None) break;
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

bool Machine::fireDueTimer(RunResult& r) {
    const std::uint64_t now = clock_.now100ns();
    std::vector<GuestTimer>& timers = cpu_.shim.timers;
    std::size_t best = timers.size();
    for (std::size_t i = 0; i < timers.size(); ++i) {
        if (timers[i].due_100ns > now) continue;
        // 确定性：同刻到期时取 id 最小的（⛔ 不依赖 vector 的插入顺序）
        if (best == timers.size() || timers[i].due_100ns < timers[best].due_100ns ||
            (timers[i].due_100ns == timers[best].due_100ns && timers[i].id < timers[best].id)) {
            best = i;
        }
    }
    if (best == timers.size()) return false;

    const GuestTimer t = timers[best];
    if (t.oneshot) {
        timers.erase(timers.begin() + static_cast<std::ptrdiff_t>(best));
    } else {
        // 周期定时器先续约（回调里 timeKillEvent 仍能按 id 杀掉它）
        timers[best].due_100ns += static_cast<std::uint64_t>(t.period_ms) * 10000ull;
    }
    if (t.callback == 0) {
        std::fprintf(stderr, "[lift] 定时器 %u 的回调为 NULL ⇒ 丢弃\n", t.id);
        return false;
    }
    // 回调帧：TimeProc(uID, uMsg, dwUser, dw1, dw2)（stdcall，5 个参数 = 20 字节 + 返回地址）
    constexpr std::uint32_t kFrame = 4u + 5u * 4u;
    const std::uint32_t base = cpu_.esp();
    if (base < kFrame || !mem_.committed(base - kFrame, kFrame)) {
        r.reason = StopReason::Unimplemented;
        r.detail = "guest 栈不够铺定时器回调帧（esp=" + std::to_string(base) + "）";
        return false;
    }
    cpu_.shim.timer_resume_ea = cpu_.eip;
    cpu_.shim.timer_saved_esp = base;
    cpu_.shim.timer_firing = true;
    cpu_.shim.timer_firing_id = t.id;
    cpu_.esp() = base - kFrame;
    mem_.wr32(base - 24u, kTimerCallbackReturnEa);
    mem_.wr32(base - 20u, t.id);
    mem_.wr32(base - 16u, 0u);  // uMsg（文档：保留，不用）
    mem_.wr32(base - 12u, t.user);
    mem_.wr32(base - 8u, 0u);   // dw1（保留）
    mem_.wr32(base - 4u, 0u);   // dw2（保留）
    cpu_.eip = t.callback;
    return true;
}

TraceInputs Machine::traceInputs() const {    TraceInputs t;
    // ★ 记 `--exe` 的**原始字符串**（不是解析后的绝对路径）：plan §5 A4 实测，同一台机器上
    //   相对路径 38,456 / 绝对路径 44,184 / 87 字符路径 53,134 条指令 —— 换个写法就是另一条路径。
    t.exe_path = setup_.exe_path.empty() ? exe_path_ : setup_.exe_path;
    t.guest_exe_path = setup_.guest_exe_path;
    t.guest_cmdline = setup_.cmdline;
    t.environment = setup_.environment_ansi;
    t.game_dir = vfs_.gameDir();
    t.appdata_dir = vfs_.appDataDir();
    t.overlay_guest = vfs_.overlayGuestPath();
    t.overlay_host = vfs_.overlayHostPath();
    t.agerc_path = agerc_path_;
    t.agerc_sha256 = agerc_image_.sha256;
    t.clock_start_100ns = clock_.epoch100ns();
    t.real_time = clock_.realTime();
    return t;
}

void Machine::dumpTraceInputs(std::FILE* out) const {
    const TraceInputs t = traceInputs();
    std::fprintf(out, "trace-input exe      = %s\n", t.exe_path.c_str());
    std::fprintf(out, "trace-input guest-exe= %s\n", t.guest_exe_path.c_str());
    std::fprintf(out, "trace-input cmdline  = %s\n", t.guest_cmdline.c_str());
    std::fprintf(out, "trace-input env      = %s\n", t.environment.c_str());
    std::fprintf(out, "trace-input game-dir = %s\n", t.game_dir.c_str());
    std::fprintf(out, "trace-input appdata  = %s\n", t.appdata_dir.c_str());
    std::fprintf(out, "trace-input overlay  = %s ⇒ %s\n", t.overlay_guest.c_str(),
                 t.overlay_host.c_str());
    std::fprintf(out, "trace-input agerc    = %s (sha256=%s)\n", t.agerc_path.c_str(),
                 t.agerc_sha256.empty() ? "-" : t.agerc_sha256.c_str());
    std::fprintf(out, "trace-input clock    = %s (start=%llu x100ns)\n",
                 t.real_time ? "REAL-TIME（⚠ 逐字节断言必须跳过）" : "virtual",
                 (unsigned long long)t.clock_start_100ns);
    std::fprintf(out, "trace-input hash     = %016llX\n", (unsigned long long)traceInputsHash());
}

std::uint64_t Machine::traceInputsHash() const {
    const TraceInputs t = traceInputs();
    std::uint64_t h = 0xCBF29CE484222325ull;
    auto mix = [&h](const void* p, std::size_t n) {
        const auto* b = static_cast<const std::uint8_t*>(p);
        for (std::size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 0x100000001B3ull;
        }
    };
    mix(t.exe_path.data(), t.exe_path.size());
    mix(t.guest_exe_path.data(), t.guest_exe_path.size());
    mix(t.guest_cmdline.data(), t.guest_cmdline.size());
    mix(t.environment.data(), t.environment.size());
    mix(t.game_dir.data(), t.game_dir.size());
    mix(t.appdata_dir.data(), t.appdata_dir.size());
    mix(t.overlay_guest.data(), t.overlay_guest.size());
    mix(t.overlay_host.data(), t.overlay_host.size());
    mix(t.agerc_path.data(), t.agerc_path.size());
    mix(t.agerc_sha256.data(), t.agerc_sha256.size());
    mix(&t.clock_start_100ns, sizeof(t.clock_start_100ns));
    const std::uint8_t real = t.real_time ? 1 : 0;
    mix(&real, 1);
    return h;
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

void Machine::reportCoverageJson(std::FILE* out) const {
    // 三类分开数（plan 的 A5 要能回答"缺口是什么"）：
    //   implemented  : 有实现（fn != nullptr）
    //   stub         : 显式登记过 stub（declared && !fn）—— 必须有 reason
    //   bare         : 导入表里有、连登记都没有（!declared && !fn）—— **裸缺口**
    // 另加 imported 标志，让调用方能把"声明了但没被导入"的条目筛掉。
    std::fprintf(out, "{\n  \"entries\": [\n");
    bool first = true;
    for (const ShimDef& d : shims_.all()) {
        std::fprintf(out,
                     "%s    {\"module\": \"%s\", \"name\": \"%s\", \"imported\": %s, \"implemented\": %s, "
                     "\"declared\": %s, \"reason\": \"%s\"}",
                     first ? "" : ",\n", d.module.c_str(), d.name.c_str(), d.imported ? "true" : "false",
                     d.fn ? "true" : "false", d.declared ? "true" : "false",
                     d.reason ? d.reason : "");
        first = false;
    }
    std::fprintf(out, "\n  ]\n}\n");
}

void Machine::dumpRunSummary(const RunResult& r, std::FILE* out) const {
    const TraceInputs ti = traceInputs();
    std::fprintf(out, "trace-in  : exe=%s | clock=%s | hash=%016llX\n", ti.exe_path.c_str(),
                 ti.real_time ? "REAL-TIME" : "virtual", (unsigned long long)traceInputsHash());
    if (r.reason != StopReason::UntilEa && r.reason != StopReason::MaxInsns && cpu_.last_count > 0) {
        std::fprintf(out, "--- 最近执行的指令（最新在前）---\n");
        for (int i = 0; i < cpu_.last_count && i < 12; ++i) {
            const int idx = (cpu_.last_head - 1 - i + 2 * X86::kLastInsns) % X86::kLastInsns;
            std::fprintf(out, "  %08X %s\n", cpu_.last_eip[idx], cpu_.last_mnemonic[idx] ? cpu_.last_mnemonic[idx] : "?");
        }
    }
    std::fprintf(out, "cpu       : %s\n", cpuBackendName(r.cpu));
    std::fprintf(out, "stop      : %s @ eip=%08X\n", stopReasonName(r.reason), r.ea);
    if (!r.detail.empty()) std::fprintf(out, "detail    : %s\n", r.detail.c_str());
    if (r.insns_counted) {
        std::fprintf(out, "insns     : %llu\n", (unsigned long long)r.insns);
    } else {
        // ★ Unicorn 后端在没装逐指令计数 hook 时不伪造指令数（见 machine.h 的 insns_counted）。
        std::fprintf(out, "insns     : (未计数：该后端没装计数 hook；给 --max-insns 才精确)\n");
    }
    std::fprintf(out, "blocks    : %zu (insns cached %zu, hits %zu, misses %zu)\n", cache_.blockCount(),
                 cache_.insnCount(), cache_.hits(), cache_.misses());
    std::fprintf(out, "committed : %llu KiB\n", (unsigned long long)(mem_.committedBytes() / 1024));
    std::fprintf(out, "shims     : imported %zu / called-unimplemented %zu\n", shims_.importedCount(),
                 shims_.calledUnimplemented().size());
    std::fprintf(out, "heap      : in_use %u peak %u allocs %u\n", heap_.bytesInUse(), heap_.peak(),
                 heap_.allocCount());
}

}  // namespace lift
