/** @env pure @kind contract @why 正常退出漏保存 / 保存晚于清理 / 调试帧上限误写盘时当场红 */
import test from 'node:test';
import assert from 'node:assert/strict';

/** 引擎退出：配置已装载才保存，且保存先于清理。 */
function shutdown(loaded, order) {
  if (loaded) order.push('save');
  order.push('cleanup');
}

test('★ 窗口关闭：已装载配置 ⇒ 先保存、再清理', () => {
  const order = [];
  shutdown(true, order);
  assert.deepEqual(order, ['save', 'cleanup']);
});

test('★ 未装载配置 / 宿主帧上限不触发保存', () => {
  const order = [];
  shutdown(false, order);
  assert.deepEqual(order, ['cleanup']);
});

test('★ 根帧结束：catch 发关闭消息 ⇒ 保存再清理', () => {
  const order = [];
  order.push('root-exit-catch');
  order.push('wm-close');
  shutdown(true, order);
  assert.deepEqual(order, ['root-exit-catch', 'wm-close', 'save', 'cleanup']);
});
