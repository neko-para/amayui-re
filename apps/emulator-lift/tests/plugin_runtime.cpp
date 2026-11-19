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
// ★ 夹具口径：**零 `Machine`、零 GPU、零真窗口、零游戏安装**（与 `tests/trigger_runner.cpp` 同一条）：
//   一块假 `GuestMemory`（假 Engine / 假池）+ 一个默认 `WindowManager` + 注入的 `ReadingsSource`。
//   插件回调的全部效果都落在这两样上（Edit → guest 内存；Input → 队列 / 光标）。
// ★ 读点 = `PluginHost::onReadPoint(wm, point)`（它内部就是 `executor_.atPoint` + 首点 `sampleNow`）。
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>

#include "check.h"
#include "plugin/plugin_host.h"
#include "runtime/engine_marker.h"  // kEngineSingletonGlobal（假 Engine 的发布点）
#include "runtime/host_window.h"
#include "runtime/memory.h"
#include "runtime/script_globals.h"
#include "runtime/value_codec.h"  // encodeInt（ENC 往返的断言；⛔ 不写死编码后的数）

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

    bool load(const std::string& file, const std::map<std::string, std::string>& params = {}) {
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
        if (!host->init(source, fixturePath(file), params, nullptr, &load_error)) return false;
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

}  // namespace

int main() {
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
    return lift_test::finish("plugin-runtime");
}
