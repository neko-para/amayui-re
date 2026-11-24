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

/** 十进制 → `0x…`（日志用；只在本文件里用，⛔ 不是位置常量）。 */
/**
 * @param {number} value
 * @returns {string}
 */
function hex(value) {
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
// 取证（2026-11-24 / win32 / 只读脚本清单 `dist/views/data/*.BIN.txt`；逐条指令见
// `docs/03-emulator/scripts/FIELD.md` §地块特效渲染模型）：
//   · `FIELD.BIN`  instr#187 `0x000FA0` `fill-zero (global-int 50ff9f) 32`      —— 进图先清这 32 个 dword
//   · `SETOBJ.BIN` instr#2   `0x00005C` `fill-zero (global-int 50ff9f) 32`      —— 再清一次，然后逐槽 bit-set
//                  instr#34  `0x00037C` `bit-set …  0`（= 不画）· instr#78/#84 `bit-set … 2`（= alpha 0）
//                  instr#86  `0x0008C4` `bit-set …  1`
//   · `DRAWOBJ.BIN` instr#7  `0x0000E0` `and (…) (50ff9f[slot]) 1` → `jcc` 跳过该槽（**不画**）
//                  instr#26 `0x0002C4` `and (…) (50ff9f[slot]) 4` → instr#29 `set-draw-color-alpha(item,0,0,ffffff)`
//                  instr#44 `0x0004E4` `add (local-int 11) fa (local-int 1)`  ⇒ **纹理槽 = 0xFA + 类型**
//                  instr#58 `0x00066C` `draw-texture (item) (0xFA+类型) 0 0 w h dstx dsty`
//   ⇒ 採集点(0x1F) 的槽 = 0x119 = `MO031B.AGF`（ALF 20797，绿草）·
//      発掘点(0x20) 的槽 = 0x11A = `MO032B.AGF`（ALF 20798，岩石）
//   ⇒ **画不画/透不透明** 只由这两位的组合决定；它们由 VM 在进图时按持久输入重算 ⇒ 改它 = 改**路由**，
//      不是改"这点位发现过没有"那个真值（真值在 `13d7` / `3e2a0` / `14d8cc` 那几个输入里，本文件不碰）。

/** 地图点位**逐槽渲染标志**（int global，32 槽 × 1 dword；VM 每次进图重建）。 */
export const FIELD_OBJECT_FLAGS_GLOBAL_ID = 0x50ff9f;

/** 地图点位**逐槽类型**（int global，32 槽；`SETOBJ` #3 把它拷进 `14d7a0`，`DRAWOBJ` #9 用它选图）。 */
export const FIELD_OBJECT_TYPE_GLOBAL_ID = 0x50ffd1;

/** `50ff9f[slot]` 位：**置位 ⇒ 该点不画**（`DRAWOBJ.BIN` instr#7 `and … 1` → `jcc` 跳过）。 */
export const FIELD_OBJECT_HIDE_BIT = 0x1;

/** `50ff9f[slot]` 位：**置位 ⇒ 照画但 alpha=0**（`DRAWOBJ.BIN` instr#26 `and … 4` → instr#29 alpha 0）。 */
export const FIELD_OBJECT_ALPHA0_BIT = 0x4;

/** 点位类型：**採集点**（`DRAWOBJ` 槽 = 0xFA+type ⇒ `0x119` = `MO031B.AGF`）。 */
export const FIELD_POINT_TYPE_GATHER = 0x1f;

/** 点位类型：**発掘点**（`DRAWOBJ` 槽 = 0xFA+type ⇒ `0x11A` = `MO032B.AGF`）。 */
export const FIELD_POINT_TYPE_DIG = 0x20;

/** 点位槽号范围（`DRAWOBJ.BIN` instr#1/#4：`local 2 = 1 … < 32`）⇒ 1..31（0 号不用）。 */
export const FIELD_OBJECT_SLOT_FIRST = 1;
export const FIELD_OBJECT_SLOT_LAST = 31;

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
 * 把"有点位且类型命中"的槽的**派生渲染位**改成"可见"：清 `HIDE` 与 `ALPHA0`。
 *
 * ★ 为什么这不算改真实值（三条，都可复核）：
 *   ① 作用面是 `50ff9f`，而 `50ff9f` 是 VM **每次进图重建**的派生表（`FIELD.BIN` #187 + `SETOBJ.BIN` #2 两处
 *      `fill-zero`，随后逐槽 `bit-set`）⇒ 它不是"这一点位发现过没有"的存储；
 *   ② 持久输入（`50ffd1` 类型表、`14d8cc`、`3e2a0`…）**一个都不写** —— 本函数只写 `50ff9f+slot`；
 *   ③ 只在 `on == 1` 时执行；`on == 0` 走完全相同的链但不写（A/B 只差这一处）。
 *
 * @param {RunCtx} ctx RunCtx。
 * @param {{ types?: number[] | null }} [options] 参与强制的类型（缺省 = 採集 + 発掘）。
 * @returns {{ slot: number, type: number, before: number, after: number, applied: boolean }[]} 实际改过的槽。
 */
export function forceFieldPointsVisible(ctx, options) {
    const types = options !== null && options !== undefined && Array.isArray(options.types)
        ? options.types
        : [FIELD_POINT_TYPE_GATHER, FIELD_POINT_TYPE_DIG];
    /** @type {{ slot: number, type: number, before: number, after: number, applied: boolean }[]} */
    const changed = [];
    for (let slot = FIELD_OBJECT_SLOT_FIRST; slot <= FIELD_OBJECT_SLOT_LAST; slot += 1) {
        const type = readIntGlobal(ctx, FIELD_OBJECT_TYPE_GLOBAL_ID + slot);
        if (type === 0 || !types.includes(type)) continue;
        const id = FIELD_OBJECT_FLAGS_GLOBAL_ID + slot;
        const before = readIntGlobal(ctx, id) >>> 0;
        const after = (before & ~(FIELD_OBJECT_HIDE_BIT | FIELD_OBJECT_ALPHA0_BIT)) >>> 0;
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
