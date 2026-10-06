/** @env pure @kind gate @why 字体链的落点/版本判据被破坏 */
/**
 * tools/test/fonts.test.mjs —— cnjp 字体构建链的守卫。
 *
 * ★ 测的是**契约**（判据能不能红 / 序列化口径 / 码页位集合），不测业务取值。
 * ★ 基底（7z 解压产物）或可信产物不在场时按本仓惯例直接 return —— 不假装通过、也不红。
 */
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import {
  FAMILY,
  PS_NAME,
  buildCnjp,
  calcCodePageRanges,
  codePageRangeValues,
  compileCmap,
  fontProblems,
  parseSfnt,
  readCmap,
  renameFont,
  sortedTagList,
} from '../lib/fonts.mjs';

const BASE = { regular: 'corpus/assets/fonts/SarasaGothicSC/SarasaGothicSC-Regular.ttf', bold: 'corpus/assets/fonts/SarasaGothicSC/SarasaGothicSC-Bold.ttf' };
const REF = { regular: 'corpus/assets/fonts/Amayui-CN_cnjp.ttf', bold: 'corpus/assets/fonts/Amayui-CN_cnjp-Bold.ttf' };
const DICT = 'data/translations/subs-cn-jp.json';
const has = (p) => fs.existsSync(p);
const ready = (w) => has(BASE[w]) && has(REF[w]) && has(DICT);

test('★ 判据：两支的构建结果与可信产物**除 head.modified（时间戳）外逐字节相同**', () => {
  for (const w of ['regular', 'bold']) {
    if (!ready(w)) return; // 基底未解压 / 基准未拉下来 ⇒ 跳过
    const dict = JSON.parse(fs.readFileSync(DICT, 'utf8'));
    const built = buildCnjp({ baseBuf: fs.readFileSync(BASE[w]), dict }).buf;
    const reference = fs.readFileSync(REF[w]);
    assert.deepEqual(fontProblems({ built, reference }), [], `${w}：归一化时间戳后应逐字节相同`);
    // 不归一化时**只允许**时间戳那三处（head 的表校验和 4B + checkSumAdjustment 4B + modified 低 3B）
    const diffs = [...built.keys()].filter((i) => built[i] !== reference[i]);
    assert.ok(diffs.length <= 11, `${w}：不该有 ${diffs.length} 个字节不同（至多 11 个都由时间戳派生）`);
  }
});

test('★ 判据红得有意义：cmap 里改一个字节就必须报出来（不是空守卫）', () => {
  if (!ready('regular')) return;
  const dict = JSON.parse(fs.readFileSync(DICT, 'utf8'));
  const built = buildCnjp({ baseBuf: fs.readFileSync(BASE.regular), dict }).buf;
  const reference = fs.readFileSync(REF.regular);
  assert.deepEqual(fontProblems({ built, reference }), []);
  const tampered = Buffer.from(built);
  const cmap = parseSfnt(tampered).byTag.get('cmap');
  tampered[cmap.offset + 64] ^= 0xff;
  assert.ok(fontProblems({ built: tampered, reference }).length > 0, '改了 cmap 却报 0 处 ⇒ 守卫是假的');
});

test('族名改写的口径：id3/id4 与 id6 在 Regular 下省略子族名、Bold 下带上', () => {
  const mk = (nameID, text) => ({ platformID: 3, platEncID: 1, langID: 1033, nameID, raw: Buffer.from(text, 'utf16le').swap16() });
  for (const [sub, full, ps] of [['Regular', FAMILY, PS_NAME], ['Bold', `${FAMILY} Bold`, `${PS_NAME}-Bold`]]) {
    const recs = [mk(1, 'X'), mk(2, sub), mk(3, `X ${sub}`), mk(4, `X ${sub}`), mk(6, 'X'), mk(16, 'X'), mk(17, sub)];
    renameFont(recs, { family: FAMILY, psName: PS_NAME });
    const text = (id) => recs.find((r) => r.nameID === id).raw.swap16().toString('utf16le');
    assert.equal(text(1), FAMILY);
    assert.equal(text(16), FAMILY);
    assert.equal(text(3), full);
    assert.equal(text(4), full);
    assert.equal(text(6), ps);
    assert.equal(text(2), sub, '子族名不该被改');
  }
});

test('码页位集合：探针字符 → 位号（932 = bit 17），且 range1/2 的切分正确', () => {
  const bits = (s) => calcCodePageRanges(new Set([...s].map((c) => c.codePointAt(0))));
  assert.ok(bits('エ').has(17), 'エ ⇒ JIS/Japan 932');
  assert.ok(bits('ㄅ').has(18));
  assert.ok(bits('ㄱ').has(19));
  assert.ok(bits('央').has(20));
  assert.ok(bits('곴').has(21));
  assert.ok(bits('ๅ').has(16));
  assert.ok(!bits('エ').has(17 + 32), '不该跑到 range2');
  // bit 0（Latin 1）要求 ASCII 齐全；bit 29 还要 ‰ 与 ∑
  const ascii = [...Array(0x5f).keys()].map((k) => 0x20 + k);
  assert.ok(calcCodePageRanges(new Set([...ascii, 0xde])).has(0));
  assert.ok(!calcCodePageRanges(new Set([0xde])).has(0), 'ASCII 不齐时 bit 0 不该置位');
  const { range1, range2 } = codePageRangeValues(new Set([0, 17, 30, 33, 63]));
  assert.equal(range1, (1 | (1 << 17) | (1 << 30)) >>> 0);
  assert.equal(range2, ((1 << 1) | (1 << 31)) >>> 0);
});

test('cmap 编译可回读：映射逐码位不变，且**字节相同的子表共享偏移**', () => {
  const cmap = new Map([[0x41, 3], [0x42, 4], [0x43, 5], [0x4e00, 900], [0x4e02, 902]]);
  const sub = { platformID: 3, platEncID: 1, format: 4, language: 0, cmap, raw: null };
  const twin = { platformID: 0, platEncID: 3, format: 4, language: 0, cmap: new Map(cmap), raw: null };
  const buf = compileCmap([sub, twin]);
  const rec = { offset: 0, length: buf.length };
  const back = readCmap(buf, rec);
  for (const s of back) {
    assert.deepEqual([...s.cmap.entries()].sort((a, b) => a[0] - b[0]), [...cmap.entries()].sort((a, b) => a[0] - b[0]));
  }
  const offsets = [0, 1].map((i) => buf.readUInt32BE(4 + i * 8 + 4));
  assert.equal(offsets[0], offsets[1], '内容相同的两个子表应共享同一份字节');
});

test('sfnt 表序口径：偏好表里的先按表序、其余按 tag 排序、DSIG 永远最后', () => {
  const tags = ['vmtx', 'GDEF', 'glyf', 'head', 'cmap', 'DSIG', 'name'];
  assert.deepEqual(sortedTagList(tags), ['head', 'cmap', 'glyf', 'name', 'GDEF', 'vmtx', 'DSIG']);
});
