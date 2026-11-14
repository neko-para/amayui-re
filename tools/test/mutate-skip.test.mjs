/** @env pure @kind gate @suite toolchain @why CTest 守卫被 skip（SKIP_RETURN_CODE 77）时，变异自检会把它记成「❌ 守卫没红」或「✅ 守卫红了」—— 前者让 `pnpm test:mutation` 在无 GPU 的机器上整档坏掉，后者是假绿；两种都不该发生 */
/**
 * tools/test/mutate-skip.test.mjs —— `tools/mutate-check.mjs` 的 **CTest skip 判定**守卫
 * （跳过的守卫 ⇒ 第四态「不可判定」，⛔ 不是红、⛔ 不是绿）
 *
 * 为什么需要它：`SKIP_RETURN_CODE 77` 让 CTest 把用例记成 `Skipped`，而 `ctest` 自身退出码
 * **仍然是 0**（本机实测：`ctest -R <skip 用例> -V` 打 `***Skipped`、exit 0）。只看退出码的话：
 *   - 记成「❌ 守卫没红」= **假红**（无 GPU 机器上 `pnpm test:mutation` 整档坏掉）；
 *   - 若某配置把 77 透出成退出码，又会记成「✅ 守卫红了」= **假绿**（更坏）。
 * ⇒ 判据（本文件）：
 *   a. **变异那一跑被 skip**（基线绿）⇒ `unverifiableKind:'other'` + `⏭ **无法判定（其它）**`，
 *      `ok:false`、verdict **不含**「守卫红了」「守卫没红」；还原 + 全文比对仍执行。
 *   b. **基线被 skip** ⇒ 本批什么都证明不了 ⇒ 直接「无法判定（其它）」，且**一个字节都不改**源文件
 *      （不施加变异）。
 *   c. 判据来源是 `-V` 日志里的 `***Skipped` **或**退出码 77（两条都认，⛔ 只认一条会在换配置后漏）。
 *
 * ★ 用**假 CTest 命令**（`node -e` 打一行 `***Skipped`、exit 0）而不是真 CTest：这里要验的是
 *   "日志 → 四态判定"的接线；真 CTest 需要 CMake 构建目录与几分钟编译。真 CTest 的
 *   `***Skipped` 输出格式已在本机手工核过（见 `tickets.md` §2 ⑥ 的验收）。
 * ★ 只写 `.tmp/mutate-skip-test/`（gitignored），⛔ 不碰真实工作树、不构建 C++。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { ctestSkippedFromLog, runCtestGuard, runMutationEntry } from '../mutate-check.mjs';
import { REPO_ROOT } from '../lib/paths.mjs';

const SCRATCH = path.join(REPO_ROOT, '.tmp', 'mutate-skip-test');

// 假 CTest：读 `AMAYUI_SKIP_PROBE` 指向的文件；含 `SKIPME` ⇒ 打一行 `***Skipped` 后 exit 0
// （真 CTest 在 `SKIP_RETURN_CODE 77` 命中时的行为：退出码 0、日志里一行 `***Skipped`）。
const FAKE_CTEST = `
const fs = require('fs');
const p = process.env.AMAYUI_SKIP_PROBE;
const text = p && fs.existsSync(p) ? fs.readFileSync(p, 'utf8') : '';
if (text.includes('SKIPME')) {
  process.stdout.write('1/1 Test #1: fake ...***Skipped   0.01 sec\\n');
  process.exit(0);
}
process.exit(0);
`;

function setup() {
  fs.mkdirSync(SCRATCH, { recursive: true });
  const target = path.join(SCRATCH, `target-${process.pid}-${Math.random().toString(36).slice(2)}.txt`);
  return { target, rel: path.relative(REPO_ROOT, target).replaceAll('\\', '/') };
}

/** `runCtest` 注入：跑假 CTest（不构建 ⇒ 不碰真构建目录），把 skip 探针指到 `target` */
function fakeRunCtest(target) {
  return (name, logFd, buildDir, opts = {}) =>
    runCtestGuard(name, logFd, buildDir, {
      ...opts,
      root: REPO_ROOT,
      cwd: REPO_ROOT,
      skipBuild: true,
      command: process.execPath,
      args: ['-e', FAKE_CTEST],
      env: { AMAYUI_SKIP_PROBE: target },
    });
}

function baseEntry(rel, from, to) {
  return {
    file: rel,
    from,
    to,
    what: '假 CTest 条目的 skip 判定',
    guard: 'ctest:fake-skip',
    ctest: 'fake-skip',
    buildDir: 'fake/out',
  };
}

test('a) 变异那一跑被 skip ⇒ 记「不可判定（其它）」，⛔ 不是红也不是绿；还原仍执行', async () => {
  const { target, rel } = setup();
  fs.writeFileSync(target, 'AAA', 'utf8');
  const verified = [];
  const r = await runMutationEntry(baseEntry(rel, 'AAA', 'SKIPME'), {
    root: REPO_ROOT,
    runCtest: fakeRunCtest(target),
    rebuild: () => ({ status: 0 }),
    verifyCtest: (name) => {
      verified.push(name);
      return { status: 0 };
    },
  });
  assert.equal(r.restoreProblem, null, `还原必须成功（实际：${r.restoreProblem}）`);
  const res = r.result;
  assert.equal(res.unverifiable, true, `skip ⇒ 不可判定（实际：${JSON.stringify(res)}）`);
  assert.equal(res.unverifiableKind, 'other', `skip 属于"其它"而不是 timeout（实际：${res.unverifiableKind}）`);
  assert.equal(res.ok, false, 'skip 不能算「守卫红了」');
  assert.match(res.verdict, /无法判定（其它）/, `verdict 要点明不可判定（实际：${res.verdict}）`);
  assert.ok(!res.verdict.includes('守卫红了'), `⛔ skip 不许记成「守卫红了」（实际：${res.verdict}）`);
  assert.ok(!res.verdict.includes('守卫没红'), `⛔ skip 不许记成「守卫没红」（假红：整档自检坏掉）（实际：${res.verdict}）`);
  assert.match(res.ran?.skipReason ?? '', /SKIP_RETURN_CODE 77/, '原因要能排障（指向 SKIP_RETURN_CODE 77）');
  assert.equal(fs.readFileSync(target, 'utf8'), 'AAA', '变异必须被还原（全文比对）');
  assert.deepEqual(verified, ['fake-skip'], '还原后仍要复验基线（与其它四态同一条路径）');
});

test('b) 基线被 skip ⇒ 直接「不可判定（其它）」，且不施加变异（一个字节都不改）', async () => {
  const { target, rel } = setup();
  fs.writeFileSync(target, 'SKIPME', 'utf8');
  let ranGuard = false;
  const r = await runMutationEntry(baseEntry(rel, 'SKIPME', 'ZZZ'), {
    root: REPO_ROOT,
    runCtest: (name, logFd, buildDir, opts) => {
      ranGuard = true;
      return fakeRunCtest(target)(name, logFd, buildDir, opts);
    },
    rebuild: () => ({ status: 0 }),
    verifyCtest: () => ({ status: 0 }),
  });
  assert.equal(r.restoreProblem, null, `不该走到还原（实际：${r.restoreProblem}）`);
  const res = r.result;
  assert.equal(res.unverifiable, true, `基线 skip ⇒ 不可判定（实际：${JSON.stringify(res)}）`);
  assert.match(res.verdict, /基线被 skip/, `verdict 要点名"基线被 skip"（实际：${res.verdict}）`);
  assert.ok(!res.verdict.includes('守卫没红'), `⛔ 基线 skip 不许记成「守卫没红」（实际：${res.verdict}）`);
  assert.equal(ranGuard, true, '基线那一跑必须真的执行过');
  assert.equal(fs.readFileSync(target, 'utf8'), 'SKIPME', '基线 skip ⇒ 不许施加变异');
});

test('c) 判据两条都认：日志里的 ***Skipped 或退出码 77（缺一不可）', () => {
  assert.equal(ctestSkippedFromLog('1/1 Test #1: x ...***Skipped   0.01 sec\n', 0), true, '有日志标记 ⇒ skip');
  assert.equal(ctestSkippedFromLog('', 77), true, '退出码 77 透出 ⇒ skip');
  assert.equal(ctestSkippedFromLog('1/1 Test #1: x ...   Passed    0.01 sec\n', 0), false, '正常通过不许算 skip');
  assert.equal(ctestSkippedFromLog('', 1), false, '守卫真红（退出码 1）不许算 skip');
});
