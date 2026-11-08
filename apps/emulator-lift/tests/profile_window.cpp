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
//
// ★ 还钉了触发条件的两条口径：
//   - `at insn N` 能当窗口边界（**帧内也能切**）⇒ 同一脚本重复跑边界相同（断言 insns 范围）。
//   - 帧停滞兜底：帧号不推进时 `at frame` 不会永远不触发（`--input-stall-ms` 的语义）。
//
// ⛔ 不钉任何墙钟数字（ms 随机器变），只钉**关系**与**边界**。
// ⛔ 不依赖游戏 / LFS：自造机器码 + 一个宿主 shim，纯 pure。
#include <cstdint>
#include <cstdio>
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

    return finish("profile-window");
}
