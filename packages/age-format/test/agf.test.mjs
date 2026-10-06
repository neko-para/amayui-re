/** @env pure @kind contract @why `parseWhBpp` 的兜底分支坏了（元数据布局解析错） */
/**
 * packages/age-format/test/agf.test.mjs —— AGF（ACGF）里**不需要样本**的那部分
 *
 * 现在只有一条：`parseWhBpp` 的兜底分支（`+20/+24` 为 0 时退回 `+0/+4`）。
 * 它用**合成 meta** 判，不需要原始游戏文件。
 *
 * ★ **拆过一刀**（本轮测试分级）：原先本文件是"往返判据 + 分段压缩 + 像素解码"一大堆，
 *   全都要三个真实样本（`MI042` / `MI040` / `SO002`，不入库）⇒ 那些属 `@env assets`，
 *   已移到 `agf.assets.test.mjs`。不拆的话，这条**任何机器都能跑**的断言会被整包归到 assets 档。
 *
 * 运行：`pnpm test`
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { parseWhBpp } from '../src/agf.mts';

test('AGF：parseWhBpp 的兜底分支（+20/+24 为 0 时退回 +0/+4）', () => {
  const meta = Buffer.alloc(32);
  meta.writeUInt32LE(640, 0);
  meta.writeUInt32LE(480, 4);
  meta.writeInt16LE(24, 30);
  assert.deepEqual(parseWhBpp(meta), [640, 480, 24]);
  const meta2 = Buffer.alloc(32);
  meta2.writeUInt32LE(320, 20);
  meta2.writeUInt32LE(200, 24);
  meta2.writeInt16LE(8, 30);
  assert.deepEqual(parseWhBpp(meta2), [320, 200, 8]);
});
