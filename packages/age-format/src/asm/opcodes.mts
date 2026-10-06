/**
 * packages/age-format/src/asm/opcodes.mjs —— **AGE 脚本指令集**（数据加载 + name/别名/opcode 三向查找）
 *
 * 来源（旧仓只读参照，逐行对照移植）：
 *   · `天結/scripts/asm/age-shared.mjs` —— `loadOpcodeTable` / `instructionForOpCode` /
 *     `instructionForLabel` / `opcodeLabel` / `instructionForToken`
 *   · `天結/scripts/asm/opcodes.json` —— 指令集数据；本目录的 `instruction-set.json` 是它**机械派生的格式层四列**
 *     （`pnpm tools opcodes derive`；`handler` / `status` 是引擎逆向知识，**有意未随格式层进来**）
 *
 * ## 非显然口径
 * 1. **`argc` 是唯一决定指令边界的字段**：一条指令的长度 = `4 + argc*8` 字节
 *    （4 字节 opcode + 每操作数 `type`/`raw_data` 各 4 字节）。反汇编写出去、重汇编读回来都只认它。
 * 2. JSON 里 `name` 可以为空串（未命名 opcode）⇒ 显示名退化成规范形式 `iXXX`（`XXX` = 十六进制 opcode，
 *    至少 3 位）。这个规范名**不落 JSON**：`instructionForToken` 能在找不到助记符时按 `iXXX` 反查，
 *    于是"反汇编输出 → 重汇编"仍闭环。
 * 3. **重复 opcode / 重复助记符不静默吞**：重复 opcode 直接抛错；别名冲突时**先注册者胜**
 *    （别名只是给新写法留后门，不该覆盖主名）。
 * 4. **本表只有格式层四列**（`opcode` / `argc` / `name` / `aliases`）：助记符是"工具输出什么文本"的输入，
 *    `argc` 是边界，`name`/`aliases` 是重汇编的查找键 —— 缺一不可。
 *    ⚠ opcode → **引擎函数**的对应（旧表里的 `handler`）与核对状态（`status`）是**知识层**，
 *    归 K 线（`docs/00-origin/knowledge-rebuild.md`）；**不要**把它们塞回本表。
 */
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));

/** 派生表的绝对路径（数据与代码同目录）。★ **文件名有意不叫 `opcodes.json`** —— 那是旧仓原表的文件名，
 *  容易让人以为"这份就是那份"；这份是**派生**的格式层四列。 */
export const OPCODES_JSON = path.join(__dirname, 'instruction-set.json');

/**
 * 指令表里的一条（= `instruction-set.json` 的一个元素）。
 * ★ 类型住在**实现旁边**（本包全 `.mts`，一份真源）：消费方
 *   `import type { OpcodeDef } from '@amayui/age-format/src/asm/index.mts'`，
 *   不许各自再声明一份（两边说法不一致时没人会发现）。
 */
export interface OpcodeDef {
  readonly opcode: number;
  readonly name: string;
  readonly argc: number;
  readonly aliases?: readonly string[];
  /** 表里可能还有别的列（本层不解释语义） */
  readonly [k: string]: unknown;
}

/** 指令表：`loadOpcodeTable()` 的返回形状 */
export interface OpcodeTable {
  /** 原始数组（顺序即文件顺序） */
  readonly entries: readonly OpcodeDef[];
  /** opcode → 条目 */
  readonly byOpcode: Map<number, OpcodeDef>;
  /** name / 别名 → 条目 */
  readonly byLabel: Map<string, OpcodeDef>;
  /** 表的来源路径（诊断用） */
  readonly source: string;
}

/**
 * 加载指令表。`file` 缺省指向本目录 `instruction-set.json`。
 * 返回 `{ entries, byOpcode, byLabel, source }`：`entries` 是原始数组，
 * `byOpcode` 是 opcode → 条目，`byLabel` 是 name/别名 → 条目。
 */
export function loadOpcodeTable(file?: string): OpcodeTable {
  const p = file || OPCODES_JSON;
  const entries = JSON.parse(fs.readFileSync(p, 'utf8'));
  if (!Array.isArray(entries)) throw new Error(`指令表不是数组: ${p}`);
  const byOpcode = new Map();
  const byLabel = new Map();
  for (const e of entries) {
    if (byOpcode.has(e.opcode)) throw new Error(`指令表重复 opcode 0x${e.opcode.toString(16)}`);
    byOpcode.set(e.opcode, e);
    if (e.name) byLabel.set(e.name, e);
    for (const a of e.aliases || []) {
      if (!byLabel.has(a)) byLabel.set(a, e);
    }
  }
  return { entries, byOpcode, byLabel, source: p };
}

/** 规范名 `iXXX` → opcode；不是这个形状 ⇒ null */
export function normalizedOpcode(token: string): number | null {
  const m = /^i([0-9a-fA-F]+)$/.exec(token);
  return m ? parseInt(m[1], 16) : null;
}

/** opcode → 指令条目；未知 ⇒ null */
export function instructionForOpCode(table: OpcodeTable, op: number): OpcodeDef | null {
  return table.byOpcode.get(op) ?? null;
}

/** 助记符（name 或 aliases）→ 指令条目；未知 ⇒ null */
export function instructionForLabel(table: OpcodeTable, label: string): OpcodeDef | null {
  return table.byLabel.get(label) ?? null;
}

/**
 * 指令的显示名（反汇编输出用）。
 * 有助记符用助记符；无助记符则按 opcode 生成规范 `iXXX`（`XXX` 左补 0 至少 3 位）。
 */
export function opcodeLabel(def: OpcodeDef): string {
  if (def.name) return def.name;
  return 'i' + (def.opcode >>> 0).toString(16).padStart(3, '0');
}

/**
 * 解析指令 token（重汇编用）：先按助记符（name/aliases）查；否则按规范 `iXXX` 反查 opcode。
 * ⇒ "反汇编输出（含未命名指令的 `iXXX`）→ 重汇编" 不丢信息。
 */
export function instructionForToken(table: OpcodeTable, token: string): OpcodeDef | null {
  const found = table.byLabel.get(token);
  if (found) return found;
  const op = normalizedOpcode(token);
  if (op !== null) return table.byOpcode.get(op) ?? null;
  return null;
}

/** 一条指令在字节码里占多少字节（`4 + argc*8`） */
export const instructionByteLength = (def: Pick<OpcodeDef, 'argc'>): number => 4 + ((def.argc >>> 0) << 3);
