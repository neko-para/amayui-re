/** @env assets @kind contract @why `0x1af` 对真机 `.STH` 的读取路径塌了（BMP 判别 / 状态码 / 上传请求），存档缩略图读不回来而合成守卫仍绿；★ 「整份字节」与「截断 ⇒ 2」这两条都是**实现现状**，不是引擎合同（RES-SAVE-STH-CAPTURE / RES-SAVE-STH-SHORTREAD） */
/**
 * tools/test/emulator-new-save-sth.assets.test.mjs —— `0x1af` 对**真实 `.STH`** 的实现守卫
 *
 * ★ **本文件不是 `FMT-SAVE-STH` 的合同证据**（合同证据只看 `tools/test/spec-save-sth.assets.test.mjs`）：
 *   实现把整份 BMP 字节放在平台侧（`RenderHost.captureTexture` 返回整份文件、`uploadTextureBytes` 收整份文件），
 *   而合同说容器由**引擎**组装 / 解析、平台只提供像素 + 行宽 + 通道掩码 ⇒ 这里断言的是**实现现状**，
 *   分歧已登记为 `RES-SAVE-STH-CAPTURE`（what ② / next ②）。搬回 core 之后这些断言要跟着删 / 改。
 *
 * 判据（前三条 = 实现行为，≠ 引擎合同）：
 * ① 真机样本（`corpus/fixtures/SAVE76.STH`）能被 `0x1af` 读进来：状态 0（这是合同：内容可解）；
 * ② 交给上传的**是整份文件字节**（长度与逐字节相同）—— ★ **实现现状，不是合同证据**；
 * ③ 同一个槽号在文件名里是 `SAVE76.STH`（至少两位、左补 0 的规则在真实槽号上仍成立）；
 * ④ 只有 40 字节（不足两段头）的截断文件 ⇒ 实现给状态 2 —— ★ **实现现状，不是引擎合同**：
 *   引擎对每次读取只看 `ReadFile` 的布尔返回值、**不比较**实际读到的字节数，短读行为本版不规定
 *   （`RES-SAVE-STH-SHORTREAD`）。这条守的是"实现自己的截断判别别退化"，⛔ 不许拿它当合同证据。
 * ★ 样本缺席（或只有 LFS 指针）⇒ skip，不是红。
 * ★ 合成字节的空值 / 失败路径在 `emulator-new-ops-100-200.test.mjs`；本条只负责"真实字节"。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';
import { Machine } from '../../apps/emulator-new/src/core/machine.ts';
import { HANDLERS } from '../../apps/emulator-new/src/core/handlers.ts';
import { readInt } from '../../apps/emulator-new/src/core/values.ts';

const FIXTURE = path.join(REPO_ROOT, 'corpus', 'fixtures', 'SAVE76.STH');
const KEY = 0x1234abcd;
const op = (type, raw) => ({ type, raw });
const OPS = [op(3, 0), op(0, 76), op(0, 9)];

// LFS 指针只有一百多字节；它和"文件不在场"一样，不能当真样本用。
const present = fs.existsSync(FIXTURE) && fs.statSync(FIXTURE).size > 4096;
const skip = present ? false : 'corpus/fixtures/SAVE76.STH 不在场（或只是 LFS 指针）';

/** 只实现 `0x1af` 用到的那两个请求；上传请求把字节原样收下（不解释、不解码）。 */
function hostWith(bytes) {
  const uploads = [];
  return {
    uploads,
    renderHost: {
      uploadTextureBytes: async (textureId, got) => {
        uploads.push({ textureId, bytes: got });
        return true;
      },
    },
    saveHost: {
      fileForSlot: (slot, kind) => path.join('save-dir', `SAVE${String(slot).padStart(2, '0')}.${kind.toUpperCase()}`),
      read: async (file) => (file.endsWith('SAVE76.STH') ? bytes : null),
    },
  };
}

// ★「整份字节」= 实现现状（RES-SAVE-STH-CAPTURE what ②）—— 不是引擎合同：合同要求 core 自建容器。
test('★ [实现现状] 0x1af 读真实 SAVE76.STH：状态 0，整份字节交给上传请求（≠ 合同证据）', { skip }, async () => {
  const bytes = fs.readFileSync(FIXTURE);
  // 先确认我们拿到的真是 BMP（否则这条守的是别的东西）：两段头 = 54 字节，像素块非空。
  assert.equal(bytes.subarray(0, 2).toString('latin1'), 'BM');
  assert.ok(bytes.length > 54, '样本必须带像素块');

  const h = hostWith(bytes);
  const m = new Machine({ body: Buffer.alloc(4), table: new Map(), key: KEY, ...h });
  await HANDLERS.get(0x1af)(m, OPS);

  assert.equal(readInt(m, OPS, 1), 0, '真实 .STH ⇒ 状态 0（合同：内容可解）');
  assert.equal(h.uploads.length, 1, '恰好一次上传请求');
  assert.equal(h.uploads[0].textureId, 9, '第 3 个操作数 = 上传目标槽');
  // ★ 下面两条是**实现现状**：实现把整份 BMP 字节原样过一遍平台（见文件头）。
  assert.equal(h.uploads[0].bytes.length, bytes.length, '实现现状：上传的是整份文件（长度相同）');
  assert.equal(Buffer.compare(Buffer.from(h.uploads[0].bytes), bytes), 0, '实现现状：上传的还是同一串字节（无重编码）');
});

// ★「截断 ⇒ 2」= 实现现状（RES-SAVE-STH-SHORTREAD）—— 引擎只看 ReadFile 布尔、不比较字节数，
//   所以「读不满就失败」不是引擎合同；这条只防实现自己的两段头判别退化。
test('★ [实现现状] 0x1af 读只剩 40 字节的 .STH：实现给状态 2 且不发起上传（引擎短读行为本版不规定）', { skip }, async () => {
  const bytes = fs.readFileSync(FIXTURE);
  const truncated = bytes.subarray(0, 40); // 有 BM、不够 14+40 两段头
  const h = hostWith(truncated);
  const m = new Machine({ body: Buffer.alloc(4), table: new Map(), key: KEY, ...h });
  await HANDLERS.get(0x1af)(m, OPS);

  assert.equal(readInt(m, OPS, 1), 2, '实现现状：两段头不齐 ⇒ 状态 2（⛔ 不是引擎合同：短读未取证）');
  assert.equal(h.uploads.length, 0, '实现现状：不可解的文件不进上传请求');
});
