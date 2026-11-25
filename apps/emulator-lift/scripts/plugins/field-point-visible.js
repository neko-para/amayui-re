// scripts/plugins/field-point-visible.js —— `FIELD` 上把「点位精灵」强制走**可见**那条分支
//
// 是什么：一个**开关**。配置域 `field.point-visible` 的 `on` ≠ 0 时（或回退的
// `--plugin-param on=1`），在 `FIELD` 就绪后把 32 槽派生渲染标志
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
// ★ **载入段 = `./lib/save.js` 的 `loadSave`（2026-11-25 改回；此前是本文件自建的四步链）**：
//   自建链的那一版**没有判页**（照抄 `save07-battle.plan` 的 4 步，行 y 直接按槽号取）。
//   2026-11-24 把它归到"进屏页 1 ⇒ 槽 7 的 y=507 落在页 1 第 7 行 = 槽 17（空槽）"，
//   但那份 appdata 的存档落点**没有留证** ⇒ 归因不完整。
//   ⚠ 2026-11-25 复现出更靠前的根因：把 `SAVE*.DAT` 复制到 `<appdata>` **根目录**
//   （不是嵌套 `SAVE/`）⇒ 读档列表**整个是空的** ⇒ 点行同样不弹确认框（`load:sbunki` 超时）。
//   ⇒ **先查 appdata 落点**（见用法 0），再谈页基 / 空槽。
//   ⇒ 现在载入段交给 `loadSave(slot)`（它自己读页号 `0x138e`、翻页、翻完等列表刷新、再点行）；
//   本文件只剩"载入之后"这一段（`fp:` 前缀），⛔ 不再重算行 y / 不重抄确认框窗口。
//   ⚠ 2026-11-24 那一版还写过"只 click 不动光标 ⇒ 行不亮"：`ctx.input.click` 自己会先 `mouseMove`
//   （`PointerCore::armClickAt` → `armMoveAt`），所以那条归因**不成立**。
//
// 用法（cwd = apps/emulator-lift；参数来源 **config > --plugin-param > 默认**）：
//   0) 私有 appdata **必须**是嵌套层：存档放 `<appdata>/Local/Eushully/天結いキャッスルマイスター/SAVE/`
//      ⇒ `bash scripts/clean-appdata.sh ../../.tmp/field-model/appdata`（建法见 `saves/README.md`）。
//      ⛔ 把 SAVE*.DAT 放到 `<appdata>` 根目录 ⇒ 引擎新建空 `SAVE/` ⇒ 读档列表为空 ⇒ 点行不弹确认框
//      （`load-save: gate timeout (load:sbunki)`；2026-11-24/25 的失败全是这一条，与 headless/window 无关）。
//   1) 在 **appdata 根**（与 `Local/` 同级，⛔ 不在 `SAVE/` 下）写 `plugin-config.json`：
//        {"version":1,"domains":{"field.point-visible":{"slot":10,"on":1,"hold":3000}}}
//      域 key（全部可选；缺省 = `slot 10` / `on 0` / `types ["1f","20"]` / `hold 3000` / `diag 0`）：
//        `slot` number / `on` number|bool / `types` hex 串 `"1f,20"` 或数组 / `hold` number / `diag` number
//   2) 不带 `--plugin-param` 直接跑：
//      ./out/emulator-lift-aot.exe --game-dir ../../links/install --appdata ../../.tmp/field-model/appdata \
//        --present headless --skip-logo --audio off --time-limit 180 --log-level warn \
//        --plugin scripts/plugins/field-point-visible.js
//   ★ 回退（夹具 / 诊断；config 里没给的 key 才生效）：
//        --plugin-param slot=10 --plugin-param on=1 --plugin-param hold=3000
//   ★ `slot=10` = SAVE10（落在 FIELD；进屏页 1 第 0 行 ⇒ 不用翻页；2026-11-25 实测）。
//   ★ 从冷启动跑 ⇒ 本文件自己盖 `./lib/boot-to-title.js`（与 `scenario-fellow-switch.js` 同一个三层 route）。

import { bootToTitle } from "./lib/boot-to-title.js";
import { route } from "./lib/chains.js";
import { loadSave } from "./lib/save.js";
import {
    FIELD_CURRENT_MAP_GLOBAL_ID,
    FIELD_MAP_INSTANCE_GLOBAL_ID,
    FIELD_OBJECT_FLAGS_GLOBAL_ID,
    FIELD_POINT_TYPE_DIG,
    FIELD_POINT_TYPE_GATHER,
    forceFieldPointsVisible,
    readFieldObjectSlots,
    readIntGlobal,
} from "./lib/guest-globals.js";
import { FIELD_MAIN_LOOP_HI, FIELD_MAIN_LOOP_LO } from "./lib/script-positions.js";

/** @typedef {import("@amayui/lift-plugin").RunCtx} RunCtx */
/** @typedef {import("@amayui/lift-plugin").Trigger} Trigger */
/** @typedef {import("@amayui/lift-plugin").FiredEvent} FiredEvent */
/** @typedef {import("@amayui/lift-plugin").TimeoutEvent} TimeoutEvent */
/** @typedef {FiredEvent | TimeoutEvent} PluginEvent */

/** 载入槽（缺省 10：`SAVE10` 落在 FIELD，进屏页 1 第 0 行 ⇒ 不用翻页；2026-11-25 实测）。 */
const DEFAULT_SLOT = 10;
/** 载入的终态脚本：这份存档落在 `FIELD`（页号/行由 `loadSave` 自己判）。 */
const LOAD_EXPECT = ["FIELD.BIN"];
/** 载入段各道门的超时（翻页 + 列表刷新 + 确认框）。 */
const LOAD_TIMEOUT_MS = 120000;
/** FIELD 就绪 + 之后各道门的上限。 */
const FIELD_TIMEOUT_MS = 300000;
/** 改完之后再活多久（给 DRAWOBJ 若干帧去画）。 */
const DEFAULT_HOLD_MS = 3000;
/** 日志里最多逐槽打几行（⛔ 不刷屏）。 */
const MAX_LOGGED_SLOTS = 12;
/** `diag` 连采的读点个数（诊断档：看 FIELD 之后脚本停在哪）。 */
const DIAG_SAMPLES = 8;

/** 配置文件里的域（`<--appdata>/plugin-config.json`；只读）。 */
const CONFIG_DOMAIN = "field.point-visible";

/**
 * `types`：config 可给 hex 串 `"1f,20"` 或数组（元素 = hex 串或数字）；params 只有逗号串。
 *
 * @param {unknown} raw 缺省 = undefined/null ⇒ 採集 + 発掘。
 * @returns {number[]}
 */
function parseTypes(raw) {
    if (raw === undefined || raw === null) {
        return [FIELD_POINT_TYPE_GATHER, FIELD_POINT_TYPE_DIG];
    }
    // `--plugin-param types=`（空串）= 没给（旧行为）；config 里给空串 ⇒ 响亮拒绝（下走 out.length）。
    if (typeof raw === "string" && raw.trim() === "") {
        return [FIELD_POINT_TYPE_GATHER, FIELD_POINT_TYPE_DIG];
    }
    /** @type {number[]} */
    const out = [];
    /** @param {unknown} item */
    const pushOne = (item) => {
        if (typeof item === "number") {
            if (!Number.isInteger(item) || item < 0) {
                throw new Error(`types 的数字项必须是非负整数（收到 ${String(item)}）`);
            }
            out.push(item);
            return;
        }
        if (typeof item !== "string") {
            throw new Error(`types 的元素必须是 hex 串或非负整数（收到 ${typeof item}）`);
        }
        const text = item.trim();
        if (text.length === 0) return;
        // ⛔ 不用正则（QJS 这版 build 没有 RegExp）：手工剥可选的 `0x`。
        const body = text.length > 2 && (text[0] === "0") && (text[1] === "x" || text[1] === "X")
            ? text.slice(2)
            : text;
        const value = Number.parseInt(body, 16);
        if (!Number.isFinite(value) || value < 0) throw new Error(`types 的项不是十六进制：${text}`);
        out.push(value);
    };
    if (Array.isArray(raw)) {
        for (const item of raw) pushOne(item);
    } else if (typeof raw === "string") {
        for (const part of raw.split(",")) pushOne(part);
    } else {
        throw new Error(`types 必须是 hex 串或数组（收到 ${typeof raw}）`);
    }
    if (out.length === 0) throw new Error("types 给空了（要么不给，要么给 1f,20 这种）");
    return out;
}

/**
 * 配置文件可用才返回域对象（否则 null ⇒ 完全回退到 `ctx.params`）。
 *
 * @param {RunCtx} ctx
 * @returns {import("@amayui/lift-plugin").ConfigDomain | null}
 */
function configDomainOrNull(ctx) {
    const config = ctx.config;
    if (config === undefined || config === null || config.available !== true) return null;
    return config.domain(CONFIG_DOMAIN);
}

/** @param {import("@amayui/lift-plugin").ConfigDomain | null} domain @param {string} key */
function hasConfig(domain, key) {
    return domain !== null && domain.has(key);
}

/**
 * 非负整数配置项：config 优先；否则 params（`intParam` 的口径）。
 *
 * @param {RunCtx} ctx
 * @param {import("@amayui/lift-plugin").ConfigDomain | null} domain
 * @param {string} key
 * @param {number} fallback
 * @returns {number}
 */
function intSetting(ctx, domain, key, fallback) {
    if (!hasConfig(domain, key)) return intParam(ctx.params, key, fallback);
    const value = domain.get(key);
    if (typeof value !== "number" || !Number.isInteger(value) || value < 0) {
        throw new Error(`config ${CONFIG_DOMAIN}.${key} 需要非负整数（收到 ${String(value)}）`);
    }
    return value;
}

/**
 * 布尔 / 数字开关：config 收 boolean 或 0/1；否则 params（`intParam` 的口径）。
 *
 * @param {RunCtx} ctx
 * @param {import("@amayui/lift-plugin").ConfigDomain | null} domain
 * @param {string} key
 * @param {boolean} fallback
 * @returns {boolean}
 */
function boolSetting(ctx, domain, key, fallback) {
    if (!hasConfig(domain, key)) return intParam(ctx.params, key, fallback ? 1 : 0) !== 0;
    const value = domain.get(key);
    if (typeof value === "boolean") return value;
    if (typeof value === "number") {
        if (!Number.isInteger(value)) {
            throw new Error(`config ${CONFIG_DOMAIN}.${key} 需要 boolean 或整数（收到 ${String(value)}）`);
        }
        return value !== 0;
    }
    throw new Error(`config ${CONFIG_DOMAIN}.${key} 需要 boolean 或整数（收到 ${typeof value}）`);
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
 * **载入之后**的那一段（`fp:` 前缀）：`FIELD` 就绪 → 改派生渲染位 → 保持若干帧。
 *
 * @param {{ on: boolean, types: number[], hold: number, diag: boolean }} opts
 * @returns {{ start: (ctx: RunCtx) => Trigger | Trigger[], onTrigger: (ctx: RunCtx, ev: PluginEvent) => Trigger | Trigger[] }}
 */
function makeFieldStage(opts) {
    return {
        start(ctx) {
            ctx.log("info", "field-points: 载入到 FIELD ⇒ 等地图主循环");
            return ctx.trigger.afterPosition("FIELD.BIN", FIELD_MAIN_LOOP_LO, FIELD_MAIN_LOOP_HI, {
                name: "fp:field",
                timeoutMs: FIELD_TIMEOUT_MS,
            });
        },
        onTrigger(ctx, ev) {
            if (ev.kind === "timeout") {
                ctx.finish(5, `field-points: timeout (${ev.name})`);
                return [];
            }
            const name = String(ev.name);
            if (name === "fp:field") {
                ctx.log("info", `field-points: FIELD 就绪 ⇒ switch on=${opts.on ? 1 : 0} ` +
                    `types=${opts.types.map(hex).join(",")}`);
                logSnapshot(ctx, "before");
                if (opts.on) {
                    const changed = forceFieldPointsVisible(ctx, { types: opts.types });
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
                if (opts.diag) {
                    // 诊断档（⛔ 不进正常链）：连采 8 个读点的 script / pos。
                    return ctx.trigger.atReadPoint("any", 1, { name: "fp:diag:0", timeoutMs: 15000 });
                }
                return ctx.trigger.afterDelay(opts.hold, { name: "fp:hold" });
            }
            if (name === "fp:hold") {
                logSnapshot(ctx, "after-hold");
                ctx.log("info", `field-points: done (on=${opts.on ? 1 : 0})`);
                ctx.finish(0, "field-points: survived");
                return [];
            }
            if (name.startsWith("fp:diag:")) {
                const n = Number(name.slice("fp:diag:".length));
                // ★ `RunCtx.read.scriptPos` 在 Run 语境是 `undefined`（`plugin.d.ts` 的 C2b 切片）
                //   ⇒ 位置读数走 `ctx.now`（`TriggerReadings{script,pos}`）。
                const now = ctx.now;
                ctx.log("info", `field-points: diag[${n}] script=${String(now.script)} ` +
                    `pos=0x${Number((now.pos ?? 0) >>> 0).toString(16)}`);
                if (n + 1 >= DIAG_SAMPLES) {
                    ctx.finish(7, "field-points: diag done");
                    return [];
                }
                return ctx.trigger.atReadPoint("any", 1, { name: `fp:diag:${n + 1}`, timeoutMs: 15000 });
            }
            ctx.fail(name, `unknown state: ${name}`);
            return [];
        },
    };
}

/** @type {object | null} */
let chain = null;

/**
 * 一次 Run 内 config / params 不变 ⇒ 链只构造一次（链本身无状态：进度只在 ev.name）。
 * ★ 2026-11-25：`ctx.config` 的读取也**只在第一次**做（每拍 `domain()` 会新建绑定对象 + 若干
 *   宿主调用；2 ms 窗口里那是可观测的份额）。
 *
 * @param {RunCtx} ctx
 * @returns {{ start: (ctx: RunCtx) => Trigger | Trigger[], onTrigger: (ctx: RunCtx, ev: PluginEvent) => Trigger | Trigger[] }}
 */
function chainFor(ctx) {
    if (chain !== null) return chain;
    const domain = configDomainOrNull(ctx);
    const slot = intSetting(ctx, domain, "slot", DEFAULT_SLOT);
    const on = boolSetting(ctx, domain, "on", false);
    const types = hasConfig(domain, "types") ? parseTypes(domain.get("types")) : parseTypes(ctx.params["types"]);
    const hold = intSetting(ctx, domain, "hold", DEFAULT_HOLD_MS);
    const diag = boolSetting(ctx, domain, "diag", false);
    const stage = makeFieldStage({ on, types, hold, diag });
    const load = loadSave(slot, {
        expect: LOAD_EXPECT,
        timeoutMs: LOAD_TIMEOUT_MS,
        onLoaded: (ctx2) => stage.start(ctx2),
    });
    const boot = bootToTitle({ onDone: (ctx2) => load.start(ctx2) });
    const afterBoot = route((ev) => String(ev.name).startsWith("load:"), load, stage);
    chain = route((ev) => String(ev.name).startsWith("boot:"), boot, afterBoot);
    return chain;
}

/** @type {import("@amayui/lift-plugin").PluginModule} */
const plugin = {
    start(ctx) {
        return chainFor(ctx).start(ctx);
    },
    onTrigger(ctx, ev) {
        return chainFor(ctx).onTrigger(ctx, ev);
    },
};

export default plugin;
