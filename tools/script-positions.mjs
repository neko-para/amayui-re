#!/usr/bin/env node
/**
 * tools/script-positions.mjs — **脚本内位置（`pos`）的锚表 → 生成物**的唯一入口
 *
 * 经派发器：`pnpm tools script-positions <describe|gen|check|explain> [args…]`
 * 也可独立跑：`node tools/script-positions.mjs gen`
 *
 * 它解决的那件事：`data/translations/patch.json` 的 `insert-after` / `delete` 会**移动**脚本内坐标
 * （每条 ±`4+argc*8` 字节），而插件消费方（`scenario-*.ts` / `lib/save.ts`）要在**补丁后坐标**上下门。
 * 译文只改字符串（字符串进池，指令长度只由 `argc` 定）⇒ 译文变长**不移动**坐标。
 *
 * 分工（⛔ 不要在这里再写一套 patch 逻辑）：
 *   · 补丁后视图 = `lib/patch.mjs` 的 `buildView('src', …)`（与 `pnpm tools patch view --kind src` 同一条路）；
 *   · 指令边界 = `packages/age-format` 的指令表（`4 + argc*8`），**不是**文本正则里另猜一套；
 *   · 本文件只做：**锚 → 命中 → pos**、渲染/比较生成物、打印推导。
 *
 * 三个落点：
 *   · `tools/script-positions.json` —— **锚表**（不透明数据；字段语义看 `--describe`，说明书 `tools/script-positions.md`）
 *   · `apps/emulator-lift/scripts/plugins/lib/script-positions.ts` —— **生成物**（`gen --write` 唯一写入口）
 *   · `data/translations/patch.json` —— **只读**输入（改它之后要重跑 `gen`）
 *
 * ★ 锚为什么是"基线指令窗口 + 基线行序"：`insert-after` / `delete` 会改掉之后所有字节偏移，`label_XXXXXXXX`
 *   本身就是绝对偏移 ⇒ 地址与 label 都是派生量。窗口把**指令序列**当锚，字符串一律屏蔽成 `"?"`（译文改的是
 *   字符串）、label 引用屏蔽成 `label_?`（地址会随插入漂）⇒ 锚里没有任何绝对地址，译文长了也不响。
 * ★ 锚歧义（同一窗口命中多次）**拒绝生成并点名**，⛔ 不"取第一个" —— 那正是静默失效的形状。
 */
import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

import {
    OPCODE_TABLE,
    instructionByteLength,
    instructionForToken,
} from '@amayui/age-format/src/asm/index.mts';

import {
    DEFAULT_PATCH,
    REPO_ROOT,
    buildView,
    disassemble,
    loadPatch,
    mapperContext,
    openSides,
    rowsOf,
} from './lib/patch.mjs';

/** `pos` 的坐标系原点：脚本头长度（`kScriptHeaderLen`，见 `engine_marker.h`） */
export const SCRIPT_HEADER_LEN = 0x3c;

/** 锚表（真源） */
export const POSITIONS_JSON = path.join(REPO_ROOT, 'tools', 'script-positions.json');
/** 生成物（消费方 import 的那份；⛔ 别手改） */
export const POSITIONS_TS = path.join(
    REPO_ROOT,
    'apps',
    'emulator-lift',
    'scripts',
    'plugins',
    'lib',
    'script-positions.ts',
);

/** 工具层自我声明：我动哪片数据、有哪些操作（`tools/cli.mjs` 的域地图从它派生） */
export const DOMAIN = {
    id: 'script-positions',
    title: '脚本内位置（pos）的锚表 → 生成物（`lib/script-positions.ts`）',
    data: [
        '`tools/script-positions.json`（**锚表 = 唯一真源**：基线指令窗口 + 基线行序 + 基线下 `pos`）',
        '`apps/emulator-lift/scripts/plugins/lib/script-positions.ts`（**生成物**；⛔ 别手改，`gen --write` 重写）',
        '输入（只读）：`data/translations/patch.json` + 基线安装树（清单 `roots.gameInstall`）' +
            ' ⇒ 补丁后视图由 `lib/patch.mjs` 的 `buildView(\'src\')` 现算（与 `pnpm tools patch view --kind src` 同一条路）',
    ],
    access: 'rw（唯一写入口 = `gen --write` 重写生成物；锚表本身是人工编辑的文本）',
    tool: 'tools/script-positions.mjs',
};

export const OPERATIONS = [
    {
        name: 'describe',
        argv: ['--describe'],
        mutates: false,
        summary: '自描述：锚字段 / 不变量（含谁在守）/ 操作 / 生成物路径（schema 的唯一真源；文档不复述）',
    },
    {
        name: 'gen',
        argv: ['--gen'],
        mutates: true,
        summary:
            '按锚表 + 补丁后视图重算每个常量 ⇒ 整份重写生成物 ts（缺省 dry-run，`--write` 才落盘；' +
            '任一锚命中 ≠ 1 次 ⇒ 拒绝写并点名）`[--write] [--patch <文件>] [--js <文件>] [--model <文件>]`',
    },
    {
        name: 'check',
        argv: ['--check'],
        mutates: false,
        summary:
            '① 每个锚在补丁后视图里**恰好命中 1 次** ② `gen` 的结果与磁盘 ts **逐字**比较（不写盘）；' +
            '不一致 exit 1 并点名常量（红 = 忘了 `gen` / patch 动了坐标 / 锚歧义）`[--patch <文件>] [--js <文件>] [--model <文件>]`',
    },
    {
        name: 'explain',
        argv: ['--explain'],
        mutates: false,
        summary:
            '人审一条：锚 → 基线窗口 → 命中的补丁后指令行 → 算出的 `pos`（含平移来源 op）`<NAME> [--patch <文件>] [--model <文件>]`',
    },
];

const HELP = `tools/script-positions.mjs —— 脚本内位置的锚表 → 生成物

  node tools/script-positions.mjs describe                     自描述：字段 / 不变量 / 操作（schema 真源）
  node tools/script-positions.mjs gen [--write]                按锚表重算并重写生成物 ts（缺省 dry-run）
  node tools/script-positions.mjs check                        gen 的结果 vs 磁盘 ts + 锚唯一性（不写盘）
  node tools/script-positions.mjs explain <NAME>               人审一条：锚 → 命中行 → pos

公共选项（诊断用；缺省就是仓里的真源）
  --patch <文件>    换一份 patch（例如 .tmp 里的临时视图）—— 只读，不落 data/
  --js    <文件>    换一份生成物（.ts；例如把 gen 的结果写到 .tmp 再 check）
  --model <文件>    换一份锚表（诊断用；缺省 tools/script-positions.json）

★ 坐标只有在 insert-after / delete 时移动（±4+argc*8）；译文只改字符串，不移动坐标。
★ patch.json 改完要跑 \"node tools/script-positions.mjs gen --write\"；check 红 = 忘了 gen。
`;

// ─────────────────────────────────────────────────────────── 行空间与掩码

/** label 引用（8 位十六进制地址）；高 4 位为 0 时反汇编器输出 `0x` + 4 位（`labelHex` 定长 8） */
const LABEL_REF = /label_[0-9a-f]{8}/g;

/**
 * 一行的**锚形态**：字符串 → `"?"`（译文只改这里）、label 引用 → `label_?`（地址随插入漂）。
 * 其余（助记符 / 操作数类型 / 立即数）原样 —— 那些才是"这条指令是什么"。
 * @param {string} line `rowsOf()` 的行（已去注释 / 已 trim）
 */
export function maskRow(line) {
    return line.replace(/"(?:[^"\\]|\\.)*"/g, '"?"').replace(LABEL_REF, 'label_?');
}

/** 指令 token（助记符或 `iXXX`） */
const tokenOf = (row) => row.split(/\s+/, 1)[0];

const lengthCache = new Map();
/**
 * 一条指令占多少字节 = `4 + argc*8`（`argc` 只由指令表定；⛔ 不按字符串长度猜）。
 * @param {string} row `rowsOf()` 的原始行
 */
export function instructionLength(row) {
    const token = tokenOf(row);
    let len = lengthCache.get(token);
    if (len === undefined) {
        const def = instructionForToken(OPCODE_TABLE, token);
        if (!def) throw new Error(`指令表里没有 \`${token}\`（行：${row}）—— 指令集换了？`);
        len = instructionByteLength(def);
        lengthCache.set(token, len);
    }
    return len;
}

/**
 * 把一段反汇编文本变成"指令网格"：`rows`（原文）/ `masked`（锚形态）/ `addr`（每条指令的绝对偏移）/ `len`。
 * 地址从 `SCRIPT_HEADER_LEN` 起按指令长度累加 ⇒ 与 `label_%08x` 同一个坐标系。
 * @param {string} text `disassemble()` 的输出（或 `buildView('src', …).text`）
 * @param {number} [start] 首条指令的绝对偏移（缺省 `0x3C`）
 */
export function gridOf(text, start = SCRIPT_HEADER_LEN) {
    const R = rowsOf(text);
    const len = R.raw.map((row) => instructionLength(row));
    const addr = [start];
    for (const n of len) addr.push(addr[addr.length - 1] + n);
    return { rows: R.raw, masked: R.raw.map(maskRow), len, addr, text };
}

/**
 * 补丁后视图快照：按脚本惰性构建（同一进程里 gen/check/explain 共用）。
 * ★ 只读：`buildView` 全程内存，不落盘；与 `pnpm tools patch view --kind src` 走同一条路。
 * @param {{patchPath?:string, headerLen?:number}} [opts]
 */
export function loadSnapshot(opts = {}) {
    const patchPath = opts.patchPath ?? DEFAULT_PATCH;
    const headerLen = opts.headerLen ?? SCRIPT_HEADER_LEN;
    const doc = loadPatch(patchPath);
    const { mapper } = mapperContext();
    const sides = openSides({ targetDir: null });
    const cache = new Map();

    /** @param {string} script */
    const scriptViews = (script) => {
        if (cache.has(script)) return cache.get(script);
        const hit = sides.base.resolve(script);
        if (!hit) throw new Error(`基线里找不到脚本 \`${script}\`（清单 roots.gameInstall 指对了吗？）`);
        const entry = doc.scripts?.[script];
        const baseText = disassemble(hit.buf);
        const viewText = entry
            ? buildView('src', hit.buf, entry, { lineToBin: mapper.lineToBin }).text
            : baseText;
        const value = {
            script,
            entry,
            baseBuf: hit.buf,
            baseGrid: gridOf(baseText, headerLen),
            viewGrid: gridOf(viewText, headerLen),
        };
        cache.set(script, value);
        return value;
    };

    return { patchPath, doc, sides, scriptViews };
}

// ─────────────────────────────────────────────────────────── 锚表（不透明数据）

const NAME_RE = /^[A-Za-z_$][\w$]*$/;
const SCRIPT_RE = /\.BIN$/;

/**
 * 读锚表并做**结构**校验（不碰 BIN）。字段语义见 `describe()`。
 * @param {string} [jsonPath]
 */
export function loadModel(jsonPath = POSITIONS_JSON) {
    let model;
    try {
        model = JSON.parse(fs.readFileSync(jsonPath, 'utf8'));
    } catch (err) {
        throw new Error(`读不了锚表 ${jsonPath}：${err.message}`);
    }
    const bad = [];
    if (model.schemaVersion !== 1) bad.push(`schemaVersion 必须是 1（实际 ${JSON.stringify(model.schemaVersion)}）`);
    if (!Number.isInteger(model.scriptHeaderLen) || model.scriptHeaderLen !== SCRIPT_HEADER_LEN) {
        bad.push(`scriptHeaderLen 必须是 ${SCRIPT_HEADER_LEN}（0x3c）`);
    }
    if (!Array.isArray(model.positions) || model.positions.length === 0) bad.push('positions 必须是非空数组');
    if (!Array.isArray(model.layout) || model.layout.length === 0) bad.push('layout 必须是非空数组');

    const names = new Set();
    for (const [k, p] of (model.positions ?? []).entries()) {
        const where = `positions[${k}]`;
        if (typeof p?.name !== 'string' || !NAME_RE.test(p.name)) bad.push(`${where}.name 不是合法标识符`);
        else if (names.has(p.name)) bad.push(`${where}.name 重复：${p.name}`);
        else names.add(p.name);
        if (typeof p?.script !== 'string' || !SCRIPT_RE.test(p.script)) bad.push(`${where}.script 必须是 *.BIN`);
        if (!Number.isInteger(p?.baselineIndex) || p.baselineIndex < 0) bad.push(`${where}.baselineIndex 必须是非负整数`);
        if (!Number.isInteger(p?.at) || p.at < 0) bad.push(`${where}.at 必须是非负整数`);
        if (p?.to !== 'start' && p?.to !== 'end') bad.push(`${where}.to 只能是 "start" / "end"`);
        if (!Array.isArray(p?.window) || p.window.length < MIN_WINDOW) {
            bad.push(`${where}.window 必须是长度 ≥ ${MIN_WINDOW} 的字符串数组（太窄的锚不可靠）`);
        } else if (p.window.some((row) => typeof row !== 'string' || row === '')) bad.push(`${where}.window 里有非字符串 / 空行`);
        if (!Number.isInteger(p?.pos) || p.pos < 0) bad.push(`${where}.pos 必须是非负整数`);
        if (p?.note !== undefined) {
            if (!Array.isArray(p.note) || p.note.some((l) => typeof l !== 'string' || !l.startsWith('//'))) {
                bad.push(`${where}.note 必须是"以 // 开头的整行注释"数组（逐字搬进生成物）`);
            }
        }
        if (p?.width !== undefined && (!Number.isInteger(p.width) || p.width < 3 || p.width > 8)) {
            bad.push(`${where}.width 必须是 3..8 的整数（生成物字面量的十六进制宽度；缺省 4）`);
        }
    }

    const inLayout = new Map();
    for (const [k, block] of (model.layout ?? []).entries()) {
        const where = `layout[${k}]`;
        if (block?.kind === 'section') {
            if (!Array.isArray(block.banner) || block.banner.length === 0 || block.banner.some((l) => typeof l !== 'string' || !l.startsWith('//'))) {
                bad.push(`${where}.banner 必须是非空的整行注释数组`);
            }
        } else if (block?.kind === 'position') {
            if (typeof block.name !== 'string') bad.push(`${where}.name 必须是字符串`);
            else if (inLayout.has(block.name)) bad.push(`${where}.name 在 layout 里重复：${block.name}`);
            else inLayout.set(block.name, true);
        } else {
            bad.push(`${where}.kind 只能是 "section" / "position"`);
        }
    }
    for (const name of names) if (!inLayout.has(name)) bad.push(`position \`${name}\` 没有出现在 layout 里（生成物会少一条）`);
    for (const name of inLayout.keys()) if (!names.has(name)) bad.push(`layout 引用了不存在的 position \`${name}\``);

    if (bad.length) throw new Error(`锚表结构不合法（${jsonPath}）：\n  - ${bad.join('\n  - ')}`);
    return model;
}

/** 锚窗口最小宽度：1–2 行的锚太容易在别处重复命中，允许写但**当场拒绝**（要求加宽） */
const MIN_WINDOW = 3;

// ─────────────────────────────────────────────────────────── 命中与 pos

/**
 * 在补丁后视图里找锚：窗口必须**恰好命中一次**。
 * @returns {{hits:number[], baseGrid:object, viewGrid:object, window:string[]}}
 */
export function locate(position, snapshot) {
    const view = snapshot.scriptViews(position.script);
    const W = position.window.length;
    const start = position.baselineIndex;
    const baseWindow = view.baseGrid.masked.slice(start, start + W);
    if (baseWindow.length !== W) {
        throw new Error(
            `${position.name}: baselineIndex=${start} 起凑不出 ${W} 条指令（基线只有 ${view.baseGrid.masked.length} 条）`,
        );
    }
    const mismatch = baseWindow.findIndex((row, k) => row !== position.window[k]);
    if (mismatch >= 0) {
        throw new Error(
            `${position.name}: 锚窗口与基线不一致（基线变了，还是锚表手改了？）\n` +
                `  第 ${mismatch} 条  锚表：${position.window[mismatch]}\n` +
                `              基线：${baseWindow[mismatch]}`,
        );
    }
    const rows = view.viewGrid.masked;
    const hits = [];
    for (let j = 0; j + W <= rows.length; j += 1) {
        let ok = true;
        for (let t = 0; t < W; t += 1) {
            if (rows[j + t] !== position.window[t]) {
                ok = false;
                break;
            }
        }
        if (ok) hits.push(j);
    }
    return { hits, baseGrid: view.baseGrid, viewGrid: view.viewGrid, window: position.window };
}

/** 从命中位置算 `pos`（`at` = 窗口里第几条指令，`to` = 该指令的起点 / 终点） */
export function posFromHit(position, located) {
    const idx = located.hits[0] + position.at;
    const { addr, len } = located.viewGrid;
    if (idx >= addr.length - 1) throw new Error(`${position.name}: at=${position.at} 越过命中窗口之后最后一条指令`);
    const base = addr[idx];
    return position.to === 'end' ? base + len[idx] : base;
}

/** 基线坐标下的 `pos`（锚表里记录的 `pos` 就是它；与补丁无关） */
export function baselinePosOf(position, baseGrid) {
    const idx = position.baselineIndex + position.at;
    if (idx >= baseGrid.addr.length - 1) throw new Error(`${position.name}: baselineIndex/at 越过基线指令数`);
    return position.to === 'end' ? baseGrid.addr[idx] + baseGrid.len[idx] : baseGrid.addr[idx];
}

/**
 * 校验"命中的补丁后地址"能被**已知 op 的平移**解释（防"窗口在别处恰好命中一次"的静默错位）。
 *
 * 三类 op 各自的影响：
 *   · `insert-after i`（i 在锚指令**之前**）⇒ +`4+argc*8`；
 *   · `delete i`（i 在锚指令之前）⇒ −该行原长；
 *   · `replace-line i`（i 在锚之前，或 i = 锚指令且取终点）⇒ +（新长 − 原长）。
 * ★ 若窗口里出现 insert / delete，锚序列已经变了 ⇒ 直接判失效（要求重锚），不猜。
 * @param {object} position 锚表条目
 * @param {object} located `locate()` 的结果
 * @param {object[]} ops 该脚本的 patch ops
 */
export function assertShiftExplained(position, located, ops) {
    const idx = position.baselineIndex + position.at;
    const start = position.baselineIndex;
    const W = position.window.length;
    for (const op of ops) {
        if (op.op === 'delete' && op.i >= start && op.i < start + W) {
            throw new Error(`${position.name}: 锚窗口里有 delete（i=${op.i}）—— 锚已失效，需对着补丁后视图重锚`);
        }
        if (op.op === 'insert-after' && op.i >= start && op.i < start + W - 1) {
            throw new Error(`${position.name}: 锚窗口里有 insert-after（i=${op.i}）—— 锚已失效，需重锚`);
        }
    }
    let expected = located.baseGrid.addr[idx];
    if (position.to === 'end') expected += located.baseGrid.len[idx];
    for (const op of ops) {
        if (op.op === 'insert-after') {
            if (op.i < idx) expected += instructionLength(op.instr);
        } else if (op.op === 'delete') {
            if (op.i < idx) expected -= located.baseGrid.len[op.i];
        } else if (op.op === 'replace-line') {
            const delta = instructionLength(op.line) - located.baseGrid.len[op.i];
            if (op.i < idx) expected += delta;
            else if (op.i === idx && position.to === 'end') expected += delta;
        }
    }
    const computed = posFromHit(position, located);
    if (computed !== expected) {
        throw new Error(
            `${position.name}: 命中的地址与 op 平移不符（窗口算出 ${hexOf(computed)}，按锚 + op 应为 ${hexOf(expected)}）` +
                ' —— 命中的可能不是同一条指令；加宽 window 或检查 baselineIndex',
        );
    }
}

/**
 * 逐个锚算 `pos`；**任一锚命中 ≠ 1 次就抛**（点名 + 候选地址 + 建议加宽）。
 * @returns {{values:Map<string,number>, located:Map<string,object>, model:object, snapshot:object}}
 */
export function computePositions(model, snapshot) {
    const problems = [];
    const values = new Map();
    const located = new Map();
    for (const position of model.positions) {
        let hit;
        try {
            hit = locate(position, snapshot);
        } catch (err) {
            problems.push(err.message);
            continue;
        }
        if (hit.hits.length !== 1) {
            const candidates = hit.hits.length
                ? hit.hits.map((j) => `0x${hit.viewGrid.addr[j].toString(16)}`).join(', ')
                : '（无）';
            problems.push(
                `${position.name}: 锚在补丁后视图里命中 ${hit.hits.length} 次（要求恰好 1 次）—— ` +
                    `候选 pos：${candidates}。把 window 加宽（基线行序 ${position.baselineIndex} 起）再试`,
            );
            continue;
        }
        located.set(position.name, hit);
        try {
            const ops = snapshot.scriptViews(position.script).entry?.ops ?? [];
            assertShiftExplained(position, hit, ops);
            values.set(position.name, posFromHit(position, hit));
        } catch (err) {
            problems.push(err.message);
        }
    }
    if (problems.length) throw new Error(`锚有问题（不写盘）：\n  - ${problems.join('\n  - ')}`);
    return { values, located, model, snapshot };
}

// ─────────────────────────────────────────────────────────── 生成物

/**
 * 生成物的**文件头**（工具拥有；body 的注释逐字来自锚表 `note` / `layout[].banner`）。
 * ★ 生成物里数值的写法（`0x` + 小写、至少 4 位）由本工具的 `hexOf()` 唯一决定。
 */
const HEADER = [
    '// scripts/plugins/lib/script-positions.ts —— **生成物**（由 tools/script-positions.mjs 生成，⛔ 别手改）',
    '//',
    '// 真源 = `tools/script-positions.json`（**锚表**：基线指令窗口 + 基线行序 ⇒ 补丁后 `pos`）。',
    '// 怎么查：`pnpm tools script-positions explain <NAME>`（锚 → 命中的补丁后指令 → `pos`）。',
    '// 怎么改：改锚表（或改 `data/translations/patch.json` 之后）跑',
    '//         `pnpm tools script-positions gen --write`；`pnpm tools script-positions check` 红 = 忘了 gen。',
    '// 字段语义 / 不变量：`pnpm tools script-positions describe`；说明书：`tools/script-positions.md`。',
    '//',
    '// 口径（下面每条数值都由"锚 + 补丁后视图"现算，⛔ 不要在这个文件里手改）：',
    '//   1. `pos` = **脚本体内绝对字节偏移**，与 `dist/views/data/<NAME>.BIN.txt` 的 `label_%08x` 同坐标系',
    '//      （`pos = 0x3C + Σ(4+argc*8)`；推导与实测见 `docs/03-emulator/input-plan.md` §2.6 / §2.8）。',
    '//   2. 命名：`<SCRIPT>_<用途>_LO` / `_HI`（**半开区间** `[LO,HI)`）或 `<SCRIPT>_<用途>_POS`（单点）。',
    '//      `<SCRIPT>` = 宿主 marker 的**裸名**（`TITLE` / `SBUNKI` / …）—— 这是位置表的命名，**不是** trigger 的',
    '//      `script` 参数：C1 的门是精确 `==`，那边必须写带 `.BIN` 的宿主拼写（见 `save.ts` 头）。',
    '//   3. ⛔ **别处不许再裸写这些字面量**。`scripts/plugins/lib/*.ts`（除本文件与 `guest-globals.ts`）里出现',
    '//      `0x…` 字面量 ⇒ `tools/test/script-map.test.mjs` 红（它**剥注释**后扫，所以头注释里印区间是允许的）。',
    '//      新增一条位置的正确做法：把锚加进 `tools/script-positions.json`，再 `gen --write`（消费方 `import` 名字）。',
    '//',
    '// 收录判据：**有读数支撑**（门命中 / 采样读数 / 清单行号旁证三者之一），且**当前有消费方或作负对照**。',
    '//   · 出处 / 重复定义 / 名字映射 / `<待核>` 全在 `.tmp/script-map/provenance.md`（那只是取证台账，不参与运行）。',
    '//   · 本文件**不收**：`<待核>` 且当前没有任何 lib 消费方的条目 —— `SBUNKI_CLICK_OLD`（0x09F8 旧窗口）、',
    '//     `SC0000_LOAD_STOP`、`SC4000_FIRST/SECOND/TEXT/TEXT2/TEXT3/TEXT4`、`BUNKI_DIALOG`。',
    '//     它们只被 `.plan` 当字面量用（不入 JS），清单与「为什么没收」见 provenance §4 第 1 / 5 条。',
    '//   · 坐标（`1070,480` / `300` / `640,323` / `690,323` / `1220,45` / `640,517` …）**不是** `pos`，',
    '//     ⛔ 不要塞进本文件（它们属于 1280×720 参考系；台账见 provenance §6）。',
    '//   · **SN0000.BIN 的 10 个常量已移出本表**（8/10 不在补丁后指令网格上）—— 重锚之后再回来，见',
    '//     `tools/script-positions.md` 的「未做 / 待核」。',
    '//',
    '// 坐标系的**唯一例外**：`SC0000` / `SC4000` / `BUNKI` 的位置值绑**安装修订版**（`links/raw` 与',
    '// `links/install` 的清单差 `+0x20` / `−0x1C`，见 `docs/03-emulator/scripts/SC0000.md`）⇒ 换安装树会',
    '// **静默不命中**。本文件目前不含这三个脚本（理由见上）。',
];

/** 生成物里数值的写法：`0x` + 小写十六进制（宽度缺省 4；历史写法不一致的条目在锚表里带 `width`） */
export const hexOf = (n, width = 4) => `0x${n.toString(16).padStart(width, '0')}`;

/**
 * 渲染整份生成物。`layout` 决定顺序与段落注释；数值只来自 `values`（锚 + 补丁后视图现算）。
 * @param {object} model 锚表
 * @param {Map<string, number>} values name → pos
 */
export function renderTs(model, values) {
    const byName = new Map(model.positions.map((p) => [p.name, p]));
    const lines = [...HEADER, ''];
    for (const block of model.layout) {
        if (block.kind === 'section') {
            if (lines[lines.length - 1] !== '') lines.push('');
            lines.push(...block.banner, '');
            continue;
        }
        const position = byName.get(block.name);
        if (!position) throw new Error(`layout 引用了不存在的 position：${block.name}`);
        if (!values.has(position.name)) throw new Error(`没有算出 ${position.name} 的 pos`);
        if (position.note?.length) {
            if (lines[lines.length - 1] !== '') lines.push('');
            lines.push(...position.note);
        }
        lines.push(`export const ${position.name} = ${hexOf(values.get(position.name), position.width ?? 4)};`);
    }
    return `${lines.join('\n')}\n`;
}

/** 从生成物文本里抠出 `name → 字面量`（用于点名叫哪条不一致） */
export function parseExportValues(text) {
    const out = new Map();
    const re = /^export const ([A-Za-z_$][\w$]*) = (0x[0-9a-fA-F]+);$/gm;
    let m;
    while ((m = re.exec(text)) !== null) out.set(m[1], m[2]);
    return out;
}

// ─────────────────────────────────────────────────────────── 动作

function writeOut(stream, text) {
    stream.write(text.endsWith('\n') ? text : `${text}\n`);
}

/** 生成物的落点：`--js` 覆盖 > 锚表里的 `js` > 常量缺省 */
function targetPath(model, args) {
    if (args.jsPath) return args.jsPath;
    return model.js ? path.resolve(REPO_ROOT, model.js) : POSITIONS_TS;
}

/** gen：算 → 渲染 →（`--write`）写盘 */
function actionGen(args) {
    const model = loadModel(args.modelPath);
    const snapshot = loadSnapshot({ patchPath: args.patchPath, headerLen: model.scriptHeaderLen });
    let values;
    try {
        ({ values } = computePositions(model, snapshot));
    } catch (err) {
        writeOut(process.stderr, `gen 拒绝写：${err.message}`);
        return 1;
    }
    const text = renderTs(model, values);
    const target = targetPath(model, args);
    const prev = fs.existsSync(target) ? fs.readFileSync(target, 'utf8') : null;
    const changed = prev !== text;

    if (!args.write) {
        writeOut(process.stdout, `gen（dry-run）：${values.size} 个常量已算出；${changed ? '与磁盘**不同**' : '与磁盘逐字相同'}。`);
        if (changed) writeOut(process.stdout, '  加 --write 才落盘（唯一写入口）。');
        return 0;
    }
    if (changed) {
        fs.writeFileSync(target, text);
        writeOut(process.stdout, `gen：写入 ${values.size} 个常量 ⇒ ${path.relative(REPO_ROOT, target)}`);
    } else {
        writeOut(process.stdout, `gen：${values.size} 个常量与磁盘逐字相同，未改动 ${path.relative(REPO_ROOT, target)}`);
    }
    return 0;
}

/** check：锚唯一性 + `gen` 结果 vs 磁盘 ts（逐字）；不写盘 */
function actionCheck(args) {
    const model = loadModel(args.modelPath);
    const snapshot = loadSnapshot({ patchPath: args.patchPath, headerLen: model.scriptHeaderLen });
    const problems = [];
    let computed;
    try {
        computed = computePositions(model, snapshot);
    } catch (err) {
        writeOut(process.stderr, `check 红：${err.message}`);
        return 1;
    }

    // ① 锚表里记录的 `pos` 必须等于**基线**网格上的值（防手误；它不随 patch 漂）
    for (const position of model.positions) {
        const baseGrid = snapshot.scriptViews(position.script).baseGrid;
        const expected = baselinePosOf(position, baseGrid);
        if (expected !== position.pos) {
            problems.push(
                `${position.name}: 锚表 pos=${hexOf(position.pos, position.width ?? 4)} 与基线网格算出的 ` +
                    `${hexOf(expected, position.width ?? 4)} 不一致` +
                    `（baselineIndex=${position.baselineIndex} at=${position.at} to=${position.to}）`,
            );
        }
    }

    // ② 生成结果 vs 磁盘 ts（逐字）
    const text = renderTs(model, computed.values);
    const target = targetPath(model, args);
    const actual = fs.existsSync(target) ? fs.readFileSync(target, 'utf8') : null;
    if (actual === null) {
        problems.push(`生成物不存在：${path.relative(REPO_ROOT, target)}（先跑 gen --write）`);
    } else if (actual !== text) {
        const want = parseExportValues(text);
        const have = parseExportValues(actual);
        for (const [name, value] of want) {
            if (!have.has(name)) problems.push(`${name}: 生成物里没有这个导出（期望 ${value}）`);
            else if (have.get(name) !== value) problems.push(`${name}: 期望 ${value} / 生成物 ${have.get(name)}`);
        }
        for (const name of have.keys()) if (!want.has(name)) problems.push(`${name}: 锚表里没有，但生成物里有`);
        if (problems.length === 0) problems.push('生成物与生成结果逐字不同（只有注释 / 文件头 / 空白差异 —— 别手改它）');
    }

    if (problems.length) {
        writeOut(process.stderr, `check 红（${problems.length} 处）：\n  - ${problems.join('\n  - ')}`);
        return 1;
    }
    const scriptCount = new Set(model.positions.map((p) => p.script)).size;
    writeOut(
        process.stdout,
        `check 绿：${model.positions.length} 个常量（${scriptCount} 支脚本）与生成物逐字一致；` +
            `${model.positions.length} 个锚在补丁后视图里各命中 1 次；锚表 pos 与基线网格一致。`,
    );
    return 0;
}

/** explain：一条锚的完整推导（人审用） */
function actionExplain(args) {
    const name = args.name;
    if (!name) {
        writeOut(process.stderr, 'explain 需要常量名：node tools/script-positions.mjs explain <NAME>');
        return 2;
    }
    const model = loadModel(args.modelPath);
    const position = model.positions.find((p) => p.name === name);
    if (!position) {
        writeOut(process.stderr, `锚表里没有 \`${name}\`。可用：${model.positions.map((p) => p.name).join(' / ')}`);
        return 2;
    }
    const snapshot = loadSnapshot({ patchPath: args.patchPath, headerLen: model.scriptHeaderLen });
    let located;
    try {
        located = locate(position, snapshot);
    } catch (err) {
        writeOut(process.stderr, `explain 红：${err.message}`);
        return 1;
    }
    const W = position.window.length;
    const L = [];
    L.push(`${name}  ←  ${position.script}`);
    L.push('');
    L.push(`锚：基线指令 [${position.baselineIndex}, ${position.baselineIndex + W})，at=${position.at}，to=${position.to}`);
    L.push('  基线窗口（字符串已屏蔽成 "?"、label → label_?）：');
    for (let k = 0; k < W; k += 1) {
        L.push(`    ${String(position.baselineIndex + k).padStart(6)}  0x${located.baseGrid.addr[position.baselineIndex + k].toString(16)}  ${position.window[k]}`);
    }
    if (located.hits.length !== 1) {
        L.push('');
        L.push(`✖ 补丁后视图里命中 ${located.hits.length} 次（要求恰好 1 次）：`);
        for (const j of located.hits) L.push(`    指令 [${j}, ${j + W})  0x${located.viewGrid.addr[j].toString(16)}`);
        L.push('  ⇒ 把 window 加宽（基线行序 ' + position.baselineIndex + ' 起），⛔ 不要"取第一个"。');
        writeOut(process.stderr, L.join('\n'));
        return 1;
    }
    const j = located.hits[0];
    const idx = j + position.at;
    const pos = posFromHit(position, located);
    L.push('');
    L.push(`命中：补丁后指令 [${j}, ${j + W})，恰好 1 次`);
    for (let k = 0; k < W; k += 1) {
        const mark = k === position.at ? '→' : ' ';
        L.push(`  ${mark} ${String(j + k).padStart(6)}  0x${located.viewGrid.addr[j + k].toString(16)}  ${located.viewGrid.rows[j + k]}`);
    }
    L.push('');
    L.push(
        `计算：pos = instr[${idx}].addr(0x${located.viewGrid.addr[idx].toString(16)})` +
            ` ${position.to === 'end' ? `+ len(0x${located.viewGrid.len[idx].toString(16)})` : '+ 0（起点）'}` +
            ` = ${hexOf(pos, position.width ?? 4)}（十进制 ${pos}）`,
    );
    L.push(`锚表记录的 pos（基线坐标）：${hexOf(position.pos, position.width ?? 4)}（十进制 ${position.pos}）`);

    // 平移来源：insert-after / delete 只影响"它之前"的指令
    const ops = snapshot.scriptViews(position.script).entry?.ops ?? [];
    const shifts = [];
    for (const op of ops) {
        if (op.op === 'insert-after' && op.i < position.baselineIndex) {
            shifts.push(`insert-after i=${op.i}（+${instructionLength(op.instr)} B）`);
        } else if (op.op === 'delete' && op.i < position.baselineIndex) {
            shifts.push(`delete i=${op.i}（−${located.baseGrid.len[op.i]} B）`);
        }
    }
    const delta = located.viewGrid.addr[j] - located.baseGrid.addr[position.baselineIndex];
    L.push('');
    L.push(`平移：${delta >= 0 ? '+' : ''}${delta} B（补丁后 − 基线）`);
    if (shifts.length) for (const s of shifts) L.push(`  来源：${s}`);
    else if (delta !== 0) L.push('  ⚠ 没有 insert/delete 落在锚之前，但地址不等 —— 窗口命中了别处？先查锚歧义');
    writeOut(process.stdout, L.join('\n'));
    return 0;
}

// ─────────────────────────────────────────────────────────── 自描述

export function describe() {
    return {
        file: 'tools/script-positions.json',
        purpose:
            '**脚本内位置（`pos`）的锚表 = 唯一真源**：把 `<SCRIPT>` 内的一段指令序列（锚）绑到插件消费方用的常量名上，' +
            '`gen` 再按**补丁后视图**现算每个常量。`pos` 是补丁后坐标（插件运行时读的是汉化 install 的 BIN）。',
        notHere:
            '译文不在这里（`data/translations/patch.json`）；补丁后视图也不在这里（由基线 BIN + patch 现算）。' +
            '生成物 `apps/emulator-lift/scripts/plugins/lib/script-positions.ts` 只是渲染结果 —— 改它会被 `check` 判红。',
        topLevel: {
            schemaVersion: 1,
            _doc: '由本工具拥有（指向这份自描述的指针；⛔ 不手改）',
            scriptHeaderLen: '`pos` 的坐标系原点 = 0x3C（`kScriptHeaderLen`）；与 `label_%08x` 同一坐标系',
            js: '生成物（`.ts`）的仓库相对路径（渲染目标；`--js` 可覆盖，诊断用）',
            layout: '**生成物的顺序与段落**（`{kind:"section"|"position"}`）；`section.banner` 是逐字搬进生成物的整行注释',
            positions: '锚表本体（见字段表）；名字必须在 layout 里各出现一次',
        },
        fields: [
            ['name', '✅', '标识符', '生成物的 `export const` 名；与消费方 import 的名字一致'],
            ['script', '✅', '`*.BIN`', '宿主脚本名（基线根里的文件名，含 `$N$` 前缀时照写）'],
            ['baselineIndex', '✅', '整数 ≥ 0', '**基线**指令行序（不含头部 / 空行 / label 定义行）—— 锚窗口的起点'],
            ['at', '✅', '整数 ≥ 0', '窗口里第几条指令是"这个 pos 所在的那条"（0 = 窗口首条）'],
            ['to', '✅', '`start` \\| `end`', '取该指令的起点还是终点（`_HI` 常用终点；与下一 label 重合时用 `start`）'],
            ['window', '✅', `长度 ≥ ${MIN_WINDOW} 的字符串数组`, '基线指令行（**已屏蔽**：字符串 → `"?"`、label 引用 → `label_?`）；必须在补丁后视图里恰好命中一次'],
            ['pos', '✅', '整数 ≥ 0', '**基线坐标**下该锚的绝对偏移（建表时的参考值）；`gen` 不用它，`check` 用它核对 `baselineIndex/at/to` 与基线网格是否自洽'],
            ['note', '⬜', '整行注释数组', '渲染时**逐字**搬到生成物里该常量之前（`// …` 开头）'],
            ['width', '⬜', '整数 3..8（缺省 4）', '生成物字面量的十六进制宽度；只为逐字保住历史写法（没有语义，不影响 `pos`）'],
        ],
        invariants: [
            {
                id: 1,
                text: '`positions[].name` 唯一；`layout` 里的 `position` 与 `positions` **双向一一对应**（顺序由 layout 定）',
                enforcedBy: '`loadModel()`（gen / check / explain 都先跑）',
            },
            {
                id: 2,
                text: '`window` 必须**逐字等于**基线网格从 `baselineIndex` 起的行（基线换了 ⇒ 锚失效，必须显式重锚）',
                enforcedBy: '`locate()`（不匹配 ⇒ 点名第几条不一致，不静默改锚）',
            },
            {
                id: 3,
                text: '每个锚在**补丁后视图**里必须**恰好命中 1 次**；歧义（0 次 / ≥2 次）⇒ 拒绝生成并点名候选地址',
                enforcedBy: '`computePositions()`；`check` 的 ①',
            },
            {
                id: 4,
                text: '`pos` 等于**基线网格**按 `baselineIndex/at/to` 算出的值（它记录建表时的坐标；不随 patch 漂）',
                enforcedBy: '`check` 的 ②（`baselinePosOf()`）',
            },
            {
                id: 5,
                text: '生成物是 `renderTs()` 的**逐字**结果（含文件头）；数值写法 = `0x` + 小写十六进制（宽度缺省 4，`width` 可覆盖历史写法）',
                enforcedBy: '`check` 的 ③ + `tools/test/script-map.test.mjs`（导出名唯一 / 裸 `0x` 只许在这两张表里）',
            },
            {
                id: 6,
                text: '锚的宽度 ≥ 3 条指令；`at` 不越界；`to` 只取 start / end',
                enforcedBy: '`loadModel()` + `posFromHit()`',
            },
            {
                id: 7,
                text:
                    '命中的补丁后地址必须能被 patch 的 insert-after / delete / replace-line（在锚之前的那些）**逐条解释**；' +
                    '锚窗口里出现 insert / delete ⇒ 锚已失效（重锚，不猜）',
                enforcedBy: '`assertShiftExplained()`（gen / check / explain 共用）',
            },
        ],
        operations: OPERATIONS,
        writePath:
            '**唯一写入口 = `gen --write`**，且它只写生成物 ts（缺省 dry-run）。锚表 `tools/script-positions.json` 是' +
            '人工编辑的文本（本工具**不回写**它 —— `pos` 是基线参考值，patch 插入 / 删除只会让生成物的值平移）；' +
            '`data/translations/patch.json` **只读**（改完 patch 之后跑 gen 重算）。',
        howToCheck:
            '`check`（纯读）：锚唯一性 + 锚表 pos 与基线网格一致 + 生成物逐字等于渲染结果；红 = 忘了 gen / 锚歧义 / ' +
            '有人手改了生成物。人审一条：`explain <NAME>`。',
        howToChange:
            '① 新增位置：先定锚（脚本 + 基线行序 + 最小可唯一窗口，字符串写 `"?"`、label 写 `label_?`）⇒ 加进 `positions`，' +
            '在 `layout` 里放到生成物的位置（`note` 写读数出处）⇒ `gen --write` ⇒ `check`；' +
            '② patch 改了（insert-after / delete 移动坐标）：`check` 会在具体常量上红 ⇒ `gen --write`；' +
            '③ ⛔ 不要手改生成物，也不要在消费方裸写 `0x…`。',
        patchSensitivity:
            '译文只改字符串 ⇒ **不移动**坐标（字符串进池，指令长度只由 `argc` 定）；`insert-after` / `delete` ' +
            '每条 ±`4+argc*8` 字节 ⇒ `gen` 按补丁后视图自动平移。若某个锚落在被插入 / 删除的窗口里，锚会失配并点名。',
    };
}

export function describeText(d = describe()) {
    const L = [];
    L.push(`# ${d.file} —— 自描述（由控制脚本给出，文档不复述 schema）`);
    L.push('');
    L.push(`用途：${d.purpose}`);
    L.push(`★ ${d.notHere}`);
    L.push('');
    L.push('## 顶层');
    for (const [k, v] of Object.entries(d.topLevel)) L.push(`* \`${k}\`: ${v}`);
    L.push('');
    L.push('## 字段');
    L.push('| 字段 | 必填 | 类型 / 枚举 | 说明 |');
    L.push('|---|---|---|---|');
    for (const [name, req, type, desc] of d.fields) L.push(`| \`${name}\` | ${req} | ${type} | ${desc} |`);
    L.push('');
    L.push('## 不变量（含谁在守）');
    for (const inv of d.invariants) L.push(`${inv.id}. ${inv.text}\n   → 守：${inv.enforcedBy}`);
    L.push('');
    L.push('## 操作');
    for (const op of d.operations) L.push(`* \`${op.name}\`${op.mutates ? '（会写）' : ''}：${op.summary}`);
    L.push('');
    L.push(`## 写路径`);
    L.push(d.writePath);
    L.push('');
    L.push(`## 怎么查`);
    L.push(d.howToCheck);
    L.push('');
    L.push(`## 怎么改`);
    L.push(d.howToChange);
    L.push('');
    L.push('## patch 对坐标的影响');
    L.push(d.patchSensitivity);
    return `${L.join('\n')}\n`;
}

// ─────────────────────────────────────────────────────────── CLI

function parseArgs(argv) {
    const out = {
        action: null,
        write: false,
        modelPath: POSITIONS_JSON,
        patchPath: DEFAULT_PATCH,
        jsPath: null,
        name: null,
    };
    const actions = {
        describe: 'describe',
        gen: 'gen',
        '--describe': 'describe',
        '--gen': 'gen',
        '--check': 'check',
        '--check-anchors': 'check',
        check: 'check',
        '--explain': 'explain',
        explain: 'explain',
        '--help': 'help',
        '-h': 'help',
        help: 'help',
    };
    const takesValue = new Set(['--patch', '--js', '--model']);
    for (let i = 0; i < argv.length; i += 1) {
        const a = argv[i];
        if (actions[a]) {
            out.action = actions[a];
            continue;
        }
        if (a === '--write') {
            out.write = true;
            continue;
        }
        if (takesValue.has(a)) {
            const v = argv[i + 1];
            if (v === undefined || v.startsWith('--')) throw new Error(`${a} 需要一个值`);
            if (a === '--patch') out.patchPath = path.resolve(v);
            else if (a === '--js') out.jsPath = path.resolve(v);
            else out.modelPath = path.resolve(v);
            i += 1;
            continue;
        }
        if (a.startsWith('--')) throw new Error(`未知参数：${a}`);
        if (out.action === 'explain' && out.name === null) {
            out.name = a;
            continue;
        }
        throw new Error(`多余的位置参数：${a}`);
    }
    return out;
}

export async function main(argv = process.argv.slice(2)) {
    let args;
    try {
        args = parseArgs(argv);
    } catch (err) {
        writeOut(process.stderr, `${err.message}\n\n${HELP}`);
        return 2;
    }
    try {
        if (args.action === null || args.action === 'describe') {
            writeOut(process.stdout, describeText());
            return 0;
        }
        if (args.action === 'help') {
            writeOut(process.stdout, HELP);
            return 0;
        }
        if (args.action === 'gen') return actionGen(args);
        if (args.action === 'check') return actionCheck(args);
        if (args.action === 'explain') return actionExplain(args);
        writeOut(process.stderr, `未知动作。\n\n${HELP}`);
        return 2;
    } catch (err) {
        writeOut(process.stderr, `ERROR: ${err.message}`);
        return 2;
    }
}

const isDirectRun =
    process.argv[1] && path.resolve(process.argv[1]) === path.resolve(fileURLToPath(import.meta.url));
if (isDirectRun) {
    main()
        .then((code) => {
            process.exitCode = code ?? 0;
        })
        .catch((err) => {
            process.stderr.write(`ERROR: ${err.stack ?? err.message}\n`);
            process.exitCode = 2;
        });
}
