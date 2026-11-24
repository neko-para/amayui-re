// scripts/plugins/lib/scenario-field-point-visible.js —— `FIELD` 上把「点位精灵」强制走**可见**那条分支
//
// 是什么：一个**开关**。`--plugin-param on=1` 时，在 `FIELD` 就绪后把 32 槽派生渲染标志
// （`FIELD_OBJECT_FLAGS_GLOBAL_ID` = `0x50ff9f`）里、类型命中（缺省 採集 `0x1F` / 発掘 `0x20`）的槽
// 的 `HIDE|ALPHA0` 两位**清掉** ⇒ `DRAWOBJ.BIN` instr#7 不再跳过、instr#26 不再把 alpha 置 0
// ⇒ 那张 `0xFA+类型` 的纹理（`0x119` = `MO031B.AGF` 绿草 / `0x11A` = `MO032B.AGF` 岩石）就画出来。
//
// ⛔ **不改"这点位发现过没有"的真值**：
//   · 写的是 `50ff9f`（VM 每次进图重算的派生表：`FIELD.BIN` #187 / `SETOBJ.BIN` #2 的 `fill-zero`，之后逐槽
//     `bit-set`），不是持久输入（`50ffd1` 类型 / `13d7` / `3e2a0` / `14d8cc` 一个都不写）。
//   · `on=0` 走**一模一样**的链，只是不做这一步 ⇒ 开/关的差异只在这两个位上。
//   · 本插件不写盘（不改 `data/**`、不写存档）；"存档里的发现位没变"用 `--appdata` 里 `SAVE*.DAT` 的
//     跑前/跑后哈希比对来证（命令见 `docs/03-emulator/scripts/FIELD.md` / 本轮报告）。
//
// 为什么 MO 纹理是这两个槽（已核，`docs/03-emulator/scripts/FIELD.md` §地块特效渲染模型）：
//   `DRAWOBJ.BIN` instr#44 `add (local-int 11) fa (local-int 1)` ⇒ 纹理槽 = `0xFA + 类型`；
//   `SETMAPTEX.BIN` instr#78/#80 把 ALF 20797 / 20798 绑到 `0x119` / `0x11A`。
//
// ★ **为什么本文件自己走载入链、而不用 `./save.js` 的 `loadSave`**（2026-11-24 / win32 实测）：
//   本安装树上 `loadSave` 两次都停在 `load:sbunki` 门超时（`calls=5`、`reason=load-save: gate timeout
//   (load:sbunki)`；槽 5 与槽 10 各一次）。同一组窗口在 `.plan` 前端是 4/4（`save07-battle.plan` 的
//   ①–④：SAVE 回调体 → `click 300 507` → SBUNKI 轮询体 → 读段之后 `[0x0B18,0x0C58)` → `click 640 323`）。
//   这里照抄那四步（窗口常量全部 import 自 `./script-positions.js`），⛔ 没有新窗口、没有新坐标。
//
// 用法（cwd = apps/emulator-lift；`on` 缺省 0 = 不动）：
//   ./out/emulator-lift-aot.exe --exe ../../corpus/assets/age/AGE.EXE__dumped.sectfix.EXE \
//     --game-dir ../../links/install --appdata ../../.tmp/field-model/appdata \
//     --present headless --skip-logo --audio off --time-limit 180 \
//     --plugin scripts/plugins/lib/scenario-field-point-visible.js \
//     --plugin-param slot=7 --plugin-param on=1

import {
    FIELD_CURRENT_MAP_GLOBAL_ID,
    FIELD_MAP_INSTANCE_GLOBAL_ID,
    FIELD_OBJECT_FLAGS_GLOBAL_ID,
    FIELD_POINT_TYPE_DIG,
    FIELD_POINT_TYPE_GATHER,
    forceFieldPointsVisible,
    readFieldObjectSlots,
    readIntGlobal,
} from "./guest-globals.js";
import {
    FIELD_MAIN_LOOP_HI,
    FIELD_MAIN_LOOP_LO,
    SAVE_INPUT_CALLBACK_HI,
    SAVE_INPUT_CALLBACK_LO,
    SBUNKI_CONFIRM_CLICK_HI,
    SBUNKI_CONFIRM_CLICK_LO,
    SBUNKI_POLL_HI,
    SBUNKI_POLL_LO,
    TITLE_INPUT_CALLBACK_HI,
    TITLE_INPUT_CALLBACK_LO,
} from "./script-positions.js";

/** @typedef {import("@amayui/lift-plugin").RunCtx} RunCtx */
/** @typedef {import("@amayui/lift-plugin").Trigger} Trigger */
/** @typedef {import("@amayui/lift-plugin").FiredEvent} FiredEvent */
/** @typedef {import("@amayui/lift-plugin").TimeoutEvent} TimeoutEvent */
/** @typedef {FiredEvent | TimeoutEvent} PluginEvent */

/** 载入槽（缺省 7：`save07-battle.plan` 用的那一槽，行 y = 507）。 */
const DEFAULT_SLOT = 7;
/** 加载屏行中心 y（实测表；只有 0..7 有读数）。坐标不是 `pos`，⛔ 不进 `script-positions.js`。 */
const ROW_Y = { 0: 91, 1: 150, 2: 209, 3: 269, 4: 328, 5: 388, 6: 447, 7: 507 };
/** 各道门的上限（宿主墙钟）。 */
const TITLE_TIMEOUT_MS = 60000;
const SAVE_TIMEOUT_MS = 120000;
const SBUNKI_TIMEOUT_MS = 60000;
const FIELD_TIMEOUT_MS = 300000;
/** 改完之后再活多久（给 DRAWOBJ 若干帧去画）。 */
const DEFAULT_HOLD_MS = 3000;
/** 日志里最多逐槽打几行（⛔ 不刷屏）。 */
const MAX_LOGGED_SLOTS = 12;
/** 点 LoadData 入口 / 确认框「是」的坐标（1280×720；照抄 `save07-battle.plan`）。 */
const LOAD_DATA_XY = [1070, 480];
const CONFIRM_XY = [640, 323];
const ROW_X = 300;

/**
 * `1f,20`（反汇编口径：**十六进制不带 0x**）→ 数字数组。
 *
 * @param {string | null | undefined} raw
 * @returns {number[]} 缺省 = 採集 + 発掘。
 */
function parseTypes(raw) {
    if (raw === undefined || raw === null || String(raw).trim() === "") {
        return [FIELD_POINT_TYPE_GATHER, FIELD_POINT_TYPE_DIG];
    }
    /** @type {number[]} */
    const out = [];
    for (const part of String(raw).split(",")) {
        const text = part.trim();
        if (text.length === 0) continue;
        // ⛔ 不用正则（QJS 这版 build 没有 RegExp）：手工剥可选的 `0x`。
        const body = text.length > 2 && (text[0] === "0") && (text[1] === "x" || text[1] === "X")
            ? text.slice(2)
            : text;
        const value = Number.parseInt(body, 16);
        if (!Number.isFinite(value) || value < 0) throw new Error(`types 的项不是十六进制：${text}`);
        out.push(value);
    }
    if (out.length === 0) throw new Error("types 给空了（要么不给，要么给 1f,20 这种）");
    return out;
}

/**
 * @param {Readonly<Record<string, string | null | undefined>>} params
 * @param {string} key
 * @param {number} fallback
 * @returns {number}
 */
function intParam(params, key, fallback) {
    const raw = params[key];
    if (raw === undefined || raw === null || String(raw).trim() === "") return fallback;
    const value = Number(raw);
    if (!Number.isInteger(value) || value < 0) throw new Error(`${key} 需要非负整数（收到 ${String(raw)}）`);
    return value;
}

/** @param {number} value @returns {string} */
const hex = (value) => "0x" + (value >>> 0).toString(16);

/**
 * 32 槽快照 → 少量日志行（只打"有类型"的槽）。
 *
 * @param {RunCtx} ctx RunCtx。
 * @param {string} tag 日志前缀（`before` / `after` / `after-hold`）。
 * @returns {void}
 */
function logSnapshot(ctx, tag) {
    const slots = readFieldObjectSlots(ctx);
    const live = slots.filter((s) => s.slot !== 0 && s.type !== 0);
    ctx.log("info", `field-points: ${tag} map=${hex(readIntGlobal(ctx, FIELD_CURRENT_MAP_GLOBAL_ID))} ` +
        `instance=${hex(readIntGlobal(ctx, FIELD_MAP_INSTANCE_GLOBAL_ID))} live_slots=${live.length}`);
    for (const s of live.slice(0, MAX_LOGGED_SLOTS)) {
        ctx.log("info", `field-points: ${tag} slot=${s.slot} type=${hex(s.type)} flags=${hex(s.flags)}`);
    }
    if (live.length > MAX_LOGGED_SLOTS) {
        ctx.log("info", `field-points: ${tag} …（还有 ${live.length - MAX_LOGGED_SLOTS} 个槽未逐条打印）`);
    }
}

/**
 * 载入链 + 场景。`slot` 只决定行 y（页基由读档界面自己从 `SAVE.DAT` 读回）。
 *
 * @param {number} slot 0..7（只支持有行 y 读数的那 8 行）。
 * @returns {{ start: (ctx: RunCtx) => Trigger | Trigger[], onTrigger: (ctx: RunCtx, ev: PluginEvent) => Trigger | Trigger[] }}
 */
function makeChain(slot) {
    const rowY = ROW_Y[slot];
    if (rowY === undefined) throw new Error(`slot ${slot} 没有行 y 读数（只支持 0..7）`);
    return {
        start(ctx) {
            return ctx.trigger.afterPosition("TITLE.BIN", TITLE_INPUT_CALLBACK_LO, TITLE_INPUT_CALLBACK_HI, {
                name: "fp:title",
                timeoutMs: TITLE_TIMEOUT_MS,
            });
        },
        onTrigger(ctx, ev) {
            if (ev.kind === "timeout") {
                ctx.finish(5, `field-points: timeout (${ev.name})`);
                return [];
            }
            switch (ev.name) {
                case "fp:title":
                    if (intParam(ctx.params, "selftest", 0) !== 0) {
                        // 能力自检（⛔ 不是场景）：证明 `ctx.read.global.get` / `ctx.edit.global.set` 在这台
                        // 构建上真的通 —— 读一格 → 写回**同一个值**（无副作用）→ 再读回比对。
                        const id = FIELD_OBJECT_FLAGS_GLOBAL_ID;
                        const before = readIntGlobal(ctx, id) >>> 0;
                        const receipt = ctx.edit.global.set("int", id, before);
                        const each = readIntGlobal(ctx, id) >>> 0;
                        const applied = receipt !== null && typeof receipt === "object" ? receipt.applied !== false : false;
                        ctx.log("info", `field-points: selftest global=${hex(id)} before=${hex(before)} ` +
                            `readback=${hex(each)} applied=${String(applied)} ` +
                            `accepted=${String(receipt !== null && typeof receipt === "object" ? receipt.accepted : "?")}`);
                        ctx.finish(applied && each === before ? 0 : 6, "field-points: selftest");
                        return [];
                    }
                    ctx.input.click(LOAD_DATA_XY[0], LOAD_DATA_XY[1]);
                    return ctx.trigger.afterPosition("SAVE.BIN", SAVE_INPUT_CALLBACK_LO, SAVE_INPUT_CALLBACK_HI, {
                        name: "fp:save",
                        timeoutMs: SAVE_TIMEOUT_MS,
                    });
                case "fp:save":
                    // ★ **先 move 再 click**：guest 的命中判定读的是**光标位置**（`ctx.input.move`），
                    //   只 click 不动光标 ⇒ 行不亮、确认框不弹（2026-11-24 实测：本链第一版没有 move，
                    //   `fp:sbunki-poll` 60 s 超时）。
                    ctx.log("info", `field-points: SAVE 回调体 ⇒ 悬停第 ${slot} 行 (${ROW_X},${rowY})`);
                    ctx.input.move(ROW_X, rowY);
                    return ctx.trigger.atReadPoint("any", 1, { name: "fp:row-hover", timeoutMs: SBUNKI_TIMEOUT_MS });
                case "fp:row-hover":
                    ctx.input.click(ROW_X, rowY);
                    if (intParam(ctx.params, "diag", 0) !== 0) {
                        // 诊断档：点完之后连采 scriptPos，看有没有确认框脚本起来（⛔ 不进正常链）。
                        return ctx.trigger.atReadPoint("any", 1, { name: "fp:diag:0", timeoutMs: 15000 });
                    }
                    return ctx.trigger.afterPosition("SBUNKI.BIN", SBUNKI_POLL_LO, SBUNKI_POLL_HI, {
                        name: "fp:sbunki-poll",
                        timeoutMs: SBUNKI_TIMEOUT_MS,
                    });
                case "fp:sbunki-poll":
                    // ★ 读段**之后**才点（在轮询体/读段里点会被吞；`save07-battle.plan` 的 ③/③' 读数）。
                    return ctx.trigger.afterPosition("SBUNKI.BIN", SBUNKI_CONFIRM_CLICK_LO, SBUNKI_CONFIRM_CLICK_HI, {
                        name: "fp:sbunki-click",
                        timeoutMs: SBUNKI_TIMEOUT_MS,
                    });
                case "fp:sbunki-click":
                    ctx.input.move(CONFIRM_XY[0], CONFIRM_XY[1]);
                    return ctx.trigger.atReadPoint("any", 1, { name: "fp:confirm-hover", timeoutMs: SBUNKI_TIMEOUT_MS });
                case "fp:confirm-hover":
                    ctx.input.click(CONFIRM_XY[0], CONFIRM_XY[1]);
                    return ctx.trigger.afterPosition("FIELD.BIN", FIELD_MAIN_LOOP_LO, FIELD_MAIN_LOOP_HI, {
                        name: "fp:field",
                        timeoutMs: FIELD_TIMEOUT_MS,
                    });
                case "fp:field": {
                    const on = intParam(ctx.params, "on", 0) !== 0;
                    const types = parseTypes(ctx.params["types"]);
                    const hold = intParam(ctx.params, "hold", DEFAULT_HOLD_MS);
                    ctx.log("info", `field-points: FIELD 就绪 ⇒ switch on=${on ? 1 : 0} types=${types.map(hex).join(",")}`);
                    logSnapshot(ctx, "before");
                    if (on) {
                        const changed = forceFieldPointsVisible(ctx, { types });
                        const did = changed.filter((c) => c.applied);
                        ctx.log("info", `field-points: forced=${did.length}/${changed.length} 个槽（清 HIDE|ALPHA0；派生位）`);
                        for (const c of did.slice(0, MAX_LOGGED_SLOTS)) {
                            ctx.log("info", `field-points: forced slot=${c.slot} type=${hex(c.type)} ` +
                                `${hex(c.before)} → ${hex(c.after)}`);
                        }
                        logSnapshot(ctx, "after");
                        ctx.log("info", `field-points: 只写过 ${hex(FIELD_OBJECT_FLAGS_GLOBAL_ID)}+slot` +
                            "（派生渲染位）；类型表与持久输入未写");
                    }
                    return ctx.trigger.afterDelay(hold, { name: "fp:hold" });
                }
                case "fp:hold":
                    logSnapshot(ctx, "after-hold");
                    ctx.log("info", `field-points: done (on=${intParam(ctx.params, "on", 0) !== 0 ? 1 : 0})`);
                    ctx.finish(0, "field-points: survived");
                    return [];
                default:
                    if (ev.name.startsWith("fp:diag:")) {
                        const n = Number(ev.name.slice("fp:diag:".length));
                        // ★ `RunCtx.read.scriptPos` 在 Run 语境是 `undefined`（`plugin.d.ts` 的 C2b 切片）
                        //   ⇒ 位置读数走 `ctx.now`（`TriggerReadings{script,pos}`）。
                        const now = ctx.now;
                        ctx.log("info", `field-points: diag[${n}] script=${String(now.script)} ` +
                            `pos=0x${Number((now.pos ?? 0) >>> 0).toString(16)}`);
                        if (n + 1 >= 8) {
                            ctx.finish(7, "field-points: diag done");
                            return [];
                        }
                        return ctx.trigger.atReadPoint("any", 1, { name: `fp:diag:${n + 1}`, timeoutMs: 15000 });
                    }
                    ctx.fail(ev.name, `unknown state: ${ev.name}`);
                    return [];
            }
        },
    };
}

/** @type {{ start: (ctx: RunCtx) => Trigger | Trigger[], onTrigger: (ctx: RunCtx, ev: PluginEvent) => Trigger | Trigger[] } | null} */
let chain = null;
/** @type {string | null} */
let chainKey = null;

/**
 * @param {Readonly<Record<string, string | null | undefined>>} params
 * @returns {{ start: (ctx: RunCtx) => Trigger | Trigger[], onTrigger: (ctx: RunCtx, ev: PluginEvent) => Trigger | Trigger[] }}
 */
function chainFor(params) {
    // `ctx.params` 在一个 Run 内不变 ⇒ 链只构造一次，不把进度塞进闭包。
    const key = `${String(params["slot"])}|${String(params["on"])}`;
    if (chain !== null && chainKey === key) return chain;
    chain = makeChain(intParam(params, "slot", DEFAULT_SLOT));
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
