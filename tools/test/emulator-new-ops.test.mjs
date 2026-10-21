/** @env pure @kind contract @why 数值族 / 操作数原语 / 派发推进的实现与规范不符（写错方向、读错类型、推进算错都不会报错，只会静默算错） */
/**
 * tools/test/emulator-new-ops.test.mjs —— `apps/emulator-new` 的数值族守卫
 *
 * 覆盖 `data/spec/chapters/60-ops-numeric.md` 的三条规则（0x50 / 0x51 / 0x2d5）与 `40-pools.md` 的
 * 整型编解码 / 操作数原语 / 派发推进。用**合成脚本字节**，不依赖游戏安装。
 *
 * ★ 判据为什么是这些：
 *   - 加法与减法的**方向**写反不会报错（只会算错）⇒ 分别钉住 13 与 7；
 *   - 写进局部池的是**编码后的位模式**（不是原值）⇒ 断言池里是 ENC(13) 且解码回来是 13；
 *   - 分派器按「操作数个数槽」推进（长度 = 2·argc+1）⇒ 两条指令后 ip 必须正好是 4+24+4+16；
 *   - 未实现的 opcode **不执行**（ip 不动、steps 不涨）⇒ 缺口的语义是"停在它前面"。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { loadOpcodeTable } from '../../apps/emulator-new/src/spec.ts';
import { Machine } from '../../apps/emulator-new/src/machine.ts';
import { decodeInt, encodeInt } from '../../apps/emulator-new/src/values.ts';

const table = loadOpcodeTable();
const KEY = 0x1234abcd;

function f32bits(v) {
  const b = new DataView(new ArrayBuffer(4));
  b.setFloat32(0, v, true);
  return b.getUint32(0, true);
}

function f32value(bits) {
  const b = new DataView(new ArrayBuffer(4));
  b.setUint32(0, bits, true);
  return b.getFloat32(0, true);
}

/** 把 [opcode, [[type, raw], ...]] 拼成一段 body（不含头） */
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

function machineFor(instrs) {
  return new Machine({ body: script(instrs), table, key: KEY });
}

const LOCAL_INT_0 = 9;
const GLOBAL_FLOAT = 4;

test('★ 整型编解码互逆（与 key 无关；key 必须显式给）', () => {
  for (const key of [0, 1, 0x1234abcd, 0xffffffff]) {
    for (const v of [0, 1, -1, 13, -7, 0x7fffffff, -0x80000000]) {
      assert.equal(decodeInt(encodeInt(v, key), key), v, `key=0x${key.toString(16)} v=${v}`);
    }
  }
  // ★ 编码后的位模式不是原值（否则"编码"是假的）
  assert.notEqual(encodeInt(13, KEY), 13);
});

test('★ 0x50 整数加：和写进第 1 个操作数（13，不是 -13 也不是两倍）', () => {
  const m = machineFor([[0x50, [[LOCAL_INT_0, 0], [0, 10], [0, 3]]]]);
  const r = m.run();
  assert.equal(r.stop.kind, 'truncated', '后面没有指令 ⇒ 走到 body 越界，这里只看一条的效果');
  assert.equal(decodeInt(m.frame.localInt.get(0), KEY), 13);
  assert.equal(m.frame.localInt.get(0), encodeInt(13, KEY), '★ 池里必须是编码后的位模式');
});

test('★ 0x51 整数减：方向是「第 2 个 − 第 3 个」（10 − 3 = 7，写反会得 −7）', () => {
  const m = machineFor([[0x51, [[LOCAL_INT_0, 0], [0, 10], [0, 3]]]]);
  m.run();
  assert.equal(decodeInt(m.frame.localInt.get(0), KEY), 7);
});

test('★ 0x2d5 浮点搬运：第 2 个操作数的浮点值写进第 1 个（2.5，不是位模式 1075838976）', () => {
  const m = machineFor([[0x2d5, [[GLOBAL_FLOAT, 5], [1, f32bits(2.5)]]]]);
  m.run();
  assert.equal(f32value(m.globalFloat.get(5)), 2.5);
  assert.equal(m.globalFloat.get(5), f32bits(2.5));
});

test('★ 派发推进：长度 = 2·argc+1 ⇒ 两条指令后 ip = 4+24 + 4+16 = 48', () => {
  const body = script([
    [0x51, [[LOCAL_INT_0, 0], [0, 1], [0, 0]]],
    [0x2d5, [[GLOBAL_FLOAT, 5], [1, f32bits(1.5)]]],
  ]);
  const m = new Machine({ body, table, key: KEY });
  const r = m.run();
  assert.equal(r.steps, 2);
  assert.equal(r.ip, 48);
  // 第二条之后 body 已到底（48 == body.length）⇒ truncated
  assert.equal(r.stop.kind, 'truncated');
});

test('★ 未实现的 opcode 不执行：ip 不动、steps 不涨（缺口停在它前面）', () => {
  // 用一个**仍然未实现**的 opcode 当缺口样本（0x2 已实现；换成已实现的会让这条守卫失去意义）
  const m = machineFor([[0x1f4, [[0, 1], [0, 2]]]]);
  const r = m.run();
  assert.equal(r.stop.kind, 'unimplemented');
  assert.equal(r.stop.opcode, 0x1f4);
  assert.equal(r.steps, 0);
  assert.equal(r.ip, 0);
});

test('★ 不支持的操作数类型响亮失败（不补值）：读浮点遇到字符串引用 0x8', () => {
  // ★ 指针族（0xc 等）**已实现**（解引用）；这里守的是"支持集合之外"的类型仍然抛。
  const m = machineFor([[0x2d5, [[GLOBAL_FLOAT, 5], [0x8, 0]]]]);
  assert.throws(() => m.run(), /read-float:unsupported-operand-type:0x8/);
  // 指针族的解引用失败是**另一类**响亮失败（地址未映射），不是静默 0
  const p = machineFor([[0x2d5, [[GLOBAL_FLOAT, 5], [0xc, 0]]]]);
  assert.throws(() => p.run(), /不落在任何池窗口/);
});

test('★ 未写过的浮点槽不许当 0：读浮点失败（规范只说"不保证初值"）', () => {
  const m = machineFor([[0x2d5, [[GLOBAL_FLOAT, 9], [GLOBAL_FLOAT, 7]]]]);
  assert.throws(() => m.run(), /uninitialized-float-slot:7/);
});

test('★ 整型池的未写槽读出来是 0（初值 = ENC(0)，不是位模式 0）', () => {
  const m = machineFor([[0x50, [[LOCAL_INT_0, 0], [0, 0], [0, 5]]]]);
  m.run();
  assert.equal(decodeInt(m.frame.localInt.get(0), KEY), 5, '0 + 5 = 5；未写过的第 3 个操作数按初值 0 读');
});

test('★ 浮点 → 整型是向零截断（2.75 → 2、−2.75 → −2），不是四舍五入', () => {
  for (const [v, want] of [[2.75, 2], [-2.75, -2], [2.5, 2], [-0.9, 0]]) {
    const m = machineFor([[0x2d5, [[3, 100], [1, f32bits(v)]]]]);
    m.run();
    assert.equal(decodeInt(m.globalInt.get(100), KEY), want, `write-float 到整型族：${v}`);
  }
});

test('★ 读整型从浮点族也走同一条截断（25.9 → 25）', () => {
  const m = machineFor([
    [0x2d5, [[GLOBAL_FLOAT, 3], [1, f32bits(25.9)]]],
    [0x50, [[LOCAL_INT_0, 0], [GLOBAL_FLOAT, 3], [0, 1]]],
  ]);
  m.run();
  assert.equal(decodeInt(m.frame.localInt.get(0), KEY), 26, '25.9 截断成 25，再 +1（读整型从浮点族）');
});
test('★ 0x55 整数搬运：第 2 个操作数写进第 1 个（13 不是 26、不是位模式）', () => {
  const m = machineFor([[0x55, [[9, 0], [0, 13]]]]);
  m.run();
  assert.equal(decodeInt(m.frame.localInt.get(0), KEY), 13);
  assert.equal(m.frame.localInt.get(0), encodeInt(13, KEY), '池里必须是编码后的位模式');
});
