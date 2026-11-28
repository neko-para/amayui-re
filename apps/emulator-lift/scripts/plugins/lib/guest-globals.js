// scripts/plugins/lib/guest-globals.js —— **guest global** 的常量 + 操作（数字只在这里写一次）
//
// 口径（v2，2026-11-28）：
//   · `kind` 只有 `"int"` / `"uint"` / `"float"` 走 `ctx.read.global.get` / `tick.edit.global.set`
//     （`"string"` 型的 set 本实现未实现 ⇒ 调用即 TypeError；见 `scripts/plugins/plugin.d.ts`）。
//   · **读 = `ctx.read`**（RunCtx 上的只读口；`readWired()` 不绑 `in_callback_` ⇒ tick handler /
//     task continuation 里用存下来的 RunCtx 读**合法**）。⛔ 不要用 `Tick.local` 替代：那读的是
//     当前帧的 local，与全局池 id 不是同一个地址空间。
//   · **写 = `tick.edit`**（P0 起 `ctx.edit` / `ctx.input` 在 tick / continuation 里仍是 TypeError
//     ⇒ 写函数**收 Tick**，调用方传刚 `await` 到的那一拍）。
//   · 读**失败**在 API 里没有错误通道 ⇒ 宿主抛 TypeError（`script_quickjs.cpp` 的
//     `jsCtxReadGlobalGet`：⛔ 不返回 0 冒充读数）。所以本文件**不**吞读异常 —— 让它穿到宿主，
//     归因里带 `file:line`（吞掉就变成"静默地什么都没改"）。
//   · 写**失败**有回执：`{ accepted, queued, applied, seq, id, code? }`（Edit 立即 ⇒ `accepted == applied`）。
//     `applied === false` ⇒ **warn（带 code / accepted / before / after）并返回 false**；
//     ⛔ 本文件不 `ctx.fail`（是否致命由调用方决定），⛔ 不静默。日志走**全局 `log`**（v2；`ctx.log` 已弃用）。
//   · ⛔ 别处不许再裸写这些 id / 位（守卫 `tools/test/script-map.test.mjs` 与 `0x…` 字面量同一条规则）。
//
// 怎么查（控制层，同一条读法）：
//   `node tools/emu-ctl.mjs global-get --kind int --index 0xb1b6 --json`

/** @typedef {import("@amayui/lift-plugin").RunCtx} RunCtx */
/** @typedef {import("@amayui/lift-plugin").Tick} Tick */
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
 * ★ v2：读走 `ctx.read`，写走**这一拍的 Tick**（`tick.edit`）—— `ctx.edit` 在 task continuation 里
 *   是 TypeError（`inCallback` 门），只有 Tick 的写面被放开。
 *
 * @param {RunCtx} ctx RunCtx（用 `ctx.read.global.get`）。
 * @param {Tick} tick 刚 `await ctx.once(...)` 到的 Tick（用 `tick.edit.global.set`）。
 * @returns {boolean} 写成功（`applied !== false`）⇒ true；否则 warn 后 false。
 */
export function hideBattleHelp(ctx, tick) {
    const before = ctx.read.global.get("int", BATTLE_HELP_GLOBAL_ID);
    const after = (before | BATTLE_HELP_HIDE_BIT) >>> 0;
    /** @type {Receipt | null | undefined} */
    const receipt = tick.edit.global.set("int", BATTLE_HELP_GLOBAL_ID, after);
    const applied = receipt !== null && typeof receipt === "object" ? receipt.applied !== false : false;
    if (!applied) {
        const code = receipt !== null && typeof receipt === "object" && receipt.code !== undefined
            ? String(receipt.code)
            : "unknown";
        const accepted = receipt !== null && typeof receipt === "object" ? String(receipt.accepted) : "unknown";
        log.warn(
            `guest-globals: hideBattleHelp 未生效（战斗帮助可能照旧出现）— ` +
                `global=${BATTLE_HELP_GLOBAL_ID} bit=${BATTLE_HELP_HIDE_BIT} ` +
                `before=${hex(before)} after=${hex(after)} accepted=${accepted} applied=${String(applied)} code=${code}`,
        );
        return false;
    }
    log.info(
        `guest-globals: hideBattleHelp ⇒ global=${BATTLE_HELP_GLOBAL_ID} ` +
            `before=${hex(before)} after=${hex(after)}（bit ${BATTLE_HELP_HIDE_BIT} 置位）`,
    );
    return true;
}

// ===========================================================================
// FIELD 地图点位精灵的**派生渲染位**（VM 自己每渲染帧重建；⛔ 不是持久值）
// ===========================================================================
//
// ★★ 2026-11-26 最终模型（真机探针 + `DRAWOBJ.SETOBJ` 逐条指令；详见
//   `docs/03-emulator/scripts/FIELD.md` §地块特效渲染模型）：
//   · 引擎**每渲染帧**自己重建这张表：`SETOBJ.BIN` #2 `0x00005C` `fill-zero (global-int 50ff9f) 32`
//     之后逐槽按"发现 / 触发状态"重算（#34 `bit-set … 0` = 画、#84 `bit-set … 2` = 动效 alpha 正常、
//     #86 `bit-set … 1` = 还没用完；全部 `0x00037C` 起）。
//     ⇒ **bit1（静态门的一半）/ bit2（动效 alpha = 静态↔动效的选择位）由引擎决定，插件⛔ 不写。**
//   · 真机读数（2026-11-26 / win32 / **存档 012** / FIELD 就绪、**没有** field 插件参与，逐槽
//     `node tools/emu-ctl.mjs global-get --kind int --index 0x50ff9f+slot`）：
//       slot3 `type=0x1f flags=0x5`（= 已触发 / 已采集 ⇒ 动效）·
//       slot4 `type=0x1f flags=0x3` · slot5 `type=0x20 flags=0x3` · slot6 `type=0x20 flags=0x3`
//       （= 没采集过 ⇒ bit1=1 / bit2=0 ⇒ 静态）⇒ **引擎自己就是对的**。
//   · ⇒ 用户早先看到的"没采集过也在动"是**旧插件的错**：它写 `|= 0x5`，把 bit2 强加上去。
//   · `DRAWOBJ.BIN`：instr#7 `0x0000E0` `and (…) (50ff9f[slot]) 1` → `jcc`（**cond==0 才跳**）
//     ⇒ **bit0=0 才整段跳过**；instr#26 `and … 4` 为 0 → #29 `set-draw-color-alpha(0x3A98+slot, 0)`
//     ⇒ **bit2=0 ⇒ 动效不可见（静态才画）**。
//   · 纹理：instr#44 `add (local-int 11) fa (local-int 1)` ⇒ 槽 = `0xFA+类型` ⇒ 採集(0x1F) =
//     `0x119` = `MO031B.AGF`（ALF 20797，绿草）· 発掘(0x20) = `0x11A` = `MO032B.AGF`（20798，岩石）。
//
// ⇒ 插件**唯一的写 = `flags | 0x1`**（补"画"位：让"未发现但本应存在"的点也画出来）。
//   ⛔ 不碰 bit1：A 链格表（`FIELD.BIN` 的 `0x100000` 位）与 `FIELD` `label_00030E54` 的战利品抽取
//     都消费 `flags & 2`。
//   ⛔ 不碰 bit2：那是引擎的静态 / 动效选择位（写完这一位就等于替引擎决定"这个点动没动过"）。
//   ⇒ 已触发 = 动效、未触发 = 静态 —— 与引擎自己的读数逐位一致（`0x5` 保持 `0x5`、`0x3` 保持 `0x3`）。

/** 地图点位**逐槽渲染标志**（int global，32 槽 × 1 dword；VM 每渲染帧重建）。 */
export const FIELD_OBJECT_FLAGS_GLOBAL_ID = 0x50ff9f;

/** 地图点位**逐槽类型**（int global，32 槽；`SETOBJ` #3 把它拷进 `14d7a0`，`DRAWOBJ` #9 用它选图）。 */
export const FIELD_OBJECT_TYPE_GLOBAL_ID = 0x50ffd1;

/** `50ff9f[slot]` 位：**置位 ⇒ 该槽参与绘制**（`DRAWOBJ.BIN` #7 `and … 1` → `jcc` 的 cond 为 0 才跳 ⇒ bit0=0 才跳过）。 */
export const FIELD_OBJECT_DRAW_BIT = 0x1;

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
 * 逐槽**行号 / 列号**（int global；`DRAWOBJ` #21/#23 把它们读进 `local 4/5`，
 * 再拿 `25c00[图像素行][行]` 的第 `列` 位做"这一格有地块要素"的门）。
 */
export const FIELD_POINT_ROW_GLOBAL_ID = 0x14d76e;
export const FIELD_POINT_COL_GLOBAL_ID = 0x14d73c;

/**
 * 每图一块的**格位图**（int global；行宽字面量 = 视图里的 `64` = **0x64 = 100** dword）。
 * ★ 视图里的立即数一律是**十六进制** —— 按十进制读会整块错位（2026-11-26 的教训）。
 */
export const FIELD_CELL_BITS_GLOBAL_ID = 0x25c00;

/** `510439[type]` = 该类型**动效**纹理槽（`0x68`/`0x69`；静态是 `0xFA+type`）。 */
export const FIELD_DYN_TEXTURE_SLOT_GLOBAL_ID = 0x510439;

/** `511080[type]` = 该类型的**类别**（静态门第一腿要 `== 3`）。 */
export const FIELD_TYPE_CLASS_GLOBAL_ID = 0x511080;

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
 * 把"有点位且类型命中"的槽的**派生渲染位**补上"要画"这一位：`flags |= 0x1`。
 *
 * ★ 为什么只写 bit0（2026-11-26 最终模型，取证见文件头）：
 *   · bit1 / bit2 **由引擎每渲染帧重算**（`SETOBJ` #2 `fill-zero` 之后逐槽 `bit-set`）⇒ 插件写它们
 *     只是在替引擎决定"这个点触发过没有"，而那是持久输入的事。`|= 0x5` 的旧写法必然让
 *     **未采集的点也放动效**（用户看到的正是这个）。
 *   · bit0 是"画不画"的**总闸**（`DRAWOBJ` #7：bit0=0 整段跳过）⇒ 补它才能让"未发现但本应存在"的
 *     点位画出来（引擎在未发现时可能不置 bit0）。
 *   · **只置位、不清位**（`after === before` 时连写回执都不发）；⛔ 不碰 bit1（`flags & 2` 被 A 链格表
 *     与 `FIELD` 的战利品抽取消费）、⛔ 不碰 bit2（静态 / 动效选择位）。
 *   · 作用面 = `50ff9f`（派生表）⇒ 不是改"这一点位发现过没有"的真值；持久输入一个都不写。
 *   · 只在 `on == 1` 时由调用方调用；`on == 0` ⇒ 连这个函数都不进（0 读 / 0 写）。
 * ★ v2：读走 `ctx.read`、写走**这一拍的 Tick**（`tick.edit.global.set`）。
 *
 * @param {RunCtx} ctx RunCtx（用 `ctx.read.global.get`）。
 * @param {Tick} tick 本拍的 Tick（用 `tick.edit.global.set`）。
 * @param {{ types?: number[] | null }} [options] 参与强制的类型（缺省 = 採集 + 発掘）。
 * @returns {{ slot: number, type: number, before: number, after: number, applied: boolean }[]}
 *   类型命中的槽的逐槽读数（`applied=false` = bit0 本来就有 / 写回执失败）。
 */
export function forceFieldPointsVisible(ctx, tick, options) {
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
        // ★ 只补 bit0；⛔ bit1 / bit2 是引擎的（见文件头）。
        const after = (before | FIELD_OBJECT_DRAW_BIT) >>> 0;
        if (after === before) {
            changed.push({ slot, type, before, after, applied: false });
            continue;
        }
        /** @type {Receipt | null | undefined} */
        const receipt = tick.edit.global.set("int", id, after);
        const applied = receipt !== null && typeof receipt === "object" ? receipt.applied !== false : false;
        if (!applied) {
            const code = receipt !== null && typeof receipt === "object" && receipt.code !== undefined
                ? String(receipt.code)
                : "unknown";
            log.warn(`guest-globals: forceFieldPointsVisible 未生效 — slot=${slot} global=${hex(id)} ` +
                `before=${hex(before)} after=${hex(after)} code=${code}`);
        }
        changed.push({ slot, type, before, after, applied });
    }
    return changed;
}
