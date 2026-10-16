/** @env assets @kind contract @why 真语料上的 item 目录契约被破坏：两侧关联错位（原型被当成定义 / 修饰名对不上）、覆盖率跌破前提、缺 C 被静默吞掉 */
/**
 * tools/test/disasm-fn.assets.test.mjs —— **item 目录**在**真语料**上的契约
 *
 * 合成的文法判据在 `disasm-fn-format.test.mjs`（`@env pure`）；这一档只放"必须用真语料才验得了"的东西：
 * * **join 往返**：`.lst` 的符号 ↔ `.c` 的定义**不许错配**（错配的表现是"C 体读起来很正常"）；
 * * **覆盖率**：「用 C 读逻辑」这个前提还成不成立；
 * * **三个具体已知点**（都是实测踩出来的，写在这里当回归钉）：
 *   1. `sub_40B6C0` —— 旧判据把它的**带尾注释原型**当成定义，行车区间落在文件顶部的原型区（一行"C 体"）；
 *   2. `sub_4D13F0` —— 名字里带 `@<eax>`（`__userpurge`）⇒ 旧判据**整条漏掉**它（表现为"没有 C"）；
 *   3. `_WinMain@16` —— MSVC 修饰名 ⇒ 去壳后必须能对到 `.c` 里的 `WinMain`（**新增**能力：旧实现只索引 `sub_`）。
 *
 * 运行：`pnpm test:assets`（要语料；语料不在场 ⇒ 如实 skip）
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';
import { buildCatalog, findFunction, impact, whoTouchesGlobal } from '../lib/catalog.mjs';
import { undecorate } from '../lib/lst-format.mjs';

const FILES_DIR = path.join(REPO_ROOT, 'corpus', 'disasm', 'files');
const LST = fs.existsSync(FILES_DIR)
  ? (fs.readdirSync(FILES_DIR).filter((f) => f.endsWith('.lst')).sort().map((f) => path.join(FILES_DIR, f))[0] ?? null)
  : null;
const skip = LST ? false : '语料未解压（先 `pnpm tools disasm build` 或解 zip）';
const cat = LST ? buildCatalog({ lstFile: LST }) : null;

test('★ 目录**自检全过**：区间不重叠 / C 侧不落在原型区 / EA 单调 / 全局索引双向一致', { skip }, () => {
  assert.deepEqual(cat.problems, [], `catalogProblems 必须为空，实际：\n${cat.problems.slice(0, 10).join('\n')}`);
});

test('★ C 层必须覆盖**绝大多数**函数 —— 否则"用 C 读逻辑"这个前提不成立', { skip }, () => {
  assert.ok(cat.stats.functions > 1000, `函数数量应当是几千量级，实际 ${cat.stats.functions}`);
  const cov = cat.stats.withC / cat.stats.functions;
  assert.ok(cov > 0.95, `★ C 覆盖率 ${(cov * 100).toFixed(1)}% —— 低于 95% 就该重新评估"两层工作流"是否还成立`);
  assert.ok(cat.stats.withoutC > 0, '★ 但总会有函数没有 C（Hex-Rays 没反编译）—— 那不是"没有逻辑"，必须报出来而不是补一个空体');
});

test('★ join 往返：`.c` 定义的名字必须能回到它被关联的那个 `.lst` 符号（不许错配）', { skip }, () => {
  let checked = 0;
  for (const f of cat.functions) {
    if (!f.hasC) continue;
    checked += 1;
    assert.ok(
      f.c.name === f.sym || undecorate(f.sym) === f.c.name,
      `★ ${f.sym} 关联到了 .c 的 \`${f.c.name}\` —— 两边名字对不上就说明 join 错配了`,
    );
  }
  assert.ok(checked > 1000, `应当检查了上千个函数，实际 ${checked}`);
});

test('★ `sub_40B6C0`：C 区间**不许**是那条带尾注释的原型（旧判据在这里给出一行"C 体"且不报错）', { skip }, () => {
  const f = findFunction(cat, { sym: 'sub_40B6C0' }).item;
  assert.ok(f.hasC, '它有真定义 ⇒ 必须有 C');
  assert.ok(f.c.from > cat.c.lastProtoLine, `C 起点 ${f.c.from} 必须**在原型区之后**（原型区到 ${cat.c.lastProtoLine}）`);
  assert.ok(f.c.to > f.c.from, `C 区间必须真的跨行（实际 ${f.c.from}-${f.c.to}）`);
  const head = fs.readFileSync(cat.cFile, 'utf8').split('\n')[f.c.from - 1];
  assert.match(head, /^int __stdcall sub_40B6C0\(/, `C 体第一行必须是定义签名，实际 ${JSON.stringify(head)}`);
  assert.ok(!head.trimEnd().endsWith(';'), '★ 第一行**不许**是原型（带 `); // idb` 那种）');
});

test('★ `sub_4D13F0`：名字里的 `@<eax>`（`__userpurge`）必须被剥掉 —— 否则定义对不上，表现为"没有 C"', { skip }, () => {
  const f = findFunction(cat, { sym: 'sub_4D13F0' }).item;
  assert.ok(f.hasC, '★ 它有 C（旧实现因为 `sub_4D13F0@<eax>(` 里的 `@` 而整条漏掉它）');
  assert.equal(f.c.name, 'sub_4D13F0', '`@<eax>` 是**约定标注**，不是名字的一部分');
  assert.equal(f.conv, '__userpurge');
  const regs = f.args.filter((a) => a.reg).map((a) => a.reg);
  assert.ok(regs.length >= 2, `★ 参数上的 \`@<reg>\` 必须被认出来（实际 ${regs.join(' / ')}）`);
  assert.match(String(f.convNote), /userpurge/, 'usercall/userpurge 必须有约定注记（偏移不可机械判定的那部分要明说）');
});

test('★ `_WinMain@16`：MSVC 修饰名去壳后必须对到 `.c` 的 `WinMain`，且参数**按名字**配上', { skip }, () => {
  const f = findFunction(cat, { sym: '_WinMain@16' }).item;
  assert.ok(f.hasC, '★ 具名函数也要能被关联（旧实现只索引 `sub_XXXXXX` ⇒ 它一律"没有 C"）');
  assert.equal(f.c.name, 'WinMain');
  assert.match(fs.readFileSync(cat.cFile, 'utf8').split('\n')[f.c.from - 1], /^int __stdcall WinMain\(/);
  assert.deepEqual(
    f.args.map((a) => [a.name, a.offset, a.pairBy]),
    [['hInstance', 8, 'name'], ['hPrevInstance', 12, 'name'], ['lpCmdLine', 16, 'name'], ['nShowCmd', 20, 'name']],
    '★ 两边名字逐字相同 ⇒ 必须**按名字**配对（顺序法在这里也能对，但依据不同，`pairBy` 要如实标）',
  );
});

test('★ 反向索引可往返：`--who <全局>` 列出的每个函数，自己的全局表里必须有它', { skip }, () => {
  const name = [...cat.globalIndex.keys()].sort((a, b) => cat.globalIndex.get(b).length - cat.globalIndex.get(a).length)[0];
  const w = whoTouchesGlobal(cat, { name });
  assert.ok(w.users.length > 1, `${name} 应当被多个函数引用（实际 ${w.users.length}）`);
  for (const u of w.users) {
    const f = cat.bySym.get(u.sym);
    assert.ok(f, `反向索引里的 ${u.sym} 必须在目录里`);
    assert.ok(f.globals.some((g) => g.name === name), `★ ${u.sym} 的全局表里必须有 ${name}（反向索引与正向表要一致）`);
  }
  // 自动名（名字里编码 EA）也必须能落到**包含它的数据 item**，而不是"查不到"
  const auto = cat.data.find((d) => d.name.startsWith('off_'));
  const w2 = whoTouchesGlobal(cat, { name: auto.name });
  assert.equal(w2.name !== null, true);
});

test('★ 影响面必带**下界说明**；取址（动态分派）的候选调用方必须另册报出', { skip }, () => {
  // `sub_42CA50`：实测它是**跳转表 / 函数指针**分派到的（没有直接 `call`）
  const f = findFunction(cat, { sym: 'sub_42CA50' }).item;
  const up = impact(cat, f, { direction: 'up', depth: 2 });
  assert.ok(Array.isArray(up.honesty.addrEvidence), 'honesty.addrEvidence 必须是数组');
  assert.ok(up.honesty.addrEvidence.length > 0, '★ 它有取址证据 ⇒ 必须报出来（否则"0 个调用方"会被读成"没人用它"）');
  assert.equal(typeof up.honesty.indirectInCorpus, 'number');
  assert.ok(up.honesty.indirectInCorpus > 0, '全语料的间接调用点数必须 > 0（那是"闭包只是下界"的理由）');
  const down = impact(cat, f, { direction: 'down', depth: 2 });
  assert.ok(down.nodes.length > 0, '它至少依赖几个函数（正向闭包不该是空的）');
});
