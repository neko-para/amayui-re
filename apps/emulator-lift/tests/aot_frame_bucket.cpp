// emulator-lift · **按帧 shim 分桶**（`--profile-slow-frames <ms>`）的 pure 守卫
// （顶层无条件注册 ⇒ pure 档；不取时、不碰 GPU / 注册表 / 游戏）
//
// ★ 守的四件事（每条都钉在"坏了会有什么后果"上）：
//   ① **名字合并 + 有界 top-N**：不同 id 可以是同一个 `module!name`（运行期注册的 COM thunk）
//      ⇒ 必须按名字相加；且每帧只留 `kTopN` 个，剩下的**只报数**。
//      红了意味着：慢帧表把同一个 shim 拆成几行（读的人会以为有两个热点），或者行数无界
//      （一个"每帧都慢"的 run 会打几十 MB，而这张表只在人看的时候有意义）。
//   ② **阈值门 = 只存慢帧**：低于阈值的帧一条都不存，而且**账不跨帧漏**
//      （`noteShim` 在快帧里累计的东西必须在那一帧结束时清掉）。
//      红了意味着：慢帧表把轻帧的 shim 时间算进下一帧 ⇒ 归因指错 shim（比"没有表"更糟）。
//   ③ **上界只报数不丢静默**：慢帧 > `kMaxSlowFrames` 时 `rows` 截断、`frames_dropped` 精确计数。
//      红了意味着：慢帧表看起来"就这么多"，实际后面还有一堆（漏报）。
//   ④ **关档 = 什么都没有**：`threshold_ms == 0` ⇒ `on == false`、`endFrame` 恒 false、`rows` 空。
//      红了意味着：没给这个 flag 的档也会攒账（默认档不再零成本，而且会多打一张表）。
//
// ★ 时间口径为什么可以纯：本夹具**直接喂** `now_ms`（= `profileProcessMs()` 的那把尺的合成值），
//   不调任何时钟 ⇒ 断言的只是"帧间差 / 阈值 / 分桶"的算术，不依赖机器快慢。
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "aot/frame_bucket.h"
#include "check.h"

namespace {

using lift::aot::FrameBucketProf;
using lift::aot::FrameShimRow;
using lift::aot::NamedShimRow;

// 小夹具：id → 名字（产品臂给的是 `ShimRegistry::byId`）。
std::vector<std::string> names(std::initializer_list<const char*> list) {
    std::vector<std::string> v;
    for (const char* s : list) v.emplace_back(s);
    return v;
}

lift::aot::ShimNameFn resolver(const std::vector<std::string>& table) {
    return [&table](std::uint32_t id) -> std::string {
        return id < table.size() ? table[id] : std::string("(unknown)");
    };
}

}  // namespace

int main() {
    ::lift_test::g_current_case = "aot-frame-bucket";

    // ---- ① 名字合并：同名 id 相加；排序 = ns↓ / calls↓ / 名字↑；全 0 行不出行 ----
    {
        ::lift_test::g_current_case = "frame-bucket-merges-by-name";
        const std::vector<std::string> t = names({"d3d9!Present", "d3dx9_43!ID3DXSprite::End",
                                                 "d3d9!Present", "d3d9!SetRenderTarget"});
        const std::vector<FrameShimRow> rows = {
            {0u, 5'000'000ull, 1ull},   // d3d9!Present
            {1u, 1'000'000ull, 203ull}, // ID3DXSprite::End
            {2u, 7'000'000ull, 2ull},   // d3d9!Present（同名，与 id 0 相加 ⇒ 12 ms / 3 次）
            {3u, 0ull, 0ull},           // 没被调用 ⇒ 不出行
        };
        const std::vector<NamedShimRow> merged = lift::aot::mergeShimUsageByName(rows, resolver(t));
        LIFT_CHECK(merged.size() == 2u);
        LIFT_CHECK(merged[0].name == "d3d9!Present");
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(merged[0].ns), 12'000'000);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(merged[0].calls), 3);
        LIFT_CHECK(merged[1].name == "d3dx9_43!ID3DXSprite::End");
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(merged[1].calls), 203);

        // 同 ns 时按 calls 降序，再按名字升序 ⇒ 同输入同输出（读数跨 run 可比）
        const std::vector<std::string> t2 = names({"b!x", "a!y"});
        const std::vector<FrameShimRow> rows2 = {{0u, 100ull, 1ull}, {1u, 100ull, 1ull}};
        const std::vector<NamedShimRow> merged2 = lift::aot::mergeShimUsageByName(rows2, resolver(t2));
        LIFT_CHECK(merged2.size() == 2u);
        LIFT_CHECK(merged2[0].name == "a!y");  // 名字升序
        LIFT_CHECK(merged2[1].name == "b!x");

        // 有界 top-N：只留前 N 个（入参不改），且剩下的数量能算出来（打印里那句"另有 M 个"）
        const std::vector<NamedShimRow> top = lift::aot::takeTopN(merged, 1u);
        LIFT_CHECK(top.size() == 1u);
        LIFT_CHECK(top[0].name == "d3d9!Present");
        LIFT_CHECK(merged.size() == 2u);  // takeTopN 不改入参
        LIFT_CHECK(lift::aot::takeTopN(merged, 99u).size() == 2u);  // 不足 N ⇒ 全给
    }

    // ---- ② 阈值门：只存 ≥ 阈值的帧；快帧的账**不跨帧漏** ----
    {
        ::lift_test::g_current_case = "frame-bucket-threshold-gate";
        const std::vector<std::string> t = names({"com!IDirect3DDevice9::Present", "d3d9!End"});
        FrameBucketProf p;
        p.begin(50u, /*now_ms=*/1000.0, /*insns=*/0ull, /*shim_count=*/t.size());

        // 帧 1：10 ms（快）—— 里面烧了 3 ms 的 shim，必须**不**流到帧 2
        p.noteShim(0u, 3'000'000ull);
        const bool slow1 = p.endFrame(1u, 1010.0, 1000ull, 1u, resolver(t));
        LIFT_CHECK(!slow1);
        LIFT_CHECK(p.rows.empty());
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(p.frames_seen), 1);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(p.slow_frames), 0);

        // 帧 2：120 ms（慢）—— 里面只有 60 ms 的 shim + 40 ms 的 End ⇒ 两行、降序
        p.noteShim(0u, 60'000'000ull);
        p.noteShim(1u, 40'000'000ull);
        const bool slow2 = p.endFrame(2u, 1130.0, 30'000ull, 1u, resolver(t));
        LIFT_CHECK(slow2);
        LIFT_CHECK(p.rows.size() == 1u);
        const lift::aot::SlowFrameRow& r = p.rows[0];
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(r.frame), 2);
        // 墙钟 = now 的帧间差（同一把尺）；insns = 该帧增量（不是累计）
        LIFT_CHECK(r.wall_ms > 119.9 && r.wall_ms < 120.1);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(r.insns), 29'000);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(r.shim_ns), 100'000'000);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(r.shim_calls), 2);
        LIFT_CHECK(r.top.size() == 2u);
        LIFT_CHECK(r.top[0].name == "com!IDirect3DDevice9::Present");
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(r.top[0].ns), 60'000'000);
        LIFT_CHECK(r.top[1].name == "d3d9!End");

        // 帧 3：恰好等于阈值 ⇒ 算慢帧（>=，不是 >）
        p.noteShim(1u, 1'000'000ull);
        LIFT_CHECK(p.endFrame(3u, 1180.0, 31'000ull, 1u, resolver(t)));
        LIFT_CHECK(p.rows.size() == 2u);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(p.frames_seen), 3);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(p.slow_frames), 2);
    }

    // ---- ③ 上界：慢帧 ≤ kMaxSlowFrames，超出只计数；每帧名字 ≤ kTopN ----
    {
        ::lift_test::g_current_case = "frame-bucket-bounded";
        std::vector<std::string> t;
        for (int i = 0; i < 40; ++i) t.push_back("mod!shim" + std::to_string(i));
        FrameBucketProf p;
        p.begin(1u, 0.0, 0ull, t.size());
        // 每帧都慢：40 个 shim 各 1 ms ⇒ 合并后 40 行 ⇒ 每帧只留 8 个 + 报"另有 32 个"
        for (std::uint32_t f = 1; f <= 40; ++f) {
            for (std::uint32_t id = 0; id < t.size(); ++id) {
                p.noteShim(id, static_cast<std::uint64_t>(id + 1u) * 1'000'000ull);
            }
            LIFT_CHECK(p.endFrame(f, static_cast<double>(f) * 2.0, 0ull, 1u, resolver(t)));
        }
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(p.slow_frames), 40);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(p.frames_dropped), 8);  // 40 - 32
        LIFT_CHECK(p.rows.size() == FrameBucketProf::kMaxSlowFrames);
        LIFT_CHECK(p.rows[0].top.size() == FrameBucketProf::kTopN);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(p.rows[0].shim_rows), 40);
        // 丢掉的 8 帧也**没有**偷偷进表（表里最早的那 32 帧帧号 1..32）
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(p.rows.back().frame), 32);
    }

    // ---- ④ 关档：阈值 0 ⇒ on == false、一条不存（默认档零成本的机械判据）----
    {
        ::lift_test::g_current_case = "frame-bucket-off-by-default";
        const std::vector<std::string> t = names({"d3d9!Present"});
        FrameBucketProf p;
        p.begin(0u, 1000.0, 0ull, t.size());
        LIFT_CHECK(!p.on);
        p.noteShim(0u, 5'000'000ull);
        LIFT_CHECK(!p.endFrame(1u, 9000.0, 10ull, 1u, resolver(t)));  // 就算 8 s 也不记（关档）
        LIFT_CHECK(p.rows.empty());
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(p.frames_seen), 0);
        // `reset()` 之后连开档状态都得是干净的（按轮重置的机械判据）
        p.begin(10u, 0.0, 0ull, t.size());
        LIFT_CHECK(p.on);
        p.reset();
        LIFT_CHECK(!p.on);
        LIFT_CHECK(p.rows.empty());
        LIFT_CHECK(p.cur_ns.empty());
    }

    // ---- ⑤ id 越界（运行期 addRuntime）⇒ 惰性扩容，⛔ 不丢这笔账 ----
    {
        ::lift_test::g_current_case = "frame-bucket-grows-for-runtime-shims";
        const std::vector<std::string> t = names({"d3d9!Present", "(unknown)", "com!ID3DXEffect::Begin"});
        FrameBucketProf p;
        p.begin(1u, 0.0, 0ull, /*shim_count=*/1);  // 开档时只认识 1 个 shim
        p.noteShim(2u, 4'000'000ull);              // 运行期注册进来的 id=2
        LIFT_CHECK(p.endFrame(1u, 100.0, 0ull, 1u, resolver(t)));
        LIFT_CHECK(p.rows.size() == 1u);
        LIFT_CHECK(p.rows[0].top.size() == 1u);
        LIFT_CHECK(p.rows[0].top[0].name == "com!ID3DXEffect::Begin");
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(p.rows[0].top[0].ns), 4'000'000);
    }

    return lift_test::finish("aot-frame-bucket");
}
