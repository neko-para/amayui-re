/** @env pure @kind contract @why 0x12e 的命中判据 / 起始下标 / 未命中口径被改坏时当场红 */
import test from 'node:test';
import assert from 'node:assert/strict';

/**
 * `CORE-INPUT-HIT-TEST-12E` 的纯合同夹具。
 *
 * ★ 这不是引擎实现，也不冒充实现：它只把规格正文的四个不等式与「start+1 起扫、不环绕」
 *   固定成可复算的样本。C++ 实现由 `apps/emulator-cxx/tests/core_selftest.cpp` 另行守。
 */
function hitTest12e({ start, margin, x, y, boxes, planeX, planeY, count }) {
  if (count <= 0) return -1;
  for (let i = start + 1; i < count; i += 1) {
    const dx = x - planeX[i];
    const dy = y - planeY[i];
    const [s0, s1, s2, s3] = boxes[i];
    if (
      margin[1] + dx - s0 >= 0 &&
      margin[3] + dy - s2 >= 0 &&
      s1 - dx - margin[0] >= 0 &&
      s3 - dy - margin[2] >= 0
    ) {
      return i;
    }
  }
  return -1;
}

// TITLE 的五按钮命中夹具：框 [0,0x9c,0,0x9c]，两个基准平面与绘制坐标一一对应。
const titleFixture = {
  margin: [0, 0, 0, 0],
  boxes: Array.from({ length: 5 }, () => [0, 0x9c, 0, 0x9c]),
  planeX: [0x44e, 0x3e0, 0x365, 0x2d9, 0x453],
  planeY: [0x126, 0x192, 0x1e5, 0x21f, 0x22a],
  count: 5,
};

test('★ 0x12e 命中查找：TITLE 五框 / 起始下标 / 无命中', () => {
  // 第 2 项框内（0x365..0x365+0x9c, 0x1e5..0x1e5+0x9c）。
  assert.equal(hitTest12e({ ...titleFixture, start: -1, x: 0x3c0, y: 0x220 }), 2);
  // 全部框外。
  assert.equal(hitTest12e({ ...titleFixture, start: -1, x: 0, y: 0 }), -1);
  // start=2 ⇒ 从第 3 项起扫：光标仍在第 2 项框内也必须找不到。
  assert.equal(hitTest12e({ ...titleFixture, start: 2, x: 0x3c0, y: 0x220 }), -1);
  // start=1 ⇒ 从第 2 项起扫：第 1 项框内也必须找不到。
  assert.equal(hitTest12e({ ...titleFixture, start: 1, x: 0x460, y: 0x160 }), -1);
  // count=0 ⇒ 不扫，直接 -1。
  assert.equal(hitTest12e({ ...titleFixture, count: 0, start: -1, x: 0x3c0, y: 0x220 }), -1);
});

test('★ 0x12e 补偿四元组：四个边界都按 m 扩 / 缩', () => {
  const boxes = [[10, 20, 30, 40]];
  const base = { start: -1, boxes, planeX: [0], planeY: [0], count: 1 };
  // 全 0 margin：dx ∈ [10,20]、dy ∈ [30,40]。
  assert.equal(hitTest12e({ ...base, margin: [0, 0, 0, 0], x: 10, y: 30 }), 0);
  assert.equal(hitTest12e({ ...base, margin: [0, 0, 0, 0], x: 9, y: 30 }), -1);
  // m1 +1 ⇒ 左边界 10−1=9；m0 +1 ⇒ 右边界 20−1=19。
  assert.equal(hitTest12e({ ...base, margin: [0, 1, 0, 0], x: 9, y: 30 }), 0);
  assert.equal(hitTest12e({ ...base, margin: [1, 0, 0, 0], x: 20, y: 30 }), -1);
  // m3 +1 ⇒ 上边界 30−1=29；m2 +1 ⇒ 下边界 40−1=39。
  assert.equal(hitTest12e({ ...base, margin: [0, 0, 0, 1], x: 10, y: 29 }), 0);
  assert.equal(hitTest12e({ ...base, margin: [0, 0, 1, 0], x: 10, y: 40 }), -1);
});
