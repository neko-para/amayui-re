// emulator-lift · **可定界的区间 profiler**（`profile_begin` / `profile_end`）
//
// ★ 为什么有它（2026-11-08）：`--profile` 只给**整轮**的热点（块命中 × 指令数、shim 总墙钟），
//   `LIFT_FRAME_TRACE=1` 只给**逐帧**的粗分。要回答"点进 EU-ROOM 的那 4 s 里谁吃的"，
//   需要一段**由脚本定界**的墙钟，并且分项相加要能闭合到这段墙钟。
//
// ★ 闭式（唯一判据）：
//       wall = guest + shim + native + interp + host + unattributed
//   各项**互斥**（都在 dispatcher 的同一条时间轴上切段），`unattributed = wall - 其余之和`。
//   差额恒等于"没被任何一段覆盖"的时间 ⇒ 允许微小的正/负值（打印出来，⛔ 不吞掉）。
//
// ★ 分项口径（每一项都答"为什么它不会和别的项重复"）：
//   - `guest`   = **块执行**墙钟（dispatcher 里 `for (Insn&)` 那一段）。只记在块循环里。
//   - `shim`    = `ShimRegistry::invoke` 的整段墙钟（**含** Present / 像素循环 / GPU submit）。
//                 shim 里不会再跑 guest 块（嵌套 guest 回调走 `GuestCallStack`，回 dispatcher 执行），
//                 所以与 `guest` 不重叠。`gpu` 那一栏是 Present 的**内部拆解**（⊂ shim，不重复计入）。
//   - `native`  = 函数级 / 段级 native override 的宿主实现（替代了 guest 指令，所以不算 guest）。
//   - `host`    = 音频泵 / 事件泵 / 输入脚本 tick / 定时器（宿主侧、且不是"解释器"）。
//   - `interp`  = 一次 dispatcher 迭代里**剩下的全部**（块缓存查找 / 时钟 / 令牌 / profile 记账 …）。
//                 由 RAII 迭代守卫按"迭代墙钟 − 上面已记的段"算出 ⇒ 覆盖"解释器自身"。
//   - `unattributed` = wall − Σ其它。★ 它不是零：嵌套/异常路径会漏计或重计，所以**必须打印**。
//
// ★ 包容 vs 自身（guest 函数表）：
//   - **self** = 该函数的块执行墙钟（块级计时，按 `.lst` 函数入口聚合）。**互斥**：所有 self 之和 = `guest`。
//   - **inclusive** = 该函数在**栈上**的墙钟（`call` 进入 → `ret` 离开）。按"同一函数只算最外层一帧"
//     累计 ⇒ 递归不会把时间算两遍。inclusive 是**树**（父包含子），⛔ 不能相加，⛔ 不参与上面的闭式。
//   - 影子栈在 `call`（`Op::Call`）压帧、`ret`（`Op::Ret`）弹帧；对不上就计数（`unbalanced`）并
//     **不猜** —— 数会打印出来。
//
// ★ 触发源是**条件**，不是"帧号"（见 input_script.h 的 TriggerKind）：
//   窗口边界 = 脚本动作被触发的那一个 **block 边界**。因此 `at frame` / `at insn` / `at guest_ms`
//   都能当边界；将来加"场景/脚本标记"只需给 Trigger 加一种 kind，本类不用改。
//
// ★ 三张表（REQ-01M6XVZ8AXJNG492WA2QAMTA35）：函数表 / **块级 EA 表** / **父子边表**。
//   - 函数表（`fns`）= 既有口径：块耗时按**函数入口**聚合（`.lst` 命中时是 `LSTFunction::ea`）。
//     它答"哪个函数吃得多"，**答不了**"这一段 inclusive 里是哪一块在跑"。
//   - 块级 EA 表（`blocks`）= 每行一个 **block 入口 EA**，self 仍是互斥的块耗时（求和 = guest 的块耗时），
//     `incl/calls/depth` 取自**包含该块的函数**（影子栈只到函数粒度 ⇒ 块没有自己的 inclusive；
//     ⛔ 打印时必须把这一点写出来，不许让人把 fn 的 incl 读成"这一块的 incl"）。
//     `calls == 0` 的行 = 窗口内**没有进入计数**（尾调用 `jmp` 进入 / 窗口起点已经在它内部）⇒
//     那不是"调用 0 次"，是"进入次数不可见"；打印时**必须显式标记**（`!BLIND`）。
//   - 块级**指令数**（`WindowBlockRow::insns`，2026-11-10 追加）：每行 = 该块在窗口内**执行过的指令数**
//     （不是时间；口径 = `cpu_.insns` 在块内的差值）。窗口内 `cpu_.insns` 还有一类增量**不在任何块里**
//     —— 函数级 / 段级 native override 用 `cpu_.insns += n` 补的"被替代指令数"（含 `native_voice` 的
//     时钟补偿）⇒ 用 `override_insns` 单独计数。闭合式（汇总行逐字打印）：
//         window_insns（end−begin） = Σ block_insns + override_insns + insns_unattributed
//     其中 `insns_unattributed` 允许非 0 但**必须打印**（⛔ 不许用"约等于"把差说过去）。
//   - 父子边表（`edges`）= 影子栈的 (parent fn_key → child fn_key) 墙钟与次数。
//     父 = 压帧**之前**的栈顶帧；栈空时没有父（顶层进入 / 窗口起点在函数内部）⇒ 记 `edge_no_parent`，
//     ⛔ 不静默丢：边表覆盖不到那部分，汇总行必须打出来。
//   - **内存上界**：块表复用既有的 `block_ns_delta_`（= 窗口内不同块 EA 的个数，本来就有）；
//     边表按 distinct (parent, child) 对数增长 ⇒ 加硬上界 `kMaxEdgePairs`，越界计数（`edge_overflow`），
//     ⛔ 不让它无界长。窗口收尾时全部清空。
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace lift {

class BlockCache;
class ShimRegistry;

// 窗口内的一行"guest 函数"记账。self 互斥（求和 = guest）；inclusive 是树（父子重叠，不可相加）。
struct WindowFnRow {
    std::uint32_t key = 0;
    std::uint64_t self_ns = 0;
    std::uint64_t inclusive_ns = 0;
    std::uint64_t calls = 0;  // 只数"最外层"帧的进入次数
};

struct WindowShimRow {
    std::uint32_t id = 0;
    std::uint64_t ns = 0;
    std::uint64_t calls = 0;
};

// 窗口内的一行"块级 EA"记账（REQ-01M6XVZ8AXJNG492WA2QAMTA35）。
// self 互斥（求和 = guest 的块耗时）；incl / calls / depth 是**包含该块的函数**的口径
// （影子栈只到函数粒度 ⇒ 块没有自己的 inclusive ⇒ ⛔ 打印时不许省略这个限定）。
struct WindowBlockRow {
    std::uint32_t ea = 0;       // block 入口 EA（不是函数入口）
    std::uint32_t fn_key = 0;   // 该块归属的函数（`.lst` 命中 = 函数入口；否则 = ea）
    std::uint64_t self_ns = 0;
    std::uint64_t incl_ns = 0;  // 函数 inclusive（块继承）
    std::uint64_t calls = 0;    // 函数最外层进入次数；0 = 窗口内没有进入计数（尾调用 / 窗口起点已在内部）
    std::uint32_t depth = 0;    // 该函数帧在影子栈上的最大深度（1 = 没有父帧）；0 = 未知
    // ★ 该块在窗口内**执行过的指令数**（不是时间）。口径 = `cpu_.insns` 在**块内**的差值
    //   （块里每执行成功一条就 `++cpu_.insns`；块中途 break 掉的剩余指令不算）。
    //   ⛔ 与 `--profile` 摘要里的 `hits × insns.size()` **不是**同一份读数：后者在 `until_ea` /
    //      `--max-insns` / 指令执行失败时会多算（那些路径没有执行完整块）。
    //   ⇒ 块级 `insns` 之和 + override 补账 必须**精确**等于窗口的 `insns end−begin`（见汇总行）。
    std::uint64_t insns = 0;
};

// 窗口内的一条父子边（parent → child 的**函数级**墙钟与次数）。
struct WindowEdgeRow {
    std::uint32_t parent = 0;
    std::uint32_t child = 0;
    std::uint64_t incl_ns = 0;  // 该边下 child 帧的墙钟合计（递归边按帧累计 ⇒ 可能大于 child 的 incl）
    std::uint64_t calls = 0;    // 该边被走的次数
};

struct WindowReport {
    std::string label;
    std::uint64_t wall_ns = 0;
    std::uint64_t guest_ns = 0;
    std::uint64_t shim_ns = 0;
    std::uint64_t native_ns = 0;
    std::uint64_t interp_ns = 0;
    std::uint64_t host_ns[4] = {0, 0, 0, 0};
    std::uint64_t host_total_ns = 0;
    // unattributed = wall − (guest + shim + native + interp + host)
    std::int64_t unattributed_ns = 0;
    std::uint64_t begin_frame = 0;
    std::uint64_t end_frame = 0;
    std::uint64_t begin_insns = 0;
    std::uint64_t end_insns = 0;
    std::uint64_t begin_guest_ms = 0;
    std::uint64_t end_guest_ms = 0;
    std::vector<WindowFnRow> fns;      // 按 self 降序
    std::vector<WindowBlockRow> blocks;  // 块级 EA，按 self 降序（全量；打印时截前 N）
    std::vector<WindowEdgeRow> edges;    // (parent, child)，按父的 inclusive 降序、同父内 child incl 降序
    std::uint64_t block_rows = 0;        // 窗口内不同块 EA 的行数（未截断）
    std::uint64_t block_self_ns = 0;     // 这些行的 self 合计（应与 guest 桶相等）
    // ---- 块级指令数的闭合（REQ-01M6XVZ8AXJNG492WA2QAMTA35）----
    //   window_insns = Σ block_insns + override_insns + insns_unattributed
    std::uint64_t block_insns = 0;        // 块行 `insns` 合计
    std::uint64_t window_insns = 0;       // end_insns − begin_insns（窗口边界上的 cpu_.insns 差）
    std::uint64_t override_insns = 0;     // 窗口内 native override 补进 cpu_.insns 的条数（⛔ 不在任何块里）
    std::int64_t insns_diff = 0;          // window_insns − block_insns
    std::int64_t insns_unattributed = 0;  // insns_diff − override_insns（⛔ 非 0 必须报出来，不许吞）
    std::uint64_t edge_rows = 0;         // 不同 (parent, child) 对数（未截断）
    std::uint64_t edge_parents = 0;      // 不同父的个数（未截断）
    std::uint64_t edge_no_parent = 0;    // 没有父帧的进入次数（顶层进入 / 窗口起点已在函数内部）
    std::uint64_t edge_overflow = 0;     // 超过上界未记的边对数（⛔ 不静默）
    std::vector<WindowShimRow> shims;  // 按 ns 降序
    std::uint64_t gpu_readback_ns = 0;
    std::uint64_t gpu_hash_ns = 0;
    std::uint64_t gpu_stats_ns = 0;
    std::uint64_t gpu_sink_ns = 0;
    std::uint64_t gpu_frames = 0;   // 窗口内 D3D9 Present 次数
    std::uint64_t unbalanced = 0;   // ret 对不上帧的次数（inclusive 的可信度指标）
    std::uint64_t open_at_end = 0;  // 窗口结束时仍在栈上的帧数
    std::uint64_t iterations = 0;
};

class WindowProfiler {
public:
    using Clock = std::chrono::steady_clock;

    // 宿主侧归因的固定槽位（闭集合：加一项就改这里 + 打印）
    enum HostSlot : int {
        HostAudio = 0,      // 音频设备推进 / 喂料泵
        HostEventPump = 1,  // presenter 的宿主事件泵（SDL 事件 → WM_*）
        HostInput = 2,      // 输入脚本 tick（注入本身）
        HostTimer = 3,      // 到期定时器的宿主侧开销（guest 回调体算 guest）
        HostSlotCount = 4,
    };
    static const char* hostSlotName(int slot);

    // ---- 开关 ----
    bool armed() const { return armed_; }
    void setArmed(bool on) { armed_ = on; }
    bool active() const { return active_; }
    bool finished() const { return finished_; }
    const std::string& label() const { return label_; }

    // ---- 窗口（由 Machine 在 block 边界调用）----
    void begin(const std::string& label, std::uint64_t frame, std::uint64_t insns, std::uint64_t guest_ms);
    void end(std::uint64_t frame, std::uint64_t insns, std::uint64_t guest_ms);
    // end 之后由 Machine 调用：把"全局累计量"换成"窗口内增量"
    void finishBlocks(const BlockCache& cache);
    void finishShims(const ShimRegistry& shims);
    void finishPresent();
    // begin 时的基线快照（Machine 调用；必须在任何一段执行之前）
    void snapshotBlocks(const BlockCache& cache);
    void snapshotShims(const ShimRegistry& shims);
    void snapshotPresent();
    // 取走结果（打印一次后清掉）
    bool takeReport(const std::function<std::uint32_t(std::uint32_t)>& fn_key, WindowReport* out);

    // ---- 段记账（窗口外一律 no-op）----
    void addGuest(std::uint64_t ns);
    void addShim(std::uint64_t ns);
    void addNative(std::uint64_t ns);
    void addHost(int slot, std::uint64_t ns);
    // ---- 块级指令数（窗口外一律 no-op）----
    //   `addBlockInsns` 由 dispatcher 在**每个块执行完之后**调一次（n = 该块实际执行的指令数）。
    void addBlockInsns(std::uint32_t ea, std::uint64_t n);
    //   `addOverrideInsns` = native override 直接补进 `cpu_.insns` 的条数（不在任何块里）。
    void addOverrideInsns(std::uint64_t n);
    // dispatcher 迭代边界：`iterationEnd` 把"没被上面任何一段覆盖的墙钟"记进 interp。
    void iterationBegin(Clock::time_point now);
    void iterationEnd(Clock::time_point now);

    // ---- guest 影子调用栈（inclusive；窗口外 no-op）----
    //   `frame_esp` = 这一帧**进入时**的 guest esp（= 被调方入口的栈顶，指向返回地址）。
    //   ★ 为什么需要它：SEH 展开是"不经 `ret` 的跳转"（`_JumpToContinuation` / CONTEXT 装载 /
    //     `RtlUnwind` 的 TargetIp），被跳过的帧永远不会 `ret` ⇒ 只能靠 esp 判断"哪些帧在落地
    //     点之下"（`dropFramesBelowEsp`）。没有它，`inclusive` 会**静默偏大**。
    void pushCall(std::uint32_t fn_key, std::uint32_t ret_ea, std::uint32_t frame_esp = 0);
    void popRet(std::uint32_t eip);
    void popNoReturn(std::uint32_t fn_key);  // native override / shim 调用：没有 ret 指令
    // 当前影子栈深度（守卫 / 诊断用：展开前后必须回得到同一个值）。
    std::size_t depth() const { return stack_.size(); }
    // ★ 展开（不经 `ret` 的跳转）落地：丢掉"进入时 esp 落在新 esp **之下**"的帧（= 被跳过的内层帧），
    //   目标帧与它的调用者保留。返回丢掉的帧数。⛔ 不增加 `unbalanced_`：这是**合法**的丢帧。
    std::size_t dropFramesBelowEsp(std::uint32_t target_esp);

private:
    struct Frame {
        std::uint32_t fn_key = 0;
        std::uint32_t ret_ea = 0;
        // 进入这一帧时的 guest esp（见 `pushCall` / `dropFramesBelowEsp`）。
        std::uint32_t frame_esp = 0;
        // 压帧**之前**的栈顶帧（0 = 没有父帧：顶层进入 / 窗口起点已在函数内部）⇒ 边表的父。
        std::uint32_t parent_key = 0;
        Clock::time_point start{};
    };
    void attributeFrame(const Frame& f, Clock::time_point now);
    void closeAllFrames(Clock::time_point now, bool count_open);
    void recordEdge(std::uint32_t parent, std::uint32_t child);
    static std::uint64_t edgeKey(std::uint32_t parent, std::uint32_t child) {
        return (static_cast<std::uint64_t>(parent) << 32) | static_cast<std::uint64_t>(child);
    }

    bool armed_ = false;
    bool active_ = false;
    bool finished_ = false;

    std::string label_;
    Clock::time_point begin_wall_{};
    Clock::time_point end_wall_{};
    std::uint64_t begin_frame_ = 0;
    std::uint64_t begin_insns_ = 0;
    std::uint64_t begin_guest_ms_ = 0;
    std::uint64_t end_frame_ = 0;
    std::uint64_t end_insns_ = 0;
    std::uint64_t end_guest_ms_ = 0;

    std::uint64_t guest_ns_ = 0;
    std::uint64_t shim_ns_ = 0;
    std::uint64_t native_ns_ = 0;
    std::uint64_t interp_ns_ = 0;
    std::uint64_t host_ns_[HostSlotCount] = {0, 0, 0, 0};
    std::uint64_t iterations_ = 0;

    // 一次 dispatcher 迭代：守卫用"迭代墙钟 − 已记段"补 interp
    Clock::time_point iter_start_{};
    std::uint64_t iter_attributed_ns_ = 0;
    // 上一个迭代的终点：下一个迭代的起点从它算起（⇒ 迭代之间的间隙也进账，不落进 unattributed）
    Clock::time_point prev_end_{};
    bool prev_end_valid_ = false;

    // 快照（全局累计量 → 窗口内增量）
    std::unordered_map<std::uint32_t, std::uint64_t> block_ns_at_begin_;
    std::unordered_map<std::uint32_t, std::uint64_t> block_ns_delta_;
    // 块级指令数（按块入口 EA）。★ 热路径靠 `last_block_*` 缓存：unordered_map 的**元素指针**
    //   在 insert / rehash 后仍然有效 ⇒ 循环里反复回到同一块时只做一次比较 + 一次加。
    std::unordered_map<std::uint32_t, std::uint64_t> block_insns_delta_;
    std::uint32_t last_block_ea_ = 0;
    std::uint64_t* last_block_insns_ = nullptr;
    std::uint64_t override_insns_ = 0;
    std::vector<std::uint64_t> shim_ns_at_begin_;
    std::vector<std::uint64_t> shim_calls_at_begin_;
    std::vector<WindowShimRow> shim_rows_;
    std::uint64_t present_at_begin_[4] = {0, 0, 0, 0};
    std::uint64_t gpu_readback_ns_ = 0;
    std::uint64_t gpu_hash_ns_ = 0;
    std::uint64_t gpu_stats_ns_ = 0;
    std::uint64_t gpu_sink_ns_ = 0;

    // guest 影子调用栈（inclusive）
    std::vector<Frame> stack_;
    std::unordered_map<std::uint32_t, std::uint64_t> incl_ns_;
    std::unordered_map<std::uint32_t, std::uint32_t> top_calls_;   // 当前栈上该函数的帧数（排重用）
    std::unordered_map<std::uint32_t, std::uint64_t> calls_by_fn_;  // 最外层进入次数
    std::unordered_map<std::uint32_t, std::uint32_t> max_depth_;    // 该函数帧的最大栈深度（1 = 无父帧）
    // 父子边：key = (parent << 32) | child。★ 硬上界，越界只计数（kMaxEdgePairs）。
    static constexpr std::size_t kMaxEdgePairs = 4096;
    std::unordered_map<std::uint64_t, std::uint64_t> edge_calls_;
    std::unordered_map<std::uint64_t, std::uint64_t> edge_incl_ns_;  // 只在 edge_calls_ 里有的 key 上累加
    std::uint64_t edge_no_parent_ = 0;
    std::uint64_t edge_overflow_ = 0;
    std::uint64_t unbalanced_ = 0;
    std::uint64_t open_at_end_ = 0;
};

}  // namespace lift
