/**
 * tools/test/cli.test.mjs — **工具层入口**（`tools/cli.mjs`）的两条契约
 *
 * ① **地图不会漏工具**：`tools/*.mjs` 里每个工具都必须自我声明 `DOMAIN` + `OPERATIONS`，
 *    否则它会从 `pnpm tools` 的域地图上消失 —— 那正是"指令多了看不清哪片数据"的来源。
 * ② **转发是薄的**：每个动作的 `argv` 首项必须是自己工具的隐藏 flag（`--xxx`），
 *    域 id / 动作名唯一；派发器必须能真的把动作转发过去（`pnpm tools <域> <动作>` 退出码 0）。
 *
 * 范围：`tools/*.mjs`（不含 `cli.mjs` 自己，也不含 `tools/test/**`）。
 *
 * 运行：`pnpm test`
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT, loadDomains, mapJson } from '../cli.mjs';

const TOOLS_DIR = path.join(REPO_ROOT, 'tools');

const toolModules = () =>
  fs
    .readdirSync(TOOLS_DIR, { withFileTypes: true })
    .filter((e) => e.isFile() && e.name.endsWith('.mjs') && e.name !== 'cli.mjs')
    .map((e) => e.name);

test('★ 每个 tools/*.mjs 都自我声明了 DOMAIN + OPERATIONS（否则它会从域地图上消失）', async () => {
  const problems = [];
  for (const f of toolModules()) {
    const mod = await import(`../${f}`);
    if (!mod.DOMAIN) problems.push(`${f} 缺 DOMAIN（我动哪片数据）`);
    if (!Array.isArray(mod.OPERATIONS) || mod.OPERATIONS.length === 0) problems.push(`${f} 缺 OPERATIONS（我有哪些操作）`);
    if (typeof mod.main !== 'function') problems.push(`${f} 缺 main()（派发器要靠它转发）`);
  }
  assert.deepEqual(problems, [], `工具自我声明不齐：\n  - ${problems.join('\n  - ')}`);
});

test('域 id / 动作名唯一，且每个动作的 argv 首项是自己的隐藏 flag', async () => {
  const domains = await loadDomains();
  const ids = domains.map((d) => d.id);
  assert.deepEqual([...new Set(ids)], ids, `域 id 重复：${ids.join(', ')}`);
  for (const d of domains) {
    const names = d.operations.map((o) => o.name);
    assert.deepEqual([...new Set(names)], names, `域 ${d.id} 的动作名重复：${names.join(', ')}`);
    for (const o of d.operations) {
      assert.ok(Array.isArray(o.argv), `域 ${d.id} 动作 ${o.name} 的 argv 必须是数组`);
      // argv 为空 = "该工具的缺省动作"（例如 old-repo 的 inventory）；非空时首项必须是自己的长 flag
      if (o.argv.length > 0) {
        assert.match(o.argv[0], /^--[a-z-]+$/, `域 ${d.id} 动作 ${o.name} 的 argv[0] 必须是自己工具的长 flag，实际：${o.argv[0]}`);
      }
      assert.equal(typeof o.mutates, 'boolean', `域 ${d.id} 动作 ${o.name} 缺 mutates`);
      assert.ok(typeof o.summary === 'string' && o.summary.length > 0, `域 ${d.id} 动作 ${o.name} 缺 summary`);
    }
  }
});

test('地图覆盖全部工具，且 JSON 版形状稳定', async () => {
  const domains = await loadDomains();
  assert.equal(domains.length, toolModules().length, '域数量必须等于 tools/*.mjs 的数量');
  const j = mapJson(domains);
  for (const d of j.domains) {
    assert.ok(d.id && d.title && d.tool && Array.isArray(d.data) && d.data.length > 0, `域 ${d.id} 的地图条目不完整`);
    assert.ok(d.operations.length > 0);
  }
  // 已知四个域必须都在（漏一个就说明自我声明丢了）
  assert.deepEqual(j.domains.map((d) => d.id).sort(), ['corpus', 'disasm', 'fixtures', 'old-repo']);
});

test('转发可用：`tools <域> <只读动作>` 能真的跑到该工具（进程内调用，不捕获输出）', async () => {
  const { main } = await import('../cli.mjs');
  // 只用**只读**动作，避免测试改数据
  assert.equal(await main(['corpus', 'list']), 0);
  assert.equal(await main(['fixtures', 'list']), 0);
  assert.equal(await main(['disasm', 'describe']), 0);
  assert.equal(await main(['old-repo', 'describe']), 0);
});

test('未知域 / 未知动作 ⇒ 退出码 2（而不是静默跑错东西）', async () => {
  const { main } = await import('../cli.mjs');
  assert.equal(await main(['nope', 'list']), 2);
  assert.equal(await main(['corpus', 'nope']), 2);
});
