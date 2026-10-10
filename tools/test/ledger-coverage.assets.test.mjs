/** @env assets @kind contract @why 库候选启发式又把引擎原语摘出前沿榜与分桶了（`sub_42B4B0`/`sub_42BA00` 这类假阳性会让"按榜单滚动取证"的人误判它已被登记） */
/**
 * tools/test/ledger-coverage.assets.test.mjs —— **库候选收窄规则**在**真语料**上的判据
 *
 * ## 为什么它必须在真语料上跑（合成语料那条不够）
 * `tools/test/ledger-coverage.test.mjs` 用合成 `.lst` 钉住了收窄规则的**形状**（有 `[ecx+5D880h]`
 * ⇒ 取消；只有 `[ecx+10h]` 或栈基址 ⇒ 保留）。但在真语料上还有一件合成语料**测不到**的事：
 * **真正被误判的那两个函数是不是回到榜上了** —— 那是这条收窄规则的**存在理由**。
 *
 * 实测（2026-10，`sub_42B4B0` = 操作数写值原语，131 调用方 / 223 处调用）：
 * 它体内的 `0x42B805 push offset aInvalidVectorT` / `0x42B80A call ?_Xout_of_range@std@@YAXPBD@Z`
 * 只是**内联**了 `vector<int>::operator[]` 的越界抛出口，却让它被 `calls-stl-internal` 判成库候选
 * ⇒ 整条从两张前沿榜与分桶里消失，而它的 float 兄弟 `sub_42BA00` 同病。
 *
 * ★ 这一条**不**断言"名单就是这 18 个"（那是数据的当前取值，`pnpm tools ledger coverage` 是哨兵）；
 *   它断言的是**两件会静默出错的事**：① 那两个引擎原语必须回到"引擎未登记"一侧；
 *   ② 收窄不许滥杀 —— 被取消的每一条都必须**带得出**那个 ≥ `ENGINE_SCALE_DISP` 的位移，
 *   而真库内部实现（`sub_40C210` 等）必须**留在**候选里。
 *
 * 运行：`pnpm test:assets`（要语料；语料不在场 ⇒ 如实 skip，不假装绿）
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';
import { coverage, ENGINE_SCALE_DISP, libraryLikely, libraryLikelyCancelled } from '../lib/coverage.mjs';
import { functionInventory } from '../lib/disasm.mjs';

const FILES_DIR = path.join(REPO_ROOT, 'corpus', 'disasm', 'files');
const LST = fs.existsSync(FILES_DIR)
  ? (fs.readdirSync(FILES_DIR).filter((f) => f.endsWith('.lst')).sort().map((f) => path.join(FILES_DIR, f))[0] ?? null)
  : null;
const skip = LST ? false : '语料未解压（先 `pnpm tools disasm build`）';

/** 台账留空：本守卫问的是"启发式把谁摘出去了"，与有没有登记无关（空台账照跑） */
const run = () => coverage([], { lstFile: LST, top: 0 });

test('★ `sub_42B4B0` / `sub_42BA00`（引擎写值原语）必须**不被**判成库候选，且回到引擎宇宙', { skip }, () => {
  const r = run();
  const lib = new Set(r.honest.libraryCandidates.map((x) => x.sym));
  const cancelled = r.honest.libraryCandidatesCancelled;
  const cancelledSyms = new Set(cancelled.map((x) => x.sym));

  for (const sym of ['sub_42B4B0', 'sub_42BA00']) {
    assert.ok(!lib.has(sym), `${sym} 是引擎原语（不是 STL 内部实现）—— 它一被摘出去，两张前沿榜与分桶就都看不见它`);
    assert.ok(cancelledSyms.has(sym), `${sym} 必须出现在"被取消"名单里（取消也要看得见，不许静默改口径）`);
    // ★ 它回到引擎宇宙 ⇒ 榜上看得见（台账留空 ⇒ 它必然是"未登记"）
    assert.ok(
      r.frontier.mostUsedUnregistered.some((x) => x.sym === sym),
      `${sym} 回到"全语料用得最多而未登记"那张榜上`,
    );
    assert.ok(
      r.allRows.some((x) => x.sym === sym),
      `${sym} 回到逐函数表（= 进分桶）`,
    );
  }
  // ★ 判据本身也要对得上语料：`sub_42B4B0` 体内那个最大非栈位移就是编解码 key 那一格（`Engine+0x5EC8C`）
  const inv = functionInventory(LST);
  assert.equal(inv.bySym.get('sub_42B4B0').maxNonStackDisp, 0x5ec8c, '写值原语体内最大的非栈位移 = `Engine+0x5EC8C`（编解码 key）');
});

test('★ 收窄不许滥杀：真库内部实现留在候选里，且每条"取消"都带得出那个 ≥ 阈值的位移', { skip }, () => {
  const r = run();
  const lib = new Set(r.honest.libraryCandidates.map((x) => x.sym));
  const cancelled = r.honest.libraryCandidatesCancelled;

  // ① 真 STL 内部实现必须留在候选里（它们的位移全是 SSO / 三指针量级）
  for (const sym of ['sub_40C210', 'sub_40C120']) {
    assert.ok(lib.has(sym), `${sym}（STL 内部实现）不许被收窄规则误杀`);
  }
  // ② 每条"取消"都必须**带得出**那个位移，且 ≥ 阈值 —— 否则规则就成了"看名字删除"
  for (const x of cancelled) {
    assert.ok(
      x.maxNonStackDisp >= ENGINE_SCALE_DISP,
      `${x.sym} 被取消，但现算出来的最大非栈位移 0x${x.maxNonStackDisp.toString(16)} < 0x${ENGINE_SCALE_DISP.toString(16)} ⇒ 取消的理由不成立`,
    );
    assert.ok(x.reasons.includes('calls-stl-internal'), `${x.sym} 的取消理由要写清是被哪条候选理由取消的`);
  }
  // ③ 两个名单互斥、且与 `universe.library` 一致（"摘出去的看得见"这条口径不许破）
  assert.equal(r.universe.library, r.honest.libraryCandidates.length, '`universe.library` 与候选名单必须是同一份');
  for (const sym of cancelled.map((x) => x.sym)) assert.ok(!lib.has(sym), `${sym} 不许同时出现在两个名单里`);
  // ④ 收窄确实**只取消不新增**：`calls-stl-internal` 只可能因为"引擎尺度位移"而消失
  const inv = functionInventory(LST);
  for (const x of cancelled) {
    assert.deepEqual(libraryLikely(inv.bySym.get(x.sym)), [], `${x.sym} 取消后不该还有任何库理由`);
    assert.deepEqual(libraryLikelyCancelled(inv.bySym.get(x.sym)), ['calls-stl-internal'], `${x.sym} 的取消理由就是 calls-stl-internal`);
  }
});
