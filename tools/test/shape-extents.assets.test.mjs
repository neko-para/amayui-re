/** @env assets @kind contract @why 块搬移 / 清零的宽度证据退化（rep 计数解不出 / 十六进制立即数解析退回旧正则 / memcpy 参数序取错）⇒ 1200B · 20000B · 740B · 572B 这几处宽度消失，而"缺宽度"只会表现成报告里一行"判不了" */
/**
 * tools/test/shape-extents.assets.test.mjs —— **块搬移 / 清零的 extent**（T3）在真语料上的守卫
 *
 * 判据（红了意味着什么）：
 *  ① `sub_41ED80` 的两处：`rep movsd` = **1200 B**（`0x12C × 4`，dst/src 都是 obj0 相对）、
 *     `_memcpy` = **20000 B**（`0x4E20`）；
 *  ② `sub_4AC0D0` 的两处：740 B（`0xB9 × 4`，绘制项记录整份复制）与 572 B（`143 × 4`，值表整份复制）。
 *
 * ★ 为什么钉这几处：这些对象**一条字段访问都没有**，唯一的 width 证据就是"整块搬 N 字节"。
 *   1200 B 那一处还兼作**十六进制立即数**的回归钉：IDA 对 `12Ch` **不补前导 0**，
 *   旧正则 `^0(…)h$` 会把它判成未知 ⇒ 计数解不出 ⇒ 宽度消失（且不报错）。
 * ★ ⛔ 这里**不**断言"这段是某个数组"：extent 只给宽度，不判成员、不放宽 G4。
 *
 * 要语料（`corpus/disasm/files/`）；缺席时 `t.skip`。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { analyzeLines, functionSpans, loadListing, objRootRegs } from '../lib/shape-access.mjs';
import { DEFAULT_OUT_DIR, REPO_ROOT } from '../lib/paths.mjs';

const hasCorpus = fs.existsSync(DEFAULT_OUT_DIR) && fs.readdirSync(DEFAULT_OUT_DIR).some((f) => f.endsWith('.lst'));
const skip = hasCorpus ? false : `语料未解压（先 \`pnpm tools disasm build\`）—— 期望目录 ${path.relative(REPO_ROOT, DEFAULT_OUT_DIR)}`;

const ctx = (() => {
  if (!hasCorpus) return null;
  const lines = loadListing();
  return { lines, spans: new Map(functionSpans(lines).map((s) => [s.sym, s])), roots: objRootRegs() };
})();

const extentsOf = (sym) => {
  const sp = ctx.spans.get(sym);
  assert.ok(sp, `语料里应有 ${sym}`);
  return analyzeLines(ctx.lines, sp.from, sp.to, ctx.roots.get(sym) ?? null, {}).extents;
};

test('★ T3 块搬移的宽度：1200B（rep movsd）· 20000B（memcpy）· 740B / 572B（记录整份复制）', { skip }, () => {
  const e1 = extentsOf('sub_41ED80');
  const mv = e1.find((e) => e.ea === 0x41edfc);
  assert.ok(mv, `0x41edfc 的 rep movsd 必须被记成 extent：${JSON.stringify(e1.map((e) => e.ea.toString(16)))}`);
  assert.equal(mv.bytes, 0x4b0, `0x12C × 4 = 1200 B（实得 ${mv.bytes}；⇒ 十六进制立即数 / rep 计数解析退化了）`);
  assert.equal(mv.dst?.abs, 0x440c, '目的 = Engine+0x440C（快照）');
  assert.equal(mv.src?.abs, 0x3f5c, '源 = Engine+0x3F5C（活动表）');
  const mc = e1.find((e) => e.ea === 0x41ee06);
  assert.ok(mc, '0x41ee06 的 _memcpy 必须被记成 extent');
  assert.equal(mc.bytes, 20000, `0x4E20 = 20000 B（实得 ${mc.bytes}）`);
  assert.equal(mc.dst?.abs, 0x54278, '目的 = Engine+0x54278');

  const e2 = extentsOf('sub_4AC0D0');
  const rec = e2.find((e) => e.ea === 0x4ac127);
  assert.ok(rec, '0x4AC127 的记录整份复制必须被记成 extent');
  assert.equal(rec.bytes, 740, `0xB9 × 4 = 740 B（实得 ${rec.bytes}）`);
  const val = e2.find((e) => e.ea === 0x4ac3ec);
  assert.ok(val, '0x4AC3EC 的值表整份复制必须被记成 extent');
  assert.equal(val.bytes, 572, `143 × 4 = 572 B（实得 ${val.bytes}）`);
});
