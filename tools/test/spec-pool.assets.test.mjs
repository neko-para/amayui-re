/** @env assets @kind gate @why 立即数标签（0/1/2）的"只读不写"结构或读原语的 case 集与语料不符（读者会把立即数当池下标，或给写原语编一个语义） */
/**
 * tools/test/spec-pool.assets.test.mjs —— **CORE-POOL 立即数标签**的语料侧守卫
 *
 * 支撑 `data/spec/chapters/40-pools.md` 的 `CORE-POOL-TAG-IMMEDIATE`：
 * ① 读整 / 读浮的跳转表里 case 0 / 1 / 2 是**真 case**（不是 default）；
 * ② 读整 / 读浮的 case 8 是 **default**（类型 8 没有读语义）；
 * ③ 写整 / 写浮的 case 集从 **3** 开始（0 / 1 / 2 不在写原语里）。
 *
 * ★ 它不断言 0/1/2 的**值语义**（那是条目正文与站点的事），只断言"读接受、写不接受"这个结构。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';
import { buildIndex, switchTables } from '../lib/disasm.mjs';

const FILES_DIR = path.join(REPO_ROOT, 'corpus', 'disasm', 'files');
const listing = (() => {
  if (!fs.existsSync(FILES_DIR)) return null;
  const lst = fs.readdirSync(FILES_DIR).filter((f) => f.endsWith('.lst')).sort();
  return lst.length ? path.join(FILES_DIR, lst[0]) : null;
})();
const skip = listing ? false : '语料未解压（先 `pnpm tools disasm build`）';

/** 跳转点前方 41 行的窗口（IDA 在注释里写着 case 数 / default / case 号） */
function window(lines, insnLine1, n = 41) {
  return lines.slice(Math.max(0, insnLine1 - n), insnLine1);
}

test('★ 读原语：case 0/1/2 是真 case，case 8 是 default（类型 8 没有读语义）', { skip }, () => {
  const idx = buildIndex(listing);
  const all = fs.readFileSync(listing, 'utf8').split('\n');
  for (const [label, ea] of [['读整型', 0x41bf50], ['读浮点', 0x41c300]]) {
    const t = switchTables(listing, idx, ea, { fileLines: all })[0];
    assert.ok(t, `${label} 应有一条跳转表`);
    const win = window(all, t.insnLine);
    assert.ok(win.some((l) => /;\s*switch\s+14\s+cases/.test(l)), `${label} 的 case 数应是 14`);
    for (const k of [0, 1, 2]) {
      assert.ok(!String(t.targets[k]?.loc ?? '').startsWith('def_'), `${label} 的 case ${k} 必须落到真 case，不能是 default`);
    }
    assert.ok(String(t.targets[8]?.loc ?? '').startsWith('def_'), `${label} 的 case 8 必须是 default（类型 8 没有读语义）`);
  }
});

/** 取一个函数（EA 起点）在清单里的行区间 */
function spanOf(lines, ea) {
  const h = ea.toString(16).toUpperCase().padStart(8, '0');
  const start = lines.findIndex((l) => l.includes(`:${h} `) && /proc near/.test(l));
  if (start < 0) return null;
  let end = start;
  while (end < lines.length && !/\bendp\b/.test(lines[end])) end += 1;
  return { start, end, body: lines.slice(start, end + 1) };
}

test('★ 句柄分支：0x8003/0x8009 在四个值原语里都有专支；0x8005/0x800B 只在取址里', { skip }, () => {
  const lines = fs.readFileSync(listing, 'utf8').split('\n');
  // 四个值原语：cmp type,8003h → jg(去看 0x8009) + jz(0x8003 专支)；后面的 cmp type,8009h → jz
  for (const [label, ea] of [['读整型', 0x41bf50], ['读浮点', 0x41c300], ['写整型', 0x42b4b0], ['写浮点', 0x42ba00]]) {
    const sp = spanOf(lines, ea);
    assert.ok(sp, `${label} 的函数区间找不到`);
    const i3 = sp.body.findIndex((l) => /cmp\s+\S+,\s*8003h\b/.test(l));
    assert.ok(i3 >= 0, `${label} 里没有 cmp …,8003h`);
    const next2 = sp.body.slice(i3 + 1, i3 + 3).join('\n');
    assert.match(next2, /\bjg\b/, `${label} 的 0x8003 比较后应有 jg（去 0x8009 分支）`);
    assert.match(next2, /\bjz\b/, `${label} 的 0x8003 比较后应有 jz（0x8003 专支）`);
    const i9 = sp.body.findIndex((l) => /cmp\s+\S+,\s*8009h\b/.test(l));
    assert.ok(i9 >= 0, `${label} 里没有 cmp …,8009h`);
    assert.match(sp.body[i9 + 1] ?? '', /\bjz\b/, `${label} 的 0x8009 比较后应有 jz`);
  }

  // 取址原语：sub ecx,8005h / sub ecx,4 / sub ecx,2 三步各自 jz（⇒ 0x8005 / 0x8009 / 0x800B）
  const addr = spanOf(lines, 0x42aea0);
  assert.ok(addr, '取址原语的函数区间找不到');
  const seq = addr.body.join('\n');
  for (const [pat, name] of [
    [/sub\s+ecx,\s*8005h[^\n]*\n[^\n]*jz/, '0x8005'],
    [/sub\s+ecx,\s*4[^\n]*\n[^\n]*jz/, '0x8009'],
    [/sub\s+ecx,\s*2[^\n]*\n[^\n]*jz/, '0x800B'],
  ]) {
    assert.match(seq, pat, `取址原语缺少 ${name} 的"减去常量再 jz"分支`);
  }
});

test('★ 字符串写入原语：容量字段 + 阈值 10h + 重叠路径', { skip }, () => {
  const lines = fs.readFileSync(listing, 'utf8').split('\n');
  const sp = spanOf(lines, 0x40c210);
  assert.ok(sp, '记录级写入原语 sub_40C210 的函数区间找不到');
  const body = sp.body.join('\n');
  const thr = (body.match(/cmp\s+\S+,\s*10h\b/g) || []).length;
  assert.ok(thr >= 2, `内联阈值比较应至少出现 2 次（容量与文本），实际 ${thr}`);
  const overlap = (body.match(/call\s+sub_40C120\b/g) || []).length;
  assert.ok(overlap >= 2, `重叠 / 扩容路径应被调用至少 2 次，实际 ${overlap}`);
  assert.match(body, /\[esi\+14h\]/, '容量字段（记录 +0x14）应被读');
});

test('★ 写原语：case 集从 3 开始（0/1/2 不在写原语里）', { skip }, () => {
  const idx = buildIndex(listing);
  const all = fs.readFileSync(listing, 'utf8').split('\n');
  for (const [label, ea, cases] of [['写整型', 0x42b4b0, 10], ['写浮点', 0x42ba00, 11]]) {
    const t = switchTables(listing, idx, ea, { fileLines: all })[0];
    assert.ok(t, `${label} 应有一条跳转表`);
    assert.equal(t.cases, cases, `${label} 的 case 数变了`);
    const win = window(all, t.insnLine);
    const start = win.find((l) => new RegExp(`sub\\s+\\S+,\\s*3\\s*;\\s*switch\\s+${cases}\\s+cases`).test(l));
    assert.ok(start, `${label} 的入口应是"减去 3 再比较"的形态（⇒ case 3..），语料里找不到`);
  }
});

test('★ 五原语入口：五个函数起点 + 表规模 12/14/14/10/11 + 类型在 [地址−4]', { skip }, () => {
  const lines = fs.readFileSync(listing, 'utf8').split('\n');
  const idx = buildIndex(listing);
  for (const [label, ea, cases] of [
    ['取址', 0x42aea0, 12],
    ['读整型', 0x41bf50, 14],
    ['读浮点', 0x41c300, 14],
    ['写整型', 0x42b4b0, 10],
    ['写浮点', 0x42ba00, 11],
  ]) {
    const sp = spanOf(lines, ea);
    assert.ok(sp, `${label} 的入口 0x${ea.toString(16)} 不是函数起点（proc near）`);
    const t = switchTables(listing, idx, ea, { fileLines: lines })[0];
    assert.ok(t, `${label} 应有一条跳转表`);
    assert.equal(t.cases, cases, `${label} 的 case 数变了：${t.cases} ≠ ${cases}`);
  }
  const l = lines.find((x) => x.startsWith('.text:0042AEF5 '));
  assert.match(l ?? '', /\[eax-4\]/, '类型标签应在 [操作数地址−4]（取址 0x42AEF5）');
});

test('★ 浮点跨族：浮→整走 __ftol2_sse，整→浮走 fild', { skip }, () => {
  const lines = fs.readFileSync(listing, 'utf8').split('\n');
  const body = (ea) => spanOf(lines, ea)?.body.join('\n') ?? '';
  assert.match(body(0x41bf50), /__ftol2_sse/, '读整型从浮点族取值的路径应调用 __ftol2_sse');
  assert.match(body(0x42ba00), /__ftol2_sse/, '写浮点到整型族的路径应调用 __ftol2_sse');
  assert.match(body(0x42b4b0), /\bfild\b/, '写整型到浮点族的路径应使用 fild');
  assert.match(body(0x41c300), /\bfild\b/, '读浮点从整型族取值的路径应使用 fild');
});
