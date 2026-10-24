/** @env assets @kind contract @why `0x1af` 对真机 `.STH` 的读取路径塌了（容器解析 / 状态码 / 上传请求形状），缩略图读不回来而合成守卫仍绿；容器编码若改坏（声明 +40 / 行宽 / 行序 / BGR），真机样本就再也编不回原字节；★ 「短读 ⇒ 2」是**实现口径**，不是引擎合同（RES-SAVE-STH-SHORTREAD） */
/**
 * tools/test/emulator-new-save-sth.assets.test.mjs —— `0x1af` 对**真实 `.STH`** 的实现守卫
 *
 * ★ **本文件不是 `FMT-SAVE-STH` 的合同证据**（合同证据只看 `tools/test/spec-save-sth.assets.test.mjs`）：
 *   这里断言的是**实现**——core 按容器解析真实字节，再把**像素**交给平台（`uploadTexture`）。
 *   容器的组装 / 解析在 `apps/emulator-new/src/core/bmp.ts`；平台只给「像素 + 行宽 + 通道掩码」。
 *
 * 判据：
 * ① 真机样本（`corpus/fixtures/SAVE76.STH`）能被 `0x1af` 读进来：状态 0（这是合同：内容可解）；
 * ② 交给平台的是一次 `uploadTexture`，尺寸 = 320 × 180、`rgba.length = 320 × 180 × 4`（RGBA8）；
 * ③ 同一个槽号在文件名里是 `SAVE76.STH`（至少两位、左补 0 的规则在真实槽号上仍成立）；
 * ④ 只有 40 字节（不足两段头）⇒ 实现给状态 2 —— ★ **实现口径，不是引擎合同**：
 *   引擎对每次读取只看 `ReadFile` 的布尔返回值、**不比较**实际读到的字节数，短读行为本版不规定
 *   （`RES-SAVE-STH-SHORTREAD`）。这条守的是"实现自己的截断判别别退化"，⛔ 不许拿它当合同证据。
 * ⑤ 容器**可逆**：真机样本解码后再编码必须逐字节还原（钉住 +40 / 行宽 / 行序 / BGR / 填充）。
 *   ⛔ 这不是"像素等价"声明，只断言容器与像素布局能还原这些样本。
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
import { decodeBmp, encodeBmp24 } from '../../apps/emulator-new/src/core/bmp.ts';

const FIXTURE = path.join(REPO_ROOT, 'corpus', 'fixtures', 'SAVE76.STH');
const KEY = 0x1234abcd;
const op = (type, raw) => ({ type, raw });
const OPS = [op(3, 0), op(0, 76), op(0, 9)];

// LFS 指针只有一百多字节；它和"文件不在场"一样，不能当真样本用。
const present = fs.existsSync(FIXTURE) && fs.statSync(FIXTURE).size > 4096;
const skip = present ? false : 'corpus/fixtures/SAVE76.STH 不在场（或只是 LFS 指针）';

/** 只实现 `0x1af` 用到的那两个请求：读文件 + 收 RGBA8 上传（不解释字节）。 */
function hostWith(bytes) {
  const uploads = [];
  return {
    uploads,
    renderHost: {
      uploadTexture: async (request) => {
        uploads.push(request);
        return true;
      },
    },
    saveHost: {
      fileForSlot: (slot, kind) => path.join('save-dir', `SAVE${String(slot).padStart(2, '0')}.${kind.toUpperCase()}`),
      read: async (file) => (file.endsWith('SAVE76.STH') ? bytes : null),
    },
  };
}

test('★ 0x1af 读真实 SAVE76.STH：状态 0，一次 uploadTexture（320 × 180 RGBA8）', { skip }, async () => {
  const bytes = fs.readFileSync(FIXTURE);
  // 先确认我们拿到的真是 24 位 BMP（否则这条守的是别的东西）：两段头 = 54 字节，像素块非空。
  assert.equal(bytes.subarray(0, 2).toString('latin1'), 'BM');
  assert.equal(bytes.readUInt16LE(28), 24, '样本是 24 位');
  assert.ok(bytes.length > 54, '样本必须带像素块');

  const h = hostWith(bytes);
  const m = new Machine({ body: Buffer.alloc(4), table: new Map(), key: KEY, ...h });
  await HANDLERS.get(0x1af)(m, OPS);

  assert.equal(readInt(m, OPS, 1), 0, '真实 .STH ⇒ 状态 0（合同：内容可解）');
  assert.equal(h.uploads.length, 1, '恰好一次上传请求');
  const upload = h.uploads[0];
  assert.equal(upload.textureId, 9, '第 3 个操作数 = 上传目标槽');
  // ★ 交给平台的是**像素**（core 已解容器），不是整份文件字节。
  assert.equal(upload.width, 320, '宽取自文件头（本作样本恒 320）');
  assert.equal(upload.height, 180, '高取自文件头（本作样本恒 180）');
  assert.equal(upload.rgba.length, 320 * 180 * 4, 'RGBA8：320 × 180 × 4');
  assert.equal(upload.color, 0, '读档路径没有颜色键 ⇒ 无键哨兵');
});

// ★「截断 ⇒ 2」= 实现口径（RES-SAVE-STH-SHORTREAD）—— 引擎只看 ReadFile 布尔、不比较字节数，
//   所以「读不满就失败」不是引擎合同；这条只防实现自己的两段头判别退化。
test('★ [实现口径] 0x1af 读只剩 40 字节的 .STH：实现给状态 2 且不发起上传（引擎短读行为本版不规定）', { skip }, async () => {
  const bytes = fs.readFileSync(FIXTURE);
  const truncated = bytes.subarray(0, 40); // 有 BM、不够 14+40 两段头
  const h = hostWith(truncated);
  const m = new Machine({ body: Buffer.alloc(4), table: new Map(), key: KEY, ...h });
  await HANDLERS.get(0x1af)(m, OPS);

  assert.equal(readInt(m, OPS, 1), 2, '实现口径：解码器判头短 ⇒ 状态 2（⛔ 不是引擎合同：短读未取证）');
  assert.equal(h.uploads.length, 0, '不可解的文件不进上传请求');
  assert.ok(m.diagnostics.some((d) => d.kind === 'bmp-header-short'), '诊断点名解码器的头短判据');
});

// ★ 容器**可逆**：真机样本解出来的像素用 RGBA8888 掩码再编码，必须逐字节还原原文件。
//   这同时钉住声明的 +40 / 行宽对齐 / 自下而上 / 蓝绿红 / 行末填充（任一处写错都对不上真机字节）。
//   ⛔ 它**不是**"像素等价"声明：只断言容器与像素布局能还原这四份样本，不涉及渲染结果。
test('★ 容器可逆：真机 .STH 解码后按 RGBA8888 掩码重新编码 ⇒ 与原文件逐字节相同', { skip }, () => {
  for (const name of ['SAVE76.STH', 'SAVE79.STH']) {
    const file = path.join(path.dirname(FIXTURE), name);
    const original = fs.readFileSync(file);
    const decoded = decodeBmp(original);
    const again = encodeBmp24({
      pixels: decoded.rgba,
      width: decoded.width,
      height: decoded.height,
      stride: decoded.width * 4,
      bytesPerPixel: 4,
      masks: { r: 0xff, g: 0xff00, b: 0xff0000, a: 0xff000000 },
    });
    assert.equal(
      Buffer.compare(Buffer.from(again), original),
      0,
      `${name}：解码 → 重新编码必须逐字节还原（声明长度 +40 / 行宽 / 行序 / BGR / 填充）`,
    );
  }
});
