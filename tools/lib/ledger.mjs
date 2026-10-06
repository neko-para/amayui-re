#!/usr/bin/env node
/**
 * tools/lib/ledger.mjs —— **知识台账**（`data/ledger/`）的领域模型：schema / 投影 / 校验 / 派生 DB 构建
 *
 * 这一层只做"数据怎么读怎么写"与不变量；**不认识命令行**（参数 → 模型 → 输出 是 `tools/ledger.mjs` 的事）。
 *
 * ## 三句话口径
 * 1. **真源是 append-only 文本**：一个 kind 一个月一个 `.jsonl` 文件，**一行一条记录**，行自带 ULID。
 *    ⇒ 两台设备各自追加的是**不同行**，git 三路合并天然干净；每条结论都有 `git log -L` 可追的链条。
 * 2. **DB 是派生只读查询层**：落 `.cache/`、永不入库、**删掉必须能一条命令重建**（判据见 `logicalDigest`）。
 * 3. **`status` 是"作者声称"，不是"事实"** ⇒ **有效状态由投影现算**（`project()`）：
 *    观察失效 ⇒ 机械降级 `stale`（**不删除、不改写日志**）；冲突 ⇒ 显式化成产物。
 *    ★ **任何"改"都是追加一条新记录**（`retract` 用 `replaces` 指向它），日志**永不就地改**。
 *
 * ## 锚点（本模型唯一的"轻"设计，但两条都不可谈判）
 * * **两种形态、都不是行号**：
 *   - `bin`：二进制 **EA**（`{type:'bin', repo, path, ea, len?, sha256?}`）——
 *     EA → 文件偏移由 PE 节表现算（`peOffsetOf`），**不落盘成行号**；
 *   - `guard`：**可执行守卫用例**（`{type:'guard', repo, path, test}`）——
 *     `test` 是**测试名里的一个片段**（子串匹配，与 `requirements` 的 `verify` 同一判据）。
 * * **`repo` 决定去哪找**：`self` = 本仓；`reference` = 只读参考仓（旧仓）。
 *   ★ 少了这一维，K2 完成锚点重挂之前 79 条 B 类的锚**全部**会被判"不存在" ⇒ 集体假红。
 *   ⇒ 只读参考仓**不在本机**时是 `warning`（不可校验），**不是** `error`。
 *
 * ## 为什么 `subject` 必须避免用 name
 * 旧仓 `fields.json` 实测：9 组同名跨 scope、2 组同 `scope+offset` 双 `confirmed`。
 * ⇒ `subject` 的规范形态是 **`scope+offset` 这类稳定键**（例：`Engine+0x5D880`），模型里只查"非空 + 无空白"。
 */
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { createRequire } from 'node:module';

// ─────────────────────────────────────────────────────────── 常量（枚举只有这一份）

/**
 * 记录种类 = **台账自己的机制分类**（一行记录是哪一类事件）。
 * ★ 别与 `system`（域）混：`kind` 回答"这条记录是什么"，`system` 回答"它关于引擎的哪一块"。
 * ★ `domain` 这一类就是**域词汇表本身**（见文件头的"分类轴"一节）—— 不另造第二个真源。
 */
export const KINDS = ['claim', 'observation', 'note', 'domain'];
/** 作者声称的状态 */
export const STATUSES = ['proposed', 'accepted', 'retracted'];
/** 投影算出来的**有效**状态（★ 与上面的 STATUSES 不是一个东西） */
export const EFFECTIVE = ['accepted', 'proposed', 'stale', 'conflict', 'retracted'];
/** 锚点形态 */
export const ANCHOR_TYPES = ['bin', 'guard'];
/** 锚点指向哪个仓（★ 第 2 条口径的关键） */
export const REPOS = ['self', 'reference'];
/** 只读参考仓的键（在 `corpus/assets.json` 的 `roots` 里） */
export const REFERENCE_ROOT_KEY = 'oldRepo';

/**
 * ★ **域处置**（闭集合）—— 只出现在 `kind=domain` 的记录上，它就是词表的"覆盖机制"。
 *
 * | disposition | 含义 | 历史行里的旧值怎么办 |
 * |---|---|---|
 * | `added` | 一个新域进词表 | 纯追加，不影响任何历史行 |
 * | `renamed` | 改名（**含义没变、只是叫法归一**） | `aliases` 让它**仍可解析** ⇒ 历史行一个字节都不用动 |
 * | `merged` | 多个旧域并成一个 | 同上（旧域名进 `aliases`） |
 * | `split` | 一个域**被拆成多个**（含义变了） | ★ **别名救不了** ⇒ 必须走**追加更正记录**（台账的 `replaces`），不许靠词表悄悄改含义 |
 */
export const DISPOSITIONS = ['added', 'renamed', 'merged', 'split'];
/** `domain` 记录专属字段（别的 kind 上出现即错 —— 同"缺陷专属字段"的纪律） */
export const DOMAIN_ONLY = ['disposition', 'aliases', 'splitInto'];

/** 域名的合法形态：非空、无空白（`system` 写了就必须过这一关；不写则进"待定域"） */
export const isDomainToken = (v) => typeof v === 'string' && v.trim() !== '' && !/\s/.test(v);

export const ID_PREFIX = 'KN-';
export const ULID_RE = /^[0-9A-HJKMNP-TV-Z]{26}$/;
/** 一行一条；一个 kind 一月一个文件 */
export const FILE_RE = /^(\d{4})-(\d{2})\.jsonl$/;

/** 默认落点（相对仓库根） */
export const DEFAULT_LEDGER_DIR = 'data/ledger';
export const DEFAULT_MANIFEST = 'corpus/assets.json';
export const DEFAULT_CACHE_DIR = '.cache';
export const DEFAULT_DB_NAME = 'ledger.sqlite';

export const DOMAIN = {
  id: 'ledger',
  title: '知识台账（append-only 文本真源 + 派生只读 SQLite 查询层）',
  data: [
    '`data/ledger/<kind>/<YYYY-MM>.jsonl`（**真源**：一行一条记录，自带 ULID）',
    '`.cache/ledger.sqlite`（**派生只读查询层**：gitignore、永不入库、可删可重建）',
  ],
  access: 'rw（唯一写入口是本工具；缺省 dry-run，写后回读复验，不绿回滚）',
  tool: 'tools/ledger.mjs',
};

export const OPERATIONS = [
  { name: 'describe', argv: ['--describe'], mutates: false, summary: '自描述：字段 / 枚举 / 不变量 / 锚点与分类轴口径 / 操作（schema 唯一真源）' },
  { name: 'report', argv: ['--report'], mutates: false, summary: '体检：各 kind 条数 / 有效状态 / 锚点可解析率 / 冲突数 / **域**分布（★ 不叫 status —— 那是字段名）' },
  { name: 'list', argv: ['--list'], mutates: false, summary: '列记录：按 `--only-kind` / `--only-effective` / `--system` / `--subject` / `--status` 筛（`--json` 给机器读）' },
  { name: 'domains', argv: ['--domains'], mutates: false, summary: '★ 域词汇表：当前值 / 别名链 / 被拆分的值 + 逐值解析表（空词表也要能看）' },
  { name: 'show', argv: ['--show'], mutates: false, summary: '一条记录的全文 + 它的锚点解析结果 + 冲突对家' },
  { name: 'add', argv: ['--add'], mutates: true, summary: '追加一条记录（缺省 dry-run）[--write]：`--kind --system --subject --claim --anchor <json>…`' },
  { name: 'retract', argv: ['--retract'], mutates: true, summary: '撤回一条（**追加**一条 `replaces` 它的记录，不改历史）[--write]' },
  { name: 'validate', argv: ['--validate'], mutates: false, summary: '不变量（红 = 退出码 1）；只读参考仓不在场时只 warn' },
  { name: 'rebuild-db', argv: ['--rebuild-db'], mutates: true, summary: '由文本真源确定性重建派生 SQLite（缺省 dry-run）[--write]' },
  { name: 'compact', argv: ['--compact'], mutates: true, summary: '分片归位 + 同 id 去重（只重排，**不删任何结论**）[--write]' },
];

/** 不变量标题（`validate` 与 `describe` 共用这一份） */
export const CHECK_TITLES = new Map([
  [1, '形态：记录 schema 合法（ULID / ISO 时刻 / kind / **system（可缺省，写了必须无空白）** / subject / claim / status / 锚点非空）、' +
    '域专属字段只出现在 `kind=domain` 上、同文件内 id 不重复'],
  [2, '追加序：每条记录的 `at` 与文件名月份自洽；文件内 ULID **严格递增**（只许追加，不许插中间）'],
  [3, '锚点：形态合法、`repo` 合法且**不越出对应仓库根**；`self` 锚必须落在**本仓已跟踪**的文件上'],
  [4, '观察可再校验：`accepted`（或 `stale`/`conflict` 的）记录，其 `self` 锚必须**当场解析得到**；' +
    '`reference` 锚在只读参考仓不在场时只 warn'],
  [5, '引线与冲突：`replaces` 必须指向**已存在**的记录且不成环；同一 `kind+subject` 上多个不同 claim ⇒ **显式冲突**'],
  [6, '派生 DB：删掉本地 DB 后一条命令能重建，且**同输入同逻辑内容**'],
  [7, '分类轴：`system` **写了就必须**无空白、且**沿别名链能追到当前域词汇表**（追不到 ⇒ 待裁决，点名但不静默过）；' +
    '**没写** ⇒ 进"待定域"名单（不报红）；被 `disposition=split` 拆过的值**不许当别名放过**（必须走追加更正记录的数据迁移）；词表自身无歧义'],
]);

// ─────────────────────────────────────────────────────────── ULID / 时间

const B32 = '0123456789ABCDEFGHJKMNPQRSTVWXYZ';
/** 生成 ULID（48 位毫秒时间 + 80 位随机）⇒ **字典序 == 时间序**，直接当追加序用 */
export function newUlid(now = Date.now(), rand = crypto.randomBytes(10)) {
  let ts = '';
  let t = now;
  for (let i = 0; i < 10; i += 1) {
    ts = B32[t % 32] + ts;
    t = Math.floor(t / 32);
  }
  let r = '';
  for (const b of rand) r += B32[(b >> 5) & 31] + B32[b & 31];
  return (ts + r).slice(0, 26);
}

/** ULID → 时刻（毫秒）。非法返回 null */
export function ulidTime(ulid) {
  if (!ULID_RE.test(ulid)) return null;
  let t = 0;
  for (const ch of ulid.slice(0, 10)) {
    const v = B32.indexOf(ch);
    if (v < 0) return null;
    t = t * 32 + v;
  }
  return t;
}

/** ULID 里的时刻 → `YYYY-MM`（决定它落哪个文件） */
export function monthOf(ulid, tzOffsetMinutes = 0) {
  const t = ulidTime(ulid);
  if (t === null) throw new Error(`ULID 非法：${ulid}`);
  const d = new Date(t - tzOffsetMinutes * 60_000);
  return `${d.getUTCFullYear()}-${String(d.getUTCMonth() + 1).padStart(2, '0')}`;
}

/** ISO-8601（UTC，毫秒）—— 时刻的唯一写法，避免两台机器写出两种形态 */
export const isoNow = (now = Date.now()) => new Date(now).toISOString();
export const ISO_RE = /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$/;

// ─────────────────────────────────────────────────────────── 读写（真源）

/** canonical 序列化：**一行一个字段序固定的 JSON** ⇒ 同输入同字节、diff 干净 */
export function serializeRecord(r) {
  if (!Array.isArray(r.anchor) || r.anchor.length === 0) {
    throw new Error('serializeRecord：anchor 必须是非空数组');
  }
  for (const a of r.anchor) {
    // ★ 未知 type 必须**响亮失败**：早先这里按 `=== 'bin' ? … : orderGuard(…)` 写，
    //   于是 `type:'binary'` 被静默改写成 `guard` —— 写路径悄悄改坏数据，正是最该防的那种 bug。
    if (!ANCHOR_TYPES.includes(a.type)) {
      throw new Error(`serializeRecord：锚点 type 非法 ${JSON.stringify(a.type)}（应为 ${ANCHOR_TYPES.join('/')}）`);
    }
    if (!REPOS.includes(a.repo)) {
      throw new Error(`serializeRecord：锚点 repo 非法 ${JSON.stringify(a.repo)}（应为 ${REPOS.join('/')}）`);
    }
  }
  const out = {
    id: r.id,
    at: r.at,
    kind: r.kind,
  };
  // ★ `system` 可缺省：**没写就不要写进盘**（`undefined` 会让 JSON.stringify 直接丢掉这个键 ——
  //   但显式判一下更好读，也免得将来有人给个 `''`）
  if (isDomainToken(r.system)) out.system = r.system;
  out.subject = r.subject;
  out.claim = r.claim;
  out.anchor = r.anchor.map((a) => (a.type === 'bin' ? orderBin(a) : orderGuard(a)));
  if (r.status !== undefined) out.status = r.status;
  if (r.replaces !== undefined) out.replaces = r.replaces;
  // ★ 域专属字段（只在 kind=domain 上；其它 kind 出现即由不变量 #1 报错，这里只管序列化顺序）
  if (r.disposition !== undefined) out.disposition = r.disposition;
  if (r.aliases !== undefined) out.aliases = [...r.aliases].sort();
  if (r.splitInto !== undefined) out.splitInto = [...r.splitInto].sort();
  if (r.note !== undefined) out.note = r.note;
  if (r.tags !== undefined) out.tags = [...r.tags].sort();
  return JSON.stringify(out);
}

const orderBin = (a) => {
  const o = { type: 'bin', repo: a.repo, path: a.path, ea: a.ea };
  if (a.len !== undefined) o.len = a.len;
  if (a.sha256 !== undefined) o.sha256 = a.sha256;
  return o;
};
const orderGuard = (a) => {
  const o = { type: 'guard', repo: a.repo, path: a.path, test: a.test };
  return o;
};

/** 台账文件清单（按 kind / 文件名排序 —— 顺序不依赖目录枚举顺序） */
export function listFiles(ledgerDir) {
  const out = [];
  if (!fs.existsSync(ledgerDir)) return out;
  for (const kind of fs.readdirSync(ledgerDir, { withFileTypes: true })) {
    if (!kind.isDirectory()) continue;
    const dir = path.join(ledgerDir, kind.name);
    for (const f of fs.readdirSync(dir, { withFileTypes: true })) {
      if (!f.isFile() || !FILE_RE.test(f.name)) continue;
      out.push({ kind: kind.name, month: f.name.slice(0, 7), file: path.join(dir, f.name) });
    }
  }
  return out.sort((a, b) => a.file.localeCompare(b.file));
}

/**
 * 读全部记录（**不做任何校验**，坏了也照样读出来 —— 校验是 `validateAll` 的事）。
 * @returns {{records: Array, problems: Array<{file:string,line:number,reason:string}>}}
 */
export function readRecords(ledgerDir) {
  const records = [];
  const problems = [];
  for (const { kind, file } of listFiles(ledgerDir)) {
    const text = fs.readFileSync(file, 'utf8');
    text.split('\n').forEach((line, i) => {
      if (line.trim() === '') return;
      let r;
      try {
        r = JSON.parse(line);
      } catch (err) {
        problems.push({ file, line: i + 1, reason: `不是合法 JSON：${err.message}` });
        return;
      }
      records.push({ ...r, _file: file, _line: i + 1, _fileKind: kind });
    });
  }
  return { records, problems };
}

/** 追加一行（**唯一写入口的底层**）：返回写盘后的回读结果，供调用方复验 */
export function appendRecord(ledgerDir, r) {
  const dir = path.join(ledgerDir, r.kind);
  fs.mkdirSync(dir, { recursive: true });
  const file = path.join(dir, `${monthOf(r.id)}.jsonl`);
  const before = fs.existsSync(file) ? fs.readFileSync(file, 'utf8') : '';
  const next = before + (before === '' || before.endsWith('\n') ? '' : '\n') + serializeRecord(r) + '\n';
  fs.writeFileSync(file, next);
  // ★ 写后回读复验：不绿回滚（与 requirements 的 saveNode 同一条纪律）
  const after = fs.readFileSync(file, 'utf8');
  if (after !== next || !after.includes(serializeRecord(r))) {
    fs.writeFileSync(file, before);
    throw new Error(`写后回读复验失败，已回滚：${file}`);
  }
  return { file, line: after.split('\n').length - 1 };
}

// ─────────────────────────────────────────────────────────── 锚点解析

/**
 * PE 里 `EA → 文件偏移`（★ EA 是**虚拟地址 VA**，既不能直接当偏移、**也不能直接当 RVA**）。
 *
 * ★ **实测踩过（由独立评审复现）**：节表里的 `VirtualAddress` 是 **RVA**，而锚点里的 EA 是
 * IDA 报的 **VA**（`Imagebase 400000` + RVA）。第一版拿 `ea` 直接和 RVA 比 ⇒ 对**任何真实镜像恒 null**，
 * 报出"EA 不在任何 PE 节里（或不是 PE）" —— 那句话**看起来像正常的地址错误**，实际是口径错。
 * 单元测试没红是因为合成 PE 的 ImageBase 是 0（VA ≡ RVA）。
 *
 * 口径：`rva = ea - ImageBase`；`ImageBase === 0` 时按"给的就是 RVA"兜底（合成夹具与 PE32+ 的稳健处理）。
 * @returns {number|null} 映射不到（不在任何节里）返回 null
 */
export function peOffsetOf(buf, ea) {
  return peMap(buf, ea)?.offset ?? null;
}

/**
 * 同上的完整版：连**映射的依据**一起给出来（诊断用 —— "为什么这个 EA 映射不到"要有话说）。
 * @returns {{rva:number, imageBase:number, section:string, offset:number}|null}
 */
export function peMap(buf, ea) {
  if (buf.length < 0x40 || buf.toString('latin1', 0, 2) !== 'MZ') return null;
  const peAt = buf.readUInt32LE(0x3c);
  if (peAt + 24 > buf.length || buf.toString('latin1', peAt, peAt + 4) !== 'PE\0\0') return null;
  const nSections = buf.readUInt16LE(peAt + 6);
  const optSize = buf.readUInt16LE(peAt + 20);
  const imageBase = peImageBase(buf, peAt);
  const rva = imageBase === 0 ? ea >>> 0 : (ea >>> 0) - imageBase;
  const table = peAt + 24 + optSize;
  for (let i = 0; i < nSections; i += 1) {
    const s = table + i * 40;
    if (s + 40 > buf.length) return null;
    const name = buf.toString('latin1', s, s + 8).replace(/\0+$/, '');
    const va = buf.readUInt32LE(s + 12); // ← RVA
    const rawSize = buf.readUInt32LE(s + 16);
    const rawPtr = buf.readUInt32LE(s + 20);
    if (va === 0) continue;
    if (rva >= va && rva < va + Math.max(rawSize, 1)) {
      return { rva, imageBase, section: name, offset: rawPtr + (rva - va) };
    }
  }
  return null;
}

/** PE 的 ImageBase：PE32（0x10B）在可选头 +28 是 u32；PE32+（0x20B）在 +24 是 u64 */
export function peImageBase(buf, peAt) {
  const magic = buf.readUInt16LE(peAt + 24);
  if (magic === 0x20b) return Number(buf.readBigUInt64LE(peAt + 24 + 24));
  if (magic === 0x10b) return buf.readUInt32LE(peAt + 24 + 28);
  return 0;
}

/** 锚点的人读形态（日志 / 报告里一律用它，别各写各的） */
export const anchorText = (a) =>
  a.type === 'bin' ? `${a.repo}:${a.path}@${a.ea}` : `${a.repo}:${a.path}#${a.test}`;

/**
 * 从一个测试文件里抽出**用例名**（`test('…')` / `it('…')` / `describe('…')` 的字面量）。
 * ★ 为什么不能只做子串匹配：实测教训 —— `test:'不存在'` 这种短串会在**断言消息、注释**里命中，
 *   于是"这条锚指向的用例根本不存在"被判成绿的。判据必须落到**真正的用例名**上（旧仓 `guard-spec` 的教训同源）。
 */
export function testNames(text) {
  const out = [];
  const re = /\b(?:test|it|describe)\s*\(\s*(?:'((?:[^'\\]|\\.)*)'|"((?:[^"\\]|\\.)*)"|`((?:[^`\\]|\\.)*)`)/g;
  let m;
  while ((m = re.exec(text)) !== null) out.push(m[1] ?? m[2] ?? m[3]);
  return out;
}

/**
 * 解析一个锚点：**它现在还能不能重校验**。
 * @returns {{ok:boolean, kind:'ok'|'warning'|'error', why:string, file?:string}}
 *   * `self` 锚：文件必须在**本仓**；`guard` 还要**用例名**真的在该文件里（不是"随便一个子串"）；
 *     `bin` 还要算得出 PE 偏移、且在给了 `sha256` 时**逐字节对上**（这是"这条观测没被换掉"的唯一证据）。
 *   * `reference` 锚：只读参考仓**不在场** ⇒ `warning`（不可校验，**不是**"失效"）——
 *     否则 K2 重挂之前 79 条 B 类会集体假红。
 */
export function resolveAnchor(a, { repoRoot, referenceRoot, tracked } = {}) {
  const where = a.repo === 'self' ? repoRoot : referenceRoot;
  if (!where) {
    return { ok: false, kind: 'warning', why: '只读参考仓不在场（无法校验，按 warn 处理）' };
  }
  if (typeof a.path !== 'string' || a.path === '') {
    // ★ 不抛：形态问题是**不变量 #3 的事**（报告里要看得见），这里只是"解析不了"
    return { ok: false, kind: 'error', why: `锚点 path 非法：${JSON.stringify(a.path)}` };
  }
  if (!ANCHOR_TYPES.includes(a.type)) {
    // ★ 必须显式挡：否则未知 type 会掉进 bin 分支，报出"缺 ea"这种**误导性**的错
    return { ok: false, kind: 'error', why: `锚点 type 非法：${JSON.stringify(a.type)}（应为 ${ANCHOR_TYPES.join('/')}）` };
  }
  const abs = path.resolve(where, a.path);
  const rel = path.relative(where, abs);
  if (rel.startsWith('..') || path.isAbsolute(rel)) {
    return { ok: false, kind: 'error', why: `锚点越出仓库根：${a.path}` };
  }
  if (!fs.existsSync(abs) || !fs.statSync(abs).isFile()) {
    // 本仓的"不存在"是**真红**；参考仓的不存在只在"参考仓在场却没这个文件"时才是红
    return { ok: false, kind: 'error', why: `文件不存在：${a.repo}:${a.path}` };
  }
  if (tracked && a.repo === 'self' && !tracked.has(a.path.replace(/\\/g, '/'))) {
    return { ok: false, kind: 'error', why: `本仓锚点必须落在**已跟踪**的文件上（未跟踪：${a.path}）` };
  }
  if (a.type === 'guard') {
    if (typeof a.test !== 'string' || a.test === '') {
      return { ok: false, kind: 'error', why: 'guard 锚缺 test（用例名片段）' };
    }
    const names = testNames(fs.readFileSync(abs, 'utf8'));
    if (names.length === 0) {
      return { ok: false, kind: 'error', why: `该文件里抽不出任何用例名（不是测试文件？）：${a.path}`, file: abs };
    }
    if (!names.some((n) => n.includes(a.test))) {
      return {
        ok: false,
        kind: 'error',
        why: `用例名在该文件里找不到：${a.test}（★ 文件存在 ≠ 用例存在；该文件实测 ${names.length} 个用例）`,
        file: abs,
      };
    }
    return { ok: true, kind: 'ok', why: '用例存在', file: abs };
  }
  // bin：必须算得出 EA → 文件偏移；给了摘要就必须对上
  if (typeof a.ea !== 'number' || !Number.isInteger(a.ea) || a.ea < 0) {
    return { ok: false, kind: 'error', why: `bin 锚的 ea 非法：${JSON.stringify(a.ea)}` };
  }
  const buf = fs.readFileSync(abs);
  const off = peOffsetOf(buf, a.ea);
  if (off === null) {
    return { ok: false, kind: 'error', why: `EA 0x${a.ea.toString(16)} 不在任何 PE 节里（或不是 PE）`, file: abs };
  }
  if (a.len !== undefined && (!Number.isInteger(a.len) || a.len <= 0)) {
    return { ok: false, kind: 'error', why: `bin 锚的 len 必须是正整数：${JSON.stringify(a.len)}` };
  }
  if (a.len !== undefined && off + a.len > buf.length) {
    return { ok: false, kind: 'error', why: `EA 0x${a.ea.toString(16)} + len ${a.len} 越出文件`, file: abs };
  }
  if (a.sha256 !== undefined) {
    const got = crypto.createHash('sha256').update(buf.subarray(off, off + (a.len ?? 0))).digest('hex');
    if (got !== a.sha256) return { ok: false, kind: 'error', why: `字节摘要不匹配（EA ${a.ea}：期望 ${a.sha256}，实际 ${got}）`, file: abs };
  }
  return { ok: true, kind: 'ok', why: `EA ${a.ea} → 偏移 0x${off.toString(16)}`, file: abs };
}

// ─────────────────────────────────────────────────────────── 分类轴（域词汇表）

/**
 * **域词汇表** —— 由 `kind=domain` 的记录**投影**出来（不另造第二个真源）。
 *
 * 为什么词表要 append-only（而不是像需求单那样就地编辑）：词表的读者有两个 ——
 * 人（读当前态）与**校验器**（要判断**已经写下的历史行**合不合法）。就地编辑会让历史行的值**悬空**：
 * 一改名就让全部历史记录变红，或只能放宽校验（等于没校验）。需求单可以就地编辑，是因为**没人需要重放它的历史**。
 *
 * @returns {{canonical:Set<string>, aliasOf:Map<string,string>, splits:Map<string,string[]>, records:Array, problems:Array}}
 *   * `canonical`：当前**有效**的域名（`disposition != 'split'`；被 split 取代的旧域不再是当前值）
 *   * `aliasOf`：历史值 → 它归属的域名（改名/归并产生的别名，**可传递**）
 *   * `splits`：历史值 → 它被拆成的域名列表（★ **别名不覆盖它**：必须走数据迁移）
 */
export function buildVocabulary(records) {
  const problems = [];
  const domains = records.filter((r) => r.kind === 'domain');
  const canonical = new Set();
  const aliasOf = new Map();
  const splits = new Map();
  const seenKey = new Map();

  // 当前值 = 词表里 domain 记录的 subject（split 的旧域不算"当前值"，它已被取代）
  for (const r of domains) {
    if (r.disposition === 'split') {
      splits.set(r.subject, [...(r.splitInto ?? [])]);
      continue;
    }
    if (seenKey.has(r.subject)) {
      problems.push({ id: r.id, reason: `域名 ${r.subject} 在词表里出现 ${seenKey.get(r.subject)} 与 ${r.id} 两次（词表不许有歧义）` });
      continue;
    }
    seenKey.set(r.subject, r.id);
    canonical.add(r.subject);
    for (const a of r.aliases ?? []) {
      if (aliasOf.has(a) && aliasOf.get(a) !== r.subject) {
        problems.push({ id: r.id, reason: `别名 ${a} 同时归属 ${aliasOf.get(a)} 与 ${r.subject}（一个历史值只能有一个去处）` });
        continue;
      }
      aliasOf.set(a, r.subject);
    }
  }
  // 当前值也是自己的别名（查表统一）
  for (const k of canonical) if (!aliasOf.has(k)) aliasOf.set(k, k);
  return { canonical, aliasOf, splits, records: domains, problems };
}

/**
 * 把一个 `system` 值解析到**当前词表**里的域名。
 * @returns {{value:string, canonical:string|null, via:'canonical'|'alias'|'split'|'unknown'|'absent', chain:string[], splitInto?:string[]}}
 *   * `absent` —— **根本没写 `system`** ⇒ 进"待定域"名单（允许，但**看得见**；不是错误）
 *   * `unknown` —— 词表里既不是当前值也不是别名 ⇒ **待裁决**（不变量 #7 会点名，但**不许静默过**）
 *   * `split` —— 它被拆过 ⇒ **别名不覆盖**，必须走"追加更正记录"的数据迁移；解析**到此为止**（不许猜）
 */
export function resolveDomain(value, vocab) {
  if (value === undefined || value === null || value === '') {
    return { value: '', canonical: null, via: 'absent', chain: [] };
  }
  const chain = [value];
  if (vocab.splits.has(value)) {
    return { value, canonical: null, via: 'split', chain, splitInto: vocab.splits.get(value) };
  }
  if (vocab.canonical.has(value)) return { value, canonical: value, via: 'canonical', chain };
  let cur = value;
  for (let i = 0; i < vocab.aliasOf.size + 1; i += 1) {
    const next = vocab.aliasOf.get(cur);
    if (next === undefined || next === cur) break;
    chain.push(next);
    if (vocab.splits.has(next)) return { value, canonical: null, via: 'split', chain, splitInto: vocab.splits.get(next) };
    cur = next;
    if (vocab.canonical.has(cur)) return { value, canonical: cur, via: 'alias', chain };
  }
  return { value, canonical: null, via: 'unknown', chain };
}

// ─────────────────────────────────────────────────────────── 投影（有效状态 / 冲突）

/**
 * 由日志**现算**当前形态 —— 日志是事实，这里是视图。
 * @returns {{entries:Array, conflicts:Array, byEffective:object}}
 *   entry = 记录 + `anchors[]`（逐条解析结果）+ `effective` + `conflictWith[]`
 */
export function project(records, opts = {}) {
  const vocab = buildVocabulary(records);
  // ★ "撤回"有两种表达：自己 `status: retracted`，或**被别条 `replaces` 指向**。
  //   ⚠ 实测踩过：这两个集合原先只在**冲突分组**里被用到（把被撤回的排除出去），
  //   于是"被 replaces 指向"的那条 `effective` **仍然是 accepted/proposed** ——
  //   代码注释写着"它不再算数"，但投影里根本没体现。现在它一并在**状态投影**里生效。
  const selfRetracted = new Set(records.filter((r) => r.status === 'retracted').map((r) => r.id));
  const replacedIds = new Set(records.filter((r) => typeof r.replaces === 'string').map((r) => r.replaces));
  const entries = records.map((r) => {
    const anchors = (r.anchor ?? []).map((a) => ({ anchor: a, ...resolveAnchor(a, opts) }));
    const hardFail = anchors.filter((x) => x.kind === 'error');
    let effective = r.status ?? 'proposed';
    if (replacedIds.has(r.id) || selfRetracted.has(r.id)) {
      // ★ 被更正记录取代（或自己声明撤回）⇒ 不再算数；**但不删除**（历史留在日志里）
      effective = 'retracted';
    } else {
      if (effective === 'accepted' && hardFail.length > 0) effective = 'stale';
      // 锚点一条都没有可用的（含"参考仓不在场"）⇒ 不许再算 accepted
      if (effective === 'accepted' && anchors.every((x) => !x.ok)) effective = 'stale';
    }
    // ★ 分类轴：值追不到当前词表 ⇒ 这条记录**待裁决**（不是 stale —— stale 说的是"观察失效"）
    const systemValue = typeof r.system === 'string' ? r.system : '';
    const system = resolveDomain(systemValue, vocab);
    return { ...r, anchors, system, effective, conflictWith: [] };
  });

  // 冲突：同 kind+subject 上**多个不同 claim** ⇒ 显式化
  //   ★ 被撤回/被取代的那条**不该再跟别人冲突**
  const retractedIds = new Set(entries.filter((e) => e.effective === 'retracted').map((e) => e.id));
  const groups = new Map();
  for (const e of entries) {
    if (retractedIds.has(e.id)) continue;
    // ★ 域记录不进冲突判定：同一个域可以有多条处置（added → renamed → merged…），
    //   那不是"两种说法打架"，而是**同一条时间线上的叠加**（由 aliases / replaces 表达）。
    if (e.kind === 'domain') continue;
    const key = `${e.kind}\u0000${e.subject}`;
    if (!groups.has(key)) groups.set(key, []);
    groups.get(key).push(e);
  }
  const conflicts = [];
  for (const [key, list] of groups) {
    const claims = new Map();
    for (const e of list) {
      const c = String(e.claim ?? '');
      if (!claims.has(c)) claims.set(c, []);
      claims.get(c).push(e);
    }
    if (claims.size < 2) continue;
    const [kind, subject] = key.split('\u0000');
    const ids = list.map((e) => e.id);
    for (const e of list) e.conflictWith = ids.filter((x) => x !== e.id);
    for (const e of list) if (e.effective === 'accepted' || e.effective === 'proposed') e.effective = 'conflict';
    conflicts.push({ kind, subject, claims: [...claims].map(([claim, es]) => ({ claim, ids: es.map((x) => x.id) })) });
  }

  const byEffective = {};
  for (const s of EFFECTIVE) byEffective[s] = 0;
  for (const e of entries) byEffective[e.effective] = (byEffective[e.effective] ?? 0) + 1;

  // 分类轴：按当前域名统计；`待定域`（没写 system）/ `split` / `unknown` 各自单列
  const bySystem = {};
  let pendingDomain = 0;
  for (const e of entries) {
    const via = e.system.via;
    if (via === 'absent') {
      pendingDomain += 1;
      bySystem['(待定域：尚未填 system)'] = (bySystem['(待定域：尚未填 system)'] ?? 0) + 1;
      continue;
    }
    const k = via === 'canonical' || via === 'alias' ? e.system.canonical
      : via === 'split' ? '(已被拆分：待数据迁移)'
        : '(未在词表中：待裁决)';
    bySystem[k] = (bySystem[k] ?? 0) + 1;
  }
  return { entries, conflicts, byEffective, bySystem, pendingDomain, vocab };
}

// ─────────────────────────────────────────────────────────── 不变量

const bad = (file, line, reason) => ({ file, line, reason });

/**
 * 全部不变量。返回 `{checks:[{id,text,problems[]}], failures, project}` —— 形状与 `requirements` 一致。
 * ★ 只读参考仓不在场 ⇒ **warning 不算失败**（口径写在上面的锚点注释里）。
 */
export function validateAll(records, opts = {}) {
  const { repoRoot, referenceRoot, ledgerDir, tracked, parsedProblems = [], recheckStatus = true } = opts;
  const checks = [];
  const add = (id, problems) => checks.push({ id, text: CHECK_TITLES.get(id), problems });
  const proj = project(records, { repoRoot, referenceRoot, tracked });
  /** 形态非法（含错 kind）的记录：先报根因，别让"id 重复 / 追加序"之类的次级症状淹没它 */
  const malformed = new Set();

  // 1 形态
  {
    const p = parsedProblems.map((x) => bad(x.file, x.line, x.reason));
    const key = (r) => `${path.resolve(r._file)}:${r._line}`;
    const seen = new Map();
    for (const r of records) {
      const at = `${path.basename(r._file)}:${r._line}`;
      const mark = () => malformed.add(key(r));
      // 身份与时刻先判：它们是**分片与追加序的基础**，错了后面几条报的都是次级症状
      if (typeof r.id !== 'string' || !ULID_RE.test(r.id)) {
        p.push(bad(r._file, r._line, `${at}: id 不是 26 字符 ULID`));
        mark();
      }
      if (typeof r.at !== 'string' || !ISO_RE.test(r.at)) {
        p.push(bad(r._file, r._line, `${at}: at 必须是 ISO-8601 毫秒 UTC（例 2026-10-04T09:00:00.000Z）`));
        mark();
      }
      if (r.kind !== undefined && r._fileKind !== undefined && r.kind !== r._fileKind) {
        p.push(bad(r._file, r._line, `${at}: kind=${r.kind} 与所在目录 ${r._fileKind}/ 不一致`));
        mark();
      }
      if (!KINDS.includes(r.kind)) {
        p.push(bad(r._file, r._line, `${at}: kind 非法 ${JSON.stringify(r.kind)}（应为 ${KINDS.join('/')}）`));
        mark();
      }
      if (typeof r.subject !== 'string' || r.subject.trim() === '' || /\s/.test(r.subject)) {
        p.push(bad(r._file, r._line, `${at}: subject 必须是非空、**无空白**的稳定键（例 Engine+0x5D880；别用 name —— 旧仓实测 9 组同名跨 scope）`));
      }
      // ★ 分类轴：`system` 可缺省（渐进填域），但**写了就必须合规**。
      //   缺省 = 进"待定域"名单（#7 单列，**不报红**）：内容可以先落，域后补。
      //   这与"锚可以指向只读参考仓"同源 —— **缺失 ≠ 失效，但要看得见**。
      if (r.system !== undefined && !isDomainToken(r.system)) {
        p.push(bad(r._file, r._line, `${at}: system 要么不写（⇒ 进"待定域"名单），要么是非空、**无空白**的域名（词表见 \`pnpm tools ledger domains\`）`));
      }
      // ★ 域专属字段只许出现在 `kind=domain` 上（同"缺陷专属字段"的纪律）
      for (const k of DOMAIN_ONLY) {
        if (r[k] !== undefined && r.kind !== 'domain') {
          p.push(bad(r._file, r._line, `${at}: ${k} 是**域记录专属**字段（当前 kind=${r.kind}）`));
        }
      }
      if (r.kind === 'domain') {
        if (!DISPOSITIONS.includes(r.disposition)) {
          p.push(bad(r._file, r._line, `${at}: 域记录必须写 disposition，且取值为 ${DISPOSITIONS.join('/')}（实际 ${JSON.stringify(r.disposition)}）`));
        }
        if (r.disposition === 'split') {
          if (!Array.isArray(r.splitInto) || r.splitInto.length < 2) {
            p.push(bad(r._file, r._line, `${at}: disposition=split 必须写 splitInto，且至少 2 项（拆成多个才有意义）`));
          }
        } else if (r.splitInto !== undefined) {
          p.push(bad(r._file, r._line, `${at}: 只有 disposition=split 才写 splitInto`));
        }
        if (r.aliases !== undefined) {
          if (!Array.isArray(r.aliases) || r.aliases.some((a) => typeof a !== 'string' || a.trim() === '' || /\s/.test(a))) {
            p.push(bad(r._file, r._line, `${at}: aliases 必须是非空字符串数组，且每项**无空白**`));
          }
        }
        if (r.disposition === 'renamed' || r.disposition === 'merged') {
          if (!Array.isArray(r.aliases) || r.aliases.length === 0) {
            p.push(bad(r._file, r._line, `${at}: disposition=${r.disposition} 必须写 aliases —— **它就是"历史行里的旧值仍可解析"的唯一依据**`));
          }
        }
      }
      if (typeof r.claim !== 'string' || r.claim.trim() === '') p.push(bad(r._file, r._line, `${at}: claim 必须是非空字符串`));
      if (r.status !== undefined && !STATUSES.includes(r.status)) {
        p.push(bad(r._file, r._line, `${at}: status 非法 ${JSON.stringify(r.status)}（应为 ${STATUSES.join('/')}）`));
      }
      if (!Array.isArray(r.anchor) || r.anchor.length === 0) {
        p.push(bad(r._file, r._line, `${at}: anchor 必须非空 —— **每条结论至少绑一条可再校验的观察**`));
      }
      if (r.id !== undefined) {
        const key = r.id;
        if (seen.has(key)) p.push(bad(r._file, r._line, `${at}: id 与 ${seen.get(key)} 重复`));
        else seen.set(key, at);
      }
    }
    add(1, p);
  }

  // 2 追加序（只许追加：文件内 ULID 严格递增；且 at 的月份 == 文件名月份）
  //   ★ 形态非法的记录不在这里重复报（#1 已经点名根因了）
  {
    const p = [];
    const kindOf = new Map();
    for (const { kind, file } of listFiles(ledgerDir ?? '')) kindOf.set(path.resolve(file), kind);
    for (const { month, file } of listFiles(ledgerDir ?? '')) {
      const rows = records.filter((r) => path.resolve(r._file) === path.resolve(file) && !malformed.has(`${path.resolve(r._file)}:${r._line}`));
      let prev = null;
      for (const r of rows) {
        if (typeof r.id === 'string' && ULID_RE.test(r.id)) {
          if (prev !== null && r.id <= prev) {
            p.push(bad(file, r._line, `追加序破坏：id ${r.id} 不大于上一行的 ${prev}（append-only 只许往后加，不许插中间/改历史）`));
          }
          prev = r.id;
        }
        if (typeof r.at === 'string' && ISO_RE.test(r.at) && typeof r.id === 'string' && ULID_RE.test(r.id)) {
          const atMonth = r.at.slice(0, 7);
          const idMonth = monthOf(r.id);
          if (atMonth !== idMonth) p.push(bad(file, r._line, `at (${r.at}) 的月份 ${atMonth} 与 ULID 推出的月份 ${idMonth} 不一致（⇒ 落错文件）`));
        }
      }
      // 文件名月份与内容自洽
      for (const r of rows) {
        if (typeof r.id === 'string' && ULID_RE.test(r.id) && monthOf(r.id) !== month) {
          p.push(bad(file, r._line, `记录落在 ${month}.jsonl，但它属于 ${monthOf(r.id)}（分片规则：按 kind + 月）`));
        }
      }
    }
    add(2, p);
  }

  /**
   * 3 锚点形态 / 不越界 / self 必须已跟踪
   * ★ **只判"在场"的记录**（`effective !== 'retracted'`）—— 与 #4 同一口径：
   *   被 `replaces` 取代的历史行**不再算数**，它的锚可能指向一条后来被改名/删掉的用例，
   *   那是**历史事实**而不是当下的错误。否则"更正一条记录"会连带要求"顺带修好历史行的死锚"，
   *   而那正好会逼人**就地改历史**（本仓禁）。
   *   ⚠ 实测踩过：批 R1 改名了几条守卫用例名 ⇒ 3 条已被取代的旧记录留下死锚 ⇒ #3 红，
   *   而它们的"取代者"完全健康。放宽到"只看在场"之后，#3 仍然能抓到**真**错误
   *   （有人新写一条记录的锚指向不存在的文件/非法 path/悬空用例）。
   */
  {
    const p = [];
    for (const e of proj.entries) {
      if (e.effective === 'retracted') continue;
      const at = `${path.basename(e._file)}:${e._line}`;
      for (const { anchor: a, kind, why } of e.anchors) {
        if (!ANCHOR_TYPES.includes(a.type)) { p.push(bad(e._file, e._line, `${at}: 锚点 type 非法 ${JSON.stringify(a.type)}`)); continue; }
        if (!REPOS.includes(a.repo)) { p.push(bad(e._file, e._line, `${at}: 锚点 repo 非法 ${JSON.stringify(a.repo)}（应为 ${REPOS.join('/')}）`)); continue; }
        if (typeof a.path !== 'string' || a.path === '' || path.isAbsolute(a.path) || a.path.includes('..')) {
          p.push(bad(e._file, e._line, `${at}: 锚点 path 必须是仓库内**相对**路径（不许绝对 / 不许含 ..）：${JSON.stringify(a.path)}`));
          continue;
        }
        if (a.type === 'guard' && (typeof a.test !== 'string' || a.test.trim() === '')) {
          p.push(bad(e._file, e._line, `${at}: guard 锚的 test（用例名片段）必须非空 —— 文件存在 ≠ 用例存在`));
          continue;
        }
        if (a.type === 'bin' && (typeof a.ea !== 'number' || !Number.isInteger(a.ea) || a.ea < 0)) {
          p.push(bad(e._file, e._line, `${at}: bin 锚的 ea 必须是非负整数（EA，不是文件偏移）`));
          continue;
        }
        if (a.type === 'bin' && a.sha256 !== undefined && !/^[0-9a-f]{64}$/.test(a.sha256)) {
          p.push(bad(e._file, e._line, `${at}: bin 锚的 sha256 必须是 64 位小写 hex`));
          continue;
        }
        if (kind === 'error') p.push(bad(e._file, e._line, `${at}: ${anchorText(a)} —— ${why}`));
      }
    }
    add(3, p);
  }

  // 4 观察可再校验（**声称** accepted 的记录，其 self 锚必须当场解析得到；reference 不在场只 warn）
  //   ★ 只对 `status === 'accepted'` 判 —— 投影把解析不了的降级成 stale 是**视图**，
  //     而"日志里声称了 accepted 却没证据"本身要一直红着（否则降级就变成了洗白）。
  {
    const p = [];
    for (const e of proj.entries) {
      if (e.status !== 'accepted' || !recheckStatus) continue;
      // ★ 已被 `replaces` 取代（或被自己撤回）的历史行**不再算数**：它的锚可能指向后来改名的用例，
      //   那是历史事实。要求"顺带修好历史行的死锚"会逼人**就地改历史**（本仓禁）。
      //   ⇒ 与 #3 同一口径：只判**在场**的记录。
      if (e.effective === 'retracted') continue;
      const at = `${path.basename(e._file)}:${e._line}`;
      const selfs = e.anchors.filter((x) => x.anchor.repo === 'self');
      if (selfs.length === 0) {
        p.push(bad(e._file, e._line, `${at}: status=accepted 但**没有任何 self 锚** —— 全靠只读参考仓的观察还不算"可再校验"（先 proposed，等 K2 重挂 EA）`));
        continue;
      }
      for (const x of selfs) {
        if (!x.ok) p.push(bad(e._file, e._line, `${at}: accepted，但 self 锚解析不了 → ${anchorText(x.anchor)}（${x.why}）⇒ 投影已自动降级 stale`));
      }
    }
    add(4, p);
  }

  // 5 引线（replaces 存在且不成环）与冲突显式化
  {
    const p = [];
    const byId = new Map(proj.entries.map((e) => [e.id, e]));
    for (const e of proj.entries) {
      if (e.replaces === undefined) continue;
      if (!byId.has(e.replaces)) p.push(bad(e._file, e._line, `replaces 指向不存在的记录：${e.replaces}`));
      else if (e.replaces === e.id) p.push(bad(e._file, e._line, `replaces 指向自己`));
    }
    // 环：沿 replaces 走，最多走 N 步
    for (const e of proj.entries) {
      let cur = e;
      const seen = new Set([e.id]);
      for (let i = 0; i < proj.entries.length + 1 && cur?.replaces !== undefined; i += 1) {
        if (seen.has(cur.replaces)) { p.push(bad(e._file, e._line, `replaces 成环：${[...seen].join(' → ')} → ${cur.replaces}`)); break; }
        seen.add(cur.replaces);
        cur = byId.get(cur.replaces);
        if (!cur) break;
      }
    }
    for (const c of proj.conflicts) {
      p.push(
        bad(
          ledgerDir ?? '',
          0,
          `**显式冲突**：${c.kind} / ${c.subject} 上有 ${c.claims.length} 种不同 claim ⇒ ${c.claims
            .map((x) => `${x.ids.join('+')}：「${x.claim.slice(0, 40)}${x.claim.length > 40 ? '…' : ''}」`)
            .join(' vs ')}`,
        ),
      );
    }
    add(5, p);
  }

  // 6 派生 DB（由 rebuildDb 的调用方把结果塞进 opts.dbProblems）
  {
    add(6, (opts.dbProblems ?? []).map((x) => bad(opts.dbProblemsFile ?? '', x.line ?? 0, x.reason)));
  }

  // 7 分类轴（域词汇表）
  {
    const p = [];
    // 7a 词表自身的歧义（同名两个来源、一个别名两个去处）
    for (const x of proj.vocab.problems) p.push(bad(ledgerDir ?? '', 0, `词表歧义：${x.reason}`));
    // 7b 每条记录的 system 值与词表的关系
    //   ★ 已经被**更正记录**取代的行（别的记录 `replaces` 指向它）不再按当前词表判它的值：
    //     它的值与它的 claim 都已被新记录接管 —— 这正是"拆分/重定义走追加更正"那条路的收尾。
    const replacedIds = new Set(proj.entries.filter((e) => typeof e.replaces === 'string').map((e) => e.replaces));
    for (const e of proj.entries) {
      const sv = e.system;
      const at = `${path.basename(e._file)}:${e._line}`;
      if (replacedIds.has(e.id)) continue;
      if (sv.via === 'absent') continue; // ★ 没填域 = 待定，不是错（`report` 会单列计数）
      if (sv.via === 'unknown') {
        p.push(
          bad(
            e._file,
            e._line,
            `${at}: system=${JSON.stringify(sv.value)} **不在域词汇表里，也不是任何已登记历史值的别名** ⇒ 待裁决：` +
              `要么补一条 \`kind=domain\` 的处置记录（added/renamed/merged），要么改这条记录的值。` +
              `★ 不静默放过 —— 否则 K1/K3 写进去的值会静默长出多套写法（旧仓实测：capability 用中文粗标签、field 用英文细标识，两套无法 join）`,
          ),
        );
      } else if (sv.via === 'split') {
        p.push(
          bad(
            e._file,
            e._line,
            `${at}: system=${JSON.stringify(sv.value)} **已被拆分**（→ ${(sv.splitInto ?? []).join(' / ')}）⇒ ` +
              `别名救不了这一类：必须按台账纪律**追加一条更正记录**（\`replaces\` 指向本条）把值落到具体的新域。` +
              `★ 不许靠词表悄悄改结论的含义`,
          ),
        );
      } else if (sv.via === 'alias') {
        // 别名可解析 ⇒ **合法**（历史行一个字节都不用动）。这里只提示别名链，不报错。
      }
    }
    // 7c 域记录：subject 就是域名，不许含空白；splitInto 的目标必须是当前值（不能指向一个不存在的域）
    for (const e of proj.entries) {
      if (e.kind !== 'domain' || e.disposition !== 'split') continue;
      for (const t of e.splitInto ?? []) {
        if (!proj.vocab.canonical.has(t)) {
          p.push(bad(e._file, e._line, `splitInto 指向 ${JSON.stringify(t)}，但它不是当前词表里的域 ⇒ 拆分目标必须是已登记的域`));
        }
      }
    }
    add(7, p);
  }

  const failures = checks.filter((c) => c.problems.length > 0).length;
  return { checks, failures, project: proj };
}

// ─────────────────────────────────────────────────────────── 派生 SQLite

/**
 * 由文本真源**确定性重建**派生 DB。
 *
 * ★ "确定性"的可检查形态：`logicalDigest()` ——
 *   建库两次、把**逻辑内容**（建表语句 + 按 id 排序的行）序列化成文本再算 sha256，必须相同。
 *   为什么不比 DB 文件字节：SQLite 文件头里带变更计数器、页里可能有空闲区 ⇒
 *   那是"实现细节"不是"内容"；而**逻辑内容**相同才是我们要的那条不变量
 *   （口径同 `decisions.md` §2 第 3 条：删掉 DB 后一条命令能重建 ⇒ 判据比的是能重建出什么）。
 *
 * @returns {{ok:boolean, reason?:string, dbPath:string, records:number, anchors:number, conflicts:number}}
 */
export function rebuildDb(records, dbPath, opts = {}) {
  let sqlite;
  try {
    sqlite = require$sqlite();
  } catch (err) {
    return { ok: false, reason: `本机 Node 没有可用的 node:sqlite：${err.message}`, dbPath, records: 0, anchors: 0, conflicts: 0 };
  }
  const proj = project(records, opts);
  fs.mkdirSync(path.dirname(dbPath), { recursive: true });
  if (fs.existsSync(dbPath)) fs.rmSync(dbPath);
  const db = new sqlite.DatabaseSync(dbPath);
  try {
    // 固定的物理参数（换机器/换次运行都不许变）
    db.exec('PRAGMA page_size = 4096');
    db.exec('PRAGMA journal_mode = MEMORY');
    db.exec('PRAGMA synchronous = OFF');
    db.exec(`
      CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);
      CREATE TABLE record (
        id TEXT PRIMARY KEY, at TEXT NOT NULL, kind TEXT NOT NULL,
        system TEXT NOT NULL, system_current TEXT, system_via TEXT NOT NULL,
        subject TEXT NOT NULL, claim TEXT NOT NULL, status TEXT NOT NULL, effective TEXT NOT NULL,
        replaces TEXT, note TEXT
      );
      CREATE TABLE anchor (
        record_id TEXT NOT NULL, ord INTEGER NOT NULL, type TEXT NOT NULL, repo TEXT NOT NULL,
        path TEXT NOT NULL, ea INTEGER, len INTEGER, sha256 TEXT, test TEXT,
        resolved TEXT NOT NULL, why TEXT NOT NULL,
        PRIMARY KEY (record_id, ord)
      );
      CREATE TABLE conflict (
        kind TEXT NOT NULL, subject TEXT NOT NULL, claim TEXT NOT NULL, record_id TEXT NOT NULL,
        PRIMARY KEY (kind, subject, claim, record_id)
      );
      -- ★ 域词汇表（由 kind=domain 的记录投影）：别名链与"被拆分"的旧值都在这 —— 查询按域过滤走它
      CREATE TABLE domain (
        record_id TEXT NOT NULL, key TEXT NOT NULL, disposition TEXT NOT NULL,
        alias TEXT, split_into TEXT, claim TEXT NOT NULL,
        PRIMARY KEY (record_id, key, alias)
      );
      CREATE TABLE domain_resolution (
        value TEXT PRIMARY KEY, canonical TEXT, via TEXT NOT NULL, chain TEXT NOT NULL, split_into TEXT
      );
      CREATE INDEX record_subject ON record (kind, subject);
      CREATE INDEX record_system ON record (system, kind);
      CREATE INDEX anchor_path ON anchor (path);
      CREATE INDEX domain_alias ON domain (alias);
    `);
    const insMeta = db.prepare('INSERT INTO meta (key, value) VALUES (?, ?)');
    insMeta.run('schema', '2');
    insMeta.run('records', String(proj.entries.length));
    const insRec = db.prepare(
      'INSERT INTO record (id, at, kind, system, system_current, system_via, subject, claim, status, effective, replaces, note) ' +
        'VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)',
    );
    const insAnchor = db.prepare(
      'INSERT INTO anchor (record_id, ord, type, repo, path, ea, len, sha256, test, resolved, why) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)',
    );
    const insConflict = db.prepare('INSERT INTO conflict (kind, subject, claim, record_id) VALUES (?, ?, ?, ?)');
    const insDomain = db.prepare('INSERT INTO domain (record_id, key, disposition, alias, split_into, claim) VALUES (?, ?, ?, ?, ?, ?)');
    const insResolve = db.prepare('INSERT INTO domain_resolution (value, canonical, via, chain, split_into) VALUES (?, ?, ?, ?, ?)');
    // ★ 一律按 id 排序写入 ⇒ 与文件枚举顺序、与追加历史无关
    const sorted = [...proj.entries].sort((a, b) => String(a.id).localeCompare(String(b.id)));
    for (const e of sorted) {
      insRec.run(
        e.id, e.at, e.kind, e.system.value, e.system.canonical, e.system.via,
        e.subject, e.claim, e.status ?? 'proposed', e.effective,
        e.replaces ?? null, e.note ?? null,
      );
      e.anchors.forEach((x, i) => {
        const a = x.anchor;
        insAnchor.run(
          e.id, i, a.type, a.repo, a.path,
          a.type === 'bin' ? a.ea : null,
          a.type === 'bin' ? a.len ?? null : null,
          a.type === 'bin' ? a.sha256 ?? null : null,
          a.type === 'guard' ? a.test : null,
          x.ok ? 'ok' : x.kind, x.why,
        );
      });
    }
    for (const c of [...proj.conflicts].sort((x, y) => `${x.kind}${x.subject}`.localeCompare(`${y.kind}${y.subject}`))) {
      for (const cl of [...c.claims].sort((x, y) => x.claim.localeCompare(y.claim))) {
        for (const id of [...cl.ids].sort()) insConflict.run(c.kind, c.subject, cl.claim, id);
      }
    }
    // 域词汇表投影：每条 domain 记录按 (key, alias) 展开成行（便于按历史值反查）
    for (const d of [...proj.vocab.records].sort((a, b) => String(a.id).localeCompare(String(b.id)))) {
      const split = d.disposition === 'split' ? (d.splitInto ?? []).join(',') : null;
      const aliases = d.aliases ?? [null];
      for (const a of aliases) insDomain.run(d.id, d.subject, d.disposition, a, split, d.claim);
    }
    // 解析表：每个出现过的值 + 每个已登记的历史值（含别名），都记一条 → 查询/排查都查这一张
    const values = new Set([
      ...proj.entries.map((e) => e.system.value),
      ...proj.vocab.aliasOf.keys(),
      ...proj.vocab.splits.keys(),
    ]);
    for (const v of [...values].sort()) {
      const r = resolveDomain(v, proj.vocab);
      insResolve.run(v, r.canonical, r.via, r.chain.join(' → '), r.splitInto ? r.splitInto.join(',') : null);
    }
    return {
      ok: true,
      dbPath,
      records: proj.entries.length,
      anchors: proj.entries.reduce((n, e) => n + e.anchors.length, 0),
      conflicts: proj.conflicts.length,
      domains: proj.vocab.canonical.size,
      digest: dbDigest(db),
    };
  } finally {
    db.close();
  }
}

/** 逻辑内容摘要：建表 SQL + 排序后的行 —— 比 DB 文件字节更能表达"重建出的是同一个东西" */
export function dbDigest(db) {
  const h = crypto.createHash('sha256');
  const schema = db.prepare("SELECT type, name, sql FROM sqlite_master WHERE name NOT LIKE 'sqlite_%' ORDER BY type, name").all();
  for (const s of schema) h.update(`${s.type}|${s.name}|${s.sql ?? ''}\n`);
  for (const t of ['record', 'anchor', 'conflict', 'domain', 'domain_resolution']) {
    const cols = db.prepare(`PRAGMA table_info(${t})`).all().map((c) => c.name);
    const order = cols.join(', ');
    for (const row of db.prepare(`SELECT ${order} FROM ${t} ORDER BY ${order}`).all()) {
      h.update(`${t}|${cols.map((c) => String(row[c])).join('\u0001')}\n`);
    }
  }
  return h.digest('hex');
}

/** 建到临时目录再摘要一次 —— `rebuild-db` 的"同输入同逻辑内容"判据靠它 */
export function rebuildTwiceDigest(records, tmpDir, opts = {}) {
  const a = rebuildDb(records, path.join(tmpDir, 'a.sqlite'), opts);
  const b = rebuildDb(records, path.join(tmpDir, 'b.sqlite'), opts);
  return { a, b, same: a.ok && b.ok && a.digest === b.digest };
}

let _sqlite;
/**
 * `node:sqlite` 是 Node 内置模块（无第三方依赖 —— 本仓 `dependencies` 保持为 0）。
 * ★ 拿不到时**硬报错并给出可执行的修法**，不静默降级成"没有查询层"。
 */
function require$sqlite() {
  if (_sqlite === undefined) {
    _sqlite = createRequire(import.meta.url)('node:sqlite');
  }
  return _sqlite;
}

// ─────────────────────────────────────────────────────────── 自描述

const CONDITIONAL = {
  status: '⬜ 缺省 = `proposed`（推荐显式写）。**它只是"作者声称"** —— 有效状态由投影现算。',
  replaces: '条件：仅"撤回/纠错"用（**追加**一条新记录并指向被取代的那条）。日志永不就地改。',
  disposition:
    '★ **域记录专属**（`kind=domain`）：added=新域进词表 · renamed=改名（含义没变）· merged=多个旧域并成一个 · ' +
    'split=**一个域被拆成多个**（含义变了 ⇒ 别名不覆盖它，必须走追加更正记录的数据迁移）。',
  aliases:
    '★ 域记录专属。**"历史行里的旧值仍然可解析"的唯一依据**（改名/归并必填）。' +
    '别名是**单向**的：老名字指向新域，不是同义词 —— 查询按当前域名归一。',
  splitInto: '★ 域记录专属，只在 `disposition=split` 上：它被拆成哪几个域（每项必须是当前词表里已登记的域）。',
  note: '⬜ 自由文本（人读的补充）。**不放结论** —— 结论在 `claim`，证据在 `anchor`。',
  tags: '⬜ 自由标签（只当标签，**不参与治理、不当分组真源**；域分类走 `system`）。',
};

export function describe() {
  const fields = [
    ['id', '✅', `${ID_PREFIX}<26 字符 ULID>`, '身份。★ **字典序 == 时间序** ⇒ 追加序直接由它决定（不依赖文件位置）；同文件内必须**严格递增**'],
    ['at', '✅', 'ISO-8601 毫秒 UTC', '时刻。★ 必须与 ULID 推出来的月份一致（分片：按 kind + 月）'],
    ['kind', '✅', KINDS.join(' | '), '★ **台账自己的机制分类**（这条记录是哪一类事件）：claim=一条语义结论 · observation=一条观察 · note=杂项 · **domain=域词汇表的一条处置声明**。必须等于所在目录名。别与 `system` 混'],
    ['system', '⬜', '无空白的域名', '★ **知识本身的分类轴**：这条结论关于引擎的哪一块（音频 / 渲染 / …）。**可缺省**（渐进填域：内容可以先落、域后补），但**写了就必须沿别名链追到当前域词汇表**（追不到 ⇒ #7 点名）。缺省 = 进"待定域"名单（`report` 单列计数，**不报红**）。★ 分类是**字段不是标签** —— 判据见 `docs/00-origin/decisions.md` §8.1'],
    ['subject', '✅', '无空白的稳定键', '这条是关于**什么**的。★ 键必须是 `scope+offset` 这类稳定身份，**绝不能是 name** —— 旧仓 `fields.json` 实测 9 组同名跨 scope、2 组同 `scope+offset` 双 `confirmed`'],
    ['claim', '✅', '非空 string', '断言本身（人读的一句话）'],
    ['anchor', '✅', '非空数组（≤ 无上限）', '★ **每条结论至少绑一条可再校验的观察**；形态见下表'],
    ['status', '⬜', STATUSES.join(' | '), CONDITIONAL.status],
    ['replaces', '⬜', `${ID_PREFIX}…`, CONDITIONAL.replaces],
    ['disposition', 'domain 必填', DISPOSITIONS.join(' | '), CONDITIONAL.disposition],
    ['aliases', '⬜（renamed/merged 必填）', '[历史值…]', CONDITIONAL.aliases],
    ['splitInto', '⬜（split 必填）', '[域名…]', CONDITIONAL.splitInto],
    ['note', '⬜', 'string', CONDITIONAL.note],
    ['tags', '⬜', '[a, b]', CONDITIONAL.tags],
  ];
  const anchorFields = [
    ['type', '✅', ANCHOR_TYPES.join(' | '), 'bin=二进制 EA · guard=可执行守卫用例'],
    ['repo', '✅', REPOS.join(' | '), '★ `self`=本仓 · `reference`=只读参考仓（旧仓）。**这一维不可省**：K2 重挂之前 79 条 B 类的锚全在参考仓'],
    ['path', '✅', '仓库内相对路径', 'bin 锚 = 二进制文件；guard 锚 = 测试文件'],
    ['ea', 'bin 必填', '非负整数', '★ **EA（虚拟地址）不是文件偏移** —— 映射由 `peOffsetOf`（PE 节表）现算，**不落盘成行号**'],
    ['len', '⬜', '正整数', '要摘的字节数（给了 `sha256` 才有意义）'],
    ['sha256', '⬜', '64 位小写 hex', '★ 该 EA 处 `len` 字节的摘要 —— "这条观测没被换掉"的唯一证据'],
    ['test', 'guard 必填', '非空 string', '★ **测试名里的一个片段**（子串匹配）—— 文件存在 ≠ 用例存在'],
  ];
  return {
    file: 'data/ledger/<kind>/<YYYY-MM>.jsonl',
    purpose:
      '「**知道什么**」：可再校验的结论 / 观察。★ 与需求台账 `data/requirements/` 是两件事 —— ' +
      '需求回答"还要做什么"，本目录回答"知道什么"。两者共用派生查询层（`packages/ledger` 的 DB），但各自一份文本真源。',
    notHere: '进度 / 待办（→ `data/requirements/`）；长调查与沿革（→ `docs/` 或 `git log`）；游戏/引擎语义的**未经校验**结论（→ 先过准入门）',
    layout:
      '一个 kind 一个月一个文件，**一行一条记录**（JSON，字段序固定 ⇒ 同输入同字节）。文件路径 `data/ledger/<kind>/<YYYY-MM>.jsonl`。' +
      '★ 目录里**没有** `index` / `PROGRESS` / 任何派生件 —— 派生 DB 落 `.cache/`（gitignore）。',
    fields: fields.map(([name, req, type, desc]) => ({ name, req, type, desc })),
    anchor: { fields: anchorFields.map(([name, req, type, desc]) => ({ name, req, type, desc })), shapes: ANCHOR_TYPES, repos: REPOS },
    effective: {
      note:
        '★ `status` 是**作者声称**，`effective` 是**投影现算** —— 日志是事实，当前形态是视图。',
      values: EFFECTIVE.join(' | '),
      rules: [
        '`accepted` 且某个 `self` 锚**解析不了** ⇒ 自动 `stale`（**不删除、不改写日志**）',
        '`accepted` 但**一个 self 锚都没有**（全靠只读参考仓）⇒ 不许算 accepted（validate #4 会点名）',
        '同一 `kind+subject` 上出现**多个不同 claim** ⇒ 双方都标 `conflict`，并作为产物报出来',
        '`retracted` 由**追加**一条 `replaces` 指向它的记录表达',
      ],
    },
    budget: { note: '台账没有条数预算 —— 它是"知道什么"的仓库，不是"还要做什么"的待办表。' },
    classification: {
      note:
        '★ **知识本身的分类轴 = `system`（必填字段），不是 tag**。读者有两个：人（读当前态）与**校验器**（要判断**已经写下的历史行**）。' +
        '做成自由标签的代价是"哪个写法才对"没有任何东西能判红 ⇒ 多套写法会静默共存（旧仓实测：`capabilities` 用中文粗标签、`fields` 用英文细标识，两套无法 join）。',
      vocabulary:
        '★ **域词汇表 = 台账里 `kind=domain` 的记录**（同一套 append-only 纪律、同一套锚点要求）—— 不另造第二个真源。' +
        '为什么词表要 append-only：就地编辑会让历史行的值**悬空**（一改名就让全部历史记录变红，或只能放宽校验）。' +
        '需求单可以就地编辑，是因为**没人需要重放它的历史**。',
      dispositions: DISPOSITIONS.join(' | '),
      rules: [
        '**改名 / 归并**（含义没变）⇒ 记 `aliases` ⇒ 别名可解析，**历史行一个字节都不用动**',
        '**拆分 / 重定义**（含义变了）⇒ **别名救不了**：必须按台账纪律**追加一条更正记录**（`replaces` 指向旧的）把值落到具体的新域',
        '★ **词表不许用来悄悄改结论的含义** —— 含义变了就是一条新知识，要过准入门、要带锚',
        '**`system` 可缺省**（渐进填域）⇒ 进"待定域"名单，`report` 单列计数，**不报红**：内容可以先落、域后补',
        '`system` 写了但既不是当前域、也不是任何已登记历史值的别名 ⇒ **待裁决**（#7 点名，不静默放过）',
        '域记录本身也**必须带锚**（"域怎么分"是知识，不是配置）—— 由 #3/#4 一起管',
      ],
      query: [
        '`pnpm tools ledger domains` —— 当前域 / 别名链 / 被拆分的值 + 逐值解析表',
        '`pnpm tools ledger list --system <域>` —— 按域列记录（支持历史值：走别名解析）',
        '派生 DB：`record.system` / `record.system_current` 列 + `domain` / `domain_resolution` 两张表（任意 join 走它）',
      ],
    },
    invariants: [...CHECK_TITLES].map(([id, text]) => ({ id, text, enforcedBy: '本工具的 validate（`pnpm tools ledger validate`）' })),
    operations: OPERATIONS,
    determinism: {
      note: '★ "DB 可删可重建"的判据不是"文件字节相同"（SQLite 文件头带变更计数器、页里可能有空闲区），而是**逻辑内容相同**：',
      how: '建两次库 → 比 `dbDigest()`（建表 SQL + 按主键排序的行）⇒ 必须相同。`pnpm tools ledger rebuild-db` 每次都会打印它。',
    },
    writePath:
      '★ **写路径只有一条**：模型里的 `appendRecord`（一行一条、写后回读复验、不绿回滚）。两个调用方共用它：' +
      '`pnpm tools ledger add`（写在 `tools/ledger.mjs`，缺省 dry-run）与测试。**不要手改 `.jsonl`、不要新建/重命名文件、不要删除行** —— ' +
      '日志是 append-only，撤回靠 `retract`（追加一条 `replaces`）。',
  };
}

export function describeText(d = describe()) {
  const L = [];
  L.push(`# ${d.file} —— 自描述（由控制脚本给出，文档不复述 schema）`);
  L.push('');
  L.push(`用途：${d.purpose}`);
  L.push(`★ 这里**不**放：${d.notHere}`);
  L.push('');
  L.push('## 形态');
  L.push(d.layout);
  L.push('');
  L.push('## 字段');
  L.push('| 字段 | 必填 | 取值 | 说明 |');
  L.push('|---|---|---|---|');
  for (const f of d.fields) L.push(`| \`${f.name}\` | ${f.req} | ${f.type} | ${f.desc} |`);
  L.push('');
  L.push('## 锚点');
  L.push('| 字段 | 必填 | 取值 | 说明 |');
  L.push('|---|---|---|---|');
  for (const f of d.anchor.fields) L.push(`| \`${f.name}\` | ${f.req} | ${f.type} | ${f.desc} |`);
  L.push('');
  L.push('## 状态：声称 vs 有效');
  L.push(d.effective.note);
  L.push(`* 取值：${d.effective.values}`);
  for (const r of d.effective.rules) L.push(`* ${r}`);
  L.push('');
  L.push('## 分类轴：`system`（域）—— 为什么是字段而不是标签');
  L.push(d.classification.note);
  L.push(`* ${d.classification.vocabulary}`);
  L.push(`* 处置取值：${d.classification.dispositions}`);
  for (const r of d.classification.rules) L.push(`* ${r}`);
  L.push('* 怎么查：');
  for (const q of d.classification.query) L.push(`  * ${q}`);
  L.push('');
  L.push('## 不变量（含"谁在守它"）');
  for (const i of d.invariants) L.push(`${i.id}. ${i.text}　—　${i.enforcedBy}`);
  L.push('');
  L.push('## 确定性');
  L.push(d.determinism.note);
  L.push(`怎么判：${d.determinism.how}`);
  L.push('');
  L.push('## 操作');
  for (const o of d.operations) L.push(`* \`${o.name}\`${o.mutates ? '（会写）' : ''} —— ${o.summary}　→ \`pnpm tools ledger ${o.name}\``);
  L.push('');
  L.push(`写入口：${d.writePath}`);
  return `${L.join('\n')}\n`;
}
