/** @env pure @kind gate @why 段落划分的判据被改坏（init 不再开段 / 类别切换不再切 / caller-saved 不再标"被调用打断"），或两道守卫被拆掉 */
/**
 * tools/test/shape-regions.test.mjs —— 段落导出（校验器功能①）与两道守卫的**纯函数**守卫
 *
 * 为什么是 `pure`：本文件喂的是**合成 `.lst`**（不读语料）—— 段落划分与守卫都是纯逻辑，
 * 所以它们能在默认 `pnpm test` 里跑，不依赖本机语料是否解压。
 * （真语料上的义务检查在 `shape-claims.assets.test.mjs`。）
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { redundantKeys, vocabViolations } from '../lib/shape-claims.mjs';
import { deriveRegions } from '../lib/shape-regions.mjs';

/** 合成一份最小 `.lst`（行文法与真语料一致：`段:EA 助记符 操作数`） */
const L = [
  '.text:00401000 sub_401000 proc near',
  '.text:00401000 mov     esi, ecx',
  '.text:00401002 xor     edi, edi',            // ← init 开段（第 1 段）
  '.text:00401004 mov     eax, [esi+5D880h]',
  '.text:0040100A inc     edi',                 // 同类 → 并入第 1 段
  '.text:0040100B cmp     edi, 3',
  '.text:0040100E jl      short loc_401004',
  '.text:00401010 mov     eax, [esi+5D8F4h]',
  '.text:00401016 lea     edi, [esi+5D6E0h]',   // ← 类别变（ptr）⇒ 必须切（第 2 段）
  '.text:0040101C xor     edi, edi',            // ← init ⇒ 必须切（第 3 段）
  '.text:0040101E inc     edi',
  '.text:0040101F call    sub_999999',          // ← caller-saved 才有意义；edi 是 callee-saved
  '.text:00401024 retn',
  '.text:00401024 sub_401000 endp',
  '',
  '.text:00402000 sub_402000 proc near',
  '.text:00402000 xor     eax, eax',            // ← eax 是 caller-saved
  '.text:00402002 inc     eax',
  '.text:00402003 call    sub_999999',          // ⇒ 该段必须标 brokenByCall
  '.text:00402008 retn',
  '.text:00402008 sub_402000 endp',
];

test('段落划分：init 开段 · 类别切换切段 · 段内类别单一', () => {
  const d = deriveRegions(L, 'sub_401000', 'edi');
  assert.equal(d.writes.length, 5, '写点：xor / inc / lea / xor / inc');
  assert.deepEqual(d.regions.map((r) => [r.cat, r.openedByInit]), [['const', true], ['ptr', false], ['const', true]]);
  assert.equal(d.regions[0].allConstOrInit, true, 'init + inc 是干净推进链');
  assert.equal(d.regions[1].allConstOrInit, false, 'lea 不是 ±const ⇒ 不能声明成推进链');
  assert.equal(d.regions[0].from, 0x401002);
  assert.equal(d.regions[0].to, 0x40100a);
});

test('caller-saved 的段落跨越 `call` ⇒ 必须标 brokenByCall（ABI 事实，不是风格）', () => {
  assert.equal(deriveRegions(L, 'sub_402000', 'eax').regions[0].brokenByCall, true);
  assert.equal(deriveRegions(L, 'sub_401000', 'edi').regions[2].brokenByCall, false, 'callee-saved 跨 call 不受影响');
});

test('退出路径上的 `pop` 不算一个段落', () => {
  const d = deriveRegions([
    '.text:00403000 sub_403000 proc near',
    '.text:00403000 xor     edi, edi',
    '.text:00403002 pop     edi',
    '.text:00403003 mov     esp, ebp',
    '.text:00403005 pop     ebp',
    '.text:00403006 retn',
    '.text:00403006 sub_403000 endp',
  ], 'sub_403000', 'edi');
  assert.equal(d.writes.length, 1, '只应剩 init；`pop`（后面紧跟收尾）属于退出路径');
});

test('★ 词汇守卫：业务词（opcode / argc / 操作数 …）出现在模型里 ⇒ 必须报出来', () => {
  assert.deepEqual(vocabViolations({ carriers: [{ fn: 'sub_1', type: 'T0*' }] }), []);
  const bad = vocabViolations({ carriers: [{ fn: 'sub_1', argc: 3 }], note: 'lengthWord' });
  assert.ok(bad.includes('argc') && bad.includes('lengthWord'), `应报出 argc 与 lengthWord，实际 ${JSON.stringify(bad)}`);
});

test('★ 冗余守卫：能从 EA + 语料机械读出的字段名不允许出现（它们是第二份真源）', () => {
  assert.deepEqual(redundantKeys({ carriers: [{ fn: 'sub_1', reg: 'ecx' }] }), []);
  const bad = redundantKeys({ carriers: [{ fn: 'sub_1', mn: 'add', callee: 'sub_2' }] });
  assert.deepEqual(bad.sort(), ['callee', 'mn']);
});
