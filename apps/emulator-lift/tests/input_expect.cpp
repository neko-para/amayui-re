// emulator-lift · **时间线的效果级判据**（`--expect-change-after` / `--expect-change-window`）—— pure 守卫
//
// ★ 判据（红了意味着什么）：`InputScript` 的时间线动作被**应用**之后，运行期必须在
//   `--expect-change-window` 个**采样**内观测到画面变化（`FrameSampler::changeCount()` 增长）；
//   到点没变 / 一次采样都没有 / 跑完仍未判定 ⇒ **非 0 结局**（`StopReason::InteractionFailed`），
//   `RunResult.detail` 写清是哪一种。⛔ 不许"发完不管却 exit 0"（BUG-16 就是这么漏掉的）。
// ★ 7 例（kebab-case，需求单拿 `<文件>#<用例名>` 当 verify 锚）：
//   ① `expect-change-satisfied`   变 ⇒ 不失败、`windows_satisfied == 1`、日志有 `[expect-change] ok`
//   ② `expect-change-not-changed` 不变（采样在增长）⇒ detail 含 `not-changed`（且非 ok）
//   ③ `expect-change-blind`       一次采样都不喂 ⇒ detail 含 `no-sample`，⛔ 不是 `not-changed`
//   ④ `expect-change-default-off` 策略关 ⇒ 同一份"没变化"夹具**不失败**（旧脚本行为不变）
//   ⑤ `expect-change-unclosed`    采样增长但没攒够窗口就结束 ⇒ detail 含 `unclosed`（⛔ 跑完不算过）
//   ⑥ `expect-change-policy-parse` 非法类别 / 空项 / 重复 / 窗口 0 / 与交互图同用 / 没脚本 /
//      **脚本里没有匹配动作** ⇒ 一律拒绝（⛔ 不静默退化、⛔ 不静默开局）
//   ⑦ `expect-change-no-action` 配置之后换了一份"没有匹配动作"的脚本 ⇒ 运行期也**不许静默绿**
//      （防御性：`instr :` 的 `expect=0` 只是一个读数，不是"通过"）
// ★ 夹具形状（照 `tests/profile_window.cpp` + `tests/interaction_graph.cpp`）：裸 `Machine` +
//   自造 guest 循环 + 一个"采样注入" shim。采样走 `FrameSampler::observeValues()`（生产路径
//   `observe()` 的被调者 —— 同一条计数路径）；**接线**单独断言
//   （`presenter().frameSampler() == &sampler()`），因为"采样器没接上"会让 `sampleCount` 恒 0、
//   而那种缺陷在 `observeValues` 夹具里看不出来（时间线通道原本根本没接线 —— 复审硬反证 §2）。
// ⛔ 不依赖游戏安装 / LFS / 真实窗口：虚拟时钟（100 ns / 条）⇒ 采样窗口是确定性推进的。
// ⛔ 不断言任何耗时 / 百分比 / 采样总数：断的是**关系**（失败类别、计数增长方向、接线）。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
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
constexpr std::uint32_t kScreenEa = 0x00402000u;  // guest 的"屏幕值"：一变 ⇒ 下一次采样 hash 变
constexpr std::uint32_t kStubEa = 0x7FFE0040u;
constexpr std::uint32_t kSentinel = 0xFFFFFFFFu;

// 极简汇编器：只编码本夹具要用的那几条（位移一律由 `ea()` 算，⛔ 不手填）。
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
    void jneRel8(std::uint8_t rel) {
        u8(0x75);
        u8(rel);
    }
    void jl(std::uint32_t target) {
        const std::int32_t rel = static_cast<std::int32_t>(target) - static_cast<std::int32_t>(ea() + 2);
        u8(0x7C);
        u8(static_cast<std::uint8_t>(rel & 0xFF));
    }
    // `mov dword ptr [imm32], imm32` = C7 05 disp32 imm32（10 字节；`jne` 的 rel8 用 10）
    void movMemImm32(std::uint32_t ea, std::uint32_t v) {
        u8(0xC7);
        u8(0x05);
        u32(ea);
        u32(v);
    }
    void call(std::uint32_t target) {
        const std::int32_t rel = static_cast<std::int32_t>(target) - static_cast<std::int32_t>(ea() + 5);
        u8(0xE8);
        u32(static_cast<std::uint32_t>(rel));
    }
    void ret() { u8(0xC3); }
};

// guest：`iterations` 轮循环，每轮调一次 sample shim（= 一个采样）；第 `flip_at` 轮把
// `[kScreenEa]` 写成 1 ⇒ 从那以后每次采样的整帧 hash 都变（= guest 画出了不同的画面）。
//   ★ `flip_at == 0` ⇒ **永远不翻**（"动作没生效"的夹具）。
Program buildProgram(std::uint32_t iterations, std::uint32_t flip_at) {
    Program p;
    p.movEcxImm(0);
    const std::uint32_t loop = p.ea();
    p.pushImm(0);
    p.call(kStubEa);
    p.incEcx();
    p.cmpEcxImm(flip_at);
    p.jneRel8(10);  // 跳到 `mov` 之后（mov 恰好 10 字节）
    p.movMemImm32(kScreenEa, 1);
    p.cmpEcxImm(iterations);
    p.jl(loop);
    p.ret();
    return p;
}

// ---- 采样注入（shim 里喂一次合成采样；⛔ 生产路径不这么做）----
lift::Machine* g_machine = nullptr;
bool g_feed = true;
std::uint64_t g_sample_index = 0;

void shimSample(lift::X86&, lift::ShimCall& call) {
    (void)call.u32(0);  // stdcall 的 1 个参数（值不用；由 `ShimRegistry::invoke` 弹参）
    if (g_feed && g_machine != nullptr) {
        const std::uint32_t screen = g_machine->mem().rd32(kScreenEa);
        ++g_sample_index;
        g_machine->sampler().observeValues(g_sample_index, 0x111ull + screen, {});
    }
    call.result = 0;
}

bool attach(lift::Machine& m) {
    m.shims().add("test", "sample", lift::Conv::Stdcall, 1, &shimSample);
    lift::ShimDef* def = m.shims().findMutable("test", "sample");
    if (def == nullptr) return false;
    def->stub_ea = kStubEa;
    m.cache().installShim(kStubEa, def->id);
    return true;
}

bool boot(lift::Machine& m, std::uint32_t iterations, std::uint32_t flip_at) {
    std::string err;
    if (!m.initBare(&err)) return false;
    if (!m.prepareStack(&err)) return false;
    const Program p = buildProgram(iterations, flip_at);
    if (!m.placeCode(kCodeEa, p.b.data(), p.b.size(), &err)) return false;
    const std::uint32_t zero = 0;
    if (!m.placeData(kScreenEa, &zero, sizeof(zero), &err)) return false;
    if (!attach(m)) return false;
    m.cpu().eip = kCodeEa;
    return true;
}

// ---- stdout + stderr 一起捕获（`[expect-change]` 的 ok / FAIL 都要断言"响亮"）----
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

bool contains(const std::string& s, const std::string& needle) {
    return s.find(needle) != std::string::npos;
}

struct Outcome {
    lift::RunResult r;
    std::string log;
    bool sampler_attached = false;
    bool expect_enabled = false;
    std::uint64_t opened = 0;
    std::uint64_t satisfied = 0;
    std::uint64_t failed = 0;
};

// 跑一次夹具：可选时间线脚本 + 可选策略（`policy_enabled=false` = 旧脚本行为）。
bool runFixture(lift::Machine& m, const char* script, bool policy_enabled, std::uint64_t window_samples,
                bool feed, Outcome* out) {
    g_machine = &m;
    g_feed = feed;
    g_sample_index = 0;
    if (script != nullptr) {
        std::string err;
        if (!m.loadInputScriptText(script, "inline:expect", &err)) {
            std::fprintf(stderr, "时间线解析失败：%s\n", err.c_str());
            return false;
        }
    }
    if (policy_enabled) {
        lift::ExpectChangePolicy policy;
        if (!lift::parseExpectChangeActions("click", &policy, nullptr)) return false;
        policy.window_samples = window_samples;
        std::string err;
        if (!m.configureExpectChange(policy, &err)) {
            std::fprintf(stderr, "策略配置失败：%s\n", err.c_str());
            return false;
        }
    }
    lift::RunOptions o;
    o.until_ea = kSentinel;
    // 兜底：⛔ 守卫不许挂死（虚拟时钟下这条正常不会触发）。
    o.time_limit_sec = 60.0;
    Capture cap;
    if (!cap.begin()) return false;
    out->r = m.run(o);
    // `instr :` 行（含 `expect=<n>` / `expect=off`）在运行摘要里 —— 捕获它才能断言是真值。
    m.dumpRunSummary(out->r, stdout);
    out->log = cap.finish();
    out->sampler_attached = m.presenter().frameSampler() == &m.sampler();
    out->expect_enabled = m.expectChangeEnabled();
    out->opened = m.expectChangeStats().windows_opened;
    out->satisfied = m.expectChangeStats().windows_satisfied;
    out->failed = m.expectChangeStats().windows_failed;
    return true;
}

// 一个 `click`：`hold 1 ms` = 显式 hold（guest 时间），不依赖 guest 的输入采样点。
//   ★ 用显式 hold 是**夹具口径**：不加它，自动 hold 要等 guest 采样（本夹具没有输入采样），
//     抬起会拖到 `--input-max-hold` 才发；判定窗在抬起前不判死（见 `evaluateExpectWindows`）。
const char* kClickScript = "at insn 20\nclick 100 200 hold 1 ms\n";

}  // namespace

int main() {
    using namespace lift_test;

    // =================================================================================
    // ① 动作后**变了** ⇒ 不失败（正例；否则"一直红"也能算"会红"）
    // =================================================================================
    {
        g_current_case = "expect-change-satisfied";
        lift::Machine m;
        // 60 轮循环；第 8 轮把屏幕值改成 1 ⇒ 之后的采样 hash 变（窗口在 insn 20 开，约第 3 个采样）。
        LIFT_CHECK(boot(m, 60u, 8u));
        Outcome o;
        LIFT_CHECK(runFixture(m, kClickScript, true, 8u, true, &o));
        LIFT_CHECK(o.r.ok());
        LIFT_CHECK(o.expect_enabled);
        // ★ 接线：没接上采样器时 `sampleCount()` 恒 0 ⇒ 这条会红（时间线通道原本就没接线）。
        LIFT_CHECK(o.sampler_attached);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(o.opened), 1);  // 只看这个 click
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(o.satisfied), 1);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(o.failed), 0);
        LIFT_CHECK(contains(o.log, "[expect-change] ok"));
        LIFT_CHECK(contains(o.log, "expect=1"));  // 运行摘要的 `instr :` 行
    }

    // =================================================================================
    // ② 动作后**没变**（采样在增长、窗口走满）=> not-changed（响亮失败）
    // =================================================================================
    {
        g_current_case = "expect-change-not-changed";
        lift::Machine m;
        // 3000 轮（≈21k 条指令 ⇒ guest 时钟 > 1 ms）⇒ `hold 1 ms` 的抬起一定会落地，
        // 窗口才开始判死；屏幕值永远不翻 ⇒ 一次变化都没有。
        LIFT_CHECK(boot(m, 3000u, 0u));
        Outcome o;
        LIFT_CHECK(runFixture(m, kClickScript, true, 3u, true, &o));
        LIFT_CHECK(!o.r.ok());
        LIFT_CHECK(o.r.reason == lift::StopReason::InteractionFailed);
        LIFT_CHECK(contains(o.r.detail, "expect-change"));
        LIFT_CHECK(contains(o.r.detail, "not-changed"));
        LIFT_CHECK(!contains(o.r.detail, "no-sample"));
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(o.failed), 1);
        LIFT_CHECK(contains(o.log, "[expect-change] FAIL"));
    }

    // =================================================================================
    // ③ **一次采样都没有**（识别源失明）=> no-sample（⛔ 不与"没变"混为一类）
    // =================================================================================
    {
        g_current_case = "expect-change-blind";
        lift::Machine m;
        LIFT_CHECK(boot(m, 60u, 0u));
        Outcome o;
        LIFT_CHECK(runFixture(m, kClickScript, true, 3u, /*feed=*/false, &o));
        LIFT_CHECK(!o.r.ok());
        LIFT_CHECK(o.r.reason == lift::StopReason::InteractionFailed);
        LIFT_CHECK(contains(o.r.detail, "no-sample"));
        LIFT_CHECK(!contains(o.r.detail, "not-changed"));
        LIFT_CHECK(contains(o.log, "[expect-change] FAIL"));
    }

    // =================================================================================
    // ④ 策略**关**：同一份"没变化"夹具 ⇒ 不失败（旧时间线的行为一个字都不变）
    // =================================================================================
    {
        g_current_case = "expect-change-default-off";
        lift::Machine m;
        LIFT_CHECK(boot(m, 3000u, 0u));
        Outcome o;
        LIFT_CHECK(runFixture(m, kClickScript, /*policy_enabled=*/false, 0u, true, &o));
        LIFT_CHECK(o.r.ok());
        LIFT_CHECK(!o.expect_enabled);
        // ★ 关着就**不采样**（零成本口径）：采样器不该被挂到 presenter 上。
        LIFT_CHECK(!o.sampler_attached);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(o.opened), 0);
        LIFT_CHECK(contains(o.log, "expect=off"));
    }

    // =================================================================================
    // ⑤ 跑完仍**未判定**（采样长过但没攒够窗口）=> unclosed（⛔ "跑完就算过"）
    // =================================================================================
    {
        g_current_case = "expect-change-unclosed";
        lift::Machine m;
        // 60 轮 ⇒ 窗口开后只攒到几十个采样，远不到 100；run 以哨兵正常结束。
        LIFT_CHECK(boot(m, 60u, 0u));
        Outcome o;
        LIFT_CHECK(runFixture(m, kClickScript, true, 100u, true, &o));
        LIFT_CHECK(!o.r.ok());
        LIFT_CHECK(o.r.reason == lift::StopReason::InteractionFailed);
        LIFT_CHECK(contains(o.r.detail, "unclosed"));
        LIFT_CHECK(!contains(o.r.detail, "no-sample"));
        LIFT_CHECK(contains(o.log, "[expect-change] FAIL"));
    }

    // =================================================================================
    // ⑥ 解析 / 配置**拒绝**（⛔ 不静默退化成"关"）
    // =================================================================================
    {
        g_current_case = "expect-change-policy-parse";
        lift::ExpectChangePolicy p;
        std::string err;
        LIFT_CHECK(!lift::parseExpectChangeActions("", &p, &err));
        LIFT_CHECK(!err.empty());
        LIFT_CHECK(!lift::parseExpectChangeActions("bogus", &p, &err));
        LIFT_CHECK(contains(err, "不认识"));
        LIFT_CHECK(!lift::parseExpectChangeActions("click,,key", &p, &err));
        LIFT_CHECK(contains(err, "空项"));
        LIFT_CHECK(!lift::parseExpectChangeActions("click,click", &p, &err));
        LIFT_CHECK(contains(err, "重复"));
        LIFT_CHECK(lift::parseExpectChangeActions("click,key,mouse", &p, &err));
        LIFT_CHECK(p.enabled && p.click && p.key && p.mouse);
        LIFT_CHECK(p.text == "click,key,mouse");
        LIFT_CHECK(lift::parseExpectChangeActions(" CLICK , Mouse ", &p, &err));
        LIFT_CHECK(p.click && p.mouse && !p.key);
        LIFT_CHECK(p.text == "click,mouse");
        // 窗口 0 ⇒ 配置期拒绝。
        {
            lift::Machine m;
            LIFT_CHECK(lift::parseExpectChangeActions("click", &p, &err));
            p.window_samples = 0;
            LIFT_CHECK(!m.configureExpectChange(p, &err));
            LIFT_CHECK(!err.empty());
        }
        // 与交互图同用 ⇒ 拒绝（图有自己的 `expect` 语义，两套判据同时开 = 两套口径）。
        {
            lift::Machine m;
            std::string gerr;
            LIFT_CHECK(m.loadInteractionGraphText(
                "entry a\nnode a\n  when always\n  action quit\n  next\n", "inline:graph", &gerr));
            LIFT_CHECK(lift::parseExpectChangeActions("click", &p, &err));
            p.window_samples = 3;
            LIFT_CHECK(!m.configureExpectChange(p, &err));
            LIFT_CHECK(contains(err, "interaction"));
        }
        // 没脚本 ⇒ 拒绝（策略要看时间线动作；⛔ 不许"配置完却什么都不看"）。
        {
            lift::Machine m;
            LIFT_CHECK(lift::parseExpectChangeActions("click", &p, &err));
            p.window_samples = 3;
            LIFT_CHECK(!m.configureExpectChange(p, &err));
            LIFT_CHECK(contains(err, "loadInputScript"));
        }
        // 脚本里**没有匹配的动作** ⇒ 拒绝（这是"零观测窗静默绿"的正规拦截点）。
        {
            lift::Machine m;
            std::string lerr;
            LIFT_CHECK(m.loadInputScriptText("at insn 10\nmouse_move 1 2\n", "inline:noclick", &lerr));
            LIFT_CHECK(lift::parseExpectChangeActions("click", &p, &err));
            p.window_samples = 3;
            LIFT_CHECK(!m.configureExpectChange(p, &err));
            LIFT_CHECK(contains(err, "没有匹配的动作"));
            // 同类脚本换个类别（mouse_move 也不在 key 集合里）⇒ 一样拒绝。
            LIFT_CHECK(lift::parseExpectChangeActions("key", &p, &err));
            p.window_samples = 3;
            LIFT_CHECK(!m.configureExpectChange(p, &err));
        }
    }

    // =================================================================================
    // ⑦ 配置之后换了脚本（没有匹配动作）⇒ 运行期**也不许静默绿**（防御性判死，detail 是 no-action）
    // =================================================================================
    {
        g_current_case = "expect-change-no-action";
        lift::Machine m;
        LIFT_CHECK(boot(m, 60u, 0u));
        std::string err;
        LIFT_CHECK(m.loadInputScriptText("at insn 20\nclick 100 200\n", "inline:policy", &err));
        lift::ExpectChangePolicy p;
        LIFT_CHECK(lift::parseExpectChangeActions("click", &p, &err));
        p.window_samples = 3;
        LIFT_CHECK(m.configureExpectChange(p, &err));
        // 换脚本是 API 允许的；换完之后一个观测窗都开不了 ⇒ run 尾部必须判死（⛔ 不是 exit 0）。
        LIFT_CHECK(m.loadInputScriptText("at insn 20\nmouse_move 1 2\n", "inline:replaced", &err));
        Outcome o;
        LIFT_CHECK(runFixture(m, nullptr, /*policy_enabled=*/false, 0u, true, &o));
        LIFT_CHECK(!o.r.ok());
        LIFT_CHECK(o.r.reason == lift::StopReason::InteractionFailed);
        LIFT_CHECK(contains(o.r.detail, "no-action"));
        LIFT_CHECK(contains(o.log, "[expect-change] FAIL"));
    }

    return finish("input-expect");
}
