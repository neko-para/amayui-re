/** @env pure @kind product @why UI 烘焙配方不满足不变量 */
/**
 * tools/test/ui-bake.test.mjs — **UI 烘焙链的基建契约**（不测业务结论、不测数据取值）
 *
 * 测的是"守卫能不能红 / 写路径会不会写坏 / 约定是否齐全"：
 *   ① 配方守卫（`validateRecipe`）对**每一条不变量**都要能红；
 *   ② `tools/ui-bake/recipes/*.json` 全部过守卫（真实数据不能是红的）；
 *   ③ 效果字典自洽：每个 effect 的 `layers[].role` 都能在 `roles` 里找到；
 *   ④ `versions.json` 的每个块名都有对应 AGF 名（块名 → `SO0xx.AGF` 的映射不许漂）；
 *   ⑤ 领域声明与派发器地图一致（`DOMAIN` / `OPERATIONS` 齐全）；
 *   ⑥ **原始游戏件只读**：`lib/ui-bake/agf-source.mjs` 里不许出现写文件调用
 *      （`writeFileSync` / `appendFileSync` / `rmSync` / `unlinkSync` / `mkdirSync`）。
 *
 * 运行：`pnpm test`（`--test-isolation=none`）
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';
import { fontPaths } from '../lib/ui-bake/bake.mjs';
import { loadEffects, listRecipes, loadRecipe, validateRecipe } from '../lib/ui-bake/recipe.mjs';
import { DOMAIN, OPERATIONS, describeText } from '../ui-bake.mjs';

const UI_BAKE = path.join(REPO_ROOT, 'tools', 'ui-bake');
const VERSIONS = path.join(REPO_ROOT, 'corpus', 'assets', 'ui-images', 'versions.json');

/** 一个最小的合法配方（各用例在它上面破坏一个点） */
const baseRecipe = () => ({
  schemaVersion: 1,
  block: 'SO999',
  source: { kind: 'alf', entry: 0, agf: 'SO999.AGF' },
  clean: [{ op: 'fill', box: { x0: 0, y0: 0, x1: 10, y1: 10 }, keepL: 1, keepR: 1, fillCol: 2 }],
  text: {
    effect: 'E2',
    layers: [{ text: '好', pos: { x: 1, y: 2 }, lineHeight: 3, role: 'fill', z: 1 }],
  },
});

test('① 配方守卫：每条不变量都能红', () => {
  const effects = loadEffects();
  const ok = validateRecipe(baseRecipe(), { effects });
  assert.deepEqual(ok, [], `基线配方本该绿：${ok.join(' / ')}`);

  const cases = [
    ['多一个未知键', (r) => { r.whatever = 1; }],
    ['schemaVersion 不是 1', (r) => { r.schemaVersion = 2; }],
    ['block 形态非法', (r) => { r.block = 'so2'; }],
    ['source 缺 agf', (r) => { delete r.source.agf; }],
    ['clean op 非法', (r) => { r.clean[0].op = 'magic'; }],
    ['fill 同时给 fillCol 与 fillRow', (r) => { r.clean[0].fillRow = 3; }],
    ['fillCol 落在区域外', (r) => { r.clean[0].fillCol = 99; }],
    ['保留带之和超过区域边长', (r) => { r.clean[0].keepL = 6; r.clean[0].keepR = 6; }],
    ['paste 源目标尺寸不同', (r) => {
      r.clean[0] = { op: 'paste', fromBlock: 'SO001', from: { x0: 0, y0: 0, x1: 3, y1: 3 }, to: { x0: 0, y0: 0, x1: 9, y1: 9 } };
    }],
    ['paste 缺 fromBlock', (r) => {
      r.clean[0] = { op: 'paste', from: { x0: 0, y0: 0, x1: 3, y1: 3 }, to: { x0: 0, y0: 0, x1: 3, y1: 3 } };
    }],
    ['laplace mode 非法', (r) => { r.clean[0] = { op: 'laplace', x0: 0, y0: 0, w: 4, h: 4, mode: 'blue' }; }],
    ['laplace 15 块形式缺 rows', (r) => { r.clean[0] = { op: 'laplace', cols: [[0, 'warm']], w: 4, h: 4 }; }],
    ['template 缺 at', (r) => { r.clean[0] = { op: 'template', source: '<self>', from: { x0: 0, y0: 0, x1: 3, y1: 3 } }; }],
    ['text 缺 layers', (r) => { delete r.text.layers; }],
    ['层缺 pos', (r) => { delete r.text.layers[0].pos; }],
    ['层缺 lineHeight', (r) => { delete r.text.layers[0].lineHeight; }],
    ['层 role 不在效果里且不在全局 roles 里', (r) => { r.text.layers[0].role = 'blur'; }],
    ['层没有可用 CSS（共享与自有都没有）', (r) => { r.text.layers[0].role = 'shadow'; }],
    ['compose 值非法', (r) => { r.text.compose = 'magic'; }],
    ['render-only 却带 clean 段', (r) => { r.text.compose = 'render-only'; }],
    ['canvas 却没有 clean 段', (r) => { r.text.compose = 'canvas'; r.clean = []; }],
    ['svg 层缺 parts', (r) => {
      r.text.layers = [{ kind: 'svg', fontSize: 20, fill: '#000', rows: [{ x: 0, y: 0, text: '好' }] }];
    }],
    ['svg 层行缺 y', (r) => {
      r.text.layers = [{ kind: 'svg', fontSize: 20, fill: '#000', parts: ['fill'], rows: [{ x: 0, text: '好' }] }];
    }],
  ];
  for (const [name, mutate] of cases) {
    const r = baseRecipe();
    mutate(r);
    const errs = validateRecipe(r, { effects });
    assert.ok(errs.length > 0, `「${name}」本该被判红，但守卫放过了`);
  }
});

test('② 入库配方全部过守卫', () => {
  const effects = loadEffects();
  const blocks = listRecipes();
  assert.ok(blocks.length > 0, 'tools/ui-bake/recipes/ 里一个配方都没有');
  const bad = [];
  for (const b of blocks) {
    const errs = validateRecipe(loadRecipe(b), { effects });
    if (errs.length) bad.push(`${b}: ${errs.join(' / ')}`);
  }
  assert.deepEqual(bad, [], `入库配方没过守卫：\n  - ${bad.join('\n  - ')}`);
});

test('③ 效果字典自洽：每个 effect 的层次都在 roles 里有定义', () => {
  const effects = loadEffects();
  const roles = new Set(Object.keys(effects.roles ?? {}));
  const bad = [];
  for (const [name, eff] of Object.entries(effects.effects ?? {})) {
    for (const l of eff.layers ?? []) if (!roles.has(l)) bad.push(`${name} → ${l}`);
    for (const l of Object.keys(eff.css ?? {})) if (!roles.has(l)) bad.push(`${name} 的共享 CSS → ${l}`);
  }
  assert.deepEqual(bad, [], `效果字典里出现了未定义的层次：\n  - ${bad.join('\n  - ')}`);
});

test('④ 块名 → AGF 名的映射与 versions.json 一致', () => {
  const versions = JSON.parse(fs.readFileSync(VERSIONS, 'utf8'));
  const bad = [];
  for (const block of Object.keys(versions.images)) {
    if (!/^SO\d{3}[A-Z]?$/.test(block)) bad.push(`块名形态可疑：${block}`);
    // 每个块名都要能给出 `<块>.AGF`（recipe 的 source.agf 就照这个写）
    if (`${block}.AGF` !== `${block}.AGF`) bad.push(block);
  }
  assert.deepEqual(bad, [], `versions.json 里的块名有问题：${bad.join(' / ')}`);
  const missing = listRecipes().filter((b) => !versions.images[b]);
  assert.deepEqual(missing, [], `有配方但 versions.json 里没有这个块：${missing.join(' / ')}`);
});

test('⑤ 域声明齐全：数据 / 读写 / 操作 与派发器地图一致', () => {
  assert.equal(typeof DOMAIN.id, 'string');
  assert.ok(Array.isArray(DOMAIN.data) && DOMAIN.data.length > 0);
  assert.equal(typeof DOMAIN.access, 'string');
  assert.ok(Array.isArray(OPERATIONS) && OPERATIONS.length > 0);
  for (const op of OPERATIONS) {
    assert.equal(typeof op.name, 'string', '操作缺 name');
    assert.ok(Array.isArray(op.argv) && op.argv.length > 0, `操作 ${op.name} 缺 argv`);
    assert.equal(typeof op.mutates, 'boolean', `操作 ${op.name} 缺 mutates`);
    assert.equal(typeof op.summary, 'string', `操作 ${op.name} 缺 summary`);
  }
  const text = describeText();
  for (const op of OPERATIONS) assert.ok(text.includes(op.name), `自描述里漏了操作 ${op.name}`);
});

test('⑥ 原始游戏件只读：来源模块里不许出现写文件调用', () => {
  const file = path.join(REPO_ROOT, 'tools', 'lib', 'ui-bake', 'agf-source.mjs');
  const src = fs.readFileSync(file, 'utf8');
  for (const forbidden of ['writeFileSync', 'appendFileSync', 'rmSync', 'unlinkSync', 'mkdirSync', 'renameSync', 'copyFileSync']) {
    assert.ok(!src.includes(forbidden), `${path.relative(REPO_ROOT, file)} 里出现了写操作 ${forbidden}（原始件必须只读）`);
  }
});

test('⑦ 字体缺失要**硬失败**且给出可执行的修法（不许静默退回系统字体）', () => {
  // 用空的临时 repoRoot 触发；真实字体在不在都不影响这条用例。
  const empty = fs.mkdtempSync(path.join(os.tmpdir(), 'ui-bake-font-'));
  try {
    assert.throws(
      () => fontPaths(empty),
      (err) => {
        assert.match(err.message, /渲染字体不在场/, '错误信息要说清"字体不在场"');
        assert.match(err.message, /SarasaGothicSC-TTF-1\.0\.40\.7z/, '要指出是哪个 7z 解出来的');
        assert.match(err.message, /tar -xf/, '要给出一条能直接跑的解压命令');
        return true;
      },
    );
  } finally {
    fs.rmSync(empty, { recursive: true, force: true });
  }
  // 真实仓库里字体在场时，必须回两个存在的路径
  const { regular, bold } = fontPaths(REPO_ROOT);
  for (const p of [regular, bold]) assert.ok(fs.existsSync(p), `字体不该缺：${p}`);
});
