/** @env pure @kind gate @why 欠账三档（已声明 / `count:null` 窗口内 / 未声明）算错时，"覆盖率"会把"判不了"读成"已覆盖"、或把已声明的成员当欠账 —— 于是下一轮补的是**不存在的成员** */
/**
 * tools/test/shape-census-debt.test.mjs —— `obj0Debt()` 的算术守卫（纯函数，不读语料）。
 *
 * 为什么单独立守卫：这个函数输出的**就是**"还要补哪些成员"的清单。
 * 三类错误都会让它变成**假清单**：
 * ① 把 `count: null` 的无界窗口算成"已声明" ⇒ 欠账凭空消失（S1g 里 `0x5ee04` 一路解释到 `0xa609c` 那类）；
 * ② 把窗口算成闭区间（`abs == 末尾` 也算命中）⇒ 真欠账被吞掉一格；
 * ③ 类型名写错却静默返回空表 ⇒ "0 个未声明"看起来像全绿。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { obj0Debt, renderDebt } from '../lib/shape-census.mjs';

const types = {
  types: {
    T_Root: {
      complete: false,
      members: [
        { off: '0x0', width: 4 },
        { off: '0x8', width: 4 },
        { off: '0x100', type: 'T_Elem', count: 2 },   // 有限窗口：0x100..0x110
        { off: '0x200', type: 'T_Elem', count: null, sweep: { stride: 4, index: 'cur' } }, // 无界窗口
      ],
    },
    T_Elem: { size: 8, complete: true, members: [{ off: '0x0', width: 4 }, { off: '0x4', width: 4 }] },
  },
};

const off = (abs, sites) => ({ abs, sites, handlers: 1, sampleEAs: [0x400000 + abs] });

test('★debt-01 三档不许混：成员本身 / 有限窗口内 = 已声明；`count:null` 窗口内 = 判不了；其余 = 欠账', () => {
  const d = obj0Debt([
    off(0x0, 3), off(0x8, 1),          // 标量成员
    off(0x104, 2),                     // 有限窗口内（元素 0 的第二个字段）
    off(0x110, 5),                     // 窗口外的下一格 ⇒ **欠账**（闭区间会把它吞掉）
    off(0x204, 4),                     // count:null 窗口内 ⇒ 判不了
    off(0x40, 7),                      // 谁都不沾 ⇒ 欠账
  ], types, 'T_Root');
  assert.equal(d.counts.declared, 3, `已声明 3 个：${JSON.stringify(d.declared.map((o) => o.abs))}`);
  assert.equal(d.counts.openWindow, 1, `count:null 窗口内 1 个：${JSON.stringify(d.openWindow.map((o) => o.abs))}`);
  assert.equal(d.counts.undeclared, 2, `未声明 2 个：${JSON.stringify(d.undeclared.map((o) => o.abs))}`);
  // ★ 欠账按站点数降序（"下一轮先补哪个"就是这个顺序）
  assert.deepEqual(d.undeclared.map((o) => o.abs), [0x40, 0x110]);
  assert.deepEqual(d.openBases, [0x200]);
});

test('★debt-02 `count:null` 窗口**无界**：基点本身算已声明，基点上方的偏移都进"判不了"（⛔ 不许算成"已声明"）', () => {
  const d = obj0Debt([off(0x200, 1), off(0x204, 3), off(0xaabc0, 2)], types, 'T_Root');
  assert.deepEqual(d.declared.map((o) => o.abs), [0x200], '基点本身是声明过的成员');
  assert.equal(d.counts.openWindow, 2, '基点上方的偏移进"判不了"（否则欠账凭空消失）');
  assert.equal(d.counts.undeclared, 0);
});

test('★debt-03 类型名写错必须**当场报错**，不许静默返回空表（"0 个未声明"会被读成全绿）', () => {
  assert.throws(() => obj0Debt([off(0x0, 1)], types, 'T0'), /types 里没有类型 T0/);
});

test('★debt-04 报告里三档的数字与清单同源（渲染不许自己再算一遍）', () => {
  const d = obj0Debt([off(0x0, 2), off(0x180, 6)], types, 'T_Root');
  const text = renderDebt(d);
  assert.match(text, /未声明.*1 个偏移 ·\s+6 处站点/, text);
  assert.match(text, /0x180/, '欠账清单要带 witness EA 行');
});
