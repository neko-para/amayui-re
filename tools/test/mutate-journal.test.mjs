/** @env pure @kind safety @why journal 自愈的判据被破坏 —— 第三种内容被静默覆盖、非法 journal 被删掉、CTest 条目只还原源文件不重建构建目录、或 CLI 不拒绝运行 —— 于是被中断的工作树留在变异态，下一次「跑绿」可能是假绿 */
/**
 * tools/test/mutate-journal.test.mjs —— `pnpm test:mutation` 的**崩溃安全**守卫
 *
 * 被测对象（`tools/mutate-check.mjs`，本仓"守卫自检"工具）：
 * 它在施加变异**之前**把 `{file, before, mutated, guard?}` 原子写进
 * `.tmp/mutate-check/journal.json`；下一次启动（含 `--list`）靠这份预写日志判断
 * "盘上是原文 / 是变异文 / 是第三种内容"，并据此恢复或拒绝运行。
 *
 * 这里守的是**判据本身**（不是"现在清单里有几条"）：
 *   a. 盘上是变异文 ⇒ 写回原文 + 删除 journal；
 *   b. 盘上是第三种内容 ⇒ **拒绝运行**、**一个字节也不写**（含同批里可恢复的那条）、journal 保留；
 *   c. 没有 journal ⇒ 幂等无操作；
 *   d. journal 非法 JSON / 形态不对 ⇒ **拒绝运行**（不崩溃、也不沉默删掉）；
 *   e. 带 CTest 守卫信息的条目恢复后**重建构建目录**；重建失败 ⇒ journal 保留（exe 可能还是变异版）；
 *   f. 真 spawn CLI（`node tools/mutate-check.mjs --list`）：拒绝场景退出码非 0，恢复场景真的写回原文。
 *
 * ★ 全程只在 `.tmp/mutate-journal-test/` 下造文件（gitignored），**不碰真实工作树**；
 *   子进程的 journal 位置用 `AMAYUI_MUTATE_JOURNAL` 指到临时目录（该环境变量只为此存在）。
 */
import { after, test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';

import {
  JOURNAL_REL,
  classifyJournalEntry,
  readMutationJournal,
  recoverMutationJournal,
  resolveJournalPath,
  writeMutationJournal,
} from '../mutate-check.mjs';
import { REPO_ROOT } from '../lib/paths.mjs';

const SCRATCH = path.join(REPO_ROOT, '.tmp', 'mutate-journal-test');
const CLI = path.join(REPO_ROOT, 'tools', 'mutate-check.mjs');

after(() => fs.rmSync(SCRATCH, { recursive: true, force: true }));

/** 每个用例一个干净根目录（在 `.tmp/` 下，不在系统 tmp） */
function makeRoot() {
  fs.mkdirSync(SCRATCH, { recursive: true });
  const root = fs.mkdtempSync(path.join(SCRATCH, 'case-'));
  return { root, journalPath: path.join(root, '.tmp', 'mutate-check', 'journal.json') };
}

/** 在 root 里造文件；返回它相对 root 的路径（journal 里存的是相对路径） */
function put(root, rel, text) {
  const abs = path.join(root, rel);
  fs.mkdirSync(path.dirname(abs), { recursive: true });
  fs.writeFileSync(abs, text, 'utf8');
  return rel;
}

const read = (root, rel) => fs.readFileSync(path.join(root, rel), 'utf8');

/** 收 log 行（恢复函数缺省打 stderr；单元用例要断言文案） */
function collector() {
  const lines = [];
  return { lines, log: (line) => lines.push(line) };
}

test('a) 盘上是变异文 ⇒ 写回原文 + 删除 journal，并报「已从上次中断恢复 N 个文件」', () => {
  const { root, journalPath } = makeRoot();
  try {
    const before = 'export const base = 0x5d894;\n';
    const mutated = 'export const base = 0x5d880;\n';
    const file = put(root, 'packages/x/layout.mts', mutated);
    writeMutationJournal(journalPath, [{ file, before, mutated, guard: 'tools/test/x.test.mjs' }]);

    const { lines, log } = collector();
    const r = recoverMutationJournal({ root, journalPath, log });

    assert.equal(r.ok, true);
    assert.equal(r.restored, 1);
    assert.equal(read(root, file), before, '必须写回原文');
    assert.equal(fs.existsSync(journalPath), false, '恢复成功后 journal 必须删除');
    assert.ok(
      lines.some((l) => l.includes('已从上次中断恢复 1 个文件')),
      `要报告恢复了几个（实际：${JSON.stringify(lines)}）`,
    );
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test('b) 第三种内容 ⇒ 拒绝运行、一个字节也不写、journal 保留', () => {
  const { root, journalPath } = makeRoot();
  try {
    // 第 1 条本来可以恢复（盘上 = mutated），第 2 条是人改过的第三种内容。
    // ★ 判据：拒绝必须是**全有或全无** —— 第 1 条也不许被写。
    const f1 = put(root, 'a/mutated.mts', 'MUTATED-A\n');
    const f2 = put(root, 'b/human.mts', 'HUMAN-EDIT\n');
    writeMutationJournal(journalPath, [
      { file: f1, before: 'ORIGINAL-A\n', mutated: 'MUTATED-A\n' },
      { file: f2, before: 'ORIGINAL-B\n', mutated: 'MUTATED-B\n' },
    ]);

    const { lines, log } = collector();
    const r = recoverMutationJournal({ root, journalPath, log });

    assert.equal(r.ok, false);
    assert.equal(r.reason, 'conflict');
    assert.equal(read(root, f1), 'MUTATED-A\n', '同批里可恢复的那条也不许写（部分恢复比不恢复更危险）');
    assert.equal(read(root, f2), 'HUMAN-EDIT\n', '第三种内容不许被覆盖');
    assert.equal(fs.existsSync(journalPath), true, '拒绝运行时 journal 必须保留');
    assert.ok(
      lines.some((l) => l.includes('工作树内容与 journal 不符，不能安全恢复')),
      `拒绝文案必须点名（实际：${JSON.stringify(lines)}）`,
    );
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test('c) 没有 journal ⇒ 幂等无操作（不建文件、不报恢复）', () => {
  const { root, journalPath } = makeRoot();
  try {
    put(root, 'x.mts', 'ORIGINAL\n');
    const { lines, log } = collector();

    const first = recoverMutationJournal({ root, journalPath, log });
    const second = recoverMutationJournal({ root, journalPath, log });

    assert.equal(first.ok, true);
    assert.equal(first.existed, false);
    assert.equal(first.restored, 0);
    assert.equal(second.ok, true, '幂等：第二次仍然是"无 journal ⇒ 正常"');
    assert.equal(fs.existsSync(journalPath), false);
    assert.deepEqual(lines, [], '没有 journal 时不该有恢复报告');
    assert.equal(read(root, 'x.mts'), 'ORIGINAL\n');
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test('d) journal 非法 JSON / 形态不对 ⇒ 拒绝运行（不崩溃，也不沉默删掉）', () => {
  const { root, journalPath } = makeRoot();
  try {
    put(root, 'x.mts', 'ORIGINAL\n');
    fs.mkdirSync(path.dirname(journalPath), { recursive: true });

    const cases = [
      ['非法 JSON', '{ "version": 1, "entries": [ oops'],
      ['形态不对（entries 不是数组）', '{"version":1,"entries":{}}'],
      ['版本不认识', '{"version":99,"entries":[]}'],
      ['条目缺 mutated', '{"version":1,"entries":[{"file":"x.mts","before":"ORIGINAL\\n"}]}'],
      ['条目指到仓库外', '{"version":1,"entries":[{"file":"../escape.mts","before":"a","mutated":"b"}]}'],
    ];
    for (const [what, text] of cases) {
      fs.writeFileSync(journalPath, text, 'utf8');
      const { lines, log } = collector();
      const r = recoverMutationJournal({ root, journalPath, log });
      assert.equal(r.ok, false, `${what}：必须拒绝运行`);
      assert.equal(r.reason, 'invalid', `${what}：拒绝理由必须是 invalid`);
      assert.equal(fs.existsSync(journalPath), true, `${what}：journal 必须保留（⛔ 不许沉默删掉）`);
      assert.equal(fs.readFileSync(journalPath, 'utf8'), text, `${what}：journal 字节不许被改`);
      assert.ok(lines.length > 0, `${what}：必须报告原因，而不是沉默`);
    }
    // ★ 读 journal 本身不写盘：整个用例里目标文件一个字节都没动
    assert.equal(read(root, 'x.mts'), 'ORIGINAL\n');
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test('e) CTest 条目：恢复后重建构建目录；重建失败 ⇒ journal 保留', () => {
  const { root, journalPath } = makeRoot();
  try {
    const before = 'case Op::Add: return a + b;\n';
    const mutated = 'case Op::Add: return a + b + 1u;\n';
    const file = put(root, 'apps/emulator-lift/src/runtime/exec_integer.cpp', mutated);
    writeMutationJournal(journalPath, [
      { file, before, mutated, guard: 'ctest:conformance', ctest: 'conformance', buildDir: 'apps/emulator-lift/out' },
    ]);

    // ① 重建失败：源文件已还原，但 exe 可能还是变异版 ⇒ journal 必须留着
    const fail = collector();
    const bad = recoverMutationJournal({
      root,
      journalPath,
      log: fail.log,
      build: () => ({ status: 1 }),
    });
    assert.equal(bad.ok, false);
    assert.equal(bad.reason, 'rebuild-failed');
    assert.equal(read(root, file), before, '源文件仍应还原（重建失败只挡"自愈完成"，不挡写回原文）');
    assert.equal(fs.existsSync(journalPath), true, '重建失败 ⇒ journal 保留');

    // ② 重建成功：用假的 build 记录调用，别真跑 cmake
    const dirs = [];
    const okLog = collector();
    const good = recoverMutationJournal({
      root,
      journalPath,
      log: okLog.log,
      build: (logFd, dir) => {
        dirs.push(dir);
        return { status: 0 };
      },
    });
    assert.equal(good.ok, true);
    assert.deepEqual(dirs, ['apps/emulator-lift/out'], '必须重建该条目记下的构建目录（否则 exe 仍是变异版）');
    assert.equal(good.rebuilt, 1);
    assert.equal(fs.existsSync(journalPath), false);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test('f) 真 spawn CLI（--list）：拒绝场景退出码非 0；可恢复场景写回原文', () => {
  const { root, journalPath } = makeRoot();
  try {
    const relative = (abs) => path.relative(REPO_ROOT, abs).split(path.sep).join('/');
    const run = (extraEnv = {}) => {
      const of = fs.openSync(path.join(root, 'stdout.txt'), 'w');
      const ef = fs.openSync(path.join(root, 'stderr.txt'), 'w');
      let res;
      try {
        res = spawnSync(process.execPath, [CLI, '--list'], {
          cwd: REPO_ROOT,
          env: { ...process.env, AMAYUI_MUTATE_JOURNAL: journalPath, ...extraEnv },
          stdio: ['ignore', of, ef],
        });
      } finally {
        fs.closeSync(of);
        fs.closeSync(ef);
      }
      return {
        status: res.status,
        stdout: fs.readFileSync(path.join(root, 'stdout.txt'), 'utf8'),
        stderr: fs.readFileSync(path.join(root, 'stderr.txt'), 'utf8'),
      };
    };

    // ① 第三种内容 ⇒ 拒绝运行（exit 3），文件与 journal 都不动
    const victim = path.join(root, 'victim.txt');
    fs.writeFileSync(victim, 'HUMAN\n', 'utf8');
    const rel = relative(victim);
    writeMutationJournal(journalPath, [{ file: rel, before: 'ORIGINAL\n', mutated: 'MUTATED\n' }]);
    const refused = run();
    assert.notEqual(refused.status, 0, '判不了就必须非 0 退出');
    assert.match(refused.stderr, /工作树内容与 journal 不符，不能安全恢复/);
    assert.equal(fs.readFileSync(victim, 'utf8'), 'HUMAN\n', '拒绝时不许写目标文件');
    assert.equal(fs.existsSync(journalPath), true, '拒绝时 journal 保留');

    // ② 盘上是变异文 ⇒ --list 也先自愈（写回原文、删 journal、退出码 0）
    fs.writeFileSync(victim, 'MUTATED\n', 'utf8');
    writeMutationJournal(journalPath, [{ file: rel, before: 'ORIGINAL\n', mutated: 'MUTATED\n' }]);
    const healed = run();
    assert.equal(healed.status, 0, `自愈后 --list 必须可用（stderr：${healed.stderr}）`);
    assert.equal(fs.readFileSync(victim, 'utf8'), 'ORIGINAL\n', 'CLI 启动时必须把变异文写回原文');
    assert.equal(fs.existsSync(journalPath), false, '自愈完成后 journal 必须删除');
    assert.match(healed.stderr, /已从上次中断恢复 1 个文件/);
    assert.ok(healed.stdout.includes('⇒ 守卫'), '--list 仍要列清单（行为不变）');
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test('g) journal 落点必须在 `.tmp/`（gitignored），且分类函数能区分三种状态', () => {
  // ★ 落点错了会让 journal 变成"工作树里的新文件"（`git status` 变脏 / 被别的守卫当成自有 JSON）
  assert.equal(JOURNAL_REL, '.tmp/mutate-check/journal.json');
  const ignore = fs.readFileSync(path.join(REPO_ROOT, '.gitignore'), 'utf8');
  assert.match(ignore, /^\/\.tmp$/m, '`.tmp/` 必须被 gitignore 覆盖（journal 不许污染工作树）');
  assert.equal(resolveJournalPath(REPO_ROOT), path.join(REPO_ROOT, JOURNAL_REL));
  assert.equal(readMutationJournal(path.join(REPO_ROOT, '.tmp', 'definitely-no-such-journal.json')).ok, true);

  const { root, journalPath } = makeRoot();
  try {
    const entry = { file: 'x.mts', before: 'A\n', mutated: 'B\n' };
    put(root, 'x.mts', 'A\n');
    assert.equal(classifyJournalEntry(root, entry).state, 'before');
    put(root, 'x.mts', 'B\n');
    assert.equal(classifyJournalEntry(root, entry).state, 'mutated');
    put(root, 'x.mts', 'C\n');
    assert.equal(classifyJournalEntry(root, entry).state, 'conflict');
    assert.equal(classifyJournalEntry(root, { ...entry, file: 'gone.mts' }).state, 'conflict');
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});
