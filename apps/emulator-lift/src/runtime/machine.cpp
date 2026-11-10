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
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

#include "runtime/d3d9_pipeline.h"
#include "runtime/exec.h"
#include "runtime/guest_token.h"
#include "runtime/present.h"
#include "runtime/sha256.h"
#include "runtime/watch.h"
#include "shims/register.h"

namespace lift {
namespace {
Machine* g_active = nullptr;

// ★ `--skip-logo` 的机器级锚点（EA 来自 `.lst`；字段 offset 来自 shape 的 T_Command 成员）：
//   - 0x004BAA37：`sub_415640`（T_Command 构造）返回之后、`mov ds:dword_55E1BC, eax` 之前。
//     EAX = 新对象；构造器已在 0x00415AE0 把 show-logo 写成 1。
//   - 0x0005EB5C：T_Command 的 show-logo 标志（`0x130` 在 0x0042F7B8 读它；`0x9` 在 0x00428A75 清 0）。
//   ⛔ 这不是"跳过 boot"：root script 仍从 dword 0 执行，只是 0x130 读到 0，不走 call-script LOGO。
constexpr std::uint32_t kShowLogoInitEa = 0x004BAA37u;
constexpr std::uint32_t kShowLogoFieldOffset = 0x0005EB5Cu;
}  // namespace

Machine* activeMachine() { return g_active; }
void setActiveMachine(Machine* m) { g_active = m; }

// ★ 这台 cpu 的**归属**（不是"最近 init 的那台"）：`Machine::wire()` 在装配时把 owner 写进
//   `X86::machine`，所以另建一台 Machine（或 `~Machine` 清空 g_active）都不会改变它。
//   ⇒ 销毁路径（`d3d9::destroyTextureState` / `destroyDeviceState` / 流水线 `destroyForDevice`）
//     用它回收 arena，而不是拿可被合法改写的 `activeMachine()` 当判据
//     （REQ-01M6X4MSCMMFEXYXCSK6V61QR2：判据失败一次，arena 就永远没有回收入口）。
//   兜底：没 wire 的 X86（直接构造的纯夹具）只有"全局那台且真是这台 cpu"才算数；否则 nullptr
//   ⇒ 调用方只能清状态块 / 不猜 arena 归属（生产路径不存在这种 X86）。
Machine* machineOf(const X86& cpu) {
    if (cpu.machine != nullptr) return cpu.machine;
    Machine* m = g_active;
    return (m != nullptr && &m->cpu() == &cpu) ? m : nullptr;
}

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
        case StopReason::InteractionFailed: return "interaction-failed";
        // ★ 两个非 ok 的停机原因（`ok()` 里没有它们）：名字就是运行摘要 / 交互失败信息里印的
        //   那一串。`resource-exhausted` = 我方容量问题；`unimplemented-slot` = 已注册的槽/形态没实现。
        case StopReason::ResourceExhausted: return "resource-exhausted";
        case StopReason::Trap: return "unimplemented-slot";
        // ★ guest 的 SEH / C++ EH 走不完（未处理异常 / 派发做不下去）。⛔ 不是正常结局。
        case StopReason::UnhandledException: return "unhandled-exception";
    }
    return "?";
}

// ★ 见 machine.h：**只**是 `main` 最后那道兜底。消费者闸门（token / gpu-strict / 交互图）
//   在 `main.cpp` 里先于本函数返回，别把它们的码并进来。
int exitCodeForRun(const RunResult& r) { return r.ok() ? 0 : 2; }

// ---------------------------------------------------------------------------------------
// 时间线的效果级判据（`--expect-change-after` / `--expect-change-window`）
// ★ 真源口径：`docs/03-emulator/interaction-model.md` §2 的 `expect_change`（这里只换触发者：
//   图的动作由节点状态机发，时间线的动作由祖先 `at …` 条件发）。⛔ 不另写一套"变没变"。
// ---------------------------------------------------------------------------------------
bool parseExpectChangeActions(const std::string& spec, ExpectChangePolicy* out, std::string* err) {
    if (out == nullptr) return false;
    auto fail = [err](const std::string& why) {
        if (err) *err = why;
        return false;
    };
    ExpectChangePolicy p;
    if (spec.empty()) return fail("是空的（只认 click / key / mouse，逗号分隔）");
    auto trimToken = [](std::string s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
        return s;
    };
    std::size_t start = 0;
    while (start <= spec.size()) {
        const std::size_t comma = spec.find(',', start);
        std::string token = trimToken(spec.substr(
            start, comma == std::string::npos ? std::string::npos : comma - start));
        if (token.empty()) return fail("有空项（`click,,key` 不是合法写法）");
        for (char& c : token) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        if (token == "click") {
            if (p.click) return fail("重复项：click");
            p.click = true;
        } else if (token == "key") {
            if (p.key) return fail("重复项：key");
            p.key = true;
        } else if (token == "mouse") {
            if (p.mouse) return fail("重复项：mouse");
            p.mouse = true;
        } else {
            return fail("不认识的类别 \"" + token + "\"（只认 click / key / mouse）");
        }
        if (!p.text.empty()) p.text += ',';
        p.text += token;
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    p.enabled = true;
    *out = p;
    if (err) err->clear();
    return true;
}

bool Machine::configureExpectChange(const ExpectChangePolicy& p, std::string* err) {
    if (interaction_.active()) {
        if (err) {
            *err = "与 --interaction-graph 互斥：图有自己的 `expect` 语义，两套判据同时开会出两套口径";
        }
        return false;
    }
    if (p.enabled && !(p.click || p.key || p.mouse)) {
        if (err) *err = "--expect-change-after 至少要一个动作类别（click / key / mouse）";
        return false;
    }
    if (p.enabled && p.window_samples == 0) {
        if (err) *err = "--expect-change-window 必须是正整数（单位 = 采样数，一次 Present = 一个样本）";
        return false;
    }
    // ★ 零匹配动作 ⇒ 一个观测窗都开不了 ⇒ "策略开着却什么也没看"（静默绿，与"发完不管"同类）。
    //   在**配置期**就拒绝（main ⇒ exit 2）；调用方必须先 `loadInputScript*`，再配置。
    if (p.enabled) {
        if (!input_script_.active()) {
            if (err) *err = "需要先 loadInputScript（策略要看时间线动作）；⛔ 不许配置完再换脚本";
            return false;
        }
        expect_policy_ = p;  // 先落值，才能复用 expectWatches（它只读策略）
        bool any = false;
        for (const InputScript::Action& a : input_script_.actions()) {
            if (expectWatches(a.kind)) {
                any = true;
                break;
            }
        }
        if (!any) {
            expect_policy_ = ExpectChangePolicy{};
            if (err) {
                *err = "时间线里没有匹配的动作（after=" + p.text +
                       "）⇒ 这个策略一个观测窗都开不了（⛔ 不静默开局；改 after 或改脚本）";
            }
            return false;
        }
        return true;
    }
    expect_policy_ = p;
    return true;
}

// 这条动作类别要不要"动作后必须看到画面变化"。`MouseMove` / `Wait` / `ProfileBegin` / `profile_end` /
// `quit` 不在集合里：hover 本来就可能不改像素（强行要求会制造假红），后三种不是输入动作。
bool Machine::expectWatches(InputScript::Kind kind) const {
    if (!expect_policy_.enabled) return false;
    switch (kind) {
        case InputScript::Kind::Click: return expect_policy_.click;
        case InputScript::Kind::KeyDown:
        case InputScript::Kind::KeyUp: return expect_policy_.key;
        case InputScript::Kind::MouseDown:
        case InputScript::Kind::MouseUp: return expect_policy_.mouse;
        default: return false;
    }
}

void Machine::openExpectWindows(std::size_t applied_before) {
    const std::vector<InputScript::Action>& actions = input_script_.actions();
    const std::size_t applied_after = input_script_.appliedCount();
    for (std::size_t i = applied_before; i < applied_after && i < actions.size(); ++i) {
        const InputScript::Action& a = actions[i];
        if (!expectWatches(a.kind)) continue;
        ExpectWindow w;
        w.action_text = a.text;
        w.changes_at_start = sampler_.changeCount();
        w.samples_at_start = sampler_.sampleCount();
        w.start_frame = presenter_.frameCount();
        w.start_insns = cpu_.insns;
        w.start_guest_ms = clock_.nowMs();
        ++expect_stats_.windows_opened;
        std::printf("[expect-change] watch \"%s\"（窗口 %llu 个采样；samples=%llu changes=%llu frame=%llu insns=%llu）\n",
                    w.action_text.c_str(), (unsigned long long)expect_policy_.window_samples,
                    (unsigned long long)w.samples_at_start, (unsigned long long)w.changes_at_start,
                    (unsigned long long)w.start_frame, (unsigned long long)w.start_insns);
        expect_windows_.push_back(w);
    }
}

// 每个块边界判一次：满足 ⇒ 记 ok 并摘掉；到窗口 ⇒ 判死（not-changed）。
//   ⛔ 不判 `--expect-change-window` 的另一种"没采样"：那个只能等到运行结束（`samples` 不增长 ⇒
//   窗口永远不到）—— 在 run 尾部按同一组谓词分类（no-sample / not-changed / unclosed）。
void Machine::evaluateExpectWindows() {
    if (expect_failed_ || expect_windows_.empty()) return;
    // ★ 动作还没**交付完**（`click` 的抬起 / 显式 down 的延迟 up 还在队列里）⇒ 先不判**失败**，
    //   但"看到了变化"照旧立刻算满足。自动 hold 会花掉若干个采样（等 guest 的输入采样点，
    //   上限 `--input-max-hold`）；窗口若在这之前到期，一次**合法**点击会被判成"画面没变"。
    //   抬起发出之后（或到上限强制抬起）立刻恢复到期判定。
    const bool deliverable = input_script_.pendingReleaseCount() == 0;
    const std::uint64_t samples = sampler_.sampleCount();
    const std::uint64_t changes = sampler_.changeCount();
    for (std::size_t i = 0; i < expect_windows_.size();) {
        ExpectWindow& w = expect_windows_[i];
        if (sampler_.changeCount() > w.changes_at_start) {
            ++expect_stats_.windows_satisfied;
            std::printf("[expect-change] ok \"%s\"（窗口内看到了变化：samples=%llu changes=%llu）\n",
                        w.action_text.c_str(), (unsigned long long)samples, (unsigned long long)changes);
            expect_windows_.erase(expect_windows_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        if (!deliverable) {
            ++i;
            continue;
        }
        if (samples >= w.samples_at_start &&
            samples - w.samples_at_start >= expect_policy_.window_samples) {
            failExpectWindow(w, "not-changed", "窗口内的采样在增长，但画面一次都没变（动作没生效？）");
            return;
        }
        ++i;
    }
}

void Machine::failExpectWindow(const ExpectWindow& w, const char* kind, const char* why) {
    ++expect_stats_.windows_failed;
    expect_failed_ = true;
    char buf[640];
    std::snprintf(buf, sizeof(buf),
                  "expect-change: %s（动作 \"%s\"；%s；samples %llu→%llu changes %llu→%llu "
                  "window=%llu 个采样；frame=%llu insns=%llu guest_ms=%llu）",
                  kind, w.action_text.c_str(), why, (unsigned long long)w.samples_at_start,
                  (unsigned long long)sampler_.sampleCount(), (unsigned long long)w.changes_at_start,
                  (unsigned long long)sampler_.changeCount(),
                  (unsigned long long)expect_policy_.window_samples, (unsigned long long)w.start_frame,
                  (unsigned long long)w.start_insns, (unsigned long long)w.start_guest_ms);
    expect_failure_ = buf;
    std::fprintf(stderr, "[expect-change] FAIL %s\n", expect_failure_.c_str());
    cpu_.running = false;
    // ★ 唤醒可能正阻塞在 `GetMessageA` 的 guest（与交互图失败同一条理由）：只置标志的话，
    //   没有块边界就永远到不了 `run()` 的检查点。
    if (cpu_.windows != nullptr) cpu_.windows->postQuit(0);
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
    // ★ 摘掉本机 cpu 的归属（防御性）：`cpu_` 本身比 `surfaces_` / `com_` **先**析构
    //   （声明顺序的逆序），所以真正的 teardown 不变式是「⛔ 销毁阶段不许有按 owner 回收 arena 的
    //   回调」——今天没有（`ComRegistry` 没有析构钩子）。清 owner 只保证任何**意外**解析到的
    //   是 nullptr 而不是半死的 this；它救不了悬垂的 `X86&`，⛔ 别把它当成 teardown 保护。
    //   ★ `AudioEngine::audio_cpu_` 那份 X86 副本**有意保留** owner（见 audio.cpp）：
    //   `~AudioEngine` 早于 `surfaces_` / `presenter_` 执行，那时按 owner 回收仍是安全的。
    cpu_.machine = nullptr;
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
    cpu_.machine = this;  // ★ 宿主侧归属（销毁路径的可回收判据；见 machineOf）
    audio_.attach(&cpu_, &mem_, &clock_);
    cpu_.audio = &audio_;
    cpu_.cs_table = &critical_sections_;
    cpu_.native_overrides = &overrides_;
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
    if (frame_trace_prev_shim_calls_.size() < defs.size()) frame_trace_prev_shim_calls_.resize(defs.size(), 0);
    struct ShimRow {
        const ShimDef* def = nullptr;
        std::uint64_t ns = 0;
        std::uint64_t calls = 0;
    };
    std::vector<ShimRow> shim_rows;
    std::uint64_t shim_ns = 0;
    for (std::size_t i = 0; i < defs.size(); ++i) {
        const std::uint64_t total = defs[i].ns;
        const std::uint64_t prev = frame_trace_prev_shim_ns_[i];
        frame_trace_prev_shim_ns_[i] = total;
        // ★ 调用次数增量：墙钟高但次数少 ⇒ 单次很慢（找实现）；次数多 ⇒ 调用点太密（找 guest 路径）。
        const std::uint64_t calls_total = defs[i].call_count;
        const std::uint64_t calls_prev = frame_trace_prev_shim_calls_[i];
        frame_trace_prev_shim_calls_[i] = calls_total;
        const std::uint64_t calls = calls_total >= calls_prev ? calls_total - calls_prev : 0;
        if (total < prev) continue;
        const std::uint64_t delta = total - prev;
        if (delta == 0) continue;
        shim_ns += delta;
        shim_rows.push_back({&defs[i], delta, calls});
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
            std::fprintf(stdout, " %s!%s=%.1fms×%llu", r.def->module.c_str(), r.def->name.c_str(),
                         (double)r.ns / 1e6, (unsigned long long)r.calls);
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

bool Machine::loadInputScript(const std::string& path, std::string* err) {
    // 换脚本前先把旧脚本仍按着的输入抬起来（⛔ 不静默留下"卡住的键"）。
    if (input_script_.active()) input_script_.abandon(*cpu_.windows, clock_.nowMs());
    if (!input_script_.loadFile(path, err)) return false;
    std::printf("input-script: %s（%zu 条动作；触发 = at frame / at guest_ms / at insn / at wall_ms / after / wait）\n",
                path.c_str(), input_script_.actionCount());
    return true;
}

bool Machine::loadInputScriptText(const std::string& text, const std::string& origin, std::string* err) {
    if (input_script_.active()) input_script_.abandon(*cpu_.windows, clock_.nowMs());
    if (!input_script_.loadText(text, origin, err)) return false;
    std::printf("input-script: %s（%zu 条动作）\n", origin.c_str(), input_script_.actionCount());
    return true;
}

bool Machine::loadInteractionGraph(const std::string& path, std::string* err) {
    // 换图前先把旧图仍按着的输入抬起来（⛔ 不静默留下"卡住的键"），并把采样器摘下来。
    if (interaction_.active()) interaction_.actuator().abandon(*cpu_.windows, clock_.nowMs());
    if (!interaction_.loadFile(path, err)) return false;
    sampler_.reset();
    interaction_.bindSampler(sampler_);
    presenter_.setFrameSampler(&sampler_);
    std::fprintf(stderr,
                 "[interaction] 图：%s（%zu 个节点，entry=%s；采样 = Present，矩形 %zu 个）\n",
                 path.c_str(), interaction_.nodes().size(), interaction_.entry().c_str(),
                 sampler_.rectCount());
    return true;
}

bool Machine::loadInteractionGraphText(const std::string& text, const std::string& origin, std::string* err) {
    if (interaction_.active()) interaction_.actuator().abandon(*cpu_.windows, clock_.nowMs());
    if (!interaction_.loadText(text, origin, err)) return false;
    sampler_.reset();
    interaction_.bindSampler(sampler_);
    presenter_.setFrameSampler(&sampler_);
    return true;
}

// 一份 `InputScript` 的公共推进：tick + profiler 窗口边界登记 + `quit` 唤醒。
// ★ 时间线（`--input-script`）与交互图的 actuator 走**同一条**路径：两条通道的动作
//   （自动 hold / 延迟抬起 / profiler / quit）只有一份实现，⛔ 不要各写一遍。
void Machine::pumpScript(InputScript& script, X86& cpu, std::uint64_t wall_ms) {
    if (!script.active()) return;
    if (cpu.windows == nullptr) return;
    // ★ 唯一状态源就是 `cpu.windows`（与 SDL 真实事件同一份）：这里只推进时间线。
    //   四个量分别对应四种触发条件（frame / guest_ms / insn / wall_ms）—— 调度只认"条件到期"。
    script.tick(*cpu.windows, presenter_.frameCount(), clock_.nowMs(), cpu.insns, wall_ms);
    // ★ profiler 窗口的边界请求：**只在这里登记**，真正开/关在下一个 block 边界的迭代起点
    //   （⇒ 同一脚本重复跑得到同一个边界；⛔ 不在动作应用点上就地开关）。
    std::string label;
    if (script.takeProfileBegin(&label)) {
        // ★ **运行期追加**的 `profile_begin` 必须在这里重新 arm（缺陷 `REQ-01M701PEB38VDQJD4QR2JEBA74`）：
        //   `run()` 只在 **prepare 期**按 `input_script_.hasProfileActions()` arm 一次；而
        //   `--interaction-graph` 的 `profile_begin/end` 是**运行中**才经 `InputScript::appendText`
        //   追加进来的（`InteractionGraph::issueAction`）⇒ prepare 期那个判据看到的是 false，
        //   `armed_` 会停在 false：窗口照开照收，可 `iterations` / 函数表的 self / shim 墙钟
        //   全是 0，而且不报错、不告警（= 静默残表）。
        //   为什么选这一点：它就是"脚本**消费** ProfileBegin 的那一刻"（`InputScript` 把请求交出来，
        //   下一个块边界才真正开窗）⇒ arm 一定早于 `WindowProfiler::begin`。一次性的，
        //   ⛔ 不在每个 tick 检查 armed —— 那会给块边界上的热路径加无谓分支。
        if (!window_prof_.armed()) {
            window_prof_.setArmed(true);
            // shim 墙钟 = `--profile` / `LIFT_FRAME_TRACE` / armed 三者的**并集**：运行期 arm
            // 必须把它一起打开，否则窗口里的 shim 行只有 `calls`、`ns` 恒 0.0ms（还是残表）。
            shims_.setTiming(true);
            // 运行摘要的 `window-prof` 位读的是 `instrument_.window_prof`（`dumpRunSummary`）——
            // 按**真实** armed 状态改，⛔ 不让摘要继续报 off。
            instrument_.window_prof = true;
            std::fprintf(stderr,
                         "[profile-window] ⓘ 运行期追加的 profile_begin(\"%s\") ⇒ 重新 arm 区间 profiler"
                         "（同时打开 shim 计时）\n",
                         label.c_str());
        }
        if (profile_begin_pending_) {
            std::fprintf(stderr,
                         "[profile-window] ⚠ 同一 tick 里有两个 profile_begin（%s / %s）⇒ 后者会顶掉前者\n",
                         profile_begin_label_.c_str(), label.c_str());
        }
        profile_begin_pending_ = true;
        profile_begin_label_ = label;
    }
    if (script.takeProfileEnd(&label)) {
        profile_end_pending_ = true;
        profile_end_label_ = label;
    }
    // ★ `quit` 还要**唤醒 guest**：窗口模式下 guest 可能正阻塞在 `GetMessageA`，它的循环里
    //   没有块边界 ⇒ 只置标志永远到不了 `Machine::run` 的那条检查。投一次 WM_QUIT 让它走退出路
    //   （没有事件源时这条无副作用：`peek` 只是把 WM_QUIT 当成队尾虚拟项）。
    if (script.quitPostPending()) {
        script.markQuitPosted();
        cpu.windows->postQuit(0);
    }
    // ★ marker 超时**同样要唤醒**：超时经常是在宿主事件泵里（guest 正阻塞在 `GetMessageA`）
    //   被 `tick()` 发现的，此时 run 的块边界检查点根本不在跑 —— 只置标志 = 永远挂住，
    //   "响亮失败"退化成"静默挂死"。与 `quit` 同一条处置：一次性投 WM_QUIT。
    if (script.markerTimeoutPostPending()) {
        script.markMarkerTimeoutPosted();
        cpu.windows->postQuit(0);
    }
}

std::uint64_t Machine::wallMs() const {
    if (!wall_start_valid_) return 0;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - wall_start_)
            .count());
}

void Machine::tickInputScript(X86& cpu) {
    if (!input_script_.active()) return;
    // ★ 开窗**就地**做（⛔ 不延迟到块边界）：`InputScript::tick` 里 `quit` 之后的同 tick 动作
    //   不会再被应用，而块边界的 `quit` 分支会先 break —— 延迟开窗会让 `click` + `quit`
    //   静默通过（那正是本策略要抓的"发完不管"）。
    const std::size_t applied_before = input_script_.appliedCount();
    pumpScript(input_script_, cpu, wallMs());
    if (expect_policy_.enabled && !expect_failed_) {
        if (input_script_.appliedCount() > applied_before) openExpectWindows(applied_before);
        evaluateExpectWindows();
    }
}

// ★ marker 采样（`TriggerKind::Marker` 的事件源）：读 VM 循环头的引擎身份并喂给时间线。
//   ★ 调用点**只有** `run()` 在块入口（`cpu_.eip` == 循环头 / 消息泵回来的块入口）时调它，
//     以及纯档夹具直接驱动一次采样。⛔ 不要在任意块边界调：帧装载器把 `cur` 当临时值用。
bool Machine::sampleMarker() {
    if (cpu_.windows == nullptr) return false;
    // ★ 采样点在 VM 循环头 ⇒ **每条主循环指令一次**。同一个 `(cur,id)` 且名字表状态没变时**不重解析名字**
    //   （否则每条指令都要付两次 std::string + CP932 转换，marker 脚本会把解释器拖慢一个量级）。
    //   ★ 名字表状态要一起比（`name_table_sig`）：TOC 从"未装载"变成"已装载"而身份没变时，
    //     名字必须重新解析（否则会永远停在 `id:0x…` = 静默失明）。
    const MarkerSample ident = marker_sampler_.sampleIdentity(mem_);
    if (ident.valid && last_marker_sample_.valid && ident.cur == last_marker_sample_.cur &&
        ident.id == last_marker_sample_.id &&
        last_marker_sample_.name_table_sig == marker_sampler_.nameTableSignature(mem_, ident.id)) {
        // ★ 四个单位都喂：0 点要按 frame / insn / guest_ms / wall_ms 四个单位重定基。
        return input_script_.noteMarker(*cpu_.windows, last_marker_sample_, presenter_.frameCount(), clock_.nowMs(),
                                        cpu_.insns, wallMs());
    }
    MarkerSample s = ident;
    if (s.valid) marker_sampler_.resolveName(mem_, &s);
    last_marker_sample_ = std::move(s);
    return input_script_.noteMarker(*cpu_.windows, last_marker_sample_, presenter_.frameCount(), clock_.nowMs(),
                                    cpu_.insns, wallMs());
}

void Machine::tickInteraction(X86& cpu) {
    if (!interaction_.active()) return;
    if (cpu.windows == nullptr) return;
    // ★ 顺序是判据的一部分：先让图判定并在命中时**追加**动作（`at frame <F+1>`，本块边界内绝不生效），
    //   再由 actuator 推进（真正发 WM_* + 自动 hold + 延迟抬起）。
    interaction_.tick(*cpu.windows, sampler_, presenter_.frameCount(), clock_.nowMs(), cpu.insns, wallMs());
    pumpScript(interaction_.actuator(), cpu, wallMs());
    // ★ 图在**宿主事件泵**里失败 / 完成时，guest 可能正阻塞在 `GetMessageA`（那时没有块边界）
    //   ⇒ 不叫醒它就永远到不了 `Machine::run` 的那条检查（`quit` 早就是这么做的，图没有理由不）。
    //   只投一次：投过就记账，避免每 5 ms 往队列里塞一条 WM_QUIT。
    if (!interaction_stop_posted_ && (interaction_.failed() || interaction_.done())) {
        interaction_stop_posted_ = true;
        cpu.windows->postQuit(0);
    }
}

RunResult Machine::run(const RunOptions& opts) {
    RunResult r;
    // ★ 单令牌（guest_token.h）：本函数起的执行循环是 guest 指令的**唯一**主执行流；
    //   音频 feeder 真线程（thread_mode_）走同一把令牌。arm 打开违规计数（未持有令牌就
    //   改 GuestHeap / HandleTable 或执行 guest 代码 ⇒ ++），`--assert-guest-token` 读它。
    guestToken().arm();
    // ★ GPU 覆盖账本按**这一轮**重算（`--gpu-strict` / dumpRunSummary 读的就是它）。
    d3d9::gpuCoverageReset();
    shims_.setTrace(opts.trace_shims, opts.trace_out ? opts.trace_out : stdout);
    // ★ 诊断：`LIFT_FRAME_TRACE=1` ⇒ 每次 Present 后打"这一帧墙钟去哪了"。
    //   只读、默认关；shim 计时只在打开时才有 steady_clock 开销。
    // ★ `--profile` 也要 shim 墙钟：`dumpProfile` 里的 shim 表读的就是这份累计（否则恒 0）。
    //   代价 = 每次 shim 调用多两次 steady_clock（`--bench --profile` 同用时 MIPS 不可当基准）。
    frame_trace_ = std::getenv("LIFT_FRAME_TRACE") != nullptr;
    // ★ 区间 profiler：只有脚本里真的写了 `profile_begin/end` 才 armed（⇒ 没有这些动作时
    //   整条记账路径都是 no-op：不会因为"加了 profiler"给所有运行多两次 steady_clock）。
    window_prof_.setArmed(input_script_.hasProfileActions());
    // ★ marker：每轮重置 seed / 边沿 / 超时 / 采样计数（同一台 Machine 可以跑多轮 ——
    //   ⛔ 不许把上一轮的 (cur,id) 当基线，否则第二轮的第一个样本就可能误发一次边沿）。
    input_script_.resetMarkerState();
    marker_sampler_.resetCounters();
    last_marker_sample_ = MarkerSample{};
    shims_.setTiming(frame_trace_ || opts.profile || window_prof_.armed());
    // ★ 运行摘要 `instr :` 行（`docs/03-emulator/measurement.md` M-02 的**口径标签**）：
    //   记的是**这一轮真正生效**的位，⛔ 不是 CLI 回显 —— `window-prof` 读 `WindowProfiler::armed()`
    //   的真值（脚本里写了 `profile_begin` 但没 arm 的缺陷会在这里如实暴露，见 §2.5 单）。
    instrument_ = InstrumentationState{};
    // ★ SEH 派发状态机：**跨块迭代**存活（一次 raise 可能要在多个块边界上调多个 handler）⇒
    //   只在这里清一次。⛔ 不许在块边界清（那会把"等 handler 返回"丢成静默 no-op）。
    seh_.reset();
    instrument_.profile = opts.profile;
    instrument_.frame_trace = frame_trace_;
    instrument_.window_prof = window_prof_.armed();
    instrument_.expect_change = expect_policy_.enabled;
    // ★ 时间线的效果级判据要读**画面采样**：没接线时 `sampleCount()` 恒 0 ⇒ 会把"没接采样器"
    //   误报成"识别源失明"。只在本策略开时接（缺省零成本）；交互图的采样器由 `loadInteractionGraph`
    //   在解析期接好，⛔ 这里不碰它（`sampler_.reset()` 会把图的矩形登记清掉）。
    if (!interaction_.active()) {
        if (expect_policy_.enabled) {
            sampler_.reset();  // 跨 run 时清掉 `last_.hash`（否则第一个样本会被记成 changed）
            sampler_.setWatchFrame(true);
        }
        presenter_.setFrameSampler(expect_policy_.enabled ? &sampler_ : nullptr);
    }
    expect_windows_.clear();
    expect_stats_ = ExpectChangeStats{};
    expect_failed_ = false;
    expect_failure_.clear();
    // ★ 防御性：策略开着但**当前脚本里没有匹配的动作**（配置之后又换了脚本，或脚本被清空）
    //   ⇒ 一个观测窗都不会开。⛔ 不许"策略开着却 exit 0"：这里直接判死，run 尾部照常改判
    //   `InteractionFailed`（detail 写明 `no-action`）。配置期的正规拦截在 `configureExpectChange`。
    if (expect_policy_.enabled) {
        bool any = false;
        for (const InputScript::Action& a : input_script_.actions()) {
            if (expectWatches(a.kind)) {
                any = true;
                break;
            }
        }
        if (!any) {
            expect_failed_ = true;
            expect_failure_ =
                "expect-change: no-action（脚本里没有匹配 --expect-change-after " + expect_policy_.text +
                " 的动作 ⇒ 一个观测窗都开不了；⛔ 不静默算过）";
            std::fprintf(stderr, "[expect-change] FAIL %s\n", expect_failure_.c_str());
        }
    }
    wall_start_ = std::chrono::steady_clock::now();
    wall_start_valid_ = true;
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
    // ★ 方案 D：打开音频设备 / dump（Off ⇒ 零成本）。begin 失败只降级，不阻止 boot。
    {
        std::string audio_err;
        if (!audio_.begin(&audio_err) && !audio_err.empty()) {
            std::fprintf(stderr, "[audio] begin 失败：%s\n", audio_err.c_str());
        }
    }
    // ★ 诊断开关（**默认关**，见 guest_token.h 的文件头）：`LIFT_TOKEN_DIAG_SKIP_LOCK=1` 让
    //   `GuestToken` 跳过 mutex —— 只为"派发层每块交多少固定税"的 A/B 存在。
    //   这里用**默认级别**（stderr，不经 LogLevel 门）喊一声：⛔ 不许让它悄悄改掉性能读数。
    //   ⛔ 只允许单线程工况；若音频 feeder 真线程已经在跑，必须喊得更大声（那已经不是"少一把锁"，
    //   而是"容器互斥被静默关掉"）。放在 audio_.begin() **之后** ⇒ `threadMode()` 才是真值。
    if (guestToken().diagSkipLock()) {
        std::fprintf(stderr,
                     "[guest-token] ⚠ 诊断开关 LIFT_TOKEN_DIAG_SKIP_LOCK=1：GuestToken 跳过 mutex"
                     "（acquire/release 只做原子记账）—— 只允许 --audio off 这类**单线程**工况；"
                     "本臂是诊断读数，⛔ 不是产品行为。\n");
        if (audio_.threadMode()) {
            std::fprintf(stderr,
                         "[guest-token] ⛔ 音频 feeder **真线程已启动**（mode=%s）⇒ 两条执行流会同时改宿主容器："
                         "本臂读数**无效**，且可能损坏 guest 堆。先用 --audio off 重跑。\n",
                         audioModeName(audio_.mode()));
        }
    }
    // 诊断开关：LIFT_TRACE_FULL=1 ⇒ `--trace-blocks` 每条带操作数文本 + 寄存器快照（缺省关）
    const bool traceFull = std::getenv("LIFT_TRACE_FULL") != nullptr;
    cpu_.running = true;
    // ★ 停机种类**按轮重置**：同一台 Machine 可以跑多轮（tests 的 `runAsmRaw` 就是），
    //   上一轮 trap / 资源耗尽的提示绝不许漏进这一轮的正常主动停（否则 ExitProcess 会被
    //   误判成 `Trap`）。置位点只有 shim 的停机函数；这里不碰 `stop_why`（既有行为：它跨轮
    //   保留，本单只负责新字段）。
    cpu_.stop_kind = HostStopKind::None;
    const auto wall_start = std::chrono::steady_clock::now();
    std::uint64_t next_time_check = 0;
    std::uint64_t next_present_check = 0;
    // ★ native override 的入口判据：**上一条执行的是 `call`**，且目标就是当前 eip。
    //   只按 EA 拦会把 `jmp`/尾调用也当成函数入口（那时栈上没有本次调用的返回地址，
    //   抬栈 + 弹返回地址 = 静默改坏控制流）。所以 call 目标只对**下一次** dispatcher 迭代有效。
    std::uint32_t next_call_target = 0;

    while (cpu_.running) {
        // ★ 区间 profiler 的**窗口边界只在这里**开/关：上一个 block 边界的脚本 tick 只登记请求
        //   ⇒ 同一脚本重复跑得到同一个边界（⛔ 不在动作应用点上就地开关）。
        if (profile_begin_pending_ || profile_end_pending_) applyPendingProfileWindow();
        // ★ 迭代守卫：一次 dispatcher 迭代的墙钟里，**没被显式记进任何段**的剩余部分记成 interp
        //   （解释器自身：块缓存查找 / 令牌 / 时钟 / profile 记账 …）。析构在 continue/break 时也会跑。
        struct IterScope {
            WindowProfiler& p;
            bool on;
            explicit IterScope(WindowProfiler& prof) : p(prof), on(prof.armed()) {
                if (on) p.iterationBegin(std::chrono::steady_clock::now());
            }
            ~IterScope() {
                if (on) p.iterationEnd(std::chrono::steady_clock::now());
            }
        } iter_scope(window_prof_);
        // ★ 单令牌：**每个块边界取一次、块结束放**（guest_token.h）。
        //   块内的一切 guest 执行（块指令 / shim / 嵌套 guest 调用 / 定时器回调）都在令牌里；
        //   feeder 真线程只能在这两条语句之间拿到令牌。
        //   ⛔ 不要把它挪进某个分支：漏一条路径 = 那条路径与 feeder 并发改宿主容器。
        GuestToken::Guard token_guard(guestToken());
        // ★ 检查点：Guard 被删掉/短路时这里立刻计数（`--assert-guest-token` 判红）。
        guestToken().check("machine.block");
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
        // ★ 指令式输入时间线（`--input-script`）：在**块边界**推进。不依赖窗口 sink / OS 焦点 /
        //   真实光标：它只写 `cpu_.windows` 的那一份状态并发同组 WM_*（见 runtime/input.h）。
        //   ⛔ 嵌套 guest 调用（WndProc / 音频 fill）与定时器回调进行中不注入：那些帧的 eip 不是
        //   主循环的块边界，插进去会把"一帧内 down+up"这类节流语义搅乱。
        //   ⛔ SEH 派发期间更不许（handler 是 guest 代码，插进去会改坏它仍活着的栈/寄存器）。
        if (input_script_.active() && !guest_calls_.active() && !cpu_.shim.timer_firing && !seh_.busy()) {
            const auto t_input = window_prof_.active() ? std::chrono::steady_clock::now()
                                                       : std::chrono::steady_clock::time_point{};
            // ★ marker 采样点 = VM 循环头的**块入口**（`0x4123F0`）与消息泵回来的块入口
            //   （`0x4123E5`；它线性跨过循环头，见 runtime/engine_marker.h 的头注）。
            //   ⛔ 不是任意块边界：帧装载器（`sub_40ED40` 的 `0x6` 路径）会把目标槽号临时写进
            //   `cur` 再还原 ⇒ 在装载器内部的块边界采样会读到**错的 cur**。
            //   没有 Marker 动作时这一段一次也不进（零成本）。
            // ★ 门控必须问 `needsIdentitySamples()`（= marker 动作 **或** 0 点配置）：
            //   ⛔ 只问 `hasMarkerActions()` 的话，"只有 `--anchor-script`、没有 marker 动作"的脚本
            //     **一次也不采样** ⇒ 0 点永不闩锁 ⇒ 收尾假报 `origin-missed:`（本单最容易漏的一条）。
            if (input_script_.needsIdentitySamples() &&
                (cpu_.eip == kMarkerLoopHeadEa || cpu_.eip == kMarkerPumpReturnEa)) {
                sampleMarker();
            }
            tickInputScript(cpu_);
            if (window_prof_.active())
                window_prof_.addHost(WindowProfiler::HostInput,
                                     static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                                    std::chrono::steady_clock::now() - t_input)
                                                                    .count()));
            // ★ 时间线的效果级判据：失败**优先于** quit（同一 tick 里动作判死 + quit 到期时，
            //   不能把"没生效"读成正常收口）。复用 `InteractionFailed`（⛔ 不新增枚举成员）；
            //   `detail` 里写清是三种里的哪一种，`main` 走 exit 5。
            if (expect_failed_) {
                if (profile_begin_pending_ || profile_end_pending_) applyPendingProfileWindow();
                r.reason = StopReason::InteractionFailed;
                r.detail = expect_failure_;
                cpu_.running = false;
                break;
            }
            // ★ marker 超时 ⇒ **响亮失败**（复用 `InteractionFailed` + 分流标志；⛔ 不新增枚举成员）。
            //   `!quitRequested()`：同 tick 里用户显式 `quit` 赢 —— ⛔ 不把"脚本自己要走"报成超时。
            if (input_script_.markerTimedOut() && !input_script_.quitRequested()) {
                if (profile_begin_pending_ || profile_end_pending_) applyPendingProfileWindow();
                r.reason = StopReason::InteractionFailed;
                r.detail = input_script_.markerTimeoutDetail();
                r.marker_timeout = true;
                std::fprintf(stderr, "[marker] FAIL %s\n", r.detail.c_str());
                cpu_.running = false;
                break;
            }
            if (input_script_.quitRequested()) {
                // ★ 先处理本 tick 登记的窗口边界：否则脚本最后一条 `profile_end` 会被 `quit`
                //   吞掉（那一段统计就静默丢了）。
                if (profile_begin_pending_ || profile_end_pending_) applyPendingProfileWindow();
                r.reason = StopReason::Stopped;
                r.detail = "input script: quit";
                cpu_.running = false;
                break;
            }
        }
        // ★ 识别驱动的交互任务（`--interaction-graph`）：同样的门控（嵌套 guest 调用 / 定时器回调
        //   进行中不推进）。三种失败 = **非 0 退出**（StopReason::InteractionFailed 不进 `ok()`）；
        //   任务走完（`next` 空）与时间线的 `quit` 一样收口成 `Stopped`（那是**成功**结束，
        //   判据是 trace 最后的 `task done`，不是退出码）。
        if (interaction_.active() && !guest_calls_.active() && !cpu_.shim.timer_firing && !seh_.busy()) {
            const auto t_graph = window_prof_.active() ? std::chrono::steady_clock::now()
                                                       : std::chrono::steady_clock::time_point{};
            tickInteraction(cpu_);
            if (window_prof_.active())
                window_prof_.addHost(WindowProfiler::HostInput,
                                     static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                                    std::chrono::steady_clock::now() - t_graph)
                                                                    .count()));
            if (interaction_.failed()) {
                if (profile_begin_pending_ || profile_end_pending_) applyPendingProfileWindow();
                r.reason = StopReason::InteractionFailed;
                r.detail = std::string("interaction: ") + graphFailureName(interaction_.failure()) + " · " +
                           interaction_.failureDetail();
                cpu_.running = false;
                break;
            }
            if (interaction_.done() || interaction_.actuator().quitRequested()) {
                if (profile_begin_pending_ || profile_end_pending_) applyPendingProfileWindow();
                r.reason = StopReason::Stopped;
                r.detail = interaction_.done() ? "interaction: task done" : "interaction: quit";
                cpu_.running = false;
                break;
            }
        }
        // ★ 显示后端：块边界也要 pump（否则两次 Present 之间窗口假死）。无 sink ⇒ 零成本。
        //   ⛔ 嵌套 guest 调用进行中不 pump：音频 fill（或任何 native 嵌套调用）正在跑 guest
        //   代码，这时再铺一个 WndProc 帧会改坏它仍活着的 x87/XMM 上下文（真机上它们是两个线程）。
        const bool nested_guest_active = guest_calls_.active() || seh_.busy();
        if (!nested_guest_active && presenter_.hasSink() && cpu_.insns >= next_present_check) {
            const auto t_pump = window_prof_.active() ? std::chrono::steady_clock::now()
                                                      : std::chrono::steady_clock::time_point{};
            presenter_.pumpHostEvents();
            if (window_prof_.active())
                window_prof_.addHost(WindowProfiler::HostEventPump,
                                     static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                                    std::chrono::steady_clock::now() - t_pump)
                                                                    .count()));
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
            // ★ 音频泵借用了 [esp] 作为 sub_4B6C30 的返回点；续延完成后必须还回原返回地址。
            audio_.afterGuestCall();
            // ★ 续延里也可能**主动结束运行**（如 `LIFT_STOP_AFTER_MOVIE` 在 DirectShow 图的
            //   停机链里置 `cpu.running=false`）。它不在 `shims_.invoke` 那条路上 ⇒ 不会经过
            //   本循环末尾"shim 返回后"的 `!cpu_.running` 检查；而 `while (cpu_.running)` 直接
            //   退出会让 `r.reason` 停在 `None`（`RunResult::ok()` 为假 ⇒ main 返回 2）。
            //   这里补上停止原因，口径与那条分支一致：这是"实现里主动停"，不是缺口。
            if (!cpu_.running) {
                r.reason = StopReason::Stopped;
                r.detail = cpu_.stop_why ? cpu_.stop_why : "guest 调用续延里主动结束运行";
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
        // ★ SEH / C++ EH 的派发（`RaiseException` / `RtlUnwind`）：**只在块边界推进**。
        //   shim 只搬参数 + 把 eip 交给 `kSehDispatchEa`；handler 帧在这里铺（同 `fireDueTimer` 的
        //   形状：铺 guest 栈 → 调 guest 函数 → 回来收 disposition）。⛔ 不在 shim 里同步跑 guest。
        //   顺序在 `until_ea` **之前**：handler 的合成返回点（`kSehHandlerReturnEa`）不是可解码地址，
        //   晚于它就会被当成 `decode-error`（那会把"异常没人接"误报成"解码失败"）。
        if (seh_.busy()) {
            const bool seeded = pumpSeh(r);
            if (r.reason != StopReason::None) break;
            if (seeded) continue;
        }
        if (opts.until_ea && cpu_.eip == opts.until_ea) {
            r.reason = StopReason::UntilEa;
            break;
        }
        if (opts.max_insns && cpu_.insns >= opts.max_insns) {
            r.reason = StopReason::MaxInsns;
            break;
        }
        // ★ `--skip-logo`：T_Command 已构造、show-logo 已初始化为 1，且指针尚未发布。
        //   在这里把引擎自己的字段写 0 —— 之后 `mov ds:dword_55E1BC, eax` 发布的就是
        //   "show-logo=0" 的对象；root script 的 `0x130` 因而跳过 `call-script 5262`。
        //   ⛔ 不跳过 boot、不改 root script、不按帧号猜 LOGO。
        if (skip_logo_ && !skip_logo_checked_ && cpu_.eip == kShowLogoInitEa) {
            skip_logo_checked_ = true;
            const std::uint32_t object = cpu_.eax();
            if (object != 0 && mem_.committed(object + kShowLogoFieldOffset, 4)) {
                const std::uint32_t before = mem_.rd32(object + kShowLogoFieldOffset);
                mem_.wr32(object + kShowLogoFieldOffset, 0);
                skip_logo_applied_ = true;
                skip_logo_object_ = object;
                std::printf("[skip-logo] T_Command=0x%08X show-logo(+0x%05X): %u -> 0\n", object,
                            kShowLogoFieldOffset, before);
                if (before != 1) {
                    std::fprintf(stderr,
                                 "[skip-logo] ⚠ 初值不是 1（读到 %u）：写 0 仍执行，但钩子/字段可能已漂\n",
                                 before);
                }
            } else {
                std::fprintf(stderr,
                             "[skip-logo] 钩子 @ %08X 命中，但对象指针 0x%08X 不可写（+0x%05X）⇒ 未应用\n",
                             kShowLogoInitEa, object, kShowLogoFieldOffset);
            }
        }
        // ★ 到期的 winmm 定时器在**块边界**触发（D5：没有定时器服务线程）。
        //   ⛔ 不嵌套：一个回调没跑完就不再触发下一个（与真机的单线程回调串行一致）。
        //   ⛔ 嵌套 guest 调用（音频 fill / native 回调）进行中也不触发：定时器只保 GPR，
        //      在 fill 的 x87 活值中间插入回调会静默改坏 ST0..ST7。
        if (!guest_calls_.active() && !cpu_.shim.timer_firing && !cpu_.shim.timers.empty() &&
            !cpu_.shim.callback.active && !seh_.busy()) {
            const auto t_timer = window_prof_.active() ? std::chrono::steady_clock::now()
                                                       : std::chrono::steady_clock::time_point{};
            const bool fired = fireDueTimer(r);
            if (window_prof_.active())
                window_prof_.addHost(WindowProfiler::HostTimer,
                                     static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                                    std::chrono::steady_clock::now() - t_timer)
                                                                    .count()));
            if (fired) continue;
            if (r.reason != StopReason::None) break;
        }

        // ★ guest 函数级 native override：只在**函数入口**（block 入口）按 EA 拦截。
        //   成功 ⇒ 按 ABI 设好 EAX/ESP/EIP 并直接回 dispatcher；失败 ⇒ 落回解释器执行原字节。
        //   ⛔ 不 patch guest 字节（SMC / 代码页保护都不用管）。
        //   ⛔ 只认"上一条是 call 且目标 = 当前 eip"：jmp / 尾调用 / 定时器和回调的合成帧
        //      都不是 call 帧，必须回退解释器（否则会弹掉一个不存在的返回地址）。
        // ★ guest 影子调用栈（inclusive）：上一个 block 以 `call` 结束 ⇒ 这一帧从这里开始计时。
        //   ⛔ 只在 profiler 窗口内记账（窗口外 no-op）；窗口结束由 `closeAllFrames` 收口。
        //   压帧发生在 override 之前：函数级 override **不会执行 ret**，由那条分支显式弹帧。
        if (window_prof_.active() && call_target != 0 && cpu_.eip == call_target) {
            window_prof_.pushCall(fnKeyOf(call_target), pending_call_ret_);
        }
        if (!overrides_.empty() && call_target != 0 && cpu_.eip == call_target) {
            if (NativeOverride* ov = overrides_.find(cpu_.eip)) {
                const char* why = nullptr;
                const std::uint64_t before = ov->fallbacks;
                // ★ 指令数基线：override 自己可能补 `cpu_.insns`（例如 native_voice 的时钟补偿）
                //   ⇒ 只按**实际增量**记，⛔ 不硬编码 "+1"（那会把补偿漏成闭合差）。
                const std::uint64_t insns_before_override = cpu_.insns;
                bool suspended = false;
                const auto t_native = window_prof_.active() ? std::chrono::steady_clock::now()
                                                            : std::chrono::steady_clock::time_point{};
                const bool took = overrides_.invoke(*ov, cpu_, mem_, &why, &suspended);
                if (window_prof_.active())
                    window_prof_.addNative(
                        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                       std::chrono::steady_clock::now() - t_native)
                                                       .count()));
                if (took) {
                    // override 替代了整个函数体（没有 ret 指令）⇒ 这里显式弹帧。
                    if (window_prof_.active()) window_prof_.popNoReturn(fnKeyOf(ov->ea));
                    if (suspended) {
                        window_prof_.addOverrideInsns(cpu_.insns - insns_before_override);
                        continue;
                    }
                    if (opts.trace_blocks) std::fprintf(stdout, "[native] %08X %s\n", ov->ea, ov->name.c_str());
                    cpu_.noteInsn(ov->ea, "[native]");
                    ++cpu_.insns;
                    clock_.tick();
                    // ★ 补账：这条 `++cpu_.insns`（+ override 自己的补偿）**不在任何块里**。
                    window_prof_.addOverrideInsns(cpu_.insns - insns_before_override);
                    continue;
                }
                if (ov->fallbacks != before && ov->fallbacks == 1 && why) {
                    std::fprintf(stderr, "[native] %s @ %08X 回退解释器：%s\n", ov->name.c_str(), ov->ea, why);
                }
                // ★ 回退路径也按**实际增量**补账：fn 可能已经改过 `cpu_.insns` 才回退
                //   （块随后从新基线开始 ⇒ 不补就会在闭合式里少一截）。
                window_prof_.addOverrideInsns(cpu_.insns - insns_before_override);
            }
        }

        // ★ 段级 override（**函数中段**的固定 EA）：在每个 block 入口按 EIP 查表 —— 与上面的
        //   函数级不同，这里**没有** call 帧语义：fn 自己写 EIP（段出口）/ 寄存器 / EFLAGS。
        //   ★ insns / 虚拟时钟按 fn 报告的**被替代指令数**补账：不补的话 `timeGetTime` 会少走一截，
        //     首帧内容可能偏离解释器路径（A4 / 首帧 hash 的硬要求）。见 README §4.3。
        if (!block_overrides_.empty()) {
            if (NativeBlockOverride* bo = block_overrides_.find(cpu_.eip)) {
                const std::uint32_t entry = cpu_.eip;
                std::uint64_t replaced = 0;
                const auto t_blockov = window_prof_.active() ? std::chrono::steady_clock::now()
                                                             : std::chrono::steady_clock::time_point{};
                const BlockOverrideOutcome outcome = bo->fn(cpu_, mem_, &replaced);
                if (window_prof_.active())
                    window_prof_.addNative(
                        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                       std::chrono::steady_clock::now() - t_blockov)
                                                       .count()));
                if (outcome == BlockOverrideOutcome::Replaced) {
                    if (opts.trace_blocks)
                        std::fprintf(stdout, "[native-block] %08X %s\n", entry, bo->name.c_str());
                    cpu_.noteInsn(entry, "[native-block]");
                    ++bo->hits;
                    const std::uint64_t n = replaced != 0 ? replaced : 1ull;
                    cpu_.insns += n;
                    clock_.tick(n);
                    // ★ 补账：段级 override 替代的指令数**不在任何块里**（闭合式里单独一类）。
                    window_prof_.addOverrideInsns(n);
                    continue;
                }
                if (outcome == BlockOverrideOutcome::Declined) {
                    // 前置条件不匹配 ⇒ 一次就够：段级项的前提是静态的（同一 EA / 同一段代码），
                    // 继续每迭代重试只是把成本加回热路径。首次打印一行，之后不再尝试。
                    if (!bo->disabled) {
                        ++bo->declines;
                        bo->disabled = true;
                        std::fprintf(stderr,
                                     "[native] 段级 %s @ %08X 前置条件不匹配 ⇒ 本次运行停用该项（回退解释器）\n",
                                     bo->name.c_str(), entry);
                    }
                } else {
                    if (bo->fallbacks == 0)
                        std::fprintf(stderr, "[native] 段级 %s @ %08X 执行失败 ⇒ 回退解释器\n",
                                     bo->name.c_str(), entry);
                    ++bo->fallbacks;
                }
            }
        }

        // ★ 方案 D：音频设备推进 + 喂料泵。`pump()` 可能铺一个 sub_4B6C30 帧（返回 Suspended）；
        //   之后 guest 自己跑到续延点，`afterGuestCall()` 再还 [esp]。⛔ 不建线程 / 不建第二栈。
        //   ⛔ 放在函数级/段级 override **之后**：否则会吞掉 `call_target`，让本次 native override
        //      被跳过（性能退化，不是语义错，但没必要）。
        if (audio_.enabled() && !guest_calls_.active() && !cpu_.shim.timer_firing && !seh_.busy()) {
            const auto t_audio = window_prof_.active() ? std::chrono::steady_clock::now()
                                                       : std::chrono::steady_clock::time_point{};
            const AudioPump ap = audio_.pump();
            if (window_prof_.active())
                window_prof_.addHost(WindowProfiler::HostAudio,
                                     static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                                    std::chrono::steady_clock::now() - t_audio)
                                                                    .count()));
            if (ap == AudioPump::Suspended) continue;
            if (ap == AudioPump::Error) {
                r.reason = StopReason::Unimplemented;
                r.detail = audio_.lastError();
                break;
            }
        }

        // ★ 调试观察口（默认关）：`LIFT_WATCH_SAMPLE=1` 时，每个 block 入口读一次
        //   `HostObjects` 登记的样本对象头 `[sample]`；值变了 ⇒ 打一行（含当前/上一个 block 起点）。
        //   只观察、不改语义；不设环境变量时零成本。
        {
            static const bool watch_on = std::getenv("LIFT_WATCH_SAMPLE") != nullptr;
            static std::uint32_t watch_prev_entry = 0;
            static std::uint32_t watch_last_val = 0;
            static std::uint32_t watch_last_ea = 0;
            static bool watch_seeded = false;
            if (watch_on && lift::watchSampleEa() != 0) {
                const std::uint32_t watch_ea = lift::watchSampleEa();
                if (watch_ea != watch_last_ea) {
                    // 观察点换了（`GuestCallStack::start` 每次把 watch 指向新的回调帧返回槽）⇒ 重新播种；
                    // 否则会把"新帧的初始值"误报成一次改写。
                    watch_last_ea = watch_ea;
                    watch_seeded = false;
                }
                if (mem_.committed(watch_ea, 4u)) {
                    const std::uint32_t v = mem_.rd32(watch_ea);
                    if (!watch_seeded) {
                        watch_last_val = v;
                        watch_seeded = true;
                    } else if (v != watch_last_val) {
                        // 解析"上一个 block"是谁：shim 块 ⇒ module!name（写者就是它）。
                        char who[160];
                        std::snprintf(who, sizeof(who), "?");
                        if (Block* pb = cache_.get(watch_prev_entry)) {
                            if (pb->is_shim) {
                                if (const ShimDef* d = shims_.byId(pb->shim_id)) {
                                    std::snprintf(who, sizeof(who), "%s!%s", d->module.c_str(), d->name.c_str());
                                }
                            }
                        }
                        std::fprintf(stderr,
                                     "[watch] ea=0x%08X old=0x%08X new=0x%08X at block 0x%08X "
                                     "(prev block 0x%08X = %s)\n",
                                     watch_ea, watch_last_val, v, cpu_.eip, watch_prev_entry, who);
                        watch_last_val = v;
                    }
                }
            }
            watch_prev_entry = cpu_.eip;
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
            const auto t_shim = window_prof_.active() ? std::chrono::steady_clock::now()
                                                      : std::chrono::steady_clock::time_point{};
            shims_.invoke(cpu_, *def);
            if (window_prof_.active()) {
                window_prof_.addShim(
                    static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                   std::chrono::steady_clock::now() - t_shim)
                                                   .count()));
                // shim 不执行 `ret` 指令 ⇒ 压进来的那一帧在这里弹掉（含 take_control 的回调帧：
                // 回调自己的 call/ret 是独立的一帧，晚些时候由 ret 正常弹）。
                window_prof_.popNoReturn(fnKeyOf(def->stub_ea));
            }
            if (frame_trace_ && presenter_.frameCount() != frame_before) flushFrameTrace();
            if (!cpu_.running) {
                // ★ 三种"停"要分清：
                //   ① `!had_impl` ⇒ 注册表里没有 fn = 缺口（`Unimplemented`，红）；
                //   ② `had_impl` + 宿主停机种类非 None ⇒ **不是正常结局**：
                //      `ResourceExhausted`（我方 arena / guest 堆不够）/ `Trap`（已注册的槽/形态没实现）；
                //   ③ `had_impl` + 种类 None ⇒ 实现里主动停（`ExitProcess` / `TerminateProcess` /
                //      `--max-frames` / 影片放完 / 窗口关闭 …）⇒ `Stopped`（正常结局）。
                //   ★ 为什么不能只看 `had_impl`：它对 ② 与 ③ **都是真** —— `ExitProcess` 与
                //     `d3dxResourceStop` / `comUnsupported` 的 fn 都非空。这就是置位点额外写
                //     `X86::stop_kind`（纯宿主字段；`Machine::run` 每轮开头重置）的原因。
                //   ★ 顺序固定：`!had_impl` 必须在种类判断之前（未实现的导入也带 `stop_why`，
                //     但它是 `Unimplemented`，不是 `Trap`）。
                if (!had_impl) {
                    r.reason = StopReason::Unimplemented;
                    r.detail = std::string("未实现的导入 ") + def->module + "!" + def->name;
                } else {
                    if (cpu_.stop_kind == HostStopKind::ResourceExhausted) {
                        r.reason = StopReason::ResourceExhausted;
                    } else if (cpu_.stop_kind == HostStopKind::Trap) {
                        r.reason = StopReason::Trap;
                    } else {
                        r.reason = StopReason::Stopped;
                    }
                    // ★ `stop_why` 非空时**优先用它**：否则"资源耗尽"与"未实现"在运行摘要里
                    //   都长成"shim 主动结束运行"，误诊只是从 stderr 挪到了 stdout。
                    r.detail = cpu_.stop_why
                                   ? std::string(cpu_.stop_why) + "（shim " + def->module + "!" + def->name + "）"
                                   : std::string("shim 主动结束运行：") + def->module + "!" + def->name;
                }
                break;
            }
            continue;
        }

        bool left_block = false;
        const auto block_start = std::chrono::steady_clock::now();
        // ★ 块级**指令数**的基线（REQ-01M6XVZ8AXJNG492WA2QAMTA35）：块内每执行成功一条就
        //   `++cpu_.insns` ⇒ "这一块执行了几条" = 前后差值（⛔ 不在热循环里再加一个自增；
        //   中途 break 掉的剩余指令本来就没进 `cpu_.insns`，差值天然只数执行过的）。
        const std::uint64_t insns_before_block = cpu_.insns;
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
                if (in.op == Op::Call) {
                    // 影子调用栈的压帧在**下一个迭代**（那时才认得出目标是不是 shim）。
                    pending_call_ret_ = in.ea + in.length;
                } else if (in.op == Op::Ret) {
                    // ★ 弹帧判据 = 返回地址与栈上的帧一致（`popRet` 里做，对不上只计数不瞎弹）。
                    window_prof_.popRet(cpu_.eip);
                }
                left_block = true;
                break;
            }
        }
        // ★ 区间 profiler 的 `guest` 桶 = **块执行**墙钟（与 shim / native / host 互斥）。
        //   `b->ns` 复用同一份计时（就是 `--profile` 与逐帧 trace 用的那一份，⛔ 不造第二套）。
        {
            const std::uint64_t block_ns = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - block_start)
                    .count());
            if (opts.profile || window_prof_.armed()) b->ns += block_ns;
            window_prof_.addGuest(block_ns);
            // ★ 块级指令数（同一窗口口径；窗口外 no-op）。差值的另一类来源 —— native override
            //   直接补进 `cpu_.insns` 的条数 —— 在下面两个 override 分支里单独记（`addOverrideInsns`）。
            window_prof_.addBlockInsns(b->ea, cpu_.insns - insns_before_block);
        }
        if (left_block) continue;
    }
    if (r.reason == StopReason::UntilEa && opts.until_ea == 0xFFFFFFFFu) {
        // 岗哨返回地址 = 夹具的正常结束，不改口径（调用方自己看 reason）
    }
    // ★ 运行停止时窗口还开着（脚本里忘了 profile_end / 脚本没跑完）⇒ **明确**收口 + 告警 + 打印。
    //   ⛔ 不许静默丢数据：漏掉一个 end 不该让整段统计消失。
    if (window_prof_.active()) {
        std::fprintf(stderr,
                     "[profile-window] ⚠ 窗口 \"%s\" 未闭合（运行停止：%s）⇒ 自动收口并打印（差一段就是漏账）\n",
                     window_prof_.label().c_str(), stopReasonName(r.reason));
        closeProfileWindow("未闭合");
    }
    // ★ 时间线的效果级判据：跑完仍未判定的观测窗**必须响亮**（⛔ 不许"跑完就算过"——
    //   `--max-frames` / 脚本尾部 `quit` 提前停都会让"点了没反应"静默通过）。
    //   三种分类（互斥，判据只读采样器的两个单调计数）：
    //     no-sample   ：动作之后**一次采样都没有**（识别源失明：没接采样器 / 一直没 Present）；
    //     not-changed ：采样在增长、窗口也走满了，但 `changeCount` 一次没动（动作没生效）；
    //     unclosed    ：采样增长过但没攒够窗口（⛔ 不判定 = 不算过）。
    //   ★ 只在 `r.ok()` 时改判：run 已经以 Unimplemented / DecodeError / Trap / ResourceExhausted
    //     结束时，那个停因更具体，⛔ 不许被 expect 吞掉（只打一条告警，退出码已非 0）。
    if (expect_failed_) {
        if (r.ok()) {
            r.reason = StopReason::InteractionFailed;
            r.detail = expect_failure_;
        }
    } else if (!expect_windows_.empty()) {
        // ★ 防御性收口：采样器已经增长、但最后一个块边界没来得及判（理论上不会走到 ——
        //   `evaluateExpectWindows` 在每个块边界都跑）⇒ 按**满足**摘掉，⛔ 不算失败。
        const std::uint64_t samples = sampler_.sampleCount();
        const std::uint64_t changes = sampler_.changeCount();
        while (!expect_windows_.empty() && changes > expect_windows_.front().changes_at_start) {
            ++expect_stats_.windows_satisfied;
            expect_windows_.erase(expect_windows_.begin());
        }
        if (!expect_windows_.empty() && !r.ok()) {
            std::fprintf(stderr,
                         "[expect-change] ⚠ 运行以 %s 结束（已是非 0），仍有 %zu 个观测窗未判定；"
                         "不覆盖原停因（%s）\n",
                         stopReasonName(r.reason), expect_windows_.size(), r.detail.c_str());
        } else if (!expect_windows_.empty()) {
            const ExpectWindow& w = expect_windows_.front();
            // ★ 动作还没**交付完**（抬起还在队列里）就结束了 ⇒ 归因改成 `unclosed`：
            //   说成"窗口走满但画面没变"（not-changed）会把"没给观测窗留时间"误报成"动作没生效"。
            const bool delivered = input_script_.pendingReleaseCount() == 0;
            const char* kind = "unclosed";
            const char* why =
                "运行结束时窗口还没攒够采样 / 还没判定（⛔ 不判定 = 不算过；给观测窗留时间，"
                "如 wait / 更长的 --expect-change-window）";
            if (samples == w.samples_at_start) {
                kind = "no-sample";
                why =
                    "动作之后一次采样都没有（识别源失明：没接采样器 / guest 一直没 Present）"
                    "⇒ 无法确认变化";
            } else if (delivered && changes == w.changes_at_start &&
                       samples - w.samples_at_start >= expect_policy_.window_samples) {
                kind = "not-changed";
                why = "窗口内的采样在增长，但画面一次都没变（动作没生效？）";
            } else if (!delivered) {
                why =
                    "运行结束时这次动作的抬起还没发出（动作没交付完）⇒ 窗口不做\"没变\"的归因；"
                    "给观测窗与自动 hold 留时间（`wait` / 别在 click 后立刻 quit）";
            }
            failExpectWindow(w, kind, why);
            r.reason = StopReason::InteractionFailed;
            r.detail = expect_failure_;
        }
    }
    // ★ 时间线 0 点（`--anchor-script`）：跑完仍未闩锁 ⇒ **收尾时响亮失败**（裁决 7/8）。
    //   ① **只**在 `r.ok()` 时改判：run 已经以 Unimplemented / Trap / DecodeError 结束时，
    //      那个停因更具体，⛔ 不许被 0 点吞掉（与上面 expect / marker 同一条纪律）。
    //   ② **"进了但 id 是别的"不算失败**：判据只有"整个 run 结束都没**观察到** X"。
    //   ③ ⛔ 不新增 `StopReason`：复用 `InteractionFailed` + `origin_missed` 分流（main ⇒ 5）。
    if (input_script_.originRequested() && !input_script_.originSet() && r.ok()) {
        r.reason = StopReason::InteractionFailed;
        r.detail = input_script_.originMissedDetail(presenter_.frameCount(), wallMs());
        r.origin_missed = true;
        std::fprintf(stderr, "[anchor] FAIL %s\n", r.detail.c_str());
    }
    instrument_.expect_windows = expect_stats_.windows_opened;
    wall_start_valid_ = false;
    r.ea = cpu_.eip;
    r.insns = cpu_.insns;
    audio_.end();  // 关闭 SDL / 回填 WAV；此后不再写 guest
    // ★ run 结束：关掉违规计数。⚠ 计数本身**保留**（summary / `--assert-guest-token` 还要读），
    //   只是之后的 Machine 析构（COM/surface 回收 guest 堆）不再算进本轮账。
    guestToken().disarm();
    return r;
}

void Machine::applyPendingProfileWindow() {
    // ★ 窗口边界 = 本 block 边界（迭代起点）。同一脚本重复跑 ⇒ 同一个边界。
    if (profile_begin_pending_) {
        profile_begin_pending_ = false;
        if (window_prof_.active()) {
            std::fprintf(stderr,
                         "[profile-window] ⚠ 上一个窗口 \"%s\" 还没结束就又来 profile_begin(\"%s\") ⇒ "
                         "先把上一个自动收口\n",
                         window_prof_.label().c_str(), profile_begin_label_.c_str());
            closeProfileWindow("被新的 begin 顶掉");
        }
        window_prof_.begin(profile_begin_label_, presenter_.frameCount(), cpu_.insns, clock_.nowMs());
        // ★ 基线快照必须在**任何一段执行之前**取（全局累计量的差 = 窗口内的量）。
        window_prof_.snapshotBlocks(cache_);
        window_prof_.snapshotShims(shims_);
        window_prof_.snapshotPresent();
        std::printf("[profile-window] begin \"%s\" @ frame=%llu insns=%llu guest_ms=%llu\n",
                    profile_begin_label_.c_str(), static_cast<unsigned long long>(presenter_.frameCount()),
                    static_cast<unsigned long long>(cpu_.insns), static_cast<unsigned long long>(clock_.nowMs()));
        std::fflush(stdout);
    }
    if (profile_end_pending_) {
        profile_end_pending_ = false;
        if (!window_prof_.active()) {
            std::fprintf(stderr,
                         "[profile-window] ⚠ profile_end(\"%s\") 没有打开的窗口 ⇒ 忽略（⛔ 不静默：这就是"
                         "\"begin 丢了\"的信号）\n",
                         profile_end_label_.c_str());
            return;
        }
        if (!profile_end_label_.empty() && profile_end_label_ != window_prof_.label()) {
            std::fprintf(stderr,
                         "[profile-window] ⚠ profile_end(\"%s\") 与打开的窗口 \"%s\" label 不一致 ⇒ 仍然收口\n",
                         profile_end_label_.c_str(), window_prof_.label().c_str());
        }
        closeProfileWindow(nullptr);
    }
}

void Machine::closeProfileWindow(const char* why) {
    (void)why;
    // ★ 窗口收尾时 profiler 还没 arm ⇒ 下面这张表是**残表**：`iterations` / 函数表 self /
    //   shim 墙钟全是 0，但那**不**代表"这段没有开销"。⛔ 不许让它静默通过（工具面告警即可，
    //   不改退出码）。正常路径永远不会走到这里：`profile_begin` 被消费时会重新 arm
    //   （见 `pumpScript`）⇒ 走到这里说明"开窗"与"arm"之间又漏了一条路。
    if (!window_prof_.armed()) {
        std::fprintf(stderr,
                     "[profile-window] ⚠ 窗口 \"%s\" 收尾时区间 profiler **未 arm** ⇒ 这张表是残表"
                     "（iterations / 函数表 self / shim 墙钟全 0，⛔ 不代表这段没有开销）；"
                     "运行期追加的 profile_begin 必须重新 arm（见 Machine::pumpScript）\n",
                     window_prof_.label().c_str());
    }
    window_prof_.end(presenter_.frameCount(), cpu_.insns, clock_.nowMs());
    window_prof_.finishBlocks(cache_);
    window_prof_.finishShims(shims_);
    window_prof_.finishPresent();
    WindowReport rep;
    const auto fn_key = [this](std::uint32_t ea) { return fnKeyOf(ea); };
    if (!window_prof_.takeReport(fn_key, &rep)) return;
    // 窗口内 Present 次数（GPU 拆解的分母）
    for (const WindowShimRow& row : rep.shims) {
        const ShimDef* d = shims_.byId(row.id);
        if (d == nullptr) continue;
        if (d->module == "com" && d->name == "IDirect3DDevice9::Present") {
            rep.gpu_frames = row.calls;
            break;
        }
    }
    printWindowReport(rep);
}

std::uint32_t Machine::fnKeyOf(std::uint32_t ea) const {
    if (lst_.loaded()) {
        if (const LstFunction* f = lst_.functionAt(ea)) return f->ea;
    }
    return ea;
}

// ★ 窗口表的打印上界（`REQ-01M6XVZ8AXJNG492WA2QAMTA35`）：巨帧上一个窗口能有上万条块行、
//   上万条边对 ⇒ **只打窗口内 top-N + 汇总**（否则一条窗口跑测就能把日志撑到几十万行）。
//   N 在**打印期**由环境变量读（⛔ 不进 option 解析面、⛔ 不碰执行路径）：缺省够看清一个段。
static std::size_t windowPrintTop(const char* env_name, std::size_t fallback, std::size_t cap) {
    const char* text = std::getenv(env_name);
    if (text == nullptr || *text == '\0') return fallback;
    char* end = nullptr;
    const unsigned long v = std::strtoul(text, &end, 10);
    if (end == text) return fallback;
    return v > cap ? cap : static_cast<std::size_t>(v);
}

// 边表里**每个父**最多列几条子边（一个父可能有上千个子边；截断要在页内看得见）。
static constexpr std::size_t kEdgeChildrenShown = 8;

void Machine::printWindowReport(const WindowReport& r) const {
    const double wall_ms = static_cast<double>(r.wall_ns) / 1e6;
    auto pct = [&](std::uint64_t ns) {
        if (r.wall_ns == 0) return 0.0;
        return static_cast<double>(ns) * 100.0 / static_cast<double>(r.wall_ns);
    };
    auto ms = [](std::uint64_t ns) { return static_cast<double>(ns) / 1e6; };
    const std::uint64_t sum =
        r.guest_ns + r.shim_ns + r.native_ns + r.interp_ns + r.host_total_ns;

    std::fprintf(stdout, "\n=== profile window: %s ===\n", r.label.c_str());
    std::fprintf(stdout,
                 "window    : wall=%.1fms frame %llu→%llu insns %llu→%llu guest_ms %llu→%llu iterations=%llu\n",
                 wall_ms, (unsigned long long)r.begin_frame, (unsigned long long)r.end_frame,
                 (unsigned long long)r.begin_insns, (unsigned long long)r.end_insns,
                 (unsigned long long)r.begin_guest_ms, (unsigned long long)r.end_guest_ms,
                 (unsigned long long)r.iterations);
    // ★ 闭式：这一行就是判据（sum + unattributed == wall）。
    std::fprintf(stdout,
                 "buckets   : wall=%.1fms guest=%.1fms shim=%.1fms native=%.1fms interp=%.1fms host=%.1fms "
                 "sum=%.1fms unattributed=%.1fms\n",
                 wall_ms, ms(r.guest_ns), ms(r.shim_ns), ms(r.native_ns), ms(r.interp_ns), ms(r.host_total_ns),
                 ms(sum), static_cast<double>(r.unattributed_ns) / 1e6);
    std::fprintf(stdout,
                 "            （unattributed = wall − (guest+shim+native+interp+host)；正 = 有没归因的时间，"
                 "负 = 有重叠/重计 ⇒ 都不许吞掉）\n");
    std::fprintf(stdout, "  host    : audio=%.1fms(%.1f%%) event-pump=%.1fms(%.1f%%) input-script=%.1fms(%.1f%%) "
                         "timer=%.1fms(%.1f%%)\n",
                 ms(r.host_ns[WindowProfiler::HostAudio]), pct(r.host_ns[WindowProfiler::HostAudio]),
                 ms(r.host_ns[WindowProfiler::HostEventPump]), pct(r.host_ns[WindowProfiler::HostEventPump]),
                 ms(r.host_ns[WindowProfiler::HostInput]), pct(r.host_ns[WindowProfiler::HostInput]),
                 ms(r.host_ns[WindowProfiler::HostTimer]), pct(r.host_ns[WindowProfiler::HostTimer]));
    if (!r.fns.empty()) {
        std::fprintf(stdout,
                     "guest fns :（self 互斥 ⇒ 求和 = guest；inclusive 是调用树 ⇒ ⛔ 不可相加；前 %zu）\n",
                     r.fns.size() < 12 ? r.fns.size() : static_cast<std::size_t>(12));
        for (std::size_t i = 0; i < r.fns.size() && i < 12; ++i) {
            const WindowFnRow& f = r.fns[i];
            const LstFunction* fn = lst_.loaded() ? lst_.functionAt(f.key) : nullptr;
            if (fn != nullptr && fn->ea == f.key) {
                std::fprintf(stdout, "  %2zu. %-24s self=%8.3fms(%5.1f%%) incl=%8.3fms(%5.1f%%) calls=%llu\n", i + 1,
                             fn->name.c_str(), ms(f.self_ns), pct(f.self_ns), ms(f.inclusive_ns),
                             pct(f.inclusive_ns), (unsigned long long)f.calls);
            } else {
                std::fprintf(stdout, "  %2zu. sub_%08X             self=%8.3fms(%5.1f%%) incl=%8.3fms(%5.1f%%) "
                                     "calls=%llu\n",
                             i + 1, f.key, ms(f.self_ns), pct(f.self_ns), ms(f.inclusive_ns), pct(f.inclusive_ns),
                             (unsigned long long)f.calls);
            }
        }
        std::fprintf(stdout,
                     "            （inclusive 影子栈：unbalanced=%llu 次 ret 对不上 / 收口时仍开着 %llu 帧 ⇒ "
                     "非 0 就说明那几段 inclusive 不可全信，self 仍准）\n",
                     (unsigned long long)r.unbalanced, (unsigned long long)r.open_at_end);
    }
    // ★ 块级 EA 表 + 父子边表（`REQ-01M6XVZ8AXJNG492WA2QAMTA35`）：上面那张 `guest fns` 表按
    //   **函数入口**聚合，看不出"这段 inclusive 里是哪一块在跑"。⛔ 不动上面那张表的格式
    //   （既有断言按行匹配），只在这里**追加**两段；两段都有自己的汇总行（top-N 截断不许静默）。
    //   ★ 段名用 `blockEA` 而不是 `blocks`：`--profile` 的运行摘要里已有一行 `blocks    : …`，
    //     同名会让"按前缀 grep 段"同时抓到两处（实测踩过）。
    {
        auto fnName = [this](std::uint32_t key) -> std::string {
            const LstFunction* fn = lst_.loaded() ? lst_.functionAt(key) : nullptr;
            if (fn != nullptr && fn->ea == key) return fn->name;
            char buf[32];
            std::snprintf(buf, sizeof(buf), "sub_%08X", key);
            return std::string(buf);
        };
        const std::size_t blk_top = windowPrintTop("LIFT_WINDOW_BLOCK_TOP", 24, 512);
        if (!r.blocks.empty()) {
            std::fprintf(stdout,
                         "blockEA   :（块级 EA；self 互斥 ⇒ 求和 = guest 的块耗时；insns = 该块窗口内**执行过的指令数**"
                         "（不是时间，口径 = cpu_.insns 在块内的差值）；incl/calls/depth 取自"
                         "**包含该块的函数**（`[]` 里是函数名；影子栈只到函数粒度 ⇒ 块没有自己的 incl）；"
                         "列 self 前 %zu / 共 %llu 条；`!BLIND` = 窗口内**没有进入计数**"
                         "（尾调用 jmp 进入 / 窗口起点已在它内部）⇒ 不是「0 次」，是「不可见」）\n",
                         blk_top, (unsigned long long)r.block_rows);
            std::size_t shown = 0;
            double shown_self_ms = 0.0;
            for (const WindowBlockRow& b : r.blocks) {
                if (shown >= blk_top) break;
                ++shown;
                shown_self_ms += ms(b.self_ns);
                const char* blind = (b.calls == 0) ? " !BLIND" : "";
                const std::string name = "[" + fnName(b.fn_key) + "]";
                if (b.depth == 0) {
                    std::fprintf(stdout, "  %2zu. %08X %-26s self=%9.3fms incl=%9.3fms calls=%llu depth=- insns=%llu%s\n",
                                 shown, b.ea, name.c_str(), ms(b.self_ns), ms(b.incl_ns),
                                 (unsigned long long)b.calls, (unsigned long long)b.insns, blind);
                } else {
                    std::fprintf(stdout,
                                 "  %2zu. %08X %-26s self=%9.3fms incl=%9.3fms calls=%llu depth=%u insns=%llu%s\n",
                                 shown, b.ea, name.c_str(), ms(b.self_ns), ms(b.incl_ns),
                                 (unsigned long long)b.calls, static_cast<unsigned>(b.depth),
                                 (unsigned long long)b.insns, blind);
                }
            }
            const double total_self_ms = ms(r.block_self_ns);
            const double guest_ms = ms(r.guest_ns);
            std::fprintf(stdout,
                         "            （块级汇总：%llu 条块行 / self 合计 = %.3fms（guest 桶 = %.3fms，差 = %.3fms）；"
                         "上表只到前 %zu 条 ⇒ 覆盖 self 的 %.1f%%）\n",
                         (unsigned long long)r.block_rows, total_self_ms, guest_ms, total_self_ms - guest_ms, shown,
                         total_self_ms == 0.0 ? 0.0 : shown_self_ms * 100.0 / total_self_ms);
        }
        // ★ **块级 insns 的闭合行**（本单的硬判据）：块级之和 vs 窗口 insns，差多少就报多少。
        //   `window` 是窗口两端的 `cpu_.insns` 差；`override` 是**显式计数的另一类来源**
        //   （函数级 / 段级 native override 直接补进 cpu_.insns 的条数，不在任何块里）；
        //   `unattributed` = diff − override ⇒ ⛔ 非 0 必须能指出是哪一类，不许"约等于"。
        std::fprintf(stdout,
                     "insnsBlk  : block=%llu window=%llu diff=%lld override=%llu unattributed=%lld\n"
                     "            （块级 insns 合计 = 上表每行 `insns` 之和（含未列出的行）；window = 窗口 "
                     "`insns` 的 end−begin；diff = window − block；override = 函数级/段级 native override "
                     "补进 cpu_.insns 的条数；unattributed = diff − override）\n",
                     (unsigned long long)r.block_insns, (unsigned long long)r.window_insns,
                     (long long)r.insns_diff, (unsigned long long)r.override_insns,
                     (long long)r.insns_unattributed);
        const std::size_t edge_top = windowPrintTop("LIFT_WINDOW_EDGE_TOP", 12, 256);
        // ★ 一条边都没有时也**必须**打汇总行（否则"边表空"与"这一段没打开"在日志里长得一样）。
        if (!r.edges.empty() || r.edge_no_parent > 0 || r.edge_overflow > 0) {
            std::fprintf(stdout,
                         "edges     :（父子边：parent → child（函数级）；incl = 该边下 child 帧的墙钟合计"
                         "（递归边按帧累计）；按**父的 inclusive** 降序取前 %zu 个父，每个父最多列 %zu 条子边；"
                         "共 %llu 条边对 / %llu 个父）\n",
                         edge_top, kEdgeChildrenShown, (unsigned long long)r.edge_rows,
                         (unsigned long long)r.edge_parents);
            std::vector<std::pair<std::uint32_t, std::uint32_t>> shown_parents;  // (parent, 子边总数)
            for (const WindowEdgeRow& e : r.edges) {
                if (shown_parents.empty() || shown_parents.back().first != e.parent) {
                    if (shown_parents.size() >= edge_top) break;
                    shown_parents.emplace_back(e.parent, 0);
                }
                ++shown_parents.back().second;
            }
            std::size_t row_no = 0;
            for (std::size_t p = 0; p < shown_parents.size(); ++p) {
                std::size_t n = 0;
                for (const WindowEdgeRow& e : r.edges) {
                    if (e.parent != shown_parents[p].first) continue;
                    if (n >= kEdgeChildrenShown) break;
                    ++n;
                    ++row_no;
                    std::fprintf(stdout, "  %2zu. %s -> %s  incl=%9.3fms calls=%llu\n", row_no,
                                 fnName(e.parent).c_str(), fnName(e.child).c_str(), ms(e.incl_ns),
                                 (unsigned long long)e.calls);
                }
                if (shown_parents[p].second > kEdgeChildrenShown) {
                    std::fprintf(stdout, "      （该父另有 %llu 条子边未列，父 = %s）\n",
                                 (unsigned long long)(shown_parents[p].second - kEdgeChildrenShown),
                                 fnName(shown_parents[p].first).c_str());
                }
            }
            std::fprintf(stdout,
                         "            （边表汇总：已列 %zu 个父 / 共 %llu 个父；"
                         "**没有父帧**的进入 %llu 次（顶层进入 / 窗口起点已在函数内部）⇒ 这些进入不在边表里；"
                         "超过上界未记 %llu 条边对）\n",
                         shown_parents.size(), (unsigned long long)r.edge_parents,
                         (unsigned long long)r.edge_no_parent, (unsigned long long)r.edge_overflow);
        }
    }
    if (!r.shims.empty()) {
        std::fprintf(stdout, "shims     :（⊂ shim 桶；前 %zu）\n",
                     r.shims.size() < 12 ? r.shims.size() : static_cast<std::size_t>(12));
        for (std::size_t i = 0; i < r.shims.size() && i < 12; ++i) {
            const WindowShimRow& s = r.shims[i];
            const ShimDef* d = shims_.byId(s.id);
            std::fprintf(stdout, "  %2zu. %-42s ns=%8.1fms(%5.1f%%) calls=%llu\n", i + 1,
                         d ? (d->module + "!" + d->name).c_str() : "?", ms(s.ns), pct(s.ns),
                         (unsigned long long)s.calls);
        }
    }
    std::fprintf(stdout,
                 "gpu       : frames=%llu readback=%.1fms hash=%.1fms stats=%.1fms sink=%.1fms"
                 "（Present 的内部拆解 ⇒ ⊂ shim 的 Present 行，⛔ 不重复计入 buckets）\n",
                 (unsigned long long)r.gpu_frames, ms(r.gpu_readback_ns), ms(r.gpu_hash_ns), ms(r.gpu_stats_ns),
                 ms(r.gpu_sink_ns));
    std::fflush(stdout);
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

// ★ SEH / C++ EH 派发（**块边界**；`RaiseException` / `RtlUnwind` 的宿主侧）。
//   shim 只搬参数 + 把 eip 交给 `kSehDispatchEa`（`take_control`），真正的"铺 handler 帧 →
//   跑 guest → 回来收 disposition"在这里 —— 与 `fireDueTimer` 同一个形状。
//   两条**响亮失败**在这里落地，⛔ 都不许静默：
//     ① 展开要跨过宿主 guest 回调帧（WndProc / 音频 fill / winmm 定时器）：真机上异常会穿过宿主
//        C++ 栈（未定义行为）⇒ 检出即停（`UnhandledException`，detail 点名是哪一种）。
//     ② 状态机自己报的停（`fs:[0]` 链走到底 / 结构铺不下 / 嵌套异常 / handler 链打转）。
bool Machine::pumpSeh(RunResult& r) {
    if (cpu_.eip == kSehDispatchEa) {
        seh_.setDispatchPending(false);
        // ★ 记录"宿主 guest 回调帧的返回槽"（0 = 不在任何回调里）。判据见 machine.h 的
        //   `seh_guest_boundary_`：展开**在回调里面** catch 是允许的（room-exit 就是），
        //   穿出回调帧则必须响亮失败。
        seh_guest_boundary_ = 0;
        const std::uint32_t stack_top = setup_.stack_top != 0 ? setup_.stack_top : layout_.stack_top;
        if (guest_calls_.active()) {
            // 从当前 esp 往上找最内层回调帧：`start()` 在那一格写的就是 `kContinuationEa`。
            for (std::uint32_t ea = cpu_.esp(); ea + 4u <= stack_top; ea += 4u) {
                if (mem_.rd32(ea) == GuestCallStack::kContinuationEa) {
                    seh_guest_boundary_ = ea;
                    break;
                }
            }
            if (seh_guest_boundary_ == 0) {
                r.reason = StopReason::UnhandledException;
                r.detail = std::string("guest 异常发生在 GuestCallStack 回调里，但栈上找不到回调帧的返回槽（") +
                           (guest_calls_.currentApi() ? guest_calls_.currentApi() : "?") +
                           "）⇒ 无法证明展开不跨宿主帧 ⇒ 本线不支持";
                seh_.reset();
                cpu_.running = false;
                return false;
            }
        } else if (cpu_.shim.timer_firing) {
            // `fireDueTimer` 的帧：`[timer_saved_esp-24] = kTimerCallbackReturnEa`，共 5 参 + 返回地址。
            seh_guest_boundary_ = cpu_.shim.timer_saved_esp - 24u;
        }
    }
    std::string why;
    const SehStep s = seh_.step(cpu_, mem_, &why);
    switch (s.kind) {
        case SehStep::Kind::None:
            return false;
        case SehStep::Kind::CallHandler:
            // 合成 handler 调用也算一帧（它的 `ret N` 会正常弹 ⇒ 不会留下"未返回帧"）。
            // 帧底 = 铺帧时的 esp（= 被调方入口的栈顶）；展开落地时靠它判断"谁被跳过"。
            window_prof_.pushCall(fnKeyOf(s.handler_ea), kSehHandlerReturnEa, s.frame_esp);
            cpu_.noteInsn(s.handler_ea, "[seh]");
            return true;
        case SehStep::Kind::Transfer:
            // ★ 跨宿主 guest 回调帧 ⇒ 响亮停（真机上异常会穿过宿主 C++ 栈 = UB）。
            if (seh_guest_boundary_ != 0 && s.esp >= seh_guest_boundary_) {
                char buf[256];
                std::snprintf(buf, sizeof(buf),
                              "guest 异常要展开跨过宿主 guest 回调帧（落地 esp=0x%08X >= 回调帧返回槽 "
                              "0x%08X；%s）⇒ 本线不支持（⛔ 不静默清栈继续跑）",
                              s.esp, seh_guest_boundary_,
                              guest_calls_.active() ? "GuestCallStack" : "winmm 定时器回调");
                r.reason = StopReason::UnhandledException;
                r.detail = buf;
                seh_.reset();
                seh_guest_boundary_ = 0;
                cpu_.running = false;
                return false;
            }
            // ★ 展开是"不经 `ret` 的跳转" ⇒ 影子栈按落地 esp 丢掉被跳过的帧
            //   （⛔ 不许让 inclusive 静默偏大；`dropFramesBelowEsp` 自己的判据在里面）。
            (void)window_prof_.dropFramesBelowEsp(s.esp);
            cpu_.noteInsn(s.eip, "[seh]");
            return true;
        case SehStep::Kind::Stop:
            r.reason = StopReason::UnhandledException;
            r.detail = why.empty() ? std::string("SEH 派发失败（无 detail）") : why;
            seh_guest_boundary_ = 0;
            cpu_.running = false;
            return false;
    }
    return false;
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
    // ★ 段级项也是输入：同一个名字意味着"中段固定 EA 被替换"，与函数级不是一回事（`blk:` 前缀）。
    for (std::size_t i = 0; i < block_overrides_.size(); ++i) {
        const NativeBlockOverride* e = block_overrides_.at(i);
        char buf[160];
        std::snprintf(buf, sizeof(buf), "blk:%s@%08X..%08X", e->name.c_str(), e->entry_ea, e->resume_ea);
        if (!t.native_overrides.empty()) t.native_overrides += ';';
        t.native_overrides += buf;
    }
    t.clock_start_100ns = clock_.epoch100ns();
    t.real_time = clock_.realTime();
    t.skip_logo = skip_logo_;
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
    if (t.skip_logo) std::fprintf(out, "trace-input skip-logo= 1（写引擎 show-logo 标志；不改 root script）\n");
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
    // ★ 只有真的开了 `--skip-logo` 才混入：默认路径的 trace hash 与旧基线保持不变
    //   （开关一变就是另一条执行路径，必须能看出差异）。
    if (t.skip_logo) {
        const std::uint8_t skip_logo = 1;
        mix(&skip_logo, 1);
    }
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
    // ★ 仪器口径标签（M-02）：一行、字段顺序固定、机器可读。读的是**本轮真实生效**的位
    //   （`window-prof` = `WindowProfiler::armed()`），⛔ 不是 CLI 参数回显 —— 所以
    //   "脚本里写了 profile_begin 但没 arm" 这类缺陷会在这里被如实打出来。
    {
        char expect_field[32];
        if (instrument_.expect_change) {
            std::snprintf(expect_field, sizeof(expect_field), "%llu",
                          (unsigned long long)instrument_.expect_windows);
        } else {
            std::snprintf(expect_field, sizeof(expect_field), "off");
        }
        std::fprintf(out, "instr     : profile=%s frame-trace=%s window-prof=%s expect=%s\n",
                     instrument_.profile ? "on" : "off", instrument_.frame_trace ? "on" : "off",
                     instrument_.window_prof ? "on" : "off", expect_field);
    }
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
    if (skip_logo_) {
        if (skip_logo_applied_) {
            std::fprintf(out, "skip-logo : applied @ T_Command=0x%08X (+0x%05X=0)\n", skip_logo_object_,
                         kShowLogoFieldOffset);
        } else if (skip_logo_checked_) {
            std::fprintf(out, "skip-logo : NOT applied（钩子命中但对象/字段不可写）\n");
        } else {
            std::fprintf(out, "skip-logo : 未命中（run 在 T_Command 构造前结束）\n");
        }
    }
    std::fprintf(out, "blocks    : %zu (insns cached %zu, hits %zu, misses %zu)\n", cache_.blockCount(),
                 cache_.insnCount(), cache_.hits(), cache_.misses());
    // ★ 单令牌不变式的**可观测**面（guest_token.h）：`violations` 非 0 ⇒ 有线程没拿令牌就
    //   执行 guest 代码 / 改 GuestHeap / HandleTable。`--assert-guest-token` 把它变成退出码。
    {
        const GuestToken& token = guestToken();
        std::fprintf(out, "token     : violations=%llu max-concurrent=%u%s%s\n",
                     static_cast<unsigned long long>(token.violations()), token.maxConcurrent(),
                     token.violations() == 0 ? "" : " first=",
                     token.violations() == 0 ? "" : (token.firstViolationSite() ? token.firstViolationSite() : "?"));
    }
    // ★ 诊断开关的**口径标签**（`guest_token.h` 文件头）：始终打一行，缺省 `skip-lock=0`。
    //   为什么无条件打：A/B 两臂的日志必须能**一眼分开**，而"没打这行"与"关着"在文本上一样。
    std::fprintf(out, "token-diag : skip-lock=%d\n", guestToken().diagSkipLock() ? 1 : 0);
    // ★ M1（REQ-01M6XVZG4T9WY8SQ1WS7P8RNW9）：**等令牌**的独立一栏（⛔ 不是 `ns` 的一部分）。
    //   口径：`waits` = 真的被挡住的取令牌次数；`wait` = 这些等待的累计墙钟（steady_clock）。
    //   重入 / 未竞争不计（未竞争路径不读时钟）⇒ 全 0 意味着"没有真的排队"，不是"没测"。
    //   ⛔ 不要拿具体数值当判据（它随机器与调度变）；判据是"它 > 0 且 `ns` 里不再含它"。
    {
        const GuestToken& token = guestToken();
        const std::uint64_t waits = token.waits();
        const double wait_ms = static_cast<double>(token.waitNs()) / 1e6;
        const double avg_us = waits ? static_cast<double>(token.waitNs()) / static_cast<double>(waits) / 1e3 : 0.0;
        std::fprintf(out, "tokenwait : waits %llu wait %.1f ms avg %.1f us\n", (unsigned long long)waits, wait_ms,
                     avg_us);
    }
    std::fprintf(out, "committed : %llu KiB\n", (unsigned long long)(mem_.committedBytes() / 1024));
    std::fprintf(out, "shims     : imported %zu / called-unimplemented %zu\n", shims_.importedCount(),
                 shims_.calledUnimplemented().size());
    std::fprintf(out, "heap      : in_use %u peak %u allocs %u\n", heap_.bytesInUse(), heap_.peak(),
                 heap_.allocCount());
    // ★ 分配器 / 内存账的**可观测计数**（不是墙钟）：
    //   `first-fit-scans` = 首次适配真正比较过的空闲块数；`coalesce-merges` = 收口时并掉的相邻空闲块；
    //   `zero-bytes` = HEAP_ZERO_MEMORY 清零写过的字节（⛔ 语义面，不是优化目标）；
    //   `commit` = `GuestMemory::commit` 的**API 次数/字节** 与**真正落到系统调用**的次数/字节。
    //   ⇒ "每次 alloc 都在提交页"这类猜测可以直接用这几个数证伪/证实。
    std::fprintf(out,
                 "heapstat  : first-fit-scans %llu coalesce-merges %llu zero-bytes %llu blocks %zu\n",
                 (unsigned long long)heap_.firstFitScans(), (unsigned long long)heap_.coalesceMerges(),
                 (unsigned long long)heap_.zeroBytes(), heap_.blockCount());
    std::fprintf(out, "commit    : api %llu/%lluB system %llu/%lluB\n",
                 (unsigned long long)mem_.commitApiCalls(), (unsigned long long)mem_.commitApiBytes(),
                 (unsigned long long)mem_.platformCommitCalls(),
                 (unsigned long long)mem_.platformCommitBytes());
    // ★ 输入时间线的可观测计数：`ticks` = 块边界 tick 次数；`action-scans` = 逐条比较动作的次数。
    //   后者只随"动作集变化"增长，**不随块数增长**（守卫 `profile-window` 钉着）。
    if (input_script_.active()) {
        std::fprintf(out, "inputstat : ticks %llu action-scans %llu\n",
                     (unsigned long long)input_script_.tickCount(),
                     (unsigned long long)input_script_.actionScans());
        // ★ "按下→抬起"账（README §4.2c 的自动 hold）：`auto` = 等过 guest 采样的抬起；
        //   `deferred-ups` = 显式 mouse_up/key_up 因为"还没被采样"被推迟的次数；
        //   `bounded-no-sample` = 到上限仍没采样 ⇒ 强制抬起（同时有 stderr 告警）；
        //   `waited-samples-max` / `hold-ms-max` = 单次最多等了几次采样 / 最长跨度（guest ms）。
        //   ⛔ 这些不是墙钟：判据是"guest 有没有观察过按下"，不是"等了多久"。
        const InputScript::ReleaseStats& rs = input_script_.releaseStats();
        std::fprintf(out,
                     "            release : total %llu auto %llu explicit %llu deferred-ups %llu "
                     "bounded-no-sample %llu waited-samples-max %llu hold-ms-max %llu\n",
                     (unsigned long long)rs.releases, (unsigned long long)rs.auto_releases,
                     (unsigned long long)rs.explicit_releases, (unsigned long long)rs.deferred_ups,
                     (unsigned long long)rs.bounded_no_sample, (unsigned long long)rs.waited_samples_max,
                     (unsigned long long)rs.hold_ms_max);
        // ★ marker 事件源的可观测账（只有脚本里写了 marker 动作才打）：
        //   `samples` = 有效采样（含播种那一次）· `edges` = 事件数 · `invalid` = **已发布但这一格不能读**
        //   的次数（`MarkerSampler::invalid()`，不重复计 unpublished）· `published` / `unpublished` =
        //   Engine 单例非 0 / 为 0 的采样次数（后者 = boot 前 / 退出后，正常）· `last` = 最近一次身份。
        //   ⇒ `samples=0` 是"采样点没命中 / 钩子接错"与"名字写错"的唯一区分判据。
        if (input_script_.hasMarkerActions()) {
            std::fprintf(out,
                         "            marker  : samples %llu edges %llu invalid %llu published %llu unpublished %llu "
                         "last (cur=%u id=0x%08X name=%s%s)\n",
                         (unsigned long long)input_script_.markerSamples(),
                         (unsigned long long)input_script_.markerEdges(),
                         (unsigned long long)marker_sampler_.invalid(),
                         (unsigned long long)marker_sampler_.published(),
                         (unsigned long long)marker_sampler_.unpublished(), input_script_.lastMarkerCur(),
                         input_script_.lastMarkerId(), input_script_.lastMarkerName().c_str(),
                         input_script_.lastMarkerNameDegraded() ? "（降级）" : "");
        }
    }
    // ★ 时间线 **0 点**（`--anchor-script <id>`）的可观测面：闩锁了就**一定**打一行（四单位齐全）。
    //   为什么不能只靠闩锁那一刻的 `[input-script] origin` 行：那行在 stderr/stdout 的滚动日志里，
    //   而"纯 anchor 运行"**没有** `marker :` 行（它只按 `hasMarkerActions()` 打）⇒ 跑完的输出里
    //   "0 点定在哪"不可读。这一行是摘要的一部分，与 `marker :` 的 last 同处可比。
    //   ★ 未闩锁 ⇒ **不打**（与 `marker :` 的"没有数就不打"同口径；失败走 `origin-missed:` detail）。
    //   ★ 格式与需求单一致：`origin    : script=<id> frame=… insns=… guest_ms=… wall_ms=…`。
    if (input_script_.originRequested() && input_script_.originSet()) {
        const TimelineOrigin& o = input_script_.origin();
        std::fprintf(out, "origin    : script=0x%08X frame=%llu insns=%llu guest_ms=%llu wall_ms=%llu\n",
                     o.id, static_cast<unsigned long long>(o.frame),
                     static_cast<unsigned long long>(o.insns), static_cast<unsigned long long>(o.guest_ms),
                     static_cast<unsigned long long>(o.wall_ms));
    }
    // ★ 交互图（`--interaction-graph`）：状态 / 迁移次数 / 发出的动作数 / 采样量 / 失败类别。
    //   失败类别单独一行（`FAIL`），因为它必须与"跑完了"（done）在读输出时一眼分开。
    if (interaction_.active()) {
        std::fprintf(out, "interaction: state=%s node=%s transitions=%llu actions=%llu samples=%llu changes=%llu\n",
                     interaction_.stateName(),
                     interaction_.current() != nullptr ? interaction_.current()->id.c_str() : "-",
                     (unsigned long long)interaction_.stateChanges(),
                     (unsigned long long)interaction_.actionsIssued(),
                     (unsigned long long)sampler_.sampleCount(), (unsigned long long)sampler_.changeCount());
        if (interaction_.failed()) {
            std::fprintf(out, "FAIL      : %s（%s）\n", graphFailureName(interaction_.failure()),
                         interaction_.failureDetail().c_str());
        }
    }
    // ★ arena 是**独立**的 GuestHeap（`init` 的 0x28000000..0x30000000，128 MiB），不并进上面那行：
    //   纹理像素 / 后缓冲 / 深度块只在这里，主堆的 in_use 对"纹理没回收"完全不可见。
    std::fprintf(out, "arena     : surfaces %zu in_use %u peak %u largest-free %u\n", surfaces_.surfaceCount(),
                 surfaces_.bytesInUse(), surfaces_.peak(), surfaces_.largestFreeBlock());
    // ★ 不按 `enabled()` 门：`--audio off` 也打一行，好让"没设备"与"静音"在同一处可读
    //   （`device=none muted=1 muted_bytes=0` ⇒ 确实没有交付，而不是被静音）。
    audio_.dumpSummary(out);
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
    // ★ 默认级别告警（`pipeWarn`）的**拒绝计数**：按 (调用点, 类别) 分桶后按类合计
    //   （`REQ-01M70SHFT6RZA7RRY1R051Z2PB` #2）。与限流桶**同一份**（进程级）⇒ 第 5 次之后
    //   只留 debug 的事件也在这里数得到；`other` = 顶点声明 / 设备销毁时的绑定释放等。
    {
        const d3d9::PipeWarnCounts& wc = d3d9::pipeWarnCounts();
        if (wc.total() != 0) {
            std::fprintf(out, "pipe-warn : vertex-stream=%llu index=%llu texture=%llu other=%llu\n",
                         (unsigned long long)wc.of(d3d9::PipeWarnCategory::VertexStream),
                         (unsigned long long)wc.of(d3d9::PipeWarnCategory::Index),
                         (unsigned long long)wc.of(d3d9::PipeWarnCategory::Texture),
                         (unsigned long long)wc.of(d3d9::PipeWarnCategory::Other));
        }
    }
    if (!overrides_.empty() || !block_overrides_.empty()) {
        std::uint64_t hits = 0;
        std::uint64_t fallbacks = 0;
        for (std::size_t i = 0; i < overrides_.size(); ++i) {
            const NativeOverride* e = overrides_.at(i);
            hits += e->hits;
            fallbacks += e->fallbacks;
        }
        for (std::size_t i = 0; i < block_overrides_.size(); ++i) {
            const NativeBlockOverride* e = block_overrides_.at(i);
            hits += e->hits;
            fallbacks += e->fallbacks;
        }
        std::fprintf(out, "native-ovr: entries=%zu blocks=%zu hits=%llu fallbacks=%llu\n", overrides_.size(),
                     block_overrides_.size(), (unsigned long long)hits, (unsigned long long)fallbacks);
        for (std::size_t i = 0; i < overrides_.size(); ++i) {
            const NativeOverride* e = overrides_.at(i);
            std::fprintf(out, "  %-12s @ %08X %s(%uB) hits=%llu fallbacks=%llu\n", e->name.c_str(), e->ea,
                         guestConvName(e->conv), e->arg_bytes, (unsigned long long)e->hits,
                         (unsigned long long)e->fallbacks);
            // ★ 条件式 override 的台账（`fast_hits` / `declines[]`，见 native_overrides.h）：
            //   桶下标 → 原因名由该 override 自己的域解释（例如 `sub_4BBAB0` 的 `VoicePollWhy`）。
            //   只在有数时多打一行：无条件项的这两项恒 0，不给日志加噪声。
            bool any = e->fast_hits != 0;
            for (std::size_t k = 0; k < NativeOverride::kDeclineSlots; ++k) any = any || e->declines[k] != 0;
            if (any) {
                std::fprintf(out, "    fast_hits=%llu declines=[", (unsigned long long)e->fast_hits);
                bool first = true;
                for (std::size_t k = 0; k < NativeOverride::kDeclineSlots; ++k) {
                    if (e->declines[k] == 0) continue;
                    std::fprintf(out, "%s#%zu=%llu", first ? "" : " ", k, (unsigned long long)e->declines[k]);
                    first = false;
                }
                std::fprintf(out, "]\n");
            }
        }
        for (std::size_t i = 0; i < block_overrides_.size(); ++i) {
            const NativeBlockOverride* e = block_overrides_.at(i);
            std::fprintf(out, "  %-12s @ %08X..%08X 段级 hits=%llu declines=%llu fallbacks=%llu%s\n",
                         e->name.c_str(), e->entry_ea, e->resume_ea, (unsigned long long)e->hits,
                         (unsigned long long)e->declines, (unsigned long long)e->fallbacks,
                         e->disabled ? "（已停用）" : "");
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
    // ★ 宿主 shim 表（`--profile` 现在也会打开 shim 计时，见 run() 的 setTiming）。
    //   口径：只统计 **shim 实现自身**（`ShimRegistry::invoke` 只包 `local.fn`；★ 计时起点在
    //   `GuestToken::Guard` **之后**，见 shims.cpp 与 REQ-01M6XVZG4T9WY8SQ1WS7P8RNW9），
    //   不含 dispatcher 弹参、native override、事件泵、**也不含等令牌**（后者单开一栏，见下）；
    //   与 guest 块表**不可相加**（两段墙钟不重叠，但也不覆盖同一段时间）。只列 `ns > 0` 的项：
    //   没被调用过的 shim 恒 0，没有解释价值。
    {
        struct ShimRow {
            const ShimDef* def = nullptr;
            std::uint64_t ns = 0;
            std::uint64_t calls = 0;
        };
        std::vector<ShimRow> shim_rows;
        shim_rows.reserve(shims_.all().size());
        std::uint64_t shim_total_ns = 0;
        for (const ShimDef& d : shims_.all()) {
            if (d.ns == 0) continue;
            shim_rows.push_back(ShimRow{&d, d.ns, d.call_count});
            shim_total_ns += d.ns;
        }
        // 排序：ns desc → calls desc → module asc → name asc（同分也要确定性，⛔ 不按注册顺序）。
        std::sort(shim_rows.begin(), shim_rows.end(), [](const ShimRow& a, const ShimRow& b) {
            if (a.ns != b.ns) return a.ns > b.ns;
            if (a.calls != b.calls) return a.calls > b.calls;
            if (a.def->module != b.def->module) return a.def->module < b.def->module;
            return a.def->name < b.def->name;
        });
        std::fprintf(out, "\n=== profile: shims（宿主实现自身墙钟；前 %zu）===\n", top);
        std::fprintf(out, "total shim wall = %.1f ms（guest-block wall = %.1f ms；两表不可相加）\n",
                     static_cast<double>(shim_total_ns) / 1e6, static_cast<double>(total_ns) / 1e6);
        for (std::size_t i = 0; i < shim_rows.size() && i < top; ++i) {
            const ShimRow& r = shim_rows[i];
            const double pct =
                shim_total_ns ? 100.0 * static_cast<double>(r.ns) / static_cast<double>(shim_total_ns) : 0.0;
            const double avg_us = r.calls ? static_cast<double>(r.ns) / static_cast<double>(r.calls) / 1e3 : 0.0;
            // ★ 先拼成 std::string 再用 %-46s 定宽（`%s!%s` 两段拼宽度要分别算，必错）。
            const std::string label = r.def->module + "!" + r.def->name;
            std::fprintf(out, "  %2zu. %-46s %9.1f ms %6.2f%% calls=%-10llu avg=%9.1f us\n", i + 1,
                         label.c_str(), static_cast<double>(r.ns) / 1e6, pct,
                         (unsigned long long)r.calls, avg_us);
        }
    }
    // ★ M1（REQ-01M6XVZG4T9WY8SQ1WS7P8RNW9）：**等令牌**的独立一栏。
    //   为什么必须单开：上面的 shim 表只含「实现自身」（起点在 Guard 之后）⇒ 拿它排序"谁在持令牌
    //   做阻塞型宿主等待"会把等待时间读成"这个 shim 很慢"。这里的账回答的是另一个问题：
    //   **为了进临界区，我等了多久**。累计 = `GuestToken`（每轮 `arm()` 清零），恒记；
    //   代价只在真的被挡时两次 steady_clock ⇒ 不开 `--profile` 也照样有数（见运行摘要）。
    {
        const GuestToken& token = guestToken();
        const std::uint64_t waits = token.waits();
        const std::uint64_t wait_ns = token.waitNs();
        const double avg_us = waits ? static_cast<double>(wait_ns) / static_cast<double>(waits) / 1e3 : 0.0;
        std::fprintf(out, "\n=== profile: guest token wait（等令牌；⛔ 不在上面的 shim 墙钟里）===\n");
        std::fprintf(out, "  waits=%llu wait=%.1f ms avg=%.1f us（这段墙钟落在临界区**之外**；与 shim 表不相加）\n",
                     (unsigned long long)waits, static_cast<double>(wait_ns) / 1e6, avg_us);
    }
    // ★ 字体度量缓存（`FontEngine::metrics`）的**命中率**：`gdi32!GetTextMetricsA` 的墙钟只是现象，
    //   这一行才是"缓存到底有没有用"的判据。**不是墙钟、不与上面两张表相加**，所以单起一格标题
    //   （格式与 shim 行相近，混在表里会被当成表内一行）。计数恒记，但本函数只在 `--profile` 时被调
    //   （`main.cpp` 的 `if (profile)`）⇒ 不打 `--profile` 看不到它。
    std::fprintf(out, "\n=== profile: font metrics cache（命中率；不是墙钟）===\n");
    {
        const std::uint64_t hits = fonts_.metricsHits();
        const std::uint64_t misses = fonts_.metricsMisses();
        const std::uint64_t total = hits + misses;
        const double rate = total ? 100.0 * static_cast<double>(hits) / static_cast<double>(total) : 0.0;
        std::fprintf(out, "  font!TEXTMETRIC-cache hits=%llu misses=%llu hit=%5.1f%%\n",
                     (unsigned long long)hits, (unsigned long long)misses, rate);
    }

    // ★ override 的函数体**不会**进块 profile（被宿主实现整段替代）⇒ 显式说明，否则 profile 会误导
    //   "这个热点已经不在"。
    if (!overrides_.empty() || !block_overrides_.empty()) {
        std::fprintf(out, "\n注意：以下函数/代码段被 native override 替代，其函数体不计入上面的块 profile：\n");
        for (std::size_t i = 0; i < overrides_.size(); ++i) {
            const NativeOverride* e = overrides_.at(i);
            std::fprintf(out, "  %-12s @ %08X hits=%llu\n", e->name.c_str(), e->ea,
                         (unsigned long long)e->hits);
        }
        for (std::size_t i = 0; i < block_overrides_.size(); ++i) {
            const NativeBlockOverride* e = block_overrides_.at(i);
            std::fprintf(out, "  %-12s @ %08X..%08X（段级）hits=%llu%s\n", e->name.c_str(), e->entry_ea,
                         e->resume_ea, (unsigned long long)e->hits, e->disabled ? "（已停用）" : "");
        }
    }
}

bool Machine::configureAudio(AudioMode mode, const std::string& dump_path, bool sound, std::string* err) {
    return audio_.configure(mode, dump_path, sound, err);
}

bool Machine::configureNativeOverrides(const std::string& list, std::string* err) {
    overrides_.clear();
    block_overrides_.clear();
    if (list.empty()) return true;
    if (!lst_.loaded()) {
        if (err) *err = "--native-overrides 需要 --lst（函数级按名字解析 EA、段级按 `.lst` 锚校验；⛔ 不硬编码 EA）";
        return false;
    }
    auto knownNames = []() {
        std::string s;
        for (std::size_t i = 0; i < knownNativeCount(); ++i) {
            if (!s.empty()) s += ", ";
            s += knownNativeAt(i).name;
            if (knownNativeAt(i).debug) s += "(debug)";
        }
        for (std::size_t i = 0; i < knownBlockNativeCount(); ++i) {
            if (!s.empty()) s += ", ";
            s += knownBlockNativeAt(i).name;
            s += "(段级)";
            if (knownBlockNativeAt(i).debug) s += "(debug)";
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
    // ⛔ `-` 后面必须跟一个已知名字；拼错就报错，不静默忽略。函数级 / 段级同名都在同一个命名空间里。
    auto knownAny = [](const std::string& n) {
        return findKnownNative(n) != nullptr || findKnownBlockNative(n) != nullptr;
    };
    std::vector<std::string> disabled;
    for (const std::string& token : tokens) {
        if (token.empty() || token.front() != '-') continue;
        const std::string name = trim(token.substr(1));
        if (name.empty()) {
            if (err) *err = "`-` 后面需要一个 native override 名字";
            return false;
        }
        if (!knownAny(name)) {
            if (err) *err = "未知的 native override 排除项：" + name + "（已知：" + knownNames() + "）";
            return false;
        }
        disabled.push_back(name);
    }
    auto isDisabled = [&](const std::string& name) {
        return std::find(disabled.begin(), disabled.end(), name) != disabled.end();
    };
    // 段级项：先做锚校验（entry 必须是 `.lst` 里的那条指令、且属于声明的函数），再入表。
    // ★ 校验失败**一定打一行**（即使 `all` 里只是跳过）—— 否则"优化没生效"在日志里看不出来。
    auto addBlock = [&](const KnownBlockNative& k, bool required) -> bool {
        if (isDisabled(k.name)) return true;
        std::string why;
        if (!verifyKnownBlockAnchor(lst_, k, &why)) {
            std::fprintf(stderr, "[native] %s\n", why.c_str());
            if (required) {
                if (err) *err = why;
                return false;
            }
            return true;
        }
        if (block_overrides_.find(k.entry_ea) != nullptr) return true;
        return block_overrides_.add(k.entry_ea, k.resume_ea, k.name, k.fn, err);
    };
    auto addName = [&](const std::string& name, bool required) -> bool {
        if (isDisabled(name)) return true;
        if (const KnownBlockNative* kb = findKnownBlockNative(name)) return addBlock(*kb, required);
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
            // ★ `all` = 只开**已验证**项；debug 项必须显式点名；排除项优先。函数级 + 段级都在 `all` 里。
            for (std::size_t i = 0; i < knownNativeCount(); ++i) {
                if (knownNativeAt(i).debug) continue;
                if (!addName(knownNativeAt(i).name, false)) return false;
            }
            for (std::size_t i = 0; i < knownBlockNativeCount(); ++i) {
                if (knownBlockNativeAt(i).debug) continue;
                if (!addName(knownBlockNativeAt(i).name, false)) return false;
            }
        } else if (!addName(token, true)) {
            return false;
        }
    }
    if (overrides_.empty() && block_overrides_.empty()) {
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
