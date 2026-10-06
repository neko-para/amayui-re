/** @env assets @kind contract @why "纯数值族"的判据失效了：某条 handler 真的碰了引擎状态（或读操作数原语没被调用） */
/**
 * tools/test/emulator-numeric-purity.test.mjs —— **"纯数值族"这条判据本身**的守卫（批 R1 迭代点 ⑤ 尾）
 *
 * ## 为什么需要它
 * 上一支守卫只核对了"成员、handler、argc、出现次数"——那都是**清单层面**。
 * 但"纯数值"这个词的**含义**是"handler 只经取操作数原语读写操作数，不碰引擎状态"。
 * 如果没人验这条**判据**，那份清单就只是"我按旧仓抄的一串 opcode"。
 * 本守卫逐条读 handler 体，把判据变成**机械可判**的。
 *
 * ## 判据（三条，都对整个函数体生效）
 * 1. 体内**必须**调用取操作数原语（`sub_41BF50` 读值 / `sub_42B4B0` 写值；
 *    float 变体 `sub_41C300` / `sub_42BA00`）—— 否则它凭什么叫"操作数指令"。
 * 2. 体内的 `Engine` 字段访问**只允许 dispatcher 的出参那三处**（所有指令都做，不是本条指令的副作用）：
 *    `帧+0x00`(cur) · `帧+0x18`(操作数基址) · `帧+0x74`(本指令 arity = `2*argc+1`)。
 *    ★ 实测样例：`add` 的体内是 `mov eax,[esi+5D880h]` → `15·cur` → `mov dword ptr [esi+ecx*8+5D8F4h], 7`
 *    （7 = `2*3+1`），随后两次 `call sub_41BF50`（读 op2/op3）与一次 `call sub_42B4B0`（写 op1）。
 * 3. `Command_Type_Exception` 那个全局（`dword_55D528`）**只许读、不许写**
 *    （取址原语把当前操作数地址暂存在那里；写它 = 篡改派发状态）。
 *
 * ★ 这条守卫**红得有意义**：谁把一条真的会改引擎状态的指令塞进"纯数值族"（或反过来漏了原语调用），
 *   它会点名是哪条 opcode、哪一行、哪个偏移。
 *
 * 运行：`pnpm test:assets`
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';
import { buildIndex, spanOfFunction } from '../lib/disasm.mjs';
import { NUMERIC_OPS, TOUCHES_ENGINE_STATE } from '../../apps/emulator/src/model/numeric-ops.ts';
// ★ handler 名（`sub_xxxxxx`）是**逆向观察**（哪段代码实现了它）⇒ 在知识层，不在模拟器里
import { OPCODE_HANDLERS } from '@amayui/age-format/src/engine/handlers.mts';

const FILES_DIR = path.join(REPO_ROOT, 'corpus', 'disasm', 'files');
const listing = (() => {
  if (!fs.existsSync(FILES_DIR)) return null;
  const lst = fs.readdirSync(FILES_DIR).filter((f) => f.endsWith('.lst')).sort();
  return lst.length ? path.join(FILES_DIR, lst[0]) : null;
})();
const skip = listing ? false : '语料未解压（先 `pnpm tools disasm build`）';

/** 取操作数的四个原语（值与 float 变体） */
const OPERAND_PRIMITIVES = ['sub_41BF50', 'sub_42B4B0', 'sub_41C300', 'sub_42BA00'];
/** dispatcher 的出参：所有指令都会写的帧内字段（允许出现） */
const DISPATCH_ALLOWED = new Map([
  [0x5d880, 'cur（当前帧下标）'],
  [0x5d898, '操作数基址槽'],
  [0x5d8f4, '本指令 arity（= 2*argc+1）'],
]);
/** `Command_Type_Exception` 的暂存全局：只许读 */
const CMD_TYPE_TMP = 0x55d528;

/** 把一行归一成指令（去前缀、压空白、去逗号后空格） */
const norm = (l) => l.replace(/^\S+:[0-9A-F]{8}\s*/, '').trim().replace(/\s+/g, ' ').replace(/, /g, ',');

/** 逐条分析一个 handler 体 */
function analyse(body, fromLine) {
  const facts = { primitiveCalls: 0, engineAccesses: [], tmpWrites: [], callTargets: new Set() };
  body.forEach((raw, i) => {
    const l = norm(raw);
    const line = fromLine + i;
    const call = /^call (\S+)/.exec(l);
    if (call) facts.callTargets.add(call[1]);
    if (OPERAND_PRIMITIVES.some((p) => l.startsWith(`call ${p}`))) facts.primitiveCalls += 1;
    // 索引形式 `[reg+reg*8+5D8xxh]` 与扁平形式 `[reg+5D8xxh]`
    for (const m of l.matchAll(/\[[^\]]*?\+(5D8[0-9A-F]{2})h\]/gi)) {
      const abs = Number.parseInt(m[1], 16);
      if (!DISPATCH_ALLOWED.has(abs)) facts.engineAccesses.push({ line, at: `0x${abs.toString(16)}`, text: l });
    }
    if (new RegExp(`mov\\s+\\[[^\\]]*\\+${CMD_TYPE_TMP.toString(16).toUpperCase()}h\\]`, 'i').test(l)) {
      facts.tmpWrites.push({ line, text: l });
    }
  });
  return facts;
}

/**
 * ★ 只扫一遍：四个用例共享同一份分析结果（每条 handler 单独扫一遍会让本文件跑 16 s；共享后 ~4 s）。
 *   懒加载 + 缓存 —— 语料不在场时返回 null，各用例自行 skip。
 */
let ANALYSIS = null;
function analysis() {
  if (ANALYSIS) return ANALYSIS;
  const idx = buildIndex(listing);
  const perOp = new Map();
  for (const op of NUMERIC_OPS) {
    const handler = OPCODE_HANDLERS[op.opcode];
    const ea = Number.parseInt(handler.slice(4), 16);
    const span = spanOfFunction(listing, idx, ea);
    perOp.set(op.opcode, { op, span, facts: analyse(span.lines, span.fromLine), body: span.lines.map(norm) });
  }
  ANALYSIS = perOp;
  return ANALYSIS;
}

test('★ 每条 handler 都必须真的调用取操作数原语（否则它不该叫"操作数指令"）', { skip }, () => {
  const bad = [];
  for (const { op, facts } of analysis().values()) {
    if (facts.primitiveCalls === 0) bad.push(`0x${op.opcode.toString(16)}（${OPCODE_HANDLERS[op.opcode]}）：体内一次都没调用取操作数原语`);
  }
  assert.deepEqual(bad, [], `这些 handler 没碰操作数原语：\n  - ${bad.join('\n  - ')}`);
});

/**
 * ★ **体内所有非零绝对位移**（机械）：这是"碰不碰引擎状态"的**唯一可靠判据**。
 * 为什么不能用"只认 `5D8xx`"那种窄正则：我原先就是这么写的，于是**漏掉了 4 条真的碰引擎状态的指令**
 * （独立复核抓到的）：`0x60` 写 `Engine+0x69330h`、`0x2DA/0x2DB/0x2DD` 碰 `+0x14D30/+0x313D0/+0x460F0`。
 * ⇒ 全称断言"体内不碰引擎状态"**不成立**；正确说法见 `numeric-ops.mjs` 的 `TOUCHES_ENGINE_STATE`。
 */
function displacements(body) {
  const out = new Set();
  for (const l of body) {
    for (const m of l.matchAll(/\[[^\]]*?([0-9A-F]{4,6})h\]/gi)) {
      const v = Number.parseInt(m[1], 16);
      if (v > 0x100) out.add(v);
    }
  }
  return out;
}

test('★ 判据修正：**"纯数值"不是"不碰引擎状态"** —— 碰的那 4 条必须逐条列在模型里', { skip }, () => {
  const found = new Map(); // opcode -> Set(位移)
  for (const { op, body } of analysis().values()) {
    const d = displacements(body);
    // 允许清单：帧内 dispatcher 出参（cur / 操作数基址 / arity）
    for (const a of [0x5d880, 0x5d898, 0x5d8f4]) d.delete(a);
    if (d.size) found.set(op.opcode, d);
  }
  const declared = new Set(TOUCHES_ENGINE_STATE);
  const actual = new Set(found.keys());
  const missing = [...actual].filter((o) => !declared.has(o)).sort((a, b) => a - b);
  const stale = [...declared].filter((o) => !actual.has(o)).sort((a, b) => a - b);
  assert.deepEqual(
    missing.map((o) => `0x${o.toString(16)}（位移 ${[...found.get(o)].sort((x, y) => x - y).map((v) => `0x${v.toString(16)}`).join(' ')}）`),
    [],
    '★ 这些 handler 碰了 dispatcher 出参以外的引擎状态，但模型没登记 ⇒ 全称断言会再次变成假的',
  );
  assert.deepEqual(stale.map((o) => `0x${o.toString(16)}`), [], '模型登记了"碰引擎状态"，实测却没找到（清单过期）');
  assert.ok(actual.size > 0, '★ 实测至少有一批会碰（不是零）—— 这正是"全称断言不成立"的证据');
});


test('★ `Command_Type_Exception` 的暂存全局只许读、不许写', { skip }, () => {
  const bad = [];
  for (const { op, facts } of analysis().values()) {
    for (const w of facts.tmpWrites) bad.push(`0x${op.opcode.toString(16)}（${OPCODE_HANDLERS[op.opcode]}）行 ${w.line}：${w.text}`);
  }
  assert.deepEqual(bad, [], `不许写 dword_55D528（取址原语的暂存）:\n  - ${bad.join('\n  - ')}`);
});

test('★ dispatcher 出参那条口径要对（`帧+0x74 ← 2*argc+1`）—— 抽 `add` 与 `mov` 两条验', { skip }, () => {
  for (const opcode of [0x50, 0x55]) {
    const { op, body } = analysis().get(opcode);
    const want = 2 * op.argc + 1;
    const m = body.map((l) => /mov dword ptr \[esi\+ecx\*8\+5D8F4h\],(\d+)/.exec(l)).find(Boolean);
    assert.ok(m, `${OPCODE_HANDLERS[op.opcode]} 体内应写着 \`帧+0x74 ← N\`（dispatcher 出参）`);
    assert.equal(Number(m[1]), want, `${OPCODE_HANDLERS[op.opcode]}（argc=${op.argc}）的出参应是 ${want}，实际 ${m[1]}`);
  }
});
