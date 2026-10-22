/** @env assets @kind contract @why `30-agf` 的协议断言与真实 AGF 脱节（24 字节头 / meta·body 12 字节块 / v2 的 ACIF 紧接 body / 像素字段与行宽），读者会按错的字节结构写解码器 */
/**
 * tools/test/spec-agf.assets.test.mjs —— **AGF 图像的语料侧守卫**
 *
 * 把 `data/spec/chapters/30-agf.md` 的断言落到安装目录里的真实 `*.AGF` 上：
 * ① 24 字节头（`ACGF` / version / unknown0=0 / meta 长度对）；② meta 之后的 12 字节 body 头；
 * ③ version 2 的 body 之后**紧接** `ACIF`（24 字节头 + 12 字节长度块），宽高与 meta 相同；
 * ④ 像素字段（meta+20/+24 宽高、meta+30 bpp、meta+56 调色板槽、行宽按 32 位对齐）。
 *
 * ★ 不证明"与引擎等价"：只证明"按本章的字节结构，真实语料能逐条解释通"。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { loadManifest } from '../lib/manifest.mjs';
import { lzssDecode } from './spec-lzss.mjs';

const u16 = (b, o) => b.readUInt16LE(o);
const u32 = (b, o) => b.readUInt32LE(o);

/** 相等 ⇒ 原样；否则 LZSS（AGF 的 meta / body / alpha 三段同一条规则） */
function section(bytes, unpacked, packed, what, file) {
  const n = Math.min(packed, bytes.length);
  if (unpacked === packed) return bytes.subarray(0, n);
  const res = lzssDecode(bytes.subarray(0, n), unpacked);
  assert.equal(res.written, unpacked, `${file}: ${what} 解压长度与头里的 unpacked 不一致`);
  return Buffer.from(res.out);
}

test('★ 真实 *.AGF：头 24 / meta·body 12 字节块 / v2 的 ACIF 紧接 body / 像素字段', (t) => {
  const { roots } = loadManifest();
  const dir = roots?.gameInstall;
  if (!dir || !fs.existsSync(dir)) { t.skip('gameInstall 不在本机'); return; }
  const names = fs.readdirSync(dir).filter((f) => /\.agf$/i.test(f));
  assert.ok(names.length > 0, '安装目录里没有 *.AGF');
  let checked = 0;
  let v2 = 0;
  let alphaTotal = 0;
  for (const name of names) {
    if (checked >= 10) break;
    const bytes = fs.readFileSync(path.join(dir, name));
    if (bytes.length < 36) continue;
    // ① 24 字节头
    assert.equal(bytes.toString('latin1', 0, 4), 'ACGF', `${name}: 签名不是 ACGF`);
    const version = u32(bytes, 4);
    assert.ok(version === 1 || version === 2, `${name}: version 不在 {1,2}`);
    assert.equal(u32(bytes, 8), 0, `${name}: unknown0 必须为 0`);
    const metaUnpacked = u32(bytes, 12);
    assert.equal(u32(bytes, 16), metaUnpacked, `${name}: meta 长度没有重复两次`);
    const metaPacked = u32(bytes, 20);
    const metaStart = 24;
    assert.ok(metaStart + metaPacked <= bytes.length, `${name}: meta 越出文件`);
    // ② meta
    const meta = section(bytes.subarray(metaStart, metaStart + metaPacked), metaUnpacked, metaPacked, 'meta', name);
    assert.equal(meta.length, metaUnpacked, `${name}: meta 长度与头不符`);
    const width = u32(meta, 20);
    const height = u32(meta, 24);
    const bpp = u16(meta, 30);
    assert.ok(width > 0 && height > 0, `${name}: 宽高为 0（本版不规定，但语料里不应出现）`);
    assert.ok([8, 24, 32].includes(bpp), `${name}: bpp=${bpp} 不在 {8,24,32}`);
    // ③ body 头 12 字节
    const bodyHdr = metaStart + metaPacked;
    assert.ok(bodyHdr + 12 <= bytes.length, `${name}: 没有 12 字节 body 头`);
    const bodyUnpacked = u32(bytes, bodyHdr + 4);
    const bodyPacked = u32(bytes, bodyHdr + 8);
    const bodyStart = bodyHdr + 12;
    assert.ok(bodyStart + bodyPacked <= bytes.length, `${name}: body 越出文件`);
    const body = section(bytes.subarray(bodyStart, bodyStart + bodyPacked), bodyUnpacked, bodyPacked, 'body', name);
    assert.equal(body.length, bodyUnpacked, `${name}: body 长度与头不符`);
    // 行按 32 位（4 字节）对齐：stride = 4 * ceil(bpp * width / 32)，body 必须正好容纳 height 行
    const stride = 4 * Math.ceil((bpp * width) / 32);
    assert.equal(stride, Math.floor(stride), `${name}: 行宽不是整字节`);
    assert.equal(body.length, stride * height, `${name}: body 长度 ${body.length} ≠ stride ${stride} × height ${height}`);
    if (bpp === 8) {
      assert.ok(meta.length >= 56 + 1024, `${name}: 8bpp 但 meta 没有 56+1024 的调色板槽`);
    }
    // ④ v2：ACIF 紧接 body（中间没有填充）
    const acif = bodyStart + bodyPacked;
    if (version === 2) {
      v2 += 1;
      assert.equal(bytes.toString('latin1', acif, acif + 4), 'ACIF', `${name}: v2 但 body 之后不是 ACIF`);
      const av = u32(bytes, acif + 4);
      assert.ok(av === 1 || av === 2, `${name}: ACIF version 不在 {1,2}`);
      assert.equal(u32(bytes, acif + 8), 0, `${name}: ACIF 的 +8 必须为 0`);
      const alphaUnpacked = u32(bytes, acif + 12);
      assert.equal(u32(bytes, acif + 16), width, `${name}: ACIF width 与 meta 不符`);
      assert.equal(u32(bytes, acif + 20), height, `${name}: ACIF height 与 meta 不符`);
      assert.equal(u32(bytes, acif + 24), alphaUnpacked, `${name}: alpha 长度没有重复两次（+24）`);
      assert.equal(u32(bytes, acif + 28), alphaUnpacked, `${name}: alpha 长度没有重复两次（+28）`);
      const alphaPacked = u32(bytes, acif + 32);
      const alphaStart = acif + 36;
      assert.ok(alphaStart + alphaPacked <= bytes.length, `${name}: alpha 越出文件`);
      const alpha = section(bytes.subarray(alphaStart, alphaStart + alphaPacked), alphaUnpacked, alphaPacked, 'alpha', name);
      assert.equal(alpha.length, width * height, `${name}: 使用端按 宽×高 逐行拷贝 ⇒ alpha 必须正好 ${width * height} 字节`);
      alphaTotal += alpha.length;
    } else {
      assert.notEqual(bytes.toString('latin1', acif, acif + 4), 'ACIF', `${name}: v1 不应读 alpha`);
    }
    checked += 1;
  }
  assert.ok(checked >= 3, `只检查到 ${checked} 个 AGF —— 语料/安装目录不对？`);
  assert.ok(v2 > 0, '没有 version 2 的样本 ⇒ ACIF 分支无从验证');
  t.diagnostic(`AGF 抽检 ${checked} 份 · v2 ${v2} · alpha 总字节 ${alphaTotal} · 全部通过`);
});
