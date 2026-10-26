/** @env pure @kind contract @why 输入族（0xfe/0xff/0xfb/0xcc/0xcd/0x100/0x2fc/0x108/0x109/0x24e）的引擎侧合同被实现成 no-op / 写错目标 / 派发次序错，而 TITLE 仍可能"看起来在跑" */
/**
 * tools/test/emulator-new-input.test.mjs —— `data/spec/chapters/60-ops-input.md` 的守卫
 *
 * 覆盖：
 * - `CORE-INPUT-WORD`：0xfe 键总数（无符号上界）/ 0xff 清字 + 采样 + 游标/上限
 * - `CORE-INPUT-CALLBACK-REGISTER`：0xcc 鼠标回调登记 / 0xfb 按键回调登记（0..31）
 * - `CORE-INPUT-MOUSE-CALLBACK-PUMP`：0xcd 时间闸 / 返回位置 / 脚本身份校验 / 目标 -1
 * - `CORE-INPUT-KEY-DISPATCH`：0x100 无按键表项 / 逐位置位 / 当前指令重派发 / 未登记放弃
 * - `CORE-INPUT-TOUCH-POINT` / `-POS` / `-BUTTON`：0x2fc / 0x109 / 0x108
 * - `CORE-INPUT-MODE-WORD`：0x24e
 * - `CORE-OPS-WAIT-POLL-INPUT`：0x101 只清第 27 位
 * - 短睡眠：0xc8 必须推进时间，不是 no-op
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { loadOpcodeTable } from '../../apps/emulator-new/src/platform/node/spec.ts';
import { Machine } from '../../apps/emulator-new/src/core/machine.ts';
import { HANDLERS } from '../../apps/emulator-new/src/core/handlers.ts';
import { decodeInt, encodeInt } from '../../apps/emulator-new/src/core/values.ts';

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
  return new Machine({ body: script(instrs), table, key: KEY, tickMs: 1, rootId: 0, ...opts });
}

/** 一个可控的输入后端；缺省"没有设备" */
function inputHost(overrides = {}) {
  return {
    sampleKeys: () => 0,
    cursor: () => null,
    buttons: () => null,
    nextTouchPoint: () => null,
    ...overrides,
  };
}

test('★ 输入字与键总数：0xfe / 0xff', async () => {
  const m = machineFor([[0xfe, [[0, 12]]], [0xff, []]], {
    inputHost: inputHost({ sampleKeys: () => 0b101 }),
  });
  await m.run();
  assert.equal(m.input.keyTotal, 12, '键总数 ← 0xfe 的操作数');
  assert.equal(m.input.word, 0b101, '0xff 先清再采样，输入字 = 平台返回值');
  assert.equal(m.frame.keyLimit, 12, '扫描上限 ← 键总数');
  assert.equal(m.frame.keyCursor, 0, '扫描游标 ← 0');
  assert.deepEqual(
    m.effects.filter((e) => e.kind === 'input.key-total' || e.kind === 'input.sample').map((e) => e.kind),
    ['input.key-total', 'input.sample'],
  );

  const bad = machineFor([[0xfe, [[0, 32]]]]);
  await assert.rejects(() => bad.run(), /超出引擎的范围检查/, '32 必须按无符号比较拒绝');
  assert.equal(bad.input.keyTotal, 7, '越界不写（保留初值 7）');

  const negative = machineFor([[0xfe, [[0, 0xffffffff]]]]);
  await assert.rejects(() => negative.run(), /超出引擎的范围检查/, '-1 按无符号看是最大值，也必须拒绝');
});

test('★ 回调登记：0xcc / 0xfb', async () => {
  const m = machineFor([[0xcc, [[0, 16], [0, 2]]], [0xfb, [[0, 4], [0, 7]]]]);
  await m.run();
  assert.deepEqual(
    { interval: m.input.mouse.interval, target: m.input.mouse.target, owner: m.input.mouse.owner, last: m.input.mouse.last },
    { interval: 16, target: 2, owner: 0, last: 0 },
    '0xcc：间隔 / 目标 / 根帧身份 / 上次时刻',
  );
  assert.equal(m.frame.keyJumps.get(4), 7, '0xfb：键 4 → 位置 7');

  const zero = machineFor([[0xcc, [[0, 0], [0, 1]]]]);
  await zero.run();
  assert.equal(zero.input.mouse.interval, 1, '间隔 0 写成 1');

  const bad = machineFor([[0xfb, [[0, 32], [0, 1]]]]);
  await assert.rejects(() => bad.run(), /SetKeyJump/, '键编号 32 必须拒绝');
  assert.equal(bad.frame.keyJumps.size, 0, '越界不登记');
});

test('★ 定时派发鼠标回调：0xcd', async () => {
  // 位置 0 = 0xcd，位置 1 = 0x2 exit，位置 2 = 0x5 ret（目标）
  const body = [[0xcd, []], [0x2, []], [0x5, []]];
  const m = machineFor(body);
  m.input.mouse = { interval: 10, last: 0, target: 2, owner: 0 };
  m.now = 10;
  const r = await m.run();
  assert.equal(m.input.mouseDispatches, 1, '间隔到 ⇒ 派发一次');
  assert.deepEqual(m.frame.retStack, [], '回调返回后返回栈已弹出');
  assert.equal(r.steps, 3, '0xcd + ret + exit');
  assert.equal(m.input.mouse.last, 10, '上次派发时刻更新');

  const early = machineFor(body);
  early.input.mouse = { interval: 10, last: 0, target: 2, owner: 0 };
  early.now = 5;
  await early.run();
  assert.equal(early.input.mouseDispatches, 0, '间隔未到 ⇒ 只前进一条');
  assert.equal(early.frame.retStack.length, 0, '不压返回位置');

  const cancel = machineFor(body);
  cancel.input.mouse = { interval: 0, last: 0, target: -1, owner: 0 };
  cancel.now = 1;
  await cancel.run();
  assert.equal(cancel.input.mouseDispatches, 0, '目标 -1 ⇒ 不派发');
  assert.equal(cancel.frame.retStack.length, 0, '压入的返回位置被撤销');

  const deep = machineFor(body);
  deep.input.mouse = { interval: 0, last: 0, target: 2, owner: 99 };
  deep.now = 1;
  await assert.rejects(() => deep.run(), /Depth/, '注册脚本身份不符 ⇒ 显示消息型异常');
});

test('★ 按键派发：0x100', async () => {
  // 位置 0 = 0x100，位置 1 = 0x2 exit，位置 2 = 0x5 ret
  const body = [[0x100, []], [0x2, []], [0x5, []]];

  const noKey = machineFor(body);
  noKey.input.keyTotal = 2;
  noKey.frame.keyLimit = 2;
  noKey.frame.keyJumps.set(2, 2);
  noKey.input.word = 0;
  await noKey.run();
  assert.equal(noKey.input.keyDispatches, 1, '输入字 0 ⇒ 派发"无按键"表项');
  assert.equal(noKey.input.word, 0);

  // 两个键都指向 ret：0x100 压"当前位置"、返回后重新派发，直到游标走完
  const multi = machineFor(body);
  multi.input.keyTotal = 2;
  multi.frame.keyLimit = 2;
  multi.frame.keyJumps.set(0, 2);
  multi.frame.keyJumps.set(1, 2);
  multi.input.word = 0b11;
  await multi.run();
  assert.equal(multi.input.keyDispatches, 2, '逐位派发两个键');
  assert.equal(multi.frame.keyCursor, 2, '游标走完');
  assert.equal(multi.input.word, 0b11, '派发不改输入字');

  const unregistered = machineFor(body);
  unregistered.input.keyTotal = 2;
  unregistered.frame.keyLimit = 2;
  unregistered.input.word = 0b10;
  await unregistered.run();
  assert.equal(unregistered.input.keyDispatches, 0, '未登记 ⇒ 不派发');
  assert.equal(unregistered.frame.retStack.length, 0, '不压返回位置');
});

test('★ 触点查询：0x2fc', async () => {
  const point = { x: 10, y: 20, flags: 0x11, extra: 3 };
  const m = machineFor([[0x2fc, [[3, 1], [3, 2], [3, 3], [3, 4], [3, 5]]]], {
    inputHost: inputHost({ nextTouchPoint: () => point }),
  });
  await m.run();
  assert.deepEqual(
    [m.globalInt.get(1), m.globalInt.get(2), m.globalInt.get(3), m.globalInt.get(4), m.globalInt.get(5)]
      .map((v) => decodeInt(v ?? encodeInt(0, KEY), KEY)),
    [1, 10, 20, 0x11, 3],
    '有触点：结果 1 / x / y / flags / 编号',
  );

  const none = machineFor([[0x2fc, [[3, 1], [3, 2], [3, 3], [3, 4], [3, 5]]]], { inputHost: inputHost() });
  for (const i of [2, 3, 4, 5]) none.globalInt.set(i, encodeInt(77, KEY));
  await none.run();
  assert.equal(decodeInt(none.globalInt.get(1), KEY), 0, '没有事件：结果 0');
  for (const i of [2, 3, 4, 5]) {
    assert.equal(decodeInt(none.globalInt.get(i), KEY), 77, `没有事件：第 ${i} 个操作数保持原值`);
  }
});

test('★ 光标与按钮查询：0x109 / 0x108', async () => {
  const m = machineFor([[0x109, [[3, 1], [3, 2]]], [0x108, [[3, 3]]]], {
    inputHost: inputHost({ cursor: () => ({ x: 100, y: 200 }), buttons: () => 3 }),
  });
  await m.run();
  assert.deepEqual(
    [1, 2, 3].map((i) => decodeInt(m.globalInt.get(i), KEY)),
    [100, 200, 3],
    '设备可用：x / y / 按钮位图',
  );

  const none = machineFor([[0x109, [[3, 1], [3, 2]]], [0x108, [[3, 3]]]], { inputHost: inputHost() });
  await none.run();
  assert.deepEqual(
    [1, 2, 3].map((i) => decodeInt(none.globalInt.get(i), KEY)),
    [-100000, -100000, 0],
    '设备不可用：两个坐标都写哨兵；按钮写 0',
  );
});

test('★ 主循环模式字：0x24e', async () => {
  const m = machineFor([[0x24e, [[0, 0x10001]]]]);
  await m.run();
  assert.equal(m.input.mode, 0x10001, '模式字原样写入');
  assert.equal(m.flags, 0, '0x24e 不置门位');
});

test('★ 0x101 只清第 27 位：其它位保持', async () => {
  const m = machineFor([[0x101, []]]);
  m.flags = 0x08000000 | 0x400 | 0x1;
  m.inputWord = 0xdeadbeef;
  // 门模型下 flags 非 0 不会派发 0x101；这里直接调 handler，单测引擎数据效果。
  HANDLERS.get(0x101)(m, []);
  assert.equal(m.flags, 0x401, '第 27 位被清；等待位 / 睡眠位保留');
  assert.equal(m.input.word, 0, '输入字归零');
  assert.equal(m.inputWord, 0, '输入字归零（旧字段同步）');
  assert.equal(m.scalars.get('pollInput.a'), 1);
  assert.equal(m.scalars.get('pollInput.b'), 0);
  assert.deepEqual(m.effects.map((e) => e.kind), ['input.poll']);
});

test('★ 短睡眠推进时间而不是 no-op', async () => {
  const m = machineFor([[0xc8, [[0, 1]]], [0x1a8, []]], { sleepHost: async () => {} });
  const before = m.now;
  const r = await m.run();
  assert.equal(r.steps, 2, '睡眠之后继续执行下一条');
  assert.equal(m.now, before + 1, '虚拟时钟前进 1ms');
  assert.equal(m.ticks, 0, '短睡眠不是一次门迭代');
  assert.equal(m.effects.some((e) => e.kind === 'engine.scalar.write' && e.detail.sleepMs === 1), true);
});

test('★ 0xcd / 0x100 的 handler 都在表里（防重复键覆盖）', () => {
  for (const opcode of [0xfe, 0xff, 0xfb, 0xcc, 0xcd, 0x100, 0x2fc, 0x108, 0x109, 0x24e]) {
    assert.equal(HANDLERS.get(opcode) !== undefined, true, `opcode 0x${opcode.toString(16)} 必须有 handler`);
  }
  // 0xfe 只能有一个 handler：旧的 thinFe 若还在表里会把键总数覆盖回 0。
  const m = new Machine({ body: script([[0xfe, [[0, 12]]]]), table, key: KEY });
  return m.run().then(() => assert.equal(m.input.keyTotal, 12, '0xfe 必须真的写键总数，而不是旧薄标量 handler'));
});

test('★ 脚本化输入解析与事件时序：CLI --input 的语法', async () => {
  const { parseInputScript, ScriptedInputHost } = await import('../../apps/emulator-new/src/platform/node/input.ts');
  const host = new ScriptedInputHost(parseInputScript(
    '100 move 640 360\n200 down 1\n260 up 1\n300 key 4 down\n360 key 4 up',
  ));
  assert.deepEqual(host.stats(), { applied: 0, lastEventAt: 0, exhausted: false }, '初始未应用');
  assert.equal(host.sampleKeys(99), 0);
  assert.equal(host.cursor(99), null, '还没 move ⇒ 光标不可用');
  assert.equal(host.nextTouchPoint(99), null);

  assert.deepEqual(host.cursor(150), { x: 640, y: 360 }, '100ms 的 move 已应用');
  assert.equal(host.sampleKeys(150), 0);

  assert.equal(host.buttons(220), 1 << 1, '200ms 的 down 1 ⇒ 位 1（button 是位号）');
  assert.equal(host.nextTouchPoint(220), null, '桌面鼠标没有触点缓冲');

  assert.equal(host.sampleKeys(310), 1 << 4, '300ms 的 key 4 down ⇒ 位 4');
  assert.equal(host.buttons(310), 0, '260ms 的 up 1 已生效');
  assert.equal(host.sampleKeys(370), 0, '360ms 的 key 4 up 已生效');
  assert.equal(host.stats().exhausted, true);
});

test('★ 鼠标命中查找：0x12e 命中 / 起始下标 / 未命中', async () => {
  const planeX = [0x44e, 0x3e0, 0x365, 0x2d9, 0x453];
  const planeY = [0x126, 0x192, 0x1e5, 0x21f, 0x22a];
  const makeHit = (start, x, y) => {
    const m = machineFor([[0x12e, [[9, 0x3f5], [9, 1], [9, 0x3f0], [9, 0x3f1], [9, 0xcd], [9, 5], [9, 0x69], [9, 0]]]]);
    m.frame.localInt.set(0x3f5, encodeInt(start, KEY));
    for (let i = 0; i < 5; i += 1) {
      for (let k = 0; k < 4; k += 1) {
        m.frame.localInt.set(0xcd + 4 * i + k, encodeInt([0, 0x9c, 0, 0x9c][k], KEY));
      }
      m.frame.localInt.set(5 + i, encodeInt(planeX[i], KEY));
      m.frame.localInt.set(0x69 + i, encodeInt(planeY[i], KEY));
    }
    m.frame.localInt.set(0, encodeInt(5, KEY));
    m.frame.localInt.set(0x3f0, encodeInt(x, KEY));
    m.frame.localInt.set(0x3f1, encodeInt(y, KEY));
    return m;
  };
  const inside = makeHit(-1, 0x3c0, 0x220); // 第 2 项框内
  await inside.run();
  assert.equal(decodeInt(inside.frame.localInt.get(0x3f5), KEY), 2);
  const outside = makeHit(-1, 0, 0);
  await outside.run();
  assert.equal(decodeInt(outside.frame.localInt.get(0x3f5), KEY), -1);
  const skip = makeHit(2, 0x3c0, 0x220);
  await skip.run();
  assert.equal(decodeInt(skip.frame.localInt.get(0x3f5), KEY), -1, 'start=2 ⇒ 从第 3 项起扫');
});

test('★ 异常必须带指令位置：opcode / 偏移 / 操作数', async () => {
  // 0x192 的落点 type 6（引用族）不在文本写入的支持集里 ⇒ 必然抛；检查包装后的上下文。
  const m = machineFor([[0x1a8, []], [0x192, [[6, 1], [0, 1]]]]);
  await assert.rejects(() => m.run(), (error) => {
    assert.match(error.message, /text-unsupported-operand-type:0x6/);
    assert.match(error.message, /opcode=0x192/);
    assert.match(error.message, /offset=4/, '偏移是本条指令在脚本体里的字节位置');
    assert.match(error.message, /operands=\[0x6:0x1,0x0:0x1\]/);
    return true;
  });
});
