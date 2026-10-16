/** @env assets @kind gate @why claims 校验器的义务被改坏（覆盖/步长/边界/下钻/两道守卫），或语料与声明不再自洽 */
/**
 * tools/test/shape-claims.assets.test.mjs —— **校验器功能②** 在真语料上的守卫
 *
 * 判据（红了意味着什么）：
 *  ① 内置样本必须**全绿**且两条守恒成立 —— 声明与语料自洽；
 *  ② 把声明**改坏一处**（步长 / 成员 / count / 重叠 / 越界 / 类型 / 业务词 / 转录字段）⇒ 必须红，
 *     而且**红的理由要对**（不是随便红）；
 *  ③ **二级对象下钻**四种局面：被目标类型解释 / 目标类型 complete 却缺成员 / 宿主没声明 ptr /
 *     宿主是 ptr 但 type=null（**不透明桶**，绿但必须报出来 —— ⛔ 不许静默通过）。
 *
 * 要语料（`corpus/disasm/files/`）；缺席时 `t.skip`。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { analyzeLines, functionSpans, loadListing, objRootRegs } from '../lib/shape-access.mjs';
import { checkClaims, sampleModel } from '../lib/shape-claims.mjs';
import { DEFAULT_OUT_DIR, REPO_ROOT } from '../lib/paths.mjs';

const hasCorpus = fs.existsSync(DEFAULT_OUT_DIR) && fs.readdirSync(DEFAULT_OUT_DIR).some((f) => f.endsWith('.lst'));
const skip = hasCorpus ? false : `语料未解压（先 \`pnpm tools disasm build\`）—— 期望目录 ${path.relative(REPO_ROOT, DEFAULT_OUT_DIR)}`;

/** 观察集只算一次（G6 的正确判据：它能解折叠形态） */
const ctx = (() => {
  if (!hasCorpus) return null;
  const lines = loadListing();
  const roots = objRootRegs();
  const observed = new Set();
  for (const sp of functionSpans(lines)) {
    const { sites } = analyzeLines(lines, sp.from, sp.to, roots.get(sp.sym) ?? null, {});
    for (const s of sites) if (s.obj === 'Engine' && s.absDisp !== null) observed.add(s.absDisp);
  }
  return { lines, observed, baseDir: REPO_ROOT };
})();

const run = (model) => checkClaims(model, ctx);
const clone = () => JSON.parse(JSON.stringify(sampleModel()));
const carrierOf = (m, id) => m.carriers.find((c) => c.id === id);

test('样本（13 个函数的载体 + 两个共享类型）与语料自洽 ⇒ 全绿 + 两条守恒成立', { skip }, () => {
  const rep = run(sampleModel());
  assert.deepEqual(rep.findings, [], `样本应全绿，实际：${JSON.stringify(rep.findings, null, 2)}`);
  const { cons } = rep;
  assert.equal(cons.obj0Acc, cons.obj0Explained + cons.obj0Residue, '守恒①：obj0 访问 = 解释 + 残差');
  assert.equal(cons.pointee, cons.pointeeOk + cons.pointeeOpaque + cons.pointeeBad, '守恒②：二级 = 解释 + 不透明 + 拒绝');
  assert.ok(cons.obj0Explained > 100, `解释数应由语料给出（实测 ${cons.obj0Explained}）`);
  assert.equal(cons.deep, 0, '样本这 13 个函数里不应有 hop≥3（有就该单列出来，而不是静默）');
});

test('★ 改坏一处 ⇒ 红，而且**红的理由要对**', { skip }, () => {
  const CASES = [
    { name: 'T1.size 120 → 124（步长义务）', want: 'G3', make: (m) => { m.types.T1.size = 124; } },
    { name: 'T1 删掉成员 0x60（数组元素内偏移无处安放）', want: 'G2', make: (m) => { m.types.T1.members = m.types.T1.members.filter((x) => x.off !== '0x60'); } },
    { name: 'T0 数组成员 count 40 → 20（末尾零引用）', want: 'G4', make: (m) => { m.types.T0.members.find((x) => x.count).count = 20; } },
    { name: 'T0 数组成员挪到 0x5D880（与标量重叠）', want: 'G1', make: (m) => { m.types.T0.members.find((x) => x.count).off = '0x5d880'; } },
    { name: 'T1.size 缩到 0x40（成员越界）', want: 'G1', make: (m) => { m.types.T1.size = 64; } },
    { name: '某个载体类型改成 T1*（几何不符）', want: 'G2', make: (m) => { carrierOf(m, 'c01').type = 'T1*'; } },
    { name: '声明里塞业务词 argc（词汇守卫）', want: '守卫·词汇', make: (m) => { carrierOf(m, 'c01').argc = 3; } },
    { name: '声明里塞转录字段 mn（冗余守卫）', want: '守卫·冗余', make: (m) => { carrierOf(m, 'c01').mn = 'add'; } },
  ];
  for (const cs of CASES) {
    const m = clone();
    cs.make(m);
    const rep = run(m);
    assert.ok(rep.findings.length > 0, `变异「${cs.name}」没红 —— 守卫失效`);
    assert.ok(rep.findings.some((f) => f.where.startsWith(cs.want) || f.where.includes(cs.want)),
      `变异「${cs.name}」红的理由应是 ${cs.want}，实际：${JSON.stringify(rep.findings.map((f) => f.where))}`);
  }
});

test('★ 二级对象下钻：三档 + 不透明单列（⛔ 不许静默通过）', { skip }, () => {
  const mk = (t0mem, tb) => {
    const m = clone();
    m.carriers = [{ id: 'p01', kind: 'arg', fn: 'sub_42AEA0', reg: 'ecx', type: 'T0*' }];
    m.types.T0.members = [...m.types.T0.members, t0mem];
    if (tb) m.types.TB = tb;
    return m;
  };
  // (a) 宿主 ptr→TB，TB 有成员 0x0 ⇒ 那一处二级访问被 TB 解释
  const a = run(mk({ off: '0x5d818', width: 4, kind: 'ptr', type: 'TB' }, { size: null, complete: false, members: [{ off: '0x0', width: 4 }] }));
  assert.deepEqual(a.findings, []);
  assert.equal(a.perCarrier[0].pointeeOk, 1, '应恰好解释 1 处二级访问（`[eax+edx*4]`，宿主 0x5D818）');
  // (b) TB complete 但没有 0x0 ⇒ 红（理由 G5'）
  const b = run(mk({ off: '0x5d818', width: 4, kind: 'ptr', type: 'TB' }, { size: null, complete: true, members: [{ off: '0x10', width: 4 }] }));
  assert.ok(b.findings.some((f) => f.where.startsWith("G5'")), `应报 G5'，实际 ${JSON.stringify(b.findings.map((f) => f.where))}`);
  // (c) 宿主没声明 kind:ptr（却是指针用法）⇒ 红
  const c = run(mk({ off: '0x5d818', width: 4 }, null));
  assert.ok(c.findings.some((f) => f.where.startsWith("G5'") && f.why.includes('kind:"ptr"')), '应因"没声明 ptr"红');
  // (d) 宿主是 ptr 但目标类型 null ⇒ 绿，且**必须**出现在不透明桶里
  const d = run(mk({ off: '0x5d818', width: 4, kind: 'ptr' }, null));
  assert.deepEqual(d.findings, []);
  assert.equal(d.perCarrier[0].pointeeOpaque, 1, '不透明必须单列（不是通过）');
  assert.ok(d.perCarrier[0].opaqueEAs.includes(0x42af25), '不透明的 EA 要打得出来');
});

test('★ 层级 ≥3 的访问必须被**单列报出**（上限冻结在层级 2，但不许静默丢）', { skip }, () => {
  const m = clone();
  m.carriers = [{ id: 'h01', kind: 'arg', fn: 'sub_409F90', reg: 'ecx', type: 'T0*' }];
  const rep = run(m);
  assert.ok(rep.cons.deep >= 1, `sub_409F90 里有层级 ≥3 的访问，必须计入"超出解析深度"（实测 ${rep.cons.deep}）`);
  assert.equal(rep.perCarrier[0].deep, rep.cons.deep);
  // ★ 证据必须完整：每一处"超出解析深度"都要打得出来（⛔ 不许只给个数字）
  assert.equal(rep.perCarrier[0].deepEAs.length, rep.perCarrier[0].deep, 'deepEAs 必须逐处列出（数字与清单要一致）');
});

test('★ 第二批函数（6 个）：残差如实报出 · 带修饰符的调用约定不被漏（回归钉）', { skip }, () => {
  const m = clone();
  m.carriers = ['sub_40FB60', 'sub_411900', 'sub_412290', 'sub_418E80', 'sub_418EB0', 'sub_418ED0']
    .map((fn, i) => ({ id: `b0${i + 1}`, kind: 'arg', fn, reg: 'ecx', type: 'T0*' }));
  const rep = run(m);
  assert.deepEqual(rep.findings, [], '这一批只该有残差，不该有红');
  assert.equal(rep.perCarrier.length, 6);
  // ★ 回归钉：`void __thiscall __noreturn sub_412290(int this)` —— 约定与函数名之间的**修饰符**
  //   曾让 `objRootRegs` 抓不到名字 ⇒ 该载体"访问 0 处"却能通过（**假绿**）。
  const b03 = rep.perCarrier.find((p) => p.c.fn === 'sub_412290');
  assert.ok(b03, 'sub_412290 必须在报告里（不在 = 又被静默丢了）');
  assert.ok(b03.total > 100, `sub_412290 以 ecx 为根有大量 obj0 访问，实际 ${b03.total}（0 就是假绿）`);
  assert.ok(rep.cons.pointeeBad > 0, '这一批含"二级对象宿主未声明"的访问 ⇒ 必须进残差');
});
