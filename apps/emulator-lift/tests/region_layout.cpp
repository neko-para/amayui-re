// emulator-lift · guest 区域布局的**关系**不变量（真源 = `runtime/image.h` 的 `GuestLayout`）
//
// ★ 为什么有这条（**pure**：只算结构体里的数，不读资产 / 不用 GPU / 不起 Machine；顶层无条件注册）：
//   SAVE07 进战斗前的 `stop : resource-exhausted @ eip=0047839D` 的修法是"抬 SurfaceArena 容量"。
//   arena 的上界一抬就会吃掉 AGERC 的 fallback base（旧的 `0x30000000`，紧贴旧上界）⇒ 容量与
//   fallback base **必须同时动**，而且两份消费者（`Machine::loadAgercModule` /
//   `AotHost::loadAgercModule`）读的必须是同一份常量。
//   "写了守卫"与"守卫真会红"是两件事（`AGENTS.md` §5.3）⇒ 本用例只钉**关系**：
//     ① `GuestLayout` 里所有命名的 guest 区间两两**不相交**（含 arena / 堆 / 栈 / feeder 栈 /
//        TEB·PEB·TLS / argv / 桩页 / feeder TEB）；
//     ② AGERC 的 fallback base **不落在任何区间里**（尤其不在 arena 内），且 **≥ arena 的上界**；
//     ③ 每条区间的 `end` 由 `uint64` 计算 ⇒ 不倒扣 / 不回绕（越出 4 GiB 也算红）。
//   ⇒ 只把 `arena_size` 调大而**不搬** fallback base、或把它改回 arena 里 ⇒ ② 当场红。
//   ⛔ 不断言"容量够用"或"某个地址等于多少"：那些是读数，会随场景变（`AGENTS.md` §10 的文档纪律；
//      判据只允许是"变了就是 bug 且变了是静默的"那种关系）。
//   ⛔ 不断言与 `GuestLayout` 无关的常量（VirtualAlloc bump 区 / ddraw 设备页 / 假句柄空间…）：
//      它们各自在别的文件里，抄进本文件等于造第二份真源。
#include <cstdint>
#include <cstdio>
#include <vector>

#include "check.h"
#include "runtime/image.h"

namespace {

using lift::GuestLayout;
using lift_test::failf;

struct Region {
    const char* name;
    std::uint64_t begin;
    std::uint64_t end;
};

// `GuestLayout` 里的**全部**命名区间（页级常量各占一页）。
std::vector<Region> regionsOf(const GuestLayout& l) {
    std::vector<Region> out;
    auto add = [&out](const char* name, std::uint64_t begin, std::uint64_t size) {
        out.push_back(Region{name, begin, begin + size});
    };
    add("heap", l.heap_base, l.heap_size);
    add("arena", l.arena_base, l.arena_size);
    add("stack",
        static_cast<std::uint64_t>(l.stack_top) - l.stack_size - l.stack_guard, l.stack_size + l.stack_guard);
    add("feeder-stack",
        static_cast<std::uint64_t>(l.feeder_stack_top) - l.feeder_stack_size, l.feeder_stack_size);
    add("teb", l.teb_ea, 0x1000u);
    add("peb", l.peb_ea, 0x1000u);
    add("tls-array", l.tls_array_ea, 0x1000u);
    add("argv-block", l.argv_block, 0x1000u);
    add("shim-page", l.shim_page, static_cast<std::uint64_t>(l.shim_slots) * 4u);
    add("feeder-teb", l.feeder_teb, 0x1000u);
    return out;
}

const Region* findContaining(const std::vector<Region>& regions, std::uint32_t ea) {
    for (const Region& r : regions) {
        if (ea >= r.begin && ea < r.end) return &r;
    }
    return nullptr;
}

// ① 每条区间的形状：非空、且不越出 32 位 guest 地址空间（`uint64` 算 ⇒ 回绕不可达）
void caseShape() {
    lift_test::g_current_case = "region-layout/shape";
    const GuestLayout l;
    const std::vector<Region> regions = regionsOf(l);
    std::printf("region-layout: %zu 个命名区间（GuestLayout 唯一真源 = runtime/image.h）\n", regions.size());
    for (const Region& r : regions) {
        std::printf("  %-12s [0x%08llX,0x%08llX)  %llu B\n", r.name,
                    static_cast<unsigned long long>(r.begin), static_cast<unsigned long long>(r.end),
                    static_cast<unsigned long long>(r.end - r.begin));
        LIFT_CHECK(r.end > r.begin);                     // 非空（size 为 0 / 减法倒扣都算红）
        LIFT_CHECK(r.end <= 0x100000000ull);             // 不越出 4 GiB guest 空间
    }
    LIFT_CHECK(!regions.empty());
}

// ② 两两不相交（含"相等 begin"也算相交 ⇒ 同址两个身份不可接受）
void casePairwiseDisjoint() {
    lift_test::g_current_case = "region-layout/pairwise-disjoint";
    const GuestLayout l;
    const std::vector<Region> regions = regionsOf(l);
    for (std::size_t i = 0; i < regions.size(); ++i) {
        for (std::size_t j = i + 1; j < regions.size(); ++j) {
            const Region& a = regions[i];
            const Region& b = regions[j];
            const bool overlap = a.begin < b.end && b.begin < a.end;
            if (overlap) {
                failf(__FILE__, __LINE__,
                      "区间重叠：%s [0x%08llX,0x%08llX) ∩ %s [0x%08llX,0x%08llX)", a.name,
                      static_cast<unsigned long long>(a.begin), static_cast<unsigned long long>(a.end),
                      b.name, static_cast<unsigned long long>(b.begin),
                      static_cast<unsigned long long>(b.end));
            }
            LIFT_CHECK(!overlap);
        }
    }
}

// ③ AGERC 的 fallback base：不在任何区间里（尤其不在 arena 内），且在 arena 上界之上。
//    ★ 这就是"只抬 arena 不搬 base"会红的那一条：旧 base `0x30000000` 会落进新 arena
//      `[0x28000000,0x38000000)` ⇒ `findContaining` 命中 ⇒ 两条断言同时红。
void caseAgercFallbackBase() {
    lift_test::g_current_case = "region-layout/agerc-fallback-outside";
    const GuestLayout l;
    const std::vector<Region> regions = regionsOf(l);
    const std::uint64_t arena_end = static_cast<std::uint64_t>(l.arena_base) + l.arena_size;

    const Region* hit = findContaining(regions, l.agerc_fallback_base);
    if (hit != nullptr) {
        failf(__FILE__, __LINE__, "AGERC fallback base 0x%08X 落进了 %s [0x%08llX,0x%08llX)",
              l.agerc_fallback_base, hit->name, static_cast<unsigned long long>(hit->begin),
              static_cast<unsigned long long>(hit->end));
    }
    LIFT_CHECK(hit == nullptr);                              // 不在任何命名区间里（含 arena）
    LIFT_CHECK(l.agerc_fallback_base >= arena_end);           // 在 arena 上界之上（有先后关系，不是"碰巧不重叠"）
    LIFT_CHECK(l.agerc_fallback_base >= l.arena_base);        // 不倒扣
    std::printf("region-layout: AGERC fallback base = 0x%08X，arena 上界 = 0x%08llX（余量 %llu B）\n",
                l.agerc_fallback_base, static_cast<unsigned long long>(arena_end),
                static_cast<unsigned long long>(l.agerc_fallback_base - arena_end));
}

}  // namespace

int main() {
    caseShape();
    casePairwiseDisjoint();
    caseAgercFallbackBase();
    return lift_test::finish("region-layout");
}
