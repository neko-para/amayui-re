/** @env pure @kind contract @why 0x216 的槽→文件绑定 / 0x23d 的 42..999 清绑定被改错时当场红 */
import test from 'node:test';
import assert from 'node:assert/strict';

function createBlank(table, slot) {
  table.set(slot, -1); // 0x1f8：空白创建清绑定
}
function setTexture(table, slot, file) {
  table.set(slot, file); // 0x1f9：成功设置纹理写文件号
}
function releaseTexture(table, slot) {
  table.set(slot, -1); // 0x1fa：释放清绑定
}
function clearMovieSlots(table) {
  for (const slot of [...table.keys()]) if (slot >= 42 && slot < 1000) table.set(slot, -1);
}

test('★ 0x216：缺省 -1；1f8/1f9/1fa 写槽→文件绑定', () => {
  const table = new Map();
  assert.equal(table.get(3) ?? -1, -1, '缺省 = 无绑定');
  createBlank(table, 3);
  assert.equal(table.get(3), -1);
  setTexture(table, 3, 1234);
  assert.equal(table.get(3), 1234, '成功设置纹理 ⇒ 文件号');
  releaseTexture(table, 3);
  assert.equal(table.get(3), -1, '释放 ⇒ 无绑定');
});

test('★ 0x23d：只清 42..999 的槽→文件绑定', () => {
  const table = new Map([[41, 7], [42, 8], [999, 9]]);
  clearMovieSlots(table);
  assert.equal(table.get(41), 7);
  assert.equal(table.get(42), -1);
  assert.equal(table.get(999), -1);
});
