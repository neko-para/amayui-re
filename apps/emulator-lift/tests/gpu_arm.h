// emulator-lift · **真 GPU 臂**的公共夹具（只给"保留的像素断言"用）
//
// 背景（`REQ-01M7A74YG08R65NJBFXF7DEJDK`）：CPU 光栅整族已删 ⇒ 夹具**没装 `GpuRaster`** 时
// `gpuEligible` 会以 `kUnGpuNoBackend` 拒批（Draw 回 `D3DERR_INVALIDCALL`、一个像素都不写）。
// ⇒ 还想断言"像素真的画对了"的用例必须**装真后端**：隐藏窗口 + `SDL_GPUDevice`（形制照
// `tests/gpu_batch.cpp` 的 `caseGpuLinesPoints` / `caseGpuRenderToTexture`）。
//
// ★ **只有"环境缺失"才 SKIP**：`win.init` 失败（无窗口系统 / 无 GPU 驱动 / 建不出 device）⇒ `attach`
//   返回 false，调用方 `LIFT_SKIP(why)`（CTest 侧 `SKIP_RETURN_CODE 77`）。
//   ⛔ "device 建出来了但坏了"一律**硬失败**（`failf` ⇒ `g_failures > 0` ⇒ 调用方的 `LIFT_SKIP`
//   在 `tests/check.h:114-118` 变成 `return 1`）：`rasterActive()==false`（shader 建不出）、
//   `rasterPrepare` / `rasterSyncFromGuest` 失败、`sync` 的读回失败。那些是**真 bug**，不是缺资产
//   —— 当 SKIP 会把 22 条像素断言整体变成"跳过"而 CI 全绿（第二类假绿）。
// ★ 读像素前**必须** `sync()`：GPU 的 Draw 只入队（合批），不读回就拿到的是上一批的 guest 内存
//   —— 对"期望清屏色"的断言会**假绿**（最危险的一类）。
#pragma once

#include <cstdint>
#include <string>

#include "check.h"
#include "runtime/machine.h"
#include "runtime/present_sdl.h"
#include "runtime/surface.h"

namespace lift_test {

// 逐通道容差比较（X8R8G8B8/A8R8G8B8 在 X8 上 A 字节是 0 ⇒ 只比 R/G/B）。
// ★ 容差是**新加的**（GPU 的 float blend / unorm 舍入 vs D3D9 的 8-bit 参考），⛔ 不是把期望改小。
inline bool pxNear(std::uint32_t got, std::uint32_t want, int tol) {
    for (int sh = 0; sh < 24; sh += 8) {
        const int d = static_cast<int>((got >> sh) & 0xFFu) - static_cast<int>((want >> sh) & 0xFFu);
        if (d < -tol || d > tol) return false;
    }
    return true;
}

inline bool pxNear(std::uint32_t got, unsigned r, unsigned g, unsigned b, int tol) {
    return pxNear(got, (r << 16) | (g << 8) | b, tol);
}

// 隐藏窗口 + 真 GPU 后端的 RAII 薄封装：`attach` 之后 `Machine` 的 Draw 才会进 GPU 批。
struct GpuArm {
    lift::SdlWindow win;
    lift::GuestSurface* back = nullptr;
    bool started = false;
    bool attached = false;

    // 建隐藏窗口的 SDL_GPU 后端（`raster_gpu=true`）。失败 = 环境缺失（调用方 SKIP 77）。
    bool start(std::string* why) {
        if (started) return true;
        lift::SdlWindowOptions opt;
        opt.title = "emulator-lift d3d9 pixel arm";
        opt.driver_list = "software";
        opt.hidden = true;
        opt.raster_gpu = true;
        std::string err;
        if (!win.init(opt, &err)) {
            if (why) *why = err.empty() ? std::string("win.init 失败") : err;
            return false;
        }
        if (!win.rasterActive()) {
            // ★ 走到这里 = `win.init` **成功**（SDL_GPU device 建出来了）但 shader / 后端初始化失败 ⇒
            //   **真 bug**，不是环境缺失。`LIFT_SKIP` 见到 `g_failures > 0` 会改成 `return 1`。
            lift_test::failf(__FILE__, __LINE__,
                             "SDL_GPU 设备建出来了但 rasterActive()==false（shader/后端初始化失败）");
            win.shutdown();
            if (why) *why = "rasterActive == false（设备在、shader/后端起不来 ⇒ 见上面的 FAIL）";
            return false;
        }
        started = true;
        return true;
    }

    // 把 `m` 的光栅接到本窗口，并把 `arena`（后缓冲）的 guest 像素作为目标与初值。
    bool attach(lift::Machine& m, std::uint32_t arena, std::string* why) {
        if (!start(why)) return false;
        back = m.surfaces().find(arena);
        if (back == nullptr) {
            if (why) *why = "找不到后缓冲表面";
            return false;
        }
        m.setGpuRaster(win.gpuRaster());
        std::string err;
        std::uint8_t* px = m.surfaces().pixels(*back);
        if (!win.rasterPrepare(back->format, back->width, back->height, &err, px)) {
            lift_test::failf(__FILE__, __LINE__, "rasterPrepare 失败（device OK）：%s", err.c_str());
            if (why) *why = "rasterPrepare: " + err;
            m.setGpuRaster(nullptr);
            return false;
        }
        if (!win.rasterSyncFromGuest(px, back->pitch, back->width, back->height, &err)) {
            lift_test::failf(__FILE__, __LINE__, "rasterSyncFromGuest 失败（device OK）：%s", err.c_str());
            if (why) *why = "rasterSyncFromGuest: " + err;
            m.setGpuRaster(nullptr);
            return false;
        }
        attached = true;
        return true;
    }

    // flush 队列 + 读回 guest 像素（读像素前的**唯一**入口）。
    bool sync(lift::Machine& m, std::string* why) {
        if (!attached || back == nullptr) {
            if (why) *why = "GpuArm 未 attach";
            return false;
        }
        std::uint8_t* px = m.surfaces().pixels(*back);
        std::string err;
        if (win.rasterSyncToGuest(px, back->pitch, back->width, back->height, &err)) return true;
        // ★ 读回失败 = 后端真坏了（不是缺资产）⇒ 硬失败；调用方的 `LIFT_SKIP` 会见 `g_failures > 0` 转红。
        lift_test::failf(__FILE__, __LINE__, "rasterSyncToGuest 失败（读回坏了）：%s", err.c_str());
        if (why) *why = err;
        return false;
    }
};

}  // namespace lift_test
