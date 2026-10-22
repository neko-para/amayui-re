/** @env pure @kind gate @why apps/emulator-new 越界读了分析产物（shape / ledger / corpus / age-format），"只读 data/spec"的验证就失去意义 */
/**
 * tools/test/emulator-new-isolation.test.mjs —— **隔离守卫**
 *
 * 目的：`apps/emulator-new` 是 spec 的**消费者**，不是实现副本。
 * 它只许读 `data/spec`；一旦 import 了 `@amayui/age-format` / `data/shape` / `data/ledger` / `corpus`，
 * 或复制了旧模拟器的代码，"只看 spec 就能实现"这件事就无法再被验证。
 *
 * ★ 分层（本文件同时守**平台边界**）：
 *   - `src/core/**`：平台无关的核心（`tsconfig.core.json` 的 `types: []` 是"不许碰 Node"的**真守卫**）；
 *   - `src/platform/**`：唯一允许 `node:*` 的地方（`node:fs` / `node:path` / `process`）；
 *   - `src/main.ts`：Node 入口，只接 argv / 退出码。
 *   ⇒ 本文件的扫描是**第二道**（只认字面量）；类型系统那道认得出间接 import 与别名，别把这里当唯一防线。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';

const SRC = path.join(REPO_ROOT, 'apps', 'emulator-new', 'src');

/** 递归列出 `src` 下所有 `.ts`（相对 `src` 的 POSIX 路径），并带上正文 */
function sources() {
  const out = [];
  const walk = (dir) => {
    for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
      const full = path.join(dir, entry.name);
      if (entry.isDirectory()) walk(full);
      else if (entry.isFile() && entry.name.endsWith('.ts')) {
        out.push({ name: path.relative(SRC, full).split(path.sep).join('/'), text: fs.readFileSync(full, 'utf8') });
      }
    }
  };
  walk(SRC);
  return out.sort((a, b) => a.name.localeCompare(b.name));
}

const byName = (name) => {
  const hit = sources().find((x) => x.name === name);
  assert.ok(hit, `找不到 ${name}`);
  return hit;
};

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

test('★ 平台边界：只有 platform/** 允许 import node:*（核心的守卫是 tsconfig.core.json 的 types: []）', () => {
  const users = sources()
    .filter((x) => /from 'node:/.test(x.text))
    .map((x) => x.name);
  assert.deepEqual(users, ['platform/node/host.ts', 'platform/node/spec.ts'],
    `import node:* 的文件只应落在平台层，实际 ${users.join(', ') || '(无)'}`);

  for (const { name, text } of sources()) {
    if (!name.startsWith('core/')) continue;
    assert.doesNotMatch(text, /from '\.\.\/platform\//, `${name} 不许反向依赖平台层（分层方向是 platform → core）`);
  }

  const main = byName('main.ts');
  assert.doesNotMatch(main.text, /from 'node:fs'/, 'main.ts 不许自己碰文件系统（那属于 platform/node/**）');
  assert.match(main.text, /from '\.\/platform\/node\/cli\.ts'/, 'main.ts 只做入口：接 CLI 的 argv / 退出码');
});

test('★ 落点：规范由 platform/node/spec.ts 读；安装目录由 core/install.ts 走 Host 取件', () => {
  const spec = byName('platform/node/spec.ts');
  assert.match(spec.text, /'data', 'spec'/, '平台层 spec 的落点常量必须拼出 data/spec');
  assert.match(spec.text, /opcode-table\.json/, '只从规范表读指令表');
  assert.match(spec.text, /config-keys\.json/, '只从规范表读配置键词表');
  assert.doesNotMatch(spec.text, /SYS4INI\.BIN/, 'spec 加载器不许碰安装目录（那是 core/install.ts + Host 的职责）');

  const install = byName('core/install.ts');
  assert.match(install.text, /SYS4INI\.BIN/, '核心的安装目录取件必须从 SYS4INI.BIN 索引入口读');
  assert.doesNotMatch(install.text, /data\/spec/, 'core/install.ts 不许读规范（那是平台层的职责）');
  assert.match(install.text, /interface Host/, '核心必须用注入的 Host 接口，而不是自己碰文件系统');
});
