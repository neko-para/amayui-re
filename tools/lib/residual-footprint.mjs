/**
 * tools/lib/residual-footprint.mjs —— **残差 × 足迹**的派生 join（纯函数；不写盘、不读游戏）
 *
 * 目的：回答「**某条残差会不会被这次里程碑（boot → TITLE → OPTION → SN0000）的输入触发**」。
 * 足迹文件是 `apps/emulator-cxx --dump-footprint <out.jsonl>` 的产物（生成物，不入库）：
 *
 *   {"schema":1,"kind":"meta", ...}            汇总（步数 / 各表条数 / 装载过的脚本）
 *   {"schema":1,"kind":"op","opcode":N,"count":C,"firstScript":S}     **动态**：真的派发过的 opcode
 *   {"schema":1,"kind":"gate","bit":B,"count":C}                      **动态**：门服务链服务过的位
 *   {"schema":1,"kind":"cell","addr":"0x…","read":R,"write":W}        **动态**：读写过的 VM 格地址
 *   {"schema":1,"kind":"writer","target":"bit:27","opcode":N,"dir":"set|clear","count":C}
 *   {"schema":1,"kind":"writer","target":"cell:0x…","opcode":N,"dir":"write","count":C}
 *                                                                     **写统计**：按（目标, opcode）归因
 *   {"schema":1,"kind":"static-op","opcode":N,"count":C}              **静态**：装载过的脚本里出现过的 opcode（上界）
 *
 * 三桶判据（与需求单 `REQ-01M6961WQTDWVMY0X4V6ZN9JR7` 的「判据」一节同一条）：
 * 1. **opcode 类**：触发者 = 规则 `covers.ops`，没有就取标题里的 `0x…`（只认 opcode 域 0..0x3FF）。
 *    动态命中 ⇒ `path`；否则静态命中 ⇒ `unknown`（数据依赖分支）；都不命中 ⇒ `offpath`。
 * 2. **位 / 格类**：从残差 `what` / `next` 里取位号 / 地址；动态 `gate` / `cell` 命中 ⇒ `path`。
 *    没命中时**写者闭包**（可注入）决定：闭包里没有任何**动态足迹内**的 opcode（含空集）⇒ `offpath`；
 *    闭包数据缺席 ⇒ `unknown`（"没取证"不是"路径外"）。
 * 3. **外部 / 资产类**（真机 / IDA / 语料外模块）⇒ `offpath`（不参与排序）。
 * 4. 其余（连触发者都没定位）⇒ `unknown`。
 *
 * ★ **路径外是派生分类**，不落库：⛔ 不改残差的 `kind`、⛔ 不新增 spec schema 字段。
 * ★ 本文件的边界：`cell` 只覆盖 VM 的池格；引擎对象字段（`shape:` 成员）没有插桩 ⇒ 那些残差是
 *   `unknown`，不是 `offpath`（见 `apps/emulator-cxx/src/core/footprint.h` 的同一句）。
 */

export const FOOTPRINT_SCHEMA = 1;
export const FOOTPRINT_KINDS = ['meta', 'op', 'gate', 'cell', 'writer', 'static-op'];
export const SCOPES = ['path', 'offpath', 'unknown'];
export const SCOPE_LABEL = { path: '路径内', offpath: '路径外', unknown: '判不了' };

/** opcode 的合法域（`STEP` 读到的 u32 只接受 0..0x3FF；见 vm.cpp）。 */
const OPCODE_MAX = 0x3ff;

const int = (value) => (Number.isInteger(value) && value >= 0 ? value : null);

function needInt(record, key, where) {
    const value = int(record[key]);
    if (value === null) throw new Error(`${where} 的 ${key} 必须是非负整数（实际 ${JSON.stringify(record[key])}）`);
    return value;
}

/**
 * 解析足迹 JSONL（逐行；空行跳过）。
 * @returns {{meta:object|null, ops:Map<number,object>, gates:Map<number,object>, cells:Map<number,object>,
 *            staticOps:Map<number,object>, writers:{bit:Map<number,Set<number>>, cell:Map<number,Set<number>>}}}
 */
export function parseFootprint(text, { file = '<footprint>' } = {}) {
    const out = {
        meta: null,
        ops: new Map(),
        gates: new Map(),
        cells: new Map(),
        staticOps: new Map(),
        // 写统计 → 写者闭包：位号 / 格地址 → 真的写过它的 opcode 集合（set / clear / write 合并）。
        writers: { bit: new Map(), cell: new Map() },
    };
    const lines = String(text).split(/\r?\n/);
    for (let i = 0; i < lines.length; i += 1) {
        const line = lines[i].trim();
        if (line === '') continue;
        let record;
        try {
            record = JSON.parse(line);
        } catch (err) {
            throw new Error(`${file}:${i + 1} 不是合法 JSON：${err.message}`);
        }
        const where = `${file}:${i + 1}`;
        if (record === null || typeof record !== 'object' || Array.isArray(record)) throw new Error(`${where} 必须是 JSON 对象`);
        if (record.schema !== FOOTPRINT_SCHEMA) throw new Error(`${where} 的 schema 必须是 ${FOOTPRINT_SCHEMA}（实际 ${JSON.stringify(record.schema)}）`);
        if (!FOOTPRINT_KINDS.includes(record.kind)) throw new Error(`${where} 的 kind 不认识：${JSON.stringify(record.kind)}`);
        if (record.kind === 'meta') {
            out.meta = record;
        } else if (record.kind === 'op') {
            out.ops.set(needInt(record, 'opcode', where), { count: needInt(record, 'count', where) });
        } else if (record.kind === 'static-op') {
            out.staticOps.set(needInt(record, 'opcode', where), { count: needInt(record, 'count', where) });
        } else if (record.kind === 'gate') {
            const bit = needInt(record, 'bit', where);
            if (bit > 31) throw new Error(`${where} 的 bit 超出 0..31：${bit}`);
            out.gates.set(bit, { count: needInt(record, 'count', where) });
        } else if (record.kind === 'cell') {
            if (typeof record.addr !== 'string' || !/^0x[0-9a-fA-F]+$/.test(record.addr)) {
                throw new Error(`${where} 的 addr 必须是 0x… 形态（实际 ${JSON.stringify(record.addr)}）`);
            }
            out.cells.set(Number.parseInt(record.addr.slice(2), 16), {
                read: needInt(record, 'read', where),
                write: needInt(record, 'write', where),
            });
        } else if (record.kind === 'writer') {
            // 两个写统计：bit:<0..31>（set / clear）或 cell:<0x…>（write），opcode = 执行这次写的那条指令。
            needInt(record, 'count', where);
            const opcode = needInt(record, 'opcode', where);
            if (typeof record.target !== 'string') throw new Error(`${where} 的 target 必须是字符串`);
            if (record.target.startsWith('bit:')) {
                const bit = Number.parseInt(record.target.slice(4), 10);
                if (!Number.isInteger(bit) || bit < 0 || bit > 31) {
                    throw new Error(`${where} 的 target 位号超出 0..31：${record.target}`);
                }
                if (record.dir !== 'set' && record.dir !== 'clear') {
                    throw new Error(`${where} 的 dir 必须是 set / clear（实际 ${JSON.stringify(record.dir)}）`);
                }
                if (!out.writers.bit.has(bit)) out.writers.bit.set(bit, new Set());
                out.writers.bit.get(bit).add(opcode);
            } else if (/^cell:0x[0-9a-fA-F]+$/.test(record.target)) {
                if (record.dir !== 'write') {
                    throw new Error(`${where} 的 cell writer dir 必须是 write（实际 ${JSON.stringify(record.dir)}）`);
                }
                const address = Number.parseInt(record.target.slice(5), 16);
                if (!out.writers.cell.has(address)) out.writers.cell.set(address, new Set());
                out.writers.cell.get(address).add(opcode);
            } else {
                throw new Error(`${where} 的 target 必须是 bit:<0..31> 或 cell:0x…（实际 ${JSON.stringify(record.target)}）`);
            }
        }
    }
    return out;
}

/** 足迹的元信息（给报告头用；不影响分类）。 */
export function footprintSummary(footprint) {
    const meta = footprint?.meta ?? {};
    return {
        steps: int(meta.steps) ?? 0,
        scripts: Array.isArray(meta.scripts) ? meta.scripts.length : 0,
        staticFailures: int(meta.staticFailures) ?? 0,
        ops: footprint?.ops?.size ?? 0,
        staticOps: footprint?.staticOps?.size ?? 0,
        gates: footprint?.gates?.size ?? 0,
        cells: footprint?.cells?.size ?? 0,
        // `writer` 记录的**条数**（(目标, opcode) 对数；meta 有就用 meta，旧文件退回目标数）。
        writerBits: int(meta.bitWriters) ?? (footprint?.writers?.bit?.size ?? 0),
        writerCells: int(meta.cellWriters) ?? (footprint?.writers?.cell?.size ?? 0),
    };
}

/**
 * 规则级的 opcode 触发者：`covers.ops` 优先；没有就取**标题**里的 `0x…`（只认 opcode 域）。
 * ★ 只从标题取，不从正文 / 残差措辞取 —— 那是"这条规则管哪些 opcode"的稳定声明。
 */
export function entryOpcodeTriggers(entry) {
    const out = new Set();
    for (const cover of entry?.covers ?? []) {
        for (const op of cover?.ops ?? []) {
            const value = typeof op === 'string' ? Number.parseInt(op.replace(/^0x/i, ''), 16) : op;
            if (Number.isInteger(value) && value >= 0 && value <= OPCODE_MAX) out.add(value);
        }
    }
    const title = String(entry?.title ?? '');
    for (const match of title.matchAll(/\b0x([0-9a-fA-F]+)\b/g)) {
        const value = Number.parseInt(match[1], 16);
        if (value >= 0 && value <= OPCODE_MAX) out.add(value);
    }
    return [...out].sort((a, b) => a - b);
}

/** 残差里点名的位号（`第 N 位` / `bit N` / `位号 N`；只认 0..31）。 */
export function residualBitTriggers(row) {
    const text = `${row?.what ?? ''}\n${row?.next ?? ''}`;
    const out = new Set();
    for (const re of [/第\s*(\d{1,2})\s*位/g, /\bbit\s*(\d{1,2})\b/gi, /位号\s*(\d{1,2})/g]) {
        for (const match of text.matchAll(re)) {
            const bit = Number.parseInt(match[1], 10);
            if (bit >= 0 && bit <= 31) out.add(bit);
        }
    }
    return [...out].sort((a, b) => a - b);
}

/**
 * 残差里点名的**格偏移**：只认带"格 / 对象字段"语境的写法（`+0x…` / `[0x…]` / `Engine + 0x…` /
 * `偏移 0x…` / `[77984h]`）。★ **裸 `0x…` 不算**：残差正文里的裸十六进制绝大多数是 **代码 EA**
 * 或 opcode（`0x49C8DA` / `0x192`），把它们当"格偏移"会让 `cell` 类虚高。
 */
export function residualCellTriggers(row) {
    const text = `${row?.what ?? ''}\n${row?.next ?? ''}`;
    const out = new Set();
    const patterns = [
        /(?:\+|\[|Engine\s*\+\s*|记录\s*\+\s*|偏移\s*)0x([0-9a-fA-F]+)/g,
        /(?:\+|\[)([0-9a-fA-F]{4,8})h\b/g,
    ];
    for (const re of patterns) {
        for (const match of text.matchAll(re)) out.add(Number.parseInt(match[1], 16));
    }
    return [...out].sort((a, b) => a - b);
}

/** 外部 / 资产类残差：主题不在引擎内（真机、IDA 导出、语料外模块）。只看 `what`（⛔ 不看 next 里的"真机对比"建议）。 */
export function isExternalResidual(row) {
    return /真机|IDA|语料外|外部(?:模块|资产|素材)|资产类/.test(String(row?.what ?? ''));
}

/**
 * 一条残差的三桶判定。
 * @param {object} row  `residualInventory()` 的行（`rule` / `what` / `next`）
 * @param {object} entry 对应规则（`covers` / `title`）
 * @param {ReturnType<typeof parseFootprint>} footprint
 * @param {{writers?: {bit?: Map<number, Iterable<number>>, cell?: Map<number, Iterable<number>>}}} [options]
 *        **写者闭包**（可选；`joinResiduals` 缺省用足迹文件里的 `writer` 记录）：
 *        位 / 地址 → 写它的 opcode 集合。没有这一层 ⇒ 位 / 格类残差记 `unknown`。
 * @returns {{scope:'path'|'offpath'|'unknown', by:string, triggers:(number|string)[]}}
 */
export function classifyResidual(row, entry, footprint, { writers = footprint?.writers } = {}) {
    const opTriggers = entryOpcodeTriggers(entry);
    if (opTriggers.length > 0) {
        const dynamic = opTriggers.filter((op) => footprint.ops.has(op));
        if (dynamic.length > 0) return { scope: 'path', by: 'op', triggers: dynamic };
        const statics = opTriggers.filter((op) => footprint.staticOps.has(op));
        if (statics.length > 0) return { scope: 'unknown', by: 'op-static', triggers: statics };
        return { scope: 'offpath', by: 'op', triggers: opTriggers };
    }

    const bits = residualBitTriggers(row);
    if (bits.length > 0) {
        const dynamic = bits.filter((bit) => footprint.gates.has(bit));
        if (dynamic.length > 0) return { scope: 'path', by: 'gate', triggers: dynamic };
        const verdict = writerClosureVerdict(bits, writers?.bit, footprint);
        if (verdict) return { ...verdict, by: 'gate-writer-closure', triggers: bits };
        return { scope: 'unknown', by: 'gate-no-writer-closure', triggers: bits };
    }

    const cells = residualCellTriggers(row);
    if (cells.length > 0) {
        const dynamic = cells.filter((at) => footprint.cells.has(at));
        if (dynamic.length > 0) return { scope: 'path', by: 'cell', triggers: dynamic.map((at) => `0x${at.toString(16)}`) };
        const verdict = writerClosureVerdict(cells, writers?.cell, footprint);
        if (verdict) return { ...verdict, by: 'cell-writer-closure', triggers: cells.map((at) => `0x${at.toString(16)}`) };
        return { scope: 'unknown', by: 'cell-no-writer-closure', triggers: cells.map((at) => `0x${at.toString(16)}`) };
    }

    if (isExternalResidual(row)) return { scope: 'offpath', by: 'external', triggers: [] };
    return { scope: 'unknown', by: 'no-trigger', triggers: [] };
}

/**
 * 写者闭包：**每个**触发者都有闭包数据才判；任一缺席 ⇒ `null`（判不了）。
 * 闭包里没有一个 opcode 在**动态**足迹里（含空集）⇒ `offpath`；否则 `path`。
 */
function writerClosureVerdict(targets, closures, footprint) {
    if (!closures || !(closures instanceof Map)) return null;
    let sawAny = false;
    for (const target of targets) {
        if (!closures.has(target)) return null;
        sawAny = true;
        const ops = [...closures.get(target)];
        if (ops.some((op) => footprint.ops.has(op))) return { scope: 'path' };
    }
    return sawAny ? { scope: 'offpath' } : null;
}

/**
 * 把残差行 join 成三桶。
 * @returns {{counts:{path:number,offpath:number,unknown:number}, rows:object[], byChannel:Map<string,{path:number,offpath:number,unknown:number}>, summary:object}}
 */
export function joinResiduals(rows, entries, footprint, options = {}) {
    const entryById = new Map((entries ?? []).map((entry) => [entry.id, entry]));
    // ★ 缺省消费足迹里的**写统计**（`writer` 记录）：写者执行过 ⇒ 路径内；没有记录 ⇒ 仍判不了。
    //   显式给 `options.writers` 时以调用方为准（给静态闭包留口）。
    const writers = options.writers ?? footprint.writers;
    const counts = { path: 0, offpath: 0, unknown: 0 };
    const byChannel = new Map();
    const out = [];
    for (const row of rows ?? []) {
        const verdict = classifyResidual(row, entryById.get(row.rule) ?? {}, footprint, { ...options, writers });
        counts[verdict.scope] += 1;
        if (!byChannel.has(row.channel)) byChannel.set(row.channel, { path: 0, offpath: 0, unknown: 0 });
        byChannel.get(row.channel)[verdict.scope] += 1;
        out.push({ ...row, ...verdict });
    }
    return { counts, rows: out, byChannel, summary: footprintSummary(footprint) };
}
