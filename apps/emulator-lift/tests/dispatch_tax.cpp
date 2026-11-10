// emulator-lift · **pure** 派发层的固定税（b / c）+ 诊断开关（`LIFT_TOKEN_DIAG_SKIP_LOCK`）守卫
//
// 上下文：REQ-01M70S2WR4S8MN2JDWJRJD6WFD（"吞吐的派发层拆分"）。要用一个数回答
//   「每块到底交多少固定税（b）、每条指令多少（c）」以及「把令牌的 mutex 拿掉能省多少」。
//
// 红了意味着什么（判据，缺一条都不算绿）：
//   ① `dispatch-tax/microbench`：**未竞争** `std::mutex` lock+unlock 与 `steady_clock::now()`
//      的每次读数落在**量级带**里（各 N=1,000,000 次 × 7 个样本取**最小**：剩下的偏差几乎全是被
//      抢占抬高的，取中位等于把一半抢占留在读数里 —— 实测 `token >= lock` 那条就会因此抖红）。
//      ⛔ 只断言量级（[2, 500] ns），**不断言具体数字、不认机器型号** —— 它变红说明
//      "测的根本不是那把锁 / 那个时钟"（例：lock 循环被优化掉 ⇒ 读数掉到亚纳秒）。
//   ② `dispatch-tax/block-scan`：自造 guest 代码把**块长**做成 1 / 3 / 8 条/块（同一份
//      `Machine` 的块缓存、同一段循环），三点线性拟合 `t_block = b + c·k`；断言 `b > 0`、`c > 0`
//      且都落在量级带里（b ∈ [0.5, 5000] ns，c ∈ [0.5, 500] ns），并断言"每块时间随块长增长"
//      （`t(8) > 1.2 × t(1)`）。⛔ 不断言 b/c 的具体数值 —— 那是**读数**，进文档不进 test。
//      ★ 另有两条成对的判据（意图：**跳过令牌锁的那一臂，每块的边际成本 b 不低于正常臂** =
//      "那一次锁的成本非负"），抗噪口径与实测读数见后面「②的抗噪口径」一节。
//
//   ③ `dispatch-tax/diag-off`：**默认关**（环境变量不存在）⇒ 运行摘要里是 `token-diag : skip-lock=0`。
//   ④ `dispatch-tax/diag-on`：`LIFT_TOKEN_DIAG_SKIP_LOCK=1` ⇒ 摘要里是 `token-diag : skip-lock=1`，
//      且 `tokenwait : waits 0`（诊断臂里**一次都不取锁**，所以不可能有"等锁"）·
//      `token : violations=0 max-concurrent=1`（单线程工况下不变式照旧成立）。
//   ⑤ `dispatch-tax/diag-really-skips`：**不靠计时**地证明"真的绕过了 mutex" —— 局部令牌持有时，
//      另一条线程的 `tryAcquire()` 在诊断臂里**成功**（`maxConcurrent()==2`），正常臂里**失败**
//      （`maxConcurrent()==1`）。把环境变量读点改成恒假 / 让 skip 分支其实仍取锁 ⇒ 这两半之一当场红。
//   ⑥ `dispatch-tax/diag-keeps-assert-token`：诊断臂里 `check()` 的违规计数照旧 —— 令牌之外 check
//      ⇒ `violations==1` 且 first-site 点名；持令牌 check ⇒ 不计数。⇒ `--assert-guest-token` 的
//      可信度不因这个开关而下降（`violations` / `max_concurrent` 与 mutex 无关）。
//
//   ⑦ `dispatch-tax/present-tail-line`：运行摘要 `present-tail` 的**打印门** ——
//      `present_tail.any == false`（本轮 0 次 Present）⇒ 摘要里**没有** `present-tail:` 行；
//      `any == true` ⇒ 有，且整行匹配固定格式（字段名 / 顺序 / `%llu`·`%.1f` 一位小数）。
//      ⛔ 不断言任何当前数字（那几个数是本用例自己塞进 `RunResult` 的，不是读数）。
//      ★ 这条是**跨平台**的：`cli-time-limit` 那条同判据的子进程守卫是 Windows 专有（非 Windows 上 SKIP），
//      这里直接调 `Machine::dumpRunSummary`（吃 `RunResult` + Machine 状态）⇒ 不需要 guest / 安装 / 子进程，
//      于是"删掉 `if (any)` 门"在 macOS 上也当场红。
//
// ②的抗噪口径（为什么不是 `fit_off.b >= fit_on.b` 一条了事）：
//   ① **旧写法会自己抖红**（实测，macOS arm64 / RelWithDebInfo）：两臂各跑一次拟合时，
//      `Δb = b_off - b_on` 的 run-to-run σ ≈ 10 ns，而它要测的东西只有 ~4 ns ⇒ 20 次连跑里
//      10 次红（原断言在**另一个 agent** 用 HEAD 版的同一文件上也 2/6 红 ⇒ 是既有的抖，不是本轮改动）。
//   ② **噪声来源是机器状态漂移，不是计时器**：一次性跑 60 万块时，两臂的两个读数相隔
//      几十~几百毫秒，频率/热/调度状态各自不同；更糟的是三点拟合的**三个点也各占一段时间**，
//      慢漂移会把那条线整体**倾斜**（实测同一轮里 t(1) 能从 74 跳到 95 ns）⇒ 倾斜直接变成 b 的噪声。
//      ⇒ 现在把 (档, 臂) **六个格子放进同一个片循环**轮流跑一小片，每片只有 ~0.6 ms（k=1），
//      并且**先测哪一臂也轮流**（(片+档)%2，否则"第二个测的那一臂"普遍更快，那个顺序偏差会
//      整个记到 Δb 上 —— 实测可达 +20 ns/块）。每格的 `t_block` = 本格**逐片**每块时间的中位
//      （⛔ 不是总时长/总块数：一台满载机器上单个片被抢占一次就能把那一格抬高几十 ns/块，
//      实测出现过 +3457 ns/块 的片 —— 中位对那种尾巴免疫）。
//   ③ **`b` 这个量本身不是"锁的成本"的好载体**：实测配对差 `Δt(k) = t_off(k) - t_on(k)`
//      随 k 增长（k=1 ≈ +2.5 ns，k=3 ≈ +4 ns，k=8 ≈ +5~20 ns/块）—— 块越长，mutex 那条
//      cache line 越容易被挤掉，取锁越贵。三点拟合把这份成本**摊进了斜率 c**，于是截距之差
//      `Δb` 只剩 ~3 ns 且被 k=8 的负权重（-0.28）拖住 ⇒ 它天生抖（σ ≈ 1.9 ns 与信号同量级）。
//   ④ 于是判据拆成**两条**（都保留"两臂对比 + 同向"的原意，都不是恒真）：
//      (a) `d_b >= kDeltaBTolNs`（-5 ns）：拟合截距之差的**下界**（容差由实测分布定，见常量注释）
//          —— 抓"诊断臂每块明显更慢"这类粗回归；
//      (b) `dt1 >= max(0.5, 10% × lock+unlock)`：**k=1 的配对差**（同一片里两臂相邻测的每块差）
//          —— 这是"每块那一次锁的成本"最干净的载体（实测中位 +2.6 ns，20 次连跑 ∈ [2.24, 3.09]，
//          且 k=1 的片数最多 ⇒ 它的中位极稳）。它才是**锐**的那条：把诊断臂的"跳过锁"改成
//          **其实也取一次锁**（skip-lock 变 no-op）⇒ 两臂代码相同 ⇒ `dt1 ≈ 0` ⇒ 这一条当场红。
//      ⇒ 两条一起看：`b` 的对比还在（不删、不放宽成"任何负数都过"），而"真的跳过了锁"这件事
//      有一个不靠运气的判据。
//
// ★ 为什么这条必须是 pure 且顶层无条件注册：它只用 `initBare()` 自造字节 + 真 `Machine`，
//   不碰游戏 / LFS / GPU / 音频设备（`Machine` 的缺省音频档就是 `Off` ⇒ 单线程）。
// ★ 为什么可以把"墙钟读数"放进 CTest：断言的是**量级不变量**（不是性能门槛）。性能判据仍走
//   `pnpm tools lift bench idle`（真工况 + N 遍 + 中位 + 落 JSON）。
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "check.h"
#include "runtime/guest_token.h"
#include "runtime/machine.h"

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint32_t kCodeEa = 0x00401000u;
constexpr const char* kDiagEnv = "LIFT_TOKEN_DIAG_SKIP_LOCK";

// 微基准：每次样本的迭代数 / 样本数（★ 任务口径：N ≥ 100 万次；样本间取最小）。
constexpr int kMicroIters = 1000000;
constexpr int kMicroSamples = 7;

// 块长扫描的采样口径（见文件头「②的抗噪口径」）：片循环把 (档, 臂) 六个格子轮流各跑一小片。
// ★ 每片块数固定 10000：再小则"每片调一次 run()"的固定开销（arm/disarm、诊断臂在 stderr 喊的
//   那一声…）开始污染读数（它只落在诊断臂上）；10000 块 ≈ 0.6 ms（k=1），开销 ≈ 0.2%。
constexpr std::uint64_t kSliceBlocks = 10000;
// 每格的片数（对应 k = 1 / 3 / 8）。★ 为什么 k=1 远多于别的档：锐判据（见下 `kLockCostFloorRatio`）
// 落在 k=1 —— 它的每片只要 0.6 ms，而 k=8 的每片要 4.6 ms；同样的时间里 k=1 能拿到 ~8 倍的
// 配对样本。k=3 / k=8 的片数只够"拟合 b/c"用（它们各自的 t_block 会被几千片的 k=1 精确定住）。
constexpr int kScanSlices[3] = {200, 60, 40};
constexpr int kScanRounds = 3;  // 整轮重复次数（判据取各轮的中位）

// 三档循环各自的落点（同一台 Machine 里同时存在 ⇒ 片循环才能把 k 也一起交替）。
// k=8 的循环只有 7×3 + 5 = 26 字节，0x100 的间隔绰绰有余。
constexpr std::uint32_t kCodeEaStride = 0x100;

// ---- 判据 (a)：拟合截距之差 Δb 的**下界**（ns）----
// ★ 不是"放宽到任何负数都过"：它是一个**有界**的下界，且由实测分布定 —— 20 次连跑（macOS arm64 /
//   RelWithDebInfo / load avg ≈ 77 的**满载**机器）`Δb`（配对口径，每轮一个、取 3 轮中位）∈
//   [-1.37, +5.92]，均值 +3.08，σ ≈ 1.8 ⇒ 取 **-5 ns** = 均值之下 ~4.5σ（比实测最负的一次还低
//   3.6 ns）。20 次连跑 0 次红（逐轮的 60 个原始值 ∈ [-7.18, +9.62]，所以判据只能落在"中位"上）。
//   它抓的是**粗回归**（诊断臂每块明显更慢）；锐判据是下面那条 k=1 的配对差。
constexpr double kDeltaBTolNs = -5.0;

// ---- 判据 (b)：k=1 的配对差（每块那一次锁的成本）的**下界** ----
// ★ 用**相对**容差（任务给的选项 3 那种形式），锚在同一次扫描里已经量过的**未竞争 lock+unlock**
//   （`measureLockUnlockNs`，本机 4.8 ~ 10.0 ns）：实测 dt1 / (lock+unlock) ∈ [0.2, 0.5]，
//   取 **10%** ⇒ 下界 = max(0.5, 0.1×(lock+unlock)) 实测 0.50 ~ 1.06 ns；而实测 dt1 在满载机器上
//   仍 ∈ [2.24, 3.09]（20 次连跑）、空载 [2.41, 2.70] ⇒ 还有 2~5 倍余量。
// ★ 零假设（skip-lock 变 no-op ⇒ 两臂代码相同）下 dt1 ≈ 0（实测 σ ≈ 0.16 ns）⇒ 0 远在下界之下
//   ⇒ **当场红**。这就是"真的跳过了锁"的锐判据：它不依赖 b/c 拟合（那个量的噪声与信号同量级）。
constexpr double kLockCostFloorRatio = 0.10;

// 防止编译器把测量循环整个优化掉（now() 的累计值必须"有人看"）。
volatile std::uint64_t g_sink = 0;

void setDiagEnv(const char* value) {
#ifdef _WIN32
    _putenv_s(kDiagEnv, value == nullptr ? "" : value);
#else
    if (value == nullptr) {
        unsetenv(kDiagEnv);
    } else {
        setenv(kDiagEnv, value, 1);
    }
#endif
}

/**
 * 计时窗口内把 **stderr 静音**。
 * ★ 为什么需要：诊断臂**每次** `Machine::run()` 都会在 stderr 喊一声"诊断开关已开"
 *   （`machine.cpp` 的产品行为，⛔ 不该为了测试去改它）；而本用例的片循环要跑几千片 ⇒ 不静音
 *   会有几千行**一模一样**的告警把 ctest 的输出淹掉。
 * ★ 静音只覆盖**采样窗口**：窗口外的输出（含本用例的读数失败 / 断言 / 摘要）照旧可见。
 */
class StderrMute {
public:
    StderrMute() {
#ifdef _WIN32
        saved_ = ::_dup(2);
        if (saved_ >= 0) {
            const int nul = ::_open("NUL", _O_WRONLY);
            if (nul >= 0) {
                ::_dup2(nul, 2);
                ::_close(nul);
            }
        }
#else
        saved_ = ::dup(2);
        if (saved_ >= 0) {
            const int nul = ::open("/dev/null", O_WRONLY);
            if (nul >= 0) {
                ::dup2(nul, 2);
                ::close(nul);
            }
        }
#endif
    }
    ~StderrMute() {
        if (saved_ < 0) return;
#ifdef _WIN32
        ::_dup2(saved_, 2);
        ::_close(saved_);
#else
        ::dup2(saved_, 2);
        ::close(saved_);
#endif
    }
    StderrMute(const StderrMute&) = delete;
    StderrMute& operator=(const StderrMute&) = delete;

private:
    int saved_ = -1;
};

double medianOf(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const std::size_t mid = v.size() / 2;
    return (v.size() % 2 != 0) ? v[mid] : (v[mid - 1] + v[mid]) / 2.0;
}

/**
 * 一组样本里的**最小**值。
 * ★ 为什么微基准用"最小"而不是中位：每个样本本身已经是 100 万次的**平均**，剩下的偏差几乎全是
 *   **被抢占**（只会把读数抬高）。两台机器/两次运行之间要比的是"最干净的那一次"，取中位等于把
 *   抢占的一半留在了读数里 —— 实测同一台机器上 `GuestToken acq+rel` 的中位能在 8.4 ~ 17.3 ns
 *   之间摆，而 `token >= lock` 这条判据要比较的就是它。取最小 ⇒ 两边都是"没有被打断的成本"。
 */
double minOf(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    return *std::min_element(v.begin(), v.end());
}

/** 未竞争 `std::mutex`：lock + unlock 一次多少 ns（样本内取每次的平均，样本间取最小）。 */
double measureLockUnlockNs() {
    std::vector<double> samples;
    samples.reserve(kMicroSamples);
    for (int s = 0; s < kMicroSamples; ++s) {
        std::mutex m;  // 每个样本一把新锁：不跨样本复用一个可能进过慢路径的锁
        const auto t0 = Clock::now();
        for (int i = 0; i < kMicroIters; ++i) {
            m.lock();
            m.unlock();
        }
        const auto t1 = Clock::now();
        samples.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() /
                          static_cast<double>(kMicroIters));
    }
    return minOf(samples);
}

/** `steady_clock::now()` 一次多少 ns（含循环里那次累加：量级带足够宽，见文件头 ①；样本间取最小）。 */
double measureSteadyClockNs() {
    std::vector<double> samples;
    samples.reserve(kMicroSamples);
    for (int s = 0; s < kMicroSamples; ++s) {
        std::uint64_t acc = 0;
        const auto t0 = Clock::now();
        for (int i = 0; i < kMicroIters; ++i) {
            acc += static_cast<std::uint64_t>(Clock::now().time_since_epoch().count());
        }
        const auto t1 = Clock::now();
        g_sink = g_sink + acc;
        samples.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() /
                          static_cast<double>(kMicroIters));
    }
    return minOf(samples);
}

/** 真令牌的一次 acquire+release（未竞争）多少 ns（样本间取最小）。 */
double measureTokenRoundTripNs() {
    lift::GuestToken tok;
    std::vector<double> samples;
    samples.reserve(kMicroSamples);
    for (int s = 0; s < kMicroSamples; ++s) {
        const auto t0 = Clock::now();
        for (int i = 0; i < kMicroIters; ++i) {
            tok.acquire();
            tok.release();
        }
        const auto t1 = Clock::now();
        samples.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() /
                          static_cast<double>(kMicroIters));
    }
    return minOf(samples);
}

/**
 * 造一段"每块 k 条指令"的循环，落在 `ea`：
 *   (k-1) 条 `add eax,1` + 1 条 `jmp rel32`（跳回 `ea`）⇒ 块 = k 条，块边界落在 jmp 上。
 * 同一份代码 / 同一个 EA 反复执行 ⇒ 块缓存**命中**（测的正是派发层，不是解码）。
 */
std::vector<std::uint8_t> makeBlockLoop(std::uint32_t k, std::uint32_t ea) {
    std::vector<std::uint8_t> b;
    for (std::uint32_t i = 0; i + 1 < k; ++i) {
        b.push_back(0x83);  // add eax, imm8
        b.push_back(0xC0);
        b.push_back(0x01);
    }
    b.push_back(0xE9);  // jmp rel32
    const std::uint32_t at = ea + static_cast<std::uint32_t>(b.size());
    const std::int32_t rel = static_cast<std::int32_t>(ea) - static_cast<std::int32_t>(at + 4u);
    const std::uint32_t rel_bits = static_cast<std::uint32_t>(rel);
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>((rel_bits >> (8 * i)) & 0xFFu));
    return b;
}

/** 片循环跑出来的**格子表**：`ns[档][臂]` / `blocks[档][臂]` + 每档的**配对差**样本。 */
struct ScanCells {
    std::vector<double> ns[3][2];    // 每格**逐片**的每块墙钟（臂 0 = 正常，1 = 诊断）
    std::vector<double> pair_dt[3];  // 每档每片的配对差（每块 ns：off - on）
    bool ok = false;
};

/** 第 `i` 档（k=1/3/8）循环的落点。 */
std::uint32_t codeEaOf(std::size_t i) {
    return kCodeEa + kCodeEaStride * static_cast<std::uint32_t>(i);
}

/**
 * 一轮扫描：一台 Machine 里放三档循环，**片循环把 (档, 臂) 六个格子轮流**各跑一小片
 * （每片 `kSliceBlocks` 块；片数见 `kScanSlices`），并把同一片里两臂的每块差记进 `pair_dt`。
 *
 * ★ 抗噪的三件事（为什么不是"每臂各跑一次长循环"）见文件头「②的抗噪口径」：
 *   ① 两臂**交替**（读数相邻 ⇒ 漂移共模）；
 *   ② **档也交替**（三点的测量时刻混在同一段时间里 ⇒ 慢漂移不会把拟合线整体倾斜）；
 *   ③ **先测哪一臂也轮流**（否则后测的那一臂普遍更快，那个顺序偏差会整个记到 Δb 上）。
 * ★ 片间翻转臂只靠**环境变量**：`Machine::run` 每次都 `guestToken().arm()`，而
 *   `LIFT_TOKEN_DIAG_SKIP_LOCK` 的唯一读点就在 `arm()`（`guest_token.cpp`）⇒ 不改运行期代码。
 */
ScanCells measureCells(const std::vector<std::uint32_t>& ks) {
    ScanCells out;
    lift::Machine m;
    std::string err;
    if (!m.initBare(&err) || !m.prepareStack(&err)) {
        std::fprintf(stderr, "dispatch-tax: 裸 Machine 初始化失败：%s\n", err.c_str());
        return out;
    }
    for (std::size_t i = 0; i < ks.size(); ++i) {
        const std::vector<std::uint8_t> code = makeBlockLoop(ks[i], codeEaOf(i));
        if (!m.placeCode(codeEaOf(i), code.data(), code.size(), &err)) {
            std::fprintf(stderr, "dispatch-tax: placeCode 失败：%s\n", err.c_str());
            return out;
        }
    }
    // ★ `cpu_.insns` 是**整台 Machine 的累计**（`run()` 不按轮清零），`max_insns` 也是拿它比 ⇒
    //   切片要给**累计**截止值，否则第二片起一进 run() 就"已达上限"（实测：t_block 掉到 0.2 ns）。
    const std::uint64_t base_insns = m.cpu().insns;
    std::uint64_t insns_done = 0;
    int max_slices = 0;
    for (int n : kScanSlices) max_slices = n > max_slices ? n : max_slices;
    {
        const StderrMute mute_stderr;  // 诊断臂每片都会喊一声（见 StderrMute 的注释）
        for (int step = 0; step < max_slices; ++step) {
            for (std::size_t i = 0; i < ks.size(); ++i) {
                if (step >= kScanSlices[i]) continue;
                const std::uint64_t slice_insns = kSliceBlocks * static_cast<std::uint64_t>(ks[i]);
                double slice_ns[2] = {0.0, 0.0};
                for (int o = 0; o < 2; ++o) {
                    const int arm = ((step + static_cast<int>(i)) % 2 == 0) ? o : 1 - o;
                    m.cpu().eip = codeEaOf(i);  // 每片从本档循环的开头进（块缓存已热）
                    setDiagEnv(arm != 0 ? "1" : nullptr);  // 开关在 run() 的 arm() 里，不在热路径
                    const std::uint64_t want = base_insns + insns_done + slice_insns;
                    lift::RunOptions opt;
                    opt.max_insns = want;  // 精确停在块边界
                    const auto t0 = Clock::now();
                    const lift::RunResult r = m.run(opt);
                    const auto t1 = Clock::now();
                    if (r.reason != lift::StopReason::MaxInsns || r.insns != want) {
                        std::fprintf(stderr,
                                     "dispatch-tax: k=%u 的自造循环没有精确跑完"
                                     "（reason=%s insns=%llu 期望 %llu）\n",
                                     ks[i], lift::stopReasonName(r.reason),
                                     static_cast<unsigned long long>(r.insns),
                                     static_cast<unsigned long long>(want));
                        return out;
                    }
                    insns_done += slice_insns;
                    const double ns_slice = std::chrono::duration<double, std::nano>(t1 - t0).count();
                    slice_ns[arm] = ns_slice / static_cast<double>(kSliceBlocks);  // 该片每块 ns
                    out.ns[i][arm].push_back(slice_ns[arm]);
                }
                out.pair_dt[i].push_back(slice_ns[0] - slice_ns[1]);  // 同一片里两臂的每块差
            }
        }
    }
    setDiagEnv(nullptr);
    out.ok = true;
    return out;
}

/** 三点（k=1/3/8）最小二乘拟合 `t_block = b + c·k`。 */
struct Fit {
    double b = 0.0;  // 每块固定税（ns）
    double c = 0.0;  // 每条指令（ns）
};

Fit fitLine(const std::vector<std::pair<double, double>>& pts) {
    const double n = static_cast<double>(pts.size());
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (const auto& p : pts) {
        sx += p.first;
        sy += p.second;
        sxx += p.first * p.first;
        sxy += p.first * p.second;
    }
    const double denom = n * sxx - sx * sx;
    Fit f;
    if (denom == 0.0) return f;
    f.c = (n * sxy - sx * sy) / denom;
    f.b = (sy - f.c * sx) / n;
    return f;
}

/** 运行摘要 → 字符串（`Machine::dumpRunSummary` 写 `std::FILE*`）。 */
std::string captureRunSummary(lift::Machine& m, const lift::RunResult& r) {
    std::FILE* tmp = std::tmpfile();
    if (tmp == nullptr) return {};
    m.dumpRunSummary(r, tmp);
    std::fflush(tmp);
    std::fseek(tmp, 0, SEEK_SET);
    std::string out;
    char buf[4096];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), tmp)) > 0) out.append(buf, n);
    std::fclose(tmp);
    return out;
}

bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

/** 诊断臂 / 正常臂各跑一段裸循环，返回运行摘要文本。 */
std::string runArmSummary(bool skip_lock, lift::RunResult* out) {
    setDiagEnv(skip_lock ? "1" : nullptr);
    lift::Machine m;
    std::string err;
    if (!m.initBare(&err) || !m.prepareStack(&err)) {
        std::fprintf(stderr, "dispatch-tax: 裸 Machine 初始化失败：%s\n", err.c_str());
        return {};
    }
    const std::vector<std::uint8_t> code = makeBlockLoop(4, kCodeEa);
    if (!m.placeCode(kCodeEa, code.data(), code.size(), &err)) {
        std::fprintf(stderr, "dispatch-tax: placeCode 失败：%s\n", err.c_str());
        return {};
    }
    m.cpu().eip = kCodeEa;
    lift::RunOptions o;
    o.max_insns = 200000ull;  // 5 万块 × 4 条
    const lift::RunResult r = m.run(o);
    *out = r;
    return captureRunSummary(m, r);
}

}  // namespace

int main() {
    using namespace lift_test;

    // ---- ① 微基准：未竞争 mutex / steady_clock::now()（量级不变量，⛔ 不断言具体数字）----
    g_current_case = "dispatch-tax/microbench";
    const double lock_ns = measureLockUnlockNs();
    const double clock_ns = measureSteadyClockNs();
    const double token_ns = measureTokenRoundTripNs();
    std::printf("dispatch-tax/microbench: lock+unlock=%.2f ns  steady_clock::now=%.2f ns  "
                "GuestToken acq+rel=%.2f ns  (N=%d × %d 样本，取最小)\n",
                lock_ns, clock_ns, token_ns, kMicroIters, kMicroSamples);
    std::printf("dispatch-tax/microbench: 量级带 lock/clock ∈ [2, 500] ns（⛔ 具体数字不是判据）\n");
    LIFT_CHECK(lock_ns >= 2.0 && lock_ns <= 500.0);
    LIFT_CHECK(clock_ns >= 2.0 && clock_ns <= 500.0);
    LIFT_CHECK(token_ns >= lock_ns);  // 令牌的一轮 = 取锁 + 几个原子记账 ⇒ 不会更快

    // ---- ② 块长扫描：k = 1 / 3 / 8 条/块；两臂（正常 / 诊断）各拟合一次 ----
    //   为什么能在这里做：`Machine` + `placeCode` 是 pure 的，块长由**自造字节**决定 ⇒
    //   不需要改 `tests/bench.cpp`（那是既有 `lift-bench` 的合成体，块长写死 3 条/块）。
    //   ★ 抗噪口径见文件头 ②：片循环把 (档, 臂) 六个格子一起交替 ⇒ 漂移共模；判据取 kScanRounds 轮 Δb 的中位。
    g_current_case = "dispatch-tax/block-scan";
    const std::vector<std::uint32_t> ks = {1u, 3u, 8u};
    std::vector<std::vector<double>> t_off_rounds(ks.size());
    std::vector<std::vector<double>> t_on_rounds(ks.size());
    std::vector<double> b_off_rounds;
    std::vector<double> b_on_rounds;
    std::vector<double> c_off_rounds;
    std::vector<double> c_on_rounds;
    std::vector<double> delta_b;  // 每轮一个 Δb = b_off - b_on（配对口径）
    std::vector<double> all_pair_dt[3];  // 全部片的配对差（每块 ns，off - on）
    for (int round = 0; round < kScanRounds; ++round) {
        const ScanCells cells = measureCells(ks);
        if (!cells.ok) {
            LIFT_CHECK(cells.ok);  // 读数失败本身算红（继续算下去只会拿 0 去拟合）
            return finish("dispatch-tax");
        }
        std::vector<std::pair<double, double>> pts_off;
        std::vector<std::pair<double, double>> pts_on;
        for (std::size_t i = 0; i < ks.size(); ++i) {
            // ★ 每格的每块时间取**逐片的中位**（不是总时长/总块数）：这台机器上单个片被抢占一次
            //   就能把那一格抬高几十 ns/块（实测出现过 +3457 ns/块 的片）；中位对那种尾巴免疫。
            const double off_ns = medianOf(cells.ns[i][0]);
            const double on_ns = medianOf(cells.ns[i][1]);
            pts_off.emplace_back(static_cast<double>(ks[i]), off_ns);
            pts_on.emplace_back(static_cast<double>(ks[i]), on_ns);
            t_off_rounds[i].push_back(off_ns);
            t_on_rounds[i].push_back(on_ns);
        }
        const Fit fo = fitLine(pts_off);
        const Fit fn = fitLine(pts_on);
        b_off_rounds.push_back(fo.b);
        b_on_rounds.push_back(fn.b);
        c_off_rounds.push_back(fo.c);
        c_on_rounds.push_back(fn.c);
        // ★ Δb 用**配对**口径：同一片里两臂相邻测的每块差，逐档取中位再拟合 ⇒ 估的还是
        //   `b_off - b_on`（两个臂各自的 b 之差），但两臂的漂移在配对里已经抵消，噪声小得多。
        std::vector<std::pair<double, double>> pts_dt;
        for (std::size_t i = 0; i < ks.size(); ++i) {
            pts_dt.emplace_back(static_cast<double>(ks[i]), medianOf(cells.pair_dt[i]));
            for (double v : cells.pair_dt[i]) all_pair_dt[i].push_back(v);
        }
        const Fit fdt = fitLine(pts_dt);
        delta_b.push_back(fdt.b);
    }
    setDiagEnv(nullptr);  // 这一段的臂全部由片循环自己设；跑完复位成缺省（正常臂）
    const double fit_b_off = medianOf(b_off_rounds);
    const double fit_b_on = medianOf(b_on_rounds);
    const double fit_c_off = medianOf(c_off_rounds);
    const double fit_c_on = medianOf(c_on_rounds);
    const double d_b = medianOf(delta_b);
    std::printf("dispatch-tax/block-scan: 片循环（每片 %llu 块，片数 k=1:%d / k=3:%d / k=8:%d，"
                "%d 轮取中位）；t_block 单位 ns\n",
                static_cast<unsigned long long>(kSliceBlocks), kScanSlices[0], kScanSlices[1],
                kScanSlices[2], kScanRounds);
    std::vector<double> t_off;
    std::vector<double> t_on;
    for (std::size_t i = 0; i < ks.size(); ++i) {
        const double t_off_i = medianOf(t_off_rounds[i]);
        const double t_on_i = medianOf(t_on_rounds[i]);
        t_off.push_back(t_off_i);
        t_on.push_back(t_on_i);
        // 每档的配对差分布（本轮读数；⛔ 不是判据，判据是 k=1 的中位 + 下界）
        std::vector<double> pd = all_pair_dt[i];
        std::sort(pd.begin(), pd.end());
        std::printf("  k=%u  t_block: 正常=%.2f  跳过令牌锁(diag)=%.2f  Δ=%.2f  "
                    "| 配对差(n=%zu): min=%.2f 中位=%.2f max=%.2f\n",
                    ks[i], t_off_i, t_on_i, t_off_i - t_on_i, pd.size(), pd.front(), medianOf(pd),
                    pd.back());
    }
    std::printf("  fit(正常)     : b=%.2f ns/块  c=%.2f ns/条\n", fit_b_off, fit_c_off);
    std::printf("  fit(diag 跳锁) : b=%.2f ns/块  c=%.2f ns/条\n", fit_b_on, fit_c_on);
    {
        // Δb 的**分布**（只进输出；判据是它的中位 + 下界，见下面那条 CHECK）
        std::vector<double> sorted = delta_b;
        std::sort(sorted.begin(), sorted.end());
        std::printf("  Δb 分布(%d 轮): min=%.2f 中位=%.2f max=%.2f  "
                    "（Δb = b_off - b_on；拟合截距差，噪声与信号同量级 ⇒ 判据是粗的）\n",
                    kScanRounds, sorted.front(), d_b, sorted.back());
        std::printf("  Δb 逐轮:");
        for (double v : delta_b) std::printf(" %.2f", v);
        std::printf("\n");
    }
    // k=1 的配对差 = "每块那一次锁的成本"最干净的载体（判据 (b)）
    const double dt1 = medianOf(all_pair_dt[0]);
    const double dt1_floor = std::max(0.5, kLockCostFloorRatio * lock_ns);
    std::printf("  dt1(k=1 配对差) = %.2f ns/块（下界 = %.2f ns = max(0.5, %.0f%% × lock+unlock)）\n", dt1,
                dt1_floor, kLockCostFloorRatio * 100.0);
    LIFT_CHECK(fit_b_off > 0.5 && fit_b_off < 5000.0);
    LIFT_CHECK(fit_c_off > 0.5 && fit_c_off < 500.0);
    // "块长真的变了"的判据：每块时间必须随块长显著增长（否则三点测的是同一件事）
    LIFT_CHECK(t_off[2] > t_off[0] * 1.2);
    LIFT_CHECK(fit_b_on > 0.0 && fit_b_on < 5000.0);
    // ---- 判据 (a)：拟合截距之差不得明显为负（下界 = 实测均值之下 ~4.5σ，见 kDeltaBTolNs）----
    LIFT_CHECK(d_b >= kDeltaBTolNs);
    // ---- 判据 (b)：跳过锁的那一臂，**每块**那一次锁的成本必须非负（锐判据，见 kLockCostFloorRatio）----
    //   零假设（skip-lock 变 no-op ⇒ 两臂代码相同）⇒ 这条红。
    LIFT_CHECK(dt1 >= dt1_floor);

    // ---- ③④ 诊断开关的默认关 / 打开（**摘要行**是唯一可观测口径）----
    g_current_case = "dispatch-tax/diag-off";
    lift::RunResult r_off;
    const std::string sum_off = runArmSummary(false, &r_off);
    LIFT_CHECK(!sum_off.empty());
    LIFT_CHECK(r_off.reason == lift::StopReason::MaxInsns);
    LIFT_CHECK(contains(sum_off, "token-diag : skip-lock=0"));
    LIFT_CHECK(contains(sum_off, "token     : violations=0 max-concurrent=1"));
    LIFT_CHECK(contains(sum_off, "tokenwait : waits 0 "));

    g_current_case = "dispatch-tax/diag-on";
    lift::RunResult r_on;
    const std::string sum_on = runArmSummary(true, &r_on);
    LIFT_CHECK(!sum_on.empty());
    LIFT_CHECK(r_on.reason == lift::StopReason::MaxInsns);
    LIFT_CHECK(contains(sum_on, "token-diag : skip-lock=1"));
    LIFT_CHECK(contains(sum_on, "token     : violations=0 max-concurrent=1"));
    // 诊断臂里一次都不取锁 ⇒ 不可能有"等锁"（这条同时证明"没有偷偷走 blockWaitNs"）
    LIFT_CHECK(contains(sum_on, "tokenwait : waits 0 "));
    std::printf("dispatch-tax/diag: 默认 skip-lock=0 / 打开 skip-lock=1，两臂都是 waits 0、"
                "violations=0、max-concurrent=1\n");

    // ---- ⑤ 不靠计时：诊断臂真的**绕过**了 mutex（另一条线程能同时拿到令牌）----
    g_current_case = "dispatch-tax/diag-really-skips";
    {
        setDiagEnv("1");
        lift::GuestToken probe;
        probe.arm();
        probe.acquire();
        std::atomic<int> other_got{0};
        std::thread t([&] {
            if (probe.tryAcquire()) {
                other_got.store(1, std::memory_order_release);
                probe.release();
            }
        });
        t.join();
        LIFT_CHECK_EQ_I64(other_got.load(), 1);  // ← 诊断臂：锁被绕过 ⇒ 另一条线程也拿得到
        LIFT_CHECK_EQ_U32(probe.maxConcurrent(), 2u);
        probe.release();  // 本局部令牌到此作废（owner 已被另一线程改过，可能计一次违规；不再使用）
    }
    {
        setDiagEnv(nullptr);
        lift::GuestToken probe;  // 全新实例：正常取锁
        probe.arm();
        probe.acquire();
        std::atomic<int> other_got{0};
        std::thread t([&] {
            if (probe.tryAcquire()) {
                other_got.store(1, std::memory_order_release);
                probe.release();
            }
        });
        t.join();
        LIFT_CHECK_EQ_I64(other_got.load(), 0);  // ← 正常臂：锁真的在，另一条线程拿不到
        LIFT_CHECK_EQ_U32(probe.maxConcurrent(), 1u);
        probe.release();
    }

    // ---- ⑥ 诊断臂里 `--assert-guest-token` 的可信度不变：违规计数照旧 ----
    g_current_case = "dispatch-tax/diag-keeps-assert-token";
    {
        setDiagEnv("1");
        lift::GuestToken probe;
        probe.arm();
        // 令牌之外 check ⇒ 必须照旧计违规（与 mutex 无关；这就是 --assert-guest-token 的判据来源）
        LIFT_CHECK(!probe.check("dispatch-tax/probe-outside"));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(probe.violations()), 1u);
        LIFT_CHECK(probe.firstViolationSite() != nullptr &&
                   std::strcmp(probe.firstViolationSite(), "dispatch-tax/probe-outside") == 0);
        // 持令牌 check ⇒ 不计数（负控：证明上面那条不是恒真）
        probe.acquire();
        LIFT_CHECK(probe.check("dispatch-tax/probe-inside"));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(probe.violations()), 1u);
        probe.release();
    }
    setDiagEnv(nullptr);

    // ---- ⑦ 运行摘要 `present-tail` 的**打印门**（跨平台；判据见文件头 ⑦）----
    g_current_case = "dispatch-tax/present-tail-line";
    {
        lift::Machine m;
        std::string err;
        LIFT_CHECK(m.initBare(&err));
        lift::RunResult r;
        r.reason = lift::StopReason::Stopped;
        r.insns = 1234;
        r.present_tail.last_frame = 7;
        r.present_tail.insns_at_last_present = 1000;
        r.present_tail.tail_insns = 234;
        r.present_tail.tail_wall_ms = 1.5;
        r.present_tail.any = false;  // 本轮 0 次 Present

        const std::string sum_none = captureRunSummary(m, r);
        LIFT_CHECK(!sum_none.empty());
        // 负控：摘要**真的**打出来了（否则"没有 present-tail"是恒真的 —— 空输出也能绿）
        LIFT_CHECK(contains(sum_none, "insns     : 1234"));
        LIFT_CHECK(!contains(sum_none, "present-tail"));  // ← 把 `if (any)` 门删掉 ⇒ 这条红

        r.present_tail.any = true;
        const std::string sum_one = captureRunSummary(m, r);
        const std::size_t at = sum_one.find("present-tail: ");
        LIFT_CHECK(at != std::string::npos);
        std::string line;
        if (at != std::string::npos) {
            const std::size_t eol = sum_one.find('\n', at);
            line = sum_one.substr(at, eol == std::string::npos ? std::string::npos : eol - at);
        }
        // 格式逐字（与裁决里那条正则同一份；⛔ 不放宽成"含 present-tail 就算过"）
        static const std::regex kPresentTail(
            "^present-tail: last_frame=([0-9]+) insns_at_last_present=([0-9]+) tail_insns=([0-9]+) "
            "tail_wall_ms=([0-9]+\\.[0-9])$");
        std::smatch caps;
        LIFT_CHECK(!line.empty() && std::regex_match(line, caps, kPresentTail));
        if (!line.empty() && caps.size() == 5) {
            // 第二道：捕获组必须就是刚塞进去的那四个值（字段顺序 / 名字接错 ⇒ 这里不等）
            LIFT_CHECK_EQ_I64(std::stoll(caps[1].str()), 7);
            LIFT_CHECK_EQ_I64(std::stoll(caps[2].str()), 1000);
            LIFT_CHECK_EQ_I64(std::stoll(caps[3].str()), 234);
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(std::stod(caps[4].str()) * 10.0 + 0.5), 15u);
        }
        std::printf("dispatch-tax/present-tail-line: any=false ⇒ 无该行；any=true ⇒ %s\n", line.c_str());
    }

    std::printf("dispatch-tax: b=%.2f ns/块 c=%.2f ns/条 lock+unlock=%.2f ns clock=%.2f ns "
                "Δb(diag)=%.2f ns dt1=%.2f ns/块\n",
                fit_b_off, fit_c_off, lock_ns, clock_ns, d_b, dt1);
    return finish("dispatch-tax");
}
