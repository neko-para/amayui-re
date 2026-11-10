// emulator-lift · **可定界的区间 profiler**（`profile_begin` / `profile_end`）—— pure 守卫
//
// ★ 守的三件事（每条都钉在"坏了会有什么后果"上）：
//   ① **闭式**：`buckets` 行的 `wall = guest + shim + native + interp + host + unattributed`。
//      断言 `sum` 与 `wall` 的差 ≤ 容差，并且 `|unattributed| ≤ 容差`。
//      容差从哪来：① steady_clock 的两次读数（窗口边界 / 迭代边界，纳秒级）；② 窗口边界落在
//      block 边界 ⇒ 触发点与开关点差一个块（我们对 begin/end 的 insns 各断言了范围）；
//      ③ 宿主调度噪声（Windows 上 QPC 分辨率 ~100 ns，但一次抢占可达 1–2 ms）。
//      ⇒ 取 `max(2 ms, 8% × wall)`：系统性漏账（漏一整桶）会远超它，噪声不会。
//   ③ **窗口外不计入**：脚本在窗口外也调用同一个慢 shim（4,000,000 次），窗口内只调用一次
//      （400,000 次）。断言窗口内那一行 `calls=1`、且窗口 `shim` 桶 < 总 shim 墙钟的 50%。
//      ⇒ 若把窗口外的调用也算进来（例如 `addShim` 不判 `active()`），这两条都会红。
//   ③ **未闭合**：脚本只写 `profile_begin` 不写 `profile_end` ⇒ 运行结束时必须**自动收口 +
//      告警 + 照样打印**（⛔ 不许静默丢数据）。断言 stdout 有表 + stderr 有"未闭合"。
//   ④ **运行期追加**（`REQ-01M701PEB38VDQJD4QR2JEBA74`）：`InputScript::appendText` 在**运行中**
//      追加的 `profile_begin/end`（`--interaction-graph` 走的就是这条路）必须**重新 arm** ——
//      否则窗口照开照收、表却全是 0（iterations=0 / self=0.000ms / shim 墙钟 0.0ms），
//      而且不报错不告警。判据见 ⑥ 的用例 `profile-window-runtime-append-arm`。
//
// ★ **`profile-instrumentation-invariant`（口径不变量）**：四臂（A 全关 / B 仅 `--profile` /
//   C 仅窗口 profiler / D 两者）跑**同一份程序**，`RunResult.insns` 必须**全等**。
//   ⇒ 它守的是"仪器不许改执行流"（改了就说明读数是另一条路径上的读数）。这是**关系**断言，
//   ⛔ 不断言任何耗时 / 百分比：膨胀量级是读数，进 `measurement.md` M-02，不进 test。
//   ⚠ 四臂成立的前提（⛔ 改测试时不要破坏）：① 裸 Machine、`real_time=false`（裸机缺省；
//   `--present window` 会自动 real-time ⇒ 那一档不成立）；② 锚点只用 `at insn`（裸机没有 Present，
//   `at frame` 会走帧停滞兜底、`wall_ms` 会读宿主墙钟）；③ 无 sink / 音频 Off / 不读宿主时钟的 shim；
//   ④ **每臂一台新 Machine**（`run()` 不重置 `cpu_.insns`，复用会累加）；⑤ 只有窗口那两臂多两行
//   `profile_*`；⑥ 清掉 `LIFT_FRAME_TRACE`（否则"全关"臂其实开着逐帧 trace）。
//   变异（`pnpm test:mutation`）：让块计时顺带改 `cpu_.insns` ⇒ 本用例当场红。
//
// ★ 还钉了触发条件的两条口径：
//   - `at insn N` 能当窗口边界（**帧内也能切**）⇒ 同一脚本重复跑边界相同（断言 insns 范围）。
//   - 帧停滞兜底：帧号不推进时 `at frame` 不会永远不触发（`--input-stall-ms` 的语义）。
//
// ⛔ 不钉任何墙钟数字（ms 随机器变），只钉**关系**与**边界**。
// ⛔ 不依赖游戏 / LFS：自造机器码 + 一个宿主 shim，纯 pure。
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "check.h"
#include "runtime/machine.h"
#include "runtime/shims.h"

namespace {

constexpr std::uint32_t kCodeEa = 0x00401000u;
constexpr std::uint32_t kSubEa = 0x00401200u;
constexpr std::uint32_t kStubEa = 0x7FFE0040u;
constexpr std::uint32_t kBurnEa = 0x7FFE0080u;

// 极简汇编器：只编码本用例要用的那几条（位移一律由 `ea()` 算，⛔ 不手填）。
struct Program {
    std::vector<std::uint8_t> b;
    std::uint32_t ea() const { return kCodeEa + static_cast<std::uint32_t>(b.size()); }
    void u8(std::uint8_t v) { b.push_back(v); }
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
    }
    void pushImm(std::uint32_t v) {
        u8(0x68);
        u32(v);
    }
    void movEcxImm(std::uint32_t v) {
        u8(0xB9);
        u32(v);
    }
    void incEcx() { u8(0x41); }
    void cmpEcxImm(std::uint32_t v) {
        u8(0x81);
        u8(0xF9);
        u32(v);
    }
    void xorEdxEdx() {
        u8(0x31);
        u8(0xD2);
    }
    void incEdx() { u8(0x42); }
    void cmpEdxImm(std::uint32_t v) {
        u8(0x81);
        u8(0xFA);
        u32(v);
    }
    // `jl rel8`：目标必须是**已知**地址（本程序里所有跳转都向后，⛔ 不用汇编器的重定位）
    void jl(std::uint32_t target) {
        const std::int32_t rel = static_cast<std::int32_t>(target) - static_cast<std::int32_t>(ea() + 2);
        u8(0x7C);
        u8(static_cast<std::uint8_t>(rel & 0xFF));
    }
    void call(std::uint32_t target) {
        const std::int32_t rel = static_cast<std::int32_t>(target) - static_cast<std::int32_t>(ea() + 5);
        u8(0xE8);
        u32(static_cast<std::uint32_t>(rel));
    }
    // `jmp rel32`（尾调用用：目标在 200+ 字节之外 ⇒ rel8 装不下）
    void jmp(std::uint32_t target) {
        const std::int32_t rel = static_cast<std::int32_t>(target) - static_cast<std::int32_t>(ea() + 5);
        u8(0xE9);
        u32(static_cast<std::uint32_t>(rel));
    }
    void ret() { u8(0xC3); }
    void padTo(std::uint32_t target) {
        while (ea() < target) u8(0x90);  // nop
    }
};

// 已知 shim：烧 guest 传来的迭代数 —— 让"窗口内 / 窗口外"用**同一份**实现但量级差 10 倍。
volatile std::uint32_t g_sink = 0;

void shimBurn(lift::X86&, lift::ShimCall& call) {
    const std::uint32_t iters = call.u32(0);
    std::uint32_t acc = 1;
    for (std::uint32_t i = 0; i < iters; ++i) acc = acc * 1664525u + 1013904223u + i;
    g_sink = acc;
    call.result = 0;
}

// ★ **运行期追加**的驱动（缺陷 `REQ-01M701PEB38VDQJD4QR2JEBA74` 的那条路径）：
//   `InputScript` 里唯一能在**运行中**把 `profile_begin/end` 塞进去的入口就是 `appendText`
//   （`--interaction-graph` 的 `InteractionGraph::issueAction` 就是这么做的）。这里用宿主 shim
//   直接调同一个入口，理由：bare machine 没有 Present ⇒ 图追加的 `at frame <F+1>` 永远不到期
//   （只能靠帧停滞兜底折算成墙钟，那是另一条口径、不是这张单要守的东西）。
//   ★ 追加的触发值是**绝对 `at insn`**（`appendText` 里 `after`/`wait` 用的是追加段自己的锚点）。
//   ★ 只在第一次调用时追加：门的判据是 `g_arm_calls`，测试末尾断言它正好是 2（= 成功过一次）。
volatile std::uint32_t g_arm_calls = 0;

void shimArmProfile(lift::X86& cpu, lift::ShimCall& call) {
    call.result = 0;
    if (g_arm_calls != 0) return;
    lift::Machine* m = cpu.machine;
    if (m == nullptr) {
        g_arm_calls = 3;  // 没有 Machine 归属 ⇒ 路径根本没走通（测试会红在这一点上）
        return;
    }
    // 窗口 = [now+50, now+400000]：起点落在 arm 之后的那个块边界，终点在 loop2 里
    // ⇒ 窗口内会跑过 loop1 的尾巴 + 一次 burn + 整个 sub + loop2 的一大段。
    const std::uint64_t now = cpu.insns;
    const std::string text = "at insn " + std::to_string(now + 50) + "\n" +   //
                             "profile_begin rt\n" +                            //
                             "at insn " + std::to_string(now + 400000) + "\n" +  //
                             "profile_end rt\n";
    std::string err;
    const bool ok = m->inputScript().appendText(text, "runtime:arm", &err);
    if (!ok) std::fprintf(stderr, "运行期 appendText 失败：%s\n", err.c_str());
    g_arm_calls = ok ? 2 : 3;
}

// 让 guest 跑一段可控长度：主体循环 + 一个真正的 guest 子函数（用来钉 inclusive）。
//   main: push 4000000; call slow        ← 窗口**外**的一次慢调用
//         mov ecx,0; loop1(100)          ← 3 insn/迭代
//         push 400000; call slow         ← 窗口**内**的一次慢调用
//         xor edx,edx; call sub          ← 窗口内的 guest 子函数（inclusive）
//         mov ecx,0; loop2(N)            ← 3 insn/迭代
//         ret
//   sub : sloop(2000){ inc edx; cmp edx,2000; jl sloop }; ret
//         ★ 子函数的**入口就是循环块**（回边跳到函数起点）⇒ 它的 self 全落在同一个块 EA 上，
//           不会因为"`.lst` 缺席 ⇒ 每个块各自成一行"把 self 摊到两行去（那会让断言读不到）。
Program buildProgram(std::uint32_t loop2_iters) {
    Program p;
    const std::uint32_t slow1_iters = 4000000u;  // 窗口外：大
    const std::uint32_t slow2_iters = 400000u;   // 窗口内：小（1/10）
    p.pushImm(slow1_iters);
    p.call(kStubEa);
    p.movEcxImm(0);
    const std::uint32_t loop1 = p.ea();
    p.incEcx();
    p.cmpEcxImm(100);
    p.jl(loop1);
    p.pushImm(slow2_iters);
    p.call(kStubEa);
    p.xorEdxEdx();  // sub 的循环计数器（edx）
    p.call(kSubEa);
    p.movEcxImm(0);
    const std::uint32_t loop2 = p.ea();
    p.incEcx();
    p.cmpEcxImm(loop2_iters);
    p.jl(loop2);
    p.ret();
    p.padTo(kSubEa);
    const std::uint32_t sloop = p.ea();  // = kSubEa：入口即循环块
    p.incEdx();
    p.cmpEdxImm(2000);
    p.jl(sloop);
    p.ret();
    return p;
}

// `profile-window-runtime-append-arm` 用的程序：**第一件事**就是调 `arm` shim，由它在运行中
// `appendText` 追加 `profile_begin/end`（见 `shimArmProfile`）。之后同一份程序里既有循环（guest
// 块耗时）也有一次慢 shim（shim 表）与一个 guest 子函数（function 表），全落在窗口内。
//   main: push 0; call arm            ← 此刻 insns 极小 ⇒ 追加的两个锚点都还在前面
//         mov ecx,0; loop1(100)
//         push 2000000; call burn     ← 窗口内的一次慢 shim（shim 表 / shim 桶都得有账）
//         xor edx,edx; call sub       ← 窗口内的 guest 子函数（function 表）
//         mov ecx,0; loop2(N)
//         ret
//   sub : 同 buildProgram（入口即循环块）
Program buildAppendProgram(std::uint32_t loop2_iters) {
    Program p;
    const std::uint32_t burn_iters = 2000000u;  // 够长：窗口内那一次 shim 的墙钟不会是 0.0ms
    p.pushImm(0);
    p.call(kStubEa);  // arm：运行期 appendText
    p.movEcxImm(0);
    const std::uint32_t loop1 = p.ea();
    p.incEcx();
    p.cmpEcxImm(100);
    p.jl(loop1);
    p.pushImm(burn_iters);
    p.call(kBurnEa);
    p.xorEdxEdx();
    p.call(kSubEa);
    p.movEcxImm(0);
    const std::uint32_t loop2 = p.ea();
    p.incEcx();
    p.cmpEcxImm(loop2_iters);
    p.jl(loop2);
    p.ret();
    p.padTo(kSubEa);
    const std::uint32_t sloop = p.ea();  // = kSubEa：入口即循环块
    p.incEdx();
    p.cmpEdxImm(2000);
    p.jl(sloop);
    p.ret();
    return p;
}

// `profile-window-block-ea-edges`（REQ-01M6XVZ8AXJNG492WA2QAMTA35）用的程序：
// **已知调用链 + 一条尾调用**，四段各有各的可观测面：
//   main: xor edx,edx; loop1(100); mov ecx,0
//         call mid                 ← mid 是"没有父帧"的进入（栈上没有调用者帧）
//         mov ecx,0; loop2(500)
//         xor edx,edx; jmp tail    ← 尾调用（`jmp`）：tail **没有** pushCall ⇒ calls=0 / !BLIND
//   mid : mloop(200); call leaf; ret  ← 入口即循环块（self 稳定 > 0）；这条 call 才是 (mid → leaf) 的边
//   leaf: lloop(2000); ret            ← 入口即循环块（self 全落在 kLeafEa 这一行）
//   tail: tloop(500); ret
// ★ 入口即循环块是**有意**的：`finishBlocks` 只收 ns 增量 > 0 的块，而"只有一条 call 的入口块"
//   可以合法地量到 0 ns（§`window_profile.h` 的块表口径）⇒ 拿它当判据会**抖**（实测抓到过）。
// ★ 窗口 = `at insn 50` → `at insn 9000`：50 在 loop1 里，9000 在 tail 循环里（总长约 9911 条）
//   ⇒ mid / leaf 的调用与返回、tail 的整个循环都落在窗口内。
constexpr std::uint32_t kMidEa = 0x00401200u;
constexpr std::uint32_t kLeafEa = 0x00401300u;
constexpr std::uint32_t kTailEa = 0x00401400u;

Program buildChainProgram() {
    Program p;
    p.xorEdxEdx();  // leaf 的循环计数器（edx）
    p.movEcxImm(0);
    const std::uint32_t loop1 = p.ea();
    p.incEcx();
    p.cmpEcxImm(100);
    p.jl(loop1);
    p.movEcxImm(0);  // mid 的循环计数器（ecx）—— mid 的入口即循环块
    p.call(kMidEa);
    p.movEcxImm(0);
    const std::uint32_t loop2 = p.ea();
    p.incEcx();
    p.cmpEcxImm(500);
    p.jl(loop2);
    p.xorEdxEdx();  // tail 的循环计数器（edx）
    p.jmp(kTailEa);
    p.padTo(kMidEa);
    const std::uint32_t mloop = p.ea();  // = kMidEa：入口即循环块
    p.incEcx();
    p.cmpEcxImm(200);
    p.jl(mloop);
    p.call(kLeafEa);
    p.ret();
    p.padTo(kLeafEa);
    const std::uint32_t lloop = p.ea();  // = kLeafEa：入口即循环块
    p.incEdx();
    p.cmpEdxImm(2000);
    p.jl(lloop);
    p.ret();
    p.padTo(kTailEa);
    const std::uint32_t tloop = p.ea();  // = kTailEa：入口即循环块
    p.incEdx();
    p.cmpEdxImm(500);
    p.jl(tloop);
    p.ret();
    return p;
}

bool attach(lift::Machine& m, const char* name, lift::Conv conv, int argc, lift::ShimFn fn,
            std::uint32_t stub_ea) {
    m.shims().add("test", name, conv, argc, fn);
    lift::ShimDef* def = m.shims().findMutable("test", name);
    if (def == nullptr) return false;
    def->stub_ea = stub_ea;
    m.cache().installShim(stub_ea, def->id);
    return true;
}

bool boot(lift::Machine& m, const Program& code) {
    std::string err;
    if (!m.initBare(&err)) return false;
    if (!m.prepareStack(&err)) return false;
    if (!m.placeCode(kCodeEa, code.b.data(), code.b.size(), &err)) return false;
    m.cpu().eip = kCodeEa;
    return true;
}

// ---- stdout + stderr 一起捕获（未闭合的告警在 stderr；表在 stdout）----
#ifdef _WIN32
using FdT = int;
int dupFd(int fd) { return _dup(fd); }
int dup2Fd(int src, int dst) { return _dup2(src, dst); }
int filenoOf(std::FILE* f) { return _fileno(f); }
void closeFd(int fd) { _close(fd); }
#else
using FdT = int;
int dupFd(int fd) { return ::dup(fd); }
int dup2Fd(int src, int dst) { return ::dup2(src, dst); }
int filenoOf(std::FILE* f) { return ::fileno(f); }
void closeFd(int fd) { ::close(fd); }
#endif

struct Capture {
    std::FILE* tmp = nullptr;
    FdT saved_out = -1;
    FdT saved_err = -1;
    bool active = false;

    bool begin() {
        tmp = std::tmpfile();
        if (tmp == nullptr) return false;
        std::fflush(stdout);
        std::fflush(stderr);
        saved_out = static_cast<FdT>(dupFd(1));
        saved_err = static_cast<FdT>(dupFd(2));
        if (saved_out < 0 || saved_err < 0) return false;
        if (dup2Fd(filenoOf(tmp), 1) < 0) return false;
        if (dup2Fd(filenoOf(tmp), 2) < 0) return false;
        active = true;
        return true;
    }

    std::string finish() {
        if (!active || tmp == nullptr) return {};
        std::fflush(stdout);
        std::fflush(stderr);
        dup2Fd(saved_out, 1);
        dup2Fd(saved_err, 2);
        closeFd(saved_out);
        closeFd(saved_err);
        saved_out = saved_err = -1;
        active = false;
        std::string out;
        std::fseek(tmp, 0, SEEK_END);
        const long size = std::ftell(tmp);
        if (size > 0) {
            out.resize(static_cast<std::size_t>(size));
            std::fseek(tmp, 0, SEEK_SET);
            const std::size_t got = std::fread(out.data(), 1, out.size(), tmp);
            out.resize(got);
        }
        std::fclose(tmp);
        tmp = nullptr;
        return out;
    }
};

std::string lineWith(const std::string& hay, const std::string& needle) {
    const std::size_t at = hay.find(needle);
    if (at == std::string::npos) return {};
    const std::size_t nl = hay.rfind('\n', at);
    const std::size_t line_begin = (nl == std::string::npos) ? 0 : nl + 1;
    const std::size_t end = hay.find('\n', at);
    return hay.substr(line_begin, (end == std::string::npos ? hay.size() : end) - line_begin);
}

// 数一段文本出现几次（"窗口只在预期的两处"用；⛔ 不做子串猜测之外的事）。
std::size_t countOf(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return 0;
    std::size_t n = 0;
    for (std::size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + needle.size())) ++n;
    return n;
}

// `profile-instrumentation-invariant` 的"全关"臂要求 `LIFT_FRAME_TRACE` 真的没开：
// CTest 一般是干净的，但"从带该环境变量的 shell 里跑"不该让这条不变量变成另一种口径。
void clearFrameTraceEnv() {
#ifdef _WIN32
    _putenv_s("LIFT_FRAME_TRACE", "");
#else
    unsetenv("LIFT_FRAME_TRACE");
#endif
}

struct Buckets {
    double wall_ms = 0;
    double guest_ms = 0;
    double shim_ms = 0;
    double native_ms = 0;
    double interp_ms = 0;
    double host_ms = 0;
    double sum_ms = 0;
    double unattributed_ms = 0;
    bool ok = false;
};

// 解析 `buckets   : wall=…ms guest=…ms …` —— 这一行是闭式的**机器可读**面。
Buckets parseBuckets(const std::string& log) {
    Buckets b;
    const std::string line = lineWith(log, "buckets");
    if (line.empty()) return b;
    const int n = std::sscanf(line.c_str(),
                              "buckets   : wall=%lfms guest=%lfms shim=%lfms native=%lfms interp=%lfms "
                              "host=%lfms sum=%lfms unattributed=%lfms",
                              &b.wall_ms, &b.guest_ms, &b.shim_ms, &b.native_ms, &b.interp_ms, &b.host_ms,
                              &b.sum_ms, &b.unattributed_ms);
    b.ok = (n == 8);
    return b;
}

// 解析 `window    : wall=… frame A→B insns C→D …`
bool parseWindowSpan(const std::string& log, std::uint64_t* begin_insns, std::uint64_t* end_insns,
                     std::uint64_t* iterations) {
    const std::string line = lineWith(log, "window    :");
    if (line.empty()) return false;
    unsigned long long a = 0;
    unsigned long long b = 0;
    unsigned long long c = 0;
    unsigned long long d = 0;
    unsigned long long it = 0;
    const int n = std::sscanf(line.c_str(),
                              "window    : wall=%*fms frame %llu\xE2\x86\x92%llu insns %llu\xE2\x86\x92%llu "
                              "guest_ms %*u\xE2\x86\x92%*u iterations=%llu",
                              &a, &b, &c, &d, &it);
    if (n != 5) return false;
    *begin_insns = c;
    *end_insns = d;
    *iterations = it;
    return true;
}

// ---- 块级 EA 表 / 父子边表（`REQ-01M6XVZ8AXJNG492WA2QAMTA35`）的机器可读面 ----
//
// ★ 块行是唯一带 `depth=` 列的行 ⇒ 解析判据里必须含它（否则会去读 `guest fns` / shims 的行）。
// ★ 边行的判据是 `<parent> -> <child>` 这**一整串**（函数表里父/子各自单独出现，不会被拼成它）。
struct BlockRow {
    bool found = false;
    double self_ms = 0.0;
    double incl_ms = 0.0;
    unsigned long long calls = 0;
    int depth = -1;      // -1 = 打印成 `depth=-`（影子栈上没见到这一帧）
    bool blind = false;  // `!BLIND` = 窗口内没有进入计数（尾调用 / 窗口起点已在其内部）
    unsigned long long insns = 0;  // ★ 块级**指令数**（该块在窗口内执行过的条数；不是时间）
};

BlockRow findBlockRow(const std::string& log, std::uint32_t ea) {
    BlockRow out;
    std::size_t at = 0;
    for (;;) {
        const std::size_t nl = log.find('\n', at);
        if (nl == std::string::npos) break;
        const std::size_t begin = nl + 1;
        const std::size_t end = log.find('\n', begin);
        const std::string line = log.substr(begin, (end == std::string::npos ? log.size() : end) - begin);
        at = begin;
        if (line.find("depth=") == std::string::npos) continue;  // 只有块行有这一列
        unsigned idx = 0;
        unsigned ea_got = 0;
        if (std::sscanf(line.c_str(), " %u. %x", &idx, &ea_got) != 2 || ea_got != ea) continue;
        // EA 之后可能还有一列 `[函数名]`（有 `.lst` 时）⇒ 从 `self=` 起解析，不对固定列宽下判据。
        const char* self_at = std::strstr(line.c_str(), " self=");
        if (self_at == nullptr) continue;
        double self_ms = 0.0;
        double incl_ms = 0.0;
        unsigned long long calls = 0;
        char depth[16] = {0};
        if (std::sscanf(self_at, " self=%lfms incl=%lfms calls=%llu depth=%15s", &self_ms, &incl_ms, &calls, depth) !=
            4) {
            continue;
        }
        out.found = true;
        out.self_ms = self_ms;
        out.incl_ms = incl_ms;
        out.calls = calls;
        out.depth = (depth[0] == '-') ? -1 : std::atoi(depth);
        out.blind = line.find("!BLIND") != std::string::npos;
        // ★ 块级指令数：`depth=` 之后是本轮追加的 ` insns=<n>`（`!BLIND` 仍排在最后）。
        if (const char* ins_at = std::strstr(line.c_str(), " insns=")) {
            (void)std::sscanf(ins_at, " insns=%llu", &out.insns);
        }
        return out;
    }
    return out;
}

std::string edgeFnName(std::uint32_t ea) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "sub_%08X", ea);
    return std::string(buf);
}

// ---- 块级**指令数**（`insns=`）的扫描面 —— 本单的硬判据（`REQ-01M6XVZ8AXJNG492WA2QAMTA35`）----
//   ★ 行判据同 `findBlockRow`：只有块行带 `depth=`；块行**一定**带 ` insns=`（缺列就是没落地）。
//   ★ 这里扫的是**日志里实际印出来的每一行**（不是只看 top-3）：只要有一行 insns=0 / 缺列就红。
struct BlockInsnsScan {
    std::size_t rows = 0;     // 块行数
    std::size_t missing = 0;  // 没有 ` insns=` 列的块行（⛔ 必须 0）
    std::size_t zero = 0;     // `insns=0` 的块行（⛔ 必须 0：执行过的块至少 1 条指令）
    unsigned long long sum = 0;
};

BlockInsnsScan scanBlockInsns(const std::string& log) {
    BlockInsnsScan out;
    std::size_t at = 0;
    for (;;) {
        const std::size_t nl = log.find('\n', at);
        if (nl == std::string::npos) break;
        const std::size_t begin = nl + 1;
        const std::size_t end = log.find('\n', begin);
        const std::string line = log.substr(begin, (end == std::string::npos ? log.size() : end) - begin);
        at = begin;
        if (line.find("depth=") == std::string::npos) continue;
        unsigned idx = 0;
        unsigned ea_got = 0;
        if (std::sscanf(line.c_str(), " %u. %x", &idx, &ea_got) != 2) continue;
        ++out.rows;
        const char* ins_at = std::strstr(line.c_str(), " insns=");
        unsigned long long v = 0;
        if (ins_at == nullptr || std::sscanf(ins_at, " insns=%llu", &v) != 1) {
            ++out.missing;
            continue;
        }
        if (v == 0) ++out.zero;
        out.sum += v;
    }
    return out;
}

// 解析 `insnsBlk  : block=… window=… diff=… override=… unattributed=…`（闭合行的机器可读面）。
struct InsnsClosure {
    bool ok = false;
    unsigned long long block = 0;
    unsigned long long window = 0;
    long long diff = 0;
    unsigned long long override_insns = 0;
    long long unattributed = 0;
};

InsnsClosure parseInsnsClosure(const std::string& log) {
    InsnsClosure c;
    const std::string line = lineWith(log, "insnsBlk  :");
    if (line.empty()) return c;
    const int n = std::sscanf(line.c_str(),
                              "insnsBlk  : block=%llu window=%llu diff=%lld override=%llu unattributed=%lld",
                              &c.block, &c.window, &c.diff, &c.override_insns, &c.unattributed);
    c.ok = (n == 5);
    return c;
}

struct EdgeRow {
    bool found = false;
    double incl_ms = 0.0;
    unsigned long long calls = 0;
};

EdgeRow findEdgeRow(const std::string& log, std::uint32_t parent, std::uint32_t child) {
    EdgeRow out;
    const std::string needle = edgeFnName(parent) + " -> " + edgeFnName(child);
    const std::size_t at = log.find(needle);
    if (at == std::string::npos) return out;
    const std::size_t end = log.find('\n', at);
    const std::string line = log.substr(at, (end == std::string::npos ? log.size() : end) - at);
    const std::size_t incl_at = line.find("incl=");
    if (incl_at == std::string::npos) return out;
    double incl_ms = 0.0;
    unsigned long long calls = 0;
    if (std::sscanf(line.c_str() + incl_at, "incl=%lfms calls=%llu", &incl_ms, &calls) != 2) return out;
    out.found = true;
    out.incl_ms = incl_ms;
    out.calls = calls;
    return out;
}

}  // namespace

int main() {
    using namespace lift_test;

    // =================================================================================
    // ① 闭式 + 窗口外不计入 + 边界可复现（`at insn` 定界）
    // =================================================================================
    {
        g_current_case = "profile-window-closure";
        Program prog = buildProgram(200000u);  // loop2 = 200k 次 ⇒ 窗口约 20 ms 量级
        lift::Machine m;
        LIFT_CHECK(boot(m, prog));
        LIFT_CHECK(attach(m, "burn", lift::Conv::Stdcall, 1, &shimBurn, kStubEa));

        // 窗口 = insns 50 → 300000：覆盖第二次慢调用 + `sub` 子函数（6000 条）+ loop2 的一大段。
        //   ★ 边界用 `at insn`（**帧内也能切**；帧号在 bare machine 里根本不推进）。
        const char* script =
            "at insn 50\n"
            "profile_begin enter\n"
            "at insn 300000\n"
            "profile_end enter\n";
        std::string err;
        LIFT_CHECK(m.loadInputScriptText(script, "inline:closure", &err));
        LIFT_CHECK(m.inputScript().hasProfileActions());

        lift::RunOptions o;
        o.until_ea = 0xFFFFFFFFu;
        o.max_insns = 200000000;
        Capture cap;
        LIFT_CHECK(cap.begin());
        const lift::RunResult r = m.run(o);
        const std::string log = cap.finish();
        LIFT_CHECK(r.reason == lift::StopReason::UntilEa);

        LIFT_CHECK(log.find("=== profile window: enter ===") != std::string::npos);
        const Buckets b = parseBuckets(log);
        LIFT_CHECK(b.ok);
        if (b.ok) {
            const double tol = (b.wall_ms * 0.08 > 2.0) ? b.wall_ms * 0.08 : 2.0;
            // ① 闭式：sum + unattributed == wall（这里等价于 sum ≈ wall 且 unattributed 小）
            LIFT_CHECK(b.sum_ms + b.unattributed_ms > b.wall_ms - tol);
            LIFT_CHECK(b.sum_ms + b.unattributed_ms < b.wall_ms + tol);
            LIFT_CHECK(b.unattributed_ms < tol && b.unattributed_ms > -tol);
            // 四个桶都得真有事（否则"闭式"可能只是"全 0 = 全 0"）
            LIFT_CHECK(b.guest_ms > 0.0);
            LIFT_CHECK(b.shim_ms > 0.0);
            LIFT_CHECK(b.interp_ms > 0.0);
            // ② 窗口外那次慢调用（4,000,000 次）不许算进来：
            //   窗口内只调用一次（400,000 次）⇒ 窗口 shim 桶远小于总 shim 墙钟。
            const auto* def = m.shims().find("test", "burn");
            LIFT_CHECK(def != nullptr);
            if (def != nullptr) {
                const double total_shim_ms = static_cast<double>(def->ns) / 1e6;
                LIFT_CHECK(total_shim_ms > 0.0);
                LIFT_CHECK(b.shim_ms < total_shim_ms * 0.5);
            }
            // 窗口内 shim 表那一行：**只调用一次**
            const std::string row = lineWith(log, "test!burn");
            LIFT_CHECK(!row.empty());
            LIFT_CHECK(row.find("calls=1") != std::string::npos);
            LIFT_CHECK(row.find("calls=10") == std::string::npos && row.find("calls=2") == std::string::npos);
        }
        // ③ 边界落在 `at insn` 上（帧内切）：begin/end 的 insns 必须落在触发点附近（差一个块）
        std::uint64_t begin_insns = 0;
        std::uint64_t end_insns = 0;
        std::uint64_t iterations = 0;
        LIFT_CHECK(parseWindowSpan(log, &begin_insns, &end_insns, &iterations));
        LIFT_CHECK(begin_insns >= 50 && begin_insns <= 80);
        LIFT_CHECK(end_insns >= 300000 && end_insns <= 300030);
        LIFT_CHECK(end_insns > begin_insns);
        LIFT_CHECK(iterations > 0);
        // ④ inclusive vs self：`sub`（kSubEa）在窗口内被调用一次，inclusive ≥ self > 0
        const std::string subrow = lineWith(log, "sub_00401200");
        LIFT_CHECK(!subrow.empty());
        if (!subrow.empty()) {
            double self_ms = 0;
            double incl_ms = 0;
            unsigned long long calls = 0;
            const char* at = std::strstr(subrow.c_str(), "self=");
            LIFT_CHECK(at != nullptr);
            if (at != nullptr) {
                const int n = std::sscanf(at, "self=%lfms(%*[^)]) incl=%lfms(%*[^)]) calls=%llu", &self_ms, &incl_ms,
                                          &calls);
                LIFT_CHECK(n == 3);
                if (n == 3) {
                    LIFT_CHECK(self_ms > 0.0);
                    LIFT_CHECK(incl_ms + 0.001 > self_ms);
                    LIFT_CHECK(calls == 1ull);
                }
            }
        }
    }

    // =================================================================================
    // ①b 口径不变量：仪器**不许改执行流**（四臂 insns 全等；窗口只在开了窗口的两臂）
    //     A 全关 / B 仅 --profile / C 仅窗口 profiler / D 两者。同一份程序、四台新 Machine。
    //     ⛔ 这里不断言任何耗时：膨胀量级是读数（measurement.md M-02），不是不变量。
    // =================================================================================
    {
        g_current_case = "profile-instrumentation-invariant";
        clearFrameTraceEnv();
        struct Arm {
            const char* name = "";
            bool use_profile = false;
            bool use_window = false;
            bool ran = false;
            bool ok = false;
            lift::RunResult r;
            std::string log;
            bool armed = false;
            bool profile_on = false;
            std::uint64_t begin_insns = 0;
            std::uint64_t end_insns = 0;
            std::uint64_t iterations = 0;
        };
        auto runArm = [](Arm* arm) -> bool {
            lift::Machine m;
            Program prog = buildProgram(200000u);
            if (!boot(m, prog)) return false;
            if (!attach(m, "burn", lift::Conv::Stdcall, 1, &shimBurn, kStubEa)) return false;
            if (arm->use_window) {
                // 与 ① 同一组 `at insn` 锚点：窗口覆盖第二次慢调用 + guest 子函数 + loop2 一段。
                const char* script =
                    "at insn 50\n"
                    "profile_begin enter\n"
                    "at insn 300000\n"
                    "profile_end enter\n";
                std::string err;
                if (!m.loadInputScriptText(script, "inline:invariant", &err)) return false;
                if (!m.inputScript().hasProfileActions()) return false;
            }
            lift::RunOptions o;
            o.until_ea = 0xFFFFFFFFu;
            o.max_insns = 200000000;
            o.profile = arm->use_profile;
            Capture cap;
            if (!cap.begin()) return false;
            arm->r = m.run(o);
            // ★ `instr :` 行在运行摘要里（main 打的就是这一份）⇒ 捕获它才能断言"读的是真实 armed 位"。
            m.dumpRunSummary(arm->r, stdout);
            arm->log = cap.finish();
            arm->armed = m.profilerArmed();
            arm->profile_on = m.instrumentation().profile;
            arm->ok = arm->r.reason == lift::StopReason::UntilEa;
            parseWindowSpan(arm->log, &arm->begin_insns, &arm->end_insns, &arm->iterations);
            arm->ran = true;
            return true;
        };

        Arm arms[4] = {{"A-off", false, false}, {"B-profile", true, false},
                       {"C-window", false, true}, {"D-both", true, true}};
        for (Arm& a : arms) LIFT_CHECK(runArm(&a));
        for (const Arm& a : arms) {
            LIFT_CHECK(a.ran);
            LIFT_CHECK(a.ok);  // 同一份程序、同一个哨兵结尾 ⇒ 四臂都该是 UntilEa
        }
        // ★ 口径不变量：四条路径的**指令数必须相等**（仪器只许花时间，不许改执行流）。
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(arms[0].r.insns),
                          static_cast<std::int64_t>(arms[1].r.insns));
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(arms[0].r.insns),
                          static_cast<std::int64_t>(arms[2].r.insns));
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(arms[0].r.insns),
                          static_cast<std::int64_t>(arms[3].r.insns));
        LIFT_CHECK(arms[0].r.insns > 0);
        // ★ 窗口只在**开了窗口 profiler** 的两臂出现（A/B 一次也不许有）。
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(countOf(arms[0].log, "=== profile window: enter ===")), 0);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(countOf(arms[1].log, "=== profile window: enter ===")), 0);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(countOf(arms[2].log, "=== profile window: enter ===")), 1);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(countOf(arms[3].log, "=== profile window: enter ===")), 1);
        // ★ 窗口边界也是执行流的函数：同一份脚本、同一份程序 ⇒ C/D 的 begin/end insns 必须相等
        //   （⛔ 不断言具体数字：那条会随程序改而红，而红的含义不是"仪器改坏了执行流"）。
        LIFT_CHECK(arms[2].begin_insns > 0 && arms[2].end_insns > arms[2].begin_insns);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(arms[2].begin_insns),
                          static_cast<std::int64_t>(arms[3].begin_insns));
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(arms[2].end_insns),
                          static_cast<std::int64_t>(arms[3].end_insns));
        LIFT_CHECK(arms[2].end_insns <= arms[2].r.insns);
        LIFT_CHECK(arms[2].iterations > 0 && arms[3].iterations > 0);
        // ★ 窗口不是**残表**：窗口内既有 shim（第二次慢调用）也有 guest 函数（`sub`）的账。
        LIFT_CHECK(arms[2].log.find("test!burn") != std::string::npos);
        LIFT_CHECK(arms[2].log.find("sub_00401200") != std::string::npos);
        // ★ `instr :` 读的是**真实 armed 位**（⛔ 不是 CLI 回显）：B 臂 profile=on / window-prof=off；
        //   C 臂反过来。这条同时钉住 `Machine::profilerArmed()` 就是摘要里那个位。
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(arms[0].armed), 0);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(arms[1].armed), 0);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(arms[2].armed), 1);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(arms[3].armed), 1);
        for (const Arm& a : arms) {
            const std::string line = lineWith(a.log, "instr     :");
            LIFT_CHECK(!line.empty());
            if (line.empty()) continue;
            LIFT_CHECK(line.find(a.use_profile ? "profile=on" : "profile=off") != std::string::npos);
            LIFT_CHECK(line.find(a.use_window ? "window-prof=on" : "window-prof=off") != std::string::npos);
            LIFT_CHECK(a.profile_on == a.use_profile);
        }
    }

    // =================================================================================
    // ② 未闭合：脚本结束仍开着窗口 ⇒ 自动收口 + 告警 + 照样打印（⛔ 不静默丢数据）
    // =================================================================================
    {
        g_current_case = "profile-window-unclosed";
        Program prog = buildProgram(20000u);
        lift::Machine m;
        LIFT_CHECK(boot(m, prog));
        LIFT_CHECK(attach(m, "burn", lift::Conv::Stdcall, 1, &shimBurn, kStubEa));
        const char* script =
            "at insn 20\n"
            "profile_begin orphan\n"
            "at insn 200\n"
            "wait 1 insn\n";
        std::string err;
        LIFT_CHECK(m.loadInputScriptText(script, "inline:unclosed", &err));

        lift::RunOptions o;
        o.until_ea = 0xFFFFFFFFu;
        o.max_insns = 200000000;
        Capture cap;
        LIFT_CHECK(cap.begin());
        const lift::RunResult r = m.run(o);
        const std::string log = cap.finish();
        LIFT_CHECK(r.reason == lift::StopReason::UntilEa);
        LIFT_CHECK(log.find("未闭合") != std::string::npos);              // 告警在 stderr
        LIFT_CHECK(log.find("=== profile window: orphan ===") != std::string::npos);  // 表照打
    }

    // =================================================================================
    // ③ `profile_end` 没有对应的 begin ⇒ 明确告警（不是静默忽略）
    // =================================================================================
    {
        g_current_case = "profile-window-end-without-begin";
        Program prog = buildProgram(1000u);
        lift::Machine m;
        LIFT_CHECK(boot(m, prog));
        LIFT_CHECK(attach(m, "burn", lift::Conv::Stdcall, 1, &shimBurn, kStubEa));
        const char* script =
            "at insn 10\n"
            "profile_end stray\n"
            "quit\n";
        std::string err;
        LIFT_CHECK(m.loadInputScriptText(script, "inline:stray", &err));
        lift::RunOptions o;
        o.until_ea = 0xFFFFFFFFu;
        o.max_insns = 200000000;
        Capture cap;
        LIFT_CHECK(cap.begin());
        const lift::RunResult r = m.run(o);
        const std::string log = cap.finish();
        LIFT_CHECK(r.reason == lift::StopReason::Stopped);  // quit 仍然生效
        LIFT_CHECK(log.find("没有打开的窗口") != std::string::npos);
        LIFT_CHECK(log.find("=== profile window:") == std::string::npos);
    }

    // =================================================================================
    // ④ 帧停滞兜底：帧号不推进时 `at frame` **不会永远不触发**（README §4.2c）
    //    正面：默认兜底开着（这里设 1 ms）⇒ 告警一次，并把剩余 frame 条件折成 wall_ms ⇒ quit 生效。
    //    反面：把它关掉（0）⇒ 不折算、脚本停在那里（run 由哨兵返回地址结束，不是 Stopped）。
    // =================================================================================
    {
        g_current_case = "profile-window-frame-stall-fallback";
        Program prog = buildProgram(1000000u);  // 3M insn ⇒ 约 100 ms，足够跑过 1 ms 的停滞阈值
        {
            lift::Machine m;
            LIFT_CHECK(boot(m, prog));
            LIFT_CHECK(attach(m, "burn", lift::Conv::Stdcall, 1, &shimBurn, kStubEa));
            const char* script =
                "at frame 3\n"
                "quit\n";
            std::string err;
            LIFT_CHECK(m.loadInputScriptText(script, "inline:stall", &err));
            m.setInputStallMs(1);
            m.setInputStallBootGraceMs(1);  // bare 机器没有 Present ⇒ 宽限也要调小
            lift::RunOptions o;
            o.until_ea = 0xFFFFFFFFu;
            o.max_insns = 200000000;
            Capture cap;
            LIFT_CHECK(cap.begin());
            const lift::RunResult r = m.run(o);
            const std::string log = cap.finish();
            LIFT_CHECK(m.inputScript().stallFallbackFired());
            LIFT_CHECK(log.find("帧停滞") != std::string::npos);
            LIFT_CHECK(r.reason == lift::StopReason::Stopped);  // 折算之后 quit 真的触发
        }
        {
            lift::Machine m;
            LIFT_CHECK(boot(m, prog));
            LIFT_CHECK(attach(m, "burn", lift::Conv::Stdcall, 1, &shimBurn, kStubEa));
            const char* script =
                "at frame 3\n"
                "quit\n";
            std::string err;
            LIFT_CHECK(m.loadInputScriptText(script, "inline:nostall", &err));
            m.setInputStallMs(0);  // 显式关闭
            lift::RunOptions o;
            o.until_ea = 0xFFFFFFFFu;
            o.max_insns = 200000000;
            const lift::RunResult r = m.run(o);
            LIFT_CHECK(!m.inputScript().stallFallbackFired());
            LIFT_CHECK(r.reason == lift::StopReason::UntilEa);
        }
    }

    // =================================================================================
    // ⑤ 语法：新的触发单位与 profile 动作必须被解析；错的必须被**拒绝**（⛔ 不猜）
    // =================================================================================
    {
        g_current_case = "profile-window-script-syntax";
        lift::Machine m;
        std::string err;
        LIFT_CHECK(m.inputScript().loadText(
            "at insn 10\nprofile_begin a\nat wall_ms 5\nprofile_end a\nat guest_ms 7\nmouse_move 1 2\n"
            "after 3 insn\nmouse_up\nat frame 5\nwait 2 frames stall 500\nquit\n",
            "inline:ok", &err));
        LIFT_CHECK(m.inputScript().hasProfileActions());
        LIFT_CHECK(!m.inputScript().loadText("at insn 10\nprofile_begin\n", "bad:label", &err));
        LIFT_CHECK(!m.inputScript().loadText("at frobnicate 3\nquit\n", "bad:unit", &err));
        LIFT_CHECK(!m.inputScript().loadText("after 3 insn\nquit\n", "bad:anchor", &err));  // 没有基准
    }

    // =================================================================================
    // ⑥ **运行期追加**的 `profile_begin/end` 必须重新 arm（REQ-01M701PEB38VDQJD4QR2JEBA74）
    //   缺陷：`run()` 只在 prepare 期按 `input_script_.hasProfileActions()` arm 一次；而
    //   `--interaction-graph` 的 `profile_begin/end` 是**运行中**经 `appendText` 追加的
    //   ⇒ `armed_` 停在 false ⇒ 窗口照开照收、表却全是 0（不报错、不告警 = 静默残表）。
    //   判据（缺一条都说明"运行期 arm"没落地）：
    //     ① `appendText` 真的在运行中被调用（`g_arm_calls==2`）——否则这条用例测的是空气；
    //     ② arm 状态：`profilerArmed()` 与运行摘要的 `window_prof` 位都是真；
    //     ③ 窗口内真的记了账：iterations > 0（迭代守卫只在 armed 时跑）、guest 函数表非空、
    //        窗口内那次 shim 的**墙钟列**非 0（shim 计时必须随 arm 一起打开）；
    //     ④ ⛔ 没有"残表"告警（`closeProfileWindow` 在未 arm 时打的那一条）。
    //   ★ 断言的是**关系**（>0 / 非空 / 无告警），⛔ 不断言任何具体耗时或 insns 数字。
    // =================================================================================
    {
        g_current_case = "profile-window-runtime-append-arm";
        g_arm_calls = 0;
        Program prog = buildAppendProgram(200000u);
        lift::Machine m;
        LIFT_CHECK(boot(m, prog));
        LIFT_CHECK(attach(m, "arm", lift::Conv::Stdcall, 1, &shimArmProfile, kStubEa));
        LIFT_CHECK(attach(m, "burn", lift::Conv::Stdcall, 1, &shimBurn, kBurnEa));
        // ★ 初始脚本里**没有** profile 动作（这正是缺陷的入口：prepare 期不可能 arm 对）。
        std::string err;
        LIFT_CHECK(m.loadInputScriptText("at insn 1\nwait 1 insn\n", "inline:runtime-arm", &err));
        LIFT_CHECK(!m.inputScript().hasProfileActions());

        lift::RunOptions o;
        o.until_ea = 0xFFFFFFFFu;
        o.max_insns = 200000000;
        Capture cap;
        LIFT_CHECK(cap.begin());
        const lift::RunResult r = m.run(o);
        const std::string log = cap.finish();
        LIFT_CHECK(r.reason == lift::StopReason::UntilEa);

        // ① 这条路径真的被走了（没有它，后面的"绿"什么也不证明）
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(g_arm_calls), 2);
        LIFT_CHECK(m.inputScript().hasProfileActions());
        // ② arm 状态：运行期追加之后必须为真（缺陷态里它是 false）
        LIFT_CHECK(m.profilerArmed());
        LIFT_CHECK(m.instrumentation().window_prof);
        // ③ 表不是残表
        LIFT_CHECK(log.find("=== profile window: rt ===") != std::string::npos);
        std::uint64_t begin_insns = 0;
        std::uint64_t end_insns = 0;
        std::uint64_t iterations = 0;
        LIFT_CHECK(parseWindowSpan(log, &begin_insns, &end_insns, &iterations));
        LIFT_CHECK(begin_insns >= 50 && begin_insns <= 200);
        LIFT_CHECK(end_insns - begin_insns > 300000);  // 追加的两个锚点之间真的被覆盖
        LIFT_CHECK(iterations > 0);                    // IterScope 只在 armed 时开
        const Buckets b = parseBuckets(log);
        LIFT_CHECK(b.ok);
        if (b.ok) {
            LIFT_CHECK(b.guest_ms > 0.0);
            LIFT_CHECK(b.interp_ms > 0.0);  // interp 只有 armed 才记
        }
        // function 表：窗口内的**块耗时**（self）必须聚成行。缺陷态里 `b->ns` 一次也没累计
        // ⇒ 行还在（inclusive 影子栈照样记），但每一行的 self 都是 0.000ms（实测 symptom）。
        LIFT_CHECK(log.find("guest fns :") != std::string::npos);
        const std::string subrow = lineWith(log, "sub_00401200");
        LIFT_CHECK(!subrow.empty());
        if (!subrow.empty()) {
            const char* self_at = std::strstr(subrow.c_str(), "self=");
            LIFT_CHECK(self_at != nullptr);
            if (self_at != nullptr) {
                double self_ms = 0.0;
                const int n = std::sscanf(self_at, "self=%lfms", &self_ms);
                LIFT_CHECK(n == 1);
                LIFT_CHECK(self_ms > 0.0);
            }
        }
        // shim 表：窗口内那一次 burn 的**墙钟列**非 0（只有 `calls` 非 0 不够 —— 计数恒记、
        // 墙钟要在 `shims_.setTiming(true)` 之后才有 ⇒ 它才是"arm 把计时也打开了"的证据）。
        const std::string row = lineWith(log, "test!burn");
        LIFT_CHECK(!row.empty());
        if (!row.empty()) {
            LIFT_CHECK(row.find("calls=1") != std::string::npos);
            const char* ns_at = std::strstr(row.c_str(), "ns=");
            LIFT_CHECK(ns_at != nullptr);
            if (ns_at != nullptr) {
                double ns_ms = 0.0;
                const int n = std::sscanf(ns_at, "ns=%lfms", &ns_ms);
                LIFT_CHECK(n == 1);
                LIFT_CHECK(ns_ms > 0.0);
            }
        }
        // ④ ⛔ 不许出现"残表"告警：它一出现就说明恰恰是本缺陷（表全 0 却照常打印）
        LIFT_CHECK(log.find("残表") == std::string::npos);
    }

    // =================================================================================
    // ⑦ **块级 EA 表 + 父子边表**（`REQ-01M6XVZ8AXJNG492WA2QAMTA35`）
    //   缺陷态：窗口表**只有按函数名聚合的行** ⇒ 看不出"这一段的 inclusive 里谁占了谁"。
    //   判据（缺一条都说明这张表没落地，或者落地的是假数据）：
    //     ① 块级表按**块 EA** 出行，且 self/incl 关系成立（incl 是**包含该块的函数**的口径）·
    //        `depth` 与已知调用链一致（leaf 比 mid 深一层）；
    //     ② 父子边表出现 `(mid → leaf)` 这条**已知的边**，次数 = 1、incl ∈ (0, mid 的 incl]；
    //     ③ `calls=0` 的段（这里 = `jmp` 尾调用进入的 tail）被**显式标记** `!BLIND`
    //        —— ⛔ 不许让人把它读成"调用了 0 次"；有进入计数的行不许带这个标记。
    //   ★ 程序形状见 `buildChainProgram`；窗口 = `at insn 50 → 9000`。
    //   ★ 变异（`pnpm test:mutation`）：把 `pushCall` 里的 `parent_key` 恒置 0 ⇒ ② 当场红。
    // =================================================================================
    {
        g_current_case = "profile-window-block-ea-edges";
        // 表的上界是打印期环境变量（巨帧只看 top-N）；本用例的块数很少，给足就够了。
#ifdef _WIN32
        _putenv_s("LIFT_WINDOW_BLOCK_TOP", "64");
        _putenv_s("LIFT_WINDOW_EDGE_TOP", "8");
#else
        setenv("LIFT_WINDOW_BLOCK_TOP", "64", 1);
        setenv("LIFT_WINDOW_EDGE_TOP", "8", 1);
#endif
        Program prog = buildChainProgram();
        lift::Machine m;
        LIFT_CHECK(boot(m, prog));
        const char* script =
            "at insn 50\n"
            "profile_begin blk\n"
            "at insn 9000\n"
            "profile_end blk\n";
        std::string err;
        LIFT_CHECK(m.loadInputScriptText(script, "inline:block-ea", &err));
        lift::RunOptions o;
        o.until_ea = 0xFFFFFFFFu;
        o.max_insns = 200000000;
        Capture cap;
        LIFT_CHECK(cap.begin());
        const lift::RunResult r = m.run(o);
        const std::string log = cap.finish();
        LIFT_CHECK(r.reason == lift::StopReason::UntilEa);
        LIFT_CHECK(log.find("=== profile window: blk ===") != std::string::npos);
        LIFT_CHECK(log.find("blockEA   :") != std::string::npos);
        LIFT_CHECK(log.find("edges     :") != std::string::npos);
        // 窗口自己的 insns 边界（块级 insns 的闭合判据要用；也是"边界落在 at insn 上"的读数）
        std::uint64_t begin_insns = 0;
        std::uint64_t end_insns = 0;
        std::uint64_t iterations = 0;
        LIFT_CHECK(parseWindowSpan(log, &begin_insns, &end_insns, &iterations));
        LIFT_CHECK(begin_insns >= 50 && begin_insns <= 80);
        LIFT_CHECK(end_insns >= 9000 && end_insns <= 9010);

        // ① 块级 EA 表：四个已知块 EA 都要出行；self/incl/depth 的关系必须成立
        const BlockRow mid = findBlockRow(log, kMidEa);
        const BlockRow leaf = findBlockRow(log, kLeafEa);
        const BlockRow tail = findBlockRow(log, kTailEa);
        LIFT_CHECK(mid.found);
        LIFT_CHECK(leaf.found);
        LIFT_CHECK(tail.found);
        if (mid.found && leaf.found) {
            // ★ 三段都是"入口即循环块"⇒ 它们的块行 self 必须真有事（0.000ms 只能来自空块）
            LIFT_CHECK(mid.self_ms > 0.0);
            LIFT_CHECK(leaf.self_ms > 0.0);
            LIFT_CHECK(tail.self_ms > 0.0);
            LIFT_CHECK(mid.incl_ms > 0.0);
            // incl 是**函数**口径 ⇒ 恒 ≥ 该块的 self（块自己的 self 只是函数 inclusive 的一部分）
            LIFT_CHECK(mid.incl_ms + 0.001 >= mid.self_ms);
            LIFT_CHECK(leaf.incl_ms + 0.001 >= leaf.self_ms);
            // mid 的 inclusive 包住整个 leaf 调用 + mloop ⇒ 严格大于 leaf 的 inclusive
            LIFT_CHECK(mid.incl_ms > leaf.incl_ms);
            // 已知调用链的深度：mid 无父帧（depth=1），leaf 在 mid 之下（depth=2）
            LIFT_CHECK_EQ_I64(mid.depth, 1);
            LIFT_CHECK_EQ_I64(leaf.depth, 2);
            LIFT_CHECK_EQ_I64(leaf.depth, mid.depth + 1);
            LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mid.calls), 1);
            LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(leaf.calls), 1);
            LIFT_CHECK(!mid.blind);
            LIFT_CHECK(!leaf.blind);
        }
        // 块表与 guest 桶必须对得上（同一份块耗时；差应只是边界上的半块）
        {
            const std::string sumline = lineWith(log, "块级汇总");
            LIFT_CHECK(!sumline.empty());
            const char* self_at = sumline.empty() ? nullptr : std::strstr(sumline.c_str(), "self 合计 = ");
            LIFT_CHECK(self_at != nullptr);
            if (self_at != nullptr) {
                unsigned long long rows = 0;
                double self_ms = 0.0;
                double guest_ms = 0.0;
                double diff_ms = 0.0;
                const int n = std::sscanf(sumline.c_str(), "            （块级汇总：%llu 条块行 / self 合计 = %lfms"
                                                          "（guest 桶 = %lfms，差 = %lfms）",
                                          &rows, &self_ms, &guest_ms, &diff_ms);
                LIFT_CHECK(n == 4);
                if (n == 4) {
                    LIFT_CHECK(rows >= 4);  // main / mid / leaf / tail 至少四块
                    LIFT_CHECK(self_ms > 0.0);
                    LIFT_CHECK(guest_ms > 0.0);
                    LIFT_CHECK(diff_ms < 2.0 && diff_ms > -2.0);
                }
            }
        }

        // ② 父子边表：**(mid → leaf)** 必须出现，次数 = 1，incl 落在 (0, mid 的 incl] 里
        const EdgeRow edge = findEdgeRow(log, kMidEa, kLeafEa);
        LIFT_CHECK(edge.found);
        if (edge.found) {
            LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(edge.calls), 1);
            LIFT_CHECK(edge.incl_ms > 0.0);
            if (leaf.found) {
                // 非递归链上，进入 leaf 的唯一边的 incl 应与 leaf 的 inclusive 同量级
                LIFT_CHECK(edge.incl_ms + 0.001 >= leaf.incl_ms);
                LIFT_CHECK(edge.incl_ms < leaf.incl_ms * 1.5 + 1.0);
            }
            if (mid.found) {
                LIFT_CHECK(edge.incl_ms <= mid.incl_ms + 0.001);
            }
        }
        // 边表的覆盖边界必须自报：mid 是"没有父帧"的进入 ⇒ 汇总里要打出这个数（⛔ 不许静默）
        {
            const std::string esum = lineWith(log, "边表汇总");
            LIFT_CHECK(!esum.empty());
            const char* at = esum.empty() ? nullptr : std::strstr(esum.c_str(), "的进入 ");
            LIFT_CHECK(at != nullptr);
            if (at != nullptr) {
                unsigned long long no_parent = 0;
                LIFT_CHECK(std::sscanf(at, "的进入 %llu 次", &no_parent) == 1);
                LIFT_CHECK(no_parent >= 1ull);  // mid 就是这一条
            }
        }

        // ③ 尾调用行（calls=0）必须显式标 `!BLIND`，且有进入计数的行不许带它
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(tail.calls), 0);
        LIFT_CHECK(tail.self_ms > 0.0);
        LIFT_CHECK(tail.blind);
        // 报告里也要有这张表的**口径**（否则 `!BLIND` 只是一个没人解释的记号）
        LIFT_CHECK(log.find("没有进入计数") != std::string::npos);

        // =========================================================================================
        // ④ 块级**指令数**（2026-11-10 追加；本单的第 ③ 步）：每行 `insns > 0`，且块级之和与
        //    窗口 insns **精确闭合**（⛔ 不是"约等于"）。
        //    判据分三层，缺一层都说明"按块累计"接错了地方：
        //      (a) 每一行块行都有 `insns=` 列且 > 0（0 / 缺列 = 累加没接上，或接在了错误粒度）；
        //      (b) 打印出来的行之和 == 汇总行报的块级合计（说明合计不是另算一份）；
        //      (c) window = Σblock + override + unattributed **逐位相等**，且这里没有 override
        //          （自造机器码、未装 native override）⇒ override = 0、unattributed = 0。
        //    ★ 另钉两个"程序形状"等式：`mid` 循环块 = 200×3 条、`leaf` 循环块 = 2000×3 条
        //      ⇒ 若把别人的指令数算到这一块头上（粒度错），这两条当场红。
        //    ★ 变异（`pnpm test:mutation`）：dispatcher 把块执行条数传成 0 ⇒ (a)(b)(c) 一起红。
        // =========================================================================================
        {
            const BlockInsnsScan scan = scanBlockInsns(log);
            const InsnsClosure c = parseInsnsClosure(log);
            LIFT_CHECK(scan.rows >= 4);
            LIFT_CHECK(scan.missing == 0);  // 每行都带 insns=
            LIFT_CHECK(scan.zero == 0);     // 每块 insns > 0
            LIFT_CHECK(c.ok);               // 闭合行本身必须存在且可解析
            if (c.ok) {
                // (b) 打印行之和 == 汇总报的块级合计（本用例块数远小于 top=64 ⇒ 全打印）
                LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(c.block), static_cast<std::int64_t>(scan.sum));
                // (c) window = Σblock + override + unattributed（逐位）
                LIFT_CHECK_EQ_I64(c.diff, 0);
                LIFT_CHECK_EQ_I64(c.unattributed, 0);
                LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(c.override_insns), 0);
                LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(c.window),
                                  static_cast<std::int64_t>(end_insns - begin_insns));
                LIFT_CHECK(c.block > 0);
            }
            // 程序形状（确定性计数，与耗时无关）：
            LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mid.insns), 200ll * 3ll);   // mloop = 3 条 × 200
            LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(leaf.insns), 2000ll * 3ll); // lloop = 3 条 × 2000
            LIFT_CHECK(tail.insns > 0);
            // 覆盖边界：窗口在 tail 循环中途收口 ⇒ tail 的块条数应明显少于整个循环
            LIFT_CHECK(tail.insns < 500ll * 3ll);
        }
    }

    return finish("profile-window");
}
