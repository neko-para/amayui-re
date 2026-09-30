/**
 * tools/lib/samples.mjs — **`corpus/fixtures/samples.json` 的领域模型**（字段 / 不变量 / 读 / 写 / 自描述）
 *
 * 分层（见 tools/README.md §0）：本文件**不解析 argv、不打印、不碰载荷文件** —— 那些属于 CLI（`tools/fixtures.mjs`）；
 * 清单（另一个域）的模型在 `lib/manifest.mjs`（所以这里 import 的是**模型**，不是 `corpus.mjs` 那个 CLI）。
 *
 * 纪律：
 *   · `samples.json` 只有**一个**写入口（CLI 调本文件的 `saveSamples`）；清单那侧走 `lib/manifest.mjs` 的 `saveManifest`。
 *   · 载荷文件的 mtime 是**判据的一部分**（槽头 = 存档时刻）⇒ 记 instant + 采集时区，跨机器可还原。
 */
import fs from 'node:fs';
import path from 'node:path';

import { DEFAULT_SAMPLES, FIXTURES_DIR } from './paths.mjs';
import { tzOffsetMinutes, wallClock } from './time.mjs';

export { DEFAULT_SAMPLES, FIXTURES_DIR, wallClock };

export const CARRY_ID = 'fixtures/save-samples';
const EXTS = ['DAT', 'STH'];
const FILE_KEY_ORDER = ['name', 'mtimeMs', 'mtimeLocal', 'tzOffsetMinutes'];

/** 工具**拥有**的顶层 `_doc`（写盘时自动注入 ⇒ 指向自描述，不会漂） */
export const SAMPLES_DOC =
  '本文件是**不透明数据**：字段语义 / 不变量 / 怎么查怎么改 见 `pnpm tools fixtures describe`（说明书 corpus/fixtures/samples.md）。不要手改。';

/** 工具层的自我声明：**我动哪片数据、有哪些操作**（`tools/cli.mjs` 的域地图从它派生） */
export const DOMAIN = {
  id: 'fixtures',
  title: '真存档样本（槽 76/77/78/79；文件级事实 + mtime）',
  data: [
    '`corpus/fixtures/samples.json`（本工具的唯一写入口）',
    '`corpus/fixtures/SAVE*.DAT` / `.STH`（载荷，LFS；本工具是它们唯一的增删入口）',
  ],
  access: 'rw（唯一编辑入口；缺省 dry-run，写后复验，不绿回滚）',
  tool: 'tools/fixtures.mjs',
};

export const OPERATIONS = [
  { name: 'list', argv: ['--list'], mutates: false, summary: '槽 / 定位 / 每个文件是否与记录的 instant 一致' },
  { name: 'describe', argv: ['--describe'], mutates: false, summary: '自描述：字段 / 不变量（含谁在守）/ 操作' },
  { name: 'restore-mtime', argv: ['--restore-mtime'], mutates: true, summary: '刚 clone：按记录的 instant 把 mtime 拨回（跨时区也对）[--write]' },
  { name: 'add', argv: ['--add'], mutates: true, summary: '加一个槽（拷文件 + 设 mtime）：`<槽> --from <源目录> [--root <roots 名>] --where <定位>` [--write]' },
  { name: 'refresh', argv: ['--refresh'], mutates: true, summary: '重新取一个槽：`<槽> --from <源目录>`（缺省按登记的 origin 找，没有就如实报出）[--write]' },
  { name: 'refresh-all', argv: ['--refresh-all'], mutates: true, summary: '按登记的 origin 全部重取一遍（没有 origin 就都给 --from）[--write]' },
  { name: 'remove', argv: ['--remove'], mutates: true, summary: '删一个槽（文件 + samples.json 的登记；有 origin 就连它一起摘）：`<槽>` [--write]' },
  { name: 'normalize', argv: ['--normalize'], mutates: true, summary: 'schema 变过后拉回规范形态 [--write]' },
];

/** 字段说明（`--describe` 用；这是**自描述**，不是第二份 schema） */
const SAMPLE_FIELD_DOC = [
  ['slot', '✅', '`^\\d{2,3}$`', '槽号；决定文件名 `SAVE<slot>.DAT` / `.STH`'],
  ['where', '✅', 'string 非空', '它在游戏里的定位（来源方给出的信息，**不是**从字节推导的）'],
  ['files', '✅', '长度 2 的数组', '固定 `DAT` 在前、`STH` 在后'],
  ['files[].name', '✅', 'string', '文件名'],
  ['files[].mtimeMs', '✅', 'number', '**源文件的 instant**（权威值）—— git 不保存 mtime，所以必须记在这里'],
  ['files[].mtimeLocal', '✅', '`yyyy-MM-ddTHH:mm:ss`', '采集时区下的墙上时间（给人看；不要当权威）'],
  ['files[].tzOffsetMinutes', '✅', 'number（东为正，如 UTC+8 = 480）', '把 instant 还原成墙上时间用；**与跑命令的机器时区无关**'],
];

/** 不变量 + **谁在守它**（自校验 / 测试 / 守卫） */
const SAMPLE_INVARIANTS = [
  ['每槽 DAT + STH 成对；槽号形态合法且不重复；where 非空', '本工具写入时自校验（`saveSamples` 写后复验，不绿回滚）'],
  ['每个文件三时间字段齐全', '同上'],
  ['槽头 +264 起七个 u16 的年月日时分秒 == `wallClock(mtimeMs, tzOffsetMinutes)`；星期与日期自洽', '`tools/test/fixtures.test.mjs`'],
  ['目录里的文件集合 == 本文件登记的集合（两边都不许多）', '`tools/test/fixtures.test.mjs`'],
  ['载荷的 `filter=lfs`（**不靠 origin 枚举**）', '`pnpm tools corpus validate` 的守卫 #6'],
];

const pad = (n) => String(n).padStart(2, '0');
const filesOf = (slot) => EXTS.map((e) => `SAVE${slot}.${e}`);

// ───────────────────────────────────────────────── samples.json 读写

export function loadSamples(p = DEFAULT_SAMPLES) {
  return JSON.parse(fs.readFileSync(p, 'utf8'));
}

function order(obj, keys) {
  const out = {};
  for (const k of keys) if (k in obj) out[k] = obj[k];
  for (const k of Object.keys(obj)) if (!(k in out)) out[k] = obj[k];
  return out;
}

/** 规范化：槽按号排序、每个槽的文件固定 DAT/STH 顺序、`_doc` 由工具拥有（= 唯一的"指针"，其余说明都在 `--describe`） */
export function canonicalSamples(doc) {
  const out = {
    schemaVersion: doc.schemaVersion,
    _doc: SAMPLES_DOC,
    samples: [...doc.samples]
      .sort((a, b) => a.slot.localeCompare(b.slot))
      .map((s) => ({
        slot: s.slot,
        where: s.where,
        files: filesOf(s.slot)
          .map((n) => s.files.find((f) => f.name === n))
          .filter(Boolean)
          .map((f) => order(f, FILE_KEY_ORDER)),
      })),
  };
  for (const k of Object.keys(doc)) if (!(k in out)) out[k] = doc[k];
  return `${JSON.stringify(out, null, 2)}\n`;
}

/** 本文件认得的顶层键；其余一律算"多余键"（由 --normalize 报出并剔除） */
export const KNOWN_TOP_KEYS = ['schemaVersion', '_doc', 'samples'];

/** 剔除多余顶层键（schema 变过之后用它把文件拉回规范形态） */
export function stripUnknownTopKeys(doc) {
  const dropped = Object.keys(doc).filter((k) => !KNOWN_TOP_KEYS.includes(k));
  for (const k of dropped) delete doc[k];
  return dropped;
}

export function saveSamples(doc, p = DEFAULT_SAMPLES) {
  const backup = fs.readFileSync(p, 'utf8');
  const text = canonicalSamples(doc);
  const tmp = `${p}.tmp-${process.pid}`;
  fs.writeFileSync(tmp, text, 'utf8');
  fs.renameSync(tmp, p);
  const reread = loadSamples(p);
  const bad = structuralProblems(reread);
  if (bad.length) {
    fs.writeFileSync(p, backup, 'utf8');
    return { ok: false, restored: true, reason: `写后复验未通过（已回滚）：\n  - ${bad.join('\n  - ')}` };
  }
  return { ok: true };
}

export function structuralProblems(doc) {
  const bad = [];
  if (doc?.schemaVersion !== 1) bad.push('schemaVersion 必须是 1');
  if (!Array.isArray(doc?.samples)) return [...bad, 'samples 必须是数组'];
  const seen = new Set();
  for (const s of doc.samples) {
    if (!/^\d{2,3}$/.test(s?.slot ?? '')) bad.push(`槽号形态非法：${JSON.stringify(s?.slot)}`);
    if (seen.has(s.slot)) bad.push(`槽重复：${s.slot}`);
    seen.add(s.slot);
    if (typeof s?.where !== 'string' || s.where.trim() === '') bad.push(`槽 ${s.slot} 缺 where`);
    const want = filesOf(s.slot);
    const got = (s.files ?? []).map((f) => f?.name);
    if (want.length !== got.length || !want.every((n) => got.includes(n))) {
      bad.push(`槽 ${s.slot} 的文件必须是 ${want.join(' + ')}，实际 ${got.join(' + ') || '（空）'}`);
    }
    for (const f of s.files ?? []) {
      if (typeof f?.mtimeMs !== 'number') bad.push(`${f?.name} 的 mtimeMs 必须是数字`);
      if (!/^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}$/.test(f?.mtimeLocal ?? '')) bad.push(`${f?.name} 的 mtimeLocal 形态非法`);
      if (typeof f?.tzOffsetMinutes !== 'number') bad.push(`${f?.name} 缺 tzOffsetMinutes（采集时区，用于把 instant 还原成墙上时间）`);
    }
  }
  return bad;
}

/** 自描述：**JSON 是不透明数据**，字段语义 / 不变量 / 操作一律由控制脚本给出（文档只指向它） */
export function describe() {
  return {
    file: 'corpus/fixtures/samples.json',
    purpose: '这一批真存档样本的结构化描述（每个样本 = 一个槽 = 一对 .DAT/.STH）；只管**文件级事实**',
    notHere:
      '存储去向（LFS）与"这批样本在清单里是哪一条"不在这里 —— 见 corpus/assets.json 的 `fixtures/save-samples`（说明书 corpus/assets.md）。' +
      '★ 该条目是**自足条目**（固化资源，没有加工链）⇒ **不登记 origin**：样本的"来源"就是入库的那一份本身',
    topLevel: { schemaVersion: 1, _doc: '由本工具拥有（写盘时自动注入指向本自描述的指针）', samples: '见下方字段表' },
    fields: SAMPLE_FIELD_DOC.map(([name, req, type, desc]) => ({ name, req, type, desc })),
    invariants: SAMPLE_INVARIANTS.map(([text, enforcedBy], i) => ({ id: i + 1, text, enforcedBy })),
    operations: OPERATIONS,
    writePath:
      '只有本工具（唯一编辑入口）；缺省 dry-run，--write 才落盘；写后复验，不绿回滚。给了 `--from`（加样本 / 重取）时还会经 corpus.mjs 的 saveManifest 同步清单里那条 origin ⇒ 清单仍只有一个写入口。**不要手改 JSON，也不要手工 touch**。',
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
  L.push('| 字段 | 必填 | 类型 | 说明 |');
  L.push('|---|---|---|---|');
  for (const f of d.fields) L.push(`| \`${f.name}\` | ${f.req} | ${f.type} | ${f.desc} |`);
  L.push('');
  L.push('## 不变量（含"谁在守它"）');
  for (const c of d.invariants) L.push(`${c.id}. ${c.text}　—　${c.enforcedBy}`);
  L.push('');
  L.push('## 操作');
  for (const o of d.operations) L.push(`* \`${o.name}\`${o.mutates ? '（会写）' : ''} —— ${o.summary}　→ \`pnpm tools fixtures ${o.name}\``);
  L.push('');
  L.push(`写入口：${d.writePath}`);
  return `${L.join('\n')}\n`;
}

// ───────────────────────────────────────────────── 领域小工具

/** 记一个文件的 mtime 事实（instant 权威 + 采集时区下的墙上时间） */
export function fileRec(p) {
  const st = fs.statSync(p);
  const tz = tzOffsetMinutes();
  return { name: path.basename(p), mtimeMs: Math.round(st.mtimeMs), mtimeLocal: wallClock(st.mtimeMs, tz), tzOffsetMinutes: tz };
}

