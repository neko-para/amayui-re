#include "runtime/engine_frames.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "runtime/memory.h"

namespace lift {
namespace {

// Engine 对象大小（C1：`0x4BAA11 push 0AABC8h` / `call operator new`）—— 只用来做 4 GiB 上界检查。
constexpr std::uint32_t kEngineObjectSize = 0x000AABC8u;

// 报告列的**静态字面量**（⛔ 不新造驼峰名）：why 进 err / `[frames]` 行；测试按它逐字断言。
enum FrameWhyBit : std::uint32_t {
    WhyEngineUnpublished = 1u << 0,
    WhyEngineBaseOverflow = 1u << 1,
    WhyCurUncommitted = 1u << 2,
    WhyCurOutOfRange = 1u << 3,
    WhyFrameRegionUncommitted = 1u << 4,
    WhyNoActiveFrame = 1u << 5,
    WhyEngineChanged = 1u << 6,
    WhyFramesUnavailable = 1u << 7,
};

struct WhyEntry {
    const char* text;
    std::uint32_t bit;
};

constexpr WhyEntry kWhys[] = {
    {"engine-unpublished", WhyEngineUnpublished},
    {"engine-base-overflow", WhyEngineBaseOverflow},
    {"cur-uncommitted", WhyCurUncommitted},
    {"cur-out-of-range", WhyCurOutOfRange},
    {"frame-region-uncommitted", WhyFrameRegionUncommitted},
    {"no-active-frame", WhyNoActiveFrame},
    {"engine-changed", WhyEngineChanged},
    {"frames-unavailable", WhyFramesUnavailable},
};

// 有界读：越界 / 未提交 ⇒ false（⛔ 绝不落到 GuestMemory 裸读：safe 模式缺省关，
// 漏一个 committed 就是宿主 SIGSEGV）。与 `engine_marker.cpp` 的同名 helper 逐字同形。
bool readU32(const GuestMemory& mem, std::uint64_t ea, std::uint32_t* out) {
    if (ea > 0xFFFFFFFFull - 3ull) return false;
    const std::uint32_t a = static_cast<std::uint32_t>(ea);
    if (!mem.committed(a, 4u)) return false;
    *out = mem.rd32(a);
    return true;
}

std::string degradedName(std::uint32_t id) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "id:0x%08X", static_cast<unsigned>(id));
    return std::string(buf);
}

}  // namespace

std::vector<ui::UiFrameInfo> EngineFrameSource::unavailable(std::string* err, const char* why) {
    if (err != nullptr) *err = why != nullptr ? why : "frames-unavailable";
    std::uint32_t bit = 0;
    for (const WhyEntry& e : kWhys) {
        if (why != nullptr && std::strcmp(e.text, why) == 0) {
            bit = e.bit;
            break;
        }
    }
    // ★ 每个原因各一次（见头文件）：已打过的原因不重复（⛔ 仍然不周期刷屏）。
    if (bit != 0 && (warned_mask_ & bit) != 0) return {};
    warned_mask_ |= bit;
    ++warnings_;
    std::fprintf(stderr, "[frames] why=%s\n", why != nullptr ? why : "frames-unavailable");
    return {};
}

std::vector<ui::UiFrameInfo> EngineFrameSource::localFrames(std::string* err) {
    std::vector<ui::UiFrameInfo> out;
    if (err != nullptr) err->clear();
    if (mem_ == nullptr) return unavailable(err, "frames-unavailable");

    const GuestMemory& mem = *mem_;

    // ① Engine 单例发布了吗（`0x4BAA3A` 之前 / 退出后是 0）。
    std::uint32_t engine = 0;
    if (!readU32(mem, kEngineSingletonGlobal, &engine) || engine == 0) {
        return unavailable(err, "engine-unpublished");
    }
    // ② 对象不跨 4 GiB（C1 的尺寸；64 位算术防回绕 ⇒ 后面的记录地址才敢用 uint32 语义）。
    if (static_cast<std::uint64_t>(engine) + kEngineObjectSize > 0x100000000ull) {
        return unavailable(err, "engine-base-overflow");
    }
    // ③ `cur`（`+0x5D880`）：**值读不到 / 越界 ⇒ 整表不可用**（top 的定义依赖它，⛔ 不猜）。
    std::uint32_t cur = 0;
    if (!readU32(mem, static_cast<std::uint64_t>(engine) + kEngineCurOffset, &cur)) {
        return unavailable(err, "cur-uncommitted");
    }
    if (cur >= kFrameCount) return unavailable(err, "cur-out-of-range");

    // ④ 第 0 条记录可提交吗（不可提交 ⇒ 整区不可用；逐槽的"不可提交"只是跳过）。
    const std::uint64_t frame_base = static_cast<std::uint64_t>(engine) + kEngineFrameBase;
    if (!mem.committed(static_cast<std::uint32_t>(frame_base), kFrameStride)) {
        return unavailable(err, "frame-region-uncommitted");
    }

    // ⑤ 逐槽（**槽号降序**扫描：最小版的顺序就是"cur 第一 + 其余降序"）。
    for (std::uint32_t slot = kFrameCount; slot-- > 0;) {
        const std::uint64_t rec = frame_base + static_cast<std::uint64_t>(kFrameStride) * slot;
        // 这一格的记录不可提交 ⇒ 无法证明它活动 ⇒ 跳过（报告 U7：真 boot 下 40 条记录是否全部
        // 已提交**未证**，所以每一步都先 committed）。
        if (!mem.committed(static_cast<std::uint32_t>(rec), kFrameStride)) continue;
        std::uint32_t body = 0;
        if (!readU32(mem, rec + kFrameBodyBaseOffset, &body)) continue;
        // C3 的活动判据：`body != 0` 且指向已提交页（空槽规范形状 = body 0，见 C4）。
        if (body == 0 || !mem.committed(body, 1u)) continue;
        std::uint32_t id = 0;
        if (!readU32(mem, rec + kFrameIdentityOffset, &id)) continue;  // 半初始化 ⇒ 不进表
        std::uint32_t parent = 0;
        if (!readU32(mem, rec + kFrameParentOffset, &parent)) continue;

        ui::UiFrameInfo info;
        info.slot = slot;
        info.script_id = id;
        info.parent = parent;
        info.top = (slot == cur);
        // ★ 脚本内位置（切片 3）：**只有 top 帧是当前执行点**（判据 = `slot == cur`，与 `top`
        //   同一个布尔）。公式 / 门 / 错误串的唯一实现在 `engine_marker.cpp:readScriptPositionAt`
        //   （⛔ 不在这里写第二份）。非 top 帧的 `+0x04` 是挂起时保存的续延 PC ⇒ 不暴露。
        //   ★ 单槽读不到 ⇒ 只让这一项 `pos_ok=false`（⛔ 不把整表升级成 err：那一帧仍在表里）。
        if (info.top) {
            const ScriptPositionSample p = readScriptPositionAt(mem, engine, slot);
            info.pos_ok = p.valid;
            info.pos = p.pos;
        }

        // ⑥ 名字（复用 MarkerSampler；每槽缓存 `(id, sig)`）。
        NameCacheEntry& cached = cache_[slot];
        if (sampler_ == nullptr) {
            // 没有解析器 ⇒ 名字只能降级（id / 列表本身照给，⛔ 不因此把整表判成不可用）。
            info.script = degradedName(id);
            info.name_degraded = true;
        } else {
            const std::uint64_t sig = sampler_->nameTableSignature(mem, id);
            if (cached.valid && cached.id == id && cached.sig == sig) {
                info.script = cached.name;
                info.name_degraded = cached.degraded;
            } else {
                MarkerSample s;
                s.valid = true;
                s.id = id;
                sampler_->resolveName(mem, &s);  // C13：解析器只看 id，不看 cur
                info.script = s.name;
                info.name_degraded = s.name_degraded;
                cached.valid = true;
                cached.id = id;
                cached.sig = s.name_table_sig;
                cached.name = s.name;
                cached.degraded = s.name_degraded;
            }
        }
        out.push_back(std::move(info));
    }

    // ⑦ 整表读完再看一眼 engine：变了（退出 / 重开）⇒ 丢弃结果（报告的安全读取第 8 步）。
    std::uint32_t engine_after = 0;
    if (!readU32(mem, kEngineSingletonGlobal, &engine_after) || engine_after != engine) {
        out.clear();
        return unavailable(err, "engine-changed");
    }
    // ⑧ 一个活动槽都没有（理论上根帧至少应活动）⇒ 整表不可用（⛔ 不给一个空数组冒充"读到 0 帧"）。
    if (out.empty()) return unavailable(err, "no-active-frame");

    // ⑨ top-first：`cur` 若活动 ⇒ 提到第一；其余保持槽号降序（循环已按降序收集）。
    //    `cur` 不活动（例如根还没装载 / 被 `0x7` 释放）⇒ 不动，`top` 已经全 false。
    if (out.front().slot != cur) {
        for (std::size_t i = 1; i < out.size(); ++i) {
            if (out[i].slot != cur) continue;
            ui::UiFrameInfo top = std::move(out[i]);
            out.erase(out.begin() + static_cast<std::ptrdiff_t>(i));
            out.insert(out.begin(), std::move(top));
            break;
        }
    }
    return out;
}

}  // namespace lift
