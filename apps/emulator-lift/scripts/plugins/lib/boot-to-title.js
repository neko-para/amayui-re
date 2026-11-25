// scripts/plugins/lib/boot-to-title.js —— **启动到 TITLE**（一道门，只做这一件事）
//
// 为什么单独一件：TITLE 门的作用域是"**进程启动**"，不是"读取存档"。`save.js` 的载入链从
//   "**已经在 TITLE**" 开始（它的 `start` 直接点 LoadData，见那个文件头）⇒ 谁要从冷启动跑，
//   就把本文件盖在前面（`route` 组合见 `../scenario-save07-battle.js`）。
//   ★ 拆开的收益是可判性：TITLE 门超时 = "启动没到 TITLE"；SAVE 门超时 = "TITLE 上那一下没被吃"。
//     混在一条链里两者都叫 `gate timeout`。
//
// 合同（与 `./chains.js` 的 `{ start, onTrigger }` 同形）：
//   · `start(ctx)` 只 arm **一道**门：`afterPosition TITLE [TITLE_INPUT_CALLBACK_LO, TITLE_INPUT_CALLBACK_HI)`。
//     区间来自 `./script-positions.js`（⛔ 本文件不写位置字面量；那条区间是**吃 click 的窗口**，
//     实测 15/15 —— 见 `docs/03-emulator/scripts/TITLE.md`）。
//   · 命中 ⇒ `return onDone ? onDone(ctx) : []`（返回值原样成为本次 `onTrigger` 的返回值；
//     `onDone` 抛错**不**在这里捕获 —— 交给宿主按既有规则记 fail）。
//   · 门超时 ⇒ `ctx.finish(5, …)` 响亮收口（⛔ 不静默 done；⛔ 不自己加 `wait` 重试）。
//   · ⛔ 全篇零 `wait` / 零 `afterDelay`：等的就是 guest 可见状态。
//
// ★ **trigger 名一律 `boot:` 前缀**（本案只有 `boot:title`）。它必须与载入链的 `load:` **不相交**：
//   两条链由 `route(isFirst(ev))` 按名字分流，重名会被另一条链抢走、**静默不触发**
//   （`./chains.js` 文件头）。
// ★ 无状态：进度只在 `ev.name`（宿主每回调新建实例也可能接旧链 —— `plugin-runtime.md` §4.6）。

import { awaitPosition, failOnTimeout, failOnUnknown } from "./gates.js";
import { TITLE_INPUT_CALLBACK_HI, TITLE_INPUT_CALLBACK_LO } from "./script-positions.js";

/** @typedef {import("@amayui/lift-plugin").RunCtx} RunCtx */
/** @typedef {import("@amayui/lift-plugin").Trigger} Trigger */
/** @typedef {import("@amayui/lift-plugin").FiredEvent} FiredEvent */
/** @typedef {import("@amayui/lift-plugin").TimeoutEvent} TimeoutEvent */
/** @typedef {FiredEvent | TimeoutEvent} PluginEvent */
/**
 * 一条链：形状 = `PluginModule` 的 `start` / `onTrigger` 两个字段（与 `./chains.js` 的 `Chain` 同形）。
 *
 * @typedef {object} Chain
 * @property {(ctx: RunCtx) => Trigger | Trigger[] | null} start
 * @property {(ctx: RunCtx, ev: PluginEvent) => Trigger | Trigger[] | null} onTrigger
 */

// ★ 宿主 marker 的**精确拼写**（C1 的 `afterPosition` 是精确 `==`，⛔ 不剥 `.BIN`、不忽略大小写）。
const SCRIPT_TITLE = "TITLE.BIN";

// 启动到 TITLE 的墙钟上限（与旧的 `save.js` TITLE 门同档：实测 `--skip-logo` 下几秒内到）。
const DEFAULT_BOOT_TIMEOUT_MS = 30000;

/** 本链唯一的具名状态（`route` 按前缀分流就看它）。 */
export const BOOT_TITLE_NAME = "boot:title";

/**
 * 造一条"启动到 TITLE"的链。
 *
 * @param {{
 *   onDone?: (ctx: RunCtx) => Trigger | Trigger[] | null,
 *   timeoutMs?: number,
 * } | null} [options]
 *   · `onDone`：门命中后要做的事（本案 = 拉起载入链的 `start`）；缺省 ⇒ `[]`（本链自己就是全部）。
 *   · `timeoutMs`：TITLE 门的超时；缺省 30000 ms。
 * @returns {Chain}
 */
export function bootToTitle(options = {}) {
    const onDone = options !== null ? options.onDone : undefined;
    if (onDone !== undefined && typeof onDone !== "function") {
        throw new Error(`bootToTitle: onDone must be a function (got ${typeof onDone})`);
    }
    const timeoutMs =
        options !== null && options.timeoutMs !== undefined ? options.timeoutMs : DEFAULT_BOOT_TIMEOUT_MS;
    if (!Number.isInteger(timeoutMs) || timeoutMs < 0) {
        throw new Error(`bootToTitle: timeoutMs must be a non-negative integer (got ${timeoutMs})`);
    }

    return {
        start(ctx) {
            ctx.log("info", "boot: waiting for TITLE input callback");
            return awaitPosition(
                ctx,
                SCRIPT_TITLE,
                TITLE_INPUT_CALLBACK_LO,
                TITLE_INPUT_CALLBACK_HI,
                BOOT_TITLE_NAME,
                timeoutMs,
            );
        },

        onTrigger(ctx, ev) {
            // 与 `loadSave` 同一条失败面：门超时 = finish(5)（⛔ 不用 quit：归因会变成 `plugin: quit`）。
            if (ev.kind === "timeout") {
                failOnTimeout(ctx, ev, "boot");
                return [];
            }
            if (ev.name !== BOOT_TITLE_NAME) {
                failOnUnknown(ctx, ev, "boot");
                return [];
            }
            ctx.log("info", "boot: TITLE hit");
            return onDone === undefined ? [] : onDone(ctx);
        },
    };
}

export default bootToTitle;
