// emulator-lift · 起真子进程做断言的用例共用：**环境没有 GPU** 时的 SKIP 判据
//
// ★ 为什么需要：CPU 光栅已退休（`REQ-01M78BF1FNNH6K8N8GEY6GQYXX`），`--present headless`
//   也要 GPU。在没有 GPU 的机器 / `SDL_VIDEODRIVER=dummy` 下，emulator-lift 会在**执行任何
//   被测行为之前** exit 2 + `拒绝启动 —— 没有可用的 GPU 光栅化后端`。那不是被测行为，是
//   **环境缺失** ⇒ 调用方 `LIFT_SKIP(77)`（原因 `无 GPU：子进程拒绝启动`）。
// ★ 判据必须**两侧都成立**（只看退出码会把真正的参数错误也吞掉）：
//     ① 子进程退出非 0，且日志里是 **GPU 拒绝**文案（`拒绝启动` + `GPU 光栅化后端`）；
//     ② **本机环境真的建不出 GPU 设备** —— `hostHasNoGpu()` 实探一次 SDL_GPU（有缓存）。
//   ⇒ GPU 可用时的一次启动回归（本该起来却没起来）仍然红；参数错误仍然红。
// ★ 首次调用才探测（正常路径零成本）；探测 = `SDL_Init(VIDEO)` + `createSdlGpuRaster()`，
//   与产品的"能不能起步"同一判据（⛔ 不拿子进程自己的文案自证）。
#pragma once

#include <memory>
#include <string>

#include <SDL3/SDL.h>

#include "check.h"
#include "runtime/present_sdl.h"

namespace lift_test {

// 本机是否真的没有可用的 GPU 光栅化后端（video 起不来 / 建不出 SDL_GPU device）。
// ★ 结果缓存：同一次测试进程只探一次；⛔ 不要拿它当"每臂都该重探"的开关。
inline bool hostHasNoGpu() {
    static const bool no_gpu = [] {
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            return true;
        }
        std::string probe_err;
        std::unique_ptr<lift::GpuRaster> raster = lift::createSdlGpuRaster(&probe_err);
        const bool unavailable = (raster == nullptr);
        raster.reset();
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return unavailable;
    }();
    return no_gpu;
}

// 子进程因"没有可用的 GPU 光栅化后端"拒绝启动 ⇒ true（调用方应 `LIFT_SKIP(77)`）。
// `log` = 子进程的 stdout+stderr 合并（或至少 stderr）；`exit_code` = 子进程退出码。
inline bool subprocessRefusedForNoGpu(int exit_code, const std::string& log) {
    if (exit_code == 0) return false;  // 没拒绝（正常结束）⇒ 绝不是这条
    const bool gpu_refusal = log.find("拒绝启动") != std::string::npos &&
                             log.find("GPU 光栅化后端") != std::string::npos;
    if (!gpu_refusal) return false;  // 参数错误 / 崩溃 ⇒ 照旧红（⛔ 不许一律当 SKIP）
    return hostHasNoGpu();           // 有 GPU 还拒绝 ⇒ 是回归，照旧红
}

}  // namespace lift_test

// 统一出口：`exit_code` + 日志（stdout+stderr 合并）→ 无 GPU 拒绝启动时 SKIP 77。
// ★ 只能用在返回 `int` 的函数里（`LIFT_SKIP` 会 `return 77`）。
#define LIFT_SKIP_IF_NO_GPU(exit_code, log)                                \
    do {                                                                   \
        if (::lift_test::subprocessRefusedForNoGpu((exit_code), (log)))     \
            LIFT_SKIP("无 GPU：子进程拒绝启动");                             \
    } while (0)
