// emulator-lift · **按帧的 shim 分桶**（`--profile-slow-frames <ms>`；opt-in，缺省关）
//
// ★ 为什么有它（2026-11-18）：`--profile` 的四套归因（`dispatch-top:` / `liftone:` / `shim:` /
//   `frame-ms:`）与 `profile_begin/end` 的**窗口**桶都只有"整轮"或"窗口"口径。
//   blind wipe 剖面的**第二段卡**是 ~10 帧 × ~120 ms 而 guest 几乎空闲
//   （`docs/03-emulator/wipe-profile.md`）—— 一帧 120 ms 摊到 1800 ms 的窗口里被 ~118 个轻帧
//   稀释成 15–25 ms/帧 ⇒ 窗口的 `shim:` 具名行**指不出**"这一帧是哪个 shim"。
//   本仪器补的就是这一格：**只对慢帧**保留**这一帧**的 shim 时间按名字分桶（+ 调用次数）。
//
// ★ 口径（唯一一份；`docs/03-emulator/wipe-profile.md` 的那一节照这里写）：
//   · **墙钟** = `profileProcessMs()` 的**帧间差** —— 与既有 `frame-ms:` 的 `t_ms` 同一把尺
//     ⇒ 阈值可以直接与 `frame-ms` 算出来的 Δt 对照。⛔ 不另起一套时钟。
//   · **shim 时间** = `aot_run.cpp` 里那条**已有的**精确计时（`windowNowNs()` 包住 `lift_shim_id`；
//     窗口 profile 的 `shim` 桶就是同一对取时）⇒ 两处**共用一次取时**，不是第二套仪器。
//   · **只统计主线程**的 shim：打点在主执行循环（`AotHost::run`）里 ⇒ 音频 feeder 线程的 shim
//     调用**结构上进不来**（feeder 走 `aot_feeder.cpp` 自己的 IR 执行器，不经过这条循环）。
//   · 一次取时的仪器税（~几十 ns/调用）**留在读数里**（⛔ 不扣）：这是诊断档，不做任何性能判据。
//   · **有界**：慢帧最多 `kMaxSlowFrames` 条、每帧最多 `kTopN` 个名字；超出的**只报数**
//     （⛔ 不静默丢、也⛔ 不让它无界长）。
//   · **缺省关**：`on == false` 时 `noteShim` / `endFrame` 各自只是一次 bool 判断 ——
//     一次取时都不多做，一行都不打（与既有 `--profile` 同一条口径）。
//
// ★ 为什么拆成 TU（`frame_bucket.cpp`）而不是纯头：纯 CTest `lift-aot-frame-bucket` 要直接驱动
//   这两个纯函数 + 有界状态机（不取时、不碰 GPU / 注册表），而产品臂只做"喂数 + 打表"。
#ifndef LIFT_AOT_FRAME_BUCKET_H
#define LIFT_AOT_FRAME_BUCKET_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace lift::aot {

// 一帧里某个 shim 的账（按 `ShimDef::id` 索引；`id` 就是注册表下标）。
struct FrameShimRow {
    std::uint32_t id = 0;
    std::uint64_t ns = 0;
    std::uint64_t calls = 0;
};

// 具名行：**按名字合并后**的一行（不同的 id 可以同名 —— 例如运行期注册的 COM thunk
// 共用同一个 `module!name`；⛔ 不要按 id 分行打印，那会把同一个名字拆成几行）。
struct NamedShimRow {
    std::string name;
    std::uint64_t ns = 0;
    std::uint64_t calls = 0;
};

// id → 名字（产品臂给的是 `ShimRegistry::byId`；测试给的是小向量）。
using ShimNameFn = std::function<std::string(std::uint32_t)>;

// ---- 纯函数（无时钟 / 无注册表；⛔ 不要在产品路径里另写一份）----

// ① 按**名字**合并：同名相加 `ns` / `calls`，去掉 `ns == 0 && calls == 0` 的行，
//    排序 = `ns` 降序 → `calls` 降序 → 名字升序（确定性 ⇒ 同输入同输出）。
std::vector<NamedShimRow> mergeShimUsageByName(const std::vector<FrameShimRow>& rows,
                                               const ShimNameFn& name_of);

// ② 取前 `top_n` 个（**有界**；不改入参、不排序 —— 入参必须已经是 ① 的顺序）。
std::vector<NamedShimRow> takeTopN(const std::vector<NamedShimRow>& merged, std::size_t top_n);

// 一条慢帧（只存**通过阈值**且没被上界丢掉的帧）。
struct SlowFrameRow {
    std::uint64_t frame = 0;      // Present 计数（与 `frame-ms:` 的 `n` 同一份）
    double wall_ms = 0.0;         // 该帧宿主墙钟 = `t_ms` 的帧间差
    std::uint64_t insns = 0;      // 该帧 guest 指令数（`cpu_.insns` 的帧间差）
    std::uint64_t presents = 1;   // 这一格里发生了几次 Present（>1 = 一次 shim 里多次 Present）
    std::uint64_t shim_ns = 0;    // 该帧主线程 shim 的精确墙钟合计
    std::uint64_t shim_calls = 0; // 该帧主线程 shim 的调用次数合计
    std::size_t shim_rows = 0;    // 按名字合并后的行数（> `top.size()` ⇒ 还有未展开的）
    std::vector<NamedShimRow> top;  // ≤ kTopN，按 ns 降序
};

struct FrameBucketProf {
    // ★ 有界：慢帧表最多这么多条（超出的只计数）、每帧最多这么多名字。
    //   为什么要上界：一个"每帧都慢"的 run 会把 300 帧的全量 shim 表撑成几十 MB 的字符串树，
    //   而这张表只在人看的时候有意义 ⇒ 截断 + 报数就够。
    static constexpr std::size_t kMaxSlowFrames = 32;
    static constexpr std::size_t kTopN = 8;

    bool on = false;
    std::uint64_t threshold_ms = 0;
    // ---- 读数 ----
    std::uint64_t frames_seen = 0;     // 走过的帧数（= 判断过阈值的次数）
    std::uint64_t slow_frames = 0;     // ≥ 阈值的帧数（**含**超上界未保存的）
    std::uint64_t frames_dropped = 0;  // 超上界未保存的慢帧数（⛔ 不静默）
    std::vector<SlowFrameRow> rows;    // ≤ kMaxSlowFrames

    // ---- 当前帧的累计（按 shim id；热路径只做一次下标判断 + 两次加）----
    std::vector<std::uint64_t> cur_ns;
    std::vector<std::uint64_t> cur_calls;

    void reset() { *this = FrameBucketProf{}; }

    // 开档：`threshold_ms == 0` ⇒ 关（与"没给这个 flag"同一条：一次取时都不多做）。
    // `now_ms` / `insns` = run 起点（第一帧的墙钟 / insns 从它算起）。
    void begin(std::uint64_t threshold_ms_arg, double now_ms, std::uint64_t insns,
               std::size_t shim_count);

    // 一次主线程 shim 调用（`ns` = 精确区间）。关档 / 未开帧时是 no-op。
    // `id` 越界（运行期 `addRuntime` 之后 id 变大）⇒ 惰性扩容，⛔ 不丢这笔账。
    void noteShim(std::uint32_t id, std::uint64_t ns);

    // 帧结束。`now_ms` = `profileProcessMs()`（**同一把尺**），`insns` = `cpu_.insns`（累计值）。
    // 返回 true = 这一帧被判成慢帧（**不管有没有存下来**：超过上界时 `frames_dropped` 会涨）。
    bool endFrame(std::uint64_t frame, double now_ms, std::uint64_t insns, std::uint64_t presents,
                  const ShimNameFn& name_of);

private:
    void clearFrame();
    double prev_ms_ = 0.0;
    std::uint64_t prev_insns_ = 0;
};

}  // namespace lift::aot

#endif  // LIFT_AOT_FRAME_BUCKET_H
