/** @env pure @kind gate @why 复审修过的关键实现锚被改掉（行为守卫在 CTest，这里给变异测试一个可直接跑的入口） */
/**
 * tools/test/emulator-cxx-review-regressions.test.mjs —— 复审修复的**源码回归锚**
 *
 * 行为断言在 `core_selftest`（`emulator-core-msg-block` / `-scene-relocate` / `-scene-records` /
 * `-misc-cells` / `-misc-ops` / `-adv-pacing`）。`tools/mutate-check.mjs` 只会跑 `node --test <文件>`，
 * 跑不了 CTest ⇒ 这里对**每个修过的关键常量**留一个源码锚，供变异测试证明“改坏它会红”。
 *
 * ⛔ 这不是语义判据；语义由 CTest 判。它只防“修复被静默回退、而 CTest 恰好没有编进当前构建”。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';

const CASES = [
  {
    file: 'apps/emulator-cxx/src/core/vm_ops_text.cpp',
    snippet: 'msg_arm_ |= 0x10000;',
    count: 2,
    why: '0x6e / 0x196 成功追加必须置 arm bit16（否则 0x305 整段是死代码）',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm_ops_scene.cpp',
    snippet: 'std::swap(scene_records_[key_a], scene_records_[key_b]);',
    count: 1,
    why: '0x214 两个键都有记录时必须交换（不是单向覆盖）',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm_ops_scene.cpp',
    snippet: 'msg_wait_c_ = value;',
    count: 1,
    why: '0x238 写的是消息等待三格的 C（与 0x243 / 0x305 同一组）',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm_ops_texture.cpp',
    snippet: 'item.set21e_int0 = g1;',
    count: 1,
    why: '0x220 的 G1 必须同步到 DrawItem（消费者读 DrawItem，不读 SceneRecord）',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm_ops_text.cpp',
    snippet: 'if (!rec.key_base_set) return;',
    count: 1,
    why: '脚本显式写基数 0 时不得被宿主兜底覆盖',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm_ops_texture.cpp',
    snippet: 'const auto mask = read_int(ops[1]);',
    count: 1,
    why: '0x258 的读序是 2 → 1（先位掩码、再编号）',
  },
  {
    file: 'apps/emulator-cxx/src/core/vm_ops_engine.cpp',
    snippet: 'config_.set_int("message:ReadTextSkip", read_int(ops[0]));',
    count: 1,
    why: '0x1ca 与 0x1cb 必须读写同一张命名键表',
  },
];

test('★ 复审修过的关键源码锚必须还在（变异测试的入口）', () => {
  const problems = [];
  for (const c of CASES) {
    const text = fs.readFileSync(path.join(REPO_ROOT, c.file), 'utf8');
    const hits = text.split(c.snippet).length - 1;
    if (hits < c.count) {
      problems.push(`${c.file}: 期望至少 ${c.count} 处 \`${c.snippet}\`（实得 ${hits}）—— ${c.why}`);
    }
  }
  assert.deepEqual(problems, [], problems.join('\n'));
});
