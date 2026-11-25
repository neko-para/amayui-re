// scripts/plugins/scenario-fellow-switch.js —— **复现**「载入存档 009 → 进 FELLOW → 立绘来回切换 3 轮（6 次）」
//
// 它是 `scripts/recipes/fellow-switch.plan` 的**取代件**（那个 `.plan` 的文件头已注明）。为什么换掉配方：
//   · 配方第 4 步之前用的是**硬编码行 y=626**（只在读档界面**第 1 页** `LOAD 000..009` 成立）；
//   · `saves/SAVE.DAT` + `RT.DAT` 在 2026-11-22 09:33 被重写（并新增 `SAVE10`）⇒ 读档界面进屏的**持久化页号变成 1**
//     （第一屏 = `LOAD 010..019`）⇒ 那一行点空 ⇒ `await position SBUNKI` 超时 / `exit 5` / `pending=51`；
//   · 用户的既定口径：**「`loadsave` 里面应该要自行判断到底是哪一页」** ⇒ 翻页这件事属于 `save.js` 的 `loadSave`
//     （它自己读页号 `0x138e`、按更短方向点左右大箭头、等页号真的变、翻完等列表刷新），⛔ 不在场景里重算坐标。
//   ⇒ 本文件 = `bootToTitle` → `loadSave(9, { expect })` → 本场景（三层 `route`，见下）。
//
// 用法（cwd = apps/emulator-lift；**今天的 `saves/`** 即可，不需要 `git archive` 还原 2026-11-21 那份）：
//   scripts/clean-appdata.sh ../../.tmp/fellow-js/appdata-1
//   ./out/emulator-lift-aot.exe --game-dir ../../links/install --appdata ../../.tmp/fellow-js/appdata-1 \
//     --present headless --skip-logo --audio off --time-limit 240 \
//     --plugin scripts/plugins/scenario-fellow-switch.js
//
// 时序 / 门（**全部来自 `scripts/recipes/fellow-switch.plan` 已取证的那几个读数**，⛔ 本文件不另立数值；位置值只在
// `./lib/script-positions.js` 里写一次，坐标是本文件的 1280×720 参考系常量）：
//   TITLE  回调体 `[0x0460,0x0520)`          → 载入屏（`boot-to-title.js`）
//   SAVE   回调体 `[0x1104,0x11C4)`          → 列表就绪；页号由 `loadSave` 自己判断（`save.js`）
//   SBUNKI 读段之后 `[0x0B18,0x0C58)`        → 「是」(640,323)
//   REIGN  回调体 `[0x2048,0x2234)`          → 目录屏就绪（前置门，不出手）
//   REIGN  读输入段 `[0x2214,0x2234)`        → ★ 右键 (640,360) 进 FELLOW（**单条** `click … "right"`）
//   FELLOW 输入读段 `[0x1684,0x16A4)`        → 切人点 (330,130) 阿瓦罗 / (400,130) 菲亚
//   切换后的转场链（两方向都实测过）：`click` → `SETTOOLLIST` → 回 `FELLOW`
//   ⇒ 窗口 = `click` → 等 `SETTOOLLIST` → 等回 `FELLOW` → 等 FELLOW 主轮询体 `[0x1570,0x15A8)` → `profile.end`
//   （`profile.begin/end` 夹住的窗口名沿用配方里的 `s1-to-avaro` / `s1-to-fia` / `s2-…` / `s3-…`，
//     方便与 `.tmp/fellow-profile/` 那份已入库读数（`docs/03-emulator/perf-fellow-switch.md` §3.2）对齐）
//   ⛔ 全篇零 `wait` / 零 `afterDelay`：每一道等都是 guest 可见状态门。
//
// ★ 右键为什么**不**手写 down/up，也**不**在回调体那一拍直接点：
//   · `.plan` 当年要 `mouse_down` → 等回主轮询体 → 等下一趟读段 → `mouse_up`，因为单发 `click … right`
//     实测会丢（抬起落在**同一个** guest 回调里 ⇒ 回调体的电平状态机没看见按下）。
//   · 插件这条路上**没有**分开发 press/release 的手段：`ctx.input.down/up` 根本不施加
//     （`src/runtime/intent_executor.cpp` 的 `IntentStep`：`MouseDown`/`MouseUp` 是 C1 的**不施加**档）
//     ⇒ 只剩一条 `ctx.input.click(x, y, "right")` 可用。
//   · ⇒ 点的那一拍**往后挪到回调尾的读输入段**：按下在回调尾落地、抬起要等之后两个 KeyLevel 读点
//     （`PointerCore::releaseDue` 闸门①）⇒ 按下必然跨过至少一个完整 guest 回调，也就是 `.plan`
//     那个形状压成一条 click。★ 这是本文件对"哪一拍点"的**唯一**新增判断依据。
//   ⚠ 已知缺口（⛔ 本文件改不了，属宿主）：插件路的 `IntentExecutor` 喂给 `PointerCore` 时
//     `callback_boundary=false` / `frames=0`（`src/runtime/intent_executor.cpp`）⇒ 闸门②'（按下必须跨过
//     一个完整回调，提交 `4f8aa5c`）**只对 `--input-plan` 生效**，插件点击只受闸门①的**读点个数**约束。
//     这就是"必须把点的那一拍挪到读段"的原因；把帧线接进 `IntentExecutor` 是另一张单的事。
//
// ★ 验证读数（2026-11-22 20:1x–20:4x / win32 / `main` `509199d` / `--present headless` /
//   `out/emulator-lift-aot.exe` sha256 `06EFB3E9…`（3 次 run 前后未变）/ 今天的 `saves/`）：
//   · **在回调体那一拍直接点**（第 1 版）：`run=finished exit=5`、`reason=scenario-fellow: gate timeout
//     (scenario:fellow:0)` —— 与 `.plan` 当年 4/5 的失效臂同形（按下与抬起落在同一个回调里）。
//   · **改到读输入段那一拍**：3/3 全绿，每次的日志关键行 = `page 1 (target 0, slot 9)` → `page 0 reached`
//     → `reached REIGN.BIN` → 6 次 `profile_end(s…-to-…)` + 6 个 `=== profile window: … ===`
//     → `6 次切换全部走完` → `run=finished exit=0`（进程 exit 0）。
//   ⚠ 这 3 次只证明"这个形状在这台机器上连过 3 次"，不是"结构上不可能被吞"——闸门②' 那条仍然缺。
//
// ★ 结构（同 `./scenario-save10-field.js`）：**三层 `route`，前缀两两不相交**
//     boot:     启动到 TITLE            （`./lib/boot-to-title.js`）
//     load:     载入存档 + 翻页 + 终态  （`./lib/save.js` 的 `loadSave`；前提是已经在 TITLE）
//     scenario: 载入之后的场景动作      （本文件）
//   外层剥 `boot:`，内层剥 `load:`，剩下全是 `scenario:`。
// ★ **无状态**：进度只在 `ev.name`（宿主每回调新建实例也能接旧链 —— `plugin-runtime.md` §4.6；
//   ⛔ 不把进度塞进闭包 / `ctx.state`）。

import { bootToTitle } from "./lib/boot-to-title.js";
import { route } from "./lib/chains.js";
import { loadSave } from "./lib/save.js";
import {
    FELLOW_POLL_HI,
    FELLOW_POLL_LO,
    FELLOW_READ_INPUT_HI,
    FELLOW_READ_INPUT_LO,
    REIGN_CALLBACK_HI,
    REIGN_CALLBACK_LO,
    REIGN_READ_INPUT_HI,
    REIGN_READ_INPUT_LO,
} from "./lib/script-positions.js";

/** @typedef {import("@amayui/lift-plugin").RunCtx} RunCtx */
/** @typedef {import("@amayui/lift-plugin").Trigger} Trigger */
/** @typedef {import("@amayui/lift-plugin").FiredEvent} FiredEvent */
/** @typedef {import("@amayui/lift-plugin").TimeoutEvent} TimeoutEvent */
/** @typedef {FiredEvent | TimeoutEvent} PluginEvent */
/**
 * 场景这一段链：形状 = `PluginModule` 的 `start` / `onTrigger`（与 `./lib/chains.js` 的 `Chain` 同形）。
 *
 * @typedef {object} Chain
 * @property {(ctx: RunCtx) => Trigger | Trigger[] | null} start
 * @property {(ctx: RunCtx, ev: PluginEvent) => Trigger | Trigger[] | null} onTrigger
 */

// marker 身份 = **文件名**（含 `.BIN`；C1 的门是精确 `==`，⛔ 不剥后缀）。
// ★ `SETTOOLLIST.BIN` 只当**脚本身份**用（`afterScript`），没有一个取证过的位置区间 ⇒ 它不是位置常量。
const SCRIPT_REIGN = "REIGN.BIN";
const SCRIPT_FELLOW = "FELLOW.BIN";
const SCRIPT_SETTOOLLIST = "SETTOOLLIST.BIN";

// 槽 9 = 存档 `SAVE09.DAT`（用户给的操作序列的第 ① 步；页号由 `loadSave` 自己判断）。
const SLOT = 9;
// 载入**落点**由调用方按预期给（工具不猜）：`SAVE09` 存在**目录屏**（`REIGN.BIN`）。
// `FIELD.BIN` 也在表里 = 同一份存档集上若游戏先进地图再切目录，门不会漏；下一道门仍是 REIGN 的回调体，
// 所以"多认一个名字"不会把失败读成成功（⛔ 不是放宽判据：真正的进度判据是下面那道 REIGN 门）。
const EXPECT = ["REIGN.BIN", "FIELD.BIN"];

// 坐标（1280×720 参考系；与配方逐字同源，⛔ 不按"看起来像"改）。
const REIGN_MENU_X = 640; // 目录中心 = 右键进 FELLOW 的那一点
const REIGN_MENU_Y = 360;
const AVARO_X = 330; // 阿瓦罗
const FIA_X = 400; // 菲亚
const PORTRAIT_Y = 130; // 两个立绘按钮同一行

// 每道门的超时（与配方同档：载入后要等目录屏就绪，切一次要等转场链走完）。
const REIGN_TIMEOUT_MS = 180000;
const FELLOW_TIMEOUT_MS = 120000;
const SWITCH_TIMEOUT_MS = 60000;

// 6 次切换（顺序 = 配方 ⑤ 的 3 轮 × 2 个方向；窗口名逐字沿用 `s1-to-avaro` … `s3-to-fia`）。
// ⛔ 这是**数据**，不是进度：链上哪一步只看 `ev.name` 里的下标。
/** @type {ReadonlyArray<{ label: string, x: number }>} */
const SWITCHES = [
    { label: "s1-to-avaro", x: AVARO_X },
    { label: "s1-to-fia", x: FIA_X },
    { label: "s2-to-avaro", x: AVARO_X },
    { label: "s2-to-fia", x: FIA_X },
    { label: "s3-to-avaro", x: AVARO_X },
    { label: "s3-to-fia", x: FIA_X },
];

// 场景状态名一律 `scenario:` 前缀（`route` 只看前缀，三层两两不相交）；进度全在名字里。
const SCENARIO_PREFIX = "scenario:";
const REIGN_NAME = "scenario:reign";
const REIGN_CLICK_NAME = "scenario:reign-click";

/**
 * 从 `scenario:<kind>:<下标>` 里取下标；不是这个形状 / 越界 ⇒ -1（调用方按 `ctx.fail` 响亮拒绝）。
 *
 * @param {string} name
 * @param {string} kind
 * @returns {number}
 */
function stepOf(name, kind) {
    const parts = name.split(":");
    if (parts.length !== 3 || parts[0] !== "scenario" || parts[1] !== kind) return -1;
    const i = Number(parts[2]);
    if (!Number.isInteger(i) || i < 0 || i >= SWITCHES.length) return -1;
    return i;
}

/** @param {number} i @returns {string} */
const fellowName = (i) => `${SCENARIO_PREFIX}fellow:${i}`;
/** @param {number} i @returns {string} */
const toolName = (i) => `${SCENARIO_PREFIX}tool:${i}`;
/** @param {number} i @returns {string} */
const backName = (i) => `${SCENARIO_PREFIX}back:${i}`;
/** @param {number} i @returns {string} */
const settleName = (i) => `${SCENARIO_PREFIX}settle:${i}`;

/** @type {Chain} */
const scenario = {
    // 载入链到终态 ⇒ 本函数被调起（`onLoaded`）：此刻脚本已是 `REIGN.BIN`（或先到 `FIELD.BIN`）。
    start(ctx) {
        ctx.log("info", "scenario-fellow: save loaded ⇒ 等 REIGN 的输入回调体（目录屏就绪）");
        return ctx.trigger.afterPosition(SCRIPT_REIGN, REIGN_CALLBACK_LO, REIGN_CALLBACK_HI, {
            name: REIGN_NAME,
            timeoutMs: REIGN_TIMEOUT_MS,
        });
    },

    onTrigger(ctx, ev) {
        // 场景自己的门超时：响亮收口（⛔ 不静默 done）。⚠ 这里**不** `profile.end()`：超时那拍可能没有开着的
        // 窗口，硬关会抛错（`ctx.profile.end` 对"没开窗"是 TypeError）；半开的窗口随 Run 收口一起放着。
        if (ev.kind === "timeout") {
            ctx.finish(5, `scenario-fellow: gate timeout (${ev.name})`);
            return [];
        }
        const name = String(ev.name);

        if (name === REIGN_NAME) {
            // 第 1 拍：目录屏就绪（回调体）⇒ 先把光标放到目录中心，再等**它这一趟的读输入段**才点。
            ctx.log(
                "info",
                `scenario-fellow: REIGN 回调体到了 ⇒ move (${REIGN_MENU_X},${REIGN_MENU_Y}) 并等这一趟的读输入段`,
            );
            ctx.input.move(REIGN_MENU_X, REIGN_MENU_Y);
            return ctx.trigger.afterPosition(
                SCRIPT_REIGN,
                REIGN_READ_INPUT_LO,
                REIGN_READ_INPUT_HI,
                { name: REIGN_CLICK_NAME, timeoutMs: REIGN_TIMEOUT_MS },
            );
        }

        if (name === REIGN_CLICK_NAME) {
            // 第 2 拍：在**回调尾的读输入段**提交单条右键 —— 按下落在回调尾、抬起要等之后两个 KeyLevel 读点
            // ⇒ 按下跨过一个完整 guest 回调（`.plan` 的 `mouse_down` → 门 → 门 → `mouse_up` 的那个形状）。
            ctx.log(
                "info",
                `scenario-fellow: REIGN 读输入段到了 ⇒ right click (${REIGN_MENU_X},${REIGN_MENU_Y}) 进 FELLOW`,
            );
            ctx.input.click(REIGN_MENU_X, REIGN_MENU_Y, "right");
            return ctx.trigger.afterPosition(
                SCRIPT_FELLOW,
                FELLOW_READ_INPUT_LO,
                FELLOW_READ_INPUT_HI,
                { name: fellowName(0), timeoutMs: FELLOW_TIMEOUT_MS },
            );
        }

        const iFellow = stepOf(name, "fellow");
        if (iFellow >= 0) {
            const sw = SWITCHES[iFellow];
            ctx.log(
                "info",
                `scenario-fellow: FELLOW 输入读段到了 ⇒ profile_begin(${sw.label}) + click (${sw.x},${PORTRAIT_Y})`,
            );
            ctx.profile.begin(sw.label);
            ctx.input.click(sw.x, PORTRAIT_Y);
            return ctx.trigger.afterScript(SCRIPT_SETTOOLLIST, {
                name: toolName(iFellow),
                timeoutMs: SWITCH_TIMEOUT_MS,
            });
        }

        const iTool = stepOf(name, "tool");
        if (iTool >= 0) {
            ctx.log("info", "scenario-fellow: SETTOOLLIST 到了 ⇒ 等回到 FELLOW");
            return ctx.trigger.afterScript(SCRIPT_FELLOW, {
                name: backName(iTool),
                timeoutMs: SWITCH_TIMEOUT_MS,
            });
        }

        const iBack = stepOf(name, "back");
        if (iBack >= 0) {
            ctx.log("info", "scenario-fellow: 回到 FELLOW ⇒ 等主轮询体（切换完成后的稳态）");
            return ctx.trigger.afterPosition(SCRIPT_FELLOW, FELLOW_POLL_LO, FELLOW_POLL_HI, {
                name: settleName(iBack),
                timeoutMs: SWITCH_TIMEOUT_MS,
            });
        }

        const iSettle = stepOf(name, "settle");
        if (iSettle >= 0) {
            ctx.log("info", `scenario-fellow: 窗口收口 profile_end(${SWITCHES[iSettle].label})`);
            ctx.profile.end();
            const next = iSettle + 1;
            if (next < SWITCHES.length) {
                // 下一轮的第一件事与第一轮同形：等 FELLOW 的**输入读段**（点得进去的那一档）再点。
                return ctx.trigger.afterPosition(
                    SCRIPT_FELLOW,
                    FELLOW_READ_INPUT_LO,
                    FELLOW_READ_INPUT_HI,
                    { name: fellowName(next), timeoutMs: SWITCH_TIMEOUT_MS },
                );
            }
            ctx.log("info", `scenario-fellow: ${SWITCHES.length} 次切换全部走完`);
            ctx.finish(0, `scenario-fellow: ${SWITCHES.length} switches done`);
            return [];
        }

        ctx.fail(name, `unknown state: ${name}`);
        return [];
    },
};

// 载入链：`SAVE09` 落在 `REIGN.BIN`（`EXPECT` 见上）；到终态把控制权交给场景。
const load = loadSave(SLOT, {
    expect: EXPECT,
    onLoaded: (ctx) => scenario.start(ctx),
});

// 启动链：TITLE 命中 ⇒ 才拉起载入链（"进程启动"与"读取存档"是两件事 —— `boot-to-title.js` 头）。
const boot = bootToTitle({ onDone: (ctx) => load.start(ctx) });

// 内层：`load:` 前缀（含它的 timeout）归载入链，其余（`scenario:`）归场景。`boot:` 已由外层剥掉。
const afterBoot = route((ev) => String(ev.name).startsWith("load:"), load, scenario);

// 外层：`boot:` 归启动链，其余交给内层。★ 三层前缀两两不相交 —— `route` 只看 `isFirst(ev)`。
export default route((ev) => String(ev.name).startsWith("boot:"), boot, afterBoot);
