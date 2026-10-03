/**
 * src/api.ts — 端点的取数层。
 *
 * ★ **读**（全部 GET，全部不改数据）与**写**（只有 `POST /api/nodes`：新建一张需求单）都在这里。
 *   写路径的**规则**不在这里 —— 服务端把请求体交给模型 `planAdd()`，与 `pnpm tools requirements add`
 *   是同一个函数；这里只负责"把 JSON 发出去、把错误变成一个能显示的 Error"。
 *
 * 形状的**真源在服务端与 `tools/lib/`**（字段语义见 `pnpm tools requirements describe` /
 * `pnpm tools patch describe`）；这里的 interface 只是给 TS 一份客户端用的类型，不是第二份 schema。
 * 页面**不加工指令行**：`/api/script` 的 `text` 由 `buildView` 产出，这里原样交给 Monaco。
 */

export interface Section {
  heading: string | null;
  text: string;
}

export interface Rollup {
  live: number;
  total: number;
  derivedStatus: string;
}

/** `/api/tree` 的一行（= 服务端 `nodeShape()`） */
export interface TreeRow {
  id: string;
  name: string;
  short: string;
  type: string;
  status: string;
  title: string;
  parent: string | null;
  depth: number;
  orphan: boolean;
  mark: string;
  kids: number;
  rollup: Rollup;
  done: number;
  total: number;
  aggregated: boolean;
  blockedBy: string[];
  tags: string[];
  severity?: string;
  verify?: string;
  repro?: string;
  doneReason?: string;
  droppedReason?: string;
  supersedes: string[];
  lines: number;
  body: string;
}

export interface TreePayload {
  ok: true;
  generatedAt: string;
  dir: string;
  readOnly: true;
  budget: { maxLinesPerNode: number; maxNodes: number };
  marks: Record<string, string>;
  totals: Record<string, number>;
  nodes: TreeRow[];
  decisions: TreeRow[];
  schema: string;
}

export interface DetailNode extends TreeRow {
  sections: Section[];
}

export interface ChainItem {
  id: string;
  short: string;
  title: string;
  type: string;
  status: string;
  mark: string;
  rollup: Rollup;
  done: number;
  total: number;
}

export interface NodePayload {
  ok: true;
  readOnly: true;
  node: DetailNode;
  parentChain: ChainItem[];
  children: ChainItem[];
}

export interface ScriptRow {
  name: string;
  baseFrom: string;
  baseBytes: number;
  /**
   * 旧管线（`annotate-speaker.js`）给它做过**页 / 说话人标注**（`SPEAKER_FILTER`：`SC*` / `SP*`，
   * 含 `$N$` 前缀）。★ 这只是**标签**，**不是** patch 的范围，也与产物根无关。
   */
  annotated: boolean;
  hasPatch: boolean;
  opCount: number;
}

export interface ScriptsPayload {
  ok: true;
  readOnly: true;
  patch: string;
  patchBytes: number;
  /** 名单的唯一来源：**基线根里所有能反汇编的 `.BIN`**（不是 patch 的键，也不是标注口径的子集） */
  source: 'baseline';
  count: number;
  /** 其中被旧管线**标注过**的支数（标签口径） */
  annotated: number;
  /** 其中有 patch 条目（= 有变更）的支数 */
  hasPatch: number;
  /** 名单里**没有条目**的支数（产物 == 基线 ⇒ 不进 patch） */
  unchanged: number;
  /** 基线根里签名认不出（不是 AGE 脚本）的 `.BIN` 个数 —— 它们不在名单里 */
  nonScript: number;
  dirs: { base: string };
  schema: string;
  scripts: ScriptRow[];
}

export type ScriptKind = 'data' | 'src';

export interface ScriptPayload {
  ok: true;
  readOnly: true;
  name: string;
  kind: ScriptKind;
  /** ★ 就是 `buildView` 的输出，逐字节；客户端不二次加工 */
  text: string;
  bytes: number;
  rows: number;
  stats: Record<string, number>;
  baseFrom: string;
  baseBytes: number;
  hasPatch: boolean;
}

export interface CheckReport {
  failures: number;
  checks: { id: number; text: string; problems: string[] }[];
}

/** `/api/health` 的 `writes`：**能不能建单**（由服务端监听地址派生）+ 表单要用的枚举（真源 = 模型） */
export interface WritesPayload {
  enabled: boolean;
  why: string;
  endpoint: string;
  fields: string[];
  types: string[];
  statuses: string[];
  severities: string[];
  /** 缺陷专属字段（`type=bug` 之外不许出现）—— 页面据此决定"显示哪几个输入框" */
  bugOnly: string[];
  defaults: { type: string; status: string };
}

export interface HealthPayload {
  ok: true;
  readOnly: true;
  requirementsDir: string;
  nodes: number;
  patch: string;
  patchBytes: number;
  patchScripts: number;
  base: string;
  /** 「旧管线标注过」的标签口径（正则）；旧 `target`（产物根）字段已废 */
  annotatedFilter: string;
  /** ★ 写路径的状态：只在监听回环时 `enabled`（页面据此决定"新建"按钮能不能按、为什么） */
  writes: WritesPayload;
  webBuilt: boolean;
  viewCache: { max: number; size: number; hits: number; misses: number };
}

/** 建单要给的字段（**值**，不是 argv）。空串 / 空数组一律省略 —— 省掉的东西由模型给缺省 */
export interface CreateSpec {
  title: string;
  type?: string;
  status?: string;
  parent?: string;
  order?: string;
  tags?: string[];
  verify?: string;
  repro?: string;
  severity?: string;
  body?: string;
}

export interface CreateResult {
  ok: true;
  created: { id: string; name: string; short: string; title: string; file: string };
  /** 台账目录（与 `/api/tree` 的 `dir` 同一个） */
  dir: string;
  /** 与 CLI 打印的 `add …` 那三行同源 —— 人一眼能看出"写到了哪、挂在哪" */
  plan: string[];
  report: CheckReport;
}

/**
 * 建单失败的两种情形合在一个 Error 里：
 *   · 请求被拒（`400/403/415/500`）：只有 `message`；
 *   · **写后守卫没过**（`422`）：`report` 是不变量逐条的问题清单（**服务端已经回滚**，磁盘干净）。
 */
export class CreateError extends Error {
  status: number;
  report: CheckReport | null;
  rolledBack: boolean;
  constructor(status: number, message: string, report: CheckReport | null = null, rolledBack = false) {
    super(message);
    this.name = 'CreateError';
    this.status = status;
    this.report = report;
    this.rolledBack = rolledBack;
  }
}

/** GET 一个 JSON 端点；`ok: false` 与 HTTP 错误都抛成可读的 Error。 */
export async function getJson<T>(url: string): Promise<T> {
  const res = await fetch(url, { headers: { accept: 'application/json' }, cache: 'no-store' });
  const data = (await res.json().catch(() => null)) as { ok?: boolean; error?: string } | null;
  if (!res.ok || !data || data.ok === false) throw new Error(data?.error ?? `HTTP ${res.status}`);
  return data as T;
}

/**
 * 建单（**唯一的写调用**）。
 *
 * ★ `Content-Type: application/json` 是**服务端的硬要求**（跨站页面发不出这种简单请求 ⇒ 浏览器会先
 *   预检，而服务端从不回 CORS 头 ⇒ 那种 POST 到不了这里）。别把这一行"顺手"改成 `text/plain`。
 */
export async function createNode(spec: CreateSpec): Promise<CreateResult> {
  const res = await fetch('/api/nodes', {
    method: 'POST',
    headers: { 'content-type': 'application/json', accept: 'application/json' },
    body: JSON.stringify(spec),
    cache: 'no-store',
  });
  const data = (await res.json().catch(() => null)) as
    | (CreateResult & { ok: true })
    | { ok: false; error?: string; report?: CheckReport; rolledBack?: boolean }
    | null;
  if (!res.ok || !data || data.ok === false) {
    const bad = (data ?? {}) as { error?: string; report?: CheckReport; rolledBack?: boolean };
    throw new CreateError(res.status, bad.error ?? `HTTP ${res.status}`, bad.report ?? null, bad.rolledBack === true);
  }
  return data;
}

export const getTree = () => getJson<TreePayload>('/api/tree');
export const getNode = (ref: string) => getJson<NodePayload>(`/api/node/${encodeURIComponent(ref)}`);
export const getScripts = () => getJson<ScriptsPayload>('/api/scripts');
export const getScript = (name: string, kind: ScriptKind) =>
  getJson<ScriptPayload>(`/api/script/${encodeURIComponent(name)}?kind=${kind}`);
export const getHealth = () => getJson<HealthPayload>('/api/health');
