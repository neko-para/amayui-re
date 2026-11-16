// emulator-lift · `ShimRegistry` 的桩 EA 解析：**槽位索引 ≡ 线性扫描**（等价合同）
//
// ★ 这条用例红了意味着什么（判据）：
//   ① 对**每一个已登记 shim**，`byStubEa(def.stub_ea)` 必须返回 `byId(def.id)` 那一条；
//   ② 对桩页里**每一个槽**（含从未发号的那些），`byStubEa` / `idForStub` 必须与"按下标扫 `defs_`、
//      取**第一条** `stub_ea` 相等者"这个**定义**逐槽相同（未登记槽 ⇒ `nullptr` / 0）；
//   ③ 页外 / 页内未对齐的 EA ⇒ `nullptr`（桩页判据的边界，AOT 的派发热路径只做一次无符号比较）；
//   ④ ② 的"已登记 shim"那一轮**不许走慢路径**（`stubIndexSlowPathHits()` 不动）——
//      这就是任务要求的那条 O(1) 契约：桩 EA 解析**不许**退化成"拿锁 + 扫 385 项"。
//      ⛔ 判据是计数器，**不是墙钟**（不写性能断言：那会变成 flaky）。
//   ⑤ `addRuntime` 追加（`defs_` 可能重新分配）之后，旧的槽映射仍指向**同一条**（id 稳定、不悬垂）；
//      每次都按 id 重新解析（表里存 id / 下标，⛔ 不存 `ShimDef*`）。
//   ⑥ 夹具直写 `ShimDef::stub_ea`（历史用法）之后，答案仍然等于线性扫描（慢路径兜底，语义不变）。
//
// ★ 为什么这不是"锁住内部实现"：② 是**对外可观察的等价性**（旧 `byStubEa` 就是那个定义）；
//   ④ 是任务书要求的 O(1) 契约；①③⑤⑥ 都是调用方看得见的结果。⛔ 不断言任何具体 EA / 计数取值。
#include <cstdint>
#include <string>

#include "check.h"
#include "runtime/shims.h"

namespace {

constexpr std::uint32_t kPage = 0x7FF80000u;   // bare 夹具的桩页（⛔ 与 GuestLayout::shim_page 分开）
constexpr std::uint32_t kSlots = 16u;
constexpr std::uint32_t kSpan = kSlots * 4u;

void shimNoop(lift::X86&, lift::ShimCall&) {}

// ★ **oracle = 旧实现本身**：按下标扫 `defs_`，返回第一条 `stub_ea` 相等者（`ea == 0` ⇒ nullptr）。
//   ⛔ 不要用 `byId` / 索引表写这个函数：那会让"等价性"变成同义反复。
const lift::ShimDef* linearByStubEa(const lift::ShimRegistry& r, std::uint32_t ea) {
    if (ea == 0) return nullptr;
    for (const lift::ShimDef& d : r.all()) {
        if (d.stub_ea == ea) return &d;
    }
    return nullptr;
}

bool sameDef(const lift::ShimDef* a, const lift::ShimDef* b) {
    if (a == nullptr || b == nullptr) return a == b;
    return a->id == b->id && a->stub_ea == b->stub_ea && a->module == b->module && a->name == b->name;
}

// 逐槽等价（②）：索引表给出的答案必须与 oracle 逐槽相同（`byStubEa` 与 `idForStub` 两条都查）。
void checkEverySlot(const lift::ShimRegistry& r, std::uint32_t page, std::uint32_t slots, const char* tag) {
    for (std::uint32_t slot = 0; slot < slots; ++slot) {
        const std::uint32_t ea = page + slot * 4u;
        const lift::ShimDef* want = linearByStubEa(r, ea);
        const lift::ShimDef* got = r.byStubEa(ea);
        ++lift_test::g_checks;
        if (!sameDef(got, want)) {
            lift_test::failf(__FILE__, __LINE__,
                             "[%s] slot=%u ea=%08X：byStubEa 给 id=%d，线性扫描给 id=%d（stub 解析不等价）",
                             tag, slot, ea, got != nullptr ? static_cast<int>(got->id) : -1,
                             want != nullptr ? static_cast<int>(want->id) : -1);
        }
        const std::uint32_t want_id = want != nullptr ? want->id : 0u;
        LIFT_CHECK_EQ_U32(r.idForStub(ea), want_id);
    }
    // 桩页里"已发号"的条数 = 索引返回非空的数量（把 ② 的一整轮收成一个总数，防"逐槽都空"假绿）
    std::uint32_t issued = 0;
    for (const lift::ShimDef& d : r.all()) {
        if (d.stub_ea >= page && (d.stub_ea - page) < slots * 4u && (d.stub_ea & 3u) == 0u) ++issued;
    }
    std::uint32_t seen = 0;
    for (std::uint32_t slot = 0; slot < slots; ++slot) {
        if (r.byStubEa(page + slot * 4u) != nullptr) ++seen;
    }
    LIFT_CHECK_EQ_U32(seen, issued);
}

// ① + ④：每个**已登记** shim 都必须由索引表直接命中（不走慢路径），且与 `byId` 同一条。
void checkRegisteredViaIndex(const lift::ShimRegistry& r, const char* tag) {
    r.resetStubIndexStats();
    std::uint32_t checked = 0;
    for (const lift::ShimDef& d : r.all()) {
        if (d.stub_ea == 0) continue;
        const lift::ShimDef* got = r.byStubEa(d.stub_ea);
        LIFT_CHECK(sameDef(got, r.byId(d.id)));
        // ★ 同一个槽被两条登记抢时，旧实现返回"先登记"的那条 ⇒ 只有**第一个**占槽者必须命中自己。
        if (got != nullptr && got->id == d.id) ++checked;
    }
    LIFT_CHECK(checked > 0);
    ++lift_test::g_checks;
    if (r.stubIndexSlowPathHits() != 0) {
        lift_test::failf(__FILE__, __LINE__,
                         "[%s] 已登记 shim 的解析走了 %llu 次慢路径（线性扫描）⇒ 槽位索引表没覆盖它们"
                         "（正是本轮要消掉的 O(385)+锁）",
                         tag, static_cast<unsigned long long>(r.stubIndexSlowPathHits()));
    }
}

}  // namespace

int main() {
    // =====================================================================================
    // ① 逐槽等价 + "已登记 ⇒ 全走索引"（含 add / addUnimplemented / addRuntime 三种登记）
    // =====================================================================================
    {
        lift_test::g_current_case = "stub-index-every-slot";
        lift::ShimRegistry r;
        r.configureStubPage(kPage, kSlots);
        // ★ 先登记两条**没有桩**的（add ⇒ stub_ea 仍是 0）⇒ 后面 addRuntime 拿到的 id 与槽号**不相等**
        //   （id = 注册序、槽 = 发号序）。这正是本轮的**唯一正确性约束**：⛔ 不许把槽号当 id。
        r.add("test", "NoStub0", lift::Conv::Stdcall, 0, &shimNoop);
        r.addUnimplemented("test", "NoStub1", "合同用例的缺口");
        lift::ShimDef* a = r.addRuntime("test", "A", lift::Conv::Stdcall, 0, &shimNoop);
        lift::ShimDef* b = r.addRuntime("test", "B", lift::Conv::Cdecl, 2, &shimNoop);
        r.add("test", "C", lift::Conv::Stdcall, 1, &shimNoop);          // 无桩（bind 才会发号）
        r.addUnimplemented("test", "D", "合同用例的缺口");               // 无桩
        lift::ShimDef* e = r.addRuntime("test", "E", lift::Conv::Stdcall, 0, nullptr);
        LIFT_CHECK(a != nullptr && b != nullptr && e != nullptr);
        LIFT_CHECK(a != nullptr && a->stub_ea == kPage);
        LIFT_CHECK(b != nullptr && b->stub_ea == kPage + 4u);
        LIFT_CHECK(e != nullptr && e->stub_ea == kPage + 8u);
        // ★ 钉住"id ≠ 槽号"这条前提（否则本用例对"槽号当 id"这种错**无感**）
        LIFT_CHECK(a != nullptr && a->id != 0u);
        LIFT_CHECK(b != nullptr && b->id != 1u);
        LIFT_CHECK(e != nullptr && e->id != 2u);
        // 幂等：同名再登记 ⇒ 同一条、同一个槽（⛔ 不得重发地址）
        lift::ShimDef* again = r.addRuntime("test", "A", lift::Conv::Stdcall, 0, &shimNoop);
        LIFT_CHECK(again != nullptr && again->id == a->id && again->stub_ea == a->stub_ea);
        LIFT_CHECK_EQ_U32(r.nextStubSlot(), 3u);
        checkRegisteredViaIndex(r, "every-slot");
        checkEverySlot(r, kPage, kSlots, "every-slot");
    }

    // =====================================================================================
    // ② 追加（`defs_` 可能重新分配）之后：**旧槽映射仍然指向同一条**（id 稳定、不悬垂）
    // =====================================================================================
    {
        lift_test::g_current_case = "stub-index-after-growth";
        lift::ShimRegistry r;
        r.configureStubPage(kPage, kSlots);
        lift::ShimDef* first = r.addRuntime("test", "First", lift::Conv::Stdcall, 0, &shimNoop);
        LIFT_CHECK(first != nullptr);
        const std::uint32_t first_id = first != nullptr ? first->id : 0u;
        const std::uint32_t first_ea = first != nullptr ? first->stub_ea : 0u;
        // 追加到**越过 vector 原来的容量**（8 条起步也够触发一次重分配；这里直接铺满）
        for (std::uint32_t i = 0; i < kSlots - 1u; ++i) {
            const std::string name = "Grow" + std::to_string(i);
            LIFT_CHECK(r.addRuntime("test", name.c_str(), lift::Conv::Stdcall, 0, &shimNoop) != nullptr);
        }
        const lift::ShimDef* again = r.byStubEa(first_ea);
        LIFT_CHECK(again != nullptr && again->id == first_id);
        LIFT_CHECK(sameDef(again, r.byId(first_id)));
        checkRegisteredViaIndex(r, "after-growth");
        checkEverySlot(r, kPage, kSlots, "after-growth");
        // 槽位耗尽：再发就失败（且不破坏已有映射）
        LIFT_CHECK(r.addRuntime("test", "Overflow", lift::Conv::Stdcall, 0, &shimNoop) == nullptr);
        LIFT_CHECK(sameDef(r.byStubEa(first_ea), r.byId(first_id)));
    }

    // =====================================================================================
    // ③ 边界：页外 / 页内未对齐 / 0 / 0xFFFFFFFF ⇒ 与线性扫描一致（通常 nullptr）
    // =====================================================================================
    {
        lift_test::g_current_case = "stub-index-boundaries";
        lift::ShimRegistry r;
        r.configureStubPage(kPage, kSlots);
        LIFT_CHECK(r.addRuntime("test", "Only", lift::Conv::Stdcall, 0, &shimNoop) != nullptr);
        const std::uint32_t probes[] = {
            0u,                        // 空
            0xFFFFFFFFu,               // 栈哨兵
            kPage - 4u,                // 页前一格
            kPage + 1u,                // 页内未对齐
            kPage + 2u,                // 页内未对齐（同一个 slot 的低位）
            kPage + kSpan - 4u,        // 页内最后一格（未发号）
            kPage + kSpan,             // 页后第一格
            kPage + kSpan + 4u,        // 更外面
        };
        for (const std::uint32_t ea : probes) {
            LIFT_CHECK(sameDef(r.byStubEa(ea), linearByStubEa(r, ea)));
            const lift::ShimDef* want = linearByStubEa(r, ea);
            LIFT_CHECK_EQ_U32(r.idForStub(ea), want != nullptr ? want->id : 0u);
        }
        LIFT_CHECK(r.byStubEa(kPage + 1u) == nullptr);
        LIFT_CHECK(r.byStubEa(kPage - 4u) == nullptr);
    }

    // =====================================================================================
    // ④ 夹具直写 `stub_ea`（历史用法）：答案仍等于线性扫描（慢路径兜底，语义不变）
    // =====================================================================================
    {
        lift_test::g_current_case = "stub-index-direct-stub-write";
        lift::ShimRegistry r;
        r.configureStubPage(kPage, kSlots);
        lift::ShimDef* d = r.addRuntime("test", "Patch", lift::Conv::Stdcall, 0, &shimNoop);
        LIFT_CHECK(d != nullptr);
        const std::uint32_t old_ea = d != nullptr ? d->stub_ea : 0u;
        const std::uint32_t new_ea = kPage + 12u * 4u;  // 页内、未发号的一格
        if (d != nullptr) d->stub_ea = new_ea;
        LIFT_CHECK(r.byStubEa(new_ea) != nullptr);
        LIFT_CHECK(r.byStubEa(new_ea) != nullptr && r.byStubEa(new_ea)->id == d->id);
        LIFT_CHECK_EQ_U32(r.idForStub(new_ea), d->id);
        // 旧槽现在是空的（与线性扫描一致 —— 字段真的改了）
        LIFT_CHECK(r.byStubEa(old_ea) == nullptr);
        checkEverySlot(r, kPage, kSlots, "direct-write");
    }

    // =====================================================================================
    // ⑤ 没有桩页：`addRuntime` 响亮失败、任何 EA 都不是桩（与线性扫描一致）
    // =====================================================================================
    {
        lift_test::g_current_case = "stub-index-no-page";
        lift::ShimRegistry r;
        LIFT_CHECK(r.addRuntime("test", "NoPage", lift::Conv::Stdcall, 0, &shimNoop) == nullptr);
        const std::uint32_t probes[] = {0u, kPage, kPage + 4u, 0xFFFFFFFFu};
        for (const std::uint32_t ea : probes) {
            LIFT_CHECK(r.byStubEa(ea) == nullptr);
            LIFT_CHECK_EQ_U32(r.idForStub(ea), 0u);
        }
        LIFT_CHECK_EQ_U32(r.nextStubSlot(), 0u);
    }

    // =====================================================================================
    // ⑥ 小桩页（2 槽）耗尽后：前两格仍然命中、第三格不存在（逐槽对齐 oracle）
    // =====================================================================================
    {
        lift_test::g_current_case = "stub-index-exhaustion";
        lift::ShimRegistry r;
        r.configureStubPage(0x7FF90000u, 2u);
        LIFT_CHECK(r.addRuntime("test", "T1", lift::Conv::Stdcall, 0, &shimNoop) != nullptr);
        LIFT_CHECK(r.addRuntime("test", "T2", lift::Conv::Stdcall, 0, &shimNoop) != nullptr);
        LIFT_CHECK(r.addRuntime("test", "T3", lift::Conv::Stdcall, 0, &shimNoop) == nullptr);
        checkEverySlot(r, 0x7FF90000u, 2u, "exhaustion");
        LIFT_CHECK(r.byStubEa(0x7FF90000u) != nullptr);
        LIFT_CHECK(r.byStubEa(0x7FF90004u) != nullptr);
        LIFT_CHECK(r.byStubEa(0x7FF90008u) == nullptr);  // 桩页外（第 3 格不存在）
    }

    return lift_test::finish("lift-shim-stub-index");
}
