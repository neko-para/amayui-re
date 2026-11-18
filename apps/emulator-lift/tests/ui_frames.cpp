// emulator-lift · `game.local_frames()`（帧列表 / 栈顶脚本名）—— **pure** 守卫（E）
//
// 判据（红了意味着什么）：
//   ① **活动判据 + 顺序 + top**：活动 = `record+0x00 != 0` 且已提交（不是"id 非 0"、不是"0..cur
//      全算"）；`top = (slot == cur)`；顺序 = **top-first**（`cur` 若活动第一，其余按槽号降序）。
//      红了 ⇒ HUD 的 `script` 行会显示另一格（栈顶定义漂了 / 空槽被算成帧 / 顺序变了）。
//   ② **空槽 / body 指向未提交页 / 记录未提交**：一律**不进表**（保守跳过，⛔ 不猜）。
//      红了 ⇒ 会把"读不到"当"没有帧"或把未初始化槽当活动帧。
//   ③ **整表不可用 ⇒ 空表 + err 静态字面量**：engine 未发布 / engine 跨 4 GiB / cur 未提交 /
//      cur 越界 / 第 0 条记录未提交 / 没有活动槽。红了 ⇒ 脚本侧会拿到一个编出来的空数组/0。
//   ④ **cur 可读但那一格不活动** ⇒ 列表照给、`top` 全 false（⛔ 不挑最高槽冒充栈顶）。
//   ⑤ **名字**：复用 `MarkerSampler`（id→名字），id 或名字表指纹变了要重新解析；名字表读不到 ⇒
//      降级成 `id:0x%08X` 但**仍进表**（⛔ 不把整表判成 undefined）。
//   ⑥ **JS 面**：`game.local_frames()` 返回数组（top-first，字段齐全、`parent`/`script_id` 是
//      uint32 原值）；`frames == nullptr` / 宿主 err / 空表 ⇒ `undefined`；属性不可写 / 不可枚举 /
//      不可配置（strict 赋值 / delete ⇒ TypeError）。
// ★ 夹具 = 裸 `GuestMemory` + `MarkerSampler` + `EngineFrameSource` 合成 Engine（形状照
//   `tests/input_marker.cpp` 的 kBase/kFrameBase/put32/installRootTable/putName）。
//   ⛔ 不依赖游戏安装 / LFS / 真实窗口。★ 所有 4 字节写前先 `committed`（safe 模式缺省关，
//   漏提交就是宿主 SIGSEGV）。
// ★ 顶层无条件注册 = **pure** 档（AGENTS.md §12.4：判据 = 注册位置）。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "check.h"
#include "runtime/engine_frames.h"
#include "runtime/engine_marker.h"
#include "runtime/memory.h"
#include "ui/element.h"
#include "ui/script.h"

namespace {

// 假 Engine 落在 [0x00480000, 0x00580000)：覆盖 cur / 40 条记录 / FileDB（+ 名字数组）与
// Engine 单例全局 `0x0055E1BC`（同一块 ⇒ 不用第二次 commit）。
constexpr std::uint32_t kBase = 0x00480000u;
constexpr std::uint32_t kBaseSize = 0x00100000u;
constexpr std::uint32_t kEngineGlobal = lift::kEngineSingletonGlobal;
constexpr std::uint32_t kNameBase = 0x004A0000u;
constexpr std::uint32_t kCodeBase = 0x004C0000u;  // 记录 +0x00 指向的"代码区基址"（必须已提交）
constexpr std::uint32_t kFrameBase = kBase + lift::kEngineFrameBase;
constexpr std::uint32_t kCurEa = kBase + lift::kEngineCurOffset;
constexpr std::uint32_t kFileDbEa = kBase + lift::kFileIndexOffset;

void put32(lift::GuestMemory& mem, std::uint32_t ea, std::uint32_t v) {
    LIFT_CHECK(mem.committed(ea, 4u));
    mem.wr32(ea, v);
}

struct Fx {
    lift::GuestMemory mem;
    lift::MarkerSampler sampler;
    lift::EngineFrameSource source;
    std::uint32_t engine = kBase;

    bool make() {
        if (!mem.reserve()) {
            std::fprintf(stderr, "GuestMemory::reserve 失败：%s\n", mem.lastError());
            return false;
        }
        if (!mem.commitZeroed(kBase, kBaseSize, lift::RegionKind::Heap, "fake-engine")) {
            std::fprintf(stderr, "commitZeroed 失败：%s\n", mem.lastError());
            return false;
        }
        source.bind(&mem, &sampler);
        publish(kBase);
        return true;
    }

    void publish(std::uint32_t value) {
        engine = value;
        put32(mem, kEngineGlobal, value);  // 发布单例（0x4BAA3A 的同形）
    }

    void setCur(std::uint32_t cur) { put32(mem, engine + lift::kEngineCurOffset, cur); }

    std::uint32_t recOf(std::uint32_t slot) const {
        return engine + lift::kEngineFrameBase + lift::kFrameStride * slot;
    }

    // 空槽的规范形状（C4）：body=0 / id=-1 / parent=-1。
    void clearFrames() {
        for (std::uint32_t slot = 0; slot < lift::kFrameCount; ++slot) {
            const std::uint32_t rec = recOf(slot);
            if (!mem.committed(rec, lift::kFrameStride)) continue;
            put32(mem, rec, 0);
            put32(mem, rec + lift::kFrameParentOffset, 0xFFFFFFFFu);
            put32(mem, rec + lift::kFrameIdentityOffset, 0xFFFFFFFFu);
        }
    }

    // 一条"已装载"记录：body 非 0（指向已提交页）、id、parent 原样。
    void setRecord(std::uint32_t slot, std::uint32_t body, std::uint32_t id, std::uint32_t parent) {
        const std::uint32_t rec = recOf(slot);
        put32(mem, rec + lift::kFrameBodyBaseOffset, body);
        put32(mem, rec + lift::kFrameIdentityOffset, id);
        put32(mem, rec + lift::kFrameParentOffset, parent);
    }

    void installRootTable(std::uint32_t count) {
        put32(mem, kFileDbEa + lift::kFileRootCountOffset, count);
        put32(mem, kFileDbEa + lift::kFileRootNameBaseOffset, kNameBase);
    }

    // 名字条目：名字在 80 字节**开头**、NUL 结尾（与 `tests/input_marker.cpp` 同形）。
    void putName(std::uint32_t id, const char* name) {
        const std::uint32_t entry = kNameBase + lift::kFileNameStride * id;
        const std::size_t n = std::strlen(name);
        for (std::size_t i = 0; i < n; ++i) {
            LIFT_CHECK(mem.committed(entry + static_cast<std::uint32_t>(i), 1u));
            mem.wr8(entry + static_cast<std::uint32_t>(i), static_cast<std::uint8_t>(name[i]));
        }
        LIFT_CHECK(mem.committed(entry + static_cast<std::uint32_t>(n), 1u));
        mem.wr8(entry + static_cast<std::uint32_t>(n), 0);
    }

    std::vector<lift::ui::UiFrameInfo> frames(std::string* err) { return source.localFrames(err); }
};

// 期望的槽序（top-first）/ top / id / parent / 名字 / 降级标志。
struct Want {
    std::uint32_t slot;
    bool top;
    std::uint32_t id;
    std::uint32_t parent;
    const char* name;
    bool degraded;
};

void expectList(const std::vector<lift::ui::UiFrameInfo>& got, const std::vector<Want>& want, const char* what) {
    ++lift_test::g_checks;
    if (got.size() != want.size()) {
        lift_test::failf(__FILE__, __LINE__, "%s：项数期望 %zu，得到 %zu", what, want.size(), got.size());
        return;
    }
    for (std::size_t i = 0; i < want.size(); ++i) {
        const lift::ui::UiFrameInfo& f = got[i];
        ++lift_test::g_checks;
        if (f.slot != want[i].slot || f.top != want[i].top || f.script_id != want[i].id ||
            f.parent != want[i].parent || f.script != want[i].name || f.name_degraded != want[i].degraded) {
            lift_test::failf(__FILE__, __LINE__,
                             "%s[%zu]：期望 slot=%u top=%d id=0x%08X parent=0x%08X script=`%s` degraded=%d，"
                             "得到 slot=%u top=%d id=0x%08X parent=0x%08X script=`%s` degraded=%d",
                             what, i, want[i].slot, static_cast<int>(want[i].top), want[i].id, want[i].parent,
                             want[i].name, static_cast<int>(want[i].degraded), f.slot, static_cast<int>(f.top),
                             f.script_id, f.parent, f.script.c_str(), static_cast<int>(f.name_degraded));
        }
    }
}

// ---- JS 面夹具：假的 UiFrameSource（两个条目 / err / 空表）----
struct FakeFrameSource final : lift::ui::UiFrameSource {
    std::vector<lift::ui::UiFrameInfo> list;
    std::string err;
    std::vector<lift::ui::UiFrameInfo> localFrames(std::string* out_err) override {
        if (out_err != nullptr) *out_err = err;
        return list;
    }
};

const char* kJsFramesScript = R"JS(
globalThis.render = function render() {
  const f = game.local_frames();
  if (f === undefined) {
    ui.text({ x: 0, y: 0, text: 'frames=undefined', size: 1 });
    return;
  }
  ui.text({ x: 0, y: 0, text: 'frames=array:' + Array.isArray(f) + ':' + f.length, size: 1 });
  for (let i = 0; i < f.length; ++i) {
    const e = f[i];
    ui.text({ x: 0, y: 0, text: 'e' + i + '=' + e.slot + ':' + e.script_id + ':' + e.parent + ':' +
                                      e.script + ':' + e.name_degraded + ':' + e.top, size: 1 });
  }
};
)JS";

const char* kJsReadonlyScript = R"JS(
globalThis.render = function render() {
  const got = [];
  try { game.local_frames = null; got.push('assign-ok'); } catch (e) { got.push(e.name); }
  try { delete game.local_frames; got.push('delete-ok'); } catch (e) { got.push('del-' + e.name); }
  const enumerable = Object.prototype.propertyIsEnumerable.call(game, 'local_frames');
  ui.text({ x: 0, y: 0, text: 'typeof=' + (typeof game.local_frames) + '|' + got.join(',') +
                                    '|enum=' + enumerable, size: 1 });
};
)JS";

std::vector<std::string> renderTexts(lift::ui::UiScript& script, const lift::ui::UiFrameReadings& readings) {
    lift::ui::UiBuilder builder;
    std::string err;
    const bool ok = script.render(builder, readings, &err);
    ++lift_test::g_checks;
    if (!ok) lift_test::failf(__FILE__, __LINE__, "render 失败：%s", err.c_str());
    std::vector<std::string> out;
    for (const lift::ui::Element& e : builder.elements()) {
        if (e.kind == lift::ui::ElementKind::Text) out.push_back(e.text);
    }
    return out;
}

void expectText(const std::vector<std::string>& got, std::size_t i, const char* want) {
    ++lift_test::g_checks;
    if (i >= got.size()) {
        lift_test::failf(__FILE__, __LINE__, "缺第 %zu 条文本（期望 `%s`）", i, want);
        return;
    }
    if (got[i] != want) {
        lift_test::failf(__FILE__, __LINE__, "第 %zu 条文本：期望 `%s`，得到 `%s`", i, want, got[i].c_str());
    }
}

void caseJsFrames() {
    using namespace lift_test;
    g_current_case = "ui-frames/js-local-frames";
    auto runtime = lift::ui::makeQuickJsRuntime();
    LIFT_CHECK(runtime != nullptr);
    if (runtime == nullptr) return;
    std::string load_err;
    std::unique_ptr<lift::ui::UiScript> script =
        runtime->load(kJsFramesScript, "ui-frames.js", nullptr, &load_err);
    LIFT_CHECK(script != nullptr);
    if (script == nullptr) {
        std::fprintf(stderr, "加载脚本失败：%s\n", load_err.c_str());
        return;
    }

    FakeFrameSource fake;
    {
        lift::ui::UiFrameInfo top;
        top.slot = 3;
        top.script_id = 5;
        top.parent = 1;
        top.script = "SC0005";
        top.name_degraded = false;
        top.top = true;
        lift::ui::UiFrameInfo below;
        below.slot = 1;
        below.script_id = 3;
        below.parent = 0xFFFFFFFFu;  // uint32 原值 ⇒ JS 侧必须是 4294967295（⛔ 不转成 -1）
        below.script = "id:0x00000003";
        below.name_degraded = true;
        below.top = false;
        fake.list = {top, below};
    }
    lift::ui::UiFrameReadings r;
    r.frames = &fake;
    const std::vector<std::string> texts = renderTexts(*script, r);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(texts.size()), 3);
    expectText(texts, 0, "frames=array:true:2");
    expectText(texts, 1, "e0=3:5:1:SC0005:false:true");
    expectText(texts, 2, "e1=1:3:4294967295:id:0x00000003:true:false");

    // `frames == nullptr` ⇒ undefined（宿主没接 / headless / 叠层没开）。
    lift::ui::UiFrameReadings no_source;
    const std::vector<std::string> none = renderTexts(*script, no_source);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(none.size()), 1);
    expectText(none, 0, "frames=undefined");

    // 宿主返回 err ⇒ undefined（即使它给了条目）。
    FakeFrameSource broken;
    broken.err = "no-active-frame";
    broken.list = fake.list;
    lift::ui::UiFrameReadings broken_r;
    broken_r.frames = &broken;
    const std::vector<std::string> bad = renderTexts(*script, broken_r);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(bad.size()), 1);
    expectText(bad, 0, "frames=undefined");

    // 空表（没有 err）⇒ undefined（⛔ 不编一个空数组冒充读数）。
    FakeFrameSource empty;
    lift::ui::UiFrameReadings empty_r;
    empty_r.frames = &empty;
    const std::vector<std::string> empty_texts = renderTexts(*script, empty_r);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(empty_texts.size()), 1);
    expectText(empty_texts, 0, "frames=undefined");

    // 只读 / 不可枚举 / 不可配置：strict 下赋值与 delete 都是 TypeError。
    g_current_case = "ui-frames/js-local-frames-readonly";
    std::string ro_err;
    std::unique_ptr<lift::ui::UiScript> ro_script =
        runtime->load(kJsReadonlyScript, "ui-frames-readonly.js", nullptr, &ro_err);
    LIFT_CHECK(ro_script != nullptr);
    if (ro_script != nullptr) {
        const std::vector<std::string> ro = renderTexts(*ro_script, r);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(ro.size()), 1);
        expectText(ro, 0, "typeof=function|TypeError,del-TypeError|enum=false");
    }
}

}  // namespace

int main() {
    using namespace lift_test;

    Fx fx;
    if (!fx.make()) {
        LIFT_CHECK(false);
        return finish("ui-frames");
    }

    // ---- ① / ② / ⑤：活动槽 + top-first + 名字；空槽与未提交 body 不进表 ----
    g_current_case = "ui-frames/active-slots-and-names";
    fx.publish(kBase);
    fx.clearFrames();
    fx.installRootTable(8);
    fx.putName(0, "ROOT.BIN");
    fx.putName(3, "SC0003");
    fx.putName(5, "SC0005");
    fx.setRecord(0, kCodeBase, 0, 0xFFFFFFFFu);
    fx.setRecord(1, kCodeBase, 3, 0);
    fx.setRecord(3, kCodeBase, 5, 1);
    fx.setRecord(5, 0x00900000u, 9, 1);  // body 指向**未提交**页 ⇒ 非活动（保守跳过）
    fx.setCur(1);
    {
        std::string err;
        const std::vector<lift::ui::UiFrameInfo> list = fx.frames(&err);
        LIFT_CHECK(err.empty());
        expectList(list,
                   {
                       {1, true, 3, 0, "SC0003", false},
                       {3, false, 5, 1, "SC0005", false},
                       {0, false, 0, 0xFFFFFFFFu, "ROOT.BIN", false},
                   },
                   "active/top-first");
    }

    // ---- ④：cur 可读但那一格不活动 ⇒ 列表照给、top 全 false（⛔ 不挑最高槽） ----
    g_current_case = "ui-frames/cur-not-active-top-none";
    fx.setCur(2);  // 槽 2 = 空槽
    {
        std::string err;
        const std::vector<lift::ui::UiFrameInfo> list = fx.frames(&err);
        LIFT_CHECK(err.empty());
        expectList(list,
                   {
                       {3, false, 5, 1, "SC0005", false},
                       {1, false, 3, 0, "SC0003", false},
                       {0, false, 0, 0xFFFFFFFFu, "ROOT.BIN", false},
                   },
                   "cur-not-active");
    }

    // ---- ⑤：名字表未装载 ⇒ 全部降级，但列表 / id / top 不受影响 ----
    g_current_case = "ui-frames/name-degrade";
    fx.installRootTable(0);  // count=0 = TOC 未装载（解析器降级）
    fx.setCur(1);
    {
        std::string err;
        const std::vector<lift::ui::UiFrameInfo> list = fx.frames(&err);
        LIFT_CHECK(err.empty());
        expectList(list,
                   {
                       {1, true, 3, 0, "id:0x00000003", true},
                       {3, false, 5, 1, "id:0x00000005", true},
                       {0, false, 0, 0xFFFFFFFFu, "id:0x00000000", true},
                   },
                   "degraded");
    }

    // ---- ⑤：名字表从"未装载"→"已装载"且 id 不变 ⇒ 缓存 (id,sig) 必须刷新 ----
    g_current_case = "ui-frames/name-cache-refreshes";
    fx.installRootTable(8);
    {
        std::string err;
        const std::vector<lift::ui::UiFrameInfo> list = fx.frames(&err);
        LIFT_CHECK(err.empty());
        expectList(list,
                   {
                       {1, true, 3, 0, "SC0003", false},
                       {3, false, 5, 1, "SC0005", false},
                       {0, false, 0, 0xFFFFFFFFu, "ROOT.BIN", false},
                   },
                   "cache-refresh");
    }

    // ---- ③：整表不可用的每一条（err 是报告列的静态字面量，逐字断言） ----
    struct FailCase {
        const char* name;
        std::uint32_t engine;
        std::uint32_t cur;
        const char* why;
    };
    // engine 0xFFF56000：+0xAABC8 跨出 4 GiB；engine 0x00600000：cur 那一页没提交。
    const FailCase failures[] = {
        {"ui-frames/cur-out-of-range", kBase, 40u, "cur-out-of-range"},
        {"ui-frames/engine-unpublished", 0u, 0u, "engine-unpublished"},
        {"ui-frames/engine-base-overflow", 0xFFF56000u, 0u, "engine-base-overflow"},
        {"ui-frames/cur-uncommitted", 0x00600000u, 0u, "cur-uncommitted"},
    };
    for (const FailCase& c : failures) {
        g_current_case = c.name;
        fx.publish(c.engine);
        if (c.engine == kBase) fx.setCur(c.cur);
        std::string err;
        const std::vector<lift::ui::UiFrameInfo> list = fx.frames(&err);
        LIFT_CHECK(list.empty());
        LIFT_CHECK(err == c.why);
        if (err != c.why) std::fprintf(stderr, "  %s: 期望 why=%s，得到 `%s`\n", c.name, c.why, err.c_str());
    }

    // ---- ② `cur` 可读、但它那一格的**记录未提交** ⇒ 跳过该槽（不崩、不猜；top 全 false） ----
    g_current_case = "ui-frames/record-uncommitted-slot-skipped";
    fx.publish(0x00600000u);
    // 只提交 cur + 前 15 条记录所在的那一页（0x0065D000..0x0065E000）；槽 39 的记录在下一页。
    LIFT_CHECK(fx.mem.commitZeroed(0x00600000u + 0x0005D000u, 0x1000u, lift::RegionKind::Heap, "frames-page0"));
    fx.setRecord(0, kCodeBase, 7, 0xFFFFFFFFu);
    fx.setCur(39);  // 越出已提交页的那一格
    {
        std::string err;
        const std::vector<lift::ui::UiFrameInfo> list = fx.frames(&err);
        LIFT_CHECK(err.empty());
        // id=7 的名字条目没写过（80 字节全 0 ⇒ 名字为空）⇒ 降级仍然进表。
        expectList(list, {{0, false, 7, 0xFFFFFFFFu, "id:0x00000007", true}}, "record-uncommitted-skip");
    }

    // ---- ③ 第 0 条记录跨页且下一页未提交 ⇒ `frame-region-uncommitted`（整表不可用） ----
    g_current_case = "ui-frames/frame-region-uncommitted";
    // engine=0x0060072C ⇒ frame0 = 0x0065DFC0（mod 0x1000 = 0xFC0）⇒ record 跨到 0x0065E000（未提交）。
    fx.publish(0x0060072Cu);
    {
        std::string err;
        const std::vector<lift::ui::UiFrameInfo> list = fx.frames(&err);
        LIFT_CHECK(list.empty());
        LIFT_CHECK(err == "frame-region-uncommitted");
        if (err != "frame-region-uncommitted") std::fprintf(stderr, "  得到 `%s`\n", err.c_str());
    }

    // ---- ③ 40 槽全部可读但没有一个活动 ⇒ `no-active-frame`（⛔ 不给空数组冒充"读到 0 帧"）----
    g_current_case = "ui-frames/no-active-frame";
    fx.publish(kBase);
    fx.clearFrames();
    fx.setCur(0);
    {
        std::string err;
        const std::vector<lift::ui::UiFrameInfo> list = fx.frames(&err);
        LIFT_CHECK(list.empty());
        LIFT_CHECK(err == "no-active-frame");
        if (err != "no-active-frame") std::fprintf(stderr, "  得到 `%s`\n", err.c_str());
    }

    // ---- ⑥ JS 面（恢复 Engine 指针；JS 用例不读 guest） ----
    fx.publish(kBase);
    fx.setCur(0);
    caseJsFrames();

    return finish("ui-frames");
}
