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
    // ★ 宿主侧"像点了 X 一样"请求关窗（`--input-script close_window` / 自动化测试用）。
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
};

// ---------------------------------------------------------------------------------------
// 一次 D3D9 Present 的墙钟拆解（`LIFT_FRAME_TRACE=1` 打印；默认关，但**计时始终开着**：
// 每帧 4 次 steady_clock ≈ 几十 ns，可以忽略，换来"Present 还剩在哪一段"是数字不是猜）。
// ★ 只由 `d3d9::present` / `HeadlessPresenter::present` 写、只由 `Machine::flushFrameTrace` 读。
// ---------------------------------------------------------------------------------------
struct PresentBreakdown {
    std::uint64_t readback_ns = 0;  // GPU target → guest 后缓冲（直出且本帧无 GPU 绘制时是 guest → target）
    std::uint64_t hash_ns = 0;      // guest 像素哈希（HeadlessPresenter）
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

    // ★ 补写"最后呈现的那一帧"（交互模型 §2：三种失败都要存最后画面）。
    //   只留**描述符**（几十字节），不拷像素 ⇒ 每帧零成本；代价是像素可能已经是下一帧的绘制中状态，
    //   所以 `hash_still_matches` 如实报告"这些像素还是不是被采样到的那一帧"（不一致也照样写，供诊断）。
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
    // 最后帧的描述符 + 当时的 hash（`dumpLastFrame` 用；不持有像素）。
    FrameSampler* sampler_ = nullptr;
    const SurfaceArena* last_arena_ = nullptr;
    GuestSurface last_surface_{};
    std::uint64_t last_surface_hash_ = 0;
    bool have_last_surface_ = false;
    // ★ `present-tail` 的盖章状态（append-only：只由 `present()` 写，⛔ 从不重置）。
    const std::uint64_t* insn_source_ = nullptr;  // `Machine::wire()` 注入；nullptr ⇒ 盖章恒 0
    std::uint64_t last_present_insns_ = 0;
    std::uint64_t last_present_ns_ = 0;           // 0 = 还没 Present 过（`msSinceLastPresent` 的哨兵）
};

}  // namespace lift
