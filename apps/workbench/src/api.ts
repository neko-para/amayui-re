/**
 * src/api.ts — 端点的取数层（**只读**）。
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
  webBuilt: boolean;
  viewCache: { max: number; size: number; hits: number; misses: number };
}

/** GET 一个 JSON 端点；`ok: false` 与 HTTP 错误都抛成可读的 Error。 */
export async function getJson<T>(url: string): Promise<T> {
  const res = await fetch(url, { headers: { accept: 'application/json' }, cache: 'no-store' });
  const data = (await res.json().catch(() => null)) as { ok?: boolean; error?: string } | null;
  if (!res.ok || !data || data.ok === false) throw new Error(data?.error ?? `HTTP ${res.status}`);
  return data as T;
}

export const getTree = () => getJson<TreePayload>('/api/tree');
export const getNode = (ref: string) => getJson<NodePayload>(`/api/node/${encodeURIComponent(ref)}`);
export const getScripts = () => getJson<ScriptsPayload>('/api/scripts');
export const getScript = (name: string, kind: ScriptKind) =>
  getJson<ScriptPayload>(`/api/script/${encodeURIComponent(name)}?kind=${kind}`);
export const getHealth = () => getJson<HealthPayload>('/api/health');
