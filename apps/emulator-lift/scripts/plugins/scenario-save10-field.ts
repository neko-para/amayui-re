// scripts/plugins/scenario-save10-field.ts —— **复现**「载入存档 10 → 在 FIELD 点 (540,450) → 退出」
//
// 三段组合（v2）：`bootToTitle(t)` → `loadSave(t, slot, …)` → 本文件的场景 —— 一条 `ctx.task` 的
// 顺序 `await`。它是**一条可复跑的复现入口**（不是产品场景）：所有翻页都走 `loadSave` 的真翻页
// （读 `0x138e` 页号 + 点左右大箭头 + 等页号真的变），⛔ 没有 `.plan`、⛔ 没有把存档改名。
//
// 时序（**这是复现的一部分**，别省）：`expect` 命中只代表 FIELD 脚本刚起来（地图还在加载）⇒
//   **先等 FIELD 的主轮询体 `[0x579C,0x5860)`**（= 地图就绪、在等输入），再 move → 一拍 → click。
//   直接点会被加载过程吞掉（实测：第一版在 `expect` 命中那一拍就点，什么也没发生）。
//
// 用法（cwd = apps/emulator-lift；存档 010/011/… 在**第 2 页**，翻页由 `save.ts` 自己做）：
//   ★ 参数来源（2026-11-25 起）：`<--appdata>/plugin-config.json` 的域 **`scenario.save10-field`**
//     （`slot` number / `expect` string[] 或逗号串），**优于** `--plugin-param`；没有 appdata 时
//     完全回退到 params。例：
//     {"version":1,"domains":{"scenario.save10-field":{"slot":10,"expect":["FIELD.BIN"]}}}
//   ./out/emulator-lift-aot.exe --exe ../../corpus/assets/age/AGE.EXE__dumped.sectfix.EXE \
//     --game-dir ../../links/install --appdata ../../.tmp/save10-crash/appdata-<x> \
//     --present headless --skip-logo --audio off --time-limit 120 --log-level info --frame-log \
//     --plugin scripts/plugins/scenario-save10-field.ts
//   ★ 回退（夹具 / 诊断）：`--plugin-param slot=10 --plugin-param expect=FIELD`。
//
// 判据（"点击之后能继续"）：点完**再活 `SURVIVE_MS`** ⇒ `ctx.finish(0)`；期间进程没了 ⇒ stderr 的 `stop :` 就是现场。
//
// ★ v2 的两处机械变化（2026-11-28）：
//   · `atReadPoint("any", 1)`（"下一个读点"）⇒ `{script:"FIELD.BIN"}`（该脚本的下一条指令）：
//     输入是**排队**的（move 先入队、click 后入队，一读点最多落地一条）⇒ 顺序保证不变，
//     这一拍只是保持"move 之后隔一拍再点"的旧形状。
//   · `afterDelay(8000)` ⇒ `await t.delay(SURVIVE_MS)`（**墙钟、NON-DIFFABLE**；P0 的 `ctx.delay`）。
//     它是**诊断判据**（"点完之后还活着"），不是业务门；⛔ 不要拿它做 A/B 或指纹。
//   · 终点 `ctx.finish(0)` 在 task 里可用（P0 起 Run 侧门）⇒ 读数仍是 `run=finished exit=0`。

import type { PluginModule, RunCtx } from "@amayui/lift-plugin";

import { bootToTitle } from "./lib/boot-to-title.ts";
import { awaitPosition, awaitWhere } from "./lib/gates.ts";
import { FIELD_MAIN_LOOP_HI, FIELD_MAIN_LOOP_LO } from "./lib/script-positions.ts";
import { loadSave } from "./lib/save.ts";
import {
    openConfigDomain,
    scriptListSetting,
    slotSetting,
} from "./lib/settings.ts";

// 点击点（用户报的坐标；1280×720 参考系，原样照抄 —— ⛔ 不要按"看起来像"改）。
const CLICK_X = 540;
const CLICK_Y = 450;
// 等"地图就绪"（FIELD 主轮询体）的上限；加载实测 ~10 s 级。
const READY_TIMEOUT_MS = 60000;
// 点完之后还要活多久才算"没崩"（宿主墙钟；这一条是**诊断判据**，不是 wait 出来的业务门）。
const SURVIVE_MS = 8000;

// ★ **没有** `startPage` 旋钮（2026-11-22 删）：读档界面进屏时 `SAVE.BIN` 第 11 行
//   `load-int (global-int 138e)` 会把页号**从 `SAVE.DAT` 重新读回** ⇒ 在进屏之前写它会被覆盖
//   （实测 `applied=true`、进屏仍读回 1；读数见 `docs/03-emulator/save-format.md` §翻页）。
//   翻页这件事由 `loadSave` 自己读页号 + 走箭头完成，本文件不再需要诊断旋钮。

const SLOT: number = 10;

const DEFAULT_EXPECT: string[] = ["FIELD.BIN"];

/** 配置文件里的域（`<--appdata>/plugin-config.json`；只读）。 */
const CONFIG_DOMAIN = "scenario.save10-field";

const plugin = {
    start(ctx: RunCtx): void {
        // ★ 一次 Run 内 config / params 不变 ⇒ 只在 `start`（Run 回调）里读一次。
        //   取配置的语义统一在 `./lib/settings.ts`（`slot` 整数校验 / `expect` 归一 / 默认 10）。
        const domain = openConfigDomain(ctx, CONFIG_DOMAIN);
        const slot = slotSetting(ctx, domain, SLOT, CONFIG_DOMAIN);
        const listed = scriptListSetting(ctx, domain, "expect", CONFIG_DOMAIN);
        // ★ 与旧行为逐字一致：config 给了空表 ⇒ `normalizeScriptList` 已 throw；params 给空（如 `","`）
        //   也回退 `DEFAULT_EXPECT`（旧代码就是这么判的）。
        const expect = listed === null || listed.length === 0 ? DEFAULT_EXPECT : listed;

        ctx.task(async (t) => {
            const titleTick = await bootToTitle(t);
            await loadSave(t, slot, { expect }, titleTick);
            log.info("scenario-save10: FIELD.BIN reached ⇒ 等主轮询体（地图就绪）");
            let tick = await awaitPosition(
                t,
                "FIELD.BIN",
                FIELD_MAIN_LOOP_LO,
                FIELD_MAIN_LOOP_HI,
                "scenario:field-ready",
                READY_TIMEOUT_MS,
                "scenario-save10",
                "timeout",
            );
            log.info(`scenario-save10: FIELD 主轮询体到了 ⇒ move (${CLICK_X},${CLICK_Y})`);
            tick.input.move(CLICK_X, CLICK_Y);
            tick = await awaitWhere(
                t,
                { script: "FIELD.BIN" },
                "scenario:hovered",
                READY_TIMEOUT_MS,
                "scenario-save10",
                "timeout",
            );
            log.info(`scenario-save10: click (${CLICK_X},${CLICK_Y})`);
            tick.input.click(CLICK_X, CLICK_Y);
            await t.delay(SURVIVE_MS);
            log.info(`scenario-save10: 点击之后 ${SURVIVE_MS} ms 仍在跑`);
            t.finish(0, "scenario-save10: survived");
        });
    },
} satisfies PluginModule;

export default plugin;
