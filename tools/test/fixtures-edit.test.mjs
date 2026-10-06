/** @env pure @kind safety @why 样本台账的写路径写坏了数据 */
/**
 * tools/test/fixtures-edit.test.mjs — **编辑路径**（`tools/fixtures.mjs`）的安全性质
 *
 * 为什么单独测：`samples.json` 的"怎么改"必须是一条命令，而命令必须**写不坏**。
 * 这里测的是写入路径的纯性质（不需要真跑一遍加样本，那需要临时 repo 根）：
 *   ① 规范形态与键序；② 结构校验能拒绝坏数据；③ 写后复验不通过 ⇒ **回滚**；
 *   ④ 墙上时间换算与机器时区无关；⑤ 多余顶层键可被剔除。
 *
 * 运行：`pnpm test`
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

import { canonicalSamples, loadSamples, saveSamples, stripUnknownTopKeys, structuralProblems } from '../lib/samples.mjs';
import { wallClock } from '../lib/time.mjs';

const good = () => ({
  schemaVersion: 1,
  _doc: '（工具会重写这一行）',
  samples: [
    {
      slot: '79',
      where: 'SN0000 开头',
      files: [
        { name: 'SAVE79.DAT', mtimeMs: 1789749538228, mtimeLocal: '2026-09-19T00:38:58', tzOffsetMinutes: 480 },
        { name: 'SAVE79.STH', mtimeMs: 1789749538273, mtimeLocal: '2026-09-19T00:38:58', tzOffsetMinutes: 480 },
      ],
    },
    {
      slot: '76',
      where: 'SC0000 第一个战斗前',
      files: [
        { name: 'SAVE76.DAT', mtimeMs: 1790130898645, mtimeLocal: '2026-09-23T10:34:58', tzOffsetMinutes: 480 },
        { name: 'SAVE76.STH', mtimeMs: 1790130898689, mtimeLocal: '2026-09-23T10:34:58', tzOffsetMinutes: 480 },
      ],
    },
  ],
});

const tmpFile = () => path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'amayui-fixtures-')), 'samples.json');

test('规范形态：槽按号排序、每槽固定 DAT/STH 顺序、同输入同字节', () => {
  const a = canonicalSamples(good());
  const b = canonicalSamples(good());
  assert.equal(a, b);
  const doc = JSON.parse(a);
  assert.deepEqual(doc.samples.map((s) => s.slot), ['76', '79'], '槽必须按号排序');
  assert.deepEqual(
    doc.samples.map((s) => s.files.map((f) => f.name)),
    [
      ['SAVE76.DAT', 'SAVE76.STH'],
      ['SAVE79.DAT', 'SAVE79.STH'],
    ],
    '每槽必须 DAT 在前、STH 在后',
  );
  assert.ok(a.endsWith('\n'));
  // 顶层键序固定
  assert.match(a, /"schemaVersion": 1,\n {2}"_doc"/);
});

test('结构校验能拒绝坏数据（缺 where / 文件不成对 / 缺 mtimeMs / 缺 tz / 槽重复）', () => {
  const noWhere = good();
  noWhere.samples[0].where = '';
  assert.match(structuralProblems(noWhere).join('\n'), /缺 where/);

  const unpaired = good();
  unpaired.samples[0].files.pop();
  assert.match(structuralProblems(unpaired).join('\n'), /文件必须是/);

  const noMs = good();
  delete noMs.samples[1].files[0].mtimeMs;
  assert.match(structuralProblems(noMs).join('\n'), /mtimeMs/);

  const noTz = good();
  delete noTz.samples[1].files[1].tzOffsetMinutes;
  assert.match(structuralProblems(noTz).join('\n'), /tzOffsetMinutes/);

  const dup = good();
  dup.samples[1].slot = '79'; // 与 samples[0] 撞号（且文件名对不上）
  assert.match(structuralProblems(dup).join('\n'), /槽重复/);

  assert.deepEqual(structuralProblems(good()), [], '好数据必须无问题');
});

test('★ 写后复验不通过 ⇒ 回滚（文件与原文逐字节相同）', () => {
  const p = tmpFile();
  fs.writeFileSync(p, canonicalSamples(good()), 'utf8');
  const before = fs.readFileSync(p, 'utf8');

  const bad = good();
  bad.samples[0].files = [bad.samples[0].files[0]]; // 只剩 .DAT ⇒ 不成对
  const res = saveSamples(bad, p);
  assert.equal(res.ok, false);
  assert.equal(res.restored, true);
  assert.equal(fs.readFileSync(p, 'utf8'), before, '被拒后文件必须与原文逐字节相同');
});

test('正常写入 + 回读：落盘的就是规范形态', () => {
  const p = tmpFile();
  fs.writeFileSync(p, '{}\n', 'utf8');
  const res = saveSamples(good(), p);
  assert.equal(res.ok, true);
  assert.equal(fs.readFileSync(p, 'utf8'), canonicalSamples(good()));
  assert.equal(loadSamples(p).samples.length, 2);
});

test('`_doc` 由工具拥有（文件里写什么都无所谓，落盘时被重写成指向自描述的那一行）', () => {
  const doc = good();
  doc._doc = '手写的旧说明';
  const out = JSON.parse(canonicalSamples(doc));
  assert.notEqual(out._doc, '手写的旧说明', '工具必须覆盖文件里的 _doc');
  assert.match(out._doc, /describe/, '工具拥有的 _doc 必须指向脚本自描述');
});

test('多余顶层键可被显式剔除（schema 变过之后用它拉回规范形态）', () => {
  const doc = good();
  doc.capturedTzOffsetMinutes = 480; // 遗留键
  assert.deepEqual(stripUnknownTopKeys(doc), ['capturedTzOffsetMinutes']);
  assert.equal('capturedTzOffsetMinutes' in doc, false);
});

test('★ 墙上时间换算与跑测试的机器时区无关', () => {
  // 2026-09-19T00:38:58 +08:00 == 2026-09-18T16:38:58Z
  const ms = Date.UTC(2026, 8, 18, 16, 38, 58);
  assert.equal(wallClock(ms, 480), '2026-09-19T00:38:58');
  assert.equal(wallClock(ms, 540), '2026-09-19T01:38:58'); // UTC+9
  assert.equal(wallClock(ms, 0), '2026-09-18T16:38:58'); // UTC
});
