// emulator-lift · **C2a** 的 pure 判据（QJS 单插件：loader / start·onTrigger / ctx 基础 / 读点顺序）
//
// 判据（红了意味着什么）——逐条按合同 `docs/03-emulator/plugin-runtime.md` §5 C2 的用例名：
//   · `plugin/api-surface`                ：`RunCtx` 的成员与 d.ts 一致；**多一个/少一个都红**
//                                            （`draw`/`run`/`dispatch`/`sample`/`screenshot`/
//                                            `read.frameHash` 必须**缺席**）。D-C2-6 / D-C2-7。
//   · `plugin/initial-state`              ：`initialState` 在 `start` 之前注入；无 `initialState` ⇒ `{}`；
//                                            `ctx.state` 是 per-Run 的（同一 Run 的第二个回调看得到）。
//   · `plugin/concurrent-arm`             ：一读点最多一个（注册顺序）；返回数组**完整替换**；`[]`=done。
//   · `plugin/module-default`（+ pair）   ：只读 `export default`；default 不是对象 / `start` 与
//                                            `onTrigger` 不成对 ⇒ **装载失败并点名**。
//   · `control-script/start-on-trigger-chain`：`loadSave(slot)` 形态的具名链；两个 slot 的 `move`
//                                            落**不同** y（实测表，⛔ 不是公式）；链到第一道确认即 done。
//   · `control-script/global-read-modify-write`：同拍 `read → edit → input`：Edit **立即**（读回新值）、
//                                            **不占** Input 预算（队列里只有那一条 click）。
//   · `plugin/finish-quit`（finish 半）   ：`ctx.finish(5)` 只结束 Run + 记 exit，⛔ 不改进程 exit。
//   · `start 的 ctx.now`（D-C2-10）       ：`start` 里 `ctx.now` 有值（script/pos/readIndex）；
//                                            `start` 里的 click **下一读点**才落地。
//
// C2b（render / ui / draw；`plugin-runtime` 的 pure 半 —— 窗口 `adopt` 是 C2b-2）：
//   · `plugin/render-surface`             ：`RenderCtx` 的成员表 = d.ts 的 RenderCtx **减 `run`**
//                                            （`ui/draw/read/game/params/log`；`Object.keys` 精确比对）；
//                                            ⛔ `edit`/`input`/`state`/`now`/…；`read.frameHash` 缺席；
//                                            没有 `render` 的插件调 render ⇒ true + 空 list。
//   · `plugin/ui-draw`                    ：`ctx.draw.rect/text/measure` 的元素字段（x/y/w/h/fill/
//                                            字号/颜色）、z 序 = 调用顺序、measure `{width,height}`
//                                            （与宿主 `measureText` 交叉核对）；`RenderCtx.ui` 可写。
//   · `plugin/ui-draw-bad-style`          ：`stroke`/`width`/`alpha`/`align` ⇒ **TypeError**（D-C2-3），
//                                            ⛔ 不静默忽略。
//   · `plugin/ui-draw-measure-no-font`    ：没有 `FontEngine` ⇒ measure 抛 TypeError（⛔ 不返回 0 尺寸）。
//   · `plugin/ui-state`                   ：`onTrigger` 只 `ctx.ui.show/hide`（⛔ 回调里没有 draw）；
//                                            本帧 display list 为空；**下一次 render** 才可见；hide 后为空。
//   · `plugin/render-read`                ：render 里的 `ctx.read.global.get` / `ctx.log` 有宿主来路
//                                            （`setCallHost` + `readWired` 不绑回调；⛔ 静默缺失）。
//   · `plugin/render-readings`            ：`read.frames` / `read.scriptPos` / `game.localFrames` /
//                                            `game.mouse` 的字段与"没有源 ⇒ undefined / 缺 pos 键"；
//                                            C2b-2：**有**源时 `mouse().buttons` 三键 = 真实电平。
//   · `plugin/render-stored-runctx-rejected`：把 **RunCtx 存下来**在 render 里用 `fail`/`finish`/
//                                            `quit`/`profile.*` ⇒ TypeError（复审 F2：成员表不是门面）。
//   · `plugin/render-budget-deadline`     ：`PluginScript::setBudget` 是**绝对** deadline
//                                            （过去 1 ns ⇒ render false + `overran()`；复审 F3）。
//
// ★ 夹具口径：**零 `Machine`、零 GPU、零真窗口、零游戏安装**（与 `tests/trigger_runner.cpp` 同一条）：
//   一块假 `GuestMemory`（假 Engine / 假池）+ 一个默认 `WindowManager` + 注入的 `ReadingsSource`。
//   插件回调的全部效果都落在这两样上（Edit → guest 内存；Input → 队列 / 光标）。
// ★ 读点 = `PluginHost::onReadPoint(wm, point)`（它内部就是 `executor_.atPoint` + 首点 `sampleNow`）。
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "check.h"
#include "plugin/plugin_host.h"
#include "runtime/engine_marker.h"  // kEngineSingletonGlobal（假 Engine 的发布点）
#include "runtime/font.h"           // FontEngine（C2b：`ctx.draw.measure` 的来路）
#include "runtime/host_window.h"
#include "runtime/memory.h"
#include "runtime/script_globals.h"
#include "runtime/value_codec.h"  // encodeInt（ENC 往返的断言；⛔ 不写死编码后的数）
#include "ui/raster.h"            // measureText / UiBuilder / Element（C2b 的交叉读数）
#include "ui/script.h"            // UiHostOps（C2b 复审 F2：判 profile 桥有没有被调到）

namespace {

namespace fs = std::filesystem;

// ---- 假 Engine / 假池（逐字照 `tests/trigger_runner.cpp`：同一块地址、同一个 ENC key）----
constexpr std::uint32_t kFxBase = 0x00480000u;
constexpr std::uint32_t kFxBaseSize = 0x00100000u;  // [0x480000, 0x580000)
constexpr std::uint32_t kFxEngine = kFxBase;
constexpr std::uint32_t kFxIntPool = 0x004A0000u;
constexpr std::uint32_t kFxFloatPool = 0x004B0000u;
constexpr std::uint32_t kFxKey = 0x1234ABCDu;  // 运行期 ENC/DEC key（⛔ 不是默认 0）
constexpr std::uint32_t kFxIntCount = 0xB1B6u;
constexpr std::uint32_t kFxFloatCount = 0xB1B6u;
constexpr std::uint32_t kFxGlobalId = 0xB1B6u;  // 合同点名的那个 id

bool put32(lift::GuestMemory& mem, std::uint32_t ea, std::uint32_t value) {
    if (!mem.committed(ea, 4u)) return false;
    mem.wr32(ea, value);
    return true;
}

bool makeFakeEngine(lift::GuestMemory& mem) {
    if (!mem.reserve()) return false;
    if (!mem.commitZeroed(kFxBase, kFxBaseSize, lift::RegionKind::Heap, "plugin-fake-globals")) return false;
    if (!put32(mem, lift::kEngineSingletonGlobal, kFxEngine)) return false;
    if (!put32(mem, kFxEngine + lift::kScriptGlobalIntBaseOffset, kFxIntPool)) return false;
    if (!put32(mem, kFxEngine + lift::kScriptGlobalIntCountOffset, kFxIntCount)) return false;
    if (!put32(mem, kFxEngine + lift::kScriptGlobalFloatBaseOffset, kFxFloatPool)) return false;
    if (!put32(mem, kFxEngine + lift::kScriptGlobalFloatCountOffset, kFxFloatCount)) return false;
    return put32(mem, kFxEngine + lift::kScriptGlobalIntKeyOffset, kFxKey);
}

std::uint32_t intAddr(std::uint32_t index) {
    return kFxIntPool + lift::kScriptGlobalSlotBytes * index;
}

std::uint32_t rawSlot(const lift::GuestMemory& mem) { return mem.rd32(intAddr(kFxGlobalId)); }

// ---- 注入的读数源（C1 的测试缝；`TriggerReadings` 是纯值）----
struct FakeReadings final : lift::plugin::ReadingsSource {
    std::string script = "SCENE1";
    std::uint32_t pos = 0x1104;
    bool pos_ok = true;
    bool mouse_ok = false;
    int mouse_x = 0;
    int mouse_y = 0;
    std::uint64_t samples = 0;

    lift::plugin::TriggerReadings sample(lift::InputPoint point) override {
        ++samples;
        lift::plugin::TriggerReadings readings;
        readings.point = point;
        readings.script = script;
        readings.script_ok = !script.empty();
        readings.pos = pos;
        readings.pos_ok = pos_ok;
        readings.mouse_ok = mouse_ok;
        readings.mouse_x = mouse_x;
        readings.mouse_y = mouse_y;
        return readings;
    }
};

std::string fixturePath(const std::string& name) {
    return (fs::path(LIFT_PLUGIN_FIXTURES_DIR) / name).string();
}

bool readFixture(const std::string& name, std::string* out) {
    std::ifstream in(fixturePath(name), std::ios::binary);
    if (!in) return false;
    out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// 直接经 `makeQuickJsPlugin` 装载一个夹具（C2b 的 render 用例：**不需要** Run / PluginHost）。
// ★ 为什么不用 `Fixture`：`plugin/ui-draw` / `plugin/render-readings` / `plugin/render-surface`
//   判的是**渲染面**，与读点 / guest 写无关 ⇒ 直接拿 `PluginScript`（它同时是 `UiScript`）更贴近
//   生产上"窗口叠层每帧调 `UiScript::render`"的形状。`font` 为 nullptr ⇒ `measure` 抛 TypeError。
std::unique_ptr<lift::ui::PluginScript> loadPlugin(const std::string& file,
                                                   const std::map<std::string, std::string>& params,
                                                   lift::FontEngine* font, std::string* err) {
    std::string source;
    if (!readFixture(file, &source)) {
        if (err != nullptr) *err = "读不到夹具：" + fixturePath(file);
        return nullptr;
    }
    lift::ui::PluginLoadOptions opts;
    opts.origin = fixturePath(file);
    opts.params = &params;
    opts.font = font;
    return lift::ui::makeQuickJsPlugin(source, opts, err);
}

// 一个夹具：假 guest 内存 + 默认窗口管理器 + 注入读数源 + 一个 PluginHost。
// ★ 装配顺序 = 生产的顺序（`attachMemory` 在 `init` 之后；读点观察者由 `onReadPoint` 直接驱动）。
struct Fixture {
    lift::GuestMemory mem;
    bool mem_ok = false;
    lift::WindowManager wm;
    std::uint32_t hwnd = 0;
    bool win_ok = false;
    FakeReadings readings;
    std::unique_ptr<lift::plugin::PluginHost> host;
    std::string load_error;

    Fixture() {
        mem_ok = makeFakeEngine(mem);
        hwnd = wm.createWindow("PluginTest", 0u, 0u, 0u, 0u, 1280, 720, 0u);
        win_ok = hwnd != 0;
    }

    bool load(const std::string& file, const std::map<std::string, std::string>& params = {},
              const lift::ui::UiHostOps* host_ops = nullptr) {
        std::string source;
        if (!readFixture(file, &source)) {
            load_error = "读不到夹具：" + fixturePath(file);
            return false;
        }
        host = std::make_unique<lift::plugin::PluginHost>(readings);
        // ★ 预算（D5 的 2 ms / 100k 是**运行时口径**，不是本用例的判据）：这里放宽到
        //   500 ms / 1e7 ops —— 只为让"QJS 冷启动（第一次 JS_Call + 若干个对象分配）"不把
        //   用例变成随机红。⛔ 生产值仍只有一份（`runtime/budget.h`；`PluginHost` 读的就是它）。
        host->runner().setBudget(lift::plugin::Budget{500'000'000ull, 10'000'000u});
        // ★ 装配顺序与 `aot_main` **一致**：`init`（编译 + 沙箱）→ `attachMemory`（Edit/Read）。
        //   `host_ops` 只在需要判"profile 桥有没有被调到"的用例里给（`plugin/render-stored-runctx-rejected`）。
        if (!host->init(source, fixturePath(file), params, host_ops, &load_error)) return false;
        host->attachMemory(mem);
        return true;
    }

    // 一个读点（本文件里"读点"的唯一含义）。
    void pump(lift::InputPoint point = lift::InputPoint::PeekMsg) { host->onReadPoint(wm, point); }

    void cursor(int* x, int* y) const { wm.cursorClientPos(hwnd, x, y); }
};

// 失败时把人可读的原因打出来（否则 readout 只有名字）。
void dumpFail(const Fixture& f) {
    if (f.host == nullptr || !f.host->failed()) return;
    std::fprintf(stderr, "  plugin fail: name=%s reason=%s\n", f.host->failName().c_str(),
                 f.host->failReason().c_str());
}

// =====================================================================================
// `plugin/api-surface`
// =====================================================================================
void caseApiSurface() {
    lift_test::g_current_case = "plugin/api-surface";
    Fixture f;
    LIFT_CHECK(f.mem_ok);
    LIFT_CHECK(f.win_ok);
    if (!f.mem_ok || !f.win_ok) return;
    const bool loaded = f.load("api.js", {{"probe", "api-surface"}});
    LIFT_CHECK(loaded);
    if (!loaded) {
        std::fprintf(stderr, "  load error: %s\n", f.load_error.c_str());
        return;
    }
    f.pump();
    dumpFail(f);
    // ★ 形状断言全在 `api.js` 里（宿主读不到 JS 对象）；`ctx.fail` 会把缺/多的键带进 reason。
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK(f.host->script() != nullptr && f.host->script()->hasRun());
    LIFT_CHECK(f.host->script() != nullptr && !f.host->script()->hasRender());
    // Edit **立即**：`api.js` 写了 int 0 ⇒ 槽里是 ENC(0)（⛔ 不是"排进队列等下一拍"）。
    LIFT_CHECK_EQ_U32(rawSlot(f.mem), lift::encodeInt(0u, kFxKey));
    // Input 只入队（一读点一条的预算之外，Edit 不进队列）。
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->queue().depth()), 1);
    LIFT_CHECK(f.host->runner().done());  // start 返回 [] ⇒ done
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->callbacks()), 1);
}

// =====================================================================================
// `plugin/initial-state`（`initialState` 注入 + per-Run 状态）
// =====================================================================================
void caseInitialState() {
    lift_test::g_current_case = "plugin/initial-state";
    Fixture f;
    LIFT_CHECK(f.mem_ok && f.win_ok);
    if (!f.mem_ok || !f.win_ok) return;
    const bool loaded = f.load("state.js");
    LIFT_CHECK(loaded);
    if (!loaded) {
        std::fprintf(stderr, "  load error: %s\n", f.load_error.c_str());
        return;
    }
    f.pump();  // start：检查 n===1，写 n=2，arm atReadPoint
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->fired()), 0);
    f.pump();  // 第二个回调：必须看到 n===2（per-Run）
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->fired()), 1);
    LIFT_CHECK(f.host->runner().done());

    // ---- 没有 `initialState` ⇒ `{}`（且可写）----
    lift_test::g_current_case = "plugin/initial-state-bare";
    Fixture b;
    const bool ok = b.load("bare.js");
    LIFT_CHECK(ok);
    if (ok) {
        b.pump();
        dumpFail(b);
        LIFT_CHECK(!b.host->failed());
        LIFT_CHECK(b.host->runner().done());
    }

    // ---- 同一文件装载**两次**（两个 runtime）⇒ 状态不共享（第二个 Run 的 n 仍是 1）----
    //   ★ C2 只有一个 Run/实例 ⇒ 这里判的是"装载边界"，⛔ 不是"同 runtime 两个 Run"。
    lift_test::g_current_case = "plugin/initial-state-two-loads";
    Fixture again;
    const bool ok2 = again.load("state.js");
    LIFT_CHECK(ok2);
    if (ok2) {
        again.pump();  // start：n 必须是 1（上一次 Run 写的 2 不许漏过来）
        dumpFail(again);
        LIFT_CHECK(!again.host->failed());
    }
}

// =====================================================================================
// `plugin/concurrent-arm`：一读点最多一个 / 返回数组完整替换 / [] = done
// =====================================================================================
void caseConcurrentArm() {
    lift_test::g_current_case = "plugin/concurrent-arm";
    Fixture f;
    LIFT_CHECK(f.mem_ok && f.win_ok);
    if (!f.mem_ok || !f.win_ok) return;
    const bool loaded = f.load("concurrent.js");
    LIFT_CHECK(loaded);
    if (!loaded) {
        std::fprintf(stderr, "  load error: %s\n", f.load_error.c_str());
        return;
    }
    // 第 1 个读点：start arm 三个门（`atPoint` 的 poll 在 start 之前 ⇒ 本拍不 fire）。
    f.readings.pos = 0x15;  // 在 A 区间 [0x10,0x20)
    f.pump();
    dumpFail(f);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->fired()), 0);
    // 第 2 个读点：A 命中（注册顺序第一个）。
    f.pump();
    dumpFail(f);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->fired()), 1);
    // 返回数组只放 B/C ⇒ **仍在 A 区间也不触发**（完整替换；⛔ 不自动保留）。
    f.pump();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->fired()), 1);
    // B：一次。
    f.readings.pos = 0x35;
    f.pump();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->fired()), 2);
    // C：一次 ⇒ 回调返回 [] = done。
    f.readings.pos = 0x55;
    f.pump();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->fired()), 3);
    LIFT_CHECK(f.host->runner().done());
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());

    // ---- 重复 name ⇒ Run fail 并点名（判据在 `TriggerRunner::install`）----
    lift_test::g_current_case = "plugin/concurrent-arm-duplicate-name";
    Fixture d;
    const bool ok = d.load("dup.js");
    LIFT_CHECK(ok);
    if (ok) {
        d.pump();
        LIFT_CHECK(d.host->runner().failed());
        LIFT_CHECK(d.host->runner().failName() == "dup");
        LIFT_CHECK(d.host->failed());  // handler 侧也记了（readout 的 run=failed 靠它）
        LIFT_CHECK(d.host->failReason().find("dup") != std::string::npos);
    }
}

// =====================================================================================
// `control-script/start-on-trigger-chain`：具名链 + 两个 slot 的 move 落**不同** y
// =====================================================================================
int chainRowY(int slot) {
    lift_test::g_current_case = "control-script/start-on-trigger-chain";
    Fixture f;
    LIFT_CHECK(f.mem_ok && f.win_ok);
    if (!f.mem_ok || !f.win_ok) return -1;
    const bool loaded = f.load("chain.js", {{"slot", std::to_string(slot)}});
    LIFT_CHECK(loaded);
    if (!loaded) {
        std::fprintf(stderr, "  load error: %s\n", f.load_error.c_str());
        return -1;
    }
    // ① 第 1 个读点（TITLE）：start 只 arm SAVE + 提交 click(1070,480)（staged）。
    f.readings.script = "TITLE";
    f.readings.pos = 0;
    f.pump();
    dumpFail(f);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->fired()), 0);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->queue().stagedDepth()), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->queue().readyDepth()), 0);
    // ② SAVE 区间：poll 之前 ② 先提升 click；poll 命中 load:save（提交 move(300,rowY)）；
    //    ⑥ 落地 click ⇒ 光标到 (1070,480)。
    f.readings.script = "SAVE";
    f.readings.pos = 0x1150;
    f.pump();
    dumpFail(f);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->fired()), 1);
    int cx = 0;
    int cy = 0;
    f.cursor(&cx, &cy);
    LIFT_CHECK_EQ_I64(cx, 1070);
    LIFT_CHECK_EQ_I64(cy, 480);
    // ③ 下一读点：提升 move(300,rowY) 并落地；atReadPoint 命中 load:row（提交 click(300,rowY)）。
    f.pump();
    dumpFail(f);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->fired()), 2);
    f.cursor(&cx, &cy);
    LIFT_CHECK_EQ_I64(cx, 300);
    const int row_y = cy;
    // ④ SBUNKI 区间：命中第一道确认 ⇒ 提交 click(640,323) + 返回 [] = done。
    f.readings.script = "SBUNKI";
    f.readings.pos = 0x0a00;
    f.pump();
    dumpFail(f);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->fired()), 3);
    LIFT_CHECK(f.host->runner().done());
    LIFT_CHECK(!f.host->failed());
    return row_y;
}

void caseChain() {
    const int y4 = chainRowY(4);
    const int y5 = chainRowY(5);
    // ★ 实测表：slot 4 → 328、slot 5 → 388（⛔ 不是 91 + 59.5*n 之类的公式；见 D-C2-8）。
    LIFT_CHECK_EQ_I64(y4, 328);
    LIFT_CHECK_EQ_I64(y5, 388);
    LIFT_CHECK(y4 != y5);  // 两个 slot 的链必须发不同坐标（"只是没报错"不够）

    // ---- 未知 slot ⇒ 插件自己 `throw`（装载成功、`start` 里抛 ⇒ Run fail 并带消息）----
    lift_test::g_current_case = "control-script/start-on-trigger-chain-bad-slot";
    Fixture f;
    const bool loaded = f.load("chain.js", {{"slot", "9"}});
    LIFT_CHECK(loaded);  // 装载不看参数；抛在 start
    if (loaded) {
        f.pump();
        LIFT_CHECK(f.host->failed());
        LIFT_CHECK(f.host->failReason().find("slot 9") != std::string::npos);
    }
}

// =====================================================================================
// `control-script/global-read-modify-write`：read → edit（立即）→ input（另计）
// =====================================================================================
void caseGlobalReadModifyWrite() {
    lift_test::g_current_case = "control-script/global-read-modify-write";
    Fixture f;
    LIFT_CHECK(f.mem_ok && f.win_ok);
    if (!f.mem_ok || !f.win_ok) return;
    // 起点 0x1200（走 `writeGlobal` ⇒ 槽里是 ENC，不是明文）。
    lift::GlobalValue seed;
    seed.kind = lift::GlobalKind::Int;
    seed.u32 = 0x1200u;
    lift::GlobalError seed_err = lift::GlobalError::None;
    LIFT_CHECK(lift::writeGlobal(f.mem, kFxGlobalId, lift::GlobalKind::Int, seed, &seed_err));
    LIFT_CHECK(seed_err == lift::GlobalError::None);
    LIFT_CHECK_EQ_U32(rawSlot(f.mem), lift::encodeInt(0x1200u, kFxKey));

    const bool loaded = f.load("readwrite.js");
    LIFT_CHECK(loaded);
    if (!loaded) {
        std::fprintf(stderr, "  load error: %s\n", f.load_error.c_str());
        return;
    }
    f.readings.script = "SCENE1";
    f.pump();  // start：arm afterScript
    dumpFail(f);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->fired()), 0);
    f.pump();  // 命中：read 0x1200 → edit |0x10 → 同拍读回 0x1210 → click
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->fired()), 1);
    // ★ Edit **立即**：槽里已经是 ENC(0x1210) —— ⛔ 不等任何读点。
    LIFT_CHECK_EQ_U32(rawSlot(f.mem), lift::encodeInt(0x1210u, kFxKey));
    // ★ Edit **不占** Input 预算：队列里只有那一条 click（回调结束在 staged）。
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->queue().depth()), 1);
    LIFT_CHECK(f.host->runner().done());
}

// =====================================================================================
// `plugin/finish-quit`（finish 半）：只结束 Run + 记 exit
// =====================================================================================
void caseFinish() {
    lift_test::g_current_case = "plugin/finish-quit";
    Fixture f;
    LIFT_CHECK(f.mem_ok && f.win_ok);
    if (!f.mem_ok || !f.win_ok) return;
    const bool loaded = f.load("finish.js");
    LIFT_CHECK(loaded);
    if (!loaded) {
        std::fprintf(stderr, "  load error: %s\n", f.load_error.c_str());
        return;
    }
    f.pump();
    dumpFail(f);
    LIFT_CHECK(f.host->finished());
    LIFT_CHECK_EQ_I64(f.host->finishCode(), 5);
    LIFT_CHECK(f.host->finishReason() == "old-exit5");
    LIFT_CHECK(!f.host->failed());          // finish ≠ fail
    LIFT_CHECK(!f.host->quitRequested());   // finish ≠ quit
    LIFT_CHECK(f.host->runner().done());    // Run 收口
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->callbacks()), 1);
    const std::string line = f.host->readout();
    LIFT_CHECK(line.find("run=finished") != std::string::npos);
    LIFT_CHECK(line.find("exit=5") != std::string::npos);
    std::printf("  [%s] %s\n", lift_test::g_current_case, line.c_str());
}

// =====================================================================================
// F2：`onTrigger` 返回的集合让 runner `install` 失败 ⇒ 必须在 readout 上可见（`run=failed`）
// =====================================================================================
void caseFailInstall() {
    lift_test::g_current_case = "plugin/fail-install";
    Fixture f;
    LIFT_CHECK(f.mem_ok && f.win_ok);
    if (!f.mem_ok || !f.win_ok) return;
    const bool loaded = f.load("fail-install.js");
    LIFT_CHECK(loaded);
    if (!loaded) {
        std::fprintf(stderr, "  load error: %s\n", f.load_error.c_str());
        return;
    }
    f.pump();  // 第 1 个读点：start arm `a`（Once）
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->fired()), 0);
    f.pump();  // 第 2 个读点：`a` fire ⇒ 回调把同一个 Once 对象放回 ⇒ install 判 exhausted
    dumpFail(f);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->fired()), 1);
    // ★ 判据的两侧：runner 自己 failed **且** handler 侧同步到了（否则 readout 会说 waiting）。
    LIFT_CHECK(f.host->runner().failed());
    LIFT_CHECK(f.host->failed());
    LIFT_CHECK(f.host->failReason().find("exhausted") != std::string::npos);
    const std::string line = f.host->readout();
    LIFT_CHECK(line.find("run=failed") != std::string::npos);
    LIFT_CHECK(line.find("name=a") != std::string::npos);
    LIFT_CHECK(line.find("exhausted") != std::string::npos);
    // ★ 读数的**唯一消息面**：green 时也打一行（ctest 只在失败时回放，平时不吵）——
    //   它是"Run 到底怎么收的口"的直接证据，比只看断言好读。
    std::printf("  [%s] %s\n", lift_test::g_current_case, line.c_str());
}

// =====================================================================================
// F3-R1：**runner 侧**的失败（`install` 拒了回调返回的集合）也要丢掉同拍 staged input
//   ★ 与 `plugin/fail-releases-input` 的区别：这条**不调** `ctx.fail` —— 失败在 JS 返回**之后**
//     （`invokeOnTrigger` 的 `install`）⇒ 归因只能由 `syncRunnerFailure` 兜。
// =====================================================================================
void caseFailInstallReleasesInput() {
    lift_test::g_current_case = "plugin/fail-install-releases-input";
    Fixture f;
    LIFT_CHECK(f.mem_ok && f.win_ok);
    if (!f.mem_ok || !f.win_ok) return;
    const bool loaded = f.load("fail-install-click.js");
    LIFT_CHECK(loaded);
    if (!loaded) {
        std::fprintf(stderr, "  load error: %s\n", f.load_error.c_str());
        return;
    }
    int cx = 0;
    int cy = 0;
    f.pump();  // 第 1 个读点：start arm `a`（Once）
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    f.pump();  // 第 2 个读点：callback 先 click(700,500) 再返回已耗尽的 Once ⇒ install fail
    dumpFail(f);
    LIFT_CHECK(f.host->runner().failed());
    LIFT_CHECK(f.host->failed());
    // ★ 判据：同拍 staged 的 click 被丢掉（队列清零 + 光标没动）——失败之后也不许落地。
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->queue().depth()), 0);
    f.cursor(&cx, &cy);
    LIFT_CHECK(!(cx == 700 && cy == 500));
    f.pump();
    f.cursor(&cx, &cy);
    LIFT_CHECK(!(cx == 700 && cy == 500));
    const std::string line = f.host->readout();
    LIFT_CHECK(line.find("run=failed") != std::string::npos);
    LIFT_CHECK(line.find("exhausted") != std::string::npos);
    std::printf("  [%s] %s\n", lift_test::g_current_case, line.c_str());
}

// =====================================================================================
// F3（父级裁决）：`fail` / `quit` ⇒ 立刻丢 staged input；`finish` ⇒ 不丢（下一个读点落地）
// =====================================================================================
void caseFailReleasesInput() {
    lift_test::g_current_case = "plugin/fail-releases-input";
    Fixture f;
    LIFT_CHECK(f.mem_ok && f.win_ok);
    if (!f.mem_ok || !f.win_ok) return;
    const bool loaded = f.load("fail-release.js");
    LIFT_CHECK(loaded);
    if (!loaded) {
        std::fprintf(stderr, "  load error: %s\n", f.load_error.c_str());
        return;
    }
    int cx = 0;
    int cy = 0;
    f.pump();  // 第 1 个读点：start arm `boom`
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    f.pump();  // 第 2 个读点：回调提交 click(200,100) **然后** fail ⇒ 必须被丢掉
    dumpFail(f);
    LIFT_CHECK(f.host->failed());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->queue().depth()), 0);  // staged 被丢掉
    f.cursor(&cx, &cy);
    LIFT_CHECK(!(cx == 200 && cy == 100));
    // 再泵一拍：失败之后**不允许**再落地（F3 的字面判据）。
    f.pump();
    f.cursor(&cx, &cy);
    LIFT_CHECK(!(cx == 200 && cy == 100));
}

void caseQuitReleasesInput() {
    lift_test::g_current_case = "plugin/quit-releases-input";
    Fixture f;
    LIFT_CHECK(f.mem_ok && f.win_ok);
    if (!f.mem_ok || !f.win_ok) return;
    const bool loaded = f.load("quit-release.js");
    LIFT_CHECK(loaded);
    if (!loaded) {
        std::fprintf(stderr, "  load error: %s\n", f.load_error.c_str());
        return;
    }
    int cx = 0;
    int cy = 0;
    f.pump();  // start：arm `bye`
    dumpFail(f);
    LIFT_CHECK(!f.host->quitRequested());
    f.pump();  // 回调：click(300,400) 然后 quit ⇒ staged 必须被丢掉
    dumpFail(f);
    LIFT_CHECK(f.host->quitRequested());  // 第二条停机源
    LIFT_CHECK(!f.host->failed());         // quit ≠ fail
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->queue().depth()), 0);
    f.cursor(&cx, &cy);
    LIFT_CHECK(!(cx == 300 && cy == 400));
    f.pump();
    f.cursor(&cx, &cy);
    LIFT_CHECK(!(cx == 300 && cy == 400));
    const std::string line = f.host->readout();
    LIFT_CHECK(line.find("run=quit") != std::string::npos);
    std::printf("  [%s] %s\n", lift_test::g_current_case, line.c_str());
}

void caseFinishLandsInput() {
    lift_test::g_current_case = "plugin/finish-lands-input";
    Fixture f;
    LIFT_CHECK(f.mem_ok && f.win_ok);
    if (!f.mem_ok || !f.win_ok) return;
    const bool loaded = f.load("finish-click.js");
    LIFT_CHECK(loaded);
    if (!loaded) {
        std::fprintf(stderr, "  load error: %s\n", f.load_error.c_str());
        return;
    }
    int cx = 0;
    int cy = 0;
    f.pump();  // start：arm `fin`
    dumpFail(f);
    f.pump();  // 回调：click(640,323) + finish(0) ⇒ **不**释放 ⇒ staged 留着（下一读点才落地）
    dumpFail(f);
    LIFT_CHECK(f.host->finished());
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->queue().depth()), 1);
    f.cursor(&cx, &cy);
    LIFT_CHECK(!(cx == 640 && cy == 323));  // 本读点还没落地（callback 的 input 一律下一读点）
    f.pump();                               // 下一读点：第 ② 步提升 ⇒ 第 ⑥ 步落地
    f.cursor(&cx, &cy);
    LIFT_CHECK_EQ_I64(cx, 640);
    LIFT_CHECK_EQ_I64(cy, 323);
    const std::string line = f.host->readout();
    LIFT_CHECK(line.find("run=finished") != std::string::npos);
    std::printf("  [%s] %s\n", lift_test::g_current_case, line.c_str());
}

// =====================================================================================
// F5-R2：`atReadPoint("fileRead", …)` 能解析 + 只在**下一个 `ReadFile` 投递点**上 fire
// =====================================================================================
void caseAtReadPointFileRead() {
    lift_test::g_current_case = "plugin/at-read-point-file-read";
    Fixture f;
    LIFT_CHECK(f.mem_ok && f.win_ok);
    if (!f.mem_ok || !f.win_ok) return;
    const bool loaded = f.load("point-file-read.js");
    LIFT_CHECK(loaded);
    if (!loaded) {
        std::fprintf(stderr, "  load error: %s\n", f.load_error.c_str());
        return;
    }
    f.pump(lift::InputPoint::PeekMsg);  // start：arm `atReadPoint("fileRead", 1)`
    dumpFail(f);
    // ★ 解析失败（旧实现：运行期词表没有 `fileRead` ⇒ TypeError）会走 `start` 抛错 ⇒ 这里先红。
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->runner().armedCount()), 1);
    LIFT_CHECK(f.host->runner().armed()[0].point_filter == lift::InputPoint::FileRead);
    // 别的读点不 fire（点过滤生效）。
    f.pump(lift::InputPoint::PeekMsg);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->fired()), 0);
    // 下一个 `ReadFile` 投递点 ⇒ fire（回调里断言 `ev.point` / `ctx.now.point` == "fileRead"）。
    f.pump(lift::InputPoint::FileRead);
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->fired()), 1);
    LIFT_CHECK(f.host->runner().done());
    std::printf("  [%s] %s\n", lift_test::g_current_case, f.host->readout().c_str());
}

// =====================================================================================
// D-C2-10：`start` 里 `ctx.now` 有值；`start` 里的 Input **下一读点**才落地
// =====================================================================================
void caseStartNowAndStaged() {
    lift_test::g_current_case = "plugin/start-now-and-staged-input";
    Fixture f;
    LIFT_CHECK(f.mem_ok && f.win_ok);
    if (!f.mem_ok || !f.win_ok) return;
    f.readings.script = "SCENE1";
    f.readings.pos = 0x1234;  // JS 侧断言这个数（params 传十进制 4660）
    const bool loaded = f.load("now.js", {{"expect_script", "SCENE1"}, {"expect_pos", "4660"}});
    LIFT_CHECK(loaded);
    if (!loaded) {
        std::fprintf(stderr, "  load error: %s\n", f.load_error.c_str());
        return;
    }
    int cx = 0;
    int cy = 0;
    f.pump();  // 第 1 个读点：sampleNow（⇒ start 里 ctx.now 有值）+ start（提交 click(11,22)）
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    // ★ Input 落 `staged_`：本读点**看不见**（ready 为 0），光标也**没动**。
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->queue().stagedDepth()), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->queue().readyDepth()), 0);
    f.cursor(&cx, &cy);
    LIFT_CHECK(!(cx == 11 && cy == 22));
    // 第 2 个读点：② 提升 → ③ poll（atReadPoint 命中）→ ⑥ 落地 click。
    f.pump();
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->fired()), 1);
    f.cursor(&cx, &cy);
    LIFT_CHECK_EQ_I64(cx, 11);
    LIFT_CHECK_EQ_I64(cy, 22);
    LIFT_CHECK(f.host->runner().done());

    // ---- `ctx.now` 在 `start` 里**没有**值 ⇒ 插件自己 fail（反证：sampleNow 是必需的）----
    lift_test::g_current_case = "plugin/start-now-requires-sample";
    Fixture n;
    LIFT_CHECK(n.load("now.js", {{"expect_script", "OTHER"}, {"expect_pos", "1"}}));
    n.pump();
    LIFT_CHECK(n.host->failed());  // script 不匹配 ⇒ JS 侧 ctx.fail（证明它**真的读到了** now）
    LIFT_CHECK(n.host->failReason().find("expect_script") != std::string::npos ||
               n.host->failReason().find("OTHER") != std::string::npos);
}

// =====================================================================================
// 模块契约（`export default` / 成对 / 只有 render）
// =====================================================================================
void caseModuleContract() {
    lift_test::g_current_case = "plugin/module-default";
    {
        Fixture f;
        LIFT_CHECK(!f.load("bad-default.js"));
        LIFT_CHECK(f.load_error.find("export default") != std::string::npos);
    }
    lift_test::g_current_case = "plugin/module-start-onTrigger-pair";
    {
        Fixture f;
        LIFT_CHECK(!f.load("bad-pair.js"));
        LIFT_CHECK(f.load_error.find("成对") != std::string::npos);
    }
    lift_test::g_current_case = "plugin/module-render-only";
    {
        Fixture f;
        const bool loaded = f.load("render-only.js");
        LIFT_CHECK(loaded);
        if (loaded) {
            LIFT_CHECK(f.host->script() != nullptr && f.host->script()->hasRender());
            LIFT_CHECK(f.host->script() != nullptr && !f.host->script()->hasRun());
            f.pump();  // 没有 Run ⇒ 立刻 done，且**不调**任何回调
            LIFT_CHECK(f.host->runner().done());
            LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->callbacks()), 0);
            LIFT_CHECK(!f.host->failed());
        }
    }
    lift_test::g_current_case = "plugin/module-missing-file";
    {
        Fixture f;
        LIFT_CHECK(!f.load("does-not-exist.js"));
    }
}

// =====================================================================================
// C2b · `plugin/render-surface`：`RenderCtx` 的成员表（= d.ts 的 RenderCtx 减 `run`）
//   ★ 形状断言全在 `render-surface.js` 里（宿主读不到 JS 对象）⇒ 判据 = render 返回 true。
//   ★ 附带钉住"没有 render 的插件调 render ⇒ true + 空 list"（present_sdl 只在挂 overlay 时调它）。
// =====================================================================================
void caseRenderSurface() {
    lift_test::g_current_case = "plugin/render-surface";
    Fixture f;
    LIFT_CHECK(f.mem_ok && f.win_ok);
    if (!f.mem_ok || !f.win_ok) return;
    const bool loaded = f.load("render-surface.js", {{"probe", "render-surface"}});
    LIFT_CHECK(loaded);
    if (!loaded) {
        std::fprintf(stderr, "  load error: %s\n", f.load_error.c_str());
        return;
    }
    LIFT_CHECK(f.host->script() != nullptr);
    if (f.host->script() == nullptr) return;
    LIFT_CHECK(f.host->script()->hasRender());
    LIFT_CHECK(!f.host->script()->hasRun());
    lift::ui::UiBuilder builder;
    lift::ui::UiFrameReadings readings;
    std::string err;
    const bool ok = f.host->script()->render(builder, readings, &err);
    if (!ok) std::fprintf(stderr, "  render error: %s\n", err.c_str());
    LIFT_CHECK(ok);
    LIFT_CHECK(builder.elements().empty());  // 形状探针不画东西

    // ---- 没有 `render` 的插件（纯 Run / headless）：`UiScript::render` ⇒ true + 空 list ----
    lift_test::g_current_case = "plugin/render-surface-no-render";
    Fixture n;
    const bool loaded2 = n.load("api.js", {{"probe", "api-surface"}});
    LIFT_CHECK(loaded2);
    if (loaded2 && n.host->script() != nullptr) {
        LIFT_CHECK(!n.host->script()->hasRender());
        lift::ui::UiBuilder b2;
        lift::ui::UiFrameReadings r2;
        std::string e2;
        LIFT_CHECK(n.host->script()->render(b2, r2, &e2));
        LIFT_CHECK(b2.elements().empty());
        LIFT_CHECK(e2.empty());
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(n.host->callbacks()), 0);  // render ≠ Run 回调
    }
}

// =====================================================================================
// C2b · `plugin/ui-draw`：`ctx.draw.{rect,text,measure}` 的元素字段 / z 序 / measure 形状
//   + 两条反证：不支持的 style 字段 ⇒ **TypeError**（D-C2-3）；缺 FontEngine 的 measure ⇒ TypeError。
// =====================================================================================
void caseUiDraw(lift::FontEngine* font) {
    lift_test::g_current_case = "plugin/ui-draw";
    LIFT_CHECK(font != nullptr);
    if (font == nullptr) return;
    std::string err;
    std::unique_ptr<lift::ui::PluginScript> script = loadPlugin("draw.js", {}, font, &err);
    LIFT_CHECK(script != nullptr);
    if (script == nullptr) {
        std::fprintf(stderr, "  load error: %s\n", err.c_str());
        return;
    }
    LIFT_CHECK(script->hasRender());
    LIFT_CHECK(!script->hasRun());
    lift::ui::UiFrameReadings readings;
    lift::ui::UiBuilder builder;
    const bool ok = script->render(builder, readings, &err);
    if (!ok) std::fprintf(stderr, "  render error: %s\n", err.c_str());
    LIFT_CHECK(ok);
    const std::vector<lift::ui::Element>& elements = builder.elements();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(elements.size()), 3);
    if (elements.size() == 3) {
        // ① 第一条 Rect：位置 / 尺寸 / 填充（`#102030CC` = 16,32,48,204）
        LIFT_CHECK(elements[0].kind == lift::ui::ElementKind::Rect);
        LIFT_CHECK_EQ_I64(elements[0].x, 10);
        LIFT_CHECK_EQ_I64(elements[0].y, 20);
        LIFT_CHECK_EQ_I64(elements[0].w, 100);
        LIFT_CHECK_EQ_I64(elements[0].h, 40);
        LIFT_CHECK_EQ_U32(elements[0].fill.r, 16u);
        LIFT_CHECK_EQ_U32(elements[0].fill.g, 32u);
        LIFT_CHECK_EQ_U32(elements[0].fill.b, 48u);
        LIFT_CHECK_EQ_U32(elements[0].fill.a, 204u);
        // ② 第二条 Text：文案 / 字号 / 颜色（`#EBFFEB`）
        LIFT_CHECK(elements[1].kind == lift::ui::ElementKind::Text);
        LIFT_CHECK(elements[1].text == "hello");
        LIFT_CHECK_EQ_I64(elements[1].font_size, 20);
        LIFT_CHECK_EQ_I64(elements[1].x, 12);
        LIFT_CHECK_EQ_I64(elements[1].y, 24);
        LIFT_CHECK_EQ_U32(elements[1].color.r, 235u);
        LIFT_CHECK_EQ_U32(elements[1].color.g, 255u);
        LIFT_CHECK_EQ_U32(elements[1].color.b, 235u);
        LIFT_CHECK_EQ_U32(elements[1].color.a, 255u);
        // ③ 第三条 Rect：w/h = `measure("hello",{size:18})` 的量。★ 用**宿主自己的** measureText
        //    交叉核对（⛔ 不是把 JS 算出来的数抄进断言）。
        LIFT_CHECK(elements[2].kind == lift::ui::ElementKind::Rect);
        int want_w = 0;
        int want_h = 0;
        std::string merr;
        LIFT_CHECK(lift::ui::measureText("hello", 18, false, *font, &want_w, &want_h, &merr));
        LIFT_CHECK_EQ_I64(elements[2].w, want_w);
        LIFT_CHECK_EQ_I64(elements[2].h, want_h);
        // ④ z 序 = 调用顺序（z 恒 0、绘制序按 seq）
        for (std::size_t i = 0; i < elements.size(); ++i) {
            LIFT_CHECK_EQ_I64(elements[i].z, 0);
            LIFT_CHECK_EQ_I64(elements[i].seq, static_cast<std::int64_t>(i));
        }
    }

    // ---- 反证 1：不支持的 style 字段 ⇒ TypeError（点名 + ⛔ 不静默忽略；D-C2-3）----
    lift_test::g_current_case = "plugin/ui-draw-bad-style";
    struct BadStyle {
        const char* bad;
        const char* field;
    };
    const BadStyle bad_cases[] = {
        {"rect.stroke", "stroke"},   {"rect.width", "width"},   {"rect.alpha", "alpha"},
        {"text.align", "align"},     {"text.stroke", "stroke"}, {"text.width", "width"},
        {"text.alpha", "alpha"},     {"measure.align", "align"},
    };
    for (const BadStyle& bc : bad_cases) {
        std::string load_err;
        std::unique_ptr<lift::ui::PluginScript> bad =
            loadPlugin("draw-bad-style.js", {{"bad", bc.bad}}, font, &load_err);
        LIFT_CHECK(bad != nullptr);
        if (bad == nullptr) {
            std::fprintf(stderr, "  load error (%s): %s\n", bc.bad, load_err.c_str());
            continue;
        }
        lift::ui::UiBuilder b;
        lift::ui::UiFrameReadings r;
        std::string rerr;
        const bool rendered = bad->render(b, r, &rerr);
        ++lift_test::g_checks;
        if (rendered) {
            lift_test::failf(__FILE__, __LINE__, "style.%s 没有抛 TypeError（⛔ 静默忽略）", bc.field);
            continue;
        }
        const std::string needle = std::string("ctx.draw: style.") + bc.field + " is not supported";
        ++lift_test::g_checks;
        if (rerr.find(needle) == std::string::npos) {
            lift_test::failf(__FILE__, __LINE__, "style.%s 的 TypeError 文本不对：%s", bc.field,
                             rerr.c_str());
        }
    }

    // ---- 反证 2：没有 FontEngine ⇒ `measure` 抛 TypeError（⛔ 不返回 0 尺寸冒充可用）----
    lift_test::g_current_case = "plugin/ui-draw-measure-no-font";
    {
        std::string load_err;
        std::unique_ptr<lift::ui::PluginScript> nofont = loadPlugin("draw.js", {}, nullptr, &load_err);
        LIFT_CHECK(nofont != nullptr);
        if (nofont != nullptr) {
            lift::ui::UiBuilder b;
            lift::ui::UiFrameReadings r;
            std::string rerr;
            LIFT_CHECK(!nofont->render(b, r, &rerr));
            LIFT_CHECK(rerr.find("ctx.draw.measure") != std::string::npos);
            LIFT_CHECK(rerr.find("font == nullptr") != std::string::npos);
        }
    }
}

// =====================================================================================
// C2b · `plugin/ui-state`：onTrigger 只改 `ctx.ui`；**下一次 render** 才画；hide 后下一次为空
//   ★ 回调里 `ctx.draw` 必须**不存在**（RunCtx 没有 draw）—— `hud-ui.js` 自己 fail；
//     成员表那条在 `plugin/render-surface`。⛔ 本用例**不**拿"回调之后某个 builder 还是空的"
//     当证据（回调根本拿不到 builder ⇒ 那是恒真断言，复审 F4）；这里只读**真的 render 过**的
//     display list 形状：show 前 = 0、show 后 = 2（且顺序/字段固定）、hide 后 = 0。
// =====================================================================================
void caseUiState() {
    lift_test::g_current_case = "plugin/ui-state";
    Fixture f;
    LIFT_CHECK(f.mem_ok && f.win_ok);
    if (!f.mem_ok || !f.win_ok) return;
    const bool loaded = f.load("hud-ui.js");
    LIFT_CHECK(loaded);
    if (!loaded) {
        std::fprintf(stderr, "  load error: %s\n", f.load_error.c_str());
        return;
    }
    LIFT_CHECK(f.host->script() != nullptr);
    if (f.host->script() == nullptr) return;
    LIFT_CHECK(f.host->script()->hasRender());
    LIFT_CHECK(f.host->script()->hasRun());

    lift::ui::UiFrameReadings r;
    std::string err;
    // ① **render 之前**的形状：还没 `ui.show` ⇒ 本帧 list 为 0（render 自己不编元素）。
    {
        lift::ui::UiBuilder before_render;
        LIFT_CHECK(f.host->script()->render(before_render, r, &err));
        LIFT_CHECK(before_render.elements().empty());
    }
    // ② 第 1 个读点：`start` arm 两道门（本拍只采样，不 fire）
    f.readings.script = "TITLE";
    f.readings.pos = 0;
    f.pump();
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->fired()), 0);
    // ③ 第 2 个读点：TITLE 命中 ⇒ onTrigger 只 `ctx.ui.show`（回调里没有 draw —— JS 已断言）
    f.pump();
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->fired()), 1);
    // ④ **下一次 render** 才画：本帧 list = 恰好 2 个元素（顺序 = 调用顺序），字段固定
    lift::ui::UiBuilder after_show;
    const bool shown_ok = f.host->script()->render(after_show, r, &err);
    if (!shown_ok) std::fprintf(stderr, "  render error: %s\n", err.c_str());
    LIFT_CHECK(shown_ok);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(after_show.elements().size()), 2);
    if (after_show.elements().size() == 2) {
        LIFT_CHECK(after_show.elements()[0].kind == lift::ui::ElementKind::Rect);
        LIFT_CHECK_EQ_I64(after_show.elements()[0].w, 120);
        LIFT_CHECK_EQ_I64(after_show.elements()[0].h, 24);
        LIFT_CHECK_EQ_U32(after_show.elements()[0].fill.r, 16u);   // #101820CC
        LIFT_CHECK_EQ_U32(after_show.elements()[0].fill.g, 24u);
        LIFT_CHECK_EQ_U32(after_show.elements()[0].fill.b, 32u);
        LIFT_CHECK_EQ_U32(after_show.elements()[0].fill.a, 204u);
        LIFT_CHECK(after_show.elements()[1].kind == lift::ui::ElementKind::Text);
        LIFT_CHECK(after_show.elements()[1].text == "mode title");
        LIFT_CHECK_EQ_I64(after_show.elements()[1].font_size, 16);
    }
    // ⑤ FIELD 命中 ⇒ hide（返回 [] = done）⇒ 下一次 render 又为 0
    f.readings.script = "FIELD";
    f.pump();
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->fired()), 2);
    LIFT_CHECK(f.host->runner().done());
    lift::ui::UiBuilder after_hide;
    LIFT_CHECK(f.host->script()->render(after_hide, r, &err));
    LIFT_CHECK(after_hide.elements().empty());
}

// =====================================================================================
// C2b · `plugin/render-stored-runctx-rejected`（复审 F2）：**存下来的 RunCtx** 不能在 render 里用
//   `fail`/`finish`/`quit`/`profile.*`
//
//   ★ render 期 `call_host` 非空（`read`/`log` 要用）⇒ 那 5 个"run-context 出口"必须靠
//     `PluginCallHost::inCallback()` 拒绝，⛔ 不能只判 `call_host != nullptr`。
//   ★ 判据两侧：① render **返回 true**（5 个出口各抛 TypeError、夹具自己接住；有任何一个被接受
//     ⇒ 夹具抛 `accepted:…` ⇒ render false）；② 宿主侧 `failed/finished/quit` 与 profile 桥的
//     begin/end 计数**全未变**（⛔ "抛了但已经生效"）。
//   ★ profile 桥必须**接上**：没接线时 `jsCtxProfileBegin` 本来就会抛 ⇒ 用例会假通过。
// =====================================================================================
struct ProfileProbe {
    int begins = 0;
    int ends = 0;
};

bool probeProfileBegin(void* ctx, const char* /*label*/, std::string* /*err*/) {
    ++static_cast<ProfileProbe*>(ctx)->begins;
    return true;
}

bool probeProfileEnd(void* ctx, std::string* text, std::string* /*err*/) {
    ++static_cast<ProfileProbe*>(ctx)->ends;
    if (text != nullptr) text->clear();
    return true;
}

void caseRenderStoredRunCtx() {
    lift_test::g_current_case = "plugin/render-stored-runctx-rejected";
    Fixture f;
    LIFT_CHECK(f.mem_ok && f.win_ok);
    if (!f.mem_ok || !f.win_ok) return;
    ProfileProbe probe;
    lift::ui::UiHostOps ops{};
    ops.ctx = &probe;
    ops.profile_begin = &probeProfileBegin;
    ops.profile_end = &probeProfileEnd;
    const bool loaded = f.load("stored-ctx.js", {}, &ops);
    LIFT_CHECK(loaded);
    if (!loaded) {
        std::fprintf(stderr, "  load error: %s\n", f.load_error.c_str());
        return;
    }
    LIFT_CHECK(f.host->script() != nullptr && f.host->script()->hasRender());
    // ① 第 1 个读点：`start` 存下 RunCtx（并 arm `hold`，本拍不 fire）
    f.readings.script = "TITLE";
    f.pump();
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->callbacks()), 1);
    // ② render：5 个 run-context 出口全部 TypeError（夹具自吞）；读数面仍然可用
    lift::ui::UiBuilder builder;
    lift::ui::UiFrameReadings readings;
    std::string err;
    const bool ok = f.host->script() != nullptr && f.host->script()->render(builder, readings, &err);
    if (!ok) std::fprintf(stderr, "  render error: %s\n", err.c_str());
    LIFT_CHECK(ok);
    LIFT_CHECK(builder.elements().empty());
    // ③ 宿主侧的效果**一个都没发生**（拒绝在 trampoline，不是"生效之后才抛"）
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK(!f.host->finished());
    LIFT_CHECK(!f.host->quitRequested());
    LIFT_CHECK_EQ_I64(probe.begins, 0);
    LIFT_CHECK_EQ_I64(probe.ends, 0);
}

// =====================================================================================
// C2b · `plugin/render-budget-deadline`（复审 F3）：`PluginScript::setBudget` 是**绝对** deadline
//   ★ `UiScript` 的口径：deadline = steady_clock 的**绝对**纳秒（`present_sdl.cpp` 给
//     `steadyNowNs() + 20ms`）⇒ 这里给一个"已经过去 1 ns"的 deadline，render 必须被打断。
//     若实现误加成相对（`steadyNowNs() + deadline`），deadline 会落进未来 ⇒ render 跑到底 ⇒ 红。
//   ★ 夹具 `for(;;)` 是承重的：中断回调每 ~10000 条字节码才跑一次，短脚本可能整段跑完都进不了中断。
// =====================================================================================
void caseRenderBudgetDeadline() {
    lift_test::g_current_case = "plugin/render-budget-deadline";
    std::string err;
    std::unique_ptr<lift::ui::PluginScript> script = loadPlugin("budget.js", {}, nullptr, &err);
    LIFT_CHECK(script != nullptr);
    if (script == nullptr) {
        std::fprintf(stderr, "  load error: %s\n", err.c_str());
        return;
    }
    LIFT_CHECK(script->hasRender());
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    const std::uint64_t deadline = static_cast<std::uint64_t>(now_ns);
    LIFT_CHECK(deadline > 1);
    // 绝对 deadline = 现在 - 1ns ⇒ 第一次中断检查就超时。
    script->setBudget(deadline - 1u, 0);
    lift::ui::UiBuilder builder;
    lift::ui::UiFrameReadings readings;
    std::string rerr;
    const bool ok = script->render(builder, readings, &rerr);
    if (ok) std::fprintf(stderr, "  render 没被预算打断（deadline 口径错了？）\n");
    LIFT_CHECK(!ok);
    LIFT_CHECK(script->overran());
    LIFT_CHECK(builder.elements().empty());
    // 唯一消息面：err 的尾巴点名 render 超预算（不是无名失败）。
    LIFT_CHECK(rerr.find("render 超出预算") != std::string::npos);
}

// =====================================================================================
// C2b · `plugin/render-read`：**render 里**的 `ctx.read.global.get` / `ctx.log` 有宿主来路
//   ★ 若 `readWired()` 仍绑 `in_callback_`（或 `init` 没 `setCallHost`）⇒ 插件在 render 里读就
//     TypeError ⇒ 本用例红（那是"render 的读数面静默缺失"，不是小事）。
// =====================================================================================
void caseRenderRead() {
    lift_test::g_current_case = "plugin/render-read";
    Fixture f;
    LIFT_CHECK(f.mem_ok && f.win_ok);
    if (!f.mem_ok || !f.win_ok) return;
    lift::GlobalValue seed;
    seed.kind = lift::GlobalKind::Int;
    seed.u32 = 0x1200u;
    lift::GlobalError seed_err = lift::GlobalError::None;
    LIFT_CHECK(lift::writeGlobal(f.mem, kFxGlobalId, lift::GlobalKind::Int, seed, &seed_err));
    LIFT_CHECK(seed_err == lift::GlobalError::None);
    const bool loaded = f.load("render-read.js", {{"probe", "global"}, {"expect", "4608"}});
    LIFT_CHECK(loaded);
    if (!loaded) {
        std::fprintf(stderr, "  load error: %s\n", f.load_error.c_str());
        return;
    }
    lift::ui::UiBuilder builder;
    lift::ui::UiFrameReadings readings;
    std::string err;
    const bool ok = f.host->script() != nullptr && f.host->script()->render(builder, readings, &err);
    if (!ok) std::fprintf(stderr, "  render error: %s\n", err.c_str());
    LIFT_CHECK(ok);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->callbacks()), 0);  // render 不是 Run 回调

    // ---- log：render 里的 `ctx.log` 可用；连续重复 ⇒ 宿主去重（只记一行）----
    lift_test::g_current_case = "plugin/render-read-log";
    Fixture g;
    const bool loaded2 = g.load("render-read.js", {{"probe", "log"}});
    LIFT_CHECK(loaded2);
    if (loaded2 && g.host->script() != nullptr) {
        lift::ui::UiBuilder b2;
        lift::ui::UiFrameReadings r2;
        std::string e2;
        const bool ok2 = g.host->script()->render(b2, r2, &e2);
        if (!ok2) std::fprintf(stderr, "  render error: %s\n", e2.c_str());
        LIFT_CHECK(ok2);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(g.host->logLines()), 1);
    }
}

// =====================================================================================
// C2b · `plugin/render-readings`：`read.frames` / `read.scriptPos` / `game.localFrames` / `game.mouse`
//   ★ 没有源 / 没有 top 帧 / 位置读不到 ⇒ **undefined / valid:false 且缺 pos**（⛔ 不编 0 / 空数组）。
// =====================================================================================
struct RenderFrameSource final : lift::ui::UiFrameSource {
    std::vector<lift::ui::UiFrameInfo> list;
    std::string err;
    std::vector<lift::ui::UiFrameInfo> localFrames(std::string* out_err) override {
        if (out_err != nullptr) *out_err = err;
        return list;
    }
};

// 两帧的固定读数：top = slot 2 / SC0000 / pos 0x1104；非 top = slot 1 / SC0009 / 无位置。
RenderFrameSource renderFrames() {
    RenderFrameSource src;
    lift::ui::UiFrameInfo top;
    top.slot = 2;
    top.script_id = 0;
    top.parent = 0xFFFFFFFFu;
    top.script = "SC0000";
    top.top = true;
    top.pos_ok = true;
    top.pos = 0x1104;
    lift::ui::UiFrameInfo below;
    below.slot = 1;
    below.script_id = 9;
    below.parent = 2;
    below.script = "SC0009";
    below.top = false;
    below.pos_ok = false;
    src.list = {top, below};
    return src;
}

// 用 `readings.js` 的一个 probe 跑一帧；false ⇒ 装载/render 失败（`*out` = 原因）。
bool renderProbe(const std::string& probe, lift::ui::UiFrameReadings readings, std::string* out) {
    std::string err;
    std::unique_ptr<lift::ui::PluginScript> script = loadPlugin("readings.js", {{"probe", probe}}, nullptr, out);
    if (script == nullptr) return false;
    lift::ui::UiBuilder builder;
    const bool ok = script->render(builder, readings, &err);
    if (!ok) *out = err;
    return ok;
}

void caseRenderReadings() {
    lift_test::g_current_case = "plugin/render-readings";
    {
        lift::ui::UiFrameReadings r;
        r.mouse_ok = true;
        r.mouse_x = 812;
        r.mouse_y = 622;
        std::string err;
        const bool ok = renderProbe("mouse", r, &err);
        if (!ok) std::fprintf(stderr, "  probe mouse: %s\n", err.c_str());
        LIFT_CHECK(ok);
    }
    {
        // ★ C2b-2：**有**读数源时安装 `buttons`，三位 = 真实电平（左按下 / 右中抬起）。
        //   红了两件事之一：没有源的宿主被编了 false，或有源时不安装 / 装错位。
        lift::ui::UiFrameReadings r;
        r.mouse_ok = true;
        r.mouse_x = 812;
        r.mouse_y = 622;
        r.mouse_buttons_ok = true;
        r.mouse_left = true;
        std::string err;
        const bool ok = renderProbe("mouse-buttons", r, &err);
        if (!ok) std::fprintf(stderr, "  probe mouse-buttons: %s\n", err.c_str());
        LIFT_CHECK(ok);
    }
    {
        RenderFrameSource src = renderFrames();
        lift::ui::UiFrameReadings r;
        r.frames = &src;
        std::string err;
        const bool ok = renderProbe("frames", r, &err);
        if (!ok) std::fprintf(stderr, "  probe frames: %s\n", err.c_str());
        LIFT_CHECK(ok);
    }
    {
        RenderFrameSource src = renderFrames();
        lift::ui::UiFrameReadings r;
        r.frames = &src;
        std::string err;
        const bool ok = renderProbe("pos", r, &err);
        if (!ok) std::fprintf(stderr, "  probe pos: %s\n", err.c_str());
        LIFT_CHECK(ok);
    }
    {
        // top 帧存在、只是这一拍位置读不到 ⇒ valid:false 且缺 pos（⛔ 不编 0）
        RenderFrameSource src = renderFrames();
        src.list[0].pos_ok = false;
        lift::ui::UiFrameReadings r;
        r.frames = &src;
        std::string err;
        const bool ok = renderProbe("pos-invalid", r, &err);
        if (!ok) std::fprintf(stderr, "  probe pos-invalid: %s\n", err.c_str());
        LIFT_CHECK(ok);
    }
    {
        // 没有帧源（nullptr）⇒ 三个口都 undefined
        lift::ui::UiFrameReadings r;
        std::string err;
        const bool ok = renderProbe("no-source", r, &err);
        if (!ok) std::fprintf(stderr, "  probe no-source: %s\n", err.c_str());
        LIFT_CHECK(ok);
    }
    {
        // 宿主报 err ⇒ 同一条（⛔ 不编空数组）
        RenderFrameSource src = renderFrames();
        src.err = "no-active-frame";
        lift::ui::UiFrameReadings r;
        r.frames = &src;
        std::string err;
        const bool ok = renderProbe("no-source", r, &err);
        if (!ok) std::fprintf(stderr, "  probe no-source/err: %s\n", err.c_str());
        LIFT_CHECK(ok);
    }
    {
        // `cur` 不活动（top 全 false）⇒ scriptPos undefined（⛔ 不拿 frames[0] 顶替）
        RenderFrameSource src = renderFrames();
        src.list[0].top = false;
        src.list[1].top = false;
        lift::ui::UiFrameReadings r;
        r.frames = &src;
        std::string err;
        const bool ok = renderProbe("no-top", r, &err);
        if (!ok) std::fprintf(stderr, "  probe no-top: %s\n", err.c_str());
        LIFT_CHECK(ok);
    }
}

}  // namespace

int main() {
    // ★ `ctx.draw.measure` 的来路是宿主字体 ⇒ 缺它这一档判不了（与 `ui-hud-script` 同一条：
    //   AGENTS.md 的纯档输入——两份宿主字体——假定在场）。⛔ 不"跳过一个 case 然后绿"。
    lift::FontEngine font;
    if (!font.fontFileAvailable(false)) {
        LIFT_SKIP("缺宿主字体 Amayui-CN_cnjp.ttf（LIFT_HOST_FONT_PATH / corpus/assets/fonts）");
    }
    caseModuleContract();
    caseApiSurface();
    caseInitialState();
    caseConcurrentArm();
    caseChain();
    caseGlobalReadModifyWrite();
    caseFinish();
    caseStartNowAndStaged();
    caseAtReadPointFileRead();
    caseFailInstall();
    caseFailInstallReleasesInput();
    caseFailReleasesInput();
    caseQuitReleasesInput();
    caseFinishLandsInput();
    // ---- C2b（render / ui / draw；pure 半）----
    caseRenderSurface();
    caseUiDraw(&font);
    caseUiState();
    caseRenderStoredRunCtx();
    caseRenderBudgetDeadline();
    caseRenderRead();
    caseRenderReadings();
    return lift_test::finish("plugin-runtime");
}
