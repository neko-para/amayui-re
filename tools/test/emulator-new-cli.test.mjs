/** @env pure @kind contract @why `--skip-logo` 的解析被删/写反后，CLI 会静默回到引擎初值 1（仍走 LOGO），而真实启动链测试直接传 `showLogo:0` 抓不到 */
/**
 * tools/test/emulator-new-cli.test.mjs —— CLI 参数解析的纯守卫
 *
 * ★ 只测参数层：`--skip-logo` 必须被解析成 `skipLogo = true`；缺省必须是 false。
 *   参数之后的 `showLogo: 0` 接线由 `emulator-new-title.assets.test.mjs` 与 mutation 条目覆盖。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { parseArgs } from '../../apps/emulator-new/src/platform/node/cli.ts';

test('★ --skip-logo 解析成 skipLogo = true（缺省 false）', () => {
  assert.equal(parseArgs(['--install', 'x']).skipLogo, false);
  assert.equal(parseArgs(['--install', 'x', '--skip-logo']).skipLogo, true);
});
