// emulator-lift · `FontEngine::metrics` 的**度量缓存**（pure：不需要字体文件）
//
// ★ 守的是什么（每一条都会红，见文件尾"为什么这些断言会红"）：
//   ① `cache/hit`：同一字体同一字号连测两次 ⇒ 第二次**命中**（`metrics` 的 FT 派生段只算一次：
//      假源被调用 1 次、`hits=1 / misses=1`），且两次写出的 56 B 逐字节相同。
//   ② `cache/size-change`：`CreateFont(-20)` → GTM → `CreateFont(-28)` → GTM ⇒ 28 的
//      `tmAscent` / `tmHeight` **大于** 20 的，且 `misses == 2`（换字号必须重算）。
//   ③ `cache/size-back`：再选回 -20 测第三次 ⇒ 与第一次逐字节相同且**命中**（20→28→20 只有首次 miss）。
//   ④ `cache/weight-not-in-key`：同字号、weight 400→500（都不算 bold）⇒ 同一缓存条目（命中），
//      但写出的 `tmWeight` 必须是 500 ⇒ 证明 `weight` **不进键**、且每次仍从 desc 覆盖。
//   ⑤ `cache/synthetic-miss`：`setSyntheticForTest(true)` 出合成签名（`ave=6` / `height=size+2`），
//      关掉之后必须 **miss**（合成值不许泄漏给真字体调用）。
//   ⑥ `cache/config-miss`：`fontSetConfig`（**同一路径**、只改 `baseline_lift`）之后必须 **miss** ——
//      键里本来就有路径，所以这条只有"配置代际清表"生效才会绿。
//   ⑦ `cache/bold-not-aliased`：同字号、weight 700（= bold 档）⇒ 必须是**另一条**（键里的 `bold` 维），
//      再选回非 bold 仍命中旧条目（没有把表清掉）。
//   ⑧ `cache/profile-line`：`--profile` 的表里必须打得出
//      `font!TEXTMETRIC-cache hits=… misses=… hit=…%`，且数字与存取器一致（可观测性本身也是判据）。
//
// ★ 为什么能在 pure 档跑：`FontEngine::setMetricsSourceForTest` 只替换"FT 派生段怎么算"，
//   **缓存 / 键 / 失效 / 计数**全部走生产路径（`FontEngine::metrics`）。假源让 ascent 随字号线性变化，
//   所以"串键"（把 20 的条目当 28 用）一定表现为 28 的 ascent 变大失败。
//
// ★ 变异自检（C++ 没有 mutate 清单 ⇒ 手动施加、确认红、再撤销；详见交付说明）：
//   · 让缓存恒返回第一条（忽略键）⇒ ②③ 红（28 的 ascent 不再大于 20）。
//   · 删掉 `fontSetConfig` 的代际清表 ⇒ ⑥ 红。
//   · 删掉 `setSyntheticForTest` 里的 `metrics_cache.clear()` ⇒ ⑤ 红。
//   · 删掉 `gdiTextMetrics` 里的 `out->weight = desc.weight;` ⇒ ④ 红（那是写 weight 的**唯一**一处：
//     `metrics()` 只出 FT 派生段，不再碰这个字段 —— 两处都写时删任一处都照样绿，实测过）。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>  // _dup / _dup2 / _fileno / _close
#else
#include <unistd.h>  // dup / dup2 / fileno / close
#endif

#include "check.h"
#include "runtime/font.h"
#include "runtime/machine.h"
#include "shims/register.h"

namespace {

constexpr std::uint32_t kCode = 0x00401000u;
constexpr std::uint32_t kCode2 = 0x00401400u;
constexpr std::uint32_t kCode3 = 0x00401500u;
constexpr std::uint32_t kCode4 = 0x00401600u;
constexpr std::uint32_t kCode5 = 0x00401700u;
constexpr std::uint32_t kCode6 = 0x00401800u;
constexpr std::uint32_t kCode7 = 0x00401900u;
constexpr std::uint32_t kCode8 = 0x00401A00u;
constexpr std::uint32_t kLogFont20 = 0x00402000u;
constexpr std::uint32_t kLogFont20W500 = 0x00402100u;
constexpr std::uint32_t kLogFont28 = 0x00402200u;
constexpr std::uint32_t kLogFont20Bold = 0x00402300u;
constexpr std::uint32_t kTm20a = 0x00403000u;
constexpr std::uint32_t kTm20b = 0x00403100u;
constexpr std::uint32_t kTm28 = 0x00403200u;
constexpr std::uint32_t kTm20w500 = 0x00403300u;
constexpr std::uint32_t kTm20c = 0x00403400u;
constexpr std::uint32_t kTmSyn = 0x00403500u;
constexpr std::uint32_t kTmAfterSyn = 0x00403600u;
constexpr std::uint32_t kTmAfterCfg = 0x00403700u;
constexpr std::uint32_t kTmBold = 0x00403800u;
constexpr std::uint32_t kDisplay = 0x00402640u;
constexpr std::uint32_t kTmSize = 56;
constexpr std::uint32_t kHdc = 0x00402710u;
constexpr std::uint32_t kFont20 = 0x00402714u;
constexpr std::uint32_t kFont28 = 0x00402718u;
constexpr std::uint32_t kFont20w500 = 0x0040271Cu;
constexpr std::uint32_t kFont20Bold = 0x00402720u;
constexpr std::uint32_t kStubBase = 0x7FF80000u;
constexpr std::uint32_t kSentinel = 0xFFFFFFFFu;

struct Asm {
    explicit Asm(std::uint32_t code_base) : base(code_base) {}
    std::vector<std::uint8_t> b;
    std::uint32_t base = 0;
    std::uint32_t ea() const { return base + static_cast<std::uint32_t>(b.size()); }
    void u8(std::uint8_t v) { b.push_back(v); }
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
    }
    void pushImm(std::uint32_t v) { u8(0x68); u32(v); }
    void pushMem(std::uint32_t slot) { u8(0xFF); u8(0x35); u32(slot); }
    void callRel(std::uint32_t target) {
        const std::uint32_t at = ea();
        u8(0xE8);
        u32(static_cast<std::uint32_t>(static_cast<std::int32_t>(target - (at + 5))));
    }
    void storeEax(std::uint32_t slot) { u8(0xA3); u32(slot); }
    void ret() { u8(0xC3); }
};

void put32(std::vector<std::uint8_t>& buf, std::size_t off, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) buf[off + i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
}

// 纯档接缝的"FT 派生段"：ascent / height 随字号**严格变大**，与真实字体同形。
// 只数被调用了几次（= `metrics` 真的算了几次）。
struct FakeSource {
    int calls = 0;
};

bool fakeMetrics(void* user, int pixel_size, bool bold, lift::HostTextMetrics* out) {
    auto* state = static_cast<FakeSource*>(user);
    ++state->calls;
    out->ascent = pixel_size;  // 20 → 20、28 → 28（串键时 28 会拿到 20）
    out->descent = 2;
    out->height = out->ascent + out->descent;
    out->internal_leading = 2;
    out->external_leading = 0;
    out->ave_char_width = pixel_size / 2 + (bold ? 1 : 0);
    out->max_char_width = pixel_size;
    out->overhang = 0;
    return true;
}

std::uint32_t stubOf(lift::Machine& m, const char* name) {
    const lift::ShimDef* d = m.shims().find("gdi32", name);
    if (!d || !d->stub_ea) {
        std::fprintf(stderr, "stub missing: %s\n", name);
        return 0;
    }
    return d->stub_ea;
}

void reprimeStack(lift::Machine& m) {
    m.cpu().esp() = m.layout().stack_top - 4u;
    m.mem().wr32(m.cpu().esp(), 0xFFFFFFFFu);
    m.cpu().running = true;
}

bool runCode(lift::Machine& m, std::uint32_t code_ea) {
    m.cpu().eip = code_ea;
    lift::RunOptions o;
    o.until_ea = kSentinel;
    o.max_insns = 200000;
    return m.run(o).reason == lift::StopReason::UntilEa;
}

bool sameBytes(lift::Machine& m, std::uint32_t a, std::uint32_t b) {
    for (std::uint32_t i = 0; i < kTmSize; ++i) {
        if (m.mem().rd8(a + i) != m.mem().rd8(b + i)) return false;
    }
    return true;
}

std::int32_t tmField(lift::Machine& m, std::uint32_t ea, std::uint32_t off) {
    return static_cast<std::int32_t>(m.mem().rd32(ea + off));
}

// ---- stdout 捕获（`dumpProfile` 打 stdout；Windows 走 _dup/_dup2，POSIX 走 dup/dup2）----
#ifdef _WIN32
int dupFd(int fd) { return _dup(fd); }
int dup2Fd(int src, int dst) { return _dup2(src, dst); }
int filenoOf(std::FILE* f) { return _fileno(f); }
void closeFd(int fd) { _close(fd); }
#else
int dupFd(int fd) { return ::dup(fd); }
int dup2Fd(int src, int dst) { return ::dup2(src, dst); }
int filenoOf(std::FILE* f) { return ::fileno(f); }
void closeFd(int fd) { ::close(fd); }
#endif

struct StdoutCapture {
    std::FILE* tmp = nullptr;
    int saved = -1;
    bool active = false;

    bool begin() {
        tmp = std::tmpfile();
        if (tmp == nullptr) return false;
        std::fflush(stdout);
        saved = dupFd(1);
        if (saved < 0) return false;
        if (dup2Fd(filenoOf(tmp), 1) < 0) return false;
        active = true;
        return true;
    }

    std::string finish() {
        if (!active || tmp == nullptr) return {};
        std::fflush(stdout);
        dup2Fd(saved, 1);
        closeFd(saved);
        saved = -1;
        active = false;
        std::string out;
        std::fseek(tmp, 0, SEEK_END);
        const long size = std::ftell(tmp);
        if (size > 0) {
            out.resize(static_cast<std::size_t>(size));
            std::fseek(tmp, 0, SEEK_SET);
            const std::size_t got = std::fread(out.data(), 1, out.size(), tmp);
            out.resize(got);
        }
        std::fclose(tmp);
        tmp = nullptr;
        return out;
    }
};

}  // namespace

int main() {
    using namespace lift_test;

    lift::Machine m;
    std::string err;
    g_current_case = "setup";
    LIFT_CHECK(m.initBare(&err));
    LIFT_CHECK(m.prepareStack(&err));
    lift::registerGdiFontShims(m.shims());
    {
        const std::size_t count = m.shims().all().size();
        for (std::uint32_t id = 0; id < count; ++id) {
            lift::ShimDef* d = const_cast<lift::ShimDef*>(m.shims().byId(id));
            if (!d || !d->fn) continue;
            d->stub_ea = kStubBase + id * 4u;
            m.cache().installShim(d->stub_ea, d->id);
        }
    }

    // 假源必须先装：生产路径（真 FT）在这条测试里永不执行。
    FakeSource fake;
    m.fonts().setMetricsSourceForTest(&fakeMetrics, &fake);

    const std::vector<std::uint8_t> zeros(0x400, 0);
    std::vector<std::uint8_t> lf20(60, 0);
    put32(lf20, 0, static_cast<std::uint32_t>(-20));
    put32(lf20, 16, 400);
    std::vector<std::uint8_t> lf20w500 = lf20;
    put32(lf20w500, 16, 500);
    std::vector<std::uint8_t> lf28(60, 0);
    put32(lf28, 0, static_cast<std::uint32_t>(-28));
    put32(lf28, 16, 400);
    std::vector<std::uint8_t> lf20bold = lf20;
    put32(lf20bold, 16, 700);  // `gdiBold` = weight >= 700 ⇒ 另一份 face、另一个键
    const char display[] = "DISPLAY";
    LIFT_CHECK(m.placeData(kLogFont20, lf20.data(), lf20.size(), &err));
    LIFT_CHECK(m.placeData(kLogFont20W500, lf20w500.data(), lf20w500.size(), &err));
    LIFT_CHECK(m.placeData(kLogFont28, lf28.data(), lf28.size(), &err));
    LIFT_CHECK(m.placeData(kLogFont20Bold, lf20bold.data(), lf20bold.size(), &err));
    LIFT_CHECK(m.placeData(kDisplay, display, sizeof(display), &err));
    for (const std::uint32_t slot : {kTm20a, kTm20b, kTm28, kTm20w500, kTm20c, kTmSyn, kTmAfterSyn,
                                     kTmAfterCfg, kTmBold}) {
        LIFT_CHECK(m.placeData(slot, zeros.data(), kTmSize, &err));
    }
    for (const std::uint32_t slot : {kHdc, kFont20, kFont28, kFont20w500, kFont20Bold}) {
        LIFT_CHECK(m.placeData(slot, zeros.data(), 4, &err));
    }

    // ---- 程序 1：建 DC / 字体 -20 / 选入 / GTM×2（第二次必须命中）----
    {
        Asm a(kCode);
        a.pushImm(0);
        a.pushImm(0);
        a.pushImm(0);
        a.pushImm(kDisplay);
        a.callRel(stubOf(m, "CreateICA"));
        a.storeEax(kHdc);
        a.pushImm(kLogFont20);
        a.callRel(stubOf(m, "CreateFontIndirectA"));
        a.storeEax(kFont20);
        a.pushMem(kFont20);
        a.pushMem(kHdc);
        a.callRel(stubOf(m, "SelectObject"));
        a.pushImm(kTm20a);
        a.pushMem(kHdc);
        a.callRel(stubOf(m, "GetTextMetricsA"));
        a.pushImm(kTm20b);
        a.pushMem(kHdc);
        a.callRel(stubOf(m, "GetTextMetricsA"));
        a.ret();
        LIFT_CHECK(m.placeCode(kCode, a.b.data(), a.b.size(), &err));
    }
    g_current_case = "run-1";
    LIFT_CHECK(runCode(m, kCode));
    const std::uint32_t hdc = m.mem().rd32(kHdc);
    LIFT_CHECK_NE_U32(hdc, 0u);

    // ---- ① 同一字体同一字号连测两次 ⇒ 第二次命中，且两 56B 逐字节相同 ----
    g_current_case = "cache/hit";
    LIFT_CHECK_EQ_I64(fake.calls, 1);  // `metrics` 的 FT 派生段只算一次
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(m.fonts().metricsMisses()), 1u);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(m.fonts().metricsHits()), 1u);
    LIFT_CHECK(sameBytes(m, kTm20a, kTm20b));

    // ---- 程序 2：字体 -28 / 选入 / GTM（换字号必须重算，且不许串键）----
    {
        Asm a(kCode2);
        a.pushImm(kLogFont28);
        a.callRel(stubOf(m, "CreateFontIndirectA"));
        a.storeEax(kFont28);
        a.pushMem(kFont28);
        a.pushMem(kHdc);
        a.callRel(stubOf(m, "SelectObject"));
        a.pushImm(kTm28);
        a.pushMem(kHdc);
        a.callRel(stubOf(m, "GetTextMetricsA"));
        a.ret();
        LIFT_CHECK(m.placeCode(kCode2, a.b.data(), a.b.size(), &err));
    }
    g_current_case = "run-2";
    reprimeStack(m);
    LIFT_CHECK(runCode(m, kCode2));

    // ---- ② 28 的 ascent / height **大于** 20 的，且 misses == 2 ----
    g_current_case = "cache/size-change";
    LIFT_CHECK_EQ_I64(tmField(m, kTm20a, 4), 20);   // tmAscent（假源：= 像素字号）
    LIFT_CHECK_EQ_I64(tmField(m, kTm20a, 0), 22);   // tmHeight = ascent + descent
    LIFT_CHECK_EQ_I64(tmField(m, kTm28, 4), 28);    // 串键时这里是 20
    LIFT_CHECK_EQ_I64(tmField(m, kTm28, 0), 30);
    LIFT_CHECK(tmField(m, kTm28, 4) > tmField(m, kTm20a, 4));
    LIFT_CHECK(tmField(m, kTm28, 0) > tmField(m, kTm20a, 0));
    LIFT_CHECK_EQ_I64(fake.calls, 2);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(m.fonts().metricsMisses()), 2u);

    // ---- 程序 3：字体 -20 但 weight=500（不算 bold ⇒ 与 -20/400 同一个键）/ 选入 / GTM ----
    {
        Asm a(kCode3);
        a.pushImm(kLogFont20W500);
        a.callRel(stubOf(m, "CreateFontIndirectA"));
        a.storeEax(kFont20w500);
        a.pushMem(kFont20w500);
        a.pushMem(kHdc);
        a.callRel(stubOf(m, "SelectObject"));
        a.pushImm(kTm20w500);
        a.pushMem(kHdc);
        a.callRel(stubOf(m, "GetTextMetricsA"));
        a.ret();
        LIFT_CHECK(m.placeCode(kCode3, a.b.data(), a.b.size(), &err));
    }
    g_current_case = "run-3";
    reprimeStack(m);
    LIFT_CHECK(runCode(m, kCode3));

    // ---- ④ weight 不进键：命中同一个条目，但 tmWeight 必须是**本次**的 ----
    g_current_case = "cache/weight-not-in-key";
    LIFT_CHECK_EQ_I64(tmField(m, kTm20w500, 28), 500);  // 命中路径也必须覆盖 weight
    LIFT_CHECK_EQ_I64(tmField(m, kTm20a, 28), 400);
    LIFT_CHECK_EQ_I64(tmField(m, kTm20w500, 4), tmField(m, kTm20a, 4));  // FT 段相同 ⇒ 同一键
    LIFT_CHECK_EQ_I64(fake.calls, 2);                                    // 没有重算
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(m.fonts().metricsHits()), 2u);

    // ---- 程序 4：选回 -20 / GTM（20 → 28 → 20，只有第一次 miss）----
    {
        Asm a(kCode4);
        a.pushMem(kFont20);
        a.pushMem(kHdc);
        a.callRel(stubOf(m, "SelectObject"));
        a.pushImm(kTm20c);
        a.pushMem(kHdc);
        a.callRel(stubOf(m, "GetTextMetricsA"));
        a.ret();
        LIFT_CHECK(m.placeCode(kCode4, a.b.data(), a.b.size(), &err));
    }
    g_current_case = "run-4";
    reprimeStack(m);
    LIFT_CHECK(runCode(m, kCode4));

    // ---- ③ 回选小字号：值不变、命中 ----
    g_current_case = "cache/size-back";
    LIFT_CHECK(sameBytes(m, kTm20c, kTm20a));
    LIFT_CHECK_EQ_I64(fake.calls, 2);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(m.fonts().metricsHits()), 3u);

    // ---- 程序 5：合成档的 GTM ----
    {
        Asm a(kCode5);
        a.pushImm(kTmSyn);
        a.pushMem(kHdc);
        a.callRel(stubOf(m, "GetTextMetricsA"));
        a.ret();
        LIFT_CHECK(m.placeCode(kCode5, a.b.data(), a.b.size(), &err));
    }
    g_current_case = "cache/synthetic-on";
    m.fonts().setSyntheticForTest(true);
    reprimeStack(m);
    LIFT_CHECK(runCode(m, kCode5));
    LIFT_CHECK_EQ_I64(tmField(m, kTmSyn, 0), 22);  // 合成签名：height = size + 2
    LIFT_CHECK_EQ_I64(tmField(m, kTmSyn, 4), 20);  // ascent = size
    LIFT_CHECK_EQ_I64(tmField(m, kTmSyn, 20), 6);  // ave = 6（真字体不会有这个值）
    LIFT_CHECK_EQ_I64(fake.calls, 2);              // 合成档不走假源

    // ---- 程序 6：关掉合成档后再 GTM ----
    {
        Asm a(kCode6);
        a.pushImm(kTmAfterSyn);
        a.pushMem(kHdc);
        a.callRel(stubOf(m, "GetTextMetricsA"));
        a.ret();
        LIFT_CHECK(m.placeCode(kCode6, a.b.data(), a.b.size(), &err));
    }
    g_current_case = "cache/synthetic-off-miss";
    m.fonts().setSyntheticForTest(false);
    reprimeStack(m);
    LIFT_CHECK(runCode(m, kCode6));

    // ---- ⑤ 关掉合成档 ⇒ 必须 miss（合成值不许被真字体调用复用）----
    LIFT_CHECK_EQ_I64(tmField(m, kTmAfterSyn, 20), 10);  // 假源 ave = 20/2（不是合成的 6）
    LIFT_CHECK_EQ_I64(fake.calls, 3);                    // ★ 真算了 ⇒ 是 miss
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(m.fonts().metricsMisses()), 3u);

    // ---- 程序 7：`fontSetConfig` 之后的 GTM ----
    {
        Asm a(kCode7);
        a.pushImm(kTmAfterCfg);
        a.pushMem(kHdc);
        a.callRel(stubOf(m, "GetTextMetricsA"));
        a.ret();
        LIFT_CHECK(m.placeCode(kCode7, a.b.data(), a.b.size(), &err));
    }
    g_current_case = "cache/config-miss";
    // ★ 路径进键，所以"换路径"这条天然会 miss；这里**故意不换路径**，只有代际清表才会绿。
    lift::FontConfig cfg = lift::fontConfig();
    cfg.baseline_lift = 0;  // 只改这个（度量与它无关 ⇒ 值必须和 20px 那条一样）
    lift::fontSetConfig(cfg);
    reprimeStack(m);
    LIFT_CHECK(runCode(m, kCode7));

    // ---- ⑥ …之后必须 miss ----
    LIFT_CHECK_EQ_I64(fake.calls, 4);                 // ★ 清表 ⇒ 真算
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(m.fonts().metricsMisses()), 4u);
    LIFT_CHECK(sameBytes(m, kTmAfterCfg, kTm20a));    // 度量不受 baseline_lift 影响

    // ---- 程序 8：同字号但 weight=700（= bold 档）/ 选入 / GTM ----
    {
        Asm a(kCode8);
        a.pushImm(kLogFont20Bold);
        a.callRel(stubOf(m, "CreateFontIndirectA"));
        a.storeEax(kFont20Bold);
        a.pushMem(kFont20Bold);
        a.pushMem(kHdc);
        a.callRel(stubOf(m, "SelectObject"));
        a.pushImm(kTmBold);
        a.pushMem(kHdc);
        a.callRel(stubOf(m, "GetTextMetricsA"));
        a.ret();
        LIFT_CHECK(m.placeCode(kCode8, a.b.data(), a.b.size(), &err));
    }
    g_current_case = "run-5";
    reprimeStack(m);
    LIFT_CHECK(runCode(m, kCode8));

    // ---- ⑦ 键里的 `bold` 维：同字号换 bold **必须**是另一条（假源里 bold 的 ave = size/2 + 1）----
    LIFT_CHECK_EQ_I64(fake.calls, 5);                 // ★ 没有复用非 bold 的那条
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(m.fonts().metricsMisses()), 5u);
    LIFT_CHECK_EQ_I64(tmField(m, kTmBold, 20), 11);   // ave = 20/2 + 1（非 bold 是 10）
    LIFT_CHECK_EQ_I64(tmField(m, kTm20a, 20), 10);
    LIFT_CHECK_EQ_I64(tmField(m, kTmBold, 4), tmField(m, kTm20a, 4));  // ascent 仍是 20（同字号）
    // 回选非 bold 仍命中旧条目（没有因为 bold 的那次把表清掉）
    {
        Asm a(kCode4);
        a.pushMem(kFont20);
        a.pushMem(kHdc);
        a.callRel(stubOf(m, "SelectObject"));
        a.pushImm(kTm20c);
        a.pushMem(kHdc);
        a.callRel(stubOf(m, "GetTextMetricsA"));
        a.ret();
        LIFT_CHECK(m.placeCode(kCode4, a.b.data(), a.b.size(), &err));
    }
    g_current_case = "cache/bold-not-aliased";
    reprimeStack(m);
    LIFT_CHECK(runCode(m, kCode4));
    LIFT_CHECK_EQ_I64(fake.calls, 5);  // 仍是命中

    // ---- ⑧ 可观测：`--profile` 里必须有一行命中率，且数字与存取器一致 ----
    g_current_case = "cache/profile-line";
    {
        StdoutCapture cap;
        LIFT_CHECK(cap.begin());
        m.dumpProfile(stdout, 5);
        const std::string log = cap.finish();
        char expect[128];
        std::snprintf(expect, sizeof(expect), "font!TEXTMETRIC-cache hits=%llu misses=%llu",
                      (unsigned long long)m.fonts().metricsHits(),
                      (unsigned long long)m.fonts().metricsMisses());
        LIFT_CHECK(log.find("font!TEXTMETRIC-cache") != std::string::npos);
        LIFT_CHECK(log.find(expect) != std::string::npos);
        LIFT_CHECK(log.find("hit=") != std::string::npos);
    }

    return finish("gdi-font-metrics-cache");
}
