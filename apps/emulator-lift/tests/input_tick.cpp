// emulator-lift · 输入时间线 tick 的**触发语义**与**扫描次数** —— pure 守卫
//
// ★ 为什么有它：`InputScript::tick` 从"每个块边界都遍历整条动作表"改成"缓存未应用动作的最小
//   到期值，不到期直接返回"。性能改动 ⇒ 必须有人钉住 ① 触发时机一刻都没变；② 扫描次数不再随
//   块数线性增长（否则"优化"可以悄悄退回去而所有用例照绿）。
//   ⛔ 不钉任何墙钟数字（ms 随机器变），只钉**关系**与**边界**。
#include <cstdint>
#include <cstdio>

#include "check.h"
#include "runtime/host_window.h"
#include "runtime/input_script.h"

int main() {
    using namespace lift_test;

    // 只有 `wait`（no-op 动作）与 `quit` ⇒ 不碰窗口 / 键鼠，纯时间线语义。
    auto load = [](lift::InputScript& s, const char* text) {
        std::string err;
        const bool ok = s.loadText(text, "inline:tick", &err);
        if (!ok) std::fprintf(stderr, "脚本解析失败：%s\n", err.c_str());
        return ok;
    };

    // ---------------------------------------------------------------------------------
    // ① 快路径不扫动作表：20 万次 tick、3 条未到期动作 ⇒ 逐条比较次数必须是**个位数**
    // ---------------------------------------------------------------------------------
    g_current_case = "fast-path-no-scan";
    {
        lift::InputScript s;
        lift::WindowManager wm;
        LIFT_CHECK(load(s, "at insn 1000000\nwait 1 insn\nat insn 2000000\nwait 1 insn\nat insn 3000000\nquit\n"));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(s.actionCount()), 3u);
        for (std::uint64_t i = 0; i < 200000ull; ++i) {
            const bool any = s.tick(wm, /*frame=*/1, /*guest_ms=*/i, /*insns=*/i, /*wall_ms=*/i);
            LIFT_CHECK(!any);  // 一条都没到期 ⇒ 不许有动作被应用
        }
        std::printf("tick: %llu ticks ⇒ %llu action comparisons\n", (unsigned long long)s.tickCount(),
                    (unsigned long long)s.actionScans());
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(s.tickCount()), 200000u);
        // ★ 判据：扫描次数只允许"一次重建"的量级（3 条动作 + 常数余量）。
        //   退回"每 tick 遍历动作表" ⇒ 8e5 次 ⇒ 这条立刻红。
        LIFT_CHECK(s.actionScans() <= 16ull);
    }

    // ---------------------------------------------------------------------------------
    // ② 到期边界一刻不差：差 1 条指令都不许触发，到点那一次 tick 必须触发
    // ---------------------------------------------------------------------------------
    g_current_case = "due-at-exact-boundary";
    {
        lift::InputScript s;
        lift::WindowManager wm;
        LIFT_CHECK(load(s, "at insn 1000\nwait 0 insn\nat insn 2000\nquit\n"));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(s.appliedCount()), 0u);
        LIFT_CHECK(!s.tick(wm, 1, 0, 999, 0));  // 差 1 条 ⇒ 不触发
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(s.appliedCount()), 0u);
        LIFT_CHECK(s.tick(wm, 1, 0, 1000, 0));  // 到点 ⇒ 触发第一条
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(s.appliedCount()), 1u);
        LIFT_CHECK(!s.quitRequested());
        // 第二条（quit）要到 2000：同一 tick 里**不许**被"顺手"触发。
        LIFT_CHECK(!s.tick(wm, 1, 0, 1999, 0));
        LIFT_CHECK(!s.quitRequested());
        LIFT_CHECK(s.tick(wm, 1, 0, 2000, 0));
        LIFT_CHECK(s.quitRequested());
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(s.appliedCount()), 2u);
    }

    // ---------------------------------------------------------------------------------
    // ③ 另外两种单位（frame / guest_ms）同样按"到了才触发"，且不受 insn 影响
    // ---------------------------------------------------------------------------------
    g_current_case = "due-frame-and-guest-ms";
    {
        lift::InputScript s;
        lift::WindowManager wm;
        LIFT_CHECK(load(s, "at frame 10\nwait 0 frames\nat guest_ms 500\nwait 0 ms\nat insn 900000\nquit\n"));
        LIFT_CHECK(!s.tick(wm, /*frame=*/9, /*guest_ms=*/499, /*insns=*/1, /*wall_ms=*/0));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(s.appliedCount()), 0u);
        LIFT_CHECK(s.tick(wm, /*frame=*/10, /*guest_ms=*/499, /*insns=*/1, /*wall_ms=*/0));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(s.appliedCount()), 1u);  // frame 那条
        LIFT_CHECK(!s.tick(wm, /*frame=*/10, /*guest_ms=*/499, /*insns=*/500000, /*wall_ms=*/0));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(s.appliedCount()), 1u);
        LIFT_CHECK(s.tick(wm, /*frame=*/10, /*guest_ms=*/500, /*insns=*/500000, /*wall_ms=*/0));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(s.appliedCount()), 2u);  // guest_ms 那条
        // insn 那条仍未到 ⇒ quit 不许提前触发
        LIFT_CHECK(!s.quitRequested());
    }

    // ---------------------------------------------------------------------------------
    // ④ 帧停滞兜底仍在（缓存了"最小非 0 阈值"，不许把这条路径绕过）
    // ---------------------------------------------------------------------------------
    g_current_case = "stall-fallback-still-fires";
    {
        lift::InputScript s;
        lift::WindowManager wm;
        s.setDefaultStallMs(1000);
        LIFT_CHECK(load(s, "at frame 100\nwait 0 frames\nat frame 200\nquit\n"));
        // 帧号卡在 1，墙钟一直推进 ⇒ 超过 1000ms 必须兜底（折算成 wall_ms 后继续）。
        bool fired = false;
        for (std::uint64_t t = 0; t < 5000 && !fired; t += 100) {
            s.tick(wm, /*frame=*/1, /*guest_ms=*/t, /*insns=*/t, /*wall_ms=*/t);
            fired = s.stallFallbackFired();
        }
        LIFT_CHECK(fired);
        // 兜底之后 frame 条件变成 wall_ms 条件 ⇒ 继续推进墙钟，动作必须照样到期（⛔ 不挂死）。
        for (std::uint64_t t = 5000; t < 60000 && s.appliedCount() < 2; t += 100) {
            s.tick(wm, /*frame=*/1, /*guest_ms=*/t, /*insns=*/t, /*wall_ms=*/t);
        }
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(s.appliedCount()), 2u);
    }

    return finish("input-tick");
}
