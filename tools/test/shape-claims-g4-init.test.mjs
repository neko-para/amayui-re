/** @env pure @kind gate @why G4 的"初始化写循环"档一旦算错（漏认 / 把两段无关代码拼起来 / 认错条数），要么把有直接观察的数组判成"条数无依据"（假红），要么给没观察的数组开口子（假绿） */
/**
 * tools/test/shape-claims-g4-init.test.mjs —— `initStompCounts()` 的口径守卫（纯函数，合成 `.lst` 片段）。
 *
 * 它守的是 G4 新增的那一档：`lea edi,[obj+OFFh]` → `mov ecx,N` → `rep stosd`
 * ⇒ "这 OFF 起被**初始化写循环**写过 N 格" = **条数有直接观察**（比"末尾被引用"更强）。
 * 现场：`0x415dca`（`T_Command` 构造函数）对 `+0x77bec` 就是 `mov ecx,200h` + `rep stosd`。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { initStompCounts } from '../lib/shape-claims.mjs';

const L = (ea, text) => `.text:${ea} ${text}`;

test('★g4init-01 认得出 `lea edi,[reg+OFFh]` + `mov ecx,N` + `rep stosd` ⇒ OFF → N', () => {
  const m = initStompCounts([
    L('00415DCA', 'lea     edi, [esi+77BECh]'),
    L('00415DD0', 'mov     ecx, 200h'),
    L('00415DD5', 'rep stosd'),
  ]);
  assert.deepEqual([...(m.get(0x77bec) ?? [])], [0x200]);
});

test('★g4init-02 中间夹 `xor eax,eax` / `mov eax,…` 仍要认（构造函数的常见形态）', () => {
  const m = initStompCounts([
    L('00415DA1', 'xor     eax, eax'),
    L('00415DA3', 'lea     edi, [esi+7915Ch]'),
    L('00415DA9', 'mov     ecx, 29h'),
    L('00415DAE', 'rep stosd'),
  ]);
  assert.deepEqual([...(m.get(0x7915c) ?? [])], [0x29]);
});

test('★g4init-03 被打断的两段**不许**拼成一个条数（否则会给没观察的数组开口子）', () => {
  const m = initStompCounts([
    L('00402000', 'lea     edi, [esi+1234h]'),
    L('00402005', 'call    sub_405000'),      // 打断
    L('0040200A', 'mov     ecx, 10h'),
    L('0040200F', 'rep stosd'),
  ]);
  assert.equal(m.has(0x1234), false, `被打断就不该入账：${JSON.stringify([...m])}`);
});

test('★g4init-04 没有 `mov ecx,N` 的 `rep stosd`（条数在别处）不许编一个数字', () => {
  const m = initStompCounts([
    L('00402010', 'lea     edi, [esi+2345h]'),
    L('00402015', 'rep stosd'),
  ]);
  assert.equal(m.has(0x2345), false);
});

test('★g4init-05 同一偏移被写多次 ⇒ 收集**全部**次数（G4 按"声明值 ∈ 集合"判）', () => {
  const m = initStompCounts([
    L('00402020', 'lea     edi, [esi+3456h]'),
    L('00402026', 'mov     ecx, 4h'),
    L('0040202B', 'rep stosd'),
    L('00402030', 'lea     edi, [esi+3456h]'),
    L('00402036', 'mov     ecx, 8h'),
    L('0040203B', 'rep stosd'),
  ]);
  assert.deepEqual([...(m.get(0x3456) ?? [])].sort((a, b) => a - b), [4, 8]);
});
