/** @env assets @kind contract @why 真实脚本的表体读错（off/len 配错、或把"元素下标"算成位置偏移）⇒ 存档 rec[259]/rec[260] 在真脚本上永远 −1 / 错位，恢复后从错误的指令继续 */
/**
 * tools/test/emulator-new-save-tables.assets.test.mjs —— **真实脚本**上的位置表 + 帧记录下标。
 *
 * ## 它守的是什么
 * 纯测试用的是合成表（`[7, 10, 15]`）；这一条把它摁在真实脚本上：
 * 1. 三张表的表项**严格升序**，且都是落在代码区里的 dword **位置**；
 * 2. 表项处就是该类 opcode 本身（`表 1 → 0x71` / `表 2 → 0x03`）⇒ 表项不是"随便一个数"；
 * 3. `0x19e` 写出的帧记录里，`rec[259]` / `rec[260]` = **位置在表里的元素下标**（线性查找），
 *    表外位置 ⇒ −1；当前帧 `rec[260]` 强制 −1。
 *
 * ## 为什么用真实脚本（合成测试盖不住的那一半）
 * 合成表的 `off` 是自己摆的；真实表体在 **body 尾部**（`off` = 表体在 body 的 dword 下标，
 * 表区在代码区**之后**）。一旦把 `off` 当成"位置的区间起点"或把下标算成 `位置 − 起点`，
 * 合成测试仍可能绿，而真脚本上所有下标都变 −1/错位 —— 这条就是判据。
 *
 * ## 运行
 * `pnpm test:assets`（要 `dist/install` 里的脚本语料；缺席时 **skip**，不静默通过）
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';
import { Machine, indexInTable } from '../../apps/emulator-new/src/core/machine.ts';
import { HANDLERS } from '../../apps/emulator-new/src/core/handlers.ts';
import { codeLimit, parseHeader } from '../../apps/emulator-new/src/core/decode.ts';
import { tablesFromHeader } from '../../apps/emulator-new/src/core/runner.ts';
import { readInt } from '../../apps/emulator-new/src/core/values.ts';
import { untransformWords } from '../../apps/emulator-new/src/core/save-codec.ts';

const KEY = 0x1234abcd;
const BIN_DIR = path.join(REPO_ROOT, 'dist', 'install');
const bins = fs.existsSync(BIN_DIR)
  ? fs.readdirSync(BIN_DIR).filter((f) => f.toLowerCase().endsWith('.bin')).sort()
  : [];
const skip = bins.length ? false : '没有 .BIN 语料（先 `pnpm tools release install`）';

const op = (type, raw) => ({ type, raw });

/** 找第一个三张表都够用的真实脚本（表 1 ≥ 3 项、表 2 ≥ 2 项、表 3 ≥ 2 项）。 */
function pickScript() {
  for (const name of bins) {
    const bytes = fs.readFileSync(path.join(BIN_DIR, name));
    if (bytes.subarray(0, 4).toString('latin1') !== 'SYS4') continue;
    const header = parseHeader(bytes);
    const body = bytes.subarray(header.headerLength);
    const tables = tablesFromHeader(header, body);
    if (!tables.msg || !tables.call || !tables.sub) continue;
    if (tables.msg.length >= 3 && tables.call.length >= 2 && tables.sub.length >= 2) {
      return { name, bytes, header, body, tables };
    }
  }
  return null;
}

function slotHost() {
  const host = {
    written: null,
    fileForSlot: (slot, kind) => `slot${slot}.${kind}`,
    read: async () => null,
    write: async (_path, bytes) => { host.written = bytes; return true; },
    remove: async () => true,
    rename: async () => true,
    copy: async () => true,
  };
  return host;
}

/** `0x19e` 写一次槽 ⇒ 主体视图（`+0` 当前帧号；帧记录基址 = `1252 + 20000`，版本 1 = 3）。 */
async function saveBody(m, host) {
  const ops = [op(3, 1), op(0, 1)];
  await HANDLERS.get(0x19e)(m, ops);
  assert.equal(readInt(m, ops, 1), 0, '0x19e 写槽成功');
  const file = host.written;
  const params = new DataView(file.buffer, file.byteOffset + 292, 20);
  const seed = params.getUint32(12, true);
  const key = params.getUint16(16, true);
  const cipher = file.slice(312);
  const plain = new Uint8Array(
    untransformWords(new Uint32Array(cipher.buffer, cipher.byteOffset, cipher.byteLength / 4), seed, key).buffer,
  );
  return new DataView(plain.buffer, 12);
}

const REC_BASE = 1252 + 20000;
const recAt = (index) => REC_BASE + 1044 * index;
const word = (body, off) => body.getUint32(off, true);

test('★ 真实脚本：三张表项严格升序且指向该类 opcode；表内位置命中、表外 −1', { skip }, () => {
  const s = pickScript();
  assert.ok(s, '语料里应有至少一个三张表都够用的 SYS4 脚本（三表全空 / 全缺 ⇒ 读表口径失效）');
  const { name, header, body, tables } = s;
  const codeEnd = codeLimit(header, body.length) / 4;

  const classes = [
    ['表 1 → 0x71', tables.msg, 0x71],
    ['表 2 → 0x03', tables.call, 0x03],
    ['表 3 → 0x8f', tables.sub, 0x8f],
  ];
  for (const [label, entries, opcode] of classes) {
    for (let k = 1; k < entries.length; k += 1) {
      assert.ok(entries[k] > entries[k - 1], `${name}/${label}: 表项必须**严格升序**（第 ${k} 项 ${entries[k]} ≤ 前一项 ${entries[k - 1]}）`);
    }
    for (const position of entries) {
      assert.ok(Number.isInteger(position) && position >= 0 && position < codeEnd, `${name}/${label}: 表项 ${position} 必须落在代码区 [0, ${codeEnd}) 内（表项是**位置**）`);
      assert.equal(body.readUInt32LE(position * 4), opcode, `${name}/${label}: 表项 ${position} 处就是 opcode 0x${opcode.toString(16)}`);
    }
  }

  // 线性查找：项 ⇒ 自己的下标；表外的位置 ⇒ −1。
  const msg = tables.msg;
  const call = tables.call;
  assert.equal(indexInTable(msg[0], msg), 0);
  assert.equal(indexInTable(msg[1], msg), 1);
  assert.equal(indexInTable(msg[2], msg), 2);
  assert.equal(indexInTable(msg[msg.length - 1] + 1, msg), -1, `${name}: 末项之后的位置不在表 1 里 ⇒ −1`);
  assert.equal(indexInTable(call[1], msg), -1, `${name}: 0x03 的位置不会出现在 0x71 表里（同一位置只有一个 opcode）`);
});

test('★ 真实脚本的存档：`rec[259]` / `rec[260]` = 真实表下标（不是位置），当前帧 `rec[260]` 强制 −1、表外 −1', { skip }, async () => {
  const s = pickScript();
  assert.ok(s, '语料里应有至少一个三张表都够用的 SYS4 脚本');
  const { name, body, tables } = s;
  const msg = tables.msg;
  const call = tables.call;

  // ① 根帧位置 = 真实 0x03 位置（它之后会变成**非当前帧**）⇒ rec[260] 命中；子帧位置 = 真实 0x71 位置。
  const host = slotHost();
  const m = new Machine({
    body: Buffer.alloc(4), table: new Map(), key: KEY,
    rootTables: tables, scriptTables: () => tables, saveHost: host,
  });
  m.frame.position = call[1];
  m.enterScript(body, 1); // 子帧成为当前帧
  m.frame.position = msg[2];

  const saved = await saveBody(m, host);
  assert.equal(word(saved, 0), 1, '两帧：当前帧号 1');
  assert.equal(word(saved, recAt(0)), 0xffffffff, `${name}: 根帧 rec[0] = −1`);
  assert.equal(word(saved, recAt(0) + 1036), 0xffffffff, `${name}: 根帧位置是 0x03 ⇒ 不在表 1 里，rec[259] = −1`);
  assert.equal(word(saved, recAt(0) + 1040), 1, `${name}: ★ 根帧（非当前帧）rec[260] = 0x03 位置 call[1] 在表 2 里的下标 1`);
  assert.equal(word(saved, recAt(1)), 0, `${name}: 子帧 rec[0] = 0（父帧号）`);
  assert.equal(word(saved, recAt(1) + 1036), 2, `${name}: ★ 子帧 rec[259] = 0x71 位置 msg[2] 在表 1 里的下标 2（⛔ 不是位置本身）`);
  assert.equal(word(saved, recAt(1) + 1040), 0xffffffff, `${name}: ★ 子帧是当前帧 ⇒ rec[260] 强制 −1`);

  // ② 当前帧位置是表外的洞（末项 + 1）⇒ rec[259] 也是 −1。
  const host2 = slotHost();
  const out = new Machine({ body: Buffer.alloc(4), table: new Map(), key: KEY, rootTables: tables, saveHost: host2 });
  out.frame.position = msg[msg.length - 1] + 1;
  const saved2 = await saveBody(out, host2);
  assert.equal(word(saved2, recAt(0) + 1036), 0xffffffff, `${name}: 表外位置 ⇒ rec[259] = −1`);
  assert.equal(word(saved2, recAt(0) + 1040), 0xffffffff, `${name}: 当前帧 ⇒ rec[260] = −1`);
});
