/** @env assets @kind contract @why 真机系统容器的字节不再符合 FMT-SAVE-SYSTEM / ENVELOPE / CODEC（规范与样本脱节） */
/**
 * tools/test/fixtures-system-container.assets.test.mjs —— **用真机样本核对系统容器的规范条目**
 *
 * 为什么要有它：`FMT-SAVE-SYSTEM` 之前只有**静态**证据（写侧 + 读侧两处汇编互证），仓库里没有任何
 * `SAVE.DAT` 样本 ⇒ 规范里那套分段从来没有被**字节**检验过。补齐样本后第一次逐段走，
 * 立刻抓到"段 5 之后还有 32 字节"这条**规范缺一段**，以及"段 3 的字数多 1 字""段 2 的键有 3 字节常量尾"
 * 三处措辞不足。这个用例把那些**现在成立**的形状钉住：规范再漂、样本再换，它都会红。
 *
 * ★ 它是 `@env assets`（样本走 LFS）⇒ **撑不起 `reviewed`**。这不是缺陷而是口径：
 *   一份二进制样本的字节级判据本来就跑不进默认门禁。
 *
 * ★ 反向判据同样重要：**同一套走查套在槽容器上必须垮**（两者的负载段结构完全不同）。
 *   只验正例的话，"什么都能过"的解析器也能绿。
 *
 * 运行：`pnpm test:assets`
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';
import { decodeSaveContainer, walkSystemPayload } from '../lib/save-container.mjs';

const FIXTURES = path.join(REPO_ROOT, 'corpus', 'fixtures');
const SYSTEM = path.join(FIXTURES, 'SAVE.DAT');
const TEXT_DB = path.join(FIXTURES, 'RT.DAT');
const SLOT = path.join(FIXTURES, 'SAVE76.DAT');

const read = (p) => fs.readFileSync(p);

/** 样本缺席（fresh clone 没拉 LFS）⇒ 如实 skip，不假装通过 */
const need = (t, ...paths) => {
  for (const p of paths) {
    if (!fs.existsSync(p)) {
      t.skip(`样本缺席：${path.relative(REPO_ROOT, p)}（先 pnpm tools fixtures restore-mtime --write / git lfs pull）`);
      return false;
    }
  }
  return true;
};

test('系统容器 SAVE.DAT：外层每一步都过（头 / 双校验和 / 变换 / 解压 / 内层头 / 密文后无残余）', (t) => {
  if (!need(t, SYSTEM)) return;
  const buf = read(SYSTEM);
  const env = decodeSaveContainer(buf);
  const h = env.header;

  assert.equal(h.magic, 'S4SD', '本作的系统容器与槽容器共用 S4SD 魔数');
  assert.equal(h.versionString, '460B');
  assert.deepEqual([env.prefix.declaredRaw, env.prefix.mirror, env.prefix.compressed].length, 3);
  assert.equal(env.prefix.declaredRaw, env.prefix.mirror, '压缩段前缀第 1、2 字恒等');
  assert.equal(env.mode, 'lzss', '系统容器的负载是压缩的（解压后字节数 != 压缩字节数）');
  assert.equal(h.declaredRawBytes, env.prefix.declaredRaw, '头 +240 == 解压后字节数（不是密文长度、也不是解密结果长度）');
  // ★ 解密结果比 "12 + 压缩字节数" 长：那是零填充，规范必须写明（FMT-SAVE-CODEC）
  assert.ok(env.paddingBytes > 0, `压缩流之后应当有零填充，实测 ${env.paddingBytes} 字节`);
  assert.ok(
    env.paddingBytes >= 37 && env.paddingBytes <= 38,
    `填充长度实测 37~38 字节（两份样本），这里得到 ${env.paddingBytes} —— 变了就说明规范里那个"余量 36"的写法要重核`,
  );
  assert.equal(env.trailingBytes, 0, '系统容器的密文之后不该有残余（槽容器的尾段不在这里）');
});

test('系统容器 SAVE.DAT：分段恰好消费到 EOF，且六段的形状与规范逐条对上', (t) => {
  if (!need(t, SYSTEM)) return;
  const env = decodeSaveContainer(read(SYSTEM));
  const walk = walkSystemPayload(env.body, { version1: env.header.version1, version2: env.header.version2 });

  assert.deepEqual(walk.problems, [], `分段不该有问题：${walk.problems.join(' / ')}`);
  assert.equal(walk.consumed, walk.total, `必须恰好消费到 EOF（消费 ${walk.consumed} / 负载 ${walk.total}）`);
  assert.deepEqual(
    walk.segs.map((s) => s.id),
    ['array', 'intKeys', 'stringKeys', 'gated', 'tailZero', 'trailer'],
    '版本门开着 ⇒ 六段按序；少一段/多一段都说明规范与样本脱节',
  );

  const byId = Object.fromEntries(walk.segs.map((s) => [s.id, s]));
  assert.ok(byId.array.count > 0);
  assert.ok(byId.intKeys.count > 0);
  assert.ok(byId.stringKeys.count > 0);
  assert.equal(byId.tailZero.value, 0, '段 5 是末尾一个 0 字');
  assert.equal(byId.trailer.len, 32, '段 6 = 8 个 0 字（规范原来的"末尾一个 0 字"就到此为止，少了这一段）');
  assert.deepEqual(byId.trailer.words, Array.from({ length: 8 }, () => 0), '收尾那 8 个字全是 0');
  // ★ 段 3 的"字数"不是两条串的字节数 / 4，而是**再多 1 字**（样本实测：两条串实占 4640 B、声明 1161 字）
  assert.equal(
    byId.stringKeys.declaredWords,
    Math.ceil(byId.stringKeys.packedBytes / 4) + 1,
    '段 3 的"字数" = ceil(两条串字节数 / 4) + 1',
  );
  assert.ok(
    byId.stringKeys.paddingInSeg3 >= 4 && byId.stringKeys.paddingInSeg3 <= 7,
    `段 3 内应当余 4~7 字节（多出来那个字 + 补齐），实测 ${byId.stringKeys.paddingInSeg3}`,
  );
  assert.ok(
    [...env.body.subarray(byId.stringKeys.off + 8 + byId.stringKeys.packedBytes, byId.stringKeys.off + byId.stringKeys.len)].every(
      (b) => b === 0,
    ),
    '段 3 那段补齐是 0（读者跳过，但形状固定）',
  );
});

test('系统容器 SAVE.DAT：段 2 / 段 3 的**键形态**（类型码 + 8 位十六进制；整型侧还多 3 字节常量尾）', (t) => {
  if (!need(t, SYSTEM)) return;
  const env = decodeSaveContainer(read(SYSTEM));
  const walk = walkSystemPayload(env.body, { version1: env.header.version1, version2: env.header.version2 });
  const byId = Object.fromEntries(walk.segs.map((s) => [s.id, s]));

  const hex8 = /^[0-9a-f]{8}$/; // 类型码之后是 8 位小写十六进制（脚本键恒 9 字节）
  for (const rec of byId.intKeys.records) {
    assert.equal(rec.key[0], 0x03, `整型键的类型码应为 03：${[...rec.key].map((b) => b.toString(16)).join(' ')}`);
    assert.ok(hex8.test(String.fromCharCode(...rec.key.subarray(1, 9))), '整型键 = 类型码 + 8 位十六进制');
    // ★ 后 3 字节是常量尾（规范原来只写"12 字节键"，没写这 3 字节）
    assert.deepEqual([...rec.key.subarray(9, 12)], [0x00, 0x51, 0x00], '整型键的 3 字节常量尾');
  }
  for (const rec of byId.stringKeys.records) {
    assert.equal(rec.key[0], 0x05, `字符串键的类型码应为 05：${[...rec.key].map((b) => b.toString(16)).join(' ')}`);
    assert.ok(rec.key.length >= 3, '字符串键至少是"类型码 + 几个十六进制字符"');
  }
});

test('★ 边界：命名键（`display:ScreenMode` 那种）**不在**系统容器里 —— 命名键表与脚本键表是两张表', (t) => {
  if (!need(t, SYSTEM)) return;
  const env = decodeSaveContainer(read(SYSTEM));
  const walk = walkSystemPayload(env.body, { version1: env.header.version1, version2: env.header.version2 });
  const byId = Object.fromEntries(walk.segs.map((s) => [s.id, s]));

  const named = (bytes) => bytes.includes(0x3a) && /^[A-Za-z]/.test(String.fromCharCode(bytes[0] ?? 0));
  for (const rec of byId.intKeys.records) {
    assert.ok(!named(rec.key), `整型键里出现命名键形状：${String.fromCharCode(...rec.key)}`);
  }
  for (const rec of byId.stringKeys.records) {
    assert.ok(!named(rec.key), `字符串键里出现命名键形状：${String.fromCharCode(...rec.key)}`);
  }
});

test('★ 反向判据：同一套分段走查套在**槽容器**上必须垮（两种容器的负载结构完全不同）', (t) => {
  if (!need(t, SLOT)) return;
  const env = decodeSaveContainer(read(SLOT));
  const walk = walkSystemPayload(env.body, { version1: env.header.version1, version2: env.header.version2 });
  assert.ok(walk.problems.length > 0 || walk.consumed !== walk.total, '槽容器的负载不该被系统容器的分段走到 EOF');
});

test('RT.DAT 是**另一种容器**：同一套 292 字节头，但魔数种类不同、且不满足存档信封的边界判据', (t) => {
  if (!need(t, TEXT_DB)) return;
  const buf = read(TEXT_DB);
  assert.equal(String.fromCharCode(buf[0], buf[1], buf[2], buf[3]), 'S3RT');
  assert.equal(String.fromCharCode(buf[4], buf[5], buf[6], buf[7]), '460B', '版本串与存档容器一致');
  assert.throws(
    () => decodeSaveContainer(buf),
    (e) => /超过读取缓冲边界|校验和/.test(e.message),
    '文本库不是存档容器：⛔ 不要用 SAVE 的规则去解它（这是"两张表"的机械判据）',
  );
});
