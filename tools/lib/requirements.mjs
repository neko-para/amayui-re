/**
 * tools/lib/requirements.mjs — **`data/requirements/` 的领域模型**（格式 / 不变量 / 读 / 写 / 自描述）
 *
 * 定位（用户口径）：这是**新仓自己的基建**，与具体业务**不耦合** ——
 * 仓库迁移 / 模拟器开发 / 引擎逆向 / 翻译更新 共用同一套字段与守卫；迁移本身只是其中一个节点。
 * 它回答的是**"还要做什么、到哪一步了"**（人看的、高层次的进度视图），
 * **不是**"知道什么"（那是知识台账 `data/ledger/` 的活）。
 *
 * 分层（`tools/README.md` §0）：本文件**不解析 argv、不打印、不认识 CLI**；CLI 在 `tools/requirements.mjs`。
 *
 * 三条来自调研的硬口径（都有出处，见 data/requirements/README.md）：
 *   · **身份与位置分离**：文件名 / id 一经创建永不改变；引用只认 id，不认路径 / 标题 / 编号。
 *   · **父子只写子侧 `parent`**：移动子节点 = 改 1 行；`children` 一律派生，文件里不许出现
 *     （在父里列数组是最差的写法：改 2 处、且同一数组是多个写者的公共热点）。
 *   · **机器不写人不看的东西**：不记 `history[]`（git log 就是沿革）、不记计数 / 进度（派生）、
 *     不记行号锚点（锚文件名 + 测试名）。
 *
 * 一屏预算（`BUDGET`）是"这套东西还能被人读"的唯一机械保障：写不进预算的内容，
 * 只能**再开一个更低的节点**，或者**滚去知识台账**。
 */
import { randomBytes } from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';

import { DEFAULT_REQUIREMENTS_DIR, REPO_ROOT } from './paths.mjs';

export { DEFAULT_REQUIREMENTS_DIR };

/** 节点文件的后缀；`README.md` 是散文，不是节点 */
const EXT = '.md';
const IGNORE = new Set(['README.md']);

/** id 前缀（**与 type 无关**：type 变了 id 也不变，否则引用会断） */
export const ID_PREFIX = 'REQ-';
const ULID_RE = /^[0-9A-HJKMNP-TV-Z]{26}$/; // Crockford base32（去掉 I/L/O/U）
/** 同一个形态的**字符串版**（给别处拼正则用，别在第二个地方抄一遍） */
export const ULID_RE_SRC = '[0-9A-HJKMNP-TV-Z]{26}';

export const TYPES = ['req', 'bug', 'spike', 'decision'];
export const SEVERITIES = ['S0', 'S1', 'S2', 'S3'];
export const STATUSES = ['open', 'doing', 'blocked', 'done', 'dropped', 'superseded'];
/** 还"活着"、会挡住父节点收口的状态 */
export const LIVE_STATUSES = ['open', 'doing', 'blocked'];
/** 已收口的状态 */
export const CLOSED_STATUSES = ['done', 'dropped', 'superseded'];
/** 缺陷专属（需求没有这两个概念） */
export const BUG_ONLY = ['repro', 'severity'];
/** 只有决策能用的字段 */
const DECISION_ONLY = new Set(['supersedes']);

/** ★ 收口凭据：需求看"能力在不在"（verify），缺陷看"分歧还在不在"（verify 或 repro） */
export const CLOSURE = {
  req: 'status=done ⇒ 必须 `verify: 路径#测试名`（能力存在的守卫）；没有守卫就写 `done_reason`',
  bug: 'status=done ⇒ 必须 `verify` 或 `repro`（分歧消失的证据）；两者都指真实存在的文件，锚点必须找得到',
};

/** ★ 一屏预算（守卫会红；放宽也要有数 —— 没数的预算等于没有） */
export const BUDGET = { maxLinesPerNode: 80, maxNodes: 40 };

/** 参数区的键序（写盘时固定，diff 干净）；不在表里的键 = 未知字段 ⇒ 报错，绝不静默忽略 */
export const KEY_ORDER = [
  'id',
  'type',
  'status',
  'parent',
  'order',
  'blocked_by',
  'verify',
  'repro',
  'severity',
  'done_reason',
  'dropped_reason',
  'supersedes',
  'tags',
];
const LIST_KEYS = new Set(['blocked_by', 'supersedes', 'tags']);
const CONDITIONAL = {
  blocked_by: 'status=blocked 时必须非空',
  verify: '可交付类收口（status=done）时必须写，且 `路径#测试名` 必须真实存在',
  repro: '★ **缺陷专属**：缺陷的"存在证明 + 重开条件"。`路径#测试名`（一个能红的复现/守卫）或 `路径#锚点`（一段可复现步骤）',
  severity: `★ **缺陷专属**：${SEVERITIES.join(' / ')}（其他 type 不写；优先级是"要不要现在做"，严重度是"坏了多厉害 \+ 坏多常"，两者别混）`,
  done_reason: 'status=done 且没有 `verify`/`repro` 时必须写（文档 / 裁决类的例外口）',
  dropped_reason: '★ `status=dropped` 时必须写：缺陷措辞是**"不是缺陷 / 不修"**（不是"做完了"）',
  supersedes: '仅 type=decision（ADR 式：推翻 = 新增一条并指向它）',
};

/** 工具层的自我声明（`tools/cli.mjs` 的域地图从它派生） */
export const DOMAIN = {
  id: 'requirements',
  title: '需求台账（高层次的进度视图：仓库迁移 / 模拟器 / 逆向 / 翻译 …）',
  data: [
    '`data/requirements/*.md`（一个节点一个文件；★ 写路径只有模型的 `planAdd` / `saveNode` —— CLI 与工作台是它的两个调用方）',
    '`data/requirements/README.md`（散文口径，不是节点）',
  ],
  access: 'rw（★ **写路径只有一条**：模型里的 `planAdd` / `saveNode`（写后回读复验、不绿回滚）。CLI 与工作台都只是它的调用方 —— 规则不写在任何一个前端里）',
  tool: 'tools/requirements.mjs',
};

export const OPERATIONS = [
  { name: 'list', argv: ['--list'], mutates: false, summary: '一览：id / type / status / 父 / 标题（+ --json）' },
  { name: 'show', argv: ['--show'], mutates: false, summary: '一个节点：字段 + 子树 + 正文：`<id 或唯一前缀>`' },
  { name: 'plan', argv: ['--plan'], mutates: false, summary: '★ 进度视图（唯一的进度真源）：按树打印 + 聚合状态' },
  { name: 'validate', argv: ['--validate'], mutates: false, summary: '跑全部不变量（红/绿 + 逐条详情）—— 全仓门禁' },
  { name: 'describe', argv: ['--describe'], mutates: false, summary: '自描述：字段 / 不变量 / 预算 / 操作' },
  { name: 'serve', argv: ['--serve'], mutates: false, summary: '起本地网页（需求 + AGE 脚本）：`--port 7788`；`apps/workbench/` 是那个项目（里面的"新建需求单"调的就是 planAdd）' },
  { name: 'add', argv: ['--add'], mutates: true, summary: '加节点：`--title <标题> [--type req] [--parent <id>] [--body <文件>] [--order n] [--tags a,b]` [--write]（与工作台的"新建"同一个 `planAdd`）' },
  { name: 'set', argv: ['--set'], mutates: true, summary: '改节点：`<id> [--status …] [--verify …] [--parent …] [--title …] [--body <文件>] …` [--write]' },
];

// ─────────────────────────────────────────────────────────── ULID

const CROCKFORD = '0123456789ABCDEFGHJKMNPQRSTVWXYZ';
const ENC_TIME = 10;
const ENC_RAND = 16;

/** 生成 ULID：48bit 毫秒时间 + 80bit 随机（Crockford base32，26 字符，字典序 = 创建序） */
export function ulid(now = Date.now(), rand = null) {
  const bytes = rand ?? new Uint8Array(randomBytes(ENC_RAND));
  let t = now;
  const time = new Array(ENC_TIME);
  for (let i = ENC_TIME - 1; i >= 0; i -= 1) {
    time[i] = CROCKFORD[t % 32];
    t = Math.floor(t / 32);
  }
  let bits = 0;
  let acc = 0;
  let out = '';
  for (const b of bytes) {
    acc = (acc << 8) | b;
    bits += 8;
    while (bits >= 5) {
      out += CROCKFORD[(acc >>> (bits - 5)) & 31];
      bits -= 5;
    }
  }
  if (bits > 0) out += CROCKFORD[(acc << (5 - bits)) & 31];
  return (time.join('') + out).slice(0, ENC_TIME + ENC_RAND);
}

export const nodeId = (u) => `${ID_PREFIX}${u}`;

// ─────────────────────────────────────────────────────────── 解析 / 序列化

/**
 * 解析一个节点文件。
 * 形态（散文口径见 data/requirements/README.md）：
 *   `# 标题` / 空行 / `- key: value`… / 空行 / `## 小节` + 散文
 * @returns {{title:string, fields:Record<string,string|string[]>, body:string}}
 */
export function parseNode(text, where = '<text>') {
  const lines = text.replace(/\r\n/g, '\n').split('\n');
  let i = 0;
  if (!lines[i]?.startsWith('# ')) throw new Error(`${where}: 第一行必须是 \`# 标题\``);
  const title = lines[i].slice(2).trim();
  i += 1;
  while (i < lines.length && lines[i].trim() === '') i += 1;

  const fields = {};
  for (; i < lines.length; i += 1) {
    const line = lines[i];
    if (line.trim() === '') break; // 参数区结束
    const m = /^-\s+([A-Za-z_][A-Za-z0-9_]*):\s*(.*)$/.exec(line);
    if (!m) throw new Error(`${where}: 参数行必须形如 \`- key: value\`，实际第 ${i + 1} 行：${JSON.stringify(line)}`);
    const [, key, raw] = m;
    if (key in fields) throw new Error(`${where}: 参数 ${key} 重复`);
    const v = raw.trim();
    if (v === '') throw new Error(`${where}: 参数 ${key} 的值不得为空`);
    if (v.startsWith('[')) {
      if (!v.endsWith(']')) throw new Error(`${where}: 列表参数 ${key} 必须写成 \`[a, b]\``);
      const inner = v.slice(1, -1).trim();
      fields[key] = inner === '' ? [] : inner.split(',').map((s) => s.trim());
    } else {
      fields[key] = v;
    }
  }
  while (i < lines.length && lines[i].trim() === '') i += 1;
  const body = lines.slice(i).join('\n').replace(/\s+$/, '');
  if (body === '') throw new Error(`${where}: 参数区之后必须有正文（至少一个 \`## 小节\`）`);
  return { title, fields, body };
}

/** 规范化序列化（同输入同字节）：标题 / 参数区（固定键序）/ 正文 */
export function canonicalNode(node) {
  const keys = [
    ...KEY_ORDER.filter((k) => k in node.fields),
    ...Object.keys(node.fields).filter((k) => !KEY_ORDER.includes(k)),
  ];
  const L = [`# ${node.title}`, ''];
  for (const k of keys) {
    const v = node.fields[k];
    L.push(`- ${k}: ${Array.isArray(v) ? `[${v.join(', ')}]` : v}`);
  }
  L.push('', node.body, '');
  return L.join('\n');
}

// ─────────────────────────────────────────────────────────── 读 / 写

export function listNodeFiles(dir = DEFAULT_REQUIREMENTS_DIR) {
  if (typeof dir !== 'string' || !fs.existsSync(dir)) return [];
  return fs
    .readdirSync(dir, { withFileTypes: true })
    .filter((d) => d.isFile() && d.name.endsWith(EXT) && !IGNORE.has(d.name))
    .map((d) => path.join(dir, d.name))
    .sort();
}

/** 载入全部节点（文件名即身份；`id` 与文件名是否一致由 `validateAll` 报，不在读取时抛） */
export function loadNodes(dir = DEFAULT_REQUIREMENTS_DIR) {
  return listNodeFiles(dir).map((abs) => {
    const text = fs.readFileSync(abs, 'utf8');
    const parsed = parseNode(text, path.basename(abs));
    return { ...parsed, file: abs, name: path.basename(abs, EXT), lines: text.replace(/\r\n/g, '\n').split('\n').length };
  });
}

/** 落盘 + 回读复验；不绿回滚（新建失败则删掉刚写的文件） */
export function saveNode(node, dir = DEFAULT_REQUIREMENTS_DIR) {
  const abs = path.join(dir, `${node.name}${EXT}`);
  const backup = fs.existsSync(abs) ? fs.readFileSync(abs, 'utf8') : null;
  const tmp = `${abs}.tmp-${process.pid}`;
  const want = canonicalNode(node);
  try {
    fs.mkdirSync(dir, { recursive: true });
    fs.writeFileSync(tmp, want, 'utf8');
    fs.renameSync(tmp, abs);
    const reread = fs.readFileSync(abs, 'utf8');
    // ★ 按**语义**比，不按书写顺序比：键序只是书写形式（落盘时本来就会被拉成 KEY_ORDER），
    //   拿"调用方插入顺序"去比会把合法写入误判成"回读不一致"（踩过一次）。
    const re = parseNode(reread, path.basename(abs));
    const problems = [];
    if (re.title !== node.title) problems.push(`标题回读不一致：${JSON.stringify(re.title)}`);
    if (re.body !== node.body) problems.push('正文回读不一致');
    const keys = new Set([...Object.keys(node.fields), ...Object.keys(re.fields)]);
    const norm = (v) => (Array.isArray(v) ? v.join(',') : String(v));
    for (const k of keys) {
      if (node.fields[k] === undefined) problems.push(`回读多出字段 ${k}`);
      else if (re.fields[k] === undefined) problems.push(`回读丢了字段 ${k}`);
      else if (norm(node.fields[k]) !== norm(re.fields[k])) {
        problems.push(`字段 ${k} 回读不一致：${norm(re.fields[k])} != ${norm(node.fields[k])}`);
      }
    }
    if (problems.length) throw new Error(problems.join('；'));
  } catch (err) {
    try {
      fs.unlinkSync(tmp);
    } catch {
      /* ignore */
    }
    if (backup === null) {
      try {
        fs.unlinkSync(abs);
      } catch {
        /* ignore */
      }
    } else {
      fs.writeFileSync(abs, backup, 'utf8');
    }
    return { ok: false, reason: `写后复验未通过（已回滚）：${err.message}` };
  }
  return { ok: true };
}

export function deleteNode(name, dir = DEFAULT_REQUIREMENTS_DIR) {
  const abs = path.join(dir, `${name}${EXT}`);
  if (!fs.existsSync(abs)) return { ok: false, reason: `没有这个节点文件：${abs}` };
  fs.unlinkSync(abs);
  return { ok: true };
}

/** 建单时正文的缺省骨架（按 type 给不同的问句 —— "该写什么"比"空着"有用） */
const DEFAULT_BODY = {
  bug: '## 复现\n\n（待写：怎么观测到这个分歧，越短越好）\n\n## 期望 / 实际\n\n（待写）\n\n## 影响\n\n（待写）',
  other: '## 判据\n\n（待写：怎么算做完，要可核对）\n\n## 范围 / 非目标\n\n（待写）',
};

/** 建单认得的所有输入键（★ 不认得的键**报错**，不许静默忽略 —— 与 validate 的"无未知字段"同一条纪律） */
const ADD_KEYS = new Set([
  'id',
  'title',
  'type',
  'status',
  'parent',
  'order',
  'tags',
  'blocked_by',
  'verify',
  'repro',
  'severity',
  'done_reason',
  'dropped_reason',
  'supersedes',
  'body',
]);

const asList = (v) =>
  Array.isArray(v)
    ? v.map((s) => String(s).trim()).filter(Boolean)
    : String(v)
        .split(',')
        .map((s) => s.trim())
        .filter(Boolean);

/**
 * ★ **建单的唯一实现**：规则在这里，**调用方只是取参数的人**。
 *
 * 两个调用方用同一份函数（见过的坑：规则写在 CLI 里，网页要建单就只能再抄一遍，两份必然漂）：
 *   · `tools/requirements.mjs --add …`（先打印 plan，`--write` 才落盘）
 *   · `apps/workbench/server.ts` 的 `POST /api/nodes`（工作台里手填一张单子）
 *
 * `spec` 是**值**（不是 argv）：`title` / `type` / `status` / `parent` / `order` / `tags` /
 * `blocked_by` / `verify` / `repro` / `severity` / `done_reason` / `dropped_reason` / `supersedes` / `body`。
 * 列表项既可以给数组，也可以给 `a,b` 逗号串（CLI 就是这么给的）。
 *
 * ★ 这里**只挡"写下去会出事"的**（标题缺失、id 撞车、parent 悬空、不认得的键）；
 *   其余一律交给 `apply()` 里的 `validateAll` —— 它才是不变量的真源，这里不抄第二份裁决。
 *   `apply()` 走的是"落盘 → 全树复验 → 不绿就**把新文件删掉**"那条路（与 CLI 的 `--set` 同一条纪律）。
 *
 * @returns {{name:string, id:string, title:string, fields:object, body:string,
 *            file:string, plan:string[], apply:() => {report:object, rollback?:boolean, removed?:string}}}
 */
export function planAdd(spec = {}, ctx = {}) {
  const dir = ctx.dir ?? DEFAULT_REQUIREMENTS_DIR;
  const nodes = ctx.nodes ?? loadNodes(dir);
  const tree = ctx.tree ?? buildTree(nodes);
  const repoRoot = ctx.repoRoot ?? REPO_ROOT;

  for (const k of Object.keys(spec)) {
    if (!ADD_KEYS.has(k)) throw new Error(`不认得的字段：${k}（要么进 schema，要么删掉 —— 不许静默忽略）`);
  }
  const title = String(spec.title ?? '').trim();
  if (title === '') throw new Error('建单需要标题（title）');
  if (title.startsWith(ID_PREFIX)) throw new Error('title 是标题，不是 id');

  // 身份：缺省由本模型生成；给了 id 就以调用方为准（种子 / 迁移要先知道 id 才能写父子引用）
  let name;
  if (spec.id !== undefined) {
    const bare = String(spec.id).startsWith(ID_PREFIX) ? String(spec.id).slice(ID_PREFIX.length) : String(spec.id);
    if (!ULID_RE.test(bare)) throw new Error(`id 形态非法：${spec.id}（应为 ${ID_PREFIX}<26 字符 Crockford base32>）`);
    // ★ 撞车必须在这里挡住：`saveNode` 会**覆盖**同名文件 —— 那不是"新建"，是"毁掉一个已有节点"
    if (nodes.some((n) => n.name === bare)) throw new Error(`id 已被占用：${nodeId(bare)}`);
    name = bare;
  } else {
    name = ulid();
  }

  const type = spec.type === undefined ? 'req' : String(spec.type);
  const status = spec.status === undefined ? 'open' : String(spec.status);
  const fields = { id: nodeId(name), type, status };

  if (spec.parent !== undefined && spec.parent !== null && spec.parent !== '') {
    const raw = String(spec.parent);
    if (raw === 'null') fields.parent = 'null';
    else {
      const p = tree.resolve(raw);
      if (!p) throw new Error(`parent 指向的节点不存在：${raw}`);
      fields.parent = p.fields.id;
    }
  }
  if (spec.order !== undefined && spec.order !== null && String(spec.order) !== '') fields.order = String(spec.order);
  for (const k of ['tags', 'blocked_by', 'supersedes']) {
    if (spec[k] === undefined) continue;
    const list = asList(spec[k]);
    // ★ 空列表**不写**：`tags: []` 与"没有 tags"是同一件事，写进去只会多一行噪声（CLI 给 `--tags ''` 也会走到这）
    if (list.length > 0) fields[k] = list;
  }
  for (const k of ['verify', 'repro', 'severity', 'done_reason', 'dropped_reason']) {
    if (spec[k] !== undefined && spec[k] !== null && String(spec[k]) !== '') fields[k] = String(spec[k]);
  }

  const rawBody = spec.body === undefined || spec.body === null ? '' : String(spec.body).replace(/\r\n/g, '\n').trim();
  const body = rawBody === '' ? (type === 'bug' ? DEFAULT_BODY.bug : DEFAULT_BODY.other) : rawBody;
  if (!body.includes('## ')) throw new Error('正文至少要有一个 `## 小节`（台账的形态要求）');

  const node = { name, title, fields, body };
  const file = path.join(dir, `${name}${EXT}`);
  const plan = [
    `add ${fields.id}  ${title}`,
    `  parent: ${fields.parent ?? '(缺！)'}  type: ${type}  status: ${status}`,
    `  → ${path.relative(repoRoot, file) || file}`,
  ];

  return {
    name,
    id: fields.id,
    title,
    fields,
    body,
    file,
    plan,
    apply: () => {
      const res = saveNode(node, dir);
      if (!res.ok) throw new Error(res.reason);
      const report = validateAll(loadNodes(dir), { repoRoot });
      if (report.failures > 0) {
        // ★ 写后守卫没过 ⇒ 必须把这颗新节点**整颗删掉**（否则树上留一个守卫不接受的文件）
        deleteNode(name, dir);
        return { rollback: true, report, removed: nodeId(name) };
      }
      return { report };
    },
  };
}

// ─────────────────────────────────────────────────────────── 树 / 派生

/**
 * 造一个"引用 → 节点"的解析器。认三种写法：
 *   ① 完整 id / 文件名（`REQ-01ABC…` 或裸 ULID）；
 *   ② **唯一前缀**；③ **唯一后缀** —— ★ 后缀是给人用的短别名：
 *   ULID 以时间开头，同一批节点的**前缀全都一样**，只有尾部能区分（实测：13 个种子的前 10 位相同）。
 * ★ 它**只依赖 nodes**（不依赖任何派生树）：否则"刚加了一个节点、又拿旧树去解析"就会出错。
 */
export function makeResolver(nodes) {
  const byKey = new Map();
  for (const n of nodes) {
    byKey.set(n.name, n);
    if (typeof n.fields.id === 'string') byKey.set(n.fields.id, n);
  }
  const unique = (list) => (list.length === 1 ? list[0] : null);
  return (ref) => {
    if (typeof ref !== 'string' || ref === '' || ref === 'null') return null;
    if (byKey.has(ref)) return byKey.get(ref);
    const bare = ref.startsWith(ID_PREFIX) ? ref.slice(ID_PREFIX.length) : ref;
    if (byKey.has(bare)) return byKey.get(bare);
    return unique(nodes.filter((n) => n.name.startsWith(bare))) ?? unique(nodes.filter((n) => n.name.endsWith(bare)));
  };
}

/**
 * 由 `parent` 派生出的图（`children` 永远算出来，文件里不存在这个字段）。
 * @returns {{resolve:(ref:string)=>object|null, children:Map<string,object[]>, roots:object[]}}
 */
export function buildTree(nodes) {
  const resolve = makeResolver(nodes);
  const children = new Map();
  const roots = [];
  for (const n of nodes) {
    const p = n.fields.parent;
    if (p === undefined) continue; // 缺失由 validate 报
    if (p === 'null') {
      roots.push(n);
      continue;
    }
    const parent = resolve(p);
    if (!parent) continue; // 悬空由 validate 报
    if (!children.has(parent.name)) children.set(parent.name, []);
    children.get(parent.name).push(n);
  }
  const sortSiblings = (list) =>
    [...list].sort((a, b) => {
      const oa = a.fields.order === undefined ? 'zzzz' : String(a.fields.order).padStart(6, '0');
      const ob = b.fields.order === undefined ? 'zzzz' : String(b.fields.order).padStart(6, '0');
      return oa === ob ? a.name.localeCompare(b.name) : oa.localeCompare(ob);
    });
  for (const [k, v] of children) children.set(k, sortSiblings(v));
  return { resolve, children, roots: sortSiblings(roots) };
}

/** 能从某个根沿 `parent` 走到它吗（成环返回 false）。★ 用 `resolve`（只依赖 nodes），不要用派生树 */
export function reachableFromRoot(node, resolve) {
  const seen = new Set();
  let cur = node;
  for (;;) {
    if (!cur || seen.has(cur.name)) return false; // 悬空 / 成环
    seen.add(cur.name);
    const p = cur.fields.parent;
    if (p === undefined) return false; // 缺 parent：由 #2 的另一条报
    if (p === 'null') return true; // 走到根了
    cur = resolve(p);
  }
}

/** 聚合（派生，永不落盘）：子树里还有多少活节点 */
export function rollup(node, tree) {
  const kids = tree.children.get(node.name) ?? [];
  let live = 0;
  let total = 0;
  for (const k of kids) {
    const r = rollup(k, tree);
    total += 1 + r.total;
    live += (LIVE_STATUSES.includes(k.fields.status) ? 1 : 0) + r.live;
  }
  const status = node.fields.status;
  return { live, total, derivedStatus: status === 'done' && live > 0 ? 'inconsistent' : status };
}

/** 深度优先列表（未连到根的节点也列出来并标 `orphan` —— 看不见的节点永远修不好） */
export function flatten(nodes, tree) {
  const resolve = makeResolver(nodes);
  const out = [];
  const shown = new Set();
  const walk = (n, depth, seen) => {
    if (seen.has(n.name)) return; // 环：validate 报
    seen.add(n.name);
    shown.add(n.name);
    out.push({ node: n, depth, rollup: rollup(n, tree), kids: (tree.children.get(n.name) ?? []).length });
    for (const k of tree.children.get(n.name) ?? []) walk(k, depth + 1, seen);
  };
  for (const r of tree.roots) walk(r, 0, new Set());
  for (const n of nodes) {
    if (!shown.has(n.name)) {
      out.push({ node: n, depth: -1, rollup: rollup(n, tree), kids: 0, orphan: !reachableFromRoot(n, resolve) });
    }
  }
  return out;
}

// ─────────────────────────────────────────────────────────── 视图事实（单一真源）

/**
 * Split a node body into its `## ` sections. The `## ` marker is the ledger's own
 * convention (`tools/requirements.mjs` already relies on it when printing), so the
 * split lives here — with the model — and every consumer (CLI, web page) reuses it
 * instead of re-implementing "what a section is".
 * @returns {{heading:string|null, text:string}[]}
 */
export function splitSections(body) {
  const out = [];
  let cur = { heading: null, lines: [] };
  for (const line of String(body ?? '').split('\n')) {
    if (line.startsWith('## ')) {
      out.push(cur);
      cur = { heading: line.slice(3).trim(), lines: [] };
      continue;
    }
    cur.lines.push(line);
  }
  out.push(cur);
  return out
    .map((s) => ({ heading: s.heading, text: s.lines.join('\n').replace(/^\n+/, '').replace(/\s+$/, '') }))
    .filter((s) => s.heading !== null || s.text !== '');
}

/** Current status marker (same table the CLI prints and a web legend reuses). */
export const STATUS_MARK = {
  open: '⬜',
  doing: '🔜',
  blocked: '⛔',
  done: '✅',
  dropped: '🚫',
  superseded: '♻️',
  inconsistent: '❌',
};

const nodeIdOf = (n) => (typeof n.fields.id === 'string' ? n.fields.id : nodeId(n.name));

/**
 * The parent chain, root first, excluding `node` itself.
 * Walks `parent` through the resolver so it is cycle-safe even on a broken tree
 * (validity is `validateAll`'s business, not this function's).
 */
export function parentChain(node, tree) {
  const chain = [];
  const seen = new Set([node.name]);
  let p = node.fields.parent;
  while (p !== undefined && p !== 'null') {
    const parent = tree.resolve(p);
    if (!parent || seen.has(parent.name)) break;
    seen.add(parent.name);
    chain.unshift(parent);
    p = parent.fields.parent;
  }
  return chain;
}

/**
 * Everything one focused node's page needs, computed once here rather than by each
 * consumer: the node itself, its parent chain, its direct children with the
 * children's own derived windows, and its body split into sections.
 */
export function describeNode(node, tree) {
  const kids = tree.children.get(node.name) ?? [];
  const roll = rollup(node, tree);
  return {
    node,
    id: nodeIdOf(node),
    short: node.name.slice(-8),
    fields: node.fields,
    title: node.title,
    body: node.body,
    lines: node.lines,
    sections: splitSections(node.body),
    rollup: roll,
    done: roll.total - roll.live,
    parentChain: parentChain(node, tree),
    children: kids.map((k) => {
      const r = rollup(k, tree);
      return {
        node: k,
        id: nodeIdOf(k),
        short: k.name.slice(-8),
        title: k.title,
        type: k.fields.type,
        status: k.fields.status,
        mark: STATUS_MARK[k.fields.status] ?? '?',
        rollup: r,
        done: r.total - r.live,
      };
    }),
  };
}

// ─────────────────────────────────────────────────────────── 不变量

/**
 * 全部不变量。每条返回 `{id, text, problems[]}`；`problems` 非空即红。
 * ★ 规则只有这一份真源：`describe()` 用的就是这份标题。
 */
export function validateAll(nodes, opts = {}) {
  const budget = { ...BUDGET, ...(opts.budget ?? {}) };
  const tree = buildTree(nodes);
  const resolve = makeResolver(nodes); // ★ 校验一律用它（不依赖派生树，避免"刚加/刚改"时误报）
  const checks = [];
  const add = (id, text, problems) => checks.push({ id, text, problems });

  // 1 形态与枚举（含 **type 专属字段**）
  {
    const bad = [];
    for (const n of nodes) {
      const at = n.name;
      const f = n.fields;
      if (!ULID_RE.test(n.name)) bad.push(`${at}: 文件名不是 26 字符 ULID`);
      if (f.id === undefined) bad.push(`${at}: 缺 id`);
      else if (f.id !== nodeId(n.name)) bad.push(`${at}: id (${f.id}) 必须等于 ${ID_PREFIX}${n.name}（文件名即身份）`);
      if (!TYPES.includes(f.type)) bad.push(`${at}: type 非法 ${JSON.stringify(f.type)}`);
      if (!STATUSES.includes(f.status)) bad.push(`${at}: status 非法 ${JSON.stringify(f.status)}`);
      if (n.title.trim() === '') bad.push(`${at}: 标题为空`);
      // ★ 缺陷专属字段不许出现在别的 type 上（防止"需求也塞 repro/severity"把两类混起来）
      for (const k of BUG_ONLY) {
        if (f[k] !== undefined && f.type !== 'bug') bad.push(`${at}: ${k} 是**缺陷专属**字段（当前 type=${f.type}）`);
      }
      if (f.type === 'bug') {
        if (f.severity !== undefined && !SEVERITIES.includes(f.severity)) {
          bad.push(`${at}: severity 非法 ${JSON.stringify(f.severity)}（应为 ${SEVERITIES.join('/')}）`);
        }
        // 缺陷的"存在证明 + 重开条件"：开着就必须有，收口了更要能证明分歧消失
        if (f.repro === undefined && f.status !== 'dropped' && f.status !== 'superseded') {
          bad.push(`${at}: type=bug 必须写 repro（存在证明 / 重开条件）—— 缺陷的定义就是"有可观测分歧"`);
        }
      }
      if (f.status === 'superseded' && f.type !== 'decision') {
        bad.push(`${at}: 只有 type=decision 可以 superseded（缺陷"不是缺陷/重复"请用 \`dropped\` + dropped_reason）`);
      }
      for (const k of Object.keys(f)) {
        if (!KEY_ORDER.includes(k)) bad.push(`${at}: 未知字段 ${k}（不许静默忽略：要么进 schema，要么删掉）`);
        if (LIST_KEYS.has(k) && !Array.isArray(f[k])) bad.push(`${at}: ${k} 必须是列表 [a, b]`);
        if (Array.isArray(f[k]) && f[k].some((s) => typeof s !== 'string' || s.trim() === '')) {
          bad.push(`${at}: ${k} 列表里有空项`);
        }
      }
    }
    add(1, '形态与枚举：文件名是 ULID、id == 文件名、type/status 合法、无未知字段、缺陷专属字段只出现在缺陷上', bad);
  }

  // 2 树闭合
  {
    const bad = [];
    for (const n of nodes) {
      const p = n.fields.parent;
      if (n.fields.type === 'decision') {
        if (p !== undefined) bad.push(`${n.name}: type=decision 不参与进度树 ⇒ 不许写 parent`);
        continue;
      }
      if (p === undefined) bad.push(`${n.name}: 缺 parent（根要显式写 parent: null，不许留空）`);
      else if (p !== 'null' && !resolve(p)) bad.push(`${n.name}: parent 悬空 → ${p}`);
      for (const key of ['blocked_by', 'supersedes']) {
        for (const ref of n.fields[key] ?? []) if (!resolve(ref)) bad.push(`${n.name}: ${key} 悬空 → ${ref}`);
      }
    }
    for (const n of nodes) {
      const seen = new Set([n.name]);
      let cur = n;
      for (;;) {
        const p = cur.fields.parent;
        if (p === undefined || p === 'null') break;
        const parent = resolve(p);
        if (!parent) break;
        if (seen.has(parent.name)) {
          bad.push(`${n.name}: parent 链成环（… → ${parent.name} → …）`);
          break;
        }
        seen.add(parent.name);
        cur = parent;
      }
    }
    const rootNames = nodes.filter((n) => n.fields.parent === 'null').map((n) => n.name);
    if (rootNames.length === 0) bad.push('树没有根（需要一个 parent: null 的节点）');
    else if (rootNames.length > 1) bad.push(`树有 ${rootNames.length} 个根，必须恰好一个：${rootNames.join(' , ')}`);
    for (const n of nodes) {
      if (n.fields.type === 'decision') continue;
      const p = n.fields.parent;
      if (p !== undefined && p !== 'null' && !reachableFromRoot(n, resolve)) bad.push(`${n.name}: 从根走不到（悬空或成环）`);
    }
    add(2, '树闭合：恰好一个根、parent 不悬空、不成环、不孤立', bad);
  }

  // 3 状态自洽（收口要有凭据；★ 需求与缺陷的凭据不同）
  {
    const bad = [];
    for (const n of nodes) {
      const f = n.fields;
      const hasProof = f.verify !== undefined || f.repro !== undefined;
      if (f.status === 'done' && !hasProof && f.done_reason === undefined) {
        bad.push(
          f.type === 'bug'
            ? `${n.name}: 缺陷收口必须有凭据 —— \`verify\` 或 \`repro\`（分歧消失的证据），或写 \`done_reason\` 说明为何无法自动化`
            : `${n.name}: status=done 必须给 verify: 路径#测试名（能力存在的守卫）或 done_reason（文档/裁决类）`,
        );
      }
      if (f.status === 'blocked' && (f.blocked_by ?? []).length === 0) bad.push(`${n.name}: status=blocked 必须有 blocked_by`);
      if (f.status === 'dropped' && f.dropped_reason === undefined) {
        bad.push(`${n.name}: status=dropped 必须写 dropped_reason（${f.type === 'bug' ? '缺陷措辞是"不是缺陷 / 不修"' : '说明为什么不做了'}）`);
      }
      if (f.supersedes !== undefined && f.type !== 'decision') bad.push(`${n.name}: 只有 type=decision 可以 supersedes`);
      if (f.done_reason !== undefined && f.status !== 'done') bad.push(`${n.name}: 写了 done_reason 但 status=${f.status}`);
      if (f.dropped_reason !== undefined && f.status !== 'dropped') bad.push(`${n.name}: 写了 dropped_reason 但 status=${f.status}`);
    }
    // ★ 证据（verify / repro）指向的文件与锚点必须真实存在 —— 锚点棘轮，两类共用
    if (opts.repoRoot) {
      for (const n of nodes) {
        for (const key of ['verify', 'repro']) {
          const v = n.fields[key];
          if (typeof v !== 'string') continue;
          const [file, anchor] = v.split('#');
          const abs = path.resolve(opts.repoRoot, file);
          if (!fs.existsSync(abs)) {
            bad.push(`${n.name}: ${key} 指向的文件不存在 → ${file}`);
            continue;
          }
          if (anchor && !fs.readFileSync(abs, 'utf8').includes(anchor)) {
            bad.push(`${n.name}: ${key} 的锚点在该文件里找不到 → ${file}#${anchor}`);
          }
        }
      }
    }
    add(3, '状态自洽：需求 done 看 verify（能力在不在）/ 缺陷 done 看 verify 或 repro（分歧还在不在）；blocked 有前置、dropped 有理由', bad);
  }

  // 4 父不先于子收口
  {
    const bad = [];
    for (const n of nodes) {
      if (n.fields.status !== 'done') continue;
      const r = rollup(n, tree);
      if (r.live > 0) bad.push(`${n.name}: 已 done，但子树里还有 ${r.live} 个未收口节点（父不许先于子收口）`);
    }
    add(4, '父不先于子收口：父 done ⇒ 子树里不许还有 open/doing/blocked', bad);
  }

  // 5 一屏预算
  {
    const bad = [];
    for (const n of nodes) {
      if (n.lines > budget.maxLinesPerNode) {
        bad.push(`${n.name}: ${n.lines} 行 > 预算 ${budget.maxLinesPerNode} 行（细节请开更低层节点，或移去知识台账 / docs）`);
      }
    }
    if (nodes.length > budget.maxNodes) bad.push(`节点总数 ${nodes.length} > 预算 ${budget.maxNodes}（超了要提升抽象，不是加节点）`);
    add(5, `一屏预算：单节点 ≤ ${budget.maxLinesPerNode} 行、总数 ≤ ${budget.maxNodes}`, bad);
  }

  const failures = checks.filter((c) => c.problems.length > 0).length;
  return { checks, failures, tree };
}

// ─────────────────────────────────────────────────────────── 自描述

export function describe() {
  const fields = [
    ['id', '✅', `${ID_PREFIX}<26 字符 ULID>`, '身份。**创建后永不改变**；引用只认它（不认路径 / 标题 / 编号）'],
    [
      'type',
      '✅',
      TYPES.join(' | '),
      'req=要交付（有判据）· bug=**可观测分歧**（上界=分歧消失）· spike=限时设问（产出是结论）· decision=架构决策（**不可变**）',
    ],
    ['status', '✅', STATUSES.join(' | '), `活：${LIVE_STATUSES.join('/')}；收口：${CLOSED_STATUSES.join('/')}`],
    ['parent', '条件', `${ID_PREFIX}… | null`, '父节点。★ **树的唯一真源**；`children` 一律派生、文件里不许出现。根显式写 `null`；`type=decision` 不许写'],
    ['order', '⬜', '整数', '同级**展示顺序**（≠ 身份，可随时重排）'],
    ['blocked_by', '条件', `[${ID_PREFIX}…]`, `前置（DAG）。${CONDITIONAL.blocked_by}`],
    ['verify', '条件', '路径#测试名', CONDITIONAL.verify],
    ['repro', '条件', '路径#测试名 | 路径#锚点', CONDITIONAL.repro],
    ['severity', '条件', SEVERITIES.join(' | '), CONDITIONAL.severity],
    ['done_reason', '条件', 'string', CONDITIONAL.done_reason],
    ['dropped_reason', '条件', 'string', CONDITIONAL.dropped_reason],
    ['supersedes', '条件', `[${ID_PREFIX}…]`, CONDITIONAL.supersedes],
    ['tags', '⬜', '[a, b]', '自由标签（**只当标签**，不当分组真源：分组靠父子关系）'],
  ];
  return {
    file: 'data/requirements/*.md',
    purpose:
      '高层次的进度视图：**还要做什么、到哪一步了**。与业务不耦合（仓库迁移 / 模拟器 / 逆向 / 翻译共用同一套字段）；' +
      '**不是**"知道什么" —— 那属于知识台账 `data/ledger/`',
    notHere:
      '逆向分析结论 / 观察 / 长调查（→ 知识台账或 docs/）；沿革历史（→ git log）；子任务级实现细节（→ 更低层节点，但先想清楚值不值得进这棵树）',
    lifecycle: {
      note: '★ **需求与缺陷的流程不一样**：需求锚"能力在不在"，缺陷锚"可观测分歧还在不在"。两者共用同一棵树与同一套字段，但收口凭据与状态语义不同。',
      req: CLOSURE.req,
      bug: CLOSURE.bug,
      dropped: '需求："不做了"；缺陷：**"不是缺陷 / 不修"**（两者都必须写 dropped_reason —— 不许静默关单）',
      superseded: '只有 decision 用（缺陷的"重复"请用 dropped + 理由指向那条）',
    },
    layout:
      '一个节点一个文件：`data/requirements/<ULID>.md`（文件名 = 身份）。文件头是 `# 标题` + 逐行 `- key: value`，其后是 Markdown 正文（`## 判据` / `## 复现` 等小节）',
    budget: BUDGET,
    fields: fields.map(([name, req, type, desc]) => ({ name, req, type, desc })),
    invariants: [
      { id: 1, text: '形态与枚举：文件名是 ULID、id == 文件名、type/status 合法、无未知字段、缺陷专属字段只出现在缺陷上' },
      { id: 2, text: '树闭合：恰好一个根、parent 不悬空、不成环、不孤立' },
      { id: 3, text: '状态自洽：需求 done 看 verify（能力在不在）/ 缺陷 done 看 verify 或 repro（分歧还在不在）；blocked 有前置、dropped 有理由' },
      { id: 4, text: '父不先于子收口：父 done ⇒ 子树里不许还有 open/doing/blocked' },
      { id: 5, text: `一屏预算：单节点 ≤ ${BUDGET.maxLinesPerNode} 行、总数 ≤ ${BUDGET.maxNodes}` },
    ].map((x) => ({ ...x, enforcedBy: '本工具的 validate（`pnpm tools requirements validate`）' })),
    operations: OPERATIONS,
    writePath:
      '★ **写路径只有一条**：模型里的 `planAdd`（新建）与 `saveNode`（落盘 + 写后回读复验，不绿回滚）。' +
      '两个调用方共用它：`pnpm tools requirements add`（缺省 dry-run，`--write` 才落盘）与工作台的"新建需求单"（`POST /api/nodes`）。' +
      '**不要手改格式、不要重命名或新建 .md**（validate 会红）。',
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
  L.push('## 不变量（validate 的断言）');
  for (const c of d.invariants) L.push(`${c.id}. ${c.text}　—　${c.enforcedBy}`);
  L.push('');
  L.push('## ★ 需求与缺陷的流程差异');
  L.push(`* ${d.lifecycle.note}`);
  L.push(`* **需求**：${d.lifecycle.req}`);
  L.push(`* **缺陷**：${d.lifecycle.bug}`);
  L.push(`* **dropped**：${d.lifecycle.dropped}`);
  L.push(`* **superseded**：${d.lifecycle.superseded}`);
  L.push('');
  L.push('## 预算（★ "人看的"靠它）');
  L.push(`单节点 ≤ ${d.budget.maxLinesPerNode} 行；节点总数 ≤ ${d.budget.maxNodes}`);
  L.push('');
  L.push('## 操作');
  for (const o of d.operations) L.push(`* \`${o.name}\`${o.mutates ? '（会写）' : ''} —— ${o.summary}　→ \`pnpm tools requirements ${o.name}\``);
  L.push('');
  L.push(`写入口：${d.writePath}`);
  return `${L.join('\n')}\n`;
}
