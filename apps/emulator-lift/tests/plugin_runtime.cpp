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
//   · `control-script/start-on-trigger-chain`：**真库** `scripts/plugins/lib/save.js`（假读数）；
//                                            参考入口 = 根 `scripts/plugins/load-save.js`（S1 拆出）：
//                                            `bootToTitle` + 载入链（2026-11-21 起 `loadSave`
//                                            自己**不等 TITLE**；TITLE 门在 `lib/boot-to-title.js`）：
//                                            `boot:title` 命中 → `start` 点 LoadData → SAVE → 读点 →
//                                            SBUNKI → `expect` 命中（用例显式给
//                                            `--plugin-param expect=FIELD,SC0000`）；两个 slot 的 `move`
//                                            落**不同** y（实测表，⛔ 不是公式）；精确坐标
//                                            (1070,480) / (300,rowY) / (640,323) + `done`/`fired=5`/`calls=6`。
//   · `control-script/save-missing-precheck`：缺存档文件（临时 appdata 只建目录、不放
//                                            `SAVE15.DAT`）⇒ `loadSave.start` 在**任何 click 之前**
//                                            `finish(5)`；`stagedDepth/depth=0`、光标不动、回调 2 次。
//   · `control-script/config-roundtrip`   ：`ctx.config` 真读写（临时 appdata + 预置
//                                            `plugin-config.json`）：六种原生值往返 / `all()` 深拷贝 /
//                                            `delete` / 保留名与 NaN ⇒ TypeError / **回调末 flush**
//                                            （文件里能读回本次写、且未知域没被重建掉）。
//   · `…-field-terminal`                  ：终态 = `expect` 里的脚本命中（FIELD 分支）直接收口；
//                                            不点 (690,323)，done 之后不再有回调。
//   · `…-second-confirm`                  ：终态的 **SBUNKI 0x0AF8 第二道确认**分支：真的点落 (690,323)
//                                            再 FIELD ⇒ done（`fired=6` / `calls=7`）；与上面那条并存。
//   · `…-bad-slot`                        ：非整数 / 越界（9 / -1 / 1.5 / abc）⇒ 构造时 throw ⇒ `run=failed`
//                                            且 reason 点名 `slot N`；0 / 7 合法（成对边界）。
//   · `control-script/load-save-onloaded` ：`loadSave` 的 **expect（落点由调用方给）+ onLoaded**：
//                                            `SC0000` 不在 expect 里 ⇒ 不是终态；`FIELD` 命中 ⇒ `onLoaded`
//                                            起场景（armed 换成场景的门）；`expect` 缺 / 元素非法 ⇒ 构造时 throw。
//   · `control-script/chains-route`        ：`chains.js` 的 `route`（**通用**，与 save 无关）：`load:*` ⇒ first、
//                                            其余 ⇒ then；**timeout 也按名字路由**；`route.start` 只启 first。
//   · `control-script/global-read-modify-write`：同拍 `read → edit → input`：Edit **立即**（读回新值）、
//                                            **不占** Input 预算（队列里只有那一条 click）。
//   · `plugin/finish-quit`（finish 半）   ：`ctx.finish(5)` 只结束 Run + 记 exit，⛔ 不改进程 exit。
//   · `start 的 ctx.now`（D-C2-10）       ：`start` 里 `ctx.now` 有值（script/pos/readIndex）；
//                                            `start` 里的 click **下一读点**才落地。
//   · `plugin/callback-js-band`           ：2 ms+ 的 JS 本体**跑完**（没被中断）⇒ 仍落 `js` 档
//                                            （`callbackBudgetReason(…, host=false)` 逐字；⛔ 不是 `(host)`）。
//   · `plugin/callback-host-pause`        ：**宿主调用暂停** —— `ctx.profile.end` 的宿主成本（假宿主
//                                            烧 5 ms CPU）**不进**用户 JS 的 2 ms（去掉暂停 ⇒ 本用例红）。
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
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <unistd.h>  // getpid（临时 appdata 的唯一性）
#endif

#include "check.h"
#include "plugin/plugin_host.h"
#include "runtime/budget.h"         // callbackDiag / pluginThreadCpuNs（2 ms 判据的读数）
#include "runtime/engine_marker.h"  // kEngineSingletonGlobal（假 Engine 的发布点）
#include "runtime/font.h"           // FontEngine（C2b：`ctx.draw.measure` 的来路）
#include "runtime/host_encoding.h"  // 临时 appdata 的 UTF-8 宿主路径（ctx.appdata 的测试）
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
// `cpu-budget.js` 的握手槽（int 池里的两个**空**槽；`<= kFxIntCount` ⇒ 可寻址）。
constexpr std::uint32_t kCpuBudgetSignalId = 0xB1B5u;  // JS 写 1：宿主线程据此挂起主线程
constexpr std::uint32_t kCpuBudgetFlagId = 0xB1B4u;    // 宿主线程恢复后写 1：JS 据此退出

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

// ★ C2c：**真库**目录（`scripts/plugins/lib`）。QJS 的 import 根 = 入口文件目录 ⇒ 夹具不能
//   import 它。★ S1 起 `lib/save.js` 只是库（没有 default 导出）；参考入口在**根** `load-save.js`。
std::string libPath(const std::string& name) {
    return (fs::path(LIFT_PLUGIN_LIB_DIR) / name).string();
}

// ★ S1：`--plugin` 的 id 根 = `scripts/plugins/`（`LIFT_PLUGIN_LIB_DIR` 的父目录）。场景入口在根上，
//   库仍在 `lib/`；`load-save.js` 是 `lib/save.js` 的参考入口（id = `load-save`）。
std::string pluginRootPath(const std::string& name) {
    return (fs::path(LIFT_PLUGIN_LIB_DIR).parent_path() / name).string();
}

// 宿主 `fs::path` → UTF-8（`ctx.appdata` 的根参数是 UTF-8；Windows 上不能拿 `.string()`）。
std::string hostPathText(const fs::path& p) {
#ifdef _WIN32
    return lift::wideToUtf8(p.wstring().c_str());
#else
    return p.string();
#endif
}

std::uint32_t processId() {
#ifdef _WIN32
    return static_cast<std::uint32_t>(::GetCurrentProcessId());
#else
    return static_cast<std::uint32_t>(::getpid());
#endif
}

bool readHostFile(const std::string& path, std::string* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool readFixture(const std::string& name, std::string* out) { return readHostFile(fixturePath(name), out); }

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

    // 从**任意绝对路径**装载（`origin` = 该路径：QJS 的 import 根 = 它的目录）。
    bool loadFile(const std::string& path, const std::map<std::string, std::string>& params = {},
                  const lift::ui::UiHostOps* host_ops = nullptr) {
        std::string source;
        if (!readHostFile(path, &source)) {
            load_error = "读不到插件：" + path;
            return false;
        }
        host = std::make_unique<lift::plugin::PluginHost>(readings);
        // ★ 预算（D5 的 2 ms / 100k 是**运行时口径**，不是本用例的判据）：这里放宽到
        //   500 ms / 1e7 ops —— 只为让"QJS 冷启动（第一次 JS_Call + 若干个对象分配）"不把
        //   用例变成随机红。⛔ 生产值仍只有一份（`runtime/budget.h`；`PluginHost` 读的就是它）。
        host->runner().setBudget(lift::plugin::Budget{500'000'000ull, 10'000'000u});
        // ★ 装配顺序与 `aot_main` **一致**：`init`（编译 + 沙箱）→ `attachMemory`（Edit/Read）。
        //   `host_ops` 只在需要判"profile 桥有没有被调到"的用例里给（`plugin/render-stored-runctx-rejected`）。
        if (!host->init(source, path, params, host_ops, &load_error)) return false;
        host->attachMemory(mem);
        return true;
    }

    bool load(const std::string& file, const std::map<std::string, std::string>& params = {},
              const lift::ui::UiHostOps* host_ops = nullptr) {
        return loadFile(fixturePath(file), params, host_ops);
    }

    // ★ S1：参考入口（config > params > 默认、自带 TITLE 门）在根 `load-save.js` —— 与 e2e
    //   同一个文件，⛔ 不复制。
    bool loadLib(const std::map<std::string, std::string>& params = {},
                 const lift::ui::UiHostOps* host_ops = nullptr) {
        return loadFile(pluginRootPath("load-save.js"), params, host_ops);
    }

    // 一个读点（本文件里"读点"的唯一含义）。
    void pump(lift::InputPoint point = lift::InputPoint::PeekMsg) { host->onReadPoint(wm, point); }

    // `ctx.appdata` 的根（**临时 appdata**；用例自己建 / 自己删）。`install_dir` 空 = 不判安装树。
    // ★ attach 在 `init` 之后也有效：`available/path` 是**每个回调**现读的（ctx 每次新建）。
    bool attachAppData(const std::string& root) {
        if (host == nullptr) {
            load_error = "attachAppData: 还没装载";
            return false;
        }
        std::string err;
        if (!host->attachAppData(root, "", &err)) {
            load_error = err;
            return false;
        }
        return true;
    }

    // `ctx.config` 的根（复用 `attachAppData` 的根；必须在 `attachAppData` 之后调）。
    bool attachConfig() {
        if (host == nullptr) {
            load_error = "attachConfig: 还没装载";
            return false;
        }
        std::string err;
        if (!host->attachConfig(&err)) {
            load_error = err;
            return false;
        }
        return true;
    }

    void cursor(int* x, int* y) const { wm.cursorClientPos(hwnd, x, y); }
};

// 失败时把人可读的原因打出来（否则 readout 只有名字）。
void dumpFail(const Fixture& f) {
    if (f.host == nullptr || !f.host->failed()) return;
    std::fprintf(stderr, "  plugin fail: name=%s reason=%s\n", f.host->failName().c_str(),
                 f.host->failReason().c_str());
}

// ★ 脚本化的夹具**没有"进屏"这一步** ⇒ 必须把读档界面的**页号**（guest int global `0x138e`）种成
//   **目标页**，否则 `save.js` 的 `readSavePage` 读到的不是 0：槽里存的是 `ENC(value, key)`，
//   而零内存 ≠ 明文 0（`ENC(0) = key`）⇒ 它会去点左右箭头翻页 ⇒ 整条脚本链的坐标/拍数全部错位。
//   真机那条路上这个 global 由 `SAVE.BIN` 第 11 行 `load-int` 写（见 `save-format.md` §加载界面的列表与翻页）。
constexpr std::uint32_t kSavePageGlobalId = 0x138eu;

void seedSavePage(Fixture& f, std::uint32_t page) {
    lift::GlobalValue v;
    v.kind = lift::GlobalKind::Int;
    v.u32 = page;
    lift::GlobalError err = lift::GlobalError::None;
    LIFT_CHECK(lift::writeGlobal(f.mem, kSavePageGlobalId, lift::GlobalKind::Int, v, &err));
    LIFT_CHECK(err == lift::GlobalError::None);
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
// `control-script/start-on-trigger-chain`：**真库** `lib/save.js`（经根入口 `load-save.js`）+ 假读数走完整条链
//
// ★ 2026-11-21 起 `loadSave.start` **不等 TITLE**（前提 = 调用方保证已在 TITLE；TITLE 门搬到了
//   `lib/boot-to-title.js`）⇒ 以根入口 `load-save.js` 当入口时走的是它的**参考入口**（`bootToTitle` + 载入链）：
//   `start` arm `boot:title`，命中后才 `click(1070,480)` 并 arm `load:save`。
//   ⇒ 下面两道 armed-name 断言就是"拆分"的地面真相：TITLE 门属于 boot 链，载入链的第一道门是 SAVE。
//
// 链与每拍的输入落点（IntentExecutor 七步：② staged→ready ③ poll ⑥ drain 一条）：
//   ① TITLE 首点：`start` 只 arm `boot:title` 门（本拍 poll 在 start 之前 ⇒ 不 fire；不提交 input）
//   ② TITLE 命中  ⇒ click(1070,480) staged
//   ③ SAVE 命中   ⇒ ② 提升 click、⑥ 落地 ⇒ 光标 (1070,480)；回调提交 move(300,rowY)
//   ④ 读点门命中  ⇒ ② 提升 move、⑥ 落地 ⇒ 光标 (300,rowY)；回调提交 click(300,rowY)
//   ⑤ SBUNKI 命中 ⇒ ⑥ 落地 click(300,rowY) ⇒ 光标仍是 (300,rowY)；回调提交 click(640,323)
//   ⑥ FIELD 命中  ⇒ ⑥ 落地 click(640,323) ⇒ 光标 (640,323)；终态 = done（fired=5 / calls=6）
//
// ★ 精确坐标是判据（⛔ 不是"没报错"）：变异 `click(300,rowY)` → `rowY+60` 在这里红（第 ⑤ 拍）。
// =====================================================================================

/** armed 集合必须恰好是那一个名字（`⛔` 不是"至少有一个"）。 */
void checkSingleArmed(Fixture& f, const char* expected) {
    const std::vector<std::string> armed = f.host->runner().armedNames();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(armed.size()), 1);
    if (armed.size() == 1) LIFT_CHECK(armed[0] == expected);
}

// 泵到 SBUNKI 门命中（= 提交 click(640,323) 并 arm 终态集合）；返回实测 row y（失败 -1）。
int driveToSbunki(Fixture& f) {
    int cx = 0;
    int cy = 0;
    // ① TITLE 第 1 个读点：start 只 arm `boot:title` 门（TITLE 门在 bootToTitle，⛔ 不在 loadSave 里）。
    f.readings.script = "TITLE.BIN";
    f.readings.pos = 0x0480;
    f.pump();
    dumpFail(f);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->fired()), 0);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->queue().stagedDepth()), 0);
    checkSingleArmed(f, "boot:title");
    // ② 下一读点（仍在 TITLE 区间）：`boot:title` 命中 ⇒ click(1070,480) staged；armed 换成载入链 ⇒
    //    `load:save`（★ 拆分后的第一道门就是 SAVE，⛔ 不再是 load:title）。
    f.pump();
    dumpFail(f);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->fired()), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->queue().stagedDepth()), 1);
    checkSingleArmed(f, "load:save");
    // ③ SAVE 读点：② 提升 click → ③ 命中 SAVE 门（提交 move(300,rowY)）→ ⑥ click 落地。
    f.readings.script = "SAVE.BIN";
    f.readings.pos = 0x1150;
    f.pump();
    dumpFail(f);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->fired()), 2);
    f.cursor(&cx, &cy);
    LIFT_CHECK_EQ_I64(cx, 1070);
    LIFT_CHECK_EQ_I64(cy, 480);
    // ④ 下一读点（仍在 SAVE 区间）：② 提升 move → ③ 读点门命中（提交 click(300,rowY)）→ ⑥ move 落地。
    f.pump();
    dumpFail(f);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->fired()), 3);
    f.cursor(&cx, &cy);
    LIFT_CHECK_EQ_I64(cx, 300);
    const int row_y = cy;
    // ⑤ SBUNKI 读点（★ 必须在 **读段之后**的 [0x0B18,0x0C58)：在这之前的点会被吞 —— 见 save.js 头）：
    //    ② 提升 click(300,rowY) → ③ 命中 SBUNKI 门（提交 click(640,323)）→ ⑥ click 落地。
    f.readings.script = "SBUNKI.BIN";
    f.readings.pos = 0x0b20;
    f.pump();
    dumpFail(f);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->fired()), 4);
    f.cursor(&cx, &cy);
    LIFT_CHECK_EQ_I64(cx, 300);
    LIFT_CHECK_EQ_I64(cy, row_y);  // ★ 行 y 的精确判据（变异 rowY+60 在这里红）
    return row_y;
}

int chainRowY(int slot) {
    lift_test::g_current_case = "control-script/start-on-trigger-chain";
    Fixture f;
    LIFT_CHECK(f.mem_ok && f.win_ok);
    if (!f.mem_ok || !f.win_ok) return -1;
    const bool loaded = f.loadLib({{"slot", std::to_string(slot)}, {"expect", "FIELD,SC0000"}});
    LIFT_CHECK(loaded);
    if (!loaded) {
        std::fprintf(stderr, "  load error: %s\n", f.load_error.c_str());
        return -1;
    }
    seedSavePage(f, static_cast<std::uint32_t>(slot / 10));  // 已停在目标页 ⇒ 不翻页（链形状见文件头）
    const int row_y = driveToSbunki(f);
    if (row_y < 0) return -1;
    // ⑥ FIELD 读点：② 提升 click(640,323) → ③ 命中 FIELD 终态（done）→ ⑥ click 落地。
    f.readings.script = "FIELD.BIN";
    f.readings.pos = 0x0600;
    f.pump();
    dumpFail(f);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->fired()), 5);
    int cx = 0;
    int cy = 0;
    f.cursor(&cx, &cy);
    LIFT_CHECK_EQ_I64(cx, 640);
    LIFT_CHECK_EQ_I64(cy, 323);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK(f.host->runner().done());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->callbacks()), 6);
    const std::string line = f.host->readout();
    LIFT_CHECK(line.find("run=done") != std::string::npos);
    // readout 的 params 是**按名字排序逗号拼接** ⇒ `expect=FIELD,SC0000,slot=N`。
    LIFT_CHECK(line.find("slot=" + std::to_string(slot)) != std::string::npos);
    LIFT_CHECK(line.find("expect=FIELD,SC0000") != std::string::npos);
    std::printf("  [%s slot=%d] %s\n", lift_test::g_current_case, slot, line.c_str());
    return row_y;
}

// -------------------------------------------------------------------------------------
// `…-field-terminal`：三选一终端的 **FIELD 分支**直接收口（假读数走这一支；真机 slot 5 在本机
//   headless 实测走 SBUNKI 0x0AF8 第二道确认，见 `.tmp/plugin-runtime/c2c-report.md` §5-2）
//   ★ 反证面：若 FIELD 分支被删 / 被当成第二道确认的入口，这里会多一次 click(690,323)
//     ⇒ 光标变成 (690,323)（本用例红）；或 fired 停在 4（也红）。
// -------------------------------------------------------------------------------------
void caseFieldTerminal() {
    lift_test::g_current_case = "control-script/start-on-trigger-chain-field-terminal";
    Fixture f;
    LIFT_CHECK(f.mem_ok && f.win_ok);
    if (!f.mem_ok || !f.win_ok) return;
    const bool loaded = f.loadLib({{"slot", "5"}, {"expect", "FIELD,SC0000"}});
    LIFT_CHECK(loaded);
    if (!loaded) {
        std::fprintf(stderr, "  load error: %s\n", f.load_error.c_str());
        return;
    }
    seedSavePage(f, 0);  // 槽 5 在页 0；夹具直接停在目标页（不翻页）
    if (driveToSbunki(f) < 0) return;
    // 终态 = FIELD：位置故意落在 SBUNKI 0x0AF8 区间里（终态由 script 名决定，不由位置决定）。
    f.readings.script = "FIELD.BIN";
    f.readings.pos = 0x0af8;
    f.pump();
    dumpFail(f);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->fired()), 5);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->callbacks()), 6);
    LIFT_CHECK(f.host->runner().done());
    LIFT_CHECK(!f.host->failed());
    int cx = 0;
    int cy = 0;
    f.cursor(&cx, &cy);
    LIFT_CHECK_EQ_I64(cx, 640);
    LIFT_CHECK_EQ_I64(cy, 323);  // ⛔ 不是 (690,323)：没有第二道确认
    // done 之后：再泵两拍也不许有新回调 / 新输入（终态是**真的**收口，不是暂时等待）。
    f.pump();
    f.pump();
    f.cursor(&cx, &cy);
    LIFT_CHECK_EQ_I64(cx, 640);
    LIFT_CHECK_EQ_I64(cy, 323);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->fired()), 5);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->callbacks()), 6);
    std::printf("  [%s] %s\n", lift_test::g_current_case, f.host->readout().c_str());
}

// -------------------------------------------------------------------------------------
// `…-second-confirm`：三选一终端的 **SBUNKI 0x0AF8 分支**（2026-11-20 mac headless 实测 slot 5
//   走的就是这条：进门即点 → guest 进 0x0AF8 → 再点 690,323 才到 FIELD）
//   ★ 判据：0x0AF8 命中 ⇒ 真的**提交并落地** click(690,323)（光标变 (690,323)），再 FIELD ⇒ done；
//     `fired=6` / `calls=7`。与 `…-field-terminal` 的 6 那条**并存**（两条都是 D-C2C-3 的合法终态）。
// -------------------------------------------------------------------------------------
void caseSecondConfirm() {
    lift_test::g_current_case = "control-script/start-on-trigger-chain-second-confirm";
    Fixture f;
    LIFT_CHECK(f.mem_ok && f.win_ok);
    if (!f.mem_ok || !f.win_ok) return;
    const bool loaded = f.loadLib({{"slot", "5"}, {"expect", "FIELD,SC0000"}});
    LIFT_CHECK(loaded);
    if (!loaded) {
        std::fprintf(stderr, "  load error: %s\n", f.load_error.c_str());
        return;
    }
    seedSavePage(f, 0);  // 槽 5 在页 0；夹具直接停在目标页（不翻页）
    if (driveToSbunki(f) < 0) return;
    int cx = 0;
    int cy = 0;
    // ④' SBUNKI 0x0AF8：② 提升并落地 click(640,323) → ③ 命中第二道确认（提交 click(690,323)）。
    f.readings.script = "SBUNKI.BIN";
    f.readings.pos = 0x0af8;
    f.pump();
    dumpFail(f);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->fired()), 5);
    f.cursor(&cx, &cy);
    LIFT_CHECK_EQ_I64(cx, 640);
    LIFT_CHECK_EQ_I64(cy, 323);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->queue().stagedDepth()), 1);  // click(690,323) 已排队
    // ⑤' FIELD：② 提升并落地 click(690,323) → ③ 命中 FIELD-2 ⇒ done。
    f.readings.script = "FIELD.BIN";
    f.readings.pos = 0x0600;
    f.pump();
    dumpFail(f);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->fired()), 6);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->callbacks()), 7);
    LIFT_CHECK(f.host->runner().done());
    LIFT_CHECK(!f.host->failed());
    f.cursor(&cx, &cy);
    LIFT_CHECK_EQ_I64(cx, 690);  // ★ 690 那一步**真的发生了**（不是只 arm 不点）
    LIFT_CHECK_EQ_I64(cy, 323);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->queue().depth()), 0);
    const std::string line = f.host->readout();
    LIFT_CHECK(line.find("run=done") != std::string::npos);
    LIFT_CHECK(line.find("calls=7") != std::string::npos);
    std::printf("  [%s] %s\n", lift_test::g_current_case, line.c_str());
}

// -------------------------------------------------------------------------------------
// `…-bad-slot`：非整数 / 越界 ⇒ `loadSave` **构造时** throw（装载成功、`start` 里抛 ⇒ Run fail）
//   ★ 0 / 7 是合法边界（成对断言）：实测行表到 7（8–9 抛）。
// -------------------------------------------------------------------------------------
void caseBadSlot() {
    lift_test::g_current_case = "control-script/start-on-trigger-chain-bad-slot";
    struct BadSlot {
        const char* param;
        const char* needle;
    };
    const BadSlot bad[] = {
        // ★ `9` 在 2026-11-22 之后**是合法槽**（读档界面 90 页 × 10 行 ⇒ 0..899；见 `save.js` 的 `MAX_SLOT`）
        //   ⇒ 越界边界挪到 900。
        {"900", "slot 900"}, {"-1", "slot -1"}, {"1.5", "slot 1.5"}, {"abc", "slot NaN"},
    };
    for (const BadSlot& b : bad) {
        Fixture f;
        LIFT_CHECK(f.mem_ok && f.win_ok);
        if (!f.mem_ok || !f.win_ok) return;
        const bool loaded = f.loadLib({{"slot", b.param}, {"expect", "FIELD,SC0000"}});
        LIFT_CHECK(loaded);  // 装载不看参数；抛在 start
        if (!loaded) {
            std::fprintf(stderr, "  load error (%s): %s\n", b.param, f.load_error.c_str());
            continue;
        }
        f.pump();
        dumpFail(f);
        LIFT_CHECK(f.host->failed());
        LIFT_CHECK(f.host->failReason().find(b.needle) != std::string::npos);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->callbacks()), 1);  // start 跑了、onTrigger 没跑
    }
    // 合法边界 0 / 7 不抛（构造成功、`start` 发 click(1070,480) 并 arm SAVE 门 —— 当拍不 fire）。
    for (const int slot : {0, 7}) {
        Fixture f;
        const bool loaded = f.loadLib({{"slot", std::to_string(slot)}, {"expect", "FIELD,SC0000"}});
        LIFT_CHECK(loaded);
        if (!loaded) continue;
        f.pump();
        dumpFail(f);
        LIFT_CHECK(!f.host->failed());
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->fired()), 0);
    }
}

void caseChain() {
    const int y4 = chainRowY(4);
    const int y5 = chainRowY(5);
    const int y7 = chainRowY(7);  // 表里第 7 行（save07-battle.plan 的槽 007 读数）
    // ★ 实测表：slot 4 → 328、slot 5 → 388、slot 7 → 507（⛔ 不是公式；见 D-C2C-3 / save07-battle.plan）。
    LIFT_CHECK_EQ_I64(y4, 328);
    LIFT_CHECK_EQ_I64(y5, 388);
    LIFT_CHECK_EQ_I64(y7, 507);
    LIFT_CHECK(y4 != y5);  // 两个 slot 的链必须发不同坐标（"只是没报错"不够）
    LIFT_CHECK(y5 != y7);  // ★ 7 是新补进表的那一行：退回 0–6 的表会让它（= undefined ⇒ 构造 throw）红
    caseFieldTerminal();
    caseSecondConfirm();
    caseBadSlot();
}


// =====================================================================================
// `control-script/load-save-onloaded`：`loadSave` 的 **expect（落点由调用方给）+ onLoaded**
//
// 判据（三条；⛔ 不判游戏业务）：
//   ① **期望值/名字表**：`expect` 只给 `FIELD.BIN` 时 `SC0000.BIN` **不是**终态（旧实现硬编码 FIELD/SC0000）；
//   ② `expect` 命中 ⇒ 先 log、再 `onLoaded`：armed 集合**换成**场景的门（`battle:field`）；
//   ③ `expect` 缺 / 元素不是非空字符串 ⇒ **构造时 throw** ⇒ `run=failed`（⛔ 不猜默认值）。
//
// ★ ①② 用**交付样例** `scripts/plugins/scenario-save07-battle.js`（S1 起入口在**根**；真 `lib/save.js`
//   + 真 `lib/chains.js`）；③ 需要一份"故意漏参数"的入口 ⇒ run 私有临时目录放 `lib/save.js` 的
//   **逐字节副本** + 小入口（import 根 = 入口目录 ⇒ 只有这样它才 import 得到真库；
//   ⛔ 不往 `scripts/plugins/lib/` 塞测试夹具）。
// =====================================================================================

// run 私有目录（`fs::temp_directory_path()` 之下；析构时删掉 ⇒ ⛔ 不污染工作树）。
struct ScopedTempDir {
    fs::path path;
    ~ScopedTempDir() {
        if (path.empty()) return;
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    bool make(const std::string& tag) {
        static int seq = 0;
        ++seq;
        std::error_code ec;
        path = fs::temp_directory_path(ec) / (tag + "-" + std::to_string(seq));
        if (ec || path.empty()) return false;
        fs::remove_all(path, ec);
        ec.clear();
        fs::create_directories(path, ec);
        return !ec;
    }
};

bool writeHostFile(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.flush();
    return static_cast<bool>(out);
}

// ③ 的两份探针：`loadSave` 的 `expect` 校验只能拿**裸 loadSave** 测（参考入口有默认值 + 一行 info）。
const char* kNoExpectProbe = R"JS(
import { loadSave } from "./save.js";

const plugin = {
    start(ctx) {
        return loadSave(7, {}).start(ctx);
    },
    onTrigger(ctx, ev) {
        return loadSave(7, {}).onTrigger(ctx, ev);
    },
};

export default plugin;
)JS";

const char* kBadExpectProbe = R"JS(
import { loadSave } from "./save.js";

const plugin = {
    start(ctx) {
        return loadSave(7, { expect: ["FIELD.BIN", ""] }).start(ctx);
    },
    onTrigger(ctx, ev) {
        return loadSave(7, { expect: ["FIELD.BIN", ""] }).onTrigger(ctx, ev);
    },
};

export default plugin;
)JS";

// =====================================================================================
// `control-script/save-missing-precheck`：缺 `SAVE<NN>.DAT` ⇒ **第一个 click 之前** `finish(5)`
//
// 判据（红了意味着什么）：`loadSave.start` 的前置检查被删 / 挪到 click 之后 ——
//   · 缺文件时**任何 click 都不得提交**（`stagedDepth / depth` 都是 0，光标不动）；
//   · Run 必须自己收口：`finished + exit=5`，reason 点名 `SAVE15.DAT` / `slot 15`；
//   · 回调数 = 2（`start` + TITLE 门那一次）：⛔ 不是在 30 s 门超时之后才失败。
//
// ★ 夹具：临时 appdata **建出**存档目录（引擎启动期会建它；见 `save-format.md` §1），
//   ⛔ 不放 `SAVE15.DAT`。这是"复制错一层 / 漏拷槽"的机械复现，不依赖 `saves/`。
// ★ 参考入口自带 `bootToTitle`（TITLE 门）⇒ 第 2 个读点才进入 `loadSave.start`；
//   boot 链的 TITLE 命中**不点任何坐标**（`boot-to-title.js`），所以 0 click 是这条链的真判据。
// =====================================================================================
void caseSaveMissingPrecheck() {
    lift_test::g_current_case = "control-script/save-missing-precheck";
    Fixture f;
    LIFT_CHECK(f.mem_ok && f.win_ok);
    if (!f.mem_ok || !f.win_ok) return;
    ScopedTempDir appdata;
    LIFT_CHECK(appdata.make("lift-plugin-save-precheck"));
    if (appdata.path.empty()) return;
    std::error_code ec;
    // ★ 非 ASCII 段必须从 UTF-8 → 宿主 `wchar_t`（`fs::path` 的窄构造按 ACP 解码，会造出错的目录名）。
    const fs::path save_dir =
        appdata.path /
        fs::path(lift::utf8ToWide("Local/Eushully/天結いキャッスルマイスター/SAVE"));
    fs::create_directories(save_dir, ec);  // 目录存在、文件不存在 ⇒ 只可能是"这一槽缺"
    LIFT_CHECK(!ec);
    if (ec) return;
    const bool loaded = f.loadLib({{"slot", "15"}, {"expect", "FIELD,SC0000"}});
    LIFT_CHECK(loaded);
    if (!loaded) {
        std::fprintf(stderr, "  load error: %s\n", f.load_error.c_str());
        return;
    }
    LIFT_CHECK(f.attachAppData(hostPathText(appdata.path)));
    // ① TITLE 读点：参考入口的 start 只 arm `boot:title`（不提交 input）。
    f.readings.script = "TITLE.BIN";
    f.readings.pos = 0x0480;
    f.pump();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->queue().stagedDepth()), 0);
    LIFT_CHECK(!f.host->finished());
    // ② TITLE 命中 ⇒ `loadSave.start`：前置检查必须在这里 finish(5) + 返回 []，⛔ 一个 click 都不发。
    f.pump();
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK(f.host->finished());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->finishCode()), 5);
    const std::string reason = f.host->finishReason();
    LIFT_CHECK(reason.find("load-save: save file missing:") != std::string::npos);
    LIFT_CHECK(reason.find("SAVE15.DAT") != std::string::npos);
    LIFT_CHECK(reason.find("slot 15") != std::string::npos);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->queue().stagedDepth()), 0);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->queue().depth()), 0);
    int cx = 0;
    int cy = 0;
    f.cursor(&cx, &cy);
    LIFT_CHECK_EQ_I64(cx, 0);
    LIFT_CHECK_EQ_I64(cy, 0);
    LIFT_CHECK(f.host->runner().done());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->callbacks()), 2);
    const std::string line = f.host->readout();
    LIFT_CHECK(line.find("run=finished") != std::string::npos);
    LIFT_CHECK(line.find("exit=5") != std::string::npos);
    LIFT_CHECK(line.find("reason=load-save: save file missing:") != std::string::npos);
    std::printf("  [%s] %s\n", lift_test::g_current_case, line.c_str());
}

// =====================================================================================
// `control-script/config-roundtrip`：`ctx.config` 的**读写面**（真 `PluginHost` + 临时 appdata）
//
// 判据（红了意味着什么）：
//   ① 预置的 `test.config.pre = 123` 被读到（= attach 真的读了 appdata 根的文件）；
//   ② 六种原生值 set → get / has / all（深拷贝）/ delete；保留名 / NaN / undefined / 函数 ⇒ TypeError；
//   ③ **回调末 flush 真的发生**：回调返回后 `plugin-config.json` 里能读回本次写，且预置的未知域
//      `keep.me` 没被重建掉（前向兼容）。
// ★ 失败都走 `ctx.fail`（JS 侧逐项点名）；C++ 侧只判 `!failed` / `done` / 文件内容。
// =====================================================================================
void caseConfigRoundtrip() {
    lift_test::g_current_case = "control-script/config-roundtrip";
    Fixture f;
    LIFT_CHECK(f.mem_ok && f.win_ok);
    if (!f.mem_ok || !f.win_ok) return;
    ScopedTempDir appdata;
    LIFT_CHECK(appdata.make("lift-plugin-config-roundtrip"));
    if (appdata.path.empty()) return;
    std::error_code ec;
    fs::create_directories(appdata.path, ec);
    LIFT_CHECK(!ec);
    const fs::path file = appdata.path / "plugin-config.json";
    LIFT_CHECK(writeHostFile(file, R"({"version":1,"domains":{"test.config":{"pre":123},"keep.me":{"untouched":true}}})"));

    const bool loaded = f.load("config-roundtrip.js");
    LIFT_CHECK(loaded);
    if (!loaded) {
        std::fprintf(stderr, "  load error: %s\n", f.load_error.c_str());
        return;
    }
    LIFT_CHECK(f.attachAppData(hostPathText(appdata.path)));
    LIFT_CHECK(f.attachConfig());
    f.pump();
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK(f.host->runner().done());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->callbacks()), 1);

    std::string after;
    LIFT_CHECK(readHostFile(hostPathText(file), &after));
    LIFT_CHECK(after.find("\"num\": 7") != std::string::npos);
    LIFT_CHECK(after.find("\"str\": \"hello\"") != std::string::npos);
    LIFT_CHECK(after.find("\"pre\": 123") != std::string::npos);
    LIFT_CHECK(after.find("keep.me") != std::string::npos);
    LIFT_CHECK(after.find("untouched") != std::string::npos);
    LIFT_CHECK(after.find("temp") == std::string::npos);  // delete 过的 key 不落盘
}

// =====================================================================================
// `plugin/config-host-pause`：大域 `all()` 的值转换是**宿主 CPU**（不计用户 2 ms）
//
// 判据（红了意味着什么）：`configValueToJs`（1024 个 key 的建树 / 定义属性）被算进
// `js_cpu_ns` ⇒ 一个只读配置、什么都没做的回调会误报 `callback budget exceeded`。
// ★ 数据 = 临时 appdata 的 `plugin-config.json`（域 `big`，1024 个 key × 950 B；
//   文件 < 1 MiB、每值 1 节点 ⇒ 合法）；夹具只 `all()` + 核对 key 数。
// ★ 判据必须**能红**（复审 I3）：只看 `js_cpu < 2 ms` 的话，去掉 HostPause 后 ~1.5 ms 仍绿。
//   这里钉"pause 下界 + pause 主导"：转换（1024 个长字符串 + 定义属性）是纯宿主 CPU ⇒
//   `host_pause_ns` 必须 ≥ 500 µs 且 ≥ 2×`js_cpu_ns`；不包转换那段时 pause 只剩 store 调用
//   （几十 µs）⇒ 两条都红。
// =====================================================================================
void caseConfigHostPause() {
    lift_test::g_current_case = "plugin/config-host-pause";
    Fixture f;
    LIFT_CHECK(f.mem_ok && f.win_ok);
    if (!f.mem_ok || !f.win_ok) return;
    ScopedTempDir appdata;
    LIFT_CHECK(appdata.make("lift-plugin-config-host-pause"));
    if (appdata.path.empty()) return;
    std::string doc = R"({"version":1,"domains":{"big":{)";
    const std::string value(950, 'x');
    for (int i = 0; i < 1024; ++i) {
        if (i != 0) doc += ",";
        char key[32];
        std::snprintf(key, sizeof(key), "\"k%04d\":\"", i);
        doc += key;
        doc += value;
        doc += "\"";
    }
    doc += "}}}";
    const fs::path file = appdata.path / "plugin-config.json";
    LIFT_CHECK(writeHostFile(file, doc));
    const bool loaded = f.load("config-all.js");
    LIFT_CHECK(loaded);
    if (!loaded) {
        std::fprintf(stderr, "  load error: %s\n", f.load_error.c_str());
        return;
    }
    LIFT_CHECK(f.attachAppData(hostPathText(appdata.path)));
    LIFT_CHECK(f.attachConfig());
    // 生产值 2 ms / op_limit 不限：这条判的**只**是 CPU deadline。
    f.host->runner().setBudget(lift::plugin::Budget{2'000'000ull, 0u});
    f.pump();
    dumpFail(f);
    const lift::plugin::CallbackDiag& d = lift::plugin::callbackDiag();
    std::printf("  [%s] js_cpu_us=%llu paused_us=%llu\n", lift_test::g_current_case,
                static_cast<unsigned long long>(d.js_cpu_ns / 1000),
                static_cast<unsigned long long>(d.host_pause_ns / 1000));
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK(f.host->runner().done());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->callbacks()), 1);
    LIFT_CHECK(d.host_pause_ns > 0);
    LIFT_CHECK(d.host_pause_ns >= 500'000ull);
    LIFT_CHECK(d.host_pause_ns >= 2 * d.js_cpu_ns);
    LIFT_CHECK(d.js_cpu_ns < 2'000'000ull);
}

void caseLoadSaveOnLoaded() {
    lift_test::g_current_case = "control-script/load-save-onloaded";
    Fixture f;
    LIFT_CHECK(f.mem_ok && f.win_ok);
    if (!f.mem_ok || !f.win_ok) return;
    const bool loaded = f.loadFile(pluginRootPath("scenario-save07-battle.js"));
    LIFT_CHECK(loaded);
    if (!loaded) {
        std::fprintf(stderr, "  load error: %s\n", f.load_error.c_str());
        return;
    }
    // 载入链（TITLE → SAVE → row → SBUNKI）由库自己走；这里只喂假读数。
    // ★ 样例入口自带 `bootToTitle`（TITLE 门）⇒ 驱动方式与 `start-on-trigger-chain` 相同。
    seedSavePage(f, 0);  // 槽 7 在页 0；夹具直接停在目标页（不翻页）
    const int row_y = driveToSbunki(f);
    if (row_y < 0) return;
    LIFT_CHECK_EQ_I64(row_y, 507);  // 槽 7（样例就是 `loadSave(7, { expect: ["FIELD.BIN"] })`）
    // ---- ① 期望值/名字表：SC0000 不在 expect 里 ⇒ **不是**终态 ----
    {
        const std::vector<std::string> armed = f.host->runner().armedNames();
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(armed.size()), 2);  // expect 1 条 + 第二道确认
        bool has_field_expect = false;
        bool has_confirm = false;
        for (const std::string& name : armed) {
            if (name == "load:expect:FIELD.BIN") has_field_expect = true;
            if (name == "load:confirm2") has_confirm = true;
        }
        LIFT_CHECK(has_field_expect);
        LIFT_CHECK(has_confirm);
    }
    f.readings.script = "SC0000.BIN";
    f.readings.pos = 0x0600;
    f.pump();
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->fired()), 4);      // 没有终态门命中
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->callbacks()), 5);  // 也没有新回调
    // ---- ② expect 命中 ⇒ 先 log、再 onLoaded；armed 换成场景的门 ----
    f.readings.script = "FIELD.BIN";
    f.readings.pos = 0x0600;  // ⛔ 不在 [0x579C,0x5860)：这是"刚进 FIELD"，场景的门还没到
    f.pump();
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->fired()), 5);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->callbacks()), 6);  // start + 载入链 5
    LIFT_CHECK(!f.host->runner().done());
    const std::vector<std::string> armed = f.host->runner().armedNames();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(armed.size()), 1);
    if (armed.size() == 1) LIFT_CHECK(armed[0] == "battle:field");  // ★ onLoaded 起了场景、场景的门被 arm
    // 场景的第一跳（走 `chains.js` 的 route）：FIELD 主轮询体命中 ⇒ scenario.onTrigger 提交 click(1220,45)。
    f.readings.pos = 0x5800;  // [0x579C,0x5860) 内部
    f.pump();
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->fired()), 6);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->queue().stagedDepth()), 1);
    {
        const std::vector<std::string> next = f.host->runner().armedNames();
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(next.size()), 1);
        if (next.size() == 1) LIFT_CHECK(next[0] == "battle:selact");
    }
    {
        const std::string line = f.host->readout();
        LIFT_CHECK(line.find("run=waiting") != std::string::npos);
        std::printf("  [%s] %s\n", lift_test::g_current_case, line.c_str());
    }

    // ---- ③ expect 缺失 / 元素非法 ⇒ 构造时 throw（Run fail 并点名，⛔ 不静默）----
    ScopedTempDir tmp;
    LIFT_CHECK(tmp.make("lift-plugin-onloaded"));
    if (tmp.path.empty()) return;
    std::error_code ec;
    // ★ 2026-11-21：`save.js` 现在 import 同目录的兄弟模块（`script-positions.js` / `boot-to-title.js` /
    //   `chains.js`）⇒ 只复制 save.js 会让探针在 module 解析期就失败。复制**整份 lib**（只复制 .js），
    //   仍然是"run 私有临时目录"，⛔ 不往真库里塞夹具。
    const fs::path lib_dir = fs::path(LIFT_PLUGIN_LIB_DIR);
    std::size_t copied_lib = 0;
    for (fs::directory_iterator it(lib_dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::error_code one;
        if (!it->is_regular_file(one) || one) continue;
        if (it->path().extension() != ".js") continue;
        std::error_code copy_ec_one;
        fs::copy_file(it->path(), tmp.path / it->path().filename(),
                      fs::copy_options::overwrite_existing, copy_ec_one);
        if (!copy_ec_one) ++copied_lib;
        else ec = copy_ec_one;
    }
    LIFT_CHECK(!ec);
    LIFT_CHECK(copied_lib > 0);
    LIFT_CHECK(writeHostFile(tmp.path / "no-expect.js", kNoExpectProbe));
    LIFT_CHECK(writeHostFile(tmp.path / "bad-expect.js", kBadExpectProbe));
    struct ExpectProbe {
        const char* file;
        const char* needle;
    };
    const ExpectProbe probes[] = {
        {"no-expect.js", "options.expect"},  // 缺 ⇒ 消息里点名 expect
        {"bad-expect.js", "expect[1]"},      // 元素不是非空字符串 ⇒ 点名下标
    };
    for (const ExpectProbe& probe : probes) {
        Fixture p;
        LIFT_CHECK(p.mem_ok && p.win_ok);
        if (!p.mem_ok || !p.win_ok) return;
        const bool ok = p.loadFile((tmp.path / probe.file).string());
        LIFT_CHECK(ok);
        if (!ok) {
            std::fprintf(stderr, "  load error (%s): %s\n", probe.file, p.load_error.c_str());
            continue;
        }
        p.pump();
        dumpFail(p);
        LIFT_CHECK(p.host->failed());
        LIFT_CHECK(p.host->failReason().find(probe.needle) != std::string::npos);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(p.host->callbacks()), 1);  // start 抛了、onTrigger 没跑
        const std::string line = p.host->readout();
        LIFT_CHECK(line.find("run=failed") != std::string::npos);
    }
    // 缺失 expect 的消息里要给出**三类别名**（ADV / 战斗 / 地图）—— ⛔ 不许只说"必填"。
    {
        Fixture p;
        LIFT_CHECK(p.mem_ok && p.win_ok);
        if (!p.mem_ok || !p.win_ok) return;
        const bool ok = p.loadFile((tmp.path / "no-expect.js").string());
        LIFT_CHECK(ok);
        if (ok) {
            p.pump();
            const std::string reason = p.host->failReason();
            LIFT_CHECK(reason.find("ADV") != std::string::npos);
            LIFT_CHECK(reason.find("BTL.BIN") != std::string::npos);
            LIFT_CHECK(reason.find("FIELD.BIN") != std::string::npos);
        }
    }
}

// =====================================================================================
// `control-script/chains-route`：`chains.js` 的 `route` —— **通用**（与 save 无关）
//
// 判据：
//   ① `load:*` 事件走 `first`（它的 click 提交/落地）；其余走 `then`（click 来源可区分）；
//   ② **timeout 也按名字路由**（`load:*` 的 timeout ⇒ first；`then:*` 的 ⇒ then）—— ⛔ 不看 kind；
//   ③ `route.start` **只启 first**（`then` 由调用方在自己的 onLoaded 里拉起 —— 探针里就是 first.onTrigger
//      调 `then.start`）。
// ★ 时钟注入（`setClockForTest`；与 `tests/recipe_driver_parity.cpp` 同一条）：超时判据**不真睡**。
// =====================================================================================
std::uint64_t g_routeNowMs = 1000;
std::uint64_t routeNowMs() { return g_routeNowMs; }

const char* kRouteProbe = R"JS(
import { route } from "./chains.js";

const first = {
    start(ctx) {
        return ctx.trigger.afterScript("FIRST.BIN", { name: "load:armed", timeoutMs: 3 });
    },
    onTrigger(ctx, ev) {
        if (ev.kind === "timeout") {
            ctx.finish(5, `first: timeout (${ev.name})`);
            return [];
        }
        ctx.input.click(11, 11);
        return then.start(ctx);   // ★ 调用方在 onLoaded 里拉起 then（route.start 只启 first）
    },
};

const then = {
    start(ctx) {
        return ctx.trigger.afterScript("THEN.BIN", { name: "then:armed", timeoutMs: 3 });
    },
    onTrigger(ctx, ev) {
        if (ev.kind === "timeout") {
            ctx.finish(7, `then: timeout (${ev.name})`);
            return [];
        }
        ctx.input.click(22, 22);
        return [];
    },
};

export default route((ev) => String(ev.name).startsWith("load:"), first, then);
)JS";

void caseChainsRoute() {
    lift_test::g_current_case = "control-script/chains-route";
    ScopedTempDir tmp;
    LIFT_CHECK(tmp.make("lift-plugin-chains"));
    if (tmp.path.empty()) return;
    std::error_code ec;
    fs::copy_file(fs::path(libPath("chains.js")), tmp.path / "chains.js",
                  fs::copy_options::overwrite_existing, ec);
    LIFT_CHECK(!ec);
    LIFT_CHECK(writeHostFile(tmp.path / "route-probe.js", kRouteProbe));

    // ---- ① 名字路由：`load:*` ⇒ first；其余 ⇒ then（click 坐标可区分来源）----
    {
        Fixture f;
        LIFT_CHECK(f.mem_ok && f.win_ok);
        if (!f.mem_ok || !f.win_ok) return;
        const bool loaded = f.loadFile((tmp.path / "route-probe.js").string());
        LIFT_CHECK(loaded);
        if (!loaded) {
            std::fprintf(stderr, "  load error: %s\n", f.load_error.c_str());
            return;
        }
        g_routeNowMs = 1000;
        f.host->runner().setClockForTest(&routeNowMs);
        f.host->executor().setClockForTest(&routeNowMs);
        f.readings.script = "FIRST.BIN";
        f.readings.pos = 0x10;
        f.pump();  // start：只启 first ⇒ armed = { load:armed }
        dumpFail(f);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->fired()), 0);
        {
            const std::vector<std::string> armed = f.host->runner().armedNames();
            LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(armed.size()), 1);
            if (armed.size() == 1) LIFT_CHECK(armed[0] == "load:armed");
        }
        f.pump();  // load:armed 命中 ⇒ first.onTrigger ⇒ click(11,11) + then.start
        dumpFail(f);
        LIFT_CHECK(!f.host->failed());
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->fired()), 1);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->queue().stagedDepth()), 1);
        {
            const std::vector<std::string> armed = f.host->runner().armedNames();
            LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(armed.size()), 1);
            if (armed.size() == 1) LIFT_CHECK(armed[0] == "then:armed");  // ★ then 由调用方拉起
        }
        f.readings.script = "THEN.BIN";  // then:armed 命中 ⇒ then.onTrigger ⇒ click(22,22)；click(11,11) 落地
        f.pump();
        dumpFail(f);
        LIFT_CHECK(!f.host->failed());
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->fired()), 2);
        int cx = 0;
        int cy = 0;
        f.cursor(&cx, &cy);
        LIFT_CHECK_EQ_I64(cx, 11);  // first 的 click 真的落地了
        LIFT_CHECK_EQ_I64(cy, 11);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->queue().stagedDepth()), 1);  // click(22,22) 来自 then
        LIFT_CHECK(f.host->runner().done());
        std::printf("  [%s] %s\n", lift_test::g_current_case, f.host->readout().c_str());
    }

    // ---- ② `load:*` 的 **timeout** 也走 first（按名字，⛔ 不按 kind）----
    {
        Fixture f;
        LIFT_CHECK(f.mem_ok && f.win_ok);
        if (!f.mem_ok || !f.win_ok) return;
        const bool loaded = f.loadFile((tmp.path / "route-probe.js").string());
        LIFT_CHECK(loaded);
        if (!loaded) return;
        g_routeNowMs = 1000;
        f.host->runner().setClockForTest(&routeNowMs);
        f.host->executor().setClockForTest(&routeNowMs);
        f.readings.script = "FIRST.BIN";
        f.readings.pos = 0x10;
        f.pump();  // start：arm load:armed（deadline = 1003）
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->timeouts()), 0);
        g_routeNowMs = 1010;  // 越过 deadline（⛔ 不真睡）
        // ★ 这一拍**不许**命中 fire（fire 优先于 timeout）⇒ 喂一个不匹配的读数点。
        f.readings.script = "OTHER.BIN";
        f.readings.pos = 0x00;
        f.pump();  // timeout 事件 name=load:armed ⇒ first.onTrigger
        dumpFail(f);
        LIFT_CHECK(f.host->finished());
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->finishCode()), 5);
        LIFT_CHECK(f.host->finishReason().find("first: timeout") != std::string::npos);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->timeouts()), 1);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->fired()), 0);
    }

    // ---- ③ `then:*` 的 timeout 走 then ----
    {
        Fixture f;
        LIFT_CHECK(f.mem_ok && f.win_ok);
        if (!f.mem_ok || !f.win_ok) return;
        const bool loaded = f.loadFile((tmp.path / "route-probe.js").string());
        LIFT_CHECK(loaded);
        if (!loaded) return;
        g_routeNowMs = 1000;
        f.host->runner().setClockForTest(&routeNowMs);
        f.host->executor().setClockForTest(&routeNowMs);
        f.readings.script = "FIRST.BIN";
        f.readings.pos = 0x10;
        f.pump();  // start
        f.pump();  // load:armed 命中（now 仍是 1000 ⇒ 不超时）⇒ then.start 拉起 then:armed（deadline 1003）
        dumpFail(f);
        LIFT_CHECK(!f.host->failed());
        LIFT_CHECK(!f.host->finished());
        g_routeNowMs = 1010;
        // ★ 同样：喂不匹配的读数点，让 timeout（而不是 fire）成为本拍事件。
        f.readings.script = "OTHER.BIN";
        f.readings.pos = 0x00;
        f.pump();  // then:armed 超时 ⇒ then.onTrigger ⇒ finish(7, …)
        dumpFail(f);
        LIFT_CHECK(f.host->finished());
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->finishCode()), 7);
        LIFT_CHECK(f.host->finishReason().find("then: timeout") != std::string::npos);
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

// ★ 宿主调用暂停的**假宿主**（`plugin/callback-host-pause`）：`ctx.profile.end` 在这里**故意**
//   烧 5 ms 本线程 CPU（照生产 `AotHost::endWindowText` 的实测 2.5–2.9 ms 再留余量）。
//   为什么烧 CPU 而不是 sleep：2 ms 判据用 CPU 钟 ⇒ sleep 根本不会进预算，"暂停生效"就变成
//   恒真断言。忙等用的是 `pluginThreadCpuNs()`（与预算同一把钟）⇒ 这段**必然**落在预算读数里，
//   只有暂停能把它扣掉。
bool probeProfileEndBurn(void* ctx, std::string* text, std::string* /*err*/) {
    ++static_cast<ProfileProbe*>(ctx)->ends;
    if (text != nullptr) text->clear();
    const std::uint64_t t0 = lift::plugin::pluginThreadCpuNs();
    while (lift::plugin::pluginThreadCpuNs() - t0 < 5'000'000ull) {
        // 忙等：故意烧 5 ms CPU（不 sleep —— 见上）。
    }
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

// =====================================================================================
// `plugin/callback-cpu-budget`：2 ms 判据**真的会触发**（CPU 钟）
//   ★ 红了意味着什么：
//     ① JS 体烧 >2 ms CPU 却被放过（预算形同虚设）；或
//     ② 失败原因不是"用户 JS 体超预算"（`interrupted` + `回调超出预算（deadline / op_limit）`；
//        ⛔ 带 `(host)` 就是宿主安全网，不是这条）。
//   ★ **机制断言（不是"失败了"）**：`op_limit = 0`（不限）⇒ 这次打断**只可能**来自 CPU deadline；
//     `overran()` 只由 QJS 中断回调置位 ⇒ 证明"CPU 钟 → 中断 → 回调失败"整条链真的走通。
//     ⛔ 若 CPU 比较坏了：JS 会跑完 5e7 次循环、正常返回 [] ⇒ 本用例红（不是靠数字猜）。
//   ★ `js_cpu_ns >= 2 ms` 是判据窗的读数（⛔ 不断言具体数值）。
// =====================================================================================
void caseCallbackCpuBudget() {
    lift_test::g_current_case = "plugin/callback-cpu-budget";
    Fixture f;
    LIFT_CHECK(f.mem_ok && f.win_ok);
    if (!f.mem_ok || !f.win_ok) return;
    const bool loaded = f.load("cpu-budget.js", {{"mode", "busy"}});
    LIFT_CHECK(loaded);
    if (!loaded) {
        std::fprintf(stderr, "  load error: %s\n", f.load_error.c_str());
        return;
    }
    // 2 ms（生产值）+ **op_limit 不限**：否则 100k 的 op_limit 会先于 CPU deadline 触发 ⇒
    // 这条用例证明的就不是"CPU 钟会触发"，而是"op_limit 会触发"。
    f.host->runner().setBudget(lift::plugin::Budget{2'000'000ull, 0u});
    f.pump();
    dumpFail(f);
    const std::string reason = f.host->failReason();
    LIFT_CHECK(f.host->failed());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->callbacks()), 1);
    // ★ 点名两段文本：QJS 的"被打断" + 回调预算的 hint（⛔ 只断言"失败了"）。
    const bool named = reason.find("interrupted") != std::string::npos &&
                       reason.find("回调超出预算") != std::string::npos;
    if (!named) std::fprintf(stderr, "  reason=%s\n", reason.c_str());
    LIFT_CHECK(named);
    // ★ 不是宿主安全网：那条会带 `(host)`。
    LIFT_CHECK(reason.find("(host)") == std::string::npos);
    // ★ 机制：QJS 中断回调真的按 CPU deadline 打断过（op_limit=0、render deadline=0 ⇒ 唯一来源）。
    LIFT_CHECK(f.host->script() != nullptr && f.host->script()->overran());
    // ★ 判据窗读数：CPU 时间确实越过了 2 ms。
    LIFT_CHECK(lift::plugin::callbackDiag().js_cpu_ns >= 2'000'000ull);
}

// =====================================================================================
// `plugin/callback-js-band`：**2 ms+ 的 JS 本体跑完 ⇒ 仍落在 `js` 档**（暂停不许把用户 JS 也扣掉）
//   ★ 红了意味着什么：
//     ① 一个真烧 >2 ms CPU、**完全没调宿主**的回调被放过（预算形同虚设）；或
//     ② 失败原因不是 `js` 档（`callbackBudgetReason("start", "", false)`）—— 例如带 `(host)`。
//   ★ 为什么这条与 `plugin/callback-cpu-budget` 不是同一条：那条烧 CPU 的**字节码**很多 ⇒ 被
//     QJS 中断回调当场打断（`overran()` 真、reason 带 `interrupted`）⇒ `over=deadline`。这条的
//     负载是**原生内建的大循环**（`Array.join`；字节码只有几十条 ≪ 每 10000 条一次的中断步长）
//     ⇒ 不被打断、正常跑完，判据落在"跑完了但 CPU 超了"的 `js` 档 —— 这正是生产上
//     `scenario:settle:0` 的形状（缺陷 `REQ-01M86VRT1YP53CJS0M885FCCJM` 的现场）。
//   ★ 机制断言：`overran() == false` 证明这次**不是**中断回调判的 ⇒ 走的是 `js_late`。
// =====================================================================================
void caseCallbackJsBand() {
    lift_test::g_current_case = "plugin/callback-js-band";
    Fixture f;
    LIFT_CHECK(f.mem_ok && f.win_ok);
    if (!f.mem_ok || !f.win_ok) return;
    const bool loaded = f.load("cpu-budget.js", {{"mode", "js-band"}});
    LIFT_CHECK(loaded);
    if (!loaded) {
        std::fprintf(stderr, "  load error: %s\n", f.load_error.c_str());
        return;
    }
    f.host->runner().setBudget(lift::plugin::Budget{2'000'000ull, 0u});
    f.pump();
    dumpFail(f);
    LIFT_CHECK(f.host->failed());
    // ★ 没有被打断（否则走的是 `deadline` 档，本用例就没钉住 `js` 档）。
    LIFT_CHECK(f.host->script() != nullptr && !f.host->script()->overran());
    // ★ 判据窗读数：用户 JS 本体的 CPU 确实越过了 2 ms（夹具的负载**没有**任何宿主调用 ⇒
    //   `paused_ns == 0` ⇒ 这个数就是用户 JS 自己的时间）。
    LIFT_CHECK(lift::plugin::callbackDiag().js_cpu_ns >= 2'000'000ull);
    LIFT_CHECK(lift::plugin::callbackDiag().host_pause_ns == 0ull);
    // ★ `js` 档**逐字**：`callbackBudgetReason(call, name, host=false)`（⛔ 不是 `(host)`）。
    const std::string want = lift::plugin::callbackBudgetReason("start", std::string(), /*host=*/false);
    const std::string got = f.host->failReason();
    if (got != want) std::fprintf(stderr, "  reason=%s（期望 %s）\n", got.c_str(), want.c_str());
    LIFT_CHECK(got == want);
}

// =====================================================================================
// `plugin/callback-host-pause`：**宿主调用暂停** —— `ctx.profile.end` 的 CPU 不计入 2 ms
//   ★ 红了意味着什么：宿主能力的成本又算回用户 JS 头上（缺陷 `REQ-01M86VRT1YP53CJS0M885FCCJM`
//     原样复发：每个 `settle` 回调 `js_cpu_us` 2.5–2.9 ms > 2000 ⇒ `run=failed`）。
//   ★ 构造（确定性）：假宿主的 `profile_end` 忙等 **5 ms 本线程 CPU**（> 2 ms 判据、< 20 ms 宿主
//     安全网）⇒ 去掉 `HostPause` 就**必然** `over=js`（⛔ 不是靠"这台机器碰巧慢"）。
//   ★ 三条一起判：
//     ① 回调**不失败**、没被中断；
//     ② 假宿主**真被调到了**（`probe.ends == 1`）且真的烧了 ≥4 ms（`paused_us` 读数）；
//     ③ 判据读的 `js_cpu_ns` 是**扣除后**的（< 2 ms）—— 这才是诊断行 `js_cpu_us` 的真值。
// =====================================================================================
void caseCallbackHostPause() {
    lift_test::g_current_case = "plugin/callback-host-pause";
    Fixture f;
    LIFT_CHECK(f.mem_ok && f.win_ok);
    if (!f.mem_ok || !f.win_ok) return;
    ProfileProbe probe;
    lift::ui::UiHostOps ops{};
    ops.ctx = &probe;
    ops.profile_begin = &probeProfileBegin;
    ops.profile_end = &probeProfileEndBurn;
    const bool loaded = f.load("budget-pause.js", {}, &ops);
    LIFT_CHECK(loaded);
    if (!loaded) {
        std::fprintf(stderr, "  load error: %s\n", f.load_error.c_str());
        return;
    }
    // 生产值 2 ms + op_limit 不限：这条判的**只**是 CPU deadline（⛔ 不让 op_limit 掺一脚）。
    f.host->runner().setBudget(lift::plugin::Budget{2'000'000ull, 0u});
    f.pump();
    dumpFail(f);
    // ① 不失败、没被打断
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK(f.host->script() != nullptr && !f.host->script()->overran());
    // ② 宿主真被调到、真烧了（⛔ 否则"没超"可能只是夹具没调宿主）
    LIFT_CHECK_EQ_I64(probe.ends, 1);
    const lift::plugin::CallbackDiag& d = lift::plugin::callbackDiag();
    LIFT_CHECK(d.host_pause_ns >= 4'000'000ull);
    // ③ 预算窗 = 扣除后的用户 JS 本体
    LIFT_CHECK(d.js_cpu_ns < 2'000'000ull);
}

#ifdef _WIN32
// =====================================================================================
// `plugin/callback-wall-only`：**墙钟 > 2 ms、CPU 远 < 2 ms ⇒ 不许失败**
//   ★ 红了意味着什么：判据退回了墙钟 ⇒ 被 OS 抢占 / 被挂起的回调又被诬告（缺陷
//     REQ-01M7VVDV69MJXRHNDGR50H7VHC 原样复发）。⚠ busy 用例**发现不了**这个错（它墙钟也超）。
//   ★ 构造（确定性，不靠"别的核抢 CPU"的概率）：JS 体写 guest 全局 `SIGNAL=1` 告诉本线程
//     "已在跑"，再自旋等 `FLAG=1`；宿主线程看到 SIGNAL 就 `SuspendThread` **主线程** 6 ms、
//     恢复后写 FLAG ⇒ 墙钟 6 ms（> 2 ms、**< 宿主安全网 20 ms**）、CPU 只有几十 µs。
//   ★ 只在 win32 跑：POSIX 没有可移植的"挂起本线程"（SIGSTOP 是进程级）。
// =====================================================================================
void caseCallbackWallOnly() {
    lift_test::g_current_case = "plugin/callback-wall-only";
    Fixture f;
    LIFT_CHECK(f.mem_ok && f.win_ok);
    if (!f.mem_ok || !f.win_ok) return;
    const bool loaded = f.load("cpu-budget.js", {{"mode", "stall"}});
    LIFT_CHECK(loaded);
    if (!loaded) {
        std::fprintf(stderr, "  load error: %s\n", f.load_error.c_str());
        return;
    }
    // 两个握手槽的**宿主视图**（主线程建夹具时就已提交；`host()` 返回稳定基址）。
    auto* signal_slot =
        reinterpret_cast<volatile std::uint32_t*>(f.mem.host(intAddr(kCpuBudgetSignalId)));
    auto* flag_slot =
        reinterpret_cast<volatile std::uint32_t*>(f.mem.host(intAddr(kCpuBudgetFlagId)));
    LIFT_CHECK(signal_slot != nullptr && flag_slot != nullptr);
    if (signal_slot == nullptr || flag_slot == nullptr) return;
    *flag_slot = 0;  // 池是零填充；显式置 0（防同一个池被别的用例写过）
    const std::uint32_t want = lift::encodeInt(1u, kFxKey);
    const DWORD main_tid = ::GetCurrentThreadId();
    std::atomic<bool> helper_ready{false};
    std::thread helper([&] {
        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_HIGHEST);  // 响应延迟要 µs 级
        const auto t0 = std::chrono::steady_clock::now();
        helper_ready.store(true);  // ★ 主线程等这一位：见下面的握手
        while (*signal_slot != want && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5)) {
            ::SwitchToThread();
        }
        HANDLE self = ::OpenThread(THREAD_SUSPEND_RESUME, FALSE, main_tid);
        if (self != nullptr) {
            // ★ 主线程此刻在 JS 体里：挂起 6 ms ⇒ 只走墙钟，CPU 不涨。
            //   ⚠ 必须 **< 宿主安全网 20 ms**：这条判的是"2 ms 判据用哪把钟"，
            //   超了 20 ms 就会先被安全网抓到（那是另一条腿，见 trigger_runner）。
            //   ⚠ **不用 `Sleep`**：它的粒度是系统 tick（本机 15.625 ms、还可能成倍）⇒
            //   想睡 6 ms 会真睡 15–31 ms、撞上 20 ms 安全网。用忙等把它钉准。
            ::SuspendThread(self);
            const auto hold = std::chrono::steady_clock::now();
            while (std::chrono::steady_clock::now() - hold < std::chrono::milliseconds(6)) {
                ::SwitchToThread();
            }
            ::ResumeThread(self);
            ::CloseHandle(self);
        }
        *flag_slot = want;  // 放行 JS（无论挂起成功与否 —— 失败时用例会因为"墙钟没超"而红）
    });
    // ★ **握手**：等 helper 真的在自旋之后才进回调。否则 `std::thread` 的启动延迟会被算成
    //   JS 体的 CPU 忙等（实测负载下 helper 晚起 1.3–3.5 ms ⇒ `js_cpu_ns` 假超 2 ms）。
    for (int i = 0; i < 100000 && !helper_ready.load(); ++i) {
        ::SwitchToThread();
    }
    LIFT_CHECK(helper_ready.load());
    f.host->runner().setBudget(lift::plugin::Budget{2'000'000ull, 0u});
    f.pump();
    helper.join();
    dumpFail(f);
    const lift::plugin::CallbackDiag& d = lift::plugin::callbackDiag();
    // ★ 墙钟确实超过 2 ms（挂起真的发生了）。
    LIFT_CHECK(d.js_ns >= 2'000'000ull);
    // ★ CPU **没有**超过 2 ms，而且远小于墙钟（判据读的是 CPU 钟）。
    LIFT_CHECK(d.js_cpu_ns < 2'000'000ull);
    LIFT_CHECK(d.js_cpu_ns * 4 < d.js_ns);
    // ★ 结论：不许因为"墙钟超了"失败 / 被中断。
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK(f.host->script() != nullptr && !f.host->script()->overran());
}
#endif  // _WIN32

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
    caseSaveMissingPrecheck();
    caseConfigRoundtrip();
    caseConfigHostPause();
    caseLoadSaveOnLoaded();
    caseChainsRoute();
    caseGlobalReadModifyWrite();
    caseCallbackCpuBudget();
    caseCallbackJsBand();
    caseCallbackHostPause();
#ifdef _WIN32
    caseCallbackWallOnly();
#endif
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
