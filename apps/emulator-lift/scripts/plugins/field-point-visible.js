// scripts/plugins/field-point-visible.js —— `FIELD` 上的**开关**：渲染帧无条件显示所有采集 / 挖掘点
//
// 是什么（★ 用户裁决 2026-11-25：目标不是"在 FIELD 里无条件显示 `?`/`!` 图标"）：
//   · 采集 / 挖掘点的本体 = **B 链点位精灵**（`DRAWOBJ.BIN` 每槽 `draw-texture`，纹理槽 = `0xFA + 类型`：
//     `0x119` = `MO031B.AGF` 绿草 / `0x11A` = `MO032B.AGF` 岩石）。`?` / `!` 是 **A 链格标记**
//     （`SO030.AGF` 槽 `0x49`），**不是**点位，本插件不改它们。
//   · 精灵画不画 = 派生渲染位 `FIELD_OBJECT_FLAGS_GLOBAL_ID`（`50ff9f[slot]`）的两位：
//     **bit0=1 ⇒ 画**（`DRAWOBJ` #7 `and … 1` → `jcc`：cond==0 才跳 ⇒ bit0=0 才跳过）·
//     **bit2=1 ⇒ 动效可见**（#26 `and … 4` 为 0 ⇒ #29 把 `0x3A98+slot` 的 alpha 置 0）。
//   · 所以本插件对**类型命中**（缺省 採集 `0x1F` / 発掘 `0x20`）的槽写 `flags |= 0x5`（只置位、⛔ 不清位）。
//     ★ bit1 **不要清**：A 链格表与 `FIELD` `label_00030E54` 的战利品抽取都消费 `flags & 2`。
//   · ★★ **静态 / 动效的选择位就是 bit2**（2026-11-26 订正，见 `lib/guest-globals.js` 文件头的取证段）：
//     静态精灵 `MO031B/MO032B` 走 `0x3ACA+slot`，它的门是
//     `DRAWOBJ` #46-55 `511080[type]==3 && eb5c1[b222][slot]==1 && (flags&6)==2` **且**
//     #56-58 的 `25c00[b222][row]` 第 `col` 位 == 1。⇒ **bit2=1 时静态一定不画、bit2=0 时动效全透明**。
//     上一轮只 `|=0x5` ⇒ 看到的一定是**动效**（用户实测正是如此）。
//   · `static`（缺省 **1**）⇒ 在**每一次 `DRAWOBJ.BIN` 的入口**（`fp:draw`）再判一次引擎自己的静态门：
//     成立就把 bit2 **清掉**（静态可见、动效 alpha 0）；不成立就**保留** bit2（= 今天的动效，⛔ 不让点消失）。
//     `static=0` ⇒ 只保留 `|=0x5`，**一个** `25c00`/`14d73c`/`14d76e`/`14d8cc`/`eb5c1` 读都不做。
//   · ★★ **为什么每渲染帧都要施加**：`SETOBJ` #2 是 `fill-zero (global-int 50ff9f) 32`，之后逐槽重算
//     ⇒ **插件写在这张派生表上的值，在同一次渲染里必被抹掉**。而 `SETOBJ` 只在 `RENDERMAP` 里跑
//     （#117 `call-script 511a` → #118 `call-script 5117`）⇒ 施加点必须在 `SETOBJ` **之后**。
//   · ★★ **为什么挂在 `DRAWOBJ.BIN` 入口（2026-11-26 第二轮订正）**：全语料 `call-script 5117` 只有 **4 处**
//     （`.tmp/field-idle/sites.md` 的全表；`node tools/script-positions.mjs` 的锚即这 4 处的入口）：
//       ① `RENDERMAP.BIN` #118 `0x0B04`（前面 #117 就是 SETOBJ = 唯一"重算过"的那一趟）；
//       ② `FIELD.BIN` #2722 `0x0F034`（`0x0F028` 那一块：画一帧 → 音效/淡出/`wait`/`poll-input`）；
//       ③ `FIELD.BIN` #9688 `0x0392BC`（叶 `label_000392b0` = `DRAWMAP → DRAWOBJ → DRAWMINIMAP`；
//          **逐帧**那条走它：`FIELD` #838 `0x04EC4` `call label_000392b0`）；
//       ④ `RESETMAPAN.BIN` #1 `0x0050`（进游戏 / 配置那一段的一次性绘制）。
//     ⇒ `DRAWOBJ` **就是**那个收敛点：挂在它的入口，4 处调用者一次性全覆盖（含逐帧那条）。
//     上一轮只挂 ① RENDERMAP 的调用点，结果**逐帧那条（③）与退图那条（②）都没被施加**，
//     而 ① 与 ③ 在同一帧里会用**不同的 flag 状态**各画一遍 ⇒ 同一格先画动效、再画静态（`0x7`/`0x3`）
//     ⇒ 用户看到"纹理交替出现/消失"。入口统一施加后，两条路都先经过这里，flag 状态一致。
//   · ★ **触发点 = guest 读点**（`TriggerRunner` 只在 `atPoint` poll；`docs/03-emulator/plugin-runtime.md` §4.2）：
//     实测 `DRAWOBJ.BIN` 体内有 `peekMsg` 读点（pos `0x64` / `0x2C4`，都早于第一条
//     `draw-texture` #58 @`0x66C`）⇒ `[DRAWOBJ_ENTRY_LO, DRAWOBJ_ENTRY_HI)` = `[0x003C,0x06BC)`
//     （入口 → 点位槽循环尾；第二条循环是 `6202a` 元素对象，不含点位精灵）的外→内边沿会 fire。
//     ⚠ `RENDERMAP [0x0B04,0x0B10)`（上一轮的窗口）在三次真机读数里**一次都没 fire**
//     （那个区间里没有会调 Win32 输入 API 的指令 ⇒ 读点永远不会落在它上面）。
//   · ★ **钩子 = `DRAWOBJ.BIN` 入口窗口**（父级/用户裁决 2026-11-26）：
//       `afterPosition("DRAWOBJ.BIN", DRAWOBJ_ENTRY_LO, DRAWOBJ_ENTRY_HI, { name: "fp:draw", mode: "reenter" })`
//     这是已知脚本位置，⛔ 不是轮询。
//   · 两条 `FIELD.BIN` setmap 钩子**保留**（只做 `|=0x5`；覆盖"退图前还没渲过"的窗口）：
//       ① `[FIELD_SETMAP_A_LO, FIELD_SETMAP_A_HI)` = `label_00018274` 内 `call-script 511a`(SETOBJ) 的
//          **返回点**（进图主路径）；
//       ② `[FIELD_SETMAP_B_LO, FIELD_SETMAP_B_HI)` = `0x035130` 块里同一条 `call-script 511a` 的返回点。
//     Reenter 的边沿是**有效读数的 外→内**（`src/runtime/trigger_runner.cpp`），`last_inside` 挂在**同一个
//     trigger 对象**上、跨 arm 保留 ⇒ 不需要 re-arm（`exhausted()` 只耗尽 Once / `maxFires>0`）。
//   · ★ **每帧日志纪律**：`fp:draw` 每帧都 fire ⇒ ⛔ 不许每帧刷日志。只在**首帧**或**状态变化**
//     （`matched` / `cleared` / `kept` 变了）打一行汇总（首帧另带逐槽明细）；`diag=1` 时每
//     `DIAG_DRAW_EVERY`（= 60）帧一行。退出 / 重进由状态变化自然覆盖。
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
//      且仍在用 `afterPosition` + `reenter` + 两个 setmap 区间 + `DRAWOBJ_ENTRY_*` / `fp:draw` +
//      `forceFieldPointsVisible` / `clearFieldStaticSkip`；
//      **端到端**（`emulator-lift-aot --plugin` headless，无 `--input-plan`）"没有任何 `boot:` / `load:`
//      行、Run 保持常驻（readout `run=waiting`、`stop: time-limit`）、首行日志 `field-points: on=0`"。
//
// ★ **常驻语义**：正常路径**永不**结束 ⇒ ⛔ 不调用 `finish`（进程按 `--time-limit` / 关窗结束），
//   只有出错才 `fail`。`--time-limit` 到期属于正常结局（宿主 exit 0）。
// ★ **不改"这点位发现过没有"的真值**：只写 `50ff9f`（派生表）；持久输入（`50ffd1` 类型 / `13d7` /
//   `3e2a0` / `14d8cc` / `eb5c1` / `25c00`）一个都不写。`on=0` ⇒ 一个 guest global 都不读、一个都不写
//   （日志明写 `on=0`）；`static=0` ⇒ 连 `25c00/14d7xx/14d8cc/eb5c1` 的读都不做（日志明写 `static=0`）。
//
// ★ **设置面（D1/D3，2026-11-25 / 2026-11-26 加 `static`）**：`start` 里 `registerSettings` 把
//   `on` / `static` / `diag` 写进保留域 `settings.schema`（设置页据此画控件）；这三项**每次 fire
//   重读**（页面 `set` 之后下一次位置触发就生效）。`types` **不在 schema 里** ⇒ 只在 start 读一次并缓存。
//
// 用法（cwd = apps/emulator-lift；参数来源 **config > --plugin-param > 默认**）：
//   1) 在 **appdata 根**（与 `Local/` 同级，⛔ 不在 `SAVE/` 下）写 `plugin-config.json`：
//        {"version":1,"domains":{"field.point-visible":{"on":1}}}
//      域 key（全部可选；缺省 = `on false` / `static true` / `types ["1f","20"]` / `diag 0`）：
//        `on` bool|0/1|"true"/"false" · `static` bool|0/1|"true"/"false" ·
//        `types` hex 串 `"1f,20"` 或数组 · `diag` bool|0/1
//      ★ `static:0` = **完全回到 2026-11-26 之前的行为**（只 `|=0x5` ⇒ 看到的是动效）。
//      ★ 旧配置里残留的 `slot` / `hold` / `poll` = **无害忽略**（本文件既不读、也不报错：
//        不再读档、不再轮询）。
//   2) 玩家自己读档进 FIELD；在此之前插件只是 arm 了三条位置触发（一个读点都没碰 guest）：
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
    clearFieldStaticSkip,
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
    DRAWOBJ_ENTRY_HI,
    DRAWOBJ_ENTRY_LO,
    FIELD_SETMAP_A_HI,
    FIELD_SETMAP_A_LO,
    FIELD_SETMAP_B_HI,
    FIELD_SETMAP_B_LO,
} from "./lib/script-positions.js";

/** @typedef {import("@amayui/lift-plugin").RunCtx} RunCtx */
/** @typedef {import("@amayui/lift-plugin").Trigger} Trigger */
/** @typedef {import("@amayui/lift-plugin").ConfigDomain} ConfigDomain */
/**
 * 本次 fire 的设置（`on` / `diag` / `static` **每次 fire 重读**；`types` 一个 Run 内不变）。
 *
 * @typedef {{ on: boolean, types: number[], diag: boolean, static: boolean }} Settings
 */

/** 配置文件里的域（`<--appdata>/plugin-config.json`）。 */
const CONFIG_DOMAIN = "field.point-visible";

/**
 * `FIELD` 的宿主 marker 拼写（trigger / `ctx.now.script` 侧是**精确 `==`**：带 `.BIN`、不折叠大小写）。
 * 位置表在 `./lib/script-positions.js`（`FIELD_SETMAP_A_LO/HI` / `FIELD_SETMAP_B_LO/HI`）。
 */
const FIELD_SCRIPT = "FIELD.BIN";

/**
 * 点位绘制脚本的宿主 marker 拼写（同上，**精确 `==`**）。
 * 位置表：`DRAWOBJ_ENTRY_LO/HI` = 入口 `#0 detach-texture` → 点位槽循环尾（`[0x003C,0x06BC)`）。
 * ★ 全语料 `call-script 5117` 的 4 处（`RENDERMAP` #118 · `FIELD` #2722/#9688 · `RESETMAPAN` #1）
 *   都从这里进 ⇒ 一条钩子覆盖全部调用者。
 */
const DRAWOBJ_SCRIPT = "DRAWOBJ.BIN";

/** 三条钩子的具名状态（每次 fire 原样放回**同一个**集合）。 */
const TRIGGER_NAME_A = "fp:setmap-a";
const TRIGGER_NAME_B = "fp:setmap-b";
const TRIGGER_NAME_DRAW = "fp:draw";

/** 日志里最多逐槽打几行（⛔ 不刷屏；宿主另有 100 行上限）。 */
const MAX_LOGGED_SLOTS = 12;

/** `diag` 档最多打几行（每次 fire 一行 `script` / `pos`）。 */
const DIAG_SAMPLES = 8;

/** `fp:draw` 每帧都 fire ⇒ `diag` 档**每 N 帧**一行（⛔ 不是每帧）。 */
const DIAG_DRAW_EVERY = 60;

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
        // ★ `static`（缺省 **true**）：进图显示**静态**点位纹理（`MO031B/MO032B`，无工具图标）；
        //   `static:0` ⇒ 完全回到 2026-11-26 之前的行为（`50ff9f |= 0x5`，看到的是动效）。
        //   ⛔ `static=0` 时**一个** `25c00`/`14d73c`/`14d76e`/`14d8cc`/`eb5c1` 读都不做。
        static: boolSetting(ctx, domain, "static", true, { domainId: CONFIG_DOMAIN }),
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
/** `static=0` 的那一行日志只打一次（它要能被用户看见，但⛔ 不许每次 fire 刷一行）。 */
let staticOffLogged = false;
/** `diag` 还剩几次（打够就安静常驻；⛔ 不结束 Run）。 */
let diagLeft = 0;
/** `fp:draw` 是否已经打过首帧汇总（首帧带逐槽明细；之后只打状态变化）。 */
let drawAnnounced = false;
/** `fp:draw` 的累计帧数（只用于 `diag` 的每 N 帧一行与日志里的 `frame=`）。 */
let drawFrames = 0;
/** 上一次 `fp:draw` 的 `matched/cleared/kept`（状态变化才打一行；`""` = 还没打过）。 */
let lastDrawState = "";

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
 * **每次进 `DRAWOBJ` 时的施加**（`fp:draw`）：先 `forceFieldPointsVisible`（`|=0x5`），再按 `static`
 * 判据对**类型命中**的槽调 `clearFieldStaticSkip`。
 *
 * ★ **为什么先 `|=0x5`**（保留 2026-11-26 之前的行为）：bit0=画、bit2=动效可见；`FIELD` #2722
 *   （`0x0F028` 那块）与 `RESETMAPAN` #1 那两条 `call-script 5117` 前面**没有** SETOBJ，两条 setmap 钩子
 *   也覆盖不到它们 ⇒ 在这里补上（入口统一，4 处调用者一视同仁）。
 * ★ **为什么每次进 DRAWOBJ 都要做**：`RENDERMAP` 那一趟先 SETOBJ（#2 `fill-zero 50ff9f 32` 再逐槽重算）
 *   ⇒ 上一次写的 bit2 在那一次渲染里已被抹掉；其它入口沿用的是上一次的值。入口施加 = 两条路都成立。
 * ★ **触发点**：`DRAWOBJ.BIN` 体内实测有 `peekMsg` 读点（pos `0x64` / `0x2C4`，都早于第一条
 *   `draw-texture`（#58，`0x66C`）⇒ `[0x003C,0x06BC)` 的外→内边沿会 fire（`TriggerRunner` 只在读点 poll）。
 * ★ **代价**：每次 31 次类型读 + 命中槽的 flags 读 + `clearFieldStaticSkip` 的判据读；写只在
 *   `after !== before` 时发生（`forceFieldPointsVisible` / `clearFieldStaticSkip` 内部保证）。
 * ★ **日志**：首帧或 `matched/cleared/kept` 变化才打**一行**（首帧另带逐槽明细）；`diag` 每
 *   `DIAG_DRAW_EVERY` 次一行。⛔ 绝不每次都刷屏。行里带 `site=<ev.name>`（三条钩子共用一个日志面）。
 * ★ `static=0` ⇒ 只 `|=0x5`：**一个** `25c00` / `14d73c` / `14d76e` / `14d8cc` / `eb5c1` 读都不做
 *   （那几张表只在 `clearFieldStaticSkip` 里读）。
 *
 * @param {RunCtx} ctx RunCtx。
 * @param {Settings} settings 本次 fire 的设置。
 * @param {string} site 本次 fire 的 trigger 名（`ev.name`；日志里区分三条钩子）。
 * @returns {void}
 */
function applyAtDraw(ctx, settings, site) {
    drawFrames += 1;
    // ★ 命中槽（type 读在里面，31 次/次）—— `static` 的循环直接复用它，⛔ 不再扫第二遍类型表。
    const changed = forceFieldPointsVisible(ctx, { types: settings.types });
    let cleared = 0;
    let kept = 0;
    /** @type {string[]} */
    const lines = [];
    if (settings.static) {
        for (const c of changed) {
            const r = clearFieldStaticSkip(ctx, c.slot);
            if (r.applied) cleared += 1;
            else if (!r.gate) kept += 1;
            lines.push(
                `field-points: draw slot=${r.slot} type=${hex(r.type)} row=${r.row} col=${r.col} ` +
                    `cellBit=${r.cellBit} gate=${r.gate ? 1 : 0} enabled=${r.enabled ? 1 : 0} ` +
                    `class=${r.classId} eb5c1=${r.triggered} ${hex(r.before)} → ${hex(r.after)}` +
                    `${r.applied ? "（静态）" : `（保留动效：${r.reason}）`}`,
            );
        }
    } else if (!staticOffLogged) {
        staticOffLogged = true;
        ctx.log(
            "info",
            "field-points: static=0（不改 50ff9f 的 bit2；25c00/14d7xx/14d8cc/eb5c1 一个读都不做）",
        );
    }
    const first = !drawAnnounced;
    const state = `${changed.length}/${cleared}/${kept}`;
    if (first || state !== lastDrawState) {
        drawAnnounced = true;
        lastDrawState = state;
        const map = readIntGlobal(ctx, FIELD_CURRENT_MAP_GLOBAL_ID);
        const instance = readIntGlobal(ctx, FIELD_MAP_INSTANCE_GLOBAL_ID);
        ctx.log(
            "info",
            `field-points: draw site=${site} frame=${drawFrames} matched=${changed.length} ` +
                `cleared=${cleared} kept=${kept} map=${hex(map)} instance=${hex(instance)}` +
                `（DRAWOBJ.BIN 入口；每次进这个脚本一次）`,
        );
        if (first) {
            for (const line of lines.slice(0, MAX_LOGGED_SLOTS)) ctx.log("info", line);
        }
    }
    if (settings.diag && drawFrames % DIAG_DRAW_EVERY === 0) {
        const pos = ctx.now.pos;
        ctx.log(
            "info",
            `field-points: diag draw site=${site} frame=${drawFrames} matched=${changed.length} ` +
                `cleared=${cleared} kept=${kept} pos=${pos === undefined ? "-" : hex(pos)}` +
                `（每 ${DIAG_DRAW_EVERY} 次一行）`,
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
                static: { type: "bool", label: "static sprite (no tool icon)", default: true },
                diag: { type: "bool", label: "diag", default: false },
            },
        });
        announced = false;
        lastInstance = null;
        staticOffLogged = false;
        drawAnnounced = false;
        drawFrames = 0;
        lastDrawState = "";
        diagOn = settings.diag;
        diagLeft = settings.diag ? DIAG_SAMPLES : 0;
        // ★ 首行日志：明写开关状态（守卫按 `on=0` 判"关着的时候什么也不做"）。
        ctx.log(
            "info",
            `field-points: on=${settings.on ? 1 : 0} static=${settings.static ? 1 : 0} ` +
                `types=${settings.types.map(hex).join(",")}（每次进 DRAWOBJ 显示所有采集/挖掘点）`,
        );
        // ★ **三个**常驻 trigger：
        //   ① / ② 两条 setmap 返回点（`FIELD.BIN`）：只 `50ff9f |= 0x5`；
        //   ③ `fp:draw`（**`DRAWOBJ.BIN` 入口** `[0x003C,0x06BC)`）：每次进点位绘制脚本一次
        //      ⇒ 先 `|=0x5`，再按静态门决定 bit2。4 处 `call-script 5117` 全部经过这里。
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
            ctx.trigger.afterPosition(DRAWOBJ_SCRIPT, DRAWOBJ_ENTRY_LO, DRAWOBJ_ENTRY_HI, {
                name: TRIGGER_NAME_DRAW,
                mode: "reenter",
            }),
        ];
        return triggers;
    },
    onTrigger(ctx, ev) {
        if (ev.kind === "timeout") {
            // 正常路径不会有 timeout（三条 afterPosition 都没配 timeoutMs）⇒ 真出现就是接线坏了。
            // ⛔ 这里**不用** `gates.js` 的 `failOnTimeout`：那条是 `finish(5)`（收口），而本开关
            //    正常路径永不收口（`plugin_field_switch.cpp` 的静态契约也钉着"代码里没有 finish"）。
            ctx.fail(String(ev.name), "field-points: unexpected timeout");
            return [];
        }
        const name = String(ev.name);
        const isSetmap = name === TRIGGER_NAME_A || name === TRIGGER_NAME_B;
        const isDraw = name === TRIGGER_NAME_DRAW;
        if (!isSetmap && !isDraw) {
            failOnUnknown(ctx, ev, "field-points");
            return [];
        }
        // ★ 每次 fire 重读 on / diag / static（D3：设置页 set 之后下一次位置触发就生效）。
        const settings = settingsFor(ctx);
        // `diag` 的边沿：页面把它打开 ⇒ 从这里再打 DIAG_SAMPLES 行（⛔ 不只在 start 定死）。
        if (settings.diag && !diagOn) {
            diagOn = true;
            diagLeft = DIAG_SAMPLES;
        } else if (!settings.diag) {
            diagOn = false;
            diagLeft = 0;
        }
        // `on=0` ⇒ 一个 guest global 都不读（只有 start 那一行日志）。`fp:draw` 每帧仍 fire，
        //   但这一支不进来 ⇒ 仍然 0 读。
        if (settings.on) {
            if (isSetmap) applyAtSetmap(ctx, settings);
            else applyAtDraw(ctx, settings, name);
        }
        // ★ `fp:draw` 的 diag 在 `applyAtDraw` 里按每 N 帧打（⛔ 不走这里的每 fire 一次）。
        if (settings.diag && !isDraw) logDiag(ctx);
        // ★ 放回**同一个**集合（不是新造的 trigger）：Reenter 的 `last_inside` 挂在它的 state 上，
        //   跨 arm 保留 ⇒ 不会在同一个读点反复触发；`exhausted()` 也不耗尽它（mode != Once、
        //   maxFires = 0）⇒ `install()` 收得下（`trigger_runner.cpp`）。
        return triggers !== null ? triggers : [ev.trigger];
    },
};

export default plugin;
