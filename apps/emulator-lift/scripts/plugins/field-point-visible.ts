// scripts/plugins/field-point-visible.ts —— `FIELD` 上的**开关**：渲染帧无条件显示所有采集 / 挖掘点
//
// 是什么（★ 用户裁决 2026-11-25：目标不是"在 FIELD 里无条件显示 `?`/`!` 图标"）：
//   · 采集 / 挖掘点的本体 = **B 链点位精灵**（`DRAWOBJ.BIN` 每槽 `draw-texture`，纹理槽 = `0xFA + 类型`：
//     `0x119` = `MO031B.AGF` 绿草 / `0x11A` = `MO032B.AGF` 岩石）。`?` / `!` 是 **A 链格标记**
//     （`SO030.AGF` 槽 `0x49`），**不是**点位，本插件不改它们。
//   · 精灵画不画 = 派生渲染位 `FIELD_OBJECT_FLAGS_GLOBAL_ID`（`50ff9f[slot]`）的位：
//     **bit0=1 ⇒ 画**（`DRAWOBJ` #7 `and … 1` → `jcc`：cond==0 才跳 ⇒ bit0=0 才整段跳过）。
//   · ★★ **bit1 / bit2 由引擎决定（2026-11-26 最终模型）**：`SETOBJ.BIN` #2 每渲染帧
//     `fill-zero (global-int 50ff9f) 32` 之后逐槽按"发现 / 触发状态"重算
//     （#34 bit0 = 画 · #84 bit2 = 动效 alpha 正常 · #86 bit1 = 还没用完）⇒
//     **bit2 就是静态 / 动效的选择位**（`DRAWOBJ` #26 `and … 4` 为 0 ⇒ #29 把动效 alpha 置 0），
//     **bit1=1 且 bit2=0** 才是静态门（#46-55 的 `(flags & 6) == 2`）。
//     真机读数（2026-11-26 / 存档 012 / FIELD 就绪、**无**本插件）：slot3 `0x5`（已触发 ⇒ 动效）·
//     slot4/5/6 `0x3`（未采集 ⇒ 静态）⇒ **引擎自己就是对的**。
//   · ⇒ 本插件**唯一的写 = `flags | 0x1`**（补"画"位：让"未发现但本应存在"的点也画出来）。
//     ⛔ 不写 bit1（`flags & 2` 被 A 链格表与 `FIELD` 的战利品抽取消费）、⛔ 不写 bit2
//     （那是引擎的静态 / 动效选择位；旧实现 `|= 0x5` 把 bit2 强加上去 ⇒ 未采集的点也在动）。
//   · ★★ **为什么每渲染帧都要补**：`SETOBJ` #2 是 `fill-zero (global-int 50ff9f) 32`，之后逐槽重算
//     ⇒ **插件写在这张派生表上的值，在同一次渲染里必被抹掉**。而 `SETOBJ` 只在 `RENDERMAP` 里跑
//     （#117 `call-script 511a` → #118 `call-script 5117`）⇒ 施加点必须在 `SETOBJ` **之后**。
//   · ★★ **为什么挂在 `DRAWOBJ.BIN` 入口**：全语料 `call-script 5117` 只有 **4 处**
//     （`.tmp/field-idle/sites.md` 的全表）：
//       ① `RENDERMAP.BIN` #118 `0x0B04`（前面 #117 就是 SETOBJ = 唯一"重算过"的那一趟）；
//       ② `FIELD.BIN` #2722 `0x0F034`（`0x0F028` 那一块：画一帧 → 音效/淡出/`wait`/`poll-input`）；
//       ③ `FIELD.BIN` #9688 `0x0392BC`（叶 `label_000392b0` = `DRAWMAP → DRAWOBJ → DRAWMINIMAP`；
//          **逐帧**那条走它：`FIELD` #838 `0x04EC4` `call label_000392b0`）；
//       ④ `RESETMAPAN.BIN` #1 `0x0050`（进游戏 / 配置那一段的一次性绘制）。
//     ⇒ `DRAWOBJ` **就是**那个收敛点：挂在它的入口，4 处调用者一次性全覆盖（含逐帧那条）。
//
// ★ **v2 形状（2026-11-28）—— 两条 `ctx.at` 订阅，没有 trigger / onTrigger**：
//   · `fp:draw` = `ctx.at({script:"DRAWOBJ.BIN", pos: DRAWOBJ_ENTRY_LO}, …)`：入口指令**每次执行一次**
//     （= 每进一次 DRAWOBJ），语义与 v1 `afterPosition(..., mode:"reenter")` 等价，而且**不依赖读点**
//     （v1 靠 `0x64/0x2C4` 的 `peekMsg` 读点才 fire；v2 是脚本派发点，更精确）。
//   · 静态门 = `ctx.at({script:"DRAWOBJ.BIN", pos: DRAWOBJ_GATE_EQ}, (tick) => { if (enabled)
//     tick.patch({ptrLocal:2}, 1); })`：每拍瞬时覆盖（handler 之后、指令执行之前写；下一拍 CAS 恢复）。
//     v1 的 `ctx.hook.at` + `handle.patch` 是"装一次、每命中都覆盖"；v2 是"每命中重新 patch 一次"——
//     语义等价（都只在 `DRAWOBJ 0x03A0` 那一拍覆盖、净变化 0），代价是每次多一次 JS 调用。
//   · `on` / `diag` 仍然**每次 fire 重读**（`./lib/settings.ts` 的 `ctx.config` 门 = Run 侧入口；
//     P0 起 tick handler 里经存下来的 RunCtx 读配置**合法**）⇒ 设置页 `set` 之后下一次入口就生效。
//   · 日志走**全局 `log`**（v2；`ctx.log` 已弃用）。
//
//   · ★ **每帧日志纪律**：`fp:draw` 每帧都 fire ⇒ ⛔ 不许每帧刷日志。`on=1` 时：
//       ① 首帧（每个 Run 一次）打一行 `field-points: draw first … instance=…`（证明订阅真进来过）；
//       ② 首帧 / `matched` / `patched` 变化时打一行汇总，并只为**真的改了**的槽打逐槽
//          `slot=… type=… flags before→after`（最多 `MAX_LOGGED_SLOTS` 行）；
//       ③ `diag=1` 时每 `DIAG_DRAW_EVERY`（= 60）帧打一行汇总 + **命中槽全量**逐槽一行
//          `field-points: diag draw frame=… slot=… type=… flags=0x… applied=0/1`
//          （`flags` = 写之前读到的值）。`diag=0` 时 ① ② 照旧、③ 不出现。
//   · ★ **判据是"已知的脚本位置"，所以用位置订阅，⛔ 不用轮询**（父级裁决 2026-11-25）。
//
// ⛔ 三件事本插件**永远不做**（用户裁决：开关不许驱动任何输入）：
//   ① ⛔ **不读档**：不 import `lib/save.ts`（`loadSave`）、不 import `lib/boot-to-title.ts`、
//      也不 import 已删除的 `lib/chains.ts`。验收时**玩家自己读档**：`--overlay --plugin
//      field-point-visible` 是目标用法（`--plugin` 与 `--input-plan` / `--control-root` 互斥 ⇒
//      "输入计划驱动"这条路不存在）。FIELD 的**自动化复现**仍归 `scenario-save10-field.ts`。
//   ② ⛔ **不点鼠标**：一次都不碰输入写面（插件 API 的输入面只有它 ⇒ 不碰 = 不驱动输入）。
//   ③ ⛔ **不写盘**：不写 `data/**`、不写存档、不写配置（config 只读；唯一的写是 `settings.schema`
//      这个**机器写的保留域**，由 `registerSettings` 在 `start` 里做，见下）。
//   ⇒ 守卫 `apps/emulator-lift/tests/plugin_field_switch.cpp` 两段判据：
//      **静态**（剥注释后）"代码里没有 input / loadSave / bootToTitle / chains / finish / quit"，
//      且仍在用位置订阅 + `DRAWOBJ_ENTRY_*` / `fp:draw` + `forceFieldPointsVisible`；
//      **端到端**（`emulator-lift-aot --plugin` headless，无 `--input-plan`）"没有任何 `boot:` / `load:`
//      行、Run 保持常驻（readout `run=waiting`、`stop: time-limit`）、首行日志 `field-points: on=0`"。
//
// ★ **常驻语义**：正常路径**永不**结束 ⇒ ⛔ 不调用 `finish`（进程按 `--time-limit` / 关窗结束），
//   只有出错才 `fail`。`--time-limit` 到期属于正常结局（宿主 exit 0）。两条 `at` 订阅算
//   `hasPendingWork` ⇒ Run 常驻（v1 靠 armed trigger 常驻）。
//   ★ **2026-11-28 P1 复审 S2：不再用"永不 settle 的 task"表达常驻**。那个 task 是绕过宿主
//     readout 顺序的 hack（`aot_main` 原先在打 readout **之前** `releaseOwner()` ⇒ `at` 订阅已被撤
//     ⇒ 只有 `tasks` 还能让 readout 说 `run=waiting`）。修法在宿主：`aot_main.cpp` 在 `releaseOwner()`
//     **之前**拍 readout 快照（顺序 = 快照 → release → 打印）⇒ 只靠 `ctx.at` 常驻的插件在 readout 里
//     现在**本来就**是 `run=waiting`（`plugin-field-switch` 的 e2e 判据照旧）。⇒ 本文件的常驻 =
//     那两条订阅本身，⛔ 不要再加一个不会 settle 的 Promise。
// ★ **不改"这点位发现过没有"的真值**：只写 `50ff9f`（派生表）；持久输入（`50ffd1` 类型 / `13d7` /
//   `3e2a0` / `14d8cc` / `eb5c1` / `25c00`）一个都不写，而且**一个都不读**。
//   ★ 例外（2026-11-26 晚）：静态门的第二条腿用 **`ctx.at` + `tick.patch`** 在 `DRAWOBJ` 的 `eq`
//     （pos `0x03A0`）执行前把 `local-ptr 2` 指向的 dword 临时改成 1、执行完恢复 ⇒ 那是**瞬时覆盖**，
//     `eb5c1` 的持久值净变化 0。
//   `on=0` ⇒ 一个 guest global 都不读、一个都不写（日志明写 `on=0`；静态门订阅也不装）。
//
// ★ **设置面（D1/D3）**：`start` 里 `registerSettings` 把 `on` / `diag` 写进保留域 `settings.schema`
//   （设置页据此画控件）；这两项**每次 fire 重读**（页面 `set` 之后下一次位置订阅就生效）。
//   `types` **不在 schema 里** ⇒ 只在 start 读一次并缓存。
//
// 用法（cwd = apps/emulator-lift；参数来源 **config > --plugin-param > 默认**）：
//   1) 在 **appdata 根**（与 `Local/` 同级，⛔ 不在 `SAVE/` 下）写 `plugin-config.json`：
//        {"version":1,"domains":{"field.point-visible":{"on":1}}}
//      域 key（全部可选；缺省 = `on false` / `types ["1f","20"]` / `diag 0`）：
//        `on` bool|0/1|"true"/"false" · `types` hex 串 `"1f,20"` 或数组 · `diag` bool|0/1
//      ★ 旧配置里残留的 `static` / `slot` / `hold` / `poll` = **无害忽略**（本文件既不读、也不报错）。
//   2) 玩家自己读档进 FIELD；在此之前插件只是装了两条位置订阅（一个读点都没碰 guest）：
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

import type { ConfigDomain, PluginModule, RunCtx, Tick } from "@amayui/lift-plugin";

import {
    FIELD_CELL_BITS_GLOBAL_ID,
    FIELD_CURRENT_MAP_GLOBAL_ID,
    FIELD_DYN_TEXTURE_SLOT_GLOBAL_ID,
    FIELD_MAP_INSTANCE_GLOBAL_ID,
    FIELD_OBJECT_FLAGS_GLOBAL_ID,
    FIELD_POINT_COL_GLOBAL_ID,
    FIELD_POINT_ROW_GLOBAL_ID,
    FIELD_POINT_TYPE_DIG,
    FIELD_POINT_TYPE_GATHER,
    FIELD_TYPE_CLASS_GLOBAL_ID,
    forceFieldPointsVisible,
    hex,
    readIntGlobal,
} from "./lib/guest-globals.ts";
import { boolSetting, openConfigDomain, registerSettings } from "./lib/settings.ts";
import {
    DRAWOBJ_ENTRY_HI,
    DRAWOBJ_ENTRY_LO,
    DRAWOBJ_GATE_EQ,
} from "./lib/script-positions.ts";

/**
 * 本次 fire 的设置（`on` / `diag` **每次 fire 重读**；`types` 一个 Run 内不变）。
 */
interface Settings {
    readonly on: boolean;
    readonly types: number[];
    readonly diag: boolean;
}

/** 配置文件里的域（`<--appdata>/plugin-config.json`）。 */
const CONFIG_DOMAIN = "field.point-visible";

/**
 * 点位绘制脚本的宿主 marker 拼写（订阅的 `script` 侧是**精确 `==`**：带 `.BIN`、不折叠大小写）。
 * 位置表：`DRAWOBJ_ENTRY_LO/HI` = 入口 `#0 detach-texture` → 点位槽循环尾（`[0x003C,0x06BC)`）。
 * ★ 全语料 `call-script 5117` 的 4 处（`RENDERMAP` #118 · `FIELD` #2722/#9688 · `RESETMAPAN` #1）
 *   都从这里进 ⇒ 一条订阅覆盖全部调用者。
 */
const DRAWOBJ_SCRIPT = "DRAWOBJ.BIN";

/** 唯一位置订阅的名字（日志标签；v1 的 trigger name 逐字保留，方便与旧读数对齐）。 */
const TRIGGER_NAME_DRAW = "fp:draw";

/** 日志里最多逐槽打几行（⛔ 不刷屏；宿主另有 100 行上限）。 */
const MAX_LOGGED_SLOTS = 12;

/** `fp:draw` 每帧都 fire ⇒ `diag` 档**每 N 帧**一行（⛔ 不是每帧）。 */
const DIAG_DRAW_EVERY = 60;

/**
 * `types`：config 可给 hex 串 `"1f,20"` 或数组（元素 = hex 串或数字）；params 只有逗号串。
 *
 * @param raw 缺省 = undefined/null ⇒ 採集 + 発掘。
 */
function parseTypes(raw: unknown): number[] {
    if (raw === undefined || raw === null) {
        return [FIELD_POINT_TYPE_GATHER, FIELD_POINT_TYPE_DIG];
    }
    // `--plugin-param types=`（空串）= 没给（旧行为）；config 里给空串 ⇒ 响亮拒绝（下走 out.length）。
    if (typeof raw === "string" && raw.trim() === "") {
        return [FIELD_POINT_TYPE_GATHER, FIELD_POINT_TYPE_DIG];
    }
    const out: number[] = [];
    const pushOne = (item: unknown): void => {
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

// ---- 配置读取（**唯一的取配置入口** = `./lib/settings.ts`；这里只留 per-Run 缓存）------------

/**
 * 域句柄（模块级缓存的是**对象**，⛔ 不是值）：`domain()` 每次会新建绑定对象 + 若干宿主调用
 * ⇒ 每次 fire 重读只 `get`，不重新 `domain()`。`undefined` = 还没打开；`null` = config 不可用。
 */
let configDomain: ConfigDomain | null | undefined;
/** `types` 一个 Run 内读一次（本期 schema 不暴露它 ⇒ 页面改不到）。 */
let typesCache: number[] | null = null;

/**
 * 本次 fire 的设置：**config > `--plugin-param` > 默认**（优先级只看"给没给"，`0` / `false` 也算给了）。
 *
 * ★ `on` / `diag` **每次 fire 重读** = D3 的 live 读点：设置页 `set` 之后下一次位置订阅就生效。
 * ★ `types` 只在第一次读（start）：它**不在**本期 schema 里，页面改不到；要暴露它时必须
 *   把这一行一起搬进下面的逐 fire 读。
 * ★ 旧配置里的 `static` / `slot` / `hold` / `poll` **不在这里出现**：它们被**无害忽略**。
 *
 * @param ctx RunCtx。
 */
function settingsFor(ctx: RunCtx): Settings {
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

/** `fp:draw` 是否已经打过首帧汇总（首帧带逐槽明细；之后只打状态变化）。 */
let drawAnnounced = false;
/** `fp:draw` 的累计帧数（只用于 `diag` 的每 N 帧一行与日志里的 `frame=`）。 */
let drawFrames = 0;
/** 上一次 `fp:draw` 的 `matched/patched`（状态变化才打一行；`""` = 还没打过）。 */
let lastDrawState = "";
/** 开关状态（每次 `fp:draw` 从配置重读；静态门 handler 必它决定这一拍要不要 patch）。 */
let enabled = false;
/** `fp:draw` 的订阅 id（`ctx.at` 失败时是 undefined）。 */
let drawSub: number | undefined;
/** 静态门订阅 id（`on=0` ⇒ 撤掉，宿主表里不留订阅）。 */
let gateSub: number | undefined;

/**
 * 装 / 撤 **静态门的"到点 + 覆盖"**（v2：一条 `ctx.at` 订阅；覆盖在 handler 里逐拍 `tick.patch`）。
 *
 * `DRAWOBJ` 的静态门第二条腿是 `eq (local-int c) (local-ptr 2) 1`（pos `0x03A0`）—— 它读的 dword 是
 * `eb5c1[map*50 + slot]`（"这个点位触发过"）。未触发的点位读到 0 ⇒ 静态门不过 ⇒ 走动效分支，
 * 而 `flags` 的 bit2=0 又把动效 alpha 清零 ⇒ **画面里什么都看不到**。
 *
 * 本插件在那条指令上装一条订阅：每条命中拍都把 `{ptrLocal: 2} → 1` 作**本拍瞬时覆盖**——指令执行前
 * 写、执行完立刻恢复 ⇒ 静态门通过（画出 `0xFA+type` 的静态纹理），`eb5c1` 的持久值**净变化 0**。
 * ⛔ 不用 `ctx.edit.global.set` 写 `eb5c1`：那是**改存档真值**（用户裁决 2026-11-26）。
 *
 * @param ctx RunCtx。
 * @param on 开关状态。
 */
function syncGate(ctx: RunCtx, on: boolean): void {
    if (on === (gateSub !== undefined)) return;
    if (on) {
        const id = ctx.at({ script: DRAWOBJ_SCRIPT, pos: DRAWOBJ_GATE_EQ }, (tick, next) => {
            // ★ `enabled` = 本拍 `fp:draw` 从配置读到的开关（同一次 DRAWOBJ 入口先跑 0x003C，再跑
            //   0x03A0）⇒ 这里与 v1 的"装/撤 hook"等价，但不需要重建订阅表。
            if (enabled) tick.patch({ ptrLocal: 2 }, 1);
            next();
        });
        if (id === undefined) {
            log.warn(`field-points: 静态门订阅装不上（${DRAWOBJ_SCRIPT} pos=0x${DRAWOBJ_GATE_EQ.toString(16)}）`);
            return;
        }
        gateSub = id;
        log.info(
            `field-points: 静态门订阅已装（${DRAWOBJ_SCRIPT} pos=0x${DRAWOBJ_GATE_EQ.toString(16)} ` +
                "ptrLocal=2 value=1；每拍本拍覆盖）",
        );
        return;
    }
    if (gateSub !== undefined) ctx.off(gateSub);
    gateSub = undefined;
    log.info("field-points: 静态门订阅已撤（ctx.off；未还原的写按 CAS 结算）");
}

/**
 * **每次进 `DRAWOBJ` 时的施加**（`fp:draw`）：对类型命中的槽补 `50ff9f` 的 **bit0**。
 *
 * ★ **为什么每次进 DRAWOBJ 都要做**：`RENDERMAP` 那一趟先 SETOBJ（#2 `fill-zero 50ff9f 32` 再逐槽重算）
 *   ⇒ 上一次补的 bit0 在那一次渲染里已被抹掉；其它入口沿用的是上一次的值。入口施加 = 四条路都成立。
 * ★ **代价**：每次 31 次类型读 + 命中槽的 flags 读；写只在 `after !== before` 时发生
 *   （`forceFieldPointsVisible` 内部保证）。⚠ 若 SETOBJ 当帧把点位槽重算成 `0x2`（bit0=0），
 *   每帧会写一次 `0x2 → 0x3` —— 那是本开关唯一要改的东西。
 * ★ **写面**：本拍的 `Tick`（`tick.edit`）；⛔ `ctx.edit` 在 tick handler 里是 TypeError。
 * ★ **日志**：首帧打一行 `draw first`（每 Run 一次）+ 汇总；`matched/patched` 变化才再打**一行**汇总；
 *   逐槽行只为**真的改了**的槽打（限 `MAX_LOGGED_SLOTS` 行）；`diag` 每 `DIAG_DRAW_EVERY` 帧一行
 *   汇总 + 命中槽全量逐槽行。⛔ 绝不每次都刷屏。
 *
 * @param ctx RunCtx（读：`ctx.read`）。
 * @param tick 本拍的 Tick（写：`tick.edit`；`tick.now.pos` 只用读数）。
 * @param settings 本次 fire 的设置。
 */
function applyAtDraw(ctx: RunCtx, tick: Tick, settings: Settings): void {
    drawFrames += 1;
    // ★ 命中槽（type 读在里面，31 次/次）；`applied=true` 的那些才是真的改了 flags。
    const changed = forceFieldPointsVisible(ctx, tick, { types: settings.types });
    const patched = changed.filter((c) => c.applied);
    const state = `${changed.length}/${patched.length}`;
    if (!drawAnnounced || state !== lastDrawState) {
        const first = !drawAnnounced;
        drawAnnounced = true;
        lastDrawState = state;
        const map = readIntGlobal(ctx, FIELD_CURRENT_MAP_GLOBAL_ID);
        // ★ 每次 Run **只此一行**的"fp:draw 真 fire 过"证据（含地图 / 实例号）：
        //   用户侧判据 = 日志里有没有 `draw first`。它与下面的汇总行**不重复**：这一行证明
        //   "订阅进来过"，汇总行证明"这次进来匹配 / 写了几个槽"。
        if (first) {
            const instance = readIntGlobal(ctx, FIELD_MAP_INSTANCE_GLOBAL_ID);
            log.info(
                `field-points: draw first frame=${drawFrames} matched=${changed.length} ` +
                    `patched=${patched.length} map=${hex(map)} instance=${hex(instance)}` +
                    "（DRAWOBJ.BIN 入口；每个 Run 只打这一次）",
            );
        }
        log.info(
            `field-points: draw frame=${drawFrames} matched=${changed.length} ` +
                `patched=${patched.length} map=${hex(map)}（DRAWOBJ.BIN 入口；每次进这个脚本一次）`,
        );
        for (const c of patched.slice(0, MAX_LOGGED_SLOTS)) {
            log.info(
                `field-points: draw slot=${c.slot} type=${hex(c.type)} flags ${hex(c.before)}→${hex(c.after)}`,
            );
        }
        if (first) {
            log.info(
                `field-points: 只写过 ${hex(FIELD_OBJECT_FLAGS_GLOBAL_ID)}+slot 的 bit0` +
                    "（bit1 / bit2 是引擎的；类型表与持久输入未写）",
            );
        }
    }
    if (settings.diag && drawFrames % DIAG_DRAW_EVERY === 0) {
        const pos = tick.now.pos;
        log.info(
            `field-points: diag draw frame=${drawFrames} matched=${changed.length} ` +
                `patched=${patched.length} pos=${pos === undefined ? "-" : hex(pos)}` +
                `（每 ${DIAG_DRAW_EVERY} 次一行；下面逐槽是命中槽全量）`,
        );
        // ★ **逐槽全量**（不是只打 applied / 只打变化）：诊断要的是"fire 那一刻引擎在 `50ff9f`
        //   里留了什么"。`flags` = **写之前**读到的值（`after` 恒等于 `flags|1`，没有额外信息）；
        //   `applied=1` 才表示本插件真的写了这一槽（`0` = bit0 本来就在 / 写回执失败）。
        for (const c of changed) {
            const stage = readIntGlobal(ctx, FIELD_CURRENT_MAP_GLOBAL_ID) >>> 0;
            const row = readIntGlobal(ctx, FIELD_POINT_ROW_GLOBAL_ID + c.slot) >>> 0;
            const col = readIntGlobal(ctx, FIELD_POINT_COL_GLOBAL_ID + c.slot) >>> 0;
            const cellA = (FIELD_CELL_BITS_GLOBAL_ID + stage * 100 + row) >>> 0;
            const cellB = (FIELD_CELL_BITS_GLOBAL_ID + stage * 100 + row * 4) >>> 0;
            const wA = readIntGlobal(ctx, cellA & ~3) >>> 0;
            const wB = readIntGlobal(ctx, cellB & ~3) >>> 0;
            const dynSlot = readIntGlobal(ctx, FIELD_DYN_TEXTURE_SLOT_GLOBAL_ID + c.type) >>> 0;
            const cls = readIntGlobal(ctx, FIELD_TYPE_CLASS_GLOBAL_ID + c.type) >>> 0;
            log.info(
                `field-points: diag draw frame=${drawFrames} slot=${c.slot} type=${hex(c.type)} ` +
                    `flags=${hex(c.before)} applied=${c.applied ? 1 : 0} ` +
                    `stage=${hex(stage)} row=${row} col=${col} cellA=${hex(cellA)} wA=${hex(wA)} ` +
                    `cellB=${hex(cellB)} wB=${hex(wB)} dyn=${hex(dynSlot)} cls=${cls}`,
            );
        }
    }
}

const plugin = {
    start(ctx: RunCtx): void {
        // ★ 位置表的不变量：静态门必须落在 DRAWOBJ 的入口窗口里（否则那条订阅永远不会命中）。
        //   这条检查同时把 `DRAWOBJ_ENTRY_HI` 的消费方留在代码里（表本身是生成物，数值不动）。
        if (!(DRAWOBJ_ENTRY_LO <= DRAWOBJ_GATE_EQ && DRAWOBJ_GATE_EQ < DRAWOBJ_ENTRY_HI)) {
            ctx.fail("field-points", "静态门 pos 不在 DRAWOBJ 入口窗口里（script-positions.ts 的读数坏了？）");
            return;
        }
        const settings = settingsFor(ctx);
        enabled = settings.on;
        // ★ D1：schema 写进保留域 `settings.schema`（机器写；先 all() 比、相同不写）。
        //   ⛔ 失败只落一行 warn（`registerSettings` 内部 catch）—— 绝不能因为写不了设置而打死 Run。
        registerSettings(ctx, {
            domain: CONFIG_DOMAIN,
            owner: "field-point-visible",
            title: "FIELD points",
            live: "run",
            entries: {
                on: { type: "bool", label: "points visible", default: false },
                diag: { type: "bool", label: "diag", default: false },
            },
        });
        drawAnnounced = false;
        drawFrames = 0;
        lastDrawState = "";
        gateSub = undefined;
        // ★ 装静态门覆盖（`on=0` ⇒ 不装：一条订阅都不进宿主表；与 v1 的 `handle.clear()` 等价）。
        syncGate(ctx, settings.on);
        // ★ **唯一**常驻触发面：`fp:draw`（**`DRAWOBJ.BIN` 入口** `DRAWOBJ_ENTRY_LO`），
        //   每次进点位绘制脚本一次 ⇒ 为类型命中的槽补 `50ff9f` 的 bit0。4 处 `call-script 5117`
        //   全部经过这里（SETOBJ 每帧 `fill-zero` 抹掉上一次的补位 ⇒ 每帧都要补）。
        //   ⛔ **不要**在这里再装一个轮询（那会把"已知位置"退化成轮询）。
        drawSub = ctx.at({ script: DRAWOBJ_SCRIPT, pos: DRAWOBJ_ENTRY_LO }, (tick, next) => {
            // ★ 每次 fire 重读 on / diag（D3：设置页 set 之后下一次位置订阅就生效）。
            const now = settingsFor(ctx);
            enabled = now.on;
            // 静态门覆盖随 `on` 切换（`on=0` ⇒ 撤掉，宿主表里不留订阅）。
            syncGate(ctx, now.on);
            // `on=0` ⇒ 一个 guest global 都不读（只有 start 那一行日志）。`fp:draw` 每帧仍 fire，
            //   但这一支不进来 ⇒ 仍然 0 读。
            if (now.on) applyAtDraw(ctx, tick, now);
            // ★ 继续同点的后续订阅（今天没有；`next()` 让链语义保持显式）。
            next();
        });
        // ★ 首行日志：明写开关状态（守卫按 `on=0` 判"关着的时候什么也不做"）。
        log.info(
            `field-points: on=${settings.on ? 1 : 0} ` +
                `types=${settings.types.map(hex).join(",")}` +
                `（每次进 ${DRAWOBJ_SCRIPT} 显示所有采集/挖掘点；订阅 ${TRIGGER_NAME_DRAW}）`,
        );
        if (drawSub === undefined) {
            // `no_injection`（`--no-s7` / 闭包没覆盖派发 EA）⇒ 宿主已经打过一行显眼 error；
            // 这里把 Run 响亮收口（⛔ 不要留一个永远不命中的订阅）。
            ctx.fail("field-points", "ctx.at 装不上（no_injection）");
            return;
        }
        // ★ **Run 常驻 = 上面这两条 `ctx.at` 订阅本身**（`fp:draw` + 可选的静态门）。
        //   `hasPendingWork` 计入 `at_subs`（`script_quickjs.cpp`）⇒ 正常路径不收口、readout
        //   `run=waiting`。★ 2026-11-28（P1 复审 S2）：这里原先还有一个**永不 settle 的 task**
        //   （`await new Promise(() => {})`）专门为了让旧 readout 顺序说出 `run=waiting` ——
        //   宿主已改成"先拍 readout 快照、再 `releaseOwner()`"⇒ 那个 task 不再需要，删掉后
        //   常驻判据回到插件语义（⛔ 不要把"永不 settle task"当常驻 API）。
        //   ⛔ 不要在正常路径 `finish`。
    },
} satisfies PluginModule;

export default plugin;
