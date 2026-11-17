// emulator-lift · AOT 音频 **S3 泵闸门**（`runtime/audio_pump_gate.h`）：该不该 pump / 节流 / 记账
//
// 判据（红了意味着什么）：
//   ① **off 档一次都不进 pump**（本单最硬的一条）：`enabled == false` 时，把 20 万个块边界喂给
//      `AudioPumpGate::due()`，**一格都不许真**；同时按 run 循环的形状记账
//      （`if (due) { ++pumped; noteCalled(); }`）⇒ `pumped == 0` 且 `calls() == 0`、
//      `nextCheck()` 原地不动。
//      红了 ⇒ 默认档（`--audio off`，不带任何 `--audio`）会去调 `AudioEngine::pump()`
//      —— 那是"进了 pump 再返回"，不是"不调用"；默认档的 audio mutex / 混音 / 设备时钟全被拖进来，
//      而 stdout 可能**看起来**没变（所以它是一条**静默**的破法，必须被钉住）。
//   ② **节流**：`enabled == true` 且没有嵌套 guest 时，10 万条 guest 指令里 `due()` 恰好真
//      **25** 次（= 1 + 100000/4096 的整数个 stride），⛔ 不是每块都真（20 万块里 25 次）。
//      红了 ⇒ 块边界（每派发一次都过一遍的热路径）每块都去抢 audio mutex（解释器里实测过：
//      那是 cache-line ping-pong 级的成本）。
//   ③ **嵌套 guest 期间不泵、解除后立刻补**：`nested_guest_active == true` 时 `due()` 恒假且
//      **不推进节流点**；一解除，`due()` 必须在**同一个 insns** 上立刻为真（⛔ 不许再等一个
//      stride：fill 迟到 4096 条指令以上是真的会欠料）。
//      红了 ⇒ 要么在别人的 guest 上下文里铺了一帧（改坏仍在跑的现场），要么欠料。
//   ④ **thread 档不泵**：`thread_mode == true` 时恒假（喂料由 feeder 线程自己做 —— 这一格就是
//      S0 谓词 `audioThreadMode(mode, realTime)` 的输出）。★ 注意：新谓词下 AOT 的
//      `--audio sdl` + 墙钟**会**是真 feeder 线程 ⇒ 这一格在 AOT 上不再恒 false；
//      闸门行为本身（thread 档不 due）不变，变的是"谁会进 thread 档"。
//      ⛔ 不要把"thread 档不 due"读成"AOT 永远不 due"：`--audio virtual` / 设备回退仍走 D 泵。
//
// ★ 为什么是 pure 档：`add_test` 在 CMakeLists 最外层（`AGENTS.md` §12.4 判据 = 注册位置）
//   ⇒ 默认 `ctest` 就跑；本文件只 include 一个**纯头**（`<cstdint>`），不碰 SDL / 设备 / guest 内存 /
//   任何资产。
// ★ 为什么判在**纯函数 + 纯状态机**上（而不是"跑一次真 run 看填充数"）：真 run 需要游戏安装树
//   （assets 档、几十秒），且 off 档按定义**不许有输出** ⇒ 在那一面没有观察点。运行时的
//   "off 档 calls == 0" 由**进程级**读数补（`LIFT_AOT_AUDIO_TRACE=1` 的 `[audio-pump] calls=…`，
//   见交付报告）；本文件钉的是那条读数**为什么**会是 0。
// ★ thread 档的失败面**不在这个头里**：`due()` 恒假 ⇒ `pump()` 的 `feeder_error_` 早退在 AOT
//   不可达 ⇒ AOT 的块边界必须直接看 `AudioEngine::feederFailed()`（见 `aot_run.cpp` 的音频块）。
#include <cstdint>
#include <initializer_list>

#include "check.h"
#include "runtime/audio_pump_gate.h"

namespace {

// 造一份"事实"（缺省 = 真开着泵、没有嵌套 guest、主线程 D 泵档）。
lift::AudioPumpFacts facts(bool enabled, bool thread_mode, bool nested, std::uint64_t insns) {
    lift::AudioPumpFacts f;
    f.enabled = enabled;
    f.thread_mode = thread_mode;
    f.nested_guest_active = nested;
    f.insns = insns;
    return f;
}

// ==== ① off 档：20 万块边界，一次都不许 due（= pump 调用次数 0）====
void testOffNeverPumps() {
    lift_test::g_current_case = "s3-pump-gate-off-never-due";
    lift::AudioPumpGate gate;
    std::uint64_t pumped = 0;
    constexpr std::uint64_t kBlocks = 200000u;
    for (std::uint64_t i = 0; i < kBlocks; ++i) {
        // 每 7 块换一次 insns 形状：0 / 恰好踩 stride / 大间隔 / 回退（长 run 里 insns 只增，
        // 但闸门不该依赖这一点 —— 它只看 `>= next_check_`）。
        const std::uint64_t insns = (i % 7u == 0u) ? 0u : (i * 131u);
        if (gate.due(facts(/*enabled=*/false, /*thread_mode=*/false, /*nested=*/false, insns))) {
            ++pumped;
            gate.noteCalled(insns);
        }
    }
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(pumped), 0);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(gate.calls()), 0);
    // off 档连节流点都不许被推进（"没真调过就不推进"的另一个方向）。
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(gate.nextCheck()), 0);
    // 8 格全表里，`enabled == false` 的每一格都必须假（thread / nested 随便怎么变）。
    for (int thread = 0; thread < 2; ++thread) {
        for (int nested = 0; nested < 2; ++nested) {
            for (std::uint64_t insns : {std::uint64_t{0}, std::uint64_t{4096}, std::uint64_t{1} << 40}) {
                LIFT_CHECK(!lift::AudioPumpGate{}.due(facts(false, thread != 0, nested != 0, insns)));
            }
        }
    }
}

// ==== ② 节流：10 万条指令恰好 25 次（1 + 24 个 stride），⛔ 不是每块 ====
void testStrideThrottles() {
    lift_test::g_current_case = "s3-pump-gate-stride-throttles";
    // 节流粒度与块边界的 `kCheckStride` 是**同一个数**（`aot_run.cpp` 的 static_assert 钉住那份，
    // 这里再钉一次字面值：改了它必须同时改两处 + 这条用例的期望值）。
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(lift::AudioPumpGate::kStride), 4096);

    lift::AudioPumpGate gate;
    std::uint64_t calls = 0;
    std::uint64_t first_call = ~std::uint64_t{0};
    constexpr std::uint64_t kInsns = 100000u;
    for (std::uint64_t insns = 0; insns < kInsns; ++insns) {
        if (gate.due(facts(/*enabled=*/true, /*thread_mode=*/false, /*nested=*/false, insns))) {
            if (first_call == ~std::uint64_t{0}) first_call = insns;
            gate.noteCalled(insns);
            ++calls;
        }
    }
    // 块边界是"每条 guest 指令都过一遍"的热路径：100000 块边界只许 25 次真泵。
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(calls), 25);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(gate.calls()), 25);
    // 第一次 = 第一个边界（`next_check_` 初值 0）；之后每 stride 一次。
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(first_call), 0);
    // 最后一次 = 24 * 4096 = 98304（下一个 102400 已越过窗口）。
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(gate.nextCheck()), 102400);
    LIFT_CHECK(calls * 100u < kInsns);  // 量级断言：⛔ 不是"每块都泵"
}

// ==== ③ 嵌套 guest 期间不泵；解除后**立刻**补（不等一个 stride）====
void testNestedSuppressesThenCatchesUp() {
    lift_test::g_current_case = "s3-pump-gate-nested-suppressed-then-catch-up";
    lift::AudioPumpGate gate;
    // 先在干净边界上真泵一次 ⇒ 下一次要等到 insns >= 4096。
    LIFT_CHECK(gate.due(facts(true, false, false, 1000)));
    gate.noteCalled(1000);
    // 嵌套窗口：从 1000 到 100000（远超一个 stride），一次都不许 due，且节流点不许动。
    std::uint64_t pumped = 0;
    for (std::uint64_t insns = 1000; insns < 100000u; insns += 37u) {
        if (gate.due(facts(true, false, /*nested=*/true, insns))) {
            ++pumped;
            gate.noteCalled(insns);
        }
    }
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(pumped), 0);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(gate.calls()), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(gate.nextCheck()), 1000 + 4096);
    // 解除嵌套的**那一刻**（同一个 insns）就该允许泵：fill 不许再等一个 stride。
    LIFT_CHECK(gate.due(facts(true, false, /*nested=*/false, 100000)));
}

// ==== ④ thread 档恒不泵（S0 谓词那一格；闸门必须表达它，与谁进 thread 档无关）====
void testThreadModeNeverPumps() {
    lift_test::g_current_case = "s3-pump-gate-thread-mode-never-due";
    lift::AudioPumpGate gate;
    std::uint64_t pumped = 0;
    for (std::uint64_t insns = 0; insns < 200000u; insns += 13u) {
        if (gate.due(facts(true, /*thread_mode=*/true, /*nested=*/false, insns))) {
            ++pumped;
            gate.noteCalled(insns);
        }
    }
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(pumped), 0);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(gate.calls()), 0);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(gate.nextCheck()), 0);
    // 同一份 insns 上把 thread 档换成 D 泵 ⇒ 立刻 due（证明上面那 0 不是"insns 恰好不满足"）。
    LIFT_CHECK(gate.due(facts(true, /*thread_mode=*/false, /*nested=*/false, 0)));
}

}  // namespace

int main() {
    testOffNeverPumps();
    testStrideThrottles();
    testNestedSuppressesThenCatchesUp();
    testThreadModeNeverPumps();
    return lift_test::finish("aot-audio-pump");
}
