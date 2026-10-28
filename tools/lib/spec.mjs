/**
 * tools/lib/spec.mjs —— **spec 层**（领域本体）的 schema / 不变量 / effect 覆盖 / 派生报告 / 自描述
 *
 * ## 三份文件，各司其职（2026-10 拆分）
 * * `data/spec/spec.json` —— **规范正文**：章节 + 规则（合同 / 状态对应 / 条件 / 可证伪预言）。
 *   它只回答"引擎是什么"，⛔ 不含处理状态、覆盖账、残差。
 * * `data/spec/bindings.json` —— **机器绑定**：每条规则的站点 EA / 函数 / `storage` 指针 / 证据。
 *   它是规范的"出处"，不是规范本身；布局只经 `storage` **指针**引用（`project-intent.md` §3）。
 * * `data/spec/audit.json` —— **范围审计**：试点块、effect 归属（`covers` 选择器 / internal / residuals）、处理状态。
 *   它回答"处理/替换/舍弃了哪些机器行为、哪些还没处理"；**不混进规范正文**。
 *
 * 章节里的规则按 `rule id` 与 bindings / audit 一一对应（由 `validate` 的 #12 守）。
 *
 * ## 三条不让步的口径（都由 `validate` 执行）
 * 1. **处理状态必须可区分**（在 audit 里）：`handled` / `replaced` / `omitted` / `discarded` / `unhandled`。
 *    `discarded` 与 `unknown` **分列**；未知不得静默变成省略或舍弃。
 * 2. **未登记义务 = 0**（只在 `pilot` 域内）：每个 effect 要么被某条规则的 `covers` 命中，
 *    要么属于 `pilot.internal`（有理由），要么在 `pilot.residuals` 里点名（带 next）。
 * 3. **正文不内联布局**：合同 / 上下文 / 状态对应里出现绝对地址 / 记录内偏移一律红；
 *    那些只能出现在 `bindings.storage`（指针）与语料里。
 *
 * ## 它**不**提供什么（诚实边界）
 * * 不判断"这条合同是否与原引擎等价"—— 没有任何自动判据能证明这一点（`project-intent.md` §2/§4）。
 * * 不证明 selector 召回完整：试点块是**声明**的，effect 枚举器有边界（见 `x86-effects` 文件头）。
 * * 不给覆盖率百分数：报告只给**计数与清单**（范围闭合 ≠ 解释正确）。
 */
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from './paths.mjs';
import { cmakeTestTier, isCmakeGuardFile, project, readRecords, testNames } from './ledger.mjs';
import { pickListing } from './disasm.mjs';
import { computeEffectCoverage, derivePilotEffects, selectorMatches } from './spec-effects.mjs';

/** 规范真源：manifest + 章节 Markdown + 两份内部数据 + 生成物 */
export const SPEC_FILE = 'data/spec/spec.json';
export const BINDINGS_FILE = 'data/spec/bindings.json';
export const AUDIT_FILE = 'data/spec/audit.json';
export const DEFAULT_DIR = 'data/spec';

/** 处理状态：**闭集合**。`replaced` = 抽象替换（放电）；`discarded` = 项目选择不保留。 */
export const DISPOSITIONS = ['handled', 'replaced', 'omitted', 'discarded', 'unhandled'];
/** 条目成熟度：`draft` = 已写下但证据链未全部过 accepted；`reviewed` = 全部证据 accepted。 */
export const ENTRY_STATUSES = ['draft', 'reviewed'];
/** 残差类别：未知 / 未处理 / 有依据的省略 / 明确舍弃（★ 与 `DISPOSITIONS` 同义词但**不共用**：
 *  残差描述"这一项没被解释"，处理状态描述"这一条合同怎么处理了它的范围"）。 */
export const RESIDUAL_KINDS = ['unknown', 'unhandled', 'omitted', 'discarded'];
/** 章节成熟度：`none`（未写）/ `partial`（有概念但无规则）/ `bootstrap`（有成篇规则） */
export const CHAPTER_STATUSES = ['none', 'partial', 'bootstrap'];

/** 证据前缀：`KN:<ULID>` 指向知识台账；`guard:<path>#<用例名片段>` 指向可执行守卫 */
export const EVIDENCE_PREFIXES = ['KN:', 'guard:'];

/**
 * 本规范的指令表（`format/opcode-table.json`）：`hasHandler: true` 的条目 = **应当有成篇语义**的闭集。
 * ★ 没有 handler 的条目执行即落缺省，不是"待写语义"的义务；所以未成篇的分母只取 `hasHandler`。
 */
export const OPCODE_TABLE_FILE = 'data/spec/format/opcode-table.json';
/** 未成篇 opcode 的分段粒度：一段 = 一个 0x100 区间（**机械**区间分组，不声称族身份）。 */
export const GAP_OPCODE_BLOCK = 0x100;
/** `audit.gaps[].ops` 的单项形态（opcode 的十六进制字面量） */
const OPCODE_TEXT_RE = /^0x[0-9a-fA-F]+$/;
const opcodeHex = (n) => `0x${n.toString(16)}`;
const hex4 = (n) => n.toString(16).toUpperCase().padStart(4, '0');

/**
 * **规范正文里不许出现的复核/实现信息**（分发的产物必须自包含、不引用汇编与本仓材料）。
 * ★ 只禁"机器层引用"；opcode 号（`0x107`）与类型标签（`0x8003`）是引擎语义，允许。
 */
export const REVIEW_INFO_PATTERNS = [
  { re: /\bsub_[0-9A-F]{6}\b/, why: '函数符号（汇编层）' },
  { re: /\bKN:[0-9A-HJKMNP-TV-Z]{26}\b/, why: '台账记录 id' },
  { re: /\bguard:[^\s`)]+/, why: '守卫用例引用' },
  { re: /\bEngine\s*\+\s*0x[0-9a-f]+/i, why: '绝对地址' },
  { re: /(?:帧|记录)\s*\+\s*0x[0-9a-f]+/i, why: '记录内偏移' },
  { re: /\b0x5[0-9a-f]{4}\b/i, why: '绝对地址常数' },
  { re: /\blayout:/, why: 'layout 指针' },
  { re: /\bshape:/, why: 'shape 指针' },
  { re: /\bdata\/(?:ledger|shape|spec)\b/, why: '本仓复核数据路径' },
  { re: /\bcorpus\/|\.lst\b/, why: '语料路径' },
  { re: /<!--\s*rule:/, why: '规则标记（渲染时应剥离）' },
];

const EA_RE = /^0x[0-9a-f]+$/i;
const FN_RE = /^sub_[0-9A-F]{6}$/;
/**
 * `shape:` 指针的两种形态：
 * * `shape:<类型>` —— 状态存在于该类型的实例里（类型身份）；
 * * `shape:<类型>@0x<off>` —— 状态就是该类型成员表里**起点为 off 的那个成员**（成员身份）。
 * ★ 成员形态必须是**声明过的起点**，不许"落在某个数组里面"：那样指针会把数组当成单格。
 */
const SHAPE_REF_RE = /^shape:([A-Za-z_][A-Za-z0-9_]*)(?:@(0x[0-9a-fA-F]+))?$/;
const ULID_RE = /^[0-9A-HJKMNP-TV-Z]{26}$/;
/** 规则标记：`<!-- rule: SPC-... -->`（Markdown 注释，分发时剥离） */
const RULE_MARKER = /<!--\s*rule:\s*([A-Za-z0-9_-]+)\s*-->/;
/** 机器范围的一项：EA 或函数符号（替换 / 舍弃的 range 只认这两类） */
const machineToken = (v) => (typeof v === 'string' && (EA_RE.test(v) || FN_RE.test(v)) ? v : null);

const rel = (p) => path.relative(REPO_ROOT, p).split(path.sep).join('/');
const hex = (n) => `0x${(n >>> 0).toString(16)}`;
const readJson = (file) => JSON.parse(fs.readFileSync(file, 'utf8'));

/**
 * 读 manifest + 章节 Markdown + 两份内部数据，并合并成校验/渲染用的模型。
 *
 * ★ 规范正文是 **Markdown 章节**（`data/spec/chapters/*.md`）——它是可分发的东西；
 *   `bindings.json` / `audit.json` 是内部复核数据，**不进入分发产物**。
 */
export function loadSpec({ dir = DEFAULT_DIR, spec, bindings, audit } = {}) {
  const at = (value, fallback) => {
    const p = value ?? path.join(dir, fallback);
    return path.isAbsolute(p) ? p : path.join(REPO_ROOT, p);
  };
  const specAbs = at(spec, 'spec.json');
  const bindAbs = at(bindings, 'bindings.json');
  const auditAbs = at(audit, 'audit.json');
  const manifest = readJson(specAbs);
  const chapters = (manifest?.chapters ?? []).map((c) => ({
    ...c,
    text: fs.readFileSync(path.resolve(path.dirname(specAbs), c.file), 'utf8'),
  }));
  const bundle = { spec: manifest, bindings: readJson(bindAbs), audit: readJson(auditAbs), chapters };
  return {
    ...bundle,
    model: mergeSpec(bundle.spec, bundle.bindings, bundle.audit, bundle.chapters),
    files: { spec: rel(specAbs), bindings: rel(bindAbs), audit: rel(auditAbs), chapters: rel(path.dirname(specAbs)) + '/chapters' },
  };
}

/**
 * 从一章 Markdown 里抽取规则：`### 标题` + `<!-- rule: ID -->` + 正文。
 * 标记必须是该 `###` 小节里的第一处（允许标题与标记之间有空行）。
 */
export function parseChapterRules(chapter) {
  const out = [];
  // 按 `##` / `###` 切段；每段自带标题。
  const parts = String(chapter.text ?? '').split(/^(?=#{2,3} )/m);
  for (const part of parts) {
    const h = /^###\s+(.+?)\s*$/m.exec(part);
    if (!h) continue;
    const m = RULE_MARKER.exec(part);
    if (!m) continue;
    const body = part
      .replace(/^###\s+.+\r?\n/, '')
      .replace(m[0], '')
      .replace(/^\s*\r?\n/, '')
      .trim();
    out.push({ id: m[1], title: h[1].trim(), chapter: chapter.id, text: body });
  }
  return out;
}

/**
 * 读指令表（`data/spec/format/opcode-table.json`）。
 * ★ 表是数组；为兼容"按 opcode 编号做键"的写法，对象形态也接受。
 * @returns {{ok: boolean, why?: string, entries: object[], withHandler: object[]}}
 */
export function loadOpcodeTable({ repoRoot = REPO_ROOT } = {}) {
  const file = path.join(repoRoot, OPCODE_TABLE_FILE);
  if (!fs.existsSync(file)) return { ok: false, why: `${OPCODE_TABLE_FILE} 不在场`, entries: [], withHandler: [] };
  const raw = readJson(file);
  const entries = Array.isArray(raw) ? raw : Object.values(raw ?? {});
  return { ok: true, entries, withHandler: entries.filter((e) => e?.hasHandler === true) };
}

/**
 * **已写 opcode**：规则 `###` **标题**里的 `0x…` 数字（跨全部章节）。
 * ★ 只认标题 —— 正文里的消息号 / 位号 / 绝对地址不是 opcode；按正文推断会把它们算进来。
 * ★ 标题里也可能出现 operand type 标签（如 `0x8003`）；调用方拿它与 `hasHandler` 求交即可。
 */
export function titledOpcodes(chapters) {
  const out = new Set();
  for (const c of chapters ?? []) {
    for (const r of parseChapterRules(c)) {
      for (const m of String(r.title ?? '').matchAll(/0x[0-9a-fA-F]+/g)) out.add(Number.parseInt(m[0], 16));
    }
  }
  return out;
}

/**
 * 未成篇 opcode 的**机械账**：`hasHandler` = 已写（规则标题）+ gaps（`ops`）。
 * ★ 报告与派生只认这一份账；⛔ 不给百分数（范围闭合 ≠ 解释正确）。
 * ★ `consistent` 为 false 时不抛异常 —— 由报告标 ⚠、由守卫 `tools/test/spec-gaps.test.mjs` 变红。
 */
export function opcodeAccounting(model, { repoRoot = REPO_ROOT } = {}) {
  const table = loadOpcodeTable({ repoRoot });
  if (!table.ok) return { ok: false, why: table.why };
  const handler = new Set(table.withHandler.map((e) => e.opcode));
  const written = titledOpcodes(model.chapters);
  /** opcode → 声明它的 gap id（数组；长度 > 1 就是重复） */
  const owners = new Map();
  for (const g of model.gaps ?? []) {
    for (const op of g.ops ?? []) {
      if (typeof op !== 'string' || !OPCODE_TEXT_RE.test(op)) continue;
      const n = Number.parseInt(op.slice(2), 16);
      if (!owners.has(n)) owners.set(n, []);
      owners.get(n).push(g.id);
    }
  }
  const overlap = [...handler].filter((n) => written.has(n) && owners.has(n));
  const outsideTable = [...owners.keys()].filter((n) => !handler.has(n));
  const duplicates = [...owners].filter(([, ids]) => ids.length > 1).map(([n]) => opcodeHex(n));
  const writtenCount = [...handler].filter((n) => written.has(n)).length;
  const gapsCovered = [...handler].filter((n) => owners.has(n)).length;
  const unwritten = handler.size - writtenCount;
  return {
    ok: true,
    total: handler.size,
    written: writtenCount,
    gapsCovered,
    unwritten,
    overlap: overlap.map(opcodeHex),
    outsideTable: outsideTable.map(opcodeHex),
    duplicates,
    consistent: unwritten === gapsCovered && overlap.length === 0 && outsideTable.length === 0 && duplicates.length === 0,
  };
}

/**
 * **机械派生**"未成篇 opcode 族/段"的 gaps（⛔ 不手写）：
 * `hasHandler` 减去**规则标题里出现的 opcode**，再按 {@link GAP_OPCODE_BLOCK} 分段。
 * ★ 只派生 opcode 级的 gap；非 opcode 级的 gap（如容器句柄分支）不在这里，也**不带 `ops`**。
 * ★ 分段边界是算术（0x100 区间），不是声称的族身份 —— `note` 里写明这一点。
 */
export function deriveOpcodeGaps(chapters, { repoRoot = REPO_ROOT } = {}) {
  const table = loadOpcodeTable({ repoRoot });
  if (!table.ok) return { ok: false, why: table.why, gaps: [] };
  const handler = [...new Set(table.withHandler.map((e) => e.opcode))].sort((a, b) => a - b);
  const written = titledOpcodes(chapters);
  const unwritten = handler.filter((n) => !written.has(n));
  const blocks = new Map();
  for (const n of unwritten) {
    const start = Math.floor(n / GAP_OPCODE_BLOCK) * GAP_OPCODE_BLOCK;
    if (!blocks.has(start)) blocks.set(start, []);
    blocks.get(start).push(n);
  }
  const gaps = [...blocks.entries()]
    .sort((a, b) => a[0] - b[0])
    .map(([start, ops]) => {
      const end = start + GAP_OPCODE_BLOCK - 1;
      const id = `GAP-OPS-${hex4(start)}-${hex4(end)}`;
      return {
        id,
        chapter: 'ops-unwritten',
        title: `opcode 0x${hex4(start)}–0x${hex4(end)} 段：尚无规范条目`,
        note: `本段 ${ops.length} 个有处理函数的 opcode 尚无任何规则标题覆盖（未成篇，尚未逐条写成规范合同）；分段只是 0x100 区间的机械分组，不代表族身份。`,
        ops: ops.map(opcodeHex),
        residual: [
          {
            id: `RES-${id}`,
            kind: 'unhandled',
            what: `本段 ${ops.length} 个有处理函数的 opcode 尚未成篇（\`ops\` 列出全部）`,
            next: '按族取证：先从被真实脚本足迹命中的 opcode 起逐条读 handler + 语料站点，写最小合同（输入 / 状态改变 / 边界），再把该 opcode 从本 gap 的 `ops` 里移出',
          },
        ],
      };
    });
  return { ok: true, gaps, total: handler.length, written: handler.filter((n) => written.has(n)).length, unwritten: unwritten.length };
}

/** 三份数据 + 章节 → 校验/渲染用的模型（缺项用空值兜底；缺项本身由 `crossFileProblems` 报） */
export function mergeSpec(spec, bindings, audit, chapters) {
  const rules = (chapters ?? []).flatMap((c) => parseChapterRules(c));
  const bindBy = new Map((bindings?.bindings ?? []).map((b) => [b.rule, b]));
  const auditBy = new Map((audit?.rules ?? []).map((a) => [a.id, a]));
  const auditChapters = new Map((audit?.chapters ?? []).map((c) => [c.id, c]));
  const entries = rules.map((r) => {
    const b = bindBy.get(r.id) ?? {};
    const a = auditBy.get(r.id) ?? {};
    return {
      ...r,
      binding: {
        sites: b.sites ?? [],
        functions: b.functions ?? [],
        storage: b.storage ?? [],
        evidence: b.evidence ?? [],
      },
      status: a.status,
      disposition: a.disposition,
      covers: a.covers ?? [],
      residual: a.residual ?? [],
      falsifiers: a.falsifiers ?? [],
      ...(a.replacement !== undefined ? { replacement: a.replacement } : {}),
      ...(a.omission !== undefined ? { omission: a.omission } : {}),
      ...(a.discard !== undefined ? { discard: a.discard } : {}),
    };
  });
  const textById = new Map((chapters ?? []).map((c) => [c.id, c.text]));
  const mergedChapters = (spec?.chapters ?? []).map((c) => {
    const a = auditChapters.get(c.id) ?? {};
    return { ...c, text: textById.get(c.id) ?? '', status: a.status, residual: a.residual ?? [] };
  });
  return {
    schema: spec?.schema,
    title: spec?.title,
    pilot: audit?.pilot,
    chapters: mergedChapters,
    planned: audit?.planned ?? [],
    entries,
    gaps: audit?.gaps ?? [],
  };
}

/**
 * 跨文件不变量：
 * 1. manifest 的章节文件存在且非空；
 * 2. 章节 Markdown 里的规则标记被正确解析（标记必须在 `###` 小节里、id 不重复）；
 * 3. bindings / audit 与规则按 id **一一对应**；
 * 4. audit 的 chapters 与 manifest 章节一一对应；planned/gaps 各自不重复。
 */
export function crossFileProblems(spec, bindings, audit, chapters) {
  const out = [];
  const arr = (v) => (Array.isArray(v) ? v : []);
  if (spec?.schema !== 3) out.push(`spec.json 的 schema 必须是 3（实际 ${JSON.stringify(spec?.schema)}）`);
  if (bindings?.schema !== 3) out.push('bindings.json 的 schema 必须是 3');
  if (audit?.schema !== 3) out.push('audit.json 的 schema 必须是 3');

  const specChapters = arr(spec?.chapters).map((c) => c?.id);
  if (new Set(specChapters).size !== specChapters.length) out.push('spec.json 的 chapter id 有重复');
  for (const c of arr(spec?.chapters)) {
    if (!c?.id || !c?.title || !c?.file) out.push(`章节缺 id/title/file：${JSON.stringify(c)}`);
    const ch = arr(chapters).find((x) => x?.id === c?.id);
    if (!ch) out.push(`章节 ${c?.id} 没有读到 Markdown 文件 ${c?.file}`);
    else {
      if (!String(ch.text ?? '').trim()) out.push(`章节 ${c?.id} 的文件是空的：${c.file}`);
      const markers = [...String(ch.text).matchAll(/<!--\s*rule:\s*([A-Za-z0-9_-]+)\s*-->/g)].map((m) => m[1]);
      const parsed = parseChapterRules(ch).map((r) => r.id);
      if (markers.length !== parsed.length) {
        out.push(`章节 ${c?.id} 有 ${markers.length - parsed.length} 个规则标记不在 "### 标题" 小节里（会被静默漏掉）`);
      }
    }
  }

  const rules = arr(chapters).flatMap((c) => parseChapterRules(c));
  const ruleIds = rules.map((r) => r.id);
  if (new Set(ruleIds).size !== ruleIds.length) out.push('章节 Markdown 里的 rule id 有重复');
  const ruleSet = new Set(ruleIds);
  const oneToOne = (label, items, readId) => {
    const ids = arr(items).map(readId);
    if (new Set(ids).size !== ids.length) out.push(`${label} 的 id 有重复`);
    for (const id of ids) if (!ruleSet.has(id)) out.push(`${label} 指向不存在的规则：${id}`);
    for (const id of ruleSet) if (!ids.includes(id)) out.push(`${label} 缺少 ${id}（bindings / audit 必须与章节规则一一对应）`);
  };
  oneToOne('bindings.json', bindings?.bindings, (b) => b?.rule);
  oneToOne('audit.json rules', audit?.rules, (a) => a?.id);

  const auditChapters = arr(audit?.chapters).map((c) => c?.id);
  if (new Set(auditChapters).size !== auditChapters.length) out.push('audit.json 的 chapter id 有重复');
  for (const id of auditChapters) if (!specChapters.includes(id)) out.push(`audit.json 的章节不在 manifest 里：${id}`);
  for (const id of specChapters) if (!auditChapters.includes(id)) out.push(`audit.json 缺少章节 ${id}`);

  const planned = arr(audit?.planned);
  const plannedIds = planned.map((c) => c?.id);
  if (new Set(plannedIds).size !== plannedIds.length) out.push('audit.json 的 planned chapter id 有重复');
  for (const c of planned) {
    if (!c?.id || !c?.title) out.push(`planned 章节缺 id/title：${JSON.stringify(c)}`);
    if (specChapters.includes(c?.id)) out.push(`planned 章节与已写章节重名：${c?.id}`);
  }

  if (!audit?.pilot || typeof audit.pilot !== 'object') out.push('audit.json 缺 pilot（验收域）');
  // ★ gap 的 `chapter` 可以指向**已写章节**，也可以指向 `planned`（未成篇章节）：后者正是
  //   "整族尚未成篇"的落点（如 `ops-unwritten` / `containers`）。
  const allChapters = new Set([...specChapters, ...plannedIds]);
  const gapIds = arr(audit?.gaps).map((g) => g?.id);
  if (new Set(gapIds).size !== gapIds.length) out.push('audit.json 的 gap id 有重复');
  for (const g of arr(audit?.gaps)) {
    if (!allChapters.has(g?.chapter)) out.push(`gap ${g?.id} 指向不存在的章节（已写或 planned）：${g?.chapter}`);
    if (ruleSet.has(g?.id)) out.push(`gap ${g?.id} 与规则 id 冲突`);
  }
  return out;
}

/** 读台账并投影（只用于"证据还有效吗"；本层不写台账） */
export function loadLedger() {
  const { records } = readRecords(path.join(REPO_ROOT, 'data', 'ledger'));
  return project(records, { repoRoot: REPO_ROOT });
}

/**
 * 试点块：`pilot.sites` 里的块入口 EA。effect 由 `x86-effects` 从语料机械枚举，
 * ★ 这是**声明的验收域**，不是全引擎；报告必须把"域外"与"域内未归属"分开说。
 */
/** 证据字符串 → 结构（只认两种前缀） */
export function parseEvidence(s) {
  if (typeof s !== 'string') return { ok: false, why: '证据必须是字符串' };
  if (s.startsWith('guard:')) {
    const body = s.slice('guard:'.length);
    const i = body.lastIndexOf('#');
    if (i <= 0 || i === body.length - 1) return { ok: false, why: `guard 证据要写成 guard:<path>#<用例名片段>：${s}` };
    return { ok: true, kind: 'guard', path: body.slice(0, i), test: body.slice(i + 1) };
  }
  const id = s.startsWith('KN:') ? s.slice(3) : s;
  if (ULID_RE.test(id)) return { ok: true, kind: 'ledger', id };
  return { ok: false, why: `证据无法解析（要 KN:<ULID> 或 guard:<path>#<用例>）：${s}` };
}

/**
 * 解析证据的**当前有效状态**。
 * * ledger：记录必须存在；`effective` 必须是 accepted 或 proposed（proposed 只算弱证据）。
 * * guard：文件必须在本仓、用例名片段必须真的命中一个 `test(...)` 字面量。
 */
export function resolveEvidence(ev, { ledger, repoRoot = REPO_ROOT } = {}) {
  const parsed = parseEvidence(ev);
  if (!parsed.ok) return { ok: false, kind: 'error', why: parsed.why, evidence: ev };
  if (parsed.kind === 'ledger') {
    if (!ledger) return { ok: false, kind: 'error', why: '台账未加载', evidence: ev };
    const hit = ledger.entries.find((e) => e.id === parsed.id);
    if (!hit) return { ok: false, kind: 'error', why: `台账里没有这条记录：${parsed.id}`, evidence: ev };
    const eff = hit.effective;
    if (eff === 'retracted' || eff === 'stale' || eff === 'conflict') {
      return { ok: false, kind: 'error', why: `证据失效（effective=${eff}）：${parsed.id}`, evidence: ev, effective: eff };
    }
    return { ok: true, kind: 'ledger', id: parsed.id, effective: eff, strong: eff === 'accepted', record: hit, evidence: ev };
  }
  const abs = path.join(repoRoot, parsed.path);
  if (!fs.existsSync(abs)) return { ok: false, kind: 'error', why: `守卫文件不存在：${parsed.path}`, evidence: ev };
  const text = fs.readFileSync(abs, 'utf8');
  const names = testNames(text);
  const hit = names.find((n) => n.includes(parsed.test));
  if (!hit) {
    return { ok: false, kind: 'error', why: `用例名在该文件里找不到：${parsed.test}`, evidence: ev };
  }
  // 测试分级：纯文本档的守卫才可以"现在就能跑"；assets / external 档默认门禁不跑（不能撑 reviewed）。
  // ★ 两套判据按**守卫文件形态**分派，不存在第三套：
  //   · JS 用例（`*.test.mjs`）：首行 pragma `@env pure|assets|external`；
  //   · CTest（`CMakeLists.txt` / `*.cmake`）：**注册位置**——顶层 `add_test` = pure，落在
  //     `if`/`foreach`/`function`/… 块里 = assets（`apps/emulator-cxx` 的安装流程测试就在
  //     `if(EMULATOR_CXX_INSTALL_DIR)` 里）。见 `tools/lib/ledger.mjs` 的 `cmakeTestTier`。
  const tier = isCmakeGuardFile(parsed.path)
    ? (cmakeTestTier(text, hit) ?? 'unknown')
    : (/@env\s+(pure|assets|external)/.exec(text)?.[1] ?? 'unknown');
  return { ok: true, kind: 'guard', path: relativePath(parsed.path), test: hit, strong: true, tier, evidence: ev };
}

function relativePath(p) {
  return p.split('\\').join('/');
}

/**
 * `storage` 指针是否真的存在（机器本体只认 shape）。
 * ★ 读 `data/shape/types.json`：类型身份 + **成员身份**（`@0x<off>` 必须恰是某个成员的声明起点）。
 * ⛔ 不再接受 `layout:` —— 那会让规范的可复核材料吊在历史知识产物（age-format）上；
 *   结构上拒绝 > 计数棘轮：新规则根本挂不回来。
 */
export function resolveStorage(ref) {
  const parsed = SHAPE_REF_RE.exec(ref);
  if (!parsed) return { ok: false, why: `storage 指针只认 shape:<类型> 或 shape:<类型>@0x<成员起点>：${ref}` };
  const [, typeId, offText] = parsed;
  const shapeRef = path.join(REPO_ROOT, 'data/shape/types.json');
  if (!fs.existsSync(shapeRef)) return { ok: false, why: 'types.json 不在场' };
  const raw = JSON.parse(fs.readFileSync(shapeRef, 'utf8')).types ?? {};
  // `types` 在 `types.json` 里是 **id → 类型** 的映射（不是数组）；两种形态都接受，避免把结构写死。
  const byId = Array.isArray(raw) ? new Map(raw.map((t) => [t.id, t])) : new Map(Object.entries(raw));
  const type = byId.get(typeId);
  if (!type) return { ok: false, why: `types.json 里找不到类型 ${typeId}` };
  if (offText === undefined) return { ok: true, kind: 'shape', token: typeId, type };
  const off = Number.parseInt(offText, 16);
  const member = (type.members ?? []).find((m) => Number.parseInt(m.off, 16) === off);
  if (!member) {
    return { ok: false, why: `${typeId} 的成员表里没有起点为 ${offText} 的成员（先补 shape 成员，再挂指针）` };
  }
  return { ok: true, kind: 'shape', token: `${typeId}@${offText}`, type, member };
}

const isNonEmptyString = (v) => typeof v === 'string' && v.trim() !== '';
const isNonEmptyArray = (v) => Array.isArray(v) && v.length > 0;

/**
 * 全部不变量。返回 `{checks, problems, derived}`。
 * ★ 负例（判别力）由 `tools/test/spec.test.mjs` 的样本提供：关掉任何一条都应让那条样本变红。
 */
export function validate(input, { ledger, repoRoot = REPO_ROOT, effects = null } = {}) {
  const problems = [];
  const add = (check, where, why) => problems.push({ check, where, why });
  const checks = [];
  // ★ 输入可以是**三份文件的 bundle**（真实调用），也可以是合并好的 model（单元样本）。
  const bundle = input && input.spec && input.bindings && input.audit && Array.isArray(input.chapters) ? input : null;
  const cross = bundle ? crossFileProblems(input.spec, input.bindings, input.audit, input.chapters) : [];
  const spec = bundle ? mergeSpec(input.spec, input.bindings, input.audit, input.chapters) : input;

  // ── 0. 顶层形态
  const top = [];
  if (spec?.schema !== 3) top.push('`schema` 必须是 3');
  for (const key of ['title', 'pilot', 'chapters', 'entries']) if (!(key in (spec ?? {}))) top.push(`缺顶层字段 ${key}`);
  if (!Array.isArray(spec?.chapters) || spec.chapters.length === 0) top.push('`chapters` 必须是非空数组');
  if (!Array.isArray(spec?.entries) || spec.entries.length === 0) top.push('`entries` 必须是非空数组');
  checks.push({ id: 1, text: '形态：schema / 顶层字段 / 章节与条目是非空数组', problems: top });
  for (const p of top) add(1, 'spec', p);

  const chapterIds = new Set((spec?.chapters ?? []).map((c) => c.id));
  const allChapterIds = new Set([...chapterIds, ...(spec?.planned ?? []).map((c) => c.id)]);
  const entryIds = new Set();

  // ── 1. 章节（已写 = manifest + audit；planned = 只有审计）
  const chap = [];
  for (const c of spec?.chapters ?? []) {
    if (!isNonEmptyString(c.id)) chap.push('章节缺 id');
    if (!isNonEmptyString(c.title)) chap.push(`章节 ${c.id} 缺 title`);
    if (!isNonEmptyString(c.file)) chap.push(`章节 ${c.id} 缺 file`);
    if (!CHAPTER_STATUSES.includes(c.status)) chap.push(`章节 ${c.id} 的 status 非法：${c.status}`);
    if (!Array.isArray(c.residual)) chap.push(`章节 ${c.id} 的 residual 必须是数组`);
    else for (const r of c.residual) if (!isNonEmptyString(r.next)) chap.push(`章节 ${c.id} 的残差缺 next（下一步问题）`);
  }
  if (chapterIds.size !== (spec?.chapters ?? []).length) chap.push('章节 id 有重复');
  for (const c of spec?.planned ?? []) {
    if (!isNonEmptyString(c.id) || !isNonEmptyString(c.title)) chap.push(`planned 章节缺 id/title：${JSON.stringify(c)}`);
    if (!CHAPTER_STATUSES.includes(c.status)) chap.push(`planned 章节 ${c.id} 的 status 非法：${c.status}`);
    if (!Array.isArray(c.residual)) chap.push(`planned 章节 ${c.id} 的 residual 必须是数组`);
    // ★ 机制归属是机器字段：`core-out` 表示 core 外（不实现、不记录），否则必须是一个机制 id
    //   （机制 id 的闭集在 `data/spec/core/README.md` §2 的表里；一致性由 `tools/test/spec-mechanisms.test.mjs` 守）。
    if (c.mechanism !== undefined && !isNonEmptyString(c.mechanism)) chap.push(`planned 章节 ${c.id} 的 mechanism 必须是非空字符串`);
  }
  checks.push({ id: 2, text: '章节：已写章节 id/title/file/status/residual 齐备；planned 章节 id/title/status/residual 齐备（mechanism 可选，写了须非空）', problems: chap });
  for (const p of chap) add(2, 'chapters', p);

  // ── 2. 试点域（候选是**块入口 EA**；effect 由语料机械枚举，schema 不预设语义）
  const pilot = [];
  if (!isNonEmptyString(spec?.pilot?.name)) pilot.push('pilot 缺 name');
  if (!isNonEmptyString(spec?.pilot?.question)) pilot.push('pilot 缺 question（本轮验收要回答什么）');
  const siteDecl = spec?.pilot?.sites ?? [];
  if (!Array.isArray(siteDecl) || siteDecl.length === 0) pilot.push('pilot.sites 必须是非空数组');
  const siteEas = new Set();
  for (const c of siteDecl) {
    if (!isNonEmptyString(c.ea) || !EA_RE.test(c.ea)) pilot.push(`候选块缺合法 ea：${JSON.stringify(c)}`);
    else if (siteEas.has(c.ea)) pilot.push(`候选块 ea 重复：${c.ea}`);
    else siteEas.add(c.ea);
    if (c.note !== undefined && !isNonEmptyString(c.note)) pilot.push(`候选块 ${c.ea} 的 note 必须是非空字符串`);
  }
  const internalKinds = new Set();
  for (const x of spec?.pilot?.internal ?? []) {
    if (!isNonEmptyString(x.kind) || !isNonEmptyString(x.why)) pilot.push(`pilot.internal 每项要有 kind + why：${JSON.stringify(x)}`);
    else if (internalKinds.has(x.kind)) pilot.push(`pilot.internal 的 kind 重复：${x.kind}`);
    else internalKinds.add(x.kind);
  }
  for (const r of spec?.pilot?.residuals ?? []) {
    if (!isNonEmptyString(r.id) || !isNonEmptyString(r.why) || !isNonEmptyString(r.next)) {
      pilot.push(`pilot.residuals 每项要有 id/what/why/next：${JSON.stringify(r)}`);
    }
    for (const b of r.blocks ?? []) if (!EA_RE.test(b)) pilot.push(`残差 ${r.id} 的 block 形态非法：${b}`);
    for (const k of r.kinds ?? []) if (!isNonEmptyString(k)) pilot.push(`残差 ${r.id} 的 kind 非法`);
    for (const o of r.ops ?? []) if (!isNonEmptyString(o)) pilot.push(`残差 ${r.id} 的 op 非法`);
  }
  checks.push({ id: 3, text: '试点域：块入口 ea 唯一、internal/residual/outOfScope 有理由；effect 由语料机械枚举', problems: pilot });
  for (const p of pilot) add(3, 'pilot', p);

  // ── 3. 条目形态 + 处理状态
  const shape = [];
  const state = [];
  const seenResidual = new Set();

  /** 一条残差的形态/凭据（条目与审计缺口共用，避免两套判据漂） */
  const checkResidual = (r, rat, out) => {
      if (!isNonEmptyString(r.id)) out.push(`${rat} 缺 id`);
      else if (seenResidual.has(r.id)) out.push(`残差 id 重复：${r.id}`);
      else seenResidual.add(r.id);
      if (!RESIDUAL_KINDS.includes(r.kind)) out.push(`${rat} 的 kind 非法：${r.kind}`);
      if (!isNonEmptyString(r.what)) out.push(`${rat} 缺 what`);
      if (!isNonEmptyString(r.next)) out.push(`${rat} 缺 next（下一步问题；残差不是"算了"）`);
      if (r.kind === 'discarded') {
        if (!isNonEmptyString(r.impact)) out.push(`${rat} 是 discarded，缺 impact`);
        if (!isNonEmptyString(r.reason)) out.push(`${rat} 是 discarded，缺 reason`);
        if (!isNonEmptyArray(r.range)) out.push(`${rat} 是 discarded，缺 range（机器范围，EA / 函数符号）`);
        else for (const item of r.range) {
          if (!machineToken(item)) out.push(`${rat} 的 range 只认 EA / sub_XXXXXX：${JSON.stringify(item)}`);
        }
      }
      if (r.kind === 'omitted') {
        if (!isNonEmptyArray(r.evidence)) out.push(`${rat} 是 omitted，evidence 必须是非空数组（每条都要是 KN:/guard:）`);
        else {
          let strong = 0;
          for (const ev of r.evidence) {
            const res = resolveEvidence(ev, { ledger, repoRoot });
            if (!res.ok) out.push(`${rat} 的 evidence 解析不到：${res.why}`);
            else if (res.strong) strong += 1;
          }
          if (strong === 0) out.push(`${rat} 是 omitted，但没有一条 accepted / guard 证据（不能只凭自由文本省略）`);
        }
      }

  };

  for (const e of spec?.entries ?? []) {
    const at = `entry:${e.id ?? '(缺 id)'}`;
    if (!isNonEmptyString(e.id)) { shape.push('条目缺 id'); continue; }
    if (entryIds.has(e.id)) shape.push(`条目 id 重复：${e.id}`);
    entryIds.add(e.id);
    if (!isNonEmptyString(e.title)) shape.push(`${at} 缺 title`);
    if (!chapterIds.has(e.chapter)) shape.push(`${at} 指向不存在的章节：${e.chapter}`);
    if (!ENTRY_STATUSES.includes(e.status)) shape.push(`${at} 的 status 非法：${e.status}`);
    if (!DISPOSITIONS.includes(e.disposition)) shape.push(`${at} 的 disposition 非法：${e.disposition}`);
    if (!isNonEmptyString(e.text)) shape.push(`${at} 缺规范正文（"### 标题" + "<!-- rule: ID -->" 之下要有内容）`);
    else if (e.text.trim().length < 20) shape.push(`${at} 的规范正文过短（${e.text.trim().length} 字符）`);
    if (!Array.isArray(e.falsifiers)) shape.push(`${at} 的 falsifiers 必须是数组（在 audit 里）`);
    if (!Array.isArray(e.residual)) shape.push(`${at} 的 residual 必须是数组`);
    if (!e.binding || typeof e.binding !== 'object') { shape.push(`${at} 缺 binding`); continue; }
    if (!Array.isArray(e.binding.sites)) shape.push(`${at} 的 binding.sites 必须是数组`);
    if (!Array.isArray(e.binding.functions)) shape.push(`${at} 的 binding.functions 必须是数组`);
    if (!Array.isArray(e.binding.storage)) shape.push(`${at} 的 binding.storage 必须是数组`);
    if (!isNonEmptyArray(e.binding.evidence)) shape.push(`${at} 缺 binding.evidence（每条合同至少一条可再校验观察）`);

    if ((e.covers ?? []).length && e.disposition !== 'handled' && e.disposition !== 'replaced') {
      state.push(`${at} 的 disposition=${e.disposition} 却声明了 covers（只有 handled/replaced 才算解释）`);
    }
    // covers：effect 选择器（{blocks?, kinds?, ops?}）；覆盖在 assets 档按语料现算
    for (const sel of e.covers ?? []) {
      const at2 = `${at} 的 covers`;
      if (!sel || typeof sel !== 'object' || Array.isArray(sel)) { shape.push(`${at2} 必须是选择器对象`); continue; }
      const keys = ['blocks', 'kinds', 'ops'].filter((k) => sel[k] !== undefined);
      if (keys.length === 0) { shape.push(`${at2} 至少要给 blocks / kinds / ops 之一`); continue; }
      for (const k of keys) {
        if (!Array.isArray(sel[k]) || sel[k].length === 0) shape.push(`${at2}.${k} 必须是非空数组`);
      }
      for (const b of sel.blocks ?? []) {
        if (!EA_RE.test(b)) { shape.push(`${at2}.blocks 里的 EA 形态非法：${b}`); continue; }
        if (e.disposition === 'handled' || e.disposition === 'replaced') {
          const hasSite = (e.binding?.sites ?? []).some((s) => Number.parseInt(String(s.ea).replace(/^0x/i, ''), 16) === Number.parseInt(String(b).replace(/^0x/i, ''), 16));
          if (!hasSite) state.push(`${at} 的 covers 选择器指向块 ${b}，但 binding.sites 里没有它（机器见证缺失）`);
        }
      }
      for (const k of sel.kinds ?? []) if (!isNonEmptyString(k)) shape.push(`${at2}.kinds 里有非字符串项`);
      for (const o of sel.ops ?? []) if (!isNonEmptyString(o)) shape.push(`${at2}.ops 里有非字符串项`);
    }

    // 处理状态各自的凭据
    const sites = Array.isArray(e.binding?.sites) ? e.binding.sites : [];
    const fns = Array.isArray(e.binding?.functions) ? e.binding.functions : [];
    if (e.disposition === 'handled') {
      if (sites.length === 0 && fns.length === 0) state.push(`${at} 是 handled 但没有任何机器绑定（sites/functions）`);
    } else if (e.disposition === 'replaced') {
      const r = e.replacement ?? {};
      for (const k of ['preserves', 'discards', 'chosen']) {
        if (k === 'chosen' ? !isNonEmptyString(r[k]) : !isNonEmptyArray(r[k])) state.push(`${at} 是 replaced，但 replacement.${k} 缺失`);
      }
      const scope = r.scope ?? {};
      const scopeTokens = new Set([...(scope.sites ?? []), ...(scope.functions ?? [])].map((x) => String(x).toLowerCase()));
      if (!isNonEmptyArray(scope.sites) && !isNonEmptyArray(scope.functions)) {
        state.push(`${at} 是 replaced，但 replacement.scope 没有声明机器范围（sites / functions）—— 越界判据无从谈起`);
      }
      // scope 必须是**本条目绑定过的机器范围**（不能凭空写 0x1）
      const boundTokens = new Set([...(e.binding?.sites ?? []).map((x) => String(x.ea).toLowerCase()), ...(e.binding?.functions ?? []).map((x) => String(x).toLowerCase())]);
      for (const item of scopeTokens) {
        if (!boundTokens.has(item)) state.push(`${at} 的 replacement.scope 越界：${item} 不在 binding.sites / binding.functions 里`);
      }
      const declared = new Set();
      for (const k of ['preserves', 'discards']) {
        for (const item of r[k] ?? []) {
          const tok = String(item).toLowerCase();
          if (!machineToken(item)) state.push(`${at} 的 replacement.${k} 只认 EA / sub_XXXXXX：${JSON.stringify(item)}`);
          else if (scopeTokens.size > 0 && !scopeTokens.has(tok)) {
            state.push(`${at} 的 replacement.${k} 越界：${item} 不在 replacement.scope 里`);
          }
          declared.add(tok);
        }
      }
      // 放电必须**交代完** scope：没被 preserves/discards 覆盖的 scope 项会静默消失
      for (const item of scopeTokens) {
        if (!declared.has(item)) state.push(`${at} 的 replacement.scope 有未交代项：${item}（必须进 preserves / discards / residual）`);
      }
      if ((e.covers ?? []).length > 0 && !isNonEmptyArray(r.discards)) state.push(`${at} 有 covers 却没有 discards 范围`);
    } else if (e.disposition === 'omitted') {
      if (!isNonEmptyString(e.omission?.why)) state.push(`${at} 是 omitted，但缺 omission.why`);
      if (!isNonEmptyArray(e.omission?.evidence)) state.push(`${at} 是 omitted，但缺 omission.evidence（有依据的省略才叫省略）`);
      else for (const ev of e.omission.evidence) {
        const r = resolveEvidence(ev, { ledger, repoRoot });
        if (!r.ok) state.push(`${at} 的 omission.evidence 解析不到：${r.why}`);
      }
    } else if (e.disposition === 'discarded') {
      if (!isNonEmptyString(e.discard?.reason)) state.push(`${at} 是 discarded，但缺 discard.reason`);
      if (!isNonEmptyString(e.discard?.impact)) state.push(`${at} 是 discarded，但缺 discard.impact`);
      if (!isNonEmptyArray(e.discard?.range)) state.push(`${at} 是 discarded，但缺 discard.range（机器范围，EA / 函数符号）`);
      else for (const item of e.discard.range) {
        if (!machineToken(item)) state.push(`${at} 的 discard.range 只认 EA / sub_XXXXXX：${JSON.stringify(item)}`);
      }
      if ((e.covers ?? []).length > 0) state.push(`${at} 是 discarded 却仍 covers 候选 —— 舍弃不能冒充解释`);
    } else if (e.disposition === 'unhandled') {
      if (!isNonEmptyArray(e.residual)) state.push(`${at} 是 unhandled，但没有残差清单`);
      if ((e.covers ?? []).length > 0) state.push(`${at} 是 unhandled 却 covers 候选 —— 未处理不能声称已解释`);
    }

    // residual
    for (const [i, r] of (e.residual ?? []).entries()) checkResidual(r, `${at} 的 residual[${i}]`, shape);
  }

  // ── 3b. 审计缺口：尚无规范条目的义务（只存在于 audit，不进规范正文）
  const gapProblems = [];
  const gapIds = new Set();
  /** opcode（数字）→ 声明它的 gap id：同一 opcode 出现在两个 gap 里会让"精确划分"不成立 */
  const gapOpOwners = new Map();
  for (const g of spec?.gaps ?? []) {
    const at = `gap:${g.id ?? '(缺 id)'}`;
    if (!isNonEmptyString(g.id)) { gapProblems.push('缺口缺 id'); continue; }
    if (gapIds.has(g.id)) gapProblems.push(`缺口 id 重复：${g.id}`);
    gapIds.add(g.id);
    if (!allChapterIds.has(g.chapter)) gapProblems.push(`${at} 指向不存在的章节（已写或 planned）：${g.chapter}`);
    if (!isNonEmptyString(g.title)) gapProblems.push(`${at} 缺 title`);
    if (!isNonEmptyString(g.note)) gapProblems.push(`${at} 缺 note（为什么尚未成篇）`);
    if (!isNonEmptyArray(g.residual)) gapProblems.push(`${at} 缺 residual（缺口必须点名具体义务）`);
    // `ops` 可选：opcode 级的 gap 用它列出覆盖的 opcode；非 opcode 级的 gap（如容器句柄分支）不填。
    if (g.ops !== undefined) {
      if (!Array.isArray(g.ops)) gapProblems.push(`${at} 的 ops 必须是数组`);
      else {
        for (const [i, op] of g.ops.entries()) {
          if (typeof op !== 'string' || !OPCODE_TEXT_RE.test(op)) {
            gapProblems.push(`${at} 的 ops[${i}] 形态非法（要 \`0x…\` 十六进制串）：${JSON.stringify(op)}`);
            continue;
          }
          const n = Number.parseInt(op.slice(2), 16);
          if (gapOpOwners.has(n)) {
            gapProblems.push(`opcode ${op} 同时出现在两个 gap 里：${gapOpOwners.get(n)} / ${g.id}`);
          } else gapOpOwners.set(n, g.id);
        }
      }
    }
    for (const [i, r] of (g.residual ?? []).entries()) checkResidual(r, `${at} 的 residual[${i}]`, gapProblems);
    for (const x of g.sites ?? []) {
      if (!EA_RE.test(x?.ea ?? '')) gapProblems.push(`${at} 的 site EA 形态非法：${x?.ea}`);
      if (!isNonEmptyString(x?.what)) gapProblems.push(`${at} 的 site ${x?.ea} 缺 what`);
    }
    for (const f of g.functions ?? []) if (!FN_RE.test(f)) gapProblems.push(`${at} 的函数符号形态非法：${f}`);
    for (const ref of g.storage ?? []) {
      const rr = resolveStorage(ref);
      if (!rr.ok) gapProblems.push(`${at} 的 storage 指针落空：${rr.why}`);
    }
    for (const ev of g.evidence ?? []) {
      const rr = resolveEvidence(ev, { ledger, repoRoot });
      if (!rr.ok) gapProblems.push(`${at} 的证据解析不到：${rr.why}`);
    }
  }
  checks.push({ id: 4, text: '条目形态：id/title/chapter/status/contract/context/stateMapping/binding 齐备', problems: shape });
  for (const p of shape) add(4, 'entries', p);
  checks.push({
    id: 5,
    text: '处理状态：handled 有机器绑定；replaced 声明 preserves/discards/chosen；omitted 有依据；discarded 不冒充解释；unhandled 有残差',
    problems: state,
  });
  for (const p of state) add(5, 'entries', p);

  // ── 4. 绑定：EA / 函数 / storage 指针
  const bind = [];
  for (const e of spec?.entries ?? []) {
    const at = `entry:${e.id}`;
    for (const s of e.binding?.sites ?? []) {
      if (!EA_RE.test(s.ea ?? '')) bind.push(`${at} 的 site EA 形态非法：${s.ea}`);
      if (!isNonEmptyString(s.what)) bind.push(`${at} 的 site ${s.ea} 缺 what`);
    }
    for (const f of e.binding?.functions ?? []) if (!FN_RE.test(f)) bind.push(`${at} 的函数符号形态非法：${f}`);
    for (const ref of e.binding?.storage ?? []) {
      const r = resolveStorage(ref);
      if (!r.ok) bind.push(`${at} 的 storage 指针落空：${r.why}`);
    }
  }
  checks.push({ id: 6, text: '绑定：EA/函数符号形态合法；storage 指针能在 shape（types.json 的类型与成员起点）里解析到', problems: bind });
  for (const p of bind) add(6, 'entries', p);

  // ── 5. 证据
  const evp = [];
  const weak = [];
  for (const e of spec?.entries ?? []) {
    const at = `entry:${e.id}`;
    let strong = 0;
    let reviewable = 0;
    let weakHere = 0;
    for (const ev of e.binding?.evidence ?? []) {
      const r = resolveEvidence(ev, { ledger, repoRoot });
      if (!r.ok) { evp.push(`${at}：${r.why}`); continue; }
      if (r.strong) {
        strong += 1;
        if (r.kind === 'ledger' || r.tier === 'pure') reviewable += 1;
      } else { weakHere += 1; weak.push(`${at}：${ev}（${r.effective}）`); }
    }
    if (e.status === 'reviewed' && strong === 0) evp.push(`${at} 是 reviewed，但没有一条 accepted / guard 证据`);
    if (e.status === 'reviewed' && reviewable === 0) {
      evp.push(`${at} 是 reviewed，但强证据只有 assets/external 档守卫（默认门禁不跑）—— 至少要有 accepted 台账记录，或 pure 档守卫（JS 用例的 @env pure / CTest 的顶层注册）`);
    }
    if (e.status === 'reviewed' && !isNonEmptyArray(e.falsifiers)) evp.push(`${at} 是 reviewed，但没有 falsifiers（可证伪的核验问题 / 预言）`);
    if (e.status === 'reviewed' && weakHere > 0) evp.push(`${at} 是 reviewed，却含 ${weakHere} 条弱证据（proposed）—— reviewed 要求证据链全部过 accepted`);
  }
  checks.push({ id: 7, text: '证据：每条证据都解析得到；reviewed 至少一条 accepted 或 guard 证据（proposed 只算弱证据）', problems: evp });
  for (const p of evp) add(7, 'entries', p);

  // ── 6. 规范正文必须自包含：不出现汇编 / 台账 / 守卫 / 本仓路径等复核信息
  const prose = [];
  for (const c of spec?.chapters ?? []) {
    const text = String(c.text ?? '').replace(/<!--\s*rule:[^>]*-->/g, '');
    for (const { re, why } of REVIEW_INFO_PATTERNS) {
      const m = re.exec(text);
      if (m) prose.push(`chapter:${c.id} 含${why}：${m[0]}（分发产物必须自包含；复核信息放 bindings/audit）`);
    }
  }
  checks.push({ id: 8, text: '自包含：规范正文不含汇编引用 / 台账 id / 守卫 / 本仓路径（复核信息只许进 bindings / audit）', problems: prose });
  for (const p of prose) add(8, 'chapters', p);

  // ── 7. 试点闭合：effect 级（assets 档现算）；纯文本档只查选择器形态（见 #4）
  const closure = [];
  let coverage = null;
  if (bundle) {
    const fx = effects ? { ok: true, effects } : derivePilotEffects(spec, { repoRoot });
    if (!fx.ok && fx.why) {
      // 语料不在场：不把"没跑"伪装成"绿"，也不把它当红；报告会明说 effect 未现算。
      coverage = null;
    } else {
      coverage = computeEffectCoverage(spec, fx.effects);
      for (const e of coverage.unregistered) closure.push(`effect 未登记：${e.block} ${e.id}（${e.op} ${e.kind}）`);
      for (const x of coverage.redundantResidual) closure.push(`残差冗余：${x.id} 同时命中了已覆盖 effect ${x.effect.id}`);
    }
  }
  checks.push({ id: 9, text: '试点闭合：每个 effect 被规则 covers / pilot.internal / pilot.residuals 归属；未登记 = 0（assets 档现算）', problems: closure });
  for (const p of closure) add(9, 'pilot', p);

  // ── 7b. 范围：域外项必须显式（空数组 = 用沉默代替排除）
  const scopeProblems = [];
  const oos = spec?.pilot?.outOfScope;
  if (!Array.isArray(oos) || oos.length === 0) scopeProblems.push('pilot.outOfScope 不能为空：域外项必须显式列出');
  for (const x of oos ?? []) {
    if (!isNonEmptyString(x.what) || !isNonEmptyString(x.why)) scopeProblems.push('pilot.outOfScope 每项都要有 what + why');
    if (x.chapter && !allChapterIds.has(x.chapter)) scopeProblems.push(`outOfScope 指向不存在的章节：${x.chapter}`);
  }
  checks.push({ id: 10, text: '范围：域外项必须显式（what + why），不许用沉默代替排除', problems: scopeProblems });
  for (const p of scopeProblems) add(10, 'pilot', p);

  // ── 8. 章节状态与条目一致（防止"bootstrap 是空壳"或"none 章里藏着合同"）
  const chapterEntries = new Map();
  for (const e of spec?.entries ?? []) chapterEntries.set(e.chapter, (chapterEntries.get(e.chapter) ?? 0) + 1);
  const chapCons = [];
  for (const c of spec?.chapters ?? []) {
    const n = chapterEntries.get(c.id) ?? 0;
    if (c.status === 'bootstrap' && n === 0) chapCons.push(`章节 ${c.id} 标成 bootstrap，但没有任何条目（成篇程度是自称）`);
    if (c.status === 'none' && n > 0) chapCons.push(`章节 ${c.id} 标成 none，却挂着 ${n} 条条目 —— 状态与内容打架`);
  }
  checks.push({ id: 11, text: '章节状态与条目一致：bootstrap ⇒ 至少一条条目；none ⇒ 不允许挂条目', problems: chapCons });
  for (const p of chapCons) add(11, 'chapters', p);

  // ── 12. 三份文件按 id 一一对应（拆分的核心守卫）
  checks.push({ id: 12, text: '跨文件：spec / bindings / audit 按 rule id 与 chapter id 一一对应（无孤儿、无缺项、无重复）', problems: cross });
  for (const p of cross) add(12, 'files', p);

  checks.push({ id: 13, text: '审计缺口：id/chapter（已写或 planned）/title/note/residual 齐备；ops 每项是 0x… 且同一 opcode 不出现在两个 gap；站点/函数/指针/证据可解析（缺口只进 audit，不进规范正文）', problems: gapProblems });
  for (const p of gapProblems) add(13, 'audit.gaps', p);

  const derived = derive(spec, { coverage, repoRoot });
  return { checks, problems, derived, weak, coverage };
}

/**
 * 从条目**派生**处理账（不落盘；报告就是它的人读投影）。
 * ★ effect 级覆盖由 `coverage` 传入（assets 档现算）；纯文本档没有它，就只报结构计数。
 * ★ `opcodes` 是"未成篇 opcode"的机械账（`hasHandler` = 已写 + gaps），报告与守卫共用它。
 */
export function derive(spec, { coverage = null, repoRoot = REPO_ROOT } = {}) {
  const dispositions = Object.fromEntries(DISPOSITIONS.map((d) => [d, 0]));
  const statuses = Object.fromEntries(ENTRY_STATUSES.map((s) => [s, 0]));
  const residualKinds = Object.fromEntries(RESIDUAL_KINDS.map((k) => [k, 0]));
  let sites = 0;
  let functions = 0;
  let storage = 0;
  let evidence = 0;
  for (const e of spec.entries ?? []) {
    dispositions[e.disposition] = (dispositions[e.disposition] ?? 0) + 1;
    statuses[e.status] = (statuses[e.status] ?? 0) + 1;
    sites += e.binding?.sites?.length ?? 0;
    functions += e.binding?.functions?.length ?? 0;
    storage += e.binding?.storage?.length ?? 0;
    evidence += e.binding?.evidence?.length ?? 0;
    for (const r of e.residual ?? []) residualKinds[r.kind] = (residualKinds[r.kind] ?? 0) + 1;
  }
  for (const g of spec.gaps ?? []) for (const r of g.residual ?? []) residualKinds[r.kind] = (residualKinds[r.kind] ?? 0) + 1;
  // ★ gaps 的 `kind=unhandled` 必须计入残差：否则"未成篇数百条"会读成 unhandled=0（假象）。
  const opcodes = opcodeAccounting(spec, { repoRoot });
  const chapters = Object.fromEntries(CHAPTER_STATUSES.map((s) => [s, 0]));
  for (const c of spec.chapters ?? []) chapters[c.status] = (chapters[c.status] ?? 0) + 1;
  for (const c of spec.planned ?? []) chapters[c.status] = (chapters[c.status] ?? 0) + 1;
  return {
    pilotSites: spec.pilot?.sites?.length ?? 0,
    effects: coverage ? coverage.rows.length : null,
    covered: coverage ? coverage.covered : null,
    internal: coverage ? coverage.internal : null,
    residualEffects: coverage ? coverage.residual : null,
    unregistered: coverage ? coverage.unregistered : [],
    effectByKind: coverage ? coverage.byKind : null,
    dispositions,
    statuses,
    residualKinds,
    chapters,
    gaps: (spec.gaps ?? []).length,
    opcodes,
    totals: { entries: spec.entries?.length ?? 0, sites, functions, storage, evidence },
  };
}

/** 人读报告（★ 只给计数与清单，不给百分数） */
export function reportText(spec, { file = SPEC_FILE, derived = derive(spec), weak = [] } = {}) {
  const d = derived;
  const L = [];
  L.push('spec 报告（派生视图；范围闭合 ≠ 解释正确）');
  L.push('');
  L.push(`文件      ${file}`);
  L.push(`验收域    ${spec.pilot.name} —— ${spec.pilot.question}`);
  L.push(`候选来源  ${spec.pilot.source ?? '（未声明枚举口径）'}`);
  L.push(`章节      ${spec.chapters.length + (spec.planned?.length ?? 0)}（已写 ${spec.chapters.length} · 未成篇 ${spec.planned?.length ?? 0}；${CHAPTER_STATUSES.map((s) => `${s}=${d.chapters[s]}`).join(' · ')}）`);
  L.push(`条目      ${d.totals.entries}（${DISPOSITIONS.map((x) => `${x}=${d.dispositions[x]}`).join(' · ')}；` +
    `${ENTRY_STATUSES.map((x) => `${x}=${d.statuses[x]}`).join(' · ')}）`);
  L.push(`审计缺口  ${d.gaps} 个（尚无规范条目的义务；只进审计，不进正文）`);
  if (d.opcodes?.ok) {
    L.push(
      `未成篇 opcode ${d.opcodes.unwritten}（gaps 覆盖 ${d.opcodes.gapsCovered}；已写成 ${d.opcodes.written}）` +
        (d.opcodes.consistent
          ? ''
          : ` ⚠ 与 gaps 不一致（重叠 ${d.opcodes.overlap.length} · 表外 ${d.opcodes.outsideTable.length} · 重复 ${d.opcodes.duplicates.length}）`),
    );
  } else {
    L.push('未成篇 opcode （指令表不在场，不现算）');
  }
  L.push('');
  L.push(`试点块    ${d.pilotSites} 个块入口；effect ${d.effects ?? '（语料不在场，不现算）'}`);
  if (d.effects !== null) {
    L.push(`  已覆盖  ${d.covered}（被 handled/replaced 规则的 covers 选择器命中）`);
    L.push(`  内部省略 ${d.internal}（pilot.internal；每类都有理由）`);
    L.push(`  已登记残差 ${d.residualEffects}（pilot.residuals；每条都有 next）`);
    L.push(`  未登记  ${d.unregistered.length}${d.unregistered.length === 0 ? ' ✅' : ' ❌'}` +
      (d.unregistered.length ? `：${d.unregistered.map((e) => e.id).join(', ')}` : ''));
    L.push('  按 effect 类别：');
    for (const [k, v] of Object.entries(d.effectByKind)) {
      L.push(`    ${k.padEnd(12)} 总 ${v.total} · 覆盖 ${v.covered} · 内部 ${v.internal} · 残差 ${v.residual} · 未登记 ${v.unregistered}`);
    }
    for (const r of spec.pilot?.residuals ?? []) {
      L.push(`  残差 ${r.id}：${r.why}（下一步：${r.next}）`);
      if (r.blocks?.length) L.push(`    块：${r.blocks.join(' · ')}`);
    }
  }
  L.push('');
  L.push(`绑定      站点 ${d.totals.sites} · 函数 ${d.totals.functions} · storage 指针 ${d.totals.storage} · 证据 ${d.totals.evidence}`);
  L.push(`残差      ${RESIDUAL_KINDS.map((k) => `${k}=${d.residualKinds[k]}`).join(' · ')}`);
  if (weak.length) L.push(`弱证据    ${weak.length} 条（proposed 台账记录，只支撑 draft 条目）`);
  const unhandledChapters = [
    ...spec.chapters.filter((c) => c.status !== 'bootstrap'),
    ...(spec.planned ?? []),
  ];
  if (unhandledChapters.length) {
    L.push('');
    L.push(`整份 spec 骨架里还没成篇的章节：`);
    for (const c of unhandledChapters) L.push(`  ${String(c.status).padEnd(9)} ${c.id} —— ${c.title}`);
  }
  L.push('');
  L.push('诚实项（不许省略）：');
  L.push('  · 计数只说明"effect 有没有被覆盖/省略/登记"，不证明合同与原引擎等价；共同误解由多路推导也发现不了。');
  L.push('  · 试点块是`pilot`里**声明**的；effect 由语料机械枚举，但枚举器有边界（见 describe），不证明域外没有同类义务。');
  L.push('  · `discarded` 与 `unknown` 分列：本报告不把它们并进"已解释"。');
  L.push('  · 弱证据（proposed）撑不起 reviewed：状态字段就是这条边界。');
  return `${L.join('\n')}\n`;
}

/** 章节骨架（人读） */
export function chaptersText(spec) {
  const L = [];
  L.push('spec 章节骨架（整份引擎；status 只表示"成篇程度"，不表示正确性）');
  L.push('');
  for (const c of spec.chapters) {
    L.push(`[${c.status}] ${c.id} —— ${c.title}  (${c.file})`);
    const entries = spec.entries.filter((e) => e.chapter === c.id);
    if (entries.length) L.push(`  规则  ${entries.map((e) => `${e.id}(${e.disposition})`).join(' · ')}`);
    const gaps = (spec.gaps ?? []).filter((g) => g.chapter === c.id);
    if (gaps.length) L.push(`  缺口  ${gaps.map((g) => `${g.id}（${g.title}）`).join(' · ')}`);
    for (const r of c.residual) L.push(`  残差  ${r.next}`);
  }
  for (const c of spec.planned ?? []) {
    L.push(`[${c.status}] ${c.id} —— ${c.title}  (未成篇)`);
    // ★ gaps 的 chapter 可以是 planned 章节（"整族尚未成篇"的落点），这里也要看得见。
    const gaps = (spec.gaps ?? []).filter((g) => g.chapter === c.id);
    if (gaps.length) L.push(`  缺口  ${gaps.map((g) => `${g.id}（${g.title}）`).join(' · ')}`);
  }
  return `${L.join('\n')}\n`;
}

/** 自描述（schema 的唯一真源） */
export function describe() {
  return {
    file: `${SPEC_FILE} + data/spec/chapters/*.md`,
    doc: 'data/spec/README.md',
    purpose:
      '引擎规范：**可分发的 Markdown 章节**（spec/chapters）就是规范正文本身（不打包成单文件）；' +
      'bindings / audit 是内部复核数据，不进入分发产物。规范正文自包含：不引用汇编、台账、守卫或本仓路径。',
    files: {
      spec: {
        file: SPEC_FILE,
        what: '**manifest**：标题 + 章节顺序（id / file / title）。它不装规范正文。',
        fields: ['schema', 'title', 'chapters[]（id/file/title）'],
      },
      chapters: {
        file: 'data/spec/chapters/*.md',
        what: '**规范正文**（分发物）：自包含的 Markdown。规则 = `### 标题` + `<!-- rule: ID -->` + 正文。',
        fields: ['## 章标题', '### 规则标题', '<!-- rule: ID -->（HTML 注释、阅读时不可见；只用于内部关联）', '规则正文'],
      },
      bindings: {
        file: BINDINGS_FILE,
        what: '**机器绑定**（内部）：每条规则一行；EA / 函数 / storage 指针 / 证据。不进入分发产物。',
        fields: ['schema', 'bindings[]（rule/sites/functions/storage/evidence）'],
      },
      audit: {
        file: AUDIT_FILE,
        what: '**范围审计**（内部）：试点块 / effect 归属（规则 covers + internal + residuals）/ 章节成熟度 / 处理状态 / 缺口 / 可证伪预言。不进入分发产物。',
        fields: ['schema', 'pilot（sites/internal/residuals/outOfScope）', 'chapters[]（id/status/residual）', 'planned[]（id/title/status/mechanism?/residual）', 'rules[]（见下 auditFields）', 'gaps[]（见下 gap）'],
      },
      format: {
        file: 'data/spec/format/opcode-table.json',
        what: '**本规范的指令表**：当前内容来自现有实现，协议部分标为待核验；不是 `age-format` 的 canonical 副本。',
        fields: ['opcode', 'argc', 'name', 'aliases', 'hasHandler'],
      },
      configKeys: {
        file: 'data/spec/format/config-keys.json',
        what: '**配置键词表**：发行默认表键 → 内部键 / 类型；玩家文件的段·键 → 内部键 / 类型；保存时的段·键顺序与条件。它是引擎内建的固定词表，不是游戏数据。',
        fields: ['defaults[]（indexKey/internal/kind/special?/extra?）', 'ini[]（section/key/internal/kind）', 'persist[]（section/key/internal/kind/if?）'],
      },
    },
    pilot: {
      meaning: '本轮验收域（在 audit.json 里）：**试点块**（块入口 EA）+ 机械枚举的 **effect** + 归属（规则 covers / internal / residuals）。schema 不预设 effect 的语义；effect 由 `x86-effects` 从语料现算。',
      fields: ['name', 'question', 'source?', 'sites[]', 'internal[]', 'residuals[]', 'outOfScope[]'],
      siteFields: ['ea', 'note?'],
      internalFields: ['kind', 'why'],
      residualFields: ['id', 'blocks?', 'kinds?', 'ops?', 'targets?', 'why', 'next'],
    },
    gap: {
      meaning:
        '**尚无规范条目的义务**（`audit.gaps[]`，内部复核数据，⛔ 不进规范正文）：一条 gap = 一个未成篇的 opcode 族/段，或一类按结构位置排除的机器分支。' +
        '它是"未成篇义务"的**唯一落点** —— 不要写进 `pilot.residuals`，也不要只留在章级/planned 残差里。',
      fields: ['id', 'chapter（已写章节或 planned 章节）', 'title', 'note（为什么尚未成篇）', 'ops?（`0x…`[]：该 gap 覆盖的 opcode；非 opcode 级的 gap 不填）', 'residual[]', 'sites? / functions? / storage? / evidence?（可选，形态同规则绑定）'],
      residualFields: ['id', 'kind', 'what', 'next'],
      invariants: [
        '`ops` 每项必须是 `/^0x[0-9a-fA-F]+$/` 的字符串',
        '同一 opcode 不许出现在两个 gap 里',
        '`ops` 覆盖的 opcode 与"已写"（规则 `###` 标题里的 opcode）必须精确划分指令表的 `hasHandler` 集合 —— 守卫 `tools/test/spec-gaps.test.mjs`',
      ],
      derivation:
        '`ops` 由 `format/opcode-table.json` 的 `hasHandler` 减去规则标题里的 opcode **机械派生**（`deriveOpcodeGaps`），⛔ 不手写；分段是 0x100 区间的算术分组，不声称族身份。',
    },
    rule: {
      specFields: [
        ['id', '✅', '`<!-- rule: ID -->`', '稳定 id（SPC-…）；bindings / audit 只认它'],
        ['title', '✅', '`### 标题`', '该规则小节的可读标题（进入分发产物）'],
        ['text', '✅', 'Markdown 正文', '规范正文；⛔ 不写汇编引用、台账 id、守卫、本仓路径'],
      ],
      bindingFields: [
        ['rule', '✅', '规则 id', '与章节里的 <!-- rule: ID --> 一一对应'],
        ['sites', '{ea,what}[]', '机器站点（EA 是唯一合法的机器锚；what 只给人看）'],
        ['functions', 'sub_XXXXXX[]', '整个函数作为绑定范围'],
        ['storage', 'shape:<类型> / shape:<类型>@0x<成员起点>[]', '机器本体指针：状态属于哪个 shape 类型 / 哪个成员（只认 shape；`layout:` 形式已结构性拒绝，见不变量 6）'],
        ['evidence', 'KN:<ULID> / guard:<path>#<用例>[]', '可再校验观察（台账记录 / 可执行守卫用例）'],
      ],
      auditFields: [
        ['id', '✅', '规则 id', '与章节里的 <!-- rule: ID --> 一一对应'],
        ['status', '✅', ENTRY_STATUSES.join(' | '), 'draft=可以含弱证据；reviewed=全部证据 strong、至少一条 accepted 台账或 pure 档守卫（JS 用例看 @env pragma；CTest 看是否顶层注册）、且有 falsifiers'],
        ['disposition', '✅', DISPOSITIONS.join(' | '), '处理状态：spec 处理 / 替换 / 有依据省略 / 明确舍弃 / 尚未处理'],
        ['covers', '⬜', '选择器[]（{blocks?,kinds?,ops?,targets?}）', '这条规则解释哪些 effect；调用按实际符号（targets）匹配，不另起名字'],
        ['residual', '✅', '对象[]', 'id / kind / what / next /（candidate? / range[] / reason? / impact? / evidence[]?）'],
        ['falsifiers', '条件', '字符串[]', '可证伪的核验问题 / 预言（reviewed 必填）；只进审计，不进分发产物'],
        ['replacement', '条件', '对象', 'disposition=replaced 时必填：preserves / discards / chosen / scope（越界即红）'],
        ['omission', '条件', '对象', 'disposition=omitted 时必填：why / evidence[]'],
        ['discard', '条件', '对象', 'disposition=discarded 时必填：reason / range[] / impact'],
      ],
    },
    dispositions: {
      handled: '由 spec 解释 / 保留；必须有机器绑定',
      replaced: '抽象替换（放电）：必须声明 preserves / discards / chosen 与 scope；preserves/discards 越出 scope ⇒ 红',
      omitted: '有依据的实现内部省略：必须给 why + evidence（证据要解析得到）',
      discarded: '项目选择不保留：必须给 reason / range（EA / 函数符号）/ impact；⛔ 不许 covers 候选',
      unhandled: '尚未处理：必须有残差；⛔ 不许 covers 候选（没规范内容的义务应进 gaps）',
    },
    residualKinds: {
      unknown: '还判不了（下一步要取证什么）',
      unhandled: '还没开始处理',
      omitted: '有依据可省略（evidence[] 必须是可解析的强证据）',
      discarded: '明确舍弃（带 reason / range[] / impact）',
    },
    invariants: [
      [1, '形态：schema=3 / manifest 顶层字段 / 章节与规则非空'],
      [2, '章节：已写章节 id/title/file/status/residual 齐备；planned 章节 id/title/status/residual 齐备（mechanism 可选，写了须非空）'],
      [3, '试点域：块入口 ea 唯一、internal/residual/outOfScope 有理由；effect 由语料机械枚举'],
      [4, '规则形态：每个 <!-- rule: ID --> 在 ### 小节里、有标题与正文；id 唯一'],
      [5, '处理状态：handled 有机器绑定；replaced 声明 preserves/discards/chosen；omitted 有依据；discarded 不冒充解释；unhandled 有残差'],
      [6, '绑定：EA/函数符号形态合法；storage 指针能在 shape（types.json 的类型与成员起点）里解析到'],
      [7, '证据：每条证据都解析得到；reviewed 要求全部证据 strong、至少一条 accepted 台账或 pure 档守卫（JS 用例看 @env pragma；CTest 看是否顶层注册）、且必须给 falsifiers'],
      [8, '自包含：规范正文不含汇编引用 / 台账 id / 守卫 / 本仓路径（复核信息只许进 bindings / audit）'],
      [9, '试点闭合（effect 级）：每个 effect 被规则 covers / pilot.internal / pilot.residuals 归属；未登记义务 = 0（assets 档现算）'],
      [10, '范围：域外项必须显式（what + why），不许用沉默代替排除'],
      [11, '章节状态与规则一致：bootstrap ⇒ 至少一条规则；none ⇒ 不允许挂规则'],
      [12, '跨文件：章节规则 / bindings / audit 按 rule id 一一对应；章节文件与 audit chapters 一一对应'],
      [13, '审计缺口：id/chapter（已写或 planned）/title/note/residual 齐备；`ops` 每项是 `0x…`、同一 opcode 不在两个 gap；站点/函数/指针/证据可解析'],
    ],
    operations: [
      ['validate', '跑全部不变量（红 = 退出码 1；条数以 describe 为准）'],
      ['report', '审计账：effect 覆盖 / 内部省略 / 残差 / **未登记** + 章节与绑定计数（不给百分数）'],
      ['chapters', '整份 spec 的章节骨架 + 每章规则与残差'],
      ['show', '一条规则的规范正文 + 证据解析结果（内部视图）'],
      ['at', '反向查询：某个 EA / 函数出现在哪些规则里'],
      ['effects', '★ 从语料机械枚举 effect：`--pilot` 或 `<sub_XXXXXX>`；调用保留实际符号（target），这是覆盖判定的输入'],
      ['describe', '本自描述（schema 的唯一真源）'],
    ],
    writePath:
      '规范正文改 `data/spec/chapters/*.md`；章节顺序改 `spec.json`；复核绑定 / 审计改 `bindings.json` / `audit.json`。' +
      '本域**没有写操作**：不分发单文件，规范正文就是 `chapters/*.md`（校验与报告只读）。',
    honest: [
      'bindings / audit 不进入分发产物；它们是复核材料，不是规范的一部分。',
      '试点块是声明的，不是穷举的；effect 由语料机械枚举，枚举器有边界（见 x86-effects 文件头）。',
      '报告不证明合同正确或完整；只证明"候选有没有被点名 / 解释"。',
      'discarded 与 unknown 分列；弱证据（proposed）只支撑 draft。',
      '没有覆盖率百分数：范围闭合 ≠ 解释正确。',
    ],
  };
}

export function describeText(d = describe()) {
  const L = [];
  L.push(`# ${d.file} —— 自描述（由控制脚本给出，文档不复述 schema）`);
  L.push('');
  L.push(`用途：${d.purpose}`);
  L.push('');
  L.push('## 文件');
  for (const [k, v] of Object.entries(d.files)) {
    L.push(`* \`${k}\` · \`${v.file}\` —— ${v.what}`);
    if (v.fields) L.push(`  字段：${v.fields.join(' · ')}`);
  }
  L.push('');
  L.push('## 试点域（audit.json）');
  L.push(`${d.pilot.meaning}`);
  L.push(`  字段：${d.pilot.fields.join(' · ')}`);
  L.push(`  块字段：${d.pilot.siteFields.join(' · ')}`);
  L.push(`  internal 字段：${d.pilot.internalFields.join(' · ')}`);
  L.push(`  residual 字段：${d.pilot.residualFields.join(' · ')}`);
  L.push('');
  L.push('## 审计缺口（audit.gaps[]：未成篇义务的唯一落点）');
  L.push(`${d.gap.meaning}`);
  L.push(`  字段：${d.gap.fields.join(' · ')}`);
  L.push(`  residual 字段：${d.gap.residualFields.join(' · ')}`);
  for (const x of d.gap.invariants) L.push(`  * ${x}`);
  L.push(`  派生：${d.gap.derivation}`);
  L.push('');
  L.push('## 规则在 Markdown 里的形态');
  for (const [name, req, type, why] of d.rule.specFields) L.push(`* \`${name}\` ${req} ${type} —— ${why}`);
  L.push('');
  L.push('## 机器绑定字段（bindings.json）');
  for (const [name, req, type, why] of d.rule.bindingFields) L.push(`* \`${name}\` ${req} ${type} —— ${why}`);
  L.push('');
  L.push('## 审计字段（audit.json）');
  for (const [name, req, type, why] of d.rule.auditFields) L.push(`* \`${name}\` ${req} ${type} —— ${why}`);
  L.push('');
  L.push('## 处理状态（disposition，闭集合）');
  for (const [k, v] of Object.entries(d.dispositions)) L.push(`* \`${k}\` —— ${v}`);
  L.push('');
  L.push('## 残差类别（residual.kind，闭集合）');
  for (const [k, v] of Object.entries(d.residualKinds)) L.push(`* \`${k}\` —— ${v}`);
  L.push('');
  L.push('## 不变量（validate 的断言）');
  for (const [id, text] of d.invariants) L.push(`${id}. ${text}`);
  L.push('');
  L.push('## 操作');
  for (const [name, why] of d.operations) L.push(`* \`${name}\` —— ${why}`);
  L.push('');
  L.push(`写入口：${d.writePath}`);
  L.push('');
  L.push('## 诚实项（本层不提供什么）');
  for (const x of d.honest) L.push(`* ${x}`);
  return `${L.join('\n')}\n`;
}

/** 校验绑定站点真的落在语料的指令行首（EA 是锚，必须指得到一条指令） */
export function checkCorpusBindings(spec, { repoRoot = REPO_ROOT } = {}) {
  const lst = pickListing(path.join(repoRoot, 'corpus', 'disasm', 'files'));
  if (!lst) return { ok: false, why: '语料未解压', problems: [] };
  const lines = fs.readFileSync(lst, 'utf8').split('\n');
  const at = new Map();
  for (const line of lines) {
    const m = /^\.text:([0-9A-F]{8}) /.exec(line);
    if (m) at.set(Number.parseInt(m[1], 16), line);
  }
  const problems = [];
  for (const e of spec.entries ?? []) {
    for (const s of e.binding?.sites ?? []) {
      const ea = Number.parseInt(String(s.ea).replace(/^0x/i, ''), 16);
      const line = at.get(ea);
      if (!line) problems.push(`${e.id}：EA ${s.ea} 不是语料里任何指令的行首（锚落空 / 落在指令中间）`);
    }
  }
  return { ok: problems.length === 0, problems };
}
