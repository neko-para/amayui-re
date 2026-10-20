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

test('★ 文件系统入口受限：spec.ts（读规范）与 main.ts（读输入脚本）之外不许碰 node:fs', () => {
  const users = sources().filter((x) => /from 'node:fs'/.test(x.text)).map((x) => x.name).sort();
  assert.deepEqual(users, ['main.ts', 'spec.ts'], `碰 node:fs 的文件应只有 spec.ts / main.ts，实际 ${users.join(', ') || '(无)'}`);
  const spec = sources().find((x) => x.name === 'spec.ts');
  assert.match(spec.text, /'data',\s*'spec'/, 'spec.ts 的落点常量必须拼出 data/spec');
  assert.match(spec.text, /opcode-table\.json/, 'spec.ts 必须只从规范表读');
  const main = sources().find((x) => x.name === 'main.ts');
  const reads = [...main.text.matchAll(/readFileSync\(([^)]*)\)/g)].map((m) => m[1].trim());
  assert.ok(reads.length > 0 && reads.every((a) => /\bfile\b/.test(a)), 'main.ts 只许读 --script 传入的输入文件（readFileSync(file)）');
});
