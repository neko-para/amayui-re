// scripts/plugins/scenario-save07-battle.js —— **可跑样例**：启动到 TITLE → 载入存档 007 → 结束回合 → 決定 → 进战斗
//
// 这就是「新场景怎么写」的样板：**不重写载入存档那一段**（那是 `save.js` 的 `loadSave`），
// 也不重写"启动到 TITLE"那一段（那是 `boot-to-title.js` 的 `bootToTitle`）；只写"载入完成之后"
// 要做的事，然后自己拿 `chains.js` 的 `route` 把三段拼起来（`save.js` 不认识场景，也不提供成品）。
//
// 跑法（cwd = apps/emulator-lift；appdata 必须用 `scripts/clean-appdata.sh` 生成的那一份 ——
// 它复制 `saves/SAVE07.*` 等，页基 = 0 ⇒ 槽 007 才点得中，见 `scripts/recipes/save07-battle.plan` 头）：
//   out\emulator-lift-aot.exe --plugin scripts/plugins/scenario-save07-battle.js ^
//     --game-dir ..\..\links\install --appdata <私有 appdata> ^
//     --skip-logo --audio off --present window --present-vsync 0 --time-limit 180 --frame-log
//
// ★ 为什么本文件必须在 `scripts/plugins/` **根**（S1 起）：宿主 QJS 的 import 根 = **入口文件所在目录**
//   （`src/ui/script_quickjs.cpp` 的 `moduleNormalize`），且解析结果必须在该根之下（`../` 越根被拒）。
//   ⇒ 入口在上、库在 `lib/`：本文件 `import "./lib/save.js"`（向下，合法），
//     `lib/` 里的库彼此 `./x.js`（同目录）。⛔ 入口不能住在库的下层（`../lib/...` 会红）。
//   同一原因：`--plugin` 的 id 只查这个根（非递归）、`lib/` 永远不是 id 目标。
// ★ **`expect` 由调用方按预期给**：这里 `['FIELD.BIN']` —— SAVE07 落在**地图**上，
//   战斗（`BTL.BIN`）是之后由用户动作（结束回合 / 決定）进的，不是载入落点。工具不猜。
// ★ **三段、三套前缀**（`route` 按名字分流，前缀必须两两不相交 —— `chains.js` 头）：
//     boot:     启动到 TITLE          （`boot-to-title.js`；冷启动跑才需要）
//     load:     载入存档 + 终态       （`save.js`；前提是已经在 TITLE）
//     battle:   载入之后的场景动作    （本文件）
//   ⇒ 外层 `route` 剥 `boot:`，内层 `route` 再剥 `load:`，剩下的就是 `battle:`。
// ★ 场景第一条动作是 **`hideBattleHelp(ctx)`**（`guest-globals.js`）：把「战斗中的帮助界面」那一位
//   （int global `0xb1b6` 的 `|0x10`）置位。**必须在载入之后、在 FIELD 上做** —— 载入会还原
//   脚本 / global 状态，在 TITLE 上置位会被随后的载入覆盖。
// ★ 门 / 坐标与 `scripts/recipes/save07-battle.plan` 的 ④–⑧ 是**同一组读数**（⛔ 不在这里另立数值）：
//   FIELD 主轮询体 → 结束回合 1220,45 → SELACT 主轮询体 → 決定 640,517 → BTL 主循环
//   （位置值只在 `./lib/script-positions.js` 里写一次；坐标是本文件的 1280×720 参考系常量）。
// ★ 全篇**零 `wait`**：每一道等都是 guest 可见状态门（零宿主墙钟、零时间等待）。
// ★ 无状态：进度只在 `ev.name`；宿主每回调新建实例也能接旧链（⛔ 不用 `ctx.state` 记步）。

import { bootToTitle } from "./lib/boot-to-title.js";
import { route } from "./lib/chains.js";
import { awaitPosition, failOnTimeout, failOnUnknown } from "./lib/gates.js";
import { hideBattleHelp } from "./lib/guest-globals.js";
import { loadSave } from "./lib/save.js";
import {
    BTL_MAIN_LOOP_HI,
    BTL_MAIN_LOOP_LO,
    FIELD_MAIN_LOOP_HI,
    FIELD_MAIN_LOOP_LO,
    SELACT_MAIN_LOOP_HI,
    SELACT_MAIN_LOOP_LO,
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

// ★ marker 身份 = **文件名**（含 `.BIN`；C1 的门是精确 `==`，⛔ 不剥后缀）。
const SCRIPT_FIELD = "FIELD.BIN";
const SCRIPT_SELACT = "SELACT.BIN";
const SCRIPT_BTL = "BTL.BIN";

// 坐标（1280×720 参考系；与 plan 同源）。
const TURN_END_X = 1220; // 右上角「ターン終了」（结束回合）
const TURN_END_Y = 45;
const DECIDE_X = 640; // 中间「決定」
const DECIDE_Y = 517;

// 每道门的超时（与 plan 同档：FIELD 读档后要等地图就绪，SELACT / BTL 要等敌方回合与资源加载）。
const FIELD_TIMEOUT_MS = 300000;
const SELACT_TIMEOUT_MS = 180000;
const BTL_TIMEOUT_MS = 180000;

/** @type {Chain} */
const scenario = {
    // 载入链到终态 ⇒ 本函数被调起（`onLoaded`）；返回的 trigger 就是新的 armed 集合。
    start(ctx) {
        // ★ 第一件事：关掉「战斗中的帮助界面」（此刻脚本已是 FIELD —— 载入已完成，不会被覆盖）。
        //   写失败只 warn + false（`guest-globals.js`）；这里**不**因此 fail：帮助气泡消失是体验项，
        //   不是"战斗没进去"，判据仍由下面那道 FIELD 门给。日志里必须能看到它的读数。
        const hidden = hideBattleHelp(ctx);
        ctx.log(
            "info",
            `scenario: save07 loaded ⇒ hideBattleHelp=${hidden} ⇒ waiting for FIELD main loop`,
        );
        return awaitPosition(
            ctx,
            SCRIPT_FIELD,
            FIELD_MAIN_LOOP_LO,
            FIELD_MAIN_LOOP_HI,
            "battle:field",
            FIELD_TIMEOUT_MS,
        );
    },

    onTrigger(ctx, ev) {
        // 场景自己的门超时：响亮收口（⛔ 不静默 done；也就不会"看起来跑完了"）。
        if (ev.kind === "timeout") {
            failOnTimeout(ctx, ev, "scenario");
            return [];
        }
        switch (ev.name) {
            case "battle:field":
                ctx.log("info", "scenario: FIELD main loop hit ⇒ turn-end (1220,45)");
                ctx.input.click(TURN_END_X, TURN_END_Y);
                return awaitPosition(
                    ctx,
                    SCRIPT_SELACT,
                    SELACT_MAIN_LOOP_LO,
                    SELACT_MAIN_LOOP_HI,
                    "battle:selact",
                    SELACT_TIMEOUT_MS,
                );
            case "battle:selact":
                ctx.log("info", "scenario: SELACT main loop hit ⇒ decide (640,517)");
                ctx.input.click(DECIDE_X, DECIDE_Y);
                return awaitPosition(
                    ctx,
                    SCRIPT_BTL,
                    BTL_MAIN_LOOP_LO,
                    BTL_MAIN_LOOP_HI,
                    "battle:btl",
                    BTL_TIMEOUT_MS,
                );
            case "battle:btl":
                ctx.log("info", "scenario: BTL main loop hit");
                ctx.finish(0, "scenario: battle reached");
                return [];
            default:
                failOnUnknown(ctx, ev);
                return [];
        }
    },
};

// 槽 7 = 存档 007（行 y 507，见 `save.js` 的 ROW_Y 实测表）。
const load = loadSave(7, {
    expect: ["FIELD.BIN"], // ★ 载入落点：调用方按预期给（工具不猜）
    onLoaded: (ctx) => scenario.start(ctx),
});

// 启动链：TITLE 命中 ⇒ 才拉起载入链（"进程启动"与"读取存档"是两件事，见 `boot-to-title.js` 头）。
const boot = bootToTitle({ onDone: (ctx) => load.start(ctx) });

// 内层：`load:` 前缀（含 timeout）归载入链，其余（`battle:`）归场景。`boot:` 已由外层剥掉。
const afterBoot = route((ev) => String(ev.name).startsWith("load:"), load, scenario);

// 外层：`boot:` 归启动链，其余交给内层。
export default route((ev) => String(ev.name).startsWith("boot:"), boot, afterBoot);
