// emulator-lift · **C2a** 的 pure 判据（QJS 单插件：loader / start·onTrigger / ctx 基础 / 读点顺序）
//
// 判据（红了意味着什么）——逐条按合同 `docs/03-emulator/plugin-runtime.md` §5 C2 的用例名：
//   · `plugin/api-surface`                ：`RunCtx` 的成员与 d.ts 一致；**多一个/少一个都红**
//                                            （`draw`/`run`/`dispatch`/`sample`/`screenshot`/
//                                            `read.frameHash` 必须**缺席**）。D-C2-6 / D-C2-7。
//   · `plugin/initial-state`              ：`initialState` 在 `start` 之前注入；无 `initialState` ⇒ `{}`；
//                                            `ctx.state` 是 per-Run 的（同一 Run 的第二个回调看得到）。
//   · `plugin/concurrent-arm`             ：一读点最多一个（注册顺序）；返回数组**完整替换**；`[]`=done。
//   · `plugin/module-default`（+ start-only / onTrigger-only）：只读 `export default`；default 不是
//                                            对象 ⇒ **装载失败并点名**；`start` **可单独存在**
//                                            （S2b：v2 用 ctx.task / ctx.once 驱动）；`onTrigger`
//                                            单独存在 ⇒ 装载失败（没有 start 就永远没人调它）。
//   · `plugin/ts-module`（S1b）           ：入口 `.ts` 经 `initFromFile`（路径形式）+ `lib/helper.ts`
//                                            运行期 import（宿主按扩展名加 `JS_EVAL_FLAG_TYPESCRIPT`）；
//                                            click 坐标 = helper 的**运行期**导出（⛔ 不是"没报错"）。
//   · `plugin/js-comparison-unchanged`（S1b）：`.js` 的 `f<a>(b)` 仍是比较（⛔ 不按泛型调用）。
//   · `plugin/ts-uppercase-ext`（S1b）     ：扩展名判据 ASCII 大小写不敏感（`.TS` / `.MTS`）。
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
// S2a（`runtime/instr_hook` 的反向 sink + `handle.onTick` + 每入口 drain）：
//   · `plugin/point-hit`                  ：假 marker / 读点上 `handle.onTick(fn)` 真的跑；tick 的
//                                            `{id,script,pos,opcode}` 与 runtime 的 `InstrHit` 逐项一致
//                                            （由 JS 写进 guest int、C++ 用 `readGlobal` 读回）；
//                                            `stats().hits` 随探针次数涨（1 → 2）。
//   · `plugin/point-hit-handler-error`    ：handler 抛错 ⇒ **一行** error（带 `hit #N`）+ handler 摘除
//                                            + Run **不 fail**；第二次命中不再跑 JS，但 entry 仍计数。
//   · `plugin/point-hit-after-run-end`    ：Run 收口（`return []`）后 hook 表空；再探针**不再跑 JS**。
//   · `plugin/drain-per-entry`            ：`call()` 与 `runInstrHits()` 的 continuation 都在**本入口**
//                                            跑完（start 的 phase=2 由下一个读点写出；tick 的 log 在
//                                            探针返回前出现）—— 漏写 drain ⇒ continuation 落到下一个入口。
//   · `plugin/drain-budget`               ：`while(true) await Promise.resolve()` 被**同一预算窗**打断
//                                            （Run fail + `overran()` + reason 含 `interrupted` /
//                                            `回调超出预算`；`pump()` 会返回 = guest 没被卡死）。
//
// C2b（render / ui / draw；`plugin-runtime` 的 pure 半 —— 窗口 `adopt` 是 C2b-2）：
//   · `plugin/render-surface`             ：`RenderCtx` 的成员表 = d.ts 的 RenderCtx **减 `run`**
//                                            （S2 起 `ui/draw/read/game/params/log/profile`；
//                                            **2026-11-26 加回 `config`** ⇒ 八成员；`Object.keys`
//                                            精确比对）；⛔ `edit`/`input`/`state`/`now`/…；
//                                            `config` 形状 + 临时 appdata 的真桥可读可写（回执
//                                            `{applied}`）；`read.frameHash` 缺席；S2 的 `game`
//                                            getter 无源 ⇒ undefined；没有 `render` 的插件调
//                                            render ⇒ true + 空 list。
//   · `plugin/ui-draw`                    ：`ctx.draw.rect/text/measure` 的元素字段（x/y/w/h/fill/
//                                            字号/颜色；S2 加 `style.key` / `style.bold`）、z 序 = 调用顺序、
//                                            measure `{width,height}`（与宿主 `measureText` 交叉核对）；
//                                            `RenderCtx.ui` 可写。
//   · `plugin/ui-draw-bad-style`          ：`stroke`/`width`/`alpha`/`align`/`text.key` ⇒ **TypeError**
//                                            （D-C2-3），⛔ 不静默忽略。
//   · `plugin/ui-draw-measure-no-font`    ：没有 `FontEngine` ⇒ measure 抛 TypeError（⛔ 不返回 0 尺寸）。
//   · `plugin/ui-state`                   ：`onTrigger` 只 `ctx.ui.show/hide`（⛔ 回调里没有 draw）；
//                                            本帧 display list 为空；**下一次 render** 才可见；hide 后为空。
//   · `plugin/render-read`                ：render 里的 `ctx.read.global.get` / `ctx.log` 有宿主来路
//                                            （`setCallHost` + `readWired` 不绑回调；⛔ 静默缺失）。
//   · `plugin/render-readings`            ：`read.frames` / `read.scriptPos` / `game.localFrames` /
//                                            `game.mouse` 的字段与"没有源 ⇒ undefined / 缺 pos 键"；
//                                            C2b-2：**有**源时 `mouse().buttons` 三键 = 真实电平。
//   · `plugin/render-profile`（S2）        ：`ctx.profile.begin` 在 **render** 里调到宿主、返回
//                                            `undefined`；`ctx.profile.end()` 回与旧 `host.profileEnd()`
//                                            同形的 `{ok,text,error}`（失败也是结果对象）。
//   · `plugin/hud-equivalence`（S2）       ：`scripts/ui/hud.js` 的**旧 loader** 与**插件 loader**
//                                            在同一份 readings 下 display list **逐字段相同**
//                                            （含 PROFILE 点击两拍，第二拍带 `INVALID` 文本 ⇒ 四态）。
//   · `plugin/ui-draw-measure-bold`（S2）  ：`ctx.draw.measure` 的 `bold` 真参与度量（与宿主
//                                            `measureText(…, bold)` 交叉核对；⛔ 静默按非粗体量）。
//   · `plugin/render-stored-runctx-rejected`：把 **RunCtx 存下来**在 render 里用 `fail`/`finish`/
//                                            `quit`/`profile.*` ⇒ TypeError（复审 F2：成员表不是门面；
//                                            S2 起 `RenderCtx` **自己**有 profile，但存下来的 RunCtx 仍被拒）。
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
#include "runtime/instr_hook.h"     // S0：Run 收口之后 `instrHookList()` 必须为空
#include "runtime/memory.h"
#include "runtime/script_globals.h"
#include "runtime/value_codec.h"  // encodeInt（ENC 往返的断言；⛔ 不写死编码后的数）
#include "runtime/x86.h"          // S2a：`instrHookProbe(X86&)` 的假 CPU（只填 mem）
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
    // ★ 路径形式走 `PluginHost::initFromFile`（与生产的 `--plugin <路径>` 同一条；⛔ 不要测试
    //   自己读源码再 `init` —— 那会漏掉 initFromFile 的读文件 / 日志归属面）。
    bool loadFile(const std::string& path, const std::map<std::string, std::string>& params = {},
                  const lift::ui::UiHostOps* host_ops = nullptr) {
        host = std::make_unique<lift::plugin::PluginHost>(readings);
        // ★ 预算（D5 的 2 ms / 100k 是**运行时口径**，不是本用例的判据）：这里放宽到
        //   500 ms / 1e7 ops —— 只为让"QJS 冷启动（第一次 JS_Call + 若干个对象分配）"不把
        //   用例变成随机红。⛔ 生产值仍只有一份（`runtime/budget.h`；`PluginHost` 读的就是它）。
        host->runner().setBudget(lift::plugin::Budget{500'000'000ull, 10'000'000u});
        // ★ 装配顺序与 `aot_main` **一致**：`init`（编译 + 沙箱）→ `attachMemory`（Edit/Read）。
        //   `host_ops` 只在需要判"profile 桥有没有被调到"的用例里给（`plugin/render-stored-runctx-rejected`）。
        if (!host->initFromFile(path, params, host_ops, &load_error)) return false;
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

// =====================================================================================
// S2a · 指令点探针的**假帧**（`runtime/instr_hook.cpp` 的偏移；零 AOT / 零安装）
//
//   `instrHookProbe` 只读 `frame+0x{00,04,08,14,20,2C,3C}` 与 FileDB 根条目；本夹具把
//   "一条脚本指令"摆成：body=0x4E0000、pc 指向 `pos=0x100`、opcode=114、脚本名 FIELD.BIN。
//   ★ 地址都在 `[0x480000,0x580000)`（`makeFakeEngine` 已全量 commit）且避开 int 池
//     （0x4A0000 + 0xB1B6×4 = 0x4CC6D8）、帧记录（0x4DD894）与 int/ptr 池（0x4D8000/0x4D9000）。
// =====================================================================================
constexpr std::uint32_t kInstrBody = 0x004E0000u;      // 脚本体（在 int 池之外）
constexpr std::uint32_t kInstrName = 0x004F0000u;      // 名字表（80 字节/条）
constexpr std::uint32_t kInstrIntBase = 0x004D8000u;   // local-int 池（S2a 只填值、不消费）
constexpr std::uint32_t kInstrPtrBase = 0x004D9000u;   // local-ptr 槽（同上）
constexpr std::uint32_t kInstrPos = 0x00000100u;       // label 坐标（与夹具 `POS` 同一个数）
constexpr std::uint32_t kInstrOpcode = 114u;           // 只是**值**（⛔ 不是 opcode 语义结论）
constexpr std::uint32_t kInstrScriptId = 3u;
// ★ S2c-2（`where` 的 script 过滤）：第二个脚本身份（名字表里 id 4 那一条）。
//   `{script}` / `{script,opcode}` 的**负控制**要"换脚本 ⇒ 不命中"，换 id 才看得见
//   （同名改 id 不够：命中后 `Entry::script_id` 已缓存，只比 id）。
constexpr std::uint32_t kInstrOtherId = 4u;
constexpr std::uint32_t kInstrPc = kInstrBody + (kInstrPos - lift::kScriptHeaderLen);
// ★ S2c（`tick.local`）：local-ptr 0 指向的 dword（`ptr(0)` = 这个地址、`ptrDword(0)` = 它的语义值）。
constexpr std::uint32_t kInstrLocalTarget = 0x004D7000u;

bool installInstrFrame(lift::GuestMemory& mem) {
    const std::uint32_t frame = kFxEngine + lift::kEngineFrameBase;  // = 0x4DD894
    if (!put32(mem, kFxEngine + lift::kEngineCurOffset, 0u)) return false;  // cur = 0
    if (!put32(mem, frame + 0x00, kInstrBody)) return false;               // body
    if (!put32(mem, frame + 0x04, kInstrPc)) return false;                 // pc
    if (!put32(mem, frame + 0x08, 8u)) return false;                       // int count
    if (!put32(mem, frame + 0x20, kInstrIntBase)) return false;            // int base
    if (!put32(mem, frame + 0x14, 2u)) return false;                       // ptr count
    if (!put32(mem, frame + 0x2C, kInstrPtrBase)) return false;            // ptr base
    if (!put32(mem, frame + 0x3C, kInstrScriptId)) return false;           // 统一文件 id
    // FileDB 根条目（`engine + kFileIndexOffset` + 0x40C/0x414）与名字条目（80 字节，NUL 结尾）。
    const std::uint32_t db = kFxEngine + lift::kFileIndexOffset;
    if (!put32(mem, db + lift::kFileRootCountOffset, 8u)) return false;
    if (!put32(mem, db + lift::kFileRootNameBaseOffset, kInstrName)) return false;
    const std::uint32_t entry = kInstrName + lift::kFileNameStride * kInstrScriptId;
    const char kName[] = "FIELD.BIN";
    for (std::size_t i = 0; i <= sizeof(kName) - 1; ++i) {
        mem.wr8(entry + static_cast<std::uint32_t>(i), static_cast<std::uint8_t>(kName[i]));
    }
    // ★ S2c-2：第二个脚本名（id 4）—— `where` 的 script 过滤负控制用。
    const std::uint32_t other = kInstrName + lift::kFileNameStride * kInstrOtherId;
    const char kOtherName[] = "OTHER.BIN";
    for (std::size_t i = 0; i <= sizeof(kOtherName) - 1; ++i) {
        mem.wr8(other + static_cast<std::uint32_t>(i), static_cast<std::uint8_t>(kOtherName[i]));
    }
    // opcode 摆在 PC 上（`instrHookProbe` 用 `mem.rd32(pc)` 读）。
    return put32(mem, kInstrPc, kInstrOpcode);
}

// 一次"脚本派发"（= AOT 注入的 `lift_instr_hook`）：摆 PC + 直接调探针。
void dispatchInstr(Fixture& f, std::uint32_t pos) {
    const std::uint32_t frame = kFxEngine + lift::kEngineFrameBase;
    LIFT_CHECK(put32(f.mem, frame + 0x04, kInstrBody + (pos - lift::kScriptHeaderLen)));
    lift::X86 cpu;  // 《只填 mem》—— 与 `tests/instr_hook.cpp` 同形
    cpu.mem = &f.mem;
    lift::instrHookProbe(cpu);
}

// ★ S2c-2：换脚本身份（帧 +0x3C 的 file id）—— `{script}` / `{script,opcode}` 的负控制。
bool setInstrScript(Fixture& f, std::uint32_t script_id) {
    const std::uint32_t frame = kFxEngine + lift::kEngineFrameBase;
    return put32(f.mem, frame + 0x3C, script_id);
}

// ★ S2c-2：把 opcode 摆到 `pos` 对应的 PC 上（`instrHookProbe` 读 `mem.rd32(pc)`）。
bool putInstrOpcode(Fixture& f, std::uint32_t pos, std::uint32_t opcode) {
    return put32(f.mem, kInstrBody + (pos - lift::kScriptHeaderLen), opcode);
}

// ★ S2c（`tick.local`）：在 `installInstrFrame` **之后**补值（⛔ 不改夹具本身）：
//   int 槽 0..7 = 语义 7 / 0 / … / 0xFFFFFFFF（int32 口径 = -1）；ptr 0 → kInstrLocalTarget、
//   ptr 1 = 0（指针 0）；kInstrLocalTarget 里放语义 9。int_count / ptr_count 已由上面摆好（8 / 2）。
bool installInstrLocalValues(lift::GuestMemory& mem) {
    const std::uint32_t ints[8] = {7u, 0u, 0u, 0u, 0u, 0u, 0u, 0xFFFFFFFFu};
    for (std::uint32_t i = 0; i < 8u; ++i) {
        if (!put32(mem, kInstrIntBase + 4u * i, lift::encodeInt(ints[i], kFxKey))) return false;
    }
    if (!put32(mem, kInstrPtrBase, kInstrLocalTarget)) return false;       // ptr 0
    if (!put32(mem, kInstrPtrBase + 4u, 0u)) return false;                 // ptr 1 = 0
    return put32(mem, kInstrLocalTarget, lift::encodeInt(9u, kFxKey));      // ptrDword(0) = 9
}

std::uint64_t instrHits(std::uint32_t handle) {
    lift::InstrHookStatus st;
    if (!lift::instrHookStats(handle, &st)) return 0;
    return st.hits;
}

// ★ S2c-2b（`tick.patch`）：local-int `index` 的**语义值**（`installInstrLocalValues` 摆的那 8 个槽）。
std::uint32_t readInstrLocalInt(Fixture& f, std::uint32_t index) {
    return lift::decodeInt(f.mem.rd32(kInstrIntBase + 4u * index), kFxKey);
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

// =====================================================================================
// `plugin/field-switch-apply`：`field-point-visible.js` 的**施加面**（纯 QJS + 假 guest 内存）
//
// 判据（红了意味着什么）：
//   ① 唯一钩子 = **`DRAWOBJ.BIN` 的入口窗口** `[0x003C,0x06BC)`（`fp:draw`；全语料 4 处
//      `call-script 5117` 的唯一入口）⇒ 位置读数落在窗口内、且从"外"进来 ⇒ fire；
//   ② 施加面 = **只补 `50ff9f[slot]` 的 bit0**（`flags | 1`），且只为**类型命中**的槽：
//      `0x2→0x3`（补 bit0）· `0x3→0x3`（不写）· `0x5→0x5`（不写：bit2 是引擎的动效位）·
//      `0x6→0x7`（补 bit0）· `0x7→0x7`（不写）；类型不命中的槽一个字节都不动。
//      ★ **bit1 / bit2 必须原样**：引擎每渲染帧 `SETOBJ` #2 `fill-zero 50ff9f 32` 后自己重算
//      （已触发 = 动效、未触发 = 静态）⇒ 插件写它们就是替引擎决定"这个点触发过没有"
//      （旧实现 `|=0x5` 的错就在这里：未采集的点也放动效）；
//   ③ 已经 fire 过且**没离开窗口** ⇒ 不再 fire（Reenter 不是每读点）⇒ **不再写**（日志行数不涨）；
//   ④ 模拟"下一渲染帧的 SETOBJ 把派生位重算回 `0x2`/`0x6`"：读到有效"外"、再回到窗口 ⇒ 重新补位；
//      `matched/patched` 没变 ⇒ **不再刷日志**；
//   ⑤ `on=0` ⇒ 一个 guest global 都不读、都不写（同一套假数据上标志原样），且 Run **常驻**（⛔ 不 finish）。
//   ★ 档位 = **pure**：假 guest 内存 + 注入读数源 + 临时 appdata 里的 `plugin-config.json`；
//     零安装树 / 零 GPU / 零子进程。
//   ★ true 值只钉"派生位被改成了什么"：不钉游戏业务（点位真的画出来归 FIELD 复现）。
// =====================================================================================

// `50ff9f` 的 32 槽派生位 / `50ffd1` 的 32 槽类型（数值的唯一出处在
// `scripts/plugins/lib/guest-globals.js`；本用例是它的**消费者**，所以在这里重复一组只读常量）。
constexpr std::uint32_t kFieldFlagsId = 0x50ff9Fu;
constexpr std::uint32_t kFieldTypeId = 0x50ffD1u;
constexpr std::uint32_t kFieldMapId = 0xB222u;        // 当前地图号（只读；日志用）
constexpr std::uint32_t kFieldDrawBit = 0x1u;         // 置位 ⇒ 画（`DRAWOBJ` #7：bit0=0 才跳过）
constexpr std::uint32_t kFieldTypeGather = 0x1Fu;     // 採集
constexpr std::uint32_t kFieldTypeDig = 0x20u;        // 発掘

// 假 guest 的 int 池够不到 `0x50ff9f`（夹具的池 = 0xB1B6 个槽）⇒ 本用例自己提交一块**够大**的池
// 并把 Engine 的 int 基址/计数指过去（`readGlobal` / `writeGlobal` 每次现读这两个字段 ⇒ 立即生效）。
constexpr std::uint32_t kFieldPoolBase = 0x10000000u;
constexpr std::uint32_t kFieldPoolCount = 0x510000u;  // > 0x50ff9f + 0x20

bool installFieldPool(Fixture& f) {
    if (!f.mem.commitZeroed(kFieldPoolBase, static_cast<std::size_t>(kFieldPoolCount) * 4u,
                            lift::RegionKind::Heap, "plugin-field-switch-globals")) {
        return false;
    }
    if (!put32(f.mem, kFxEngine + lift::kScriptGlobalIntBaseOffset, kFieldPoolBase)) return false;
    return put32(f.mem, kFxEngine + lift::kScriptGlobalIntCountOffset, kFieldPoolCount);
}

bool seedFieldInt(Fixture& f, std::uint32_t id, std::uint32_t value) {
    lift::GlobalValue v;
    v.kind = lift::GlobalKind::Int;
    v.u32 = value;
    lift::GlobalError err = lift::GlobalError::None;
    const bool ok = lift::writeGlobal(f.mem, id, lift::GlobalKind::Int, v, &err);
    LIFT_CHECK(err == lift::GlobalError::None);
    return ok;
}

std::uint32_t readFieldInt(Fixture& f, std::uint32_t id) {
    lift::GlobalValue v;
    lift::GlobalError err = lift::GlobalError::None;
    LIFT_CHECK(lift::readGlobal(f.mem, id, lift::GlobalKind::Int, &v, &err));
    LIFT_CHECK(err == lift::GlobalError::None);
    return v.u32;
}

// 同一套初始数据：**五种派生位形态**（bit0 / bit1 / bit2 的组合）+ 一个类型不命中的槽。
// ★ `0x5` 与 `0x3` 取自真机读数（2026-11-26 / 存档 012：slot3 `0x5` = 已触发 ⇒ 动效；
//   slot4/5/6 `0x3` = 未采集 ⇒ 静态）；`0x2` / `0x6` / `0x7` 是把 bit0 抽掉 / 全置的边界。
bool seedFieldScene(Fixture& f) {
    if (!seedFieldInt(f, kFieldMapId, 0x11u)) return false;
    if (!seedFieldInt(f, kFieldTypeId + 1u, kFieldTypeGather)) return false;
    if (!seedFieldInt(f, kFieldTypeId + 2u, kFieldTypeDig)) return false;
    if (!seedFieldInt(f, kFieldTypeId + 3u, kFieldTypeGather)) return false;
    if (!seedFieldInt(f, kFieldTypeId + 4u, kFieldTypeDig)) return false;
    if (!seedFieldInt(f, kFieldTypeId + 5u, kFieldTypeGather)) return false;
    if (!seedFieldInt(f, kFieldTypeId + 6u, 0x03u)) return false;  // 不命中（⛔ 不是点位类型）
    // 派生位：SETOBJ 每帧 `fill-zero` 后重算出来的形态（本用例把它们当"当帧输入"）。
    if (!seedFieldInt(f, kFieldFlagsId + 1u, 0x2u)) return false;  // bit0=0 ⇒ 补成 0x3
    if (!seedFieldInt(f, kFieldFlagsId + 2u, 0x3u)) return false;  // 未采集（静态）⇒ 不写
    if (!seedFieldInt(f, kFieldFlagsId + 3u, 0x5u)) return false;  // 已触发（动效）⇒ ⛔ 不许变成 0x7
    if (!seedFieldInt(f, kFieldFlagsId + 4u, 0x6u)) return false;  // bit0=0 + bit1 + bit2 ⇒ 补成 0x7
    if (!seedFieldInt(f, kFieldFlagsId + 5u, 0x7u)) return false;  // 全置 ⇒ 不写
    return seedFieldInt(f, kFieldFlagsId + 6u, 0u);                // 类型不命中 ⇒ 一个字节都不动
}

void caseFieldSwitchApply() {
    // ---- ① on=1：`DRAWOBJ` 入口（reenter）⇒ 只补 bit0 / 同窗口不重复 fire / 下一帧重新补 ----
    lift_test::g_current_case = "plugin/field-switch-apply";
    {
        Fixture f;
        LIFT_CHECK(f.mem_ok && f.win_ok);
        if (!f.mem_ok || !f.win_ok) return;
        LIFT_CHECK(installFieldPool(f));
        ScopedTempDir appdata;
        LIFT_CHECK(appdata.make("lift-plugin-field-switch-apply"));
        if (appdata.path.empty()) return;
        // 旧配置里残留的 `poll` / `static` = **无害忽略**（本轮删了 `static` 这条语义，键不读了）。
        LIFT_CHECK(writeHostFile(appdata.path / "plugin-config.json",
                                 R"({"version":1,"domains":{"field.point-visible":{"on":1,"poll":0,"static":1}}})"));
        const bool loaded = f.loadFile(pluginRootPath("field-point-visible.js"));
        LIFT_CHECK(loaded);
        if (!loaded) {
            std::fprintf(stderr, "  load error: %s\n", f.load_error.c_str());
            return;
        }
        LIFT_CHECK(f.attachAppData(hostPathText(appdata.path)));
        LIFT_CHECK(f.attachConfig());
        LIFT_CHECK(seedFieldScene(f));

        // 唯一的触发点 = `DRAWOBJ.BIN` 入口窗口（读数由宿主采样给；区间在 `script-positions.js`）。
        // ★ trigger 的 `script` 是**精确 `==`** 的宿主 marker 拼写（带 `.BIN`）。
        f.readings.script = "DRAWOBJ.BIN";
        f.readings.pos = 0x0064;  // DRAWOBJ_ENTRY [0x003C,0x06BC) 内（实测读点 pos）

        // ① 第一拍 = `start`：只 arm 一条位置触发，一个槽都不写。
        f.pump();
        dumpFail(f);
        LIFT_CHECK(!f.host->failed());
        LIFT_CHECK_EQ_U32(readFieldInt(f, kFieldFlagsId + 1u), 0x2u);
        LIFT_CHECK_EQ_U32(readFieldInt(f, kFieldFlagsId + 2u), 0x3u);
        LIFT_CHECK_EQ_U32(readFieldInt(f, kFieldFlagsId + 3u), 0x5u);
        LIFT_CHECK_EQ_U32(readFieldInt(f, kFieldFlagsId + 4u), 0x6u);
        LIFT_CHECK_EQ_U32(readFieldInt(f, kFieldFlagsId + 5u), 0x7u);
        // ② 第二拍 = 有效"外→内"边沿 ⇒ fire ⇒ **只补 bit0**（`0x2→0x3` / `0x6→0x7`）；
        //    已经带 bit0 的槽一个字节都不写（`0x3` / `0x5` / `0x7` 原样）⇒ bit1 / bit2 不动。
        f.pump();
        dumpFail(f);
        LIFT_CHECK(!f.host->failed());
        LIFT_CHECK_EQ_U32(readFieldInt(f, kFieldFlagsId + 1u), 0x3u);
        LIFT_CHECK_EQ_U32(readFieldInt(f, kFieldFlagsId + 2u), 0x3u);
        LIFT_CHECK_EQ_U32(readFieldInt(f, kFieldFlagsId + 3u), 0x5u);
        LIFT_CHECK_EQ_U32(readFieldInt(f, kFieldFlagsId + 4u), 0x7u);
        LIFT_CHECK_EQ_U32(readFieldInt(f, kFieldFlagsId + 5u), 0x7u);
        LIFT_CHECK_EQ_U32(readFieldInt(f, kFieldFlagsId + 6u), 0u);
        const std::uint64_t logs_draw = f.host->logLines();
        LIFT_CHECK(logs_draw >= 2u);  // 首行 `on=… types=…` + `field-points: draw matched=5 patched=2`
        // ③ 同一读点再拍：`fp:draw` 的 Reenter 也不每读点 fire ⇒ 日志不涨。
        f.pump();
        dumpFail(f);
        LIFT_CHECK_EQ_U32(readFieldInt(f, kFieldFlagsId + 1u), 0x3u);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->logLines()),
                          static_cast<std::int64_t>(logs_draw));

        // ④ 下一渲染帧：`SETOBJ` 又把派生位重算回 `0x2` / `0x6`；先读到有效"外"、再回到入口窗口
        //    ⇒ 重新补 bit0。`matched/patched` 没变 ⇒ ⛔ 不再刷一行日志（每帧 fire，但只在状态变化时打）。
        LIFT_CHECK(seedFieldInt(f, kFieldFlagsId + 1u, 0x2u));
        LIFT_CHECK(seedFieldInt(f, kFieldFlagsId + 4u, 0x6u));
        f.readings.script = "FIELD.BIN";
        f.readings.pos = 0x0600;  // 有效"外"（不在任何窗口里）：只写边沿，不回调
        f.pump();
        dumpFail(f);
        LIFT_CHECK(!f.host->failed());
        f.readings.script = "DRAWOBJ.BIN";
        f.readings.pos = 0x0064;
        f.pump();
        dumpFail(f);
        LIFT_CHECK(!f.host->failed());
        LIFT_CHECK_EQ_U32(readFieldInt(f, kFieldFlagsId + 1u), 0x3u);
        LIFT_CHECK_EQ_U32(readFieldInt(f, kFieldFlagsId + 4u), 0x7u);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->logLines()),
                          static_cast<std::int64_t>(logs_draw));

        // ⑤ 常驻：没人收口（⛔ 不 finish / 不 done / 不 fail）。
        LIFT_CHECK(!f.host->finished());
        LIFT_CHECK(!f.host->failed());
        LIFT_CHECK(!f.host->runner().done());
        LIFT_CHECK(f.host->callbacks() >= 2u);
    }

    // ---- ② on=0：一个 guest global 都不读 / 都不写，Run 照样常驻 ----
    lift_test::g_current_case = "plugin/field-switch-off-noop";
    {
        Fixture f;
        LIFT_CHECK(f.mem_ok && f.win_ok);
        if (!f.mem_ok || !f.win_ok) return;
        LIFT_CHECK(installFieldPool(f));
        ScopedTempDir appdata;
        LIFT_CHECK(appdata.make("lift-plugin-field-switch-off"));
        if (appdata.path.empty()) return;
        LIFT_CHECK(writeHostFile(appdata.path / "plugin-config.json",
                                 R"({"version":1,"domains":{"field.point-visible":{"on":0,"poll":0}}})"));
        const bool loaded = f.loadFile(pluginRootPath("field-point-visible.js"));
        LIFT_CHECK(loaded);
        if (!loaded) {
            std::fprintf(stderr, "  load error: %s\n", f.load_error.c_str());
            return;
        }
        LIFT_CHECK(f.attachAppData(hostPathText(appdata.path)));
        LIFT_CHECK(f.attachConfig());
        LIFT_CHECK(seedFieldScene(f));
        // `fp:draw` 每帧仍 fire，但 `on=0` ⇒ 一个 guest 读都不做（标志逐槽原样）。
        f.readings.script = "DRAWOBJ.BIN";
        f.readings.pos = 0x0064;
        for (int i = 0; i < 3; ++i) f.pump();
        dumpFail(f);
        LIFT_CHECK(!f.host->failed());
        LIFT_CHECK_EQ_U32(readFieldInt(f, kFieldFlagsId + 1u), 0x2u);
        LIFT_CHECK_EQ_U32(readFieldInt(f, kFieldFlagsId + 2u), 0x3u);
        LIFT_CHECK_EQ_U32(readFieldInt(f, kFieldFlagsId + 3u), 0x5u);
        LIFT_CHECK_EQ_U32(readFieldInt(f, kFieldFlagsId + 4u), 0x6u);
        LIFT_CHECK_EQ_U32(readFieldInt(f, kFieldFlagsId + 5u), 0x7u);
        LIFT_CHECK_EQ_U32(readFieldInt(f, kFieldFlagsId + 6u), 0u);
        LIFT_CHECK(!f.host->finished());
        LIFT_CHECK(!f.host->runner().done());
        LIFT_CHECK(f.host->callbacks() >= 1u);
        // ★ 首行日志明写 on=0（`host.logLines()` 只有计数；文本面由 `plugin-field-switch` 的 e2e 判）。
    }
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
    lift_test::g_current_case = "plugin/module-start-only";
    {
        // ★ S2b（v2 兼容层第一处放宽）：`start` 单独存在（没有 `onTrigger`）⇒ **合法**。
        Fixture f;
        LIFT_CHECK(f.mem_ok && f.win_ok);
        const bool loaded = f.load("start-only.js");
        LIFT_CHECK(loaded);
        if (loaded) {
            LIFT_CHECK(f.host->script() != nullptr && f.host->script()->hasRun());
            LIFT_CHECK(f.host->script() != nullptr && !f.host->script()->hasRender());
            f.pump();
            dumpFail(f);
            LIFT_CHECK(!f.host->failed());
            LIFT_CHECK(f.host->runner().done());
            LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->callbacks()), 1);
            LIFT_CHECK(f.host->readout().find("run=done") != std::string::npos);
        }
    }
    lift_test::g_current_case = "plugin/module-onTrigger-only";
    {
        // 反方向仍拒绝：`onTrigger` 单独存在（没有 start 就永远没人调它）⇒ 装载失败并点名。
        Fixture f;
        LIFT_CHECK(!f.load("bad-pair.js"));
        LIFT_CHECK(f.load_error.find("onTrigger") != std::string::npos);
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
//   ★ 2026-11-26：`config` 加回 RenderCtx（7→8）。夹具要判"可读可写、回执 {applied}" ⇒
//     给真 `PluginHost` 桥 + 临时 appdata（与 `control-script/config-roundtrip` 同一个来路），
//     并把 `readings.host` 设成 `UiHostOps`（生产 = `AotHost::uiHostOps()` + `setConfigBridge`）。
//   ★ 附带钉住"没有 render 的插件调 render ⇒ true + 空 list"（present_sdl 只在挂 overlay 时调它）。
// =====================================================================================
void caseRenderSurface() {
    lift_test::g_current_case = "plugin/render-surface";
    Fixture f;
    LIFT_CHECK(f.mem_ok && f.win_ok);
    if (!f.mem_ok || !f.win_ok) return;
    ScopedTempDir appdata;
    LIFT_CHECK(appdata.make("lift-plugin-render-surface"));
    if (appdata.path.empty()) return;
    const bool loaded = f.load("render-surface.js", {{"probe", "render-surface"}});
    LIFT_CHECK(loaded);
    if (!loaded) {
        std::fprintf(stderr, "  load error: %s\n", f.load_error.c_str());
        return;
    }
    // 桥的来路 = 真 `PluginHost`（`config()` 返回 `this`）；没有 appdata ⇒ available=false ⇒
    // 夹具的"可写"那半会红（⛔ 不静默跳过，那会让这条判据永远不执行）。
    LIFT_CHECK(f.attachAppData(hostPathText(appdata.path)));
    LIFT_CHECK(f.attachConfig());
    LIFT_CHECK(f.host->script() != nullptr);
    if (f.host->script() == nullptr) return;
    LIFT_CHECK(f.host->script()->hasRender());
    LIFT_CHECK(!f.host->script()->hasRun());
    lift::ui::UiHostOps ops{};
    ops.config = f.host->config();
    lift::ui::UiBuilder builder;
    lift::ui::UiFrameReadings readings;
    readings.host = &ops;
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
        // ★ S2：`style.key` → `Element.key`（命中区；与经典 `ui.rect({key})` 同一个字段）。
        LIFT_CHECK(elements[0].key == "hit10");
        // ② 第二条 Text：文案 / 字号 / 颜色（`#EBFFEB`）；★ S2：`style.bold` → `Element.bold`。
        LIFT_CHECK(elements[1].kind == lift::ui::ElementKind::Text);
        LIFT_CHECK(elements[1].text == "hello");
        LIFT_CHECK_EQ_I64(elements[1].font_size, 20);
        LIFT_CHECK(elements[1].bold);
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
        {"text.alpha", "alpha"},     {"text.key", "key"},       {"measure.align", "align"},
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

    // ---- S2 复审 minor 4：`measure` 的 `bold` 真的参与度量（⛔ 静默按非粗体量字）----
    //   ★ 判据 = 与**宿主自己的** `measureText(..., bold)` 交叉核对（⛔ 不是抄 JS 的数）。
    lift_test::g_current_case = "plugin/ui-draw-measure-bold";
    {
        std::string load_err;
        std::unique_ptr<lift::ui::PluginScript> bold =
            loadPlugin("measure-bold.js", {}, font, &load_err);
        LIFT_CHECK(bold != nullptr);
        if (bold != nullptr) {
            lift::ui::UiBuilder b;
            lift::ui::UiFrameReadings r;
            std::string rerr;
            LIFT_CHECK(bold->render(b, r, &rerr));
            const std::vector<lift::ui::Element>& e = b.elements();
            LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(e.size()), 2);
            if (e.size() == 2) {
                int bw = 0;
                int bh = 0;
                int nw = 0;
                int nh = 0;
                std::string merr;
                LIFT_CHECK(lift::ui::measureText("hello", 18, true, *font, &bw, &bh, &merr));
                LIFT_CHECK(lift::ui::measureText("hello", 18, false, *font, &nw, &nh, &merr));
                LIFT_CHECK_EQ_I64(e[0].w, bw);
                LIFT_CHECK_EQ_I64(e[0].h, bh);
                LIFT_CHECK_EQ_I64(e[1].w, nw);
                LIFT_CHECK_EQ_I64(e[1].h, nh);
                // 读数（不是断言）：本机字体的粗体 / 常规度量是否可区分（缺粗体 face 时提示）。
                std::printf("  [%s] bold=%dx%d normal=%dx%d\n", lift_test::g_current_case, bw, bh, nw, nh);
            }
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
// S0（缺陷 1）· **Run 收口必须释放本 Run 的 hook**（每条收口路径一条用例）
//
//   红 = `start` 里 `ctx.hook.at` + `handle.patch` 装的覆盖活过 Run 收口 ⇒ 后续脚本指令仍按条
//   改写 guest。三条收口各判一次：`ctx.fail` / `return []`（done，install 在回调**返回之后**置位）/
//   2 ms CPU 预算超限。
//   ★ 非空断言：夹具装成功时打一行 `hook-release: installed …` ⇒ `logLines() >= 1`
//     （否则"at() 本来就失败"会让"Run 结束后表为空"假绿）。
//   ★ 夹具 = `tests/plugin_fixtures/hook-release.js`（`mode` 选收口路径）。
// =====================================================================================
void caseHookReleaseOnFail() {
    lift_test::g_current_case = "plugin/hook-release-on-fail";
    Fixture f;
    LIFT_CHECK(f.mem_ok && f.win_ok);
    if (!f.mem_ok || !f.win_ok) return;
    const bool loaded = f.load("hook-release.js", {{"mode", "fail"}});
    LIFT_CHECK(loaded);
    if (!loaded) {
        std::fprintf(stderr, "  load error: %s\n", f.load_error.c_str());
        return;
    }
    f.pump();
    dumpFail(f);
    LIFT_CHECK(f.host->failed());
    LIFT_CHECK(f.host->failName() == "hook");
    LIFT_CHECK(f.host->failReason().find("release-on-fail") != std::string::npos);
    LIFT_CHECK(f.host->logLines() >= 1);  // hook 真的装上了
    // ★ 判据：Run 结束后一条 hook 都不许活（同进程后续用例不许背着活 hook）。
    LIFT_CHECK(f.host->instrHookList().empty());
    LIFT_CHECK(!lift::instrHooksActive());
}

void caseHookReleaseOnDone() {
    lift_test::g_current_case = "plugin/hook-release-on-done";
    Fixture f;
    LIFT_CHECK(f.mem_ok && f.win_ok);
    if (!f.mem_ok || !f.win_ok) return;
    const bool loaded = f.load("hook-release.js", {{"mode", "done"}});
    LIFT_CHECK(loaded);
    if (!loaded) {
        std::fprintf(stderr, "  load error: %s\n", f.load_error.c_str());
        return;
    }
    f.pump();
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK(f.host->runner().done());
    LIFT_CHECK(f.host->logLines() >= 1);  // hook 真的装上了
    // ★ 判据：`install([])` 的 done 发生在 `onTrigger`/`start` 返回之后 ⇒ 读点入口必须补一次释放。
    LIFT_CHECK(f.host->instrHookList().empty());
    LIFT_CHECK(!lift::instrHooksActive());
}

void caseHookReleaseOnBudget() {
    lift_test::g_current_case = "plugin/hook-release-on-budget";
    Fixture f;
    LIFT_CHECK(f.mem_ok && f.win_ok);
    if (!f.mem_ok || !f.win_ok) return;
    const bool loaded = f.load("hook-release.js", {{"mode", "budget"}});
    LIFT_CHECK(loaded);
    if (!loaded) {
        std::fprintf(stderr, "  load error: %s\n", f.load_error.c_str());
        return;
    }
    // 生产值 2 ms + op_limit 不限：这条判的**只**是 CPU deadline（与 `plugin/callback-cpu-budget` 同形）。
    f.host->runner().setBudget(lift::plugin::Budget{2'000'000ull, 0u});
    f.pump();
    dumpFail(f);
    LIFT_CHECK(f.host->failed());
    // 机制断言：QJS 中断回调真的按 CPU deadline 打断过（⛔ 不是别的失败路径）。
    LIFT_CHECK(f.host->script() != nullptr && f.host->script()->overran());
    LIFT_CHECK(f.host->logLines() >= 1);  // hook 在忙等之前就装上了
    // ★ 判据：`runCallback` 的预算失败分支也是 Run 收口 ⇒ hook 必须一起撤。
    LIFT_CHECK(f.host->instrHookList().empty());
    LIFT_CHECK(!lift::instrHooksActive());
}

// =====================================================================================
// S2a · `plugin/point-hit`：真插件 `handle.onTick` + 假 marker / 读点
//
//   红了 = 指令点命中后 JS 回调没跑 / 参数漂（`InstrHit` 与 tick 对象之间断链）。
//   判据（全部是客体读数，⛔ 不是"没报错"）：
//     ① 一次探针 ⇒ log 行 **恰好 +1**（tick 里的 `tick.log`）且 `stats().hits == 1`；
//     ② 第二次探针 ⇒ 再 +1、`hits == 2`；
//     ③ 触发一个 `afterPosition` 读点 ⇒ onTrigger 把 `tick.{seen,pos,opcode,script,id}` 与
//        `ctx.hook.list()[0].hits` 写进 guest int ⇒ C++ 逐项读回相等。
//   ★ 为什么字段校验放在 onTrigger：tick 入口 `inCallback()` 恒 false ⇒ `ctx.fail` 也 TypeError
//     （那会把"参数错"伪装成"handler 抛错"）。onTrigger 是合法的 Run 回调。
// =====================================================================================
constexpr std::uint32_t kTickOutSeen = 0xb100u;
constexpr std::uint32_t kTickOutStatsHits = 0xb101u;
constexpr std::uint32_t kTickOutPos = 0xb102u;
constexpr std::uint32_t kTickOutOpcode = 0xb103u;
constexpr std::uint32_t kTickOutScriptOk = 0xb104u;
constexpr std::uint32_t kTickOutId = 0xb105u;

void casePointHit() {
    lift_test::g_current_case = "plugin/point-hit";
    Fixture f;
    LIFT_CHECK(f.mem_ok && f.win_ok);
    if (!f.mem_ok || !f.win_ok) return;
    LIFT_CHECK(installInstrFrame(f.mem));
    const bool loaded = f.load("instr-tick.js", {{"mode", "hit"}});
    LIFT_CHECK(loaded);
    if (!loaded) {
        std::fprintf(stderr, "  load error: %s\n", f.load_error.c_str());
        return;
    }
    f.pump();  // start：装 hook + onTick
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    const std::vector<lift::InstrHookStatus> hooks = f.host->instrHookList();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(hooks.size()), 1);
    if (hooks.size() != 1) return;
    const std::uint32_t handle = hooks[0].id;
    const std::uint64_t base = f.host->logLines();
    LIFT_CHECK(base >= 1u);  // start 的 install 日志（非空断言：hook 真装上了）

    dispatchInstr(f, kInstrPos);
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->logLines()), static_cast<std::int64_t>(base + 1));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(instrHits(handle)), 1);

    dispatchInstr(f, kInstrPos);
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->logLines()), static_cast<std::int64_t>(base + 2));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(instrHits(handle)), 2);

    // 让 afterPosition 命中：onTrigger 把 tick 的字段写回 guest。
    // ★ 先记下命中数（onTrigger 返回 `[]` ⇒ Run 收口 ⇒ hook 表会被清空，之后 `instrHookStats` 查不到）。
    const std::uint64_t hits_before_fire = instrHits(handle);
    f.readings.pos = 0x1000u;
    f.pump();
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_U32(readFieldInt(f, kTickOutSeen), 2u);
    LIFT_CHECK_EQ_U32(readFieldInt(f, kTickOutStatsHits), 2u);
    LIFT_CHECK_EQ_U32(readFieldInt(f, kTickOutPos), kInstrPos);
    LIFT_CHECK_EQ_U32(readFieldInt(f, kTickOutOpcode), kInstrOpcode);
    LIFT_CHECK_EQ_U32(readFieldInt(f, kTickOutScriptOk), 1u);
    LIFT_CHECK_EQ_U32(readFieldInt(f, kTickOutId), handle);
    std::printf("  [%s] handle=%u hits=%llu\n", lift_test::g_current_case, handle,
                static_cast<unsigned long long>(hits_before_fire));
}

// =====================================================================================
// S2a · `plugin/point-hit-handler-error`：handler 抛错 ⇒ 一行 error + 摘 handler + Run 继续
//
//   红了 = ① 异常穿出 `extern "C"`（崩溃）/ ② 失败被升级成 Run fail / ③ handler 没被摘
//   （第二次命中又打一行 + 再烧一次 JS）/ ④ entry 被误摘（`stats().hits` 不再涨）。
// =====================================================================================
void casePointHitHandlerError() {
    lift_test::g_current_case = "plugin/point-hit-handler-error";
    Fixture f;
    LIFT_CHECK(f.mem_ok && f.win_ok);
    if (!f.mem_ok || !f.win_ok) return;
    LIFT_CHECK(installInstrFrame(f.mem));
    const bool loaded = f.load("instr-tick.js", {{"mode", "throw"}});
    LIFT_CHECK(loaded);
    if (!loaded) {
        std::fprintf(stderr, "  load error: %s\n", f.load_error.c_str());
        return;
    }
    f.pump();
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    const std::vector<lift::InstrHookStatus> hooks = f.host->instrHookList();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(hooks.size()), 1);
    if (hooks.size() != 1) return;
    const std::uint32_t handle = hooks[0].id;
    const std::uint64_t base = f.host->logLines();

    // 第一次：handler 抛 "boom" ⇒ 恰好一行 error（消息带 `hit #1`），Run **不 fail**。
    dispatchInstr(f, kInstrPos);
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->logLines()), static_cast<std::int64_t>(base + 1));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(instrHits(handle)), 1);

    // 第二次：handler 已摘 ⇒ 不跑 JS、不再打日志；entry 还在 ⇒ `hits == 2`。
    dispatchInstr(f, kInstrPos);
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->logLines()), static_cast<std::int64_t>(base + 1));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(instrHits(handle)), 2);
}

// =====================================================================================
// S2a · `plugin/point-hit-after-run-end`：Run 收口之后探针**不再调 JS**
//
//   红了 = handler 的 JS 值活过 Run（`on_released` 没接上）⇒ 下一个探针还跑用户 JS，
//   而且那时 `ctx` / 预算窗都不存在（UB 面）。判据：Run done + 表空 + log 行数不变。
// =====================================================================================
void casePointHitAfterRunEnd() {
    lift_test::g_current_case = "plugin/point-hit-after-run-end";
    Fixture f;
    LIFT_CHECK(f.mem_ok && f.win_ok);
    if (!f.mem_ok || !f.win_ok) return;
    LIFT_CHECK(installInstrFrame(f.mem));
    const bool loaded = f.load("instr-tick.js", {{"mode", "done"}});
    LIFT_CHECK(loaded);
    if (!loaded) {
        std::fprintf(stderr, "  load error: %s\n", f.load_error.c_str());
        return;
    }
    f.pump();  // start 里 `return []` ⇒ install([]) 置 done ⇒ 读点入口补一次释放
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK(f.host->runner().done());
    LIFT_CHECK(f.host->instrHookList().empty());
    LIFT_CHECK(!lift::instrHooksActive());
    const std::uint64_t base = f.host->logLines();
    LIFT_CHECK(base >= 1u);

    dispatchInstr(f, kInstrPos);
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->logLines()), static_cast<std::int64_t>(base));
}

// =====================================================================================
// S2a · `plugin/drain-per-entry`：continuation 在**同一个入口**跑完；跨入口看不到中间态
//
//   红了 = 某个 host→JS 入口漏了 drain ⇒ continuation 落到**下一个入口**才跑（静默语义漂：
//   它看到的是下一个读点 / 下一个 tick 的上下文）。两个方向：
//     ① `call()`（start）：`phase` 的 continuation ⇒ 下一个读点（onTrigger）写出的必须是 2；
//     ② `runInstrHits`（tick）：tick 里排的 continuation 必须在**探针返回前**打日志。
// =====================================================================================
constexpr std::uint32_t kDrainOutPhase = 0xb110u;

void caseDrainPerEntry() {
    lift_test::g_current_case = "plugin/drain-per-entry";
    Fixture f;
    LIFT_CHECK(f.mem_ok && f.win_ok);
    if (!f.mem_ok || !f.win_ok) return;
    LIFT_CHECK(installInstrFrame(f.mem));
    const bool loaded = f.load("drain-entry.js", {{"mode", "ok"}});
    LIFT_CHECK(loaded);
    if (!loaded) {
        std::fprintf(stderr, "  load error: %s\n", f.load_error.c_str());
        return;
    }
    // 首点就读在 `afterPosition` 的区间上：start 装 trigger，**下一个读点**才 fire。
    f.readings.pos = 0x1000u;
    f.pump();  // start
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->instrHookList().size()), 1);

    // ① start 入口的 drain：第二个读点 fire ⇒ 写出 phase（2 = continuation 已在 start 内跑完）。
    //   ★ 顺序有意如此：onTrigger 之后**才**做 tick 探针 —— 否则探针自己的 drain（②）会把
    //     start 残留的 continuation 顺手跑掉，①就变成"换个入口也能过"的假绿。
    f.pump();
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_U32(readFieldInt(f, kDrainOutPhase), 2u);

    // ② runInstrHits 入口的 drain：探针返回时那一行 log 必须已经出现（同一入口内）。
    const std::uint64_t base = f.host->logLines();
    dispatchInstr(f, kInstrPos);
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->logLines()), static_cast<std::int64_t>(base + 1));
    std::printf("  [%s] phase=%u\n", lift_test::g_current_case, readFieldInt(f, kDrainOutPhase));
}

// =====================================================================================
// S2a · `plugin/drain-budget`：`while(true) await Promise.resolve()` ⇒ 同一预算窗打断
//
//   红了 = drain 在 `cpu_deadline_ns` 清零之后跑（无 deadline ⇒ 无中断）⇒ **guest 永久卡死**
//   （这条用例本身就是"卡死"的证据：它不会返回）。判据 = Run fail + `overran()` +
//   reason 含 `interrupted` 与 `回调超出预算`。★ ⛔ 不断言"耗时 ≈2 ms"（`pluginThreadCpuNs()`
//   在 Windows 上是 `QueryThreadCycleTime`，只保证能判跑飞）。
// =====================================================================================
void caseDrainBudget() {
    lift_test::g_current_case = "plugin/drain-budget";
    Fixture f;
    LIFT_CHECK(f.mem_ok && f.win_ok);
    if (!f.mem_ok || !f.win_ok) return;
    const bool loaded = f.load("drain-entry.js", {{"mode", "spin"}});
    LIFT_CHECK(loaded);
    if (!loaded) {
        std::fprintf(stderr, "  load error: %s\n", f.load_error.c_str());
        return;
    }
    // 生产值 2 ms + op_limit 不限：判的**只**是 CPU deadline（与 `plugin/callback-cpu-budget` 同形）。
    f.host->runner().setBudget(lift::plugin::Budget{2'000'000ull, 0u});
    f.pump();  // 不返回 = drain 没有 deadline（本用例的红就是你看到它挂住）
    dumpFail(f);
    LIFT_CHECK(f.host->failed());
    LIFT_CHECK(f.host->script() != nullptr && f.host->script()->overran());
    const std::string& reason = f.host->failReason();
    LIFT_CHECK(reason.find("interrupted") != std::string::npos);
    LIFT_CHECK(reason.find("回调超出预算") != std::string::npos);
    std::printf("  [%s] reason=%s\n", lift_test::g_current_case, reason.c_str());
}


// =====================================================================================
// S2b · `ctx.once` / `ctx.task` / timeout 泵 / Run 存活
//
// 判据（每条都点名"红了 = 什么坏了"）：
//   · `plugin/once-resolve`   ：once 命中 ⇒ resolve 的 Tick 与 `onTick` 同形（pos/script/id/opcode），
//                                临时 hook 命中即撤；task 的 continuation 在**泵**里跑完。
//   · `plugin/once-timeout`   ：不命中 + 短 timeout ⇒ 泵先 reject（catch 能 log）**随后** fail Run；
//                                `instrHookList()` 空、无 hook 残留、readout `run=failed`。
//   · `plugin/task-await`     ：task 的 phase=1→2 在**同一入口**完成（跨入口看到 1 = 红）；settle 后收口。
//   · `plugin/task-await-close`：start 返回 null + task 立即 settle ⇒ readout `run=done`。
//   · `plugin/task-keeps-run` ：task `await ctx.once(长 timeout)` ⇒ `runner.done()` 但 Run 不收口；
//                                once 命中 ⇒ task 结束 ⇒ 收口。
//   · `plugin/task-reject`    ：task 抛 boom ⇒ run=failed + reason 含 boom。
//   · `plugin/pump-budget`    ：泵里 `while(true) await Promise.resolve()` ⇒ 预算中断、Run fail、不卡死。
// =====================================================================================
constexpr std::uint32_t kS2bOutPos = 0xb120u;
constexpr std::uint32_t kS2bOutScriptOk = 0xb121u;
constexpr std::uint32_t kS2bOutId = 0xb122u;
constexpr std::uint32_t kS2bOutOpcodeOk = 0xb123u;
constexpr std::uint32_t kS2bOutHooks = 0xb124u;
constexpr std::uint32_t kS2bOutPhase = 0xb125u;

bool loadS2b(Fixture& f, const char* mode) {
    LIFT_CHECK(f.mem_ok && f.win_ok);
    if (!f.mem_ok || !f.win_ok) return false;
    const bool loaded = f.load("s2b-async.js", {{"mode", mode}});
    LIFT_CHECK(loaded);
    if (!loaded) std::fprintf(stderr, "  load error: %s\n", f.load_error.c_str());
    return loaded;
}

void caseOnceResolve() {
    lift_test::g_current_case = "plugin/once-resolve";
    Fixture f;
    if (!loadS2b(f, "once-resolve")) return;
    LIFT_CHECK(installInstrFrame(f.mem));
    f.pump();  // start：注册 once（临时 hook）+ task
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->callbacks()), 1);
    const std::vector<lift::InstrHookStatus> hooks = f.host->instrHookList();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(hooks.size()), 1);
    if (hooks.size() != 1) return;
    const std::uint32_t handle = hooks[0].id;
    const std::uint64_t base = f.host->logLines();

    // 命中：resolve + 撤临时 hook。★ S2c：探针入口自己的 drain 就把 task 的 continuation
    //   跑完（每个 host→JS 入口都 drain 到静止）⇒ 下面那次 pump 只是再来一个读点。
    dispatchInstr(f, kInstrPos);
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK(f.host->instrHookList().empty());

    // 泵：task 的 continuation 跑完（模块变量 lastTick 记下 Tick 字段，并 log 一行）。
    f.pump();
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->logLines()), static_cast<std::int64_t>(base + 1));

    // 下一次合法 Run 回调把 lastTick 写回 guest（Run 还活着：armed 的 dump trigger）。
    f.readings.pos = 0x1000u;
    f.pump();
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_U32(readFieldInt(f, kS2bOutPos), kInstrPos);
    LIFT_CHECK_EQ_U32(readFieldInt(f, kS2bOutScriptOk), 1u);
    LIFT_CHECK_EQ_U32(readFieldInt(f, kS2bOutId), handle);
    LIFT_CHECK_EQ_U32(readFieldInt(f, kS2bOutOpcodeOk), 1u);
    LIFT_CHECK_EQ_U32(readFieldInt(f, kS2bOutHooks), 0u);
    LIFT_CHECK(f.host->runner().done());
    LIFT_CHECK(f.host->readout().find("run=done") != std::string::npos);
}

void caseOnceTimeout() {
    lift_test::g_current_case = "plugin/once-timeout";
    Fixture f;
    if (!loadS2b(f, "once-timeout")) return;
    f.pump();  // start：task 里 `await ctx.once(..., timeoutMs=1)`
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->instrHookList().size()), 1);
    const std::uint64_t base = f.host->logLines();
    // 超过 1 ms（墙钟；不是 2 ms 的 CPU 预算）⇒ 下一个泵必须判到期。
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    f.pump();
    dumpFail(f);
    LIFT_CHECK(f.host->failed());
    // ① reason 点名 timeout（父级裁决：timeout = 失败截止，用户 catch 了也照样 fail）。
    const std::string& reason = f.host->failReason();
    LIFT_CHECK(reason.find("once timeout") != std::string::npos);
    // ② catch 在**泵**里 drain 到了（那行 log 是 catch 打的，证明"先 reject 再 fail"的顺序）。
    LIFT_CHECK(f.host->logLines() > base);
    // ③ 临时 hook 与条目一起消失（无 hook 残留）。
    LIFT_CHECK(f.host->instrHookList().empty());
    LIFT_CHECK(!lift::instrHooksActive());
    LIFT_CHECK(f.host->readout().find("run=failed") != std::string::npos);
    std::printf("  [%s] reason=%s\n", lift_test::g_current_case, reason.c_str());
}

void caseTaskAwait() {
    // ---- ① 跨入口看不到 phase=1：continuation 必须在 start 的入口末尾跑完 ----
    lift_test::g_current_case = "plugin/task-await";
    {
        Fixture f;
        if (!loadS2b(f, "task-await")) return;
        f.pump();  // start：task 写 phase=1、await，然后在本入口末尾续到 2
        dumpFail(f);
        LIFT_CHECK(!f.host->failed());
        // start 一行 + continuation 一行（continuation 若漏 drain，这一行会落到第二个读点之后）。
        LIFT_CHECK(f.host->logLines() >= 2u);
        f.readings.pos = 0x1000u;
        f.pump();  // onTrigger 把模块变量 phase 写进 guest
        dumpFail(f);
        LIFT_CHECK(!f.host->failed());
        LIFT_CHECK_EQ_U32(readFieldInt(f, kS2bOutPhase), 2u);
        LIFT_CHECK(f.host->runner().done());
        LIFT_CHECK(f.host->readout().find("run=done") != std::string::npos);
    }
    // ---- ② task settle ⇒ Run 收口（start 返回 null；没有 armed trigger 也能 done）----
    lift_test::g_current_case = "plugin/task-await-close";
    {
        Fixture f;
        if (!loadS2b(f, "task-close")) return;
        f.pump();
        dumpFail(f);
        LIFT_CHECK(!f.host->failed());
        // start 一行 + task 续到结尾一行（证明 task 真的跑了，不是"没登记也刚好 done"）。
        LIFT_CHECK(f.host->logLines() >= 2u);
        LIFT_CHECK(f.host->runner().done());
        LIFT_CHECK(f.host->readout().find("run=done") != std::string::npos);
    }
}

void caseTaskKeepsRun() {
    lift_test::g_current_case = "plugin/task-keeps-run";
    Fixture f;
    if (!loadS2b(f, "task-keeps-run")) return;
    LIFT_CHECK(installInstrFrame(f.mem));
    f.pump();  // start 返回 null（runner done），但 task 还在 await once ⇒ Run 必须活着
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK(f.host->runner().done());
    // ★ 判据：Run 还活着 —— once 的临时 hook 还在；readout 不能写 `run=done`。
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->instrHookList().size()), 1);
    LIFT_CHECK(f.host->readout().find("run=waiting") != std::string::npos);
    const std::uint64_t base = f.host->logLines();

    dispatchInstr(f, kInstrPos);  // once 命中 ⇒ 撤 hook；continuation 在本 tick 入口的 drain 里跑
    LIFT_CHECK(f.host->instrHookList().empty());
    f.pump();  // 泵：再来一个读点 ⇒ Run 收口
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->logLines()), static_cast<std::int64_t>(base + 1));
    LIFT_CHECK(f.host->instrHookList().empty());
    LIFT_CHECK(!lift::instrHooksActive());
    LIFT_CHECK(f.host->readout().find("run=done") != std::string::npos);
}

void caseTaskReject() {
    lift_test::g_current_case = "plugin/task-reject";
    Fixture f;
    if (!loadS2b(f, "task-reject")) return;
    f.pump();  // start 里 `ctx.task(async () => { throw new Error("boom"); })`
    dumpFail(f);
    LIFT_CHECK(f.host->failed());
    LIFT_CHECK(f.host->failReason().find("boom") != std::string::npos);
    LIFT_CHECK(f.host->readout().find("run=failed") != std::string::npos);
    std::printf("  [%s] reason=%s\n", lift_test::g_current_case, f.host->failReason().c_str());
}

void casePumpBudget() {
    lift_test::g_current_case = "plugin/pump-budget";
    Fixture f;
    if (!loadS2b(f, "pump-budget")) return;
    // 生产值 2 ms + op_limit 不限：判的**只**是泵的 CPU deadline（与 `plugin/drain-budget` 同形）。
    f.host->runner().setBudget(lift::plugin::Budget{2'000'000ull, 0u});
    f.pump();  // start：task 在 await once（不 spin）
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->instrHookList().size()), 1);
    // ★ S2c：once **命中**的 continuation 现在在 tick 入口自己的 drain 里跑（每个入口都 drain）；
    //   想让忙等落在**泵**里，用 **timeout 的 catch**（泵先 reject 到期的一次，再 drain 出 catch）。
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const std::uint64_t base = f.host->logLines();
    f.pump();  // 不返回 = 泵的 drain 没有 deadline（本用例的红就是挂住）
    dumpFail(f);
    LIFT_CHECK(f.host->failed());
    // 泵里的 task 真的 resume 了（"about to spin" 那行），否则"没 fail"可能是"根本没跑到"。
    LIFT_CHECK(f.host->logLines() > base);
    LIFT_CHECK(f.host->script() != nullptr && f.host->script()->overran());
    const std::string& reason = f.host->failReason();
    LIFT_CHECK(reason.find("interrupted") != std::string::npos);
    LIFT_CHECK(reason.find("泵里的异步状态超出预算") != std::string::npos);
    std::printf("  [%s] reason=%s\n", lift_test::g_current_case, reason.c_str());
}

// =====================================================================================
// ★ S2-3（复审 S2-3）：`hasPendingWork` 的 `onces` / `hook_handlers` 两支各有一条**行为**守卫
//
//   `plugin/bare-once-keeps-run`：start 里**不包 task** 直接 `ctx.once(...)`（长 timeout），
//     start 返回 null ⇒ `runner.done()` 但 Run **不收口**（once 的临时 hook 还在、`run=waiting`）；
//     命中后 `.then` 的 continuation 在泵里跑完 ⇒ 收口（`run=done`）。
//     ★ 红了意味着什么：`hasPendingWork` 不把 once 当存活来源 ⇒ 第一拍就 `releaseRunHooks`
//       撤掉临时 hook —— promise 永远 pending，而**没有报错**（旧变异 M1 就是删这支仍全绿）。
//
//   `plugin/onTick-keeps-run`：`ctx.hook.at(...).onTick(fn)` + 返回 null ⇒ Run 靠 sync 订阅活着；
//     撤订阅（合法的 Run 回调里 `onTick(null)`）后再来一个读点 ⇒ 收口。
//     ★ `onTick(null)` 只能在 Run 回调里（`inCallback()` 门）⇒ runner 已 done（没有 armed trigger）
//       ⇒ 用例直接驱动一次 `PluginHost::onTrigger`（TriggerHandler 的公开入口），这正是
//       "只有订阅让 Run 活着"的形状。
// =====================================================================================
void caseBareOnceKeepsRun() {
    lift_test::g_current_case = "plugin/bare-once-keeps-run";
    Fixture f;
    if (!loadS2b(f, "bare-once")) return;
    LIFT_CHECK(installInstrFrame(f.mem));
    f.pump();  // start：裸 once（不进 task）+ 返回 null ⇒ runner done，Run 靠 once 活着
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK(f.host->runner().done());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->instrHookList().size()), 1);
    LIFT_CHECK(f.host->readout().find("run=waiting") != std::string::npos);
    const std::uint64_t base = f.host->logLines();

    dispatchInstr(f, kInstrPos);  // 命中 ⇒ resolve + 撤临时 hook（`.then` 由下一个泵推进）
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK(f.host->instrHookList().empty());

    f.pump();  // 泵：`.then` 的 continuation 跑完（一行 log），once 账已空 ⇒ 收口
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK(f.host->logLines() > base);  // continuation 真的跑了（不是"没登记也刚好 done"）
    LIFT_CHECK(f.host->readout().find("run=done") != std::string::npos);
    LIFT_CHECK(!lift::instrHooksActive());
    std::printf("  [%s] %s\n", lift_test::g_current_case, f.host->readout().c_str());
}

void caseOnTickKeepsRun() {
    lift_test::g_current_case = "plugin/onTick-keeps-run";
    Fixture f;
    if (!loadS2b(f, "onTick-keeps-run")) return;
    LIFT_CHECK(installInstrFrame(f.mem));
    f.pump();  // start：hook.at + onTick(fn)，返回 null ⇒ runner done，Run 靠订阅活着
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK(f.host->runner().done());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->instrHookList().size()), 1);
    LIFT_CHECK(f.host->readout().find("run=waiting") != std::string::npos);

    f.pump();  // 再来一个读点：仍活着（没有 task / once，只靠 hook_handlers 支）
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->instrHookList().size()), 1);
    LIFT_CHECK(f.host->readout().find("run=waiting") != std::string::npos);

    // 撤订阅 = 合法的 Run 回调（runner 已 done ⇒ 直接驱动 `PluginHost::onTrigger`）。
    lift::plugin::TriggerEvent ev;
    ev.what = lift::plugin::TriggerEvent::What::Fired;
    ev.name = "manual";
    ev.trigger = f.host->factory().afterPosition("manual", "SCENE1", 0x1000u, 0x1001u,
                                                 lift::plugin::TriggerMode::Once);
    std::string ev_err;
    (void)f.host->onTrigger(f.host->executor().context(), ev, &ev_err);
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());

    f.pump();  // 撤订阅之后的读点 ⇒ 收口（runner 已 done + 没有待推工作）
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK(f.host->readout().find("run=done") != std::string::npos);
    LIFT_CHECK(f.host->instrHookList().empty());
    LIFT_CHECK(!lift::instrHooksActive());
    std::printf("  [%s] %s\n", lift_test::g_current_case, f.host->readout().c_str());
}

// =====================================================================================
// ★ S2-4（复审 S2-4）：`ctx.hook.clear()` 撤掉 once 的**临时** hook 时，必须 reject 那个 Promise
//   `plugin/once-hook-clear`：task `await ctx.once(长 timeout)`，onTrigger 里 `ctx.hook.clear()`
//   ⇒ once reject ⇒ task 的 catch 在**同一个** `call()` 的 drain 里跑完 ⇒ 收口（`run=done`）。
//   ★ 红了意味着什么：`on_released` 只删账不 reject ⇒ task 永远 pending、deadline 随条目消失
//     ⇒ readout 一直 `run=waiting`（只能等 `--time-limit`），且 catch 那行 log 永远不出现。
//   ★ 本用例的语义选择：once 被撤 = **可 catch 的拒绝**（不自己 fail Run）；catch 掉了 ⇒ done。
// =====================================================================================
void caseOnceHookClear() {
    lift_test::g_current_case = "plugin/once-hook-clear";
    Fixture f;
    if (!loadS2b(f, "once-hook-clear")) return;
    LIFT_CHECK(installInstrFrame(f.mem));
    f.pump();  // start：task 里 await once（长 timeout），arm `clear`
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->instrHookList().size()), 1);
    const std::uint64_t base = f.host->logLines();

    f.readings.pos = 0x1000u;
    f.pump();  // onTrigger：ctx.hook.clear() ⇒ once 被 reject ⇒ task 的 catch 在本次 drain 里跑
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK(f.host->logLines() > base);  // catch 真的跑了（once 被 reject）
    LIFT_CHECK(f.host->instrHookList().empty());
    // ★ 关键：不许出现"一直 waiting 直到 --time-limit"。
    LIFT_CHECK(f.host->readout().find("run=done") != std::string::npos);
    std::printf("  [%s] %s\n", lift_test::g_current_case, f.host->readout().c_str());
}

// =====================================================================================
// ★ S2-1（复审 S2-1）：`ctx.finish` / `ctx.quit` 之后**同一入口**里 task 拒绝，不许把 Run 翻成 failed
//   `plugin/task-finish-then-reject`：start 里 `ctx.task(async () => { throw … })` + `ctx.finish(5)`
//   ⇒ 本入口 drain 之后 settle 看到"已到终态" ⇒ 静默丢弃 ⇒ readout 仍是 `run=finished exit=5`。
//   `plugin/task-quit-then-reject`：同形，readout 是 `run=quit`。
//   ★ 红了意味着什么：settle 只判 `failed()` ⇒ `fail("task", …)` 覆盖 reason、把 finished 翻成 failed
//     （`exit=` 读数因此失去"Run 是正常收口"的证据）。
// =====================================================================================
void caseTaskFinishThenReject() {
    lift_test::g_current_case = "plugin/task-finish-then-reject";
    {
        Fixture f;
        if (!loadS2b(f, "task-finish-reject")) return;
        f.pump();  // start：task 立即拒绝 + finish(5)，同一 call() 的 drain 之后 settle
        dumpFail(f);
        LIFT_CHECK(f.host->finished());
        LIFT_CHECK(!f.host->failed());
        LIFT_CHECK_EQ_I64(f.host->finishCode(), 5);
        const std::string line = f.host->readout();
        LIFT_CHECK(line.find("run=finished") != std::string::npos);
        LIFT_CHECK(line.find("exit=5") != std::string::npos);
        LIFT_CHECK(line.find("run=failed") == std::string::npos);
        std::printf("  [%s] %s\n", lift_test::g_current_case, line.c_str());
    }
    lift_test::g_current_case = "plugin/task-quit-then-reject";
    {
        Fixture f;
        if (!loadS2b(f, "task-quit-reject")) return;
        f.pump();
        dumpFail(f);
        LIFT_CHECK(f.host->quitRequested());
        LIFT_CHECK(!f.host->failed());
        const std::string line = f.host->readout();
        LIFT_CHECK(line.find("run=quit") != std::string::npos);
        LIFT_CHECK(line.find("run=failed") == std::string::npos);
        std::printf("  [%s] %s\n", lift_test::g_current_case, line.c_str());
    }
}

// =====================================================================================
// ★ S2-2（复审 S2-2）：`ctx.finish`（= `PluginHost::finish`）之后调 `render` ⇒ 不泵 / 不 settle / 不跑用户 JS
//   `plugin/render-after-finish`：start 起一个 task `await ctx.once(长 timeout)`；用例直接调
//   `PluginHost::finish(5)`（与 `ctx.finish` 同一个宿主入口）⇒ `releaseRunHooks` 把 once 的临时
//   hook 撤掉（S2-4 起 reject ⇒ catch 的 continuation 已进 job 队列）；然后调 `PluginScript::render`
//   ⇒ 必须直接早退：泵若跑，catch 的 log 会涨；render 若跑，"render-ran" 也会涨。
//   ★ 红了意味着什么：render 泵不判终态 ⇒ 收口后仍 drain 用户 continuation（并可能把 finished 翻 failed）。
// =====================================================================================
void caseRenderAfterFinish() {
    lift_test::g_current_case = "plugin/render-after-finish";
    Fixture f;
    LIFT_CHECK(f.mem_ok && f.win_ok);
    if (!f.mem_ok || !f.win_ok) return;
    const bool loaded = f.load("render-after-finish.js");
    LIFT_CHECK(loaded);
    if (!loaded) {
        std::fprintf(stderr, "  load error: %s\n", f.load_error.c_str());
        return;
    }
    LIFT_CHECK(installInstrFrame(f.mem));
    f.pump();  // start：task await once（长 timeout）；runner done 但 Run 还活着
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK(f.host->runner().done());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->instrHookList().size()), 1);
    LIFT_CHECK(f.host->readout().find("run=waiting") != std::string::npos);

    f.host->finish(5, "bye");  // = `ctx.finish`：终态 + releaseRunHooks（once 被撤）
    LIFT_CHECK(f.host->finished());
    LIFT_CHECK(f.host->instrHookList().empty());
    const std::uint64_t base = f.host->logLines();

    lift::ui::UiBuilder builder;
    lift::ui::UiFrameReadings readings;
    std::string err;
    const bool ok = f.host->script() != nullptr && f.host->script()->render(builder, readings, &err);
    if (!ok) std::fprintf(stderr, "  render error: %s\n", err.c_str());
    LIFT_CHECK(ok);
    // ★ 判据：不泵（catch 的 log 不出现）、不跑用户 JS（"render-ran" 不出现）、不 settle。
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->logLines()), static_cast<std::int64_t>(base));
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK(f.host->readout().find("run=finished") != std::string::npos);
    std::printf("  [%s] logLines=%llu %s\n", lift_test::g_current_case,
                static_cast<unsigned long long>(f.host->logLines()), f.host->readout().c_str());
}

// =====================================================================================
// S2c · `ctx.at(where, (tick, next) => …)` 的同步订阅面（链 / next 截断 / off / Tick / stale）
//
// 红 = 同点多订阅被 v1 去重拒掉 / 链不按注册序 / `next()` 截断失效 / `off` 不撤账 /
//      handler 抛错留下僵尸订阅 / `Tick.now` 与 `Tick.local` 读不到或读错 / stale 不抛。
// ★ 夹具 = `tests/plugin_fixtures/at-chain.js`（`mode=` 分档；guest 写回只在 onTrigger 里做）。
// =====================================================================================
constexpr std::uint32_t kAtOutA = 0xb130;
constexpr std::uint32_t kAtOutB = 0xb131;
constexpr std::uint32_t kAtOutC = 0xb132;
constexpr std::uint32_t kAtOutN = 0xb133;
constexpr std::uint32_t kAtOutF0 = 0xb134;
constexpr std::uint32_t kAtOutF1 = 0xb135;
constexpr std::uint32_t kAtOutF2 = 0xb136;
constexpr std::uint32_t kAtOutF3 = 0xb137;
constexpr std::uint32_t kAtOutF4 = 0xb138;
constexpr std::uint32_t kAtOutF5 = 0xb139;
constexpr std::uint32_t kAtOutF6 = 0xb13a;
constexpr std::uint32_t kAtUndef = 0x7ffffffe;  // 夹具的 `undefined` 哨兵（⛔ 不是 0）

bool loadAtChain(Fixture& f, const char* mode,
                 const std::map<std::string, std::string>& extra = {}) {
    LIFT_CHECK(f.mem_ok && f.win_ok);
    if (!f.mem_ok || !f.win_ok) return false;
    std::map<std::string, std::string> params = extra;
    params["mode"] = mode;
    const bool loaded = f.load("at-chain.js", params);
    LIFT_CHECK(loaded);
    if (!loaded) std::fprintf(stderr, "  load error: %s\n", f.load_error.c_str());
    return loaded;
}

// `plugin/at-chain-order`：同点两条 at（h1 调 next 两次 = 幂等、h2 不调）
//   ⇒ 顺序 [1,2]、同一个 Tick 对象、tick.id = hits[0].handle、两条 hits / kind 各对。
void caseAtChainOrder() {
    lift_test::g_current_case = "plugin/at-chain-order";
    Fixture f;
    if (!loadAtChain(f, "chain-order")) return;
    LIFT_CHECK(installInstrFrame(f.mem));
    f.pump();  // start：装两条 at（Run 靠 at 订阅 + armed trigger 活着）
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    const std::vector<lift::InstrHookStatus> before = f.host->instrHookList();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(before.size()), 2);
    if (before.size() != 2) return;
    const std::uint32_t h1 = before[0].id;
    const std::uint32_t h2 = before[1].id;
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(before[0].hits), 0);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(before[1].hits), 0);
    LIFT_CHECK(h1 < h2);  // 注册序 = handle 升序

    dispatchInstr(f, kInstrPos);
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    const std::vector<lift::InstrHookStatus> after = f.host->instrHookList();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(after.size()), 2);
    for (const lift::InstrHookStatus& row : after) {
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(row.hits), 1);
        LIFT_CHECK(row.hook_kind == lift::InstrHookKind::Sub);
        LIFT_CHECK(row.form == lift::InstrHookForm::Pos);
        LIFT_CHECK_EQ_U32(row.pos, kInstrPos);
    }

    f.readings.pos = 0x1000u;
    f.pump();  // onTrigger：写回 order / 同一性 / kind / hits，然后撤两条
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutA), 1u);
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutB), 2u);
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutC), kAtUndef);  // 只有两条 ⇒ 没有第三个
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutN), 2u);
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF0), 1u);   // 一次派发一个 Tick 对象
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF1), h1);   // tick.id = hits[0].handle
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF2), 1u);   // h1 < h2
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF3), 1u);   // h1 kind == "sub"
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF4), 1u);   // h2 kind == "sub"
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF5), 1u);   // h1 hits
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF6), 1u);   // h2 hits
}

// `plugin/at-next-truncates`：h1 不调 next ⇒ 只跑 h1（h2 的 hits 不涨）；下一拍调 ⇒ [1,2]。
void caseAtNextTruncates() {
    lift_test::g_current_case = "plugin/at-next-truncates";
    Fixture f;
    if (!loadAtChain(f, "next-truncates")) return;
    LIFT_CHECK(installInstrFrame(f.mem));
    f.pump();  // start
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    const std::vector<lift::InstrHookStatus> hooks = f.host->instrHookList();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(hooks.size()), 2);
    if (hooks.size() != 2) return;
    const std::uint32_t h1 = hooks[0].id;
    const std::uint32_t h2 = hooks[1].id;

    // 截断拍：h1 不调 next ⇒ 只跑 h1（`order=[1]`）。
    //   ★ `hits` 是 **匹配数**（probe 里 `e.hits += 1`，与 handler 是否跑无关）⇒ 两条都是 1；
    //     "截断"的唯一读数在 `order`（h2 的 handler 没跑）。
    dispatchInstr(f, kInstrPos);
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    {
        const std::vector<lift::InstrHookStatus> rows = f.host->instrHookList();
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(rows.size()), 2);
        if (rows.size() == 2) {
            LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(rows[0].hits), 1);
            LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(rows[1].hits), 1);  // 匹配数（不是执行数）
        }
    }
    f.readings.pos = 0x1000u;
    f.pump();  // onTrigger#1：写回第一拍读数 + allowNext=true
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutA), 1u);
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutB), kAtUndef);  // ★ h2 的 handler 没跑
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutN), 1u);
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF0), 1u);   // h1 匹配数
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF1), 1u);   // h2 匹配数（同样涨；机制见上）

    // 继续拍：h1 调 next ⇒ [1,2]。
    dispatchInstr(f, kInstrPos);
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    {
        const std::vector<lift::InstrHookStatus> rows = f.host->instrHookList();
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(rows.size()), 2);
        if (rows.size() == 2) {
            LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(rows[0].hits), 2);
            LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(rows[1].hits), 2);
        }
    }
    f.readings.pos = 0x1000u;
    f.pump();  // onTrigger#2：写回第二拍读数 + 撤两条
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutC), 2u);
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF2), 1u);
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF3), 2u);
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF4), 2u);  // h2 的匹配数（第二拍）
}

// `plugin/at-off`：① onTrigger 里 off(h2) ⇒ true；再撤一次 ⇒ false；表少一条。
//   ② 后续派发只有 h1 跑（h1 调 next ⇒ 若 h2 还在会多一行 log）。
//   ③ mode=tick-off：handler 里 off(自己) 不抛、**不截断**（h2 照跑）、下一拍不再命中。
void caseAtOff() {
    lift_test::g_current_case = "plugin/at-off";
    {
        Fixture f;
        if (!loadAtChain(f, "off")) return;
        LIFT_CHECK(installInstrFrame(f.mem));
        f.pump();  // start：两条 at（h1 调 next）
        dumpFail(f);
        LIFT_CHECK(!f.host->failed());
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->instrHookList().size()), 2);

        f.readings.pos = 0x1000u;
        f.pump();  // onTrigger：off(h2) 两次 + 写回
        dumpFail(f);
        LIFT_CHECK(!f.host->failed());
        LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF0), 1u);  // 第一次 true
        LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF1), 0u);  // 第二次 false（幂等）
        LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF2), 1u);  // 表少一条

        // ② 后续派发只有 h1（它调 next；h2 若还在会变成 +2 行）。
        const std::uint64_t base = f.host->logLines();
        dispatchInstr(f, kInstrPos);
        dumpFail(f);
        LIFT_CHECK(!f.host->failed());
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->logLines()),
                          static_cast<std::int64_t>(base + 1));
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->instrHookList().size()), 1);

        // ★ 撤掉的订阅**不许**留在 ui 侧的账上：`at_subs` 是 `hasPendingWork` 的来源之一，
        //   僵尸账会让 Run 永远不收口（`implOnInstrReleased` 少一句 erase 就红在这里）。
        f.readings.pos = 0x1000u;
        f.pump();  // onTrigger#2：撤 h1 + return [] ⇒ 收口
        dumpFail(f);
        LIFT_CHECK(!f.host->failed());
        LIFT_CHECK(f.host->instrHookList().empty());
        LIFT_CHECK(f.host->readout().find("run=done") != std::string::npos);
    }
    {
        // ③ tick handler 里撤自己：门 = call_host（不是 inCallback）⇒ 不抛；撤除 ≠ 截断。
        Fixture f;
        if (!loadAtChain(f, "tick-off")) return;
        LIFT_CHECK(installInstrFrame(f.mem));
        f.pump();  // start：两条 at（h1 会撤自己）
        dumpFail(f);
        LIFT_CHECK(!f.host->failed());
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->instrHookList().size()), 2);
        const std::uint64_t base = f.host->logLines();

        dispatchInstr(f, kInstrPos);  // h1: off=self + log；next ⇒ h2 照跑 + log
        dumpFail(f);
        LIFT_CHECK(!f.host->failed());
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->logLines()),
                          static_cast<std::int64_t>(base + 2));
        const std::vector<lift::InstrHookStatus> rows = f.host->instrHookList();
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(rows.size()), 1);  // 自己那条被撤

        dispatchInstr(f, kInstrPos);  // 下一拍：只有 h2
        dumpFail(f);
        LIFT_CHECK(!f.host->failed());
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->logLines()),
                          static_cast<std::int64_t>(base + 3));
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->instrHookList().size()), 1);
    }
}

// `plugin/at-handler-error`：h1 抛 ⇒ 一行 error（at #N hit #M）+ **撤整条订阅** + 截断；
//   h2 不跑但 hits 照涨；第二次派发 log 不再增长（h1 已不在），h2 的 hits 继续涨。
void caseAtHandlerError() {
    lift_test::g_current_case = "plugin/at-handler-error";
    Fixture f;
    if (!loadAtChain(f, "handler-error")) return;
    LIFT_CHECK(installInstrFrame(f.mem));
    f.pump();
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    const std::vector<lift::InstrHookStatus> hooks = f.host->instrHookList();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(hooks.size()), 2);
    if (hooks.size() != 2) return;
    const std::uint32_t h1 = hooks[0].id;
    const std::uint32_t h2 = hooks[1].id;
    const std::uint64_t base = f.host->logLines();

    dispatchInstr(f, kInstrPos);  // h1 抛 "boom"
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());  // 观察者挂掉不 fail Run
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->logLines()),
                      static_cast<std::int64_t>(base + 1));  // 恰好一行
    // ★ S2c-1 复审 F7（S3）：不只判行数 —— 消息必须点名 `at #N` / `hit #M`（plan 的判据）。
    //   出口 = `PluginHost::lastLogLine()`（只读观测，⛔ 不改日志行为）。
    {
        const std::string& line = f.host->lastLogLine();
        LIFT_CHECK(line.find("at #") != std::string::npos);
        LIFT_CHECK(line.find("hit #") != std::string::npos);
        LIFT_CHECK(line.find("boom") != std::string::npos);  // handler 抛的原文
    }
    {
        const std::vector<lift::InstrHookStatus> rows = f.host->instrHookList();
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(rows.size()), 1);  // 整条订阅被撤
        if (rows.size() == 1) {
            LIFT_CHECK_EQ_U32(rows[0].id, h2);
            LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(rows[0].hits), 1);  // runtime 计数照涨
        }
    }
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(instrHits(h1)), 0);  // h1 已经不在了

    dispatchInstr(f, kInstrPos);  // 第二次：h1 不再跑（log 不涨），h2 的 hits 涨
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->logLines()),
                      static_cast<std::int64_t>(base + 1));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(instrHits(h2)), 2);

    f.readings.pos = 0x1000u;
    f.pump();  // onTrigger：写回读数 + 撤 h2（让 Run 收口）
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF0), 1u);   // 表里只剩 h2
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF1), 2u);   // h2 hits
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF2), 1u);   // h1 不在表里
    LIFT_CHECK(f.host->instrHookList().empty());
}

// `plugin/tick-now`：`tick.now.{script,pos,opcode?}` 是本拍快照；平铺 deprecated 字段相等；
//   读不到 opcode（pc 在未提交页）⇒ `now` 里**缺键**（⛔ 不编 0）。
void caseTickNow() {
    lift_test::g_current_case = "plugin/tick-now";
    {
        Fixture f;
        if (!loadAtChain(f, "now")) return;
        LIFT_CHECK(installInstrFrame(f.mem));
        f.pump();
        dumpFail(f);
        LIFT_CHECK(!f.host->failed());
        const std::vector<lift::InstrHookStatus> hooks = f.host->instrHookList();
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(hooks.size()), 1);
        if (hooks.size() != 1) return;
        const std::uint32_t handle = hooks[0].id;

        dispatchInstr(f, kInstrPos);
        dumpFail(f);
        LIFT_CHECK(!f.host->failed());
        f.readings.pos = 0x1000u;
        f.pump();
        dumpFail(f);
        LIFT_CHECK(!f.host->failed());
        LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutA), 1u);  // now.script == "FIELD.BIN"
        LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutB), kInstrPos);
        LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutC), 1u);  // now 里有 opcode
        LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutN), kInstrOpcode);
        LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF0), kInstrPos);  // 平铺 pos == now.pos
        LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF1), 1u);         // 平铺 script
        LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF2), kInstrOpcode);// 平铺 opcode
        LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF3), handle);      // tick.id
    }
    {
        // 变体：pc 摆在**未提交页** 0x590000 ⇒ pos = 0x3C + 0xB0000；`mem.rd32(pc)` 读不到。
        constexpr std::uint32_t kNoOpcodePos = lift::kScriptHeaderLen + 0xB0000u;
        Fixture f;
        if (!loadAtChain(f, "now", {{"pos", std::to_string(kNoOpcodePos)}})) return;
        LIFT_CHECK(installInstrFrame(f.mem));
        f.pump();
        dumpFail(f);
        LIFT_CHECK(!f.host->failed());
        dispatchInstr(f, kNoOpcodePos);
        dumpFail(f);
        LIFT_CHECK(!f.host->failed());
        f.readings.pos = 0x1000u;
        f.pump();
        dumpFail(f);
        LIFT_CHECK(!f.host->failed());
        LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutA), 1u);  // 脚本名仍可读
        LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutB), kNoOpcodePos);
        LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutC), 0u);  // ("opcode" in now) === false
        LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutN), kAtUndef);
        LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF2), kAtUndef);  // 平铺 opcode 也缺键
    }
}

// `plugin/tick-local`：`tick.local.{int,ptr,ptrDword}` 的语义值；越界 / 指针 0 ⇒ undefined。
void caseTickLocal() {
    lift_test::g_current_case = "plugin/tick-local";
    Fixture f;
    if (!loadAtChain(f, "local")) return;
    LIFT_CHECK(installInstrFrame(f.mem));
    LIFT_CHECK(installInstrLocalValues(f.mem));
    f.pump();
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->instrHookList().size()), 1);

    dispatchInstr(f, kInstrPos);
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    f.readings.pos = 0x1000u;
    f.pump();
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutA), 7u);                  // int(0) 的语义值
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutB), 0xFFFFFFFFu);         // int(7) = 0xFFFFFFFF（int32 = -1）
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutC), kAtUndef);            // int(8) 越界 ⇒ undefined
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutN), kInstrLocalTarget);   // ptr(0) = 地址原样
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF0), 9u);                 // ptrDword(0) = 语义 9
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF1), kAtUndef);           // ptr(2) 越界
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF2), kAtUndef);           // ptrDword(1)：指针 0
    // ★ S2c-2 复审 F9：小数 index ⇒ TypeError（⛔ 不截断成 int(0) —— 那会读到相邻槽）。
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF3), 1u);
}

// `plugin/tick-stale`：Tick 存进模块变量，下一次 **call() 入口**里 `local.*` ⇒ stale_tick；
//   `now` 快照仍读旧值（不抛）；`log` 仍可用。
void caseTickStale() {
    lift_test::g_current_case = "plugin/tick-stale";
    Fixture f;
    if (!loadAtChain(f, "stale")) return;
    LIFT_CHECK(installInstrFrame(f.mem));
    f.pump();
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    dispatchInstr(f, kInstrPos);  // handler 把 Tick 存进模块变量
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    const std::uint64_t base = f.host->logLines();

    f.readings.pos = 0x1000u;
    f.pump();  // onTrigger（call 入口）：local.int ⇒ stale；now.pos 是旧值；log 成功
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutA), 1u);       // 抛了、消息含 stale_tick
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutB), kInstrPos);// now 是快照（上一拍的 0x100）
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutC), 1u);       // log 仍可用
    LIFT_CHECK(f.host->logLines() > base);                 // "late log" 真的打出来了
}

// =====================================================================================
// S2c-2b · Tick 的写面（`edit` / `input` / `patch`）+ stale 写 + once 的 Tick
//
// ★ 判据口径：tick handler 只写**模块变量**（tick 入口没有 `ctx.edit` 之外的回写面），回读统一
//   在下一个 `onTrigger`（`put`）或 C++ 直接读 guest/队列 —— 两者都是行为读数。
// =====================================================================================
constexpr std::uint32_t kTickWriteTarget = 0xb150;  // 夹具的 `TICK_TARGET`（int 池内）

// `plugin/tick-edit`：tick 里 `edit.global.set` ⇒ 读点后 guest int 变；同一 handler 里
//   `ctx.fail` 仍 TypeError（"写面不是把 in_callback_ 打开"的唯一可判读数）。
void caseTickEdit() {
    lift_test::g_current_case = "plugin/tick-edit";
    Fixture f;
    if (!loadAtChain(f, "tick-edit")) return;
    LIFT_CHECK(installInstrFrame(f.mem));
    f.pump();  // start：装订阅
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->instrHookList().size()), 1);

    dispatchInstr(f, kInstrPos);
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    // ★ Edit = **立即持久写**：探针还没返回，guest int 已经是 42。
    LIFT_CHECK_EQ_U32(readFieldInt(f, kTickWriteTarget), 42u);

    f.readings.pos = 0x1000u;
    f.pump();  // onTrigger：把回执 + `ctx.fail` 的结果写回 guest
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutA), 1u);  // accepted
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutB), 1u);  // applied
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutC), 0u);  // queued == false（Edit 不排队）
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutN), 1u);  // ctx.fail ⇒ TypeError（写面没打开 in_callback_）
}

// `plugin/tick-input`：tick 里 `input.click` ⇒ 回执 queued、下一读点 applied。
void caseTickInput() {
    lift_test::g_current_case = "plugin/tick-input";
    Fixture f;
    if (!loadAtChain(f, "tick-input")) return;
    LIFT_CHECK(installInstrFrame(f.mem));
    f.pump();
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());

    dispatchInstr(f, kInstrPos);
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    // ★ tick 不是读点 ⇒ 只入队（staged），不落地。
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->queue().depth()), 1);

    f.readings.pos = 0x1000u;
    f.pump();  // 本读点：② 提升 → ⑥ 落地 click；回调里读到的仍是 queued
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutA), 1u);  // accepted
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutB), 1u);  // queued
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutC), 0u);  // applied == false（回执时刻）
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutN), 1u);  // 有 id（> 0）
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF0), 1u); // handler 内 status == "queued"
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF1), 1u); // onTrigger 内（drain 之前）仍 queued
    const std::uint32_t click_id = readFieldInt(f, kAtOutF2);
    LIFT_CHECK(click_id > 0);
    // ★ 下一读点落地：pump 之后宿主队列记 Applied。
    LIFT_CHECK(f.host->queue().status(click_id) == lift::plugin::IntentQueue::Status::Applied);
}

// `plugin/tick-stale-write`：旧 Tick 的 `edit` / `input` / `patch` ⇒ `stale_tick` 抛、guest/队列零变化。
void caseTickStaleWrite() {
    lift_test::g_current_case = "plugin/tick-stale-write";
    Fixture f;
    if (!loadAtChain(f, "stale-write")) return;
    LIFT_CHECK(installInstrFrame(f.mem));
    LIFT_CHECK(seedFieldInt(f, kTickWriteTarget, 0u));  // 零变化要有基线（ENC(0) ≠ 零内存）
    f.pump();
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());

    dispatchInstr(f, kInstrPos);  // handler 把 Tick 存进模块变量
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->queue().depth()), 0);

    f.readings.pos = 0x1000u;
    f.pump();  // onTrigger（call 入口，不是 tick 入口）：三个写面都必须 stale
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutA), 1u);  // edit 抛
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutB), 1u);  // 消息含 stale_tick
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutC), 1u);  // input 抛
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutN), 1u);
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF0), 1u);  // patch 抛
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF1), 1u);
    // ★ 门在写之前：guest 一个字节都没动、队列里一条都没有。
    LIFT_CHECK_EQ_U32(readFieldInt(f, kTickWriteTarget), 0u);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->queue().depth()), 0);
}

// `plugin/tick-patch-cas`：tick 里 `patch({localInt:0},5)` ⇒ 该条指令读到 5；窗口里"别人写"
//   ⇒ CAS 不写回、`restore_conflicts += 1`。`max`/越界 ⇒ 回执（另见 `tick-patch-limit`）。
void caseTickPatchCas() {
    // ---- ① 正常窗口：恢复回最初 raw（7），`restores == 1`、`conflicts == 0` ----
    lift_test::g_current_case = "plugin/tick-patch-cas";
    {
        Fixture f;
        if (!loadAtChain(f, "tick-patch")) return;
        LIFT_CHECK(installInstrFrame(f.mem));
        LIFT_CHECK(installInstrLocalValues(f.mem));
        f.pump();
        dumpFail(f);
        LIFT_CHECK(!f.host->failed());
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->instrHookList().size()), 1);

        dispatchInstr(f, kInstrPos);  // handler 设 one-shot；probe 在 handler **之后**应用
        dumpFail(f);
        LIFT_CHECK(!f.host->failed());
        LIFT_CHECK_EQ_U32(readInstrLocalInt(f, 0u), 5u);  // ★ 该条指令读到 5（修前 = 7）

        f.readings.pos = 0x1000u;
        f.pump();  // phase 0：回执 + 重新 arm
        dumpFail(f);
        LIFT_CHECK(!f.host->failed());
        LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutA), 1u);  // accepted
        LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutB), 1u);  // applied
        LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutC), 0u);  // 没有 code
        LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF1), 7u);  // handler 里读到的是**打补丁前**的 7

        dispatchInstr(f, kInstrPos + 8u);  // 下一条指令派发 ⇒ 恢复
        dumpFail(f);
        LIFT_CHECK(!f.host->failed());
        LIFT_CHECK_EQ_U32(readInstrLocalInt(f, 0u), 7u);  // 最初 raw
        f.readings.pos = 0x1000u;
        f.pump();  // phase 1：stats
        dumpFail(f);
        LIFT_CHECK(!f.host->failed());
        LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutN), 1u);   // restores == 1
        LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF0), 0u);  // restore_conflicts == 0
    }
    // ---- ② 窗口里"别人写" ⇒ CAS 不写回、`restore_conflicts += 1` ----
    {
        Fixture f;
        if (!loadAtChain(f, "tick-patch")) return;
        LIFT_CHECK(installInstrFrame(f.mem));
        LIFT_CHECK(installInstrLocalValues(f.mem));
        f.pump();
        dumpFail(f);
        LIFT_CHECK(!f.host->failed());

        dispatchInstr(f, kInstrPos);
        dumpFail(f);
        LIFT_CHECK(!f.host->failed());
        LIFT_CHECK_EQ_U32(readInstrLocalInt(f, 0u), 5u);
        // 另一条执行流写同一地址（明文也行：CAS 只比 4 字节原样）。
        LIFT_CHECK(put32(f.mem, kInstrIntBase, 0xDEADBEEFu));
        f.readings.pos = 0x1000u;
        f.pump();  // phase 0
        dumpFail(f);
        LIFT_CHECK(!f.host->failed());

        dispatchInstr(f, kInstrPos + 8u);
        dumpFail(f);
        LIFT_CHECK(!f.host->failed());
        LIFT_CHECK_EQ_U32(f.mem.rd32(kInstrIntBase), 0xDEADBEEFu);  // 别人的写还在
        f.readings.pos = 0x1000u;
        f.pump();  // phase 1
        dumpFail(f);
        LIFT_CHECK(!f.host->failed());
        LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutN), 0u);   // restores == 0
        LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF0), 1u);  // ★ restore_conflicts == 1
    }
}

// `plugin/tick-patch-limit`：9 个不同地址 ⇒ 前 8 条生效、第 9 条回执 `too_many_slots` + `skips += 1`；
//   越界 target ⇒ `bad_target`（⛔ 不静默变成 0 写）。
void caseTickPatchLimit() {
    lift_test::g_current_case = "plugin/tick-patch-limit";
    Fixture f;
    if (!loadAtChain(f, "tick-patch-limit")) return;
    LIFT_CHECK(installInstrFrame(f.mem));
    LIFT_CHECK(installInstrLocalValues(f.mem));
    f.pump();
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->instrHookList().size()), 10);

    dispatchInstr(f, kInstrPos);  // 10 条同点订阅一条链（每个 handler 调 next）
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    for (std::uint32_t i = 0; i < 8u; ++i) {
        LIFT_CHECK_EQ_U32(readInstrLocalInt(f, i), 0x500u + i);  // 前 8 个地址生效
    }
    LIFT_CHECK_EQ_U32(readInstrLocalInt(f, 0u), 0x500u);  // 首个 = 0x500
    LIFT_CHECK_EQ_U32(f.mem.rd32(kInstrLocalTarget), lift::encodeInt(9u, kFxKey));  // 第 9 个没写

    f.readings.pos = 0x1000u;
    f.pump();
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutA), 8u);   // 8 条 accepted
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutB), 1u);   // 1 条 too_many_slots
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutC), 1u);   // 1 条 bad_target（越界）
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutN), 10u);  // 10 条都调用了 `tick.patch`
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF0), 1u);  // 第 9 条的 entry.skips == 1
}

// `plugin/tick-once-tick-writable`：`once` resolve 的 `.then` 里 `tick.local` / `tick.edit` 可用
//   （continuation 在同一入口的 drain 里 ⇒ Tick 还没 stale）。
void caseTickOnceTickWritable() {
    lift_test::g_current_case = "plugin/tick-once-tick-writable";
    Fixture f;
    if (!loadAtChain(f, "once-tick-writable")) return;
    LIFT_CHECK(installInstrFrame(f.mem));
    LIFT_CHECK(installInstrLocalValues(f.mem));
    LIFT_CHECK(seedFieldInt(f, kTickWriteTarget, 0u));
    f.pump();
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());

    dispatchInstr(f, kInstrPos);  // once 命中 ⇒ resolve + `.then` 在本入口 drain 里跑
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_U32(readFieldInt(f, kTickWriteTarget), 0x77u);  // .then 里的 tick.edit 生效

    f.readings.pos = 0x1000u;
    f.pump();
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutA), 1u);  // onceRan
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutB), 7u);  // local.int(0)（installInstrLocalValues）
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutC), 1u);  // tick.edit accepted && applied
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutN), 0u);  // 没抛
}


// `plugin/at-multi-same-pos`：同点 4 条（at×2 + v1 onTick + once）⇒ 一条链；
//   截断只影响 at 参与者；once 的 `.then` 在**同一入口的 drain** 里跑；`duplicate` 只对 v1 生效。
void caseAtMultiSamePos() {
    lift_test::g_current_case = "plugin/at-multi-same-pos";
    Fixture f;
    if (!loadAtChain(f, "multi-same-pos")) return;
    LIFT_CHECK(installInstrFrame(f.mem));
    f.pump();  // start：4 条全都装上（同点不互斥）
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    const std::vector<lift::InstrHookStatus> hooks = f.host->instrHookList();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(hooks.size()), 4);
    if (hooks.size() != 4) return;
    const std::uint32_t h1 = hooks[0].id;
    const std::uint32_t h2 = hooks[1].id;
    const std::uint32_t v1 = hooks[2].id;
    LIFT_CHECK(hooks[0].hook_kind == lift::InstrHookKind::Sub);
    LIFT_CHECK(hooks[1].hook_kind == lift::InstrHookKind::Sub);
    LIFT_CHECK(hooks[2].hook_kind == lift::InstrHookKind::V1At);
    LIFT_CHECK(hooks[3].hook_kind == lift::InstrHookKind::Once);

    dispatchInstr(f, kInstrPos);  // 一次派发命中 4 条
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    // once 命中即撤；剩下 3 条（h1 / h2 / v1）各 hits == 1。
    const std::vector<lift::InstrHookStatus> after = f.host->instrHookList();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(after.size()), 3);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(instrHits(h1)), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(instrHits(h2)), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(instrHits(v1)), 1);

    f.readings.pos = 0x1000u;
    f.pump();  // onTrigger：写回 order / onceRan / kind，然后撤（v1.clear + off at）
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutA), 1u);  // h1
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutB), 2u);  // h2（它不 next，但自己是链尾）
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutC), 99u); // v1 onTick（被动，不受截断影响）
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutN), 3u);
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF0), 1u); // once 的 .then 在同一入口的 drain 里跑了
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF1), 3u); // 表里剩 h1 / h2 / v1
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF2), 1u);
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF3), 1u);
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF4), 1u);
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF5), 1u);
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF6), 1u);
}

// `plugin/at-keeps-run`：`start` 返回 null + 一条 at ⇒ Run 靠订阅存活；handler 里 off(self)
//   ⇒ 再一个读点后收口。红了 = `hasPendingWork` 不含 at_subs ⇒ 第一拍就收口、表被清空。
void caseAtKeepsRun() {
    lift_test::g_current_case = "plugin/at-keeps-run";
    Fixture f;
    if (!loadAtChain(f, "keeps-run")) return;
    LIFT_CHECK(installInstrFrame(f.mem));
    f.pump();  // start 返回 null ⇒ runner done，但 at 订阅让 Run 活着
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK(f.host->runner().done());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->instrHookList().size()), 1);
    LIFT_CHECK(f.host->readout().find("run=waiting") != std::string::npos);

    f.pump();  // 再来一个读点：仍活着（只靠 at_subs 这一支）
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->instrHookList().size()), 1);
    LIFT_CHECK(f.host->readout().find("run=waiting") != std::string::npos);

    dispatchInstr(f, kInstrPos);  // handler：ctx.off(self) ⇒ 订阅没了
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK(f.host->instrHookList().empty());

    f.pump();  // 没有待推工作 ⇒ 收口
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK(f.host->readout().find("run=done") != std::string::npos);
    LIFT_CHECK(!lift::instrHooksActive());
    std::printf("  [%s] %s\n", lift_test::g_current_case, f.host->readout().c_str());
}

// `plugin/at-next-outside`：把 `next` 存下来、在 handler 返回之后（下一个 call() 入口）调用
//   ⇒ 抛 TypeError（消息含 `next_outside_chain`）；插件自己接住 ⇒ Run 不崩、能收口。
//   红了 = `next()` 在链外静默生效 / 静默 no-op（"我以为我继续了、其实没有"）。
void caseAtNextOutside() {
    lift_test::g_current_case = "plugin/at-next-outside";
    Fixture f;
    if (!loadAtChain(f, "next-outside")) return;
    LIFT_CHECK(installInstrFrame(f.mem));
    f.pump();  // start：一条 at + armed trigger
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->instrHookList().size()), 1);

    dispatchInstr(f, kInstrPos);  // handler 把 next 存进模块变量（⛔ 不在 handler 里调）
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());

    f.readings.pos = 0x1000u;
    f.pump();  // onTrigger：链外调 next ⇒ 抛；夹具自己接住 + 写回 + off
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());                    // ★ 观察者用错 API 不 fail Run
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutA), 1u);  // 确实抛了
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutB), 1u);  // 消息含 next_outside_chain
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutC), 1u);  // 抛的是 TypeError
    LIFT_CHECK(f.host->instrHookList().empty());

    f.pump();  // 再一个读点：订阅没了 ⇒ 收口（与 at-off 同一节奏）
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK(f.host->readout().find("run=done") != std::string::npos);
}

// `plugin/tick-valid-during-drain`：handler 里排的 microtask 在**同一次入口的 drain** 里跑 ⇒
//   Tick 仍有效（`local.int(0)` 读得到、不 stale）。
//   红了 = `ticks.clear()` 被挪到 drain 之前（老 Tick 在 continuation 里已经失效）。
void caseTickValidDuringDrain() {
    lift_test::g_current_case = "plugin/tick-valid-during-drain";
    Fixture f;
    if (!loadAtChain(f, "drain-tick-local")) return;
    LIFT_CHECK(installInstrFrame(f.mem));
    LIFT_CHECK(installInstrLocalValues(f.mem));
    f.pump();  // start：一条 at
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->instrHookList().size()), 1);

    dispatchInstr(f, kInstrPos);  // handler 排 microtask；本入口末尾的 drain 跑到它
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());

    f.readings.pos = 0x1000u;
    f.pump();  // onTrigger：写回 drain 里那次 local 读的读数
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutA), 7u);   // local.int(0) 的语义值
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutB), 0u);   // 没有 stale_tick
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutC), 1u);   // microtask 确实跑了（不是"没跑也绿"）
}

// =====================================================================================
// S2c-2 · `where` 过滤的五种形态（区间 / 整本脚本 / opcode / mix）
//
// 红 = JS 面没放开 / 放开后匹配错 / `hook.list()` 的形状与装进去的不一致：
//   · `plugin/at-where-range` ：`[lo,hi)` 的四个边界（lo 命中 / hi-1 命中 / hi 不命中 / lo-1 不命中）。
//   · `plugin/at-where-script`：`{script}` 两不同 pos 都命中；换脚本 ⇒ 0 命中。
//   · `plugin/at-where-opcode`：`{opcode:114}` 两条不同 pos 都命中；`{script,opcode}` 只在该脚本；
//     未提交 pc ⇒ 不命中（⛔ 不编 0）；`hook.list()` 的 form/script/opcode 逐项对。
//   · `plugin/at-where-mixed` ：同拍三种形态按注册序成一条链（`next()` 顺序 = [1,2,3]）。
// =====================================================================================
constexpr std::uint32_t kAtRangeHePos = 0x0107u;  // hi - 1
constexpr std::uint32_t kAtRangeHiPos = 0x0108u;  // hi（开区间 ⇒ 不命中）
constexpr std::uint32_t kAtRangeLoMinus1 = 0x00FFu;

// `plugin/at-where-range`：`{script,lo,hi}` = `[lo,hi)` 的四个边界 + `hook.list()` 的形状面逐字段。
void caseAtWhereRange() {
    lift_test::g_current_case = "plugin/at-where-range";
    Fixture f;
    if (!loadAtChain(f, "where-range")) return;
    LIFT_CHECK(installInstrFrame(f.mem));
    f.pump();  // start：一条 {script,lo,hi}
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    const std::vector<lift::InstrHookStatus> hooks = f.host->instrHookList();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(hooks.size()), 1);
    if (hooks.size() != 1) return;
    const std::uint32_t h = hooks[0].id;
    LIFT_CHECK(hooks[0].hook_kind == lift::InstrHookKind::Sub);
    LIFT_CHECK(hooks[0].form == lift::InstrHookForm::Range);
    LIFT_CHECK_EQ_U32(hooks[0].lo, kInstrPos);
    LIFT_CHECK_EQ_U32(hooks[0].hi, kAtRangeHiPos);
    LIFT_CHECK_EQ_U32(hooks[0].pos, 0u);  // 非 Pos 形态：pos 保持 0（form 决定哪个字段有意义）
    LIFT_CHECK(hooks[0].script == "FIELD.BIN");

    // 边界四点：lo-1 ⇒ 0；lo ⇒ 1；hi-1 ⇒ 2；hi ⇒ 仍 2。
    dispatchInstr(f, kAtRangeLoMinus1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(instrHits(h)), 0);
    dispatchInstr(f, kInstrPos);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(instrHits(h)), 1);
    dispatchInstr(f, kAtRangeHePos);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(instrHits(h)), 2);
    dispatchInstr(f, kAtRangeHiPos);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(instrHits(h)), 2);

    f.readings.pos = 0x1000u;
    f.pump();  // onTrigger：写回 handler 的读数（只该跑 2 次）
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutA), 2u);      // whereRan
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutB), kInstrPos);  // 第一次命中的 now.pos
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutC), kAtRangeHePos);// 第二次命中的 now.pos
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutN), 2u);
    // ★ S2c-2 复审 F4：`hook.list()` 的 JS 观测面（`hookStatusObject`）逐字段 —— 修前 JS 面只断
    //   `kind` / `hits`，`form/lo/hi/opcode` 只在 C++ 的 `InstrHookStatus` 上断过。
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF0), 1u);  // 表里恰好这一行
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF1), 1u);  // kind === "sub"
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF2), 1u);  // form === "range"
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF3), kInstrPos);      // lo
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF4), kAtRangeHiPos);  // hi
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF5), 0u);             // opcode（非 Opcode 形态 = 0）
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF6), 0u);             // pos（非 Pos 形态 = 0）
    LIFT_CHECK(f.host->instrHookList().empty());
}

// `plugin/at-where-script`：`{script}` = 整本脚本的每条指令；换脚本 ⇒ 0 命中。
void caseAtWhereScript() {
    lift_test::g_current_case = "plugin/at-where-script";
    Fixture f;
    if (!loadAtChain(f, "where-script")) return;
    LIFT_CHECK(installInstrFrame(f.mem));
    f.pump();  // start：一条 {script}
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    const std::vector<lift::InstrHookStatus> hooks = f.host->instrHookList();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(hooks.size()), 1);
    if (hooks.size() != 1) return;
    const std::uint32_t h = hooks[0].id;
    LIFT_CHECK(hooks[0].form == lift::InstrHookForm::Script);
    LIFT_CHECK(hooks[0].script == "FIELD.BIN");
    LIFT_CHECK_EQ_U32(hooks[0].pos, 0u);

    // 同一脚本的两条不同指令都命中（整本脚本，不是单点）。
    dispatchInstr(f, kInstrPos);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(instrHits(h)), 1);
    dispatchInstr(f, kInstrPos + 0x10u);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(instrHits(h)), 2);

    // 换脚本 ⇒ 0 命中（同一个 pos 也不命中：判据是脚本身份，不是位置）。
    LIFT_CHECK(setInstrScript(f, kInstrOtherId));
    dispatchInstr(f, kInstrPos);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(instrHits(h)), 2);

    // 回到原脚本 ⇒ 又命中（反面自检：上一步不是"整条订阅坏了"）。
    LIFT_CHECK(setInstrScript(f, kInstrScriptId));
    dispatchInstr(f, kInstrPos + 0x20u);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(instrHits(h)), 3);

    f.readings.pos = 0x1000u;
    f.pump();
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutA), 3u);          // whereRan
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutB), kInstrPos);
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutC), kInstrPos + 0x10u);
    LIFT_CHECK(f.host->instrHookList().empty());
}

// `plugin/at-where-opcode`：全局 `{opcode}` vs `{script,opcode}`；未提交 pc ⇒ 不命中
//   （★ 带 `{opcode:0}` 的订阅：读失败若"编 0"会假命中）；`hook.list()` 逐字段。
void caseAtWhereOpcode() {
    lift_test::g_current_case = "plugin/at-where-opcode";
    Fixture f;
    if (!loadAtChain(f, "where-opcode")) return;
    LIFT_CHECK(installInstrFrame(f.mem));
    LIFT_CHECK(putInstrOpcode(f, kInstrPos + 0x10u, kInstrOpcode));  // 第二条指令同 opcode
    f.pump();  // start：三条（全局 opcode + 带 script 的 opcode + opcode 0）
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    const std::vector<lift::InstrHookStatus> hooks = f.host->instrHookList();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(hooks.size()), 3);
    if (hooks.size() != 3) return;
    const std::uint32_t all = hooks[0].id;
    const std::uint32_t scoped = hooks[1].id;
    const std::uint32_t zero = hooks[2].id;  // ★ `{opcode:0}`（"编 0"的判据）
    LIFT_CHECK(hooks[0].form == lift::InstrHookForm::Opcode);
    LIFT_CHECK(hooks[0].script.empty());  // {opcode} 不带 script
    LIFT_CHECK_EQ_U32(hooks[0].opcode, kInstrOpcode);
    LIFT_CHECK_EQ_U32(hooks[0].pos, 0u);
    LIFT_CHECK(hooks[1].form == lift::InstrHookForm::Opcode);
    LIFT_CHECK(hooks[1].script == "FIELD.BIN");  // {script,opcode}
    LIFT_CHECK_EQ_U32(hooks[1].opcode, kInstrOpcode);
    LIFT_CHECK(hooks[2].form == lift::InstrHookForm::Opcode);
    LIFT_CHECK(hooks[2].script.empty());
    LIFT_CHECK_EQ_U32(hooks[2].opcode, 0u);

    // 两条不同 pos 都命中（opcode 是判据，不是位置）。
    dispatchInstr(f, kInstrPos);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(instrHits(all)), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(instrHits(scoped)), 1);
    dispatchInstr(f, kInstrPos + 0x10u);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(instrHits(all)), 2);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(instrHits(scoped)), 2);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(instrHits(zero)), 0);  // 114 ≠ 0

    // 换脚本、opcode 相同 ⇒ 全局那条命中、`{script,opcode}` **不**命中。
    LIFT_CHECK(setInstrScript(f, kInstrOtherId));
    dispatchInstr(f, kInstrPos);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(instrHits(all)), 3);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(instrHits(scoped)), 2);

    // 未提交 pc（`rd32` 读不到）⇒ **不命中**（⛔ 不编 0）。★ `{opcode:0}` 这条就是"编 0"的判据：
    //   订阅的 opcode 恰好是 0，读失败若被当成 0 就会命中（修前 `op_h` 用 0x72 区分不出来）。
    LIFT_CHECK(setInstrScript(f, kInstrScriptId));
    constexpr std::uint32_t kNoOpcodePos = lift::kScriptHeaderLen + 0xB0000u;
    dispatchInstr(f, kNoOpcodePos);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(instrHits(all)), 3);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(instrHits(scoped)), 2);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(instrHits(zero)), 0);  // ★ 编 0 ⇒ 这里会红

    // opcode 不是 114（这里是 0）⇒ 114 的两条都不命中；★ `{opcode:0}` **真读到 0 就命中**
    //   （正控制：证明上面那个 0 不是"订阅是死的"）。
    LIFT_CHECK(putInstrOpcode(f, kInstrPos, 0u));
    dispatchInstr(f, kInstrPos);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(instrHits(all)), 3);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(instrHits(scoped)), 2);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(instrHits(zero)), 1);

    f.readings.pos = 0x1000u;
    f.pump();
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutA), 3u);  // 全局 opcode 的 handler 次数
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutB), 2u);  // 带 script 的 handler 次数
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutN), 1u);  // {opcode:0} 的 handler 次数（正控制）
    // ★ S2c-2 复审 F4/F10：`hook.list()` 的 JS 观测面逐字段（两形态 + opcode 0）。
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF0), 1u);          // 表里 3 行
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF1), 1u);          // hooks[0].form === "opcode"
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF2), kInstrOpcode);// hooks[0].opcode
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF3), 1u);          // hooks[1].form + script 对
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF4), kInstrOpcode);// hooks[1].opcode
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF5), 1u);          // hooks[2].form === "opcode"
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF6), 0u);          // hooks[2].opcode === 0
    LIFT_CHECK(f.host->instrHookList().empty());
}

// `plugin/at-where-mixed`：同拍 `{script}` + `{script,pos}` + `{opcode}` 按注册序成**一条链**。
void caseAtWhereMixed() {
    lift_test::g_current_case = "plugin/at-where-mixed";
    Fixture f;
    if (!loadAtChain(f, "where-mixed")) return;
    LIFT_CHECK(installInstrFrame(f.mem));
    LIFT_CHECK(putInstrOpcode(f, kInstrPos, kInstrOpcode));
    f.pump();  // start：按注册序装 {script} → {script,pos} → {opcode}
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    const std::vector<lift::InstrHookStatus> hooks = f.host->instrHookList();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(hooks.size()), 3);
    if (hooks.size() != 3) return;
    LIFT_CHECK(hooks[0].form == lift::InstrHookForm::Script);
    LIFT_CHECK(hooks[1].form == lift::InstrHookForm::Pos);
    LIFT_CHECK(hooks[2].form == lift::InstrHookForm::Opcode);
    LIFT_CHECK(hooks[0].hook_kind == lift::InstrHookKind::Sub);
    LIFT_CHECK(hooks[1].hook_kind == lift::InstrHookKind::Sub);
    LIFT_CHECK(hooks[2].hook_kind == lift::InstrHookKind::Sub);
    LIFT_CHECK(hooks[0].id < hooks[1].id && hooks[1].id < hooks[2].id);

    dispatchInstr(f, kInstrPos);  // 三条都命中 ⇒ 一条链
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    for (const lift::InstrHookStatus& row : f.host->instrHookList()) {
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(row.hits), 1);
    }

    f.readings.pos = 0x1000u;
    f.pump();
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutA), 1u);  // {script} 先跑
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutB), 2u);  // 然后 {script,pos}
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutC), 3u);  // 最后 {opcode}
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutN), 3u);
    LIFT_CHECK(f.host->instrHookList().empty());
}

// `plugin/at-where-big`（S2c-2 复审 F10）：`{script,lo,hi}` 的 `lo/hi > 2^31` 必须**原样**从
//   `hook.list()` 读回（uint32 语义；`newInt32` 出口会把 0x90000000 读成负数）。
//   红了 = JS 出口按 int32 截断（`row.lo === 0x90000000` 为 false）。
void caseAtWhereBig() {
    lift_test::g_current_case = "plugin/at-where-big";
    Fixture f;
    if (!loadAtChain(f, "where-big")) return;
    LIFT_CHECK(installInstrFrame(f.mem));
    f.pump();  // start：一条大区间订阅（没有任何派发 ⇒ 它的 handler 不该跑）
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    const std::vector<lift::InstrHookStatus> hooks = f.host->instrHookList();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(hooks.size()), 1);
    if (hooks.size() != 1) return;
    LIFT_CHECK(hooks[0].form == lift::InstrHookForm::Range);
    LIFT_CHECK_EQ_U32(hooks[0].lo, 0x90000000u);  // C++ 侧本来就是 uint32
    LIFT_CHECK_EQ_U32(hooks[0].hi, 0x90000010u);

    f.readings.pos = 0x1000u;
    f.pump();
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutA), 0x90000000u);  // JS 读回 lo
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutB), 0x90000010u);  // JS 读回 hi
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutC), 1u);           // form === "range"
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutN), 0u);           // 没有派发 ⇒ handler 没跑
    // ★ 判据（不能用"回写 guest 再读回"代替：0x90000000 与 int32 -1879048192 回写都是同一 u32）。
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF0), 1u);          // row.lo === 2415919104
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF1), 1u);          // row.hi === 2415919120
    LIFT_CHECK(f.host->instrHookList().empty());
}

// `plugin/at-where-null-field`（S2c-2 复审 F1）：`where` 的键**存在但值是 null/undefined** ⇒
//   TypeError；`hook.list()` **不新增条目**（⛔ 不静默退化成 `{script}` 整本订阅）。
//   红了 = `{script,pos:undefined}` 变成 `{script}`（表里多一条、且每拍命中 —— 成本面最大的形态）。
void caseAtWhereNullField() {
    lift_test::g_current_case = "plugin/at-where-null-field";
    Fixture f;
    if (!loadAtChain(f, "where-null-field")) return;
    LIFT_CHECK(installInstrFrame(f.mem));
    f.pump();  // start：6 条非法形态各抛 + 1 条合法订阅
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    // ★ 判据 ①（宿主侧，不依赖 JS 写回）：6 条非法形态一条都不许装进来。
    const std::vector<lift::InstrHookStatus> hooks = f.host->instrHookList();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(hooks.size()), 1);
    if (!hooks.empty()) LIFT_CHECK(hooks[0].form == lift::InstrHookForm::Pos);

    f.readings.pos = 0x1000u;
    f.pump();
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutA), 6u);  // 6 条都抛
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutB), 6u);  // 抛的是 TypeError
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutC), 1u);  // 表里只剩正控制那条
    LIFT_CHECK(readFieldInt(f, kAtOutN) > 0u);        // 正控制拿到了 id（合法面没坏）
    LIFT_CHECK(f.host->instrHookList().empty());
}

// `plugin/tick-patch-once-tick`（S2c-2 复审 F2）：`once` 的 Tick 调 `tick.patch` ⇒ 回执
//   `code:"once_tick"`（⛔ 不是 `no_such_hook`），guest 零变化；同一个 `.then` 里 `local` / `edit` 仍可用。
//   红了 = 回执是 `no_such_hook`（条目已撤的间接读数）或 patch 真的落进了 guest。
void caseTickPatchOnceTick() {
    lift_test::g_current_case = "plugin/tick-patch-once-tick";
    Fixture f;
    if (!loadAtChain(f, "once-tick-patch")) return;
    LIFT_CHECK(installInstrFrame(f.mem));
    LIFT_CHECK(installInstrLocalValues(f.mem));
    LIFT_CHECK(seedFieldInt(f, kTickWriteTarget, 0u));
    f.pump();
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());

    dispatchInstr(f, kInstrPos);  // once 命中 ⇒ resolve + `.then` 在本入口 drain 里跑
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_U32(readFieldInt(f, kTickWriteTarget), 0x77u);  // `.then` 里的 tick.edit 仍生效
    LIFT_CHECK_EQ_U32(readInstrLocalInt(f, 0u), 7u);              // ★ patch 没有落地（仍是 7）
    dispatchInstr(f, kInstrPos + 8u);  // 下一拍：没有待恢复的槽 ⇒ 值原样
    LIFT_CHECK_EQ_U32(readInstrLocalInt(f, 0u), 7u);

    f.readings.pos = 0x1000u;
    f.pump();
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutA), 1u);  // onceRan
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutB), 1u);  // ★ code === "once_tick"
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutC), 0u);  // accepted === false（⛔ 不假装收下）
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutN), 7u);  // local.int(0) 仍可用
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF0), 1u); // tick.edit accepted && applied
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF1), 0u); // applied === false
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF2), 0u); // 回执带 code（不是抛、不是空回执）
    LIFT_CHECK_EQ_U32(readFieldInt(f, kAtOutF3), 0u); // 没有异常
}

// `plugin/at-tick-id-attribution`（S2c-1 复审 F2）：**两个 Impl 同活**时，一次派发的 `hits[]`
//   同时含两边的句柄；每个 Impl 的 `tick.id` 必须是**自己**那条订阅的句柄（⛔ 不是 `hits[0]`）。
//   红了 = 取值退回全局 `hits[0]` ⇒ 后注册的实例拿到别人的 id（文档/用户面误导）。
void caseAtTickIdAttribution() {
    lift_test::g_current_case = "plugin/at-tick-id-attribution";
    Fixture a;
    Fixture b;
    if (!loadAtChain(a, "tick-id")) return;
    if (!loadAtChain(b, "tick-id")) return;
    LIFT_CHECK(installInstrFrame(a.mem));
    LIFT_CHECK(installInstrFrame(b.mem));
    a.pump();  // A 的 start：装 hA
    b.pump();  // B 的 start：装 hB（两个 Impl 的 sink 都活着）
    dumpFail(a);
    dumpFail(b);
    LIFT_CHECK(!a.host->failed());
    LIFT_CHECK(!b.host->failed());
    const std::vector<lift::InstrHookStatus> hooks = lift::instrHookList();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(hooks.size()), 2);
    if (hooks.size() != 2) return;
    const std::uint32_t h_a = hooks[0].id;
    const std::uint32_t h_b = hooks[1].id;
    LIFT_CHECK(h_a < h_b);

    // 一次派发（用 A 的 guest）：两个 sink 都收到 [hA, hB] ⇒ 两边各自的 handler 各跑一次。
    dispatchInstr(a, kInstrPos);
    dumpFail(a);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(instrHits(h_a)), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(instrHits(h_b)), 1);

    // A 的 onTrigger 会收口（off + return []）⇒ releaseOwner 也撤掉同 owner 的 hB；
    // ★ 但 B 的 Tick 在 dispatch 里已经建好 / handler 已经跑过 ⇒ tick.id 仍然可判。
    a.readings.pos = 0x1000u;
    a.pump();
    dumpFail(a);
    LIFT_CHECK(!a.host->failed());
    b.readings.pos = 0x1000u;
    b.pump();
    dumpFail(b);
    LIFT_CHECK(!b.host->failed());

    LIFT_CHECK_EQ_U32(readFieldInt(a, kAtOutA), h_a);  // ★ A 的 tick.id = 自己的句柄
    LIFT_CHECK_EQ_U32(readFieldInt(a, kAtOutB), 1u);
    LIFT_CHECK_EQ_U32(readFieldInt(b, kAtOutA), h_b);  // ★ B 的 tick.id = 自己的句柄（修前 = h_a）
    LIFT_CHECK_EQ_U32(readFieldInt(b, kAtOutB), 1u);
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
    /** `profile_end` 的回传文本（S2：hud-equivalence 用它覆盖 `INVALID` 那一态）。 */
    std::string end_text;
};

bool probeProfileBegin(void* ctx, const char* /*label*/, std::string* /*err*/) {
    ++static_cast<ProfileProbe*>(ctx)->begins;
    return true;
}

bool probeProfileEnd(void* ctx, std::string* text, std::string* /*err*/) {
    auto* probe = static_cast<ProfileProbe*>(ctx);
    ++probe->ends;
    if (text != nullptr) *text = probe->end_text;
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
    // ★ S0（缺陷 4）：`start` 里装的 hook 是"render 没动过 hook 表"的**基准**。
    //   夹具（`stored-ctx.js`）会在 render 里对 `saved.hook.*` 与 start 那个句柄逐个调
    //   ⇒ **全部**必须 TypeError；表里那一行必须原样（id / patch / hits / restores 都不动）。
    const std::vector<lift::InstrHookStatus> hooks_before = f.host->instrHookList();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(hooks_before.size()), 1);
    // ② render：5 个 run-context 出口 + 7 个 hook 出口全部 TypeError（夹具自吞）；读数面仍然可用
    lift::ui::UiBuilder builder;
    lift::ui::UiFrameReadings readings;
    std::string err;
    const bool ok = f.host->script() != nullptr && f.host->script()->render(builder, readings, &err);
    if (!ok) std::fprintf(stderr, "  render error: %s\n", err.c_str());
    LIFT_CHECK(ok);
    LIFT_CHECK(builder.elements().empty());
    // ★ S0：render 期一条 hook 出口都没被接受（表逐字段不变 —— 不是"抛了但已经生效"）。
    {
        const std::vector<lift::InstrHookStatus> hooks_after = f.host->instrHookList();
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(hooks_after.size()),
                          static_cast<std::int64_t>(hooks_before.size()));
        if (hooks_before.size() == 1 && hooks_after.size() == 1) {
            LIFT_CHECK_EQ_U32(hooks_after[0].id, hooks_before[0].id);
            LIFT_CHECK(hooks_after[0].patch == hooks_before[0].patch);
            LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(hooks_after[0].hits),
                              static_cast<std::int64_t>(hooks_before[0].hits));
            LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(hooks_after[0].restores),
                              static_cast<std::int64_t>(hooks_before[0].restores));
        }
    }
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
// S2 · `plugin/render-profile`：`ctx.profile.begin/end` 在 **render** 里可用
//   ★ 判据：① 能调到宿主（计数）；② 返回 `undefined`（与 RunCtx 的 `ctx.profile` 同形）；
//     ③ 宿主失败 ⇒ TypeError（文案点名 `ctx.profile.begin: <why>`，⛔ 静默 no-op）。
//   ★ 反证（存下来的 RunCtx 的 `profile.*` 在 render 里仍被拒）在
//     `plugin/render-stored-runctx-rejected` —— 两份 trampoline 的边界。
// =====================================================================================
struct ProfileResultFake {
    int begins = 0;
    int ends = 0;
    bool begin_ok = true;
    std::string begin_err;
    bool end_ok = true;
    std::string end_err;
};

bool resultProfileBegin(void* ctx, const char* /*label*/, std::string* err) {
    auto* fake = static_cast<ProfileResultFake*>(ctx);
    ++fake->begins;
    if (err != nullptr) *err = fake->begin_err;
    return fake->begin_ok;
}

bool resultProfileEnd(void* ctx, std::string* text, std::string* err) {
    auto* fake = static_cast<ProfileResultFake*>(ctx);
    ++fake->ends;
    if (text != nullptr) text->clear();
    if (err != nullptr) *err = fake->end_err;
    return fake->end_ok;
}

std::string soleText(const lift::ui::UiBuilder& builder) {
    for (const lift::ui::Element& e : builder.elements()) {
        if (e.kind == lift::ui::ElementKind::Text) return e.text;
    }
    return {};
}

void caseRenderProfile() {
    lift_test::g_current_case = "plugin/render-profile";
    struct Run {
        const char* mode;
        bool begin_ok;
        bool end_ok;
        std::int64_t want_begins;
        std::int64_t want_ends;
        const char* want_prefix;
    };
    const Run runs[] = {
        // ★ S2 复审 I3：`end()` 返回结果对象（`ok:text` 是 `text`，这里夹具清空文本）。
        {"ok", true, true, 1, 1, "ok:"},
        // begin 抛（void 契约）⇒ 夹具提前返回，`end` 一次都不调。
        {"fail-begin", false, true, 1, 0, "begin-threw: ctx.profile.begin: already-open"},
        // end 失败**不抛**：回 `{ok:false,error}`（结果对象）。
        {"fail-end", true, false, 0, 1, "end-result:false:not-open"},
    };
    for (const Run& run : runs) {
        Fixture f;
        LIFT_CHECK(f.mem_ok && f.win_ok);
        if (!f.mem_ok || !f.win_ok) return;
        ProfileResultFake fake;
        fake.begin_ok = run.begin_ok;
        fake.begin_err = "already-open";
        fake.end_ok = run.end_ok;
        fake.end_err = "not-open";
        lift::ui::UiHostOps ops{};
        ops.ctx = &fake;
        ops.profile_begin = &resultProfileBegin;
        ops.profile_end = &resultProfileEnd;
        const bool loaded = f.load("render-profile.js", {{"mode", run.mode}}, &ops);
        LIFT_CHECK(loaded);
        if (!loaded) {
            std::fprintf(stderr, "  load error (%s): %s\n", run.mode, f.load_error.c_str());
            continue;
        }
        LIFT_CHECK(f.host->script() != nullptr && f.host->script()->hasRender());
        lift::ui::UiBuilder builder;
        lift::ui::UiFrameReadings readings;
        std::string err;
        const bool ok = f.host->script() != nullptr && f.host->script()->render(builder, readings, &err);
        if (!ok) std::fprintf(stderr, "  render error (%s): %s\n", run.mode, err.c_str());
        LIFT_CHECK(ok);
        const std::string text = soleText(builder);
        ++lift_test::g_checks;
        if (text.rfind(run.want_prefix, 0) != 0) {
            lift_test::failf(__FILE__, __LINE__, "render-profile(%s)：文本应是 `%s…`，得到 `%s`", run.mode,
                             run.want_prefix, text.c_str());
        }
        // 宿主侧计数：按 mode 精确判（fail-begin 不 end；fail-end 只 end 一次）。
        LIFT_CHECK_EQ_I64(fake.begins, run.want_begins);
        LIFT_CHECK_EQ_I64(fake.ends, run.want_ends);
    }
}

// =====================================================================================
// S2 · `plugin/hud-equivalence`：`scripts/ui/hud.js` 的**旧 loader** 与**插件 loader**
//   在同一份 readings 下产出**逐字段相同**的 display list（含行序 / 颜色 / key / bold）。
//   ★ 这是"--overlay 的像素/行序不变"的**确定性**判据（不需要 GPU / 窗口 / 墙钟）。
//   ★ 两份实例都从**同一份源码文件**装载（`LIFT_UI_SCRIPT_DIR/hud.js`）⇒ 改 hud.js 两边一起变，
//     ⛔ 不可能"旧版一套、新版一套"。
//   ★ PROFILE 的点击帧也对比（begin / end 成功两拍；第二拍带 `INVALID` 文本 ⇒ 四态里的
//     `INVALID（已复制）` 两边都必须出现 —— S2 复审 I3 的回传路）。
// =====================================================================================
bool sameHudList(const std::vector<lift::ui::Element>& a, const std::vector<lift::ui::Element>& b,
                 std::string* why) {
    if (a.size() != b.size()) {
        *why = "元素数不同：" + std::to_string(a.size()) + " vs " + std::to_string(b.size());
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const lift::ui::Element& x = a[i];
        const lift::ui::Element& y = b[i];
        const bool same = x.kind == y.kind && x.x == y.x && x.y == y.y && x.w == y.w && x.h == y.h &&
                          x.text == y.text && x.key == y.key && x.font_size == y.font_size &&
                          x.bold == y.bold && x.fill.r == y.fill.r && x.fill.g == y.fill.g &&
                          x.fill.b == y.fill.b && x.fill.a == y.fill.a && x.color.r == y.color.r &&
                          x.color.g == y.color.g && x.color.b == y.color.b && x.color.a == y.color.a &&
                          x.z == y.z && x.seq == y.seq;
        if (!same) {
            *why = "第 " + std::to_string(i) + " 个元素不同（kind/text/几何/颜色/key/bold 之一）";
            return false;
        }
    }
    return true;
}

void caseHudEquivalence(lift::FontEngine* font) {
    lift_test::g_current_case = "plugin/hud-equivalence";
    LIFT_CHECK(font != nullptr);
    if (font == nullptr) return;
    const std::string path = std::string(LIFT_UI_SCRIPT_DIR) + "/hud.js";
    std::string source;
    if (!readHostFile(path, &source)) {
        LIFT_CHECK(false);
        return;
    }
    // ---- ① 同一份 readings 的**普通帧**（frame/mouse/fps/script/worst/frames）----
    auto runtime = lift::ui::makeQuickJsRuntime();
    std::string err;
    std::unique_ptr<lift::ui::UiScript> classic = runtime->load(source, path, font, &err);
    if (classic == nullptr) {
        std::fprintf(stderr, "  classic load error: %s\n", err.c_str());
        LIFT_CHECK(false);
        return;
    }
    ProfileProbe probe;
    lift::ui::UiHostOps ops{};
    ops.ctx = &probe;
    ops.profile_begin = &probeProfileBegin;
    ops.profile_end = &probeProfileEnd;
    Fixture plugin;
    const bool loaded = plugin.loadFile(path, {}, &ops);
    LIFT_CHECK(loaded);
    if (!loaded) {
        std::fprintf(stderr, "  plugin load error: %s\n", plugin.load_error.c_str());
        return;
    }
    plugin.host->setFont(font);
    LIFT_CHECK(plugin.host->script() != nullptr);

    RenderFrameSource frames = renderFrames();
    lift::ui::UiFrameReadings r;
    r.frame = 1234;
    r.mouse_ok = true;
    r.mouse_x = 812;
    r.mouse_y = 622;
    r.mouse_buttons_ok = true;
    r.mouse_left = true;
    r.fps_ok = true;
    r.fps = 59.9;
    r.script_ok = true;
    r.script = "startup";
    r.worst_ok = true;
    r.worst_wall_ms = 33.3;
    r.worst_guest_ok = true;
    r.worst_guest_ms = 21.5;
    r.worst_shim_ms = 4.2;
    r.frames = &frames;
    r.host = &ops;
    // ★ D4：PROFILE 默认隐藏 ⇒ 普通帧先给 `overlayKeySeq = 0`（基线，按钮不在场）；
    //   下面再喂一帧 `1` 把它打开（两边都吃同一份读数 ⇒ 等价仍可判）。
    r.overlay_key_ok = true;
    r.overlay_key_seq = 0;

    lift::ui::UiBuilder a;
    lift::ui::UiBuilder b;
    std::string cerr;
    std::string perr;
    LIFT_CHECK(classic->render(a, r, &cerr));
    LIFT_CHECK(plugin.host->script()->render(b, r, &perr));
    LIFT_CHECK(a.elements().size() >= 7);  // 面板 + HUD 行 + 设置入口（不许"两边都空"地等价）
    ++lift_test::g_checks;
    {
        std::string why;
        if (!sameHudList(a.elements(), b.elements(), &why)) {
            lift_test::failf(__FILE__, __LINE__, "hud 普通帧不等价：%s", why.c_str());
        }
    }

    // ---- ② PROFILE 点击帧（begin → end，成功两拍）----
    //   ★ D4：按钮默认隐藏 ⇒ 先让两边各吃一帧 `overlayKeySeq 0→1`（计数变了才切显隐）。
    lift::ui::UiFrameReadings show = r;
    show.frame = 1235;
    show.overlay_key_seq = 1;
    {
        lift::ui::UiBuilder ca;
        lift::ui::UiBuilder pb;
        LIFT_CHECK(classic->render(ca, show, &cerr));
        LIFT_CHECK(plugin.host->script()->render(pb, show, &perr));
        ++lift_test::g_checks;
        std::string why;
        if (!sameHudList(ca.elements(), pb.elements(), &why)) {
            lift_test::failf(__FILE__, __LINE__, "hud 显示 PROFILE 的那一帧不等价：%s", why.c_str());
        }
        ++lift_test::g_checks;
        if (pb.elements().size() <= b.elements().size()) {
            lift_test::failf(__FILE__, __LINE__, "overlayKeySeq 0→1 之后 PROFILE 按钮没出现（元素数没变多）");
        }
    }
    lift::ui::UiFrameReadings click = r;
    click.overlay_key_ok = true;   // `show` 之后一直是 1 ⇒ 不再切换，按钮留在场
    click.overlay_key_seq = 1;
    click.click_ok = true;
    click.click_key = "profile";
    click.click_x = 10;
    click.click_y = 10;
    bool status_seen = false;
    bool invalid_seen = false;
    for (int hit = 1; hit <= 2; ++hit) {
        click.frame = 1234 + static_cast<std::uint64_t>(hit);
        if (hit == 2) {
            // ★ S2 复审 I3：报告文本里有 `INVALID` ⇒ 两边都必须画 `INVALID（已复制）`。
            probe.end_text = "=== profile window: overlay ===\nunaccounted ... INVALID";
        }
        lift::ui::UiBuilder ca;
        lift::ui::UiBuilder pb;
        LIFT_CHECK(classic->render(ca, click, &cerr));
        LIFT_CHECK(plugin.host->script()->render(pb, click, &perr));
        ++lift_test::g_checks;
        std::string why;
        if (!sameHudList(ca.elements(), pb.elements(), &why)) {
            lift_test::failf(__FILE__, __LINE__, "hud PROFILE 第 %d 拍不等价：%s", hit, why.c_str());
        }
        for (const lift::ui::Element& e : pb.elements()) {
            if (e.kind != lift::ui::ElementKind::Text || e.text.rfind("profile: ", 0) != 0) continue;
            status_seen = true;
            if (hit == 2 && e.text == "profile: INVALID（已复制）") invalid_seen = true;
        }
    }
    // 状态行确实进了 list（⛔ 不是"两边都没画"地等价）；`INVALID` 那一态也真的画出来了。
    LIFT_CHECK(status_seen);
    LIFT_CHECK(invalid_seen);
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

// =====================================================================================
// S1b · `plugin/ts-module`：**入口 `.ts` 经 `initFromFile` 路径装载**（宿主按扩展名加
//   `JS_EVAL_FLAG_TYPESCRIPT`）；`lib/helper.ts` 走 `moduleLoad`（按**被导入模块名**的扩展名）。
//
// 判据（红了意味着什么）：
//   ① 插件入口没带 flag ⇒ `entry.ts` 在 `interface` / 注解处语法错（装载失败）；
//   ② `moduleLoad` 没带 flag ⇒ `lib/helper.ts` 被当 JS 解析（装载失败）；
//   ③ helper 的运行期导出没被用到 ⇒ start 里提交的 click 坐标不是 helper 算出来的 (311,207)；
//   ④ Run 没以 done 收口 / 回调不是恰好 1 次。
// ★ ⛔ 只断言"没报错"：坐标 + `run=done` + log 行数都是可判读数（click 在**下一读点**落地）。
// =====================================================================================
void caseTsModule() {
    lift_test::g_current_case = "plugin/ts-module";
    Fixture f;
    LIFT_CHECK(f.mem_ok && f.win_ok);
    if (!f.mem_ok || !f.win_ok) return;
    // ★ **路径形式**（不是 id）：与生产 `--plugin <路径>` 同一条 `initFromFile`。
    const std::string path = (fs::path(LIFT_PLUGIN_FIXTURES_DIR) / "ts-smoke" / "entry.ts").string();
    const bool loaded = f.loadFile(path);
    LIFT_CHECK(loaded);
    if (!loaded) {
        std::fprintf(stderr, "  load error: %s\n", f.load_error.c_str());
        return;
    }
    f.pump();  // start：helper.describe(…) → ctx.log；helper.point() → click(311,207) 入队
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());
    LIFT_CHECK(f.host->script() != nullptr && f.host->script()->hasRun());
    LIFT_CHECK(f.host->runner().done());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->callbacks()), 1);
    // helper 的 describe() 结果真的经 `ctx.log` 出去了（⛔ 不是"没报错"）。
    LIFT_CHECK(f.host->logLines() >= 1u);
    const std::string line = f.host->readout();
    LIFT_CHECK(line.find("run=done") != std::string::npos);
    // 下一读点：start 里提交的 click 落地 ⇒ 坐标 = helper.ts 的运行期导出。
    int cx = 0;
    int cy = 0;
    f.pump();
    f.cursor(&cx, &cy);
    LIFT_CHECK_EQ_I64(cx, 311);
    LIFT_CHECK_EQ_I64(cy, 207);
    std::printf("  [%s] %s\n", lift_test::g_current_case, line.c_str());
}

// =====================================================================================
// S1b · `plugin/js-comparison-unchanged`：`.js` **不带** `JS_EVAL_FLAG_TYPESCRIPT`。
//   `f < a > (b)` 在 flags-on 下会被读成泛型调用 `f<a>(b)`（上游自认的唯一语义变化）；
//   本仓的 `.js` 必须仍按比较解析 —— `r` 是 false（泛型调用会得到 `f(5)` = 15）。
// ★ 这是"`.js` 行为不变"的**最小判据**：本切片只按扩展名加 flag ⇒ `.js` 永远 flags=0。
// =====================================================================================
void caseJsComparisonUnchanged() {
    lift_test::g_current_case = "plugin/js-comparison-unchanged";
    Fixture f;
    LIFT_CHECK(f.mem_ok && f.win_ok);
    if (!f.mem_ok || !f.win_ok) return;
    const bool loaded = f.load("comparison-call.js");
    LIFT_CHECK(loaded);
    if (!loaded) {
        std::fprintf(stderr, "  load error: %s\n", f.load_error.c_str());
        return;
    }
    f.pump();
    dumpFail(f);
    LIFT_CHECK(!f.host->failed());  // 夹具在 `r !== false` 时 `ctx.fail` ⇒ 这里红
    LIFT_CHECK(f.host->runner().done());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(f.host->callbacks()), 1);
}

// =====================================================================================
// S1b · `plugin/ts-uppercase-ext`：扩展名判据 **ASCII 大小写不敏感**（`.TS` / `.MTS`）。
//   ★ 用**内存源码 + origin 字符串**（⛔ 不落文件）：大小写只在宿主判据里折叠，与当前卷是否
//     大小写敏感无关 ⇒ 这条在 Linux 上也成立。
//   ★ 源码含只有 TS 才能编过的语法（`interface` + 注解）⇒ 不带 flag 必失败。
// =====================================================================================
void caseTsUppercaseExtension() {
    lift_test::g_current_case = "plugin/ts-uppercase-ext";
    const char* const kSource =
        "interface Probe { value?: number }\n"
        "const probe: Probe = { value: 1 };\n"
        "export default {\n"
        "    start() { return []; },\n"
        "    onTrigger() { return []; },\n"
        "};\n";
    for (const char* origin : {"UPPER.TS", "UPPER.MTS"}) {
        lift::ui::PluginLoadOptions opts;
        opts.origin = origin;
        std::string err;
        std::unique_ptr<lift::ui::PluginScript> plugin =
            lift::ui::makeQuickJsPlugin(kSource, opts, &err);
        ++lift_test::g_checks;
        if (plugin == nullptr) {
            lift_test::failf(__FILE__, __LINE__, "%s: 大写扩展名没拿到 TS flag：%s", origin, err.c_str());
        }
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
    // ---- S1b：`.ts` 直读（宿主按扩展名加 `JS_EVAL_FLAG_TYPESCRIPT`）----
    caseTsModule();
    // ---- S1b：扩展名判据 ASCII 大小写不敏感（`.TS` / `.MTS`）----
    caseTsUppercaseExtension();
    // ---- S1b：`.js` 不带 TS flag 的最小判据（`f<a>(b)` 仍是比较）----
    caseJsComparisonUnchanged();
    caseApiSurface();
    caseInitialState();
    caseConcurrentArm();
    caseChain();
    caseSaveMissingPrecheck();
    caseConfigRoundtrip();
    caseConfigHostPause();
    caseFieldSwitchApply();
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
    // ---- S0（缺陷 1：Run 收口的每条路径都要释放本 Run 的 hook）----
    caseHookReleaseOnFail();
    caseHookReleaseOnDone();
    caseHookReleaseOnBudget();
    // ---- S2a（指令点 → JS 的反向 sink + 每入口 drain + Run 收口）----
    casePointHit();
    casePointHitHandlerError();
    casePointHitAfterRunEnd();
    caseDrainPerEntry();
    caseDrainBudget();
    // ---- S2b（`ctx.once` / `ctx.task` / timeout 泵 / Run 存活）----
    caseOnceResolve();
    caseOnceTimeout();
    caseTaskAwait();
    caseTaskKeepsRun();
    caseTaskReject();
    casePumpBudget();
    // ---- S2b 复审收口（S2-1 / S2-2 / S2-3 / S2-4）----
    caseBareOnceKeepsRun();
    caseOnTickKeepsRun();
    caseOnceHookClear();
    caseTaskFinishThenReject();
    caseRenderAfterFinish();
    // ---- S2c（`ctx.at` 的同步订阅面 / 链 / next 截断 / off / Tick 只读面 / stale）----
    caseAtChainOrder();
    caseAtNextTruncates();
    caseAtOff();
    caseAtHandlerError();
    caseTickNow();
    caseTickLocal();
    caseTickStale();
    // ---- S2c-2b（Tick 的写面：edit / input / patch + stale 写 + once 的 Tick）----
    caseTickEdit();
    caseTickInput();
    caseTickStaleWrite();
    caseTickPatchCas();
    caseTickPatchLimit();
    caseTickOnceTickWritable();
    // ---- S2c-2 复审收口（F2 once 的 Tick 不许 patch / F1 where 的 null 字段）----
    caseTickPatchOnceTick();
    caseAtMultiSamePos();
    caseAtKeepsRun();
    // ---- S2c-1 复审收口（F5 next 的链外调用 / F4 Tick 含 drain / F7 消息）----
    caseAtNextOutside();
    caseTickValidDuringDrain();
    // ---- S2c-2（`where` 过滤的五种形态 + 多 Impl 的 `tick.id` 归属）----
    caseAtWhereRange();
    caseAtWhereScript();
    caseAtWhereOpcode();
    caseAtWhereMixed();
    // ---- S2c-2 复审收口（F1 null/undefined 字段 / F10 > 2^31 的 lo/hi）----
    caseAtWhereNullField();
    caseAtWhereBig();
    caseAtTickIdAttribution();
    // ---- C2b（render / ui / draw；pure 半）----
    caseRenderSurface();
    caseUiDraw(&font);
    caseUiState();
    caseRenderStoredRunCtx();
    caseRenderBudgetDeadline();
    caseRenderRead();
    caseRenderReadings();
    // ---- S2（`hud` 变插件的前置：RenderCtx.profile + 双 loader 等价）----
    caseRenderProfile();
    caseHudEquivalence(&font);
    return lift_test::finish("plugin-runtime");
}
