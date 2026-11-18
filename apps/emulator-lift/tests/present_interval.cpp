// emulator-lift · `D3DPRESENT_PARAMETERS.PresentationInterval` 的白名单判定（pure 档）
//
// 判据（红了意味着什么）：
//   ① `D3DPRESENT_INTERVAL_IMMEDIATE (0x80000000)` —— 引擎默认配置 `display:PresentInterval=0`
//      写的就是它 —— 必须**通过**；
//   ② `DEFAULT (0)` / `ONE (1)` / `TWO (2)` / `FOUR (4)` / `EIGHT (8)` / 其余任意值（含全 1）
//      必须**全拒**。
//   ③ 常量 `kPresentIntervalImmediate` 必须就是 SDK 的 `D3DPRESENT_INTERVAL_IMMEDIATE`
//      （白名单被改成别的值 ⇒ ① 与 ③ 一起红，而不是"换个值也照样绿"）。
//
// ★ 为什么判据落在**纯函数**上（`runtime/d3d9.h` 的 `presentIntervalSupported`）：裁决是
//   "用户不会改配置 ⇒ 一定是 IMMEDIATE；其余值响亮停"（`REQ-01M7QVQR3ENQW3ZWNR4VFTPYJY`，
//   2026-11-19）。判定必须**只有一处**（解析 D3DPRESENT_PARAMETERS 的那处 = `readPresentParams`），
//   而"哪几个值被拒"是纯布尔事实 ⇒ 纯夹具足够，不需要真跑引擎。
//   ⚠ 本用例**钉不到**"调用点有没有调用它"：把 `readPresentParams` 里的那次调用删掉，这里仍全绿。
//     那条缝的人工复核命令写在交付报告 `## 2`（`rg -n 'presentIntervalSupported' src`）。
// ★ 判据走**运行期**断言（不是 `static_assert`）：判定被改成恒真时必须**跑起来并红**，
//   这样 `tools/mutate-check.mjs` 的 `ctest:present-interval` 才验得到"它会红"；
//   编译期断言会让变异表现成"构建失败"，那在变异自检里是**不可判定**、不是红（同 `guest_exit.cpp`）。
#include <cstdint>
#include <initializer_list>

#include "check.h"
#include "runtime/d3d9.h"

int main() {
    using namespace lift_test;
    using lift::d3d9::kPresentIntervalImmediate;
    using lift::d3d9::presentIntervalSupported;

    // ---- ③ 白名单常量 = SDK 的 D3DPRESENT_INTERVAL_IMMEDIATE ----
    g_current_case = "present-interval/constant";
    LIFT_CHECK_EQ_U32(kPresentIntervalImmediate, 0x80000000u);

    // ---- ① IMMEDIATE 通过 ----
    g_current_case = "present-interval/immediate";
    LIFT_CHECK(presentIntervalSupported(0x80000000u));
    LIFT_CHECK(presentIntervalSupported(kPresentIntervalImmediate));

    // ---- ② 其余值全拒（含 DEFAULT=0；引擎的 `display:PresentInterval` 配置项映射的就是这几个）----
    g_current_case = "present-interval/reject";
    for (std::uint32_t v : {0u /* DEFAULT */, 1u /* ONE */, 2u /* TWO */, 4u /* FOUR */,
                            8u /* EIGHT */, 3u, 0x10u, 0x7FFFFFFFu, 0x80000001u,
                            0xFFFFFFFFu}) {
        LIFT_CHECK(!presentIntervalSupported(v));
    }

    return finish("lift-present-interval");
}
