/** @env pure @kind contract @why 0x6d 被实现成 no-op / 静默失败，或"两份缓冲"退化成一份，会让依赖翻页的脚本在错误的缓冲上跑且不报错 */
/**
 * tools/test/emulator-new-ops-memflip.test.mjs —— `40-pools.md` 的 SPC-POOL-MEMFLIP（opcode 0x6d）守卫
 *
 * ★ 判据为什么是这些（每条都能被"写错"弄红）：
 *   - **配置门**：键 `set:EnableMemFlip` 为 0 / 缺失 ⇒ 引擎抛**显示消息型异常**，六个选择不变；
 *     ⛔ 静默不动作、或反而翻页，都是把合同写反。推进格数**先**写 1（异常路径上它已是 1）。
 *   - **两份缓冲**：翻页后同族同下标读到的是另一组的初值；**翻回来**原值仍在（= 翻页不搬数据）。
 *     实现成一份缓冲（或翻页时搬数据）⇒ 第一条断言红。
 *   - **未登记 = 未映射**：备用组只在启动期键非 0 时登记；若键在启动后被改写（spec 归为 UB）而
 *     翻页成功，随后的访问必须**响亮失败**，⛔ 不许懒分配、也不许悄悄读主组。
 *   - **下标空间不随翻页变**：同族同下标的地址在两份缓冲里编号相同、元素宽度不变；
 *     窗口外 / 未对齐的地址在翻页后仍然抛。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { loadOpcodeTable } from '../../apps/emulator-new/src/platform/node/spec.ts';
import { Machine } from '../../apps/emulator-new/src/core/machine.ts';
import { ConfigStore } from '../../apps/emulator-new/src/core/config.ts';
import { slotAddress, slotBytes, resolveAddress } from '../../apps/emulator-new/src/core/address.ts';
import { decodeInt } from '../../apps/emulator-new/src/core/values.ts';

const table = await loadOpcodeTable();
const KEY = 0x1234abcd;

function script(instrs) {
  const words = [];
  for (const [opcode, ops] of instrs) {
    words.push(opcode);
    for (const [type, raw] of ops) words.push(type, raw);
  }
  const buf = Buffer.alloc(words.length * 4);
  words.forEach((w, i) => buf.writeUInt32LE(w >>> 0, i * 4));
  return buf;
}

function machineFor(instrs, opts = {}) {
  return new Machine({ body: script(instrs), table, key: KEY, tickMs: 1, ...opts });
}

/** 配置库：`ConfigStore.setInt` 不校验键名（这里只用 set:EnableMemFlip 一个键） */
function configWith(entries = {}) {
  const store = new ConfigStore({ defaults: [], ini: [], persist: [] });
  for (const [key, value] of Object.entries(entries)) store.setInt(key, value);
  return store;
}

const NOP = 0x1a8;

test('★ 0x6d：配置键缺失 / 为 0 ⇒ 显示消息型异常、推进格数先写 1、六个选择不变', async () => {
  const missing = machineFor([[0x6d, []]]);
  await assert.rejects(() => missing.run(), /display-message.*memflip/, '键缺失 ⇒ 必须是显示消息型异常');
  assert.equal(missing.operandCount, 1, '推进格数**先**写成 1（异常路径上它已经是 1）');
  assert.equal(missing.globalFlip, 0, '异常路径上选择不变');
  assert.equal(missing.globalFlipAvailable, false, '键缺失 ⇒ 备用缓冲未登记');

  const zero = machineFor([[0x6d, []]], { config: configWith({ 'set:EnableMemFlip': 0 }) });
  await assert.rejects(() => zero.run(), /display-message.*memflip/, '键为 0 ⇒ 同一个异常');
  assert.equal(zero.operandCount, 1);
  assert.equal(zero.globalFlip, 0);
  assert.equal(zero.effects.some((e) => e.kind === 'pool.memflip'), false, '异常路径不许留下"翻过页"的痕迹');

  const on = machineFor([[0x6d, []], [NOP, []]], { config: configWith({ 'set:EnableMemFlip': 1 }) });
  const result = await on.run();
  assert.equal(on.globalFlip, 1, '键非 0 ⇒ 整体对调');
  assert.equal(on.globalFlipAvailable, true, '启动期键非 0 ⇒ 备用缓冲已登记');
  assert.equal(on.effects.filter((e) => e.kind === 'pool.memflip').length, 1);
  assert.equal(result.steps, 2, '推进格数 1 ⇒ 照常走下一格');
});

test('★★ 配置非 0 ⇒ 翻页后读写落在另一组缓冲，翻回原值仍在', async () => {
  const m = machineFor([
    [0x55, [[3, 0], [0, 42]]],   // 0x55 整数搬运：写全局整型池 [0] = 42（主缓冲）
    [0x6d, []],                  // 翻页 → 读 / 写都落到备用缓冲
    [0x55, [[9, 0], [3, 0]]],    // 读全局整型池 [0] → 局部整型池 [0]
    [0x6d, []],                  // 翻回主缓冲
    [0x55, [[9, 1], [3, 0]]],    // 再读一次 → 局部整型池 [1]
  ], { config: configWith({ 'set:EnableMemFlip': 1 }) });
  await m.run();

  assert.equal(decodeInt(m.frame.localInt.get(0), KEY), 0,
    '翻页后读到的是**另一组**缓冲的初值（整型族的分配初值 = 编码后的零）；读到 42 说明只有一份缓冲');
  assert.equal(decodeInt(m.frame.localInt.get(1), KEY), 42, '翻回原选择后原值仍在（翻页不搬数据）');
  assert.equal(m.globalFlip, 0, '两次翻页回到主缓冲');
  assert.equal(m.effects.filter((e) => e.kind === 'pool.memflip').length, 2);
});

test('★★ 键为 0/缺失 ⇒ 备用缓冲未登记：翻页后访问是"未映射"（不是静默读到主缓冲）', async () => {
  // 启动期键为 0 ⇒ 只登记主缓冲；运行期把键改成 1（spec 归为 UB）⇒ 0x6d 按执行期读到 1 而翻页成功。
  const config = configWith({ 'set:EnableMemFlip': 0 });
  const m = machineFor([[0x6d, []], [0x55, [[9, 0], [3, 0]]]], { config });
  config.setInt('set:EnableMemFlip', 1);

  await assert.rejects(
    () => m.run(),
    /未映射/,
    '备用缓冲从未分配 ⇒ 翻页后的访问必须响亮失败（⛔ 不许懒分配，也不许悄悄读主缓冲）',
  );
  assert.equal(m.globalFlip, 1, '翻页本身按执行期读到的键成功（失败发生在随后的访问上）');
  assert.equal(m.globalFlipAvailable, false, '未登记状态不因一次翻页而改变');
});

test('★ 翻页不改下标空间 / 元素宽度；窗口外与未对齐地址在翻页后仍然响亮失败', async () => {
  const m = machineFor([[0x6d, []], [NOP, []]], { config: configWith({ 'set:EnableMemFlip': 1 }) });

  const main = slotAddress(3, 5, 0, 0);
  const back = slotAddress(3, 5, 0, 1);
  assert.notEqual(main, back, '两份缓冲是两组窗口（不是同一段地址）');
  assert.equal(resolveAddress(main).index, 5);
  assert.equal(resolveAddress(back).index, 5, '同族同下标在两组的编号相同（翻页不改下标空间）');
  assert.equal(resolveAddress(main).bank, 0);
  assert.equal(resolveAddress(back).bank, 1);
  assert.equal(slotBytes(3), 4, '元素宽度是族的属性，不随翻页变化');
  assert.equal(slotAddress(3, 6, 0, 1) - back, 4, '备用组里的元素步长同样是 4');

  assert.throws(() => resolveAddress(0x00001000), /不落在任何池窗口/, '窗口外的地址不因翻页变合法');
  assert.throws(() => resolveAddress(slotAddress(3, 0, 0, 0) + 2), /没对齐到元素边界/);

  await m.run();
  assert.equal(m.globalFlip, 1);
  assert.equal(slotAddress(3, 5, 0, 1), back, '翻页后两组窗口的算术不变（只换选择）');
  assert.throws(() => resolveAddress(0x00001000), /不落在任何池窗口/, '翻页之后仍未映射');

  // 行为侧：翻页后经指针读一个窗口外地址 ⇒ 仍然抛（不会因为翻页"落到某个池里"）
  const viaPointer = machineFor([[0x6d, []], [0x55, [[9, 0], [12, 0]]]], {
    config: configWith({ 'set:EnableMemFlip': 1 }),
  });
  viaPointer.frame.localPtr.set(0, 0x00001000);
  await assert.rejects(() => viaPointer.run(), /不落在任何池窗口/);
});
