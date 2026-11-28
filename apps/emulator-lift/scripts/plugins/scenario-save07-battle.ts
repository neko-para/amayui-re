// scripts/plugins/scenario-save07-battle.ts —— **可跑样例**：启动到 TITLE → 载入存档 007 → 结束回合 → 決定 → 进战斗
//
// 这就是「新场景怎么写」的样板：**不重写载入存档那一段**（那是 `save.ts` 的 `loadSave`），
// 也不重写"启动到 TITLE"那一段（那是 `boot-to-title.ts` 的 `bootToTitle`）；只写"载入完成之后"
// 要做的事，然后用**一条 `ctx.task` 的顺序 `await`** 把三段串起来（v2 没有 `route` / 具名链）。
//
// 跑法（cwd = apps/emulator-lift；appdata 必须用 `scripts/clean-appdata.sh` 生成的那一份 ——
// 它复制 `saves/SAVE07.*` 等，页基 = 0 ⇒ 槽 007 才点得中，见 `scripts/recipes/save07-battle.plan` 头）：
//   out\emulator-lift-aot.exe --plugin scripts/plugins/scenario-save07-battle.ts ^
//     --game-dir ..\..\links\install --appdata <私有 appdata> ^
//     --skip-logo --audio off --present window --present-vsync 0 --time-limit 180 --frame-log
//
// ★ 为什么本文件必须在 `scripts/plugins/` **根**（S1 起）：宿主 QJS 的 import 根 = **入口文件所在目录**
//   （`src/ui/script_quickjs.cpp` 的 `moduleNormalize`），且解析结果必须在该根之下（`../` 越根被拒）。
//   ⇒ 入口在上、库在 `lib/`：本文件 `import "./lib/save.ts"`（向下，合法），
//     `lib/` 里的库彼此 `./x.ts`（同目录）。⛔ 入口不能住在库的下层（`../lib/...` 会红）。
//   同一原因：`--plugin` 的 id 只查这个根（非递归）、`lib/` 永远不是 id 目标。
// ★ **`expect` 由调用方按预期给**：这里 `['FIELD.BIN']` —— SAVE07 落在**地图**上，
//   战斗（`BTL.BIN`）是之后由用户动作（结束回合 / 決定）进的，不是载入落点。工具不猜。
// ★ **三段 = 三次顺序 `await`**（v2）：
//     bootToTitle(t)                            启动到 TITLE（返回 TITLE 命中那一拍的 Tick）
//     loadSave(t, 7, { expect }, titleTick)     载入存档 + 终态（返回终态那一拍的 Tick）
//     本文件的 scenario.run(t, tick)            载入之后的场景动作
//   v1 的 `boot:` / `load:` / `battle:` 前缀分流（`chains.ts` 的 `route`）**已删**：前缀只留在
//   门名里（失败文案逐字沿用），不再参与路由。
// ★ 场景第一条动作是 **`hideBattleHelp(ctx, tick)`**（`guest-globals.ts`）：把「战斗中的帮助界面」那一位
//   （int global `0xb1b6` 的 `|0x10`）置位。**必须在载入之后、在 FIELD 上做** —— 载入会还原
//   脚本 / global 状态，在 TITLE 上置位会被随后的载入覆盖。
// ★ 门 / 坐标与 `scripts/recipes/save07-battle.plan` 的 ④–⑧ 是**同一组读数**（⛔ 不在这里另立数值）：
//   FIELD 主轮询体 → 结束回合 1220,45 → SELACT 主轮询体 → 決定 640,517 → BTL 主循环
//   （位置值只在 `./lib/script-positions.ts` 里写一次；坐标是本文件的 1280×720 参考系常量）。
// ★ 全篇**零 `wait`**：每一道等都是 guest 可见状态门（零宿主墙钟、零时间等待）。
// ★ **无模块级进度状态**：所有进度都在这一次 `ctx.task` 的局部变量里（v2 的 async 链就是状态机）。

import type { PluginModule, RunCtx, Tick } from "@amayui/lift-plugin";

import { bootToTitle } from "./lib/boot-to-title.ts";
import { awaitPosition } from "./lib/gates.ts";
import { hideBattleHelp } from "./lib/guest-globals.ts";
import { loadSave } from "./lib/save.ts";
import {
    BTL_MAIN_LOOP_HI,
    BTL_MAIN_LOOP_LO,
    FIELD_MAIN_LOOP_HI,
    FIELD_MAIN_LOOP_LO,
    SELACT_MAIN_LOOP_HI,
    SELACT_MAIN_LOOP_LO,
} from "./lib/script-positions.ts";

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

/**
 * 载入完成之后的场景动作（`tick` = `loadSave` 的终态 Tick；写面只在那一拍的有效期内）。
 */
async function run(ctx: RunCtx, tick: Tick): Promise<void> {
    // ★ 第一件事：关掉「战斗中的帮助界面」（此刻脚本已是 FIELD —— 载入已完成，不会被覆盖）。
    //   写失败只 warn + false（`guest-globals.ts`）；这里**不**因此 fail：帮助气泡消失是体验项，
    //   不是"战斗没进去"，判据仍由下面那道 FIELD 门给。日志里必须能看到它的读数。
    const hidden = hideBattleHelp(ctx, tick);
    log.info(`scenario: save07 loaded ⇒ hideBattleHelp=${hidden} ⇒ waiting for FIELD main loop`);
    tick = await awaitPosition(
        ctx,
        SCRIPT_FIELD,
        FIELD_MAIN_LOOP_LO,
        FIELD_MAIN_LOOP_HI,
        "battle:field",
        FIELD_TIMEOUT_MS,
        "scenario",
    );
    log.info("scenario: FIELD main loop hit ⇒ turn-end (1220,45)");
    tick.input.click(TURN_END_X, TURN_END_Y);
    tick = await awaitPosition(
        ctx,
        SCRIPT_SELACT,
        SELACT_MAIN_LOOP_LO,
        SELACT_MAIN_LOOP_HI,
        "battle:selact",
        SELACT_TIMEOUT_MS,
        "scenario",
    );
    log.info("scenario: SELACT main loop hit ⇒ decide (640,517)");
    tick.input.click(DECIDE_X, DECIDE_Y);
    await awaitPosition(
        ctx,
        SCRIPT_BTL,
        BTL_MAIN_LOOP_LO,
        BTL_MAIN_LOOP_HI,
        "battle:btl",
        BTL_TIMEOUT_MS,
        "scenario",
    );
    log.info("scenario: BTL main loop hit");
    ctx.finish(0, "scenario: battle reached");
}

const plugin = {
    start(ctx: RunCtx): void {
        // 槽 7 = 存档 007（行 y 507，见 `save.ts` 的 ROW_Y 实测表）；落点 = FIELD（工具不猜）。
        ctx.task(async (t) => {
            const titleTick = await bootToTitle(t);
            const terminal = await loadSave(t, 7, { expect: ["FIELD.BIN"] }, titleTick);
            await run(t, terminal);
        });
    },
} satisfies PluginModule;

export default plugin;
