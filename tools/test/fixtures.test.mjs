/** @env pure @kind gate @suite toolchain @why 样本台账与判据脱节 */
/**
 * tools/test/fixtures.test.mjs — 真存档样本（`corpus/fixtures/`）的守卫
 *
 * 真源分工：
 *   · `samples.json` —— 这一批 fixture 的**结构化描述**：`samples[]`（槽 → 游戏内定位 `where` → 各文件 mtime）
 *     与 `containers[]`（**不属任何槽**的系统容器 / 玩家配置，身份 = 文件名 + `what`）
 *   · `corpus/assets.json` —— 只管"入库 / 去向 / 走哪种存储"：fixture 是**自足条目**
 *     （固化资源，没有加工链）⇒ 它**不登记 origin**，"内容没变"由 git 的对象校验与下面的 mtime 事实回答
 *     （2026-11-18 起该条目是 `storage: git`；此前载荷走 LFS）。
 *
 * 钉住：① 结构合法；② 登记了的槽 .DAT/.STH 成对（**0 个槽也合法**）；③ 盘上文件与登记集合**两边相等**；
 *       ④ mtime 与 samples.json 一致时，「槽头 +264 起七个 u16 == 文件 mtime」必须成立；
 *       ⑤ mtime 已漂移时**如实 skip**（fresh clone 的正常状态）并指向恢复命令 —— 不假装通过；
 *       ⑥ `containers` 的名字唯一、`what` 非空、与槽载荷不重名；
 *       ⑦ 目录里的条目只能是**登记的载荷 / 目录文档 / OS 元数据**（判据 = `classifyFixtureEntry`
 *          纯函数，见下；`REQ-01M7HAK2H6R8RWEG15RBDH5585`：Finder 的 `.DS_Store` 不再撞穿它，
 *          而塞一个真样本（`random.bin`）必须依旧红）。
 *
 * ★ **2026-11-18（用户裁决）：本目录的存档载荷已全部删除**（18 个文件：顶层槽 76–79 的 .DAT/.STH +
 *   系统容器 SAVE.DAT / RT.DAT + 老汉化 936/GBK 的 `saves/` 子目录）⇒ `samples[]` / `containers[]`
 *   现在是**空数组**，而**空清单合法**（`lib/samples.mjs` 的 `structuralProblems` 对空数组返回 []）。
 *   于是判据按"当前登记了几个样本"取：
 *   · **结构**（①⑥）与**目录白名单**（③⑦）照旧逐条成立 —— 它们不依赖盘上有载荷；
 *     ★ ⑦ 的 `listed` 是**合成集合**，⛔ 不再由 `allFiles()` 推出来（空清单会让它变成空集 ⇒ 判据假松）；
 *   · "文件都在盘上"（③）与"槽头 == mtime"（④）只在**登记了样本**时才走断言，0 个样本时如实 `skip`
 *     （⛔ 不留"空转 pass"的假绿，也⛔ 不用 `t.diagnostic + return` —— `layering.test.mjs` 盯着这个形态）；
 *   · 规范样本已移到仓库根 `saves/`（**用户自维护**，⛔ 不是这个工具的 dest）。
 *   工具本身**不退役**：`samples.json` 仍只有一个写入口，将来补样本仍走 `pnpm tools fixtures add`。
 *
 * 运行：`pnpm test`
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';
import { FIXTURE_DOC_FILES, classifyFixtureEntry, isAllowedFixtureEntry } from '../lib/samples.mjs';
import { wallClock } from '../lib/time.mjs';

const FIXTURES = path.join(REPO_ROOT, 'corpus', 'fixtures');
const SAMPLES = path.join(FIXTURES, 'samples.json');

const samples = () => JSON.parse(fs.readFileSync(SAMPLES, 'utf8'));

const pad = (n) => String(n).padStart(2, '0');

const allFiles = () => [
  ...samples().samples.flatMap((s) => s.files.map((f) => f.name)),
  ...(samples().containers ?? []).map((c) => c.name),
];

test('samples.json 存在，结构合法（每个样本都要有 where、files[] 与三时间字段）', () => {
  assert.ok(fs.existsSync(SAMPLES), 'corpus/fixtures/samples.json 必须存在');
  const doc = samples();
  assert.equal(doc.schemaVersion, 1);
  // ★ 2026-11-18 起允许 0 个样本（载荷已按用户裁决删除）；"至少一个"不再是结构不变量。
  assert.ok(Array.isArray(doc.samples), 'samples 必须是数组（可以为空，但字段要在）');
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

test('★ 系统容器 / 玩家配置（`containers`）：不属任何槽，名字唯一、`what` 非空、三时间字段齐全', () => {
  const doc = samples();
  assert.ok(Array.isArray(doc.containers), 'containers 必须是数组（可以为空，但字段要在）');
  const payload = new Set(doc.samples.flatMap((s) => s.files.map((f) => f.name)));
  const seen = new Set();
  for (const c of doc.containers) {
    assert.ok(typeof c.name === 'string' && c.name.trim() !== '', 'containers 里有条目的 name 为空');
    assert.ok(!seen.has(c.name), `containers 里重名：${c.name}`);
    seen.add(c.name);
    assert.ok(!payload.has(c.name), `${c.name} 同时被登记成槽载荷与系统容器（一个文件只能有一个身份）`);
    assert.ok(typeof c.what === 'string' && c.what.trim() !== '', `${c.name} 缺 what（它是什么 / 这份样本本身的口径）`);
    assert.match(c.mtimeLocal, /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}$/, `${c.name} 的 mtimeLocal 形态异常`);
    assert.equal(typeof c.mtimeMs, 'number', `${c.name} 的 mtimeMs 必须是数字`);
    assert.equal(typeof c.tzOffsetMinutes, 'number', `${c.name} 缺 tzOffsetMinutes`);
  }
});

test('登记了的槽：每个槽恰好 .DAT + .STH 成对（0 个槽也合法）', () => {
  const doc = samples();
  const bySlot = new Map(doc.samples.map((s) => [s.slot, s]));
  assert.equal(bySlot.size, doc.samples.length, '槽号不许重复');
  // ★ 判据按"**登记了的**槽"取，不再要求 76–79 齐备：2026-11-18 载荷删除后不允许留一条恒红的
  //   "缺槽"断言；反过来，将来补回任何一个槽，这条仍会当场检查它的 DAT/STH 是否成对。
  for (const s of doc.samples) {
    assert.deepEqual(
      s.files.map((f) => f.name).sort(),
      [`SAVE${s.slot}.DAT`, `SAVE${s.slot}.STH`].sort(),
      `槽 ${s.slot} 的文件不成对`,
    );
  }
});

test('样本文件都在盘上且非空', (t) => {
  const files = allFiles();
  // ★ 0 个样本时这条**没有可断言的对象** ⇒ 如实 skip（node:test 记 skip，不是 pass）。
  //   ⛔ 不要让它空转 pass：那正是 AGENTS.md §5.2 要防的假绿。
  if (files.length === 0) return t.skip('samples.json 里 0 个样本（2026-11-18 载荷已删除）⇒ 无盘上文件可断言');
  for (const n of files) {
    const p = path.join(FIXTURES, n);
    assert.ok(fs.existsSync(p), `缺 ${n}`);
    assert.ok(fs.statSync(p).size > 0, `${n} 是空文件`);
  }
});

test('目录里除了载荷、目录 README、samples.json 及其同名说明书之外，不应有别的文件（防"悄悄塞进来的样本"）', () => {
  const listed = new Set([...allFiles(), ...FIXTURE_DOC_FILES]);
  const actual = fs.readdirSync(FIXTURES, { withFileTypes: true }).filter((d) => d.isFile()).map((d) => d.name);
  const bad = actual.filter((n) => !isAllowedFixtureEntry(n, listed));
  assert.deepEqual(bad, [], `这些文件既没登记在 samples.json 里，也不是目录文档 / OS 元数据（要么登记，要么删掉）：\n  - ${bad.join('\n  - ')}`);
});

test('★ 目录白名单判据（纯函数）：OS 元数据放过；未登记的普通文件（真样本）必须红', () => {
  // ★ `listed` 是**合成集合**（2026-11-18 起盘上已无真样本）：本条判的是**纯函数**，不是盘上现状。
  //   ⛔ 不要换回 `allFiles()`：空清单会让它退化成空集，① 那组"已登记必须放过"就全靠 ② 的兜底通过 ⇒ 判据假松。
  const listed = new Set([
    'SAVE76.DAT',
    'SAVE76.STH',
    'SAVE77.DAT',
    'SAVE77.STH',
    'SAVE78.DAT',
    'SAVE78.STH',
    'SAVE79.DAT',
    'SAVE79.STH',
    'SAVE.DAT',
    'RT.DAT',
    ...FIXTURE_DOC_FILES,
  ]);

  // ① 已登记：载荷 / 系统容器 / 目录文档 —— 一律放过
  for (const n of ['SAVE76.DAT', 'SAVE76.STH', 'RT.DAT', 'SAVE.DAT', 'README.md', 'samples.json', 'samples.md', '.gitkeep']) {
    assert.equal(classifyFixtureEntry(n, listed), 'listed', `${n} 已登记（或属目录文档），必须放过`);
    assert.equal(isAllowedFixtureEntry(n, listed), true, `${n} 已登记（或属目录文档），必须放过`);
  }

  // ② OS 元数据：点文件（`._*` AppleDouble、Finder 的 `.DS_Store`）+ 非点文件的三项
  for (const n of ['.DS_Store', 'Thumbs.db', 'THUMBS.DB', 'desktop.ini', '._SAVE76.DAT', 'Icon\r']) {
    assert.equal(classifyFixtureEntry(n, listed), 'os-metadata', `${JSON.stringify(n)} 是 OS 元数据，必须放过`);
    assert.equal(isAllowedFixtureEntry(n, listed), true, `${JSON.stringify(n)} 是 OS 元数据，必须放过`);
  }

  // ③ 真样本 / 普通文件：**未登记就必须红** —— 这条守卫不许因为 ② 而变松
  for (const n of ['random.bin', 'SAVE75.DAT', 'SAVE76.DAT.bak', 'samples.json.bak', 'README']) {
    assert.equal(classifyFixtureEntry(n, listed), 'unknown', `${n} 未登记，必须红`);
    assert.equal(isAllowedFixtureEntry(n, listed), false, `${n} 未登记，必须红`);
  }

  // ④ 嵌套目录：判据只看 basename ⇒ 嵌套的 `.DS_Store` 一样忽略，嵌套的真样本一样红
  assert.equal(classifyFixtureEntry('saves/.DS_Store', listed), 'os-metadata', '嵌套目录的 .DS_Store 也必须忽略');
  assert.equal(classifyFixtureEntry('saves/Thumbs.db', listed), 'os-metadata', '嵌套目录的 Thumbs.db 也必须忽略');
  assert.equal(classifyFixtureEntry('saves/random.bin', listed), 'unknown', '嵌套的真样本同样不许放过');
  assert.equal(classifyFixtureEntry('saves\\random.bin', listed), 'unknown', '另一平台的路径分隔符同样不许绕过');
});

test('每个 .DAT 的槽头时刻 == wallClock(mtimeMs, tzOffsetMinutes)（mtime 已漂移则如实 skip）', (t) => {
  let checked = 0;
  let drifted = 0;
  const total = samples().samples.length;
  // ★ 0 个样本 ⇒ 没有可核对的对象，如实 skip（否则 `checked > 0 || drifted === total` 会 0===0 空转 pass）。
  if (total === 0) return t.skip('samples.json 里 0 个样本（2026-11-18 载荷已删除）⇒ 无槽头可核对');
  for (const s of samples().samples) {
    const rec = s.files.find((f) => f.name.endsWith('.DAT'));
    const p = path.join(FIXTURES, rec.name);
    // ★ 比对的是 **instant**（不是本机墙上时间）⇒ 跨时区也成立
    const cur = Math.round(fs.statSync(p).mtimeMs);
    if (Math.abs(cur - rec.mtimeMs) > 1000) {
      drifted += 1;
      // ★ 这条**不是 skip**：本用例仍然在断言其它样本，而且末尾有"不许全部静默跳过"的兜底断言。
      //   所以这里只报告"哪一个漂了"，措辞别写"跳过不变量断言"（那会让人以为整个用例没跑）。
      t.diagnostic(
        `${rec.name}: mtime 与记录的 instant 不一致（盘上 ${new Date(cur).toISOString()} / 记录 ${new Date(rec.mtimeMs).toISOString()}）` +
          `⇒ 该样本不参与断言；恢复：pnpm tools fixtures restore-mtime --write`,
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
