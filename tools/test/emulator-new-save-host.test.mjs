/** @env pure @kind contract @why 槽号拼名 / 写读 / 改名的实现错一处，真实存档就会落到别的文件或静默读空 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';

import { NodeSaveHost } from '../../apps/emulator-new/src/platform/node/save-host.ts';

test('★ NodeSaveHost：SAVE<至少两位>.DAT/.STH；write → read → copy → rename → remove', async () => {
  const dir = await fs.mkdtemp(path.join(os.tmpdir(), 'amayui-save-'));
  try {
    const h = new NodeSaveHost(dir);
    assert.equal(path.basename(h.fileForSlot(3, 'dat')), 'SAVE03.DAT');
    assert.equal(path.basename(h.fileForSlot(123, 'sth')), 'SAVE123.STH');
    assert.equal(path.basename(h.fileForSlot(-2, 'dat')), 'SAVE-02.DAT');

    const dat = h.fileForSlot(3, 'dat');
    assert.equal(await h.read(dat), null, '不存在 ⇒ null，不是空字节');
    assert.equal(await h.write(dat, new Uint8Array([1, 2, 3])), true);
    assert.deepEqual([...(await h.read(dat))], [1, 2, 3]);

    const copy = h.fileForSlot(4, 'dat');
    assert.equal(await h.copy(dat, copy), true);
    assert.deepEqual([...(await h.read(copy))], [1, 2, 3]);

    const moved = h.fileForSlot(5, 'dat');
    assert.equal(await h.rename(copy, moved), true);
    assert.equal(await h.read(copy), null);
    assert.deepEqual([...(await h.read(moved))], [1, 2, 3]);

    assert.equal(await h.remove(dat), true);
    assert.equal(await h.read(dat), null);
    assert.equal(await h.remove(dat), false, '删不存在的文件 ⇒ false');
  } finally {
    await fs.rm(dir, { recursive: true, force: true });
  }
});
