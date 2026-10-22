/**
 * 平台层（Node）：`data/spec/**` 的**唯一读取点**。
 *
 * ★ 读的是规范自带的伴随数据：`format/opcode-table.json`（指令表）与
 *   `format/config-keys.json`（配置键词表）。核心只收**纯数据**（`core/spec.ts` 的形状 + 构造器）
 *   ⇒ 换一个平台（浏览器 / worker）时，只要换这一层的 `fetch` / `Host`，核心一行不动。
 * ★ 只读 `data/spec/`；不读 shape / ledger / corpus / age-format（隔离守卫会红）。
 */
import path from 'node:path';
import { fileURLToPath } from 'node:url';

import { buildConfigKeys, buildOpcodeTable } from '../../core/spec.ts';
import type { ConfigKeySchema, OpcodeRow } from '../../core/spec.ts';
import { nodeHost } from './host.ts';

const HERE = path.dirname(fileURLToPath(import.meta.url));
export const REPO_ROOT = path.resolve(HERE, '..', '..', '..', '..', '..');
export const SPEC_DIR = path.join(REPO_ROOT, 'data', 'spec');

/** 指令表：`data/spec/format/opcode-table.json`（规范自带的伴随数据） */
export async function loadOpcodeTable(specDir: string = SPEC_DIR): Promise<Map<number, OpcodeRow>> {
  const file = path.join(specDir, 'format', 'opcode-table.json');
  const rows = JSON.parse(await nodeHost.readText(file)) as OpcodeRow[];
  return buildOpcodeTable(rows);
}

/** 配置键词表：`data/spec/format/config-keys.json`（规范自带的伴随数据） */
export async function loadConfigKeys(specDir: string = SPEC_DIR): Promise<ConfigKeySchema> {
  const file = path.join(specDir, 'format', 'config-keys.json');
  return buildConfigKeys(JSON.parse(await nodeHost.readText(file)) as ConfigKeySchema);
}
