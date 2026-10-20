/** @env pure @kind gate @why apps/emulator-new 越界读了分析产物（shape / ledger / corpus / age-format），"只读 data/spec"的验证就失去意义 */
/**
 * tools/test/emulator-new-isolation.test.mjs —— **隔离守卫**
 *
 * 目的：`apps/emulator-new` 是 spec 的**消费者**，不是实现副本。
 * 它只许读 `data/spec`；一旦 import 了 `@amayui/age-format` / `data/shape` / `data/ledger` / `corpus`，
 * 或复制了旧模拟器的代码，"只看 spec 就能实现"这件事就无法再被验证。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';

const SRC = path.join(REPO_ROOT, 'apps', 'emulator-new', 'src');

function sources() {
  return fs
    .readdirSync(SRC, { withFileTypes: true })
    .filter((x) => x.isFile() && x.name.endsWith('.ts'))
    .map((x) => ({ name: x.name, text: fs.readFileSync(path.join(SRC, x.name), 'utf8') }));
}

test('★ 只读 data/spec：src 里不许出现 age-format / shape / ledger / corpus / 旧模拟器', () => {
  const bad = [];
  for (const { name, text } of sources()) {
    for (const [re, what] of [
      [/@amayui\/age-format/, 'age-format 包'],
      [/\bdata\/shape\b/, 'data/shape'],
      [/\bdata\/ledger\b/, 'data/ledger'],
      [/\bcorpus\//, 'corpus 语料'],
      [/\bapps\/emulator\//, '旧模拟器路径'],
    ]) {
      if (re.test(text)) bad.push(`${name}：${what}`);
    }
  }
  assert.deepEqual(bad, [], `越界引用：\n  - ${bad.join('\n  - ')}`);
});

test('★ 文件系统入口受限：spec.ts（读规范）与 host.ts（读安装目录）之外不许碰 node:fs', () => {
  const users = sources().filter((x) => /from 'node:fs'/.test(x.text)).map((x) => x.name).sort();
  assert.deepEqual(users, ['host.ts', 'spec.ts'],
    `碰 node:fs 的文件应只有 spec.ts（规范）/ host.ts（安装目录），实际 ${users.join(', ') || '(无)'}`);

  const spec = sources().find((x) => x.name === 'spec.ts');
  assert.match(spec.text, /'data',\s*'spec'/, 'spec.ts 的落点常量必须拼出 data/spec');
  assert.match(spec.text, /opcode-table\.json/, 'spec.ts 必须只从规范表读');

  const host = sources().find((x) => x.name === 'host.ts');
  assert.match(host.text, /SYS4INI\.BIN/, 'host.ts 必须从安装目录的 SYS4INI.BIN 索引入口读');
  assert.doesNotMatch(host.text, /data\/spec/, 'host.ts 不许读规范（那是 spec.ts 的职责）');

  const main = sources().find((x) => x.name === 'main.ts');
  assert.doesNotMatch(main.text, /from 'node:fs'/, 'main.ts 不许自己碰文件系统（走 host.ts）');
});
