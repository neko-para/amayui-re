/** @env assets @kind contract @why 真实样本头的七格按错偏移（+268 星期 / +278 毫秒 / 版本）读时不会抛，只会把日期时间整体错位 */
/**
 * tools/test/emulator-new-save-read-fields.assets.test.mjs —— `0x1a0` 的字段映射绑定**真实存档头**
 *
 * 判据：`corpus/fixtures/SAVE76.DAT` 的 292 字节头读出来必须是
 * `[2026, 9, 23, 10, 34, 58, 170]`（年 / 月 / 日 / 时 / 分 / 秒 / 游戏时间）。
 * ★ 样本缺席（或只有 LFS 指针）⇒ skip，不是红。
 * ★ 合成头守卫在 `emulator-new-ops-100-200.test.mjs`；本条只负责“真实字节”。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';
import { Machine } from '../../apps/emulator-new/src/core/machine.ts';
import { HANDLERS } from '../../apps/emulator-new/src/core/handlers.ts';
import { readInt } from '../../apps/emulator-new/src/core/values.ts';

const FIXTURE = path.join(REPO_ROOT, 'corpus', 'fixtures', 'SAVE76.DAT');
const KEY = 0x1234abcd;
const op = (type, raw) => ({ type, raw });
const OPS = [op(3, 0), op(0, 76), op(3, 1), op(3, 2), op(3, 3), op(3, 4), op(3, 5), op(3, 6), op(3, 7)];

// LFS 指针只有一百多字节；它和"文件不在场"一样，不能当真样本用。
const present = fs.existsSync(FIXTURE) && fs.statSync(FIXTURE).size >= 292;
const skip = present ? false : 'corpus/fixtures/SAVE76.DAT 不在场（或只是 LFS 指针）';

test('★ SAVE76.DAT 槽头读取：0x1a0 的 op3..9 = [2026,9,23,10,34,58,170]', { skip }, async () => {
  const bytes = fs.readFileSync(FIXTURE);
  assert.equal(bytes.subarray(0, 4).toString('latin1'), 'S4SD');
  // 判别力：该样本的星期与日、毫秒与秒都不同 ⇒ 若实现读错一个字段，期望值会当场对不上。
  assert.equal(bytes.readUInt16LE(268), 3, '样本头 +268 是星期');
  assert.equal(bytes.readUInt16LE(278), 627, '样本头 +278 是毫秒');

  const m = new Machine({
    body: Buffer.alloc(4),
    table: new Map(),
    key: KEY,
    saveHost: { fileForSlot: (slot, kind) => `SAVE${slot}.${kind}`, read: async () => bytes },
  });
  await HANDLERS.get(0x1a0)(m, OPS);
  assert.equal(readInt(m, OPS, 1), 0);
  assert.deepEqual(
    [1, 2, 3, 4, 5, 6, 7].map((k) => readInt(m, OPS, 2 + k)),
    [2026, 9, 23, 10, 34, 58, 170],
  );
});
