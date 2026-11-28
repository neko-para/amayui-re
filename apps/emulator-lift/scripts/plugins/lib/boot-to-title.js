// scripts/plugins/lib/boot-to-title.js —— **启动到 TITLE**（一道门，只做这一件事；v2 async 版）
//
// 为什么单独一件：TITLE 门的作用域是"**进程启动**"，不是"读取存档"。`save.js` 的载入链从
//   "**已经在 TITLE**" 开始（它的 `run` 第一件事就是点 LoadData）⇒ 谁要从冷启动跑，就先
//   `await bootToTitle(ctx)`，再 `await loadSave(ctx, slot, …)`（顺序 `await`，没有 `route`）。
//   ★ 拆开的收益是可判性：TITLE 门超时 = "启动没到 TITLE"；SAVE 门超时 = "TITLE 上那一下没被吃"。
//     混在一条链里两者都叫 `gate timeout`。
//
// v2 形状（2026-11-28）：
//   · `await bootToTitle(ctx, { timeoutMs })` → 命中那一拍的 **Tick**（`tick.input` 可用）。
//     调用方拿它去点 TITLE 上的按钮 / 交给 `loadSave` 点 LoadData（那一下必须仍然在**吃 click
//     的窗口**里提交，而 Tick 的写面只在产生它的那次入口内有效 ⇒ 调用要紧跟 `await`）。
//   · 门 = `once({script:"TITLE.BIN", lo:TITLE_INPUT_CALLBACK_LO, hi:TITLE_INPUT_CALLBACK_HI})`：
//     区间来自 `./script-positions.js`（⛔ 本文件不写位置字面量；那条区间是**吃 click 的窗口**，
//     实测 15/15 —— 见 `docs/03-emulator/scripts/TITLE.md`）。`once` 的区间语义 = **首次命中**，
//     与 v1 `afterPosition(..., mode:"once")` 逐条相同 ⇒ 不需要挑一个"安全单点"。
//   · 门超时 ⇒ `ctx.finish(5, "boot: gate timeout (boot:title)")` 响亮收口（⛔ 不静默 done；
//     ⛔ 不自己加 `wait` 重试）。`gates.js` 的 `awaitPosition` 统一做这件事。
//   · ⛔ 全篇零 `wait` / 零 `delay`：等的就是 guest 可见状态。

import { awaitPosition } from "./gates.js";
import { TITLE_INPUT_CALLBACK_HI, TITLE_INPUT_CALLBACK_LO } from "./script-positions.js";

/** @typedef {import("@amayui/lift-plugin").RunCtx} RunCtx */
/** @typedef {import("@amayui/lift-plugin").Tick} Tick */

// ★ 宿主 marker 的**精确拼写**（C1 的 `afterPosition` 是精确 `==`，⛔ 不剥 `.BIN`、不忽略大小写）。
const SCRIPT_TITLE = "TITLE.BIN";

// 启动到 TITLE 的墙钟上限（与旧的 `save.js` TITLE 门同档：实测 `--skip-logo` 下几秒内到）。
const DEFAULT_BOOT_TIMEOUT_MS = 30000;

/** 本链唯一的具名门（失败文案里逐字保留；v1 的 `route` 分流键已随 `chains.js` 删除）。 */
export const BOOT_TITLE_NAME = "boot:title";

/**
 * 等 TITLE 的输入回调窗口，命中后返回**那一拍的 Tick**。
 *
 * @param {RunCtx} ctx RunCtx（`ctx.once` 的门 = Run 侧入口；在 `ctx.task` 里调用）。
 * @param {{ timeoutMs?: number } | null} [options]
 *   · `timeoutMs`：TITLE 门的超时；缺省 30000 ms。
 * @returns {Promise<Tick>}
 */
export async function bootToTitle(ctx, options = {}) {
    const timeoutMs =
        options !== null && options.timeoutMs !== undefined ? options.timeoutMs : DEFAULT_BOOT_TIMEOUT_MS;
    if (!Number.isInteger(timeoutMs) || timeoutMs <= 0) {
        throw new Error(`bootToTitle: timeoutMs must be a positive integer (got ${timeoutMs})`);
    }
    log.info("boot: waiting for TITLE input callback");
    const tick = await awaitPosition(
        ctx,
        SCRIPT_TITLE,
        TITLE_INPUT_CALLBACK_LO,
        TITLE_INPUT_CALLBACK_HI,
        BOOT_TITLE_NAME,
        timeoutMs,
        "boot",
    );
    log.info("boot: TITLE hit");
    return tick;
}

export default bootToTitle;
