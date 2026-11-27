// @ts-check
// emulator-lift · **二级设置页**（D2/D4；纯绘制 + 交互，**无模块级可变状态**）
//
// ★ 本文件只依赖两样东西：
//   ① `h` = 经典回退 HUD 的 `HudHost`（`measure` / `rect` / `text` / `hover` / `active` / `click` /
//      `rightClick`）**外加 `h.config`**（D2 的 `{ available, domain(id) }`；`undefined` = 没有桥）；
//   ② `state` = 调用方（`hud.js`）持有的那一小块状态（目前只有 `status`）。
//   脚本自己的状态一律留在 `hud.js`；本文件**没有**模块级可变对象（⛔ 不缓存上一帧的值）。
//
// 契约（父级裁决 `.tmp/settings-overlay/decision.md`）：
//   · `h.config === undefined` ⇒ 页面**不打开**（调用方只画一行状态）：`canOpen(h)` 是那个判据；
//     `drawPage` 在这种情况下返回 `{ closed: true, status: "没有插件在跑" }` 且**不画任何控件**
//     （⛔ 不造假开关 —— 预设模式 `--plugin hud` 就没有 config）。
//     ★ 桥可用但 `settings.schema` 空表（没有可画项）⇒ 也**不打开**（`hud.js` 用 `hasSettings(h)`
//     判；状态行 `settings: 没有注册设置`）—— ⛔ 不打开一个什么也改不了的空白页。
//   · 读：打开/每帧每个可见项 `domain.get(key, default)`（⛔ 不 `all()` 拉值；`all()` **只**用于
//     `settings.schema` 这一个域）。schema 坏形状（不是对象 / 缺 `entries`）⇒ 跳过该域。
//   · 写：点击当拍 `set`，看回执 `{ applied, code }`：`applied:false` ⇒ `settings: 只读` /
//     `settings: 没有 appdata`；`set` **抛错** ⇒ `status = "settings: <message>"`（⛔ 绝不冒出 render）。
//   · 非法存储值 ⇒ 显示 `<invalid>`，第一次 `inc/dec` 写回 `default ± step`（再按 min/max 钳）。
//
// 版式（1280×720 参考像素，无滚动；实测见 `.tmp/settings-overlay/settings-js.md`）：
//   全屏背板 `#000000A6` + `key:"settings.bg"`（**先画** ⇒ 模态，吞掉所有点击）；
//   页板 `x=0`、`y=round((720-H)/2)`、`PAD=8`、行高 `max(20, measure("Ag",16).h+2)`；
//   bool 整行 26px 一个命中区（`set:<domain>:<key>`，右端 20×20 方框 + `on/off` 文本）；
//   number 行 `[−]` 24×24（`dec:`）+ 值文本（72 宽右对齐）+ `[+]` 24×24（`inc:`）；
//   `BACK` 112×28（`kit.Button`，`key:"settings.back"`）；最多 12 行，超出打 `… 还有 N 项`。
//
// ⛔ 不做滚动 / 输入框 / 下拉 / 键盘导航（D6：键只有 F9 一个读数）。
// ⛔ 不 import `scripts/plugins/` 的任何东西（两个 JS 上下文互不 import）。

import { Button, Checkbox } from "./kit.js";

/** 设置注册表所在的保留域（与 `plugins/lib/settings.js` 的 `SETTINGS_SCHEMA_DOMAIN` 同一个字面量）。 */
const SCHEMA_DOMAIN = "settings.schema";

const PAD = 8;
const SIZE = 16;
/** bool 行 / 命中区高度（参考像素）。 */
const ROW_H = 26;
/** 复选框方框边长（与 `kit.Checkbox` 的几何约定一致）。 */
const BOX = 20;
/** number 值文本区宽度（右对齐；值真的比它宽时向左溢，v1 接受）。 */
const VALUE_W = 72;
/** `[−]` / `[+]` 的边长。 */
const MINI = 24;
const BACK_W = 112;
const BACK_H = 28;
/** 一页最多画多少**项**（domain 小节标题不计入）。 */
const MAX_ROWS = 12;

const BG = "#000000A6";
const PANEL_BG = "#101820E6";
const FG = "#EBFFEB";
const DIM = "#9FB0C0";
const WARN = "#FFB4B4";
/** D1 的固定文案（⛔ 不承诺"都当拍生效"：陈旧 schema 无法从 JS 侧区分）。 */
const LIVE_LINE = "live: 正在运行的插件当拍生效；其余为预配置";

/** @param {string} domain @param {string} key @returns {string} */
function setKey(domain, key) {
    return "set:" + domain + ":" + key;
}
/** @param {string} domain @param {string} key @returns {string} */
function decKey(domain, key) {
    return "dec:" + domain + ":" + key;
}
/** @param {string} domain @param {string} key @returns {string} */
function incKey(domain, key) {
    return "inc:" + domain + ":" + key;
}

/**
 * 命中键的唯一真源（页面内部与测试都按这三个函数拼）。
 * ★ 形状固定为 `set|dec|inc:<domain>:<key>`；调用方只当字符串用（⛔ 不解析）。
 */
export const PAGE_KEYS = {
    bg: "settings.bg",
    back: "settings.back",
    set: setKey,
    dec: decKey,
    inc: incKey,
};

/** @typedef {{ x: number, y: number, w: number, h: number }} PageRect */
/** @typedef {{ x: number, y: number, w: number, h: number, fill: string, key?: string }} PageRectOptions */
/** @typedef {{ x: number, y: number, text: string, size: number, color: string, bold?: boolean }} PageTextOptions */
/**
 * @typedef {{
 *   get: (key: string, fallback?: unknown) => unknown,
 *   has: (key: string) => boolean,
 *   set: (key: string, value: unknown) => { applied: boolean, code?: string },
 *   delete: (key: string) => boolean,
 *   all: () => Record<string, unknown>,
 * }} PageConfigDomain
 */
/** @typedef {{ available: boolean, domain: (id: string) => PageConfigDomain }} PageConfigApi */
/**
 * @typedef {{
 *   measure: (text: string, size: number) => { w: number, h: number },
 *   rect: (o: PageRectOptions) => void,
 *   text: (o: PageTextOptions) => void,
 *   hover?: string,
 *   active?: string,
 *   click?: { key: string, x: number, y: number },
 *   rightClick?: boolean,
 *   config?: PageConfigApi,
 * }} SettingsHost
 */
/** @typedef {{ status?: string }} SettingsPageState */
/** @typedef {{ key: string, type: "bool" | "number", label: string, default: unknown, min: number, max: number, step: number }} SchemaItem */
/** @typedef {{ id: string, title: string, owner?: string, live: string, items: SchemaItem[] }} SchemaDomain */
/** @typedef {{ domain: string, item: SchemaItem, kind: "set" | "dec" | "inc" }} ClickedRow */
/** @typedef {{ drawn: SchemaDomain[], hidden: number }} PagePlan */

/**
 * 页面能不能打开：`h.config` 存在且 `available === true`（D2）。
 *
 * ★ 这是 `hud.js` 的**唯一**判据：不可用 ⇒ 只画一行状态（`没有插件在跑`），⛔ 不画死控件。
 * ★ 本函数只回答"桥在不在"；"打开之后有没有东西可画"是 `hasSettings`（见下）—— 两者合起来才是
 *   `hud.js` 入口的放行条件（空 schema 打开 = 一张空白页，D2 明确不要）。
 *
 * @param {SettingsHost | null | undefined} h 宿主适配器（可为 `undefined` —— 测试直接判这一条）。
 * @returns {boolean}
 */
export function canOpen(h) {
    if (h === null || h === undefined) return false;
    const config = h.config;
    return config !== undefined && config !== null && config.available === true;
}

/**
 * 桥可用时**有没有可画的设置项**（`settings.schema` 至少一项，坏形状的域 / 项照 `drawPage` 的规则跳过）。
 *
 * ★ 为什么单列一条：桥可用（`available === true`）但 `settings.schema` 空表时，`drawPage` 仍画得出
 *   "SETTINGS + live + BACK"的**空白页** —— 用户点了 `settings…` 却什么也改不了，而页面上没有任何
 *   说明。D2 的口径是"没有插件注册设置就不打开"；判据在 `hud.js` 的入口（状态行
 *   `settings: 没有注册设置`），本文件只提供"有没有"这一个读数。
 * ★ 与 `drawPage` 共用 `readSchema` / `planRows` ⇒ "画得出来的项" = 这里的"有"（⛔ 不另立一套判据）。
 *
 * @param {SettingsHost | null | undefined} h 宿主适配器。
 * @returns {boolean}
 */
export function hasSettings(h) {
    if (h === null || h === undefined) return false;
    if (!canOpen(h)) return false;
    return planRows(readSchema(h)).drawn.length > 0;
}

/**
 * 画二级设置页（每帧一次；display list 由宿主清空后只追加）。
 *
 * @param {SettingsHost} h 宿主适配器（含 `config`）。
 * @param {SettingsPageState} [state] 调用方持有的状态（本文件只改 `status`）。
 * @returns {{ closed?: boolean, status?: string }}
 *   `closed:true` = 关闭二级页（点 `BACK` / 没有 config）；`status` = 要留到下一帧的状态行。
 */
export function drawPage(h, state) {
    if (!canOpen(h)) return { closed: true, status: "没有插件在跑" };
    try {
        return drawOpen(h, state);
    } catch (e) {
        // ⛔ 页面里的任何宿主调用抛错都不许冒出 render（否则叠层被永久关掉）。
        return { status: errorStatus(e) };
    }
}

/**
 * 可打开时才走这里（`canOpen` 已保证 `h.config` 存在）。
 *
 * @param {SettingsHost} h 宿主适配器。
 * @param {SettingsPageState | undefined} state 调用方状态。
 * @returns {{ closed?: boolean, status?: string }}
 */
function drawOpen(h, state) {
    /** @type {Array<{ id: string, dom: PageConfigDomain }>} */
    const cache = [];
    const schema = readSchema(h);
    const plan = planRows(schema);
    let status = state !== null && state !== undefined && typeof state.status === "string" ? state.status : "";

    // ★ 任意位置右键 = 返回（与游戏内一致的 back；含控件上 —— 所以放在交互段**最前面**，先于 `h.click`）。
    //   ⛔ 不影响左键：`rightClick` 是 `hud.js` 每帧算的**按下沿**，与 `h.click`（一次性载荷）互不吞。
    if (h.rightClick === true) {
        writeState(state, status);
        return { closed: true };
    }

    // ① 交互**先于**绘制：同一帧就能显示 set 之后的值（D3 的"当拍生效"）。
    const click = h.click;
    if (click !== undefined && click !== null) {
        const key = String(click.key);
        if (key === PAGE_KEYS.back) {
            writeState(state, status);
            return { closed: true };
        }
        const hit = findClicked(plan.drawn, key);
        if (hit !== null) status = applyClick(h, cache, hit);
    }

    // ② 尺寸（每一帧按当前 schema / 状态重算；无滚动）。
    const lineH = Math.max(20, h.measure("Ag", SIZE).h + 2);
    let rowCount = 0;
    for (const sd of plan.drawn) rowCount += sd.items.length;
    const contentH =
        lineH + // SETTINGS
        lineH + // live 行
        plan.drawn.length * lineH + // 每域一个小节标题
        rowCount * ROW_H +
        (plan.hidden > 0 ? lineH : 0) +
        (status !== "" ? lineH : 0) +
        PAD +
        BACK_H;
    const panelH = contentH + 2 * PAD;
    const pageY = Math.round((720 - panelH) / 2);

    let maxW = h.measure("SETTINGS", SIZE).w;
    const liveW = h.measure(LIVE_LINE, SIZE).w;
    if (liveW > maxW) maxW = liveW;
    if (status !== "") {
        const statusW = h.measure(status, SIZE).w;
        if (statusW > maxW) maxW = statusW;
    }
    if (BACK_W > maxW) maxW = BACK_W;
    for (const sd of plan.drawn) {
        const titleW = h.measure(sectionTitle(sd), SIZE).w;
        if (titleW > maxW) maxW = titleW;
        for (const item of sd.items) {
            const labelW = h.measure(item.label, SIZE).w;
            const rightW =
                item.type === "bool"
                    ? BOX + 6 + h.measure("off", SIZE).w
                    : 2 * MINI + 12 + VALUE_W;
            if (labelW + rightW > maxW) maxW = labelW + rightW;
        }
    }
    const panelW = Math.min(560, Math.max(320, maxW + 2 * PAD));
    const rowW = panelW - 2 * PAD;

    // ③ 绘制：背板（模态）→ 页板 → 文本 / 控件 → BACK。
    h.rect({ x: 0, y: 0, w: 1280, h: 720, fill: BG, key: PAGE_KEYS.bg });
    h.rect({ x: 0, y: pageY, w: panelW, h: panelH, fill: PANEL_BG });
    let y = pageY + PAD;
    h.text({ x: PAD, y, text: "SETTINGS", size: SIZE, color: FG, bold: true });
    y += lineH;
    h.text({ x: PAD, y, text: LIVE_LINE, size: SIZE, color: DIM });
    y += lineH;

    for (const sd of plan.drawn) {
        h.text({ x: PAD, y, text: sectionTitle(sd), size: SIZE, color: DIM, bold: true });
        y += lineH;
        for (const item of sd.items) {
            const rowY = y;
            if (item.type === "bool") {
                const read = readBool(domainOf(h, cache, sd.id), item);
                const valueText = read.ok ? (read.value ? "on" : "off") : "<invalid>";
                Checkbox(
                    {
                        key: setKey(sd.id, item.key),
                        rect: { x: PAD, y: rowY, w: rowW, h: ROW_H },
                        label: item.label,
                        checked: read.ok ? read.value : false,
                    },
                    h,
                );
                const valueW = h.measure(valueText, SIZE).w;
                h.text({
                    x: PAD + rowW - BOX - 6 - valueW,
                    y: rowY + Math.max(0, Math.round((ROW_H - SIZE) / 2)),
                    text: valueText,
                    size: SIZE,
                    color: read.ok ? FG : WARN,
                });
            } else {
                const read = readNumber(domainOf(h, cache, sd.id), item);
                const valueText = read.ok ? formatNumber(read.value) : "<invalid>";
                h.text({
                    x: PAD + 2,
                    y: rowY + Math.max(0, Math.round((ROW_H - SIZE) / 2)),
                    text: item.label,
                    size: SIZE,
                    color: FG,
                });
                const incX = PAD + rowW - MINI;
                const decX = incX - 4 - VALUE_W - 4 - MINI;
                const valueW = h.measure(valueText, SIZE).w;
                h.text({
                    x: incX - 4 - valueW,
                    y: rowY + Math.max(0, Math.round((ROW_H - SIZE) / 2)),
                    text: valueText,
                    size: SIZE,
                    color: read.ok ? FG : WARN,
                });
                Button(
                    { key: decKey(sd.id, item.key), rect: { x: decX, y: rowY + 1, w: MINI, h: MINI }, label: "−" },
                    h,
                );
                Button(
                    { key: incKey(sd.id, item.key), rect: { x: incX, y: rowY + 1, w: MINI, h: MINI }, label: "+" },
                    h,
                );
            }
            y += ROW_H;
        }
    }

    if (plan.hidden > 0) {
        h.text({ x: PAD, y, text: "… 还有 " + plan.hidden + " 项", size: SIZE, color: DIM });
        y += lineH;
    }
    if (status !== "") {
        h.text({ x: PAD, y, text: status, size: SIZE, color: WARN });
        y += lineH;
    }
    Button(
        {
            key: PAGE_KEYS.back,
            rect: { x: PAD, y: pageY + panelH - PAD - BACK_H, w: BACK_W, h: BACK_H },
            label: "BACK",
        },
        h,
    );

    writeState(state, status);
    return status === "" ? {} : { status };
}

/**
 * 读 schema：`h.config.domain("settings.schema").all()`，**容忍坏形状**。
 *
 * @param {SettingsHost} h 宿主适配器（`canOpen` 已保证有 config）。
 * @returns {SchemaDomain[]}
 */
function readSchema(h) {
    /** @type {SchemaDomain[]} */
    const out = [];
    const config = h.config;
    if (config === undefined || config === null) return out;
    /** @type {Record<string, unknown>} */
    let all;
    try {
        all = config.domain(SCHEMA_DOMAIN).all();
    } catch (e) {
        return out;
    }
    if (all === null || typeof all !== "object" || Array.isArray(all)) return out;
    for (const id of Object.keys(all)) {
        const raw = all[id];
        if (raw === null || typeof raw !== "object" || Array.isArray(raw)) continue;
        const obj = /** @type {Record<string, unknown>} */ (raw);
        const rawEntries = obj["entries"];
        if (rawEntries === null || typeof rawEntries !== "object" || Array.isArray(rawEntries)) continue;
        const entries = /** @type {Record<string, unknown>} */ (rawEntries);
        /** @type {SchemaItem[]} */
        const items = [];
        for (const key of Object.keys(entries)) {
            const rawEntry = entries[key];
            if (rawEntry === null || typeof rawEntry !== "object" || Array.isArray(rawEntry)) continue;
            const entry = /** @type {Record<string, unknown>} */ (rawEntry);
            const type = entry["type"];
            if (type !== "bool" && type !== "number") continue;
            const label = typeof entry["label"] === "string" ? entry["label"] : key;
            items.push({
                key,
                type,
                label,
                default: entry["default"],
                min: typeof entry["min"] === "number" ? entry["min"] : -Infinity,
                max: typeof entry["max"] === "number" ? entry["max"] : Infinity,
                step: typeof entry["step"] === "number" ? entry["step"] : 1,
            });
        }
        out.push({
            id,
            title: typeof obj["title"] === "string" ? obj["title"] : id,
            // ★ 可选归属（插件名）：缺省 = 不写 ⇒ 小节标题保持旧的 `<title>`（⛔ 不回退到 domain id）。
            owner: typeof obj["owner"] === "string" && obj["owner"] !== "" ? obj["owner"] : undefined,
            live: typeof obj["live"] === "string" ? obj["live"] : "",
            items,
        });
    }
    return out;
}

/**
 * 把 schema 压成"这一页画什么"：每域最多吃到 12 项上限；`hidden` = 没画出来的项数。
 *
 * @param {SchemaDomain[]} schema 读出来的域表。
 * @returns {PagePlan}
 */
function planRows(schema) {
    let total = 0;
    for (const sd of schema) total += sd.items.length;
    let left = MAX_ROWS;
    /** @type {SchemaDomain[]} */
    const drawn = [];
    for (const sd of schema) {
        if (left <= 0) break;
        /** @type {SchemaItem[]} */
        const items = [];
        for (const item of sd.items) {
            if (items.length >= left) break;
            items.push(item);
        }
        if (items.length === 0) continue;
        drawn.push({ id: sd.id, title: sd.title, owner: sd.owner, live: sd.live, items });
        left -= items.length;
    }
    let rowCount = 0;
    for (const sd of drawn) rowCount += sd.items.length;
    return { drawn, hidden: total - rowCount };
}

/**
 * 点击落在哪一行（只有**画出来的**行才有命中区）。
 *
 * @param {SchemaDomain[]} drawn 已画出来的域。
 * @param {string} key `game.click.key`。
 * @returns {ClickedRow | null}
 */
function findClicked(drawn, key) {
    for (const sd of drawn) {
        for (const item of sd.items) {
            if (item.type === "bool" && key === setKey(sd.id, item.key)) {
                return { domain: sd.id, item, kind: "set" };
            }
            if (item.type === "number") {
                if (key === decKey(sd.id, item.key)) return { domain: sd.id, item, kind: "dec" };
                if (key === incKey(sd.id, item.key)) return { domain: sd.id, item, kind: "inc" };
            }
        }
    }
    return null;
}

/**
 * 施加一次点击（`set` 的那一下）：返回新的状态行（空串 = 成功 / 清掉旧状态）。
 *
 * ★ 非法存储值 + `inc/dec` ⇒ 基数用 `default`（第一次写回 `default ± step`），再按 min/max 钳。
 * ★ `set` 的回执 / 抛错都在这里消化（⛔ 不冒到 render 外）。
 *
 * @param {SettingsHost} h 宿主适配器。
 * @param {Array<{ id: string, dom: PageConfigDomain }>} cache 本帧的域对象缓存。
 * @param {ClickedRow} clicked 命中的行。
 * @returns {string}
 */
function applyClick(h, cache, clicked) {
    try {
        const domain = domainOf(h, cache, clicked.domain);
        const item = clicked.item;
        /** @type {unknown} */
        let next;
        if (clicked.kind === "set") {
            next = !readBool(domain, item).value;
        } else {
            const read = readNumber(domain, item);
            const base = read.ok ? read.value : entryDefaultNumber(item);
            next = clampNumber(base + (clicked.kind === "inc" ? item.step : -item.step), item);
        }
        const receipt = domain.set(item.key, next);
        if (receipt === null || receipt === undefined || receipt.applied !== true) {
            const code =
                receipt !== null && receipt !== undefined && typeof receipt.code === "string"
                    ? receipt.code
                    : "";
            return code === "no_appdata" ? "settings: 没有 appdata" : "settings: 只读";
        }
        return "";
    } catch (e) {
        return errorStatus(e);
    }
}

/**
 * 取一个配置域（本帧内缓存；`domain()` 每次会新建绑定对象 + 若干宿主调用）。
 *
 * @param {SettingsHost} h 宿主适配器。
 * @param {Array<{ id: string, dom: PageConfigDomain }>} cache 本帧缓存。
 * @param {string} id 域 id。
 * @returns {PageConfigDomain}
 */
function domainOf(h, cache, id) {
    for (const entry of cache) {
        if (entry.id === id) return entry.dom;
    }
    const config = h.config;
    if (config === undefined || config === null) throw new Error("no config");
    const dom = config.domain(id);
    cache.push({ id, dom });
    return dom;
}

/**
 * 读 bool（并集语义：boolean / 0-1 / `"0" "1" "true" "false"`；坏值 ⇒ `ok:false`）。
 *
 * @param {PageConfigDomain} domain 域对象。
 * @param {SchemaItem} item schema 项。
 * @returns {{ ok: boolean, value: boolean }}
 */
function readBool(domain, item) {
    const raw = domain.get(item.key, item.default);
    if (raw === undefined) return { ok: true, value: entryDefaultBool(item) };
    if (typeof raw === "boolean") return { ok: true, value: raw };
    if (typeof raw === "number") {
        if (raw === 0) return { ok: true, value: false };
        if (raw === 1) return { ok: true, value: true };
    }
    if (typeof raw === "string") {
        const text = raw.trim().toLowerCase();
        if (text === "1" || text === "true") return { ok: true, value: true };
        if (text === "0" || text === "false") return { ok: true, value: false };
    }
    return { ok: false, value: entryDefaultBool(item) };
}

/**
 * 读 number（有限数字才算合法；其它 ⇒ `ok:false` ⇒ 页面显示 `<invalid>`）。
 *
 * @param {PageConfigDomain} domain 域对象。
 * @param {SchemaItem} item schema 项。
 * @returns {{ ok: boolean, value: number }}
 */
function readNumber(domain, item) {
    const raw = domain.get(item.key, item.default);
    if (raw === undefined) return { ok: true, value: entryDefaultNumber(item) };
    if (typeof raw === "number" && Number.isFinite(raw)) return { ok: true, value: raw };
    return { ok: false, value: entryDefaultNumber(item) };
}

/** @param {SchemaItem} item @returns {boolean} */
function entryDefaultBool(item) {
    return typeof item.default === "boolean" ? item.default : false;
}

/** @param {SchemaItem} item @returns {number} */
function entryDefaultNumber(item) {
    return typeof item.default === "number" && Number.isFinite(item.default) ? item.default : 0;
}

/**
 * 按 schema 的 min/max 钳（⛔ 不做 step 对齐：step 只用于增减）。
 *
 * @param {number} value 候选值。
 * @param {SchemaItem} item schema 项。
 * @returns {number}
 */
function clampNumber(value, item) {
    let out = value;
    if (out < item.min) out = item.min;
    if (out > item.max) out = item.max;
    return out;
}

/** @param {number} value 数字值 @returns {string} */
function formatNumber(value) {
    return String(value);
}

/** @param {SchemaDomain} sd 域 @returns {string} */
function sectionTitle(sd) {
    // ★ owner 有值才加 `<owner> · <title>`；缺省保持旧 `<title>`（⛔ 不写 domain id，现有页面不变）。
    const base = sd.owner !== undefined && sd.owner !== "" ? sd.owner + " · " + sd.title : sd.title;
    return sd.live === "" ? base : base + "  [" + sd.live + "]";
}

/** @param {unknown} e 抛出来的东西 @returns {string} */
function errorStatus(e) {
    return "settings: " + messageOf(e);
}

/** @param {unknown} e 抛出来的东西 @returns {string} */
function messageOf(e) {
    const anyE = /** @type {any} */ (e);
    if (anyE !== null && anyE !== undefined && typeof anyE.message === "string") return anyE.message;
    return String(e);
}

/**
 * 把状态行写回调用方状态（没有 state / 形状不对就丢掉）。
 *
 * @param {SettingsPageState | undefined} state 调用方状态。
 * @param {string} status 新状态行。
 * @returns {void}
 */
function writeState(state, status) {
    if (state !== undefined && state !== null && typeof state === "object") state.status = status;
}
