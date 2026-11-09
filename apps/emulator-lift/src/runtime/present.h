// emulator-lift · headless 记录 + 可选显示 sink（P3）
//
// ★ 唯一的呈现汇点：D3D9 Present 与 DDraw Flip 都走 `HeadlessPresenter::present()`。
//   `present()` 永远先算 hash / 帧号，然后 **best-effort** 交给可选的 `PresentSink`（真实窗口 / GPU），
//   最后再做可选 BMP dump；显示失败只记账，⛔ 不改变 guest 的 Present 结果。
// ★ 本头文件保持 **SDL-free**：所有 pure CTest 都 include 它；SDL 类型只出现在 present_sdl.cpp。
// ★ 单线程契约：sink 只在主线程（Machine::run / shim）被调用；sink 不允许抛异常。
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "runtime/gpu_raster.h"
#include "runtime/surface.h"

namespace lift {

class FrameSampler;

class PresentSink {
public:
    virtual ~PresentSink() = default;
    // 只读消费 guest 像素；false = 这一帧没送出去（err 必须填）。不得修改 guest 内存。
    virtual bool submitFrame(const SurfaceArena& arena, const GuestSurface& surface, std::string* err) = 0;
    // 帧边界 / 块边界的事件泵（headless 无 sink 时不会调用）。
    virtual void pumpHostEvents() {}
    // 用户关窗（或平台 quit）；false = 继续跑。
    virtual bool stopRequested() const { return false; }
    // 诊断一行（driver / video / gpu），可空。
    virtual std::string describe() const { return {}; }
    // 可选：同一 SDL_GPUDevice 上的 GPU 光栅化后端（--raster gpu 时才非空）。
    virtual GpuRaster* gpuRaster() { return nullptr; }
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

private:
    void noteSinkFailure(const std::string& why);

    std::unique_ptr<PresentSink> sink_;
    std::uint64_t frame_count_ = 0;
    std::uint64_t last_hash_ = 0;
    std::string last_dump_;
    std::uint64_t sink_failures_ = 0;
    std::string last_sink_error_;
    bool sink_latched_ = false;
    // 最后帧的描述符 + 当时的 hash（`dumpLastFrame` 用；不持有像素）。
    FrameSampler* sampler_ = nullptr;
    const SurfaceArena* last_arena_ = nullptr;
    GuestSurface last_surface_{};
    std::uint64_t last_surface_hash_ = 0;
    bool have_last_surface_ = false;
};

}  // namespace lift
