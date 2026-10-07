/** @env pure @kind contract @why 配置文件格式塌了：键/值里的控制字节或换行把行切坏 ⇒ **配置静默丢项**（而"读不到"看起来就像"没配过"） */
/**
 * tools/test/emulator-file-config.test.mjs —— **实例隔离的文件配置**（只有纯函数）
 *
 * ## 它守的是什么
 * 配置的**键**打头就是控制字节（`\x03`/`\x05`，见 `ops.ts` 的 `configKeyInt`），
 * 而**值**可能是任意日文文本、也可能含换行/制表符/竖线 ⇒ 任何"用分隔符切"的文本格式都会被值打穿。
 * ⇒ 本层把两个字段都按 UTF-8 逐字节十六进制编码；这条守卫就是钉住"**任意字节都能往返**"。
 *
 * ★ 另外钉住两条**不做的事**：认不出的头部要**抛**（静默当空配置 = "配置丢了"看起来像"第一次运行"）；
 *   键升序 ⇒ 同内容同字节（可比、可 diff）。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { CONFIG_HEADER, parseConfig, serializeConfig } from '../../apps/emulator/frontends/headless/file-config.ts';

test('★ 任意字节往返：控制字节的键、含换行/制表符/竖线/日文的值', () => {
  const entries = [
    ['\u0003000004d2', '1234'],
    ['\u000500000bbb', 'ＭＳ ゴシック'],
    ['\u000500000bbc', '含 换行\n与\t制表符|与竖线'], // ★ 分隔符出现在值里 —— 用分隔符切的格式会在这里崩
    ['\u000500000bbd', ''],
  ];
  const text = serializeConfig(entries);
  assert.ok(text.startsWith(CONFIG_HEADER + '\n'), '第一行是版本行');
  // ★ 逐行：值里的换行**不许**产生新行（否则行数会变）
  assert.equal(text.trimEnd().split('\n').length, entries.length + 1, '每项一行；值里的换行不能把行切坏');
  assert.deepEqual(parseConfig(text).slice().sort(), entries.slice().sort(), '往返逐项相同');
});

test('★ 键升序 ⇒ 同内容同字节；认不出的头部必须抛（不许静默当空配置）', () => {
  const a = serializeConfig([['\u0003b', '2'], ['\u0003a', '1']]);
  const b = serializeConfig([['\u0003a', '1'], ['\u0003b', '2']]);
  assert.equal(a, b, '输入顺序不同 ⇒ 字节必须相同');
  assert.throws(() => parseConfig('# 别的什么格式/9\n00|31'), /配置文件头不对/, '认不出的头部必须抛');
  assert.throws(() => parseConfig(`${CONFIG_HEADER}\n没有分隔符`), /没有分隔符/);
  assert.deepEqual(parseConfig(''), [], '空文本 ⇒ 空配置（第一次运行本来就是空的）');
});
