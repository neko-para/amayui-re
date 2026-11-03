#!/usr/bin/env node
/**
 * tools/platform.mjs — **CLI**：平台面普查（**只读**）——从反汇编语料机械抽出三张表
 *
 * ① 导入函数（按 `; Imports from <dll>` 分组）② COM vtable 调用点（按 `+ <offset>` 计数）
 * ③ 动态解析（`LoadLibrary` / `GetProcAddress` 站点）。**只做抽取与展示，不做语义裁决。**
 *
 * 经派发器：`pnpm tools platform <describe|census> [args…]`
 * 也可独立跑：`node tools/platform.mjs --census`
 *
 * ★ 分工（`tools/README.md` §0）：本文件是 CLI + 抽取模型（这份输出没有第二个 schema 副本，
 *   `describe()` 就是字段语义的唯一真源）；语料行文法复用 `lib/lst-format.mjs`，
 *   EA/行号 → 函数符号复用 `lib/catalog.mjs`（⛔ 不复刻第二份 .lst 文法）。
 * ★ 只读：语料一个字节都不写；`census --out` 只写**派生的** JSON（生成物，缺省不写）。
 * ★ 铁律：输出里**没有**接口名 / 方法名 / 语义结论 —— 只有 `dll / name / offset / ea / caller / counts`。
 *   解不出来的进 `honesty[]` / `ignored`，⛔ 不填占位名。
 */
import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

import { buildCatalog, functionOfEa, listModules, resolveModule } from './lib/catalog.mjs';
import { AUTO_NAME_EA_RE, REG_RE, parseListingLine } from './lib/lst-format.mjs';
import { DEFAULT_OUT_DIR, REPO_ROOT } from './lib/paths.mjs';

export const DOMAIN = {
  id: 'platform',
  title: '**平台面普查**（只读）：导入函数 / COM vtable 调用点 / 动态解析 三张表（机械抽取，不做语义裁决）',
  data: [
    '`corpus/disasm/files/*.lst`（**只读**：导入块 / `call` 调用点 / 动态解析站点）',
    '`corpus/disasm/files/*.c`（**只读**：vtable 调用点、caller 行区间、动态解析兜底）',
  ],
  access: '**只读语料**；`census --out <file>` 才写一份派生 JSON（生成物，缺省不写、写后回读复验）',
  tool: 'tools/platform.mjs',
};

export const OPERATIONS = [
  {
    name: 'describe',
    argv: ['--describe'],
    mutates: false,
    summary: '自描述：数据源 / 字段语义 / 不变量 / 操作 / 诚实项（**本工具 JSON 的 schema 唯一真源**）',
  },
  {
    name: 'census',
    argv: ['--census'],
    mutates: true,
    summary:
      '三张表 + 诚实项：缺省人读（age+agerc）；`--json` 机器读；`--module age|agerc|both` 选模块；`--out <file>` 写派生 JSON（缺省只打印）',
  },
];

/** 输出 schema 版本。改字段语义 = 改它（describe 与守卫一起动）。 */
export const SCHEMA = 1;
/** `census` 缺省看哪几个模块（键经 `resolveModule` 解析，文件名前缀也可 ⇒ `agerc` 命中 `AGERC.DLL`）。 */
export const DEFAULT_MODULE_KEYS = ['age', 'agerc'];
/** 动态解析向前找 `push offset … ; "<字符串>"` 的行数上限（任务书 §3.4：最多 12 行）。 */
export const DEFAULT_DYN_WINDOW = 12;
/** 人读输出里 vtable 直方图/样例保留前多少个 offset。 */
export const DEFAULT_SAMPLE_LIMIT = 20;

// ─────────────────────────────────────────────────────────── 通用

/** 相对仓库根的正斜杠路径（打印用；跨平台一致、跨机器可复现） */
const rel = (p) => path.relative(REPO_ROOT, p).split(path.sep).join('/');
const cmpStr = (a, b) => (a < b ? -1 : a > b ? 1 : 0);
const hex = (n) => `0x${Number(n).toString(16).toUpperCase()}`;

/** `.lst` 的「自动数据名」：`ds:dword_55E1B4` 这类是全局函数指针，**不是**命名导入 */
const DS_AUTO_RE = /^(dword|off|byte|word|unk)_[0-9A-Fa-f]+$/;
/** `.lst` 的自动代码标签（`sub_401000` / `loc_401013` …）——本地调用，不是平台面 */
const AUTO_CODE_RE = /^(sub|loc|nullsub|locret|def)_/;
/** 导入名里的 MSVC 导入前缀：`__imp_Direct3DCreate9` 的**符号**是 `Direct3DCreate9` */
const IMP_PREFIX_RE = /^__imp_/;

/**
 * 导入条目的**符号名**：只做一件事 —— 去掉 MSVC 的 `__imp_` 前缀。
 * ★ 实测两份语料形态不同：AGE 声明 `CreateEventA dd ?`（本来就干净），
 *   AGERC 声明 `extrn __imp_Direct3DCreate9:dword`（**带前缀**）而 thunk 叫 `j_Direct3DCreate9`。
 *   不归一 ⇒ AGERC 的 4 个 `__imp_` 导入会**看起来一次都没被调用**（假阴性）。
 */
export function importSymbol(name) {
  return String(name).replace(IMP_PREFIX_RE, '');
}

// ─────────────────────────────────────────────────────────── ① 导入块（`.lst`）

/** `; Imports from <dll>`（`<dll>` 原样保留大小写：DLL 名就是分类，⛔ 不按函数名猜 DLL） */
const IMPORT_HEADER_RE = /^;\s*Imports from (\S+)\s*$/;
/** `Name dd ?`（可带尾注释 —— 实测 OLEAUT32 是 `VariantInit dd ?        ; Import by ordinal 8`） */
const IMPORT_DD_RE = /^([A-Za-z_$?@][\w.$?@]*)\s+dd\s+\?\s*(?:;.*)?$/;
/** `extrn Name:dword`（AGERC 的命名 IAT 形态） */
const IMPORT_EXTRN_RE = /^extrn\s+([A-Za-z_$?@][\w.$?@]*):dword\s*(?:;.*)?$/;

/**
 * 解析 `.lst` 的导入块。
 *
 * 文法（**实测两份语料**）：
 * * `; Imports from <dll>` 开一组；
 * * 组内条目两种形态 —— `Name dd ?`（AGE）与 `extrn Name:dword`（AGERC）；
 * * **首组前面有前导**（`.lst` 在段头注释里先写 `; Imports from`，再写 `_data segment` /
 *   `; Segment type: …`，条目在那之后）⇒ 组开始后允许跳过**注释与空行**，直到第一条条目；
 * * 条目**已开始**后，遇到非条目、非注释行（`align 8` / `db 0` / 空行）即**结束本组**
 *   ——否则下一段的无名数据会被错记到上一个 DLL 名下。
 *
 * @param {string[]} lines `.lst` 的全部行
 * @returns {{groups: {dll: string, entries: {ea: number, name: string, decl: 'dd'|'extrn'}[]}[]}}
 */
export function parseImportGroups(lines) {
  const groups = [];
  let cur = null;
  let started = false;
  const close = () => {
    cur = null;
    started = false;
  };
  for (const line of lines) {
    const parsed = parseListingLine(line);
    if (!parsed) continue;
    const body = parsed.body.trim();
    const head = IMPORT_HEADER_RE.exec(body);
    if (head) {
      close();
      cur = { dll: head[1], entries: [] };
      groups.push(cur);
      continue;
    }
    if (!cur) continue;
    if (body === '' || body.startsWith(';')) continue; // 组内注释 / 空行 / 段头前导
    const dd = IMPORT_DD_RE.exec(body);
    if (dd) {
      cur.entries.push({ ea: parsed.ea, name: dd[1], decl: 'dd' });
      started = true;
      continue;
    }
    const ext = IMPORT_EXTRN_RE.exec(body);
    if (ext) {
      cur.entries.push({ ea: parsed.ea, name: ext[1], decl: 'extrn' });
      started = true;
      continue;
    }
    if (started) close(); // 条目之后的第一条非条目行 = 本组结束
  }
  return { groups };
}

// ─────────────────────────────────────────────────────────── ② 调用点（`.lst`）

/**
 * 解析 `.lst` 的 `call` / `jmp` 目标（**只**分形态，不判语义）。
 *
 * 形态（⛔ 不猜、不补名）：
 * * `j_<Name>` ⇒ `thunk`（IDA 的跳转 thunk）；
 * * `ds:<Name>` 且 `<Name>` **不**匹配 `(dword|off|byte|word|unk)_[0-9A-Fa-f]+` ⇒ `iat`；
 * * `ds:自动数据名` / 寄存器 / 内存操作数 / 自动代码标签 ⇒ **不计入**，分类计数（诚实项用）；
 * * 裸符号名 `call <Name>` ⇒ `direct`（实测 AGE 有这一形态：`call LoadLibraryA`，既非 `j_` 也非 `ds:`）。
 *   ★ 是否真的是导入，由 `aggregateImports` 按**精确符号相等**判定；这里只记录目标名。
 *
 * @param {string[]} lines
 * @returns {{sites: {ea:number, form:'thunk'|'iat'|'direct', name:string}[],
 *            jmpSites: {ea:number, form:'thunk'|'iat'|'direct', name:string}[],
 *            ignored: {dsAutoData:number, register:number, indirect:number, autoCode:number}}}
 */
export function parseCallSites(lines) {
  const sites = [];
  const jmpSites = [];
  const ignored = { dsAutoData: 0, register: 0, indirect: 0, autoCode: 0 };
  const classify = (target) => {
    if (target.startsWith('j_')) {
      const name = target.slice(2);
      return name === '' ? null : { form: 'thunk', name };
    }
    if (target.startsWith('ds:')) {
      const name = target.slice(3);
      if (DS_AUTO_RE.test(name)) return { skip: 'dsAutoData' };
      if (!/^[A-Za-z_$?@][\w.$?@]*$/.test(name)) return { skip: 'indirect' };
      return { form: 'iat', name };
    }
    if (REG_RE.test(target) || target.includes('[') || target.includes('ptr')) return { skip: 'indirect' };
    if (AUTO_NAME_EA_RE.test(target) || AUTO_CODE_RE.test(target)) return { skip: 'autoCode' };
    if (!/^[A-Za-z_$?@][\w.$?@]*$/.test(target)) return { skip: 'indirect' };
    return { form: 'direct', name: target };
  };
  for (const line of lines) {
    const parsed = parseListingLine(line);
    if (!parsed) continue;
    const body = parsed.body.trim();
    const m = /^(call|jmp)\s+(\S+)$/.exec(body);
    if (!m) continue;
    const got = classify(m[2]);
    if (!got) continue;
    if (got.skip) {
      ignored[got.skip] += 1;
      continue;
    }
    (m[1] === 'call' ? sites : jmpSites).push({ ea: parsed.ea, form: got.form, name: got.name });
  }
  return { sites, jmpSites, ignored };
}

// ─────────────────────────────────────────────────────────── ③ vtable 调用点（`.c`）

/** `+ <offset>))(`：offset 是十进制或 `0x` 十六进制（`.c` 里只有偏移，没有 EA） */
const VTABLE_OFFSET_RE = /\+\s*(0x[0-9A-Fa-f]+|\d+)\s*\)\)\s*\(/;

/**
 * 解析 `.c` 里的 vtable 调用点：`(*(<cast>)(<obj> + <offset>))(<args>)`。
 *
 * 判据（**只认形态**，⛔ 不猜接口）：
 * 1. 找 `(*(`（"取函数指针并强转"的外壳）；
 * 2. 从它开始找**第一个** `+ <num>))(` —— 这就是 vtable offset；
 *    外面/里面更早出现的 `+ num` 是**对象字段**偏移（实测 `*(_DWORD *)(this + 697620) + 4`），
 *    它的 `)` 后面跟的不是 `))(` 而是 ` + …`，所以不会被误取。
 * 3. 一行里可以有多处站点，逐个计数。
 *
 * @param {string[]} lines `.c` 的全部行
 * @returns {{sites: {line:number, offset:number}[], candidateLines:number, withoutOffset:number}}
 */
export function parseVtableSites(lines) {
  const sites = [];
  let candidateLines = 0;
  let withoutOffset = 0;
  for (let i = 0; i < lines.length; i += 1) {
    const line = lines[i];
    let pos = 0;
    let counted = false;
    for (;;) {
      const start = line.indexOf('(*(', pos);
      if (start < 0) break;
      if (!counted) {
        candidateLines += 1;
        counted = true;
      }
      const m = VTABLE_OFFSET_RE.exec(line.slice(start));
      if (!m) {
        withoutOffset += 1;
        pos = start + 3;
        continue;
      }
      const offset = /^0x/i.test(m[1]) ? Number.parseInt(m[1].slice(2), 16) : Number.parseInt(m[1], 10);
      sites.push({ line: i + 1, offset });
      pos = start + m.index + m[0].length;
    }
  }
  return { sites, candidateLines, withoutOffset };
}

// ─────────────────────────────────────────────────────────── ④ 动态解析（`.lst` 优先，`.c` 兜底）

/** `call` 的目标是这三个 API 之一（`ds:` / `j_` / 裸名三种写法都认） */
const DYN_API_RE = /^(LoadLibrary[AW]|GetProcAddress)$/;
/** `push offset <sym> ; "<字符串>"`（字符串在**注释**里；取不到就不填，⛔ 不用符号名猜字符串） */
const PUSH_STRING_RE = /^push\s+offset\s+[A-Za-z_$?@][\w.$?@]*\s*;\s*"((?:[^"\\]|\\.)*)"/;

/**
 * 解析 `.lst` 的动态解析站点。
 *
 * 规则（任务书 §3.4，按**实测**修正过一处）：找 `call` 目标是 `LoadLibraryA/W` / `GetProcAddress`
 * 的 EA；**向前最多 `window` 行**找最近的 `push offset <sym> ; "<字符串>"`：
 * * `arg` = 最近的那个字符串（LoadLibrary 是库名；GetProcAddress 实测是函数名）；
 * * `prevArg` = 次近的那个（**只是原始抽取**：实测它常常是**上一个调用的函数名**，不是库名
 *   ⇒ ⛔ 不要当库名用，见 `honesty`）；
 * * 窗口里没有字符串 ⇒ `arg = null`，计入 `unresolved`（⛔ 不填占位名）。
 * * 只认 `call`；`jmp` 尾跳单独计数（`jmpSkipped`），不进站点表。
 *
 * @param {string[]} lines `.lst` 行
 * @param {{window?:number, callerAtEa?:((ea:number)=>string|null)|null}} [opts]
 */
export function parseDynamicSites(lines, { window = DEFAULT_DYN_WINDOW, callerAtEa = null } = {}) {
  const sites = [];
  let jmpSkipped = 0;
  const jmpByApi = {};
  for (let i = 0; i < lines.length; i += 1) {
    const parsed = parseListingLine(lines[i]);
    if (!parsed) continue;
    const body = parsed.body.trim();
    const call = /^call\s+(?:ds:|j_)?(\S+)$/.exec(body);
    if (!call || !DYN_API_RE.test(call[1])) {
      const jmp = /^jmp\s+(?:ds:|j_)?(\S+)$/.exec(body);
      if (jmp && DYN_API_RE.test(jmp[1])) {
        jmpSkipped += 1;
        jmpByApi[jmp[1]] = (jmpByApi[jmp[1]] ?? 0) + 1;
      }
      continue;
    }
    const pushed = [];
    for (let k = i - 1; k >= 0 && i - k <= window; k -= 1) {
      const prev = parseListingLine(lines[k]);
      if (!prev) continue;
      const hit = PUSH_STRING_RE.exec(prev.body.trim());
      if (hit) pushed.push(hit[1]); // 最近的在前
    }
    sites.push({
      api: call[1],
      ea: parsed.ea,
      line: i + 1,
      arg: pushed[0] ?? null,
      prevArg: pushed[1] ?? null,
      caller: callerAtEa ? callerAtEa(parsed.ea) : null,
    });
  }
  return { sites, unresolved: sites.filter((s) => s.arg === null).length, jmpSkipped, jmpByApi };
}

/** 括号配对（跳过字符串里的括号）；没闭合 ⇒ 行尾 */
function closingParen(text, open) {
  let depth = 0;
  let quote = false;
  for (let i = open; i < text.length; i += 1) {
    const ch = text[i];
    if (quote) {
      if (ch === '\\') i += 1;
      else if (ch === '"') quote = false;
      continue;
    }
    if (ch === '"') quote = true;
    else if (ch === '(') depth += 1;
    else if (ch === ')') {
      depth -= 1;
      if (depth === 0) return i;
    }
  }
  return text.length;
}

/**
 * `.c` **兜底**：只在 `.lst` 一个站点都抽不到时启用（`.lst` 是权威的那一侧）。
 * ★ `.c` **没有地址**（Hex-Rays 不打印 EA）⇒ 站点的 `ea` 恒为 `null`，只有行号；
 *   这是这一侧的固有诚实项，不是"没做"。
 */
export function parseDynamicSitesFromC(lines, { callerAtLine = null } = {}) {
  const sites = [];
  const API_RE = /\b(LoadLibrary[AW]|GetProcAddress)\s*\(/g;
  for (let i = 0; i < lines.length; i += 1) {
    const line = lines[i];
    API_RE.lastIndex = 0;
    let m;
    while ((m = API_RE.exec(line)) !== null) {
      const api = m[1];
      const open = m.index + m[0].length - 1;
      const args = line.slice(open + 1, closingParen(line, open));
      const strings = [...args.matchAll(/"((?:[^"\\]|\\.)*)"/g)].map((x) => x[1]);
      sites.push({
        api,
        ea: null,
        line: i + 1,
        arg: api === 'GetProcAddress' ? (strings[strings.length - 1] ?? null) : (strings[0] ?? null),
        prevArg: null,
        caller: callerAtLine ? callerAtLine(i + 1) : null,
      });
    }
  }
  return { sites, unresolved: sites.filter((s) => s.arg === null).length, jmpSkipped: 0, jmpByApi: {} };
}

// ─────────────────────────────────────────────────────────── 聚合

/** 一种调用形态都没有 ⇒ `null`；thunk+iat ⇒ `both`；带 `direct` ⇒ 后缀 `+direct` */
function callFormOf(calls) {
  const base = calls.thunk && calls.iat ? 'both' : calls.thunk ? 'thunk' : calls.iat ? 'iat' : null;
  if (calls.direct) return base ? `${base}+direct` : 'direct';
  return base;
}

/**
 * 把调用点归到导入函数上（**只按名字精确相等**，thunk 重名后缀是唯一例外）。
 *
 * 归一规则（写死在 `describe()` 里，**不是**猜）：
 * * `symbol` = 声明名去掉 `__imp_`；
 * * `j_<Name>_<n>` 在**精确匹配失败**时去掉尾部 `_<n>` 再试（IDA 给"重复 thunk"加的后缀：
 *   实测 `j_CreateEventA_0/1/2` 三个 thunk 都指向同一个导入 `CreateEventA`）。
 *   ⛔ 只对 `thunk` 形态做；`direct` / `iat` 形态**不做**（那里 `X_0` 可能就是另一个函数）。
 *
 * @param {{groups: object[]}} parsed    `parseImportGroups` 的返回值
 * @param {{ea:number, form:string, name:string}[]} sites `parseCallSites().sites`
 * @param {{ea:number, form:string, name:string}[]} [jmpSites] 同一个 parseCallSites 的 `jmpSites`
 */
export function aggregateImports(parsed, sites, jmpSites = []) {
  const functions = [];
  const bySymbol = new Map();
  const duplicateSymbols = [];
  const declCounts = { dd: 0, extrn: 0 };
  for (const g of parsed.groups) {
    for (const e of g.entries) {
      const symbol = importSymbol(e.name);
      declCounts[e.decl] += 1;
      const rec = {
        dll: g.dll,
        name: e.name,
        symbol,
        iat: e.ea,
        decl: e.decl,
        calls: { thunk: 0, iat: 0, direct: 0, total: 0 },
        callForm: null,
        samples: { thunk: null, iat: null, direct: null },
      };
      functions.push(rec);
      if (bySymbol.has(symbol)) duplicateSymbols.push(symbol);
      else bySymbol.set(symbol, rec);
    }
  }
  const unattributed = {
    thunk: 0,
    iat: 0,
    direct: 0,
    names: { thunk: new Set(), iat: new Set(), direct: new Set() },
  };
  const match = (site) => {
    let hit = bySymbol.get(site.name) ?? null;
    if (!hit && site.form === 'thunk') hit = bySymbol.get(site.name.replace(/_\d+$/, '')) ?? null;
    return hit;
  };
  for (const site of sites) {
    const hit = match(site);
    if (!hit) {
      unattributed[site.form] += 1;
      unattributed.names[site.form].add(site.name);
      continue;
    }
    hit.calls[site.form] += 1;
    hit.calls.total += 1;
    if (hit.samples[site.form] === null) hit.samples[site.form] = site.ea;
  }
  let jmpToImport = 0;
  for (const site of jmpSites) if (match(site)) jmpToImport += 1;

  for (const f of functions) f.callForm = callFormOf(f.calls);

  const byDllMap = new Map();
  for (const f of functions) {
    if (!byDllMap.has(f.dll)) byDllMap.set(f.dll, []);
    byDllMap.get(f.dll).push(f);
  }
  const byDll = [...byDllMap.entries()]
    .sort((a, b) => cmpStr(a[0], b[0]))
    .map(([dll, list]) => ({
      dll,
      count: list.length,
      functions: list.sort((a, b) => cmpStr(a.name, b.name)),
    }));

  return {
    dllCount: byDll.length,
    functionCount: functions.length,
    duplicateSymbols: [...new Set(duplicateSymbols)].sort(cmpStr),
    declCounts,
    byDll,
    unattributed: {
      thunk: unattributed.thunk,
      iat: unattributed.iat,
      direct: unattributed.direct,
      thunkNames: [...unattributed.names.thunk].sort(cmpStr),
      iatNames: [...unattributed.names.iat].sort(cmpStr),
      directNames: [...unattributed.names.direct].sort(cmpStr),
    },
    jmpToImport,
  };
}

/**
 * vtable 站点聚合：offset 直方图（升序）+ 前 N 个 offset 的**首个**站点样例（行号 / 可选 caller）。
 * @param {{sites:{line:number,offset:number}[], candidateLines:number, withoutOffset:number}} parsed
 * @param {((line:number)=>string|null)|null} [callerAtLine] `.c` 行号 → 函数符号（解不出 ⇒ null）
 */
export function aggregateVtables(parsed, callerAtLine = null, limit = DEFAULT_SAMPLE_LIMIT) {
  const histogram = new Map();
  const firstByOffset = new Map();
  let callerResolved = 0;
  for (const site of parsed.sites) {
    histogram.set(site.offset, (histogram.get(site.offset) ?? 0) + 1);
    if (!firstByOffset.has(site.offset)) firstByOffset.set(site.offset, site);
    if (callerAtLine && callerAtLine(site.line)) callerResolved += 1;
  }
  const rows = [...histogram.keys()].sort((a, b) => a - b).map((offset) => ({ offset, count: histogram.get(offset) }));
  const samples = [...rows]
    .sort((a, b) => b.count - a.count || a.offset - b.offset)
    .slice(0, limit)
    .map(({ offset, count }) => {
      const first = firstByOffset.get(offset);
      const caller = callerAtLine ? callerAtLine(first.line) : null;
      return { offset, count, line: first.line, caller };
    });
  return {
    siteCount: parsed.sites.length,
    offsetCount: rows.length,
    histogram: rows,
    samples,
    caller: {
      available: typeof callerAtLine === 'function',
      resolved: callerResolved,
      unresolved: parsed.sites.length - callerResolved,
    },
    candidateLines: parsed.candidateLines,
    withoutOffset: parsed.withoutOffset,
  };
}

/** 动态解析的按 API 计数（键升序） */
function apiCountsOf(sites) {
  const map = new Map();
  for (const s of sites) map.set(s.api, (map.get(s.api) ?? 0) + 1);
  return [...map.entries()].sort((a, b) => cmpStr(a[0], b[0])).map(([api, count]) => ({ api, count }));
}

// ─────────────────────────────────────────────────────────── 一份普查

/**
 * **一份普查**（纯函数：只吃行数组 + 两个可选的"行/EA → 函数符号"查表器）。
 * 测试就靠它用内联小样本跑（⛔ 不读 `corpus/`）。
 *
 * @param {object} o
 * @param {string} o.key      模块键（`age` / `agerc-dll`）
 * @param {string} o.title    语料对名（例如 `AGE.EXE__dumped.sectfix.EXE`）
 * @param {string[]} o.lstLines
 * @param {string[]} o.cLines
 * @param {((line:number)=>string|null)|null} [o.callerAtLine] `.c` 行号 → 函数符号
 * @param {((ea:number)=>string|null)|null} [o.callerAtEa]   `.lst` EA → 函数符号
 */
export function buildCensus({ key, title, lstLines, cLines, callerAtLine = null, callerAtEa = null }) {
  const parsedImports = parseImportGroups(lstLines);
  const calls = parseCallSites(lstLines);
  const imports = aggregateImports(parsedImports, calls.sites, calls.jmpSites);

  const vtable = aggregateVtables(parseVtableSites(cLines), callerAtLine);

  let dyn = parseDynamicSites(lstLines, { callerAtEa });
  const source = dyn.sites.length > 0 ? 'lst' : 'c';
  if (source === 'c') dyn = parseDynamicSitesFromC(cLines, { callerAtLine });

  const dynamicUnresolvedCaller = dyn.sites.filter((s) => s.caller === null).length;
  const loadLibrary = dyn.sites.filter((s) => s.api.startsWith('LoadLibrary')).length;
  const getProcAddress = dyn.sites.filter((s) => s.api === 'GetProcAddress').length;

  const ignored = {
    dsAutoData: calls.ignored.dsAutoData,
    register: calls.ignored.register,
    indirect: calls.ignored.indirect,
    autoCode: calls.ignored.autoCode,
    thunkNotImport: imports.unattributed.thunk,
    iatNotImport: imports.unattributed.iat,
    directNotImport: imports.unattributed.direct,
    jmpToImport: imports.jmpToImport,
    jmpToDynamicApi: dyn.jmpSkipped,
  };

  const honesty = [];
  honesty.push('vtable offset 未映射接口（无类型证据）：只给 `*(vtable + off)` 与计数，⛔ 不写接口名 / 方法名。');
  honesty.push(`调用点是静态计数（语料里 \`call\` 指令的出现次数），不是运行时路径命中；只计 \`call\`，不含 \`jmp\` 尾跳（另有 ${ignored.jmpToImport} 处 \`jmp\` 到导入、${ignored.jmpToDynamicApi} 处 \`jmp\` 到动态解析 API）。`);
  honesty.push(`\`call ds:自动数据名\`（如 \`ds:dword_*\`）${ignored.dsAutoData} 处**不计入**导入调用：那是全局函数指针的间接调用，不是命名导入。`);
  honesty.push(
    `导入名按语料声明原样（\`name\`：${imports.declCounts.dd} 条 \`dd ?\` + ${imports.declCounts.extrn} 条 \`extrn <Name>:dword\`）；` +
      '匹配调用点用 `symbol`（只去 `__imp_` 前缀）；`j_<Name>_<n>` 只在精确匹配失败时去掉 `_<n>`（IDA 重复 thunk 后缀）。',
  );
  honesty.push(
    `归不到导入的调用点（未计入）：thunk ${ignored.thunkNotImport} 处 / 命名 ds ${ignored.iatNotImport} 处 / 裸名 ${ignored.directNotImport} 处` +
      '（后者含 CRT / 本地具名函数；裸名形态实测存在，任务书的两种形态没有覆盖它）。',
  );
  honesty.push(
    `动态解析 ${loadLibrary} LoadLibrary / ${getProcAddress} GetProcAddress；未解析 ${dyn.unresolved} 处（实参是变量或字符串不在注释里 ⇒ \`arg=null\`，⛔ 不填占位名）；源 = .${source}。`,
  );
  honesty.push(
    'GetProcAddress 的"库 + 函数"两串**未配对**：只给最近 push 字符串 `arg` 与次近 `prevArg`；' +
      '实测 `prevArg` 常常是上一个调用的函数名（不是库名）⇒ ⛔ 不要当库名用。',
  );
  honesty.push(
    vtable.caller.available
      ? `vtable caller 来自 \`.c\` 函数定义行区间：解析出 ${vtable.caller.resolved} / 未解析 ${vtable.caller.unresolved} 处（解不出 ⇒ null，不猜）。`
      : 'vtable caller **未解析**（`.c` 侧查表不可用）：只给行号。',
  );
  honesty.push(
    callerAtEa
      ? `动态解析站点的 caller 来自 \`.lst\` EA → 函数区间：解析出 ${dyn.sites.length - dynamicUnresolvedCaller} / 未解析 ${dynamicUnresolvedCaller} 处。`
      : '动态解析站点**未给 caller**（`.lst` EA → 函数区间查表不可用），只有 EA / 行号。',
  );
  if (source === 'c') honesty.push('`.lst` 抽不到动态解析站点 ⇒ 启用 `.c` 兜底：`.c` 没有地址，站点 `ea` 恒为 null。');
  honesty.push(
    `.c 里含「(*(」的行 ${vtable.candidateLines} 行；其中 ${vtable.withoutOffset} 处没有「+ <offset>))(」形态 ⇒ 不是 vtable 站点（强转 / 间接调用），未计入。`,
  );

  const sources = { lst: `<module>.lst`, c: `<module>.c` };
  return {
    module: key,
    file: title,
    sources,
    imports,
    vtable,
    dynamic: {
      source,
      windowLines: source === 'lst' ? DEFAULT_DYN_WINDOW : null,
      apiCounts: apiCountsOf(dyn.sites),
      unresolved: dyn.unresolved,
      sites: dyn.sites,
      jmpSkipped: dyn.jmpSkipped,
      jmpByApi: Object.fromEntries(Object.entries(dyn.jmpByApi).sort((a, b) => cmpStr(a[0], b[0]))),
    },
    ignored,
    honesty,
  };
}

// ─────────────────────────────────────────────────────────── 人读输出

/** 三张表 + 诚实项（人读）。输入是 `censusDocument()` 的返回值。 */
export function censusText(doc) {
  const L = [];
  for (const m of doc.modules) {
    L.push(`平台面普查 —— ${m.module}（${m.file}）`);
    L.push(`数据源  ${m.sources.lst}${m.sources.c ? ` + ${m.sources.c}` : ''}`);
    L.push('');
    L.push(`导入    ${m.imports.dllCount} 个 DLL / ${m.imports.functionCount} 个函数`);
    const width = Math.max(0, ...m.imports.byDll.flatMap((g) => g.functions.map((f) => f.name.length)));
    for (const g of m.imports.byDll) {
      L.push(`  ${g.dll} (${g.count})`);
      for (const f of g.functions) {
        const tail = f.calls.total === 0 ? '调用 0 处' : `调用 ${f.calls.total} 处  ${f.callForm}`;
        const sym = f.symbol === f.name ? '' : ` → ${f.symbol}`;
        L.push(`    ${f.name.padEnd(width)}  iat ${hex(f.iat)}  ${tail}${sym}`);
      }
    }
    L.push('');
    const v = m.vtable;
    L.push(`vtable  ${v.siteCount} 个站点（${v.offsetCount} 个 offset）；直方图（前 ${v.samples.length}）`);
    for (const s of v.samples) {
      L.push(`  ${`+${s.offset}`.padEnd(9)}${String(s.count).padStart(6)}   例 行 ${s.line}${s.caller ? `（${s.caller}）` : ''}`);
    }
    L.push('');
    const dyn = m.dynamic;
    const loadLibrary = dyn.apiCounts.filter((x) => x.api.startsWith('LoadLibrary')).reduce((n, x) => n + x.count, 0);
    const getProcAddress = dyn.apiCounts.filter((x) => x.api === 'GetProcAddress').reduce((n, x) => n + x.count, 0);
    L.push(`动态    ${loadLibrary} LoadLibrary / ${getProcAddress} GetProcAddress；未解析 ${dyn.unresolved}（源 = .${dyn.source}）`);
    L.push(`        arg = 该站点前 ${dyn.windowLines ?? '?'} 行内最近的 push 字符串；库名未配对（见诚实项）`);
    for (const s of dyn.sites) {
      const where = s.ea === null ? `行 ${s.line}` : `${hex(s.ea)} 行 ${s.line}`;
      L.push(`  ${s.api.padEnd(14)} ${where}  ${s.arg === null ? '（未解析）' : `"${s.arg}"`}${s.caller ? `  ← ${s.caller}` : ''}`);
    }
    L.push('');
    L.push('诚实项');
    for (const h of m.honesty) L.push(`  · ${h}`);
    L.push('');
  }
  if (doc.honesty.length > 0) {
    L.push('诚实项（口径，与具体语料无关）');
    for (const h of doc.honesty) L.push(`  · ${h}`);
    L.push('');
  }
  return `${L.join('\n').replace(/\n+$/, '')}\n`;
}

/** 机器读：顶层文档（模块按 key 升序、无时间戳 ⇒ 同一输入逐字节相同） */
export function censusDocument(modules) {
  return {
    schema: SCHEMA,
    tool: 'platform',
    modules: [...modules].sort((a, b) => cmpStr(a.module, b.module)),
    honesty: GLOBAL_HONESTY,
  };
}

/** 与具体语料无关的诚实项（`describe` 与 `census --json` 共用同一份，不写第二遍） */
export const GLOBAL_HONESTY = [
  'vtable offset 未映射接口（无类型证据）：只有原始 offset + 计数 + 样例，⛔ 没有接口名 / 方法名。',
  'vtable 的判据是**形态** `(*(…)(… + off))(`；函数指针表 / 手工跳转也可能长这样，不保证每一处都是 COM 方法。',
  '调用点是静态计数，不是路径命中：`call` 出现几次就是几次，不代表运行时会走到。',
  '`LoadLibraryA/W` 的调用点**同时**计入「导入调用点」与「动态解析站点」——同一个 `call`，两张表两个视角，不是重复计数。',
  '动态站点的 `arg` 是窗口内**最近**的 `push offset … ; "…"`；`.lst` 没有作用域信息，同一函数里它可能来自上一个调用 ⇒ 只当候选字符串，不当配对结论。',
  '不猜 DLL：DLL 名只从 `; Imports from <dll>` 取，⛔ 不按函数名反推 DLL。',
  '不猜接口 / 不填占位名：解不出来的记 `unresolved` / `arg=null`，并计入诚实项。',
  '只读：不写 `corpus/`、`data/`、`apps/`；不跑子进程、不联网。',
  '确定性：同一输入两次输出逐字节相同（排序：DLL 名 → 函数名；offset 升序；EA 升序；无时间戳）。',
  '`.c` 侧没有地址（Hex-Rays 不打印 EA）⇒ `.c` 兜底抽出的动态解析站点 `ea` 为 null。',
  '本工具是**普查**，不是 spec：它给坐标与计数，语义结论要过 `AGENTS.md` §6 准入门。',
];

// ─────────────────────────────────────────────────────────── 自描述（schema 唯一真源）

const FIELDS = [
  { name: 'schema', type: 'number', meaning: `输出 schema 版本（当前 ${SCHEMA}）；改字段语义就改它` },
  { name: 'tool', type: 'string', meaning: '恒为 `platform`' },
  { name: 'modules[]', type: 'object[]', meaning: '一个语料对一条（按 `module` 键升序）' },
  { name: 'modules[].module', type: 'string', meaning: '模块键（`lib/catalog.mjs` 的 `listModules().key`：`age` / `agerc-dll`）' },
  { name: 'modules[].file', type: 'string', meaning: '语料对名（`.lst` / `.c` 去掉后缀）' },
  { name: 'modules[].sources', type: '{lst,c}', meaning: '相对仓库根的语料路径（`.c` 不在场 ⇒ null）' },
  { name: 'imports.dllCount', type: 'number', meaning: '导入块里的 DLL 数（`; Imports from <dll>` 的**去重**组数）' },
  { name: 'imports.functionCount', type: 'number', meaning: '导入块里的条目数（`dd ?` + `extrn …:dword`）' },
  { name: 'imports.declCounts', type: '{dd,extrn}', meaning: '两种条目声明形态各多少条（形态差异是语料事实，不是我的选择）' },
  { name: 'imports.duplicateSymbols', type: 'string[]', meaning: '同一 `symbol` 出现在多个 DLL 的（去重）——出现时调用点只归第一个，明说而不是静默' },
  { name: 'imports.byDll[]', type: 'object[]', meaning: '按 DLL 名升序；每组的 `functions[]` 按函数名升序' },
  { name: 'imports.byDll[].functions[].name', type: 'string', meaning: '**语料里的声明名原样**（可能是 `__imp_X`）' },
  { name: 'imports.byDll[].functions[].symbol', type: 'string', meaning: '调用点匹配用的符号名（只去 `__imp_` 前缀）' },
  { name: 'imports.byDll[].functions[].iat', type: 'number|null', meaning: 'IAT 槽的 **EA**（`.lst` 行首的地址；十六进制只用于人读）' },
  { name: 'imports.byDll[].functions[].decl', type: "'dd'|'extrn'", meaning: '该条目在语料里的声明形态' },
  { name: 'imports.byDll[].functions[].calls', type: '{thunk,iat,direct,total}', meaning: '归到它的调用点计数（按形态）；`total` 是三者和' },
  { name: 'imports.byDll[].functions[].callForm', type: "'thunk'|'iat'|'both'|'direct'|null", meaning: '形态摘要：thunk+iat ⇒ `both`；含裸名 ⇒ 加 `+direct`；一次都没调用 ⇒ null' },
  { name: 'imports.byDll[].functions[].samples', type: '{thunk,iat,direct}', meaning: '每种形态**首个**站点 EA（没有 ⇒ null；只用于抽查，不是全部站点）' },
  { name: 'imports.jmpToImport', type: 'number', meaning: '`jmp` 尾跳到导入的次数（**不计入** `calls`，单列）' },
  { name: 'imports.unattributed', type: '{thunk,iat,direct,*Names}', meaning: '归不到导入的调用点：各形态次数 + 去重名字（升序）' },
  { name: 'vtable.siteCount', type: 'number', meaning: '`(*(…)(… + off))(` 形态的站点数（同一行多处分别计数）' },
  { name: 'vtable.histogram[]', type: '{offset,count}', meaning: 'offset 直方图，**offset 升序**（offset 单位 = 字节）' },
  { name: 'vtable.samples[]', type: '{offset,count,line,caller}', meaning: '前 N（缺省 20）个 offset（**按 count 降序**，同数按 offset 升序）：首个站点的行号 + 可选 caller' },
  { name: 'vtable.caller', type: '{available,resolved,unresolved}', meaning: 'caller 是否可用与解析覆盖（不可用 ⇒ 只有行号）' },
  { name: 'vtable.candidateLines', type: 'number', meaning: '`.c` 里含 `(*(` 的行数（候选）' },
  { name: 'vtable.withoutOffset', type: 'number', meaning: '含 `(*(` 但没有 `+ <off>))(` 形态的处数（不是 vtable 站点）' },
  { name: 'dynamic.source', type: "'lst'|'c'", meaning: '站点来自哪一侧（`.lst` 优先；`.lst` 一个都没有才用 `.c` 兜底）' },
  { name: 'dynamic.windowLines', type: 'number|null', meaning: '`.lst` 侧向前找 `push offset … ; "…"` 的行数上限（`.c` 兜底 ⇒ null）' },
  { name: 'dynamic.apiCounts[]', type: '{api,count}', meaning: '按 API 名升序的站点数' },
  { name: 'dynamic.sites[]', type: '{api,ea,line,arg,prevArg,caller}', meaning: '站点，**按 EA 升序**（`.c` 兜底 ⇒ ea=null 按行号升序）；`arg`=最近 push 字符串，`prevArg`=次近（**不是**库名断言）' },
  { name: 'dynamic.unresolved', type: 'number', meaning: '窗口里没有 push 字符串 ⇒ `arg=null` 的站点数' },
  { name: 'dynamic.jmpSkipped', type: 'number', meaning: '`jmp` 尾跳到这三个 API 的次数（**不计入** `sites`）' },
  { name: 'ignored', type: 'object', meaning: '各类**明确不计入**的行数（ds 自动数据名 / 寄存器 / 间接 / 自动代码标签 / 归不到导入的三种形态 / jmp）' },
  { name: 'honesty[]', type: 'string[]', meaning: '该模块的诚实项（带本模块的实际数字）' },
];

const INVARIANTS = [
  {
    id: 'deterministic',
    text: '同一输入两次运行**逐字节相同**：模块按 key、DLL 按名、函数按名、offset 升序、EA 升序；无时间戳 / 无随机 / 无绝对路径。',
    enforcedBy: '`node --test tools/test/platform.test.mjs`（同一 fixture 两次 JSON.stringify）+ `census --json` 二次比对',
  },
  {
    id: 'no-guess',
    text: 'DLL 名只来自 `; Imports from <dll>`；接口名 / 方法名**一个都不写**；解不出的字符串不填占位名（`arg=null` + `unresolved`）。',
    enforcedBy: '`node --test tools/test/platform.test.mjs`（动态解析未解析样例）+ 本文件没有接口名常量',
  },
  {
    id: 'call-forms',
    text: '`j_<Name>` ⇒ thunk；`ds:<Name>`（非 `(dword|off|byte|word|unk)_[0-9A-Fa-f]+`）⇒ iat；裸名且**精确等于**导入符号 ⇒ direct；其余不计（分类计数）。',
    enforcedBy: '`node --test tools/test/platform.test.mjs`（`call ds:dword_55E1B4` 不计入）',
  },
  {
    id: 'thunk-dedup-suffix',
    text: '`j_<Name>_<n>` 仅在精确匹配失败时去掉尾部 `_<n>` 再匹配（IDA 重复 thunk 后缀）；`direct` / `iat` 形态不做这一步。',
    enforcedBy: '`node --test tools/test/platform.test.mjs`（`j_CreateEventA_2` ↔ 导入 `CreateEventA`）',
  },
  {
    id: 'vtable-offset-is-innermost',
    text: 'vtable offset = `(*(` 之后**第一个** `+ <num>))(` 的 num；更早的 `+ num`（对象字段偏移）不取。',
    enforcedBy: '`node --test tools/test/platform.test.mjs`（`*(_DWORD *)(this + 697620) + 4` ⇒ 4）',
  },
  {
    id: 'unparsed-counted',
    text: '解析不了的**不静默丢**：调用点进 `ignored`，vtable 进 `withoutOffset`，动态解析进 `unresolved`，`jmp` 单列。',
    enforcedBy: '`node --test tools/test/platform.test.mjs`（未解析样例断言 `unresolved===1`）',
  },
  {
    id: 'read-only-corpus',
    text: '语料只读：不写 `corpus/`；唯一写盘是 `census --out` 的派生 JSON（写后回读复验，缺省不写）。',
    enforcedBy: '本文件的写盘只有一个 `writeFileSync`（`--out` 分支）',
  },
  {
    id: 'honest-sources',
    text: '`.c` 没有地址 ⇒ `.c` 兜底的站点 `ea=null`；`.lst` 优先，`.lst` 零站点才兜底，并在输出里明写 `source`。',
    enforcedBy: '`buildCensus` 的 `source` 字段 + `honesty[]`',
  },
];

export function describe() {
  return {
    schema: SCHEMA,
    tool: 'tools/platform.mjs',
    domain: DOMAIN.id,
    purpose:
      '从反汇编语料**机械**抽出三张平台面表：① 导入函数（按 `; Imports from <dll>` 分组）' +
      '② COM vtable 调用点（按 `+ <offset>` 计数）③ 动态解析（`LoadLibrary` / `GetProcAddress` 站点）。' +
      '★ 只做抽取与展示，**不做语义裁决**：输出里没有接口名 / 方法名 / 用途结论。',
    sources: [
      { file: 'corpus/disasm/files/<module>.lst', what: '导入块 / `call` 调用点 / 动态解析站点（**每个 EA 都能回查**）', readOnly: true },
      { file: 'corpus/disasm/files/<module>.c', what: 'vtable 调用点 / caller 行区间 / 动态解析兜底（**没有 EA**）', readOnly: true },
      { file: 'lib/catalog.mjs', what: 'EA → 函数符号、`.c` 行号 → 函数符号（复用，不复刻第二份 `.lst` 文法）', readOnly: true },
    ],
    notHere: [
      '不判断"这个导入是干什么的"、不把 vtable offset 翻译成接口方法名（没有类型证据）。',
      '不给路径命中 / 运行时证据：计数是静态的。',
      '不是 spec：语义结论要过 `AGENTS.md` §6 准入门（锚 EA + 可再校验观察）。',
      '不写语料、不改仓库任何文件；`--out` 只写派生 JSON（`.cache/` 是生成物区）。',
    ],
    cli: [
      'pnpm tools platform describe',
      'pnpm tools platform census [--json] [--module age|agerc|both] [--out <file>]',
      'node tools/platform.mjs --describe [--json]',
      'node tools/platform.mjs --census [--json] [--module age|agerc|both] [--out <file>]',
      '★ `--module` 接模块键或文件名前缀（`age` / `agerc`）；缺省 age+agerc；`--files-dir` 可换语料目录（缺文件 ⇒ 退出码 2 + 期望路径）',
    ],
    importForms: [
      { syntax: '; Imports from <dll>', meaning: '开一组；DLL 名就是分类（⛔ 不按函数名猜）' },
      { syntax: '<Name> dd ?  [; Import by ordinal N]', meaning: '数据段里的导入槽（AGE 形态）；地址 = 行首 EA' },
      { syntax: 'extrn <Name>:dword', meaning: '命名 IAT（AGERC 形态）；同样是导入条目' },
      { syntax: 'align 8 / db 0 / 空行', meaning: '结束本组（之后的无名数据不会被错记到上一个 DLL）' },
    ],
    callForms: [
      { syntax: 'call j_<Name>', meaning: 'thunk 调用（form=thunk）' },
      { syntax: 'call ds:<Name>', meaning: '命名 IAT 调用（form=iat）；`ds:dword_*` 等自动数据名**不计**' },
      { syntax: 'call <Name>（裸名）', meaning: 'form=direct；**仅当**前缀/符号精确等于某导入符号才归它（实测 AGE 存在，任务书只写了前两种）' },
      { syntax: 'jmp …', meaning: '尾跳：单列 `jmpToImport` / `jmpSkipped`，⛔ 不进调用点表' },
    ],
    fields: FIELDS,
    invariants: INVARIANTS,
    operations: OPERATIONS.map((o) => ({ name: o.name, argv: o.argv, mutates: o.mutates, summary: o.summary })),
    honesty: GLOBAL_HONESTY,
  };
}

export function describeText() {
  const d = describe();
  const L = [];
  L.push(`platform — ${d.tool}（域 ${d.domain}；schema ${d.schema}）`);
  L.push('');
  L.push(`目的　${d.purpose}`);
  L.push('');
  L.push('数据源（只读）');
  for (const s of d.sources) L.push(`  ${s.file.padEnd(46)} ${s.what}`);
  L.push('');
  L.push('不做什么');
  for (const x of d.notHere) L.push(`  · ${x}`);
  L.push('');
  L.push('命令行');
  for (const x of d.cli) L.push(`  ${x}`);
  L.push('');
  L.push('导入块文法（`.lst`）');
  for (const g of d.importForms) L.push(`  ${g.syntax.padEnd(44)} ${g.meaning}`);
  L.push('');
  L.push('调用点文法（`.lst`）');
  for (const g of d.callForms) L.push(`  ${g.syntax.padEnd(44)} ${g.meaning}`);
  L.push('');
  L.push(`字段（\`census --json\` 的 schema 唯一真源）`);
  for (const f of d.fields) L.push(`  ${f.name.padEnd(44)} ${f.type.padEnd(30)} ${f.meaning}`);
  L.push('');
  L.push('不变量');
  for (const inv of d.invariants) {
    L.push(`  ${inv.id.padEnd(24)} ${inv.text}`);
    L.push(`  ${''.padEnd(24)} 守：${inv.enforcedBy}`);
  }
  L.push('');
  L.push('操作');
  for (const o of d.operations) L.push(`  ${o.name.padEnd(10)}${o.mutates ? '(会写)' : '      '} ${o.summary}`);
  L.push('');
  L.push('诚实项');
  for (const h of d.honesty) L.push(`  · ${h}`);
  return `${L.join('\n')}\n`;
}

// ─────────────────────────────────────────────────────────── CLI

const HELP = `tools/platform.mjs — 平台面普查（只读）：导入函数 / COM vtable 调用点 / 动态解析

  node tools/platform.mjs --describe [--json]
  node tools/platform.mjs --census [--json] [--module age|agerc|both] [--out <file>]
  pnpm tools platform <describe|census> [args…]

  --module <键>   只看一个模块（缺省 age+agerc；键或文件名前缀，如 agerc）
  --json          机器读（排序确定、无时间戳）
  --out <文件>    把同一份 JSON 写盘（缺省只打印；写后回读逐字节复验）
  --files-dir <目录>  换语料目录（缺省 corpus/disasm/files/；缺文件 ⇒ 退出码 2 + 期望路径）

★ 只做抽取与展示：⛔ 不写接口名 / 语义结论；解不出的进 honesty / ignored / unresolved。
★ 语料只读。唯一写盘是 --out 的派生 JSON。
`;

function parseArgs(argv) {
  const out = { op: null, json: false, module: null, out: null, filesDir: DEFAULT_OUT_DIR };
  const OPS = new Set(OPERATIONS.map((o) => o.name));
  const takesValue = new Set(['module', 'out', 'files-dir']);
  for (let i = 0; i < argv.length; i += 1) {
    const a = argv[i];
    if (a === '--help' || a === '-h') out.op = 'help';
    else if (a === '--json') out.json = true;
    else if (a.startsWith('--') && OPS.has(a.slice(2))) out.op = a.slice(2);
    else if (a.startsWith('--')) {
      const key = a.slice(2);
      if (!takesValue.has(key)) throw new Error(`未知参数：${a}（--help 看用法）`);
      const value = argv[i + 1];
      if (value === undefined) throw new Error(`${a} 需要一个值`);
      i += 1;
      if (key === 'files-dir') out.filesDir = path.resolve(value);
      else out[key] = value;
    } else if (out.op === null && OPS.has(a)) out.op = a;
    else throw new Error(`多余的参数：${a}（--help 看用法）`);
  }
  return out;
}

/** 缺语料 ⇒ 抛错（CLI 层转成退出码 2），错误信息里必须写出**期望路径** */
function resolveModules(filesDir, wanted) {
  const mods = listModules(filesDir);
  if (mods.length === 0) {
    throw new Error(
      `找不到反汇编语料：${filesDir} 下没有 *.lst\n` +
        `  期望：${path.join(filesDir, 'AGE.EXE__dumped.sectfix.EXE.lst')} 与 ${path.join(filesDir, 'AGERC.DLL.lst')}\n` +
        '  （`corpus/disasm/files/` 是本地解压产物，不入库 ⇒ 先 `pnpm tools disasm restore`）',
    );
  }
  const keys = wanted && wanted !== 'both' ? [wanted] : DEFAULT_MODULE_KEYS;
  return keys.map((key) => {
    const mod = resolveModule(filesDir, key);
    if (!mod) {
      throw new Error(
        `找不到模块 ${JSON.stringify(key)}：${filesDir} 下只有 ${mods.map((m) => m.key).join(' / ')}\n` +
          `  期望：${path.join(filesDir, `${key}.lst`)}（或文件名以 ${key} 开头的语料对）`,
      );
    }
    if (!mod.cFile) {
      throw new Error(`模块 ${mod.key} 缺同名 .c：期望 ${mod.lstFile.replace(/\.lst$/, '.c')}（vtable 表要 .c 侧）`);
    }
    return mod;
  });
}

/** `.c` 行号 → 函数符号：在 `cat.c.funcs`（按 from 升序）上二分；解不出 ⇒ null */
function lineLookup(cFuncs) {
  const defs = [...cFuncs].sort((a, b) => a.from - b.from);
  const starts = defs.map((d) => d.from);
  return (line) => {
    let lo = 0;
    let hi = starts.length - 1;
    let hit = -1;
    while (lo <= hi) {
      const mid = (lo + hi) >> 1;
      if (starts[mid] <= line) {
        hit = mid;
        lo = mid + 1;
      } else hi = mid - 1;
    }
    if (hit < 0) return null;
    const def = defs[hit];
    return line <= def.to ? def.name : null;
  };
}

/** 一个模块的 caller 查表器（失败 ⇒ `{note}`：降级为"caller 未解析"，不猜） */
function callerMaps(mod) {
  try {
    const cat = buildCatalog({ lstFile: mod.lstFile, cFile: mod.cFile, moduleKey: mod.key });
    const cFuncs = cat.c?.funcs ?? [];
    return {
      callerAtLine: cFuncs.length > 0 ? lineLookup(cFuncs) : null,
      callerAtEa: cat.functions.length > 0 && Array.isArray(cat.starts) ? (ea) => functionOfEa(cat, ea) : null,
      note: null,
    };
  } catch (err) {
    return { callerAtLine: null, callerAtEa: null, note: `caller 未解析（catalog 构建失败：${err.message}）` };
  }
}

function runCensus(args) {
  const mods = resolveModules(args.filesDir, args.module);
  const censuses = [];
  for (const mod of mods) {
    const lstLines = fs.readFileSync(mod.lstFile, 'utf8').split('\n');
    const cLines = fs.readFileSync(mod.cFile, 'utf8').split('\n');
    const maps = callerMaps(mod);
    const census = buildCensus({
      key: mod.key,
      title: mod.base,
      lstLines,
      cLines,
      callerAtLine: maps.callerAtLine,
      callerAtEa: maps.callerAtEa,
    });
    census.sources = { lst: rel(mod.lstFile), c: rel(mod.cFile) };
    if (maps.note) census.honesty.push(maps.note);
    censuses.push(census);
  }
  const doc = censusDocument(censuses);
  const json = `${JSON.stringify(doc, null, 2)}\n`;

  if (args.out === null) {
    process.stdout.write(args.json ? json : censusText(doc));
    return 0;
  }
  const abs = path.resolve(args.out);
  fs.mkdirSync(path.dirname(abs), { recursive: true });
  fs.writeFileSync(abs, json, 'utf8');
  const back = fs.readFileSync(abs, 'utf8');
  if (back !== json) throw new Error(`回读不一致：${rel(abs)}（写盘坏了，别信这份产物）`);
  if (args.json) process.stdout.write(json);
  process.stdout.write(`已写 ${rel(abs)}（${Buffer.byteLength(json, 'utf8')} 字节；回读逐字节一致）\n`);
  return 0;
}

function run(argv) {
  const args = parseArgs(argv);
  if (!args.op) {
    process.stdout.write(HELP);
    return 2;
  }
  if (args.op === 'help') {
    process.stdout.write(HELP);
    return 0;
  }
  if (args.op === 'describe') {
    process.stdout.write(args.json ? `${JSON.stringify(describe(), null, 2)}\n` : describeText());
    return 0;
  }
  if (args.op === 'census') return runCensus(args);
  throw new Error(`未知动作：${args.op}`);
}

export function main(argv = process.argv.slice(2)) {
  try {
    return run(argv);
  } catch (err) {
    process.stderr.write(`ERROR: ${err.message}\n`);
    return 2;
  }
}

const isDirectRun = process.argv[1] && path.resolve(process.argv[1]) === path.resolve(fileURLToPath(import.meta.url));
if (isDirectRun) {
  process.exitCode = main();
}
