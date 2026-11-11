// emulator-lift · **pure** 守卫：派生 mip 链（`REQ-01M73G2724479XAVS176WBZ65Q`）
//
// 判据（红了意味着什么；缺一条都不算）：
//   ① **box filter 逐像素**：2×2 → 1×1 的四个通道（含 alpha）= `(sum + n/2) / n`；
//      符号/取整错一条就红（这是"缩小采样看到的是不是平均"的唯一机械证据）；
//   ② 3×3 → 1×1 按**覆盖范围**取整块（不丢最后一行/列）；
//   ③ 链的级数 / 各级尺寸 / 行距 / 偏移 / 总字节 = `w>>i × h>>i`（最小 1×1）之和；
//   ④ LOD 在 **1:1 / 2:1 / 4:1** 三档给出的级号 = 0 / 1 / 2（★ 1:1 必须仍是 level 0 ——
//      这是"不改变已经正确的情形"的判据）；放大、非有限、越界都被 clamp；
//   ④b **真实右侧批次的几何**（`REQ-01M73JV31QVQX206Y3YG5QDMND`）：屏幕 200×640 上
//      跨 200×1818 texel ⇒ LOD 1.506 ⇒ 2048²(12 级) 的最近级 = **2**（只有一级 ⇒ 0）；
//   ⑤ 屏幕空间 UV 导数是**透视校正**的（`u = U/W`）：w≠1 的三角形上必须与手算的
//      `du/dx = (U_x - u·W_x)/W` 一致，且**不等于**"先按顶点除以 w 再做仿射梯度"的值。
// ★ 全部是纯函数（不建 Machine、不碰 guest / SDL）⇒ 顶层无条件注册 ⇒ pure 档。
//   `tools/mutate-check.mjs` 有对应条目（把 LOD 恒取 0）⇒ 本用例会红。
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

#include "check.h"
#include "runtime/d3d9.h"
#include "runtime/d3d9_pipeline.h"

namespace {

constexpr double kEps = 1e-9;

bool closeTo(double a, double b, double eps = kEps) { return std::fabs(a - b) <= eps; }

// ---- ① 2×2 → 1×1 的 box filter 逐像素（B,G,R,A 四个通道各一个读数）----
void caseBoxFilter2x2() {
    lift_test::g_current_case = "d3d9-mip/box-2x2-to-1x1";
    // 源：BGR A，四像素互不相同（任何"只取左上"或"丢掉 alpha"的实现都会红）
    const std::uint8_t src[4][4] = {
        {10, 20, 30, 40},  // (0,0)
        {12, 22, 32, 42},  // (1,0)
        {14, 24, 34, 44},  // (0,1)
        {16, 26, 36, 46},  // (1,1)
    };
    std::uint8_t dst[4] = {0xEE, 0xEE, 0xEE, 0xEE};
    lift::d3d9::mipDownsampleBox(&src[0][0], 2, 2, 8, dst, 1, 1, 4, 4);
    // sum = 52/92/132/172 ⇒ (sum + 2) / 4 = 13/23/33/43
    LIFT_CHECK_EQ_U32(dst[0], 13u);
    LIFT_CHECK_EQ_U32(dst[1], 23u);
    LIFT_CHECK_EQ_U32(dst[2], 33u);
    LIFT_CHECK_EQ_U32(dst[3], 43u);

    // 取整口径：0+1+2+3 = 6 ⇒ (6 + 2) / 4 = 2（不是 1，也不是 3）
    const std::uint8_t r[4][4] = {{0, 0, 0, 0}, {1, 0, 0, 0}, {2, 0, 0, 0}, {3, 0, 0, 0}};
    std::uint8_t one[4] = {0xEE, 0xEE, 0xEE, 0xEE};
    lift::d3d9::mipDownsampleBox(&r[0][0], 2, 2, 8, one, 1, 1, 4, 4);
    LIFT_CHECK_EQ_U32(one[0], 2u);
    // 1+1+1+0 = 3 ⇒ (3 + 2) / 4 = 1（1.0 的下取整也是 1；这里钉住"不是 0"）
    const std::uint8_t r2[4][4] = {{1, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 0, 0}, {0, 0, 0, 0}};
    std::uint8_t one2[4] = {0xEE, 0xEE, 0xEE, 0xEE};
    lift::d3d9::mipDownsampleBox(&r2[0][0], 2, 2, 8, one2, 1, 1, 4, 4);
    LIFT_CHECK_EQ_U32(one2[0], 1u);

    // alpha 也参与（上例 dst[3] 已经是 alpha 的平均；再钉一条"alpha 全同 ⇒ 不漂"）
    const std::uint8_t a[4][4] = {{0, 0, 0, 7}, {0, 0, 0, 7}, {0, 0, 0, 7}, {0, 0, 0, 7}};
    std::uint8_t adst[4] = {0, 0, 0, 0};
    lift::d3d9::mipDownsampleBox(&a[0][0], 2, 2, 8, adst, 1, 1, 4, 4);
    LIFT_CHECK_EQ_U32(adst[3], 7u);

    // 尺寸对不上（dst 不是"减半"）⇒ 什么都不做（⛔ 不猜调用方想要别的缩放）
    const std::uint8_t src4[16][4] = {};
    std::uint8_t untouched[4] = {0x11, 0x22, 0x33, 0x44};
    lift::d3d9::mipDownsampleBox(&src4[0][0], 4, 4, 16, untouched, 1, 1, 4, 4);
    LIFT_CHECK_EQ_U32(untouched[0], 0x11u);
    LIFT_CHECK_EQ_U32(untouched[3], 0x44u);
}

// ---- ② 奇数尺寸按覆盖范围取整块 ----
void caseBoxFilterOdd() {
    lift_test::g_current_case = "d3d9-mip/box-odd";
    // 3×3 → 1×1：九个数 0..8 ⇒ sum = 36 ⇒ (36 + 4) / 9 = 4（平均 4.0）
    std::uint8_t src[9][4] = {};
    for (int i = 0; i < 9; ++i) {
        src[i][0] = static_cast<std::uint8_t>(i);
        src[i][3] = 255;
    }
    std::uint8_t dst[4] = {0xEE, 0xEE, 0xEE, 0xEE};
    lift::d3d9::mipDownsampleBox(&src[0][0], 3, 3, 12, dst, 1, 1, 4, 4);
    // 若实现只取 2×2（丢掉最后一行/列）⇒ sum = 0+1+3+4 = 8 ⇒ 2 ⇒ 红
    LIFT_CHECK_EQ_U32(dst[0], 4u);
    LIFT_CHECK_EQ_U32(dst[3], 255u);
}

// ---- ③ 链的几何：级数 / 尺寸 / 行距 / 偏移 / 总字节 ----
void caseChainGeometry() {
    lift_test::g_current_case = "d3d9-mip/chain-geometry";
    using lift::d3d9::mipChainBytes;
    using lift::d3d9::mipLevelCount;
    using lift::d3d9::mipLevelHeight;
    using lift::d3d9::mipLevelOffset;
    using lift::d3d9::mipLevelPitch;
    using lift::d3d9::mipLevelWidth;
    LIFT_CHECK_EQ_U32(mipLevelCount(1u, 1u), 1u);
    LIFT_CHECK_EQ_U32(mipLevelCount(2u, 2u), 2u);
    LIFT_CHECK_EQ_U32(mipLevelCount(2048u, 2048u), 12u);
    LIFT_CHECK_EQ_U32(mipLevelCount(350u, 35u), 9u);   // 350→1 需要 9 级
    LIFT_CHECK_EQ_U32(mipLevelCount(2048u, 1u), 12u);  // 一维长条：跟着最长轴
    LIFT_CHECK_EQ_U32(mipLevelWidth(350u, 3u), 43u);
    LIFT_CHECK_EQ_U32(mipLevelHeight(35u, 6u), 1u);   // 最小 1（⛔ 不出现 0）
    LIFT_CHECK_EQ_U32(mipLevelWidth(4u, 9u), 1u);
    LIFT_CHECK_EQ_U32(mipLevelPitch(2048u, 32u, 1u), 4096u);  // 1024 texel × 4 B
    LIFT_CHECK_EQ_U32(mipLevelPitch(350u, 32u, 0u), 1400u);
    LIFT_CHECK_EQ_U32(mipLevelOffset(4u, 4u, 32u, 1u), 0u);
    LIFT_CHECK_EQ_U32(mipLevelOffset(4u, 4u, 32u, 2u), 16u);   // level1 = 2×2 × 8 B/行 = 16
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mipChainBytes(2u, 2u, 32u)), 4);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mipChainBytes(4u, 4u, 32u)), 16 + 4);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mipChainBytes(1u, 1u, 32u)), 0);
    // 2048² A8R8G8B8 的链 ≈ 5.59 MiB（只钉"量级与来源"，⛔ 这不是"当前实现的某个数"）
    const std::int64_t big = static_cast<std::int64_t>(mipChainBytes(2048u, 2048u, 32u));
    LIFT_CHECK(big > 5 * 1024 * 1024 && big < 6 * 1024 * 1024);
}

// 由"屏幕三角形 + 该三角形的 uv（已乘 1/w）"直接算出级号（与光栅里那条路同一组纯函数）。
std::uint32_t levelForTriangle(const double vx[3], const double vy[3], const double invw[3],
                               const double u_over_w[3], const double v_over_w[3], double tex_w, double tex_h,
                               std::uint32_t levels) {
    double dux = 0.0, dvx = 0.0, duy = 0.0, dvy = 0.0;
    if (!lift::d3d9::screenUvDerivatives(vx, vy, invw, u_over_w, v_over_w, &dux, &dvx, &duy, &dvy)) return 0u;
    const double lod = lift::d3d9::lodFromUvDerivatives(dux, dvx, duy, dvy, tex_w, tex_h);
    return lift::d3d9::mipLevelForLod(lod, levels);
}

// ---- ④ LOD 在 1:1 / 2:1 / 4:1 三档的级号 ----
void caseLodLevels() {
    lift_test::g_current_case = "d3d9-mip/lod-levels";
    using lift::d3d9::lodFromUvDerivatives;
    using lift::d3d9::mipLevelForLod;

    // 1024×1024 纹理画在 1024×1024 屏幕上 ⇒ 1 texel/像素 ⇒ LOD 0 ⇒ level 0
    LIFT_CHECK(closeTo(lodFromUvDerivatives(1.0 / 1024.0, 0.0, 0.0, 1.0 / 1024.0, 1024.0, 1024.0), 0.0));
    // 2:1（每屏幕像素 2 texel）⇒ LOD 1
    const double lod2 = lodFromUvDerivatives(2.0 / 1024.0, 0.0, 0.0, 2.0 / 1024.0, 1024.0, 1024.0);
    LIFT_CHECK(closeTo(lod2, 1.0));
    // 4:1 ⇒ LOD 2
    const double lod4 = lodFromUvDerivatives(4.0 / 1024.0, 0.0, 0.0, 4.0 / 1024.0, 1024.0, 1024.0);
    LIFT_CHECK(closeTo(lod4, 2.0));
    const std::uint32_t levels = 12u;
    LIFT_CHECK_EQ_U32(mipLevelForLod(0.0, levels), 0u);
    LIFT_CHECK_EQ_U32(mipLevelForLod(lod2, levels), 1u);
    LIFT_CHECK_EQ_U32(mipLevelForLod(lod4, levels), 2u);
    LIFT_CHECK_EQ_U32(mipLevelForLod(3.0, levels), 3u);
    // 放大（footprint < 1）⇒ 必然 level 0
    LIFT_CHECK_EQ_U32(mipLevelForLod(lodFromUvDerivatives(0.5 / 1024.0, 0.0, 0.0, 0.5 / 1024.0, 1024.0, 1024.0),
                                     levels),
                      0u);
    LIFT_CHECK_EQ_U32(mipLevelForLod(-1000.0, levels), 0u);
    // 最近级：1.5:1 落在 1 级、1.2:1 落在 0 级（`floor(lod+0.5)` 的口径）
    LIFT_CHECK_EQ_U32(mipLevelForLod(std::log2(1.5), levels), 1u);
    LIFT_CHECK_EQ_U32(mipLevelForLod(std::log2(1.2), levels), 0u);
    // clamp：越界 / 非有限 / 只有一级
    LIFT_CHECK_EQ_U32(mipLevelForLod(100.0, 4u), 3u);
    LIFT_CHECK_EQ_U32(mipLevelForLod(100.0, 1u), 0u);
    LIFT_CHECK_EQ_U32(mipLevelForLod(std::nan(""), levels), 0u);
    LIFT_CHECK_EQ_U32(mipLevelForLod(std::numeric_limits<double>::infinity(), levels), 0u);
    // 退化足迹（0 尺寸 / 0 导数）⇒ level 0
    LIFT_CHECK_EQ_U32(mipLevelForLod(lodFromUvDerivatives(0.0, 0.0, 0.0, 0.0, 16.0, 16.0), levels), 0u);
    LIFT_CHECK_EQ_U32(mipLevelForLod(lodFromUvDerivatives(1.0, 0.0, 0.0, 1.0, 0.0, 0.0), levels), 0u);

    // 三档等价于"真三角形"：4×4 纹理画在 4×4 / 2×2 / 1×1 的屏幕四边形上（XYZRHW ⇒ w=1）
    const double lv4 = 4.0;
    const double invw1[3] = {1.0, 1.0, 1.0};
    {
        // 1:1：屏幕 4×4，uv 0..1
        const double vx[3] = {0.0, 4.0, 0.0};
        const double vy[3] = {0.0, 0.0, 4.0};
        const double uw[3] = {0.0, 1.0, 0.0};
        const double vw[3] = {0.0, 0.0, 1.0};
        LIFT_CHECK_EQ_U32(levelForTriangle(vx, vy, invw1, uw, vw, lv4, lv4, 4u), 0u);
    }
    {
        // 2:1：屏幕 2×2
        const double vx[3] = {0.0, 2.0, 0.0};
        const double vy[3] = {0.0, 0.0, 2.0};
        const double uw[3] = {0.0, 1.0, 0.0};
        const double vw[3] = {0.0, 0.0, 1.0};
        LIFT_CHECK_EQ_U32(levelForTriangle(vx, vy, invw1, uw, vw, lv4, lv4, 3u), 1u);
    }
    {
        // 4:1：屏幕 1×1
        const double vx[3] = {0.0, 1.0, 0.0};
        const double vy[3] = {0.0, 0.0, 1.0};
        const double uw[3] = {0.0, 1.0, 0.0};
        const double vw[3] = {0.0, 0.0, 1.0};
        LIFT_CHECK_EQ_U32(levelForTriangle(vx, vy, invw1, uw, vw, lv4, lv4, 3u), 2u);
    }
}

// ---- ④b 真实右侧批次的几何：200×640 屏幕四边形上的 200×1818 texel 足迹 ----
//   `REQ-01M73JV31QVQX206Y3YG5QDMND`：TITLE 右侧 `(1060,40)200×640` 的 1px 暗短划。
//   几何（A14 判据，纯几何、不依赖游戏）：`vx={1060,1260,1060}`、`vy={40,40,680}`、
//   uv 跨 200 × 1818 texel（页面 2048²）⇒ x 1.0 texel/px、y 2.84 texel/px ⇒ LOD 1.506。
//   这条钉的是"**这是一个缩小采样、必须取 mip**"：最近级 = 2（`mipLevelForLod(·, 12)`）；
//   同一 LOD 在只有一级的链上必须退回 0（`mipLevelForLod(·, 1)`）—— 那一格正是
//   "CPU 臂 level 0 / GPU 臂硬件取 ≥1 级"分叉的最小表达。
void caseRightBatchTriangleLod() {
    lift_test::g_current_case = "d3d9-mip/triangle-lod-right-batch";
    using lift::d3d9::lodFromUvDerivatives;
    using lift::d3d9::mipLevelForLod;
    const double tex = 2048.0;
    // page22 的 uv 原点（`[draw]` 的 uv= 下界，只为让读数贴近真实批；平移不影响导数）
    const double u0 = 0.3555;
    const double v0 = 0.3513;
    const double vx[3] = {1060.0, 1260.0, 1060.0};
    const double vy[3] = {40.0, 40.0, 680.0};
    const double invw1[3] = {1.0, 1.0, 1.0};  // XYZRHW 批（`gpu_invw_ok` 放行的那种）
    const double uw[3] = {u0, u0 + 200.0 / tex, u0};             // 200 texel / 200 px = 1.0
    const double vw[3] = {v0, v0, v0 + 1818.0 / tex};            // 1818 texel / 640 px = 2.84
    double dux = 0.0, dvx = 0.0, duy = 0.0, dvy = 0.0;
    LIFT_CHECK(lift::d3d9::screenUvDerivatives(vx, vy, invw1, uw, vw, &dux, &dvx, &duy, &dvy));
    // 屏幕导数（texel / 屏幕像素）：x 轴 1.0、y 轴 2.84（uv 跨 200×1818 分摊到 200×640 px）
    LIFT_CHECK(closeTo(dux * tex, 1.0, 1e-9));
    LIFT_CHECK(closeTo(duy * tex, 0.0, 1e-9));
    LIFT_CHECK(closeTo(dvy * tex, 1818.0 / 640.0, 1e-9));
    const double lod = lodFromUvDerivatives(dux, dvx, duy, dvy, tex, tex);
    LIFT_CHECK(closeTo(lod, std::log2(1818.0 / 640.0), 1e-9));  // 1.5060…
    // ★ 判据：这是缩小采样 ⇒ 2048²（12 级）的链上最近级 = 2
    LIFT_CHECK_EQ_U32(mipLevelForLod(lod, 12u), 2u);
    LIFT_CHECK_EQ_U32(levelForTriangle(vx, vy, invw1, uw, vw, tex, tex, 12u), 2u);
    // 而"只有一级"（没有链）时必须退回 0 —— 两臂分叉的那一格，钉住它是**链**的差别而不是公式
    LIFT_CHECK_EQ_U32(mipLevelForLod(lod, 1u), 0u);
    // 相邻行每行跨 2.84 texel ⇒ 逐行落在不同纹素相位（这就是 1px 短划的来源）
    LIFT_CHECK(dvy * tex > 2.5 && dvy * tex < 3.2);
}

// ---- ⑤ 透视校正：w≠1 时不能用"顶点 uv 的仿射梯度" ----
void casePerspectiveDerivatives() {
    lift_test::g_current_case = "d3d9-mip/perspective-derivatives";
    // 屏幕三角形 (0,0)-(4,0)-(0,4)；w = 1 / 0.5 / 2（即 1/w = 1 / 2 / 0.5）。
    // uv：u = {0,1,0}、v = {0,0,1} ⇒ 顶点存的是 u/w、v/w。
    const double vx[3] = {0.0, 4.0, 0.0};
    const double vy[3] = {0.0, 0.0, 4.0};
    const double invw[3] = {1.0, 2.0, 0.5};
    const double uw[3] = {0.0 * 1.0, 1.0 * 2.0, 0.0 * 0.5};
    const double vw[3] = {0.0 * 1.0, 0.0 * 2.0, 1.0 * 0.5};
    double dux = 0.0, dvx = 0.0, duy = 0.0, dvy = 0.0;
    LIFT_CHECK(lift::d3d9::screenUvDerivatives(vx, vy, invw, uw, vw, &dux, &dvx, &duy, &dvy));
    // 手算（det = 16）：
    //   U = Σ l·(u/w) = {0,2,0}   ⇒ Ux = 0.5,          Uy = 0
    //   V = Σ l·(v/w) = {0,0,0.5} ⇒ Vx = 0,            Vy = 2/16 = 0.125
    //   W = Σ l·(1/w) = {1,2,0.5} ⇒ Wx = 4/16 = 0.25,  Wy = -0.5/... = -0.125
    //   质心：W = 7/6、u = (2/3)/(7/6) = 4/7、v = (1/6)/(7/6) = 1/7
    //   du/dx = (0.5 - (4/7)·0.25)/(7/6)  = (0.5 - 1/7)/(7/6)  = (5/14)·(6/7) = 15/49
    //   dv/dx = (0   - (1/7)·0.25)/(7/6)  = (-1/28)·(6/7)       = -3/98
    //   du/dy = (0   - (4/7)·(-0.125))/(7/6) = (1/14)·(6/7)     = 3/49
    //   dv/dy = (0.125 + (1/7)·0.125)/(7/6)  = (1/7)·(6/7)      = 6/49
    LIFT_CHECK(closeTo(dux, 15.0 / 49.0, 1e-12));
    LIFT_CHECK(closeTo(dvx, -3.0 / 98.0, 1e-12));
    LIFT_CHECK(closeTo(duy, 3.0 / 49.0, 1e-12));
    LIFT_CHECK(closeTo(dvy, 6.0 / 49.0, 1e-12));
    // ★ 反面对照：把顶点 uv 直接归一化（u_i/w_i 再除 1/w_i = u_i）后做**仿射**梯度 ⇒
    //   `du/dx = 0.25`、`du/dy = 0`（这条三角形上 u 只沿 x 变）。正确的透视值是
    //   `du/dx = 15/49 ≈ 0.3061`、`du/dy = 3/49 ≈ 0.0612` ⇒ 两条都与仿射值分得开
    //   （所以"退回仿射梯度"时这里**会**红，不是恒真的断言）。
    LIFT_CHECK(std::fabs(dux - 0.25) > 1e-3);
    LIFT_CHECK(std::fabs(duy - 0.0) > 1e-3);
}

// ---- 退化三角形 ⇒ 返回 false 且导数归零（调用方按 level 0 走）----
void caseDegenerateTriangle() {
    lift_test::g_current_case = "d3d9-mip/degenerate";
    const double vx[3] = {0.0, 1.0, 2.0};
    const double vy[3] = {0.0, 1.0, 2.0};  // 三点共线
    const double invw[3] = {1.0, 1.0, 1.0};
    const double uw[3] = {0.0, 1.0, 0.0};
    const double vw[3] = {0.0, 0.0, 1.0};
    double dux = 123.0, dvx = 123.0, duy = 123.0, dvy = 123.0;
    LIFT_CHECK(!lift::d3d9::screenUvDerivatives(vx, vy, invw, uw, vw, &dux, &dvx, &duy, &dvy));
    LIFT_CHECK(closeTo(dux, 0.0) && closeTo(dvx, 0.0) && closeTo(duy, 0.0) && closeTo(dvy, 0.0));
    // 空指针 ⇒ 不崩、返回 false
    LIFT_CHECK(!lift::d3d9::screenUvDerivatives(nullptr, vy, invw, uw, vw, nullptr, nullptr, nullptr, nullptr));
}

}  // namespace

int main() {
    using namespace lift_test;
    g_current_case = "d3d9-mip";
    caseBoxFilter2x2();
    caseBoxFilterOdd();
    caseChainGeometry();
    caseLodLevels();
    caseRightBatchTriangleLod();
    casePerspectiveDerivatives();
    caseDegenerateTriangle();
    return finish("d3d9-mip");
}
