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
