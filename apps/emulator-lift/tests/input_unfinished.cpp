// emulator-lift · **输入脚本的否定证据**：条件永不满足的动作在 run 结束时必须留下可解析的读数
// —— pure 守卫（BUG-20 / `REQ-01M72QSNQCH3GHH3158A7YX96V`）
//
// ★ 判据（红了意味着什么）：一份 `--input-script` 时间线跑完之后，
//   ① 有**未应用动作**且不是脚本自己 `quit` ⇒ 运行摘要里**必须**有
//      `input-script: unfinished pending=<n> releases=<m> first="<原动作行>"`，且 `pending` / `first`
//      与脚本原文逐字对得上；
//   ② 动作**全部应用** ⇒ 这一行**不许**出现（`[input-script] finished` 那条照旧有）；
//   ③ 脚本中间 `quit`、后面还有动作 ⇒ 这一行**不许**出现（`quit` 之后剩下的动作是脚本**有意**跳过的）；
//      收口行也必须照实报 `quit：已应用 <applied> / 共 <total> 条动作（…剩 <n> 条未应用…）`，
//      ⛔ 不许声称"全部应用"（`REQ-01M72WHFMCWQ0KEJDQG0XH21AH`）；
//   ④ **乱序应用**（前一条的条件不满足、后一条先到期）⇒ `first` 必须是**真正没跑的那条**
//      （⛔ 不能靠 `applied_` 当下标 —— 应用顺序不一定等于文件顺序）；
//   ⑤ 原文含 `"` ⇒ 字段内换成 `'`（否则字段提前闭合、行不可解析），且整行**仍是一行**；
//   ⑥ **一份脚本都没装** ⇒ 摘要里连 `inputstat :` 都不该有，更没有这一行（整段在 `active()` 里）。
//   ★ 为什么值得一条守卫：`finished` 那条日志在 `finished()` **或 `quit`** 时打，而"没打那行"同时意味着
//     "脚本跑完了 / 条件写错了 / run 提前停了"——这三件事在旧输出里**分不开**（缺陷单里就是这么把读数
//     算错一帧的）。这一行把否定证据变成正面读数（几条没跑 + 第一条是什么）。
// ★ 夹具形状（照 `tests/input_expect.cpp`）：裸 `Machine` + 自造 guest 循环 + 捕获 stdout。
//   **一次 Present 都不发生** ⇒ `frame` 恒 0 ⇒ `at frame 50` 永远不满足（要的正是这个）。
// ⛔ 不依赖游戏安装 / LFS / 真实窗口；⛔ 不断言任何墙钟数字，只断言摘要行的**逐字内容**。
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

namespace {

constexpr std::uint32_t kCodeEa = 0x00401000u;
constexpr std::uint32_t kSentinel = 0xFFFFFFFFu;  // `prepareStack` 压的哨兵 = run 的正常停点

// 极简汇编器：只编码本夹具要用的那几条（位移一律由 `ea()` 算，⛔ 不手填）。
struct Program {
    std::vector<std::uint8_t> b;
    std::uint32_t ea() const { return kCodeEa + static_cast<std::uint32_t>(b.size()); }
    void u8(std::uint8_t v) { b.push_back(v); }
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
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
    void jl(std::uint32_t target) {
        const std::int32_t rel = static_cast<std::int32_t>(target) - static_cast<std::int32_t>(ea() + 2);
        u8(0x7C);
        u8(static_cast<std::uint8_t>(rel & 0xFF));
    }
    void ret() { u8(0xC3); }
};

// guest：`iterations` 轮 `inc / cmp / jl` 循环然后 `ret`（→ 哨兵）。
//   ★ **不调 Present** ⇒ `frame` 一直是 0：这就是"条件永不满足"的夹具（不是猜的，是本夹具的性质）。
Program buildLoop(std::uint32_t iterations) {
    Program p;
    p.movEcxImm(0);
    const std::uint32_t loop = p.ea();
    p.incEcx();
    p.cmpEcxImm(iterations);
    p.jl(loop);
    p.ret();
    return p;
}

bool boot(lift::Machine& m) {
    std::string err;
    if (!m.initBare(&err)) return false;
    if (!m.prepareStack(&err)) return false;
    const Program p = buildLoop(40u);
    if (!m.placeCode(kCodeEa, p.b.data(), p.b.size(), &err)) return false;
    m.cpu().eip = kCodeEa;
    return true;
}

// ---- stdout + stderr 一起捕获（摘要与 `[input-script]` 日志都要断言）----
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

// 把日志里**那一行**原样打出来（守卫的诊断面：红的时候能立刻看到实参，而不用去猜）。
void printLine(const std::string& log, const char* needle) {
    const std::size_t at = log.find(needle);
    if (at == std::string::npos) {
        std::printf("  [%s] <没有这一行>\n", needle);
        return;
    }
    const std::size_t end = log.find('\n', at);
    std::printf("  %s\n", log.substr(at, end == std::string::npos ? std::string::npos : end - at).c_str());
}

// 跑一次夹具：装时间线脚本 → `run` → **把运行摘要打进同一个捕获流**（与 `main` 同一条路径）。
bool runFixture(lift::Machine& m, const char* script, std::string* log, lift::RunResult* r) {
    std::string err;
    // `script == nullptr` ⇒ **一份脚本都不装**（guard ③：`active()` 门必须连一个字都不打）。
    if (script != nullptr && !m.loadInputScriptText(script, "inline:unfinished", &err)) {
        std::fprintf(stderr, "时间线解析失败：%s\n", err.c_str());
        return false;
    }
    lift::RunOptions o;
    o.until_ea = kSentinel;
    o.time_limit_sec = 60.0;  // 兜底：⛔ 守卫不许挂死
    Capture cap;
    if (!cap.begin()) return false;
    *r = m.run(o);
    m.dumpRunSummary(*r, stdout);
    *log = cap.finish();
    return true;
}

const char* kFinishedLog = "[input-script] finished";
// `quit` **提前**结束时的收口行（`REQ-01M72WHFMCWQ0KEJDQG0XH21AH`）：必须是"已应用 X / 共 N 条"，
// ⛔ 不是 `finished：<总数> 条动作全部应用`（那与 `input-script: unfinished pending=…` 互相矛盾）。
const char* kQuitLog = "[input-script] quit：";

}  // namespace

int main() {
    using namespace lift_test;

    // =================================================================================
    // ① 条件永不满足（`at frame 50`，而本次 run 一次 Present 都没有）⇒ 摘要必须有那一行
    // =================================================================================
    {
        g_current_case = "unfinished-never-due";
        lift::Machine m;
        LIFT_CHECK(boot(m));
        std::string log;
        lift::RunResult r;
        LIFT_CHECK(runFixture(m, "at frame 50\nmouse_move 10 10\n", &log, &r));
        LIFT_CHECK(r.ok());
        // ★ 逐字：`pending` / `releases` / `first` 三个字段都要对得上（`first` = 原动作行）。
        printLine(log, "input-script: unfinished");
        LIFT_CHECK(contains(log, "input-script: unfinished pending=1 releases=0 first=\"mouse_move 10 10\""));
        // ★ `[input-script] finished` 那条**不该**有（它只在 `finished() || quit_requested_` 时打）。
        LIFT_CHECK(!contains(log, kFinishedLog));
    }

    // =================================================================================
    // ② 全部已应用 ⇒ 这一行**不许**出现（A/B 的正例；否则"一直打"也能算"会打"）
    // =================================================================================
    {
        g_current_case = "unfinished-all-applied";
        lift::Machine m;
        LIFT_CHECK(boot(m));
        std::string log;
        lift::RunResult r;
        // `wait` 是 no-op 动作：它的触发值是 11（`at insn 10` 的锚 + `wait 1 insn`）⇒ 到 run 停时 applied == size。
        LIFT_CHECK(runFixture(m, "at insn 10\nwait 1 insn\n", &log, &r));
        LIFT_CHECK(r.ok());
        printLine(log, kFinishedLog);
        LIFT_CHECK(!contains(log, "input-script: unfinished"));
        // ★ 正例（本单翻面判据的另一半）：真正"全部应用"时**仍**必须打"全部应用" + 逐字报数，
        //   ⛔ 不是把这句话删掉或改成一句含糊的话（`REQ-01M72WHFMCWQ0KEJDQG0XH21AH`）。
        LIFT_CHECK(contains(log, "[input-script] finished：1 条动作全部应用"));
        LIFT_CHECK(!contains(log, kQuitLog));
    }

    // =================================================================================
    // ③ 负控：脚本自己 `quit`（后面还有动作）⇒ 这一行**不许**出现
    //    理由：`quit` 之后的动作是脚本**有意**跳过的；报它会让正常脚本天天告警。
    // =================================================================================
    {
        g_current_case = "unfinished-quit-negative-control";
        lift::Machine m;
        LIFT_CHECK(boot(m));
        std::string log;
        lift::RunResult r;
        LIFT_CHECK(runFixture(m, "at insn 10\nquit\nat insn 20\nwait 1 insn\n", &log, &r));
        LIFT_CHECK(r.reason == lift::StopReason::Stopped);  // 真的是 `quit` 停的（不是程序跑完）
        printLine(log, kQuitLog);
        LIFT_CHECK(!contains(log, "input-script: unfinished"));
        // ★ **翻面**（`REQ-01M72WHFMCWQ0KEJDQG0XH21AH`）：`quit` 提前结束 ⇒ 收口行必须**照实**报
        //   "已应用 1 / 共 2 条 + 剩 1 条未应用"（脚本里共 2 条动作：`quit` 与应用不到的 `wait`），
        //   ⛔ 不许声称"全部应用"—— 旧文案打的是**总**动作数、说的是全部应用。
        LIFT_CHECK(
            contains(log, "[input-script] quit：已应用 1 / 共 2 条动作（quit 提前结束，剩 1 条未应用；"));
        LIFT_CHECK(!contains(log, "全部应用"));
        LIFT_CHECK(!contains(log, kFinishedLog));
    }

    // =================================================================================
    // ①b **乱序应用**：动作是**按到期 / 边沿**应用的，不是按文件顺序 ⇒
    //    `first` 必须是"真正没跑的那条"（扫 `applied` 标志），⛔ 不能取 `actions_[applied_]`
    //    （那会指向**已应用**的后一条）。脚本：`at frame 50`（永不满足）+ 后一条 `at insn 30`（会应用）。
    // =================================================================================
    {
        g_current_case = "unfinished-out-of-order-applied";
        lift::Machine m;
        LIFT_CHECK(boot(m));
        std::string log;
        lift::RunResult r;
        LIFT_CHECK(runFixture(m, "at frame 50\nmouse_move 10 10\nat insn 30\nmouse_move 20 20\n", &log, &r));
        LIFT_CHECK(r.ok());
        // ★ 只看摘要那一行：`20 20` 会出现在 `[input-script]` 的**应用日志**里（它是真应用了的那条），
        //   所以判据必须是**整行逐字**，⛔ 不是"日志里有没有 20 20"。
        printLine(log, "input-script: unfinished");
        LIFT_CHECK(contains(log, "input-script: unfinished pending=1 releases=0 first=\"mouse_move 10 10\"\n"));
    }

    // =================================================================================
    // ①c **转义**：`first` 里的 `"` 必须换成 `'`，否则字段被提前闭合、整行也不可解析。
    //    label 解析**不校验字符**（`profile_begin` 只要求"单个词"）⇒ 脚本里能塞进 `"`。
    // =================================================================================
    {
        g_current_case = "unfinished-quote-escaped";
        lift::Machine m;
        LIFT_CHECK(boot(m));
        std::string log;
        lift::RunResult r;
        LIFT_CHECK(runFixture(m, "at frame 50\nprofile_begin a\"b\n", &log, &r));
        LIFT_CHECK(r.ok());
        printLine(log, "input-script: unfinished");
        // ★ 末尾带 `\n`：证明字段在这里就闭合了（仍是**一行**），后面没有 `b"` 之类的残留。
        LIFT_CHECK(contains(log, "input-script: unfinished pending=1 releases=0 first=\"profile_begin a'b\"\n"));
        LIFT_CHECK(!contains(log, "a\"b"));  // 摘要里⛔ 不许出现裸的 a"b
    }

    // =================================================================================
    // ①d `active()` 门：**一份脚本都没装**的一轮 run，摘要里⛔ 不许出现这一行
    //    （它整段在 `input_script_.active()` 里面；挪出去 ⇒ 无脚本也会打出 `pending=0 … first=""`）。
    // =================================================================================
    {
        g_current_case = "unfinished-no-script";
        lift::Machine m;
        LIFT_CHECK(boot(m));
        std::string log;
        lift::RunResult r;
        LIFT_CHECK(runFixture(m, /*script=*/nullptr, &log, &r));
        LIFT_CHECK(r.ok());
        LIFT_CHECK(!contains(log, "input-script: unfinished"));
        LIFT_CHECK(!contains(log, "inputstat :"));  // 整段（含 inputstat）都不该有
        LIFT_CHECK(!contains(log, kFinishedLog));
    }

    // =================================================================================
    // ④ `pending==0` 但还有**没交付的抬起**（显式 hold 1000 帧，而帧号恒 0）⇒
    //    `first` 取第一条未完成抬起的原文（⛔ 不能只报 `pending`，那会把这类收口说成"动作没跑"）
    // =================================================================================
    {
        g_current_case = "unfinished-pending-release";
        lift::Machine m;
        LIFT_CHECK(boot(m));
        std::string log;
        lift::RunResult r;
        LIFT_CHECK(runFixture(m, "at insn 10\nclick 100 200 hold 1000 frames\n", &log, &r));
        LIFT_CHECK(r.ok());
        printLine(log, "input-script: unfinished");
        LIFT_CHECK(contains(log, "input-script: unfinished pending=0 releases=1 first=\"click 100 200 hold 1000 frames\""));
        LIFT_CHECK(!contains(log, kFinishedLog));
    }

    return finish("input-unfinished");
}
