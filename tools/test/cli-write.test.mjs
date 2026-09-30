/**
 * tools/test/cli-write.test.mjs — **CLI 写入路径**的冒烟 + 行尾口径
 *
 * 为什么单独一份：`corpus --scan --write` 与 `fixtures --normalize --write` 是真实存在的写命令，
 * 却**没有任何测试跑到**；结果两个未定义标识符（`CARRYING` / `KNOWN_TOP_KEYS`）在重构里静默活了下来
 * ⇒ 谁会去用谁就吃 `ReferenceError`。本文件把这两条路（以及 normalize 的两种）钉住。
 *
 * 另一条不变量：**Windows 上也只许吐 LF**（`.gitattributes` 首行 `* text=auto eol=lf`）。
 * 这里按**字节**断言（不含 0x0d），不是"读成字符串看着像 LF"。
 *
 * ★ 不碰真 JSON：清单/样本都先拷到 `.tmp/` 下的副本，`--manifest` / `--samples` 指向副本；
 *   `--write` 因此改的是副本，真文件逐字节不动（finally 里连副本一起清掉）。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { DEFAULT_MANIFEST, loadManifest, resolveOrigin } from '../lib/manifest.mjs';
import { DEFAULT_SAMPLES, loadSamples } from '../lib/samples.mjs';
import { REPO_ROOT } from '../lib/paths.mjs';
import { sha256File } from '../lib/fsx.mjs';
import { main as corpusMain } from '../corpus.mjs';
import { main as fixturesMain } from '../fixtures.mjs';

const SCRATCH = path.join(REPO_ROOT, '.tmp', 'cli-write-test');
const REAL_SAMPLES = DEFAULT_SAMPLES;

/** 真清单里那条"不入库 + 带 sha256"的条目（`--scan` 的目标） */
const SCAN_ID = 'reference/old-agents-md';

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

/**
 * 造一份"能过全部守卫"的临时清单：拿真清单当底（所有 origin 都指向真仓里真实存在的文件，
 * 守卫 #4 会现算 sha256），但把 `fixtures/save-samples` 的 origin 清空（自足条目）、
 * 并把 `reference/old-agents-md` 的 sha256 抹掉 —— 那正是 `--scan` 要补的东西。
 *
 * @param {{stripSha?:boolean}} opts stripSha=true 时那份清单**本身不合法**（#4 会红），
 *   只适合测 `--scan`（它自己负责补）；测 `--normalize` 这类"直接落盘"的命令要用合法底稿。
 */
function scratchManifest({ stripSha = true } = {}) {
  const m = JSON.parse(fs.readFileSync(DEFAULT_MANIFEST, 'utf8'));
  const target = m.entries.find((e) => e.id === SCAN_ID);
  assert.ok(target, `真清单里必须有 ${SCAN_ID}（本用例拿它当 --scan 的目标）`);
  const o = target.origin[0];
  if (stripSha) {
    delete o.sha256;
  } else {
    // 现算真值（按清单 roots 解析，不手拼路径），让这份底稿自己能过 #4
    o.sha256 = sha256File(resolveOrigin(REPO_ROOT, m.roots, o));
  }
  const fx = m.entries.find((e) => e.id === 'fixtures/save-samples');
  assert.ok(fx, '真清单里必须有 fixtures/save-samples');
  fx.origin = [];
  return m;
}

const crCount = (p) => fs.readFileSync(p).filter((b) => b === 0x0d).length;
const hasLf = (p) => fs.readFileSync(p).includes(0x0a);

function withScratch(fn, opts = {}) {
  fs.rmSync(SCRATCH, { recursive: true, force: true });
  fs.mkdirSync(SCRATCH, { recursive: true });
  const manifest = path.join(SCRATCH, 'assets.json');
  const samples = path.join(SCRATCH, 'samples.json');
  fs.writeFileSync(manifest, `${JSON.stringify(scratchManifest(opts), null, 2)}\n`, 'utf8');
  fs.copyFileSync(REAL_SAMPLES, samples);
  try {
    fn({ manifest, samples });
  } finally {
    fs.rmSync(SCRATCH, { recursive: true, force: true });
  }
}

test('corpus --scan --write：能跑通、能把缺的 sha256 补上、且产出全 LF', () => {
  withScratch(({ manifest }) => {
    const before = loadManifest(manifest).entries.find((e) => e.id === SCAN_ID);
    assert.equal(before.origin[0].sha256, undefined, '前置：这条 entry 本来没有 sha256');

    // `--no-hash --no-recipe`：本用例只测**写入路径**。守卫 #4 的"现算 sha256 比盘上"由 corpus validate 覆盖，
    // 守卫 #7 的 recipe 断言由 corpus validate 与 corpus-manifest 的端到端用例覆盖；这里再跑一遍只是浪费
    // （recipe 要真跑 107 万行的保真断言 ⇒ 单次 normalize 复验就是十几秒）。
    assert.equal(run(() => corpusMain(['--scan', '--write', '--no-hash', '--no-recipe', '--manifest', manifest])), 0, '--scan --write 必须退出 0');
    const after = loadManifest(manifest).entries.find((e) => e.id === SCAN_ID);
    assert.match(after.origin[0].sha256 ?? '', /^[0-9a-f]{64}$/, '--scan 必须把 sha256 补上');
    assert.equal(crCount(manifest), 0, '清单产出必须是 LF（Windows 上也不许出现 CR）');
  });
});

test('corpus --normalize --write：跑通且产出全 LF（键序稳定 ⇒ 再跑一次同字节）', () => {
  withScratch(({ manifest }) => {
    const args = ['--normalize', '--write', '--no-hash', '--no-recipe', '--manifest', manifest];
    assert.equal(run(() => corpusMain(args)), 0, '--normalize --write 必须退出 0');
    const once = fs.readFileSync(manifest);
    assert.equal(run(() => corpusMain(args)), 0);
    assert.ok(once.equals(fs.readFileSync(manifest)), '规范形态必须幂等（同输入同字节）');
    assert.equal(crCount(manifest), 0, '清单产出必须是 LF');
  }, { stripSha: false });
});

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

test('真 JSON 未被这些用例改动（自足条目与文件级事实都还在）', () => {
  const real = loadManifest(DEFAULT_MANIFEST);
  const fx = real.entries.find((e) => e.id === 'fixtures/save-samples');
  assert.deepEqual(fx.origin, [], '真清单里 fixtures/save-samples 仍是自足条目');
  assert.ok(loadSamples(REAL_SAMPLES).samples.length >= 4, '真 samples.json 仍登记着样本');
  assert.ok(hasLf(DEFAULT_MANIFEST) && hasLf(REAL_SAMPLES));
});
