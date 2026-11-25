// scripts/plugins/field-point-visible.js —— `FIELD` 上的**开关**：setmap 时无条件显示所有采集 / 挖掘点
//
// 是什么（★ 用户裁决 2026-11-25：目标不是"在 FIELD 里无条件显示 `?`/`!` 图标"）：
//   · 采集 / 挖掘点的本体 = **B 链点位精灵**（`DRAWOBJ.BIN` 每槽 `draw-texture`，纹理槽 = `0xFA + 类型`：
//     `0x119` = `MO031B.AGF` 绿草 / `0x11A` = `MO032B.AGF` 岩石）。`?` / `!` 是 **A 链格标记**
//     （`SO030.AGF` 槽 `0x49`），**不是**点位，本插件不改它们。
//   · 精灵画不画 = 派生渲染位 `FIELD_OBJECT_FLAGS_GLOBAL_ID`（`50ff9f[slot]`）的两位：
//     **bit0=1 ⇒ 画**（`DRAWOBJ` #7 `and … 1` → `jcc`：cond==0 才跳 ⇒ bit0=0 才跳过）·
//     **bit2=1 ⇒ 正常 alpha**（#26 `and … 4` 为 0 ⇒ #29 把 alpha 置 0）。
//   · 所以本插件对**类型命中**（缺省 採集 `0x1F` / 発掘 `0x20`）的槽写 `flags |= 0x5`（只置位、⛔ 不清位）。
//     ★ bit1 **不要清**：A 链格表与 `FIELD` `label_00030E54` 的战利品抽取都消费 `flags & 2`。
//   · `50ff9f` 是 VM **每次进图重算**的派生表（`FIELD.BIN` #187 / `SETOBJ.BIN` #2 的 `fill-zero`，
//     之后逐槽 `bit-set`）⇒ **每次 setmap 都要重新施加**。
//   · ★ **钩子 = setmap 完成、SETOBJ 已算完、FIELD 还没建表 / 画**（父级/用户裁决 2026-11-25）：
//     两个常驻 `afterPosition(FIELD.BIN, …, { mode: "reenter" })` ——
//       ① `[FIELD_SETMAP_A_LO, FIELD_SETMAP_A_HI)` = `label_00018274` 内 `call-script 511a`(SETOBJ) 的
//          **返回点**（进图主路径）；
//       ② `[FIELD_SETMAP_B_LO, FIELD_SETMAP_B_HI)` = `0x035130` 块里同一条 `call-script 511a` 的返回点。
//     两个 trigger 都 arm 着；任一处 fire 都把**同一份 32 槽表**重算一遍（换图 / 重进 FIELD 会重新 SETOBJ）。
//     Reenter 的边沿是**有效读数的 外→内**（`src/runtime/trigger_runner.cpp`），`last_inside` 挂在**同一个
//     trigger 对象**上、跨 arm 保留 ⇒ 不需要 re-arm（`exhausted()` 只耗尽 Once / `maxFires>0`）。
//   · ★ **判据是"已知的脚本位置"，所以用位置触发，⛔ 不用轮询**（父级裁决 2026-11-25）。
//
// ⛔ 三件事本插件**永远不做**（用户裁决：开关不许驱动任何输入）：
//   ① ⛔ **不读档**：不 import `lib/save.js`（`loadSave`）、不 import `lib/boot-to-title.js`、不 import
//      `lib/chains.js`。验收时**玩家自己读档**：`--overlay --plugin field-point-visible` 是目标用法
//      （`--plugin` 与 `--input-plan` / `--control-root` 互斥 ⇒ "输入计划驱动"这条路不存在）。
//   FIELD 的**自动化复现**仍归 `scenario-save10-field.js`（那是另一个入口，不是本开关）。
//   ② ⛔ **不点鼠标**：一次都不碰 `input.*`（插件 API 的输入面只有它 ⇒ 不碰 = 不驱动输入）。
//   ③ ⛔ **不写盘**：不写 `data/**`、不写存档、不写配置（config 只读；唯一的写是 `settings.schema`
//      这个**机器写的保留域**，由 `registerSettings` 在 `start` 里做，见下）。
//   ⇒ 守卫 `apps/emulator-lift/tests/plugin_field_switch.cpp` 两段判据：
//      **静态**（剥注释后）"代码里没有 input / loadSave / bootToTitle / chains / finish / quit"，
//      且仍在用 `afterPosition` + `reenter` + 两个 setmap 区间 + `forceFieldPointsVisible`；
//      **端到端**（`emulator-lift-aot --plugin` headless，无 `--input-plan`）"没有任何 `boot:` / `load:`
//      行、Run 保持常驻（readout `run=waiting`、`stop: time-limit`）、首行日志 `field-points: on=0`"。
//
// ★ **常驻语义**：正常路径**永不**结束 ⇒ ⛔ 不调用 `finish`（进程按 `--time-limit` / 关窗结束），
//   只有出错才 `fail`。`--time-limit` 到期属于正常结局（宿主 exit 0）。
// ★ **不改"这点位发现过没有"的真值**：只写 `50ff9f`（派生表）；持久输入（`50ffd1` 类型 / `13d7` /
//   `3e2a0` / `14d8cc`）一个都不写。`on=0` ⇒ 一个 guest global 都不读、一个都不写（日志明写 `on=0`）。
//
// ★ **设置面（D1/D3，2026-11-25）**：`start` 里 `registerSettings` 把 `on` / `diag` 写进保留域
//   `settings.schema`（设置页据此画控件）；这两项**每次 fire 重读**（页面 `set` 之后下一次位置触发
//   就生效）。`types` **不在 schema 里** ⇒ 只在 start 读一次并缓存。
//
// 用法（cwd = apps/emulator-lift；参数来源 **config > --plugin-param > 默认**）：
//   1) 在 **appdata 根**（与 `Local/` 同级，⛔ 不在 `SAVE/` 下）写 `plugin-config.json`：
//        {"version":1,"domains":{"field.point-visible":{"on":1}}}
//      域 key（全部可选；缺省 = `on false` / `types ["1f","20"]` / `diag 0`）：
//        `on` bool|0/1|"true"/"false" · `types` hex 串 `"1f,20"` 或数组 · `diag` bool|0/1
//      ★ 旧配置里残留的 `slot` / `hold` / `poll` = **无害忽略**（本文件既不读、也不报错：
//        不再读档、不再轮询）。
//   2) 玩家自己读档进 FIELD；在此之前插件只是 arm 了两条位置触发（一个读点都没碰 guest）：
//      ./out/emulator-lift-aot.exe --game-dir ../../links/install --appdata <本树 .tmp 下的私有 appdata> \
//        --present headless --skip-logo --audio off --time-limit 180 --log-level warn \
//        --plugin field-point-visible
//      ★ 目标用法带窗口（`--overlay --plugin field-point-visible`）：`--overlay` = 要窗口 + `hud`，
//        本插件与它可叠加（两者都只读/写各自的面；本插件不画、不抢输入）。
//      ★ `--time-limit` 只决定"看多久"：插件不 finish，进程按时间上限/关窗收口。
//   ★ 回退（夹具 / 诊断；config 里没给的 key 才生效）：
//        --plugin-param on=1 --plugin-param types=1f,20
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
    openConfigDomain,
    registerSettings,
} from "./lib/settings.js";
import {
    FIELD_SETMAP_A_HI,
    FIELD_SETMAP_A_LO,
    FIELD_SETMAP_B_HI,
    FIELD_SETMAP_B_LO,
} from "./lib/script-positions.js";

/** @typedef {import("@amayui/lift-plugin").RunCtx} RunCtx */
/** @typedef {import("@amayui/lift-plugin").Trigger} Trigger */
/** @typedef {import("@amayui/lift-plugin").ConfigDomain} ConfigDomain */
/**
 * 本次 fire 的设置（`on` / `diag` **每次 fire 重读**；`types` 一个 Run 内不变）。
 *
 * @typedef {{ on: boolean, types: number[], diag: boolean }} Settings
 */

/** 配置文件里的域（`<--appdata>/plugin-config.json`）。 */
const CONFIG_DOMAIN = "field.point-visible";

/**
 * `FIELD` 的宿主 marker 拼写（trigger / `ctx.now.script` 侧是**精确 `==`**：带 `.BIN`、不折叠大小写）。
 * 位置表在 `./lib/script-positions.js`（`FIELD_SETMAP_A_LO/HI` / `FIELD_SETMAP_B_LO/HI`）。
 */
const FIELD_SCRIPT = "FIELD.BIN";

/** 两条 setmap 钩子的具名状态（每次 fire 原样放回**同一个**集合）。 */
const TRIGGER_NAME_A = "fp:setmap-a";
const TRIGGER_NAME_B = "fp:setmap-b";

/** 日志里最多逐槽打几行（⛔ 不刷屏；宿主另有 100 行上限）。 */
const MAX_LOGGED_SLOTS = 12;

/** `diag` 档最多打几行（每次 fire 一行 `script` / `pos`）。 */
const DIAG_SAMPLES = 8;

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
 * ⇒ 每次 fire 重读只 `get`，不重新 `domain()`。`undefined` = 还没打开；`null` = config 不可用。
 *
 * @type {ConfigDomain | null | undefined}
 */
let configDomain;
/** `types` 一个 Run 内读一次（本期 schema 不暴露它 ⇒ 页面改不到）。 */
/** @type {number[] | null} */
let typesCache = null;
/** `diag` 的上一次读数（页面当拍能改它 ⇒ 这里跟边沿，而不是只在 start 定死）。 */
let diagOn = false;

/**
 * 本次 fire 的设置：**config > `--plugin-param` > 默认**（优先级只看"给没给"，`0` / `false` 也算给了）。
 *
 * ★ `on` / `diag` **每次 fire 重读** = D3 的 live 读点：设置页 `set` 之后下一次位置触发就生效。
 * ★ `types` 只在第一次读（start）：它**不在**本期 schema 里，页面改不到；要暴露它时必须
 *   把这一行一起搬进下面的逐 fire 读。
 * ★ 旧配置里的 `slot` / `hold` / `poll` **不在这里出现**：它们被**无害忽略**。
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
    };
}

// ---- 常驻状态（模块级 = per-plugin；一个进程只有一个 Run）------------------------------

/** 两条 trigger 的**同一个集合**（每次 fire 原样放回；⛔ 不新建 ⇒ Reenter 的边沿不丢）。 */
/** @type {Trigger[] | null} */
let triggers = null;
/** 上一次施加时的地图实例（`FIELD_MAP_INSTANCE_GLOBAL_ID`）；null = 还没施加过。 */
/** @type {number | null} */
let lastInstance = null;
/** "首次施加"那一段日志只打一次。 */
let announced = false;
/** `diag` 还剩几次（打够就安静常驻；⛔ 不结束 Run）。 */
let diagLeft = 0;

/**
 * 在 setmap 完成点施加一次（⛔ `on == 0` 时调用方根本不会进来 ⇒ 一个 guest global 都不读）。
 *
 * ★ 只由**位置触发的 fire** 调（`on` 为真时）；Reenter 每次重新进入区间都会 fire 一次
 *   ⇒ "VM 每次进图重建 `50ff9f`" 由**重新 setmap** 那一次 fire 覆盖，⛔ 不靠轮询兜底。
 *   "只在真的需要时写"由 `forceFieldPointsVisible` 逐槽 `before/after` 保证
 *   （`after === before` ⇒ 不发写回执）。进度只在 `lastInstance`（地图实例号）与 `diagLeft`。
 *
 * @param {RunCtx} ctx RunCtx。
 * @param {Settings} settings 本次 fire 的设置。
 * @returns {void}
 */
function applyAtSetmap(ctx, settings) {
    const first = !announced;
    const map = readIntGlobal(ctx, FIELD_CURRENT_MAP_GLOBAL_ID);
    const instance = readIntGlobal(ctx, FIELD_MAP_INSTANCE_GLOBAL_ID);
    const instanceChanged = instance !== lastInstance;
    lastInstance = instance;
    const changed = forceFieldPointsVisible(ctx, { types: settings.types });
    const did = changed.filter((c) => c.applied);
    if (did.length === 0) return;   // 本来就已经要画 ⇒ 不写、不刷屏
    announced = true;
    if (first) {
        ctx.log(
            "info",
            `field-points: setmap forced=${did.length}/${changed.length} 个槽（置 DRAW|ALPHA；派生位 ` +
                `${hex(FIELD_OBJECT_FLAGS_GLOBAL_ID)}）map=${hex(map)} instance=${hex(instance)}`,
        );
        for (const c of did.slice(0, MAX_LOGGED_SLOTS)) {
            ctx.log(
                "info",
                `field-points: setmap slot=${c.slot} type=${hex(c.type)} ${hex(c.before)} → ${hex(c.after)}`,
            );
        }
        ctx.log("info", `field-points: 只写过 ${hex(FIELD_OBJECT_FLAGS_GLOBAL_ID)}+slot（派生渲染位）；类型表与持久输入未写`);
    } else if (instanceChanged) {
        ctx.log(
            "info",
            `field-points: setmap re-applied instance=${hex(instance)} forced=${did.length}/${changed.length}`,
        );
    }
}

/**
 * `diag` 档：每次 fire 打一行 `script` / `pos`（最多 `DIAG_SAMPLES` 行；只用**宿主当拍读数**
 * `ctx.now`，不读 guest）。
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
            `（剩 ${diagLeft} 次；打够继续常驻）`,
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
                diag: { type: "bool", label: "diag", default: false },
            },
        });
        announced = false;
        lastInstance = null;
        diagOn = settings.diag;
        diagLeft = settings.diag ? DIAG_SAMPLES : 0;
        // ★ 首行日志：明写开关状态（守卫按 `on=0` 判"关着的时候什么也不做"）。
        ctx.log(
            "info",
            `field-points: on=${settings.on ? 1 : 0} types=${settings.types.map(hex).join(",")}` +
                `（setmap 时显示所有采集/挖掘点）`,
        );
        // ★ **两个**常驻 trigger：setmap 的两条返回点（进图主路径 / `0x035130` 块）。
        //   ⛔ **不要**在这里再 arm 一个轮询（那会把"已知位置"退化成轮询）。
        triggers = [
            ctx.trigger.afterPosition(FIELD_SCRIPT, FIELD_SETMAP_A_LO, FIELD_SETMAP_A_HI, {
                name: TRIGGER_NAME_A,
                mode: "reenter",
            }),
            ctx.trigger.afterPosition(FIELD_SCRIPT, FIELD_SETMAP_B_LO, FIELD_SETMAP_B_HI, {
                name: TRIGGER_NAME_B,
                mode: "reenter",
            }),
        ];
        return triggers;
    },
    onTrigger(ctx, ev) {
        if (ev.kind === "timeout") {
            // 正常路径不会有 timeout（两个 afterPosition 都没配 timeoutMs）⇒ 真出现就是接线坏了。
            // ⛔ 这里**不用** `gates.js` 的 `failOnTimeout`：那条是 `finish(5)`（收口），而本开关
            //    正常路径永不收口（`plugin_field_switch.cpp` 的静态契约也钉着"代码里没有 finish"）。
            ctx.fail(String(ev.name), "field-points: unexpected timeout");
            return [];
        }
        const name = String(ev.name);
        if (name !== TRIGGER_NAME_A && name !== TRIGGER_NAME_B) {
            failOnUnknown(ctx, ev, "field-points");
            return [];
        }
        // ★ 每次 fire 重读 on / diag（D3：设置页 set 之后下一次位置触发就生效；types 仍只读一次）。
        const settings = settingsFor(ctx);
        // `diag` 的边沿：页面把它打开 ⇒ 从这里再打 DIAG_SAMPLES 行（⛔ 不只在 start 定死）。
        if (settings.diag && !diagOn) {
            diagOn = true;
            diagLeft = DIAG_SAMPLES;
        } else if (!settings.diag) {
            diagOn = false;
            diagLeft = 0;
        }
        // `on=0` ⇒ 一个 guest global 都不读（只有 start 那一行日志）。
        if (settings.on) applyAtSetmap(ctx, settings);
        if (settings.diag) logDiag(ctx);
        // ★ 放回**同一个**集合（不是新造的 trigger）：Reenter 的 `last_inside` 挂在它的 state 上，
        //   跨 arm 保留 ⇒ 不会在同一个读点反复触发；`exhausted()` 也不耗尽它（mode != Once、
        //   maxFires = 0）⇒ `install()` 收得下（`trigger_runner.cpp`）。
        return triggers !== null ? triggers : [ev.trigger];
    },
};

export default plugin;
