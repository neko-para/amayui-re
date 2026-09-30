/**
 * tools/test/json-docs.test.mjs — 两条**约定**的守卫
 *
 * ① **同名说明书**（`docs/00-origin/decisions.md` §6）：`foo.json` 旁边必须有 `foo.md`，
 *    且那份 `.md` 必须提到这个 JSON、含 `## 怎么查` / `## 怎么改`、并**指向脚本的自描述**（`--describe`）。
 * ② **脚本自描述可用**：JSON 被当作**不透明数据**，schema 的真源是控制脚本 ⇒ `describe()` 必须真的给得出东西
 *    （字段表、枚举、不变量、操作）。这条把"自描述"从口号变成可检查的东西。
 *
 * 范围：只覆盖**本仓自有**的结构化文件；**生态文件**（`package.json` 等）显式排除。
 *
 * 运行：`pnpm test`
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { KINDS, ROLES, STORAGES, describe as describeManifest } from '../lib/manifest.mjs';
import { REPO_ROOT } from '../lib/paths.mjs';
import { describe as describeSamples } from '../lib/samples.mjs';

/** 外部工具定义的文件（说明书不在本仓）—— 保持这份清单短且显式 */
const ECOSYSTEM = new Set(['package.json']);
const SKIP_DIRS = new Set(['.git', 'node_modules', '.tmp', '.staging', 'files']);

/** 说明书必须覆盖的三件事（正是"只讲查询不讲编辑"的反面）；最后一项是"指向脚本自描述" */
const REQUIRED = ['## 怎么查', '## 怎么改', 'describe'];

function walkJson(dir, out = []) {
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    if (e.isDirectory()) {
      if (SKIP_DIRS.has(e.name)) continue;
      walkJson(path.join(dir, e.name), out);
    } else if (e.isFile() && e.name.endsWith('.json')) {
      out.push(path.join(dir, e.name));
    }
  }
  return out;
}

const rel = (p) => path.relative(REPO_ROOT, p).split(path.sep).join('/');

test('每个自有 JSON 都有同名 .md，且覆盖「怎么查」「怎么改」并指向脚本自描述', () => {
  const files = walkJson(REPO_ROOT).filter((p) => !ECOSYSTEM.has(path.basename(p)));
  assert.ok(files.length > 0, '至少应当扫到 corpus/assets.json 与 corpus/fixtures/samples.json');

  const problems = [];
  for (const jsonPath of files) {
    const r = rel(jsonPath);
    const docPath = jsonPath.replace(/\.json$/, '.md');
    if (!fs.existsSync(docPath)) {
      problems.push(`${r} 旁边缺同名说明书 ${path.basename(docPath)}`);
      continue;
    }
    const doc = fs.readFileSync(docPath, 'utf8');
    if (!doc.includes(path.basename(jsonPath))) problems.push(`${rel(docPath)} 里没提到 ${path.basename(jsonPath)}`);
    for (const need of REQUIRED) {
      if (!doc.includes(need)) problems.push(`${rel(docPath)} 缺「${need}」`);
    }
  }
  assert.deepEqual(problems, [], `说明书约定被破坏：\n  - ${problems.join('\n  - ')}`);
});

test('★ 脚本自描述可用：assets.json 的 schema/枚举/不变量/操作都由 corpus.mjs 给出', () => {
  const d = describeManifest();
  assert.equal(d.file, 'corpus/assets.json');
  assert.ok(d.entry.fields.length >= 8, '字段表不能空');
  // 枚举必须与校验器**用的是同一批常量**（不是文档里另抄一份）
  const kinds = d.entry.fields.find((f) => f.name === 'kind');
  for (const k of KINDS) assert.ok(kinds.type.includes(k), `kind 枚举少了 ${k}`);
  const storages = d.entry.fields.find((f) => f.name === 'storage');
  for (const s of STORAGES) assert.ok(storages.type.includes(s), `storage 枚举少了 ${s}`);
  assert.ok(ROLES.length > 0 && d.entry.fields.find((f) => f.name === 'role'));
  assert.equal(d.invariants.length, 9, '9 条不变量必须都能自描述出来');
  assert.ok(d.operations.length >= 4 && d.writePath.length > 0, '必须给出"怎么查/怎么改"与唯一写入口');
});

test('★ 脚本自描述可用：samples.json 的字段/不变量（含谁在守）/操作都由 fixtures.mjs 给出', () => {
  const d = describeSamples();
  assert.equal(d.file, 'corpus/fixtures/samples.json');
  for (const need of ['slot', 'where', 'files[].mtimeMs', 'files[].tzOffsetMinutes']) {
    assert.ok(d.fields.some((f) => f.name === need), `字段表少了 ${need}`);
  }
  assert.ok(d.invariants.length >= 4, '不变量不能空');
  for (const inv of d.invariants) assert.ok(inv.enforcedBy && inv.enforcedBy.length > 0, `不变量 ${inv.id} 没说"谁在守它"`);
  assert.ok(d.operations.length >= 4 && d.writePath.includes('唯一'));
});
