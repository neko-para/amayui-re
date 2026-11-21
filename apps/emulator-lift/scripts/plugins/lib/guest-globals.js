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

/**
 * 「战斗中的帮助界面」那一位（int global）。
 * 旁证两处：`docs/03-emulator/plugin-runtime.md` §C1（`global.get(0xb1b6)` → `set(0xb1b6, v|0x10)`）、
 * `docs/03-emulator/save05-black-rect.md`（`global-get --index 0xb1b6` 的 "SO033A 覆盖图那位"）。
 */
export const BATTLE_HELP_GLOBAL_ID = 0xb1b6;

/** 置位 = **不再展示**"战斗中的帮助界面"（教学气泡）。 */
export const BATTLE_HELP_HIDE_BIT = 0x10;

/** 十进制 → `0x…`（日志用；只在本文件里用，⛔ 不是位置常量）。 */
function hex(value) {
    return "0x" + (value >>> 0).toString(16);
}

/**
 * 把「战斗中的帮助界面」关掉（读 → `| BATTLE_HELP_HIDE_BIT` → 写）。
 *
 * ★ 调用时机由调用方决定：**载入完成之后、在 FIELD 上做**（载入会还原脚本 / global 状态，
 *   在 TITLE 上置位会被随后的载入覆盖）。
 *
 * @param {object} ctx RunCtx（需要 `ctx.read.global.get` / `ctx.edit.global.set` / `ctx.log`）。
 * @returns {boolean} 写成功（`applied !== false`）⇒ true；否则 warn 后 false。
 */
export function hideBattleHelp(ctx) {
    const before = ctx.read.global.get("int", BATTLE_HELP_GLOBAL_ID);
    const after = (before | BATTLE_HELP_HIDE_BIT) >>> 0;
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
