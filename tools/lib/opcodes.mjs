/**
 * tools/lib/opcodes.mjs —— **AGE 脚本指令表的领域模型**（读登记来源 → 机械派生格式层四列）
 *
 * ## 为什么存在这个派生（不是"顺手复制一份"）
 *
 * 旧仓 `scripts/asm/opcodes.json` 里混了三类东西，判据是**能不能再观测一次**：
 *
 * | | 字段 | 类别 | 复核路径 |
 * |---|---|---|---|
 * | ✅ | `opcode` / `argc` | **观测**：引擎实际怎么切字节流 | 格式层正在用（104/106 个真实 BIN 往返逐字节相同） |
 * | ✅ | `handler` | **观测**：引擎分派表把 opcode 映射到哪个处理函数 | 引擎分派表 → 函数 **EA**；★ **当前未知入口但路径存在** |
 * | ⛔ | `status` | **人工自述标签**（`已核对` / `仅映射`） | **没有机械复核路径** ⇒ 丢掉不损失可复核信息 |
 *
 * ★ 所以 `handler` 与 `argc` 是**同类**（都是观测），只是格式层现在只用得到 `argc`：
 *   `handler` **登记继承**（`knowledge/opcode-handlers`），`status` **登记弃置**（`knowledge/opcode-status-labels`）。
 *   —— "要么都继承"的那条路：两个登记条目都在清单里，派生器只吐四列。
 *
 * ## 权威性：`argc` 的权威来自**引擎**，不是这份表
 * 拿 `handler` 指到的处理函数、数出它**推进字节偏移的位置**，就能机械算出每条指令的长度
 * ⇒ 本表里的 `argc` 是**待复核副本**。在 K 线复核之前，它的凭据是**可执行观测**（上面那条 104/106）。
 *
 * ★ 派生是**确定性的**：同输入 ⇒ 同字节（键序固定、一个 opcode 一行）。
 */
import fs from 'node:fs';
import path from 'node:path';

/** 格式层保留的字段（顺序即落盘键序） */
export const FORMAT_FIELDS = ['opcode', 'argc', 'name', 'aliases'];

/**
 * **观测**字段：与 `argc` 同类（都能再观测一次复核）⇒ 都**继承**。
 * `handler` 格式层不用，但它登记在清单里（`knowledge/opcode-handlers`）。
 */
export const OBSERVED_FIELDS = ['opcode', 'argc', 'handler'];
/** **人工自述标签**：没有机械复核路径 ⇒ 唯一被丢掉的字段（留痕见 `knowledge/opcode-status-labels`） */
export const LABEL_FIELDS = ['status'];
/** 兼容旧名：派生时要丢弃的字段 = 自述标签（**不含** handler —— 它是观测） */
export const KNOWLEDGE_FIELDS = LABEL_FIELDS;

export const DOMAIN = {
  id: 'opcodes',
  title: 'AGE 脚本指令表（从旧仓旧表机械派生格式层四列）',
  data: [
    '`packages/age-format/src/asm/instruction-set.json`（**唯一写入口就是本工具**；消费者是 `src/asm/opcodes.mts` 的 `OPCODE_TABLE` —— 它随模块自带、走 ESM JSON import，不再用 `fs` 读）',
  ],
  access: 'rw（唯一写入口；缺省 dry-run，写后回读复验，不绿回滚）',
  tool: 'tools/opcodes.mjs',
};

export const OPERATIONS = [
  { name: 'report', argv: ['--report'], mutates: false, summary: '对账：旧表条目数 / 各类字段数 / 派生后会丢哪些字段、哪些取值' },
  { name: 'derive', argv: ['--derive'], mutates: true, summary: '从旧表派生格式层四列并写入（缺省 dry-run）[--write]' },
  { name: 'describe', argv: ['--describe'], mutates: false, summary: '自描述：口径 / 字段 / 不变量 / 操作' },
];

export const CHECK_TITLES = new Map([
  [1, '派生表只含格式层四列（`handler` / `status` 一律不出现）'],
  [2, '格式层必需信息齐备：`opcode` 唯一、`argc` 是 0..255 整数、`name` 是字符串、`aliases` 是字符串数组'],
  [3, '落盘形态规范：一个 opcode 一行、键序固定、同输入同字节'],
]);

/** 从旧表（原始 JSON 数组）里抽出格式层四列 */
export function deriveFormatTable(rawEntries) {
  if (!Array.isArray(rawEntries)) throw new Error('旧表不是数组');
  const out = [];
  for (const e of rawEntries) {
    if (typeof e?.opcode !== 'number' || typeof e?.argc !== 'number') {
      throw new Error(`旧表有条目缺 opcode/argc：${JSON.stringify(e).slice(0, 120)}`);
    }
    out.push({
      opcode: e.opcode,
      argc: e.argc,
      name: typeof e.name === 'string' ? e.name : '',
      aliases: Array.isArray(e.aliases) ? [...e.aliases] : [],
    });
  }
  out.sort((a, b) => a.opcode - b.opcode);
  return out;
}

/** 规范序列化：一个 opcode 一行（diff 干净、同输入同字节） */
export function canonicalTableString(table) {
  const lines = table.map(
    (e) =>
      `  {"opcode": ${e.opcode}, "argc": ${e.argc}, "name": ${JSON.stringify(e.name)}, "aliases": [${e.aliases
        .map((a) => JSON.stringify(a))
        .join(', ')}]}`,
  );
  return `[\n${lines.join(',\n')}\n]\n`;
}

/** 读旧表（旧仓只读来源） */
export function loadSourceTable(file) {
  return JSON.parse(fs.readFileSync(file, 'utf8'));
}

/** 校验派生表本身（不依赖任何外部文件） */
export function validateTable(table) {
  const problems = [];
  if (!Array.isArray(table) || table.length === 0) problems.push('表为空或不是数组');
  const seen = new Set();
  for (const [i, e] of (table ?? []).entries()) {
    const at = `entries[${i}]${e?.opcode !== undefined ? ` (opcode ${e.opcode})` : ''}`;
    for (const k of Object.keys(e ?? {})) {
      if (!FORMAT_FIELDS.includes(k)) problems.push(`${at}: 出现了非格式层字段 ${k}（知识层字段不得入库）`);
    }
    for (const k of FORMAT_FIELDS) if (!(k in (e ?? {}))) problems.push(`${at}: 缺格式层字段 ${k}`);
    if (typeof e?.opcode !== 'number' || !Number.isInteger(e.opcode) || e.opcode < 0) problems.push(`${at}: opcode 非非负整数`);
    if (seen.has(e?.opcode)) problems.push(`${at}: opcode 重复`);
    seen.add(e?.opcode);
    if (typeof e?.argc !== 'number' || !Number.isInteger(e.argc) || e.argc < 0 || e.argc > 255) {
      problems.push(`${at}: argc 必须是 0..255 的整数（它决定指令边界）`);
    }
    if (typeof e?.name !== 'string') problems.push(`${at}: name 必须是字符串`);
    if (!Array.isArray(e?.aliases) || e.aliases.some((a) => typeof a !== 'string')) problems.push(`${at}: aliases 必须是字符串数组`);
  }
  const checks = [
    { id: 1, text: CHECK_TITLES.get(1), problems: problems.filter((p) => p.includes('非格式层字段')) },
    { id: 2, text: CHECK_TITLES.get(2), problems: problems.filter((p) => !p.includes('非格式层字段')) },
    { id: 3, text: CHECK_TITLES.get(3), problems: [] },
  ];
  return { checks, failures: checks.filter((c) => c.problems.length > 0).length, entries: (table ?? []).length };
}

/** 对账报告：旧表里有什么、派生后会丢什么（"丢了什么必须说得出来"） */
export function report(rawEntries) {
  const counts = {};
  const handlerValues = new Set();
  const statusValues = new Set();
  let named = 0;
  let withAliases = 0;
  for (const e of rawEntries) {
    for (const k of Object.keys(e ?? {})) counts[k] = (counts[k] ?? 0) + 1;
    if (e.handler) handlerValues.add(e.handler);
    statusValues.add(String(e.status ?? ''));
    if (e.name) named += 1;
    if ((e.aliases ?? []).length) withAliases += 1;
  }
  const all = Object.keys(counts);
  const dropped = all.filter((k) => LABEL_FIELDS.includes(k));
  const inherited = all.filter((k) => OBSERVED_FIELDS.includes(k));
  return {
    entries: rawEntries.length,
    fieldCounts: counts,
    formatFields: FORMAT_FIELDS,
    /** 继承下来的**观测**（含 handler —— 格式层不用，但有登记条目） */
    inheritedFields: inherited,
    /** 唯一被丢掉的：人工自述标签 */
    droppedFields: dropped,
    droppedHandlerCount: 0,
    handlerCount: inherited.includes('handler') ? counts.handler : 0,
    distinctHandlers: handlerValues.size,
    statusValues: [...statusValues],
    namedEntries: named,
    entriesWithAliases: withAliases,
  };
}

export function describe() {
  return {
    file: 'packages/age-format/src/asm/instruction-set.json',
    purpose:
      'AGE 脚本的**格式层**指令表：只含 opcode / argc / name / aliases —— 从旧仓旧表**机械派生**，' +
      '不搬运知识层字段（handler / status 是引擎逆向结论，归知识线）',
    source:
      '旧仓 `scripts/asm/opcodes.json`（只读来源）。`--report` 会给出它的条目数与将丢弃的字段/取值。',
    fields: [
      ['opcode', '✅', '非负整数', '指令号；唯一'],
      ['argc', '✅', '0..255 整数', '★ **唯一决定指令边界的字段**：一条指令 = `4 + argc*8` 字节'],
      ['name', '✅', '字符串（可为空串）', '助记符；空串 ⇒ 显示名退化为规范形式 `iXXX`（不落表，由查找器生成）'],
      ['aliases', '✅', '字符串数组', '别称；同一助记符多处注册时**先注册者胜**'],
    ],
    dropped: KNOWLEDGE_FIELDS.map((f) => ({
      field: f,
      why: '引擎逆向结论（opcode → 引擎函数 / 核对状态）⇒ 属知识层，K3 通过前不得进新仓台账；且被 knowledge-rebuild.md §2 判为 A 类"数据丢弃"',
    })),
    invariants: [...CHECK_TITLES].map(([id, text]) => ({ id, text })),
    operations: OPERATIONS,
    writePath: '只有 `tools/opcodes.mjs --derive`（唯一写入口）；缺省 dry-run，写后回读复验，不绿回滚。**不要手改 JSON**。',
  };
}

export function describeText(d = describe()) {
  const L = [];
  L.push(`# ${d.file} —— 自描述（由控制脚本给出，文档不复述 schema）`);
  L.push('');
  L.push(`用途：${d.purpose}`);
  L.push(`来源：${d.source}`);
  L.push('');
  L.push('## 字段（格式层全部字段，仅此四个）');
  L.push('| 字段 | 必填 | 取值 | 说明 |');
  L.push('|---|---|---|---|');
  for (const [n, req, type, desc] of d.fields) L.push(`| \`${n}\` | ${req} | ${type} | ${desc} |`);
  L.push('');
  L.push('## ★ 有意丢弃的字段（知识层）');
  for (const x of d.dropped) L.push(`* \`${x.field}\` —— ${x.why}`);
  L.push('');
  L.push('## 不变量（`--derive` 写前写后都验）');
  for (const c of d.invariants) L.push(`${c.id}. ${c.text}`);
  L.push('');
  L.push('## 操作');
  for (const o of d.operations) L.push(`* \`${o.name}\`${o.mutates ? '（会写）' : ''} —— ${o.summary}　→ \`pnpm tools opcodes ${o.name}\``);
  L.push('');
  L.push(`写入口：${d.writePath}`);
  return `${L.join('\n')}\n`;
}

/** 派生 + 写盘（先验后写、写后回读复验；不绿回滚） */
export function deriveTo(rawEntries, targetFile) {
  const table = deriveFormatTable(rawEntries);
  const text = canonicalTableString(table);
  const pre = validateTable(table);
  if (pre.failures > 0) return { ok: false, reason: `派生结果自身不合规：${JSON.stringify(pre.checks.filter((c) => c.problems.length))}` };
  const backup = fs.existsSync(targetFile) ? fs.readFileSync(targetFile, 'utf8') : null;
  try {
    fs.writeFileSync(targetFile, text, 'utf8');
    const reread = JSON.parse(fs.readFileSync(targetFile, 'utf8'));
    const post = validateTable(reread);
    if (post.failures > 0) throw new Error('回读复验未通过');
    if (fs.readFileSync(targetFile, 'utf8') !== text) throw new Error('回读字节与写出不一致');
  } catch (err) {
    if (backup === null) fs.unlinkSync(targetFile);
    else fs.writeFileSync(targetFile, backup, 'utf8');
    return { ok: false, reason: `写/复验失败（已回滚）：${err.message}` };
  }
  return { ok: true, entries: table.length, bytes: Buffer.byteLength(text) };
}
