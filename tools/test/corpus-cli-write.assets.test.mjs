/** @env assets @kind safety @why corpus --scan/--normalize 写路径坏了，或 Windows 上吐了 CR */
/**
 * tools/test/corpus-cli-write.assets.test.mjs — **corpus 写命令**的冒烟（要真清单的 origin 都在盘上）
 *
 * ★ 从 `cli-write.test.mjs` 拆出来的原因（本轮测试分级）：这两条拿**真清单**当底稿，
 *   而守卫 #3/#4 会去现算那些 `origin` 文件的 sha256 ⇒ **要求装了游戏 / 资产在场**。
 *   实测（把 `gameInstall` 临时指空）：这两条**红**（`--scan` 退出 1；`--normalize` 撞上"11 处缺失"的回滚），
 *   而 fixtures 那三条照样绿 ⇒ 两类混在一起时，"写路径冒烟"会跟着机器环境一起红。
 *
 * ★ 不碰真 JSON：清单先拷到 `.tmp/` 下的副本，`--manifest` 指向副本 ⇒ `--write` 改的是副本。
 * ★ 跑 `--no-hash --no-recipe`：本用例只测**写入路径**；守卫 #4/#7 由 `pnpm tools corpus validate`
 *   与 `corpus-manifest.assets.test.mjs` 覆盖（recipe 要真跑 107 万行的保真断言，单次就十几秒）。
 *
 * 运行：`pnpm test:assets`（或 `pnpm test:all`）
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { DEFAULT_MANIFEST, loadManifest, resolveOrigin } from '../lib/manifest.mjs';
import { REPO_ROOT } from '../lib/paths.mjs';
import { sha256File } from '../lib/fsx.mjs';
import { main as corpusMain } from '../corpus.mjs';

const SCRATCH = path.join(REPO_ROOT, '.tmp', 'corpus-cli-write-test');

/** 真清单里那条"不入库 + 带 sha256"的条目（`--scan` 的目标） */
const SCAN_ID = 'reference/old-agents-md';

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
 * 守卫 #4 会现算 sha256），但把 `fixtures/save-samples` 的 origin 清空（自足条目），
 * 并把 `reference/old-agents-md` 的 sha256 抹掉 —— 那正是 `--scan` 要补的东西。
 */
function scratchManifest({ stripSha = true } = {}) {
  const m = JSON.parse(fs.readFileSync(DEFAULT_MANIFEST, 'utf8'));
  const target = m.entries.find((e) => e.id === SCAN_ID);
  assert.ok(target, `真清单里必须有 ${SCAN_ID}（本用例拿它当 --scan 的目标）`);
  const o = target.origin[0];
  if (stripSha) delete o.sha256;
  else o.sha256 = sha256File(resolveOrigin(REPO_ROOT, m.roots, o));
  const fx = m.entries.find((e) => e.id === 'fixtures/save-samples');
  assert.ok(fx, '真清单里必须有 fixtures/save-samples');
  fx.origin = [];
  return m;
}

const crCount = (p) => fs.readFileSync(p).filter((b) => b === 0x0d).length;

function withScratch(fn, opts = {}) {
  fs.rmSync(SCRATCH, { recursive: true, force: true });
  fs.mkdirSync(SCRATCH, { recursive: true });
  const manifest = path.join(SCRATCH, 'assets.json');
  fs.writeFileSync(manifest, `${JSON.stringify(scratchManifest(opts), null, 2)}\n`, 'utf8');
  try {
    fn({ manifest });
  } finally {
    fs.rmSync(SCRATCH, { recursive: true, force: true });
  }
}

test('corpus --scan --write：能跑通、能把缺的 sha256 补上、且产出全 LF', () => {
  withScratch(({ manifest }) => {
    const before = loadManifest(manifest).entries.find((e) => e.id === SCAN_ID);
    assert.equal(before.origin[0].sha256, undefined, '前置：这条 entry 本来没有 sha256');
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
