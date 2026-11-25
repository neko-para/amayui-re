// scripts/plugins/field-point-visible.js —— `FIELD` 上的**开关**：把「点位精灵」的派生渲染位强制成可见
//
// 是什么（★ 父级裁决 2026-11-25：开关 = 开关，⛔ 不是自驱复现脚本）：
//   · 玩家**自己**进 `FIELD` 时（本拍读数 `ctx.now.script == "FIELD.BIN"`），把 32 槽派生渲染标志
//     （`FIELD_OBJECT_FLAGS_GLOBAL_ID` = `0x50ff9f`）里、类型命中（缺省 採集 `0x1F` / 発掘 `0x20`）的槽
//     的 `HIDE|ALPHA0` 两位**清掉** ⇒ `DRAWOBJ.BIN` instr#7 不再跳过、instr#26 不再把 alpha 置 0
//     ⇒ 那张 `0xFA+类型` 的纹理（`0x119` = `MO031B.AGF` 绿草 / `0x11A` = `MO032B.AGF` 岩石）就画出来。
//   · `50ff9f` 是 VM **每次进图重算**的派生表（`FIELD.BIN` #187 / `SETOBJ.BIN` #2 的 `fill-zero`，
//     之后逐槽 `bit-set`）⇒ **每次进图都要重新施加**：本插件**常驻**轮询（`poll` 节流），
//     只在真的需要时写（`forceFieldPointsVisible` 逐槽给 `before/after`；`after === before` 的槽
//     根本不发写回执）。
//
// ⛔ 三件事本插件**永远不做**（父级裁决：开关不许驱动任何输入）：
//   ① ⛔ **不读档**：不 import `lib/save.js`（`loadSave`）、不 import `lib/boot-to-title.js`、不 import
//      `lib/chains.js`。验收时**玩家自己读档**：`--overlay --plugin field-point-visible` 是目标用法
//      （`--plugin` 与 `--input-plan` / `--control-root` 互斥 ⇒ "输入计划驱动"这条路不存在）。
//   FIELD 的**自动化复现**仍归 `scenario-save10-field.js`（那是另一个入口，不是本开关）。
//   ② ⛔ **不点鼠标**：一次都不碰 `input.*`（插件 API 的输入面只有它 ⇒ 不碰 = 不驱动输入）。
//   ③ ⛔ **不写盘**：不写 `data/**`、不写存档、不写配置（config 只读；唯一的写是 `settings.schema`
//      这个**机器写的保留域**，由 `registerSettings` 在 `start` 里做，见下）。
//   ⇒ 守卫 `apps/emulator-lift/tests/plugin_field_switch.cpp` 两段判据：
//      **静态**（剥注释后）"代码里没有 input / loadSave / bootToTitle / chains / finish / quit"；
//      **端到端**（`emulator-lift-aot --plugin` headless，无 `--input-plan`）"没有任何 `boot:` / `load:`
//      行、Run 保持常驻（readout `run=waiting`、`stop: time-limit`）、首行日志 `field-points: on=0`"。
//
// ★ **常驻语义**：正常路径**永不**结束 ⇒ ⛔ 不调用 `finish`（进程按 `--time-limit` / 关窗结束），
//   只有出错才 `fail`。`--time-limit` 到期属于正常结局（宿主 exit 0）。
// ★ **不改"这点位发现过没有"的真值**：只写 `50ff9f`（派生表）；持久输入（`50ffd1` 类型 / `13d7` /
//   `3e2a0` / `14d8cc`）一个都不写。`on=0` ⇒ 一个 guest global 都不读、一个都不写（日志明写 `on=0`）。
//
// ★ **设置面（D1/D3，2026-11-25）**：`start` 里 `registerSettings` 把 `on` / `poll` / `diag` 写进保留域
//   `settings.schema`（设置页据此画控件）；这三项**每拍重读**（页面 `set` 当拍就能改到本插件）。
//   `types` **不在 schema 里**⇒ 只在 start 读一次并缓存（要暴露它就得一起搬进 live 读点）。
//
// 用法（cwd = apps/emulator-lift；参数来源 **config > --plugin-param > 默认**）：
//   1) 在 **appdata 根**（与 `Local/` 同级，⛔ 不在 `SAVE/` 下）写 `plugin-config.json`：
//        {"version":1,"domains":{"field.point-visible":{"on":1}}}
//      域 key（全部可选；缺省 = `on false` / `types ["1f","20"]` / `poll 250` / `diag 0`）：
//        `on` bool|0/1|"true"/"false" · `types` hex 串 `"1f,20"` 或数组 · `poll` 毫秒（非负整数）· `diag` bool|0/1
//      ★ 旧配置里残留的 `slot` / `hold` = **无害忽略**（本文件既不读、也不报错：不再读档、不再一次性结束）。
//   2) 玩家自己读档进 FIELD；插件在此之前只做零 guest 读的轮询：
//      ./out/emulator-lift-aot.exe --game-dir ../../links/install --appdata <本树 .tmp 下的私有 appdata> \
//        --present headless --skip-logo --audio off --time-limit 180 --log-level warn \
//        --plugin field-point-visible
//      ★ 目标用法带窗口（`--overlay --plugin field-point-visible`）：`--overlay` = 要窗口 + `hud`，
//        本插件与它可叠加（两者都只读/写各自的面；本插件不画、不抢输入）。
//      ★ `--time-limit` 只决定"看多久"：插件不 finish，进程按时间上限/关窗收口。
//   ★ 回退（夹具 / 诊断；config 里没给的 key 才生效）：
//        --plugin-param on=1 --plugin-param poll=250 --plugin-param types=1f,20
//   ★ `poll=0` = **每个 guest 读点都查一次**（省掉节流的墙钟比较，但每拍都要读 31 个类型槽 +
//     命中槽的标志）⇒ 只在诊断时用；正常档用 250 ms（FIELD 主轮询体每拍都有读点，肉眼无差）。
//
// 为什么 MO 纹理是这两个槽（已核，`docs/03-emulator/scripts/FIELD.md` §地块特效渲染模型）：
//   `DRAWOBJ.BIN` instr#44 `add (local-int 11) fa (local-int 1)` ⇒ 纹理槽 = `0xFA + 类型`；
//   `SETMAPTEX.BIN` instr#78/#80 把 ALF 20797 / 20798 绑到 `0x119` / `0x11A`。

import {
    FIELD_CURRENT_MAP_GLOBAL_ID,
    FIELD_MAP_INSTANCE_GLOBAL_ID,
    FIELD_OBJECT_FLAGS_GLOBAL_ID,
    FIELD_POINT_TYPE_DIG,
    FIELD_POINT_TYPE_GATHER,
    forceFieldPointsVisible,
    hex,
    readIntGlobal,
} from "./lib/guest-globals.js";
import { failOnUnknown } from "./lib/gates.js";
import {
    boolSetting,
    intSetting,
    openConfigDomain,
    registerSettings,
} from "./lib/settings.js";

/** @typedef {import("@amayui/lift-plugin").RunCtx} RunCtx */
/** @typedef {import("@amayui/lift-plugin").Trigger} Trigger */
/** @typedef {import("@amayui/lift-plugin").ConfigDomain} ConfigDomain */
/**
 * 本拍设置（`on` / `diag` / `poll` **每拍重读**；`types` 一个 Run 内不变）。
 *
 * @typedef {{ on: boolean, types: number[], diag: boolean, poll: number }} Settings
 */

/** 配置文件里的域（`<--appdata>/plugin-config.json`）。 */
const CONFIG_DOMAIN = "field.point-visible";

/**
 * `FIELD` 的宿主 marker 拼写（trigger / `ctx.now.script` 侧是**精确 `==`**：带 `.BIN`、不折叠大小写）。
 * ⛔ 不是脚本内位置（位置表在 `./lib/script-positions.js`，本文件不用）。
 */
const FIELD_SCRIPT = "FIELD.BIN";

/** 轮询间隔缺省（毫秒）：`250` ⇒ 每秒 4 次；每次只读 `50ff9f` 那组（见文件头）。 */
const DEFAULT_POLL_MS = 250;

/** 日志里最多逐槽打几行（⛔ 不刷屏；宿主另有 100 行上限）。 */
const MAX_LOGGED_SLOTS = 12;

/** `diag` 档连采的读点个数（诊断用；⛔ 不再用 finish 结束 —— 采够就安静常驻）。 */
const DIAG_SAMPLES = 8;

/** 本 Run 唯一的具名状态（每拍新造 afterDelay 对象，名字不变）。 */
const POLL_NAME = "fp:poll";

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

// ---- 配置读取（**唯一的取配置入口** = `./lib/settings.js`；这里只留 per-Run 缓存）------------

/**
 * 域句柄（模块级缓存的是**对象**，⛔ 不是值）：`domain()` 每次会新建绑定对象 + 若干宿主调用
 * ⇒ 每拍重读只 `get`，不重新 `domain()`。`undefined` = 还没打开；`null` = config 不可用。
 *
 * @type {ConfigDomain | null | undefined}
 */
let configDomain;
/** `types` 一个 Run 内读一次（本期 schema 不暴露它 ⇒ 页面改不到）。 */
/** @type {number[] | null} */
let typesCache = null;
/** `diag` 的上一拍值（页面当拍能改它 ⇒ 这里跟边沿，而不是只在 start 定死）。 */
let diagOn = false;

/**
 * 本拍设置：**config > `--plugin-param` > 默认**（优先级只看"给没给"，`0` / `false` 也算给了）。
 *
 * ★ `on` / `poll` / `diag` **每拍重读** = D3 的 live 读点：设置页 `set` 之后下一次轮询就生效。
 * ★ `types` 只在第一次读（start）：它**不在**本期 schema 里，页面改不到；要暴露它时必须
 *   把这一行一起搬进下面的逐拍读。
 * ★ 旧配置里的 `slot` / `hold` **不在这里出现**：它们被**无害忽略**。
 *
 * @param {RunCtx} ctx RunCtx。
 * @returns {Settings}
 */
function settingsFor(ctx) {
    if (configDomain === undefined) configDomain = openConfigDomain(ctx, CONFIG_DOMAIN);
    const domain = configDomain;
    if (typesCache === null) {
        typesCache = domain !== null && domain.has("types")
            ? parseTypes(domain.get("types"))
            : parseTypes(ctx.params["types"]);
    }
    return {
        on: boolSetting(ctx, domain, "on", false, { domainId: CONFIG_DOMAIN }),
        types: typesCache,
        diag: boolSetting(ctx, domain, "diag", false, { domainId: CONFIG_DOMAIN }),
        poll: intSetting(ctx, domain, "poll", DEFAULT_POLL_MS, { min: 0, domainId: CONFIG_DOMAIN }),
    };
}

// ---- 常驻状态（模块级 = per-plugin；一个进程只有一个 Run）------------------------------

/** 上一次施加时的地图实例（`FIELD_MAP_INSTANCE_GLOBAL_ID`）；null = 还没施加过。 */
/** @type {number | null} */
let lastInstance = null;
/** "FIELD 就绪"那行只打一次。 */
let greeted = false;
/** `diag` 还剩几次（采够就安静常驻；⛔ 不结束 Run）。 */
let diagLeft = 0;

/**
 * 下一拍（`poll` 毫秒之后，落在下一个 guest 读点上）再来一次。
 *
 * ★ 每次**新造**一个 afterDelay 对象：Once 的对象触发过就耗尽（同一对象再放回 = fail）；
 *   新对象 = 新预算。`poll: 0` ⇒ 下一个读点立刻到期（= "每个读点都查"）。
 *
 * @param {RunCtx} ctx RunCtx。
 * @param {Settings} settings 本拍设置。
 * @returns {Trigger}
 */
function armPoll(ctx, settings) {
    return ctx.trigger.afterDelay(settings.poll, { name: POLL_NAME });
}

/**
 * 在 `FIELD` 上施加一次（⛔ 不在 FIELD 就一个 guest global 都不读）。
 *
 * ★ 每拍都试：VM 在**每次进图**重建 `50ff9f` ⇒ 只在"地图实例变了"时施加会漏掉"同一实例重进"；
 *   而"只在真的需要时写"由 `forceFieldPointsVisible` 逐槽 `before/after` 保证（`after === before`
 *   ⇒ 不发写回执）。进度只在 `lastInstance`（地图实例号）与 `diagLeft`。
 *
 * @param {RunCtx} ctx RunCtx。
 * @param {Settings} settings 本拍设置。
 * @returns {void}
 */
function applyInField(ctx, settings) {
    const now = ctx.now;
    if (now.script !== FIELD_SCRIPT) return;   // 不在 FIELD：不读 guest、不写 guest
    const first = !greeted;
    greeted = true;
    const map = readIntGlobal(ctx, FIELD_CURRENT_MAP_GLOBAL_ID);
    const instance = readIntGlobal(ctx, FIELD_MAP_INSTANCE_GLOBAL_ID);
    const instanceChanged = instance !== lastInstance;
    lastInstance = instance;
    const changed = forceFieldPointsVisible(ctx, { types: settings.types });
    if (first) {
        ctx.log(
            "info",
            `field-points: FIELD 就绪 ⇒ switch on=${settings.on ? 1 : 0} ` +
                `types=${settings.types.map(hex).join(",")} map=${hex(map)} instance=${hex(instance)}`,
        );
    } else if (instanceChanged) {
        ctx.log("info", `field-points: re-applied after map change instance=${hex(instance)}`);
    }
    const did = changed.filter((c) => c.applied);
    if (did.length > 0) {
        ctx.log(
            "info",
            `field-points: forced=${did.length}/${changed.length} 个槽（清 HIDE|ALPHA0；派生位 ` +
                `${hex(FIELD_OBJECT_FLAGS_GLOBAL_ID)}）`,
        );
        for (const c of did.slice(0, MAX_LOGGED_SLOTS)) {
            ctx.log(
                "info",
                `field-points: forced slot=${c.slot} type=${hex(c.type)} ${hex(c.before)} → ${hex(c.after)}`,
            );
        }
        ctx.log("info", `field-points: 只写过 ${hex(FIELD_OBJECT_FLAGS_GLOBAL_ID)}+slot（派生渲染位）；类型表与持久输入未写`);
    }
}

/**
 * `diag` 档：连采 `script` / `pos`（只用**宿主当拍读数** `ctx.now`，不读 guest）。
 *
 * @param {RunCtx} ctx RunCtx。
 * @returns {void}
 */
function logDiag(ctx) {
    if (diagLeft <= 0) return;
    diagLeft -= 1;
    const now = ctx.now;
    const pos = now.pos;
    ctx.log(
        "info",
        `field-points: diag script=${String(now.script)} pos=${pos === undefined ? "-" : hex(pos)} ` +
            `（剩 ${diagLeft} 次；采够继续常驻）`,
    );
}

/** @type {import("@amayui/lift-plugin").PluginModule} */
const plugin = {
    start(ctx) {
        const settings = settingsFor(ctx);
        // ★ D1：schema 写进保留域 `settings.schema`（机器写；先 all() 比、相同不写）。
        //   ⛔ 失败只落一行 warn（`registerSettings` 内部 catch）—— 绝不能因为写不了设置而打死 Run。
        registerSettings(ctx, {
            domain: CONFIG_DOMAIN,
            title: "FIELD points",
            live: "run",
            entries: {
                on: { type: "bool", label: "points visible", default: false },
                poll: { type: "number", label: "poll ms", default: DEFAULT_POLL_MS, min: 0, max: 5000, step: 50 },
                diag: { type: "bool", label: "diag", default: false },
            },
        });
        greeted = false;
        lastInstance = null;
        diagOn = settings.diag;
        diagLeft = settings.diag ? DIAG_SAMPLES : 0;
        // ★ 首行日志：明写开关状态（守卫按 `on=0` 判"关着的时候什么也不做"）。
        ctx.log(
            "info",
            `field-points: on=${settings.on ? 1 : 0} types=${settings.types.map(hex).join(",")} ` +
                `poll=${settings.poll}（常驻开关：等玩家自己进 FIELD；不读档、不点鼠标、不写盘）`,
        );
        return armPoll(ctx, settings);
    },
    onTrigger(ctx, ev) {
        if (ev.kind === "timeout") {
            // 正常路径不会有 timeout（只有 afterDelay，它不叠 timeoutMs）⇒ 真出现就是接线坏了。
            // ⛔ 这里**不用** `gates.js` 的 `failOnTimeout`：那条是 `finish(5)`（收口），而本开关
            //    正常路径永不收口（`plugin_field_switch.cpp` 的静态契约也钉着"代码里没有 finish"）。
            ctx.fail(String(ev.name), "field-points: unexpected timeout");
            return [];
        }
        if (String(ev.name) !== POLL_NAME) {
            failOnUnknown(ctx, ev, "field-points");
            return [];
        }
        // ★ 每拍重读 on / poll / diag（D3：设置页 set 之后下一次轮询就生效；types 仍只读一次）。
        const settings = settingsFor(ctx);
        // `diag` 的边沿：页面把它打开 ⇒ 从这里再采 DIAG_SAMPLES 次（⛔ 不只在 start 定死）。
        if (settings.diag && !diagOn) {
            diagOn = true;
            diagLeft = DIAG_SAMPLES;
        } else if (!settings.diag) {
            diagOn = false;
            diagLeft = 0;
        }
        // `on=0`（且 diag=0）⇒ 连 `ctx.now` 都不看：只有最小轮询的 re-arm，一个 guest global 都不读。
        if (settings.on) applyInField(ctx, settings);
        if (settings.diag) logDiag(ctx);
        return armPoll(ctx, settings);
    },
};

export default plugin;
