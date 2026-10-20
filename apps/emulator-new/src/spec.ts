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
