// emulator-lift · 按帧 shim 分桶的实现（口径见 `frame_bucket.h` 的文件头）
//
// ★ 这里**不做打印**（打印在 `aot_host.cpp` 的 `printSlowFrames`：只有它认得出 `frame-ms:` 那
//   一张表的口径与 `ShimRegistry` 的名字），也**不取时**（时间由调用方用既有计时喂进来）。
#include "aot/frame_bucket.h"

#include <algorithm>
#include <utility>

namespace lift::aot {

std::vector<NamedShimRow> mergeShimUsageByName(const std::vector<FrameShimRow>& rows,
                                               const ShimNameFn& name_of) {
    std::vector<NamedShimRow> out;
    out.reserve(rows.size());
    for (const FrameShimRow& r : rows) {
        if (r.ns == 0 && r.calls == 0) continue;  // 没被调用过的 shim 不出行（没有解释价值）
        const std::string name = name_of ? name_of(r.id) : std::string("(unknown)");
        // ★ 同名合并：线性查找就够（一帧里的 distinct shim 是几十个量级，且这条路径只在**慢帧**
        //   保存时走一次）。
        bool merged = false;
        for (NamedShimRow& m : out) {
            if (m.name != name) continue;
            m.ns += r.ns;
            m.calls += r.calls;
            merged = true;
            break;
        }
        if (merged) continue;
        NamedShimRow row;
        row.name = name;
        row.ns = r.ns;
        row.calls = r.calls;
        out.push_back(std::move(row));
    }
    // 确定性排序：ns 降序 → calls 降序 → 名字升序（同输入同输出 ⇒ 读数可比、用例可断言）
    std::sort(out.begin(), out.end(), [](const NamedShimRow& a, const NamedShimRow& b) {
        if (a.ns != b.ns) return a.ns > b.ns;
        if (a.calls != b.calls) return a.calls > b.calls;
        return a.name < b.name;
    });
    return out;
}

std::vector<NamedShimRow> takeTopN(const std::vector<NamedShimRow>& merged, std::size_t top_n) {
    if (merged.size() <= top_n) return merged;
    return std::vector<NamedShimRow>(merged.begin(),
                                     merged.begin() + static_cast<std::ptrdiff_t>(top_n));
}

void FrameBucketProf::begin(std::uint64_t threshold_ms_arg, double now_ms, std::uint64_t insns,
                            std::size_t shim_count) {
    reset();
    threshold_ms = threshold_ms_arg;
    // ★ 0 = 关：与"没给 `--profile-slow-frames`"同一条（⛔ 不留一条"阈值恒真"的缝）。
    on = threshold_ms_arg != 0;
    prev_ms_ = now_ms;
    prev_insns_ = insns;
    if (!on) return;
    cur_ns.assign(shim_count, 0);
    cur_calls.assign(shim_count, 0);
}

void FrameBucketProf::noteShim(std::uint32_t id, std::uint64_t ns) {
    if (!on) return;
    const std::size_t i = static_cast<std::size_t>(id);
    if (i >= cur_ns.size()) {
        // 运行期 `addRuntime`（COM vtable / 动态导出）会发更大的 id ⇒ 扩容而不是丢账。
        const std::size_t grown = i + 1;
        cur_ns.resize(grown, 0);
        cur_calls.resize(grown, 0);
    }
    cur_ns[i] += ns;
    ++cur_calls[i];
}

void FrameBucketProf::clearFrame() {
    if (!cur_ns.empty()) std::fill(cur_ns.begin(), cur_ns.end(), 0ull);
    if (!cur_calls.empty()) std::fill(cur_calls.begin(), cur_calls.end(), 0ull);
}

bool FrameBucketProf::endFrame(std::uint64_t frame, double now_ms, std::uint64_t insns,
                               std::uint64_t presents, const ShimNameFn& name_of) {
    if (!on) return false;
    const double wall_ms = now_ms - prev_ms_;
    const std::uint64_t insns_delta = insns >= prev_insns_ ? insns - prev_insns_ : 0;
    prev_ms_ = now_ms;
    prev_insns_ = insns;
    ++frames_seen;
    // ---- 取出这一帧的 shim 行（只取被调用过的）----
    std::vector<FrameShimRow> cur;
    cur.reserve(cur_ns.size());
    std::uint64_t shim_ns = 0;
    std::uint64_t shim_calls = 0;
    for (std::size_t id = 0; id < cur_ns.size(); ++id) {
        if (cur_ns[id] == 0 && cur_calls[id] == 0) continue;
        cur.push_back(FrameShimRow{static_cast<std::uint32_t>(id), cur_ns[id], cur_calls[id]});
        shim_ns += cur_ns[id];
        shim_calls += cur_calls[id];
    }
    clearFrame();  // ★ 无论慢不慢都要清：否则上一帧的账会漏进下一帧
    if (!(wall_ms >= static_cast<double>(threshold_ms))) return false;
    ++slow_frames;
    if (rows.size() >= kMaxSlowFrames) {
        ++frames_dropped;
        return true;
    }
    const std::vector<NamedShimRow> merged = mergeShimUsageByName(cur, name_of);
    SlowFrameRow r;
    r.frame = frame;
    r.wall_ms = wall_ms;
    r.insns = insns_delta;
    r.presents = presents;
    r.shim_ns = shim_ns;
    r.shim_calls = shim_calls;
    r.shim_rows = merged.size();
    r.top = takeTopN(merged, kTopN);
    rows.push_back(std::move(r));
    return true;
}

}  // namespace lift::aot
