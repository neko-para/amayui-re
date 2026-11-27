// @ts-check
// emulator-lift · 插件侧**唯一**的取配置入口（`lib/`；只被 `scripts/plugins/` 的入口 import）
//
// ★ 为什么有一层 lib（2026-11-25）：`load-save.js` / `scenario-save10-field.js` 各自抄了一份
//   `configDomainOrNull` + `hasConfig` + `intSetting` / `boolSetting` / …；`field-point-visible.js`
//   也有一份。四份语义**不完全一样**（`precheck` 收 `"true"`、`on` 只收数字…），改一处另一处静默过期。
//   ⇒ 本文件是"config > `--plugin-param` > 默认"这条优先级的**唯一**实现。
//
// 口径（父级裁决 `.tmp/settings-overlay/decision.md` D1/D3）：
//   · `openConfigDomain` 只在 `config.available === true` 时返回域对象；否则 **null**（完全回退 params）。
//     ★ 调用点用 `domain === null || !domain.has(k)` 收窄（⛔ 不要 `@ts-ignore`、⛔ 不要把类型改宽）。
//   · "给了"的判据 = **存了这个 key**（`slot:0` / `on:false` / `""` 之外的 falsy 值也算给了）。
//     实现在 `configValue()`：一次 `get(key, MISSING)`（⛔ 不是 has+get 两次）；哨兵对象是**同一个
//     JS 引用**（宿主 `get` 对 "没有这个 key" 走 `JS_DupValue(fallback)`）⇒ `raw === MISSING` 可靠。
//   · **`null` = 没给**（与 2026-11 前逐字一致）：`<domain>.<key>: null` ⇒ 回退 `ctx.params` / 默认，
//     ⛔ 不 throw（旧 API 就是这么读的；`null` 不是一种合法的设置值）。
//   · 非法值一律 **throw**（文案带 `<domain>.<key>`）；⛔ 不做范围钳制 / ⛔ 不偷偷换默认值。
//   · `registerSettings` 把插件自报的 schema 写进**保留域** `settings.schema`（D1）；它**永不抛错** ——
//     宁可少一个设置项，也不能让 `start` 打死 Run。
//
// ⛔ 本文件不 import `scripts/ui/` 的任何东西（两个 JS 上下文互不 import）；⛔ 不用 `JSON` / `Date` /
//   `RegExp`（沙箱没有）；⛔ 不写裸 `0x…` 字面量（`tools/test/script-map.test.mjs` 扫 `lib/`）。

/** @typedef {import("@amayui/lift-plugin").RunCtx} RunCtx */
/** @typedef {import("@amayui/lift-plugin").ConfigDomain} ConfigDomain */

/**
 * 设置注册表所在的**保留域**（D1）。
 * ★ 页面侧 `scripts/ui/settings-page.js` 里有同一个字面量 —— 跨上下文不能 import ⇒ 两处都别改名。
 */
export const SETTINGS_SCHEMA_DOMAIN = "settings.schema";

/** `get(key, MISSING)` 的哨兵：区分"没有这个 key"与"存了 `null` / `false` / `0` / `""`。 */
const MISSING = { missing: true };

/**
 * 配置文件可用才返回域对象（否则 **null** ⇒ 完全回退到 `ctx.params`）。
 *
 * @param {RunCtx} ctx RunCtx。
 * @param {string} domainId 配置域 id（`^[A-Za-z0-9._-]{1,64}$`）。
 * @returns {ConfigDomain | null}
 */
export function openConfigDomain(ctx, domainId) {
    const config = ctx.config;
    if (config === undefined || config === null || config.available !== true) return null;
    return config.domain(domainId);
}

/**
 * 读一个配置值；**没有这个 key / 值是 `null`** ⇒ `undefined`。一次宿主调用（`has` + `get` 是两次）。
 *
 * ★ `null` 也算"没给"（与 2026-11 前的逐字口径一致）：旧代码把 `<domain>.<key>: null` 当没写这个
 *   key ⇒ 回退 `ctx.params` / 默认。⛔ 不要把 `null` 当"值非法"扔给 `normalizeBool`（那会让旧配置
 *   在 `start` 里 throw、Run 直接 failed）。
 *
 * @param {ConfigDomain | null} domain 域对象（不可用 = null）。
 * @param {string} key 配置键。
 * @returns {unknown}
 */
function configValue(domain, key) {
    if (domain === null) return undefined;
    const raw = domain.get(key, MISSING);
    if (raw === MISSING || raw === null) return undefined;
    return raw;
}

/**
 * `--plugin-param` 的空判（`undefined` / `null` / 空白串 = 没给）。
 *
 * @param {Readonly<Record<string, string>>} params 插件参数表。
 * @param {string} key 参数名。
 * @returns {string | undefined}
 */
function paramOrUndefined(params, key) {
    const raw = params[key];
    if (raw === undefined || raw === null || String(raw).trim() === "") return undefined;
    return String(raw);
}

/** @param {string | undefined} domainId @param {string} key @returns {string} */
function describe(domainId, key) {
    return domainId === undefined || domainId === "" ? key : domainId + "." + key;
}

/** @param {number} min @param {number} max @returns {string} */
function rangeText(min, max) {
    if (max === Infinity) return min <= 0 ? "非负" : ">=" + min;
    return "[" + min + "," + max + "]";
}

/** @param {unknown} e @returns {string} */
function messageOf(e) {
    const anyE = /** @type {any} */ (e);
    if (anyE !== null && anyE !== undefined && typeof anyE.message === "string") return anyE.message;
    return String(e);
}

/**
 * 整数设置：**config 优先**（存了就算给了），否则 `ctx.params`，再否则 `fallback`。
 *
 * ★ `opts.coerce === true` 时 config 侧也接受可转成数字的字符串（`load-save` 的 `timeoutMs` 旧行为
 *   就是 `Number(raw)`）；缺省 **strict**（config 值必须是 number 本身）。
 * ★ **不做范围钳制**：越界 = 非法 = throw（范围真源永远在调用方 / `lib/save.js`）。
 *
 * @template T
 * @param {RunCtx} ctx RunCtx。
 * @param {ConfigDomain | null} domain 域对象（不可用 = null）。
 * @param {string} key 配置键。
 * @param {T} fallback 两边都没给时返回它（`undefined` = 调用方自己决定默认）。
 * @param {{ min?: number, max?: number, domainId?: string, coerce?: boolean }} [opts]
 * @returns {number | T}
 */
export function intSetting(ctx, domain, key, fallback, opts) {
    const min = opts !== undefined && opts.min !== undefined ? opts.min : 0;
    const max = opts !== undefined && opts.max !== undefined ? opts.max : Infinity;
    const label = describe(opts !== undefined ? opts.domainId : undefined, key);
    const raw = configValue(domain, key);
    if (raw !== undefined) {
        const value =
            opts !== undefined && opts.coerce === true && typeof raw !== "number" ? Number(raw) : raw;
        if (
            typeof value !== "number" ||
            !Number.isFinite(value) ||
            !Number.isInteger(value) ||
            value < min ||
            value > max
        ) {
            throw new Error(`config ${label} 需要 ${rangeText(min, max)} 的整数（收到 ${String(raw)}）`);
        }
        return value;
    }
    const text = paramOrUndefined(ctx.params, key);
    if (text === undefined) return fallback;
    const value = Number(text);
    if (!Number.isFinite(value) || !Number.isInteger(value) || value < min || value > max) {
        throw new Error(`${label} 需要 ${rangeText(min, max)} 的整数（收到 ${text}）`);
    }
    return value;
}

/**
 * 布尔设置的**并集语义**（2026-11-25 统一）：`boolean` / 数字 `0|1` / 字符串
 * `"0" "1" "true" "false"`（大小写不敏感、两端空白忽略）。其它一律 throw。
 *
 * ★ 与 `precheck` 的旧口径对齐（它本来就收 `"true"`），并把 `field-point-visible` 的
 *   `on`/`diag` 一起收进来（旧实现收任意非零整数 —— 那一档**不放宽**到 `2`，见 README）。
 *
 * @template T
 * @param {RunCtx} ctx RunCtx。
 * @param {ConfigDomain | null} domain 域对象（不可用 = null）。
 * @param {string} key 配置键。
 * @param {T} fallback 两边都没给时返回它（`undefined` = 调用方自己决定默认）。
 * @param {{ domainId?: string }} [opts]
 * @returns {boolean | T}
 */
export function boolSetting(ctx, domain, key, fallback, opts) {
    const label = describe(opts !== undefined ? opts.domainId : undefined, key);
    const raw = configValue(domain, key);
    if (raw !== undefined) return normalizeBool(raw, label);
    const text = paramOrUndefined(ctx.params, key);
    if (text === undefined) return fallback;
    return normalizeBool(text, label);
}

/**
 * @param {unknown} raw 配置值 / params 字符串。
 * @param {string} label 报错用的 `<domain>.<key>`。
 * @returns {boolean}
 */
function normalizeBool(raw, label) {
    if (typeof raw === "boolean") return raw;
    if (typeof raw === "number") {
        if (raw === 0) return false;
        if (raw === 1) return true;
        throw new Error(`config ${label} 需要 boolean 或 0/1（收到 ${String(raw)}）`);
    }
    if (typeof raw === "string") {
        const text = raw.trim().toLowerCase();
        if (text === "1" || text === "true") return true;
        if (text === "0" || text === "false") return false;
    }
    throw new Error(
        `config ${label} 需要 boolean 或 "0"/"1"/"true"/"false"（收到 ${String(raw)}）`,
    );
}

/**
 * 文本设置（`saveDir`）：config 优先（必须是非空字符串），params 回退，缺 ⇒ `fallback`。
 *
 * @param {RunCtx} ctx RunCtx。
 * @param {ConfigDomain | null} domain 域对象（不可用 = null）。
 * @param {string} key 配置键。
 * @param {string | undefined} [fallback] 两边都没给时返回它。
 * @param {{ domainId?: string }} [opts]
 * @returns {string | undefined}
 */
export function textSetting(ctx, domain, key, fallback, opts) {
    const label = describe(opts !== undefined ? opts.domainId : undefined, key);
    const raw = configValue(domain, key);
    if (raw !== undefined) {
        if (typeof raw !== "string" || raw.trim() === "") {
            throw new Error(`config ${label} 必须是非空字符串（收到 ${String(raw)}）`);
        }
        return raw;
    }
    const text = paramOrUndefined(ctx.params, key);
    if (text === undefined) return fallback;
    return text;
}

/**
 * `expect` 的脚本名表归一：`string[]` / 逗号串 ⇒ `string[]`（裸名补 `.BIN`、元素两端 trim）。
 * 空 / 非字符串元素 / 给空 ⇒ **throw**（⛔ 不猜默认值）。
 *
 * @param {unknown} value config 里的原值。
 * @param {string} [domainId] 报错用的域 id。
 * @param {string} [key] 报错用的 key（缺省 `expect`）。
 * @returns {string[]}
 */
export function normalizeScriptList(value, domainId, key) {
    const label = describe(domainId, key === undefined ? "expect" : key);
    /** @type {string[]} */
    const out = [];
    /** @param {unknown} item */
    const push = (item) => {
        if (typeof item !== "string" || item.trim() === "") {
            throw new Error(`config ${label} 的元素必须是非空字符串（收到 ${String(item)}）`);
        }
        const name = item.trim();
        out.push(name.endsWith(".BIN") ? name : name + ".BIN");
    };
    if (Array.isArray(value)) {
        for (const item of value) push(item);
    } else if (typeof value === "string") {
        for (const part of value.split(",")) {
            if (part.trim().length !== 0) push(part);
        }
    } else {
        throw new Error(`config ${label} 必须是 string[] 或逗号串（收到 ${typeof value}）`);
    }
    if (out.length === 0) {
        throw new Error(`config ${label} 给空了（要么不给，要么给 ["FIELD.BIN"]）`);
    }
    return out;
}

/**
 * `expect` 设置：config 优先（`normalizeScriptList`）；否则 params 逗号串；都没有 ⇒ **null**
 * （= 没给，默认值留给调用方 —— `load-save.js` 与 `scenario-save10-field.js` 的默认不同）。
 *
 * ★ params 全空（如 `","`）⇒ **`[]`**（不是 null）：与 `load-save.js` 的旧行为逐字一致
 *   （`loadSave` 会对空 `expect` 响亮 throw）；`scenario-save10-field.js` 自己把 `[]` 当"没给"。
 *
 * @param {RunCtx} ctx RunCtx。
 * @param {ConfigDomain | null} domain 域对象（不可用 = null）。
 * @param {string} key 配置键（`expect`）。
 * @param {string} [domainId] 报错用的域 id。
 * @returns {string[] | null}
 */
export function scriptListSetting(ctx, domain, key, domainId) {
    const raw = configValue(domain, key);
    if (raw !== undefined) return normalizeScriptList(raw, domainId, key);
    const text = paramOrUndefined(ctx.params, key);
    if (text === undefined) return null;
    /** @type {string[]} */
    const out = [];
    for (const part of text.split(",")) {
        const name = part.trim();
        if (name.length === 0) continue;
        out.push(name.endsWith(".BIN") ? name : name + ".BIN");
    }
    return out;
}

/**
 * 存档槽：config 侧只判 `Number.isInteger`；params 侧 `Number(raw)`；空 ⇒ `fallback`。
 * **不做范围 / 负数校验**（范围真源 = `lib/save.js:414` 的 `MAX_SLOT`）。
 *
 * @param {RunCtx} ctx RunCtx。
 * @param {ConfigDomain | null} domain 域对象（不可用 = null）。
 * @param {number | undefined} [fallback] params 没给时用它；省略 = `Number(params.slot)`（`load-save` 旧行为）。
 * @param {string} [domainId] 报错用的域 id。
 * @returns {number}
 */
export function slotSetting(ctx, domain, fallback, domainId) {
    const raw = configValue(domain, "slot");
    if (raw !== undefined) {
        if (typeof raw !== "number" || !Number.isInteger(raw)) {
            throw new Error(`config ${describe(domainId, "slot")} 必须是整数（收到 ${String(raw)}）`);
        }
        return raw;
    }
    const text = paramOrUndefined(ctx.params, "slot");
    if (text === undefined) {
        // ★ 逐字保留旧行为：load-save 的"没给"是 `Number(undefined)` = NaN（由 loadSave 响亮 throw）；
        //   `""` / 空白串是 Number("") = 0。save10 传了 fallback（10）⇒ 走 fallback。
        return fallback !== undefined ? fallback : Number(ctx.params["slot"]);
    }
    return Number(text);
}

/**
 * @typedef {{ type: "bool" | "number", label: string, default?: unknown, min?: number,
 *             max?: number, step?: number }} SettingsEntrySpec
 * @typedef {{ domain: string, title: string, owner?: string, live: "frame" | "run" | "restart",
 *             entries: Record<string, SettingsEntrySpec> }} SettingsSpec
 * @typedef {{ owner?: string, title: string, live: string,
 *             entries: Record<string, unknown> }} NormalizedSchema
 */

/**
 * 把插件设置 schema 写进**保留域** `settings.schema`（D1）。
 *
 * 写入形状：`{ "<plugin-domain>": { title, live, entries: { <key>: { type, label, default?, min?, max?, step? } } } }`；
 * 有 `owner` 时多一个 `owner` 字段（= 插件名，设置页小节标题的归属前缀）。
 * ★ 先 `all()` 比一下：**相同就不写**（⛔ 不产生每 Run 一次的脏写）。
 * ★ **永不抛错**：任何 `set` 失败（超上限 / 只读 / 没有 appdata）只落一行 `ctx.log("warn", …)`。
 *
 * @param {RunCtx} ctx RunCtx。
 * @param {SettingsSpec} spec 插件自报的 schema。
 * @returns {void}
 */
export function registerSettings(ctx, spec) {
    try {
        if (spec === null || typeof spec !== "object") return;
        if (typeof spec.domain !== "string" || spec.domain === "") return;
        const domain = openConfigDomain(ctx, SETTINGS_SCHEMA_DOMAIN);
        // ★ 没有 appdata / 没有桥 ⇒ 设置页本来就不可用（页面只显示一行状态），这里静默跳过。
        if (domain === null) return;
        const next = buildSchema(spec);
        if (next === null) return;
        const all = domain.all();
        const current = all !== null && typeof all === "object" ? all[spec.domain] : undefined;
        if (sameSchema(current, next)) return;
        const receipt = domain.set(spec.domain, next);
        if (receipt === null || receipt === undefined || receipt.applied !== true) {
            const code =
                receipt !== null && receipt !== undefined && typeof receipt.code === "string"
                    ? receipt.code
                    : "not_applied";
            logWarn(ctx, `settings.schema: ${spec.domain} 没写进去（${code}）`);
        }
    } catch (e) {
        logWarn(ctx, `settings.schema: 注册 ${String(spec !== null && spec !== undefined ? spec.domain : "?")} 失败：${messageOf(e)}`);
    }
}

/**
 * 把调用方给的 spec 归一成**将要写盘**的那个值（只留下认识的字段；顺序 = 插入序）。
 *
 * @param {SettingsSpec} spec
 * @returns {NormalizedSchema | null}
 */
function buildSchema(spec) {
    if (typeof spec.title !== "string") return null;
    if (spec.entries === null || typeof spec.entries !== "object") return null;
    const live = spec.live === "frame" || spec.live === "run" ? spec.live : "restart";
    /** @type {Record<string, unknown>} */
    const entries = {};
    for (const key of Object.keys(spec.entries)) {
        const entry = spec.entries[key];
        if (entry === null || typeof entry !== "object") continue;
        if (entry.type !== "bool" && entry.type !== "number") continue;
        if (typeof entry.label !== "string") continue;
        /** @type {Record<string, unknown>} */
        const item = { type: entry.type, label: entry.label };
        if (entry.default !== undefined) item["default"] = entry.default;
        if (typeof entry.min === "number") item["min"] = entry.min;
        if (typeof entry.max === "number") item["max"] = entry.max;
        if (typeof entry.step === "number") item["step"] = entry.step;
        entries[key] = item;
    }
    /** @type {NormalizedSchema} */
    const out = { title: spec.title, live, entries };
    // ★ `owner` 可选：缺省 = **不写**（页面回退到旧的 `<title>`，⛔ 不写 domain id）。
    if (typeof spec.owner === "string" && spec.owner !== "") out.owner = spec.owner;
    return out;
}

/**
 * 两个 schema 节点是否**逐字段相同**（`title` / `live` / `entries` 的每个 key 与每个已知字段）。
 *
 * @param {unknown} left 从 `all()` 读回来的当前值。
 * @param {NormalizedSchema} right 将要写的值。
 * @returns {boolean}
 */
function sameSchema(left, right) {
    if (left === null || typeof left !== "object" || Array.isArray(left)) return false;
    const obj = /** @type {Record<string, unknown>} */ (left);
    if (obj["title"] !== right.title || obj["live"] !== right.live) return false;
    // ★ `owner` 也参与比较：旧盘上没有 owner、新 spec 有 ⇒ 要重写一次（否则新归属永远不落盘）。
    if (obj["owner"] !== right.owner) return false;
    const rawEntries = obj["entries"];
    if (rawEntries === null || typeof rawEntries !== "object" || Array.isArray(rawEntries)) return false;
    const entries = /** @type {Record<string, unknown>} */ (rawEntries);
    const leftKeys = Object.keys(entries);
    const rightKeys = Object.keys(right.entries);
    if (leftKeys.length !== rightKeys.length) return false;
    for (const key of rightKeys) {
        if (!sameEntry(entries[key], right.entries[key])) return false;
    }
    return true;
}

/**
 * @param {unknown} left 当前节点。
 * @param {unknown} right 将要写的节点。
 * @returns {boolean}
 */
function sameEntry(left, right) {
    if (left === null || typeof left !== "object" || Array.isArray(left)) return false;
    const obj = /** @type {Record<string, unknown>} */ (left);
    const fields = ["type", "label", "default", "min", "max", "step"];
    for (const field of fields) {
        const hasLeft = obj[field] !== undefined;
        const hasRight = /** @type {Record<string, unknown>} */ (right)[field] !== undefined;
        if (hasLeft !== hasRight) return false;
        if (hasLeft && obj[field] !== /** @type {Record<string, unknown>} */ (right)[field]) return false;
    }
    return true;
}

/**
 * 唯一允许的日志失败面：注册失败写一行 warn，**⛔ 不再抛**。
 *
 * @param {RunCtx} ctx RunCtx。
 * @param {string} message 文案。
 * @returns {void}
 */
function logWarn(ctx, message) {
    try {
        ctx.log("warn", message);
    } catch (e) {
        // ⛔ 连日志都失败时也不许打死 Run（`ctx.log` 的失败不是本插件的失败）。
        const ignored = /** @type {any} */ (e);
        void ignored;
    }
}
