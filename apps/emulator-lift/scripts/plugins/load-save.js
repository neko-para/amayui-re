// scripts/plugins/load-save.js —— **根入口**：`loadSave` 的参考插件（S1 从 `lib/save.js` 拆出来）
//
// 为什么在根（用户口径 2026-11-25）：
//   · `--plugin` 的 id 只查 `scripts/plugins/` **根**（非递归），`lib/` **永远不是 id 目标**；
//   · `lib/` 只放被 import 的库 ⇒ "需要单独加载的入口"必须住在根上，库的"参考入口"也拆到根。
//   ⇒ `lib/save.js` 只剩 `loadSave` / `SAVE_DIR_REL` 等（没有 default 导出）；本文件是唯一的载入入口。
//   id = `load-save`（文件名 stem）。路径写法仍可用（`scripts/plugins/load-save.js`）。
//
// ★ import 口径：入口根 = 本文件所在目录（`scripts/plugins/`）⇒ 库一律 `./lib/...`（**向下** import
//   允许；⛔ `../` 越出 plugin 根会被 QJS 拒）。
//
// 参考入口（**只是参考**；库消费者用 `./lib/save.js` 的 `loadSave`）
//
// 参数来源（2026-11-25 起）：**config > --plugin-param > 默认**
//   · config = `<--appdata>/plugin-config.json` 的域 **`save`**（ctx.config；没 appdata ⇒ 完全回退）；
//     key：`slot`（number）、`expect`（string[] 或逗号串）、可选 `saveDir`（string，相对 appdata 根）、
//     `timeoutMs`（number）、`precheck`（boolean）。
//   · params = `--plugin-param k=v`（夹具 / 诊断口）；五个 key（`slot` / `expect` / `saveDir` /
//     `timeoutMs` / `precheck`）都支持 params 回退（后三个是 2026-11-25 加的，值 = 字符串）。
//   · 默认：`expect` 缺 ⇒ `FIELD,SC0000` 并打一行 info；`slot` 没有默认值（缺 ⇒ `loadSave` 响亮 throw）。
// ★ "给了"的判据是 `has()`（⛔ 不是真值）：`slot: 0` 这种 falsy 值也算给了。
// ★ 2026-11-25：**取配置的六件事全部搬进 `./lib/settings.js`**（`openConfigDomain` / `slotSetting` /
//   `scriptListSetting` / `textSetting` / `intSetting` / `boolSetting`）；本文件只保留"哪几个 key + 默认值"。
//   行为逐字不变（`slot` 的整数校验 / `expect` 归一 / `precheck` 并集语义 / `timeoutMs` 的正整数）。
//
// `slot`：非整数 / 越界 ⇒ `loadSave` 构造时 throw ⇒ 宿主记 `run=failed`（进程 exit 不受影响）。
// `expect`：裸名自动补 `.BIN`，带 `.BIN` 也接受。
//
// ★ 参考入口**自带 TITLE 门**（`./lib/boot-to-title.js`）：它可以在**任意启动点**当插件直接跑
//   （`plugin/load-save-e2e` 就是这么用的），所以形状 = `boot:` 前缀归 boot、其余归 load。
//   ⚠ 这与库接口不同：`loadSave(...)` 单独用时**调用方**保证已在 TITLE（`./lib/save.js` 文件头）。
// ---------------------------------------------------------------------------
import { bootToTitle } from "./lib/boot-to-title.js";
import { route } from "./lib/chains.js";
import { loadSave } from "./lib/save.js";
import {
    boolSetting,
    intSetting,
    openConfigDomain,
    scriptListSetting,
    slotSetting,
    textSetting,
} from "./lib/settings.js";

/** @typedef {import("@amayui/lift-plugin").RunCtx} RunCtx */
/** @typedef {{ start: (ctx: RunCtx) => any, onTrigger: (ctx: RunCtx, ev: any) => any }} Chain */
/** @typedef {{ expect?: string[], saveDir?: string, timeoutMs?: number, precheck?: boolean }} LoadSaveOptions */

/** 配置文件里的域（`<--appdata>/plugin-config.json`；只读）。 */
const CONFIG_DOMAIN = "save";
/** `expect` 的默认落点（只在该入口既没有 config 也没有 params 时用；打一行 info 说明）。 */
const DEFAULT_EXPECT = ["FIELD.BIN", "SC0000.BIN"];

/**
 * @typedef {import("@amayui/lift-plugin").ConfigDomain} ConfigDomain
 */

/**
 * 本入口的四个可选值：**唯一**的取配置入口 = `./lib/settings.js`（2026-11-25 抽取）。
 * ★ 这里的顺序就是"config > `--plugin-param` > 默认"；`slot` 的"没给"仍交给 `loadSave`（NaN / 越界）。
 *
 * @param {RunCtx} ctx RunCtx。
 * @param {ConfigDomain | null} domain 域对象（不可用 = null）。
 * @returns {LoadSaveOptions}
 */
function optionsFor(ctx, domain) {
    const expect = scriptListSetting(ctx, domain, "expect", CONFIG_DOMAIN);
    /** @type {LoadSaveOptions} */
    const options = { expect: expect === null ? DEFAULT_EXPECT : expect };
    const saveDir = textSetting(ctx, domain, "saveDir", undefined, { domainId: CONFIG_DOMAIN });
    if (saveDir !== undefined) options.saveDir = saveDir;
    const timeoutMs = intSetting(ctx, domain, "timeoutMs", undefined, {
        min: 1,
        domainId: CONFIG_DOMAIN,
        // ★ 逐字保留旧行为：config 里的 `timeoutMs` 也接受 `"5000"`（旧实现是 `Number(raw)`）。
        coerce: true,
    });
    if (timeoutMs !== undefined) options.timeoutMs = timeoutMs;
    const precheck = boolSetting(ctx, domain, "precheck", undefined, { domainId: CONFIG_DOMAIN });
    if (precheck !== undefined) options.precheck = precheck;
    return options;
}

// ★ 参考入口的链**只构造一次**（一次 Run 内 config / params 不变；链本身无状态 ⇒ 缓存链对象
//   不影响"进度只在 ev.name"）。⚠ 这**不是**在修预算问题：2026-11-21 实测缓存与"每次重建"
//   在 `plugin/load-save-e2e` 上**同样**会撞 `onTrigger: callback budget exceeded: load:confirm2`
//   （2 ms 那条线；跟踪单 `REQ-01M7VVDV69MJXRHNDGR50H7VHC`，⛔ 不靠放宽预算 / 删日志来"修"）。
//   ★ 2026-11-25：参数源改成 `ctx.config` 之后，**每拍重读 config 的代价也要省掉**
//   （`domain()` 每次新建一个绑定对象 + 若干宿主调用；2 ms 窗口里那是可观测的份额）⇒ 第一次
//   `chainFor` 之后直接复用整条链。
/** @type {Chain | null} */
let entryChain = null;

/**
 * `boot:` 前缀归启动链（含它的 timeout），其余归载入链 —— 两段名字不相交（`./chains.js` 头）。
 *
 * @param {RunCtx} ctx
 * @returns {Chain}
 */
function chainFor(ctx) {
    if (entryChain !== null) return entryChain;
    const domain = openConfigDomain(ctx, CONFIG_DOMAIN);
    const slot = slotSetting(ctx, domain, undefined, CONFIG_DOMAIN);
    const load = loadSave(slot, optionsFor(ctx, domain));
    const boot = bootToTitle({ onDone: (ctx2) => load.start(ctx2) });
    entryChain = route((ev) => String(ev.name).startsWith("boot:"), boot, load);
    return entryChain;
}

/** @type {Chain} */
const plugin = {
    start(ctx) {
        const domain = openConfigDomain(ctx, CONFIG_DOMAIN);
        if (scriptListSetting(ctx, domain, "expect", CONFIG_DOMAIN) === null) {
            ctx.log(
                "info",
                `load-save: expect 未给 ⇒ 用默认 ${DEFAULT_EXPECT.join(",")}` +
                    `（写在 plugin-config.json 的 ${CONFIG_DOMAIN}.expect，或用 --plugin-param expect=FIELD,SC0000）`,
            );
        }
        return chainFor(ctx).start(ctx);
    },
    onTrigger(ctx, ev) {
        return chainFor(ctx).onTrigger(ctx, ev);
    },
};

export default plugin;
