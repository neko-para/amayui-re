// emulator-lift · **黄金图回归**的比较器与 PNG 编码（`REQ-01M73G49T01W1CQDSME83ES0YD`）
//
// ★ 为什么单开一个模块：GPU 臂的"画面对不对"需要一条**不依赖 CPU oracle**的判据
//   （`docs/00-origin/decisions.md` §8.1 的口径：CPU=oracle / GPU=product）。判据本身是
//   **纯函数**（两张 RGBA + 容差 ⇒ 判定 + 读数），所以与 guest / D3D9 / SDL / 文件系统都无关：
//   `tests/golden_image.cpp` 的 pure 用例直接喂合成的 RGBA / 合成 PNG 给它，
//   端到端 fixture 只负责"起模拟器 → 读 BMP → 调它"。
// ★ **⛔ 不用整帧哈希当唯一判据**：实测同配置两次运行的"亮度档数"出现过 243 vs 248
//   （GPU 驱动 / 浮点路径的正常抖动）⇒ 哈希只会把正常抖动报成回归。这里的判据是**三层**：
//     ① 分块（缺省 16×16）内 `max-channel |Δ|` 的均值 ≤ 容差（局部整块丢失 / 错层的主抓点）；
//     ② 全图 `mean |Δch|` ≤ 容差（整幅偏色 / gamma 错的主抓点）；
//     ③ 单像素 `max-channel |Δ|` 超过 `outlier_delta` 的像素比例 ≤ `max_outlier_ratio`
//        （零散噪点的兜底；⛔ 它不是主判据，块判据才是局部缺陷的主抓点）。
//   ★ 判据是**与**：任何一层不满足都判红，且 `Report` 里逐项留痕（红的时候能看出是哪一层）。
// ★ 只比 **R/G/B**，不比 alpha：`--dump-frame` 出来的 BMP 把 alpha 字节写成 0
//   （`SurfaceArena::dumpBmp`），拿它当基准却比 alpha 只会得到"永远不同"。
// ★ 坐标口径：`RgbaImage` 是 PNG 的 top-down 行序（见 `runtime/png_decode.h`）；BMP 的
//   自下而上行序由调用方（fixture）翻，⛔ 不要在这个模块里猜文件格式。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "runtime/png_decode.h"

namespace lift {
namespace golden {

// 判定阈值（容差的**唯一**落点；fixture / 用例都从这里取，⛔ 不要各自抄一份）。
// ★ 缺省值来自**实测标定**（2026-11-11，见 `.tmp/reveq/REQ-01M73G49T01W1CQDSME83ES0YD/report.md`）：
//   同配置两次连跑逐像素**完全相同**（max |Δ| = 0）⇒ 实测噪声为 0；这里留的余量是给**跨驱动 /
//   跨机器**的（需求口径：跨驱动允许 ≤1 LSB）。⇒ 1 LSB 的均匀差必须仍然判过（pure 用例钉住），
//   而"整块画错"必须判红（pure 用例钉住）。
struct Tolerance {
    // 16×16 块内 `max-channel |Δ|` 均值上限（1.0 = 允许全块 1 LSB 的跨驱动差）。
    double block_mean = 1.5;
    // 全图 `mean |Δch|`（三个通道一起平均）上限。
    double global_mean = 1.5;
    // 单像素 `max-channel |Δ|` 超过它就记成"超阈像素"。
    std::uint32_t outlier_delta = 16;
    // 超阈像素占全图比例上限。
    double max_outlier_ratio = 0.0025;
    // 分块边长（像素）。
    std::uint32_t block_side = 16;
};

// 一次比较的**全部读数**（红的判据 + 报告里要写的实测数字都在这里，⛔ 不另存一份）。
struct Report {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    double global_mean = 0.0;            // 全图 mean |Δch|（R/G/B 一起）
    double block_mean_worst = 0.0;       // 最差块的块内 max-channel |Δ| 均值
    std::uint32_t worst_block_x = 0;     // 最差块左上角（像素）
    std::uint32_t worst_block_y = 0;
    std::uint32_t blocks = 0;            // 块总数（含边缘的残块）
    std::uint32_t failing_blocks = 0;    // 块均值 > tolerance.block_mean 的块数
    std::uint32_t outlier_pixels = 0;    // max-channel |Δ| > tolerance.outlier_delta 的像素数
    double outlier_ratio = 0.0;          // outlier_pixels / (width*height)
    std::uint32_t differing_pixels = 0;  // 至少一个通道不同的像素数
    std::uint32_t max_channel_diff = 0;  // 全图单通道最大差
    // 逐项判定（`size_ok` = 宽高一致且像素数自洽）。
    bool size_ok = false;
    bool block_ok = false;
    bool global_ok = false;
    bool outlier_ok = false;

    // 判据是**与**：四层全过才算过。
    bool pass() const { return size_ok && block_ok && global_ok && outlier_ok; }
};

// **纯函数**：比较 `actual` 与 `golden`（两张 RGBA），结果写进 `*out`（`out` 不许为空）。
// 返回 `out->pass()`。⛔ 不做 I/O、⛔ 不依赖任何全局状态。
bool compare(const png::RgbaImage& actual, const png::RgbaImage& golden, const Tolerance& tolerance,
             Report* out);

// 把 RGBA 编码成 PNG 字节（8-bit / RGBA / top-down）。
// 两个用途：① pure 用例造"合成 PNG"；② `LIFT_UPDATE_GOLDEN=1` 覆盖黄金图。
// 输入不合法（宽高 0 / 像素数与宽高不符 / 分配失败）⇒ `false` + `*why`，⛔ 不抛。
bool encodeRgba8(const png::RgbaImage& image, std::vector<std::uint8_t>* out, std::string* why);

}  // namespace golden
}  // namespace lift
