/** @env pure @kind safety @why fixtures 写/只读路径不写坏数据、或 Windows 上吐了 CR */
/**
 * tools/test/fixtures-cli-write.test.mjs — **fixtures 命令的写路径与只读路径**（不需要游戏/旧仓）
 *
 * 为什么要有它：`fixtures --normalize --write` 是真实存在的写命令，却曾**没有任何测试跑到**，
 * 结果两个未定义标识符（`CARRYING` / `KNOWN_TOP_KEYS`）在重构里静默活了下来 ⇒ 谁去用谁吃 `ReferenceError`。
 *
 * 另一条不变量：**Windows 上也只许吐 LF**（`.gitattributes` 首行 `* text=auto eol=lf`）。
 * 这里按**字节**断言（不含 0x0d），不是"读成字符串看着像 LF"。
 *
 * ★ **拆过一刀**（本轮测试分级）：原先本文件还含 `corpus --scan/--normalize` 两条 ——
 *   它们要**真清单里那些 origin 文件都在盘上**（= 装了游戏）⇒ 已移到 `corpus-cli-write.assets.test.mjs`。
 *   实测依据：把 `gameInstall` 临时指空，那两条**红**，而这里三条**照样绿**。
 *
 * ★ 全程不碰真 JSON：样本先拷到 `.tmp/` 下的副本，`--samples` 指向副本 ⇒ `--write` 改的是副本。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { DEFAULT_SAMPLES, loadSamples, saveSamples } from '../lib/samples.mjs';
import { REPO_ROOT } from '../lib/paths.mjs';
import { main as fixturesMain } from '../fixtures.mjs';

const SCRATCH = path.join(REPO_ROOT, '.tmp', 'fixtures-cli-write-test');

/** 把 stdout 静音跑一段（CLI 会打印计划；退出码才是判据） */
function run(fn) {
  const w = process.stdout.write.bind(process.stdout);
  process.stdout.write = () => true;
  try {
    return fn();
  } finally {
    process.stdout.write = w;
  }
}

/** 副本环境：`samples.json` 拷一份，读写都打在副本上 */
function withScratch(fn) {
  fs.rmSync(SCRATCH, { recursive: true, force: true });
  fs.mkdirSync(SCRATCH, { recursive: true });
  const samples = path.join(SCRATCH, 'samples.json');
  fs.copyFileSync(DEFAULT_SAMPLES, samples);
  try {
    fn({ samples });
  } finally {
    fs.rmSync(SCRATCH, { recursive: true, force: true });
  }
}

const crCount = (p) => fs.readFileSync(p).filter((b) => b === 0x0d).length;
const hasLf = (p) => fs.readFileSync(p).includes(0x0a);

test('fixtures --normalize --write：跑通、槽序规范、产出全 LF', () => {
  withScratch(({ samples }) => {
    const doc = loadSamples(samples);
    doc.samples.reverse(); // 搅乱顺序，看 --normalize 是否拉回规范形态
    fs.writeFileSync(samples, `${JSON.stringify(doc, null, 2)}\n`, 'utf8');

    assert.equal(run(() => fixturesMain(['--normalize', '--write', '--samples', samples])), 0, 'fixtures --normalize --write 必须退出 0');
    const out = loadSamples(samples);
    assert.deepEqual(out.samples.map((s) => s.slot), [...out.samples.map((s) => s.slot)].sort(), '槽必须按号排序');
    assert.equal(crCount(samples), 0, 'samples.json 产出必须是 LF');
  });
});

test('fixtures --list：只读路径不写、不改动 samples.json 的字节', () => {
  withScratch(({ samples }) => {
    const before = fs.readFileSync(samples);
    assert.equal(run(() => fixturesMain(['--list', '--samples', samples])), 0);
    assert.ok(before.equals(fs.readFileSync(samples)), '--list 不许写文件');
  });
});

test('真 JSON 未被这些用例改动（真 samples.json 仍登记着样本、仍是 LF）', () => {
  assert.ok(loadSamples(DEFAULT_SAMPLES).samples.length >= 4, '真 samples.json 仍登记着样本');
  assert.ok(hasLf(DEFAULT_SAMPLES), '真 samples.json 必须是 LF');
});

test('★ containers 的写路径守卫：与槽载荷重名 / 自身重名 / 缺 what ⇒ 拒绝并**回滚**（一个文件只能有一个身份）', () => {
  const base = { mtimeMs: 1, mtimeLocal: '2026-01-01T00:00:00', tzOffsetMinutes: 480 };
  withScratch(({ samples }) => {
    // ① 与槽载荷重名：同一份文件既是 SAVE76.DAT 又是"系统容器" ⇒ 两条不变量会互相打架
    const clash = loadSamples(samples);
    clash.containers = [{ ...base, name: 'SAVE76.DAT', what: '伪装成容器' }];
    const before = fs.readFileSync(samples);
    const r1 = saveSamples(clash, samples);
    assert.equal(r1.ok, false, '与槽载荷重名必须被写入口拒绝');
    assert.match(r1.reason, /一个文件只能有一个身份/, r1.reason);
    assert.ok(before.equals(fs.readFileSync(samples)), '拒绝时必须把原文件写回（不许留半坏的 JSON）');

    // ② 容器自身重名
    const dup = loadSamples(samples);
    dup.containers = [
      { ...base, name: 'RT.DAT', what: 'a' },
      { ...base, name: 'RT.DAT', what: 'b' },
    ];
    const r2 = saveSamples(dup, samples);
    assert.equal(r2.ok, false, '容器重名必须被拒绝');
    assert.match(r2.reason, /重名/, r2.reason);

    // ③ 缺 what：容器的身份就是"文件名 + what"，缺了它这份样本说不清是什么
    const noWhat = loadSamples(samples);
    noWhat.containers = [{ ...base, name: 'RT.DAT' }];
    const r3 = saveSamples(noWhat, samples);
    assert.equal(r3.ok, false, '缺 what 必须被拒绝');
    assert.match(r3.reason, /缺 what/, r3.reason);
  });
});
