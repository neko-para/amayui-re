#!/usr/bin/env node
/** @env assets @kind gate @suite toolchain @why `collapsed` 的布尔判据只认折叠的一种说法 ⇒ `COLLAPSED CHUNK OF FUNCTION` 站点不被标 collapsed，被当成"函数体只有一行注释"的普通站点，依赖它的判定在这一类站点上绿而错 */
/**
 * tools/test/anchor-lst-collapsed.assets.test.mjs —— ★ **机械遍历整份语料**的 `collapsed` 守卫
 *
 * ## 为什么是"遍历每一条注释行"而不是"挑几个样例"
 * 需求单 `REQ-01M71BT8GMBMMYQNA8FGWWJB` 的缺陷形态是**词表少了一种说法**：
 * `COLLAPSED_RE` 原先 `= /COLLAPSED FUNCTION|START OF FUNCTION CHUNK/i`，漏掉 IDA 的第二种说法
 * `; [NNNNNNNN BYTES: COLLAPSED CHUNK OF FUNCTION <名>. …]`。实测本语料 **122 行**属这一类
 * ⇒ 那些 EA 的 `collapsed` 是 `false`，被归进"有 listing 行但无真指令"的普通站点。
 * 样例法对这种缺陷**无效**（样例恰好用 `COLLAPSED FUNCTION` 就永远绿）⇒ 判据必须是
 * "**含 `COLLAPSED` 的注释行，100% 被 `COLLAPSED_RE` 命中**"，逐行机械算。
 *
 * ## 三条断言（覆盖两个判据，且各自机械遍历）
 * 1. `COLLAPSED_RE`（`anchor-lst.mjs`，**布尔**站点判据）覆盖每一条含 `COLLAPSED` 的注释行；
 * 2. 同一条判据也覆盖第三形态 `; START OF FUNCTION CHUNK FOR …`（它不含 `COLLAPSED`，是**单独**分支，
 *    漏掉它同样是把折叠块当函数体）—— 所以它也要逐行算；
 * 3. `disasm.mjs` 的 `parseCollapsedLine`（**读长度**的判据）必须覆盖**同一群体**，且读出的字节数
 *    等于这一行自己写的 `[NNNNNNNN BYTES:`。⇒ 那个函数**不需要**另立一条守卫：它跑在同一批行上、
 *    有独立的第二个断言（长度），红了会指名是哪一行。
 * 4. 端到端：`parseListingText` 给这些 EA 打的 `collapsed` 必须是 `true`（缺陷的**可观察后果**就在这里）。
 *
 * ## 缺件不许假绿
 * `corpus/disasm/files/*.lst` 是解压产物（`.gitignore` 的 `/corpus/disasm/files/`）⇒ 不在场时
 * 每条用例 `t.skip(...)` 并写明"缺了它这条守卫退化成什么"（AGENTS.md §5 测试卫生）。
 *
 * ★ 不断言"当前有多少条"（那是语料取值，不是不变量，见 AGENTS.md §10）：只断言"一条都没漏"，
 *   外加"这一类**存在**"（存在性前提，否则守卫恒绿却什么也没守）。
 *
 * 运行：`pnpm test:assets`（`@env assets`）；直接跑：
 *   `node --test --test-isolation=none tools/test/anchor-lst-collapsed.assets.test.mjs`
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';
import { parseCollapsedLine, pickListing } from '../lib/disasm.mjs';
import { COLLAPSED_RE, LST_LOC_RE, isCommentLine, parseListingText } from '../lib/anchor-lst.mjs';

const FILES_DIR = path.join(REPO_ROOT, 'corpus', 'disasm', 'files');

/** 本单要抓的那一类（`COLLAPSED CHUNK OF FUNCTION`）—— 用它做"存在性前提" */
const CHUNK_FORM_RE = /COLLAPSED\s+CHUNK\s+OF\s+FUNCTION/i;
/** 折叠注释里自带的行内字节数声明 */
const DECLARED_BYTES_RE = /\[([0-9A-Fa-f]{8})\s+BYTES:/;
const START_CHUNK_RE = /START\s+OF\s+FUNCTION\s+CHUNK/i;

function loadListing() {
  try {
    const p = pickListing(FILES_DIR);
    return p && fs.existsSync(p) ? p : null;
  } catch {
    return null;
  }
}

const LST = loadListing();
const NEED =
  `缺 \`corpus/disasm/files/*.lst\`（${FILES_DIR}）—— 它是 \`collapsed\` 判据的唯一真源；` +
  '缺了它"含 COLLAPSED 的注释行有没有被命中"这一句无从算起（**不是"全都命中了"**）';

let cache = null;
/** 全语料扫一遍：返回折叠相关的行与"没命中"的那些。只读文件一次（18 MB / 53 万行）。 */
function scan() {
  if (cache) return cache;
  const lines = fs.readFileSync(LST, 'utf8').split(/\r?\n/);
  const out = {
    listingLines: 0,
    commentLines: 0,
    collapsed: [], // 含 COLLAPSED 的注释行（原文）
    startChunk: [], // 含 START OF FUNCTION CHUNK 的注释行（原文）
    missCollapsedRe: [], // 含 COLLAPSED 却没被 COLLAPSED_RE 命中
    missChunkFormRe: [], // `COLLAPSED CHUNK OF FUNCTION` 却没被 COLLAPSED_RE 命中（本单的缺陷形态）
    missStartChunk: [], // 含 START OF FUNCTION CHUNK 却没被 COLLAPSED_RE 命中
    missParseCollapsed: [], // 含 COLLAPSED 却没被 parseCollapsedLine 认出来
    missDeclaredBytes: [], // parseCollapsedLine 读出的字节数 ≠ 该行自己的 `[NNNN BYTES:`
  };
  for (const raw of lines) {
    const m = LST_LOC_RE.exec(raw);
    if (!m) continue;
    out.listingLines += 1;
    const body = m[3].trim();
    if (!isCommentLine(body)) continue;
    out.commentLines += 1;
    const hasCollapsed = /COLLAPSED/i.test(body);
    const hasStartChunk = START_CHUNK_RE.test(body);
    if (!hasCollapsed && !hasStartChunk) continue;
    if (hasCollapsed) out.collapsed.push(raw.trim());
    if (hasStartChunk) out.startChunk.push(raw.trim());
    if (hasCollapsed && !COLLAPSED_RE.test(body)) {
      out.missCollapsedRe.push(raw.trim());
      if (CHUNK_FORM_RE.test(body)) out.missChunkFormRe.push(raw.trim());
    }
    if (hasStartChunk && !COLLAPSED_RE.test(body)) out.missStartChunk.push(raw.trim());
    const p = parseCollapsedLine(raw);
    if (hasCollapsed && p === null) out.missParseCollapsed.push(raw.trim());
    if (p !== null) {
      const d = DECLARED_BYTES_RE.exec(body);
      const want = d ? Number.parseInt(d[1], 16) : null;
      if (p.declaredBytes !== want) out.missDeclaredBytes.push(`${raw.trim()} ⇒ parseCollapsedLine.declaredBytes=${p.declaredBytes}，行内声明=${want}`);
    }
  }
  cache = out;
  return out;
}

const sample = (arr, n = 5) => arr.slice(0, n).map((x) => `    ${x}`).join('\n') +
  (arr.length > n ? `\n    …另有 ${arr.length - n} 条` : '');

// ─────────────────────────────────────────── ① 布尔判据：100% 覆盖
test('★ 机械遍历：每条含 `COLLAPSED` 的注释行都必须被 `COLLAPSED_RE` 命中（100%，不是抽样）', (t) => {
  if (!LST) {
    t.skip(NEED);
    return;
  }
  const s = scan();
  // ★ 存在性前提：这一类必须在语料里（否则这条守卫恒绿却什么也没守）
  assert.ok(s.collapsed.length > 0, `语料里一条含 COLLAPSED 的注释行都没有（扫了 ${s.listingLines} 行 listing）⇒ 守卫没在守任何东西，先确认语料是不是同一份导出`);
  assert.ok(
    s.collapsed.some((l) => CHUNK_FORM_RE.test(l)),
    '★ 语料里 `COLLAPSED CHUNK OF FUNCTION` 形态一条都没有 ⇒ 本单要抓的那一类不在语料里，判据会恒绿（先确认语料是不是同一份导出，而不是把这条删掉）',
  );
  assert.equal(
    s.missCollapsedRe.length,
    0,
    `★ ${s.missCollapsedRe.length}/${s.collapsed.length} 条含 COLLAPSED 的注释行**没被** \`COLLAPSED_RE\` 命中 ` +
      `⇒ 这些 EA 的 collapsed 是 false，被当成"函数体只有一行注释"的普通站点（其中 \`COLLAPSED CHUNK OF FUNCTION\` ${s.missChunkFormRe.length} 条）：\n` +
      sample(s.missCollapsedRe, 8),
  );
  t.diagnostic(
    `遍历 ${s.listingLines} 行 listing / ${s.commentLines} 条注释行：含 COLLAPSED ${s.collapsed.length} 条，` +
      `其中 \`COLLAPSED CHUNK OF FUNCTION\` ${s.collapsed.filter((l) => CHUNK_FORM_RE.test(l)).length} 条 —— 命中率 100%`,
  );
});

test('★ 第三形态同样逐行算：每条 `START OF FUNCTION CHUNK` 注释行也必须被 `COLLAPSED_RE` 命中（它不含 COLLAPSED，是单独分支）', (t) => {
  if (!LST) {
    t.skip(NEED);
    return;
  }
  const s = scan();
  assert.ok(s.startChunk.length > 0, '语料里一条 `START OF FUNCTION CHUNK` 注释行都没有 ⇒ 这条守卫恒绿（先确认语料）');
  assert.equal(
    s.missStartChunk.length,
    0,
    `★ ${s.missStartChunk.length}/${s.startChunk.length} 条 \`START OF FUNCTION CHUNK\` 注释行没被 \`COLLAPSED_RE\` 命中 —— 它们也是折叠块的位置（不是函数体）：\n` +
      sample(s.missStartChunk, 8),
  );
  t.diagnostic(`\`START OF FUNCTION CHUNK\` 注释行 ${s.startChunk.length} 条 —— 命中率 100%`);
});

// ─────────────────────────────────────────── ② 长度判据：覆盖同一群体
test('★ `parseCollapsedLine` 覆盖同一群体，且读出的字节数 = 这一行自己写的 `[NNNNNNNN BYTES:`（不需要另立守卫）', (t) => {
  if (!LST) {
    t.skip(NEED);
    return;
  }
  const s = scan();
  assert.equal(
    s.missParseCollapsed.length,
    0,
    `★ ${s.missParseCollapsed.length}/${s.collapsed.length} 条含 COLLAPSED 的注释行**没被** \`parseCollapsedLine\` 认出来 ` +
      `⇒ "折叠区间有多长"在这些站点上取不到（与 \`COLLAPSED_RE\` 是两套判据，不许一套认了另一套没认）：\n` +
      sample(s.missParseCollapsed, 8),
  );
  assert.equal(
    s.missDeclaredBytes.length,
    0,
    `★ \`parseCollapsedLine\` 读出的 \`declaredBytes\` 与该行自己声明的字节数不一致（${s.missDeclaredBytes.length} 条）—— ` +
      `"取到了那一段"这句话就不成立：\n${sample(s.missDeclaredBytes, 8)}`,
  );
  // 前提：这一批里既有带长度的（COLLAPSED），也有不带长度的（START OF FUNCTION CHUNK ⇒ null）
  const withDecl = s.collapsed.filter((l) => DECLARED_BYTES_RE.test(l));
  assert.ok(withDecl.length > 0, '语料里没有一条带 `[NNNN BYTES:` 的折叠行 ⇒ 长度那一半没在测');
  const noDecl = s.startChunk.filter((l) => !DECLARED_BYTES_RE.test(l));
  assert.ok(noDecl.length > 0, '语料里没有一条"无长度声明"的 START OF FUNCTION CHUNK ⇒ `declaredBytes: null` 那一半没在测');
  t.diagnostic(
    `parseCollapsedLine：${withDecl.length} 条读到行内声明的字节数，${noDecl.length} 条 \`START OF FUNCTION CHUNK\` 无声明（null）`,
  );
});

// ─────────────────────────────────────────── ③ 端到端：可观察后果
test('★ 端到端：`parseListingText` 给每个"有折叠注释"的 EA 打的 `collapsed` 必须是 true（含 CHUNK 形态的站点）', (t) => {
  if (!LST) {
    t.skip(NEED);
    return;
  }
  const text = fs.readFileSync(LST, 'utf8');
  const idx = parseListingText(text);
  const bad = [];
  let chunkEas = 0;
  let collapsedEas = 0;
  for (const [ea, e] of idx.byEa) {
    const hasCollapsedBody = e.bodies.some((b) => isCommentLine(b) && /COLLAPSED/i.test(b));
    const hasChunkBody = e.bodies.some((b) => isCommentLine(b) && CHUNK_FORM_RE.test(b));
    if (!hasCollapsedBody) continue;
    collapsedEas += 1;
    if (hasChunkBody) chunkEas += 1;
    if (e.collapsed !== true) {
      bad.push(`0x${ea.toString(16)} collapsed=${e.collapsed} bodies[0]=${e.bodies[0]?.slice(0, 90)}`);
    }
  }
  assert.ok(collapsedEas > 0, `没有任何 EA 带折叠注释（扫了 ${idx.byEa.size} 个 EA）⇒ 这条守卫恒绿，先确认语料`);
  assert.ok(chunkEas > 0, '★ 没有 EA 带 `COLLAPSED CHUNK OF FUNCTION` 注释 ⇒ 本单的缺陷形态不在语料里，这条用例会恒绿');
  assert.equal(
    bad.length,
    0,
    `★ ${bad.length}/${collapsedEas} 个折叠站点 EA 的 \`collapsed\` 不是 true ⇒ 它们会被当成"函数体只有一行注释"（本单的缺陷正是如此）：\n` +
      bad.slice(0, 8).map((x) => `    ${x}`).join('\n') +
      (bad.length > 8 ? `\n    …另有 ${bad.length - 8} 个` : ''),
  );
  t.diagnostic(`折叠站点 EA ${collapsedEas} 个（其中 CHUNK 形态 ${chunkEas} 个）—— collapsed 全为 true`);
});
