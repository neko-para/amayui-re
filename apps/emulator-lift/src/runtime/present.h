// emulator-lift · headless 记录 + 可选显示 sink（P3）
//
// ★ 唯一的呈现汇点：D3D9 Present 与 DDraw Flip 都走 `HeadlessPresenter::present()`。
//   `present()` 永远先算 hash / 帧号，然后 **best-effort** 交给可选的 `PresentSink`（真实窗口 / GPU），
//   最后再做可选 BMP dump；显示失败只记账，⛔ 不改变 guest 的 Present 结果。
//   ★ BMP dump 同理（`REQ-01M73YBCC3DZ05Y9NSDTRKTMMY`）：它是**宿主诊断**，写失败**不**让 guest 的
//     Present 失败 —— 否则 `d3d9::present` 会在 `--max-frames` 停机检查之前返回，进程空转到别的
//     停止条件（实测 546 s）。失败**第一次**打默认级别 warn，之后只计数，计数进 run summary
//     （`dumpFailures=`，由 `main` 读 `dumpFailures()` 打印）。
// ★ 本头文件保持 **SDL-free**：所有 pure CTest 都 include 它；SDL 类型只出现在 present_sdl.cpp。
// ★ 单线程契约：sink 只在主线程（Machine::run / shim）被调用；sink 不允许抛异常。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "runtime/gpu_raster.h"
#include "runtime/overlay.h"
#include "runtime/surface.h"

namespace lift {

class FrameSampler;
// ★ 输入计划（`runtime/input_plan.h`）：只为了在 Present 成功那一刻推 `PlanEventKind::Frame`
//   （= `presents` 门的刻度）。⛔ 本头不 include 它（present.h 保持轻；`present.cpp` 里 include）。
class InputPlan;
// ★ 系统对话框的数据（定义在 `runtime/message_box.h`；本头只前向声明 ⇒ 保持 SDL-free）。
struct ModalSpec;

class PresentSink {
public:
    virtual ~PresentSink() = default;
    // 只读消费 guest 像素；false = 这一帧没送出去（err 必须填）。不得修改 guest 内存。
    virtual bool submitFrame(const SurfaceArena& arena, const GuestSurface& surface, std::string* err) = 0;
    // 帧边界 / 块边界的事件泵（headless 无 sink 时不会调用）。
    virtual void pumpHostEvents() {}
    // 用户关窗（或平台 quit）；false = 继续跑。
    virtual bool stopRequested() const { return false; }
    // ★ 宿主侧"像点了 X 一样"请求关窗（`InputScript` 的 `close_window` 动作 / 自动化测试用；
    //   `--input-script` 的 CLI 入口 2026-11-16 已退役）。
    //   默认 no-op（headless 没有窗口）；窗口 sink 走与 `SDL_EVENT_WINDOW_CLOSE_REQUESTED`
    //   **同一条**路（`present_sdl.cpp` 的 `Impl::requestClose`），因此关窗宽限/二次确认语义一致。
    virtual void requestClose() {}
    // 诊断一行（driver / video / gpu），可空。
    virtual std::string describe() const { return {}; }
    // 可选：同一 SDL_GPUDevice 上的 GPU 光栅化后端（生产路径恒非空：无 GPU ⇒ 拒绝启动）。
    virtual GpuRaster* gpuRaster() { return nullptr; }
    // ★ 调试叠层（`--overlay`）的两行数据（`script` 身份 / `worst` 3 s 最大值）。
    //   **默认 no-op**：headless / 测试替身不画叠层，⛔ 不给它们加负担。
    //   叠层开关只有 sink 知道（文本 / 纹理 / 直出都归它）⇒ `Machine` 在 `run()` 开头问一次。
    //   ⛔ 本接口保持 SDL-free（`OverlayStatus` 来自 `runtime/overlay.h`，零 SDL 类型）。
    virtual void setOverlayStatus(const OverlayStatus& /*status*/) {}
    virtual bool overlayEnabled() const { return false; }
    // ★ **系统对话框**（`runtime/message_box.h` 的 `ModalSpec`）：唯一的宿主呈现缝。
    //   窗口 sink 用 `SDL_ShowMessageBox` 实现（会阻塞；实现方负责把阻塞放进 `GuestToken::YieldGuard`），
    //   默认 `false` = 没有系统框（headless / 测试替身）⇒ 调用方立即用默认值 + 一次 warn。
    //   `result` 只在返回 true 时有效；`spec.buttons[i].id` 原样当 SDL 的 buttonid 交回。
    virtual bool showSystemMessageBox(const ModalSpec& /*spec*/, std::uint32_t* /*result*/) { return false; }
    // ★ 取消**已经发起**的那次关窗请求（清 close_pending / 宽限帧 / deadline；窗口留着）。
    //   AGERC `_ShowDialog@12` 回答 2/0（タイトル / キャンセル）时调用；回答 1（終了）不调。
    //   默认 no-op ⇒ headless / 测试替身零改动。
    virtual void cancelCloseRequest() {}
    // ★ 控制层 `resume` 的落点（S4；主线程）：把"恢复时刻"记在 sink 上 ⇒ 早于它入队的 OS 输入
    //   一律丢弃（暂停 10 s 的点击/拖动不许在 resume 首轮压进 guest），暂停期间的关窗 / ESC
    //   当**宿主 stop**（`bye.reason:"window-closed"`）。默认 no-op ⇒ headless / 测试替身零改动。
    virtual void noteControlResume() {}
    // ★ 宿主 stop 的**原因**（控制层 `bye.reason` 用）：nullptr = 不表态（按 run 结果映射）。
    //   只在"暂停期间的关窗 / ESC"这一条路上返回 `"window-closed"`（其余停机原因不归控制层）。
    virtual const char* controlStopReason() const { return nullptr; }
    // ★ `--cursor-warp`（宿主真光标，**缺省关**）：严格是**宿主副作用**，⛔ 不改变 guest 可见状态。
    //   `hostCursorWarpEnabled()` = 这个 sink 现在愿不愿意挪：开关关着 / 没有可见窗口 / 平台不支持 ⇒ false。
    //   `hostCursorWarp(cx,cy)` 的入参是 **guest 客户区像素**（0..image_w-1 的逻辑空间），
    //   ⛔ 不是屏幕坐标 —— 减 guest 窗口原点那一步在 `WindowManager::cursorClientPos`，
    //   加宿主窗口原点与 letterbox 在窗口 sink（两边共用 `runtime/cursor_map.h` 的那一份公式）。
    //   返回 false = 没挪（没有窗口 / 窗口隐藏 / 系统拒绝）；调用方据此决定 `SetCursorPos` 的返回值。
    //   默认 false/false ⇒ headless / 测试替身零改动。
    virtual bool hostCursorWarpEnabled() const { return false; }
    virtual bool hostCursorWarp(int /*guest_client_x*/, int /*guest_client_y*/) { return false; }
};

// ---------------------------------------------------------------------------------------
// 宿主"退出类事件" → 动作的**纯映射**（SDL-free；判据在 pure CTest，不在 SDL 实现里）
//
// ★ 为什么需要它：SDL 3.4.18 在**最后一个窗口**收到关闭请求时**无条件**再推一条
//   `SDL_EVENT_QUIT`（`SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE` 缺省 `"1"`，见 SDL 3.4.18
//   `SDL_hints.h` 的原文："\"1\": SDL will send a quit event when the last window is requesting
//   to close. (default)"；发源在 `src/events/SDL_windowevents.c` 的 `SDL_SendQuit()`）。
//   那条 QUIT 不是"用户要退出应用"，只是"最后一个窗口在请求关闭"的同义反复 ⇒ 只要已经有一条
//   关窗请求在途（`close_pending`），它**不得**收 run —— 否则 guest 一条指令都没机会再跑，
//   `WM_CLOSE` 留在队列里，确认框（guest 自己的 `_ShowDialog(3)`）永远不会被创建。
//   （现场与反汇编锚见 `.tmp/close-path/wm-close.md`。）
// ★ 纯函数 = 合同：换一份呈现实现之后这个映射仍然要成立（`AGENTS.md` §10 的自检三问）。
enum class HostQuitSource {
    WindowCloseRequested,  // SDL_EVENT_WINDOW_CLOSE_REQUESTED（真 ×；也由 requestClose 测试注入）
    AppQuit,               // SDL_EVENT_QUIT（应用级退出；也可能是最后窗口关闭的附带品）
};

enum class HostQuitAction {
    PostWmClose,  // 投一条 guest `WM_CLOSE(0)`，由 guest 的 WndProc 决定要不要退（会弹确认框）
    StopRun,      // 宿主直接收 run（`stopRequested()` 变真）
    Ignore,       // 什么都不做（已经有一条关窗请求在途 / 不可重入）
};

inline HostQuitAction hostQuitAction(HostQuitSource source, bool close_pending) {
    switch (source) {
        case HostQuitSource::WindowCloseRequested:
            // 已有一次关窗请求在途 ⇒ 不重投（同一形状不叠两个确认框）。
            return close_pending ? HostQuitAction::Ignore : HostQuitAction::PostWmClose;
        case HostQuitSource::AppQuit:
            // ★ 本修复的**唯一**合同：`AppQuit` 在 `close_pending` 为真时**不得**产生 stop。
            return close_pending ? HostQuitAction::Ignore : HostQuitAction::StopRun;
    }
    return HostQuitAction::Ignore;
}

// ---------------------------------------------------------------------------------------
// ★ **控制层 resume 的陈旧事件闸门**（S4；纯映射，SDL-free）
//
// 现场（`design-review.md` §2-4）：`pause` 是"完全停"（主线程停在汇点等 cv，`pumpEvents` 一次不跑）
// ⇒ 暂停 10 s 里点/拖窗口的事件全排在 **Win32 队列**里（根本没进 SDL 队列；下一次 `SDL_PumpEvents`
// 才转换进来）；`resume` 首轮 `SDL_PollEvent` 会把它们当"刚发生"：① 关窗事件 ⇒ `postMessage(WM_CLOSE)`
// ⇒ 这一轮直接结束；② ESC 同；③ 10 s 的输入压进同一毫秒并塞进**无上界**的 guest 消息队列。
//
// 判据 = `SDL_Event.common.timestamp` 与"恢复时刻"比（输入事件：Win32 后端用
// `WIN_GetEventTimestamp()` 把戳回溯到投递时刻）：
//   · `< 恢复时刻` 且是 **OS 输入**类 ⇒ 丢弃（⛔ 不进 guest 队列）；
//   · `< 恢复时刻` 且是 **ESC / SDL_QUIT**（用户想退出）⇒ 当**宿主 stop**
//     （`bye.reason:"window-closed"`），⛔ 不投 `WM_CLOSE` 进 guest；
//   · ★ **关窗拿不到陈旧戳**：`WM_CLOSE` → `SDL_SendWindowEvent(…, SDL_EVENT_WINDOW_CLOSE_REQUESTED, 0, 0)`
//     把戳置 0，随后 `SDL_PushEvent` 填**当下**（pump 时刻，必晚于恢复时刻）⇒ `classifyStaleEvent`
//     恒 `Keep` ⇒ 暂停期间的关窗**仍会投 `WM_CLOSE` 进 guest**，故不按陈旧处理。
//     见 `REQ-01M7JNVD59RD49RSKD9N2S9TXW`（纯映射里 `CloseRequest` 仍返回 `HostStop`，只是生产上到不了）。
//   · 其余（resize / expose / render 等）照旧 —— 它们没有"事件洪峰"这回事。
//   `resume_ns == 0`（没启用）/ `timestamp == 0`（SDL 没盖戳的合成事件）⇒ 一律 Keep（不猜）。
// 判据在 `tests/present_window.cpp`（真 SDL 队列 + `SDL_PushEvent` 造陈旧戳），⛔ 不靠肉眼。
// ---------------------------------------------------------------------------------------
enum class HostEventClass {
    Other,         // 非输入（窗口尺寸 / 曝光 / 渲染设备…）
    OsInput,       // 鼠标 / 键盘 / 焦点（会写 guest 输入状态）
    CloseRequest,  // 用户关窗（SDL_EVENT_WINDOW_CLOSE_REQUESTED）
    EscapeKey,     // 键盘 ESC（与关窗同一条用户意图）
    AppQuit,       // SDL_EVENT_QUIT
};

enum class StaleEventAction {
    Keep,      // 照旧处理
    Drop,      // 丢弃（陈旧 OS 输入）
    HostStop,  // 暂停期间的退出意图 ⇒ 宿主收口（不进 guest）
};

inline StaleEventAction classifyStaleEvent(HostEventClass cls, std::uint64_t timestamp_ns,
                                           std::uint64_t resume_ns) {
    if (resume_ns == 0 || timestamp_ns == 0 || timestamp_ns >= resume_ns) return StaleEventAction::Keep;
    switch (cls) {
        case HostEventClass::CloseRequest:
        case HostEventClass::EscapeKey:
        case HostEventClass::AppQuit:
            return StaleEventAction::HostStop;
        case HostEventClass::OsInput:
            return StaleEventAction::Drop;
        case HostEventClass::Other:
            return StaleEventAction::Keep;
    }
    return StaleEventAction::Keep;
}

// ---------------------------------------------------------------------------------------
// 一次 D3D9 Present 的墙钟拆解（`LIFT_FRAME_TRACE=1` 打印；默认关，但**计时始终开着**：
// 每帧 4 次 steady_clock ≈ 几十 ns，可以忽略，换来"Present 还剩在哪一段"是数字不是猜）。
// ★ 只由 `d3d9::present` / `HeadlessPresenter::present` 写、只由 `Machine::flushFrameTrace` 读。
// ---------------------------------------------------------------------------------------
struct PresentBreakdown {
    std::uint64_t readback_ns = 0;  // GPU target → guest 后缓冲（直出且本帧无 GPU 绘制时是 guest → target）
    std::uint64_t hash_ns = 0;      // guest 像素哈希（HeadlessPresenter）
    std::uint64_t freeze_ns = 0;    // 冻结像素副本（只在控制层启用时；缺省 0 = 一次判空）
    std::uint64_t sink_ns = 0;      // sink->submitFrame（SDL 上传+RenderPresent / GPU 直出 blit+submit）
    std::uint64_t stats_ns = 0;     // frameStats 直方图（d3d9 Present 的诊断行）
    bool direct = false;            // 本帧的呈现是否走 GPU 直出（swapchain blit）
};
PresentBreakdown& presentBreakdown();

// ---------------------------------------------------------------------------------------
// 运行摘要 `present-tail` 一行的**打印字段**：`Machine::run` 收口时填，`Machine::dumpRunSummary` 打。
//   口径 = **最后一次 Present 调用 → 本次 run 停**之间的 guest 指令数。
//   ★ 是"**调用**"不是"成功"：`present()` 先 `++frame_count_` 再交给 sink（sink 失败只记账、不回滚），
//     所以 frameCount / 盖章记的都是**调用**（见 `present.cpp` 的 `present()`）。
//   ★ 确定性只在**可复现档**（`--present headless` + 虚拟时钟）成立：那时 `tail_insns` 可进断言；
//     real-time 档（`--present window` / `--real-time` / `--audio sdl`）下三个读数都只是单样本
//     （同命令两轮就会不同）。`tail_wall_ms` 在任何档位都只是宿主墙钟；
//     `last_frame` / `insns_at_last_present` 是**跨 run 累计**值（盖章 append-only，永不清）。
//   ★ `any == false`（本轮一次 Present 都没有）⇒ **不打这一行**；⛔ 不要把"没有这行"读成"尾段为 0"。
// ---------------------------------------------------------------------------------------
struct PresentTail {
    bool any = false;                        // 本轮 run 内是否 ≥1 次 Present（per-run 门 = frameCount 差分）
    std::uint64_t last_frame = 0;            // 最后一次 Present 之后的 frameCount()
    std::uint64_t insns_at_last_present = 0; // Present **那一刻**的 cpu_.insns（没注入源 ⇒ 恒 0）
    std::uint64_t tail_insns = 0;            // = `insns:` − insns_at_last_present（clamp 到 ≥ 0）
    double tail_wall_ms = 0.0;                // Present 那一刻 → run 收口 的宿主墙钟
};

class HeadlessPresenter {
public:
    HeadlessPresenter();
    ~HeadlessPresenter();
    HeadlessPresenter(const HeadlessPresenter&) = delete;
    HeadlessPresenter& operator=(const HeadlessPresenter&) = delete;

    bool present(const SurfaceArena& arena, const GuestSurface& surface, const std::string& dump_path,
                 std::string* err);

    // 转移 sink 所有权（Machine 是唯一装配点；不依赖 main 的声明顺序）。
    void setSink(std::unique_ptr<PresentSink> sink);
    void clearSink();
    bool hasSink() const { return sink_ != nullptr; }
    PresentSink* sink() { return sink_.get(); }
    const PresentSink* sink() const { return sink_.get(); }

    // 块边界的宿主事件泵 + 关窗检查（无 sink ⇒ 零成本）。
    void pumpHostEvents();
    bool stopRequested() const;

    // ★ **控制层 Present 闸门**（S2）：在 sink / dump **之后**、`present()` 返回之前调一次
    //   （`present.cpp` 的落点；理由：`++frame_count_` 与 `last_surface_hash_` 已定稿、帧已提交）。
    //   nullptr = 没有控制层 ⇒ 一次判空（默认档逐位不变）。回调在主线程执行，可以阻塞（暂停）。
    using PresentGateFn = bool (*)(void* user);
    void setPresentGate(PresentGateFn fn, void* user) {
        gate_fn_ = fn;
        gate_user_ = user;
    }
    bool hasPresentGate() const { return gate_fn_ != nullptr; }
    // 控制层 resume 的宿主即时动作（窗口档的陈旧事件闸门；转发给 sink）。
    void noteControlResume();
    // 宿主 stop 的原因（控制层 bye.reason；转发给 sink）。
    const char* controlStopReason() const;

    std::uint64_t frameCount() const { return frame_count_; }
    std::uint64_t lastHash() const { return last_hash_; }
    std::string lastDump() const { return last_dump_; }

    // ★ 运行摘要 `present-tail` 的**盖章**（append-only，永不清）：`present()` 里与 `++frame_count_`
    //   **同一刻**记下 (insns / ns)，frame 就是 `frameCount()`。⛔ 不要在别的层另找一个"Present 那一刻"。
    //   insns 源由 `Machine::wire()` 注入（只读指针，生命周期 = Machine）；没注入 ⇒ 静默留 0
    //   ⇒ 摘要会打出 `insns_at_last_present=0` 且恒等式不成立（由守卫暴露，⛔ 不要在这里猜）。
    void setInsnSource(const std::uint64_t* insns) { insn_source_ = insns; }
    std::uint64_t lastPresentInsns() const { return last_present_insns_; }
    // 最后一次 Present 那一刻 → 现在 的宿主墙钟（ms）；还没 Present 过 ⇒ 0.0。
    double msSinceLastPresent() const;

    // ★ 画面特征采样（`runtime/screen_sample.h`）：采样点只能在这里 —— Present 是**唯一**呈现汇点。
    //   没挂 sampler ⇒ 一次也不读像素（零成本）；⛔ 不要在别的层另找一个"采样时机"。
    void setFrameSampler(FrameSampler* sampler) { sampler_ = sampler; }
    FrameSampler* frameSampler() const { return sampler_; }

    // ★ `--input-plan` 的 `presents` 门：Present **成功那一次**记账（`++frame_count_` 同一刻）。
    //   非拥有指针（计划的生命周期 = 调用方；`nullptr` = 没有计划 ⇒ 一次判空）。⛔ 不在这里
    //   施加任何 step：计划只记账，到期 step 只能由**下一个 guest 读点**消费（`input_plan.h` 铁律 3）。
    void setInputPlan(InputPlan* plan) { input_plan_ = plan; }

    // ★ **冻结最后呈现帧的像素副本**（`REQ-01M7JAGEKMYWWQPMWGV01Y1FNQ`）：唯一产品调用点 =
    //   `aot_run.cpp` 里与 `setPresentGate` 同处同条件（`control_ != nullptr`）。缺省关 ⇒
    //   `present()` 里一次判空、零拷贝。⛔ 不要派生自 `hasPresentGate()`（那样任何新闸门都会
    //   静默背上 3.7 MB/帧）；⛔ `--dump-frame` 不走副本（它是"当场写"的独立对照）。
    void setFreezeFrame(bool on);
    // 诊断：冻结失败**次数**（0 = 一次都没失败）。形状同 `dumpFailures()`：计数 + 只第一次 warn。
    // 失败 ⇒ `frozen_valid_ = false` ⇒ `dumpLastFrame` **退回写现场**（= 今天的行为，⛔ 不新增错误码）。
    std::uint64_t freezeFailures() const { return freeze_failures_; }

    // ★ 补写"最后呈现的那一帧"（交互模型 §2：三种失败都要存最后画面）。
    //   · 冻结有效（控制层启用且这一帧冻成了）⇒ 写**副本**（描述符 + 像素一起冻，⛔ 不读 guest）；
    //   · 没冻结 / 冻结失败 ⇒ **退回写现场**（= 今天的行为，⛔ 不新增错误码 / 不新增 why）。
    //   `hash_still_matches` = **在它实际写出的那份字节上**算 hash，与记录的 `last_surface_hash_` 比
    //   （副本路径下因此恒 true：这张 BMP 就是 presenter 记录的那一帧；现场路径下仍是今天的语义）。
    bool dumpLastFrame(const std::string& host_path, bool* hash_still_matches, std::string* err) const;
    bool hasLastFrame() const { return have_last_surface_; }

    // 诊断：sink 失败次数 / 最后一次错误 / sink 描述（latch 后仍保留）。
    std::uint64_t sinkFailures() const { return sink_failures_; }
    const std::string& lastSinkError() const { return last_sink_error_; }
    std::string sinkDescription() const;

    // ★ 诊断：`--dump-frame` 每帧写文件失败的**次数**（0 = 一次都没失败）。
    //   ⛔ 这不是"跳过导出"的开关：失败只记账 + 第一次 warn，guest 的 Present 照旧成功。
    //   读点：`main` 在 `dumpRunSummary` 之后打 `dumpFailures=<它>`。
    std::uint64_t dumpFailures() const { return dump_failures_; }

private:
    void noteSinkFailure(const std::string& why);
    // 帧导出失败：计数 + **只第一次**打默认级别 warn（⛔ 不每帧刷屏）。
    void noteDumpFailure(const std::string& path, const std::string& why);
    // 冻结失败：计数 + **只第一次** warn（形状照 `noteDumpFailure`）；⛔ 不把更旧的一帧冒充最后一帧，
    // 而是让 `frozen_valid_` 保持 false ⇒ 截图退回写现场。
    void noteFreezeFailure(const std::string& why);
    // 把**这一帧**的像素拷进副本（描述符一起存）。只在 `freeze_` 为真时被 `present()` 调。
    void freezePresentedFrame(const SurfaceArena& arena, const GuestSurface& surface);

    std::unique_ptr<PresentSink> sink_;
    std::uint64_t frame_count_ = 0;
    std::uint64_t last_hash_ = 0;
    std::string last_dump_;
    std::uint64_t sink_failures_ = 0;
    std::string last_sink_error_;
    bool sink_latched_ = false;
    // ★ 帧导出（BMP）失败账：`dump_failures_` 每帧累加；`dump_failure_warned_` 保证只喊一次。
    std::uint64_t dump_failures_ = 0;
    bool dump_failure_warned_ = false;
    // 最后帧的描述符 + 当时的 hash（`dumpLastFrame` 的**退回路径**用）。
    FrameSampler* sampler_ = nullptr;
    // ★ `presents` 门的 emit 点（`--input-plan`；nullptr = 没有计划）。只记账，不施加 step。
    InputPlan* input_plan_ = nullptr;
    const SurfaceArena* last_arena_ = nullptr;
    GuestSurface last_surface_{};
    std::uint64_t last_surface_hash_ = 0;
    bool have_last_surface_ = false;
    // ★ **冻结副本**（只在 `freeze_` 为真时写）：描述符 + 像素一起存 ⇒ 截图时不从现场 surface 重算。
    //   `assign` 复用容量（**不是**每帧一块）。`frozen_valid_ == false` ⇒ 截图退回写现场。
    bool freeze_ = false;
    std::vector<std::uint8_t> frozen_pixels_;
    GuestSurface frozen_desc_{};
    bool frozen_valid_ = false;
    std::uint64_t freeze_failures_ = 0;
    bool freeze_failure_warned_ = false;
    // ★ 控制层 Present 闸门（nullptr = 没有控制层 ⇒ `present()` 里一次判空）。
    PresentGateFn gate_fn_ = nullptr;
    void* gate_user_ = nullptr;
    // ★ `present-tail` 的盖章状态（append-only：只由 `present()` 写，⛔ 从不重置）。
    const std::uint64_t* insn_source_ = nullptr;  // `Machine::wire()` 注入；nullptr ⇒ 盖章恒 0
    std::uint64_t last_present_insns_ = 0;
    std::uint64_t last_present_ns_ = 0;           // 0 = 还没 Present 过（`msSinceLastPresent` 的哨兵）
};

}  // namespace lift
