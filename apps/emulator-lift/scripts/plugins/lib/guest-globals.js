// scripts/plugins/lib/guest-globals.js —— **guest global** 的常量 + 操作（数字只在这里写一次）
//
// 口径：
//   · `kind` 只有 `"int"` / `"uint"` / `"float"` 走 `ctx.read.global.get` / `ctx.edit.global.set`
//     （`"string"` 型的 set 本实现未实现 ⇒ 调用即 TypeError；见 `scripts/plugins/plugin.d.ts`）。
//   · 读**失败**在 API 里没有错误通道 ⇒ 宿主抛 TypeError（`script_quickjs.cpp` 的
//     `jsCtxReadGlobalGet`：⛔ 不返回 0 冒充读数）。所以本文件**不**吞读异常 —— 让它穿到宿主，
//     归因里带 `file:line`（吞掉就变成"静默地什么都没改"）。
//   · 写**失败**有回执：`{ accepted, queued, applied, seq, id, code? }`（Edit 立即 ⇒ `accepted == applied`）。
//     `applied === false` ⇒ **warn（带 code / accepted / before / after）并返回 false**；
//     ⛔ 本文件不 `ctx.fail`（是否致命由调用方决定），⛔ 不静默。
//   · ⛔ 别处不许再裸写这些 id / 位（守卫 `tools/test/script-map.test.mjs` 与 `0x…` 字面量同一条规则）。
//
// 怎么查（控制层，同一条读法）：
//   `node tools/emu-ctl.mjs global-get --kind int --index 0xb1b6 --json`

/** @typedef {import("@amayui/lift-plugin").RunCtx} RunCtx */
/** @typedef {import("@amayui/lift-plugin").Receipt} Receipt */

/**
 * 「战斗中的帮助界面」那一位（int global）。
 * 旁证两处：`docs/03-emulator/plugin-runtime.md` §C1（`global.get(0xb1b6)` → `set(0xb1b6, v|0x10)`）、
 * `docs/03-emulator/save05-black-rect.md`（`global-get --index 0xb1b6` 的 "SO033A 覆盖图那位"）。
 */
export const BATTLE_HELP_GLOBAL_ID = 0xb1b6;

/** 置位 = **不再展示**"战斗中的帮助界面"（教学气泡）。 */
export const BATTLE_HELP_HIDE_BIT = 0x10;

/**
 * 读档 / 存档界面（`SAVE.BIN`）的**页号**（int global，0 基，0..89）。
 *
 * 读数（2026-11-22 / win32 / HEAD `adc4fe3` / `--present headless` + `emu-ctl`）：
 *   · 进屏时 `global-get --kind int --index 0x138e` = **1**，画面 = `LOAD 010..019`（页 1）；
 *     `emu-ctl click 1235 357`（右侧大箭头 = 下一页）后读回 **2**，画面 = `LOAD 020..029`；
 *     `click 45 357`（上一页）两次 + 一次 → 2→1→0→**89**（画面 `LOAD 890..899`）⇒ 页是**环形** 0..89。
 *   · 翻页用**左右大箭头**：左 `(45,357)` = 上一页、右 `(1235,357)` = 下一页（±1 页、环形 0..89）。
 *     ⛔ **顶部页签不是页号的写入口**：从页 1 点页签 0（x = 606 + 42k, y = 32），`0x138e` 连 60 个读点
 *     都还是 1（2026-11-22 实测，读数见 `docs/03-emulator/save-format.md` §翻页）⇒ 那排只影响"显示哪十年组"。
 *   · 进屏时脚本第 11 行 `load-int (global-int 138e)` 会把这个值**从 `SAVE.DAT` 重新读回**
 *     ⇒ 在进屏**之前**写它（例如插件在 TITLE 处 preset）会被覆盖（实测 `startPage=0` applied=true、进屏仍读回 1）。
 *   · 脚本侧同源读数：`SAVE.BIN.txt` 的 `global-int 138e` / `mul (global-int 138e) a`（行 553/766/1537/1899）；
 *     页基（脚本局部 `61b`）= `138e*10`；见 `docs/03-emulator/save-format.md` §加载界面的列表与翻页。
 *
 * ⚠ 这是**页**，不是槽：槽 = 页 * 10 + 行（行 0..9）。槽 10 在第 1 页第 0 行。
 */
export const SAVE_PAGE_GLOBAL_ID = 0x138e;

/**
 * 读一页的页号（`SAVE.BIN` 的 int global）。
 *
 * 读失败在 API 里没有错误通道 ⇒ 宿主抛 TypeError（见文件头）⇒ 本函数**不吞**。
 *
 * @param {RunCtx} ctx RunCtx（需要 `ctx.read.global.get`）。
 * @returns {number} 0..89（⛔ 不夹取：越界说明读数不是页号，调用方要响亮拒绝）。
 */
export function readSavePage(ctx) {
    const page = ctx.read.global.get("int", SAVE_PAGE_GLOBAL_ID);
    if (typeof page !== "number" || !Number.isFinite(page)) {
        throw new TypeError(`readSavePage: global ${SAVE_PAGE_GLOBAL_ID} 不是数字（${typeof page}）`);
    }
    return page | 0;
}

/**
 * 十进制 → `0x…`（**日志用**；⛔ 不是位置常量 —— 位置只在 `./script-positions.js`）。
 *
 * ★ 2026-11-25 起**导出**：`../field-point-visible.js` 曾抄了一份逐字相同的实现（两份日志格式
 *   会各自漂）。⛔ 不要把它当"十六进制解析"用（`types` 的 hex 串是 `parseTypes` 的事）。
 *
 * @param {number} value 非负 / 负数都按 32 位无符号打印（`>>> 0`）。
 * @returns {string}
 */
export function hex(value) {
    return "0x" + (value >>> 0).toString(16);
}

/**
 * 把「战斗中的帮助界面」关掉（读 → `| BATTLE_HELP_HIDE_BIT` → 写）。
 *
 * ★ 调用时机由调用方决定：**载入完成之后、在 FIELD 上做**（载入会还原脚本 / global 状态，
 *   在 TITLE 上置位会被随后的载入覆盖）。
 *
 * @param {RunCtx} ctx RunCtx（需要 `ctx.read.global.get` / `ctx.edit.global.set` / `ctx.log`）。
 * @returns {boolean} 写成功（`applied !== false`）⇒ true；否则 warn 后 false。
 */
export function hideBattleHelp(ctx) {
    const before = ctx.read.global.get("int", BATTLE_HELP_GLOBAL_ID);
    const after = (before | BATTLE_HELP_HIDE_BIT) >>> 0;
    /** @type {Receipt | null | undefined} */
    const receipt = ctx.edit.global.set("int", BATTLE_HELP_GLOBAL_ID, after);
    const applied = receipt !== null && typeof receipt === "object" ? receipt.applied !== false : false;
    if (!applied) {
        const code = receipt !== null && typeof receipt === "object" && receipt.code !== undefined
            ? String(receipt.code)
            : "unknown";
        const accepted = receipt !== null && typeof receipt === "object" ? String(receipt.accepted) : "unknown";
        ctx.log(
            "warn",
            `guest-globals: hideBattleHelp 未生效（战斗帮助可能照旧出现）— ` +
                `global=${BATTLE_HELP_GLOBAL_ID} bit=${BATTLE_HELP_HIDE_BIT} ` +
                `before=${hex(before)} after=${hex(after)} accepted=${accepted} applied=${String(applied)} code=${code}`,
        );
        return false;
    }
    ctx.log(
        "info",
        `guest-globals: hideBattleHelp ⇒ global=${BATTLE_HELP_GLOBAL_ID} ` +
            `before=${hex(before)} after=${hex(after)}（bit ${BATTLE_HELP_HIDE_BIT} 置位）`,
    );
    return true;
}

// ===========================================================================
// FIELD 地图点位精灵的**派生渲染位**（VM 自己在每次进图时重建；⛔ 不是持久值）
// ===========================================================================
//
// 取证（2026-11-25 订正 / win32 / `DRAWOBJ.BIN` 逐条指令 + `jcc` 极性；逐条指令见
// `docs/03-emulator/scripts/FIELD.md` §地块特效渲染模型）：
//   · `FIELD.BIN`  instr#187 `0x000FA0` `fill-zero (global-int 50ff9f) 32`      —— 进图先清这 32 个 dword
//   · `SETOBJ.BIN` instr#2   `0x00005C` `fill-zero (global-int 50ff9f) 32`      —— 再清一次，然后逐槽 bit-set
//                  instr#34  `0x00037C` `bit-set …  0`（= **画**）· instr#78/#84 `bit-set … 2`（= **alpha 正常**）
//                  instr#86  `0x0008C4` `bit-set …  1`（= 格标记 / 战利品抽取那一位；本文件**不动**）
//   · `DRAWOBJ.BIN` instr#7  `0x0000E0` `and (…) (50ff9f[slot]) 1` → `jcc`（**cond==0 才跳**）⇒ **bit0=0 才跳过**
//                  instr#26 `0x0002C4` `and (…) (50ff9f[slot]) 4` 为 0 → instr#29 `set-draw-color-alpha(item,0,0,ffffff)`
//                  instr#44 `0x0004E4` `add (local-int 11) fa (local-int 1)`  ⇒ **纹理槽 = 0xFA + 类型**
//                  instr#58 `0x00066C` `draw-texture (item) (0xFA+类型) 0 0 w h dstx dsty`
//   ⇒ 採集点(0x1F) 的槽 = 0x119 = `MO031B.AGF`（ALF 20797，绿草）·
//      発掘点(0x20) 的槽 = 0x11A = `MO032B.AGF`（ALF 20798，岩石）
//   ⇒ **画不画/透不透明** 只由这两位的组合决定（bit0=1 画、bit2=1 正常 alpha；⚠ 2026-11-24 的
//      "置位=不画/HIDE" 读反了 `jcc` 极性）；它们由 VM 在进图时按持久输入重算 ⇒ 改它 = 改**路由**，
//      不是改"这点位发现过没有"那个真值（真值在 `13d7` / `3e2a0` / `14d8cc` 那几个输入里，本文件不碰）。
//   · ⛔ **bit1（0x2）不要清**：A 链格表（`FIELD.BIN` 的 `0x100000` 位）与 `FIELD` `label_00030E54`
//      的按 `flags & 2` 抽取战利品都消费它。

/** 地图点位**逐槽渲染标志**（int global，32 槽 × 1 dword；VM 每次进图重建）。 */
export const FIELD_OBJECT_FLAGS_GLOBAL_ID = 0x50ff9f;

/** 地图点位**逐槽类型**（int global，32 槽；`SETOBJ` #3 把它拷进 `14d7a0`，`DRAWOBJ` #9 用它选图）。 */
export const FIELD_OBJECT_TYPE_GLOBAL_ID = 0x50ffd1;

/** `50ff9f[slot]` 位：**置位 ⇒ 该槽参与绘制**（`DRAWOBJ.BIN` #7 `and … 1` → `jcc` 的 cond 为 0 才跳 ⇒ bit0=0 才跳过）。 */
export const FIELD_OBJECT_DRAW_BIT = 0x1;

/** `50ff9f[slot]` 位：**"还没用完"**（`SETOBJ.BIN` #86 只在未耗尽支置位；`FIELD` #6132 的交互扫描只认它）。 */
export const FIELD_OBJECT_NOT_USED_BIT = 0x2;

/**
 * `50ff9f[slot]` 位：**动效 alpha 门 = "现在放动效"**（`DRAWOBJ.BIN` #26 `and … 4`；为 0 ⇒ #29 把
 * `0x3A98+slot` 的 alpha 置 0 ⇒ **动效不可见**）。
 *
 * ★ 2026-11-26 订正：它同时是**静态 / 动效的选择位** —— 静态门要求 `(flags & 6) == 2`
 *   （`DRAWOBJ.BIN` #46-55）⇒ **bit2=0 才画静态**。这不是"静态的 alpha"：静态走 `0x3ACA+slot`、
 *   全脚本从不设它的 alpha（恒不透明）。
 */
export const FIELD_OBJECT_ALPHA_BIT = 0x4;

/** 点位类型：**採集点**（`DRAWOBJ` 槽 = 0xFA+type ⇒ `0x119` = `MO031B.AGF`）。 */
export const FIELD_POINT_TYPE_GATHER = 0x1f;

/** 点位类型：**発掘点**（`DRAWOBJ` 槽 = 0xFA+type ⇒ `0x11A` = `MO032B.AGF`）。 */
export const FIELD_POINT_TYPE_DIG = 0x20;

/** 点位槽号范围（`DRAWOBJ.BIN` instr#1/#4：`local 2 = 1 … < 32`）⇒ 1..31（0 号不用）。 */
export const FIELD_OBJECT_SLOT_FIRST = 1;
export const FIELD_OBJECT_SLOT_LAST = 31;

// ---------------------------------------------------------------------------
// 点位精灵的**静态 / 动效选择**（2026-11-26 订正：选择位不是 `25c00`，是 `50ff9f` bit2）
// ---------------------------------------------------------------------------
//
// 取证（全都可复核；行号 = `dist/views/data/<NAME>.BIN.txt`，`instr#` = 同目录 FIELD 反汇编的编号）：
//   · `50ff9f[slot]` 的 **bit2 (0x4) = 动效 alpha 门**（`FIELD_OBJECT_ALPHA_BIT`）：
//     `DRAWOBJ.BIN.txt:39-43` `and a, 50ff9f[slot], 4` → `eq b, a, 0` → `jcc b`（cond==0 才跳）
//     ⇒ **bit2=0 ⇒ `set-draw-color-alpha(0x3A98+slot, 0)`（动效全透明）**；bit2=1 ⇒ 不动。
//   · 静态精灵（`0x3ACA+slot` = `0xFA+type` = `MO031B/MO032B`）的**三个门**：
//     ① `DRAWOBJ.BIN.txt:46-55`：`511080[type]==3 && eb5c1[b222][slot]==1 && (50ff9f&6)==2`
//        （`(…&6)==2` = **bit1=1 且 bit2=0**）—— 不满足 ⇒ 直接跳到循环尾，**不画静态**；
//     ② `DRAWOBJ.BIN.txt:56-58`：`check-bit 25c00[b222][row], col` → `jcc`（cond==0 才跳）
//        ⇒ **该位=0 ⇒ 跳过静态；=1 ⇒ 画静态**（`59-74` 的 `draw-texture`）；
//     ③ 同一位在 `DRAWOBJ.BIN.txt:148-156`：=1 ⇒ 动效 `alpha=5110e4[type]`；=0 ⇒ `alpha=0`。
//     ⇒ `25c00` 是**"这一格有地块要素"的共同门**（`SETLAND.BIN.txt:47-49` 置位 / `DELLAND` 清位 /
//       进图由 `510d73` 重建：`FIELD` instr#339-350），**不是**静态/动效的选择位 ——
//       把它清零会让静态与动效**同时**不画（点直接消失）。
//   · 进图默认态 = 静态：`SETOBJ.BIN.txt:111-130` 里 `511080==3` 的分支**从不**置 bit2，且只有
//     "没耗尽" 才置 bit1 ⇒ `50ff9f = 0x3`。触发点位时 `FIELD` instr#6242-6245（`0x024bfc`）才
//     `bit-set 50ff9f[slot] 2` + `eb5c1[b222][slot]=1` ⇒ **动效**。这正是"已发现进图静态、
//     走到位置触发后 7 帧"的机制（上一轮 `|=0x5` 一定看到动效，就是因为它把 bit2 也置上了）。
//   · `eb5c1[b222][slot]` = 该点位**触发过**（`FIELD` instr#6244 置 1；`9106-9128` / `9281-9303`
//     两段是逐图表的保存 / 恢复）⇒ 静态只对"已触发过"的点成立。
//
// ⛔ 本文件**不写** `25c00` / `eb5c1` / `50ffd1` / `14d73c` / `14d76e` / `14d8cc`：静态只靠
//   **清 `50ff9f` 的 bit2**。`25c00` 的位只读（读它是为了确认"引擎真的会画静态"，否则清 bit2
//   会把那个点变成**什么都不画**）。

/** 逐槽**列**（int global，`14d73c[slot]`；`DRAWOBJ` instr#21 / `FIELD` #394 等处同一条表）。 */
export const FIELD_CELL_COL_GLOBAL_ID = 0x14d73c;

/** 逐槽**行**（int global，`14d76e[slot]`；`DRAWOBJ` instr#23 / `FIELD` #396）。`25c00` 的第二维。 */
export const FIELD_CELL_ROW_GLOBAL_ID = 0x14d76e;

/** 逐图**格位图**（int global，`25c00[b222*64 + row]` 的 dword 位 `col`；`DRAWOBJ` instr#56）。只读。 */
export const FIELD_CELL_BITS_GLOBAL_ID = 0x25c00;

/** `25c00` 的**每图行宽（dword）**：`lookup-array-2d … 25c00 b222 64 row` ⇒ `b222*64 + row`。 */
export const FIELD_CELL_ROW_STRIDE = 64;

/** 格网边长（`65f2` / `6df2` 的 `fill-zero 0x20*0x20` ⇒ 32×32；`row`/`col` 只许落在这里）。 */
export const FIELD_CELL_GRID = 32;

/** 逐槽**启用位图**（int global，`14d8cc[slot]`；`SETOBJ` instr#46-49 用它决定动不动这个槽）。只读。 */
export const FIELD_SLOT_ENABLE_GLOBAL_ID = 0x14d8cc;

/** `14d8cc[slot]` 的**位号 = 当前层号**（int global `a9bb`；`SETOBJ` instr#47 的第三个操作数）。只读。 */
export const FIELD_SLOT_ENABLE_BIT_GLOBAL_ID = 0xa9bb;

/** 逐图逐槽**"触发过"**位（int global，`eb5c1[b222*32 + slot]`；`FIELD` instr#6244 置 1）。只读。 */
export const FIELD_POINT_TRIGGERED_GLOBAL_ID = 0xeb5c1;

/** `eb5c1` 的**每图行宽（dword）**：`lookup-array-2d … eb5c1 b222 32 slot` ⇒ `b222*32 + slot`。 */
export const FIELD_POINT_TRIGGERED_STRIDE = 32;

/** 逐**类型**档（int global，`511080[type]`；`OBINIT` 把 31/32 = `採集/発掘` 写成 **3**）。只读。 */
export const FIELD_POINT_CLASS_GLOBAL_ID = 0x511080;

/** `511080[type]` 的值：**动作点位档**（`DRAWOBJ` 静态门的第一个条件就是它 == 3）。 */
export const FIELD_POINT_CLASS_ACTION = 3;

/**
 * 当前**地图号**（int global）：`STINIT` / `FIELD` / `SETOBJ` 的门都是 `eq … (global-int b222) <mapNo>`，
 * 逐地图的表一律 `lookup-array-2d (base) (global-int b222) …`（例 `SETOBJ.BIN` instr#16/#68 的 `3e2a0`、
 * `DRAWOBJ.BIN` instr#18 的 `4a5f0`）。只用于读数/日志，⛔ 不写。
 */
export const FIELD_CURRENT_MAP_GLOBAL_ID = 0xb222;

/**
 * 运行期**地图实例下标**（int global）：逐地图表运行时按它索引（`62d72[61c72][…]` / `71cb4[61c72]` /
 * `62ab6[61c72]`）。只用于读数/日志，⛔ 不写。
 */
export const FIELD_MAP_INSTANCE_GLOBAL_ID = 0x61c72;

/**
 * 读一格 int global。读失败在 API 里没有错误通道 ⇒ 宿主抛 TypeError（⛔ 本文件不吞）。
 *
 * @param {RunCtx} ctx RunCtx（需要 `ctx.read.global.get`）。
 * @param {number} id raw 池下标。
 * @returns {number}
 */
export function readIntGlobal(ctx, id) {
    const value = ctx.read.global.get("int", id);
    if (typeof value !== "number" || !Number.isFinite(value)) {
        throw new TypeError(`readIntGlobal: global ${hex(id)} 不是数字（${typeof value}）`);
    }
    return value | 0;
}

/**
 * 读一格 int global，并**响亮失败**（`null` 之外的形状一律 throw）—— 给"读数即证据"的调用方用。
 *
 * @param {RunCtx} ctx RunCtx。
 * @param {number} id raw 池下标。
 * @returns {number}
 */
export function readIntGlobalStrict(ctx, id) {
    return readIntGlobal(ctx, id) >>> 0;
}

/**
 * 一次性读 `FIELD_OBJECT_TYPE_GLOBAL_ID` / `FIELD_OBJECT_FLAGS_GLOBAL_ID` 的 32 槽快照。
 *
 * @param {RunCtx} ctx RunCtx。
 * @returns {{ slot: number, type: number, flags: number }[]} 下标 0..31（`[0]` 恒不用）。
 */
export function readFieldObjectSlots(ctx) {
    /** @type {{ slot: number, type: number, flags: number }[]} */
    const out = [];
    for (let slot = 0; slot <= FIELD_OBJECT_SLOT_LAST; slot += 1) {
        out.push({
            slot,
            type: readIntGlobal(ctx, FIELD_OBJECT_TYPE_GLOBAL_ID + slot),
            flags: readIntGlobal(ctx, FIELD_OBJECT_FLAGS_GLOBAL_ID + slot),
        });
    }
    return out;
}

/**
 * 把"有点位且类型命中"的槽的**派生渲染位**改成"要画 + 正常 alpha"：置 `DRAW` 与 `ALPHA`（`|= 0x5`）。
 *
 * ★ 为什么这不算改真实值（三条，都可复核）：
 *   ① 作用面是 `50ff9f`，而 `50ff9f` 是 VM **每次进图重建**的派生表（`FIELD.BIN` #187 + `SETOBJ.BIN` #2 两处
 *      `fill-zero`，随后逐槽 `bit-set`）⇒ 它不是"这一点位发现过没有"的存储；
 *   ② 持久输入（`50ffd1` 类型表、`14d8cc`、`3e2a0`…）**一个都不写** —— 本函数只写 `50ff9f+slot`；
 *      并且**只置位、不清位**（bit1 = 格标记 / 战利品抽取要消费它，见上）；
 *   ③ 只在 `on == 1` 时执行；`on == 0` 走完全相同的链但不写（A/B 只差这一处）。
 *
 * @param {RunCtx} ctx RunCtx。
 * @param {{ types?: number[] | null }} [options] 参与强制的类型（缺省 = 採集 + 発掘）。
 * @returns {{ slot: number, type: number, before: number, after: number, applied: boolean }[]}
 *   类型命中的槽的逐槽读数（`applied=false` = 这两位本来就有 / 写回执失败）。
 */
export function forceFieldPointsVisible(ctx, options) {
    const types = options !== null && options !== undefined && Array.isArray(options.types)
        ? options.types
        : [FIELD_POINT_TYPE_GATHER, FIELD_POINT_TYPE_DIG];
    const visibleBits = FIELD_OBJECT_DRAW_BIT | FIELD_OBJECT_ALPHA_BIT;
    /** @type {{ slot: number, type: number, before: number, after: number, applied: boolean }[]} */
    const changed = [];
    for (let slot = FIELD_OBJECT_SLOT_FIRST; slot <= FIELD_OBJECT_SLOT_LAST; slot += 1) {
        const type = readIntGlobal(ctx, FIELD_OBJECT_TYPE_GLOBAL_ID + slot);
        if (type === 0 || !types.includes(type)) continue;
        const id = FIELD_OBJECT_FLAGS_GLOBAL_ID + slot;
        const before = readIntGlobal(ctx, id) >>> 0;
        // ★ 只 `|=`，⛔ 不清任何位（尤其 bit1）：清了它会改动 A 链格表与战利品抽取。
        const after = (before | visibleBits) >>> 0;
        if (after === before) {
            changed.push({ slot, type, before, after, applied: false });
            continue;
        }
        /** @type {Receipt | null | undefined} */
        const receipt = ctx.edit.global.set("int", id, after);
        const applied = receipt !== null && typeof receipt === "object" ? receipt.applied !== false : false;
        if (!applied) {
            const code = receipt !== null && typeof receipt === "object" && receipt.code !== undefined
                ? String(receipt.code)
                : "unknown";
            ctx.log("warn", `guest-globals: forceFieldPointsVisible 未生效 — slot=${slot} global=${hex(id)} ` +
                `before=${hex(before)} after=${hex(after)} code=${code}`);
        }
        changed.push({ slot, type, before, after, applied });
    }
    return changed;
}

/**
 * `25c00[b222*64 + row]` 的 **int 池下标**（`lookup-array-2d … 25c00 b222 64 row` 的同一算式）。
 *
 * @param {number} map `b222`（当前地图号，`FIELD_CURRENT_MAP_GLOBAL_ID`）。
 * @param {number} row `14d76e[slot]`（0..`FIELD_CELL_GRID`-1）。
 * @returns {number}
 */
function fieldCellIndex(map, row) {    return FIELD_CELL_BITS_GLOBAL_ID + map * FIELD_CELL_ROW_STRIDE + row;
}

/**
 * 读一个槽的**格坐标 + 当层启用位**（`14d76e` / `14d73c` / `14d8cc` + 位号 `a9bb`；⛔ 不写）。
 *
 * ★ `enabled` 的极性按 `SETOBJ.BIN.txt:46-49` 读：`check-bit 14d8cc[slot], a9bb` → `eq k, bit, 0`
 *   → `jcc k`（cond==0 才跳）⇒ **位=1 才落下去继续算这个槽**；位=0 直接 `ret`（不动它）。
 *   `a9bb` 是**层号**（`SC1820` 把它写成 0/1/2；`BTRTN` / `MVRTN` 拿它索引 3 格数组）。
 * ★ `inGrid` = `row`/`col` 落不落在 32×32 格网里（`65f2`/`6df2` 的 `fill-zero 0x20*0x20`）。
 *   越界 ⇒ `fieldCellIndex` 会算到**别的地图**的 dword 上 ⇒ 调用方必须拒绝（见 `clearFieldStaticSkip`）。
 *
 * @param {RunCtx} ctx RunCtx（需要 `ctx.read.global.get`）。
 * @param {number} slot 槽号 1..31。
 * @returns {{ row: number, col: number, enabled: boolean, inGrid: boolean, enableBit: number, enableMask: number }}
 */
export function readFieldCell(ctx, slot) {
    const row = readIntGlobal(ctx, FIELD_CELL_ROW_GLOBAL_ID + slot);
    const col = readIntGlobal(ctx, FIELD_CELL_COL_GLOBAL_ID + slot);
    const enableBit = readIntGlobal(ctx, FIELD_SLOT_ENABLE_BIT_GLOBAL_ID);
    const enableMask = readIntGlobal(ctx, FIELD_SLOT_ENABLE_GLOBAL_ID + slot) >>> 0;
    const bitInRange = enableBit >= 0 && enableBit < 32;
    const enabled = bitInRange && ((enableMask >>> enableBit) & 1) === 1;
    const inGrid = row >= 0 && row < FIELD_CELL_GRID && col >= 0 && col < FIELD_CELL_GRID;
    return { row, col, enabled, inGrid, enableBit, enableMask };
}

/**
 * 把"类型命中的槽"在**进图那一刻**改成**静态精灵**（`MO031B/MO032B`，无工具图标）。
 *
 * 判据（全按引擎自己的门，⛔ 不发明语义；`DRAWOBJ.BIN.txt` 行号）：
 *   · 静态门 ①（`46-55`）：`511080[type]==3 && eb5c1[b222][slot]==1 && (flags & 6)==2`；
 *   · 静态门 ②（`56-58`）：`25c00[b222][row]` 的第 `col` 位 == 1；
 *   · 再叠一条本插件的保守条件：`14d8cc` 在当层**启用**（否则引擎自己都不算这个槽）。
 *   ⇒ 门**成立** ⇒ 清 `50ff9f[slot]` 的 bit2 ⇒ 动效 alpha 0（`DRAWOBJ` `39-43`）、静态可见；
 *     门**不成立** ⇒ 置 bit2（= 今天的动效，⛔ 不让点消失）。
 *   ★ `50ff9f` 是 VM 每次进图重建的派生表 ⇒ 每次进图都要重新施加；⛔ 本函数不动 bit0 / bit1。
 *   ★ **只写 `50ff9f+slot` 一个 global**：`25c00` / `14d73c` / `14d76e` / `14d8cc` / `eb5c1` / `511080`
 *     全是**读**（清 `25c00` 会让静态与动效同时不画 —— 见文件头 2026-11-26 的取证段）。
 *
 * ★ 失败只 `warn`（读越界 / 写回执失败 / 格坐标出界）⇒ 返回 `applied=false`，**不 throw**：
 *   一条 warn 不能把 setmap 打死（结果面由调用方汇总 `cleared=N/M`）。
 *
 * @param {RunCtx} ctx RunCtx（需要 `ctx.read.global.get` / `ctx.edit.global.set` / `ctx.log`）。
 * @param {number} slot 槽号 1..31（调用方已按类型筛过）。
 * @returns {{ slot: number, type: number, row: number, col: number, cellBit: number, b222: number,
 *             enabled: boolean, classId: number, triggered: number, gate: boolean,
 *             before: number, after: number, applied: boolean, reason: string }}
 *   `applied=true` = 真的把 bit2 **清掉**了（`gate` 成立且写回执成功）。
 */
export function clearFieldStaticSkip(ctx, slot) {
    /** @type {{ slot: number, type: number, row: number, col: number, cellBit: number, b222: number,
     *           enabled: boolean, classId: number, triggered: number, gate: boolean,
     *           before: number, after: number, applied: boolean, reason: string }} */
    const out = {
        slot,
        type: 0,
        row: -1,
        col: -1,
        cellBit: 0,
        b222: 0,
        enabled: false,
        classId: 0,
        triggered: 0,
        gate: false,
        before: 0,
        after: 0,
        applied: false,
        reason: "ok",
    };
    const id = FIELD_OBJECT_FLAGS_GLOBAL_ID + slot;
    try {
        const map = readIntGlobal(ctx, FIELD_CURRENT_MAP_GLOBAL_ID);
        out.b222 = map;
        const cell = readFieldCell(ctx, slot);
        out.row = cell.row;
        out.col = cell.col;
        out.enabled = cell.enabled;
        const type = readIntGlobal(ctx, FIELD_OBJECT_TYPE_GLOBAL_ID + slot);
        out.type = type;
        const flags = readIntGlobal(ctx, id) >>> 0;
        out.before = flags;
        out.after = flags;
        if (!cell.inGrid) {
            // 越界的 row/col 会把算式算到**别的地图**的 dword 上 ⇒ 只 warn，一个字节都不写。
            out.reason = "off-grid";
            ctx.log("warn", `guest-globals: clearFieldStaticSkip slot=${slot} 格坐标出界 ` +
                `row=${cell.row} col=${cell.col}（格网 ${FIELD_CELL_GRID}）⇒ 保留动效`);
            return out;
        }
        const word = readIntGlobal(ctx, fieldCellIndex(map, cell.row)) >>> 0;
        out.cellBit = (word >>> cell.col) & 1;
        const classId = readIntGlobal(ctx, FIELD_POINT_CLASS_GLOBAL_ID + type);
        out.classId = classId;
        const triggered = readIntGlobal(
            ctx,
            FIELD_POINT_TRIGGERED_GLOBAL_ID + map * FIELD_POINT_TRIGGERED_STRIDE + slot,
        );
        out.triggered = triggered;
        // 静态门 = `DRAWOBJ` #46-55 + #56-58 + 当层启用（见上）。
        const gate = cell.enabled
            && classId === FIELD_POINT_CLASS_ACTION
            && triggered === 1
            && (flags & FIELD_OBJECT_NOT_USED_BIT) !== 0
            && out.cellBit === 1;
        out.gate = gate;
        const after = gate
            ? (flags & ~FIELD_OBJECT_ALPHA_BIT) >>> 0
            : (flags | FIELD_OBJECT_ALPHA_BIT) >>> 0;
        out.after = after;
        if (after === flags) {
            out.reason = gate ? "already-static" : "already-dynamic";
            return out;
        }
        /** @type {Receipt | null | undefined} */
        const receipt = ctx.edit.global.set("int", id, after);
        const applied = receipt !== null && typeof receipt === "object" ? receipt.applied !== false : false;
        out.applied = applied && gate;
        if (!applied) {
            const code = receipt !== null && typeof receipt === "object" && receipt.code !== undefined
                ? String(receipt.code)
                : "unknown";
            out.reason = "write-failed";
            ctx.log("warn", `guest-globals: clearFieldStaticSkip 未生效 — slot=${slot} global=${hex(id)} ` +
                `before=${hex(flags)} after=${hex(after)} code=${code}`);
        }
        return out;
    } catch (e) {
        const anyE = /** @type {any} */ (e);
        const why = anyE !== null && anyE !== undefined && typeof anyE.message === "string"
            ? anyE.message
            : String(e);
        out.reason = "read-failed";
        ctx.log("warn", `guest-globals: clearFieldStaticSkip slot=${slot} 读失败（${why}）⇒ 保留原样`);
        return out;
    }
}
