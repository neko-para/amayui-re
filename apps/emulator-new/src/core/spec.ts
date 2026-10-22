/**
 * spec 数据的**形状** + 纯构造器（核心层）。
 * ★ 本文件**不读文件**：`data/spec/**` 由平台层读成纯数据再传进来（`platform/node/spec.ts`）。
 *   核心因此不认识路径、不认识 `data/spec`、也不认识 `node:fs` —— 浏览器里同样能建表。
 * ★ 只接受规范自带的伴随数据（`format/opcode-table.json` / `format/config-keys.json`）；
 *   不读 shape / ledger / corpus / age-format。
 */

export interface OpcodeRow {
  opcode: number;
  argc: number;
  name: string;
  aliases?: string[];
}

/**
 * 建指令表：`data/spec/format/opcode-table.json` 解出来的行（平台层负责 `JSON.parse`）。
 * ★ 后出现的同 opcode 行覆盖先出现的（与旧实现一致：直接 `Map.set`）。
 */
export function buildOpcodeTable(rows: readonly OpcodeRow[]): Map<number, OpcodeRow> {
  const table = new Map<number, OpcodeRow>();
  for (const row of rows) table.set(row.opcode, row);
  return table;
}

/* ──────────────────────────── 配置键词表 ──────────────────────────── */

export type ConfigValueKind = 'int' | 'string' | 'list' | 'ignore';

export interface DefaultKeyRow {
  /** 索引默认表里的大写键（匹配大小写不敏感） */
  indexKey: string;
  /** 配置库内部键；`kind=ignore` 时为 null */
  internal: string | null;
  kind: ConfigValueKind;
  /** 复合 / 归一化处理 */
  special?: 'saveVersionSplit' | 'bool' | 'ignore' | 'noSetMusic' | 'enableAntiFont' | 'int64';
  /** 派生写（同一次解析里再写一个键） */
  extra?: { internal: string; from: 'value' | 'value-1'; if?: 'value!=0' }[];
}

export interface IniKeyRow {
  section: string;
  key: string;
  internal: string;
  kind: 'int' | 'string';
  /** 条件读取：该内部键为 0 时忽略本行 */
  if?: string;
}

export interface PersistKeyRow {
  section: string;
  key: string;
  internal: string;
  kind: 'int' | 'string';
  /** 条件写出：该内部键为 0 时跳过本行 */
  if?: string;
}

export interface ConfigKeySchema {
  defaults: DefaultKeyRow[];
  ini: IniKeyRow[];
  persist: PersistKeyRow[];
}

/**
 * 配置键词表（`data/spec/format/config-keys.json` 解出来的纯数据）。
 * ★ 只做形状断言，不复制第二份键表 —— 词表本体仍然只有规范里那一份。
 */
export function buildConfigKeys(raw: ConfigKeySchema): ConfigKeySchema {
  for (const field of ['defaults', 'ini', 'persist'] as const) {
    if (!Array.isArray(raw[field])) throw new Error(`config-keys-malformed:${field}`);
  }
  return raw;
}
