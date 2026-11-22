// scripts/plugins/lib/scenario-save10-field.js —— **复现**「载入存档 10 → 在 FIELD 点 (540,450) → 退出」
//
// 三段组合（`plugin-runtime.md` §4.6a 规则 3）：`bootToTitle` → `loadSave(slot)` → 本文件的场景。
// 它是**一条可复跑的复现入口**（不是产品场景）：所有翻页都走 `loadSave` 的真翻页
// （读 `0x138e` 页号 + 点顶部页签 + 读点门等页号真的变），⛔ 没有 `.plan`、⛔ 没有把存档改名。
//
// 时序（**这是复现的一部分**，别省）：`expect` 命中只代表 FIELD 脚本刚起来（地图还在加载）⇒
//   **先等 FIELD 的主轮询体 `[0x579C,0x5860)`**（= 地图就绪、在等输入），再 move → 读点 → click。
//   直接点会被加载过程吞掉（实测：第一版在 `expect` 命中那一拍就点，什么也没发生）。
//
// 用法（cwd = apps/emulator-lift；存档 010/011/… 在**第 2 页**，翻页由 `save.js` 自己做）：
//   ./out/emulator-lift-aot.exe --exe ../../corpus/assets/age/AGE.EXE__dumped.sectfix.EXE \
//     --game-dir ../../links/install --appdata ../../.tmp/save10-crash/appdata-<x> \
//     --present headless --skip-logo --audio off --time-limit 120 --log-level info --frame-log \
//     --plugin scripts/plugins/lib/scenario-save10-field.js --plugin-param slot=10 --plugin-param expect=FIELD
//
// 判据（"点击之后能继续"）：点完**再活 `SURVIVE_MS`** ⇒ `finish(0)`；期间进程没了 ⇒ stderr 的 `stop :` 就是现场。

import { bootToTitle } from "./boot-to-title.js";
import { route } from "./chains.js";
import { FIELD_MAIN_LOOP_HI, FIELD_MAIN_LOOP_LO } from "./script-positions.js";
import { loadSave } from "./save.js";

/** @typedef {import("@amayui/lift-plugin").RunCtx} RunCtx */
/** @typedef {import("@amayui/lift-plugin").Trigger} Trigger */
/** @typedef {import("@amayui/lift-plugin").FiredEvent} FiredEvent */
/** @typedef {import("@amayui/lift-plugin").TimeoutEvent} TimeoutEvent */
/** @typedef {FiredEvent | TimeoutEvent} PluginEvent */

// 点击点（用户报的坐标；1280×720 参考系，原样照抄 —— ⛔ 不要按"看起来像"改）。
const CLICK_X = 540;
const CLICK_Y = 450;
// 等"地图就绪"（FIELD 主轮询体）的上限；加载实测 ~10 s 级。
const READY_TIMEOUT_MS = 60000;
// 点完之后还要活多久才算"没崩"（宿主墙钟；这一条是**诊断判据**，不是 wait 出来的业务门）。
const SURVIVE_MS = 8000;

/** @type {import("./chains.js").Chain} */
const scenario = {
    start(ctx) {
        ctx.log("info", "scenario-save10: FIELD.BIN reached ⇒ 等主轮询体（地图就绪）");
        return ctx.trigger.afterPosition("FIELD.BIN", FIELD_MAIN_LOOP_LO, FIELD_MAIN_LOOP_HI, {
            name: "scenario:field-ready",
            timeoutMs: READY_TIMEOUT_MS,
        });
    },
    onTrigger(ctx, ev) {
        if (ev.kind === "timeout") {
            ctx.finish(5, `scenario-save10: timeout (${ev.name})`);
            return [];
        }
        switch (ev.name) {
            case "scenario:field-ready":
                ctx.log("info", `scenario-save10: FIELD 主轮询体到了 ⇒ move (${CLICK_X},${CLICK_Y})`);
                ctx.input.move(CLICK_X, CLICK_Y);
                return ctx.trigger.atReadPoint("any", 1, { name: "scenario:hovered", timeoutMs: READY_TIMEOUT_MS });
            case "scenario:hovered":
                ctx.log("info", `scenario-save10: click (${CLICK_X},${CLICK_Y})`);
                ctx.input.click(CLICK_X, CLICK_Y);
                return ctx.trigger.afterDelay(SURVIVE_MS, { name: "scenario:survive" });
            case "scenario:survive":
                ctx.log("info", `scenario-save10: 点击之后 ${SURVIVE_MS} ms 仍在跑`);
                ctx.finish(0, "scenario-save10: survived");
                return [];
            default:
                ctx.fail(ev.name, `unknown state: ${ev.name}`);
                return [];
        }
    },
};

// ★ **没有** `startPage` 旋钮（2026-11-22 删）：读档界面进屏时 `SAVE.BIN` 第 11 行
//   `load-int (global-int 138e)` 会把页号**从 `SAVE.DAT` 重新读回** ⇒ 在进屏之前写它会被覆盖
//   （实测 `applied=true`、进屏仍读回 1；读数见 `docs/03-emulator/save-format.md` §翻页）。
//   翻页这件事由 `loadSave` 自己读页号 + 走箭头完成，本文件不再需要诊断旋钮。

/** @type {number} */
const SLOT = 10;

/** @type {string[]} */
const DEFAULT_EXPECT = ["FIELD.BIN"];

/**
 * @param {Readonly<Record<string, string | null | undefined>>} params
 * @returns {number}
 */
function slotFromParams(params) {
    const raw = params["slot"];
    if (raw === undefined || raw === null || String(raw).trim() === "") return SLOT;
    return Number(raw);
}

/**
 * @param {Readonly<Record<string, string | null | undefined>>} params
 * @returns {string[]}
 */
function expectFromParams(params) {
    const raw = params["expect"];
    if (raw === undefined || raw === null || String(raw).trim() === "") return DEFAULT_EXPECT;
    const out = [];
    for (const part of String(raw).split(",")) {
        const name = part.trim();
        if (name.length === 0) continue;
        out.push(name.endsWith(".BIN") ? name : name + ".BIN");
    }
    return out.length === 0 ? DEFAULT_EXPECT : out;
}

/** @type {import("./chains.js").Chain | null} */
let chain = null;
/** @type {string | null} */
let chainKey = null;

/**
 * `ctx.params` 在一个 Run 内不变 ⇒ 链只构造一次（链本身无状态：进度只在 `ev.name`）。
 *
 * @param {Readonly<Record<string, string | null | undefined>>} params
 * @returns {import("./chains.js").Chain}
 */
function chainFor(params) {
    const key = `${String(params["slot"])}|${String(params["expect"])}`;
    if (chain !== null && chainKey === key) return chain;
    const load = loadSave(slotFromParams(params), {
        expect: expectFromParams(params),
        onLoaded: (ctx) => scenario.start(ctx),
    });
    const boot = bootToTitle({
        onDone: (ctx) => load.start(ctx),
    });
    // 两层 route：外层剥 `boot:`，内层剥 `load:`，剩下的是场景自己的 `scenario:` 前缀。
    // ★ 三个前缀两两不相交（`boot:` / `load:` / `scenario:`）—— `route` 只看 `isFirst(ev)`。
    const afterBoot = route((ev) => String(ev.name).startsWith("load:"), load, scenario);
    chain = route((ev) => String(ev.name).startsWith("boot:"), boot, afterBoot);
    chainKey = key;
    return chain;
}

/** @type {import("@amayui/lift-plugin").PluginModule} */
const plugin = {
    start(ctx) {
        return chainFor(ctx.params).start(ctx);
    },
    onTrigger(ctx, ev) {
        return chainFor(ctx.params).onTrigger(ctx, ev);
    },
};

export default plugin;
