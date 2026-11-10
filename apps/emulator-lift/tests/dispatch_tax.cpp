// emulator-lift · **pure** 派发层的固定税（b / c）+ 诊断开关（`LIFT_TOKEN_DIAG_SKIP_LOCK`）守卫
//
// 上下文：REQ-01M70S2WR4S8MN2JDWJRJD6WFD（"吞吐的派发层拆分"）。要用一个数回答
//   「每块到底交多少固定税（b）、每条指令多少（c）」以及「把令牌的 mutex 拿掉能省多少」。
//
// 红了意味着什么（判据，缺一条都不算绿）：
//   ① `dispatch-tax/microbench`：**未竞争** `std::mutex` lock+unlock 与 `steady_clock::now()`
//      的每次读数落在**量级带**里（各 N=1,000,000 次 × 5 个样本取中位）。
//      ⛔ 只断言量级（[2, 500] ns），**不断言具体数字、不认机器型号** —— 它变红说明
//      "测的根本不是那把锁 / 那个时钟"（例：lock 循环被优化掉 ⇒ 读数掉到亚纳秒）。
//   ② `dispatch-tax/block-scan`：自造 guest 代码把**块长**做成 1 / 3 / 8 条/块（同一份
//      `Machine` 的块缓存、同一段循环），三点线性拟合 `t_block = b + c·k`；断言 `b > 0`、`c > 0`
//      且都落在量级带里（b ∈ [0.5, 5000] ns，c ∈ [0.5, 500] ns），并断言"每块时间随块长增长"
//      （`t(8) > 1.2 × t(1)`）。⛔ 不断言 b/c 的具体数值 —— 那是**读数**，进文档不进 test。
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
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "check.h"
#include "runtime/guest_token.h"
#include "runtime/machine.h"

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint32_t kCodeEa = 0x00401000u;
constexpr const char* kDiagEnv = "LIFT_TOKEN_DIAG_SKIP_LOCK";

// 微基准：每次样本的迭代数 / 样本数（★ 任务口径：N ≥ 100 万次取中位）。
constexpr int kMicroIters = 1000000;
constexpr int kMicroSamples = 5;

// 块长扫描：每档执行的**块数**（每块 k 条指令 ⇒ insns = blocks × k）/ 每档重复次数（取最小值）。
constexpr std::uint64_t kScanBlocks = 600000;
constexpr int kScanReps = 3;

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

double medianOf(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const std::size_t mid = v.size() / 2;
    return (v.size() % 2 != 0) ? v[mid] : (v[mid - 1] + v[mid]) / 2.0;
}

/** 未竞争 `std::mutex`：lock + unlock 一次多少 ns（样本内取每次的平均，样本间取中位）。 */
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
    return medianOf(std::move(samples));
}

/** `steady_clock::now()` 一次多少 ns（含循环里那次累加：量级带足够宽，见文件头 ①）。 */
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
    return medianOf(std::move(samples));
}

/** 真令牌的一次 acquire+release（未竞争）多少 ns —— 派发层每块付的"那一次取锁"的完整成本。 */
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
    return medianOf(std::move(samples));
}

/**
 * 造一段"每块 k 条指令"的循环：
 *   (k-1) 条 `add eax,1` + 1 条 `jmp rel32`（跳回起点）⇒ 块 = k 条，块边界落在 jmp 上。
 * 同一份代码 / 同一个 EA 反复执行 ⇒ 块缓存**命中**（测的正是派发层，不是解码）。
 */
std::vector<std::uint8_t> makeBlockLoop(std::uint32_t k) {
    std::vector<std::uint8_t> b;
    for (std::uint32_t i = 0; i + 1 < k; ++i) {
        b.push_back(0x83);  // add eax, imm8
        b.push_back(0xC0);
        b.push_back(0x01);
    }
    b.push_back(0xE9);  // jmp rel32
    const std::uint32_t at = kCodeEa + static_cast<std::uint32_t>(b.size());
    const std::int32_t rel = static_cast<std::int32_t>(kCodeEa) - static_cast<std::int32_t>(at + 4u);
    const std::uint32_t rel_bits = static_cast<std::uint32_t>(rel);
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>((rel_bits >> (8 * i)) & 0xFFu));
    return b;
}

/** 跑一段 k 条/块的循环 `blocks` 次，返回**每块**墙钟 ns（σ = 3 次取最小，抗噪）。 */
double measureNsPerBlock(std::uint32_t k) {
    double best = 0.0;
    for (int rep = 0; rep < kScanReps; ++rep) {
        lift::Machine m;
        std::string err;
        if (!m.initBare(&err) || !m.prepareStack(&err)) {
            std::fprintf(stderr, "dispatch-tax: 裸 Machine 初始化失败：%s\n", err.c_str());
            return 0.0;
        }
        const std::vector<std::uint8_t> code = makeBlockLoop(k);
        if (!m.placeCode(kCodeEa, code.data(), code.size(), &err)) {
            std::fprintf(stderr, "dispatch-tax: placeCode 失败：%s\n", err.c_str());
            return 0.0;
        }
        m.cpu().eip = kCodeEa;
        lift::RunOptions o;
        o.max_insns = kScanBlocks * static_cast<std::uint64_t>(k);  // 精确停在块边界
        const auto t0 = Clock::now();
        const lift::RunResult r = m.run(o);
        const auto t1 = Clock::now();
        if (r.reason != lift::StopReason::MaxInsns ||
            r.insns != kScanBlocks * static_cast<std::uint64_t>(k)) {
            std::fprintf(stderr,
                         "dispatch-tax: k=%u 的自造循环没有精确跑完（reason=%s insns=%llu 期望 %llu）\n",
                         k, lift::stopReasonName(r.reason), static_cast<unsigned long long>(r.insns),
                         static_cast<unsigned long long>(kScanBlocks * static_cast<std::uint64_t>(k)));
            return 0.0;
        }
        const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() /
                          static_cast<double>(kScanBlocks);
        if (rep == 0 || ns < best) best = ns;
    }
    return best;
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
    const std::vector<std::uint8_t> code = makeBlockLoop(4);
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
                "GuestToken acq+rel=%.2f ns  (N=%d × %d 样本，取中位)\n",
                lock_ns, clock_ns, token_ns, kMicroIters, kMicroSamples);
    std::printf("dispatch-tax/microbench: 量级带 lock/clock ∈ [2, 500] ns（⛔ 具体数字不是判据）\n");
    LIFT_CHECK(lock_ns >= 2.0 && lock_ns <= 500.0);
    LIFT_CHECK(clock_ns >= 2.0 && clock_ns <= 500.0);
    LIFT_CHECK(token_ns >= lock_ns);  // 令牌的一轮 = 取锁 + 几个原子记账 ⇒ 不会更快

    // ---- ② 块长扫描：k = 1 / 3 / 8 条/块；两臂（正常 / 诊断）各拟合一次 ----
    //   为什么能在这里做：`Machine` + `placeCode` 是 pure 的，块长由**自造字节**决定 ⇒
    //   不需要改 `tests/bench.cpp`（那是既有 `lift-bench` 的合成体，块长写死 3 条/块）。
    g_current_case = "dispatch-tax/block-scan";
    const std::vector<std::uint32_t> ks = {1u, 3u, 8u};
    std::vector<double> t_off;
    std::vector<double> t_on;
    for (std::uint32_t k : ks) {
        setDiagEnv(nullptr);
        t_off.push_back(measureNsPerBlock(k));
        setDiagEnv("1");
        t_on.push_back(measureNsPerBlock(k));
    }
    setDiagEnv(nullptr);
    std::vector<std::pair<double, double>> pts_off;
    std::vector<std::pair<double, double>> pts_on;
    for (std::size_t i = 0; i < ks.size(); ++i) {
        pts_off.emplace_back(static_cast<double>(ks[i]), t_off[i]);
        pts_on.emplace_back(static_cast<double>(ks[i]), t_on[i]);
    }
    const Fit fit_off = fitLine(pts_off);
    const Fit fit_on = fitLine(pts_on);
    std::printf("dispatch-tax/block-scan: blocks/档=%llu，%d 次取最小；t_block 单位 ns\n",
                static_cast<unsigned long long>(kScanBlocks), kScanReps);
    for (std::size_t i = 0; i < ks.size(); ++i) {
        std::printf("  k=%u  t_block: 正常=%.2f  跳过令牌锁(diag)=%.2f  Δ=%.2f\n", ks[i], t_off[i], t_on[i],
                    t_off[i] - t_on[i]);
    }
    std::printf("  fit(正常)     : b=%.2f ns/块  c=%.2f ns/条\n", fit_off.b, fit_off.c);
    std::printf("  fit(diag 跳锁) : b=%.2f ns/块  c=%.2f ns/条  （Δb=%.2f ns ⇒ 每块那一次锁的成本）\n",
                fit_on.b, fit_on.c, fit_off.b - fit_on.b);
    LIFT_CHECK(fit_off.b > 0.5 && fit_off.b < 5000.0);
    LIFT_CHECK(fit_off.c > 0.5 && fit_off.c < 500.0);
    // "块长真的变了"的判据：每块时间必须随块长显著增长（否则三点测的是同一件事）
    LIFT_CHECK(t_off[2] > t_off[0] * 1.2);
    // 诊断臂不该更慢（是"少一把锁"，不是"换了一种锁"）；具体收益是读数，不是判据
    LIFT_CHECK(fit_on.b > 0.0 && fit_on.b < 5000.0);
    LIFT_CHECK(fit_off.b >= fit_on.b);

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

    std::printf("dispatch-tax: b=%.2f ns/块 c=%.2f ns/条 lock+unlock=%.2f ns clock=%.2f ns "
                "Δb(diag)=%.2f ns\n",
                fit_off.b, fit_off.c, lock_ns, clock_ns, fit_off.b - fit_on.b);
    return finish("dispatch-tax");
}
