// emulator-lift · Machine：装配 + dispatcher + fault 诊断
//
// ★ 执行循环（README §2.2）：`eip → 块缓存 → 顺序执行 → 控制流只改 eip 并回循环`。
//   shim 桩地址也是块缓存里的条目（`Block::is_shim`），所以 `call [IAT]` 与 `call 0x401234`
//   在 dispatcher 眼里是同一件事。
// ★ 停下来的原因全部进 `RunResult`（D4 的 L1/L2 就是 `FirstShim` / `UntilEa` 两个原因）。
#include "runtime/machine.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

#include "runtime/d3d9_pipeline.h"
#include "runtime/exec.h"
#include "runtime/present.h"
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
    }
    return "?";
}

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

void Machine::flushFrameTrace() {
    if (!frame_trace_) return;
    const auto now = std::chrono::steady_clock::now();
    const double wall_ms = std::chrono::duration<double, std::milli>(now - frame_trace_last_).count();
    frame_trace_last_ = now;
    const std::uint64_t insns = cpu_.insns - frame_trace_last_insns_;
    frame_trace_last_insns_ = cpu_.insns;

    // ---- 宿主 shim（含 Present / Sprite 的像素循环 / GPU submit）----
    const std::vector<ShimDef>& defs = shims_.all();
    if (frame_trace_prev_shim_ns_.size() < defs.size()) frame_trace_prev_shim_ns_.resize(defs.size(), 0);
    struct ShimRow {
        const ShimDef* def = nullptr;
        std::uint64_t ns = 0;
    };
    std::vector<ShimRow> shim_rows;
    std::uint64_t shim_ns = 0;
    for (std::size_t i = 0; i < defs.size(); ++i) {
        const std::uint64_t total = defs[i].ns;
        const std::uint64_t prev = frame_trace_prev_shim_ns_[i];
        frame_trace_prev_shim_ns_[i] = total;
        if (total < prev) continue;
        const std::uint64_t delta = total - prev;
        if (delta == 0) continue;
        shim_ns += delta;
        shim_rows.push_back({&defs[i], delta});
    }
    std::sort(shim_rows.begin(), shim_rows.end(), [](const ShimRow& a, const ShimRow& b) {
        if (a.ns != b.ns) return a.ns > b.ns;
        if (a.def->module != b.def->module) return a.def->module < b.def->module;
        return a.def->name < b.def->name;
    });

    // ---- guest 块（只有 `--profile` 才累计 b.ns；否则这一栏是 0）----
    struct GuestRow {
        std::uint32_t fn_ea = 0;
        const LstFunction* fn = nullptr;
        std::uint64_t ns = 0;
    };
    std::unordered_map<std::uint32_t, GuestRow> guest_by_fn;
    std::uint64_t guest_ns = 0;
    for (const auto& kv : cache_.guestBlocks()) {
        const Block& b = *kv.second;
        const std::uint64_t prev = frame_trace_prev_block_ns_[b.ea];
        frame_trace_prev_block_ns_[b.ea] = b.ns;
        if (b.ns < prev) continue;
        const std::uint64_t delta = b.ns - prev;
        if (delta == 0) continue;
        guest_ns += delta;
        const LstFunction* fn = lst_.loaded() ? lst_.functionAt(b.ea) : nullptr;
        const std::uint32_t key = fn ? fn->ea : b.ea;
        GuestRow& row = guest_by_fn[key];
        row.fn_ea = key;
        row.fn = fn;
        row.ns += delta;
    }
    std::vector<GuestRow> guest_rows;
    guest_rows.reserve(guest_by_fn.size());
    for (const auto& kv : guest_by_fn) guest_rows.push_back(kv.second);
    std::sort(guest_rows.begin(), guest_rows.end(), [](const GuestRow& a, const GuestRow& b) {
        if (a.ns != b.ns) return a.ns > b.ns;
        return a.fn_ea < b.fn_ea;
    });

    std::fprintf(stdout, "[frame] n=%llu wall=%.1fms insns=%llu guest=%.1fms shim=%.1fms other=%.1fms\n",
                 (unsigned long long)presenter_.frameCount(), wall_ms, (unsigned long long)insns,
                 (double)guest_ns / 1e6, (double)shim_ns / 1e6,
                 (double)(static_cast<long long>(wall_ms * 1e6) - static_cast<long long>(guest_ns) -
                          static_cast<long long>(shim_ns)) /
                     1e6);
    if (!guest_rows.empty()) {
        std::fputs("        guest:", stdout);
        for (std::size_t i = 0; i < guest_rows.size() && i < 5; ++i) {
            const GuestRow& r = guest_rows[i];
            std::fprintf(stdout, " %s=%.1fms", r.fn ? r.fn->name.c_str() : "?", (double)r.ns / 1e6);
        }
        std::fputc('\n', stdout);
    }
    if (!shim_rows.empty()) {
        std::fputs("        shim :", stdout);
        for (std::size_t i = 0; i < shim_rows.size() && i < 5; ++i) {
            const ShimRow& r = shim_rows[i];
            std::fprintf(stdout, " %s!%s=%.1fms", r.def->module.c_str(), r.def->name.c_str(),
                         (double)r.ns / 1e6);
        }
        std::fputc('\n', stdout);
    }
    // ★ 逐帧 GPU 覆盖增量：一眼看出"这一帧有没有回退"，不必等 summary。
    {
        const d3d9::GpuCoverage& cov = d3d9::gpuCoverage();
        auto delta = [](std::uint64_t now, std::uint64_t prev) { return now >= prev ? now - prev : 0ull; };
        const std::uint64_t gd = delta(cov.gpu_draws, frame_trace_prev_gpu_draws_);
        const std::uint64_t fb = delta(cov.fallback_draws, frame_trace_prev_gpu_fallback_);
        const std::uint64_t fbp = delta(cov.fallback_pixels, frame_trace_prev_gpu_fallback_px_);
        const std::uint64_t sp = delta(cov.sprite_gpu_draws, frame_trace_prev_sprite_total_);
        const std::uint64_t sf = delta(cov.sprite_fallback_draws, frame_trace_prev_sprite_fallback_);
        frame_trace_prev_gpu_draws_ = cov.gpu_draws;
        frame_trace_prev_gpu_fallback_ = cov.fallback_draws;
        frame_trace_prev_gpu_fallback_px_ = cov.fallback_pixels;
        frame_trace_prev_sprite_total_ = cov.sprite_gpu_draws;
        frame_trace_prev_sprite_fallback_ = cov.sprite_fallback_draws;
        std::fprintf(stdout, "        gpu  : draws=%llu fallback=%llu fallback_pixels=%llu sprite_gpu=%llu sprite_fallback=%llu\n",
                     (unsigned long long)gd, (unsigned long long)fb, (unsigned long long)fbp,
                     (unsigned long long)sp, (unsigned long long)sf);
    }
    // ★ Present 拆解（§4.3.1）：4 段加起来 ≈ shim 里的 `com!IDirect3DDevice9::Present`。
    //   `direct=1` = 本帧呈现走 GPU 直出（target → swapchain，没有 SDL_UpdateTexture）。
    {
        const PresentBreakdown& pb = presentBreakdown();
        std::fprintf(stdout,
                     "        pres : readback=%.1fms hash=%.1fms stats=%.1fms sink=%.1fms direct=%d\n",
                     static_cast<double>(pb.readback_ns) / 1e6, static_cast<double>(pb.hash_ns) / 1e6,
                     static_cast<double>(pb.stats_ns) / 1e6, static_cast<double>(pb.sink_ns) / 1e6,
                     pb.direct ? 1 : 0);
    }
    std::fflush(stdout);
}

RunResult Machine::run(const RunOptions& opts) {
    RunResult r;
    // ★ GPU 覆盖账本按**这一轮**重算（`--gpu-strict` / dumpRunSummary 读的就是它）。
    d3d9::gpuCoverageReset();
    shims_.setTrace(opts.trace_shims, opts.trace_out ? opts.trace_out : stdout);
    // ★ 诊断：`LIFT_FRAME_TRACE=1` ⇒ 每次 Present 后打"这一帧墙钟去哪了"。
    //   只读、默认关；shim 计时只在打开时才有 steady_clock 开销。
    frame_trace_ = std::getenv("LIFT_FRAME_TRACE") != nullptr;
    shims_.setTiming(frame_trace_);
    if (frame_trace_) {
        frame_trace_last_ = std::chrono::steady_clock::now();
        frame_trace_last_insns_ = cpu_.insns;
        frame_trace_prev_shim_ns_.assign(shims_.all().size(), 0);
        frame_trace_prev_block_ns_.clear();
        const d3d9::GpuCoverage& cov = d3d9::gpuCoverage();
        frame_trace_prev_gpu_draws_ = cov.gpu_draws;
        frame_trace_prev_gpu_fallback_ = cov.fallback_draws;
        frame_trace_prev_gpu_fallback_px_ = cov.fallback_pixels;
        frame_trace_prev_sprite_total_ = cov.sprite_gpu_draws;
        frame_trace_prev_sprite_fallback_ = cov.sprite_fallback_draws;
    }
    if (opts.real_time) clock_.setRealTime(true);
    if (opts.trace_inputs) dumpTraceInputs(stdout);
    // 诊断开关：LIFT_TRACE_FULL=1 ⇒ `--trace-blocks` 每条带操作数文本 + 寄存器快照（缺省关）
    const bool traceFull = std::getenv("LIFT_TRACE_FULL") != nullptr;
    cpu_.running = true;
    const auto wall_start = std::chrono::steady_clock::now();
    std::uint64_t next_time_check = 0;
    std::uint64_t next_present_check = 0;
    // ★ native override 的入口判据：**上一条执行的是 `call`**，且目标就是当前 eip。
    //   只按 EA 拦会把 `jmp`/尾调用也当成函数入口（那时栈上没有本次调用的返回地址，
    //   抬栈 + 弹返回地址 = 静默改坏控制流）。所以 call 目标只对**下一次** dispatcher 迭代有效。
    std::uint32_t next_call_target = 0;

    while (cpu_.running) {
        const std::uint32_t call_target = next_call_target;
        next_call_target = 0;
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
        // ★ 显示后端：块边界也要 pump（否则两次 Present 之间窗口假死）。无 sink ⇒ 零成本。
        //   关窗必须设 r.reason（只把 cpu_.running 置 false 会落成 StopReason::None ⇒ exit 2）。
        if (presenter_.hasSink() && cpu_.insns >= next_present_check) {
            presenter_.pumpHostEvents();
            if (presenter_.stopRequested()) {
                r.reason = StopReason::Stopped;
                r.detail = "present: 窗口关闭";
                cpu_.running = false;
                break;
            }
            next_present_check = cpu_.insns + 4096u;
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
            // ★ 整份通用寄存器都要还回去：真机上回调在另一个线程，被打断线程的寄存器组由 OS 保住。
            //   只还 EIP/ESP 的话，回调随手改掉的 EAX/ECX/EDX 会漏进被插入点的后续指令
            //   （实测形态：thiscall 的 this（ECX）在 `call` 与 callee 第一条指令之间被改坏）。
            const TimerSavedRegs& sr = cpu_.shim.timer_saved_regs;
            cpu_.eax() = sr.eax;
            cpu_.ebx() = sr.ebx;
            cpu_.ecx() = sr.ecx;
            cpu_.edx() = sr.edx;
            cpu_.esi() = sr.esi;
            cpu_.edi() = sr.edi;
            cpu_.ebp() = sr.ebp;
            cpu_.eflags = sr.eflags;
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

        // ★ guest 函数级 native override：只在**函数入口**（block 入口）按 EA 拦截。
        //   成功 ⇒ 按 ABI 设好 EAX/ESP/EIP 并直接回 dispatcher；失败 ⇒ 落回解释器执行原字节。
        //   ⛔ 不 patch guest 字节（SMC / 代码页保护都不用管）。
        //   ⛔ 只认"上一条是 call 且目标 = 当前 eip"：jmp / 尾调用 / 定时器和回调的合成帧
        //      都不是 call 帧，必须回退解释器（否则会弹掉一个不存在的返回地址）。
        if (!overrides_.empty() && call_target != 0 && cpu_.eip == call_target) {
            if (NativeOverride* ov = overrides_.find(cpu_.eip)) {
                const char* why = nullptr;
                const std::uint64_t before = ov->fallbacks;
                bool suspended = false;
                if (overrides_.invoke(*ov, cpu_, mem_, &why, &suspended)) {
                    if (suspended) continue;
                    if (opts.trace_blocks) std::fprintf(stdout, "[native] %08X %s\n", ov->ea, ov->name.c_str());
                    cpu_.noteInsn(ov->ea, "[native]");
                    ++cpu_.insns;
                    clock_.tick();
                    continue;
                }
                if (ov->fallbacks != before && ov->fallbacks == 1 && why) {
                    std::fprintf(stderr, "[native] %s @ %08X 回退解释器：%s\n", ov->name.c_str(), ov->ea, why);
                }
            }
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
        // ★ profile：块级命中（零查找；只在 --profile 时多一次自增）。
        if (opts.profile && !b->is_shim) ++b->hits;
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
            const std::uint64_t frame_before = presenter_.frameCount();
            shims_.invoke(cpu_, *def);
            if (frame_trace_ && presenter_.frameCount() != frame_before) flushFrameTrace();
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
        const auto block_start = std::chrono::steady_clock::now();
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
                next_call_target = (in.op == Op::Call) ? cpu_.eip : 0;
                left_block = true;
                break;
            }
        }
        if (opts.profile) {
            b->ns += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - block_start)
                    .count());
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
    // ★ 被打断的通用寄存器整份存下来（返回点在 kTimerCallbackReturnEa 那一段还回去）。
    //   ⛔ 不改时钟语义：这里不碰 clock_，只是"协作式回调"的上下文保存。
    {
        TimerSavedRegs& sr = cpu_.shim.timer_saved_regs;
        sr.eax = cpu_.eax();
        sr.ebx = cpu_.ebx();
        sr.ecx = cpu_.ecx();
        sr.edx = cpu_.edx();
        sr.esi = cpu_.esi();
        sr.edi = cpu_.edi();
        sr.ebp = cpu_.ebp();
        sr.eflags = cpu_.eflags;
    }
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
    // ★ override 集也是**输入**：同一份 exe / 同一串参数，换一组 override 就是另一条执行路径。
    for (std::size_t i = 0; i < overrides_.size(); ++i) {
        const NativeOverride* e = overrides_.at(i);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s@%08X/%s/%u", e->name.c_str(), e->ea, guestConvName(e->conv),
                      e->arg_bytes);
        if (!t.native_overrides.empty()) t.native_overrides += ';';
        t.native_overrides += buf;
    }
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
    std::fprintf(out, "trace-input override = %s\n",
                 t.native_overrides.empty() ? "(none)" : t.native_overrides.c_str());
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
    mix(t.native_overrides.data(), t.native_overrides.size());
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
    if (presenter_.sinkFailures() != 0) {
        std::fprintf(out, "present   : sink 失败 %llu 次（最后：%s）\n",
                     (unsigned long long)presenter_.sinkFailures(),
                     presenter_.lastSinkError().empty() ? "-" : presenter_.lastSinkError().c_str());
    }
    if (d3d9::gpuDrawCount() != 0) {
        std::fprintf(out, "raster    : GPU Draw %llu / 三角形 %llu（sink %s）\n",
                     (unsigned long long)d3d9::gpuDrawCount(),
                     (unsigned long long)d3d9::gpuTriangleCount(),
                     presenter_.sinkDescription().empty() ? "-" : presenter_.sinkDescription().c_str());
    }
    // ★ 固定打印 GPU 覆盖账（`--raster cpu` 时全 0；GPU 启用后回退必须可见，⛔ 不静默）。
    {
        const d3d9::GpuCoverage& cov = d3d9::gpuCoverage();
        std::fprintf(out, "gpu       : draws=%llu gpu=%llu fallback=%llu fallback_pixels=%llu\n",
                     (unsigned long long)cov.draw_calls, (unsigned long long)cov.gpu_draws,
                     (unsigned long long)cov.fallback_draws, (unsigned long long)cov.fallback_pixels);
        if (cov.sprite_draws != 0 || cov.sprite_fallback_draws != 0 || cov.sprite_skipped != 0) {
            std::fprintf(out,
                         "            sprite: draws=%llu gpu=%llu fallback=%llu fallback_pixels=%llu skipped=%llu\n",
                         (unsigned long long)cov.sprite_draws, (unsigned long long)cov.sprite_gpu_draws,
                         (unsigned long long)cov.sprite_fallback_draws,
                         (unsigned long long)cov.sprite_fallback_pixels,
                         (unsigned long long)cov.sprite_skipped);
        }
        if (!cov.fallback_reasons.empty()) {
            std::vector<std::pair<const std::string*, std::uint64_t>> rows;
            rows.reserve(cov.fallback_reasons.size());
            for (const auto& kv : cov.fallback_reasons) rows.emplace_back(&kv.first, kv.second);
            std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
                return a.second != b.second ? a.second > b.second : *a.first < *b.first;
            });
            std::fprintf(out, "            fallback top:");
            const std::size_t top = rows.size() < 5 ? rows.size() : 5;
            for (std::size_t i = 0; i < top; ++i) {
                std::fprintf(out, " [%llu×] %s ;", (unsigned long long)rows[i].second, rows[i].first->c_str());
            }
            std::fprintf(out, "\n");
        }
        if (cov.gpu_failures != 0) {
            std::fprintf(out, "            gpu_failures=%llu（准入通过但后端绘制失败，已 CPU 重放）\n",
                         (unsigned long long)cov.gpu_failures);
        }
    }
    if (!overrides_.empty()) {
        std::uint64_t hits = 0;
        std::uint64_t fallbacks = 0;
        for (std::size_t i = 0; i < overrides_.size(); ++i) {
            const NativeOverride* e = overrides_.at(i);
            hits += e->hits;
            fallbacks += e->fallbacks;
        }
        std::fprintf(out, "native-ovr: entries=%zu hits=%llu fallbacks=%llu\n", overrides_.size(),
                     (unsigned long long)hits, (unsigned long long)fallbacks);
        for (std::size_t i = 0; i < overrides_.size(); ++i) {
            const NativeOverride* e = overrides_.at(i);
            std::fprintf(out, "  %-12s @ %08X %s(%uB) hits=%llu fallbacks=%llu\n", e->name.c_str(), e->ea,
                         guestConvName(e->conv), e->arg_bytes, (unsigned long long)e->hits,
                         (unsigned long long)e->fallbacks);
        }
    }
}

void Machine::dumpProfile(std::FILE* out, std::size_t top) const {
    struct Row {
        std::uint32_t ea = 0;
        std::uint64_t hits = 0;
        std::uint64_t weight = 0;
        std::uint64_t ns = 0;
        const LstFunction* fn = nullptr;
    };
    std::vector<Row> rows;
    rows.reserve(cache_.guestBlocks().size());
    std::uint64_t total = 0;
    std::uint64_t total_ns = 0;
    for (const auto& kv : cache_.guestBlocks()) {
        const Block& b = *kv.second;
        if (b.hits == 0) continue;
        Row row;
        row.ea = b.ea;
        row.hits = b.hits;
        row.weight = b.hits * static_cast<std::uint64_t>(b.insns.size());
        row.ns = b.ns;
        row.fn = lst_.loaded() ? lst_.functionAt(b.ea) : nullptr;
        total += row.weight;
        total_ns += row.ns;
        rows.push_back(row);
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.weight != b.weight ? a.weight > b.weight : a.ea < b.ea; });
    std::fprintf(out, "\n=== profile: guest blocks（weight = hits × 块内指令数）===\n");
    std::fprintf(out, "total weight = %llu（块 %zu 个；按 weight 降序，前 %zu）\n",
                 (unsigned long long)total, rows.size(), top);
    // ★ 对账：正常（无 override / 无 SMC 清块）时 total weight 应等于 cpu_.insns。
    //   不等 ⇒ 有块被 SMC 丢掉 / 有 override 绕过了块 / 执行到一半停了 —— 不要把 profile 当精确账本。
    std::fprintf(out, "insns=%llu，差=%lld（override / SMC 清块 / 最后一个块只执行一部分时不为 0 是正常的）\n",
                 (unsigned long long)cpu_.insns,
                 (long long)cpu_.insns - (long long)total);
    for (std::size_t i = 0; i < rows.size() && i < top; ++i) {
        const Row& r = rows[i];
        const double pct = total ? 100.0 * static_cast<double>(r.weight) / static_cast<double>(total) : 0.0;
        if (r.fn) {
            std::fprintf(out, "  %2zu. %08X %7.2f%% hits=%-10llu w=%-12llu | %s+0x%X | %s\n", i + 1, r.ea, pct,
                         (unsigned long long)r.hits, (unsigned long long)r.weight, r.fn->name.c_str(),
                         r.ea - r.fn->ea, lst_.textAt(r.ea).c_str());
        } else {
            std::fprintf(out, "  %2zu. %08X %7.2f%% hits=%-10llu w=%-12llu | ? | %s\n", i + 1, r.ea, pct,
                         (unsigned long long)r.hits, (unsigned long long)r.weight, lst_.textAt(r.ea).c_str());
        }
    }

    // 函数聚合：按 `.lst` 的 proc 入口归并（⚠ 近似；chunk 会归到相邻函数，见 lst_index.h）。
    struct FuncRow {
        std::uint32_t key = 0;
        const LstFunction* fn = nullptr;
        std::uint64_t hits = 0;
        std::uint64_t weight = 0;
        std::uint64_t ns = 0;
        std::size_t blocks = 0;
    };
    std::unordered_map<std::uint32_t, FuncRow> by_func;
    for (const Row& r : rows) {
        const std::uint32_t key = r.fn ? r.fn->ea : r.ea;
        FuncRow& f = by_func[key];
        f.key = key;
        f.fn = r.fn;
        f.hits += r.hits;
        f.weight += r.weight;
        f.ns += r.ns;
        ++f.blocks;
    }
    std::vector<FuncRow> funcs;
    funcs.reserve(by_func.size());
    for (const auto& kv : by_func) funcs.push_back(kv.second);
    std::sort(funcs.begin(), funcs.end(), [](const FuncRow& a, const FuncRow& b) {
        if (a.weight != b.weight) return a.weight > b.weight;
        return a.key < b.key;  // ★ 同权重也要确定性（无 .lst 时 fn 为 null，不能全按 0）
    });
    std::fprintf(out, "\n=== profile: functions（按函数入口聚合；前 %zu）===\n", top);
    for (std::size_t i = 0; i < funcs.size() && i < top; ++i) {
        const FuncRow& f = funcs[i];
        const double pct = total ? 100.0 * static_cast<double>(f.weight) / static_cast<double>(total) : 0.0;
        std::fprintf(out, "  %2zu. %08X %7.2f%% hits=%-10llu w=%-12llu blocks=%-4zu %s%s%s\n", i + 1,
                     f.fn ? f.fn->ea : 0u, pct, (unsigned long long)f.hits, (unsigned long long)f.weight, f.blocks,
                     f.fn ? f.fn->name.c_str() : "(no .lst)",
                     (f.fn && !f.fn->signature.empty()) ? "  " : "", f.fn ? f.fn->signature.c_str() : "");
    }
    // ★ 墙钟表：只统计划 guest 块的执行时间（shim / native override 不在内）。
    std::vector<FuncRow> by_time = funcs;
    std::sort(by_time.begin(), by_time.end(), [](const FuncRow& a, const FuncRow& b) {
        if (a.ns != b.ns) return a.ns > b.ns;
        return a.key < b.key;
    });
    std::fprintf(out, "\n=== profile: functions by wall time（guest 块执行时间；前 %zu）===\n", top);
    std::fprintf(out, "total guest-block wall = %.1f ms\n", static_cast<double>(total_ns) / 1e6);
    for (std::size_t i = 0; i < by_time.size() && i < top; ++i) {
        const FuncRow& f = by_time[i];
        const double pct = total_ns ? 100.0 * static_cast<double>(f.ns) / static_cast<double>(total_ns) : 0.0;
        std::fprintf(out, "  %2zu. %08X %7.1f ms %6.2f%% hits=%-10llu blocks=%-4zu %s\n", i + 1,
                     f.fn ? f.fn->ea : 0u, static_cast<double>(f.ns) / 1e6, pct, (unsigned long long)f.hits, f.blocks,
                     f.fn ? f.fn->name.c_str() : "(no .lst)");
    }
    // ★ override 的函数体**不会**进块 profile（被宿主实现整段替代）⇒ 显式说明，否则 profile 会误导
    //   "这个热点已经不在"。
    if (!overrides_.empty()) {
        std::fprintf(out, "\n注意：以下函数被 native override 替代，其函数体不计入上面的块 profile：\n");
        for (std::size_t i = 0; i < overrides_.size(); ++i) {
            const NativeOverride* e = overrides_.at(i);
            std::fprintf(out, "  %-12s @ %08X hits=%llu\n", e->name.c_str(), e->ea,
                         (unsigned long long)e->hits);
        }
    }
}

bool Machine::configureNativeOverrides(const std::string& list, std::string* err) {
    overrides_.clear();
    if (list.empty()) return true;
    if (!lst_.loaded()) {
        if (err) *err = "--native-overrides 需要 --lst（按函数名解析 EA；⛔ 不硬编码 EA）";
        return false;
    }
    auto knownNames = []() {
        std::string s;
        for (std::size_t i = 0; i < knownNativeCount(); ++i) {
            if (i) s += ", ";
            s += knownNativeAt(i).name;
            if (knownNativeAt(i).debug) s += "(debug)";
        }
        return s;
    };
    auto trim = [](std::string s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
        return s;
    };
    std::vector<std::string> tokens;
    {
        std::size_t start = 0;
        while (start <= list.size()) {
            const std::size_t comma = list.find(',', start);
            tokens.push_back(trim(list.substr(start, comma == std::string::npos ? std::string::npos : comma - start)));
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }

    // `-name` = 排除。排除项**优先于**启用项（与出现顺序无关）：
    // `all,-sub_48B090` 与 `-sub_48B090,all` 都表示“all 但不含 sub_48B090”。
    // ⛔ `-` 后面必须跟一个已知名字；拼错就报错，不静默忽略。
    std::vector<std::string> disabled;
    for (const std::string& token : tokens) {
        if (token.empty() || token.front() != '-') continue;
        const std::string name = trim(token.substr(1));
        if (name.empty()) {
            if (err) *err = "`-` 后面需要一个 native override 名字";
            return false;
        }
        if (findKnownNative(name) == nullptr) {
            if (err) *err = "未知的 native override 排除项：" + name + "（已知：" + knownNames() + "）";
            return false;
        }
        disabled.push_back(name);
    }
    auto isDisabled = [&](const std::string& name) {
        return std::find(disabled.begin(), disabled.end(), name) != disabled.end();
    };
    auto addName = [&](const std::string& name, bool required) -> bool {
        if (isDisabled(name)) return true;
        const KnownNative* k = findKnownNative(name);
        if (!k) {
            if (required && err) *err = "未知的 native override：" + name + "（已知：" + knownNames() + "）";
            return !required;
        }
        const LstFunction* f = lst_.findFunction(name);
        if (!f) {
            if (required && err) *err = "在 .lst 里找不到函数入口：" + name;
            return !required;
        }
        // 别名（两个名字指向同一个 EA）只加一次：`all` 不该因为别名而硬失败。
        if (overrides_.find(f->ea) != nullptr) return true;
        return overrides_.add(f->ea, name, k->conv, k->arg_bytes, k->fn, err);
    };

    for (const std::string& token : tokens) {
        if (token.empty() || token.front() == '-') continue;
        if (token == "all") {
            // ★ `all` = 只开**已验证**项；debug 项必须显式点名；排除项优先。
            for (std::size_t i = 0; i < knownNativeCount(); ++i) {
                if (knownNativeAt(i).debug) continue;
                if (!addName(knownNativeAt(i).name, false)) return false;
            }
        } else if (!addName(token, true)) {
            return false;
        }
    }
    if (overrides_.empty()) {
        if (err) *err = "没有解析到任何 native override（检查名字 / .lst）";
        return false;
    }
    return true;
}

GpuRaster* Machine::gpuRaster() {
    if (gpu_raster_ != nullptr) return gpu_raster_;
    PresentSink* s = presenter_.sink();
    return s != nullptr ? s->gpuRaster() : nullptr;
}

const GpuRaster* Machine::gpuRaster() const {
    if (gpu_raster_ != nullptr) return gpu_raster_;
    const PresentSink* s = presenter_.sink();
    return s != nullptr ? const_cast<PresentSink*>(s)->gpuRaster() : nullptr;
}

}  // namespace lift
