/** @env pure @kind gate @why 发布链默认又去重烧 AGF（要 headless Chrome）⇒ "没有浏览器就发不出包"，且进包字节变成"这次烧出来的"而不是固化下来的那一版 */
/**
 * tools/test/release-agf-source.test.mjs —— **AGF 进包来源**的守卫
 *
 * 它钉住一条口径（`release/README.md` §3）：**默认用入库件，不重烧**。为什么值得一条守卫：
 * * 重烧要 headless Chrome（`tools/ui-bake.md` §5.1）⇒ 把**默认路径**绑在浏览器上，等于"没有浏览器就发不出包"；
 * * 更隐蔽的后果是**字节不可复现**：同一个配方在不同 Chrome/字体环境下烧出来的字节不同，
 *   而"发出去的那一版"必须是固定的 —— 所以默认读固化下来的 `corpus/assets/ui-agf/*.AGF`。
 *
 * 运行：`pnpm test`（`@env pure`：临时目录 + 注入的 ctx，不烧图、不读游戏）
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

import { DEFAULT_AGF_DIR, DEFAULT_BAKED_DIR, collectAgfs, loadContext } from '../lib/release.mjs';

const tmp = () => fs.mkdtempSync(path.join(os.tmpdir(), 'amayui-agf-'));
const put = (dir, block, bytes) => fs.writeFileSync(path.join(dir, `${block}.AGF`), Buffer.from(bytes));

test('★ 缺省 AGF 目录 = 入库件（不是就地烧出来的那份）', () => {
  const ctx = loadContext();
  assert.equal(ctx.agfDir, DEFAULT_AGF_DIR, '缺省必须是入库件目录');
  assert.equal(DEFAULT_AGF_DIR, path.join(process.cwd(), 'corpus', 'assets', 'ui-agf'));
  assert.notEqual(DEFAULT_AGF_DIR, DEFAULT_BAKED_DIR, '入库件与 dist/ui-bake 是两个不同的目录');
  // 库里确实有 10 张（集合与配方一致，见下一条）
  const have = fs.readdirSync(DEFAULT_AGF_DIR).filter((f) => f.endsWith('.AGF')).sort();
  assert.deepEqual(
    have,
    ['SO001.AGF', 'SO002.AGF', 'SO009A.AGF', 'SO009B.AGF', 'SO017.AGF', 'SO020.AGF', 'SO021.AGF', 'SO025.AGF', 'SO030.AGF', 'SO039.AGF'],
    'v1.14 发布包里的 10 张 UI 图就是入库集',
  );
});

test('★ 字节从 agfDir 取（换个目录就换字节），缺件是 problem 且给出**不依赖浏览器**的修法', () => {
  const dir = tmp();
  put(dir, 'SO001', 'AAAA');
  const r = collectAgfs({ agfDir: dir, blocks: ['SO001', 'SO002'] });
  assert.equal(r.agfs.length, 1);
  assert.equal(r.agfs[0].block, 'SO001');
  assert.equal(r.agfs[0].buf.toString(), 'AAAA', '字节来自 agfDir');
  // ★ `file` 是"指回真源"的那一列：仓内给相对路径，仓外（临时目录）就给绝对路径 —— 只要指得到就行
  assert.ok(r.agfs[0].file.endsWith('SO001.AGF'), `file 要指回真源，实际 ${r.agfs[0].file}`);
  assert.equal(r.problems.length, 1, 'SO002 没有入库件 ⇒ problem');
  assert.match(r.problems[0], /AGF 缺件/);
  assert.match(r.problems[0], /--baked dist\/ui-bake/, '要给出"本次用 --baked"这条**不需要先固化**的绕法');
});

test('★ 两个集合都要对账：入库件有、配方没有 ⇒ warning（孤儿产物），不许静默带进包', () => {
  const dir = tmp();
  put(dir, 'SO001', 'A');
  put(dir, 'ZZZZ', 'B');
  // ★ 走**缺省集合**那条路（配方 ∪ 入库件）才看得到孤儿 —— `blocks` 是显式收窄，不参与对账
  const r = collectAgfs({ agfDir: dir, recipeBlocks: ['SO001'] });
  assert.deepEqual(r.agfs.map((a) => a.block), ['SO001', 'ZZZZ'], '孤儿的字节也在 agfs 里（由 warnings 提示人去核）');
  assert.equal(r.warnings.length, 1);
  assert.match(r.warnings[0], /ZZZZ\.AGF 在入库件里，却没有配方/);
});

test('★ sha256 逐件算出来（进包清单要指回真源），且两件不同字节不会混', () => {
  const dir = tmp();
  put(dir, 'SO001', 'AAAA');
  put(dir, 'SO002', 'BBBB');
  const r = collectAgfs({ agfDir: dir, blocks: ['SO001', 'SO002'] });
  assert.equal(r.agfs.length, 2);
  assert.notEqual(r.agfs[0].sha256, r.agfs[1].sha256);
  assert.match(r.agfs[0].sha256, /^[0-9a-f]{64}$/);
});
