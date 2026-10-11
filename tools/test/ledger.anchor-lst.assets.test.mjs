#!/usr/bin/env node
/** @env assets @kind gate @why 台账的 `bin` 锚可以落在**指令中间** / 不在 `.lst` 行首而没有任何判据发现 ⇒ "锚 EA 与 claim 引用的地址错位（0x1000 级 / 指令中间）"整类绿而错 */
/**
 * tools/test/ledger.anchor-lst.assets.test.mjs —— ★ **站点口径的永久守卫**（本单的核心收口物）
 *
 * ## 口径（用户已批准，见需求单 `REQ-01M4N3QC4SYG4C36QDBQCFRMSW`）
 * * **默认**：`bin` 锚只许落 **站点起点** = `.lst` 里行首那条真指令的地址
 *   （另有"折叠 / 孤立 CHUNK 站点"：该 EA 处只有 `;` 注释行 —— 它是 listing 的**位置**，同样算站点）；
 * * **例外**：**落指令中间**的锚必须（a）claim 里说清"是哪条指令的第几个字节"、且（b）带 `len` + `sha256`
 *   ⇒ 逐条登记进 `tools/lib/anchor-lst.mjs` 的 `AUDIT_ALLOWLIST`（**B 类不在表里 ⇒ 红**）。
 *
 * ## 为什么必须在**真语料**上跑（合成夹具不够）
 * 普查踩出来的坑全是**真 listing 的形状**：同一个 EA 上叠着 `proc near` + 栈变量定义 + 指令（实测
 * `sub_48F000` 那 9 行）；`; [00000106 BYTES: COLLAPSED FUNCTION …]` 这种"只有注释行的真站点"。
 * 合成夹具只钉**判据的形状**，真语料才证明"在场 345 个锚现在全在站点上"。
 *
 * ## 怎么才会红
 * 1. `tools/lib/anchor-lst.mjs` 去掉"B 类必须在 allowlist"这条判据（`node tools/mutate-check.mjs --list`）
 *    ⇒ 下面第 1、6 条当场红；
 * 2. allowlist 里某条的 `EA` 挪一字节 / `len` 或 `sha256` 与现算不符 ⇒ 第 2 条红；
 * 3. 台账里新增一条落指令中间的锚（或把某个站点锚改歪）而没进 allowlist ⇒ 第 1 条红（**那正是要红的**）；
 * 4. 某条被裁决过的锚不再是站点（有人又改了 EA）⇒ 第 4 条红。
 *
 * ## ★ 缺件不许假绿
 * * `.lst` 不在场 ⇒ 第 1/2/4/5 条 `t.skip(...)` 并**写明缺什么、缺了它退化成什么**
 *   （缺了它连"这个 EA 是不是行首真指令"都判不了 ⇒ 整个口径失效）；
 * * `.lst` 在、**参考件**（`raw-parts/AGE.EXE__dumped.sectfix.EXE`）不在 ⇒ 第 2 条 skip，
 *   改跑**降级分支**（第 3 条，用例名里明说"降级"）：只判 A/B/S 分类与 allowlist 的 EA 成员关系，
 *   **不判** `len`/`sha256` 摘要一致性；
 * * 第 6/7/8/9 条是**纯合成夹具**（不吃任何资产）⇒ 无论资产在不在场都跑。
 *
 * 运行：`pnpm test:assets`（`@env assets`）
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';
import { DEFAULT_MANIFEST, loadManifest } from '../lib/manifest.mjs';
import { DEFAULT_LEDGER_DIR, hashAnchorAt, project, readRecords } from '../lib/ledger.mjs';
import { pickListing } from '../lib/disasm.mjs';
import {
  AUDIT_ALLOWLIST,
  AUDIT_C_EXCEPTIONS,
  AUDIT_SITE_EXCEPTIONS,
  auditEntries,
  classifyEa,
  isRealInsnLine,
  parseListingText,
} from '../lib/anchor-lst.mjs';

const LEDGER_DIR = path.join(REPO_ROOT, DEFAULT_LEDGER_DIR);
const FILES_DIR = path.join(REPO_ROOT, 'corpus', 'disasm', 'files');
const REF_BIN_REL = 'raw-parts/AGE.EXE__dumped.sectfix.EXE';

/**
 * 载入守卫要的三样东西：`.lst` 索引 · 在场记录（带**实质 claim**）· 参考件摘要函数。
 * ★ 缺件**不许**在这里悄悄兜底成"没问题"：`reason` 非 null ⇒ 调用方 `t.skip(reason)`。
 */
function loadContext() {
  let lst = null;
  try {
    lst = pickListing(FILES_DIR);
  } catch {
    lst = null;
  }
  if (!lst || !fs.existsSync(lst)) {
    return {
      reason:
        `缺 \`corpus/disasm/files/*.lst\`（${FILES_DIR}）—— 它是"这个 EA 是不是行首真指令"的**唯一真源**；` +
        '缺了它，站点口径整条都判不了（分类器会把所有锚判成 D，这里**不猜**，直接 skip）',
    };
  }
  const idx = parseListingText(fs.readFileSync(lst, 'utf8'));

  const { records } = readRecords(LEDGER_DIR);
  const byId = new Map(records.map((r) => [r.id, r]));
  let referenceRoot = null;
  try {
    // ★ `DEFAULT_MANIFEST` 本身就是绝对路径 ⇒ 必须**不带参数**调（带了会拼成 `<repo>\<repo>\corpus\assets.json`）
    referenceRoot = loadManifest().roots?.oldRepo ?? null;
  } catch {
    referenceRoot = null;
  }
  const proj = project(records, { repoRoot: REPO_ROOT, referenceRoot, ledgerDir: LEDGER_DIR });
  const entries = proj.entries.map((e) => {
    // ★ 实质 claim = 走 `replaces` 链到**最早**那条（订正版的 claim 尾巴常只有一句 boilerplate）
    let cur = e;
    const seen = new Set();
    while (cur?.replaces && byId.has(cur.replaces) && !seen.has(cur.id)) {
      seen.add(cur.id);
      cur = byId.get(cur.replaces);
    }
    return { ...e, substantiveClaim: cur?.claim ?? e.claim };
  });

  const bufCache = new Map();
  const binPath = referenceRoot ? path.join(referenceRoot, REF_BIN_REL) : null;
  const binPresent = Boolean(binPath && fs.existsSync(binPath));
  /** 参考件在场 ⇒ 现算摘要；不在场 ⇒ null（**降级**，不假装验过） */
  const digestOf = (rel, ea, len) => {
    if (!binPresent || rel !== REF_BIN_REL) return null;
    if (!bufCache.has(rel)) bufCache.set(rel, fs.readFileSync(binPath));
    const r = hashAnchorAt(bufCache.get(rel), ea, len);
    return r.ok ? r.sha256 : null;
  };

  return { idx, lst, entries, binPresent, binPath, digestOf, referenceRoot, reason: null };
}

const ctx = loadContext();
const needListing = (t) => {
  if (ctx.reason) {
    t.skip(ctx.reason);
    return false;
  }
  return true;
};

// ─────────────────────────────────────────────────────────── ① 口径本身（真语料）

test('★ 站点口径：在场 reference bin 锚必须落 `.lst` 行首真指令（A）或折叠/孤立站点（S）；落指令中间的必须在 allowlist 里', (t) => {
  if (!needListing(t)) return;
  const r = auditEntries(ctx.entries, { idx: ctx.idx, digestOf: ctx.digestOf });
  // ★ 前提断言：语料真的在场且被解析出来了 —— 否则下面全是恒真
  assert.ok(r.counts.anchors > 300, `在场 reference bin 锚只有 ${r.counts.anchors} 个（应该 300+）⇒ 语料/台账没被读到，这条守卫没在守任何东西`);
  assert.ok(ctx.idx.realStarts.length > 10000, `语料只解出 ${ctx.idx.realStarts.length} 条真指令 ⇒ 分类器没生效（\`.lst\` 没读到 / 判据被改瞎）`);
  assert.equal(classifyEa(ctx.idx, 0x48f000).cls, 'A', '★ 同一 EA 多行的合并：`sub_48F000` 那个 EA 上叠着 proc near / 栈变量定义 / `push ebp` ⇒ 必须判成 A（行首真指令）');
  assert.equal(classifyEa(ctx.idx, 0x4f0f4f).cls, 'S', '★ 折叠站点（`; [00000106 BYTES: COLLAPSED FUNCTION …]`）必须判成 S（真站点），不是"落指令中间"');

  const bad = r.problems.filter((p) => ['mid-insn', 'not-a-site', 'not-in-corpus'].includes(p.code));
  assert.deepEqual(
    bad.map((p) => `${p.code} ${p.id} 0x${p.ea.toString(16)}`),
    [],
    `★ 站点口径被破坏（${bad.length} 条）—— 落指令中间必须在 claim 里说清第几个字节并登记进 AUDIT_ALLOWLIST，否则吸附到行首：\n` +
      bad.map((p) => `    ${p.why}`).join('\n'),
  );
});

test('★ allowlist 逐条复核：EA 仍在指令中间 · `len` = `nextRealInsn(ea) − ea` · `sha256` = 该处字节摘要（需参考件）', (t) => {
  if (!needListing(t)) return;
  if (!ctx.binPresent) {
    t.skip(
      `缺只读参考件 \`${REF_BIN_REL}\`（清单 roots.oldRepo = ${ctx.referenceRoot ?? '(未登记)'}）—— ` +
        '缺了它 `len`/`sha256` 无从现算 ⇒ 本用例整体不判（**降级分支见下一条**），不是"摘要都对"',
    );
    return;
  }
  const r = auditEntries(ctx.entries, { idx: ctx.idx, digestOf: ctx.digestOf });
  const bad = r.problems.filter((p) => ['allow-stale', 'allow-len', 'allow-sha', 'allow-unused'].includes(p.code));
  assert.deepEqual(
    bad.map((p) => `${p.code} 0x${p.ea.toString(16)}`),
    [],
    `★ allowlist 与现算不一致（${bad.length} 条）—— 白名单里每一列（EA/len/sha256）都必须与现算逐字相同：\n` +
      bad.map((p) => `    ${p.why}`).join('\n'),
  );
  assert.equal(AUDIT_ALLOWLIST.length, r.allowlistChecked.length, 'allowlist 每一条都必须有一条在场锚与它对应（不许留尸体）');
});

test('★ 降级分支（参考件不在场）：只判 A/B/S 分类与 allowlist 的 EA 成员关系，**不判** len/sha256 摘要', (t) => {
  if (!needListing(t)) return;
  if (ctx.binPresent) {
    t.skip('参考件在场 ⇒ 走**严格分支**（上一条用例）；本用例只在缺参考件时才判定');
    return;
  }
  const r = auditEntries(ctx.entries, { idx: ctx.idx, digestOf: null });
  const bad = r.problems.filter((p) => ['mid-insn', 'not-a-site', 'not-in-corpus', 'allow-stale'].includes(p.code));
  assert.deepEqual(bad.map((p) => p.code), [], `★ 降级分支：分类与 allowlist 的 EA 成员关系仍必须成立：\n${bad.map((p) => `    ${p.why}`).join('\n')}`);
});

// ─────────────────────────────────────────────────────────── ② 25 个"待裁决"的裁决可复核

test('★ 25 个"待裁决"锚的裁决可复核：仍在此处 · 仍是站点 · 订正过的那 15 条旧 EA 当时确实落指令中间', (t) => {
  if (!needListing(t)) return;
  assert.equal(AUDIT_SITE_EXCEPTIONS.length, 25, '口径应用的样本 = 普查点名的 25 个锚（需求单 REQ-01M4N3QC4SYG4C36QDBQCFRMSW）');
  const live = new Map(ctx.entries.filter((e) => e.effective !== 'retracted').map((e) => [e.id, e]));
  const problems = [];
  let fixed = 0;
  for (const x of AUDIT_SITE_EXCEPTIONS) {
    const e = live.get(x.id);
    if (!e) { problems.push(`${x.id} 不在场（裁决必须挂在场锚上）`); continue; }
    const a = (e.anchor ?? []).find((y) => y.type === 'bin' && y.ea === x.ea);
    if (!a) { problems.push(`${x.id} 上找不到 EA 0x${x.ea.toString(16)} 的 bin 锚（裁决与台账不一致）`); continue; }
    if (typeof a.len !== 'number' || typeof a.sha256 !== 'string') problems.push(`${x.id}@0x${x.ea.toString(16)} 缺 len/sha256（口径要求例外也要带摘要）`);
    const c = classifyEa(ctx.idx, x.ea);
    if (c.cls !== 'A' && c.cls !== 'S') problems.push(`${x.id}@0x${x.ea.toString(16)} 现在分类 ${c.cls}（裁决时是站点）`);
    if (x.was !== x.ea) {
      fixed += 1;
      const o = classifyEa(ctx.idx, x.was);
      if (o.cls !== 'B' || o.prev !== x.ea) {
        problems.push(`${x.id} 的旧 EA 0x${x.was.toString(16)} 现在分类 ${o.cls}（应仍是"落 0x${x.ea.toString(16)} 内"）—— 订正的理由对不上了`);
      }
    }
    if (!x.why || x.why.length < 20 || /不确定|不知道/.test(x.why)) problems.push(`${x.id}@0x${x.ea.toString(16)} 的裁决理由不具体`);
  }
  assert.deepEqual(problems, [], problems.join('\n'));
  assert.equal(fixed, 15, '口径第 3 步（吸附到行首）的样本 = 15 条（另 10 条本来就是站点起点 ⇒ 不需要订正）');
});

// ─────────────────────────────────────────────────────────── ③ C 类：0x1000 级错位

test('★ C 类：claim 点名地址与锚 EA **恰好差一页**且 EA 不在那个函数体内 ⇒ 红（已裁决的两条例外除外）', (t) => {
  if (!needListing(t)) return;
  const r = auditEntries(ctx.entries, { idx: ctx.idx, digestOf: ctx.digestOf });
  const found = r.cCandidates.map((c) => `${c.id}@0x${c.ea.toString(16)}~0x${c.cited.toString(16)}`).sort();
  const known = AUDIT_C_EXCEPTIONS.map((c) => `${c.id}@0x${c.ea.toString(16)}~0x${c.cited.toString(16)}`).sort();
  assert.deepEqual(
    found,
    known,
    `★ C 类候选与已裁决例外不一致 —— 新出现的必须逐条裁决（改 AUDIT_C_EXCEPTIONS），消失的要把例外删掉：\n    实测 ${JSON.stringify(found)}\n    例外 ${JSON.stringify(known)}`,
  );
  assert.ok(known.length > 0, '本机实测有 2 条（差恰好一页）⇒ 例外表不许为空（空了说明判据被改瞎了）');
});

// ─────────────────────────────────────────────────────────── ④ 合成夹具（不吃资产，钉判据形状）

test('★ 合成夹具：B 类不在 allowlist ⇒ 红；allowlist 的 EA 挪一字节 / len 不符 ⇒ 红；A 类 ⇒ 不红（判据双向）', () => {
  // 合成 listing：0x1000 处 `push ebp`(1B) / 0x1001 `mov ebp,esp`(2B) / 0x1003 `sub esp,10h`(3B)
  const idx = parseListingText(
    [
      '.text:00001000 sub_1000 proc near',
      '.text:00001000 var_4= dword ptr -4',
      '.text:00001000 push    ebp',
      '.text:00001001 mov     ebp, esp',
      '.text:00001003 sub     esp, 10h',
      '.text:00001006 ; [00000020 BYTES: COLLAPSED FUNCTION ___report_gsfailure]',
      '.text:00001026 retn',
    ].join('\n'),
  );
  const ent = (ea, claim = '锚 = EA') => [{ id: 'SYN-1', effective: 'accepted', claim, anchor: [{ type: 'bin', repo: 'reference', path: 'x.exe', ea }] }];
  const code = (entries, allowlist) => auditEntries(entries, { idx, allowlist }).problems.map((p) => p.code);

  // A 类（行首真指令）⇒ 默认合法，一条都不许有
  assert.deepEqual(code(ent(0x1000), []), [], '行首真指令的锚必须默认合法（口径的"默认"这一半）');
  // B 类（0x1001 是 `mov ebp,esp` 的第 0 字节？—— 0x1002 才是第 1 字节）⇒ 必须红
  assert.deepEqual(code(ent(0x1002), []), ['mid-insn'], '★ 落指令中间且不在 allowlist ⇒ 必须红（口径的另一半）');
  // allowlist 把它收进来（EA/len/sha 全对）⇒ 不再红
  // ★ 注意 `len` 的口径是 `nextRealInsn(ea) − ea`（落到指令中间时**不是**整条指令的长度，而是从 EA 起的剩余字节）
  const ok = [{ id: 'SYN-1', ea: 0x1002, len: 1, sha256: 'x' }];
  assert.deepEqual(code(ent(0x1002), ok), [], 'allowlist 收进来的 B 类锚不许再红');
  // ★ allowlist 的 EA 挪一字节（0x1003 是另一条指令的行首 ⇒ 白名单条目失效）⇒ 必须红
  assert.ok(code(ent(0x1002), [{ id: 'SYN-1', ea: 0x1003, sha256: 'x' }]).includes('allow-unused'), '★ 白名单里那条挪了 EA ⇒ 既"不再对应任何锚"又"新 EA 不是指令中间"，必须红');
  // ★ allowlist 的 len 与现算不符 ⇒ 必须红
  assert.deepEqual(code(ent(0x1002), [{ ...ok[0], len: 99 }]), ['allow-len'], '★ allowlist 的 len 挪一格 ⇒ 必须红（否则"同一条指令"无从保证）');
  // ★ allowlist 的 sha256 与现算不符 ⇒ 必须红（摘要 = "这条观察没被换掉"的唯一证据）
  const codeSha = (entries, allowlist) => auditEntries(entries, { idx, allowlist, digestOf: () => 'f'.repeat(64) }).problems.map((p) => p.code);
  assert.deepEqual(codeSha(ent(0x1002), [{ ...ok[0], sha256: 'a'.repeat(64) }]), ['allow-sha'], '★ allowlist 的 sha256 与现算不符 ⇒ 必须红（EA 挪过 / 字节被换过都得抓）');
  assert.deepEqual(codeSha(ent(0x1002), [{ ...ok[0], sha256: 'f'.repeat(64) }]), [], '摘要对得上 ⇒ 不红（判据双向）');
  // ★ allowlist 里留一条没有对应锚的"尸体" ⇒ 必须红
  assert.ok(code(ent(0x1000), [{ id: 'SYN-9', ea: 0x1002, len: 1, sha256: 'x' }]).includes('allow-unused'), '★ 白名单不许留尸体（没有对应在场锚的条目 ⇒ 红）');
  // 折叠站点（只有 `;` 注释行的真站点）⇒ 算站点，不许红
  assert.deepEqual(code(ent(0x1006), []), [], '折叠站点是真站点 ⇒ 不许红（普查踩出来的坑）');
});

test('★ 空 / 缺语料不许假绿：空 listing ⇒ 每个锚都判 `not-in-corpus`（红），不是"零问题"', () => {
  const empty = parseListingText('');
  const r = auditEntries([{ id: 'SYN-2', effective: 'accepted', claim: 'x', anchor: [{ type: 'bin', repo: 'reference', path: 'x.exe', ea: 0x401000 }] }], { idx: empty, allowlist: [] });
  assert.equal(r.counts.anchors, 1);
  assert.deepEqual(r.problems.map((p) => p.code), ['not-in-corpus'], '★ 语料空 ⇒ 判不了必须**红**：恒绿的"零问题"会让"这台机器上没跑"长得像"跑了且绿"');
});

test('★ 假指令行判据本身：`proc near` / `endp` / `名字= dword ptr 8` / 纯标签 / 指示符 都不算指令；普通指令行算', () => {
  for (const fake of ['sub_48F000 proc near', 'sub_48F000 endp', 'var_4= dword ptr -4', 'Buffer= byte ptr -10h', 'arg_0= dword ptr  8', 'loc_48F02F:', '.686p', 'assume cs:_text', '_text segment para public \'CODE\' use32', 'align 10h', 'db 90h']) {
    assert.equal(isRealInsnLine(fake), false, `★ 假指令行没被排除：「${fake}」—— 把它算成"行首真指令"会让"锚落在指令中间"整类消失`);
  }
  for (const real of ['push    ebp', 'mov     [esi+0A3DE4h], ebx', 'call    sub_48EE60', 'jnz     short loc_407FC5']) {
    assert.equal(isRealInsnLine(real), true, `真指令行被误排除：「${real}」`);
  }
  assert.equal(isRealInsnLine('; [00000106 BYTES: COLLAPSED FUNCTION ___report_gsfailure]'), false, '注释行不是指令（它靠"该 EA 有 listing 行"走 S 类，不是靠这里）');
});
