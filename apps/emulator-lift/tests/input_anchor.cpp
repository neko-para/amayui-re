// emulator-lift · 时间线的 **0 点**（`--anchor-script <id>`）—— pure 守卫
//
// ★ 判据（红了意味着什么）：`--anchor-script <id>` 把时间线的 0 点定在"**首次观察到进入**引擎脚本
//   `<id>`"的那一个可观察样本上 —— 四个单位（frame / insn / guest_ms / wall_ms）全部重定基；
//   闩锁**只发生一次**（重进不定基）；闩锁前**抑制**全部数值触发；闩锁 pending 期间**禁用**帧停滞
//   折算；`id 0` 是**合法目标**（⛔ 没有"关闭"哨兵）；跑完仍未闩锁 ⇒ 收尾**响亮失败**
//   （`InteractionFailed` + `origin-missed:` detail + 退出码 5 的分流）；
//   与 `--interaction-graph` 同给 ⇒ **解析期报错**。
//
// ★ 用例（kebab-case；需求单拿 `<文件>#<用例名>` 当 verify 锚）：
//   ① `anchor/fires-when-already-inside`          开局就在目标脚本 ⇒ 首个样本即闩锁（0 点 = 首个样本）
//   ② `anchor/latch-after-other-script`           先经过别的脚本 ⇒ 0 点落在进入那条 insn 边界；之前全部抑制
//   ③ `anchor/latch-once`                         A→X→B→X：第二次进入 **不**重新定基
//   ④ `anchor/id-zero-is-a-target`                `id 0` 能当目标（哨兵不存在）
//   ⑤ `anchor/missed-is-loud`                     未闩锁就结束 ⇒ `origin-missed:` + `InteractionFailed` + 非 0
//   ⑥ `anchor/rejects-interaction-graph`          与 `--interaction-graph` 同给 ⇒ 解析期报错（+ 空脚本 exit 2）
//   ⑦ `anchor/no-stall-fallback-while-pending`    闩锁 pending 期间帧停滞折算**不**发生（`!stallFallbackFired()`）
//   ⑧ `anchor/rebase-all-four-units`              四个单位各一条重定基断言（frame / insn / guest_ms / wall_ms）
//   ⑨ `anchor/samples-without-marker-action`      真 `Machine::run`："只有 anchor、没有 marker 动作"也采样（L6 门控）
//   ⑩ `anchor/passing-other-scripts-is-not-a-failure`  真 `Machine::run`："进了但 id 是别的"**不算失败**
//   ⑪ `anchor/summary-shows-origin`            真 `Machine::run` 后 dump summary：命中**有** `origin` 行、未命中**没有**
//   ⑫ `anchor/absolute-segment-fires-before-latch`    `before-anchor` 段的动作在闩锁**之前**照常应用（修前被抑制 ⇒ 红）
//   ⑬ `anchor/absolute-segment-not-rebased`     同一脚本两段各四条：绝对段的四个单位**不被** `+= origin`，相对段的**被**
//   ⑭ `anchor/rejects-before-anchor-without-after`  解析期错误：`before-anchor` 缺配对的 `after-anchor`（指回标记行）
//   ⑮ `anchor/rejects-duplicate-before-anchor`  解析期错误：重复 `before-anchor`（带行号）
//   ⑯ `anchor/rejects-after-anchor-without-before`  解析期错误：`after-anchor` 在 `before-anchor` 之前
//   ⑰ `anchor/rejects-segment-without-anchor`   解析期错误：没开 `--anchor-script` 却写段标记
//   ⑱ `anchor/rejects-profile-in-before-segment` 解析期错误：绝对段里出现 `profile_begin` / `profile_end`
//   ⑲ `anchor/absolute-frame-and-insn`          绝对段的 `at frame N` 与 `at insn N` 各一条 ⇒ 两个单位都走绝对路径
// ★ 夹具形状：一半是纯 `InputScript`（直接给四个单位，⛔ 不靠真实墙钟推进）；
//   另一半照 `tests/input_marker.cpp` 的裸 `Machine` + 假 Engine（`commitZeroed` 一块内存 +
//   `wr32(0x55E1BC, base)`），自造字节摆在 VM 循环头 `0x4123F0`。⛔ 不依赖游戏安装 / LFS / 真实窗口。
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
#include "runtime/engine_marker.h"
#include "runtime/input_script.h"
#include "runtime/machine.h"
#include "runtime/memory.h"

namespace {

// 假 Engine 落在 [0x00480000, 0x00580000)：覆盖 base+0x5D880（cur）/ base+0x5D894+0x78*cur（记录）
// 以及 Engine 单例全局 0x0055E1BC（同一块里 ⇒ 不用第二次 commit）。与 `input_marker.cpp` 同形。
constexpr std::uint32_t kBase = 0x00480000u;
constexpr std::uint32_t kBaseSize = 0x00100000u;
constexpr std::uint32_t kEngineGlobal = 0x0055E1BCu;
constexpr std::uint32_t kCodeBase = 0x004C0000u;  // 记录 +0x00 指向的"代码区基址"（必须已提交）
constexpr std::uint32_t kLoopHeadEa = lift::kMarkerLoopHeadEa;
constexpr std::uint32_t kCurEa = kBase + lift::kEngineCurOffset;

void put32(lift::GuestMemory& mem, std::uint32_t ea, std::uint32_t v) {
    LIFT_CHECK(mem.committed(ea, 4u));
    mem.wr32(ea, v);
}

bool boot(lift::Machine& m) {
    std::string err;
    if (!m.initBare(&err)) {
        std::fprintf(stderr, "initBare 失败：%s\n", err.c_str());
        return false;
    }
    if (!m.mem().commitZeroed(kBase, kBaseSize, lift::RegionKind::Heap, "fake-engine")) {
        std::fprintf(stderr, "commit 失败：%s\n", m.mem().lastError());
        return false;
    }
    put32(m.mem(), kEngineGlobal, kBase);  // 发布单例（0x4BAA3A 的同形）
    return true;
}

// 帧记录：`+0x00` = 代码区基址（形状有效），`+0x3C` = 统一文件 id。
void setRecord(lift::Machine& m, std::uint32_t cur, std::uint32_t id) {
    const std::uint32_t rec = kBase + lift::kEngineFrameBase + lift::kFrameStride * cur;
    put32(m.mem(), rec, kCodeBase);
    put32(m.mem(), rec + lift::kFrameIdentityOffset, id);
}

void setIdentity(lift::Machine& m, std::uint32_t cur, std::uint32_t id) {
    put32(m.mem(), kCurEa, cur);
    setRecord(m, cur, id);
}

bool loadScript(lift::Machine& m, const char* text) {
    std::string err;
    const bool ok = m.loadInputScriptText(text, "inline:anchor", &err);
    if (!ok) std::fprintf(stderr, "脚本解析失败：%s\n", err.c_str());
    return ok;
}

// 原地空转（`jmp 0x4123F0`）：身份**永不变化**，只用来看"门控有没有放行采样"与"没闩锁 ⇒ 响亮失败"。
std::vector<std::uint8_t> spinAtLoopHead() { return {0xEB, 0xFE}; }

// `mov dword ptr [0x004DD880], 1` + `jmp 0x4123F0`：首个样本 cur=0（id=1），之后 cur=1（id=4）
//   ⇒ 覆盖"先经过别的脚本、再进入目标"的端到端形状（⛔ 不是只在纯夹具里模拟）。
std::vector<std::uint8_t> setCurOneAndLoop() {
    return {0xC7, 0x05, 0x80, 0xD8, 0x4D, 0x00, 0x01, 0x00, 0x00, 0x00,  // mov dword [0x004DD880], 1
            0xEB, 0xF4};                                                 // jmp 0x4123F0（0x4123FC − 12）
}

// ---- stdout + stderr 一起捕获（"响亮失败"要断言"真的打出来了"）----
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

bool contains(const std::string& s, const std::string& needle) { return s.find(needle) != std::string::npos; }

// `Machine::dumpRunSummary` 的**整段文本**（run summary 是"跑完的输出"，不是内部状态）。
//   ★ 用 `Capture` 重定向 fd 而不是 `open_memstream`（后者在 clang-cl / MSVC 上没有）。
std::string summaryText(const lift::Machine& m, const lift::RunResult& r) {
    Capture cap;
    if (!cap.begin()) return {};
    m.dumpRunSummary(r, stdout);
    return cap.finish();
}

// 一个"有效身份"样本（纯夹具直接构造，绕过 GuestMemory）。
lift::MarkerSample sample(std::uint32_t cur, std::uint32_t id, const char* name = "X.BIN") {
    lift::MarkerSample s;
    s.valid = true;
    s.cur = cur;
    s.id = id;
    s.name = name;
    return s;
}

}  // namespace

int main() {
    using namespace lift_test;

    // =================================================================================
    // ① 开局就在目标脚本 ⇒ **首个样本即闩锁**（0 点 = 首个样本；marker 的"首样本只播种"不适用）
    //    ★ 这条就是 anchor 与 marker 的分界线：把闩锁放到 `marker_seeded_` 之后 ⇒ 本用例红。
    // =================================================================================
    {
        g_current_case = "anchor/fires-when-already-inside";
        lift::InputScript s;
        lift::WindowManager wm;
        std::string err;
        s.setOriginScript(7);
        LIFT_CHECK(s.originRequested());
        LIFT_CHECK(!s.originSet());
        LIFT_CHECK(s.loadText("at frame 1\nquit\n", "inline:anchor", &err));
        LIFT_CHECK(!s.hasMarkerActions());        // ★ 脚本里一条 marker 动作都没有
        LIFT_CHECK(s.needsIdentitySamples());     // ★ 门控：只有 anchor 也必须采样（L6）
        // ★ 首个样本 ⇒ 当场闩锁（marker 的"首样本只播种"在这里不适用；`noteMarker` 的返回值是
        //   "有没有 marker **边沿**"，与 0 点无关 ⇒ 判据只看 `originSet()` / `origin()`）。
        s.noteMarker(wm, sample(0, 7, "TARGET.BIN"), 10, 100, 1234, 555);
        LIFT_CHECK(s.originSet());
        LIFT_CHECK(s.originLatches() == 1);
        LIFT_CHECK(s.origin().id == 7);
        LIFT_CHECK(s.origin().frame == 10);
        LIFT_CHECK(s.origin().guest_ms == 100);
        LIFT_CHECK(s.origin().insns == 1234);
        LIFT_CHECK(s.origin().wall_ms == 555);
        LIFT_CHECK(s.actions()[0].trigger.kind == lift::TriggerKind::Frame);
        LIFT_CHECK(s.actions()[0].trigger.value == 11);  // `at frame 1` + origin.frame(10)
        LIFT_CHECK(!s.tick(wm, 10, 100, 1234, 555));     // 0 点那一帧还不应用
        LIFT_CHECK(s.appliedCount() == 0);
        LIFT_CHECK(s.tick(wm, 11, 140, 1400, 560));      // 下一帧 ⇒ 应用
        LIFT_CHECK(s.appliedCount() == 1);
        LIFT_CHECK(s.quitRequested());
    }

    // =================================================================================
    // ② 先经过别的脚本再进入：0 点落在**进入的那一条 insn 边界**上，且**之前**的数值触发被抑制
    //    （帧早已越过 `at frame 1` 也一条都不应用）
    // =================================================================================
    {
        g_current_case = "anchor/latch-after-other-script";
        lift::InputScript s;
        lift::WindowManager wm;
        std::string err;
        s.setOriginScript(2);
        LIFT_CHECK(s.loadText("at frame 1\nquit\n", "inline:anchor", &err));
        s.noteMarker(wm, sample(0, 1, "INIT.BIN"), 99, 900, 5000, 400);  // 不是目标 ⇒ 不闩锁
        LIFT_CHECK(!s.originSet());
        LIFT_CHECK(!s.tick(wm, 99, 900, 5000, 400));
        LIFT_CHECK(s.appliedCount() == 0);
        LIFT_CHECK(!s.tick(wm, 500, 5000, 90000, 3000));
        LIFT_CHECK(s.appliedCount() == 0);
        LIFT_CHECK(!s.quitRequested());
        // ★ 目标出现 ⇒ 闩锁（`noteMarker` 的返回值是"有没有 marker **边沿**"，与 0 点无关）
        s.noteMarker(wm, sample(1, 2, "TARGET.BIN"), 120, 1200, 20000, 1500);
        LIFT_CHECK(s.originSet());
        LIFT_CHECK(s.origin().id == 2);
        LIFT_CHECK(s.origin().frame == 120);
        LIFT_CHECK(s.origin().insns == 20000);  // ★ 0 点 = 进入那一条 insn 边界
        LIFT_CHECK(s.origin().guest_ms == 1200);
        LIFT_CHECK(s.originLatches() == 1);
        LIFT_CHECK(s.actions()[0].trigger.value == 121);
        LIFT_CHECK(!s.tick(wm, 120, 1200, 20000, 1500));
        LIFT_CHECK(s.appliedCount() == 0);
        LIFT_CHECK(s.tick(wm, 121, 1210, 20100, 1510));
        LIFT_CHECK(s.appliedCount() == 1);
        LIFT_CHECK(s.quitRequested());
    }

    // =================================================================================
    // ③ 重进**不定基**：A→X→B→X，第二次进入 X 只记 info（`originReentries`），0 点不动
    // =================================================================================
    {
        g_current_case = "anchor/latch-once";
        lift::InputScript s;
        lift::WindowManager wm;
        std::string err;
        s.setOriginScript(2);
        LIFT_CHECK(s.loadText("at frame 3\nquit\n", "inline:anchor", &err));
        s.noteMarker(wm, sample(0, 1, "A.BIN"), 10, 100, 1000, 100);
        s.noteMarker(wm, sample(1, 2, "X.BIN"), 20, 200, 1000, 200);  // 闩锁
        LIFT_CHECK(s.origin().frame == 20);
        LIFT_CHECK(s.origin().insns == 1000);
        LIFT_CHECK(s.actions()[0].trigger.value == 23);
        s.noteMarker(wm, sample(0, 1, "A.BIN"), 30, 300, 3000, 300);
        s.noteMarker(wm, sample(1, 2, "X.BIN"), 40, 400, 4000, 400);  // 重进 X
        LIFT_CHECK(s.originLatches() == 1);      // ★ 只定一次
        LIFT_CHECK(s.origin().frame == 20);      // ★ 0 点没被推到 40
        LIFT_CHECK(s.origin().insns == 1000);
        LIFT_CHECK(s.actions()[0].trigger.value == 23);  // ⛔ 不是 43
        LIFT_CHECK(s.originReentries() == 1);
    }

    // =================================================================================
    // ④ `id 0` 是**合法目标**（根脚本）：没有 `0 = 关闭` 的哨兵
    // =================================================================================
    {
        g_current_case = "anchor/id-zero-is-a-target";
        lift::InputScript s;
        lift::WindowManager wm;
        std::string err;
        s.setOriginScript(0);  // ⛔ 不是"关闭"
        LIFT_CHECK(s.originRequested());
        LIFT_CHECK(s.loadText("at insn 10\nquit\n", "inline:anchor", &err));
        s.noteMarker(wm, sample(0, 0, "ROOT.BIN"), 3, 30, 300, 30);
        LIFT_CHECK(s.originSet());
        LIFT_CHECK(s.origin().id == 0);
        LIFT_CHECK(s.originLatches() == 1);
        LIFT_CHECK(s.actions()[0].trigger.kind == lift::TriggerKind::Insn);
        LIFT_CHECK(s.actions()[0].trigger.value == 310);
        LIFT_CHECK(!s.tick(wm, 3, 30, 309, 31));
        LIFT_CHECK(s.appliedCount() == 0);
        LIFT_CHECK(s.tick(wm, 3, 30, 310, 32));
        LIFT_CHECK(s.appliedCount() == 1);
    }

    // =================================================================================
    // ⑤ 未闩锁就结束 ⇒ **响亮失败**：`origin-missed:` + `InteractionFailed` + 非 0 退出码分流
    //    （"进了但 id 是别的"不算失败 —— 那条在 ②/⑩ 钉着）
    // =================================================================================
    {
        g_current_case = "anchor/missed-is-loud";
        lift::Machine m;
        LIFT_CHECK(boot(m));
        setIdentity(m, 0, 7);  // 当前 id = 7；目标 99 **永不出现**
        std::string err;
        const std::vector<std::uint8_t> code = spinAtLoopHead();
        LIFT_CHECK(m.placeCode(kLoopHeadEa, code.data(), code.size(), &err));
        LIFT_CHECK(loadScript(m, "at insn 200\nquit\n"));  // 只有 anchor 能给出 0 点
        m.inputScript().setOriginScript(99);
        lift::RunOptions o;
        o.max_insns = 2000000ull;  // 兜底：0 点坏了也不会挂死（只是查不出闩锁）
        o.time_limit_sec = 10.0;
        std::string log;
        lift::RunResult r;
        {
            Capture cap;
            LIFT_CHECK(cap.begin());
            r = m.run(o);
            log = cap.finish();
        }
        LIFT_CHECK(r.reason == lift::StopReason::InteractionFailed);
        LIFT_CHECK(r.origin_missed);
        LIFT_CHECK(!r.ok());
        LIFT_CHECK(lift::exitCodeForRun(r) != 0);  // main 走 5（这里钉"非 0"这条分流）
        LIFT_CHECK(r.detail.rfind("origin-missed:", 0) == 0);
        LIFT_CHECK(contains(r.detail, "0x00000063"));          // 目标 id 打出来了
        LIFT_CHECK(contains(r.detail, "没**观察到**进入脚本"));  // ★ 措辞：不是"没进入"
        LIFT_CHECK(contains(r.detail, "last="));
        LIFT_CHECK(contains(log, "[anchor] FAIL origin-missed:"));
        LIFT_CHECK(!m.inputScript().originSet());
        // 采样点确实在工作（不是"没接采样器"被说成"没进入脚本"）
        LIFT_CHECK(m.inputScript().markerSamples() >= 1);
        LIFT_CHECK(m.markerSampler().published() >= 1);
    }

    // =================================================================================
    // ⑥ 互斥 / 用法：`--anchor-script` + `--interaction-graph` ⇒ **解析期报错**；
    //    给了 anchor 而脚本一个动作都没有 ⇒ 用法错误（exit 2 的那条路径）
    //    ★ 判据的**唯一实现**是 `validateOriginCli` / `validateOriginScript`（`main` 也用它 exit 2）。
    // =================================================================================
    {
        g_current_case = "anchor/rejects-interaction-graph";
        const std::string both = lift::validateOriginCli(true, true, true);
        LIFT_CHECK(!both.empty());
        LIFT_CHECK(contains(both, "互斥"));
        LIFT_CHECK(contains(both, "interaction-graph"));
        const std::string no_script = lift::validateOriginCli(true, false, false);
        LIFT_CHECK(!no_script.empty());
        LIFT_CHECK(contains(no_script, "--input-script"));
        LIFT_CHECK(lift::validateOriginCli(true, true, false).empty());   // 只有时间线 ⇒ 通过
        LIFT_CHECK(lift::validateOriginCli(false, false, false).empty());  // 没给 anchor ⇒ 通过
        // "给了 anchor 但脚本里一个触发都没有" ⇒ 用法错误
        LIFT_CHECK(!lift::validateOriginScript(true, 0).empty());
        LIFT_CHECK(lift::validateOriginScript(true, 1).empty());
        LIFT_CHECK(lift::validateOriginScript(false, 0).empty());
        // 空脚本本身由解析器拒绝（main 在解析失败那条路上 exit 2）
        lift::InputScript s;
        std::string err;
        LIFT_CHECK(!s.loadText("# 只有注释，一个动作都没有\n", "inline:anchor", &err));
        LIFT_CHECK(contains(err, "没有任何动作"));
    }

    // =================================================================================
    // ⑦ 闩锁 **pending** 期间帧停滞折算**不**发生（裁决 11）
    //    ★ 折算一次就把 `at frame N` 永久变成墙钟条件 ⇒ 0 点还没定就已经是假的。
    //    同时钉住"闩锁之后兜底**恢复**"（否则帧真停了会挂死）。
    // =================================================================================
    {
        g_current_case = "anchor/no-stall-fallback-while-pending";
        lift::InputScript s;
        lift::WindowManager wm;
        std::string err;
        s.setDefaultStallMs(1000);
        s.setDefaultBootGraceMs(0);
        s.setOriginScript(5);
        LIFT_CHECK(s.loadText("at frame 100\nquit\n", "inline:anchor", &err));
        s.noteMarker(wm, sample(0, 1, "INIT.BIN"), 3, 100, 700, 100);  // 不是目标 ⇒ 不闩锁
        LIFT_CHECK(!s.originSet());
        LIFT_CHECK(!s.tick(wm, 3, 100, 700, 100));   // 第一帧样本（墙钟基线）
        LIFT_CHECK(!s.tick(wm, 3, 200, 900, 2000));  // 帧号停住 2 s ⇒ 平时早就折算了
        LIFT_CHECK(!s.stallFallbackFired());         // ★ pending ⇒ 不折算
        LIFT_CHECK(s.actions()[0].trigger.kind == lift::TriggerKind::Frame);  // ⛔ 没被折成 WallMs
        LIFT_CHECK(s.actions()[0].trigger.value == 100);
        // 闩锁：0 点定在 frame 3 ⇒ `at frame 100` 变成 103
        s.noteMarker(wm, sample(1, 5, "TARGET.BIN"), 3, 300, 1100, 3000);
        LIFT_CHECK(s.originSet());
        LIFT_CHECK(s.actions()[0].trigger.value == 103);
        LIFT_CHECK(!s.stallFallbackFired());
        LIFT_CHECK(s.tick(wm, 3, 400, 1300, 4000));  // 帧仍停在 3、墙钟 +4 s ⇒ 兜底**恢复**
        LIFT_CHECK(s.stallFallbackFired());
        LIFT_CHECK(s.actions()[0].trigger.kind == lift::TriggerKind::WallMs);  // 折算是**一次**的
    }

    // =================================================================================
    // ⑧ 0 点作用于**全部四个单位**：frame / insn / guest_ms / wall_ms 各一条重定基断言
    //    （值 + 行为各一遍：差 1 不应用、够了才应用）
    // =================================================================================
    {
        g_current_case = "anchor/rebase-all-four-units";
        lift::InputScript s;
        lift::WindowManager wm;
        std::string err;
        s.setOriginScript(9);
        LIFT_CHECK(s.loadText(
            "at frame 10\nwait 0 frames\n"
            "at insn 1000\nwait 0 insns\n"
            "at guest_ms 500\nwait 0 ms\n"
            "at wall_ms 200\nwait 0 wall_ms\n"
            "at frame 999999\nquit\n",
            "inline:anchor", &err));
        LIFT_CHECK(s.actionCount() == 5);
        s.noteMarker(wm, sample(0, 1, "INIT.BIN"), 5, 300, 700, 100);  // 不是目标 ⇒ 不闩锁
        LIFT_CHECK(!s.originSet());
        // pending：四个单位一条都不应用（哪怕早就越过）
        LIFT_CHECK(!s.tick(wm, 1000, 5000, 100000, 5000));
        LIFT_CHECK(s.appliedCount() == 0);
        s.noteMarker(wm, sample(1, 9, "TARGET.BIN"), 7, 300, 777, 100);  // 闩锁
        LIFT_CHECK(s.origin().frame == 7);
        LIFT_CHECK(s.origin().guest_ms == 300);
        LIFT_CHECK(s.origin().insns == 777);
        LIFT_CHECK(s.origin().wall_ms == 100);
        LIFT_CHECK(s.actions()[0].trigger.kind == lift::TriggerKind::Frame);
        LIFT_CHECK(s.actions()[0].trigger.value == 17);    // 10 + 7
        LIFT_CHECK(s.actions()[1].trigger.kind == lift::TriggerKind::Insn);
        LIFT_CHECK(s.actions()[1].trigger.value == 1777);  // 1000 + 777
        LIFT_CHECK(s.actions()[2].trigger.kind == lift::TriggerKind::GuestMs);
        LIFT_CHECK(s.actions()[2].trigger.value == 800);   // 500 + 300
        LIFT_CHECK(s.actions()[3].trigger.kind == lift::TriggerKind::WallMs);
        LIFT_CHECK(s.actions()[3].trigger.value == 300);   // 200 + 100
        LIFT_CHECK(s.actions()[4].trigger.value == 999999 + 7);
        LIFT_CHECK(!s.tick(wm, 16, 799, 1776, 299));  // 四个单位各差 1 ⇒ 一条都不应用
        LIFT_CHECK(s.appliedCount() == 0);
        LIFT_CHECK(s.tick(wm, 17, 800, 1777, 300));   // 四个单位同时到达 ⇒ 四条 wait 全应用
        LIFT_CHECK(s.appliedCount() == 4);
        LIFT_CHECK(!s.quitRequested());
    }

    // =================================================================================
    // ⑨ 真 `Machine::run`：**只有 anchor、没有 marker 动作**也必须采样（L6 门控）
    //    ★ 门控退回 `hasMarkerActions()` ⇒ 一次也不采样 ⇒ 永不闩锁 ⇒ 本用例红（收尾报 origin-missed）。
    // =================================================================================
    {
        g_current_case = "anchor/samples-without-marker-action";
        lift::Machine m;
        LIFT_CHECK(boot(m));
        setIdentity(m, 0, 4);
        std::string err;
        const std::vector<std::uint8_t> code = spinAtLoopHead();
        LIFT_CHECK(m.placeCode(kLoopHeadEa, code.data(), code.size(), &err));
        LIFT_CHECK(loadScript(m, "at insn 400\nquit\n"));
        LIFT_CHECK(!m.inputScript().hasMarkerActions());  // ★ 只有 anchor，没有 marker 动作
        m.inputScript().setOriginScript(4);
        lift::RunOptions o;
        o.max_insns = 2000000ull;
        o.time_limit_sec = 10.0;
        const lift::RunResult r = m.run(o);
        LIFT_CHECK(r.reason == lift::StopReason::Stopped);
        LIFT_CHECK(r.detail == "input script: quit");
        LIFT_CHECK(!r.origin_missed);
        LIFT_CHECK(r.ok());
        LIFT_CHECK(m.inputScript().originSet());
        LIFT_CHECK(m.inputScript().origin().id == 4);
        LIFT_CHECK(m.inputScript().originLatches() == 1);
        LIFT_CHECK(m.markerSampler().published() >= 1);  // ★ 采样真的发生了
        LIFT_CHECK(m.inputScript().markerSamples() >= 1);
        // ★ 配置跨 `clear()` 存活、状态随脚本清空（K8：两者混在同一个重置点里一定会有一边错）。
        //   配置被清 ⇒ anchor 静默失效（一条失败都不报）—— 那正是"静默失败"，必须守。
        LIFT_CHECK(loadScript(m, "at insn 10\nquit\n"));  // `loadText` 内部会 `clear()`
        LIFT_CHECK(m.inputScript().originRequested());
        LIFT_CHECK(m.inputScript().originScriptId() == 4);
        LIFT_CHECK(!m.inputScript().originSet());
    }

    // =================================================================================
    // ⑩ 真 `Machine::run`：**"进了但 id 是别的"不算失败**（路过 INIT 再进目标 ⇒ 正常收口）
    //    ★ 若把"每到不是 X 就报错"写进来 ⇒ 本用例红。
    // =================================================================================
    {
        g_current_case = "anchor/passing-other-scripts-is-not-a-failure";
        lift::Machine m;
        LIFT_CHECK(boot(m));
        setRecord(m, 0, 1);  // cur=0 ⇒ id=1（第一个样本，不是目标）
        setRecord(m, 1, 4);  // cur=1 ⇒ id=4（目标）
        put32(m.mem(), kCurEa, 0);
        std::string err;
        const std::vector<std::uint8_t> code = setCurOneAndLoop();
        LIFT_CHECK(m.placeCode(kLoopHeadEa, code.data(), code.size(), &err));
        LIFT_CHECK(loadScript(m, "at insn 5000\nquit\n"));
        m.inputScript().setOriginScript(4);
        lift::RunOptions o;
        o.max_insns = 2000000ull;
        o.time_limit_sec = 10.0;
        const lift::RunResult r = m.run(o);
        LIFT_CHECK(r.reason == lift::StopReason::Stopped);
        LIFT_CHECK(!r.origin_missed);
        LIFT_CHECK(r.ok());
        LIFT_CHECK(m.inputScript().originSet());
        LIFT_CHECK(m.inputScript().origin().id == 4);
        LIFT_CHECK(m.inputScript().originLatches() == 1);
        LIFT_CHECK(m.markerSampler().published() >= 2);  // 先看到 id=1，再看到 id=4
    }

    // =================================================================================
    // ⑪ 摘要可见性：闩锁命中 ⇒ run summary 里**有** `origin    :` 行；未命中 ⇒ **没有**
    //    ★ 缺口（本单最后一轮）：纯 anchor 运行没有 `marker :` 行（它只按 `hasMarkerActions()`
    //      打），采样账只在失败 detail 里 ⇒ "0 点定在哪"在跑完的输出里看不到。
    //    ★ 破坏方式：删掉 `dumpRunSummary` 里那几行 ⇒ 命中那半边红；改成无条件打 ⇒ 未命中那半边红。
    // =================================================================================
    {
        g_current_case = "anchor/summary-shows-origin";
        // 命中：目标 id = 4（真 run 后 dump summary）
        {
            lift::Machine m;
            LIFT_CHECK(boot(m));
            setIdentity(m, 0, 4);
            std::string err;
            const std::vector<std::uint8_t> code = spinAtLoopHead();
            LIFT_CHECK(m.placeCode(kLoopHeadEa, code.data(), code.size(), &err));
            LIFT_CHECK(loadScript(m, "at insn 400\nquit\n"));
            m.inputScript().setOriginScript(4);
            lift::RunOptions o;
            o.max_insns = 2000000ull;
            o.time_limit_sec = 10.0;
            const lift::RunResult r = m.run(o);
            LIFT_CHECK(r.ok());
            LIFT_CHECK(m.inputScript().originSet());
            const std::string sum = summaryText(m, r);
            LIFT_CHECK(contains(sum, "origin    : script=0x00000004 frame="));
            LIFT_CHECK(contains(sum, " insns="));
            LIFT_CHECK(contains(sum, " guest_ms="));
            LIFT_CHECK(contains(sum, " wall_ms="));
        }
        // 未命中：目标 id = 99 ⇒ 失败；摘要里**没有** origin 行（失败只在 detail 里说）
        {
            lift::Machine m;
            LIFT_CHECK(boot(m));
            setIdentity(m, 0, 7);
            std::string err;
            const std::vector<std::uint8_t> code = spinAtLoopHead();
            LIFT_CHECK(m.placeCode(kLoopHeadEa, code.data(), code.size(), &err));
            LIFT_CHECK(loadScript(m, "at insn 200\nquit\n"));
            m.inputScript().setOriginScript(99);
            lift::RunOptions o;
            o.max_insns = 2000000ull;
            o.time_limit_sec = 10.0;
            const lift::RunResult r = m.run(o);
            LIFT_CHECK(r.origin_missed);
            LIFT_CHECK(!m.inputScript().originSet());
            const std::string sum = summaryText(m, r);
            LIFT_CHECK(!contains(sum, "origin    :"));
            LIFT_CHECK(contains(r.detail, "origin-missed:"));  // 失败形态本身不变
        }
    }

    // =================================================================================
    // ⑫ `before-anchor` 段的动作在**闩锁之前**照常应用（`REQ-01M710E6PV4PPHECPPCCTRF5RA`）
    //    ★ 修前：0 点 pending ⇒ 抑制**全部**数值触发 ⇒ `at frame 2` 的 wait 一条也不应用（本用例红）。
    //    ★ 它只让"输入能在 0 点之前跑"，⛔ 不改"没进入目标脚本就响亮失败"（⑤ 钉着）。
    // =================================================================================
    {
        g_current_case = "anchor/absolute-segment-fires-before-latch";
        lift::InputScript s;
        lift::WindowManager wm;
        std::string err;
        s.setOriginScript(2);
        LIFT_CHECK(s.loadText(
            "before-anchor\n"
            "at frame 2\n"
            "wait 0 frames\n"
            "after-anchor\n"
            "at insn 100\n"
            "quit\n",
            "inline:anchor", &err));
        LIFT_CHECK(s.actionCount() == 2);
        LIFT_CHECK(s.actions()[0].absolute);
        LIFT_CHECK(!s.actions()[1].absolute);
        LIFT_CHECK(!s.tick(wm, 1, 10, 50, 10));  // 还没到 frame 2
        LIFT_CHECK(s.appliedCount() == 0);
        LIFT_CHECK(s.tick(wm, 2, 20, 60, 20));   // ★ 闩锁**之前**：绝对段照常到期
        LIFT_CHECK(s.appliedCount() == 1);
        LIFT_CHECK(!s.quitRequested());
        LIFT_CHECK(!s.originSet());
        // 相对段（`at insn 100`）在闩锁之前仍然被抑制（哪怕早就越过）
        LIFT_CHECK(!s.tick(wm, 400, 4000, 90000, 3000));
        LIFT_CHECK(s.appliedCount() == 1);
        LIFT_CHECK(!s.quitRequested());
        s.noteMarker(wm, sample(0, 1, "INIT.BIN"), 50, 500, 1000, 100);  // 不是目标 ⇒ 不闩锁
        LIFT_CHECK(!s.originSet());
        s.noteMarker(wm, sample(1, 2, "TARGET.BIN"), 50, 500, 1000, 100);  // 闩锁
        LIFT_CHECK(s.originSet());
        LIFT_CHECK(s.actions()[0].trigger.value == 2);    // ⛔ 绝对段没被 += origin（不是 52）
        LIFT_CHECK(s.actions()[1].trigger.value == 1100);  // 相对段 += insns(1000)
        LIFT_CHECK(!s.tick(wm, 50, 500, 1099, 100));
        LIFT_CHECK(s.tick(wm, 50, 500, 1100, 100));
        LIFT_CHECK(s.quitRequested());
    }

    // =================================================================================
    // ⑬ 同一脚本里的两段：绝对段的四个单位**不被** `+= origin` 改写，相对段的**被**
    //    ★ 关键：闩锁时**八条全部未应用** —— 绝对段若被重定基，前四条断言当场红
    //      （4→24 / 700→1200 / 55→555 / 66→166）。先应用再断言是**恒真**的（已应用的不动）。
    // =================================================================================
    {
        g_current_case = "anchor/absolute-segment-not-rebased";
        lift::InputScript s;
        lift::WindowManager wm;
        std::string err;
        s.setOriginScript(3);
        LIFT_CHECK(s.loadText(
            "before-anchor\n"
            "at frame 4\nwait 0 frames\n"
            "at insn 700\nwait 0 insns\n"
            "at guest_ms 55\nwait 0 ms\n"
            "at wall_ms 66\nwait 0 wall_ms\n"
            "after-anchor\n"
            "at frame 4\nwait 0 frames\n"
            "at insn 700\nwait 0 insns\n"
            "at guest_ms 55\nwait 0 ms\n"
            "at wall_ms 66\nwait 0 wall_ms\n",
            "inline:anchor", &err));
        LIFT_CHECK(s.actionCount() == 8);
        for (int i = 0; i < 4; ++i) LIFT_CHECK(s.actions()[static_cast<std::size_t>(i)].absolute);
        for (int i = 4; i < 8; ++i) LIFT_CHECK(!s.actions()[static_cast<std::size_t>(i)].absolute);
        // 闩锁前：四个单位都还没到（样本取小值）⇒ 一条都不应用（绝对段的抑制豁免也不会凭空触发）
        LIFT_CHECK(!s.tick(wm, 1, 1, 1, 1));
        LIFT_CHECK(s.appliedCount() == 0);
        s.noteMarker(wm, sample(0, 1, "INIT.BIN"), 20, 500, 500, 100);  // 不是目标 ⇒ 不闩锁
        LIFT_CHECK(!s.originSet());
        s.noteMarker(wm, sample(1, 3, "TARGET.BIN"), 20, 500, 500, 100);  // 闩锁
        LIFT_CHECK(s.originSet());
        LIFT_CHECK(s.origin().frame == 20);
        LIFT_CHECK(s.origin().guest_ms == 500);
        LIFT_CHECK(s.origin().insns == 500);
        LIFT_CHECK(s.origin().wall_ms == 100);
        // ★ 八条**全部未应用**时逐条对账：绝对段按声明原样，相对段 += origin
        LIFT_CHECK(s.actions()[0].trigger.value == 4);    // ⛔ 不是 24
        LIFT_CHECK(s.actions()[1].trigger.value == 700);  // ⛔ 不是 1200
        LIFT_CHECK(s.actions()[2].trigger.value == 55);   // ⛔ 不是 555
        LIFT_CHECK(s.actions()[3].trigger.value == 66);   // ⛔ 不是 166
        LIFT_CHECK(s.actions()[4].trigger.value == 24);   // 4 + 20
        LIFT_CHECK(s.actions()[5].trigger.value == 1200);  // 700 + 500
        LIFT_CHECK(s.actions()[6].trigger.value == 555);  // 55 + 500
        LIFT_CHECK(s.actions()[7].trigger.value == 166);  // 66 + 100
        // 行为：绝对段四条先到期并应用；相对段各差 1 时仍然一条不动
        LIFT_CHECK(s.tick(wm, 4, 55, 700, 66));
        LIFT_CHECK(s.appliedCount() == 4);
        LIFT_CHECK(!s.tick(wm, 23, 554, 1199, 165));
        LIFT_CHECK(s.appliedCount() == 4);
        LIFT_CHECK(s.tick(wm, 24, 555, 1200, 166));
        LIFT_CHECK(s.appliedCount() == 8);
    }

    // =================================================================================
    // ⑭ `before-anchor` **必须配** `after-anchor`：缺配对 ⇒ 解析期错误（父级裁决；⛔ 不猜隐式边界）
    //    ★ 为什么不是"绝对到文件尾"或"绝对止于闩锁"：两种都能自圆其说，而脚本字面看不出来
    //      ⇒ 静默歧义。要求显式写 `after-anchor`，歧义就不存在。
    //    ★ 报错必须指回 `before-anchor` 那一行（不是文件尾）。
    // =================================================================================
    {
        g_current_case = "anchor/rejects-before-anchor-without-after";
        lift::InputScript s;
        std::string err;
        s.setOriginScript(6);
        LIFT_CHECK(!s.loadText("at frame 1\nbefore-anchor\nat frame 5\nwait 0 frames\n", "inline:pair", &err));
        LIFT_CHECK(contains(err, "inline:pair:2:"));  // ★ 指向 `before-anchor` 那一行
        LIFT_CHECK(contains(err, "必须配 `after-anchor`"));
        LIFT_CHECK(s.actionCount() == 0);  // 失败 ⇒ 不留下半个脚本
        // 反方向：补上 `after-anchor` ⇒ 同一段文本解析通过（错误只针对"缺配对"）
        LIFT_CHECK(s.loadText(
            "at frame 1\nbefore-anchor\nat frame 5\nwait 0 frames\nafter-anchor\nat frame 9\nquit\n", "inline:pair2",
            &err));
    }

    // =================================================================================
    // ⑮⑯⑰⑱ 段标记的**解析期**错误（每条都必须响亮 + 带行号；⛔ 不静默忽略）
    // =================================================================================
    {
        g_current_case = "anchor/rejects-duplicate-before-anchor";
        lift::InputScript s;
        std::string err;
        s.setOriginScript(1);
        LIFT_CHECK(!s.loadText(
            "before-anchor\nat frame 1\nwait 0 frames\nafter-anchor\nbefore-anchor\nat frame 2\nquit\n",
            "inline:dup", &err));
        LIFT_CHECK(contains(err, "inline:dup:5:"));  // ★ 带行号
        LIFT_CHECK(contains(err, "重复的 `before-anchor`"));
        LIFT_CHECK(s.actionCount() == 0);  // 失败 ⇒ 不留下半个脚本
    }
    {
        g_current_case = "anchor/rejects-after-anchor-without-before";
        lift::InputScript s;
        std::string err;
        s.setOriginScript(1);
        LIFT_CHECK(!s.loadText("after-anchor\nat frame 1\nquit\n", "inline:order", &err));
        LIFT_CHECK(contains(err, "inline:order:1:"));
        LIFT_CHECK(contains(err, "出现在 `before-anchor` 之前"));
    }
    {
        g_current_case = "anchor/rejects-segment-without-anchor";
        lift::InputScript s;  // ★ 没开 `--anchor-script`
        std::string err;
        LIFT_CHECK(!s.loadText("before-anchor\nat frame 1\nquit\n", "inline:noanchor", &err));
        LIFT_CHECK(contains(err, "inline:noanchor:1:"));
        LIFT_CHECK(contains(err, "--anchor-script"));
        // `after-anchor` 同理（也是"没有 0 点就没有段"）
        LIFT_CHECK(!s.loadText("at frame 1\nquit\nafter-anchor\n", "inline:noanchor2", &err));
        LIFT_CHECK(contains(err, "--anchor-script"));
        // 反方向：**给了** `--anchor-script` ⇒ 同样的文本解析通过
        lift::InputScript ok;
        ok.setOriginScript(1);
        LIFT_CHECK(ok.loadText("before-anchor\nat frame 1\nwait 0 frames\nafter-anchor\nat frame 2\nquit\n",
                               "inline:ok", &err));
    }
    {
        g_current_case = "anchor/rejects-profile-in-before-segment";
        lift::InputScript s;
        std::string err;
        s.setOriginScript(1);
        LIFT_CHECK(!s.loadText("before-anchor\nat frame 1\nprofile_begin win\nat frame 2\nquit\n", "inline:prof",
                               &err));
        LIFT_CHECK(contains(err, "inline:prof:3:"));  // ★ 行号
        LIFT_CHECK(contains(err, "不能出现在 `before-anchor` 段里"));
        LIFT_CHECK(!s.loadText("before-anchor\nat frame 1\nprofile_end win\nquit\n", "inline:prof2", &err));
        LIFT_CHECK(contains(err, "inline:prof2:3:"));
        // 相对段里照常允许（⛔ 不是"profiler 整体禁用"）
        LIFT_CHECK(s.loadText("before-anchor\nat frame 1\nwait 0 frames\nafter-anchor\nat frame 2\nprofile_begin win\n"
                              "at frame 3\nquit\n",
                              "inline:prof3", &err));
    }

    // =================================================================================
    // ⑲ 绝对段的 `at frame N` 与 `at insn N` **各一条** ⇒ 两个单位都走绝对路径
    //    （闩锁前各自到期；闩锁后值不变；相对段那一条照常 `+= origin`）
    // =================================================================================
    {
        g_current_case = "anchor/absolute-frame-and-insn";
        lift::InputScript s;
        lift::WindowManager wm;
        std::string err;
        s.setOriginScript(4);
        LIFT_CHECK(s.loadText(
            "before-anchor\nat frame 9\nwait 0 frames\nat insn 50\nwait 0 insns\n"
            "after-anchor\nat frame 9\nquit\n",
            "inline:anchor", &err));
        LIFT_CHECK(s.actionCount() == 3);
        LIFT_CHECK(!s.tick(wm, 8, 0, 49, 0));
        LIFT_CHECK(s.appliedCount() == 0);
        LIFT_CHECK(s.tick(wm, 9, 0, 49, 0));  // ★ 绝对 frame 到期
        LIFT_CHECK(s.appliedCount() == 1);
        LIFT_CHECK(!s.tick(wm, 9, 0, 49, 0));  // insn 还差 1
        LIFT_CHECK(s.tick(wm, 9, 0, 50, 0));   // ★ 绝对 insn 到期
        LIFT_CHECK(s.appliedCount() == 2);
        s.noteMarker(wm, sample(1, 4, "TARGET.BIN"), 40, 400, 1000, 80);  // 闩锁
        LIFT_CHECK(s.originSet());
        LIFT_CHECK(s.actions()[0].trigger.value == 9);   // 绝对 frame：原样
        LIFT_CHECK(s.actions()[1].trigger.value == 50);  // 绝对 insn：原样
        LIFT_CHECK(s.actions()[2].trigger.value == 49);  // 相对 frame：9 + origin.frame(40)
        LIFT_CHECK(!s.tick(wm, 48, 0, 0, 0));
        LIFT_CHECK(s.tick(wm, 49, 0, 0, 0));
        LIFT_CHECK(s.quitRequested());
    }

    return finish("input-anchor");
}
