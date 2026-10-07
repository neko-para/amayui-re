/**
 * apps/emulator/frontends/headless/chain-report.ts —— **启动链的指令缺口报告**（Node 侧，只读安装）
 *
 * ## 为什么需要它
 * 用户口径（A2）：**要按标准的从 `SYSTEM4` 启动的流程来实现**。而"实现启动链"如果不先量一下，
 * 就只剩两条路：一条一条撞（每轮只多跑一条指令），或者凭印象猜"大概需要几十条"。
 * ⇒ 先把**闭包**算出来：`SYSTEM4` → 它 `call-script` 的每一个 id → 那些脚本再 call 的……
 * 每一份脚本里**用到了哪些 opcode**、其中**哪些还没有 handler**。于是"还差什么"是一张有数字的表，
 * 而且**可以复跑**（脚本没变，数字就不该变）。
 *
 * ## 它怎么知道链上有哪些脚本
 * `call-script`（opcode 由指令表查出）的操作数 0 是**统一文件 id**（立即数）—— 而
 * **id == ALF 条目下标**（已登记进台账的观察）。⇒ 闭包可以纯静态算出来。
 * 另加一类：扩展包的 `$n$AUTORUN.BIN`（由无名指令 `i143` 派发，目标取决于装了哪些包）
 * ⇒ 按名字模式直接扫索引，把它们一并算进链里。
 *
 * ## ★ 它**不跑**任何东西
 * 只反汇编 + 数数。这是刻意的：报告要能在"模拟器还跑不起来"的时候就用（否则它就是鸡生蛋）。
 */

import { readAlf } from '@amayui/age-format/src/alf.mts';
import { OPCODE_TABLE, instructionForLabel } from '@amayui/age-format/src/asm/runtime.mts';
import { iterate } from '../../src/model/iterate.ts';
import { HANDLERS } from '../../src/vm/ops.ts';
import type { ReadSource } from '../../src/host/fs.ts';

/** 一条"某个 opcode 缺 handler"的记录（按用到它的脚本数降序 = 优先级） */
export interface MissingOpcode {
  opcode: number;
  name: string;
  argc: number;
  /** 闭包里**有多少份脚本**用到它（★ 与 `sites` 是两回事，别混） */
  scripts: number;
  /** 闭包里**总共出现多少次**（实现优先级看它：一处 handler 抵掉这么多次停摆） */
  sites: number;
  /** 一次实例（哪份脚本的第几条指令），便于立刻去取证 */
  sample: { script: string; index: number; byteOffset: number };
}

/** 一份脚本的统计 */
export interface ScriptStat {
  id: number;
  name: string;
  bytes: number;
  instructions: number;
  /** 这份脚本用到的 distinct opcode */
  opcodes: number;
  /** 其中没有 handler 的 */
  missing: number;
}

export interface ChainReport {
  scripts: ScriptStat[];
  /** 缺 handler 的 opcode（按"用到它的脚本数"降序） */
  missing: MissingOpcode[];
  /** 闭包的入口 id（通常是 0 = 根脚本） */
  roots: number[];
  /** 闭包在哪儿被**截断**的（`until` 命中）—— 截断点必须显式写出来，不许让人以为这就是全部 */
  cut: { at: string; fromScript: string; targetName: string; targetId: number } | null;
  /** 有 id 但索引里没有（或反之）的情况 —— 必须显式报出来，不许静默跳过 */
  unresolved: string[];
}

/** `call-script` 的 opcode 号**从指令表查**（不写死）；查不到就直接报错 */
function callScriptOpcode(): number {
  const def = instructionForLabel(OPCODE_TABLE, 'call-script');
  if (!def) throw new Error('指令表里找不到 `call-script` —— 闭包算不出来（不要写死 opcode 号）');
  return def.opcode;
}

/** `$n$AUTORUN.BIN`（扩展包的启动脚本；由无名指令 i143 派发） */
const AUTORUN_RE = /^\$\d+\$AUTORUN\.BIN$/i;

/**
 * 从入口 id 出发算出 `call-script` 闭包，并对每份脚本统计指令缺口。
 *
 * ★ `until`（缺省 `LOGO.BIN`）是**截断点**：某份脚本里一旦遇到 `call-script <until>`，
 *   就**不再从那之后**的指令里收目标 id。理由：`SYSTEM4` 是**线性**启动脚本
 *   （`… → INIT2 → LOGO → INIT → TITLE`），而 `TITLE` 会 `call-script` 出**整个游戏**
 *   （实测不截断时闭包 = 212 份脚本 / 15 万条指令，其中绝大多数与"启动到 LOGO"无关）。
 *   ⇒ 截断让这份报告回答的是"**跑到 LOGO 为止还差什么**"。
 *   传 `until: null` 得到全闭包（那是"整个游戏要什么"，是另一个问题）。
 */
export function buildChainReport(
  read: ReadSource,
  indexPath: string,
  opts: { roots?: number[]; until?: string | null } = {},
): ChainReport {
  const alf = readAlf(indexPath);
  const nameOf = (id: number): string | null => alf.entries[id]?.filename ?? null;
  const callOp = callScriptOpcode();
  const byOpcode = OPCODE_TABLE.byOpcode;

  const untilName = opts.until === undefined ? 'LOGO.BIN' : opts.until;
  const untilId = untilName === null
    ? null
    : alf.entries.findIndex((e) => e.filename.toUpperCase() === untilName.toUpperCase());
  if (untilId !== null && untilId < 0) throw new Error(`--until 指定的脚本不在索引里：${untilName}`);

  const unresolved: string[] = [];
  const scripts: ScriptStat[] = [];
  const missingByOpcode = new Map<number, MissingOpcode>();
  const missingScripts = new Map<number, Set<string>>();
  let cut: ChainReport['cut'] = null;

  // —— 闭包：从入口 + 所有 $n$AUTORUN 出发，沿 call-script 的立即数操作数走 ——
  const roots = opts.roots ?? [0];
  const queue: number[] = [...roots];
  for (let i = 0; i < alf.entries.length; i += 1) {
    if (AUTORUN_RE.test(alf.entries[i].filename)) queue.push(i);
  }
  const seen = new Set<number>();

  while (queue.length) {
    const id = queue.shift()!;
    if (seen.has(id)) continue;
    seen.add(id);
    const name = nameOf(id);
    if (!name) { unresolved.push(`id ${id}（0x${id.toString(16)}）：索引里没有这个名字`); continue; }
    const bytes = read.read(name);
    if (!bytes) { unresolved.push(`id ${id} = ${name}：读不到字节`); continue; }

    const iter = iterate(bytes, { table: OPCODE_TABLE, strict: false });
    const distinct = new Set<number>();
    for (const ins of iter.instructions) {
      distinct.add(ins.opcode);
      if (!HANDLERS[ins.opcode]) {
        const def = byOpcode.get(ins.opcode);
        const prev = missingByOpcode.get(ins.opcode);
        if (prev) prev.sites += 1;
        else {
          missingByOpcode.set(ins.opcode, {
            opcode: ins.opcode,
            name: def?.name ?? '',
            argc: Number(def?.argc ?? -1),
            scripts: 0, sites: 1,
            sample: { script: name, index: ins.index, byteOffset: ins.byteOffset },
          });
        }
        let set = missingScripts.get(ins.opcode);
        if (!set) { set = new Set(); missingScripts.set(ins.opcode, set); }
        set.add(name);
      }
      // 只把 `call-script` 的目标入队 —— 它的操作数 0 是**立即数**（统一文件 id）
      if (ins.opcode === callOp) {
        const a0 = ins.args[0];
        if (a0 && a0.type === 0) {
          const target = a0.rawData >>> 0;
          // ★ 截断：命中直到点之后，**这一份脚本**不再贡献新的目标
          if (untilId !== null && untilId >= 0 && target === untilId) {
            if (!cut) cut = { at: `${name}#${ins.index}`, fromScript: name, targetName: untilName ?? '', targetId: target };
            break;
          }
          queue.push(target);
        } else unresolved.push(`${name} 的第 ${ins.index} 条 call-script 的操作数 0 不是立即数 ⇒ 目标算不出来`);
      }
    }
    let missingHere = 0;
    for (const op of distinct) if (!HANDLERS[op]) missingHere += 1;
    scripts.push({ id, name, bytes: bytes.length, instructions: iter.instructions.length, opcodes: distinct.size, missing: missingHere });
  }

  for (const [op, set] of missingScripts) {
    const rec = missingByOpcode.get(op);
    if (rec) rec.scripts = set.size;
  }

  return {
    scripts: scripts.sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0)),
    missing: [...missingByOpcode.values()].sort((a, b) => b.sites - a.sites || a.opcode - b.opcode),
    roots,
    cut,
    unresolved,
  };
}

/** 人读的一屏报告（★ 数字由**跑一遍**得出，不手写） */
export function describeChainReport(r: ChainReport): string {
  const L: string[] = [];
  const totalIns = r.scripts.reduce((n, s) => n + s.instructions, 0);
  L.push(`启动链闭包：${r.scripts.length} 份脚本 · 共 ${totalIns} 条指令 · 入口 id ${r.roots.join(' / ')}`);
  if (r.cut) L.push(`闭包截断于：${r.cut.at} —— 那里 call-script 的是 **${r.cut.targetName}**（id 0x${r.cut.targetId.toString(16)}）；它之后的 call 目标不计入`);
  else L.push('闭包**未截断**（`until: null`）⇒ 下面这些是"整个游戏要什么"，不是"启动到 LOGO 要什么"');
  L.push('');
  L.push('缺 handler 的 opcode（按**出现次数**降序 —— 这就是实现顺序）：');
  L.push('  opcode  助记符          argc  出现  脚本数  一处实例');
  for (const m of r.missing) {
    L.push(`  0x${m.opcode.toString(16).padStart(4, '0')}  ${(m.name || '(无名)').padEnd(14)} ${String(m.argc).padStart(4)}  ${String(m.sites).padStart(5)}  ${String(m.scripts).padStart(5)}  ${m.sample.script}#${m.sample.index}`);
  }
  L.push('');
  L.push('逐脚本：');
  for (const s of r.scripts) {
    L.push(`  ${s.name.padEnd(20)} id=0x${s.id.toString(16)}  指令 ${String(s.instructions).padStart(6)}  opcode ${String(s.opcodes).padStart(3)}  缺 ${String(s.missing).padStart(3)}`);
  }
  if (r.unresolved.length) {
    L.push('');
    L.push('★ 算不出来的（必须显式看，不许静默跳过）：');
    for (const u of r.unresolved) L.push(`  - ${u}`);
  }
  return L.join('\n');
}
