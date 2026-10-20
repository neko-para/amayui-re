/**
 * spec 的唯一读取点。
 * ★ 本文件是 `apps/emulator-new/src/` 里**唯一**碰文件系统的模块（守卫会检查）。
 * ★ 只读 `data/spec/`；不读 shape / ledger / corpus / age-format。
 */
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
export const REPO_ROOT = path.resolve(HERE, '..', '..', '..');
export const SPEC_DIR = path.join(REPO_ROOT, 'data', 'spec');

export interface OpcodeRow {
  opcode: number;
  argc: number;
  name: string;
  aliases?: string[];
}

/** 指令表：`data/spec/format/opcode-table.json`（规范自带的伴随数据） */
export function loadOpcodeTable(): Map<number, OpcodeRow> {
  const file = path.join(SPEC_DIR, 'format', 'opcode-table.json');
  const rows = JSON.parse(fs.readFileSync(file, 'utf8')) as OpcodeRow[];
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
  special?: 'saveVersionSplit' | 'bool' | 'ignore';
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

/** 配置键词表：`data/spec/format/config-keys.json`（规范自带的伴随数据） */
export function loadConfigKeys(): ConfigKeySchema {
  const file = path.join(SPEC_DIR, 'format', 'config-keys.json');
  return JSON.parse(fs.readFileSync(file, 'utf8')) as ConfigKeySchema;
}
