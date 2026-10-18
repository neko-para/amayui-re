/** @env assets @kind gate @why 红了说明"两档都要报"的口径丢了：未解析基址会被误读成模型缺口（实测 sub_41BF50：29/64 只是 call 杀了 caller-saved） */
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { DEFAULT_OUT_DIR } from '../lib/paths.mjs';
import { main } from '../shape.mjs';

const hasCorpus = fs.existsSync(DEFAULT_OUT_DIR) && fs.readdirSync(DEFAULT_OUT_DIR).some((f) => f.endsWith('.lst'));
const skip = hasCorpus ? false : '缺解压语料（corpus/disasm/files/）';

/** 在进程内截获 CLI 的 stdout（沙箱里 spawn 捕获管道会 EPERM，所以不能用子进程） */
async function capture(argv) {
  const chunks = [];
  const orig = process.stdout.write.bind(process.stdout);
  process.stdout.write = (s) => { chunks.push(String(s)); return true; };
  try { await main(argv); } finally { process.stdout.write = orig; }
  return chunks.join('');
}

test('coverage 必须**同时报两档**未解析基址（保守 / 校准），且校准档不得更差', { skip }, async () => {
  const out = await capture(['--coverage', 'sub_41BF50']);
  assert.match(out, /未解析基址：保守档 \d+ · 校准档 \d+/, `缺两档对照行：\n${out.slice(0, 600)}`);
  const m = /未解析基址：保守档 (\d+) · 校准档 (\d+)/.exec(out);
  assert.ok(Number(m[2]) <= Number(m[1]), `校准档不该比保守档更多未解析：${m[0]}`);
  assert.match(out, /L0/, '必须有 L0 判据行');
});

test('access 支持 --calibrated（同一批站点，未解析基址不该更多）', { skip }, async () => {
  const cons = await capture(['--access', 'sub_41BF50']);
  const cal = await capture(['--access', 'sub_41BF50', '--calibrated']);
  const count = (s) => (s.match(/⚠ base/g) ?? []).length;
  assert.ok(count(cal) <= count(cons), `校准档的 ⚠base 数应 ≤ 保守档：${count(cal)} vs ${count(cons)}`);
});

test('coverage --json 归档自洽，且"目标未知的间接跳转"不许被静默算成闭合', { skip }, async () => {
  const out = await capture(['--coverage', 'sub_41BF50', 'sub_42C5E0', '--json']);
  const a = JSON.parse(out.slice(out.indexOf('{')));
  assert.equal(a.total, 2, `归档里应有 2 个函数：\n${out.slice(0, 400)}`);
  for (const f of a.functions) {
    if (f.closed) assert.deepEqual(f.reasons, [], `${f.sym} 闭合却带原因`);
    else assert.ok(f.reasons.length > 0, `${f.sym} 未闭合却没有原因（归档会把它读成"能用"）`);
  }
  // ★ 这条是"跳表"的代表：`jmp ds:…` 的目标集没导出 ⇒ 必须留在未闭合里（⛔ 不许静默变绿）
  const j = a.functions.find((f) => f.sym === 'sub_41BF50');
  assert.ok(j && j.closed === false, `sub_41BF50 有目标未知的间接跳转 ⇒ 必须未闭合：${JSON.stringify(j)}`);
  assert.ok(j.reasonKinds.includes('indirectJmp'), `原因里要有 indirectJmp：${JSON.stringify(j.reasonKinds)}`);
  assert.ok(a.byReason.indirectJmp.includes('sub_41BF50'), '归档分组里要能查到它（"下一层能用的清单"就是这样被消费的）');
});
