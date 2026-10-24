/** @env pure @kind contract @why 0x1a1 是"无确认的读回"：把它做成写（或成功时写 op1）会静默覆盖玩家存档、并把读回成功报成失败 */
/**
 * tools/test/emulator-new-save-1a1.test.mjs —— `0x1a1`（无确认的读回）的合同守卫。
 *
 * ★ 判据为什么是这些（`data/spec/chapters/60-ops-save.md` 的 `CORE-OPS-SAVE-WRITE`）：
 *   - 打不开 ⇒ 第 1 个操作数 ← 1：文件不存在**不是**「创建空档」；
 *   - 能打开 ⇒ 读头 + 读负载，**成功路径不写第 1 个操作数**：把它写成 0 会凭空造出一条
 *     「0x1a1 有返回值」的合同（那是 `0x19f` 的口径），脚本据它分支就会走错；
 *   - 本层**没有**负载还原 / 整池重编码 ⇒ 必须留下 `save-restore-unmodeled` /
 *     `save-recode-unmodeled` 诊断（⛔ 不假装恢复过、也不写假的重编码）；
 *   - 头不合法这一支**未取证**（合同只说"打不开"）⇒ 记 `save-1a1-header-invalid` 且
 *     **不写**第 1 个操作数（⛔ 不照抄 `0x1a0` 的 2）。
 *   - 三条路径都**不许**出现任何 `write` 调用：这是读，写是 `0x19e`。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { Machine } from '../../apps/emulator-new/src/core/machine.ts';
import { HANDLERS } from '../../apps/emulator-new/src/core/handlers.ts';
import { encodeInt, readInt } from '../../apps/emulator-new/src/core/values.ts';

const KEY = 0x1234abcd;
const op = (type, raw) => ({ type, raw });

function machine(opts = {}) {
  return new Machine({ body: Buffer.alloc(4), table: new Map(), key: KEY, ...opts });
}

function saveHost(overrides = {}) {
  const calls = [];
  return {
    calls,
    fileForSlot: (slot, kind) => `slot${slot}.${kind}`,
    read: async (p) => { calls.push(`read:${p}`); return overrides.read?.(p) ?? null; },
    write: async (p, b) => { calls.push(`write:${p}`); return overrides.write?.(p, b) ?? true; },
    rename: async (a, b) => { calls.push(`rename:${a}->${b}`); return overrides.rename?.(a, b) ?? true; },
    remove: async (p) => { calls.push(`remove:${p}`); return overrides.remove?.(p) ?? true; },
    copy: async (a, b) => { calls.push(`copy:${a}->${b}`); return overrides.copy?.(a, b) ?? true; },
    ...(overrides.confirmOverwrite ? { confirmOverwrite: (s) => overrides.confirmOverwrite(s) } : {}),
  };
}

/** 一个头合法的 292 字节存档（魔数 + 头字段随便填，`0x1a1` 只校验长度与魔数）。 */
function validHeader() {
  const buf = Buffer.alloc(292);
  buf.write('S4SD', 0, 'latin1');
  buf.writeUInt32LE(3, 284);
  buf.writeUInt32LE(20, 288);
  return buf;
}

/** 第 1 个操作数放一个**不等于 0/1** 的哨兵值 ⇒ 「没写」与「写成 0/1」当场分得开。 */
function seedOp1(m, ops, value) {
  m.globalInt.set(ops[0].raw, encodeInt(value, KEY));
}

const SENTINEL = 0x5eed;

test('★ `0x1a1`：文件打不开 ⇒ op1 ← 1，且一次写都不发生', async () => {
  const host = saveHost({ read: () => null });
  const m = machine({ saveHost: host });
  const ops = [op(3, 4), op(0, 5)];
  await HANDLERS.get(0x1a1)(m, ops);

  assert.equal(readInt(m, ops, 1), 1, '打不开 ⇒ 第 1 个操作数 ← 1');
  assert.deepEqual(host.calls, ['read:slot5.dat'], '★ 只读该槽的 .DAT；没有任何写 / 删 / 改名');
  assert.ok(!m.diagnostics.some((d) => d.kind === 'save-restore-unmodeled'), '没读到 ⇒ 不记还原诊断');
  assert.ok(!m.diagnostics.some((d) => d.kind === 'save-recode-unmodeled'), '没读到 ⇒ 不记重编码诊断');
});

test('★ `0x1a1`：文件存在且头合法 ⇒ 成功路径**不写** op1（保持原值）、无写调用、有还原诊断', async () => {
  const host = saveHost({ read: () => validHeader() });
  const m = machine({ saveHost: host });
  const ops = [op(3, 4), op(0, 5)];
  seedOp1(m, ops, SENTINEL);
  await HANDLERS.get(0x1a1)(m, ops);

  assert.equal(readInt(m, ops, 1), SENTINEL, '★ 成功路径不写第 1 个操作数（写成 0 会让脚本以为读回有返回值）');
  assert.deepEqual(host.calls, ['read:slot5.dat'], '★ 只读打开；不许写 / 截断该槽文件');
  const kinds = m.diagnostics.map((d) => d.kind);
  assert.ok(kinds.includes('save-restore-unmodeled'), '★ 负载还原（帧 / 池 / 局部）本层未建模 ⇒ 必须留痕');
  assert.ok(kinds.includes('save-recode-unmodeled'), '★ 整池重编码本层未建模（core 无现成入口）⇒ 必须留痕');
  assert.ok(!kinds.includes('save-1a1-header-invalid'), '头合法 ⇒ 不许报头不合法');
});

test('★ `0x1a1`：头不合法（长度不足 / 魔数不对）⇒ 记 save-1a1-header-invalid，op1 不写', async () => {
  const short = Buffer.alloc(291);
  short.write('S4SD', 0, 'latin1');
  const badMagic = Buffer.alloc(292);
  badMagic.write('S3SX', 0, 'latin1');

  for (const [label, bytes] of [['长度 < 292', short], ['魔数不对', badMagic]]) {
    const host = saveHost({ read: () => bytes });
    const m = machine({ saveHost: host });
    const ops = [op(3, 4), op(0, 5)];
    seedOp1(m, ops, SENTINEL);
    await HANDLERS.get(0x1a1)(m, ops);

    assert.equal(readInt(m, ops, 1), SENTINEL, `${label} ⇒ 不写 op1（合同未取证这一支，⛔ 不照抄 0x1a0 的 2）`);
    assert.deepEqual(host.calls, ['read:slot5.dat'], `${label} ⇒ 只读、不写`);
    const kinds = m.diagnostics.map((d) => d.kind);
    assert.ok(kinds.includes('save-1a1-header-invalid'), `${label} ⇒ 记 save-1a1-header-invalid`);
    assert.ok(!kinds.includes('save-restore-unmodeled'), `${label} ⇒ 读失败，不许记"还原未建模"`);
  }
});
