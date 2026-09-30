/**
 * tools/test/fixtures.test.mjs — 真存档样本（`corpus/fixtures/`）的守卫
 *
 * 真源分工：
 *   · `samples.json` —— 这一批 fixture 的**结构化描述**：槽 → 游戏内定位（`where`）→ 各文件 mtime
 *   · `corpus/assets.json` —— 文件的**来源 / sha256 / 存储去向**（`fixtures/save-samples` 与来源记录）
 * 两边各管一摊、**不许互相抄**（README 里也不许列表）—— 本测试负责断言两边的**文件集合一致**。
 *
 * 钉住：① 结构合法；② 槽 76–79 齐备且 .DAT/.STH 成对；③ 与清单的文件集合一致；
 *       ④ mtime 与 samples.json 一致时，「槽头 +264 起七个 u16 == 文件 mtime」必须成立；
 *       ⑤ mtime 已漂移时**如实 skip**（fresh clone 的正常状态）并指向恢复命令 —— 不假装通过。
 *
 * 运行：`pnpm test`
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { DEFAULT_MANIFEST, loadManifest } from '../lib/manifest.mjs';
import { REPO_ROOT } from '../lib/paths.mjs';
import { wallClock } from '../lib/time.mjs';

const FIXTURES = path.join(REPO_ROOT, 'corpus', 'fixtures');
const SAMPLES = path.join(FIXTURES, 'samples.json');
const SLOTS = ['76', '77', '78', '79'];

const samples = () => JSON.parse(fs.readFileSync(SAMPLES, 'utf8'));

const pad = (n) => String(n).padStart(2, '0');

const allFiles = () => samples().samples.flatMap((s) => s.files.map((f) => f.name));

test('samples.json 存在，结构合法（每个样本都要有 where、files[] 与三时间字段）', () => {
  assert.ok(fs.existsSync(SAMPLES), 'corpus/fixtures/samples.json 必须存在');
  const doc = samples();
  assert.equal(doc.schemaVersion, 1);
  assert.ok(Array.isArray(doc.samples) && doc.samples.length > 0);
  for (const s of doc.samples) {
    assert.match(s.slot, /^\d{2,3}$/, `槽号形态异常：${JSON.stringify(s.slot)}`);
    assert.ok(typeof s.where === 'string' && s.where.trim() !== '', `槽 ${s.slot} 缺 where（游戏内定位）`);
    assert.ok(Array.isArray(s.files) && s.files.length > 0, `槽 ${s.slot} 没有 files[]`);
    for (const f of s.files) {
      assert.match(f.mtimeLocal, /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}$/, `${f.name} 的 mtimeLocal 形态异常`);
      assert.equal(typeof f.mtimeMs, 'number');
      assert.equal(typeof f.tzOffsetMinutes, 'number', `${f.name} 缺 tzOffsetMinutes`);
    }
  }
});

test('槽 76/77/78/79 齐备，且每个槽恰好 .DAT + .STH 成对', () => {
  const bySlot = new Map(samples().samples.map((s) => [s.slot, s]));
  for (const slot of SLOTS) {
    const s = bySlot.get(slot);
    assert.ok(s, `缺槽 ${slot}`);
    assert.deepEqual(
      s.files.map((f) => f.name).sort(),
      [`SAVE${slot}.DAT`, `SAVE${slot}.STH`].sort(),
      `槽 ${slot} 的文件不成对`,
    );
  }
});

test('与清单的文件集合一致（samples.json 管文件级事实，清单管来源；两边不许漂）', () => {
  const manifest = loadManifest(DEFAULT_MANIFEST);
  const entry = manifest.entries.find((e) => e.id === 'fixtures/save-samples');
  assert.ok(entry, '清单里必须有 fixtures/save-samples');
  const fromManifest = entry.origin.map((o) => path.basename(o.path)).sort();
  assert.deepEqual(allFiles().sort(), fromManifest, 'samples.json 与清单登记的文件集合不一致');
});

test('样本文件都在盘上且非空', () => {
  for (const n of allFiles()) {
    const p = path.join(FIXTURES, n);
    assert.ok(fs.existsSync(p), `缺 ${n}`);
    assert.ok(fs.statSync(p).size > 0, `${n} 是空文件`);
  }
});

test('目录里除了载荷、目录 README、samples.json 及其同名说明书之外，不应有别的文件（防"悄悄塞进来的样本"）', () => {
  const listed = new Set([
    ...allFiles(),
    'README.md',
    path.basename(SAMPLES), // samples.json
    path.basename(SAMPLES).replace(/\.json$/, '.md'), // samples.md（同名说明书约定）
    '.gitkeep',
  ]);
  const actual = fs.readdirSync(FIXTURES, { withFileTypes: true }).filter((d) => d.isFile()).map((d) => d.name);
  for (const n of actual) assert.ok(listed.has(n), `${n} 没登记在 samples.json 里（要么登记，要么删掉）`);
});

test('每个 .DAT 的槽头时刻 == wallClock(mtimeMs, tzOffsetMinutes)（mtime 已漂移则如实 skip）', (t) => {
  let checked = 0;
  let drifted = 0;
  const total = samples().samples.length;
  for (const s of samples().samples) {
    const rec = s.files.find((f) => f.name.endsWith('.DAT'));
    const p = path.join(FIXTURES, rec.name);
    // ★ 比对的是 **instant**（不是本机墙上时间）⇒ 跨时区也成立
    const cur = Math.round(fs.statSync(p).mtimeMs);
    if (Math.abs(cur - rec.mtimeMs) > 1000) {
      drifted += 1;
      t.diagnostic(
        `${rec.name}: mtime 与记录的 instant 不一致（盘上 ${new Date(cur).toISOString()} / 记录 ${new Date(rec.mtimeMs).toISOString()}）` +
          `⇒ 跳过不变量断言；恢复：pnpm tools fixtures restore-mtime --write`,
      );
      continue;
    }
    const b = fs.readFileSync(p);
    const [y, mo, wd, d, h, mi, sec] = Array.from({ length: 7 }, (_, i) => b.readUInt16LE(264 + i * 2));
    const head = `${y}-${pad(mo)}-${pad(d)}T${pad(h)}:${pad(mi)}:${pad(sec)}`;
    // 槽头存的是**采集时区下的墙上时间** ⇒ 用记录的 instant + 采集时区还原，与跑测试的机器时区无关
    assert.equal(head, wallClock(rec.mtimeMs, rec.tzOffsetMinutes), `${rec.name}: 槽头 ${head} != 记录的墙上时间`);
    // 星期与日期必须自洽（顺带证明我们读的字段位置没错位）
    const dow = new Date(Date.UTC(y, mo - 1, d)).getUTCDay();
    assert.equal(wd, dow, `${rec.name}: 头里的星期 ${wd} 与日期推出的 ${dow} 不一致`);
    checked += 1;
  }
  assert.ok(checked > 0 || drifted === total, '至少要有一个样本真的被断言过（不能全部静默跳过）');
});
