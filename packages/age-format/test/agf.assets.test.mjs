/** @env assets @kind contract @why AGF 往返不再逐字节相同 / 分段压缩或像素解码口径被改坏 */
/**
 * packages/age-format/test/agf.assets.test.mjs —— AGF（ACGF）的**往返判据** + 分段压缩规则自洽
 *
 * ★ 样本不入库（原始游戏文件）：缺席即**如实 skip**。三个样本是按"覆盖不同分支"选的，见 `corpus/assets/samples.md`：
 *   `MI042.AGF` meta 压缩形态 / `MI040.AGF` meta 原样形态 / `SO002.AGF` 更小的一张。
 * ★ 从 `agf.test.mjs` 拆出来的原因（本轮测试分级）：这八条都要样本在场 ⇒ `@env assets`。
 * ★ 全程纯 JS，不 spawn；PNG 编解码不在这里（格式层只承诺容器 + 像素排布）。
 *
 * 运行：`pnpm test:assets`
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
  readAgfBuffer,
  writeAgf,
  roundTripEqual,
  decodeRgba,
  encodeBody,
  alphaFromRgba,
  extractPaletteRgb,
  packSection,
  lzssCompress,
  lzssDecompress,
  strideFor,
  sizeFor,
} from '../src/agf.mts';
import { loadSample, fileOf, sha256 } from './samples.mjs';

const sample = loadSample('assets/samples-agf');
if (!sample) console.log('[AGF] 跳过：样本不在场（原始游戏文件不入库，见 corpus/assets/samples.md）');
const skip = sample ? false : '样本不在场（游戏文件不入库）';

const NAMES = ['MI042.AGF', 'MI040.AGF', 'SO002.AGF'];

test('AGF：三个样本读进来再写出去，逐字节相同', { skip }, () => {
  for (const name of NAMES) {
    const f = fileOf(sample, name);
    const agf = readAgfBuffer(f.buf, f.abs);
    assert.ok(roundTripEqual(agf), `${name} 必须逐字节相同`);
    assert.equal(sha256(writeAgf(agf)), f.sha256, `${name} 与清单登记 sha256 一致`);
  }
});

test('AGF：头部三段尺寸口径（+12/+16 是未压缩、+20 才是压缩后）', { skip }, () => {
  const f = fileOf(sample, 'MI042.AGF');
  const agf = readAgfBuffer(f.buf, f.abs);
  assert.equal(f.buf.readInt32LE(12), agf.metaUnpackedSize, '+12 = 未压缩大小');
  assert.equal(f.buf.readInt32LE(16), agf.metaUnpackedSize, '+16 = 未压缩大小（与 +12 同值）');
  assert.equal(f.buf.readInt32LE(20), agf.metaPackedSize, '+20 = 压缩后大小');
  assert.notEqual(agf.metaUnpackedSize, agf.metaPackedSize, '这张样本正是"meta 被压缩"的那条分支');
  // 写回时必须保持这个口径：写成压缩后大小 → 长度对、内容错（这就是当初抓到 MI042/MI047 的判据）
  const out = writeAgf(agf);
  assert.equal(out.readInt32LE(12), agf.metaUnpackedSize);
  assert.equal(out.readInt32LE(20), agf.metaPackedSize);
});

test('AGF：★ 游戏**自己打包**的段：用本包压缩器复算必须逐字节相同', { skip }, () => {
  // ★ 口径限定（实测得出，见 corpus/assets/samples.md）：这条规则对"游戏自身打包"的件成立；
  //   旧仓注入生成的件会把 body 直接写**未压缩**（哪怕压得小），那是那条工具链的选择，不是格式规则。
  const cases = [
    ['MI042.AGF', 'meta'],
    ['MI042.AGF', 'body'],
    ['MI042.AGF', 'alpha'],
    ['MI040.AGF', 'body'],
    ['MI040.AGF', 'alpha'],
  ];
  for (const [name, label] of cases) {
    const f = fileOf(sample, name);
    const agf = readAgfBuffer(f.buf, f.abs);
    const seg = () => {
      if (label === 'meta') return [agf.meta, agf.metaRaw];
      if (label === 'body') return [agf.body, agf.bodyRaw];
      return [agf.acif.alpha, agf.acif.alphaRaw];
    };
    const [data, stored] = seg();
    assert.ok(stored.length !== data.length, `${name} ${label} 这一份应当是压缩存放的`);
    const packed = lzssCompress(data);
    assert.equal(packed.length, stored.length, `${name} ${label} 压缩后长度必须与盘上一致`);
    assert.ok(packed.equals(stored), `${name} ${label} 压缩结果必须与盘上逐字节一致`);
    const sec = packSection(data);
    assert.ok(sec.raw.equals(stored));
    assert.equal(sec.compressed, true);
  }
});

test('AGF：未压缩存放的段位（`packed === unpacked` ⇒ 原样，不再试着解压）', { skip }, () => {
  const f = fileOf(sample, 'SO002.AGF');
  const agf = readAgfBuffer(f.buf, f.abs);
  assert.equal(agf.bodyUnpackedSize, agf.bodyPackedSize, 'SO002 的 body 是原样存放');
  assert.ok(agf.body.equals(agf.bodyRaw), '原样存放时"解压结果"就是那一段原始字节');
  assert.equal(agf.acif.alphaUnpackedSize, agf.acif.alphaPackedSize, 'ACIF alpha 也是原样存放');
  assert.ok(agf.acif.alpha.equals(agf.acif.alphaRaw));
});

test('AGF：meta 原样的那张（MI040）压缩后确实**变大**（所以原样才是对的）', { skip }, () => {
  const f = fileOf(sample, 'MI040.AGF');
  const agf = readAgfBuffer(f.buf, f.abs);
  assert.equal(agf.metaUnpackedSize, agf.metaPackedSize, '这张的 meta 是原样存放');
  assert.ok(lzssCompress(agf.meta).length > agf.meta.length, '重压应当变大 —— 否则"原样"这个判定就不成立');
});

test('AGF：像素解码的尺寸 / bpp / stride 自洽，且 alpha 长度 = w*h', { skip }, () => {
  for (const name of NAMES) {
    const f = fileOf(sample, name);
    const agf = readAgfBuffer(f.buf, f.abs);
    const img = decodeRgba(agf);
    assert.equal(img.width, agf.width);
    assert.equal(img.height, agf.height);
    assert.equal(img.rgba.length, agf.width * agf.height * 4);
    assert.ok([8, 24, 32].includes(agf.bpp), `${name} bpp=${agf.bpp}`);
    assert.equal(agf.body.length, sizeFor(agf.width, agf.height, agf.bpp) || agf.body.length);
    assert.ok(strideFor(agf.width, agf.bpp) >= agf.width * (agf.bpp / 8));
    if (agf.acif) assert.equal(agf.acif.alpha.length, agf.width * agf.height, 'ACIF alpha 长度必须是 w*h');
  }
});

test('AGF：★ 改一个像素再打包 → 回读解出来就是改后的值（不是"只会原样抄回去"）', { skip }, () => {
  const f = fileOf(sample, 'SO002.AGF');
  const agf = readAgfBuffer(f.buf, f.abs);
  const img = decodeRgba(agf);
  const palette = agf.bpp === 8 ? extractPaletteRgb(agf.meta) : null;

  // 8bpp 是**调色板量化**（编码只能写"最接近的调色板项"）⇒ 断言必须用调色板里真实存在的一项，
  // 否则量化的"最近色"会让断言假红（实测：纯红 (255,0,0) 被量化成 (253,0,0)）。
  const target = palette ? palette[17] : [255, 0, 0];
  const edited = Buffer.from(img.rgba);
  edited[0] = target[0];
  edited[1] = target[1];
  edited[2] = target[2];
  edited[3] = 255;

  const body = encodeBody({ width: img.width, height: img.height, rgba: edited }, agf.bpp, palette);
  assert.equal(body.length, agf.body.length, '同尺寸重编码后 body 长度必须不变');
  const alpha = agf.acif ? alphaFromRgba({ width: img.width, height: img.height, rgba: edited }) : null;

  const bodySec = packSection(body);
  const out = writeAgf(agf, {
    bodyRaw: bodySec.raw,
    bodyUnpackedSize: bodySec.unpackedSize,
    ...(alpha ? { alphaRaw: packSection(alpha).raw, alphaUnpackedSize: alpha.length } : {}),
  });
  // 新文件必须能被自己读回来，且 (0,0) 就是我们写进去的那个颜色
  const re = readAgfBuffer(out, '<memory>');
  const reImg = decodeRgba(re);
  assert.equal(reImg.rgba[0], target[0]);
  assert.equal(reImg.rgba[1], target[1]);
  assert.equal(reImg.rgba[2], target[2]);
  assert.equal(reImg.rgba[3], 255, 'alpha 也要按改后的值生效');
  // 未改动的部分不该被牵连
  assert.equal(re.width, agf.width);
  assert.equal(re.height, agf.height);
  assert.equal(re.bpp, agf.bpp);
});

test('AGF：LZSS 两个方向互逆（AGF 族的 +3 参数，别与 ALF 族的 +2 混用）', { skip }, () => {
  const f = fileOf(sample, 'MI042.AGF');
  const agf = readAgfBuffer(f.buf, f.abs);
  const back = lzssDecompress(agf.bodyRaw, agf.bodyUnpackedSize);
  assert.ok(back.equals(agf.body), 'body 解压必须与已解压的结果一致');
  assert.ok(lzssCompress(agf.body).equals(agf.bodyRaw), 'body 压缩必须与盘上一致');
});
