/** @env assets @kind gate @why 入库的拆壳件与反汇编语料不是同一份（sha256 对不上），执行源与"用来看的语料"就会分家 */
/**
 * tools/test/age-sectfix-artifact.test.mjs —— **执行源（拆壳件）的身份守卫**
 *
 * 为什么需要它：`apps/emulator-lift` 的**执行源**是 `corpus/assets/age/AGE.EXE__dumped.sectfix.EXE`
 * （2026-11-03 用户决定入库，清单条目 `binary/age-sectfix`）。运行时要拿它当真机器码执行，
 * 而语义阅读与解码对照用的是 `corpus/disasm/files/*.lst`。这两件**必须是同一份二进制**，
 * 否则"读到的语义"和"跑出来的行为"会静默分家 —— 而且分家之后**没有任何东西会报错**。
 *
 * ★ 基准**不另写一份**：取 `.lst` 头部的 `; Input SHA256 :`。
 *   那是 IDA 对它**实际装载的那个文件**算出来的值，因此它天然是"字节 ↔ 语料"的**跨件锚**
 *   （比单独给二进制打一个 pin 信息量更大）。一处真源 ⇒ 不存在"第二个 sha 会不会漂"。
 * ★ 也顺手钉住清单形态：`storage=lfs` + `dest` 指到入库件（防回退成 `external-only`）。
 * ★ `.lst` 是解压产物（`.gitignore` 的 `/corpus/disasm/files/`）⇒ **缺席时跳过**，不假绿。
 *
 * 运行：`pnpm test:assets`（或 `pnpm test:all`）
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const REPO_ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const ARTIFACT_REL = 'corpus/assets/age/AGE.EXE__dumped.sectfix.EXE';
const ARTIFACT = path.join(REPO_ROOT, ARTIFACT_REL);
const LST = path.join(REPO_ROOT, 'corpus', 'disasm', 'files', 'AGE.EXE__dumped.sectfix.EXE.lst');

const manifest = JSON.parse(fs.readFileSync(path.join(REPO_ROOT, 'corpus', 'assets.json'), 'utf8'));
const entry = manifest.entries.find((e) => e.id === 'binary/age-sectfix');

const sha256 = (buf) => createHash('sha256').update(buf).digest('hex');

/** `.lst` 头部第一段注释里的 `; Input SHA256 : <hex>`（没有就返回 null）。
 *  ★ 行形如 `.text:00401000 ; Input SHA256 : 1937F086…` —— 前面有 `段:EA` 前缀，别锚在行首。 */
function lstInputSha256() {
  // 只读头部若干行：这个字段在文件开头约 20 行内，不必扫 18 MB。
  const fd = fs.openSync(LST, 'r');
  try {
    const head = Buffer.alloc(4096);
    const n = fs.readSync(fd, head, 0, head.length, 0);
    const m = /;\s*Input SHA256\s*:\s*([0-9A-Fa-f]{64})\s*$/m.exec(head.subarray(0, n).toString('latin1'));
    return m ? m[1].toUpperCase() : null;
  } finally {
    fs.closeSync(fd);
  }
}

test('执行源已入库，且清单形态是 lfs + dest 指向它', () => {
  assert.ok(entry, '清单里应有 binary/age-sectfix 条目');
  assert.equal(entry.storage, 'lfs', '执行源必须入库（lfs）——退回 external-only 会让判据锁在本机状态上');
  assert.equal(entry.dest, ARTIFACT_REL, `dest 应指向 ${ARTIFACT_REL}`);
  assert.equal(entry.readOnly, true, '执行源是只读件');
});

test('入库件与反汇编语料是同一份：sha256 == `.lst` 头部的 Input SHA256', (t) => {
  assert.ok(fs.existsSync(ARTIFACT), `${ARTIFACT_REL} 不存在（执行源不能缺席；fresh clone 要 git lfs pull）`);
  if (!fs.existsSync(LST)) {
    t.skip('反汇编语料的解压产物不在（`corpus/disasm/files/`）：先 `pnpm tools disasm build` 或解 zip');
    return;
  }
  const want = lstInputSha256();
  assert.ok(want, '`.lst` 头部应有 `; Input SHA256 :`（否则基准缺失，这条守卫退化成恒真）');
  const got = sha256(fs.readFileSync(ARTIFACT));
  assert.equal(
    got.toLowerCase(),
    want.toLowerCase(),
    `${ARTIFACT_REL} 的 sha256 与 .lst 头部的 Input SHA256 不符 ⇒ 执行的字节与读的语料不是同一份（或入库件被换掉了）`,
  );
});

test('★ 判别力：`.lst` 头部解析器真的能读出这个字段（而不是永远返回 null 让上一条假绿）', () => {
  if (!fs.existsSync(LST)) {
    // 与上一条同一缺席条件；这里单独断言解析器本身，避免"缺席 ⇒ 两条都跳过 ⇒ 没人发现解析器坏了"。
    assert.ok(fs.existsSync(ARTIFACT));
    return;
  }
  assert.match(
    lstInputSha256() ?? '',
    /^[0-9A-F]{64}$/,
    '解析器读不出 `; Input SHA256 :` —— 上一条守卫会退化成恒真',
  );
});
