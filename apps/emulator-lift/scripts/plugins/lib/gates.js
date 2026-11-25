// scripts/plugins/lib/gates.js —— **门的薄封装**（只包 `ctx.trigger.*` + 两条失败收口）
//
// 为什么有一层（2026-11-25）：`afterPosition` / `afterScript` / `atReadPoint` 的**调用形状**在十几处
//   逐字相同（`{ name, timeoutMs }`），"门超时 ⇒ finish(5)"与"未知状态 ⇒ ctx.fail"各 6 处。
//   复制的是**形状**，不是数值 —— 区间 / 坐标 / 超时按文件、按场景各不同（属读数）⇒ 本文件只收形状。
//
// ⛔ 三条边界（别把工具做成第二份真源）：
//   · **不 import `./script-positions.js`**：区间数字由调用方给（那张表是位置读数的唯一真源，
//     本文件一格都不写）；trigger 名字（`boot:` / `load:` / `scenario:` …）同样由调用方给 ——
//     名字是链的**进度真源**（`ev.name`），也是 `route` 分流判据。
//   · **不 import `./chains.js`**：`route` 是**零 ctx 的纯路由**（`chains.js` 头）；把 `ctx.trigger`
//     混进路由会把"路由"与"运行期"绑死。
//   · **不改判据**：`failOnTimeout` 只做 `finish(5)`（⛔ 不 `quit` —— 那会把归因改成 `plugin: quit`）。
//     ★ 两个 `failOn*` 都返回 `true`（= 已经收口），但**调用点写成显式两行**：
//       `failOnTimeout(ctx, ev, label); return [];` —— `if (failOnTimeout(…)) return [];` 在
//       `checkJs` 下**不成立**（TS 不会把 boolean 返回值折成恒真 ⇒ 函数尾被算作 `undefined`，
//       `Chain.onTrigger` 的 `Trigger | Trigger[] | null` 立刻报 TS2322）。返回值留给"内联表达式"
//       场景（如 `cond ? failOnUnknown(...) : next()`），本仓调用点不用。
//
// ★ 文案（前缀）逐字保留原实现：`<label>: <phrase> (<ev.name>)` / `<label>: unknown state: <ev.name>`。
//   `label === ""` ⇒ 不带前缀（存档链的历史文案就是 `unknown state: …`）。⛔ 不要在这里"统一文案"：
//   `plugin_e2e.cpp` 的负断言与排障口径都按前缀分链（`load-save: gate timeout` …）。

/** @typedef {import("@amayui/lift-plugin").RunCtx} RunCtx */
/** @typedef {import("@amayui/lift-plugin").TriggerOptions} TriggerOptions */
/** @typedef {import("@amayui/lift-plugin").TriggerMode} TriggerMode */
/** @typedef {import("@amayui/lift-plugin").AfterScriptTrigger} AfterScriptTrigger */
/** @typedef {import("@amayui/lift-plugin").AfterPositionTrigger} AfterPositionTrigger */
/** @typedef {import("@amayui/lift-plugin").AtReadPointTrigger} AtReadPointTrigger */
/** @typedef {import("@amayui/lift-plugin").FiredEvent} FiredEvent */
/** @typedef {import("@amayui/lift-plugin").TimeoutEvent} TimeoutEvent */
/** @typedef {FiredEvent | TimeoutEvent} PluginEvent */
/** @typedef {{ mode?: TriggerMode, maxFires?: number }} GateOpts */

/**
 * 造 `TriggerOptions`：`name` / `timeoutMs` 永远由参数决定，`opts` 只追加可选字段。
 *
 * @param {string} name 状态名。
 * @param {number} timeoutMs 门超时（毫秒）。
 * @param {GateOpts | undefined} opts 追加字段（缺省 = 不追加）。
 * @returns {TriggerOptions}
 */
function triggerOptions(name, timeoutMs, opts) {
    if (opts === undefined) return { name, timeoutMs };
    return { name, timeoutMs, mode: opts.mode, maxFires: opts.maxFires };
}

/**
 * 等"某个脚本的 `[lo,hi)` 位置区间"（`ctx.trigger.afterPosition` 的薄封装）。
 *
 * @param {RunCtx} ctx RunCtx。
 * @param {string} script **marker 文件名**（例 `FIELD.BIN`；宿主是精确相等，⛔ 不剥 `.BIN`）。
 * @param {number} lo 区间下界（含）。
 * @param {number} hi 区间上界（不含）。
 * @param {string} name 状态名（同一 armed 集合内唯一）。
 * @param {number} timeoutMs 门超时（毫秒）。
 * @param {GateOpts} [opts] 追加的可选字段（`mode` / `maxFires`）；⛔ 不能用它覆盖 name / timeoutMs。
 * @returns {AfterPositionTrigger}
 */
export function awaitPosition(ctx, script, lo, hi, name, timeoutMs, opts) {
    return ctx.trigger.afterPosition(script, lo, hi, triggerOptions(name, timeoutMs, opts));
}

/**
 * 等"脚本身份"（`ctx.trigger.afterScript` 的薄封装；电平门、无区间）。
 *
 * @param {RunCtx} ctx RunCtx。
 * @param {string} script **marker 文件名**（例 `SETTOOLLIST.BIN`）。
 * @param {string} name 状态名。
 * @param {number} timeoutMs 门超时（毫秒）。
 * @param {GateOpts} [opts] 追加的可选字段（`mode` / `maxFires`）。
 * @returns {AfterScriptTrigger}
 */
export function awaitScript(ctx, script, name, timeoutMs, opts) {
    return ctx.trigger.afterScript(script, triggerOptions(name, timeoutMs, opts));
}

/**
 * 等"下一个读点"（`ctx.trigger.atReadPoint("any", 1, …)` 的薄封装）。
 *
 * ★ 两个参数是**形状**的一部分（`"any"` / `1`）：全仓每一处 read point 门都是这个形状
 *   （`save.js` 的页链 / `scenario-save10-field.js` 的 hover 门）⇒ 收进这里，调用方只给名字与超时。
 *
 * @param {RunCtx} ctx RunCtx。
 * @param {string} name 状态名。
 * @param {number} timeoutMs 门超时（毫秒）。
 * @param {GateOpts} [opts] 追加的可选字段（`mode` / `maxFires`）。
 * @returns {AtReadPointTrigger}
 */
export function awaitReadPoint(ctx, name, timeoutMs, opts) {
    return ctx.trigger.atReadPoint("any", 1, triggerOptions(name, timeoutMs, opts));
}

/**
 * 门超时 = **响亮收口**：`ctx.finish(5, "<label>: <phrase> (<ev.name>)")`。
 *
 * ★ 为什么是 `finish(5)`：那是历史 exit 5 的映射，也是 `plugin_e2e` / 排障沿用的归因；
 *   ⛔ 不要用 `ctx.quit` —— 归因会变成 `plugin: quit`，看起来像"用户关窗口"。
 *
 * @param {RunCtx} ctx RunCtx。
 * @param {PluginEvent} ev 本次事件（只取 `ev.name`）。
 * @param {string} label 前缀（`boot` / `load-save` / `scenario` / `scenario-fellow` / …）。
 * @param {string} [phrase] 超时短语；缺省 `gate timeout`（`scenario-save10` 的历史文案是 `timeout`）。
 * @returns {boolean} 恒 true（= 已经 finish(5)，调用方 `return []`）。
 */
export function failOnTimeout(ctx, ev, label, phrase = "gate timeout") {
    ctx.finish(5, `${label}: ${phrase} (${ev.name})`);
    return true;
}

/**
 * 未知状态 = `ctx.fail`（+ 调用方 `return []`）；文案 = `<label>: unknown state: <ev.name>`。
 *
 * @param {RunCtx} ctx RunCtx。
 * @param {PluginEvent} ev 本次事件（只取 `ev.name`）。
 * @param {string} [label] 前缀；**省略 / 空串** = 不带前缀（存档链的历史文案 `unknown state: …`）。
 * @returns {boolean} 恒 true（= 已经 ctx.fail，调用方 `return []`）。
 */
export function failOnUnknown(ctx, ev, label = "") {
    ctx.fail(ev.name, `${label === "" ? "" : label + ": "}unknown state: ${ev.name}`);
    return true;
}
