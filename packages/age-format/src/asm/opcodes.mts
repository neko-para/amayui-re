/**
 * packages/age-format/src/asm/opcodes.mts —— **AGE 脚本指令集**（本包自带的那份数据 + name/别名/opcode 三向查找）
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
 *
 * ## ★ 表是**随模块一起来的**（`import … with { type: 'json' }`），本文件**不碰文件系统**
 * 数据就在同目录的 `instruction-set.json`，用 **ESM 的 JSON 模块**直接 import ⇒ Node 原生支持，
 * 打包器（Vite 等）与浏览器（`with { type: 'json' }`）也支持 ⇒ **一份数据、三种宿主都能读，零 polyfill**。
 * ★ 于是整个 `src/asm/**` **一个 `node:` 依赖都没有**（判据：`rg 'node:' packages/age-format/src/asm/` 为空），
 *   而这正是上一轮那条边界的根因 —— 原来 `node:fs` 是**唯一**把 asm 拖成"Node 专用"的东西。
 * ★ **"加载任意一份表"不是本层的事**：真要读别的文件，调用方自己
 *   `buildOpcodeTable(JSON.parse(fs.readFileSync(p,'utf8')), p)` —— 纯函数 `buildOpcodeTable` 就是那道缝。
 *   （原先有个 `loadOpcodeTable(file?)`，实测**全仓无一处传路径** ⇒ 那个能力是空想出来的，已删。）
 * ★ 表在**模块加载时构建一次**（`OPCODE_TABLE`）：工具侧一次进程里 `assemble()` 会被调很多次，
 *   原先每次都要重读 + 重解析整个 JSON；现在只有一次。
 */
import defs from './instruction-set.json' with { type: 'json' };

/**
 * 指令表里的一条（= `instruction-set.json` 的一个元素）。
 * ★ 类型住在**实现旁边**（本包全 `.mts`，一份真源）：消费方
 *   `import type { OpcodeDef } from '@amayui/age-format/src/asm/runtime.mts'`，
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

/** 指令表：`buildOpcodeTable()` / `OPCODE_TABLE` 的形状 */
export interface OpcodeTable {
  /** 原始数组（顺序即文件顺序） */
  readonly entries: readonly OpcodeDef[];
  /** opcode → 条目 */
  readonly byOpcode: Map<number, OpcodeDef>;
  /** name / 别名 → 条目 */
  readonly byLabel: Map<string, OpcodeDef>;
  /** 表的来源标识（诊断用；不再是一条文件路径 —— 本层不碰文件系统） */
  readonly source: string;
}

/**
 * **建表**（纯函数）：把一串 `OpcodeDef` 变成三向可查的 `OpcodeTable`。
 * ★ 数据从哪来**与本函数无关** —— 本包自带的那份走模块顶层的 JSON import（见 `OPCODE_TABLE`）；
 *   要读别的文件 / 从网络取，调用方自己把数组弄来再调本函数。**这就是那道缝**。
 *
 * @param entries 原始数组（顺序即文件顺序）
 * @param source  来源标识（**只用于诊断**；纯函数不碰文件系统，所以由调用方给）
 */
export function buildOpcodeTable(entries: readonly OpcodeDef[], source: string): OpcodeTable {
  if (!Array.isArray(entries)) throw new Error(`指令表不是数组: ${source}`);
  const byOpcode = new Map<number, OpcodeDef>();
  const byLabel = new Map<string, OpcodeDef>();
  for (const e of entries) {
    if (byOpcode.has(e.opcode)) throw new Error(`指令表重复 opcode 0x${e.opcode.toString(16)}`);
    byOpcode.set(e.opcode, e);
    if (e.name) byLabel.set(e.name, e);
    // 别名不覆盖主名：先注册者胜
    for (const a of e.aliases || []) {
      if (!byLabel.has(a)) byLabel.set(a, e);
    }
  }
  return { entries, byOpcode, byLabel, source };
}

/**
 * ★ **本包自带的那份指令表**（模块加载时构建一次）。
 *
 * 数据来自同目录的 `instruction-set.json`（ESM JSON 模块 import）—— 于是**没有文件系统**、
 * 没有"装载函数"、也没有"路径"这个概念。三种宿主都能读同一份：Node（原生支持 import attributes）、
 * 打包器（Vite 等把 JSON 转成模块）、浏览器（`with { type: 'json' }`）。
 *
 * ★ `source` 记的是**逻辑名**（不是路径）：本层已经不知道自己在什么目录下了。
 */
export const OPCODE_TABLE: OpcodeTable = buildOpcodeTable(defs, 'instruction-set.json');

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
